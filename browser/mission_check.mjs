// Headless Chrome check of the playable mission, with no npm dependencies.
//
//   node browser/mission_check.mjs [--url http://127.0.0.1:8123/?mission=1]
//   [--shot /tmp/swbf2-mission.png] [--wait 120]
//
// It loads the page, checks the engine's own mission state, drives W through a real DevTools key
// event (not a position write), and captures the rendered canvas. The class record must be the
// small extracted one, the trooper's model must be appended, and the position must actually move.
import { spawn } from 'child_process';
import fs from 'fs';

const argv = process.argv.slice(2);
const arg = (name, dflt) => {
  const i = argv.indexOf('--' + name);
  return i >= 0 ? argv[i + 1] : dflt;
};
const URL_ = arg('url', 'http://127.0.0.1:8123/?mission=1');
const SHOT = arg('shot', '/tmp/swbf2-mission.png');
const WAIT = +arg('wait', 120);
const PORT = +arg('port', 9444);
const CHROME = process.env.CHROME || '/usr/bin/chromium';

const chrome = spawn(CHROME, [
  '--headless=new', '--no-sandbox', '--disable-gpu', '--use-gl=angle',
  '--use-angle=swiftshader', '--enable-unsafe-swiftshader',
  '--remote-debugging-port=' + PORT, '--window-size=1280,720',
  '--user-data-dir=/tmp/swbf2-mission-profile', 'about:blank',
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
const target = await (await fetch(`http://127.0.0.1:${PORT}/json/new?about:blank`, { method: 'PUT' })).json();
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
  if (msg.method === 'Runtime.exceptionThrown') {
    pageErrors.push(msg.params.exceptionDetails?.exception?.description || msg.params.exceptionDetails?.text);
  } else if (msg.method === 'Inspector.targetCrashed') {
    pageErrors.push('RENDERER CRASHED');
  } else if (msg.method === 'Runtime.consoleAPICalled') {
    consoleLines.push((msg.params.args || []).map((a) => a.value ?? a.description ?? a.type).join(' '));
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

try {
  await send('Page.enable');
  await send('Runtime.enable');
  await send('Inspector.enable');
  await send('Page.navigate', { url: URL_ });

  const deadline = Date.now() + WAIT * 1000;
  let ready = false;
  while (Date.now() < deadline) {
    try { ready = await evaluate('window.__demoDone === true'); } catch {}
    if (ready) break;
    await sleep(500);
  }
  if (!ready) throw new Error('the mission never reached its completion flag');

  const before = await evaluate('window.__missionState()');
  const viewport = await evaluate(`(() => {
    const p = window.__swbf2._malloc(28);
    window.__swbf2.ccall('swbf2_viewport_state', 'number', ['number', 'number'], [p, 7]);
    const out = Array.from({ length: 7 }, (_, i) => window.__swbf2.getValue(p + i * 4, 'float'));
    window.__swbf2._free(p);
    return out;
  })()`);
  const fsInfo = await evaluate(`(() => {
    const M = window.__swbf2;
    const s = M.FS.stat('/data/side_imp.lvl');
    return { size: s.size, files: M.FS.readdir('/data').filter((n) => !n.startsWith('.')) };
  })()`);
  await evaluate('window.__swbf2.ccall("swbf2_frame", null, ["number"], [1/30])');
  const beforeMove = await evaluate('window.__missionState()');

  // A real browser key event: the input driver turns this into the engine's move axis.
  await send('Input.dispatchKeyEvent', {
    type: 'keyDown', key: 'w', code: 'KeyW', windowsVirtualKeyCode: 87,
  });
  await sleep(1200);
  await send('Input.dispatchKeyEvent', {
    type: 'keyUp', key: 'w', code: 'KeyW', windowsVirtualKeyCode: 87,
  });
  await sleep(250);
  const after = await evaluate('window.__missionState()');

  // Move into range of the first target, then hold the real left mouse button. The weapon,
  // ordnance and damage are all the engine's; this only supplies the trigger.
  await send('Input.dispatchKeyEvent', {
    type: 'keyDown', key: 'w', code: 'KeyW', windowsVirtualKeyCode: 87,
  });
  await sleep(1800);
  await send('Input.dispatchKeyEvent', {
    type: 'keyUp', key: 'w', code: 'KeyW', windowsVirtualKeyCode: 87,
  });
  // Line up with the second target, which is 0.75 m left of the player's initial line.
  await send('Input.dispatchKeyEvent', {
    type: 'keyDown', key: 'd', code: 'KeyD', windowsVirtualKeyCode: 68,
  });
  await sleep(180);
  await send('Input.dispatchKeyEvent', {
    type: 'keyUp', key: 'd', code: 'KeyD', windowsVirtualKeyCode: 68,
  });
  await send('Input.dispatchMouseEvent', {
    type: 'mousePressed', x: 400, y: 300, button: 'left', clickCount: 1,
  });
  await sleep(3000);
  await send('Input.dispatchMouseEvent', {
    type: 'mouseReleased', x: 400, y: 300, button: 'left', clickCount: 1,
  });
  await sleep(500);
  const combat = JSON.parse(await evaluate(
    'window.__swbf2.ccall("swbf2_combat_report", "string", [], [])'));
  const audio = await evaluate(`(() => ({
    state: window.__missionAudioState(),
    hash: window.__swbf2.ccall('audio_browser_sample_hash', 'number').toString(16),
    rate: window.__swbf2.ccall('audio_browser_sample_rate', 'number'),
    frames: window.__swbf2.ccall('audio_browser_sample_frames', 'number'),
    bytes: window.__swbf2.ccall('audio_browser_sample_bytes', 'number'),
  }))()`);

  const screenshot = await send('Page.captureScreenshot', { format: 'png' });
  fs.writeFileSync(SHOT, Buffer.from(screenshot.data, 'base64'));
  const hud = await evaluate(`(() => {
    const engineLog = [];
    for (let i = 0; i < 500; ++i) {
      const line = window.__swbf2.ccall('swbf2_log_line', 'string', ['number'], [i]);
      if (!line) break;
      engineLog.push(line);
    }
    return {
      health: document.getElementById('hHealth')?.textContent,
      caption: document.getElementById('caption')?.textContent,
      note: document.getElementById('note')?.textContent,
      log: engineLog.slice(-24),
    };
  })()`);

  const distance = Math.hypot(after.x - beforeMove.x, after.z - beforeMove.z);
  const checks = {
    missionReady: before.ready === true,
    stormtrooperModelAppended: before.modelSegments > 0,
    stormtrooperRasterised: before.trooperDrawn > 0 && after.trooperDrawn > 0,
    engineChaseCamera: before.engineCamera === 1 && Number.isFinite(before.fov),
    healthFromEngine: before.health === 400,
    smallClassRecord: fsInfo.size === 3406400,
    wMovesTrooper: distance > 0.5,
    blasterCreatedOrdnance: combat.ordnance_total > 0,
    blasterDamagedTarget: combat.damage_dealt > 0 && combat.hits > 0,
    audioStarted: audio.state.started === true,
    fireSampleExact: audio.hash === '2eb95650' && audio.rate === 44100 &&
      audio.frames === 48360 && audio.bytes === 96720,
    targetsSpawned: combat.targets_spawned === 4,
    hudShowsHealth: hud.health === '400',
    noPageErrors: pageErrors.length === 0,
  };
  const failed = Object.entries(checks).filter(([, ok]) => !ok).map(([name]) => name);
  console.log(JSON.stringify({
    checks, failed, before, after, viewport, distance, combat, audio, fsInfo, hud, pageErrors,
    consoleTail: consoleLines.slice(-12), shot: SHOT,
  }, null, 1));
  if (failed.length) process.exitCode = 1;
} catch (e) {
  console.error(e && e.stack || e);
  console.error(consoleLines.slice(-20).join('\n'));
  process.exitCode = 1;
} finally {
  try { ws.close(); } catch {}
  chrome.kill('SIGTERM');
}
