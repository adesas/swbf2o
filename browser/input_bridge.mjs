// Phase 5 Lock-Free SharedArrayBuffer Input Bridge (Main Thread DOM -> Worker DirectInput8).
//
// Translates browser DOM KeyboardEvent.code, Mouse/PointerLock events, and Gamepad API
// state into a 1 KiB SharedArrayBuffer read once per sim tick by the engine WebWorker and
// fed directly into browser/platform/dinput_browser.cpp (IDirectInputDevice8A::GetDeviceState).

export const INPUT_SAB_BYTES = 1024;
export const INPUT_KEYS_OFFSET = 256;

// Int32 indices in the 0..255 header region
export const IN_DX = 0;
export const IN_DY = 1;
export const IN_DZ = 2;
export const IN_CURSOR_X = 3;
export const IN_CURSOR_Y = 4;
export const IN_USE_ABS = 5;
export const IN_MOUSE_BTNS = 6;
export const IN_GP_CONNECTED = 7;
export const IN_GP_LX = 8;
export const IN_GP_LY = 9;
export const IN_GP_RX = 10;
export const IN_GP_RY = 11;
export const IN_GP_LT = 12;
export const IN_GP_RT = 13;
export const IN_GP_BTNS = 14;
export const IN_GP_POV = 15;
export const IN_PTR_LOCK_WANTED = 16;
export const IN_EVENT_SEQ = 17;

// W3C KeyboardEvent.code -> Microsoft DirectInput DIK_* scancode (matches spike/shim/dinput.h).
export const CODE_TO_DIK = Object.freeze({
  Escape: 0x01,
  Digit1: 0x02,
  Digit2: 0x03,
  Digit3: 0x04,
  Digit4: 0x05,
  Digit5: 0x06,
  Digit6: 0x07,
  Digit7: 0x08,
  Digit8: 0x09,
  Digit9: 0x0a,
  Digit0: 0x0b,
  Minus: 0x0c,
  Equal: 0x0d,
  Backspace: 0x0e,
  Tab: 0x0f,
  KeyQ: 0x10,
  KeyW: 0x11,
  KeyE: 0x12,
  KeyR: 0x13,
  KeyT: 0x14,
  KeyY: 0x15,
  KeyU: 0x16,
  KeyI: 0x17,
  KeyO: 0x18,
  KeyP: 0x19,
  BracketLeft: 0x1a,
  BracketRight: 0x1b,
  Enter: 0x1c,
  ControlLeft: 0x1d,
  KeyA: 0x1e,
  KeyS: 0x1f,
  KeyD: 0x20,
  KeyF: 0x21,
  KeyG: 0x22,
  KeyH: 0x23,
  KeyJ: 0x24,
  KeyK: 0x25,
  KeyL: 0x26,
  Semicolon: 0x27,
  Quote: 0x28,
  Backquote: 0x29,
  ShiftLeft: 0x2a,
  Backslash: 0x2b,
  KeyZ: 0x2c,
  KeyX: 0x2d,
  KeyC: 0x2e,
  KeyV: 0x2f,
  KeyB: 0x30,
  KeyN: 0x31,
  KeyM: 0x32,
  Comma: 0x33,
  Period: 0x34,
  Slash: 0x35,
  ShiftRight: 0x36,
  NumpadMultiply: 0x37,
  AltLeft: 0x38,
  Space: 0x39,
  CapsLock: 0x3a,
  F1: 0x3b,
  F2: 0x3c,
  F3: 0x3d,
  F4: 0x3e,
  F5: 0x3f,
  F6: 0x40,
  F7: 0x41,
  F8: 0x42,
  F9: 0x43,
  F10: 0x44,
  NumLock: 0x45,
  ScrollLock: 0x46,
  Numpad7: 0x47,
  Numpad8: 0x48,
  Numpad9: 0x49,
  NumpadSubtract: 0x4a,
  Numpad4: 0x4b,
  Numpad5: 0x4c,
  Numpad6: 0x4d,
  NumpadAdd: 0x4e,
  Numpad1: 0x4f,
  Numpad2: 0x50,
  Numpad3: 0x51,
  Numpad0: 0x52,
  NumpadDecimal: 0x53,
  F11: 0x57,
  F12: 0x58,
  NumpadEnter: 0x9c,
  ControlRight: 0x9d,
  NumpadDivide: 0xb5,
  PrintScreen: 0xb7,
  AltRight: 0xb8,
  Pause: 0xc5,
  Home: 0xc7,
  ArrowUp: 0xc8,
  PageUp: 0xc9,
  ArrowLeft: 0xcb,
  ArrowRight: 0xcd,
  End: 0xcf,
  ArrowDown: 0xd0,
  PageDown: 0xd1,
  Insert: 0xd2,
  Delete: 0xd3,
});

