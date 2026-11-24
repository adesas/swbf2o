#!/usr/bin/env node
// End-to-End Bring-Your-Own-Copy (BYOC) Verification:
//   1. Starts tools/serve.py in DEFAULT BYOC mode (WITHOUT --serve-gamedata).
//   2. Verifies /api/server-config reports { gamedataHosted: false, mode: "byoc" }.
//   3. Verifies /GameData/core.lvl and /browser/gamedata-manifest.json both return HTTP 404.
//   4. Opens /play in Headless Chrome + WebGPU, confirms BYOC card is shown and launching
//      without GameData is cleanly gated.
//   5. Feeds real local GameData files into #fileByocFolder via CDP DOM.setFileInputFiles
//      (exact user <input webkitdirectory> codepath -> File objects -> Worker FileReaderSync).
//   6. Launches Mos Eisley (tat2g_con), verifies live WebGPU rendering with 0 WebGPU errors,
//      >0 localReads / localBytes, and 0 /GameData/ HTTP requests.

import { spawn } from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import http from 'node:http';

const ROOT = path.resolve(path.dirname(new URL(import.meta.url).pathname), '..');
const GAME_DIR = process.env.SWBF2_GAMEDATA_DIR ||
  path.join(ROOT, 'data', 'Star wars battlefront 2', 'GameData', 'data', '_lvl_pc');
const HTTP_PORT = 8148;
const CDP_PORT = 9700 + (process.pid % 200);
const OUT_DIR = process.env.SWBF2_SCREENSHOT_DIR || path.join(ROOT, 'build', 'screenshots');

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

function httpRequest(url) {
  return new Promise((resolve, reject) => {
    http.get(url, (res) => {
      let body = '';
      res.on('data', (c) => body += c);
      res.on('end', () => resolve({ status: res.statusCode, body }));
    }).on('error', reject);
  });
}

async function waitForHttp(url, timeoutMs = 10000) {
  const end = Date.now() + timeoutMs;
  while (Date.now() < end) {
    try {
      await httpRequest(url);
      return;
    } catch {
      await sleep(100);
    }
  }
  throw new Error(`Timed out waiting for ${url}`);
}

function collectLocalFiles(dir, prefix = '', out = []) {
  for (const ent of fs.readdirSync(dir, { withFileTypes: true })) {
    const full = path.join(dir, ent.name);
    const rel = prefix ? `${prefix}/${ent.name}` : ent.name;
    if (ent.isDirectory()) {
      collectLocalFiles(full, rel, out);
    } else if (/\.(lvl|bnk|mvs|def|ini|lua|txt|cfg)$/i.test(ent.name)) {
      out.push({ full, rel });
    }
  }
  return out;
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
    const r = await this.send('Runtime.evaluate', { expression: expr, awaitPromise: true, returnByValue: true });
    if (r.exceptionDetails) throw new Error(r.exceptionDetails.exception?.description || r.exceptionDetails.text);
    return r.result.value;
  }
  close() { try { this.ws.close(); } catch {} }
}

if (!fs.existsSync(path.join(GAME_DIR, 'core.lvl'))) {
  console.error(`Local GameData not found at ${GAME_DIR} (set SWBF2_GAMEDATA_DIR to run this test).`);
  process.exit(1);
}

// Start serve.py in strict BYOC mode (zero hosted GameData!)
const httpServer = spawn('python3', ['tools/serve.py', String(HTTP_PORT), '--host', '127.0.0.1', '--tls-port', '0', '--byoc-only'], {
  cwd: ROOT,
  stdio: ['ignore', 'pipe', 'pipe'],
});
const profileDir = `/tmp/swbf2-byoc-${process.pid}`;
let chrome = null;
let cdp = null;
let exitCode = 1;

