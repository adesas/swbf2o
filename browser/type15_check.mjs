// Check PKT_GAME_REPLICA_UPDATE (type 15) against an independent implementation of the same rules.
//
// The engine's half is the real thing: ReadDestMask, ReadTurnLowBits, ReadSwitchResponse and
// ReadMove, and their writers, copied verbatim out of NetGame.cpp by tools/extract_type15.py. The
// model half is tools/type15_check.py, a second reading of the same source.
//
// If they agree, the type-15 format is pinned - the 64 bit mask, the 11 bit turn with its reserved
// codes and rollover, the switch response, and the move list with its reserved fixed-field
// endpoints. If they disagree, one of the two readings is wrong.
//
//   node browser/type15_check.mjs
import path from 'path';
import url from 'url';
import { execFileSync } from 'child_process';

const here = path.dirname(url.fileURLToPath(import.meta.url));
const root = path.join(here, '..');

const oracle = JSON.parse(execFileSync('python3', [path.join(root, 'tools/type15_check.py'), '--json'],
    { encoding: 'utf8' }));

const factory = (await import(path.join(root, 'build/type15.js'))).default;
const Module = await factory({ onAbort: (why) => { console.error('ABORT:', why); process.exit(3); } });

const report = JSON.parse(Module.ccall('type15_report', 'string', [], []));

let fails = 0;
const check = (name, got, want) => {
    const ok = JSON.stringify(got) === JSON.stringify(want);
    if (!ok) fails++;
    console.log(`  ${ok ? 'ok ' : 'FAIL'} ${name}`);
    if (!ok) console.log(`       engine: ${JSON.stringify(got)}\n       model:    ${JSON.stringify(want)}`);
    return ok;
};

console.log('PKT_GAME_REPLICA_UPDATE: the engine\'s readers and writers against an independent model');
for (let i = 0; i < report.scenarios.length; i++) {
    const e = report.scenarios[i];
    const m = oracle.scenarios[i];
    console.log(` ${e.name}`);
    check('the bytes on the wire agree', e.bytes, m.bytes);
    check('the destination mask', e.mask, m.mask);
    check('the turn', e.turn, m.turn);
    check('the client turn', e.client_turn, m.client_turn);
    check('the lag', e.lag, m.lag);
    check('the switch response', e.switch_response, m.switch_response);
    check('how many moves were read', e.moves, m.moves);
    check('the end marker was seen', e.ended, m.ended);
    check('the inputs that came out', e.inputs, m.inputs);
}

console.log('  the reserved codes are the point: 0x7ff is "no turn", 0x7fe escapes with one bit,');
console.log('  the fixed fields reserve their endpoints, and the buttons are 30 bits of mBoolInputs');
console.log('  plus 2 bits of mMouseMode - one word, two meanings.');

if (fails) {
    console.log(`\nDISAGREEMENT (${fails} failed)`);
    process.exit(1);
}
console.log('\nagree (0 failed)');
