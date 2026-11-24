// Phase 6 Lock-Free SharedArrayBuffer UDP Ring + WebRTC DataChannel / WebSocket Client.
//
// Main Thread owns BrowserNetClient (RTCPeerConnection + RTCDataChannel({ ordered: false, maxRetransmits: 0 })
// + WebSocket signaling/broadcast/relay).
// Worker owns NetSabBridge, called synchronously from browser/platform/winsock_browser.cpp on every
// sendto()/recvfrom() so even SWBF2's synchronous CNetLaunchGame/CNetPostLoadJoinGame spin loops
// exchange live WebRTC/WebSocket UDP packets without blocking or deadlocking.

export const NET_RING_HEADER_BYTES = 64;
export const NET_RING_SLOTS = 128; // must be power of 2
export const NET_RING_SLOT_BYTES = 1040; // 16-byte header + 1024-byte max datagram
export const NET_RING_SAB_BYTES = NET_RING_HEADER_BYTES + NET_RING_SLOTS * NET_RING_SLOT_BYTES;

const IDX_WRITE = 0;
const IDX_READ = 1;
const IDX_WAKE = 2;
const IDX_DROPPED = 3;

export function createNetRingPair() {
  return {
    txSab: new SharedArrayBuffer(NET_RING_SAB_BYTES),
    rxSab: new SharedArrayBuffer(NET_RING_SAB_BYTES),
  };
}

export class NetRingQueue {
  constructor(sab) {
    this.sab = sab;
    this.ctrl = new Int32Array(sab, 0, NET_RING_HEADER_BYTES / 4);
    this.u8 = new Uint8Array(sab);
    this.dv = new DataView(sab);
    this.mask = NET_RING_SLOTS - 1;
  }

  push(toIp, toPort, fromIp, fromPort, payloadBytes) {
    const len = payloadBytes.byteLength;
    if (len <= 0 || len > 1024) return false;
    const w = Atomics.load(this.ctrl, IDX_WRITE);
    const r = Atomics.load(this.ctrl, IDX_READ);
    if (((w - r) | 0) >= NET_RING_SLOTS) {
      Atomics.add(this.ctrl, IDX_DROPPED, 1);
      return false;
    }
    const base = NET_RING_HEADER_BYTES + (w & this.mask) * NET_RING_SLOT_BYTES;
    this.dv.setUint32(base + 0, toIp >>> 0, true);
    this.dv.setUint16(base + 4, toPort & 0xffff, true);
    this.dv.setUint16(base + 6, fromPort & 0xffff, true);
    this.dv.setUint32(base + 8, fromIp >>> 0, true);
    this.dv.setInt32(base + 12, len | 0, true);
    this.u8.set(payloadBytes, base + 16);

    Atomics.store(this.ctrl, IDX_WRITE, (w + 1) | 0);
    Atomics.add(this.ctrl, IDX_WAKE, 1);
    Atomics.notify(this.ctrl, IDX_WAKE, 1);
    return true;
  }

  popInto(handler) {
    const w = Atomics.load(this.ctrl, IDX_WRITE);
    const r = Atomics.load(this.ctrl, IDX_READ);
    if (w === r) return false;
    const base = NET_RING_HEADER_BYTES + (r & this.mask) * NET_RING_SLOT_BYTES;
    const toIp = this.dv.getUint32(base + 0, true);
    const toPort = this.dv.getUint16(base + 4, true);
    const fromPort = this.dv.getUint16(base + 6, true);
    const fromIp = this.dv.getUint32(base + 8, true);
    const len = this.dv.getInt32(base + 12, true);
    const slice = this.u8.subarray(base + 16, base + 16 + len);
    handler(toIp, toPort, fromIp, fromPort, slice, len);
    Atomics.store(this.ctrl, IDX_READ, (r + 1) | 0);
    return true;
  }

