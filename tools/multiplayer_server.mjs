#!/usr/bin/env node
// Phase 6 Zero-Dependency WebRTC Signaling + Room Directory + Binary WebSocket UDP Relay Server.
//
// Usage:
//   vendor/emsdk/node/24.19.0_64bit/bin/node tools/multiplayer_server.mjs [port] [--host 127.0.0.1]
//
// Responsibilities:
//   1. Assigns every peer in a room a stable RFC-1918 IPv4 address in 10.200.<room>.<slot>
//      (satisfies SWBF2 NetMatch::IsPrivateIP() so native LAN UDP packets work unmodified).
//   2. Relays WebRTC SDP offer/answer & ICE candidates so browsers establish direct unordered,
//      unreliable RTCDataChannel ({ ordered: false, maxRetransmits: 0 }) links.
//   3. Routes 12-byte-framed binary UDP datagrams for LAN broadcast discovery (255.255.255.255:3656)
//      and WebSocket relay fallback whenever WebRTC ICE is still negotiating or blocked by NAT.
//   4. Serves GET /api/rooms and GET /api/health for browser room-list discovery.

import http from 'node:http';
import crypto from 'node:crypto';

const args = process.argv.slice(2);
let PORT = 8133;
let HOST = '0.0.0.0';
for (let i = 0; i < args.length; ++i) {
  if (args[i] === '--host' && args[i + 1]) {
    HOST = args[++i];
  } else if (/^\d+$/.test(args[i])) {
    PORT = Number(args[i]);
  }
}

const WS_GUID = '258EAFA5-E914-47DA-95CA-C5AB0DC85B11';
const MAX_WS_PAYLOAD = 65536;
const MAX_WS_BUFFER = 131072;
const MAX_ROOMS = 100;
const MAX_PEERS_PER_IP = 16;

// Minimal RFC 6455 WebSocket wrapper over Node net.Socket (text + binary + ping/pong/close).
class SimpleWebSocket {
  constructor(socket) {
    this.socket = socket;
    this.buf = Buffer.alloc(0);
    this.closed = false;
    this.onText = null;
    this.onBinary = null;
    this.onClose = null;
    // Periodic ping keeps idle staging-lobby sockets alive behind Cloudflare Tunnel / Nginx.
    this.pingTimer = setInterval(() => {
      if (!this.closed) this._sendFrame(0x09, Buffer.alloc(0));
    }, 25000);
    this.pingTimer.unref?.();

    socket.setNoDelay(true);
    socket.on('data', (chunk) => this._onData(chunk));
    socket.on('close', () => this._finish());
    socket.on('error', () => this._finish());
  }

  _finish() {
    if (this.closed) return;
    this.closed = true;
    if (this.pingTimer) {
      clearInterval(this.pingTimer);
      this.pingTimer = null;
    }
    try { this.socket.destroy(); } catch {}
    if (this.onClose) this.onClose();
  }

  _onData(chunk) {
    if (this.buf.length + chunk.length > MAX_WS_BUFFER) {
      this.close();
      return;
    }
    this.buf = this.buf.length ? Buffer.concat([this.buf, chunk]) : chunk;
    while (this.buf.length >= 2) {
      const b0 = this.buf[0];
      const b1 = this.buf[1];
      const opcode = b0 & 0x0f;
      const masked = (b1 & 0x80) !== 0;
      let payloadLen = b1 & 0x7f;
      let offset = 2;
      if (payloadLen === 126) {
        if (this.buf.length < 4) return;
        payloadLen = this.buf.readUInt16BE(2);
        offset = 4;
      } else if (payloadLen === 127) {
        if (this.buf.length < 10) return;
        const hi = this.buf.readUInt32BE(2);
        const lo = this.buf.readUInt32BE(6);
        payloadLen = hi * 0x100000000 + lo;
        offset = 10;
      }
      if (payloadLen > MAX_WS_PAYLOAD) {
        this.close();
        return;
      }
      const maskLen = masked ? 4 : 0;
      if (this.buf.length < offset + maskLen + payloadLen) return;

      const payload = Buffer.allocUnsafe(payloadLen);
      if (masked) {
        const m0 = this.buf[offset];
        const m1 = this.buf[offset + 1];
        const m2 = this.buf[offset + 2];
        const m3 = this.buf[offset + 3];
        const start = offset + 4;
        for (let i = 0; i < payloadLen; ++i) {
          const m = (i & 3) === 0 ? m0 : (i & 3) === 1 ? m1 : (i & 3) === 2 ? m2 : m3;
          payload[i] = this.buf[start + i] ^ m;
        }
      } else {
        this.buf.copy(payload, 0, offset, offset + payloadLen);
      }
      this.buf = this.buf.subarray(offset + maskLen + payloadLen);

      if (opcode === 0x8) {
        this.close();
        return;
      } else if (opcode === 0x9) {
        this._sendFrame(0x0a, payload); // pong
      } else if (opcode === 0x1 && this.onText) {
        this.onText(payload.toString('utf8'));
      } else if (opcode === 0x2 && this.onBinary) {
        this.onBinary(payload);
      }
    }
  }