try {
  await waitForHttp(`http://127.0.0.1:${HTTP_PORT}/api/server-config`);

  // 1. Verify server-config + 404 on /GameData/ and /browser/gamedata-manifest.json
  const cfgRes = await httpRequest(`http://127.0.0.1:${HTTP_PORT}/api/server-config`);
  const cfg = JSON.parse(cfgRes.body);
  const gdRes = await httpRequest(`http://127.0.0.1:${HTTP_PORT}/GameData/core.lvl`);
  const manRes = await httpRequest(`http://127.0.0.1:${HTTP_PORT}/browser/gamedata-manifest.json`);

  console.log('[byoc] server-config:', cfg, 'GET /GameData/core.lvl:', gdRes.status, 'GET manifest:', manRes.status);

  // 2. Launch Chrome + WebGPU
  const chromeArgs = [
    '--headless=new', '--no-sandbox', '--disable-crash-reporter',
    '--autoplay-policy=no-user-gesture-required', '--window-size=1280,820',
    '--enable-unsafe-webgpu', '--enable-features=Vulkan', '--use-angle=vulkan',
    '--ignore-gpu-blocklist', '--disable-gpu-sandbox',
    `--remote-debugging-port=${CDP_PORT}`,
    `--user-data-dir=${profileDir}`,
    'about:blank',
  ];
  chrome = spawn('google-chrome-stable', chromeArgs, { stdio: ['ignore', 'pipe', 'pipe'] });
  await waitForHttp(`http://127.0.0.1:${CDP_PORT}/json/list`, 15000);
  const targets = JSON.parse((await httpRequest(`http://127.0.0.1:${CDP_PORT}/json/list`)).body);
  const page = targets.find((t) => t.type === 'page') || targets[0];
  cdp = new CDP(page.webSocketDebuggerUrl);
  await cdp.send('Page.enable');
  await cdp.send('Runtime.enable');
  await cdp.send('DOM.enable');
  await cdp.send('Emulation.setDeviceMetricsOverride', {
    width: 1280, height: 820, deviceScaleFactor: 1, mobile: false,
  });

  await cdp.send('Page.navigate', { url: `http://127.0.0.1:${HTTP_PORT}/play` });
  await sleep(2000);

  // 3. Confirm BYOC card is visible & launching without files is cleanly blocked
  const beforeState = await cdp.evalAsync(`(async () => {
    const card = document.getElementById('byocCard');
    await window.__playApi.launch();
    const errText = document.getElementById('errMsg')?.textContent || '';
    return {
      cardVisible: card?.classList.contains('visible'),
      state: window.__playApi.getState(),
      errText,
    };
  })()`);
  console.log('[byoc] before picking folder:', JSON.stringify({
    cardVisible: beforeState.cardVisible,
    byoc: beforeState.state.byoc,
    booted: beforeState.state.booted,
    errText: beforeState.errText,
  }));

  fs.mkdirSync(OUT_DIR, { recursive: true });
  const shotLauncher = await cdp.send('Page.captureScreenshot', { format: 'png' });
  fs.writeFileSync(path.join(OUT_DIR, 'byoc_launcher.png'), Buffer.from(shotLauncher.data, 'base64'));

  // 4. Mount real local GameData files through the real <input id="fileByocFolder" webkitdirectory>
  //    Using core + tat2 + common/side/sound/load files (fast & representative) or full directory!
  const allLocal = collectLocalFiles(GAME_DIR);
  const subset = allLocal.filter((f) => {
    const l = f.rel.toLowerCase();
    return (
      !l.includes('/') ||
      l.startsWith('tat/') ||
      l.startsWith('side/') ||
      l.startsWith('load/') ||
      l.startsWith('sound/common') ||
      l.startsWith('sound/tat') ||
      l.startsWith('sound/shell') ||
      l.startsWith('sound/cw') ||
      l.startsWith('sound/gcw') ||
      l.startsWith('fpm/')
    );
  });
  // Create a dedicated file input so play.mjs's change handler doesn't clear inp.value='' before
  // we pair each real Chromium File object with its _lvl_pc-relative path (CDP setFileInputFiles
  // populates real File objects backed by OS disk files, without setting webkitRelativePath).
  await cdp.evalAsync(`(() => {
    const inp = document.createElement('input');
    inp.type = 'file';
    inp.multiple = true;
    inp.id = '__byocTestInput';
    inp.style.display = 'none';
    document.body.appendChild(inp);
  })()`);
  const doc = await cdp.send('DOM.getDocument', { depth: -1 });
  const inputNode = await cdp.send('DOM.querySelector', {
    nodeId: doc.root.nodeId,
    selector: '#__byocTestInput',
  });
  await cdp.send('DOM.setFileInputFiles', {
    nodeId: inputNode.nodeId,
    files: subset.map((f) => f.full),
  });
  await sleep(300);
  const relList = subset.map((f) => f.rel);
  const mounted = await cdp.evalAsync(`(async () => {
    const inp = document.getElementById('__byocTestInput');
    const rels = ${JSON.stringify(relList)};
    const entries = [];
    for (let i = 0; i < inp.files.length && i < rels.length; ++i) {
      entries.push({ relPath: '_lvl_pc/' + rels[i], file: inp.files[i] });
    }
    const idx = await window.__playApi.mountLocalEntries(entries, 'GameData');
    return { valid: idx.valid, fileCount: idx.fileCount, mapCount: idx.mapCount, totalBytes: idx.totalBytes };
  })()`);
  console.log('[byoc] mounted local GameData:', mounted);

  // 5. Select 1280x720 (matches headless viewport) and launch tat2g_con directly from local FileReaderSync
  await cdp.evalAsync(`(async () => {
    const sel = document.getElementById('selInitRes');
    if (sel) sel.value = '1280x720';
    await window.__playApi.launch({ initialMission: 'tat2g_con' });
  })()`);
  await sleep(3000);

  const live = await cdp.evalAsync(`(async () => {
    const vfsRes = await window.__playApi.callWorker('vfsOp', { op: 'stats' });
    const rb = await window.__playApi.callWorker('interactiveAction', { action: 'readback' }).catch(() => null);
    let nonBlackPct = 0;
    if (rb && rb.pixels) {
      const px = new Uint8Array(rb.pixels);
      let nb = 0;
      const total = (rb.width || 0) * (rb.height || 0);
      for (let i = 0; i < px.length; i += 4) {
        if (px[i] > 4 || px[i + 1] > 4 || px[i + 2] > 4) nb++;
      }
      nonBlackPct = total > 0 ? Number(((100 * nb) / total).toFixed(2)) : 0;
    }
    return {
      state: window.__playApi.getState(),
      vfs: vfsRes.vfsStats,
      nonBlackPct,
    };
  })()`);

  const shotLive = await cdp.send('Page.captureScreenshot', { format: 'png' });
  fs.writeFileSync(path.join(OUT_DIR, 'byoc_live_tat2.png'), Buffer.from(shotLive.data, 'base64'));

  const st = live.state?.status?.status || {};
  const vfs = live.vfs || {};
  console.log('[byoc] live gameplay:', JSON.stringify({
    booted: live.state.booted,
    bootMs: live.state.bootMs,
    fps: live.state.fps,
    state: st.state,
    wgpuErrors: st.wgpuErrors,
    draws: st.draws,
    nonBlackPct: live.nonBlackPct,
    vfsMode: vfs.mode,
    localReads: vfs.localReads,
    localBytes: vfs.localBytes,
    httpRequests: vfs.httpRequests,
    httpBytes: vfs.httpBytes,
  }));
  if ((st.wgpuErrors || 0) > 0) {
    console.log('[byoc] console logs:', cdp.logs.filter((l) => /wgpu|error|warn/i.test(l)).slice(-10));
  }

  const checks = [
    ['server-config reports BYOC mode', cfg.gamedataHosted === false && cfg.mode === 'byoc'],
    ['GET /GameData/core.lvl blocked with 404', gdRes.status === 404],
    ['GET /browser/gamedata-manifest.json blocked with 404', manRes.status === 404],
    ['BYOC card visible on /play', beforeState.cardVisible === true],
    ['Launch cleanly gated before folder selection', beforeState.state.booted === false && /GameData/i.test(beforeState.errText)],
    ['Local GameData validated cleanly', mounted.valid === true && mounted.fileCount >= 20],
    ['Engine booted into Mos Eisley mission', live.state.booted === true && st.state === 'mission'],
    ['Zero WebGPU errors & active draws', (st.wgpuErrors || 0) === 0 && (st.draws || 0) > 20],
    ['100% local FileReaderSync reads (ZERO HTTP GameData requests)', vfs.mode === 'local' && vfs.localReads > 0 && vfs.localBytes > 1000000 && vfs.httpRequests === 0 && vfs.httpBytes === 0],
  ];

  let pass = 0;
  for (const [name, ok] of checks) {
    console.log(`  [${ok ? 'PASS' : 'FAIL'}] ${name}`);
    if (ok) pass++;
  }
  console.log(`\nBYOC CHECK RESULT: ${pass}/${checks.length} PASSED`);
  exitCode = pass === checks.length ? 0 : 1;
} catch (err) {
  console.error('[byoc] fatal:', err);
  if (cdp?.logs?.length) console.error('Recent console logs:\n' + cdp.logs.slice(-20).join('\n'));
} finally {
  if (cdp) cdp.close();
  if (chrome) { try { chrome.kill('SIGKILL'); } catch {} }
  try { httpServer.kill('SIGTERM'); } catch {}
  try { fs.rmSync(profileDir, { recursive: true, force: true }); } catch {}
}
process.exit(exitCode);
