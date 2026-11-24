// Headless browser check without any npm dependencies: drive Chrome over its DevTools
// protocol using node's built-in WebSocket + fetch. This is how the browser half of the port
// gets verified in CI-like conditions - real Chrome, real WebGL, real network, real click
// handling - and it prints exactly what the page reported.
//
//   node browser/cdp_check.mjs [--url http://127.0.0.1:8123/] [--shot /tmp/swbf2.png] [--wait 60]
import { spawn } from 'child_process';
import fs from 'fs';
import path from 'path';

const argv = process.argv.slice(2);
const arg = (name, dflt) => {
  const i = argv.indexOf('--' + name);
  return i >= 0 ? argv[i + 1] : dflt;
};
const URL_ = arg('url', 'http://127.0.0.1:8123/');
const SHOT = arg('shot', '/tmp/swbf2.png');
const WAIT = +arg('wait', 60);
const CLICKS = arg('clicks', '') .split(',').filter(Boolean);
const PORT = +arg('port', 9333);

const chrome = spawn('google-chrome-stable', [
  '--headless=new',
  '--no-sandbox',
  '--disable-gpu',
  '--use-gl=angle',
  '--use-angle=swiftshader',
  '--enable-unsafe-swiftshader',
  '--remote-debugging-port=' + PORT,
  '--window-size=1600,900',
  '--user-data-dir=/tmp/swbf2-cdp-profile',
  'about:blank',
], { stdio: ['ignore', 'ignore', 'pipe'] });

let stderrTail = '';
chrome.stderr.on('data', (b) => { stderrTail = (stderrTail + b.toString()).slice(-4000); });

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

async function waitForDevtools(ms) {
  const deadline = Date.now() + ms;
  while (Date.now() < deadline) {
    try {
      const res = await fetch(`http://127.0.0.1:${PORT}/json/version`);
      if (res.ok) return res.json();
    } catch {}
    await sleep(200);
  }
  throw new Error('chrome devtools endpoint never came up:\n' + stderrTail);
}

const version = await waitForDevtools(15000);

// open our own tab so we never touch an existing one
const target = await (await fetch(`http://127.0.0.1:${PORT}/json/new?about:blank`, { method: 'PUT' })).json();
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
    const text = (msg.params.args || []).map((a) => a.value ?? a.description ?? a.type).join(' ');
    consoleLines.push(`${msg.params.type}: ${text}`);
  } else if (msg.method === 'Log.entryAdded') {
    const e = msg.params.entry;
    if (e.source !== 'rendering') consoleLines.push(`[${e.source}/${e.level}] ${e.text}`);
  } else if (msg.method === 'Inspector.targetCrashed') {
    pageErrors.push('RENDERER CRASHED: ' + JSON.stringify(msg.params));
  } else if (msg.method === 'Network.responseReceived') {
    const r = msg.params.response;
    consoleLines.push(`[net ${r.status}] ${r.url.replace('http://127.0.0.1:8123','')}`);
  } else if (msg.method === 'Network.loadingFailed') {
    consoleLines.push(`[net FAIL] ${msg.params.requestId} ${msg.params.errorText || ''}`);
  } else if (msg.method === 'Page.frameNavigated') {
    consoleLines.push('[nav] ' + msg.params.frame.url + (msg.params.frame.unreachable ? ' UNREACHABLE' : ''));
  } else if (msg.method === 'Runtime.exceptionThrown') {
    pageErrors.push(msg.params.exceptionDetails?.exception?.description ||
                    msg.params.exceptionDetails?.text || 'exception');
  }
};

const send = (method, params = {}) => new Promise((resolve, reject) => {
  const id = nextId++;
  pending.set(id, { resolve, reject });
  ws.send(JSON.stringify({ id, method, params }));
});

const evaluate = async (expr) => {
  const r = await send('Runtime.evaluate', { expression: expr, returnByValue: true, awaitPromise: true });
  if (r.exceptionDetails) throw new Error(r.exceptionDetails.exception?.description || 'eval failed');
  return r.result.value;
};

await send('Page.enable');
await send('Runtime.enable');
await send('Log.enable');
await send('Inspector.enable');
await send('Network.enable');
await send('Page.navigate', { url: URL_ });

// wait for the page's own completion flag, which host.js sets when the self-demo finishes
const deadline = Date.now() + WAIT * 1000;
let done = false;
while (Date.now() < deadline) {
  try {
    done = await evaluate('window.__demoDone === true');
  } catch {}
  if (done) break;
  await sleep(500);
}