  _sendFrame(opcode, payloadBuf) {
    if (this.closed) return;
    const len = payloadBuf.length;
    let header;
    if (len < 126) {
      header = Buffer.allocUnsafe(2);
      header[0] = 0x80 | opcode;
      header[1] = len;
    } else if (len < 65536) {
      header = Buffer.allocUnsafe(4);
      header[0] = 0x80 | opcode;
      header[1] = 126;
      header.writeUInt16BE(len, 2);
    } else {
      header = Buffer.allocUnsafe(10);
      header[0] = 0x80 | opcode;
      header[1] = 127;
      header.writeUInt32BE(Math.floor(len / 0x100000000), 2);
      header.writeUInt32BE(len >>> 0, 6);
    }
    try {
      this.socket.write(Buffer.concat([header, payloadBuf]));
    } catch {
      this._finish();
    }
  }

  sendJson(obj) {
    this._sendFrame(0x01, Buffer.from(JSON.stringify(obj), 'utf8'));
  }

  sendBinary(buf) {
    this._sendFrame(0x02, buf);
  }

  close() {
    if (!this.closed) {
      this._sendFrame(0x08, Buffer.alloc(0));
      this._finish();
    }
  }
}

// Room & virtual-LAN IP registry.
const rooms = new Map(); // roomId -> { id, map, hostName, createdAt, nextSlot, peers: Map<peerId, peer> }
const ipConnCounts = new Map(); // clientIp -> active ws count
let nextPeerSeq = 1;
let totalRelayedPackets = 0;

function getClientIp(req, socket) {
  const cf = req.headers['cf-connecting-ip'];
  if (cf) return String(cf).trim();
  const xff = req.headers['x-forwarded-for'];
  if (xff) return String(xff).split(',')[0].trim();
  return socket.remoteAddress || '127.0.0.1';
}

function isLoopbackIp(ip) {
  return !ip || ip === '127.0.0.1' || ip === '::1' || ip === '::ffff:127.0.0.1';
}

function roomSubnetByte(roomId) {
  let h = 1;
  for (let i = 0; i < roomId.length; ++i) {
    h = ((h * 31) + roomId.charCodeAt(i)) & 0xff;
  }
  return (h % 250) + 1;
}

function makeIpNetOrder(a, b, c, d) {
  // Network byte order as stored in sockaddr_in::sin_addr.s_addr on little-endian WASM/x86.
  return ((a & 0xff) | ((b & 0xff) << 8) | ((c & 0xff) << 16) | ((d & 0xff) << 24)) >>> 0;
}

function formatIpNetOrder(ip) {
  return `${ip & 0xff}.${(ip >>> 8) & 0xff}.${(ip >>> 16) & 0xff}.${(ip >>> 24) & 0xff}`;
}

function roomHasHost(room) {
  for (const p of room.peers.values()) {
    if (p.role === 'host') return true;
  }
  return false;
}

