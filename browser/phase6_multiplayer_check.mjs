#!/usr/bin/env node
// Phase 6 End-to-End Online Multiplayer Verification:
// Two separate Headless Chrome WebGPU tabs (Host + Client) discover each other through
// tools/multiplayer_server.mjs, establish a live unordered/unreliable WebRTC RTCDataChannel,
// run SWBF2's real LAN Find -> Join -> Lobby -> Launch -> Load -> 30 Hz MissionState replication
// on tat2g_con (Mos Eisley Conquest), spawn both human players, move over the wire, and render
// with zero WebGPU errors.

import { spawn } from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import http from 'node:http';

const ROOT = path.resolve(path.dirname(new URL(import.meta.url).pathname), '..');
const HTTP_PORT = 8134;
const MP_PORT = 8135;
const CDP_PORT = 9500 + (process.pid % 300);
const OUT_DIR = process.env.SWBF2_SCREENSHOT_DIR ||
  path.join(ROOT, 'build', 'screenshots');
fs.mkdirSync(OUT_DIR, { recursive: true });

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

function fetchJson(url) {
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

async function waitForHttp(url, timeoutMs = 10000) {
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
  constructor(wsUrl, label) {
    this.ws = new WebSocket(wsUrl);
    this.label = label;
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
        this.logs.push(`[${this.label}:${msg.params.type}] ${txt}`);
      } else if (msg.method === 'Runtime.exceptionThrown') {
        const d = msg.params.exceptionDetails;
        this.logs.push(`[${this.label}:exception] ${d.text} ${d.exception?.description || ''}`);
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
  close() { try { this.ws.close(); } catch {} }
}

const httpServer = spawn('python3', ['tools/serve.py', String(HTTP_PORT), '--serve-gamedata'], {
  cwd: ROOT,
  stdio: ['ignore', 'pipe', 'pipe'],
});
const mpServer = spawn(process.execPath, ['tools/multiplayer_server.mjs', String(MP_PORT), '--host', '127.0.0.1'], {
  cwd: ROOT,
  stdio: ['ignore', 'pipe', 'pipe'],
});

let chrome = null;
let hostCdp = null;
let clientCdp = null;
const userDataDir = `/tmp/swbf2_phase6_chrome_${process.pid}`;
fs.mkdirSync(userDataDir, { recursive: true });

function cleanup() {
  try { hostCdp?.close(); } catch {}
  try { clientCdp?.close(); } catch {}
  try { chrome?.kill('SIGKILL'); } catch {}
  try { httpServer.kill('SIGKILL'); } catch {}
  try { mpServer.kill('SIGKILL'); } catch {}
  try { fs.rmSync(userDataDir, { recursive: true, force: true }); } catch {}
}

function bail(err) {
  if (err) console.error(err);
  if (hostCdp?.logs.length) console.error('--- Host logs ---\n' + hostCdp.logs.slice(-40).join('\n'));
  if (clientCdp?.logs.length) console.error('--- Client logs ---\n' + clientCdp.logs.slice(-40).join('\n'));
  cleanup();
  process.exit(1);
}

try {
  await waitForHttp(`http://127.0.0.1:${HTTP_PORT}/browser/phase6_multiplayer_check.html`);
  await waitForHttp(`http://127.0.0.1:${MP_PORT}/api/health`);

  chrome = spawn('google-chrome-stable', [
    '--headless=new',
    '--no-sandbox',
    '--disable-crash-reporter',
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
  const hostTarget = targets?.find((t) => t.type === 'page');
  if (!hostTarget?.webSocketDebuggerUrl) throw new Error('Chrome CDP host page target not found');

  // Open a second independent tab for the Client peer.
  const clientTarget = await new Promise((resolve, reject) => {
    const req = http.request({
      host: '127.0.0.1',
      port: CDP_PORT,
      path: '/json/new?about:blank',
      method: 'PUT',
    }, (res) => {
      let data = '';
      res.on('data', (c) => data += c);
      res.on('end', () => {
        try { resolve(JSON.parse(data)); } catch (e) { reject(e); }
      });
    });
    req.on('error', reject);
    req.end();
  });
  if (!clientTarget?.webSocketDebuggerUrl) throw new Error('Chrome CDP client page target not found');

  hostCdp = new CDP(hostTarget.webSocketDebuggerUrl, 'host');
  clientCdp = new CDP(clientTarget.webSocketDebuggerUrl, 'client');
  await Promise.all([
    hostCdp.send('Runtime.enable'), hostCdp.send('Page.enable'),
    clientCdp.send('Runtime.enable'), clientCdp.send('Page.enable'),
  ]);

  const room = 'mos_eisley_p6';
  const wsParam = encodeURIComponent(`ws://127.0.0.1:${MP_PORT}`);
  const hostUrl = `http://127.0.0.1:${HTTP_PORT}/browser/phase6_multiplayer_check.html?role=host&room=${room}&ws=${wsParam}&name=HostOne`;
  const clientUrl = `http://127.0.0.1:${HTTP_PORT}/browser/phase6_multiplayer_check.html?role=client&room=${room}&ws=${wsParam}&name=ClientTwo`;

  await Promise.all([
    hostCdp.send('Page.navigate', { url: hostUrl }),
    clientCdp.send('Page.navigate', { url: clientUrl }),
  ]);

  // Wait for both pages to load module harness.
  for (let i = 0; i < 60; ++i) {
    const [hr, cr] = await Promise.all([
      hostCdp.evalAsync('Boolean(window.__mpReady)'),
      clientCdp.evalAsync('Boolean(window.__mpReady)'),
    ]);
    if (hr && cr) break;
    await sleep(100);
  }

  console.log('[phase6] booting Host and Client WebGPU + OPFS + WebRTC tabs...');
  // Boot Host first, then Client (so Client gets welcome + peers=[Host] and initiates WebRTC offer!).
  const hostBoot = await hostCdp.evalAsync('window.__mpApi.boot()');
  const clientBoot = await clientCdp.evalAsync('window.__mpApi.boot()');
  const roomsApi = await fetchJson(`http://127.0.0.1:${MP_PORT}/api/rooms`);
  console.log(`[phase6] IPs assigned: host=${hostBoot.welcome.ip} client=${clientBoot.welcome.ip} rooms=${JSON.stringify(roomsApi.rooms)}`);

  // Wait briefly for WebRTC ICE + DataChannel to open between the two tabs.
  let hostNet = null, clientNet = null;
  for (let i = 0; i < 40; ++i) {
    const [hs, cs] = await Promise.all([
      hostCdp.evalAsync('window.__mpApi.getSnapshot()'),
      clientCdp.evalAsync('window.__mpApi.getSnapshot()'),
    ]);
    hostNet = hs.netClient;
    clientNet = cs.netClient;
    if (hostNet.webrtcOpenChannels > 0 && clientNet.webrtcOpenChannels > 0) break;
    await sleep(100);
  }
  console.log(`[phase6] WebRTC DataChannels: hostOpen=${hostNet.webrtcOpenChannels} clientOpen=${clientNet.webrtcOpenChannels}`);

  // 1. Host starts native LAN Host lobby; Client starts native LAN FindGame server browser.
  await hostCdp.evalAsync('window.__mpApi.hostLan("MosEisleyWeb", "tat2g_con")');
  await sleep(250);
  await clientCdp.evalAsync('window.__mpApi.findLan()');

  let clientFound = null;
  for (let i = 0; i < 40; ++i) {
    clientFound = await clientCdp.evalAsync('window.__mpApi.getSnapshot()');
    if ((clientFound.snapshot.netStatus?.findGameCount || 0) >= 1) break;
    await sleep(150);
  }
  console.log(`[phase6] LAN discovery: findGameCount=${clientFound.snapshot.netStatus?.findGameCount} foundName="${clientFound.snapshot.netStatus?.foundName}"`);

  // 2. Client joins discovered Host lobby via real PKT_JOIN / PKT_SHELL_UPDATE!
  await clientCdp.evalAsync('window.__mpApi.joinFound(0)');
  let hostLobby = null, clientLobby = null;
  for (let i = 0; i < 50; ++i) {
    [hostLobby, clientLobby] = await Promise.all([
      hostCdp.evalAsync('window.__mpApi.getSnapshot()'),
      clientCdp.evalAsync('window.__mpApi.getSnapshot()'),
    ]);
    const hns = hostLobby.snapshot.netStatus;
    const cns = clientLobby.snapshot.netStatus;
    if (cns?.joinConnected && (hns?.hostPlayerCount || 0) >= 2 && hns?.hostLaunchReady) break;
    await sleep(150);
  }
  console.log(`[phase6] Lobby handshake: hostPlayers=${hostLobby.snapshot.netStatus?.hostPlayerCount} hostLaunchReady=${hostLobby.snapshot.netStatus?.hostLaunchReady} clientConnected=${clientLobby.snapshot.netStatus?.joinConnected} clientLocalIdx=${clientLobby.snapshot.netStatus?.localIdx}`);

  // 3. Host launches match -> PKT_LAUNCH / PKT_LAUNCH_ACK -> BOTH tabs load tat2g_con -> PKT_LOAD / PKT_LOAD_ACK!
  await hostCdp.evalAsync('window.__mpApi.launchMatch()');
  let hostMission = null, clientMission = null;
  for (let i = 0; i < 120; ++i) {
    [hostMission, clientMission] = await Promise.all([
      hostCdp.evalAsync('window.__mpApi.getSnapshot()'),
      clientCdp.evalAsync('window.__mpApi.getSnapshot()'),
    ]);
    if (hostMission.snapshot.status.state === 'mission' &&
        clientMission.snapshot.status.state === 'mission' &&
        hostMission.snapshot.netStatus?.readyToPlay &&
        clientMission.snapshot.netStatus?.readyToPlay) {
      break;
    }
    await sleep(250);
  }
  console.log(`[phase6] Mission synced: hostState=${hostMission.snapshot.status.state} ready=${hostMission.snapshot.netStatus?.readyToPlay} players=${hostMission.snapshot.netStatus?.connectedPlayers} | clientState=${clientMission.snapshot.status.state} ready=${clientMission.snapshot.netStatus?.readyToPlay} clientTurn=${clientMission.snapshot.netStatus?.clientTurn} ackedTurn=${clientMission.snapshot.netStatus?.ackedTurn}`);

  // 4. Spawn BOTH players into the live Mos Eisley match and move Client forward over the wire!
  const [hostSpawn, clientSpawn] = await Promise.all([
    hostCdp.evalAsync('window.__mpApi.spawnLocalPlayer()'),
    clientCdp.evalAsync('window.__mpApi.spawnLocalPlayer()'),
  ]);
  const clientPosBefore = {
    x: clientSpawn.snapshot.status.playerX,
    z: clientSpawn.snapshot.status.playerZ,
    turn: clientSpawn.snapshot.netStatus?.clientTurn || 0,
  };
  await clientCdp.evalAsync('window.__mpApi.moveForward(800)');
  await sleep(600);

  const [hostFinal, clientFinal] = await Promise.all([
    hostCdp.evalAsync('window.__mpApi.getSnapshot()'),
    clientCdp.evalAsync('window.__mpApi.getSnapshot()'),
  ]);
  const clientMovedM = +Math.hypot(
    clientFinal.snapshot.status.playerX - clientPosBefore.x,
    clientFinal.snapshot.status.playerZ - clientPosBefore.z,
  ).toFixed(3);

  const [hostRb, clientRb] = await Promise.all([
    hostCdp.evalAsync('window.__mpApi.readbackPng()'),
    clientCdp.evalAsync('window.__mpApi.readbackPng()'),
  ]);
  for (const [rb, name] of [[hostRb, 'phase6_host_tat2.png'], [clientRb, 'phase6_client_tat2.png']]) {
    if (rb?.png?.startsWith('data:image/png;base64,')) {
      const outPath = path.join(OUT_DIR, name);
      fs.writeFileSync(outPath, Buffer.from(rb.png.slice('data:image/png;base64,'.length), 'base64'));
      console.log(`saved screenshot: ${outPath}`);
    }
  }

  await Promise.all([
    hostCdp.evalAsync('window.__mpApi.stop()'),
    clientCdp.evalAsync('window.__mpApi.stop()'),
  ]);

  const summary = {
    roomsApi: roomsApi.rooms,
    host: {
      ip: hostBoot.welcome.ip,
      lobbyPlayers: hostLobby.snapshot.netStatus?.hostPlayerCount,
      missionState: hostFinal.snapshot.status.state,
      playerAlive: hostFinal.snapshot.status.playerAlive,
      playerPos: [hostFinal.snapshot.status.playerX, hostFinal.snapshot.status.playerY, hostFinal.snapshot.status.playerZ],
      remoteAlive: hostFinal.snapshot.status.remoteAlive,
      remotePos: [hostFinal.snapshot.status.remoteX, hostFinal.snapshot.status.remoteY, hostFinal.snapshot.status.remoteZ],
      draws: hostRb.meta?.draws,
      triangles: hostRb.meta?.triangles,
      wgpuErrors: hostFinal.snapshot.status.wgpuErrors,
      netStatus: hostFinal.snapshot.netStatus,
      netClient: hostFinal.netClient,
    },
    client: {
      ip: clientBoot.welcome.ip,
      foundCount: clientFound.snapshot.netStatus?.findGameCount,
      foundName: clientFound.snapshot.netStatus?.foundName,
      lobbyConnected: clientLobby.snapshot.netStatus?.joinConnected,
      missionState: clientFinal.snapshot.status.state,
      playerAlive: clientFinal.snapshot.status.playerAlive,
      playerPos: [clientFinal.snapshot.status.playerX, clientFinal.snapshot.status.playerY, clientFinal.snapshot.status.playerZ],
      remoteAlive: clientFinal.snapshot.status.remoteAlive,
      movedMeters: clientMovedM,
      draws: clientRb.meta?.draws,
      triangles: clientRb.meta?.triangles,
      wgpuErrors: clientFinal.snapshot.status.wgpuErrors,
      netStatus: clientFinal.snapshot.netStatus,
      netClient: clientFinal.netClient,
    },
  };
  console.log(JSON.stringify(summary, null, 2));

  const checks = [
    ['Room directory API /api/rooms listed active room', Array.isArray(roomsApi.rooms) && roomsApi.rooms.some((r) => r.id === room && r.players >= 2)],
    ['Host & Client assigned distinct RFC-1918 10.200.x.y IPs',
      hostBoot.welcome.ip.startsWith('10.200.') && clientBoot.welcome.ip.startsWith('10.200.') && hostBoot.welcome.ip !== clientBoot.welcome.ip],
    ['Direct WebRTC RTCDataChannel opened between the 2 Chrome tabs',
      (hostNet.webrtcOpenChannels || 0) >= 1 && (clientNet.webrtcOpenChannels || 0) >= 1],
    ['Client discovered Host via real SWBF2 LAN PKT_FIND/PKT_FOUND broadcast',
      (clientFound.snapshot.netStatus?.findGameCount || 0) >= 1],
    ['Client joined Host lobby via real PKT_JOIN/PKT_SHELL_UPDATE (hostPlayers >= 2, joinConnected)',
      clientLobby.snapshot.netStatus?.joinConnected === true && (hostLobby.snapshot.netStatus?.hostPlayerCount || 0) >= 2],
    ['Both Host and Client transitioned into MissionState (tat2g_con) via PKT_LAUNCH/PKT_LOAD',
      hostFinal.snapshot.status.state === 'mission' && clientFinal.snapshot.status.state === 'mission'],
    ['Both Host and Client reached readyToPlay === true in sync',
      hostFinal.snapshot.netStatus?.readyToPlay === true && clientFinal.snapshot.netStatus?.readyToPlay === true],
    ['Host reports connectedPlayers >= 2 in live match', (hostFinal.snapshot.netStatus?.connectedPlayers || 0) >= 2],
    ['Client 30 Hz turn replication active (clientTurn > 10 && ackedTurn > 5)',
      (clientFinal.snapshot.netStatus?.clientTurn || 0) > 10 && (clientFinal.snapshot.netStatus?.ackedTurn || 0) > 5],
    ['Real SWBF2 game packets flowed directly over WebRTC DataChannel (txWebrtc > 20 && rxWebrtc > 20)',
      (hostFinal.netClient.txWebrtc || 0) > 20 && (hostFinal.netClient.rxWebrtc || 0) > 20 &&
      (clientFinal.netClient.txWebrtc || 0) > 20 && (clientFinal.netClient.rxWebrtc || 0) > 20],
    ['Both Host and Client players spawned live in the same Mos Eisley match',
      hostFinal.snapshot.status.playerAlive === true && clientFinal.snapshot.status.playerAlive === true],
    ['Host sees Client remote soldier entity replicated alive in world',
      hostFinal.snapshot.status.remoteAlive === true],
    ['Client moved cleanly under authoritative host replication (> 0.5m)', clientMovedM > 0.5],
    ['Both tabs rendered live 3D Mos Eisley scene (draws > 100, nonBlackPct > 50)',
      (hostRb.meta?.draws || 0) > 100 && (clientRb.meta?.draws || 0) > 100 &&
      (hostRb.meta?.nonBlackPct || 0) > 50 && (clientRb.meta?.nonBlackPct || 0) > 50],
    ['Zero WebGPU errors across both multiplayer tabs',
      (hostFinal.snapshot.status.wgpuErrors || 0) === 0 && (clientFinal.snapshot.status.wgpuErrors || 0) === 0],
  ];

  let passed = 0;
  for (const [name, ok] of checks) {
    console.log(`${ok ? 'PASS' : 'FAIL'}: ${name}`);
    if (ok) ++passed;
  }
  console.log(`=== Phase 6 Two-Tab WebRTC Multiplayer Check: ${passed} / ${checks.length} passed ===`);
  if (passed !== checks.length) {
    bail();
  }
} catch (err) {
  bail(err);
} finally {
  cleanup();
}
