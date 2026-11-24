/* The transport contract, measured against the engine's own socket code.

pcNetSocket.cpp is linked here unchanged, so the numbers come from the engine's functions
(OpenDirectSocket, SendSocket, RecvSockets, SetAddr, GetAddr, FindSource, and MakeReceivedPacket as
RecvSockets calls it) sitting on top of browser/platform/winsock_browser.cpp.
tools/net_transport_check.py models the same rules from the source; browser/net_wire_check.mjs and
browser/net_transport_check.mjs compare the two and fail on any disagreement.

Usage from node: net_transport_report() -> pointer to a NUL-terminated JSON string.
*/
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "net_transport.h"
#include "winsock2.h"

#include "NetPacket.h"
#include "NetComm.h"
#include "pcNetSocket.h"

extern bool isLAN;
extern int netSocketErrorCount;
extern unsigned int netDirectSocket;
extern unsigned char netLanReceiveBuffer[2048];

void __fastcall PackHeader(NetPacket* pPacket);
unsigned int __fastcall UnpackHeader(NetPacket* pPacket);
int __fastcall FindSource(const void* pSource, unsigned int uType);

static char out[4096];
static int outPos;

static void emit(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    outPos += vsnprintf(out + outPos, sizeof(out) - (size_t)outPos, fmt, ap);
    va_end(ap);
}

static void hex(const unsigned char* p, int n)
{
    for (int i = 0; i < n; ++i) emit("%02x", p[i]);
}

static void pattern(int n, unsigned char* buf)
{
    for (int i = 0; i < n; ++i) buf[i] = (unsigned char)((i * 3 + 1) & 0xff);
}

static int equalBytes(const unsigned char* a, const unsigned char* b, int n)
{
    for (int i = 0; i < n; ++i) if (a[i] != b[i]) return 0;
    return 1;
}

/* A datagram built the way the engine builds one: write operations, flush, pack the header, then take
 * the bytes Deliver would send. This is what the transport carries. */
static unsigned char datagram[1024];

static int makeDatagram(void)
{
    NetPacket::Create();
    NetPktGroup g;
    g.Reset();
    g.isSmall = true;
    g.BeginWrite();
    g.WriteBits(0xdeadbeefu, 20);      /* crosses a word boundary */
    g.WriteBits(7, 3);                 /* leaves a partial word */
    g.WriteBytes("abc", 3);            /* aligned to a word boundary first */
    g.EndWrite();

    NetComm::Header(g.GetFront()).type = 2;      /* PKT_QUERY, so the receive prefix is 3 */
    NetComm::Header(g.GetFront()).src = 64;
    NetComm::Header(g.GetFront()).count = 1;
    NetComm::Header(g.GetFront()).idx = 0;
    PackHeader(g.GetFront());

    NetPacket* p = g.GetFront();
    int len = p->mMaxIndex - p->mCommIndex;
    memcpy(datagram, p->mData + p->mCommIndex, (size_t)len);
    return len;
}

