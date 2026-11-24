/* The browser's Winsock layer: the engine's socket calls land here and go to the transport.

This is the file that replaces pcNetSocket.cpp's Winsock dependency. pcNetSocket.cpp itself stays the
engine's code - the shapes it depends on are reproduced exactly, because the engine's behaviour on top
of them is written against these shapes:

  - a socket is non-blocking and a "no data" result is an error with WSAEWOULDBLOCK, not a zero-length
    read. RecvSockets loops on that, up to 80 times per update, and DecodeError counts the errors it
    sees (10035 among them).
  - in strict test mode (default autoRoute=0, exercised by browser/net_transport_check.mjs), a send to
    an address with no registered channel fails with WSAENOTCONN (10057), also counted.
  - in live Phase 6 multiplayer mode (enabled via swbf2_net_set_local_ip), each peer holds a virtual
    RFC-1918 IPv4 address (10.200.x.y) returned by gethostbyname()/getlocalhost(), multi-socket handles
    (direct UDP 3658/3659 + broadcast send/recv 3656) are allocated independently, and sendto()/recvfrom()
    pump lock-free SharedArrayBuffer rings directly to the main thread's WebRTC DataChannel / WebSocket
    UDP client (browser/net_client.mjs) even inside synchronous CNetLaunchGame/CNetLoadGame spin loops.
*/
#include <stdio.h>
#include <string.h>

#include "net_transport.h"
#include "winsock2.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

static struct NetEndpoint endpoints[NET_MAX_ENDPOINTS];
static struct NetChannel channels[NET_MAX_ENDPOINTS * NET_MAX_ENDPOINTS];
static int channelCount;
static int transportStarted;
static int lastTouched;

// Phase 6 live virtual-LAN state (off by default so standalone net_transport_check stays strict).
static int g_autoRoute = 0;
static unsigned int g_localIp = 0;  // network byte order (e.g. 10.200.0.1 -> 0x0100C80A)
static unsigned long g_txPackets = 0;
static unsigned long g_txBytes = 0;
static unsigned long g_rxPackets = 0;
static unsigned long g_rxBytes = 0;

static char g_hostName[] = "swbf2-web";
static unsigned int g_hostAddrBytes = 0;
static char* g_hostAddrList[2] = { 0, 0 };
static char* g_hostAliases[1] = { 0 };
static struct hostent g_hostEnt;
static char g_ntoaBuf[32];

#ifdef __EMSCRIPTEN__
// Synchronous SharedArrayBuffer bridge hooks installed on Module.netBridge by browser/worker.mjs.
// Safe no-ops when Module.netBridge is absent (e.g. Node checks or single-player).
EM_JS(void, js_net_tx, (unsigned int toIp, unsigned int toPort, unsigned int fromPort,
                        const unsigned char* buf, int len), {
  const b = Module.netBridge;
  if (b && typeof b.send === 'function') {
    b.send(toIp >>> 0, toPort >>> 0, fromPort >>> 0, buf >>> 0, len | 0);
  }
});

EM_JS(int, js_net_rx_poll, (int allowWait), {
  const b = Module.netBridge;
  if (b && typeof b.pollRx === 'function') {
    return b.pollRx(allowWait | 0) | 0;
  }
  return 0;
});

static double g_lastEmptyRecvMs = -1000.0;
static int g_consecutiveEmptyRecvs = 0;
#endif

void net_reset(void)
{
    memset(endpoints, 0, sizeof(endpoints));
    memset(channels, 0, sizeof(channels));
    channelCount = 0;
    transportStarted = 0;
    lastTouched = -1;
}

struct NetEndpoint* net_endpoint(int handle)
{
    if (handle < 0 || handle >= NET_MAX_ENDPOINTS) return 0;
    return &endpoints[handle];
}

int net_open(int id)
{
    struct NetEndpoint* e = net_endpoint(id);
    if (!e) return -1;
    memset(e, 0, sizeof(*e));
    e->open = 1;
    e->nonblocking = 1;         /* the engine sets FIONBIO; this transport is always non-blocking */
    lastTouched = id;
    return id;
}

