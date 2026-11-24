// Concrete check for the browser PCM16/WebAudio slice.
//
//   NODE_OPTIONS= node browser/audio_check.mjs
//
// Two independent measurements are made:
//   1. Node loads common.bnk through the C++ PblFile/SoundFL reader, drains the PCM queue and
//      hashes every byte.  This pins the exact sample, format, rate and size.
//   2. Headless Chromium loads the same bank through the same C ABI, starts a real AudioContext,
//      and measures non-zero signal energy after the WebAudio output node.  This proves that the
//      PCM reaches a running WebAudio graph; it does not rely on a synthesised fallback sound.
//
// --no-browser runs only the byte-exact Node check.  The CDP helper is run with the emsdk's Node
// 24 when the default Node has no built-in WebSocket implementation.

import fs from 'fs';
import path from 'path';
import url from 'url';
import crypto from 'crypto';
import net from 'net';
import { spawn } from 'child_process';

const here = path.dirname(url.fileURLToPath(import.meta.url));
const root = path.join(here, '..');
const GAME = path.join(root, 'data/Star wars battlefront 2/GameData/data/_lvl_pc');
const BANK_REL = path.join('sound', 'common.bnk');
const BANK_PATH = path.join(GAME, BANK_REL);
const MODULE_PATH = process.env.SWBF2_MODULE || path.join(root, 'build/browser/swbf2.js');

const EXPECTED = Object.freeze({
  name: 'force_push_fire',
  hash: 0x6aa27e0e,
  format: 'PCM16',
  rate: 22050,
  channels: 1,
  bytes: 90986,
  frames: 45493,
  pcmSha256: 'ba7b1ad54541c5499556de1ca8c7a30a2adaaedff879a2abb43d3e073e726391',
  bankSha256: '43976f384fba818f0d06de322aafaaa985ffca52950591254dd4ab3a3be420f0',
});

const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

function fail(message) {
  throw new Error(message);
}

function assert(condition, message) {
  if (!condition) fail(message);
}

async function makeModule() {
  const factory = (await import(url.pathToFileURL(MODULE_PATH).href)).default;
  assert(typeof factory === 'function', `module factory is not a function: ${MODULE_PATH}`);
  return factory({
    locateFile: (file) => path.join(path.dirname(MODULE_PATH), file),
    onAbort: (why) => { throw new Error(`wasm abort: ${why}`); },
  });
}

function stageBank(Module) {
  try { Module.FS.mkdir('/data'); } catch {}
  const bytes = fs.readFileSync(BANK_PATH);
  Module.FS.writeFile('/data/common.bnk', bytes);
  return bytes.length;
}