  waitForData(timeoutMs = 2) {
    const w = Atomics.load(this.ctrl, IDX_WRITE);
    const r = Atomics.load(this.ctrl, IDX_READ);
    if (w !== r) return true;
    const seq = Atomics.load(this.ctrl, IDX_WAKE);
    if (Atomics.load(this.ctrl, IDX_WRITE) !== Atomics.load(this.ctrl, IDX_READ)) return true;
    try {
      Atomics.wait(this.ctrl, IDX_WAKE, seq, timeoutMs);
    } catch {
      // Main thread cannot Atomics.wait; Worker can.
    }
    return Atomics.load(this.ctrl, IDX_WRITE) !== Atomics.load(this.ctrl, IDX_READ);
  }
}

// Worker-side bridge installed as Module.netBridge so winsock_browser.cpp pumps SAB rings synchronously.
export class WorkerNetBridge {
  constructor({ txSab, rxSab, module, localIpInt }) {
    this.tx = new NetRingQueue(txSab);
    this.rx = new NetRingQueue(rxSab);
    this.module = module;
    this.localIpInt = localIpInt >>> 0;
    this.rxScratch = module._malloc(1024);
    module._swbf2_net_set_local_ip(this.localIpInt);
    module.netBridge = this;
  }

  send(toIp, toPort, fromPort, bufPtr, len) {
    if (len <= 0 || len > 1024) return;
    const bytes = this.module.HEAPU8.subarray(bufPtr, bufPtr + len);
    this.tx.push(toIp >>> 0, toPort & 0xffff, this.localIpInt, fromPort & 0xffff, bytes);
  }

  pollRx(allowWait) {
    let count = 0;
    const drain = () => {
      while (count < 64 && this.rx.popInto((_toIp, toPort, fromIp, fromPort, slice, len) => {
        this.module.HEAPU8.set(slice, this.rxScratch);
        this.module._swbf2_net_push_in(fromIp >>> 0, fromPort & 0xffff, toPort & 0xffff, this.rxScratch, len | 0);
        ++count;
      })) {}
    };
    drain();
    if (count === 0 && allowWait) {
      if (this.rx.waitForData(1)) {
        drain();
      }
    }
    return count;
  }

  dispose() {
    if (this.module && this.module.netBridge === this) {
      this.module.netBridge = null;
    }
    if (this.rxScratch && this.module) {
      this.module._free(this.rxScratch);
      this.rxScratch = 0;
    }
  }
}

// Pack/unpack the 12-byte UDP framing used on both WebRTC DataChannels and WebSocket relay.
function packUdpFrame(toIp, toPort, fromIp, fromPort, payload) {
  const out = new Uint8Array(12 + payload.byteLength);
  const dv = new DataView(out.buffer);
  dv.setUint32(0, toIp >>> 0, true);
  dv.setUint16(4, toPort & 0xffff, true);
  dv.setUint32(6, fromIp >>> 0, true);
  dv.setUint16(10, fromPort & 0xffff, true);
  out.set(payload, 12);
  return out;
}

// Main-thread WebRTC DataChannel + WebSocket UDP client.
export class BrowserNetClient {
  constructor({
    serverUrl,
    room = 'lobby',
    role = 'client',
    name = 'Player',
    map = 'tat2g_con',
    title = '',
    password = '',
    playlist = null,
    meta = null,
    enableWebRtc = true,
    iceServers = [],
    onPeersChanged = null,
    onChat = null,
    onRoomUpdated = null,
    onHostLeft = null,
  } = {}) {
    this.serverUrl = serverUrl;
    this.room = room;
    this.role = role;
    this.name = name;
    this.map = map;
    this.title = title || room;
    this.password = password || '';
    this.playlist = Array.isArray(playlist) && playlist.length ? playlist : [map];
    this.meta = meta || {};
    this.roomMeta = null;
    this.enableWebRtc = enableWebRtc && typeof globalThis.RTCPeerConnection === 'function';
    this.iceServers = iceServers;
    this.onPeersChanged = onPeersChanged;
    this.onChat = onChat;
    this.onRoomUpdated = onRoomUpdated;
    this.onHostLeft = onHostLeft;

    const pair = createNetRingPair();
    this.txSab = pair.txSab;
    this.rxSab = pair.rxSab;
    this.txRing = new NetRingQueue(this.txSab);
    this.rxRing = new NetRingQueue(this.rxSab);

    this.ws = null;
    this.peerId = '';
    this.localIp = '';
    this.localIpInt = 0;
    this.connected = false;
    this.peers = new Map();       // peerId -> { peerId, ip, ipInt, role, name, pc, dc, audioEl, voiceSenders }
    this.peerByIp = new Map();    // ipInt -> peer
    this.pumpTimer = 0;
    this.localVoiceStream = null;
    this.voiceEnabled = false;
    this.voiceMuted = false;

    this.stats = {
      txWebrtc: 0,
      txWs: 0,
      rxWebrtc: 0,
      rxWs: 0,
      rtcChannelsOpened: 0,
    };
  }