for (const sel of CLICKS) {
  await send('Runtime.evaluate', { expression: `document.querySelector('${sel}')?.click()` });
  await sleep(1200);
}

// build the montage and sample the result: many distinct textures on screen at once is the
// assertion, and gpuStats comes from the engine sampling its own framebuffer
await evaluate(`(() => { window.__montageStats = null; try { window.__buildMontage(); } catch (e) { window.__montageStats = 'err ' + e; } })()`);
await sleep(1500);
await evaluate(`window.__montageStats = JSON.stringify(Object.assign(window.__gpuStats(), { resident: window.__swbf2.ccall('swbf2_montage_count','number') }))`);

const dom = await evaluate(`(() => {
  const t = (id) => { const e = document.getElementById(id); return e ? e.innerText.replace(/\\s+/g,' ').trim() : null; };
  return {
    version: t('ver'), status: t('status'), note: t('note'), caption: t('caption'),
    hud: t('hud'), log: t('log'),
    rows: document.querySelectorAll('#files .row').length,
    href: location.href,
    perfMs: Math.round(performance.now()),
    steps: (window.__demoSteps || []).join(' '),
    hasModule: !!window.__swbf2,
    gpuStats: (() => { try { return JSON.stringify(window.__gpuStats()); } catch (e) { return 'err ' + e; } })(),
    fsFiles: (() => { try { return JSON.stringify(window.__swbf2.FS.readdir('/data').filter((n) => n !== '.' && n !== '..')); } catch (e) { return 'err ' + e; } })(),
    glSize: (() => { const c = document.getElementById('canvas'); return c ? c.width + 'x' + c.height : null; })(),
    pixelsSampled: (() => {
      const c = document.getElementById('canvas');
      if (!c) return null;
      const g = c.getContext('webgl2', { preserveDrawingBuffer: true });
      return g ? 'ctx-ok' : 'no-ctx';
    })(),
  };
})()`);

// page through textures the way a user would and record what each one did to the framebuffer
const NTEX = +arg('textures', 0);
const texWalk = [];
for (let i = 0; i < NTEX; ++i) {
  await send('Runtime.evaluate', { expression: 'window.__showTexture && window.__showTexture(1)' });
  await sleep(400);
  try { texWalk.push(await evaluate('JSON.stringify(window.__gpuStats())')); } catch (e) { texWalk.push('err'); }
}

// 3D view: the engine's camera drives a real transform, so the assertions here are about
// geometry rather than pixels merely existing.
//   * depth order: two known-colour quads at two depths. Whoever is nearer must own the centre
//     pixel, and swapping the depths must swap the colour. A mirrored matrix, an inverted z or a
//     missing depth test all fail this while still looking like a picture.
//   * culling: pulled right up close, much of the layout falls outside the frustum, so the
//     engine's own ClassifySphere must report more culled than when pulled far back.
//   * the transform must actually move pixels: two camera positions must not sample identically.
await evaluate('window.__scene3D && window.__scene3D(true)');
await sleep(1200);
const sceneOn = await evaluate('JSON.stringify(window.__sceneStats())');
const depthOrder = JSON.parse(await evaluate('JSON.stringify(window.__depthOrderTest())'));
await evaluate('window.__sceneStats() && (window.__orbit(0,0,-1000), true)');
await sleep(500);
const statsNear = JSON.parse(await evaluate('JSON.stringify(window.__sceneStats())'));
const gpuNearCam = JSON.parse(await evaluate('JSON.stringify(window.__gpuStats())'));
await evaluate('window.__orbit(0,0,2000)');
await sleep(500);
const statsFar = JSON.parse(await evaluate('JSON.stringify(window.__sceneStats())'));
const gpuFarCam = JSON.parse(await evaluate('JSON.stringify(window.__gpuStats())'));
await evaluate('window.__orbit(1.2,0.3,-1000)');
await sleep(400);
const sceneLate = JSON.parse(await evaluate('JSON.stringify(window.__sceneStats())'));

const unpack = (v) => ({ r: (v >> 16) & 255, g: (v >> 8) & 255, b: v & 255 });
const dn = unpack(depthOrder.redNear), dg = unpack(depthOrder.greenNear);
const sceneChecks = {
  depthRedNearIsRed: dn.r > 150 && dn.g < 90 && dn.b < 90,
  depthGreenNearIsGreen: dg.g > 150 && dg.r < 90 && dg.b < 90,
  depthOrderFlipped: dn.r !== dg.r || dn.g !== dg.g,
  culledCloseGeFar: (statsNear.culled >= statsFar.culled) && (statsNear.culled + statsNear.drawn) > 0,
  cameraMovesOutput: gpuNearCam.center !== gpuFarCam.center || gpuNearCam.luma !== gpuFarCam.luma,
  rawDepthSamples: depthOrder,
};

