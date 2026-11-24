// The browser half of the input/camera oracle, and the page-side input driver.
//
//   node browser/input_camera.mjs [level] [class] [class-file]
//
// It loads a level through the engine's own loader, loads the soldier's class the way the mission
// script loads one, spawns the soldier at the ground the engine's own ray query finds, gives the
// engine the size of the picture (the chase camera's field of view is computed from it), attaches
// the engine's own chase camera to the trooper, and then runs the movement script - which lives
// in the shared harness (spike/level_harness.cpp), so the browser build and the native build drive
// the same trooper with the same injected input by construction rather than by two scripts
// agreeing. The numbers are printed in the same JSON shape the native oracle prints, and
// tools/input_camera_check.py compares them field by field.
//
// The same file is also the page's input driver: import it and call attachInputCamera(Module) and
// the page's keyboard and pointer-lock mouse events become the engine's input record.
//
// The files are staged into the wasm VFS the same way the browser host stages them. The class file
// is the big one: side/cis.lvl is a hundred megabytes, and the harness reads only the one nested
// level it needs out of it.
import fs from 'fs';
import path from 'path';
import url from 'url';

const here = path.dirname(url.fileURLToPath(import.meta.url));
const root = path.join(here, '..');

/* The button bits, named for what the engine's own PlayerController does with them. The numbers
   are the engine's: bits.mBoolInputs (FLJoystick.h, ePROCESSEDINPUT_TYPE) and the bit test the
   engine uses for the special move, the roll (PlayerController.cpp:480). */
export const BUTTON = {
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
};

/* The picture size the oracle gives the engine. The native oracle uses the same one
   (spike/native/native_main_input_camera.cpp), because the chase camera's field of view is
   computed from the engine's view and screen dimensions (ChaseCamera.cpp:306-309). */
export const ORACLE_VIEWPORT = { width: 1280, height: 720 };

/* The mouse scale the engine uses when it turns a mouse delta into a turn rate
   (RawControllerInputs::ReadLatest, pcFLJoystick.cpp:745-760: the delta divided by the frame's
   seconds, times the mouse sensitivity over 2048). */
const MOUSE_SCALE = 1 / 2048;

/* ------------------------------------------------------------------ the page's input driver */

/* Turns the page's keyboard and pointer-lock mouse into the engine's input record.
 *
 * The page has no keyboard and no mouse as far as the engine is concerned - its DirectInput
 * device layer fails, because a browser has no DirectInput (browser/platform/dinput_browser.cpp) -
 * so the page's events are injected into the engine's own ProcessedInputs, which is what the
 * engine's player controller reads (PlayerController.cpp:157, 247-500).
 *
 * The engine's own conventions are kept:
 *   - W and S are the move axis, A and D the strafe axis, both -1..1;
 *   - the mouse is a rate, not a position: the engine clamps it to +-16 in mouse mode
 *     (PlayerController.cpp:276-300), and it expects the pixels-per-second scale the engine's own
 *     device layer produces (the delta over the frame's seconds, over 2048);
 *   - the buttons are the engine's own bits.
 */
