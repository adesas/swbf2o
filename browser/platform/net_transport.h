#pragma once

/* The transport under the engine's NetSocket seam.

pcNetSocket.cpp is the only place the engine touches a socket, and it is written against Winsock: one
non-blocking UDP socket, sendto per peer, recvfrom polled once per update, and the peer's (ip, port)
pair as its identity. The browser has no raw UDP. What has per-peer channels is WebRTC DataChannels or
a WebSocket relay, and those have no address - so the host has to give each connection an identity the
engine can use as an address, and the engine must be able to use it.

Four things follow, and they are the contract this layer implements:

  - Each peer gets a fabricated (ip, port). The values do not matter; they must be stable and unique
    per connection, because NetCommP::GetAddr(iDest) returns that address and FindSource matches
    received datagrams against it to decide which player a packet came from. Change the mapping and the
    game layer attributes packets to the wrong player.
  - C cannot put bytes on a wire by itself. sendto hands a datagram to the host, and the host's channel
    for that address either exists (the send succeeds) or does not (the send fails, which the engine
    counts in DecodeError). The host delivers the peer's answer into an inbox, and recvfrom takes it.
    Nothing here knows what the wire is.
  - The engine polls at most 80 datagrams per update (NetSocket::RecvSockets) and discards anything
    beyond that, so a browser transport must deliver at most 80 per update or buffer the rest itself;
    it cannot rely on the engine to drain them.
  - The engine expects "no data" to be an error result with WSAEWOULDBLOCK, not an empty read, and
    counts errors in DecodeError. An adapter that returns 0 instead of an error changes what the engine
    does.

The host drives it: open the endpoint, register a channel per peer, take what the engine sent, deliver
what a peer sent, and let the engine poll.
*/

#define NET_MAX_ENDPOINTS 16
#define NET_MAX_QUEUE 128
#define NET_MAX_DATAGRAM 1024

#define NET_EOK 0
#define NET_EWOULDBLOCK 10035   /* WSAEWOULDBLOCK */
#define NET_ENOTCONN 10057      /* WSAENOTCONN: a send to an address with no channel */

struct NetDatagram {
    unsigned int fromIp;
    unsigned short fromPort;
    unsigned int toIp;
    unsigned short toPort;
    int len;
    unsigned char data[NET_MAX_DATAGRAM];
};

struct NetEndpoint {
    int open;
    int nonblocking;
    int connected;
    unsigned int ip;               /* the address this endpoint answers to */
    unsigned short port;
    unsigned int connectedIp;      /* peer address from connect() (e.g. INADDR_BROADCAST:3656) */
    unsigned short connectedPort;
    int lastError;
    int inboxCount;
    int recvfromCalls;   /* measured: how many times the engine asked for data in one update */
    struct NetDatagram inbox[NET_MAX_QUEUE];
    int outboxCount;
    struct NetDatagram outbox[NET_MAX_QUEUE];   /* what the host is expected to carry to a peer */
};

struct NetChannel {                 /* a peer the host can reach, by address */
    unsigned int ip;
    unsigned short port;
    int endpoint;
};

void net_reset(void);
int net_open(int id);
void net_bind(int handle, unsigned int ip, unsigned short port);
int net_channel(int endpoint, unsigned int ip, unsigned short port);
int net_channel_lookup(int endpoint, unsigned int ip, unsigned short port);
int net_post(int endpoint, const unsigned char* bytes, int len, unsigned int fromIp, unsigned short fromPort);

struct NetEndpoint* net_endpoint(int handle);
int net_pending_out(int endpoint);                  /* datagrams the host has not carried yet */
const struct NetDatagram* net_out_at(int endpoint, int index);
int net_pop_in(int endpoint, struct NetDatagram* out);   /* what recvfrom would see */
int net_recvfrom_calls(int endpoint);
int net_error(int endpoint);
int net_transport_started(void);
int net_last_touched(void);
