// Headless Chrome CDP check for the Phase 0 WebGPU smoke test.
// Starts tools/serve.py on a loopback port if --url is omitted, launches Chrome with WebGPU
// enabled (hardware Vulkan when available, SwiftShader fallback otherwise), opens
// /browser/webgpu_smoke.html, and asserts all WebGPU + OffscreenCanvas + BC1 + depth checks.
import { spawn } from 'child_process';
import fs from 'fs';

const argv = process.argv.slice(2);
const arg = (name, dflt) => {
  const i = argv.indexOf('--' + name);
  return i >= 0 ? argv[i + 1] : dflt;
};
const HTTP_PORT = +arg('http-port', 8199);
const CDP_PORT = +arg('cdp-port', 9399);
const SHOT = arg('shot', '/tmp/wgpu_smoke.png');
const WAIT_MS = +arg('wait', 20) * 1000;
const CHROME_BIN = arg('chrome', 'google-chrome-stable');
const FORCE_SWIFTSHADER = argv.includes('--swiftshader');

const server = spawn('python3', ['tools/serve.py', String(HTTP_PORT), '--host', '127.0.0.1'], {
  stdio: ['ignore', 'pipe', 'pipe'],
});
await new Promise((res) => setTimeout(res, 600));

const chromeArgs = [
  '--headless=new',
  '--no-sandbox',
  '--disable-crash-reporter',
  '--enable-unsafe-webgpu',
  '--remote-debugging-port=' + CDP_PORT,
  '--window-size=640,480',
  '--user-data-dir=/tmp/swbf2-wgpu-cdp-' + process.pid,
];
if (FORCE_SWIFTSHADER) {
  chromeArgs.push(
    '--use-angle=swiftshader',
    '--enable-unsafe-swiftshader',
    '--disable-gpu-sandbox',
  );
} else {
  chromeArgs.push(
    '--enable-features=Vulkan',
    '--use-angle=vulkan',
    '--ignore-gpu-blocklist',
    '--disable-gpu-sandbox',
  );
}
chromeArgs.push('about:blank');

const chrome = spawn(CHROME_BIN, chromeArgs, { stdio: ['ignore', 'ignore', 'pipe'] });
let stderrTail = '';
chrome.stderr.on('data', (b) => { stderrTail = (stderrTail + b.toString()).slice(-4000); });

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const cleanup = () => {
  try { chrome.kill(); } catch {}
  try { server.kill(); } catch {}
  try { fs.rmSync('/tmp/swbf2-wgpu-cdp-' + process.pid, { recursive: true, force: true }); } catch {}
};

async function waitDevtools(ms) {
  const end = Date.now() + ms;
  while (Date.now() < end) {
    try {
      const r = await fetch(`http://127.0.0.1:${CDP_PORT}/json/version`);
      if (r.ok) return r.json();
    } catch {}
    await sleep(150);
  }
  throw new Error('Chrome DevTools failed to start:\n' + stderrTail);
}

try {
  const version = await waitDevtools(15000);
  const target = await (await fetch(`http://127.0.0.1:${CDP_PORT}/json/new?about:blank`, { method: 'PUT' })).json();
  const ws = new WebSocket(target.webSocketDebuggerUrl);
  await new Promise((res, rej) => { ws.onopen = res; ws.onerror = rej; });

  let nextId = 1;
  const pending = new Map();
  const consoleLines = [];
  const pageErrors = [];
  ws.onmessage = (ev) => {
    const msg = JSON.parse(ev.data);
    if (msg.id && pending.has(msg.id)) {
      const { resolve, reject } = pending.get(msg.id);
      pending.delete(msg.id);
      msg.error ? reject(new Error(JSON.stringify(msg.error))) : resolve(msg.result);
      return;
    }
    if (msg.method === 'Runtime.consoleAPICalled') {
      consoleLines.push((msg.params.args || []).map((a) => a.value ?? a.description ?? a.type).join(' '));
    } else if (msg.method === 'Runtime.exceptionThrown') {
      pageErrors.push(msg.params.exceptionDetails?.exception?.description || msg.params.exceptionDetails?.text || 'exception');
    }
  };
  const send = (method, params = {}) => new Promise((resolve, reject) => {
    const id = nextId++;
    pending.set(id, { resolve, reject });
    ws.send(JSON.stringify({ id, method, params }));
  });
  const evaluate = async (expression) => {
    const r = await send('Runtime.evaluate', { expression, returnByValue: true, awaitPromise: true });
    if (r.exceptionDetails) throw new Error(r.exceptionDetails.exception?.description || 'eval failed');
    return r.result.value;
  };

  await send('Page.enable');
  await send('Runtime.enable');
  await send('Page.navigate', { url: `http://127.0.0.1:${HTTP_PORT}/browser/webgpu_smoke.html` });

  const end = Date.now() + WAIT_MS;
  let result = null;
  while (Date.now() < end) {
    try {
      result = await evaluate('window.__wgpuSmokeResult || null');
    } catch {}
    if (result) break;
    await sleep(150);
  }

  const shot = await send('Page.captureScreenshot', { format: 'png' });
  fs.writeFileSync(SHOT, Buffer.from(shot.data, 'base64'));
  ws.close();

  const out = {
    browser: version.Browser,
    result,
    pageErrors,
    consoleTail: consoleLines.slice(-10),
    chromeStderrTail: stderrTail.split('\n').filter(Boolean).slice(-20),
    screenshot: SHOT,
  };
  console.log(JSON.stringify(out, null, 2));
  cleanup();
  process.exit(result && result.ok && pageErrors.length === 0 ? 0 : 1);
} catch (err) {
  console.error(err);
  cleanup();
  process.exit(2);
}
