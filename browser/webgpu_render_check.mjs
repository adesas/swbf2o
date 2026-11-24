#!/usr/bin/env node
// Phase 2 Real Engine WebGPU Renderer Verification (Headless Chrome + Hardware Vulkan).
//
// Starts tools/serve.py, opens /browser/webgpu_render_check.html in headless Chrome with
// WebGPU enabled, waits for the Dedicated Worker to boot RedRenderer + FLRenderer + tat/tat2
// + rep_inf_ep3_rifleman over OPFS VFS, asserts all rendering & zero-error checks, and writes
// phase2_tat2_camA.png and phase2_tat2_camB.png for visual inspection.

import { spawn } from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import http from 'node:http';

const ROOT = path.resolve(path.dirname(new URL(import.meta.url).pathname), '..');
const PORT = 8129;
const CDP_PORT = 9249;
const OUT_DIR = process.env.SWBF2_SCREENSHOT_DIR ||
  path.join(ROOT, 'build', 'screenshots');
fs.mkdirSync(OUT_DIR, { recursive: true });

function sleep(ms) { return new Promise((r) => setTimeout(r, ms)); }

async function fetchJson(url) {
  return new Promise((resolve, reject) => {
    http.get(url, (res) => {
      let data = '';
      res.on('data', (c) => data += c);
      res.on('end', () => {
        try { resolve(JSON.parse(data)); } catch (e) { reject(e); }
      });
    }).on('error', reject);
  });
}

async function waitForServer(url, child, timeoutMs = 10000) {
  const end = Date.now() + timeoutMs;
  while (Date.now() < end) {
    try {
      await new Promise((res, rej) => {
        http.get(url, (r) => { r.resume(); res(); }).on('error', rej);
      });
      return;
    } catch {
      await sleep(100);
    }
  }
  const why = child && child.exitCode !== null
    ? ` (the static server exited with code ${child.exitCode} - the port is already taken, which usually means a previous run leaked tools/serve.py; kill it)`
    : '';
  throw new Error(`Timed out waiting for ${url}${why}`);
}

class CDP {
  constructor(wsUrl) {
    this.ws = new WebSocket(wsUrl);
    this.nextId = 1;
    this.pending = new Map();
    this.logs = [];
    this.ready = new Promise((resolve, reject) => {
      this.ws.addEventListener('open', resolve);
      this.ws.addEventListener('error', reject);
    });
    this.ws.addEventListener('message', (ev) => {
      const msg = JSON.parse(String(ev.data));
      if (msg.id && this.pending.has(msg.id)) {
        const { resolve, reject } = this.pending.get(msg.id);
        this.pending.delete(msg.id);
        if (msg.error) reject(new Error(msg.error.message));
        else resolve(msg.result);
      } else if (msg.method === 'Runtime.consoleAPICalled') {
        const txt = (msg.params.args || []).map((a) => a.value ?? a.description ?? '').join(' ');
        this.logs.push(`[console.${msg.params.type}] ${txt}`);
      } else if (msg.method === 'Runtime.exceptionThrown') {
        const d = msg.params.exceptionDetails;
        this.logs.push(`[exception] ${d.text} ${d.exception?.description || ''}`);
      }
    });
  }
  async send(method, params = {}) {
    await this.ready;
    const id = this.nextId++;
    return new Promise((resolve, reject) => {
      this.pending.set(id, { resolve, reject });
      this.ws.send(JSON.stringify({ id, method, params }));
    });
  }
  close() { try { this.ws.close(); } catch {} }
}

const server = spawn('python3', ['tools/serve.py', String(PORT), '--serve-gamedata'], {
  cwd: ROOT,
  stdio: ['ignore', 'pipe', 'pipe'],
});

let chrome = null;
let cdp = null;
const userDataDir = `/tmp/swbf2_phase2_chrome_${process.pid}`;

// process.exit() from inside the try below skips the finally block that reaps tools/serve.py and
// Chrome. An orphaned serve.py keeps the port, and the next run of this same check then "times
// out waiting for the server" - a failure that looks like an engine regression and is not one.
// Reap first, then exit.
function bail() {
  try { if (cdp) cdp.close(); } catch {}
  try { if (chrome) chrome.kill('SIGKILL'); } catch {}
  try { server.kill('SIGKILL'); } catch {}
  try { fs.rmSync(userDataDir, { recursive: true, force: true }); } catch {}
  process.exit(1);
}
fs.mkdirSync(userDataDir, { recursive: true });