export function attachInputCamera(Module, options = {}) {
  const state = {
    keys: new Set(),
    mouseDX: 0,
    mouseDY: 0,
    locked: false,
    enabled: true,
    lastTime: 0,
  };

  const canvas = options.canvas || (typeof document !== 'undefined' ? document.body : null);

  const onKey = (down) => (ev) => {
    if (!state.enabled) return;
    const code = ev.code || ev.key;
    if (down) state.keys.add(code); else state.keys.delete(code);
    if (['KeyW', 'KeyA', 'KeyS', 'KeyD', 'Space', 'ShiftLeft', 'ControlLeft', 'KeyC', 'KeyR']
        .includes(code)) {
      if (ev.preventDefault) ev.preventDefault();
    }
  };
  const onKeyDown = onKey(true);
  const onKeyUp = onKey(false);

  const onMouseMove = (ev) => {
    if (!state.locked || !state.enabled) return;
    state.mouseDX += ev.movementX || 0;
    state.mouseDY += ev.movementY || 0;
  };
  const onLockChange = () => {
    state.locked = (document && document.pointerLockElement) ? true : false;
    if (!state.locked) state.keys.clear();
  };

  if (typeof window !== 'undefined') {
    window.addEventListener('keydown', onKeyDown);
    window.addEventListener('keyup', onKeyUp);
    window.addEventListener('mousemove', onMouseMove);
    document.addEventListener('pointerlockchange', onLockChange);
  }

  /* Asks the browser for the pointer lock. A page cannot take it without a user gesture, so this
     has to be called from a click. */
  const requestPointerLock = () => {
    if (canvas && canvas.requestPointerLock) canvas.requestPointerLock();
  };

  /* Sends the page's current input to the engine. Call this once per frame, before the engine's
     turn update. */
  const send = (dt) => {
    const k = state.keys;
    const move = (k.has('KeyW') ? 1 : 0) - (k.has('KeyS') ? 1 : 0);
    const strafe = (k.has('KeyD') ? 1 : 0) - (k.has('KeyA') ? 1 : 0);
    let buttons = 0;
    if (k.has('Space')) buttons |= BUTTON.JUMP;
    if (k.has('ShiftLeft') || k.has('ShiftRight')) buttons |= BUTTON.SPRINT;
    if (k.has('ControlLeft') || k.has('ControlRight') || k.has('KeyC')) buttons |= BUTTON.ROLL;
    if (k.has('KeyR')) buttons |= BUTTON.RELOAD;
    if (k.has('KeyF')) buttons |= BUTTON.FIRE;

    const seconds = dt > 0 ? dt : 1 / 60;
    const turn = (state.mouseDX * MOUSE_SCALE) / seconds;
    const pitch = (-state.mouseDY * MOUSE_SCALE) / seconds;
    state.mouseDX = 0;
    state.mouseDY = 0;

    Module.ccall('swbf2_set_input', null,
      ['number', 'number', 'number', 'number', 'number', 'number'],
      [move, strafe, turn, pitch, buttons, 2]);
  };

  return { state, send, requestPointerLock };
}

/* ------------------------------------------------------------------ the oracle's node half */

