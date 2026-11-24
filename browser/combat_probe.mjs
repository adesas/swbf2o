// The browser half of the combat oracle.
//
//   node browser/combat_probe.mjs tat/tat2 rep_inf_ep3_rifleman cis_inf_rifleman 4
//
// It runs the same harness (spike/combat_harness.cpp) and the same fight script
// (CombatHarnessRun) inside the wasm module that the native oracle's wasm half is built from,
// and prints the report the harness built. tools/combat_oracle_check.py compares that report
// with the native build's, byte for byte: the two builds have to fight the same fight to the
// same numbers.
//
// The module under test is build/combat_oracle/swbf2_combat.js by default, or whatever
// SWBF2_MODULE points at.
//
// The files are staged into the wasm VFS the same way the browser host stages them. The two
// faction classes are staged from the extracted class records (tools/extract_class_level.py):
// the record is the same bytes the 100 MB side file holds, and the name the engine is given is
// the same name the native build is given ("side/rep.lvl;rep_inf_ep3_rifleman"), so both builds
// read the same class out of a file with the same name.
import fs from 'fs';
import path from 'path';
import url from 'url';

const here = path.dirname(url.fileURLToPath(import.meta.url));
const root = path.join(here, '..');
const GAME = path.join(root, 'data/Star wars battlefront 2/GameData/data/_lvl_pc');
const modulePath = process.env.SWBF2_MODULE || path.join(root, 'build/combat_oracle/swbf2_combat.js');
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

try { Module.FS.mkdir('/data'); } catch (e) { /* already there */ }

// The name the engine is given, and the file that holds those bytes. The engine's own path
// translation flattens the separators ("side/rep.lvl" becomes "side_rep.lvl"), so the staged
// name is the flattened one and the engine finds it under the name it was given.
const staged = [
  ['common.lvl', path.join(GAME, 'common.lvl')],
  ['core.lvl', path.join(GAME, 'core.lvl')],
  ['ingame.lvl', path.join(GAME, 'ingame.lvl')],
  ['mission.lvl', path.join(GAME, 'mission.lvl')],
  ['tat/tat2.lvl', path.join(GAME, 'tat/tat2.lvl')],
  ['side/rep.lvl', path.join(root, 'build/class_data/side/rep/rep_inf_ep3_rifleman.lvl')],
  ['side/cis.lvl', path.join(root, 'build/class_data/side/cis/cis_inf_rifleman.lvl')],
];

for (const [name, src] of staged) {
  if (!fs.existsSync(src)) {
    console.error(`missing ${src}: run tools/extract_class_level.py first`);
    process.exit(1);
  }
  Module.FS.writeFile('/data/' + name.replace(/\//g, '_'), fs.readFileSync(src));
}

const level = process.argv[2] || 'tat/tat2.lvl';
const playerClass = process.argv[3] || 'rep_inf_ep3_rifleman';
const playerClassFile = process.argv[4] || `side/rep.lvl;${playerClass}`;
const targetClass = process.argv[5] || 'cis_inf_rifleman';
const targetClassFile = process.argv[6] || `side/cis.lvl;${targetClass}`;
const targets = parseInt(process.argv[7] || '4', 10);

const rc = Module.ccall('swbf2_combat_run', 'number',
  ['string', 'string', 'string', 'string', 'string', 'number'],
  [level, playerClass, playerClassFile, targetClass, targetClassFile, targets]);
if (rc !== 1) {
  console.error(`swbf2_combat_run -> ${rc}`);
  process.exit(2);
}

// The report the harness built, printed in the same shape the native oracle prints it, so that
// the check script can compare the two builds' bytes.
console.log('COMBAT_ORACLE_JSON ' + Module.ccall('swbf2_combat_report', 'string', [], []));