  async connect() {
    const qs = new URLSearchParams({
      room: this.room,
      role: this.role,
      name: this.name,
      map: this.map,
      title: this.title,
      playlist: this.playlist.join(','),
    });
    if (this.password) qs.set('password', this.password);
    if (this.meta) {
      if (this.meta.maxPlayers !== undefined) qs.set('maxPlayers', String(this.meta.maxPlayers));
      if (this.meta.numBots !== undefined) qs.set('numBots', String(this.meta.numBots));
      if (this.meta.difficulty !== undefined) qs.set('difficulty', String(this.meta.difficulty));
      if (this.meta.reinfMult !== undefined) qs.set('reinfMult', String(this.meta.reinfMult));
      if (this.meta.heroes !== undefined) qs.set('heroes', this.meta.heroes ? '1' : '0');
      if (this.meta.teamDamage !== undefined) qs.set('teamDamage', this.meta.teamDamage ? '1' : '0');
      if (this.meta.autoAssign !== undefined) qs.set('autoAssign', this.meta.autoAssign ? '1' : '0');
    }
    const url = `${this.serverUrl.replace(/\/$/, '')}/ws?${qs.toString()}`;
    return new Promise((resolve, reject) => {
      const ws = new WebSocket(url);
      ws.binaryType = 'arraybuffer';
      this.ws = ws;
      let settled = false;

      const timer = setTimeout(() => {
        if (!settled) {
          settled = true;
          reject(new Error(`Timed out connecting to ${url}`));
        }
      }, 10000);

      ws.onerror = () => {
        if (!settled) {
          settled = true;
          clearTimeout(timer);
          reject(new Error(`WebSocket connection failed: ${url}`));
        }
      };

      ws.onclose = () => {
        this.connected = false;
        if (this.pumpTimer) {
          clearInterval(this.pumpTimer);
          this.pumpTimer = 0;
        }
        if (!settled) {
          settled = true;
          clearTimeout(timer);
          reject(new Error('Lobby connection closed before welcome.'));
        }
      };

      ws.onmessage = (ev) => {
        if (typeof ev.data === 'string') {
          let msg;
          try { msg = JSON.parse(ev.data); } catch { return; }
          if (msg.type === 'join-error') {
            if (!settled) {
              settled = true;
              clearTimeout(timer);
              const err = new Error(msg.message || msg.code || 'Unable to join lobby');
              err.code = msg.code || 'JOIN_ERROR';
              reject(err);
            }
            return;
          }
          if (msg.type === 'welcome') {
            this.peerId = msg.peerId;
            this.localIp = msg.ip;
            this.localIpInt = msg.ipInt >>> 0;
            this.role = msg.role || this.role;
            this.roomMeta = msg.roomMeta || null;
            this.connected = true;
            for (const p of (msg.peers || [])) {
              this._addPeer(p, true); // newcomer initiates WebRTC offer to existing peers
            }
            if (!this.pumpTimer) {
              this.pumpTimer = setInterval(() => this._pumpTx(), 8);
            }
            this.onPeersChanged?.(this.getStats());
            if (!settled) {
              settled = true;
              clearTimeout(timer);
              resolve({
                peerId: this.peerId,
                ip: this.localIp,
                ipInt: this.localIpInt,
                room: this.room,
                role: this.role,
                roomMeta: this.roomMeta,
              });
            }
            return;
          }
          if (msg.type === 'peer-joined') {
            this._addPeer(msg, false);
            this.onPeersChanged?.(this.getStats());
            return;
          }
          if (msg.type === 'peer-left') {
            this._removePeer(msg.peerId);
            this.onPeersChanged?.(this.getStats());
            return;
          }
          if (msg.type === 'room-updated') {
            this.roomMeta = msg.roomMeta || this.roomMeta;
            this.onRoomUpdated?.(this.roomMeta);
            return;
          }
          if (msg.type === 'lobby-chat') {
            this.onChat?.(msg);
            return;
          }
          if (msg.type === 'host-left') {
            this.onHostLeft?.(msg);
            return;
          }
          if (msg.type === 'rtc-offer' || msg.type === 'rtc-answer' || msg.type === 'rtc-ice') {
            this._handleSignal(msg);
          }
          return;
        }

        // Binary UDP frame from WebSocket relay
        if (ev.data instanceof ArrayBuffer) {
          this._onIncomingUdp(new Uint8Array(ev.data), 'ws');
        }
      };
    });
  }

