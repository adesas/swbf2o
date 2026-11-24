// Headless check: drive the wasm host from node against the real GameData, with no browser
// and no WebGL. This is what proves the engine half of the port works - the engine's file
// layer opening a real .lvl, the engine's chunk walker enumerating it, the engine's Lua 5.0
// VM executing a real mission script - before any of it is trusted in a browser tab.
//
//   node browser/node_test.mjs [file.lvl ...]
import fs from 'fs';
import path from 'path';
import url from 'url';
import { execFileSync } from 'child_process';

const here = path.dirname(url.fileURLToPath(import.meta.url));
const root = path.join(here, '..');
const GAME = path.join(root, 'data/Star wars battlefront 2/GameData/data/_lvl_pc');

const factory = (await import(path.join(root, 'build/browser/swbf2.js'))).default;

const Module = await factory({
  locateFile: (f) => path.join(root, 'build/browser', f),
  onAbort: (why) => { console.error('ABORT:', why); process.exit(3); },
});

const ccall = (...a) => Module.ccall(...a);

ccall('swbf2_init', 'number', ['number'], [0]);
console.log('host: ' + ccall('swbf2_version', 'string'));

// the wasm host expects GameData under /data
try { Module.FS.mkdir('/data'); } catch (e) { /* already there */ }

function dumpLog(since) {
  let i = 0;
  for (;;) {
    const line = Module.ccall('swbf2_log_line', 'string', ['number'], [i]);
    if (!line) break;
    if (i >= since) console.log('  | ' + line);
    i++;
    if (i > 400) break;
  }
  return i;
}

