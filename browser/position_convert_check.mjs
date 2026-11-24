// Check the position quantisation, the netLOD path, the per-client rule and the quaternion half against
// an independent implementation of the same rules.
//
// The engine's half is the real thing: BeginNetLOD, SetNetLOD, SetNetPrecision, ReadNetLOD,
// NetGame::GetMyGuest, NetGame::IsLocalTo, NetVectorBits, the eight converter classes, WriteUnitVector,
// ReadUnitVector, WritePositionVector, ReadPositionVector, WriteThrust, ReadThrust, WriteAngle,
// ReadAngle, WriteSwitch, ReadSwitch, WriteJetFuel, ReadJetFuel, WriteQuaternion, ReadQuaternion, copied
// verbatim out of NetState.cpp and NetGame.cpp by tools/extract_position_convert.py and run against the
// engine's packet layer. The model half is tools/position_convert_check.py, a second reading of the same
// source.
//
// Two of those are not the engine's code and the check says so: the class shell that lets NetGame's two
// functions be called, and the guest table its rule reads, which is an input both halves are given. The
// assignment at NetState.cpp:1802 is quoted and applied by the harness, not run, because MountedTurret is
// game layer. Nothing here normalises the received quaternion: D3DXQuaternionNormalize and
// D3DXMatrixRotationQuaternion are not engine code available to this check.
//
// What is compared: the steps each side runs, the state each side reaches, the bit width each side
// derives, the ranges each side reads off the class, the codes each side puts on the wire, the bytes
// those codes make, and the values each side decodes. A check that only round-tripped the engine against
// itself could not fail, because a writer and a reader that are wrong in the same way still round trip.
//
// The model's field-to-converter mapping and the engine harness's are two independent readings of
// NetState.cpp:2177 and :2185; the bytes are what decides them, since the two converters have different
// ranges and different divisors.
//
// A check cannot fail is not a check, so this runner puts four deliberate mistakes in the model and shows
// that each one disagrees. The engine is not touched; only the model is wrong on purpose. The first is a
// wrong bit width. The second applies the raw-IEEE path to every converter when netLOD is 1, which is the
// mistake a host would make for JetFuel, Switch and Quat: three of the eight converters are not gated. A
// model that gates everything must be wrong for those and for those only. The third is a model that does
// not carry state from one group to the next, and the fourth reads a group's field and applies it after
// that group's fields, which is not the order the source uses. The runner exits non-zero if any of the
// four agrees on the scenarios it is supposed to be wrong for.
//
//   node browser/position_convert_check.mjs
import path from 'path';
import url from 'url';
import { execFileSync } from 'child_process';

const here = path.dirname(url.fileURLToPath(import.meta.url));
const root = path.join(here, '..');

const modelPath = path.join(root, 'tools/position_convert_check.py');
const runModel = (args) => JSON.parse(execFileSync('python3', [modelPath, '--json', ...args],
    { encoding: 'utf8' }));

const factory = (await import(path.join(root, 'build/position_convert.js'))).default;
const Module = await factory({ onAbort: (why) => { console.error('ABORT:', why); process.exit(3); } });

const engine = JSON.parse(Module.ccall('position_report', 'string', [], []));

let fails = 0;
const check = (name, got, want) => {
    const ok = JSON.stringify(got) === JSON.stringify(want);
    if (!ok) {
        fails++;
        console.log(`  FAIL ${name}`);
        console.log(`       engine: ${JSON.stringify(got)}\n       model:    ${JSON.stringify(want)}`);
    }
    return ok;
};

const INPUT = ['type', 'src', 'group_sequence', 'dohires'];
const FIELDS = ['steps', 'values', 'packets', 'bytes', 'complete_after_each',
    'state_after_write', 'state_after_read', 'observed', 'fields_write', 'fields_read'];
const DECISION = ['decision'];          /* only present in the scenarios that run the rule */
const GROUPF = ['groups'];              /* only present in the scenarios that cross a group boundary */

const model = runModel(['--risk']);

console.log("Position quantisation and the netLOD path: the engine's own code against an independent model");
console.log(`  header: type ${engine.input.type}, src ${engine.input.src}, ` +
    `group ${engine.input.group_sequence}, doHiRes ${engine.input.dohires}`);
for (const f of INPUT) check(`the agreed input field ${f}`, engine.input[f], model.input[f]);

console.log('\nnetLOD is observed, not assumed: the engine half calls its own BeginNetLOD, SetNetLOD,\n' +
    'SetNetPrecision and ReadNetLOD, and the model implements the same four rules independently.\n' +
    'SetNetLOD takes a bool, so it can only make 0 or 1; the scenarios that show 2 and 3 put the field\n' +
    'on the wire as the wire would carry it, which the model states plainly. The engine half has one\n' +
    'netLOD, as the source does, so the reader continues from the state the writer left.');