void net_bind(int handle, unsigned int ip, unsigned short port)
{
    struct NetEndpoint* e = net_endpoint(handle);
    if (!e) return;
    e->ip = ip;
    e->port = port;             /* stored as the engine's GetAddr would read it back */
}

int net_channel(int endpoint, unsigned int ip, unsigned short port)
{
    int found = net_channel_lookup(endpoint, ip, port);
    if (found >= 0) return found;
    if (channelCount >= (int)(sizeof(channels) / sizeof(channels[0]))) return -1;
    channels[channelCount].ip = ip;
    channels[channelCount].port = port;
    channels[channelCount].endpoint = endpoint;
    return channelCount++;
}

int net_channel_lookup(int endpoint, unsigned int ip, unsigned short port)
{
    for (int i = 0; i < channelCount; ++i) {
        if (channels[i].ip == ip && channels[i].port == port && channels[i].endpoint == endpoint) return i;
    }
    return -1;
}

int net_post(int endpoint, const unsigned char* bytes, int len, unsigned int fromIp, unsigned short fromPort)
{
    struct NetEndpoint* e = net_endpoint(endpoint);
    if (!e || len <= 0 || len > NET_MAX_DATAGRAM || e->inboxCount >= NET_MAX_QUEUE) return -1;
    struct NetDatagram* d = &e->inbox[e->inboxCount++];
    d->fromIp = fromIp;
    d->fromPort = fromPort;
    d->toIp = e->ip ? e->ip : g_localIp;
    d->toPort = e->port;
    d->len = len;
    memcpy(d->data, bytes, (size_t)len);
    return len;
}

int net_pending_out(int endpoint)
{
    struct NetEndpoint* e = net_endpoint(endpoint);
    return e ? e->outboxCount : 0;
}

const struct NetDatagram* net_out_at(int endpoint, int index)
{
    struct NetEndpoint* e = net_endpoint(endpoint);
    if (!e || index < 0 || index >= e->outboxCount) return 0;
    return &e->outbox[index];
}

int net_pop_in(int endpoint, struct NetDatagram* out)
{
    struct NetEndpoint* e = net_endpoint(endpoint);
    if (!e || e->inboxCount == 0) return 0;
    *out = e->inbox[0];
    for (int i = 0; i + 1 < e->inboxCount; ++i) e->inbox[i] = e->inbox[i + 1];
    e->inboxCount--;
    return out->len;
}

int net_error(int endpoint)
{
    struct NetEndpoint* e = net_endpoint(endpoint);
    return e ? e->lastError : NET_EOK;
}

/* --- Phase 6 live virtual-LAN exports -------------------------------------------------------- */

