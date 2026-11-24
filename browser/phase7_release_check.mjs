#!/usr/bin/env node
// Phase 7 Production Release Verification:
// Verifies the -O2 -ffp-contract=off -msimd128 release WASM build + production browser/play.html
// launcher in Headless Chrome + Hardware Vulkan:
//   1. Binary size & gzip measurement of build/browser/swbf2.wasm
//   2. coi-serviceworker.js served & syntactically valid
//   3. play.html preflight checks (WebGPU BCn, COOP/COEP, OPFS, 48 kHz WebAudio)
//   4. Live Multiplayer Room Browser displays active rooms from tools/multiplayer_server.mjs
//   5. Start-gated Launch boots Shell, DOM Enter persists .profile into OPFS
//   6. OPFS Save Export -> Clear Saves -> Import Saves round-trip restores .profile intact
//   7. Live Resolution Resize (1024x768 -> 1280x720) with 0 WebGPU errors & steady FPS
//   8. OPFS Clear Asset Cache cleanly zeroes cachedBlocks

import { spawn } from 'node:child_process';
import fs from 'node:fs';
import zlib from 'node:zlib';
import path from 'node:path';
import http from 'node:http';

const ROOT = path.resolve(path.dirname(new URL(import.meta.url).pathname), '..');
const HTTP_PORT = 8136;
const MP_PORT = 8137;
const CDP_PORT = 9600 + (process.pid % 200);
const OUT_DIR = process.env.SWBF2_SCREENSHOT_DIR ||
  path.join(ROOT, 'build', 'screenshots');
fs.mkdirSync(OUT_DIR, { recursive: true });

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