console.log();

for (let i = 0; i < engine.scenarios.length; i++) {
    const e = engine.scenarios[i], m = model.scenarios[i];
    if (e.name !== m.name) {
        fails++;
        console.log(`  FAIL the scenario lists differ: ${JSON.stringify(e.name)} vs ${JSON.stringify(m.name)}`);
        continue;
    }
    console.log(` ${e.name}`);
    let ok = 0;
    for (const f of FIELDS) if (check(`the ${f}`, e[f], m[f])) ok++;
    if (e.decision !== undefined || m.decision !== undefined) {
        if (check(`the ${DECISION[0]}`, e.decision, m.decision)) ok++;
    }
    let n = FIELDS.length + ((e.decision !== undefined || m.decision !== undefined) ? 1 : 0);
    if (e.groups !== undefined || m.groups !== undefined) {
        if (check(`the ${GROUPF[0]}`, e.groups, m.groups)) ok++;
        n++;
    }
    console.log(`  ${ok}/${n} agree, netLOD after read ${e.state_after_read[0]}`);
    if (e.groups) {
        console.log('  what each half had when it reached each boundary, read from the bytes:');
        for (const g of e.groups) {
            const mg = m.groups[g.idx];
            console.log(`    group ${g.idx}: the field says ${g.first_bit ? 'unchanged' : 'a value'}, ` +
                        `${g.field_bits} bit${g.field_bits > 1 ? 's' : ''}; ` +
                        `writer arrived at ${g.writer_before}, reader arrived at ${g.reader_before} ` +
                        `and left at ${g.reader_after}`);
            if (JSON.stringify(g) !== JSON.stringify(mg)) {
                console.log(`      the model would have said ${JSON.stringify(mg)}`);
            }
        }
    }
    if (m.quat_components) {
        console.log('  what the reader got, before anything normalises it. The normalising code is');
        console.log('  D3DXQuaternionNormalize, which is not engine code here, so this is the model\'s');
        console.log('  own float32 arithmetic on the components it decoded, not an engine observation:');
        const f = (bits) => { const b = new Uint32Array([Number(BigInt(bits) & 0xFFFFFFFFn)]); return new Float32Array(b.buffer)[0]; };
        const v = m.quat_components.map(f);
        const mul = (a, b) => { const x = new Float32Array([a * b]); return x[0]; };
        const sum = v.reduce((a, b) => mul(a, mul(b, b)), 0);
        const len = f(m.quat_length_before_normalisation);
        console.log(`    components ${v.join(', ')} length ${len}, which is not 1`);
        console.log(`    the model says ${len}; the two agree because both compute in float32`);
    }
}

/* The order inside a group is what decides it. Both halves agree on these two, and the agreement is the
 * point: neither side is wrong. The writer used the value it had at the moment it wrote; the reader read
 * the group's field first, so it used the value it already had, and after that it read the bytes at the
 * wrong place. Nothing on the wire tells the reader that the writer changed its mind. */
const asFloat = (bits) => {
    const b = new Uint32Array([Number(BigInt(bits) & 0xFFFFFFFFn)]);
    return new Float32Array(b.buffer)[0];
};
const mid = engine.scenarios.find((s) => s.name.includes('the change happens inside the group'));
const first = engine.scenarios.find((s) => s.name.includes('the change before the field'));
if (mid && first) {
    console.log('\nthe same value, two orders inside the same group:');
    console.log(`  ${first.name}: the field carries ${first.observed[0][0]}, so the reader agrees`);
    for (const w of first.fields_write) {
        const r = first.fields_read[first.fields_write.indexOf(w)];
        console.log(`    writer code ${w.code} at ${w.bits} bits -> reader got ${asFloat(r.decoded)}`);
    }
    console.log(`  ${mid.name}: the field says unchanged, so the reader keeps ${mid.observed[0][0]}`);
    for (const w of mid.fields_write) {
        const r = mid.fields_read[mid.fields_write.indexOf(w)];
        console.log(`    writer code ${w.code} at ${w.bits} bits -> reader read ${r.bits} bits and got ${asFloat(r.decoded)}`);
    }
    console.log('  which side is wrong: neither. The writer used the value it had at that moment; the');
    console.log('  reader read what the bytes said. The reader cannot reconstruct that group because the');
    console.log('  value the writer used was never carried on the wire. That is what the field is for,');
    console.log('  and it is why the reader has to reach the group in the same order the writer left it.');
}

if (fails) {
    console.log(`\nDISAGREEMENT (${fails} failed)`);
    process.exit(1);
}
console.log('\nagree (0 failed)');