extern "C" {

void swbf2_net_begin_frame(void)
{
#ifdef __EMSCRIPTEN__
    g_consecutiveEmptyRecvs = 0;
#endif
}

void swbf2_net_set_local_ip(unsigned int ipNetOrder)
{
    g_localIp = ipNetOrder;
    g_autoRoute = (ipNetOrder != 0) ? 1 : 0;
    g_txPackets = 0;
    g_txBytes = 0;
    g_rxPackets = 0;
    g_rxBytes = 0;
}

int swbf2_net_push_in(unsigned int fromIp, unsigned int fromPort, unsigned int toPort,
                      const unsigned char* buf, int len)
{
    if (!buf || len <= 0 || len > NET_MAX_DATAGRAM) return 0;
    int delivered = 0;
    for (int i = 0; i < NET_MAX_ENDPOINTS; ++i) {
        struct NetEndpoint* e = &endpoints[i];
        if (!e->open) continue;
        // Broadcast discovery packets (port 3656) go to the bound broadcast recv socket;
        // unicast game packets go to the socket bound to toPort (e.g. 3658 or 3659).
        if (e->port != 0 && e->port == (unsigned short)toPort && !e->connected) {
            if (net_post(i, buf, len, fromIp, (unsigned short)fromPort) > 0) {
                ++delivered;
            }
        }
    }
    // Fallback: if no socket matched exact port and packet isn't broadcast (3656), deliver to
    // the first open bound non-broadcast direct socket (handles ephemeral/remapped client port).
    if (delivered == 0 && toPort != 3656u) {
        for (int i = 0; i < NET_MAX_ENDPOINTS; ++i) {
            struct NetEndpoint* e = &endpoints[i];
            if (e->open && !e->connected && e->port != 0 && e->port != 3656u) {
                if (net_post(i, buf, len, fromIp, (unsigned short)fromPort) > 0) {
                    ++delivered;
                    break;
                }
            }
        }
    }
    if (delivered > 0) {
        ++g_rxPackets;
        g_rxBytes += (unsigned long)len;
    }
    return delivered;
}

void swbf2_net_counters(unsigned long* txPkts, unsigned long* txBytes,
                        unsigned long* rxPkts, unsigned long* rxBytes,
                        unsigned int* localIp)
{
    if (txPkts) *txPkts = g_txPackets;
    if (txBytes) *txBytes = g_txBytes;
    if (rxPkts) *rxPkts = g_rxPackets;
    if (rxBytes) *rxBytes = g_rxBytes;
    if (localIp) *localIp = g_localIp;
}

}  // extern "C"

/* --- the functions pcNetSocket.cpp calls ----------------------------------------------------- */

int WSAStartup(unsigned short version, void* data)
{
    (void)version;
    (void)data;
    transportStarted = 1;
    return 0;
}

void WSACleanup(void) { transportStarted = 0; }

int socket(int af, int type, int proto)
{
    (void)af; (void)type; (void)proto;
    // Allocate the lowest free handle (0 first, preserving net_transport_check compatibility,
    // while allowing direct + broadcast send + broadcast recv sockets simultaneously).
    for (int i = 0; i < NET_MAX_ENDPOINTS; ++i) {
        if (!endpoints[i].open) {
            return net_open(i);
        }
    }
    return -1;
}

int bind(int s, const struct sockaddr* a, int len)
{
    (void)len;
    const struct sockaddr_in* in = (const struct sockaddr_in*)a;
    lastTouched = s;
    net_bind(s, in->sin_addr.s_addr, ntohs(in->sin_port));
    return 0;
}

int connect(int s, const struct sockaddr* a, int len)
{
    (void)len;
    struct NetEndpoint* e = net_endpoint(s);
    if (!e || !a) return -1;
    const struct sockaddr_in* in = (const struct sockaddr_in*)a;
    lastTouched = s;
    e->connected = 1;
    e->connectedIp = in->sin_addr.s_addr;
    e->connectedPort = ntohs(in->sin_port);
    net_channel(s, e->connectedIp, e->connectedPort);
    return 0;
}

int ioctlsocket(int s, long cmd, unsigned long* arg)
{
    struct NetEndpoint* e = net_endpoint(s);
    if (!e) return -1;
    lastTouched = s;
    if (cmd == FIONBIO && arg) e->nonblocking = (int)*arg;
    return 0;
}

int setsockopt(int s, int level, int opt, const void* v, int len)
{
    (void)level; (void)opt; (void)v; (void)len;
    lastTouched = s;
    return 0;
}

int getsockopt(int s, int level, int opt, void* v, int* len)
{
    (void)level; (void)opt;
    if (v && len) { memset(v, 0, (size_t)*len); *len = 4; }
    lastTouched = s;
    return 0;
}

int closesocket(int s)
{
    struct NetEndpoint* e = net_endpoint(s);
    if (!e) return -1;
    e->open = 0;
    e->connected = 0;
    e->port = 0;
    e->inboxCount = 0;
    e->outboxCount = 0;
    return 0;
}