function serializeRoom(r) {
  const hostPeer = Array.from(r.peers.values()).find((p) => p.role === 'host') || Array.from(r.peers.values())[0];
  const playersList = Array.from(r.peers.values()).map((p) => ({
    peerId: p.peerId,
    name: p.name,
    role: p.role,
    ip: p.ip,
  }));
  return {
    id: r.id,
    title: r.title || r.id,
    name: r.hostName,
    map: r.map,
    playlist: r.playlist || [r.map],
    players: r.peers.size,
    maxPlayers: r.maxPlayers || 16,
    numBots: r.numBots ?? 16,
    difficulty: r.difficulty ?? 2,
    reinfMult: r.reinfMult ?? 100,
    heroes: r.heroes !== false,
    teamDamage: !!r.teamDamage,
    autoAssign: !!r.autoAssign,
    hasPassword: !!r.password,
    status: r.status || 'lobby',
    hostIp: hostPeer ? hostPeer.ip : '',
    createdAt: r.createdAt,
    playersList,
  };
}

function getOrCreateRoom(roomId, meta = {}) {
  let room = rooms.get(roomId);
  if (!room) {
    const map = meta.map || 'tat2g_con';
    room = {
      id: roomId,
      title: meta.title || roomId,
      map,
      playlist: Array.isArray(meta.playlist) && meta.playlist.length ? meta.playlist : [map],
      hostName: meta.hostName || roomId,
      maxPlayers: Math.max(2, Math.min(64, Number(meta.maxPlayers) || 16)),
      numBots: Math.max(0, Math.min(32, meta.numBots !== undefined ? Number(meta.numBots) : 16)),
      difficulty: Math.max(1, Math.min(3, Number(meta.difficulty) || 2)),
      reinfMult: Math.max(25, Math.min(500, Number(meta.reinfMult) || 100)),
      heroes: meta.heroes !== false,
      teamDamage: !!meta.teamDamage,
      autoAssign: !!meta.autoAssign,
      password: meta.password || '',
      status: 'lobby',
      subnet: roomSubnetByte(roomId),
      createdAt: Date.now(),
      usedSlots: new Set(),
      peers: new Map(),
      byIp: new Map(),
    };
    rooms.set(roomId, room);
  }
  return room;
}

function allocSlot(room, preferHost) {
  if (preferHost && !room.usedSlots.has(1)) {
    room.usedSlots.add(1);
    return 1;
  }
  for (let s = 2; s < 254; ++s) {
    if (!room.usedSlots.has(s)) {
      room.usedSlots.add(s);
      return s;
    }
  }
  return 254;
}

const server = http.createServer((req, res) => {
  res.setHeader('Access-Control-Allow-Origin', '*');
  res.setHeader('Access-Control-Allow-Methods', 'GET, OPTIONS');
  res.setHeader('Cache-Control', 'no-store');
  if (req.method === 'OPTIONS') {
    res.writeHead(204);
    res.end();
    return;
  }

  const u = new URL(req.url || '/', `http://${req.headers.host || 'localhost'}`);
  if (u.pathname === '/api/health') {
    let peers = 0;
    let activeRooms = 0;
    for (const r of rooms.values()) {
      if (roomHasHost(r)) {
        ++activeRooms;
        peers += r.peers.size;
      }
    }
    res.writeHead(200, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify({ ok: true, rooms: activeRooms, peers, relayedPackets: totalRelayedPackets }));
    return;
  }
  if (u.pathname === '/api/rooms') {
    const list = [];
    for (const r of rooms.values()) {
      if (!roomHasHost(r)) continue;
      list.push(serializeRoom(r));
    }
    res.writeHead(200, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify({ rooms: list }));
    return;
  }
  res.writeHead(404, { 'Content-Type': 'application/json' });
  res.end(JSON.stringify({ error: 'not found' }));
});