export function createInputBuffer() {
  const sab = new SharedArrayBuffer(INPUT_SAB_BYTES);
  const i32 = new Int32Array(sab, 0, INPUT_KEYS_OFFSET / 4);
  Atomics.store(i32, IN_GP_POV, -1);
  Atomics.store(i32, IN_USE_ABS, 1);
  return sab;
}

function clampAxis(v) {
  if (!Number.isFinite(v)) return 0;
  const dead = Math.abs(v) < 0.08 ? 0 : v;
  return Math.max(-1000, Math.min(1000, Math.round(dead * 1000)));
}

export class InputBridgeWriter {
  constructor({ sab = null, canvas = null, targetWindow = globalThis.window, gameWidth = 800, gameHeight = 600 } = {}) {
    this.sab = sab || createInputBuffer();
    this.i32 = new Int32Array(this.sab, 0, INPUT_KEYS_OFFSET / 4);
    this.keys = new Uint8Array(this.sab, INPUT_KEYS_OFFSET, 256);
    this.canvas = canvas;
    this.win = targetWindow;
    this.gameWidth = gameWidth;
    this.gameHeight = gameHeight;
    this.attached = false;
    this.pointerLocked = false;
    this.mouseButtons = 0;
    this._raf = 0;
    this._syntheticGamepad = false;
    this._suppressNextUnlockPause = false;
    this._lastEscapeKeyMs = 0;
    this._lastSyntheticEscMs = 0;

    this._onKeyDown = this._onKeyDown.bind(this);
    this._onKeyUp = this._onKeyUp.bind(this);
    this._onMouseMove = this._onMouseMove.bind(this);
    this._onMouseDown = this._onMouseDown.bind(this);
    this._onMouseUp = this._onMouseUp.bind(this);
    this._onWheel = this._onWheel.bind(this);
    this._onContextMenu = (e) => e.preventDefault();
    this._onBlur = () => this.clearAll();
    this._onPointerLockChange = this._onPointerLockChange.bind(this);
  }

  setGameSize(w, h) {
    if (w > 0) this.gameWidth = w;
    if (h > 0) this.gameHeight = h;
  }

  attach() {
    if (this.attached) return this;
    this.attached = true;
    const w = this.win;
    const c = this.canvas;
    if (w) {
      w.addEventListener('keydown', this._onKeyDown, { passive: false });
      w.addEventListener('keyup', this._onKeyUp, { passive: false });
      w.addEventListener('mouseup', this._onMouseUp);
      w.addEventListener('blur', this._onBlur);
    }
    if (c) {
      c.addEventListener('mousemove', this._onMouseMove);
      c.addEventListener('mousedown', this._onMouseDown);
      c.addEventListener('wheel', this._onWheel, { passive: false });
      c.addEventListener('contextmenu', this._onContextMenu);
    }
    if (globalThis.document) {
      document.addEventListener('pointerlockchange', this._onPointerLockChange);
    }
    if (w && typeof w.requestAnimationFrame === 'function') {
      const poll = () => {
        if (!this.attached) return;
        this.pollGamepad();
        this._raf = w.requestAnimationFrame(poll);
      };
      this._raf = w.requestAnimationFrame(poll);
    }
    return this;
  }