  sendChat(text) {
    if (this.ws && this.ws.readyState === WebSocket.OPEN && text && text.trim()) {
      this.ws.send(JSON.stringify({ type: 'lobby-chat', text: text.trim().slice(0, 240) }));
    }
  }

  updateRoomMeta(meta = {}) {
    if (this.ws && this.ws.readyState === WebSocket.OPEN) {
      this.ws.send(JSON.stringify({ type: 'room-meta', ...meta }));
    }
  }

  async enableVoice({ startMuted = false } = {}) {
    if (this.localVoiceStream) {
      this.voiceEnabled = true;
      this.setVoiceMuted(startMuted);
      return true;
    }
    if (!navigator.mediaDevices || typeof navigator.mediaDevices.getUserMedia !== 'function') {
      throw new Error('Microphone / WebRTC Voice Chat requires HTTPS and browser mediaDevices support.');
    }
    const stream = await navigator.mediaDevices.getUserMedia({
      audio: { echoCancellation: true, noiseSuppression: true, autoGainControl: true },
      video: false,
    });
    this.localVoiceStream = stream;
    this.voiceEnabled = true;
    this.setVoiceMuted(startMuted);
    for (const peer of this.peers.values()) {
      await this._attachLocalVoiceToPeer(peer, true);
    }
    return true;
  }

  setVoiceMuted(muted) {
    this.voiceMuted = !!muted;
    if (this.localVoiceStream) {
      for (const track of this.localVoiceStream.getAudioTracks()) {
        track.enabled = !this.voiceMuted;
      }
    }
  }

  disableVoice() {
    this.voiceEnabled = false;
    this.voiceMuted = false;
    if (this.localVoiceStream) {
      for (const track of this.localVoiceStream.getTracks()) {
        try { track.stop(); } catch {}
      }
      this.localVoiceStream = null;
    }
    for (const peer of this.peers.values()) {
      if (peer.pc && peer.voiceSenders?.length) {
        for (const s of peer.voiceSenders) {
          try { peer.pc.removeTrack(s); } catch {}
        }
        peer.voiceSenders = [];
        this._renegotiatePeer(peer).catch(() => {});
      }
    }
  }

  async _attachLocalVoiceToPeer(peer, renegotiate = false) {
    if (!this.localVoiceStream || !peer.pc) return;
    if (!peer.voiceSenders) peer.voiceSenders = [];
    if (peer.voiceSenders.length > 0) return;
    for (const track of this.localVoiceStream.getAudioTracks()) {
      try {
        peer.voiceSenders.push(peer.pc.addTrack(track, this.localVoiceStream));
      } catch {}
    }
    if (renegotiate) {
      await this._renegotiatePeer(peer);
    }
  }

