# The input and camera oracle: drive the trooper, in two builds

The ground oracle puts a capsule down. The spawn oracle builds a trooper. This one drives it
with the same input in the native ILP32 and browser wasm32 builds and compares every turn.

## The engine's own path

The browser has no DirectInput device, so the host does not invent one. It writes one
`ProcessedInputs` record through `LevelHarnessSetInput`, using the engine's own
`ProcessedInputs::Clear` and `ProcessedInputs::Set`. After that, everything is engine code:

```
ProcessedInputs
  -> PlayerController::Update       (PlayerController.cpp:247-500)
  -> Controllable::mControlMove / mControlTurn / mControlFire
  -> EntitySoldier::Update
  -> Thread::UpdateAll              (the game's own turn loop)
  -> CameraManager::Update          (ChaseCamera)
```

The chase camera is attached by `CameraManager::Chase` and `ChaseMode`, not by a browser-side
follow calculation. The canvas supplies the interface view's pixel dimensions through
`LevelHarnessSetViewport`: the chase camera divides by those dimensions when it computes its
aspect ratio, so a zero-sized view is not a valid browser viewport.

The 30 Hz frame loop calls the engine's clock step, then the controller and the camera. There
is no second simulation clock in JavaScript.

## Measured result

```
$ python3 tools/input_camera_check.py
  attached True, ready True, 290 driven turns
  final position (-13.4076, -0.02, 20.974), facing (-0.694086, 0, 0.72017)
  camera at (-11.2531, 2.55468, 18.7384), fov 0.525119 rad

PASS: the native and browser builds drive the trooper the same way and agree on every number,
      turn by turn.
```

The script settles, walks, strafes, turns, jumps, recovers, rolls and fires. The position,
velocity, facing, control values, soldier state, energy, fire/special-move triggers and camera
are compared exactly.

## The roll needed two engine steps, not a trigger fix

The first roll attempt was rejected while the jump's `INPUTLOCK_ACTION` was active. The engine
clears `mControlSpecialMove` during that lock (`EntitySoldier.cpp:6360-6369`), so the movement
script now includes a recovery phase. On the ground the engine accepts the roll: `mState =
ROLL` and the energy bar spends its cost.

The first accepted roll then found a missing start-up step: `SoldierAnimatorClass::PostLoad`
builds the action-animation table used by `GetLowerBodyActionAnimation(DIVE, HUMAN_RIFLE)`.
The harness now runs the engine's `SoldierAnimationBank::Reset` and
`SoldierAnimatorClass::PostLoad` once after the class files are loaded, before the first
soldier is built. The roll animation exists and the roll completes. No trigger implementation
was changed.

## Drawing the class's own model

The class record is a `lvl_` record, so its first child id is the class hash, not `lvl_`. The
model walker descends through that record only on the append path; ordinary level reads keep
their original behaviour and do not include alternate world variants. The class's high
resolution model is selected by the engine's own `GameModel::_uiNameHash`, not by a hard-coded
model name.

Each unit gets an instance whose matrix is read from its `GameObject`. The map's geometry and
texture-name table are preserved when class models are appended. Culling uses the same
row-major matrix as the shader; transposing it would discard the translation and cull a moving
trooper as if it were still at the origin. A draw counter (`swbf2_trooper_drawn`) reports the
segments actually rasterised, so "the model loaded" is not mistaken for "the model is on
screen".

## Commands

```
bash tools/build_browser.sh
bash tools/build_native_oracle.sh
bash tools/build_input_camera_oracle.sh
python3 tools/input_camera_check.py
node browser/mission_check.mjs
```

`browser/mission_check.mjs` additionally proves, in headless Chrome, that a real W key event
moves the trooper and that the trooper remains rasterised after the move.
