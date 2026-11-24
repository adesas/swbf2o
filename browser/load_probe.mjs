// Scratch: load a level through the engine's own loader and report what the engine holds.
//   node browser/load_probe.mjs tat/tat2
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
  onExit: (status) => { console.log('module exit status ' + status); },
});

process.on('exit', () => {
  try {
    console.log('--- engine log at exit ---');
    let i = 0;
    for (;;) {
      const line = Module.ccall('swbf2_log_line', 'string', ['number'], [i]);
      if (!line) break;
      console.log('  | ' + line);
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
  if (!fs.existsSync(src)) { console.log(`skip ${rel}: not found`); return null; }
  const size = fs.statSync(src).size;
  const dst = '/data/' + rel.replace(/\//g, '_');
  Module.FS.writeFile(dst, fs.readFileSync(src));
  return { dst, src, size };
}

function dumpLog(since) {
  let i = 0;
  for (;;) {
    const line = Module.ccall('swbf2_log_line', 'string', ['number'], [i]);
    if (!line) break;
    if (i >= since) console.log('  | ' + line);
    i++;
    if (i > 2000) break;
  }
  return i;
}

for (const rel of ['common.lvl', 'ingame.lvl', 'tat/tat2.lvl', 'mission.lvl']) {
  const s = stage(rel);
  if (s) console.log(`staged ${rel} -> ${s.dst} (${(s.size / 1048576).toFixed(1)} MB)`);
}

const level = process.argv[2] || 'tat/tat2';
let seen = dumpLog(0).length;
const rc = ccall('swbf2_load_level', 'number', ['string'], [level]);
console.log(`swbf2_load_level(${level}) -> ${rc}`);
seen = dumpLog(seen);
console.log('collision meshes in the engine table: ' + ccall('swbf2_collision_meshes', 'number'));
console.log('--- engine log tail ---');
dumpLog(Math.max(0, seen - 25));