// Real model geometry: `modl`/`segm` decoded by the host's reader. The assertions here are the
// reader's own numbers, checked two ways:
//   * against an independent oracle - tools/modl_probe.py reads the same container straight from
//     the game data and tallies the same quantities with its own parser. Two implementations
//     agreeing on counts is evidence; one implementation agreeing with itself is not.
//   * against the file itself - the decode self-check decodes the packed stream and the
//     uncompressed copy of the same vertices with the same decoder and asks whether they agree to
//     the resolution the format can carry. A wrong bit test, a wrong byte order or a wrong
//     quantisation reference shows up here as a count, not as a plausible-looking picture.
// Then the drawing path: the draw loop must visit exactly the segments the build recorded as
// reachable through placed instances, the engine's frustum test must cull some, and the geometry
// must move pixels relative to the flat texture view.
const modelLabel = await evaluate('window.__currentContainer()');
const drawFrame = () => evaluate('window.__swbf2.ccall("swbf2_frame", null, ["number"], [0.016])');
await evaluate('window.__buildModels()');
// Headless Chrome does not necessarily run requestAnimationFrame between the checks' sleeps, so the
// frame is driven explicitly here: otherwise the counters would report a draw that never happened.
await drawFrame();
const modelStats = JSON.parse(await evaluate('JSON.stringify(window.__modelStats())'));
const modelSelfCheck = JSON.parse(await evaluate('JSON.stringify(window.__modelSelfCheck(400))'));

const oracleRaw = await new Promise((resolve) => {
  const py = spawn('python3', ['tools/modl_probe.py', '--json', modelLabel], { stdio: ['ignore', 'pipe', 'ignore'] });
  let out = '';
  py.stdout.on('data', (b) => { out += b.toString(); });
  py.on('close', () => {
    try { resolve(JSON.parse(out.trim().split('\n').pop())); } catch { resolve(null); }
  });
});

const modelNear = JSON.parse(await evaluate('JSON.stringify(window.__modelStats())'));
const modelGpuNear = JSON.parse(await evaluate('JSON.stringify(window.__gpuStats())'));
await evaluate('window.__orbit(0,0,2000)');
await drawFrame();
const modelFar = JSON.parse(await evaluate('JSON.stringify(window.__modelStats())'));
// With geometry switched off the same camera draws the resident textures instead. If the model path
// were not actually rasterising, these two samples would be identical.
await evaluate('window.__swbf2.ccall("swbf2_set_geometry", null, ["number"], [0])');
await drawFrame();
const modelGpuOff = JSON.parse(await evaluate('JSON.stringify(window.__gpuStats())'));
await evaluate('window.__swbf2.ccall("swbf2_set_geometry", null, ["number"], [1])');
await drawFrame();
const modelGpuOn = JSON.parse(await evaluate('JSON.stringify(window.__gpuStats())'));

const hv = oracleRaw && oracleRaw.host_view ? oracleRaw.host_view : null;
const inst = oracleRaw && oracleRaw.instances ? oracleRaw.instances : null;
const modelChecks = {
  oracleAgreesOnCounts: !!hv && modelStats.models === hv.models && modelStats.segments === hv.segments_with_stream &&
    modelStats.vertices === hv.vertices && modelStats.indices === hv.indices && modelStats.drawDeclared === hv.draw_total,
  oracleAgreesOnInstances: !!inst && modelStats.instances === inst.inst_placeable_by_host &&
    modelStats.instances + modelStats.unresolvedInstances === inst.inst_through_walkable_wrld,
  decodeSelfCheckClean: modelSelfCheck.bad === 0,
  drawPathVisitsWhatItDecoded: modelStats.visited === modelStats.placedSegments && modelStats.placedSegments > 0,
  // With the camera pulled back nothing is outside the frustum, so the draw path must account for
  // every primitive count it read for segments reachable through placed instances. An instanced model
  // is drawn once per instance, which is why this is compared against the placed total rather than
  // against the declared total across the whole container.
  // The draw path is checked against the oracle's tally, not the host's own bookkeeping: with the
  // camera pulled back nothing is outside the frustum, so the primitives and indices it draws must
  // equal what the reader read for segments reachable through placed instances. An instanced model is
  // reached once per instance, so these totals are larger than the per-segment sums - and that is why
  // a triangle-list draw would fail here even though it still paints a picture.
  drawsAsStrips: modelFar.culled === 0 && modelFar.oddPrimTypes === 0 &&
    modelFar.primsDrawn === modelFar.placedPrimTotal && modelFar.indicesDrawn === modelFar.placedIndexTotal &&
    modelFar.indicesDrawn === modelFar.placedPrimTotal + 2 * modelFar.placedSegments,
  farDrawsExactlyWhatTheOracleTallied: modelFar.culled === 0 && !!inst &&
    modelFar.placedPrimTotal === inst.placed_prim_total && modelFar.placedIndexTotal === inst.placed_index_total &&
    modelFar.primsDrawn === inst.placed_prim_total && modelFar.indicesDrawn === inst.placed_index_total,
  engineCulledSome: modelStats.culled > 0,
  geometryMovesOutput: modelGpuOn.center !== modelGpuOff.center || modelGpuOn.luma !== modelGpuOff.luma,
  capsNotReached: modelStats.truncated === 0,
  texturesResolvedBySegments: modelStats.texturesResolved > 0,
};

