// The browser half of the spawn oracle.
//
//   node browser/spawn_probe.mjs tat/tat2 cis_inf_rifleman
//
// It loads a level through the engine's own loader, loads the soldier's class the way the
// mission script loads one, spawns the soldier at the ground the engine's own ray query finds,
// and runs the engine's own update loop. The result is printed in the same JSON shape the
// native oracle prints, and the two are compared by tools/spawn_oracle_check.py: if the two
// builds build the soldier differently, the numbers differ and the check fails.
//
// The files are staged into the wasm VFS the same way the browser host stages them. The class
// file is the big one: side/cis.lvl is a hundred megabytes, and the harness reads only the one
// nested level it needs out of it.
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
  // A relative name comes from the game data dump. An absolute name is an extracted class
  // record (tools/extract_class_level.py): stage it at the same absolute VFS path, so the
  // engine's own "file;class" name resolves identically in the two builds.
  const src = path.isAbsolute(rel) ? rel : path.join(GAME, rel);
  if (!fs.existsSync(src)) { console.error(`skip ${rel}: not found`); return null; }
  const dst = path.isAbsolute(rel) ? rel : '/data/' + rel.replace(/\//g, '_');
  if (path.isAbsolute(dst)) Module.FS.mkdirTree(path.dirname(dst));
  Module.FS.writeFile(dst, fs.readFileSync(src));
  return dst;
}

for (const rel of ['common.lvl', 'core.lvl', 'ingame.lvl', 'tat/tat2.lvl', 'mission.lvl']) {
  stage(rel);
}

const level = process.argv[2] || 'tat/tat2';
const className = process.argv[3] || 'cis_inf_rifleman';
// The class file: the name is "file;class", the form the engine's Lua callback uses.
const classFile = process.argv[4] || `side/cis.lvl;${className}`;
const [classPath] = classFile.split(';');
stage(classPath);

const rc = ccall('swbf2_load_level', 'number', ['string'], [level]);
if (rc !== 1) {
  console.error(`swbf2_load_level(${level}) -> ${rc}`);
  process.exit(1);
}

// The base class the soldier class is derived from. The engine's own chain is
// soldier -> com_inf_default -> cis_inf_default -> cis_inf_default_rifleman ->
// cis_inf_rifleman, and the first link of it lives in ingame.lvl.
if (ccall('swbf2_load_class', 'number', ['string'], ['ingame.lvl;com_inf_default']) !== 1) {
  console.error('swbf2_load_class(ingame.lvl;com_inf_default) failed');
  process.exit(1);
}
if (ccall('swbf2_load_class', 'number', ['string'], [classFile]) !== 1) {
  console.error(`swbf2_load_class(${classFile}) failed`);
  process.exit(1);
}

// The ground the engine's own collision query finds under the spawn point: the drop probe is
// the engine's own ray query, so the spawn point is the engine's ground rather than a number
// the probe made up.
const restPtr = Module._malloc(4);
ccall('swbf2_ground_probe_result', 'number',
  ['number', 'number', 'number', 'number', 'number', 'number'],
  [0, 0, 0, 0, restPtr, 0]);
const groundRest = Module.getValue(restPtr, 'float');
Module._free(restPtr);

const xPtr = Module._malloc(4);
const yPtr = Module._malloc(4);
const zPtr = Module._malloc(4);
const hpPtr = Module._malloc(4);
const mhpPtr = Module._malloc(4);
const classWeaponsPtr = Module._malloc(4);
const builtWeaponsPtr = Module._malloc(4);
const bodiesPtr = Module._malloc(4);
const spawned = ccall('swbf2_spawn_soldier', 'number',
  ['string', 'number', 'number', 'number', 'number', 'number', 'number', 'number', 'number',
   'number', 'number', 'number', 'number'],
  [className, 0, groundRest, 0, 0,
   xPtr, yPtr, zPtr, hpPtr, mhpPtr, classWeaponsPtr, builtWeaponsPtr, bodiesPtr]);

// The engine's own updates: the soldier's physics runs on the turn thread, and the game's own
// loop advances those threads with Thread::UpdateAll.
const updates = 60;
for (let i = 0; i < updates; i++) ccall('swbf2_update', 'number', ['number'], [1 / 30]);

const result = {
  level,
  class: className,
  class_file: classFile,
  spawned: spawned === 1,
  ground_rest: groundRest,
  x: Module.getValue(xPtr, 'float'),
  y: Module.getValue(yPtr, 'float'),
  z: Module.getValue(zPtr, 'float'),
  health: Module.getValue(hpPtr, 'float'),
  max_health: Module.getValue(mhpPtr, 'float'),
  class_weapons: Module.getValue(classWeaponsPtr, 'i32'),
  built_weapons: Module.getValue(builtWeaponsPtr, 'i32'),
  collision_bodies: Module.getValue(bodiesPtr, 'i32'),
  updates,
};
for (const p of [xPtr, yPtr, zPtr, hpPtr, mhpPtr, classWeaponsPtr, builtWeaponsPtr, bodiesPtr]) {
  Module._free(p);
}

// The marker tells the check script which line is the result: the module's own prints share
// stdout with it.
console.log('SPAWN_ORACLE_JSON ' + JSON.stringify(result));
