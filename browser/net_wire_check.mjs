// Check the net packet wire format against an independent implementation of the same rules.
//
// Two halves: the engine's own functions (spike/net_wire.cpp, compiled to wasm and run here) and an
// independent implementation of the rules read from the source (tools/net_wire_check.py). If they
// agree, the format is pinned. If they disagree, one of the two readings of the source is wrong -
// which is exactly what a round-trip cannot show, because an encoder and a decoder that are wrong in
// the same way still round-trip.
//
//   node browser/net_wire_check.mjs
import fs from 'fs';
import path from 'path';
import url from 'url';
import { execFileSync } from 'child_process';

const here = path.dirname(url.fileURLToPath(import.meta.url));
const root = path.join(here, '..');

const oracle = JSON.parse(execFileSync('python3', [path.join(root, 'tools/net_wire_check.py'), '--json'],
    { encoding: 'utf8' }));

const factory = (await import(path.join(root, 'build/net_wire.js'))).default;
const Module = await factory({ onAbort: (why) => { console.error('ABORT:', why); process.exit(3); } });

const strAt = (ptr) => {
    let end = ptr;
    while (Module.HEAPU8[end] !== 0) end++;
    return Buffer.from(Module.HEAPU8.slice(ptr, end)).toString('utf8');
};
const report = (order) => JSON.parse(strAt(Module.ccall('wire_report', 'number', ['string'], [order])));

let fails = 0;
const check = (name, got, want) => {
    const ok = JSON.stringify(got) === JSON.stringify(want);
    if (!ok) fails++;
    console.log(`  ${ok ? 'ok ' : 'FAIL'} ${name}`);
    if (!ok) console.log(`       engine: ${JSON.stringify(got)}\n       model:    ${JSON.stringify(want)}`);
    return ok;
};

const norm = (v) => typeof v === 'number' ? String(v) : v;
const sameValues = (a, b) => a.length === b.length && a.every((v, i) => norm(v) === norm(b[i]));

console.log('engine side (the engine\'s own functions) vs the independent model');

for (const [label, order] of [['send_order', '01'], ['scrambled', '10']]) {
    const got = report(order);
    const want = oracle['decode_' + label];
    console.log(`\nreceiving in ${label} (${order.split('').join(',')})`);
    check('operations agree', got.ops, oracle.ops);
    check('datagram bytes and headers agree',
        got.datagrams.map(d => ({ bytes: d.bytes, header: d.header, len: d.len })),
        oracle.datagrams.map(d => ({ bytes: d.bytes, header: d.header, len: d.len })));
    check('receive order', got.decode.order, want.order);
    check('group completion after each fragment', got.decode.complete_after_each, want.complete_after_each);
    if (!sameValues(got.decode.values, want.values)) {
        fails++;
        console.log('  FAIL values read back');
        for (let i = 0; i < Math.max(got.decode.values.length, want.values.length); ++i) {
            if (norm(got.decode.values[i]) !== norm(want.values[i])) {
                console.log(`       op ${i} (kind ${oracle.ops[i].kind}): engine ${got.decode.values[i]} model ${want.values[i]}`);
            }
        }
    } else {
        console.log(`  ok  values read back (${got.decode.values.length} operations)`);
    }
}

/* What the check established, stated as facts rather than assumed. */
const f = oracle.facts;
console.log('\nwhat this pins:');
console.log(`  the wire datagram is 5 header bytes then the payload: type, group as 2 bytes LE, idx,` +
    ` count | (compressed << 7); the 8-byte PktHeader is a view over the buffer, not the wire layout`);
console.log(`  the writer starts at buffer index ${f.data_index} and the datagram starts at` +
    ` ${f.comm_index} = data index - 5; a receiver places the datagram at buffer + prefix, prefix ${f.prefix}` +
    ` (or ${f.prefix_when_type_ge_0x1e} when the first byte, the type, is >= 0x1e)`);
console.log(`  a group is split into one datagram per packet at the packet limit` +
    ` (${f.packet_limit_small} for small, ${f.packet_limit_big} for big): this one produced ${f.expected_packets}`);
console.log(`  bits are packed LSB-first inside 32-bit words and a value crossing a word boundary is` +
    ` still read back correctly, so fields below 32 bits are usually not word aligned`);
console.log(`  raw byte fields are aligned to a word boundary by writing zero bits first, and the reader` +
    ` discards the rest of the word it has read before taking bytes`);
console.log(`  fixed fields reserve code 0 and (1<<bits)-1 for the endpoints, so an interior value can` +
    ` only reach (1<<bits)-2`);
const crossed = oracle.ops.filter(o => o.kind !== 'BYTES' && o.bits < 32).length;
console.log(`  fragments may arrive out of order (${['01','10'].join(' or ')}) and are reordered by idx;` +
    ` the group is only complete when all count fragments are present - the check verifies both orders`);
console.log(`  ${crossed} sub-32-bit fields exercised, group completion checked in both orders`);
if (oracle.model_would_be_wrong_in_double) {
    const d = oracle.model_would_be_wrong_in_double.interior_codes_that_differ;
    console.log('  the model must compute in float32 like the engine: in double it would differ on ' +
        Object.entries(d).map(([k, v]) => k + '=' + v).join(', ') + ' interior codes. That is not a pass, it is the risk.');
}


console.log(`\n${fails === 0 ? 'agree' : 'DISAGREEMENT'} (${fails} failed)`);
process.exit(fails === 0 ? 0 : 1);