async function runNodePcmCheck() {
  assert(fs.existsSync(BANK_PATH), `sound bank is missing: ${BANK_PATH}`);

  const bankBytes = fs.readFileSync(BANK_PATH);
  const bankSha256 = crypto.createHash('sha256').update(bankBytes).digest('hex');
  assert(bankSha256 === EXPECTED.bankSha256, 'common.bnk SHA-256 does not match the pinned bank');

  const Module = await makeModule();
  const stagedBytes = stageBank(Module);
  assert(stagedBytes === bankBytes.length, 'staged bank length changed');

  // A missing hash must fail honestly and must not leave a fabricated sample queued.
  const missingStatus = Module.ccall(
    'audio_browser_load_sample', 'number', ['string', 'number'],
    ['/data/common.bnk', 0x12345678],
  );
  assert(missingStatus === 0, `missing sample status was ${missingStatus}, expected 0`);
  assert(Module.ccall('audio_browser_pending_frames', 'number') === 0,
    'missing sample left frames in the PCM queue');

  // The name goes through the engine's own PblHash implementation.
  const status = Module.ccall(
    'audio_browser_load_sample_by_name', 'number', ['string', 'string'],
    ['/data/common.bnk', EXPECTED.name],
  );
  assert(status === 1, `sample load status was ${status}, expected 1`);

  const metadata = {
    hash: Module.ccall('audio_browser_sample_hash', 'number') >>> 0,
    rate: Module.ccall('audio_browser_sample_rate', 'number') >>> 0,
    bytes: Module.ccall('audio_browser_sample_bytes', 'number') >>> 0,
    channels: Module.ccall('audio_browser_sample_channels', 'number'),
    frames: Module.ccall('audio_browser_sample_frames', 'number'),
  };
  assert(metadata.hash === EXPECTED.hash, 'sample hash mismatch');
  assert(metadata.rate === EXPECTED.rate, 'sample rate mismatch');
  assert(metadata.bytes === EXPECTED.bytes, 'sample byte count mismatch');
  assert(metadata.channels === EXPECTED.channels, 'sample channel count mismatch');
  assert(metadata.frames === EXPECTED.frames, 'sample frame count mismatch');

  const pcmPtr = Module._malloc(EXPECTED.bytes + 4096);
  const ratePtr = Module._malloc(4);
  const channelPtr = Module._malloc(4);
  assert(pcmPtr && ratePtr && channelPtr, 'could not allocate PCM drain buffers');

  const hash = crypto.createHash('sha256');
  let frames = 0;
  let minSample = 32767;
  let maxSample = -32768;
  let nonZero = 0;
  while (true) {
    const copied = Module.ccall(
      'audio_browser_drain_pcm', 'number', ['number', 'number', 'number', 'number'],
      [pcmPtr, 8192, ratePtr, channelPtr],
    );
    if (copied <= 0) break;
    const rate = Module.getValue(ratePtr, 'i32');
    const channels = Module.getValue(channelPtr, 'i32');
    assert(rate === EXPECTED.rate, 'drained chunk rate changed');
    assert(channels === EXPECTED.channels, 'drained chunk channel count changed');

    const byteCount = copied * channels * 2;
    const bytes = Buffer.from(Module.HEAPU8.subarray(pcmPtr, pcmPtr + byteCount));
    hash.update(bytes);
    for (let i = 0; i + 1 < bytes.length; i += 2) {
      const sample = bytes.readInt16LE(i);
      if (sample < minSample) minSample = sample;
      if (sample > maxSample) maxSample = sample;
      if (sample !== 0) nonZero++;
    }
    frames += copied;
  }

  const pcmSha256 = hash.digest('hex');
  assert(frames === EXPECTED.frames, `drained ${frames} frames, expected ${EXPECTED.frames}`);
  assert(pcmSha256 === EXPECTED.pcmSha256, 'drained PCM SHA-256 mismatch');
  assert(Module.ccall('audio_browser_pending_frames', 'number') === 0,
    'PCM queue is not empty after the sample was drained');
  assert(nonZero > 0, 'drained PCM contains only zeroes');

  // Exercise the generic queue seam that the engine mixer can use later.
  const testPcm = new Int16Array([123, -456, 789, -1011]);
  const testPtr = Module._malloc(testPcm.length * 2);
  Module.HEAPU8.set(new Uint8Array(testPcm.buffer), testPtr);
  const pushStatus = Module.ccall(
    'audio_browser_push_pcm', 'number', ['number', 'number', 'number', 'number'],
    [testPtr, 2, 2, 48000],
  );
  assert(pushStatus === 1, 'generic PCM push failed');
  const copied = Module.ccall(
    'audio_browser_drain_pcm', 'number', ['number', 'number', 'number', 'number'],
    [testPtr, 1, ratePtr, channelPtr],
  );
  assert(copied === 1, 'generic PCM drain failed');
  assert(Module.getValue(ratePtr, 'i32') === 48000, 'generic PCM rate mismatch');
  assert(Module.getValue(channelPtr, 'i32') === 2, 'generic PCM channel count mismatch');
  assert(Module.HEAPU8[testPtr] === 123 && Module.HEAPU8[testPtr + 1] === 0,
    'generic PCM bytes changed in transit');

  Module._free(pcmPtr);
  Module._free(ratePtr);
  Module._free(channelPtr);
  Module._free(testPtr);

  Module.ccall('audio_browser_set_sink_ready', null, ['number'], [1]);
  assert(Module.ccall('audio_browser_sink_ready', 'number') === 1, 'sink-ready handshake failed');
  Module.ccall('audio_browser_set_sink_ready', null, ['number'], [0]);

  return {
    bankBytes: bankBytes.length,
    bankSha256,
    missingHashStatus: missingStatus,
    loadStatus: status,
    metadata,
    drainedFrames: frames,
    pcmSha256,
    pcmMin: minSample,
    pcmMax: maxSample,
    nonZeroSamples: nonZero,
    genericQueueOk: true,
  };
}