  async _renegotiatePeer(peer) {
    if (!peer.pc || !this.ws || this.ws.readyState !== WebSocket.OPEN) return;
    if (peer.pc.signalingState !== 'stable') return;
    try {
      const offer = await peer.pc.createOffer();
      if (peer.pc.signalingState !== 'stable') return;
      await peer.pc.setLocalDescription(offer);
      this.ws.send(JSON.stringify({ type: 'rtc-offer', to: peer.peerId, payload: offer }));
    } catch {}
  }

  _addPeer(info, initiateOffer) {
    if (this.peers.has(info.peerId)) return this.peers.get(info.peerId);
    const peer = {
      peerId: info.peerId,
      ip: info.ip,
      ipInt: info.ipInt >>> 0,
      role: info.role,
      name: info.name,
      pc: null,
      dc: null,
      audioEl: null,
      voiceSenders: [],
      pendingIce: [],
    };
    this.peers.set(peer.peerId, peer);
    this.peerByIp.set(peer.ipInt, peer);

    if (this.enableWebRtc) {
      this._setupRtc(peer, initiateOffer);
    }
    return peer;
  }

  _removePeer(peerId) {
    const peer = this.peers.get(peerId);
    if (!peer) return;
    if (peer.audioEl) {
      try { peer.audioEl.pause(); peer.audioEl.srcObject = null; peer.audioEl.remove(); } catch {}
      peer.audioEl = null;
    }
    try { peer.dc?.close(); } catch {}
    try { peer.pc?.close(); } catch {}
    this.peers.delete(peerId);
    this.peerByIp.delete(peer.ipInt);
  }

  _bindDataChannel(peer, dc) {
    dc.binaryType = 'arraybuffer';
    peer.dc = dc;
    dc.onopen = () => {
      this.stats.rtcChannelsOpened++;
    };
    dc.onmessage = (ev) => {
      if (ev.data instanceof ArrayBuffer) {
        this._onIncomingUdp(new Uint8Array(ev.data), 'webrtc');
      }
    };
  }

  async _setupRtc(peer, initiateOffer) {
    try {
      const pc = new RTCPeerConnection({ iceServers: this.iceServers });
      peer.pc = pc;
      pc.onicecandidate = (ev) => {
        if (ev.candidate && this.ws && this.ws.readyState === WebSocket.OPEN) {
          this.ws.send(JSON.stringify({
            type: 'rtc-ice',
            to: peer.peerId,
            payload: ev.candidate,
          }));
        }
      };
      pc.ondatachannel = (ev) => {
        this._bindDataChannel(peer, ev.channel);
      };
      pc.ontrack = (ev) => {
        const stream = ev.streams?.[0] || new MediaStream([ev.track]);
        if (typeof document !== 'undefined') {
          if (!peer.audioEl) {
            const el = document.createElement('audio');
            el.autoplay = true;
            el.playsInline = true;
            el.style.display = 'none';
            document.body.appendChild(el);
            peer.audioEl = el;
          }
          peer.audioEl.srcObject = stream;
          peer.audioEl.play().catch(() => {});
        }
      };

      if (this.localVoiceStream) {
        await this._attachLocalVoiceToPeer(peer, false);
      }

      if (initiateOffer) {
        // Genuine unreliable, unordered UDP semantics over WebRTC DataChannel!
        const dc = pc.createDataChannel('swbf2-udp', { ordered: false, maxRetransmits: 0 });
        this._bindDataChannel(peer, dc);
        const offer = await pc.createOffer();
        await pc.setLocalDescription(offer);
        if (this.ws && this.ws.readyState === WebSocket.OPEN) {
          this.ws.send(JSON.stringify({ type: 'rtc-offer', to: peer.peerId, payload: offer }));
        }
      }
    } catch {
      // WebSocket relay remains active automatically.
    }
  }

