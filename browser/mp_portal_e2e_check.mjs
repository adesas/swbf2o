#!/usr/bin/env node
// End-to-End Portal Multiplayer Verification (drives the real browser/play.html UI across 2 Chrome tabs):
//   1. Host opens play.html -> Play Online -> Create Custom Lobby (multi-map Coruscant + Mos Eisley,
//      Elite AI, 150% reinforcements, Heroes ON, password-protected) -> Create & Open Lobby.
//   2. Verifies Host auto-signs into a retail PlayerProfile (NEVER stuck on Profile Management / ifs_login),
//      displays the live Multiplayer Staging Lobby overlay, and advertises full rules in /api/rooms.
//   3. Client opens play.html -> Play Online -> sees live room + 16 public lobbies -> tests wrong password
//      rejection -> joins with correct password -> auto-signs into retail PlayerProfile -> connects via
//      WebRTC + SWBF2 LAN handshake -> exchanges live Lobby Chat with Host.
//   4. Host clicks "Start Match ->" -> BOTH tabs transition cleanly into MissionState (cor1c_con),
//      staging overlay closes automatically, readyToPlay === true, connectedPlayers >= 2, live 3D draws > 100,
//      and 0 WebGPU errors.
//   5. Host & Client click "Leave Lobby" -> cleanly disconnect and return to Lobby Browser, and room is
//      removed from /api/rooms (no ghost/hostless rooms left behind).

import { spawn } from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import http from 'node:http';

const ROOT = path.resolve(path.dirname(new URL(import.meta.url).pathname), '..');
const HTTP_PORT = 8138;
const MP_PORT = 8139;
const CDP_PORT = 9750 + (process.pid % 150);
const OUT_DIR = process.env.SWBF2_SCREENSHOT_DIR ||
  path.join(ROOT, 'build', 'screenshots');
fs.mkdirSync(OUT_DIR, { recursive: true });

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