  detach() {
    if (!this.attached) return;
    this.attached = false;
    const w = this.win;
    const c = this.canvas;
    if (w) {
      if (this._raf) w.cancelAnimationFrame(this._raf);
      this._raf = 0;
      w.removeEventListener('keydown', this._onKeyDown);
      w.removeEventListener('keyup', this._onKeyUp);
      w.removeEventListener('mouseup', this._onMouseUp);
      w.removeEventListener('blur', this._onBlur);
    }
    if (c) {
      c.removeEventListener('mousemove', this._onMouseMove);
      c.removeEventListener('mousedown', this._onMouseDown);
      c.removeEventListener('wheel', this._onWheel);
      c.removeEventListener('contextmenu', this._onContextMenu);
    }
    if (globalThis.document) {
      document.removeEventListener('pointerlockchange', this._onPointerLockChange);
    }
    this.clearAll();
  }

  clearAll() {
    this.keys.fill(0);
    this.mouseButtons = 0;
    Atomics.store(this.i32, IN_MOUSE_BTNS, 0);
    Atomics.store(this.i32, IN_DX, 0);
    Atomics.store(this.i32, IN_DY, 0);
    Atomics.store(this.i32, IN_DZ, 0);
  }

  isPointerLockWanted() {
    return Atomics.load(this.i32, IN_PTR_LOCK_WANTED) !== 0;
  }

  exitPointerLock() {
    if (this.canvas && document.pointerLockElement === this.canvas) {
      this._suppressNextUnlockPause = true;
      try { document.exitPointerLock?.(); } catch {}
    }
  }

  _onPointerLockChange() {
    const wasLocked = this.pointerLocked;
    this.pointerLocked = !!(this.canvas && document.pointerLockElement === this.canvas);
    Atomics.store(this.i32, IN_USE_ABS, this.pointerLocked ? 0 : 1);
    const suppressed = this._suppressNextUnlockPause;
    this._suppressNextUnlockPause = false;
    if (wasLocked && !this.pointerLocked && this.isPointerLockWanted() && !suppressed && !document.hidden) {
      const now = performance.now();
      // Browsers intercept Esc to exit PointerLock without firing keydown(Escape).
      // Pulse DIK_ESCAPE so SWBF2 opens its Pause Menu (Respawn / Options) right inside fullscreen!
      if (now - this._lastEscapeKeyMs > 250) {
        this._lastSyntheticEscMs = now;
        this.tapKey(CODE_TO_DIK.Escape, 80);
      }
    }
  }

  _onKeyDown(ev) {
    const tag = ev.target && ev.target.tagName;
    if (tag === 'INPUT' || tag === 'TEXTAREA' || tag === 'SELECT') return;
    const dik = CODE_TO_DIK[ev.code];
    if (dik === undefined) return;
    // Prevent browser defaults (Esc staying in fullscreen via Keyboard Lock, F1 Hero accept,
    // F2-F4 squad commands, Alt focusing menu bar, Space/Tab/Arrows scrolling).
    const suppressDefault =
      ev.code === 'Escape' ||
      ev.code === 'Space' ||
      ev.code === 'Tab' ||
      ev.code === 'AltLeft' ||
      ev.code === 'AltRight' ||
      ev.code.startsWith('Arrow') ||
      ev.code === 'F1' ||
      ev.code === 'F2' ||
      ev.code === 'F3' ||
      ev.code === 'F4' ||
      ev.code === 'F6' ||
      ev.code === 'F7' ||
      ev.code === 'F8' ||
      ev.code === 'F9' ||
      ev.code === 'F10';
    if (ev.cancelable && suppressDefault) {
      ev.preventDefault();
    }
    if (ev.code === 'Escape') {
      const now = performance.now();
      if (now - this._lastSyntheticEscMs < 200) return;
      this._lastEscapeKeyMs = now;
    }
    Atomics.store(this.keys, dik, 0x80);
    Atomics.add(this.i32, IN_EVENT_SEQ, 1);
  }

  _onKeyUp(ev) {
    const dik = CODE_TO_DIK[ev.code];
    if (dik === undefined) return;
    Atomics.store(this.keys, dik, 0x00);
    Atomics.add(this.i32, IN_EVENT_SEQ, 1);
  }