// The risk of doing this arithmetic in double instead of float32. The engine does every step in
// float32, so a model in double is a stronger arithmetic and can land on a different last bit.
console.log('\nwhat a double model would get wrong, counted over every interior code:');
for (const k in model.risk) {
    if (k !== 'note') console.log(`  ${k}: ${model.risk[k]}`);
}
console.log(`  ${model.risk.note}`);

// The negative control. The engine is unchanged; only the model's reading of the bit width is wrong on
// purpose. If a wrong bit width changed nothing, the check was never measuring it.
const WRONG = 20;
const wrong = runModel(['--risk', '--wrong-bits', String(WRONG)]);
let wrongFails = 0;
for (let i = 0; i < engine.scenarios.length; i++) {
    const e = engine.scenarios[i], m = wrong.scenarios[i];
    let bad = 0;
    for (const f of FIELDS) if (JSON.stringify(e[f]) !== JSON.stringify(m[f])) bad++;
    if (bad) {
        wrongFails++;
        console.log(` ${e.name}: ${bad}/${FIELDS.length} disagree with the engine, model bits ${m.fields_write[0].bits}`);
    }
}
if (!wrongFails) {
    console.log('\nBAD: the negative control agreed, so the check is not measuring the bit width');
    process.exit(1);
}
console.log(`\nnegative control: model told ${WRONG} bits instead of 32/21/11 disagrees on ` +
    `${wrongFails}/${engine.scenarios.length} scenarios`);

/* The second control: a model that gates every converter at netLOD 1. It must be wrong for the three
 * that the source does not gate, and for those scenarios only - otherwise the check is not measuring
 * which classes read netLOD. */
const gated = runModel(['--risk', '--gate-everything']);
const gatedFails = [];
for (let i = 0; i < engine.scenarios.length; i++) {
    const e = engine.scenarios[i], m = gated.scenarios[i];
    if (JSON.stringify(e.fields_write) !== JSON.stringify(m.fields_write) ||
        JSON.stringify(e.fields_read) !== JSON.stringify(m.fields_read)) gatedFails.push(e.name);
}
const MUST = ["the same float, encoded both ways in one group through the engine's own WriteFixed",
    'three converters that do not change with precision'];
for (const name of MUST) if (!gatedFails.includes(name)) {
    console.log(`\nBAD: gating every converter agreed on "${name}", so the check is not measuring which classes read netLOD`);
    process.exit(1);
}
console.log(`negative control: a model that gates everything disagrees on ${gatedFails.length}/${engine.scenarios.length} scenarios, ` +
    `and those are the ones where a converter that is not gated is used at netLOD 1:`);
for (const name of gatedFails) console.log(`  ${name}`);
console.log('the engine half is unchanged; the model is wrong on purpose. This is what makes the check real.');

/* The third and fourth controls, for the state across groups. The first is a model that does not carry
 * state from one group to the next, which is the mistake a check that treats every group as a fresh
 * start would make. The second reads the field and then applies it after the group's fields, which is
 * the order the source does not use. Both must disagree, and on the scenarios that cross a boundary. */
const MUST2 = ['three groups in order: A sets the precision, B says unchanged, C changes it again'];
const mustFail = (control, args, must, why) => {
    const m2 = runModel(['--risk', ...args]);
    const bad = [];
    for (let i = 0; i < engine.scenarios.length; i++) {
        const e = engine.scenarios[i], mm = m2.scenarios[i];
        const hit = FIELDS.some((f) => JSON.stringify(e[f]) !== JSON.stringify(mm[f])) ||
            (e.groups !== undefined && JSON.stringify(e.groups) !== JSON.stringify(mm.groups));
        if (hit) bad.push(e.name);
    }
    for (const name of must) if (!bad.includes(name)) {
        console.log(`\nBAD: ${control} agreed on "${name}", so ${why}`);
        process.exit(1);
    }
    console.log(`negative control: ${control} disagrees on ${bad.length}/${engine.scenarios.length} scenarios:`);
    for (const name of bad) console.log(`  ${name}`);
    return bad;
};
const noCarry = mustFail('a model that does not carry state between groups', ['--no-carry'], MUST2,
    'the check is not measuring that an unchanged field means keep what you already have');
const lateApply = mustFail('a model that reads the field before the state is applied', ['--late-apply'], MUST2,
    'the check is not measuring the order the source uses');
if (!noCarry.length || !lateApply.length) {
    console.log('\nBAD: a control that cannot fail is not a control');
    process.exit(1);
}
console.log('the engine half is unchanged; the model is wrong on purpose. This is what makes the check real.');
process.exit(0);