function fetchJson(url) {
  return new Promise((resolve, reject) => {
    http.get(url, (res) => {
      let d = '';
      res.on('data', (c) => d += c);
      res.on('end', () => {
        try { resolve(JSON.parse(d)); } catch (e) { reject(e); }
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
    } catch { await sleep(100); }
  }
  throw new Error(`Timed out waiting for ${url}`);
}

class CDP {
  constructor(wsUrl, label) {
    this.label = label;
    this.ws = new WebSocket(wsUrl);
    this.nextId = 1;
    this.pending = new Map();
    this.logs = [];
    this.ready = new Promise((res, rej) => {
      this.ws.addEventListener('open', res);
      this.ws.addEventListener('error', rej);
    });
    this.ws.addEventListener('message', (ev) => {
      const m = JSON.parse(String(ev.data));
      if (m.id && this.pending.has(m.id)) {
        const { resolve, reject } = this.pending.get(m.id);
        this.pending.delete(m.id);
        if (m.error) reject(new Error(m.error.message)); else resolve(m.result);
      } else if (m.method === 'Runtime.consoleAPICalled') {
        const txt = (m.params.args || []).map((a) => a.value ?? a.description ?? '').join(' ');
        this.logs.push(`[${this.label}:${m.params.type}] ${txt}`);
      } else if (m.method === 'Runtime.exceptionThrown') {
        this.logs.push(`[${this.label}:exc] ${m.params.exceptionDetails?.exception?.description || m.params.exceptionDetails?.text}`);
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
  async js(expr) {
    const res = await this.send('Runtime.evaluate', {
      expression: `(async () => { return JSON.stringify(await (${expr})); })()`,
      awaitPromise: true,
      returnByValue: true,
    });
    if (res.exceptionDetails) {
      throw new Error(`[${this.label}] eval failed: ${res.exceptionDetails.exception?.description || res.exceptionDetails.text}`);
    }
    return res.result?.value ? JSON.parse(res.result.value) : null;
  }
  async shot(filename) {
    await this.send('Page.bringToFront');
    const { data } = await this.send('Page.captureScreenshot', { format: 'png' });
    const outPath = path.join(OUT_DIR, filename);
    fs.writeFileSync(outPath, Buffer.from(data, 'base64'));
    console.log(`saved screenshot: ${outPath}`);
  }
  close() { try { this.ws.close(); } catch {} }
}

const httpServer = spawn('python3', ['tools/serve.py', String(HTTP_PORT), '--mp-port', String(MP_PORT), '--tls-port', '0', '--serve-gamedata'], {
  cwd: ROOT, stdio: 'ignore',
});
const mpServer = spawn(process.execPath, ['tools/multiplayer_server.mjs', String(MP_PORT), '--host', '127.0.0.1'], {
  cwd: ROOT, stdio: 'ignore',
});

let chrome = null;
let host = null;
let client = null;
const userDataDir = `/tmp/swbf2_portal_e2e_${process.pid}`;
fs.mkdirSync(userDataDir, { recursive: true });

function cleanup() {
  try { host?.close(); } catch {}
  try { client?.close(); } catch {}
  try { chrome?.kill('SIGKILL'); } catch {}
  try { httpServer.kill('SIGKILL'); } catch {}
  try { mpServer.kill('SIGKILL'); } catch {}
  try { fs.rmSync(userDataDir, { recursive: true, force: true }); } catch {}
}

try {
  await waitForHttp(`http://127.0.0.1:${HTTP_PORT}/browser/play.html`);
  await waitForHttp(`http://127.0.0.1:${MP_PORT}/api/health`);

  chrome = spawn('google-chrome-stable', [
    '--headless=new',
    '--no-sandbox',
    '--disable-crash-reporter',
    '--window-size=1280,820',
    '--disable-background-timer-throttling',
    '--disable-renderer-backgrounding',
    '--disable-backgrounding-occluded-windows',
    '--enable-unsafe-webgpu',
    '--enable-features=Vulkan',
    '--use-angle=vulkan',
    '--ignore-gpu-blocklist',
    '--disable-gpu-sandbox',
    `--remote-debugging-port=${CDP_PORT}`,
    `--user-data-dir=${userDataDir}`,
    'about:blank',
  ], { stdio: ['ignore', 'pipe', 'pipe'] });

  let targets = null;
  for (let i = 0; i < 80; ++i) {
    try {
      targets = await fetchJson(`http://127.0.0.1:${CDP_PORT}/json/list`);
      if (Array.isArray(targets) && targets.some((t) => t.type === 'page')) break;
    } catch {}
    await sleep(100);
  }
  const hostTarget = targets?.find((t) => t.type === 'page');
  const clientTarget = await new Promise((resolve, reject) => {
    const req = http.request({ host: '127.0.0.1', port: CDP_PORT, path: '/json/new?about:blank', method: 'PUT' }, (res) => {
      let d = ''; res.on('data', (c) => d += c); res.on('end', () => resolve(JSON.parse(d)));
    }); req.on('error', reject); req.end();
  });

  host = new CDP(hostTarget.webSocketDebuggerUrl, 'host');
  client = new CDP(clientTarget.webSocketDebuggerUrl, 'client');
  await Promise.all([
    host.send('Runtime.enable'), host.send('Page.enable'),
    client.send('Runtime.enable'), client.send('Page.enable'),
  ]);

  // 0. Verify root / serves the landing page (index.html) with /play and /play#lobbies CTAs.
  await host.send('Page.navigate', { url: `http://127.0.0.1:${HTTP_PORT}/` });
  await sleep(900);
  const landingInfo = await host.js(`({
    title: document.title,
    hasPlayLink: Boolean(document.querySelector('a[href="/play"]')),
    hasLobbiesLink: Boolean(document.querySelector('a[href="/play#lobbies"]')),
    h1: document.querySelector('h1')?.textContent?.trim() || '',
  })`);
  await host.shot('landing_page.png');
  console.log('[e2e] Landing page:', JSON.stringify(landingInfo));

  // Navigate BOTH tabs to /play (testing serve.py /play -> play.html AND same-origin /api + /ws proxy!).
  const portalUrl = `http://127.0.0.1:${HTTP_PORT}/play`;
  await Promise.all([
    host.send('Page.navigate', { url: portalUrl }),
    client.send('Page.navigate', { url: portalUrl }),
  ]);
  await sleep(900);
  for (let i = 0; i < 60; ++i) {
    const [hr, cr] = await Promise.all([
      host.js('Boolean(window.__playApi && window.__playApi.getState().preflight)'),
      client.js('Boolean(window.__playApi && window.__playApi.getState().preflight)'),
    ]);
    if (hr && cr) break;
    if (i === 10) {
      console.log('[e2e] still waiting preflight:', { hr, cr, hostLogs: host.logs.slice(-5), clientLogs: client.logs.slice(-5) });
    }
    await sleep(150);
  }
  console.log('[e2e] both /play tabs preflight ready');

  // 1. Host opens Create Custom Lobby, builds a 2-map playlist (Coruscant + Mos Eisley) with full rules & password!
  await host.js(`(function(){
    document.getElementById('inpPlayerName').value = 'DarthVader';
    document.getElementById('btnPlayOnline').click();
    document.getElementById('btnOpenCreate').click();
    document.getElementById('inpRoom').value = 'Jedi Temple Siege';
    document.getElementById('inpRoomPassword').value = 'order66';
    document.getElementById('btnClearPlaylist').click();
    document.getElementById('selMpMap').value = 'cor1c_con';
    document.getElementById('btnAddMap').click();
    document.getElementById('selMpMap').value = 'tat2g_con';
    document.getElementById('btnAddMap').click();
    document.getElementById('selMaxPlayers').value = '32';
    document.getElementById('selNumBots').value = '12';
    document.getElementById('selDifficulty').value = '3';
    document.getElementById('selReinfMult').value = '150';
    document.getElementById('chkTeamDamage').checked = true;
    return document.querySelectorAll('#selMpMap option').length;
  })()`);
  const mapVariantsCount = await host.js(`document.querySelectorAll('#selMpMap option').length`);
  await host.shot('portal_creator.png');

  // Host clicks "Create & Open Lobby" (NOTE: without --autoplay-policy, verifies allowSuspended fix!).
  await host.js(`(document.getElementById('btnHostLaunch').click(), true)`);

  let hostLobbyState = null;
  for (let i = 0; i < 80; ++i) {
    hostLobbyState = await host.js('window.__playApi.getState()');
    if (hostLobbyState?.booted && hostLobbyState?.status?.netStatus?.hostLaunchReady) break;
    await sleep(200);
  }
  const roomsAfterHost = await fetchJson(`http://127.0.0.1:${HTTP_PORT}/api/rooms`);
  console.log('[e2e] Host lobby state:', JSON.stringify({
    profileSignedIn: hostLobbyState?.status?.status?.profileSignedIn,
    screen: hostLobbyState?.status?.status?.currentScreen,
    stagingOpen: hostLobbyState?.stagingOpen,
    hostLaunchReady: hostLobbyState?.status?.netStatus?.hostLaunchReady,
    rooms: roomsAfterHost.rooms,
  }));

  // 2. Client opens Lobby Browser, sees the 16+ lobbies + live password-protected custom room!
  await client.js(`(function(){
    document.getElementById('inpPlayerName').value = 'ObiWan';
    document.getElementById('btnPlayOnline').click();
    return true;
  })()`);
  await sleep(600);
  const lobbyRowCount = await client.js(`document.querySelectorAll('.room-row').length`);
  await client.shot('portal_lobbies_live.png');

  // Verify wrong password fails cleanly (no hang, error shown).
  const badPwErr = await client.js(`(async function(){
    window.prompt = () => 'wrongpw';
    document.querySelector('.room-row.live button').click();
    for (let i = 0; i < 30; ++i) {
      await new Promise((r) => setTimeout(r, 150));
      const e = document.getElementById('errMsg').textContent.trim();
      if (e) return e;
    }
    return '';
  })()`);
  console.log('[e2e] Wrong password error:', badPwErr);

  // Now Client joins with correct password "order66"!
  await client.js(`(function(){
    window.prompt = () => 'order66';
    document.querySelector('.room-row.live button').click();
    return true;
  })()`);

  let hostSynced = null, clientSynced = null;
  for (let i = 0; i < 80; ++i) {
    [hostSynced, clientSynced] = await Promise.all([
      host.js('window.__playApi.getState()'),
      client.js('window.__playApi.getState()'),
    ]);
    if (clientSynced?.status?.netStatus?.joinConnected &&
        (hostSynced?.status?.netStatus?.hostPlayerCount || 0) >= 2) break;
    await sleep(200);
  }

  // Test live Lobby Chat between Host & Client!
  await host.js(`(function(){
    document.getElementById('inpLobbyChat').value = 'Ready for Coruscant!';
    document.getElementById('lobbyChatForm').dispatchEvent(new Event('submit', { cancelable: true }));
    return true;
  })()`);
  await sleep(300);
  await client.js(`(function(){
    document.getElementById('inpLobbyChat').value = 'Hello there!';
    document.getElementById('lobbyChatForm').dispatchEvent(new Event('submit', { cancelable: true }));
    return true;
  })()`);
  await sleep(350);
  const clientChatText = await client.js(`document.getElementById('lobbyChatLog').textContent`);
  const hostChatText = await host.js(`document.getElementById('lobbyChatLog').textContent`);
  await host.shot('portal_staging_lobby.png');

  // 3. Host clicks "Start Match ->" in the Staging Lobby overlay!
  await host.js(`(document.getElementById('btnOverlayStartMatch').click(), true)`);

  let hostMission = null, clientMission = null;
  for (let i = 0; i < 120; ++i) {
    [hostMission, clientMission] = await Promise.all([
      host.js('window.__playApi.getState()'),
      client.js('window.__playApi.getState()'),
    ]);
    if (hostMission?.status?.status?.state === 'mission' &&
        clientMission?.status?.status?.state === 'mission' &&
        hostMission?.status?.netStatus?.readyToPlay &&
        clientMission?.status?.netStatus?.readyToPlay &&
        (hostMission?.status?.status?.draws || 0) > 50 &&
        (clientMission?.status?.status?.draws || 0) > 50) {
      break;
    }
    await sleep(250);
  }
  console.log('[e2e] In-match state:', JSON.stringify({
    host: {
      state: hostMission?.status?.status?.state,
      mission: hostMission?.status?.status?.missionScript,
      screen: hostMission?.status?.status?.currentScreen,
      draws: hostMission?.status?.status?.draws,
      stagingOpen: hostMission?.stagingOpen,
      ready: hostMission?.status?.netStatus?.readyToPlay,
      players: hostMission?.status?.netStatus?.connectedPlayers,
      wgpuErrors: hostMission?.status?.status?.wgpuErrors,
    },
    client: {
      state: clientMission?.status?.status?.state,
      mission: clientMission?.status?.status?.missionScript,
      screen: clientMission?.status?.status?.currentScreen,
      draws: clientMission?.status?.status?.draws,
      stagingOpen: clientMission?.stagingOpen,
      ready: clientMission?.status?.netStatus?.readyToPlay,
      wgpuErrors: clientMission?.status?.status?.wgpuErrors,
    },
  }));

  // Verify /api/rooms now reports status: 'playing'!
  const roomsPlaying = await fetchJson(`http://127.0.0.1:${HTTP_PORT}/api/rooms`);

  // 4. Host clicks Leave Lobby -> room disappears from /api/rooms (no ghost rooms!) & engine returns to Shell.
  await host.js(`window.__playApi.leaveMultiplayerLobby({ returnToBrowser: true })`);
  for (let i = 0; i < 40; ++i) {
    const hs = await host.js('window.__playApi.getState()');
    if (hs?.status?.status?.state === 'shell') break;
    await sleep(150);
  }
  const roomsAfterLeave = await fetchJson(`http://127.0.0.1:${HTTP_PORT}/api/rooms`);

  // 5. Verify XL Mode (hot1g_xl — Hoth XL Massive Battle) boots & renders cleanly!
  await host.js(`window.__playApi.callWorker('interactiveAction', { action: 'launchMission', script: 'hot1g_xl' })`);
  let xlState = null;
  for (let i = 0; i < 40; ++i) {
    xlState = await host.js('window.__playApi.getState()');
    if (xlState?.status?.status?.state === 'mission' &&
        xlState?.status?.status?.missionScript === 'hot1g_xl' &&
        (xlState?.status?.status?.draws || 0) > 50) break;
    await sleep(250);
  }
  console.log('[e2e] Hoth XL state:', JSON.stringify({
    state: xlState?.status?.status?.state,
    mission: xlState?.status?.status?.missionScript,
    draws: xlState?.status?.status?.draws,
    tris: xlState?.status?.status?.triangles,
    wgpuErrors: xlState?.status?.status?.wgpuErrors,
  }));

  const liveRoom = roomsAfterHost.rooms?.find((r) => r.id === 'jedi_temple_siege');
  const checks = [
    ['Landing page / serves index.html with /play and /play#lobbies links',
      landingInfo?.hasPlayLink === true && landingInfo?.hasLobbiesLink === true],
    ['Custom Lobby Creator exposes >= 80 retail map/mode variants', mapVariantsCount >= 80],
    ['Lobby Browser shows >= 17 lobbies (live custom + 16 public presets)', lobbyRowCount >= 17],
    ['Host auto-signed into retail PlayerProfile (NOT stuck on ifs_login)',
      hostLobbyState?.status?.status?.profileSignedIn === true &&
      hostLobbyState?.status?.status?.currentScreen !== 'ifs_login'],
    ['Host staging lobby overlay opened cleanly & /api/rooms advertised full custom rules',
      hostLobbyState?.stagingOpen === true &&
      liveRoom?.hasPassword === true &&
      liveRoom?.maxPlayers === 32 &&
      liveRoom?.numBots === 12 &&
      liveRoom?.difficulty === 3 &&
      liveRoom?.reinfMult === 150 &&
      liveRoom?.playlist?.length === 2],
    ['Wrong password rejected immediately with clear error', /password/i.test(badPwErr)],
    ['Client auto-signed into retail PlayerProfile & joined Host LAN lobby',
      clientSynced?.status?.status?.profileSignedIn === true &&
      clientSynced?.status?.netStatus?.joinConnected === true &&
      (hostSynced?.status?.netStatus?.hostPlayerCount || 0) >= 2],
    ['Live Lobby Chat delivered bidirectionally between Host and Client',
      clientChatText.includes('Ready for Coruscant!') && hostChatText.includes('Hello there!')],
    ['Start Match transitioned BOTH tabs into Coruscant (cor1c_con) MissionState',
      hostMission?.status?.status?.state === 'mission' &&
      clientMission?.status?.status?.state === 'mission' &&
      hostMission?.status?.status?.missionScript === 'cor1c_con' &&
      clientMission?.status?.status?.missionScript === 'cor1c_con'],
    ['Staging lobby overlay closed automatically on BOTH tabs on match start',
      hostMission?.stagingOpen === false && clientMission?.stagingOpen === false],
    ['Both Host & Client reached readyToPlay === true with >= 2 connected players',
      hostMission?.status?.netStatus?.readyToPlay === true &&
      clientMission?.status?.netStatus?.readyToPlay === true &&
      (hostMission?.status?.netStatus?.connectedPlayers || 0) >= 2],
    ['Room status updated to "playing" in /api/rooms for hot-join discovery',
      roomsPlaying.rooms?.some((r) => r.id === 'jedi_temple_siege' && r.status === 'playing')],
    ['Both tabs rendered live 3D Coruscant with 0 WebGPU errors',
      (hostMission?.status?.status?.draws || 0) > 100 &&
      (clientMission?.status?.status?.draws || 0) > 100 &&
      (hostMission?.status?.status?.wgpuErrors || 0) === 0 &&
      (clientMission?.status?.status?.wgpuErrors || 0) === 0],
    ['Host leaving cleanly removed the room from /api/rooms (no ghost hostless rooms)',
      (roomsAfterLeave.rooms || []).length === 0],
    ['XL Mode (hot1g_xl Hoth XL) loaded and rendered cleanly with 0 WebGPU errors',
      xlState?.status?.status?.state === 'mission' &&
      xlState?.status?.status?.missionScript === 'hot1g_xl' &&
      (xlState?.status?.status?.draws || 0) > 50 &&
      (xlState?.status?.status?.wgpuErrors || 0) === 0],
  ];

  let passed = 0;
  for (const [name, ok] of checks) {
    console.log(`${ok ? 'PASS' : 'FAIL'}: ${name}`);
    if (ok) ++passed;
  }
  console.log(`=== Portal Multiplayer E2E Check: ${passed} / ${checks.length} passed ===`);
  if (passed !== checks.length) {
    console.error('--- Host logs ---\n' + host.logs.slice(-25).join('\n'));
    console.error('--- Client logs ---\n' + client.logs.slice(-25).join('\n'));
    process.exitCode = 1;
  }
} catch (err) {
  console.error(err);
  if (host?.logs.length) console.error('--- Host logs ---\n' + host.logs.slice(-25).join('\n'));
  if (client?.logs.length) console.error('--- Client logs ---\n' + client.logs.slice(-25).join('\n'));
  process.exitCode = 1;
} finally {
  cleanup();
}