server.on('upgrade', (req, socket) => {
  const key = req.headers['sec-websocket-key'];
  if (!key) {
    socket.destroy();
    return;
  }
  const accept = crypto.createHash('sha1').update(key + WS_GUID).digest('base64');
  socket.write(
    'HTTP/1.1 101 Switching Protocols\r\n' +
    'Upgrade: websocket\r\n' +
    'Connection: Upgrade\r\n' +
    `Sec-WebSocket-Accept: ${accept}\r\n\r\n`
  );

  const u = new URL(req.url || '/ws', `http://${req.headers.host || 'localhost'}`);
  const roomId = (u.searchParams.get('room') || 'lobby').replace(/[^a-zA-Z0-9_-]/g, '').slice(0, 32) || 'lobby';
  let role = u.searchParams.get('role') === 'host' ? 'host' : 'client';
  const name = (u.searchParams.get('name') || (role === 'host' ? 'Host' : 'Player')).slice(0, 32);
  const map = (u.searchParams.get('map') || 'tat2g_con').slice(0, 32);
  const title = (u.searchParams.get('title') || roomId).slice(0, 48);
  const password = (u.searchParams.get('password') || '').slice(0, 64);
  const playlistRaw = (u.searchParams.get('playlist') || '').slice(0, 512);
  const playlist = playlistRaw
    ? playlistRaw.split(',').map((s) => s.trim()).filter(Boolean).slice(0, 32)
    : [map];
  const maxPlayers = Math.max(2, Math.min(64, Number(u.searchParams.get('maxPlayers')) || 16));
  const numBots = u.searchParams.has('numBots') ? Math.max(0, Math.min(32, Number(u.searchParams.get('numBots')))) : 16;
  const difficulty = Math.max(1, Math.min(3, Number(u.searchParams.get('difficulty')) || 2));
  const reinfMult = Math.max(25, Math.min(500, Number(u.searchParams.get('reinfMult')) || 100));
  const heroes = u.searchParams.get('heroes') !== '0';
  const teamDamage = u.searchParams.get('teamDamage') === '1';
  const autoAssign = u.searchParams.get('autoAssign') === '1';

  const ws = new SimpleWebSocket(socket);
  const clientIp = getClientIp(req, socket);
  if (!isLoopbackIp(clientIp)) {
    const curConns = ipConnCounts.get(clientIp) || 0;
    if (curConns >= MAX_PEERS_PER_IP) {
      ws.sendJson({ type: 'join-error', code: 'RATE_LIMIT', message: 'Too many simultaneous connections from your IP.' });
      ws.close();
      return;
    }
    ipConnCounts.set(clientIp, curConns + 1);
  }
  let ipReleased = false;
  const releaseIp = () => {
    if (ipReleased || isLoopbackIp(clientIp)) return;
    ipReleased = true;
    const n = (ipConnCounts.get(clientIp) || 1) - 1;
    if (n <= 0) ipConnCounts.delete(clientIp);
    else ipConnCounts.set(clientIp, n);
  };

  let room = rooms.get(roomId);

  // If two players click an open public lobby simultaneously as host, seamlessly join the second
  // player as a client into the first host's room rather than overwriting the host.
  if (role === 'host' && room && roomHasHost(room)) {
    role = 'client';
  }

  if (role === 'client') {
    if (!room || !roomHasHost(room)) {
      ws.sendJson({ type: 'join-error', code: 'NO_HOST', message: 'Lobby host is no longer active.' });
      releaseIp();
      ws.close();
      return;
    }
    if (room.peers.size >= (room.maxPlayers || 16)) {
      ws.sendJson({ type: 'join-error', code: 'FULL', message: 'This lobby is full.' });
      releaseIp();
      ws.close();
      return;
    }
    if (room.password && password !== room.password) {
      ws.sendJson({ type: 'join-error', code: 'BAD_PASSWORD', message: 'Incorrect lobby password.' });
      releaseIp();
      ws.close();
      return;
    }
  } else {
    if (!room && rooms.size >= MAX_ROOMS) {
      ws.sendJson({ type: 'join-error', code: 'SERVER_FULL', message: 'Server lobby limit reached. Please join an existing lobby.' });
      releaseIp();
      ws.close();
      return;
    }
    room = getOrCreateRoom(roomId, {
      title,
      map,
      playlist,
      hostName: name,
      maxPlayers,
      numBots,
      difficulty,
      reinfMult,
      heroes,
      teamDamage,
      autoAssign,
      password,
    });
    room.title = title;
    room.hostName = name;
    room.map = map;
    room.playlist = playlist;
    room.maxPlayers = maxPlayers;
    room.numBots = numBots;
    room.difficulty = difficulty;
    room.reinfMult = reinfMult;
    room.heroes = heroes;
    room.teamDamage = teamDamage;
    room.autoAssign = autoAssign;
    room.password = password;
    room.status = 'lobby';
  }

  const slot = allocSlot(room, role === 'host');
  const ipInt = makeIpNetOrder(10, 200, room.subnet, slot);
  const ip = formatIpNetOrder(ipInt);
  const peerId = `p${nextPeerSeq++}`;

  const peer = { peerId, slot, ip, ipInt, role, name, ws, room };
  room.peers.set(peerId, peer);
  room.byIp.set(ipInt, peer);

  const existingPeers = [];
  for (const other of room.peers.values()) {
    if (other.peerId === peerId) continue;
    existingPeers.push({ peerId: other.peerId, ip: other.ip, ipInt: other.ipInt, role: other.role, name: other.name });
    other.ws.sendJson({ type: 'peer-joined', peerId, ip, ipInt, role, name });
  }

  ws.sendJson({
    type: 'welcome',
    peerId,
    ip,
    ipInt,
    room: roomId,
    role,
    roomMeta: serializeRoom(room),
    peers: existingPeers,
  });

  ws.onText = (text) => {
    let msg;
    try { msg = JSON.parse(text); } catch { return; }
    if (!msg || typeof msg !== 'object') return;
    if (msg.type === 'rtc-offer' || msg.type === 'rtc-answer' || msg.type === 'rtc-ice') {
      const target = room.peers.get(msg.to);
      if (target) {
        target.ws.sendJson({ type: msg.type, from: peerId, fromIpInt: ipInt, payload: msg.payload });
      }
      return;
    }
    if (msg.type === 'room-meta' && role === 'host') {
      if (msg.map) room.map = String(msg.map).slice(0, 32);
      if (msg.title) room.title = String(msg.title).slice(0, 48);
      if (msg.name) room.hostName = String(msg.name).slice(0, 32);
      if (msg.status === 'lobby' || msg.status === 'playing') room.status = msg.status;
      if (Array.isArray(msg.playlist) && msg.playlist.length) room.playlist = msg.playlist.slice(0, 32);
      const serialized = serializeRoom(room);
      for (const p of room.peers.values()) {
        p.ws.sendJson({ type: 'room-updated', roomMeta: serialized });
      }
      return;
    }
    if (msg.type === 'lobby-chat') {
      const now = Date.now();
      if (!peer.chatWindow || now - peer.chatWindow > 4000) {
        peer.chatWindow = now;
        peer.chatCount = 0;
      }
      if (++peer.chatCount > 6) return;
      const clean = String(msg.text || '').trim().slice(0, 240);
      if (!clean) return;
      const chatPayload = { type: 'lobby-chat', from: peerId, name, role, text: clean, ts: now };
      for (const p of room.peers.values()) {
        p.ws.sendJson(chatPayload);
      }
    }
  };

  // Binary UDP frame: [toIp(4 LE uint32), toPort(2 LE uint16), fromIp(4 LE uint32), fromPort(2 LE uint16), payload...]
  ws.onBinary = (frame) => {
    if (frame.length < 13) return;
    const toIp = frame.readUInt32LE(0);
    // Stamp authoritative sender IP so peers cannot spoof addresses.
    frame.writeUInt32LE(ipInt, 6);
    ++totalRelayedPackets;

    if (toIp === 0xffffffff) {
      for (const other of room.peers.values()) {
        if (other.peerId !== peerId) {
          other.ws.sendBinary(frame);
        }
      }
      return;
    }
    const target = room.byIp.get(toIp);
    if (target) {
      target.ws.sendBinary(frame);
    }
  };

  ws.onClose = () => {
    releaseIp();
    room.peers.delete(peerId);
    room.byIp.delete(ipInt);
    room.usedSlots.delete(slot);
    for (const other of room.peers.values()) {
      other.ws.sendJson({ type: 'peer-left', peerId, ip, ipInt });
    }
    if (role === 'host' && !roomHasHost(room)) {
      for (const other of room.peers.values()) {
        other.ws.sendJson({ type: 'host-left', room: roomId });
      }
      rooms.delete(roomId);
      return;
    }
    if (room.peers.size === 0) {
      rooms.delete(roomId);
    }
  };
});

server.listen(PORT, HOST, () => {
  console.log(`swbf2-multiplayer-server listening on http://${HOST}:${PORT} (ws://${HOST}:${PORT}/ws)`);
});