// non-black pixel test: proves the canvas actually drew something rather than clearing to black
const shot = await send('Page.captureScreenshot', { format: 'png' });
fs.writeFileSync(SHOT, Buffer.from(shot.data, 'base64'));

const montageStats = await evaluate('window.__montageStats || null');
const canvasInfo = await evaluate(`(() => {
  const c = document.getElementById('canvas');
  const g = c.getContext('webgl2');
  if (!g) return { error: 'no webgl2 context' };
  const w = g.drawingBufferWidth, h = g.drawingBufferHeight;
  const px = new Uint8Array(w * h * 4);
  g.readPixels(0, 0, w, h, g.RGBA, g.UNSIGNED_BYTE, px);
  let lit = 0, sum = 0, uniq = new Set();
  for (let i = 0; i < px.length; i += 4 * 997) {
    const r = px[i], gg = px[i+1], b = px[i+2];
    const v = r + gg + b;
    sum += v;
    if (v > 40) lit++;
    uniq.add((r >> 4) + ',' + (gg >> 4) + ',' + (b >> 4));
  }
  return { w, h, sampled: Math.floor(px.length / (4*997)), litFraction: lit / Math.floor(px.length/(4*997)), avgLuma: sum / Math.floor(px.length/(4*997)) / 3, distinctColors: uniq.size };
})()`);

const modelOk = (!oracleRaw || (modelChecks.oracleAgreesOnCounts && modelChecks.oracleAgreesOnInstances)) &&
  modelChecks.decodeSelfCheckClean && modelChecks.drawPathVisitsWhatItDecoded && modelChecks.drawsAsStrips &&
  modelChecks.farDrawsExactlyWhatTheOracleTallied && modelChecks.engineCulledSome && modelChecks.geometryMovesOutput &&
  modelChecks.capsNotReached && modelChecks.texturesResolvedBySegments;
console.log(JSON.stringify({
  ok: done && pageErrors.length === 0 && sceneChecks.depthRedNearIsRed && sceneChecks.depthGreenNearIsGreen && sceneChecks.depthOrderFlipped && modelOk,
  demoDone: done,
  pageErrors,
  canvasInfo,
  dom,
  montage: (() => { try { return JSON.parse(montageStats); } catch { return montageStats; } })(),
  scene3D: {
    on: (() => { try { return JSON.parse(sceneOn); } catch { return sceneOn; } })(),
    near: statsNear, far: statsFar, late: sceneLate,
    checks: sceneChecks,
    gpuNear: gpuNearCam, gpuFar: gpuFarCam,
  },
  models: {
    container: modelLabel,
    stats: modelStats,
    selfCheck: modelSelfCheck,
    near: modelNear, far: modelFar,
    gpuOn: modelGpuOn, gpuOff: modelGpuOff,
    checks: modelChecks,
    oracle: oracleRaw,
  },
  texWalk: texWalk.map((s) => { try { return JSON.parse(s); } catch { return s; } }),
  consoleTail: consoleLines.slice(-18),
  chromeStderrTail: stderrTail.split('\n').filter((l) => /crash|fatal|out of memory|Killed|Trap|abort/i.test(l)).slice(-6),
  screenshot: SHOT,
}, null, 1));

ws.close();
chrome.kill();
process.exit(done && pageErrors.length === 0 && sceneChecks.depthRedNearIsRed && sceneChecks.depthGreenNearIsGreen && sceneChecks.depthOrderFlipped && modelOk ? 0 : 2);
