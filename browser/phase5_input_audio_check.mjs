#!/usr/bin/env node
// Phase 5 Verification: Interactive DOM -> SharedArrayBuffer Input (Keyboard/Mouse/PointerLock/Gamepad)
// + Real 3D SoundFL Software Mixer -> SharedArrayBuffer WebAudio AudioWorklet in Headless Chrome + Vulkan.

import { spawn } from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import http from 'node:http';

const ROOT = path.resolve(path.dirname(new URL(import.meta.url).pathname), '..');
const PORT = 8132;
const CDP_PORT = 9400 + (process.pid % 400);
const OUT_DIR = process.env.SWBF2_SCREENSHOT_DIR ||
  path.join(ROOT, 'build', 'screenshots');
fs.mkdirSync(OUT_DIR, { recursive: true });

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

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
  close() { try { this.ws.close(); } catch {} }
}

const server = spawn('python3', ['tools/serve.py', String(PORT), '--serve-gamedata'], {
  cwd: ROOT,
  stdio: ['ignore', 'pipe', 'pipe'],
});

let chrome = null;
let cdp = null;
const userDataDir = `/tmp/swbf2_phase5_chrome_${process.pid}`;
fs.mkdirSync(userDataDir, { recursive: true });

function bail() {
  try { if (cdp) cdp.close(); } catch {}
  try { if (chrome) chrome.kill('SIGKILL'); } catch {}
  try { server.kill('SIGKILL'); } catch {}
  try { fs.rmSync(userDataDir, { recursive: true, force: true }); } catch {}
  process.exit(1);
}

try {
  const url = `http://127.0.0.1:${PORT}/browser/phase5_input_audio_check.html`;
  await waitForServer(url, server);

  chrome = spawn('google-chrome-stable', [
    '--headless=new',
    '--no-sandbox',
    '--disable-crash-reporter',
    '--autoplay-policy=no-user-gesture-required',
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
  await cdp.send('Page.navigate', { url });

  let result = null;
  const deadline = Date.now() + 60000;
  while (Date.now() < deadline) {
    const ev = await cdp.send('Runtime.evaluate', {
      expression: 'window.__phase5Result ? JSON.stringify(window.__phase5Result) : ""',
      returnByValue: true,
    });
    if (ev.result?.value) {
      result = JSON.parse(ev.result.value);
      break;
    }
    await sleep(250);
  }

  if (!result) {
    console.error('Timed out waiting for __phase5Result. Logs:\n' + cdp.logs.slice(-60).join('\n'));
    bail();
  }

  const pngEv = await cdp.send('Runtime.evaluate', { expression: 'window.__phase5PngLive', returnByValue: true });
  if (typeof pngEv.result?.value === 'string' && pngEv.result.value.startsWith('data:image/png;base64,')) {
    const outPath = path.join(OUT_DIR, 'phase5_tat2_live.png');
    fs.writeFileSync(outPath, Buffer.from(pngEv.result.value.slice('data:image/png;base64,'.length), 'base64'));
    console.log(`saved screenshot: ${outPath}`);
  }

  const diagLogs = cdp.logs.filter((l) => /\[phase5\]|ds:|exception/i.test(l));
  if (diagLogs.length) {
    console.log('--- Diagnostic logs ---\n' + diagLogs.slice(0, 40).join('\n') + '\n-----------------------');
  }
  console.log(JSON.stringify(result, null, 2));

  const shellAudio = result.shellSnapshot?.audioStats || {};
  const missionAudio = result.missionSnapshot?.audioStats || {};
  const worklet = result.missionAudioSink || {};

  const checks = [
    ['result.ok', result.ok === true],
    ['crossOriginIsolated === true (SharedArrayBuffer active)', result.crossOriginIsolated === true],
    ['wgpuReady.ok', result.wgpuReady?.ok === true],
    ['SoundFL mixConfig === 1 (mixConfig_Software 3D mixer)', shellAudio.mixConfig === 1 && missionAudio.mixConfig === 1],
    ['SoundFL created DirectSound buffers (> 0)', (shellAudio.totalBuffersCreated || 0) > 0],
    ['Shell audio mixed real retail PCM (nonZeroFrames > 500, maxPeak > 0.01)',
      (shellAudio.nonZeroFrames || 0) > 500 && (shellAudio.maxPeak || 0) > 0.01],
    ['WebAudio AudioWorkletNode running at 48 kHz via SharedArrayBuffer',
      worklet.usingWorklet === true && worklet.contextState === 'running' && worklet.contextRate === 48000],
    ['AudioWorklet drained > 20,000 stereo frames from SAB ring', (worklet.totalFramesRead || 0) > 20000],
    ['WebAudio AnalyserNode measured live non-zero Shell audio output',
      (result.analyserShell?.peak || 0) > 0.005 && (result.analyserShell?.nonZero || 0) > 100],
    ['Real DOM KeyboardEvent (Enter) -> SAB -> DirectInput8 logged into main menu',
      result.afterLoginScreen === 'ifs_sp_campaign' || result.afterLoginScreen === 'ifs_main'],
    ['Real DOM MouseEvents (canvas click) -> SAB -> DirectInput8 spawned into tat2g_con',
      result.spawnSnapshot?.status?.playerAlive === true],
    ['Worker signals pointerLockWanted === true in live 3D gameplay',
      result.spawnSnapshot?.status?.pointerLockWanted === true],
    ['Real DOM KeyW -> SAB -> DirectInput8 moved player (> 0.5m)', (result.wasdDistanceMoved || 0) > 0.5],
    ['Real DOM MouseMove movementX (PointerLock) -> SAB -> DirectInput8 rotated player (> 5 deg)',
      (result.mouseLookAngleDeg || 0) > 5.0],
    ['Real DOM MouseDown left click -> SAB -> DirectInput8 fired blaster weapon',
      result.fireCheck?.fired === true],
    ['Mission 3D SoundFL mixed live combat/ambient audio (overallRms > 0.001, maxPeak > 0.02)',
      (missionAudio.overallRmsL || 0) > 0.001 && (missionAudio.overallRmsR || 0) > 0.001 && (missionAudio.maxPeak || 0) > 0.02],
    ['Mission 3D audio exhibits real stereo spatialization (L != R)',
      (result.missionSnapshot?.stereoMeanDiff || 0) > 0.0001],
    ['WebAudio AnalyserNode measured live non-zero Mission 3D audio output',
      (result.analyserMission?.peak || 0) > 0.005 && (result.analyserMission?.nonZero || 0) > 100],
    ['Clean return to Shell after interactive gameplay + audio', result.returnToShell?.state === 'shell'],
    ['zero WebGPU errors across entire Phase 5 interactive session',
      (result.missionSnapshot?.status?.wgpuErrors || 0) === 0 && (result.returnToShell?.wgpuErrors || 0) === 0],
  ];

  let passed = 0;
  for (const [name, ok] of checks) {
    console.log(`${ok ? 'PASS' : 'FAIL'}: ${name}`);
    if (ok) ++passed;
  }
  console.log(`=== Phase 5 Interactive Input + 3D WebAudio Check: ${passed} / ${checks.length} passed ===`);
  if (passed !== checks.length) {
    console.error('Recent browser logs:\n' + cdp.logs.slice(-60).join('\n'));
    bail();
  }
} finally {
  if (cdp) cdp.close();
  if (chrome) chrome.kill('SIGKILL');
  server.kill('SIGKILL');
  fs.rmSync(userDataDir, { recursive: true, force: true });
}
