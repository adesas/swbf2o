// Headless Chrome CDP verification for Phase 1: Dedicated Worker + OPFS on-demand GameData VFS.
//
// Starts tools/serve.py (COOP/COEP + HTTP Range), opens /browser/vfs_check.html in Chrome,
// waits for both COLD (on-demand HTTP Range -> OPFS) and WARM (fresh Worker, 0 HTTP bytes)
// runs to complete, and asserts every single streaming, sparsity, parity, and persistence check.
import { spawn } from 'child_process';
import fs from 'fs';

const argv = process.argv.slice(2);
const arg = (name, dflt) => {
  const i = argv.indexOf('--' + name);
  return i >= 0 ? argv[i + 1] : dflt;
};
const HTTP_PORT = +arg('http-port', 8198);
const CDP_PORT = +arg('cdp-port', 9398);
const WAIT_MS = +arg('wait', 90) * 1000;
const CHROME_BIN = arg('chrome', 'google-chrome-stable');

const server = spawn('python3', ['tools/serve.py', String(HTTP_PORT), '--host', '127.0.0.1', '--serve-gamedata'], {
  stdio: ['ignore', 'pipe', 'pipe'],
});
await new Promise((r) => setTimeout(r, 600));

const profileDir = '/tmp/swbf2-vfs-cdp-' + process.pid;
const chrome = spawn(CHROME_BIN, [
  '--headless=new',
  '--no-sandbox',
  '--disable-crash-reporter',
  '--remote-debugging-port=' + CDP_PORT,
  '--window-size=1024,768',
  '--user-data-dir=' + profileDir,
  'about:blank',
], { stdio: ['ignore', 'ignore', 'pipe'] });

let stderrTail = '';
chrome.stderr.on('data', (b) => { stderrTail = (stderrTail + b.toString()).slice(-4000); });

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const cleanup = () => {
  try { chrome.kill(); } catch {}
  try { server.kill(); } catch {}
  try { fs.rmSync(profileDir, { recursive: true, force: true }); } catch {}
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
  const pageErrors = [];
  const consoleLines = [];
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
  await send('Page.navigate', { url: `http://127.0.0.1:${HTTP_PORT}/browser/vfs_check.html` });

  const end = Date.now() + WAIT_MS;
  let result = null;
  while (Date.now() < end) {
    try {
      result = await evaluate('window.__vfsCheckResult || null');
    } catch {}
    if (result) break;
    await sleep(250);
  }
  ws.close();

  console.log(JSON.stringify({
    browser: version.Browser,
    result,
    pageErrors,
    consoleTail: consoleLines.slice(-15),
  }, null, 2));
  cleanup();
  process.exit(result && result.ok && pageErrors.length === 0 ? 0 : 1);
} catch (err) {
  console.error(err);
  cleanup();
  process.exit(2);
}