  _mapClientToGame(clientX, clientY) {
    if (!this.canvas) return [Math.round(clientX), Math.round(clientY)];
    const rect = this.canvas.getBoundingClientRect();
    const w = rect.width > 0 ? rect.width : this.gameWidth;
    const h = rect.height > 0 ? rect.height : this.gameHeight;
    // Account for canvas object-fit: contain letterboxing/pillarboxing so taps and clicks
    // land pixel-accurately on 4:3 menus and 16:9 missions across phones, tablets, and ultrawides.
    const gameAspect = this.gameWidth / this.gameHeight;
    const rectAspect = w / h;
    let drawW = w;
    let drawH = h;
    let offX = 0;
    let offY = 0;
    if (rectAspect > gameAspect) {
      drawW = h * gameAspect;
      offX = (w - drawW) * 0.5;
    } else if (rectAspect < gameAspect) {
      drawH = w / gameAspect;
      offY = (h - drawH) * 0.5;
    }
    const gx = Math.max(0, Math.min(this.gameWidth - 1, Math.round(((clientX - rect.left - offX) * this.gameWidth) / drawW)));
    const gy = Math.max(0, Math.min(this.gameHeight - 1, Math.round(((clientY - rect.top - offY) * this.gameHeight) / drawH)));
    return [gx, gy];
  }

  // Direct programmatic input helpers (used by mobile TouchControls for zero-overhead SAB input).
  setKey(codeOrDik, down) {
    const dik = typeof codeOrDik === 'number' ? codeOrDik : CODE_TO_DIK[codeOrDik];
    if (dik === undefined || dik < 0 || dik >= 256) return;
    Atomics.store(this.keys, dik, down ? 0x80 : 0x00);
    Atomics.add(this.i32, IN_EVENT_SEQ, 1);
  }

  addMouseDelta(dx, dy) {
    const ix = Math.round(dx || 0);
    const iy = Math.round(dy || 0);
    if (ix === 0 && iy === 0) return;
    Atomics.store(this.i32, IN_USE_ABS, 0);
    if (ix !== 0) Atomics.add(this.i32, IN_DX, ix);
    if (iy !== 0) Atomics.add(this.i32, IN_DY, iy);
    Atomics.add(this.i32, IN_EVENT_SEQ, 1);
  }

  addMouseWheel(dz) {
    const iz = Math.round(dz || 0);
    if (iz === 0) return;
    Atomics.add(this.i32, IN_DZ, iz);
    Atomics.add(this.i32, IN_EVENT_SEQ, 1);
  }

  setMouseButton(bit, down) {
    if (down) this.mouseButtons |= bit;
    else this.mouseButtons &= ~bit;
    Atomics.store(this.i32, IN_MOUSE_BTNS, this.mouseButtons);
    Atomics.add(this.i32, IN_EVENT_SEQ, 1);
  }

  moveCursor(clientX, clientY) {
    const [gx, gy] = this._mapClientToGame(clientX, clientY);
    Atomics.store(this.i32, IN_CURSOR_X, gx);
    Atomics.store(this.i32, IN_CURSOR_Y, gy);
    Atomics.store(this.i32, IN_USE_ABS, 1);
    Atomics.add(this.i32, IN_EVENT_SEQ, 1);
  }

  setCursorDown(clientX, clientY, down, bit = 1) {
    this.moveCursor(clientX, clientY);
    this.setMouseButton(bit, down);
  }

  tapKey(codeOrDik, durationMs = 75) {
    this.setKey(codeOrDik, true);
    setTimeout(() => this.setKey(codeOrDik, false), durationMs);
  }

  hasPhysicalGamepad() {
    return Atomics.load(this.i32, IN_GP_CONNECTED) !== 0;
  }