const BROWSER_TEST_EXPRESSION = String.raw`(async () => {
  const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));
  const { loadSampleAndStart } = await import('/browser/webaudio.mjs');
  await new Promise((resolve, reject) => {
    const script = document.createElement('script');
    script.src = '/build/browser/swbf2.js';
    script.onload = resolve;
    script.onerror = reject;
    document.head.appendChild(script);
  });
  const Module = await globalThis.SWBF2({ locateFile: (file) => '/build/browser/' + file });
  try { Module.FS.mkdir('/data'); } catch {}
  const response = await fetch('/GameData/sound/common.bnk');
  if (!response.ok) throw new Error('bank fetch failed: ' + response.status);
  const bank = new Uint8Array(await response.arrayBuffer());
  Module.FS.writeFile('/data/common.bnk', bank);

  const context = new AudioContext({ latencyHint: 'interactive' });
  const analyser = context.createAnalyser();
  analyser.fftSize = 2048;
  const sink = await loadSampleAndStart({
    module: Module,
    bankPath: '/data/common.bnk',
    sampleName: 'force_push_fire',
    context,
    workletUrl: '/browser/webaudio-worklet.mjs',
  });
  sink.output.connect(analyser);
  analyser.connect(context.destination);

  const samples = new Float32Array(analyser.fftSize);
  let peak = 0;
  let nonZero = 0;
  let snapshots = 0;
  for (let i = 0; i < 60; ++i) {
    await sleep(50);
    analyser.getFloatTimeDomainData(samples);
    ++snapshots;
    for (const sample of samples) {
      peak = Math.max(peak, Math.abs(sample));
      if (sample !== 0) ++nonZero;
    }
  }

  const result = {
    contextState: context.state,
    contextRate: context.sampleRate,
    usingWorklet: sink.stats().usingWorklet,
    framesDelivered: sink.stats().framesDelivered,
    chunksDelivered: sink.stats().chunksDelivered,
    peak,
    nonZero,
    snapshots,
    sampleHash: Module.ccall('audio_browser_sample_hash', 'number') >>> 0,
    sampleRate: Module.ccall('audio_browser_sample_rate', 'number') >>> 0,
    sampleBytes: Module.ccall('audio_browser_sample_bytes', 'number') >>> 0,
    bankBytes: bank.byteLength,
  };
  sink.stop();
  await context.close();
  return result;
})()`;

async function cdpBrowserCheck(baseUrl, devtoolsPort) {
  assert(typeof globalThis.WebSocket === 'function',
    'the CDP helper needs a Node build with global WebSocket');

  let version;
  const deadline = Date.now() + 15000;
  while (Date.now() < deadline) {
    try {
      const response = await fetch(`http://127.0.0.1:${devtoolsPort}/json/version`);
      if (response.ok) {
        version = await response.json();
        break;
      }
    } catch {}
    await sleep(100);
  }
  assert(version, 'Chromium DevTools endpoint did not start');

  const targetResponse = await fetch(
    `http://127.0.0.1:${devtoolsPort}/json/new?about:blank`, { method: 'PUT' });
  const target = await targetResponse.json();
  const socket = new globalThis.WebSocket(target.webSocketDebuggerUrl);
  await new Promise((resolve, reject) => {
    socket.onopen = resolve;
    socket.onerror = reject;
  });

  let nextId = 1;
  const pending = new Map();
  const pageErrors = [];
  socket.onmessage = (event) => {
    const message = JSON.parse(event.data);
    if (message.id && pending.has(message.id)) {
      const item = pending.get(message.id);
      pending.delete(message.id);
      if (message.error) item.reject(new Error(JSON.stringify(message.error)));
      else item.resolve(message.result);
      return;
    }
    if (message.method === 'Runtime.exceptionThrown') {
      pageErrors.push(message.params.exceptionDetails?.exception?.description ||
        message.params.exceptionDetails?.text || 'page exception');
    }
  };

  const send = (method, params = {}) => new Promise((resolve, reject) => {
    const id = nextId++;
    pending.set(id, { resolve, reject });
    socket.send(JSON.stringify({ id, method, params }));
  });

  await send('Page.enable');
  await send('Runtime.enable');
  await send('Page.navigate', { url: `${baseUrl}/browser/manifest.json` });
  await sleep(500);

  const evaluation = await send('Runtime.evaluate', {
    expression: BROWSER_TEST_EXPRESSION,
    awaitPromise: true,
    returnByValue: true,
  });
  socket.close();

  if (evaluation.exceptionDetails) {
    const description = evaluation.exceptionDetails.exception?.description ||
      evaluation.exceptionDetails.text || 'browser evaluation failed';
    fail(description);
  }
  assert(!pageErrors.length, `browser page errors: ${pageErrors.join(' | ')}`);
  return evaluation.result.value;
}