function fetchJson(url) {
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

async function waitForHttp(url, timeoutMs = 10000) {
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
  throw new Error(`Timed out waiting for ${url}`);
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
  async evalAsync(expr) {
    const res = await this.send('Runtime.evaluate', {
      expression: `(async () => { return JSON.stringify(await (${expr})); })()`,
      awaitPromise: true,
      returnByValue: true,
    });
    if (res.exceptionDetails) {
      throw new Error(`eval failed: ${res.exceptionDetails.exception?.description || res.exceptionDetails.text}`);
    }
    return res.result?.value ? JSON.parse(res.result.value) : null;
  }
  close() { try { this.ws.close(); } catch {} }
}

const wasmPath = path.join(ROOT, 'build/browser/swbf2.wasm');
const wasmBuf = fs.readFileSync(wasmPath);
const wasmGzipBytes = zlib.gzipSync(wasmBuf, { level: 9 }).length;

const httpServer = spawn('python3', ['tools/serve.py', String(HTTP_PORT), '--serve-gamedata'], { cwd: ROOT, stdio: ['ignore', 'pipe', 'pipe'] });
const mpServer = spawn(process.execPath, ['tools/multiplayer_server.mjs', String(MP_PORT), '--host', '127.0.0.1'], { cwd: ROOT, stdio: ['ignore', 'pipe', 'pipe'] });

let chrome = null;
let cdp = null;
let dummyRoomWs = null;
const userDataDir = `/tmp/swbf2_phase7_chrome_${process.pid}`;
fs.mkdirSync(userDataDir, { recursive: true });

function cleanup() {
  try { dummyRoomWs?.close(); } catch {}
  try { cdp?.close(); } catch {}
  try { chrome?.kill('SIGKILL'); } catch {}
  try { httpServer.kill('SIGKILL'); } catch {}
  try { mpServer.kill('SIGKILL'); } catch {}
  try { fs.rmSync(userDataDir, { recursive: true, force: true }); } catch {}
}

try {
  await waitForHttp(`http://127.0.0.1:${HTTP_PORT}/browser/play.html`);
  await waitForHttp(`http://127.0.0.1:${MP_PORT}/api/health`);

  // Register one live room on the signaling server so play.html's room browser displays it.
  dummyRoomWs = new WebSocket(`ws://127.0.0.1:${MP_PORT}/ws?room=galaxies_live&role=host&name=ServerOne&map=tat2g_con`);
  await new Promise((res, rej) => {
    dummyRoomWs.addEventListener('open', res);
    dummyRoomWs.addEventListener('error', rej);
  });
  await sleep(150);

  // Verify coi-serviceworker.js is served cleanly.
  const coiRes = await fetch(`http://127.0.0.1:${HTTP_PORT}/browser/coi-serviceworker.js`);
  const coiOk = coiRes.ok && (await coiRes.text()).includes('Cross-Origin-Embedder-Policy');

  chrome = spawn('google-chrome-stable', [
    '--headless=new',
    '--no-sandbox',
    '--disable-crash-reporter',
    '--autoplay-policy=no-user-gesture-required',
    '--window-size=1280,820',
    '--enable-unsafe-webgpu',
    '--enable-features=Vulkan',
    '--use-angle=vulkan',
    '--ignore-gpu-blocklist',
    '--disable-gpu-sandbox',
    `--remote-debugging-port=${CDP_PORT}`,
    `--user-data-dir=${userDataDir}`,
    'about:blank',
  ], { stdio: ['ignore', 'pipe', 'pipe'] });

  let chromeErr = null;
  chrome.on('error', (e) => { chromeErr = e; });
  let targets = null;
  for (let i = 0; i < 80; ++i) {
    if (chromeErr) throw chromeErr;
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
  const mpParam = encodeURIComponent(`ws://127.0.0.1:${MP_PORT}`);
  await cdp.send('Page.navigate', { url: `http://127.0.0.1:${HTTP_PORT}/browser/play.html?mp=${mpParam}` });

  // Wait for preflight + room list.
  let state0 = null;
  for (let i = 0; i < 60; ++i) {
    state0 = await cdp.evalAsync('window.__playApi ? window.__playApi.getState() : null');
    if (state0?.preflight && state0.rooms.length > 0) break;
    await sleep(150);
  }

  // Capture screenshot of the production launcher UI (Main View + Multiplayer Lobby View)!
  const launcherShot = await cdp.send('Page.captureScreenshot', { format: 'png' });
  fs.writeFileSync(path.join(OUT_DIR, 'phase7_launcher.png'), Buffer.from(launcherShot.data, 'base64'));
  console.log(`saved screenshot: ${path.join(OUT_DIR, 'phase7_launcher.png')}`);

  await cdp.evalAsync('window.__playApi.showLobbyView && window.__playApi.showLobbyView()');
  await sleep(150);
  const lobbyShot = await cdp.send('Page.captureScreenshot', { format: 'png' });
  fs.writeFileSync(path.join(OUT_DIR, 'phase7_lobbies.png'), Buffer.from(lobbyShot.data, 'base64'));
  console.log(`saved screenshot: ${path.join(OUT_DIR, 'phase7_lobbies.png')}`);
  await cdp.evalAsync('window.__playApi.showMainView && window.__playApi.showMainView()');

  // Launch Game (Main Menu) via the start-gated launcher button!
  await cdp.evalAsync('window.__playApi.launch()');
  await sleep(1200);

  // Press DOM Enter on ifs_login so a real .profile is saved into OPFS!
  await cdp.evalAsync(`(async () => {
    for (let i = 0; i < 3; ++i) {
      window.dispatchEvent(new KeyboardEvent('keydown', { code: 'Enter', bubbles: true, cancelable: true }));
      await new Promise((r) => setTimeout(r, 120));
      window.dispatchEvent(new KeyboardEvent('keyup', { code: 'Enter', bubbles: true, cancelable: true }));
      await new Promise((r) => setTimeout(r, 500));
      const s = window.__playApi.getState();
      if (s.status?.status?.currentScreen === 'ifs_sp_campaign' || s.status?.status?.currentScreen === 'ifs_main') break;
    }
    return window.__playApi.getState();
  })()`);

  // Test Save Profile Export -> Clear Saves -> Import Saves round-trip!
  const saveRoundTrip = await cdp.evalAsync(`(async () => {
    const exp = await window.__playApi.callWorker('vfsOp', { op: 'exportSaves' });
    const beforeCount = exp.bundle.files.length;
    const hasProfile = exp.bundle.files.some((f) => f.name.endsWith('.profile'));
    const cleared = await window.__playApi.callWorker('vfsOp', { op: 'clearSaves' });
    const afterClearCount = (cleared.vfsStats.saveFiles || []).length;
    const imp = await window.__playApi.callWorker('vfsOp', { op: 'importSaves', bundle: exp.bundle });
    const afterImportFiles = imp.vfsStats.saveFiles || [];
    return {
      beforeCount,
      hasProfile,
      afterClearCount,
      imported: imp.imported,
      restoredProfile: afterImportFiles.some((f) => f.endsWith('.profile')),
    };
  })()`);

  // Test live Resolution Resize (1024x768 -> 1280x720) & measure steady FPS!
  await cdp.evalAsync(`window.__playApi.callWorker('interactiveAction', { action: 'resize', width: 1280, height: 720 })`);
  await sleep(800);
  const afterResize = await cdp.evalAsync('window.__playApi.getState()');

  // Test OPFS Clear Asset Cache!
  const cacheCleared = await cdp.evalAsync(`window.__playApi.callWorker('vfsOp', { op: 'clearCache' })`);
  await cdp.evalAsync(`window.__playApi.callWorker('stopInteractive', { closeHandlesOnExit: true })`);

  const summary = {
    releaseBinary: {
      wasmRawMiB: +(wasmBuf.length / 1048576).toFixed(2),
      wasmGzipMiB: +(wasmGzipBytes / 1048576).toFixed(2),
    },
    coiServiceWorkerOk: coiOk,
    preflight: state0?.preflight,
    roomsDiscovered: state0?.rooms,
    bootMs: afterResize?.bootMs,
    fps: afterResize?.fps,
    screenAfterLogin: afterResize?.status?.status?.currentScreen,
    wgpuErrors: afterResize?.status?.status?.wgpuErrors,
    saveRoundTrip,
    cachedBlocksAfterClear: cacheCleared?.vfsStats?.cachedBlocks,
  };
  console.log(JSON.stringify(summary, null, 2));

  const checks = [
    ['Release swbf2.wasm gzipped size < 4.5 MiB', summary.releaseBinary.wasmGzipMiB < 4.5],
    ['coi-serviceworker.js served cleanly with COEP headers', coiOk === true],
    ['Launcher preflight detected WebGPU, COOP/COEP, OPFS, and WebAudio',
      state0?.preflight?.webgpu === true && state0?.preflight?.coi === true &&
      state0?.preflight?.opfs === true && state0?.preflight?.audio === true],
    ['Launcher Room Browser discovered live room from multiplayer_server.mjs',
      Array.isArray(state0?.rooms) && state0.rooms.some((r) => r.id === 'galaxies_live')],
    ['Release build booted Shell in < 5,000 ms', (afterResize?.bootMs || 99999) < 5000],
    ['DOM Enter logged into PC Main Menu on release build',
      summary.screenAfterLogin === 'ifs_sp_campaign' || summary.screenAfterLogin === 'ifs_main'],
    ['OPFS Save Export -> Clear -> Import restored .profile intact',
      saveRoundTrip.hasProfile === true && saveRoundTrip.afterClearCount === 0 && saveRoundTrip.restoredProfile === true],
    ['Live WebGPU resolution resize (1280x720) succeeded with 0 WebGPU errors',
      (summary.wgpuErrors || 0) === 0 && (afterResize?.fps || 0) >= 20],
    ['OPFS Clear Asset Cache cleanly reset cachedBlocks to 0', summary.cachedBlocksAfterClear === 0],
  ];

  let passed = 0;
  for (const [name, ok] of checks) {
    console.log(`${ok ? 'PASS' : 'FAIL'}: ${name}`);
    if (ok) ++passed;
  }
  console.log(`=== Phase 7 Production Release & Launcher Check: ${passed} / ${checks.length} passed ===`);
  if (passed !== checks.length) {
    console.error('Recent logs:\n' + cdp.logs.slice(-40).join('\n'));
    process.exitCode = 1;
  }
} catch (err) {
  console.error(err);
  if (cdp?.logs.length) console.error('Recent logs:\n' + cdp.logs.slice(-40).join('\n'));
  process.exitCode = 1;
} finally {
  cleanup();
}