async function main() {
  const GAME = path.join(root, 'data/Star wars battlefront 2/GameData/data/_lvl_pc');
  const modulePath = process.env.SWBF2_MODULE || path.join(root, 'build/browser/swbf2.js');
  const factory = (await import(url.pathToFileURL(modulePath).href)).default;

  const Module = await factory({
    locateFile: (f) => path.join(path.dirname(modulePath), f),
    onAbort: (why) => { console.error('ABORT:', why); process.exit(3); },
  });
  const ccall = (...a) => Module.ccall(...a);

  ccall('swbf2_init', 'number', ['number'], [0]);
  try { Module.FS.mkdir('/data'); } catch (e) { /* already there */ }

  function stage(rel) {
    const src = path.join(GAME, rel);
    if (!fs.existsSync(src)) { console.error(`skip ${rel}: not found`); return null; }
    const dst = '/data/' + rel.replace(/\//g, '_');
    Module.FS.writeFile(dst, fs.readFileSync(src));
    return dst;
  }

  const level = process.argv[2] || 'tat/tat2';
  const className = process.argv[3] || 'cis_inf_rifleman';
  const classFile = process.argv[4] || `side/cis.lvl;${className}`;

  for (const rel of ['common.lvl', 'core.lvl', 'ingame.lvl', 'tat/tat2.lvl', 'mission.lvl']) {
    stage(rel);
  }
  stage(classFile.split(';')[0]);

  if (ccall('swbf2_load_level', 'number', ['string'], [level]) !== 1) {
    console.error(`swbf2_load_level(${level}) failed`);
    process.exit(1);
  }
  if (ccall('swbf2_load_class', 'number', ['string'], ['ingame.lvl;com_inf_default']) !== 1) {
    console.error('swbf2_load_class(ingame.lvl;com_inf_default) failed');
    process.exit(1);
  }
  if (ccall('swbf2_load_class', 'number', ['string'], [classFile]) !== 1) {
    console.error(`swbf2_load_class(${classFile}) failed`);
    process.exit(1);
  }

  /* The ground the engine's own collision query finds under the spawn point. */
  const restPtr = Module._malloc(4);
  ccall('swbf2_ground_probe_result', 'number',
    ['number', 'number', 'number', 'number', 'number', 'number'],
    [0, 0, 0, 0, restPtr, 0]);
  const groundRest = Module.getValue(restPtr, 'float');
  Module._free(restPtr);

  const xPtr = Module._malloc(4);
  const yPtr = Module._malloc(4);
  const zPtr = Module._malloc(4);
  const spawned = ccall('swbf2_spawn_soldier', 'number',
    ['string', 'number', 'number', 'number', 'number', 'number', 'number', 'number', 'number',
     'number', 'number', 'number', 'number'],
    [className, 0, groundRest, 0, 0, xPtr, yPtr, zPtr, 0, 0, 0, 0, 0]);
  const spawnX = Module.getValue(xPtr, 'float');
  const spawnY = Module.getValue(yPtr, 'float');
  const spawnZ = Module.getValue(zPtr, 'float');
  for (const p of [xPtr, yPtr, zPtr]) Module._free(p);
  if (spawned !== 1) {
    console.error('swbf2_spawn_soldier failed');
    process.exit(1);
  }

  /* The picture size, so the chase camera's field of view is a real number and not 0/0. */
  const bViewport = ccall('swbf2_set_viewport', 'number', ['number', 'number'],
    [ORACLE_VIEWPORT.width, ORACLE_VIEWPORT.height]);

  /* The engine's chase camera on the trooper, and the engine's own player controller. */
  const bAttached = ccall('swbf2_control_attach', 'number', []);

  /* The movement script, from the shared harness: the same input sequence the native oracle runs. */
  const iTurns = ccall('swbf2_movement_script_length', 'number', []);
  const FLOATS = 24;
  const INTS = 6;
  const fBuf = Module._malloc(FLOATS * iTurns * 4);
  const iBuf = Module._malloc(INTS * iTurns * 4);
  const iRan = ccall('swbf2_movement_script', 'number',
    ['number', 'number', 'number', 'number'],
    [fBuf, FLOATS, iBuf, INTS]);

  const states = [];
  for (let i = 0; i < iRan; ++i) {
    const rec = [];
    for (let n = 0; n < INTS; ++n) {
      rec.push(Module.getValue(iBuf + (i * INTS + n) * 4, 'i32'));
    }
    for (let f = 0; f < FLOATS; ++f) {
      rec.push(Module.getValue(fBuf + (i * FLOATS + f) * 4, 'float'));
    }
    states.push(rec);
  }
  Module._free(fBuf);
  Module._free(iBuf);

  /* The same record shape the native oracle prints: four ints (turns, state, fire, special), the
     engine's own input bits and mouse mode, then the numbers. */
  const NAMES = ['updates', 'iState', 'iFireDown', 'iSpecialMoveDown', 'uiHostButtons',
    'uiHostMouseMode', 'x', 'y', 'z', 'dirX', 'dirY', 'dirZ', 'velX', 'velY', 'velZ', 'controlMove',
    'controlStrafe', 'controlTurn', 'controlPitch', 'fHealth', 'camX', 'camY', 'camZ', 'camDirX',
    'camDirY', 'camDirZ', 'camFov', 'camZoom', 'fRollSpeedFactor', 'fEnergy'];

  console.log('INPUT_CAMERA_ORACLE_JSON ' + JSON.stringify({
    level,
    class: className,
    class_file: classFile,
    attached: bAttached === 1,
    ready: ccall('swbf2_control_ready', 'number', []) === 1,
    viewport: bViewport === 1,
    ground_rest: groundRest,
    x: spawnX,
    y: spawnY,
    z: spawnZ,
    script_turns: iRan,
    recorded: states.length,
    names: NAMES,
    states,
  }));
}

if (import.meta.url === url.pathToFileURL(process.argv[1]).href) {
  main().catch((e) => { console.error(e); process.exit(1); });
}
