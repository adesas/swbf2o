// Browser-only input driver for the playable mission.
//
// The engine's DirectInput seam honestly says that a browser has no DirectInput device. The page
// therefore writes its keyboard and pointer-lock mouse state into the engine's own ProcessedInputs
// through swbf2_set_input; PlayerController, EntitySoldier and ChaseCamera do everything after
// that. This file deliberately has no node imports: it is loaded as an ES module by index.html.

export const BUTTON = Object.freeze({
  FIRE: 1 << 0,
  FIRE_SECONDARY: 1 << 1,
  SPRINT: 1 << 2,
  JUMP: 1 << 3,
  CROUCH: 1 << 4,
  ZOOM: 1 << 5,
  VIEW: 1 << 6,
  RELOAD: 1 << 7,
  USE: 1 << 8,
  ROLL: 1 << 29,
});

// RawControllerInputs::ReadLatest divides the pixel delta by the frame's seconds and applies the
// engine's 1/2048 mouse scale (pcFLJoystick.cpp:745-760).
const MOUSE_SCALE = 1 / 2048;

export function attachInputCamera(Module, options = {}) {
  const canvas = options.canvas;
  const state = {
    keys: new Set(),
    mouseButtons: 0,
    mouseDX: 0,
    mouseDY: 0,
    locked: false,
    enabled: true,
  };

  const prevent = new Set([
    'KeyW', 'KeyA', 'KeyS', 'KeyD', 'Space', 'ShiftLeft', 'ShiftRight',
    'ControlLeft', 'ControlRight', 'KeyC', 'KeyR', 'KeyV', 'KeyZ', 'KeyE', 'KeyF',
  ]);
  const onKey = (down) => (ev) => {
    if (!state.enabled) return;
    const code = ev.code || ev.key;
    if (down) state.keys.add(code); else state.keys.delete(code);
    if (prevent.has(code)) ev.preventDefault();
  };
  const onKeyDown = onKey(true);
  const onKeyUp = onKey(false);

  const onMouseMove = (ev) => {
    if (!state.locked || !state.enabled) return;
    state.mouseDX += ev.movementX || 0;
    state.mouseDY += ev.movementY || 0;
  };
  const onMouseButton = (ev, down) => {
    if (!state.enabled) return;
    if (ev.button === 0) state.mouseButtons = down
      ? (state.mouseButtons | BUTTON.FIRE) : (state.mouseButtons & ~BUTTON.FIRE);
    if (ev.button === 2) state.mouseButtons = down
      ? (state.mouseButtons | BUTTON.FIRE_SECONDARY) : (state.mouseButtons & ~BUTTON.FIRE_SECONDARY);
    if (down && canvas && document.pointerLockElement !== canvas) requestPointerLock();
    ev.preventDefault();
  };
  const onMouseDown = (ev) => onMouseButton(ev, true);
  const onMouseUp = (ev) => onMouseButton(ev, false);
  const onLockChange = () => {
    state.locked = document.pointerLockElement === canvas;
    if (!state.locked) {
      state.keys.clear();
      state.mouseButtons = 0;
    }
  };
  const onBlur = () => {
    state.keys.clear();
    state.mouseButtons = 0;
  };
  const onContextMenu = (ev) => ev.preventDefault();

  function requestPointerLock() {
    if (!canvas || !canvas.requestPointerLock) return;
    try {
      const result = canvas.requestPointerLock();
      if (result && result.catch) result.catch(() => {});
    } catch (_) { /* the browser only grants this from a user gesture */ }
  }

  if (typeof window !== 'undefined') {
    window.addEventListener('keydown', onKeyDown);
    window.addEventListener('keyup', onKeyUp);
    window.addEventListener('mousemove', onMouseMove);
    window.addEventListener('mouseup', onMouseUp);
    window.addEventListener('blur', onBlur);
    document.addEventListener('pointerlockchange', onLockChange);
    if (canvas) {
      canvas.addEventListener('mousedown', onMouseDown);
      canvas.addEventListener('contextmenu', onContextMenu);
    }
  }

  // Call once per requestAnimationFrame, before swbf2_frame. The engine's own 30 Hz tick then
  // consumes this record; there is no second simulation clock here.
  function send(dt) {
    if (!state.enabled) return;
    const k = state.keys;
    const move = (k.has('KeyW') ? 1 : 0) - (k.has('KeyS') ? 1 : 0);
    const strafe = (k.has('KeyD') ? 1 : 0) - (k.has('KeyA') ? 1 : 0);
    let buttons = state.mouseButtons;
    if (k.has('Space')) buttons |= BUTTON.JUMP;
    if (k.has('ShiftLeft') || k.has('ShiftRight')) buttons |= BUTTON.SPRINT;
    if (k.has('ControlLeft') || k.has('ControlRight') || k.has('KeyC')) buttons |= BUTTON.ROLL;
    if (k.has('KeyR')) buttons |= BUTTON.RELOAD;
    if (k.has('KeyV')) buttons |= BUTTON.VIEW;
    if (k.has('KeyZ')) buttons |= BUTTON.ZOOM;
    if (k.has('KeyE')) buttons |= BUTTON.USE;
    if (k.has('KeyF')) buttons |= BUTTON.FIRE;

    const seconds = dt > 0 ? dt : 1 / 60;
    const turn = (state.mouseDX * MOUSE_SCALE) / seconds;
    const pitch = (-state.mouseDY * MOUSE_SCALE) / seconds;
    state.mouseDX = 0;
    state.mouseDY = 0;
    Module.ccall('swbf2_set_input', null,
      ['number', 'number', 'number', 'number', 'number', 'number'],
      [move, strafe, turn, pitch, buttons, 2]);
  }

  function detach() {
    if (typeof window !== 'undefined') {
      window.removeEventListener('keydown', onKeyDown);
      window.removeEventListener('keyup', onKeyUp);
      window.removeEventListener('mousemove', onMouseMove);
      window.removeEventListener('mouseup', onMouseUp);
      window.removeEventListener('blur', onBlur);
      document.removeEventListener('pointerlockchange', onLockChange);
      if (canvas) {
        canvas.removeEventListener('mousedown', onMouseDown);
        canvas.removeEventListener('contextmenu', onContextMenu);
      }
    }
  }

  return { state, send, requestPointerLock, detach };
}

export const SWBF2Input = { attach: (Module, canvas) => attachInputCamera(Module, { canvas }) };
if (typeof window !== 'undefined') window.SWBF2Input = SWBF2Input;