  _onMouseMove(ev) {
    const dx = Math.round(ev.movementX || 0);
    const dy = Math.round(ev.movementY || 0);
    if (dx !== 0) Atomics.add(this.i32, IN_DX, dx);
    if (dy !== 0) Atomics.add(this.i32, IN_DY, dy);

    const lockedOrRel = this.pointerLocked || this.isPointerLockWanted();
    Atomics.store(this.i32, IN_USE_ABS, lockedOrRel ? 0 : 1);
    if (!lockedOrRel) {
      const [gx, gy] = this._mapClientToGame(ev.clientX, ev.clientY);
      Atomics.store(this.i32, IN_CURSOR_X, gx);
      Atomics.store(this.i32, IN_CURSOR_Y, gy);
    }
    Atomics.add(this.i32, IN_EVENT_SEQ, 1);
  }

  _buttonBit(button) {
    if (button === 0) return 1;
    if (button === 2) return 2;
    if (button === 1) return 4;
    if (button === 3) return 8;
    return 0;
  }

  _onMouseDown(ev) {
    if (this.canvas && this.isPointerLockWanted() && !this.pointerLocked && typeof this.canvas.requestPointerLock === 'function') {
      try {
        const p = this.canvas.requestPointerLock();
        if (p && typeof p.catch === 'function') p.catch(() => {});
      } catch {}
    }
    if (!this.pointerLocked && !this.isPointerLockWanted()) {
      const [gx, gy] = this._mapClientToGame(ev.clientX, ev.clientY);
      Atomics.store(this.i32, IN_CURSOR_X, gx);
      Atomics.store(this.i32, IN_CURSOR_Y, gy);
      Atomics.store(this.i32, IN_USE_ABS, 1);
    } else {
      Atomics.store(this.i32, IN_USE_ABS, 0);
    }
    this.mouseButtons |= this._buttonBit(ev.button);
    Atomics.store(this.i32, IN_MOUSE_BTNS, this.mouseButtons);
    Atomics.add(this.i32, IN_EVENT_SEQ, 1);
  }

  _onMouseUp(ev) {
    this.mouseButtons &= ~this._buttonBit(ev.button);
    Atomics.store(this.i32, IN_MOUSE_BTNS, this.mouseButtons);
    Atomics.add(this.i32, IN_EVENT_SEQ, 1);
  }

  _onWheel(ev) {
    if (ev.cancelable) ev.preventDefault();
    const dz = ev.deltaY < 0 ? 120 : (ev.deltaY > 0 ? -120 : 0);
    if (dz !== 0) Atomics.add(this.i32, IN_DZ, dz);
    Atomics.add(this.i32, IN_EVENT_SEQ, 1);
  }

  // Feed explicit gamepad state (for automated tests or custom controllers).
  setSyntheticGamepad({ connected = true, lx = 0, ly = 0, rx = 0, ry = 0, lt = 0, rt = 0, buttons = 0, pov = -1 } = {}) {
    this._syntheticGamepad = !!connected;
    Atomics.store(this.i32, IN_GP_CONNECTED, connected ? 1 : 0);
    Atomics.store(this.i32, IN_GP_LX, clampAxis(lx));
    Atomics.store(this.i32, IN_GP_LY, clampAxis(ly));
    Atomics.store(this.i32, IN_GP_RX, clampAxis(rx));
    Atomics.store(this.i32, IN_GP_RY, clampAxis(ry));
    Atomics.store(this.i32, IN_GP_LT, clampAxis(lt));
    Atomics.store(this.i32, IN_GP_RT, clampAxis(rt));
    Atomics.store(this.i32, IN_GP_BTNS, buttons | 0);
    Atomics.store(this.i32, IN_GP_POV, pov | 0);
    Atomics.add(this.i32, IN_EVENT_SEQ, 1);
  }