try {
  await waitForServer(`http://127.0.0.1:${PORT}/browser/webgpu_render_check.html`, server);

  chrome = spawn('google-chrome-stable', [
    '--headless=new',
    '--no-sandbox',
    '--disable-crash-reporter',
    '--enable-unsafe-webgpu',
    '--enable-features=Vulkan',
    '--use-angle=vulkan',
    '--ignore-gpu-blocklist',
    '--disable-gpu-sandbox',
    `--remote-debugging-port=${CDP_PORT}`,
    `--user-data-dir=${userDataDir}`,
    'about:blank',
  ], { stdio: ['ignore', 'pipe', 'pipe'] });

  let chromeSpawnErr = null;
  chrome.on('error', (err) => { chromeSpawnErr = err; });
  let chromeStderr = '';
  chrome.stderr.on('data', (b) => { chromeStderr = (chromeStderr + b.toString()).slice(-3000); });
  let targets = null;
  for (let i = 0; i < 80; ++i) {
    if (chromeSpawnErr) throw chromeSpawnErr;
    try {
      targets = await fetchJson(`http://127.0.0.1:${CDP_PORT}/json/list`);
      if (Array.isArray(targets) && targets.some((t) => t.type === 'page')) break;
    } catch {}
    await sleep(100);
  }
  const page = targets?.find((t) => t.type === 'page');
  if (!page?.webSocketDebuggerUrl) throw new Error('Chrome CDP page target not found');

  cdp = new CDP(page.webSocketDebuggerUrl);
  await cdp.send('Runtime.enable');
  await cdp.send('Page.enable');
  await cdp.send('Page.navigate', { url: `http://127.0.0.1:${PORT}/browser/webgpu_render_check.html` });

  let result = null;
  const deadline = Date.now() + 90000;
  while (Date.now() < deadline) {
    const ev = await cdp.send('Runtime.evaluate', {
      expression: 'window.__phase2Result ? JSON.stringify(window.__phase2Result) : ""',
      returnByValue: true,
    });
    if (ev.result?.value) {
      result = JSON.parse(ev.result.value);
      break;
    }
    await sleep(250);
  }

  if (!result) {
    console.error('Timed out waiting for __phase2Result. Recent logs:\n' + cdp.logs.slice(-40).join('\n'));
    bail();
  }

  // Save PNG screenshots if produced (Phase 3 filenames only; do not overwrite Phase 2 artifacts).
  for (const [varName, fileNames] of [
    ['window.__phase2PngA', ['phase3_tat2_camA.png']],
    ['window.__phase2PngB', ['phase3_tat2_camB.png']],
  ]) {
    const ev = await cdp.send('Runtime.evaluate', { expression: varName, returnByValue: true });
    const dataUrl = ev.result?.value;
    if (typeof dataUrl === 'string' && dataUrl.startsWith('data:image/png;base64,')) {
      const b64 = dataUrl.slice('data:image/png;base64,'.length);
      const buf = Buffer.from(b64, 'base64');
      for (const fileName of fileNames) {
        const outPath = path.join(OUT_DIR, fileName);
        fs.writeFileSync(outPath, buf);
        console.log(`saved screenshot: ${outPath}`);
      }
    }
  }

  console.log(JSON.stringify(result, null, 2));

  const checks = [
    ['result.ok', result.ok === true],
    ['crossOriginIsolated', result.crossOriginIsolated === true],
    ['wgpuReady.ok', result.wgpuReady?.ok === true],
    ['initOk', result.initOk === true],
    ['loadOk', result.loadOk === true],
    ['pipeline === 1 (PIPELINE_PROG1)', result.stats?.pipeline === 1],
    ['geometryMode === 1 (GEOMETRYMODE_COMPRESSED)', result.stats?.geometryMode === 1],
    ['skinningMode === 1 (SKINNING_HARDWARE)', result.stats?.skinningMode === 1],
    ['all 2332 vs_1_1 shaders compiled cleanly in Dawn/Tint', result.shaderValidation?.vsTotal === 2332 && result.shaderValidation?.vsCompiled === 2332],
    ['all 226 ps_1_1 shaders compiled cleanly in Dawn/Tint', result.shaderValidation?.psTotal === 226 && result.shaderValidation?.psCompiled === 226],
    ['shaderValidation.shaderCompileErrors === 0', result.shaderValidation?.shaderCompileErrors === 0],
    ['texturesLoaded > 100', (result.stats?.texturesLoaded || 0) > 100],
    ['terrainLayers > 0', (result.stats?.terrainLayers || 0) > 0],
    ['trooperSpawned', result.stats?.trooperSpawned === true],
    ['frameA.draws > 50', (result.frameA?.draws || 0) > 50],
    ['frameA.triangles > 1000', (result.frameA?.triangles || 0) > 1000],
    ['frameA.skinnedDraws > 0 (trooper hardware-skinned draws)', (result.frameA?.skinnedDraws || 0) > 0],
    ['frameA.skinnedTriangles > 100 (trooper skinned geometry)', (result.frameA?.skinnedTriangles || 0) > 100],
    ['trooperDiffPct > 0.2 (trooper owns visible pixels)', (result.trooperDiffPct || 0) > 0.2],
    ['frameA.wgpuErrors === 0', result.frameA?.wgpuErrors === 0],
    ['frameB.wgpuErrors === 0', result.frameB?.wgpuErrors === 0],
    ['frameA.nonBlackPct > 40', (result.frameA?.nonBlackPct || 0) > 40],
    ['frameA.distinctColors12bit > 50', (result.frameA?.distinctColors12bit || 0) > 50],
    ['frameB.nonBlackPct > 40', (result.frameB?.nonBlackPct || 0) > 40],
    ['frameDiffPct > 10', (result.frameDiffPct || 0) > 10],
  ];

  let passed = 0;
  for (const [name, ok] of checks) {
    console.log(`${ok ? 'PASS' : 'FAIL'}: ${name}`);
    if (ok) ++passed;
  }
  console.log(`=== Phase 3 WebGPU Render Check: ${passed} / ${checks.length} passed ===`);
  if (passed !== checks.length) {
    console.error('Recent browser logs:\n' + cdp.logs.slice(-60).join('\n'));
    console.error('Chrome stderr tail:\n' + chromeStderr);
    bail();
  }
} finally {
  if (cdp) cdp.close();
  if (chrome) chrome.kill('SIGKILL');
  server.kill('SIGKILL');
  fs.rmSync(userDataDir, { recursive: true, force: true });
}
