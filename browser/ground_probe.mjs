// The browser half of the ground oracle.
//
//   node browser/ground_probe.mjs tat/tat2
//
// It loads a level through the engine's own loader and then drops a capsule at the same points
// the native oracle uses, printing the result in the same JSON shape. The two outputs are
// compared by tools/ground_oracle_check.py: if the engine's terrain and collision code put the
// capsule in a different place in the two builds, the numbers differ and the check fails.
//
// The points are the ones the native oracle has hard-coded (spike/native/native_main_ground_oracle.cpp):
// a grid across the world's extent.
import fs from 'fs';
import path from 'path';
import url from 'url';

const here = path.dirname(url.fileURLToPath(import.meta.url));
const root = path.join(here, '..');
const GAME = path.join(root, 'data/Star wars battlefront 2/GameData/data/_lvl_pc');
// The module under test: the browser build by default, or whatever SWBF2_MODULE points at.
const modulePath = process.env.SWBF2_MODULE || path.join(root, 'build/browser/swbf2.js');
const factory = (await import(url.pathToFileURL(modulePath).href)).default;

const Module = await factory({
  locateFile: (f) => path.join(path.dirname(modulePath), f),
  onAbort: (why) => { console.error('ABORT:', why); process.exit(3); },
});

process.on('exit', () => {
  try {
    let i = 0;
    for (;;) {
      const line = Module.ccall('swbf2_log_line', 'string', ['number'], [i]);
      if (!line) break;
      console.error('  | ' + line);
      i++;
      if (i > 500) break;
    }
  } catch (e) { /* the module may already be gone */ }
});
const ccall = (...a) => Module.ccall(...a);

ccall('swbf2_init', 'number', ['number'], [0]);
try { Module.FS.mkdir('/data'); } catch (e) { /* already there */ }

function stage(rel) {
  const src = path.join(GAME, rel);
  if (!fs.existsSync(src)) { console.error(`skip ${rel}: not found`); return null; }
  const dst = '/data/' + rel.replace(/\//g, '_');
  Module.FS.writeFile(dst, fs.readFileSync(src));
  return dst;
}

for (const rel of ['common.lvl', 'ingame.lvl', 'tat/tat2.lvl', 'mission.lvl']) {
  stage(rel);
}

const level = process.argv[2] || 'tat/tat2';
const rc = ccall('swbf2_load_level', 'number', ['string'], [level]);
if (rc !== 1) {
  console.error(`swbf2_load_level(${level}) -> ${rc}`);
  process.exit(1);
}

// The same grid the native oracle uses: the origin and rings out to 100 units.
const points = [
  [0, 0], [10, 0], [-10, 0], [0, 10], [0, -10],
  [25, 25], [-25, 25], [25, -25], [-25, -25],
  [50, 0], [0, 50], [-50, 0], [0, -50],
  [100, 100], [-100, 100], [100, -100], [-100, -100],
];

const samples = [];
for (const [x, z] of points) {
  const out = Module._malloc(24);
  const terrain = Module._malloc(4);
  const ground = Module._malloc(4);
  const rest = Module._malloc(4);
  const steps = Module._malloc(4);
  const ok = ccall('swbf2_ground_probe_result', 'number',
    ['number', 'number', 'number', 'number', 'number', 'number'],
    [x, z, terrain, ground, rest, steps]);
  samples.push({
    x, z, ok: ok === 1,
    terrain: Module.getValue(terrain, 'float'),
    ground: Module.getValue(ground, 'float'),
    rest: Module.getValue(rest, 'float'),
    steps: Module.getValue(steps, 'i32'),
  });
  for (const p of [out, terrain, ground, rest, steps]) Module._free(p);
}

// The marker tells the check script which line is the result: the module's own prints share
// stdout with it.
console.log('GROUND_ORACLE_JSON ' + JSON.stringify({
  level,
  collision_meshes: ccall('swbf2_collision_meshes', 'number'),
  samples,
}));