  pollGamepad() {
    if (this._syntheticGamepad) return;
    if (!globalThis.navigator || typeof navigator.getGamepads !== 'function') return;
    const pads = navigator.getGamepads();
    let pad = null;
    for (let i = 0; i < pads.length; ++i) {
      if (pads[i] && pads[i].connected) { pad = pads[i]; break; }
    }
    if (!pad) {
      Atomics.store(this.i32, IN_GP_CONNECTED, 0);
      return;
    }
    const ax = pad.axes || [];
    const btns = pad.buttons || [];
    let mask = 0;
    for (let i = 0; i < Math.min(32, btns.length); ++i) {
      if (btns[i] && btns[i].pressed) mask |= (1 << i);
    }
    // Standard gamepad D-pad (buttons 12=Up, 13=Down, 14=Left, 15=Right) -> DirectInput POV
    const up = !!(btns[12] && btns[12].pressed);
    const down = !!(btns[13] && btns[13].pressed);
    const left = !!(btns[14] && btns[14].pressed);
    const right = !!(btns[15] && btns[15].pressed);
    let pov = -1;
    if (up && right) pov = 4500;
    else if (down && right) pov = 13500;
    else if (down && left) pov = 22500;
    else if (up && left) pov = 31500;
    else if (up) pov = 0;
    else if (right) pov = 9000;
    else if (down) pov = 18000;
    else if (left) pov = 27000;

    const lt = btns[6] ? btns[6].value : 0;
    const rt = btns[7] ? btns[7].value : 0;

    Atomics.store(this.i32, IN_GP_CONNECTED, 1);
    Atomics.store(this.i32, IN_GP_LX, clampAxis(ax[0] || 0));
    Atomics.store(this.i32, IN_GP_LY, clampAxis(ax[1] || 0));
    Atomics.store(this.i32, IN_GP_RX, clampAxis(ax[2] || 0));
    Atomics.store(this.i32, IN_GP_RY, clampAxis(ax[3] || 0));
    Atomics.store(this.i32, IN_GP_LT, clampAxis(lt));
    Atomics.store(this.i32, IN_GP_RT, clampAxis(rt));
    Atomics.store(this.i32, IN_GP_BTNS, mask);
    Atomics.store(this.i32, IN_GP_POV, pov);
  }
}

export class InputBridgeReader {
  constructor(sab, module) {
    this.sab = sab;
    this.module = module;
    this.i32 = new Int32Array(sab, 0, INPUT_KEYS_OFFSET / 4);
    this.keys = new Uint8Array(sab, INPUT_KEYS_OFFSET, 256);
    this.keysPtr = module._malloc(256);
  }

  setPointerLockWanted(wanted) {
    Atomics.store(this.i32, IN_PTR_LOCK_WANTED, wanted ? 1 : 0);
  }

  sync() {
    const dx = Atomics.exchange(this.i32, IN_DX, 0);
    const dy = Atomics.exchange(this.i32, IN_DY, 0);
    const dz = Atomics.exchange(this.i32, IN_DZ, 0);
    const cx = Atomics.load(this.i32, IN_CURSOR_X);
    const cy = Atomics.load(this.i32, IN_CURSOR_Y);
    const useAbs = Atomics.load(this.i32, IN_USE_ABS);
    const mouseBtns = Atomics.load(this.i32, IN_MOUSE_BTNS);
    const gpConnected = Atomics.load(this.i32, IN_GP_CONNECTED);
    const lx = Atomics.load(this.i32, IN_GP_LX);
    const ly = Atomics.load(this.i32, IN_GP_LY);
    const rx = Atomics.load(this.i32, IN_GP_RX);
    const ry = Atomics.load(this.i32, IN_GP_RY);
    const lt = Atomics.load(this.i32, IN_GP_LT);
    const rt = Atomics.load(this.i32, IN_GP_RT);
    const gpBtns = Atomics.load(this.i32, IN_GP_BTNS);
    const gpPov = Atomics.load(this.i32, IN_GP_POV);

    // Copy 256-byte DirectInput keyboard state into persistent WASM scratch buffer.
    this.module.HEAPU8.set(this.keys, this.keysPtr);

    this.module._swbf2_input_sync_state(
      this.keysPtr,
      dx, dy, dz,
      cx, cy, useAbs, mouseBtns,
      gpConnected, lx, ly, rx, ry, lt, rt, gpBtns, gpPov,
    );
  }

  dispose() {
    if (this.keysPtr) {
      this.module._free(this.keysPtr);
      this.keysPtr = 0;
    }
  }
}
