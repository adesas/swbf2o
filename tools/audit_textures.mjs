// Texture format audit, headless.
//
// The browser half can only show what the port can actually read out of the game's containers,
// so this walks every container it is pointed at, decodes every texture in it, and reports
// coverage per encoding. It runs the same wasm module the page runs, minus WebGL: the point is
// to measure the reader, not the renderer.
//
//   node tools/audit_textures.mjs [name.lvl ...]     (defaults to a size-stratified sample)
//   node tools/audit_textures.mjs --all              (every container, slow)
import fs from 'fs';
import path from 'path';
import { createRequire } from 'module';
import { fileURLToPath } from 'url';

const here = path.dirname(fileURLToPath(import.meta.url));
const root = path.join(here, '..');
const GAME = path.join(root, 'data/Star wars battlefront 2/GameData/data/_lvl_pc');

const argv = process.argv.slice(2);
const all = argv.includes('--all');
const named = argv.filter((a) => !a.startsWith('--'));

const manifest = JSON.parse(fs.readFileSync(path.join(root, 'browser/manifest.json'), 'utf8'));
const pick = (f) => f.size <= 80 * 1048576;
let files = named.length
  ? manifest.filter((f) => named.some((n) => f.path === n || f.path.endsWith('/' + n)))
  : all ? manifest.filter(pick)
  : [
      ...manifest.filter(pick).slice(0, 4),
      ...manifest.filter((f) => f.size > 4 * 1048576 && f.size < 8 * 1048576).slice(0, 4),
      ...manifest.filter((f) => f.size > 20 * 1048576 && f.size < 60 * 1048576).slice(0, 4),
    ];
files = [...new Map(files.map((f) => [f.path, f])).values()];

process.stdout.write(`loading wasm module… `);
const { default: SWBF2 } = await import(path.join(root, 'build/browser/swbf2.js'));
const M = await SWBF2({
  locateFile: (f) => path.join(root, 'build/browser', f),
  print: () => {},
  printErr: () => {},
});
M.FS.mkdir('/data');
M.ccall('swbf2_init', 'number', ['number'], [0]);  // 0 = no renderer here
process.stdout.write(`ok\n`);

const report = () => M.ccall('swbf2_last_report', 'string') || '';
const read = (p) => new Uint8Array(fs.readFileSync(p));

const formats = new Map();      // encoding name -> decoded count
const failures = new Map();     // reason -> count
let containers = 0, seen = 0, decoded = 0, bytes = 0;

for (const f of files) {
  const fsPath = '/data/' + f.path.replace(/\//g, '_');
  M.FS.writeFile(fsPath, read(path.join(GAME, f.path)));
  containers++;

  M.ccall('swbf2_probe_file', 'number', ['string'], [fsPath]);
  const rep = report();
  const m = rep.match(/(\d+) tex_/);
  const texCount = m ? +m[1] : 0;
  if (!texCount) {
    process.stdout.write(`  ${f.path.padEnd(34)} no textures\n`);
    M.FS.unlink(fsPath);
    continue;
  }

  let local = 0, localFail = 0, miss = 0;
  for (let i = 0; i < texCount + 16; ++i) {
    const rc = M.ccall('swbf2_decode_texture', 'number', ['string', 'number'], [fsPath, i]);
    if (rc > 0) {
      const name = M.ccall('swbf2_texture_format_name', 'string') || '?';
      formats.set(name, (formats.get(name) || 0) + 1);
      local++; decoded++; bytes += rc; seen++; miss = 0;
    } else if (rc === -2) {
      const why = report().replace(/'[^']*'/g, "'…'").replace(/\/data\/\S+/g, '…');
      const key = why.length > 90 ? why.slice(0, 90) : why;
      failures.set(key, (failures.get(key) || 0) + 1);
      localFail++; seen++; miss = 0;
    } else {
      seen++;
      if (++miss >= 3) break;  // index past the end of this container
    }
  }
  process.stdout.write(
    `  ${f.path.padEnd(34)} ${String(local).padStart(4)}/${String(texCount).padEnd(4)} decoded` +
    `${localFail ? `  ${localFail} unsupported` : ''}\n`);
  M.FS.unlink(fsPath);
}

console.log('\nencodings decoded:');
for (const [k, v] of [...formats].sort((a, b) => b[1] - a[1])) console.log(`  ${k.padEnd(14)} ${v}`);
if (failures.size) {
  console.log('\nnot decodable here:');
  for (const [k, v] of [...failures].sort((a, b) => b[1] - a[1]).slice(0, 8))
    console.log(`  x${String(v).padEnd(5)} ${k}`);
}
console.log(`\n${containers} containers, ${decoded} textures decoded of ${seen} examined, ` +
            `${(bytes / 1048576).toFixed(1)} MB of pixel data`);