// stage a GameData file into the wasm filesystem under a flat name
function stage(rel) {
  const src = path.join(GAME, rel);
  if (!fs.existsSync(src)) { console.log(`skip ${rel}: not found`); return null; }
  const size = fs.statSync(src).size;
  if (size > 40 * 1024 * 1024) { console.log(`skip ${rel}: ${size} bytes is too big for this check`); return null; }
  const dst = '/data/' + rel.replace(/\//g, '_');
  Module.FS.writeFile(dst, fs.readFileSync(src));
  return { dst, src, size };
}

const wanted = process.argv.slice(2).length
  ? process.argv.slice(2)
  : ['core.lvl', 'common.lvl', 'ingame.lvl', 'tat/tat2.lvl', 'mission.lvl'];

let failures = 0;
for (const rel of wanted) {
  const staged = stage(rel);
  if (!staged) continue;
  console.log(`\n=== ${rel} (${(staged.size / 1048576).toFixed(1)} MB)`);
  let seen = dumpLog(0).length;
  const rc = ccall('swbf2_probe_file', 'number', ['string'], [staged.dst]);
  console.log('  engine walk rc=' + rc + ': ' + ccall('swbf2_last_report', 'string'));
  seen = dumpLog(seen);
  if (rc !== 0) failures++;

  // real textures out of the container: decoding is the part that runs headless
  for (const idx of [0, 1, 2]) {
    const r = ccall('swbf2_show_texture', 'number', ['string', 'number'], [staged.dst, idx]);
    if (r === 0 || r === -10 - 1) console.log(`  texture[${idx}]: ${ccall('swbf2_last_report', 'string')}`);
    else break;
  }
  dumpLog(seen);
}

// real mission scripts, executed by the engine's own Lua
console.log('\n=== LuaFL executing scripts out of a mission container');
for (const rel of ['ingame.lvl', 'tat/tat2.lvl']) {
  const staged = stage(rel);
  if (!staged) continue;
  const ran = ccall('swbf2_run_scripts_from', 'number', ['string', 'number'], [staged.dst, 25]);
  console.log(`  ${rel}: swbf2_run_scripts_from -> ${ran} scripts executed`);
}
console.log('  lua calls total: ' + ccall('swbf2_lua_calls', 'number'));

// engine logic still steps at 30 Hz with no renderer attached
for (let i = 0; i < 120; i++) ccall('swbf2_frame', null, ['number'], [1 / 60]);
console.log(`\nframes=${ccall('swbf2_frames', 'number')} simTime=${ccall('swbf2_sim_time', 'number').toFixed(2)}s`);

// ---- model geometry, with no browser -------------------------------------
// The reader needs no renderer, so this half runs headless: parsing, the vertex decode, and the
// self-check in which the file answers for itself - the packed stream against the uncompressed copy
// of the same vertices that the same segment stores. Then the numbers are compared against
// tools/modl_probe.py, which parses the same container with a separate implementation. Two
// independent readers agreeing is evidence; one reader agreeing with itself is not.
const MODEL_CONTAINERS = ['tat/tat2.lvl', 'yav/yav1.lvl', 'cor/cor1.lvl'];
let modelFailures = 0;
console.log('\n=== model reader (modl/segm), headless');
for (const rel of MODEL_CONTAINERS) {
  const staged = stage(rel);
  if (!staged) continue;
  const segments = ccall('swbf2_build_models', 'number', ['string'], [staged.dst]);
  console.log('  ' + ccall('swbf2_last_report', 'string'));
  const stats = {
    models: ccall('swbf2_model_models', 'number'),
    segments,
    vertices: ccall('swbf2_model_vertices', 'number'),
    indices: ccall('swbf2_model_indices', 'number'),
    drawDeclared: ccall('swbf2_model_draw_declared', 'number'),
    instances: ccall('swbf2_model_instances', 'number'),
    unresolvedInstances: ccall('swbf2_model_unresolved_instances', 'number'),
    placedSegments: ccall('swbf2_model_placed_segments', 'number'),
    placedPrimTotal: ccall('swbf2_model_placed_prim_total', 'number'),
    placedIndexTotal: ccall('swbf2_model_placed_index_total', 'number'),
    oddPrimTypes: ccall('swbf2_model_odd_prim_types', 'number'),
    truncated: ccall('swbf2_model_truncated', 'number'),
    texturesResolved: ccall('swbf2_model_textures_resolved', 'number'),
  };

  let oracle = null;
  try {
    const out = execFileSync('python3', [path.join(root, 'tools/modl_probe.py'), '--json', rel], { encoding: 'utf8' });
    oracle = JSON.parse(out.trim().split('\n').pop());
  } catch (e) {
    console.log('  oracle unavailable: ' + e);
  }
  if (oracle) {
    const hv = oracle.host_view, inst = oracle.instances;
    const agree = stats.models === hv.models && stats.segments === hv.segments_with_stream &&
      stats.vertices === hv.vertices && stats.indices === hv.indices &&
      stats.drawDeclared === hv.draw_total && stats.instances === inst.inst_placeable_by_host &&
      stats.instances + stats.unresolvedInstances === inst.inst_through_walkable_wrld &&
      stats.placedSegments === inst.placed_segments && stats.placedPrimTotal === inst.placed_prim_total &&
      stats.placedIndexTotal === inst.placed_index_total && stats.oddPrimTypes === 0 &&
      oracle.strip_arithmetic_bad === 0;
    if (agree) {
      console.log(`  vs the python oracle: counts agree (${stats.segments} segments, ${stats.vertices} vertices, ` +
        `${stats.indices} indices, ${stats.drawDeclared} primitives declared, all triangle strips)`);
      if (inst.inst_under_unrecognised_group) {
        console.log(`  instances the host cannot reach: ${inst.inst_under_unrecognised_group}, all inside a group id ` +
          `the documented walk policy does not descend into - explained, not disagreed`);
      }
    } else {
      console.log('  vs the python oracle: DISAGREE ' + JSON.stringify({ c: stats, o: { hv, inst, strip_bad: oracle.strip_arithmetic_bad } }));
      modelFailures++;
    }
  }

  const bad = ccall('swbf2_model_selfcheck', 'number', ['string', 'number'], [staged.dst, 400]);
  console.log('  ' + ccall('swbf2_last_report', 'string'));
  if (bad) modelFailures++;
  if (segments <= 0) modelFailures++;
}

console.log('\n--- engine log tail ---');
dumpLog(0);
process.exit(failures || modelFailures ? 2 : 0);