extern "C" const char* net_transport_report(void)
{
    net_reset();
    NetPacket::Create();
    NetComm::Init();
    isLAN = true;                       /* the LAN path; there is no GameSpy Galaxy in the browser */

    int hostPort = NetSocket::OpenDirectSocket(true);
    int ourHandle = (int)netDirectSocket;

    /* A browser connection has no address, so the host gives it one. The value does not matter; it
     * must be stable and unique, because it is what the game layer uses to identify a player. */
    const unsigned int peerIp = 0x0a000001u;
    const unsigned short peerPort = 9001;

    unsigned short storedPort = NetSocket::SetAddr(NetCommP::GetAddr(1), peerIp, peerPort);
    int ip = 0, port = 0;
    NetSocket::GetAddr(NetCommP::GetAddr(1), ip, port);

    emit("{");
    emit("\"transport_started\":%d,", net_transport_started());
    emit("\"host_port_from_opendirectsocket\":%d,", hostPort);
    emit("\"our_endpoint\":{\"ip\":%u,\"port\":%u},", net_endpoint(ourHandle)->ip, net_endpoint(ourHandle)->port);
    emit("\"setaddr_returns_htons\":%u,", storedPort);
    emit("\"getaddr\":{\"ip\":%u,\"port\":%u},", ip, port);
    emit("\"round_trip\":%d,", (ip == (int)peerIp && port == (int)peerPort) ? 1 : 0);

    int channel = net_channel(ourHandle, peerIp, peerPort);
    int len = makeDatagram();
    int sent = NetSocket::SendSocket(1, (char*)datagram, len);
    const struct NetDatagram* carried = net_out_at(ourHandle, 0);
    int preserved = (sent == len && carried && equalBytes(datagram, carried->data, len)) ? 1 : 0;

    int beforeUnrouted = netSocketErrorCount;
    int other = net_open(2);
    net_bind(other, 0x0a000009u, 9009);            /* an address the host has no channel for */
    int failed = NetSocket::SendSocket(2, (char*)datagram, len);
    emit("\"send\":{\"result\":%d,\"bytes_preserved\":%d,\"channel_found\":%d,"
         "\"no_channel_result\":%d,\"error\":%d,\"counted_by_decode_error\":%d},",
         sent, preserved, channel >= 0 ? 1 : 0, failed, net_error(ourHandle),
         netSocketErrorCount - beforeUnrouted);

    /* An empty inbox must be an error with WSAEWOULDBLOCK, not a zero-length read. Measured twice:
     * what the transport returns, and what the engine does with it. */
    struct sockaddr_in from;
    memset(&from, 0, sizeof(from));
    int fromLen = 0;
    int empty = recvfrom(ourHandle, out, 64, 0, (struct sockaddr*)&from, &fromLen);
    int beforeCounted = netSocketErrorCount;
    NetSocket::RecvSockets();                      /* the engine's own loop, on an empty inbox */
    emit("\"empty_read\":{\"result\":%d,\"error\":%d,\"counted_by_decode_error\":%d},",
         empty, net_error(ourHandle), netSocketErrorCount - beforeCounted);

    /* The engine reads at most 80 datagrams per update and discards anything beyond. The transport
     * cannot tell which ones the engine handled, so this measures what it took in one call; the limit
     * itself is stated from the source, not measured here. */
    int beforeCalls = net_recvfrom_calls(ourHandle);
    unsigned char raw[32];
    pattern(24, raw);
    raw[0] = 30;                                   /* PKT_NONE and above: no game-layer handling */
    int posted = 0;
    for (int i = 0; i < 100; ++i) {
        if (net_post(ourHandle, raw, 24, peerIp, peerPort) > 0) posted++;
    }
    NetSocket::RecvSockets();
    int calls = net_recvfrom_calls(ourHandle) - beforeCalls;
    int left = net_endpoint(ourHandle)->inboxCount;
    emit("\"receive\":{\"posted\":%d,\"calls_in_one_update\":%d,\"left_in_queue\":%d,"
         "\"loop_limit_from_source\":80,\"engine_buffer\":%d},", posted, calls, left,
         (int)sizeof(netLanReceiveBuffer));

    /* MakeReceivedPacket, as the engine's receive path runs it. Only types PKT_NONE and above can be
     * measured here: a real packet type is routed by NetCommP::RecvPacket into the game layer, which
     * this check does not have. RecvPacket hands the packet back to the free list, but the indices and
     * the buffer survive, so where the engine placed the datagram can be read back. The marker is set
     * to -1 first: still -1 afterwards means no packet was taken. The prefix value for types below
     * 0x1e is pinned by browser/net_wire_check.mjs, not measured here. */
    net_reset();
    NetPacket::Create();
    NetComm::Init();
    isLAN = true;
    int h = NetSocket::OpenDirectSocket(true);
    int handle = (int)netDirectSocket;
    (void)h;
    NetSmallPacket* slot = &netSmallPackets[0];
    net_channel(handle, peerIp, peerPort);

    unsigned char t30[24];
    pattern(24, t30);
    t30[0] = 30;                                   /* PKT_NONE and above -> prefix 0 */
    slot->mCommIndex = -1;
    int took = net_post(handle, t30, 24, peerIp, peerPort);
    NetSocket::RecvSockets();
    int prefixAtOrAbove = slot->mCommIndex;
    int accepted = prefixAtOrAbove == 0 ? 1 : 0;

    unsigned char big[1200];
    memset(big, 0, sizeof(big));
    big[0] = 30;
    slot->mCommIndex = -1;
    net_post(handle, big, 1100, peerIp, peerPort);  /* 1100 > 1012 - 0 -> rejected, no packet */
    NetSocket::RecvSockets();
    int rejected = slot->mCommIndex;

    emit("\"received\":{\"prefix_for_type_at_or_above_0x1e\":%d,\"accepted\":%d,"
         "\"over_size_limit_rejected\":%d,\"bytes_preserved\":%d},",
         prefixAtOrAbove, accepted, rejected < 0 ? 1 : 0,
         equalBytes(t30, (const unsigned char*)slot->mData, 24) ? 1 : 0);

    /* The peer identity the game layer uses: FindSource matches the sender's address against the
     * address recorded for each destination. */
    struct sockaddr_in in;
    memset(&in, 0, sizeof(in));
    in.sin_family = AF_INET;
    in.sin_addr.s_addr = peerIp;
    in.sin_port = htons(peerPort);
    NetSocketPeerRecord peer;
    memset(&peer, 0, sizeof(peer));
    memcpy(peer.mSocketAddress, &in, 16);
    NetSocket::SetAddr(NetCommP::GetAddr(1), peerIp, peerPort);
    int found = FindSource(&peer, 4);              /* PKT_JOIN, which does not route to the host */
    int foundHost = FindSource(&peer, 12);         /* PKT_GAME_UPDATE, which does: search starts at 64 */
    int type15Before = FindSource(&peer, 15);      /* PKT_GAME_REPLICA_UPDATE, before slot 64 is set */
    /* A replica update is looked for at the host destination, so the peer has to be recorded there.
     * SetAddr takes the record pointer - netDests[i] + 0x20 - which is what NetCommP::GetAddr
     * returns. Passing the index does not compile against the engine's signature. */
    NetSocket::SetAddr(NetCommP::GetAddr(64), peerIp, peerPort);
    int type15After = FindSource(&peer, 15);
    emit("\"find_source\":{\"destination_for_this_peer\":%d,\"for_a_host_routing_type\":%d,"
         "\"type_15_before_setting_64\":%d,\"type_15_after_setting_64\":%d},",
         found, foundHost, type15Before, type15After);

    emit("\"fabricated_peer\":{\"ip\":%u,\"port\":%u}", peerIp, peerPort);
    emit("}");
    return out;
}