// The same work under a second name, for winsock_c_browser.cpp: the engine's NetGame.cpp declares
// closesocket with C linkage and an unsigned int parameter, which is a different symbol, and one
// translation unit cannot declare a name with two linkages.
int net_close_socket(int s)
{
    return closesocket(s);
}

int sendto(int s, const void* buf, int len, int flags, const struct sockaddr* to, int tolen)
{
    (void)flags; (void)tolen;
    struct NetEndpoint* e = net_endpoint(s);
    if (!e || !buf || len <= 0) return -1;
    lastTouched = s;

    unsigned int dstIp = 0;
    unsigned short dstPort = 0;
    if (to) {
        const struct sockaddr_in* in = (const struct sockaddr_in*)to;
        dstIp = in->sin_addr.s_addr;
        dstPort = ntohs(in->sin_port);
    } else if (e->connected) {
        dstIp = e->connectedIp;
        dstPort = e->connectedPort;
    } else {
        e->lastError = NET_ENOTCONN;
        return -1;
    }

    if (!g_autoRoute) {
        if (net_channel_lookup(s, dstIp, dstPort) < 0) {
            e->lastError = NET_ENOTCONN;          /* the host has no channel for that peer */
            return -1;
        }
        if (len > NET_MAX_DATAGRAM || e->outboxCount >= NET_MAX_QUEUE) {
            e->lastError = NET_ENOTCONN;
            return -1;
        }
        struct NetDatagram* d = &e->outbox[e->outboxCount++];
        d->fromIp = e->ip;
        d->fromPort = e->port;
        d->toIp = dstIp;
        d->toPort = dstPort;
        d->len = len;
        memcpy(d->data, buf, (size_t)len);
        return len;
    }

    if (len > NET_MAX_DATAGRAM) {
        e->lastError = NET_ENOTCONN;
        return -1;
    }

    // Live Phase 6 multiplayer mode: push straight to SharedArrayBuffer TX ring -> WebRTC/WS.
    unsigned short srcPort = e->port ? e->port : 3656u;
    ++g_txPackets;
    g_txBytes += (unsigned long)len;
#ifdef __EMSCRIPTEN__
    js_net_tx(dstIp, dstPort, srcPort, (const unsigned char*)buf, len);
#endif
    return len;
}

int recvfrom(int s, void* buf, int len, int flags, struct sockaddr* from, int* fromlen)
{
    (void)flags;
    struct NetEndpoint* e = net_endpoint(s);
    if (!e) return -1;
    lastTouched = s;
    e->recvfromCalls++;

#ifdef __EMSCRIPTEN__
    if (g_autoRoute && e->inboxCount == 0) {
        // First do a non-blocking drain of the SharedArrayBuffer RX ring.
        js_net_rx_poll(0);
        if (e->inboxCount == 0) {
            // Only yield briefly on Atomics.wait when genuinely trapped in a tight synchronous
            // spin-loop (e.g. CNetLaunchGame / CNetPostLoadJoinGame: >=16 consecutive empty
            // polls within 0.5 ms of each other). NEVER block during normal 60 FPS gameplay!
            double now = swbf2_now_ms();
            if (now - g_lastEmptyRecvMs < 0.5) {
                ++g_consecutiveEmptyRecvs;
                if (g_consecutiveEmptyRecvs >= 16) {
                    js_net_rx_poll(1);
                    g_consecutiveEmptyRecvs = 0;
                }
            } else {
                g_consecutiveEmptyRecvs = 1;
            }
            g_lastEmptyRecvMs = swbf2_now_ms();
        } else {
            g_consecutiveEmptyRecvs = 0;
        }
    }
#endif

    struct NetDatagram d;
    int got = net_pop_in(s, &d);
    if (got <= 0) {
        e->lastError = NET_EWOULDBLOCK;       /* the engine loops on this */
        return -1;
    }
    int take = got < len ? got : len;         /* the engine's buffer is 2048, but it clamps anyway */
    memcpy(buf, d.data, (size_t)take);
    if (from) {
        struct sockaddr_in* out = (struct sockaddr_in*)from;
        out->sin_family = AF_INET;
        out->sin_addr.s_addr = d.fromIp;
        out->sin_port = htons(d.fromPort);
    }
    if (fromlen) *fromlen = (int)sizeof(struct sockaddr_in);
    return take;
}

