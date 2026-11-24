#!/usr/bin/env node
// Phase 4 Native GameState Verification (ShellState <-> MissionState in Headless Chrome + Vulkan).

import { spawn } from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import http from 'node:http';

const ROOT = path.resolve(path.dirname(new URL(import.meta.url).pathname), '..');
const PORT = 8131;
// Optional extra query for the page, for diagnostic runs only, e.g.
//   SWBF2_PAGE_QUERY=magenta=1 ... node browser/phase4_gamestate_check.mjs
// turns the scene clear magenta so undrawn regions stop hiding as black-on-black.
const PAGE_QUERY = process.env.SWBF2_PAGE_QUERY ? `?${process.env.SWBF2_PAGE_QUERY}` : '';
const CDP_PORT = 9300 + (process.pid % 400);
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
const userDataDir = `/tmp/swbf2_phase4_chrome_${process.pid}`;
fs.mkdirSync(userDataDir, { recursive: true });

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

try {
  await waitForServer(`http://127.0.0.1:${PORT}/browser/phase4_gamestate_check.html${PAGE_QUERY}`, server);

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
  await cdp.send('Page.navigate', { url: `http://127.0.0.1:${PORT}/browser/phase4_gamestate_check.html${PAGE_QUERY}` });

  let result = null;
  const deadline = Date.now() + 45000;
  while (Date.now() < deadline) {
    const ev = await cdp.send('Runtime.evaluate', {
      expression: 'window.__phase4Result ? JSON.stringify(window.__phase4Result) : ""',
      returnByValue: true,
    });
    if (ev.result?.value) {
      result = JSON.parse(ev.result.value);
      break;
    }
    await sleep(250);
  }

  if (!result) {
    const interesting = cdp.logs.filter((l) => !l.includes('[shader-guard]'));
    console.error('Timed out waiting for __phase4Result. Logs:\n' + interesting.slice(-80).join('\n'));
    bail();
  }

  for (const [varName, fileName] of [
    ['window.__phase4PngShell', 'phase4_shell_login.png'],
    ['window.__phase4PngMain', 'phase4_shell_main.png'],
    ['window.__phase4PngTat', 'phase4_mission_tat2.png'],
    ['window.__phase4PngEli', 'phase4_mission_tat2_eli.png'],
    ['window.__phase4PngSpa', 'phase4_mission_spa1.png'],
  ]) {
    const ev = await cdp.send('Runtime.evaluate', { expression: varName, returnByValue: true });
    const dataUrl = ev.result?.value;
    if (typeof dataUrl === 'string' && dataUrl.startsWith('data:image/png;base64,')) {
      const buf = Buffer.from(dataUrl.slice('data:image/png;base64,'.length), 'base64');
      const outPath = path.join(OUT_DIR, fileName);
      fs.writeFileSync(outPath, buf);
      console.log(`saved screenshot: ${outPath}`);
    }
  }

  const diagLogs = cdp.logs.filter((l) => /bink|\[phase4\]|\[debug\]|exception/i.test(l));
  if (diagLogs.length) {
    console.log('--- Diagnostic logs ---\n' + diagLogs.slice(0, 40).join('\n') + '\n-----------------------');
  }
  console.log(JSON.stringify(result, null, 2));

  const checks = [
    ['result.ok', result.ok === true],
    ['wgpuReady.ok', result.wgpuReady?.ok === true],
    ['bootOk', result.bootOk === true],
    ['shellStatus.state === "shell"', result.shellStatus?.state === 'shell'],
    ['shellStatus.currentScreen === "ifs_login"', result.shellStatus?.currentScreen === 'ifs_login'],
    ['frameShell.draws > 5', (result.frameShell?.draws || 0) > 5],
    ['frameShell.nonBlackPct > 5', (result.frameShell?.nonBlackPct || 0) > 5],
    ['DirectInput8 login persisted .profile in OPFS', Array.isArray(result.savedProfiles) && result.savedProfiles.some((p) => p.endsWith('.profile'))],
    ['DirectInput8 login transitioned naturally to PC main menu (no Lua bypass)', result.curScreenAfterLogin === 'ifs_sp_campaign' || result.curScreenAfterLogin === 'ifs_main'],
    ['OPFS .profile persisted across fresh worker reload', Array.isArray(result.persistedProfilesAfterReload) && result.persistedProfilesAfterReload.some((p) => p.endsWith('.profile'))],
    ['mainStatus.currentScreen is PC main menu (ifs_sp_campaign / ifs_main)', result.mainStatus?.currentScreen === 'ifs_sp_campaign' || result.mainStatus?.currentScreen === 'ifs_main'],
    ['Bink1 .mvs moviePlaying === true', result.mainStatus?.moviePlaying === true],
    ['Bink1 .mvs dimensions 800x600', result.mainStatus?.movieWidth === 800 && result.mainStatus?.movieHeight === 600],
    ['Bink1 .mvs decoded >= 5 frames', (result.mainStatus?.movieFrame || 0) >= 5],
    ['frameMain renders live animated Bink1 video behind menu (nonBlackPct > 50 && binkAnimatedPct > 0.5)',
      (result.frameMain?.nonBlackPct || 0) > 50 && (result.binkAnimatedPct || 0) > 0.5],
    ['tat2g_con Conquest state === "mission"', result.tatStatus?.state === 'mission'],
    ['tat2g_con Conquest spawned into live gameplay (playerAlive === true)', result.tatStatus?.playerAlive === true],
    ['tat2g_con Conquest draws > 100', (result.frameTat?.draws || 0) > 100],
    ['tat2g_con Conquest hardware-skinned draws > 0', (result.tatStatus?.skinnedDraws || 0) > 0],
    ['returnShellStatus.state === "shell"', result.returnShellStatus?.state === 'shell'],
    ['tat2g_eli Hero Assault state === "mission"', result.eliStatus?.state === 'mission'],
    ['tat2g_eli Hero Assault spawned into live gameplay (playerAlive === true)', result.eliStatus?.playerAlive === true],
    ['tat2g_eli Hero Assault draws > 100', (result.frameEli?.draws || 0) > 100],
    ['tat2g_eli Hero Assault hardware-skinned draws > 0', (result.eliStatus?.skinnedDraws || 0) > 0],
    ['spa1g_ass Space Assault state === "mission"', result.spaStatus?.state === 'mission'],
    ['spa1g_ass Space Assault spawned into live gameplay (playerAlive === true)', result.spaStatus?.playerAlive === true],
    ['spa1g_ass Space Assault draws > 50', (result.frameSpa?.draws || 0) > 50],
    ['spa1g_ass Space Assault hardware-skinned draws > 0', (result.spaStatus?.skinnedDraws || 0) > 0],
    ['zero WebGPU errors across Shell <-> Conquest <-> Hero Assault <-> Space Assault', (result.spaStatus?.wgpuErrors || 0) === 0],
  ];

  let passed = 0;
  for (const [name, ok] of checks) {
    console.log(`${ok ? 'PASS' : 'FAIL'}: ${name}`);
    if (ok) ++passed;
  }
  console.log(`=== Phase 4 Native GameState + Bink1 Check: ${passed} / ${checks.length} passed ===`);
  if (passed !== checks.length) {
    console.error('Recent browser logs:\n' + cdp.logs.slice(-80).join('\n'));
    bail();
  }
} finally {
  if (cdp) cdp.close();
  if (chrome) chrome.kill('SIGKILL');
  server.kill('SIGKILL');
  fs.rmSync(userDataDir, { recursive: true, force: true });
}
