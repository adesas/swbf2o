// Fast loop for debugging the offscreen/GL path: boot the page, wait for the demo, then ask the
// engine to diagnose its own framebuffer and report the frustum a transform was built from.
// Same Chrome flags as browser/cdp_check.mjs; prints one JSON object.
import { spawn } from 'child_process';

const argv = process.argv.slice(2);
const arg = (name, dflt) => { const i = argv.indexOf('--' + name); return i >= 0 ? argv[i + 1] : dflt; };
const URL_ = arg('url', 'http://127.0.0.1:8123/?file=tat/tat2.lvl');
const PORT = +arg('port', 9334);
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

const chrome = spawn('google-chrome-stable', [
  '--headless=new', '--no-sandbox', '--disable-gpu',
  '--use-gl=angle', '--use-angle=swiftshader', '--enable-unsafe-swiftshader',
  '--remote-debugging-port=' + PORT, '--window-size=1024,640',
  '--user-data-dir=/tmp/swbf2-fboprobe-profile', 'about:blank',
], { stdio: ['ignore', 'ignore', 'pipe'] });
let stderrTail = '';
chrome.stderr.on('data', (b) => { stderrTail = (stderrTail + b.toString()).slice(-3000); });

const out = { pageErrors: [], chromeErr: [] };
try {
  const deadline = Date.now() + 15000;
  while (true) {
    try { if ((await fetch(`http://127.0.0.1:${PORT}/json/version`)).ok) break; } catch {}
    if (Date.now() > deadline) throw new Error('devtools never came up: ' + stderrTail);
    await sleep(200);
  }
  const target = await (await fetch(`http://127.0.0.1:${PORT}/json/new?about:blank`, { method: 'PUT' })).json();
  const ws = new WebSocket(target.webSocketDebuggerUrl);
  await new Promise((res, rej) => { ws.onopen = res; ws.onerror = rej; });

  let nextId = 1;
  const pending = new Map();
  ws.onmessage = (ev) => {
    const msg = JSON.parse(ev.data);
    if (msg.id && pending.has(msg.id)) {
      const { resolve, reject } = pending.get(msg.id);
      pending.delete(msg.id);
      msg.error ? reject(new Error(JSON.stringify(msg.error))) : resolve(msg.result);
    } else if (msg.method === 'Runtime.exceptionThrown') {
      const d = msg.params.exceptionDetails;
      out.pageErrors.push(String((d.exception && d.exception.description) || d.text).split('\n')[0]);
    }
  };
  const send = (method, params = {}) => new Promise((resolve, reject) => {
    const id = nextId++;
    pending.set(id, { resolve, reject });
    ws.send(JSON.stringify({ id, method, params }));
  });
  const ev = async (expr) => {
    const r = await send('Runtime.evaluate', { expression: expr, returnByValue: true, awaitPromise: true });
    if (r.exceptionDetails) return 'EXC ' + ((r.exceptionDetails.exception && r.exceptionDetails.exception.description) || r.exceptionDetails.text);
    return r.result ? r.result.value : undefined;
  };

  await send('Page.enable');
  await send('Runtime.enable');
  await send('Page.navigate', { url: URL_ });

  for (let waited = 0; waited < 90000; waited += 500) {
    await sleep(500);
    if (await ev('!!(window.__swbf2 && window.__demoDone && window.__fboProbe)')) break;
  }
  await sleep(2000);

  out.montage = JSON.parse(await ev('JSON.stringify(window.__gpuStats())'));
  out.cacheCount = await ev('window.__swbf2.ccall("swbf2_montage_count","number")');
  out.cacheViaWrap = await ev('[window.__swbf2.ccall("swbf2_montage_count","number"), window.__swbf2.ccall("swbf2_texture_packed","number"), typeof window.__swbf2.ccall]');
  out.hudTex = await ev('(document.getElementById("hTex")||{}).textContent');
  out.fboProbe = JSON.parse(await ev('JSON.stringify(window.__fboProbe())'));
  out.cacheAfterProbe = await ev('window.__swbf2.ccall("swbf2_montage_count","number")');
  out.depthAfterProbe = JSON.parse(await ev('JSON.stringify(window.__depthOrderTest())'));
  out.screenAfter = JSON.parse(await ev('JSON.stringify(window.__gpuStats())'));

  // the on-screen 3D path with a known camera: what does the engine think, and what is lit?
  await ev('window.__scene3D(true)');
  await sleep(800);
  out.sceneDefault = JSON.parse(await ev('JSON.stringify(Object.assign(window.__sceneStats(), { cache: window.__swbf2.ccall("swbf2_montage_count","number") }))'));
  await ev('window.__orbit(0,0,2000)');   // pull far back: everything should be inside
  await sleep(600);
  out.sceneFar = JSON.parse(await ev('JSON.stringify(window.__sceneStats())'));
  out.gpuFar = JSON.parse(await ev('JSON.stringify(window.__gpuStats())'));
  await ev('window.__orbit(0,0,-1500)');  // push in: some must fall outside the frustum
  // The probe path never built a montage, so its cache was empty and the 3D view could only show
  // the placeholder quad. Build one, then look at the scene the way the page's own buttons do.
  await ev('window.__buildMontage()');
  await sleep(1500);
  await ev('window.__scene3D(true)');
  await sleep(600);
  await ev('window.__orbit(0.55,0.25,-1500)');
  await sleep(700);
  out.sceneWithTextures = JSON.parse(await ev('JSON.stringify(window.__sceneStats())'));
  // (No framebuffer dump here: reading the default framebuffer between frames returns
  // black unless the context is created with preserveDrawingBuffer, and enabling that for a
  // debug view would trade real render cost for a picture. The engine's own in-frame
  // sampling - swbf2_gpu_* via window.__gpuStats - is what proves visibility.)
  await sleep(600);
  out.sceneNear = JSON.parse(await ev('JSON.stringify(window.__sceneStats())'));
  out.gpuNear = JSON.parse(await ev('JSON.stringify(window.__gpuStats())'));
} catch (e) {
  out.error = String(e);
} finally {
  out.chromeErr = stderrTail.split('\n').filter((l) => /error|trap|abort|out of memory/i.test(l)).slice(-4);
  console.log(JSON.stringify(out, null, 1));
  chrome.kill();
}
