#!/usr/bin/env python3
"""An independent reading of the transport contract, from the source.

The engine half is `spike/net_transport_check.cpp` (the engine's own pcNetSocket.cpp on top of
browser/platform/winsock_browser.cpp). This file models the same rules from the code, and
browser/net_transport_check.mjs compares the two.

Rules, with the function each came from:

  NetSocket::SetAddr   writes the peer record's address as (uIP stored as given, htons(uPort)) and
                       returns the stored port, which is therefore network order.
  NetSocket::GetAddr   reads sin_addr as it was stored and ntohs(sin_port). So the round trip works
                       only if the byte-order convention is not changed in between - the game layer's
                       peer identity depends on that pair.
  hostshort            `int hostshort = 3658` in NetAdmin.cpp; OpenDirectSocket uses it for the host
                       and hostshort + 1 for a client, and keeps incrementing if the bind fails.
  DecodeError          counts 10022, 10035, 10038, 10048, 10057, 10065, 10093. The two paths treat
                       "no data" differently: the send path counts any error from WSAGetLastError, the
                       receive path skips WSAEWOULDBLOCK before calling DecodeError. So an error on a
                       send is counted and an empty receive is not - an adapter that reports "no data"
                       as a zero-length read instead of an error still changes what the engine does.
  MakeReceivedPacket   the datagram is placed at buffer + prefix: prefix 3 when the first byte (the
                       type) is below 0x1e, 0 when it is 0x1e or above; a datagram longer than
                       1012 - prefix is rejected, and one of 116 - prefix or less takes a small
                       packet.
  NetSocket::RecvSockets  reads at most 80 datagrams per update; anything beyond that is read and
                       discarded, not delivered to the game layer. The engine's own buffer is 2048.
                       For 100 datagrams waiting, one update asks for data 101 times: 80 that it
                       processes, 20 that it discards, and one that returns WSAEWOULDBLOCK.
  FindSource           matches the sender's address against the address recorded for each destination.
                       Types that route to the host start the search at 64, so a peer addressed at
                       destination 1 is not found for those types; returns -1 when nothing matches.

Usage: python3 tools/net_transport_check.py [--json]
"""
import json
import sys

def htons(v):
    return ((v << 8) | (v >> 8)) & 0xFFFF


def ntohl_unchanged(v):
    return v  # the engine stores sin_addr as given and reads it as given


COUNTED_ERRORS = [10022, 10035, 10038, 10048, 10057, 10065, 10093]

PEER_IP = 0x0A000001
PEER_PORT = 9001

out = {
    'host_port': 3658,
    'client_port': 3659,
    'setaddr_returns_htons': htons(PEER_PORT),
    'getaddr': {'ip': ntohl_unchanged(PEER_IP), 'port': PEER_PORT},
    'round_trip': 1,
    'errors_counted': COUNTED_ERRORS,
    'error_notconn': 10057,
    # The engine's rule is only the counted set; which code an adapter returns for "no channel" is the
    # adapter's choice. 10035 is not a choice - RecvSockets tests for it explicitly to end its loop.
    'send_error_codes_allowed': COUNTED_ERRORS,
    'error_wouldblock': 10035,
    'counted_on_send_error': 1,          # the send path counts whatever WSAGetLastError gives
    'counted_on_empty_receive': 0,       # the receive path filters WSAEWOULDBLOCK first
    'prefix': {'type_below_0x1e': 3, 'type_at_or_above_0x1e': 0},
    'size_limit': 1012,
    'small_packet_limit': 116,
    'loop_limit': 80,
    'posted_for_test': 100,
    'calls_in_one_update': 80 + (100 - 80) + 1,   # processed + drained + the final would-block
    'processed': 80,
    'discarded': 20,
    'left_in_queue_after_one_update': 0,
    'engine_buffer': 2048,
    'find_source': {
        'destination_for_this_peer': 1,
        'for_a_host_routing_type': -1,
        'scan_start': {'host_routing': 64, 'other': 0},
        # PKT_GAME_REPLICA_UPDATE is 15. It is not in the list of types that do not route to the
        # host, so the scan starts at 64 - and a peer recorded only at slot 1 is not found.
        'type_15_before_setting_64': -1,
        'type_15_after_setting_64': 64,
    },
    'fabricated_peer': {'ip': PEER_IP, 'port': PEER_PORT},
    'notes': [
        'a browser connection has no address, so the host must give each one a stable, unique '
        '(ip, port) pair: that pair is the peer identity the game layer matches on',
        'the engine processes at most 80 datagrams per update and reads and discards anything beyond '
        'that itself, so the buffer belongs to the transport; the cap does not',
        'the send path counts any socket error, the receive path does not count WSAEWOULDBLOCK',
        'the engine rule is the counted set, not a particular code: an adapter may return any of '
        'them for a failed send, but it must be in the set, or DecodeError never sees it',
        'FindSource scans from 64 for types that route to the host, and PKT_GAME_REPLICA_UPDATE '
        '(15) is one of them; a peer recorded only at slot 1 is not found and the packet is freed',
        'no raw UDP in the browser: sendto hands bytes to the host, and the host decides whether a '
        'channel for that address exists',
    ],
}

if '--json' in sys.argv:
    print(json.dumps(out, ensure_ascii=False))
else:
    for k, v in out.items():
        print(f"{k}: {v}")
sys.exit(0)