async function freePort() {
  return new Promise((resolve, reject) => {
    const server = net.createServer();
    server.on('error', reject);
    server.listen(0, '127.0.0.1', () => {
      const port = server.address().port;
      server.close(() => resolve(port));
    });
  });
}

function findChromium() {
  const candidates = [
    process.env.CHROMIUM,
    '/usr/bin/chromium',
    '/usr/bin/chromium-browser',
    '/usr/bin/google-chrome-stable',
    '/usr/bin/google-chrome',
  ].filter(Boolean);
  for (const candidate of candidates) {
    try {
      fs.accessSync(candidate, fs.constants.X_OK);
      return candidate;
    } catch {}
  }
  return null;
}

async function waitForServer(baseUrl) {
  const deadline = Date.now() + 15000;
  while (Date.now() < deadline) {
    try {
      const response = await fetch(`${baseUrl}/browser/manifest.json`);
      if (response.ok) return;
    } catch {}
    await sleep(100);
  }
  fail('the static server did not start');
}

async function runBrowserCheck() {
  const chromium = findChromium();
  assert(chromium, 'no Chromium/Chrome executable was found (set CHROMIUM)');

  const httpPort = await freePort();
  const devtoolsPort = await freePort();
  const baseUrl = `http://127.0.0.1:${httpPort}`;
  const profile = path.join('/tmp', `swbf2-audio-check-${process.pid}`);
  fs.mkdirSync(profile, { recursive: true });

  const server = spawn('python3', ['tools/serve.py', String(httpPort), '--host', '127.0.0.1', '--serve-gamedata'], {
    cwd: root,
    stdio: ['ignore', 'ignore', 'ignore'],
  });
  const browser = spawn(chromium, [
    '--headless=new',
    '--no-sandbox',
    '--disable-gpu',
    '--disable-dev-shm-usage',
    '--autoplay-policy=no-user-gesture-required',
    `--remote-debugging-port=${devtoolsPort}`,
    `--user-data-dir=${profile}`,
    'about:blank',
  ], { stdio: ['ignore', 'ignore', 'pipe'] });

  let browserStderr = '';
  browser.stderr.on('data', (chunk) => {
    browserStderr = (browserStderr + chunk.toString()).slice(-4000);
  });

  const cleanup = () => {
    try { browser.kill('SIGKILL'); } catch {}
    try { server.kill('SIGKILL'); } catch {}
    try { fs.rmSync(profile, { recursive: true, force: true }); } catch {}
  };
  process.on('exit', cleanup);

  try {
    await waitForServer(baseUrl);

    let browserResult;
    if (typeof globalThis.WebSocket === 'function') {
      browserResult = await cdpBrowserCheck(baseUrl, devtoolsPort);
    } else {
      const helperNode = path.join(root, 'vendor/emsdk/node/24.19.0_64bit/bin/node');
      assert(fs.existsSync(helperNode),
        'default Node has no global WebSocket and the emsdk Node 24 helper is missing');
      const helper = spawn(helperNode, [
        url.fileURLToPath(import.meta.url), '--cdp-helper', baseUrl, String(devtoolsPort),
      ], {
        cwd: root,
        env: { ...process.env, NODE_OPTIONS: '' },
        stdio: ['ignore', 'pipe', 'pipe'],
      });
      let stdout = '';
      let stderr = '';
      helper.stdout.on('data', (chunk) => { stdout += chunk.toString(); });
      helper.stderr.on('data', (chunk) => { stderr += chunk.toString(); });
      const helperStatus = await new Promise((resolve) => {
        helper.on('close', (code) => resolve(code));
      });
      const line = stdout.split('\n').findLast((value) => value.startsWith('AUDIO_CDP_JSON '));
      assert(line, `CDP helper failed (${helperStatus}): ${stderr || stdout}`);
      browserResult = JSON.parse(line.slice('AUDIO_CDP_JSON '.length));
    }

    assert(browserResult.sampleHash === EXPECTED.hash, 'browser sample hash mismatch');
    assert(browserResult.sampleRate === EXPECTED.rate, 'browser sample rate mismatch');
    assert(browserResult.sampleBytes === EXPECTED.bytes, 'browser sample byte count mismatch');
    assert(browserResult.contextState === 'running', 'browser AudioContext was not running');
    assert(browserResult.peak > 0.01, `WebAudio output peak was ${browserResult.peak}`);
    assert(browserResult.nonZero > 0, 'WebAudio output contained only zeroes');

    return { ...browserResult, chromium, browserStderrTail: browserStderr.split('\n').slice(-4) };
  } finally {
    cleanup();
  }
}

