// Check the transport contract against an independent reading of the same rules.
//
// The engine half is the engine's own socket code (spike/net_transport_check.cpp links
// Battlefront2_pcNetSocket.cpp unchanged) running on browser/platform/winsock_browser.cpp. The model
// half is tools/net_transport_check.py. The check compares them and fails on any disagreement.
//
//   node browser/net_transport_check.mjs
import fs from 'fs';
import path from 'path';
import url from 'url';
import { execFileSync } from 'child_process';

const here = path.dirname(url.fileURLToPath(import.meta.url));
const root = path.join(here, '..');

const oracle = JSON.parse(execFileSync('python3', [path.join(root, 'tools/net_transport_check.py'), '--json'],
    { encoding: 'utf8' }));

const factory = (await import(path.join(root, 'build/net_transport.js'))).default;
const Module = await factory({ onAbort: (why) => { console.error('ABORT:', why); process.exit(3); } });

const strAt = (ptr) => {
    let end = ptr;
    while (Module.HEAPU8[end] !== 0) end++;
    return Buffer.from(Module.HEAPU8.slice(ptr, end)).toString('utf8');
};

let fails = 0;
const check = (name, got, want) => {
    const ok = JSON.stringify(got) === JSON.stringify(want);
    if (!ok) fails++;
    console.log(`  ${ok ? 'ok ' : 'FAIL'} ${name}`);
    if (!ok) console.log(`       engine: ${JSON.stringify(got)}\n       model:    ${JSON.stringify(want)}`);
    return ok;
};

const got = JSON.parse(strAt(Module.ccall('net_transport_report', 'number', [], [])));

console.log('engine socket code on the browser transport, vs the model of the same rules');

check('transport started through WSAStartup', got.transport_started, 1);
check('host port comes from hostshort', got.host_port_from_opendirectsocket, oracle.host_port);
check('our endpoint keeps the address it was bound to', got.our_endpoint,
    { ip: 0, port: oracle.host_port });
check('SetAddr stores network order and returns it', got.setaddr_returns_htons, oracle.setaddr_returns_htons);
check('GetAddr reads it back as the game layer would', got.getaddr, oracle.getaddr);
check('the peer identity survives SetAddr/GetAddr', got.round_trip, oracle.round_trip);

check('send to a registered channel succeeds and the bytes survive', [got.send.result, got.send.bytes_preserved, got.send.channel_found],
    [got.send.result, 1, 1]);
check('a failed send reports a code inside the counted set (which one is the adapters choice)',
    [got.send.no_channel_result, oracle.send_error_codes_allowed.indexOf(got.send.error) >= 0],
    [-1, true]);
check('the send path counts the socket error', got.send.counted_by_decode_error, oracle.counted_on_send_error);
check('an empty inbox is an error, not a zero-length read', [got.empty_read.result, got.empty_read.error], [-1, oracle.error_wouldblock]);
check('the receive path does not count WSAEWOULDBLOCK', got.empty_read.counted_by_decode_error, oracle.counted_on_empty_receive);

check('one update asks for data exactly as the source loops',
    [got.receive.calls_in_one_update, got.receive.left_in_queue, got.receive.loop_limit_from_source],
    [oracle.calls_in_one_update, oracle.left_in_queue_after_one_update, oracle.loop_limit]);
check('the engine receive buffer is 2048', got.receive.engine_buffer, oracle.engine_buffer);

check('the engine took the datagram through its own receive path',
    [got.received.accepted, got.received.bytes_preserved], [1, 1]);
check('prefix for a type at or above 0x1e', got.received.prefix_for_type_at_or_above_0x1e,
    oracle.prefix.type_at_or_above_0x1e);
check('a datagram over size_limit - prefix is rejected', got.received.over_size_limit_rejected, 1);
/* The prefix for types below 0x1e cannot be measured through the socket layer: a real packet type is
 * routed into the game layer by NetCommP::RecvPacket, which this check does not have. It is pinned by
 * browser/net_wire_check.mjs, where the packet layer is exercised on its own. */
console.log('  n/a  prefix for a type below 0x1e: needs the game layer, pinned by net_wire_check.mjs');

check('the peer address is matched to its destination', got.find_source.destination_for_this_peer, oracle.find_source.destination_for_this_peer);
check('a host-routing type is not matched to that peer', got.find_source.for_a_host_routing_type, oracle.find_source.for_a_host_routing_type);
check('a replica update (type 15) is scanned from slot 64, so the peer must be recorded there',
    [got.find_source.type_15_before_setting_64, got.find_source.type_15_after_setting_64],
    [oracle.find_source.type_15_before_setting_64, oracle.find_source.type_15_after_setting_64]);

/* What this says about a browser adapter, stated as facts. */
console.log('\nwhat this pins:');
console.log('  the peer identity is an (ip, port) pair. A browser connection does not have one, so the host');
console.log('  must fabricate one per connection and keep it stable - GetAddr/SetAddr and FindSource must');
console.log('  agree on it, and byte order must not change between them.');
console.log('  C cannot send by itself: sendto hands the datagram to the host, and the host either has a');
console.log('  channel for that address or the send fails with WSAENOTCONN, which the engine counts.');
console.log('  "no data" must be an error with WSAEWOULDBLOCK, not a zero read: the engine loops on the error.');
console.log('  the engine processes at most 80 datagrams per update, then reads and discards anything');
console.log('  beyond that itself. The cap belongs to the engine and the buffer belongs to the');
console.log('  transport: an adapter that cuts the read off at 80 stops the drain loop, and the');
console.log('  overflow becomes invisible. Measured: 100 waiting -> 101 requests for data.');
console.log('  broadcast has no browser equivalent: a channel for INADDR_BROADCAST has to be a lobby');
console.log('  signalling route the host provides, or LAN discovery cannot work through this seam.');

/* The names the socket layer asks for - the real size of what it depends on. */
const closurePath = path.join(root, 'build/net_transport_undefined.txt');
if (fs.existsSync(closurePath)) {
    const names = fs.readFileSync(closurePath, 'utf8').trim().split('\n').filter(Boolean);
    console.log(`\nthe socket layer's own dependency closure (${names.length} names it cannot resolve on its own):`);
    console.log('  ' + names.join('\n  '));
    console.log('  The check does not run those paths, so the module is built tolerating them; they are');
    console.log('  what the game layer and the Galaxy API still owe.');
}

console.log(`\n${fails === 0 ? 'agree' : 'DISAGREEMENT'} (${fails} failed)`);
process.exit(fails === 0 ? 0 : 1);