  async _handleSignal(msg) {
    const peer = this.peers.get(msg.from);
    if (!peer || !this.enableWebRtc) return;
    if (!peer.pc) {
      await this._setupRtc(peer, false);
    }
    const pc = peer.pc;
    if (!pc) return;
    try {
      if (msg.type === 'rtc-offer') {
        if (pc.signalingState !== 'stable') {
          // Polite-peer rollback avoids glare stalls during mid-match voice renegotiation.
          const polite = this.peerId > peer.peerId;
          if (!polite) return;
          try { await pc.setLocalDescription({ type: 'rollback' }); } catch {}
        }
        await pc.setRemoteDescription(msg.payload);
        for (const cand of peer.pendingIce) {
          try { await pc.addIceCandidate(cand); } catch {}
        }
        peer.pendingIce.length = 0;
        const answer = await pc.createAnswer();
        await pc.setLocalDescription(answer);
        if (this.ws && this.ws.readyState === WebSocket.OPEN) {
          this.ws.send(JSON.stringify({ type: 'rtc-answer', to: peer.peerId, payload: answer }));
        }
      } else if (msg.type === 'rtc-answer') {
        if (pc.signalingState !== 'have-local-offer') return;
        await pc.setRemoteDescription(msg.payload);
        for (const cand of peer.pendingIce) {
          try { await pc.addIceCandidate(cand); } catch {}
        }
        peer.pendingIce.length = 0;
      } else if (msg.type === 'rtc-ice' && msg.payload) {
        if (pc.remoteDescription) {
          await pc.addIceCandidate(msg.payload);
        } else {
          peer.pendingIce.push(msg.payload);
        }
      }
    } catch {
      // Ignore transient ICE errors; WebSocket relay stays live.
    }
  }

  _onIncomingUdp(frame, source) {
    if (frame.byteLength < 13) return;
    const dv = new DataView(frame.buffer, frame.byteOffset, frame.byteLength);
    const toIp = dv.getUint32(0, true);
    const toPort = dv.getUint16(4, true);
    const fromIp = dv.getUint32(6, true);
    const fromPort = dv.getUint16(10, true);
    const payload = frame.subarray(12);
    if (source === 'webrtc') this.stats.rxWebrtc++;
    else this.stats.rxWs++;
    this.rxRing.push(toIp, toPort, fromIp, fromPort, payload);
  }

  _pumpTx() {
    let drained = 0;
    while (drained < 64 && this.txRing.popInto((toIp, toPort, fromIp, fromPort, payload) => {
      ++drained;
      const frame = packUdpFrame(toIp, toPort, fromIp || this.localIpInt, fromPort, payload);
      const target = (toIp !== 0xffffffff) ? this.peerByIp.get(toIp >>> 0) : null;
      if (target && target.dc && target.dc.readyState === 'open') {
        if ((target.dc.bufferedAmount || 0) > 65536) return; // drop stale unreliable UDP on congestion
        try {
          target.dc.send(frame);
          this.stats.txWebrtc++;
          return;
        } catch {}
      }
      if (this.ws && this.ws.readyState === WebSocket.OPEN) {
        if ((this.ws.bufferedAmount || 0) > 65536) return; // prevent TCP bufferbloat lag spikes
        this.ws.send(frame);
        this.stats.txWs++;
      }
    })) {}
  }

  getStats() {
    let webrtcOpenChannels = 0;
    const peerList = [];
    for (const p of this.peers.values()) {
      const dcState = p.dc ? p.dc.readyState : 'none';
      if (dcState === 'open') ++webrtcOpenChannels;
      peerList.push({ peerId: p.peerId, ip: p.ip, role: p.role, name: p.name, dcState });
    }
    return {
      connected: this.connected,
      peerId: this.peerId,
      localIp: this.localIp,
      localIpInt: this.localIpInt,
      room: this.room,
      peerCount: this.peers.size,
      webrtcOpenChannels,
      peers: peerList,
      ...this.stats,
    };
  }

  close() {
    this.disableVoice();
    if (this.pumpTimer) {
      clearInterval(this.pumpTimer);
      this.pumpTimer = 0;
    }
    for (const peerId of Array.from(this.peers.keys())) {
      this._removePeer(peerId);
    }
    try { this.ws?.close(); } catch {}
    this.ws = null;
    this.connected = false;
  }
}