async function main() {
  const noBrowser = process.argv.includes('--no-browser');
  const nodeResult = await runNodePcmCheck();
  let browserResult = null;
  if (!noBrowser) browserResult = await runBrowserCheck();

  const checks = {
    bankHashPinned: nodeResult.bankSha256 === EXPECTED.bankSha256,
    missingSampleFails: nodeResult.missingHashStatus === 0,
    sampleHashMatches: nodeResult.metadata.hash === EXPECTED.hash,
    sampleFormatIsPcm16: nodeResult.metadata.bytes === EXPECTED.bytes &&
      nodeResult.metadata.rate === EXPECTED.rate && nodeResult.metadata.channels === 1,
    sampleSizeMatches: nodeResult.metadata.bytes === EXPECTED.bytes &&
      nodeResult.metadata.frames === EXPECTED.frames,
    pcmHashMatches: nodeResult.pcmSha256 === EXPECTED.pcmSha256,
    queueDrainsCompletely: nodeResult.drainedFrames === EXPECTED.frames,
    webAudioHasSignal: !browserResult || (browserResult.peak > 0.01 && browserResult.nonZero > 0),
  };
  const ok = Object.values(checks).every(Boolean);

  console.log(JSON.stringify({
    ok,
    sample: {
      name: EXPECTED.name,
      hash: `0x${EXPECTED.hash.toString(16).padStart(8, '0')}`,
      format: EXPECTED.format,
      rate: EXPECTED.rate,
      channels: EXPECTED.channels,
      bytes: EXPECTED.bytes,
      frames: EXPECTED.frames,
      durationSeconds: EXPECTED.frames / EXPECTED.rate,
      pcmSha256: EXPECTED.pcmSha256,
    },
    checks,
    node: nodeResult,
    browser: browserResult,
  }, null, 2));
  if (!ok) process.exitCode = 2;
}

const helperIndex = process.argv.indexOf('--cdp-helper');
if (helperIndex >= 0) {
  const baseUrl = process.argv[helperIndex + 1];
  const devtoolsPort = Number(process.argv[helperIndex + 2]);
  cdpBrowserCheck(baseUrl, devtoolsPort)
    .then((result) => {
      console.log('AUDIO_CDP_JSON ' + JSON.stringify(result));
    })
    .catch((error) => {
      console.error(error.stack || error);
      process.exit(1);
    });
} else {
  main().catch((error) => {
    console.error(error.stack || error);
    process.exit(1);
  });
}