int WSAGetLastError(void) { return net_error(lastTouched); }

/* pcNetSocket.cpp uses plain send()/recv() for netBroadcastSendSocket (connect()ed to
 * INADDR_BROADCAST:3656). Passing to=0 tells sendto() to use the connected destination. */
int send(int s, const void* b, int len, int flags) { return sendto(s, b, len, flags, 0, 0); }

int recv(int s, void* b, int len, int flags)
{
    struct NetEndpoint* e = net_endpoint(s);
    if (!e) return -1;
    struct sockaddr_in from;
    int fromLen = 0;
    return recvfrom(s, b, len, flags, (struct sockaddr*)&from, &fromLen);
}

/* The engine never uses these, but the shim declares them; defining them here keeps the
 * "the transport is the implementation" rule true for the whole Winsock surface. */
int listen(int s, int backlog) { (void)backlog; return net_endpoint(s) ? 0 : -1; }
int accept(int s, struct sockaddr* a, int* len)
{
    struct NetEndpoint* e = net_endpoint(s);
    if (!e) return -1;
    if (a) { struct NetDatagram d; if (net_pop_in(s, &d) <= 0) { e->lastError = NET_EWOULDBLOCK; return -1; } }
    return 0;
}
int getsockname(int s, struct sockaddr* a, int* len)
{
    struct NetEndpoint* e = net_endpoint(s);
    if (!e) return -1;
    struct sockaddr_in* out = (struct sockaddr_in*)a;
    out->sin_family = AF_INET;
    out->sin_addr.s_addr = e->ip ? e->ip : g_localIp;
    out->sin_port = htons(e->port);
    if (len) *len = (int)sizeof(struct sockaddr_in);
    return 0;
}
int getpeername(int s, struct sockaddr* a, int* len) { return getsockname(s, a, len); }
int select(int nfds, void* r, void* w, void* e2, struct timeval* t) { (void)nfds; (void)r; (void)w; (void)e2; (void)t; return 0; }

extern "C" struct hostent* gethostbyname(const char* name)
{
    (void)name;
    unsigned int ip = g_localIp;
    if (ip == 0) {
        if (!g_autoRoute) return 0;
        ip = 0x0100C80Au;  // default 10.200.0.1 in network byte order
    }
    g_hostAddrBytes = ip;
    g_hostAddrList[0] = (char*)&g_hostAddrBytes;
    g_hostAddrList[1] = 0;
    g_hostAliases[0] = 0;
    g_hostEnt.h_name = g_hostName;
    g_hostEnt.h_aliases = g_hostAliases;
    g_hostEnt.h_addrtype = AF_INET;
    g_hostEnt.h_length = 4;
    g_hostEnt.h_addr_list = g_hostAddrList;
    return &g_hostEnt;
}

char* inet_ntoa(struct in_addr a)
{
    unsigned int ip = a.s_addr;
    snprintf(g_ntoaBuf, sizeof(g_ntoaBuf), "%u.%u.%u.%u",
             ip & 0xffu, (ip >> 8) & 0xffu, (ip >> 16) & 0xffu, (ip >> 24) & 0xffu);
    return g_ntoaBuf;
}

int WSAAsyncSelect(int s, void* h, unsigned int msg, long evt) { (void)h; (void)msg; (void)evt; return net_endpoint(s) ? 0 : -1; }

int net_recvfrom_calls(int endpoint)
{
    struct NetEndpoint* e = net_endpoint(endpoint);
    return e ? e->recvfromCalls : 0;
}

int net_last_touched(void) { return lastTouched; }
int net_transport_started(void) { return transportStarted; }
