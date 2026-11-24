# The spawn oracle: one trooper, in two builds

The ground oracle (docs/ground-oracle.md) showed that the browser build and the native build
put a capsule in the same place. This one asks the next question: does the browser build build
the *same entity* the native build builds?

The answer has to be measured, because a trooper is not a capsule. A trooper is a class (a
soldier's definition: its weapons, its properties, its animation banks), an entity (the class
built into the world), four weapon objects, six collision bodies, a health value and a physics
state. Every one of those comes out of the engine's own code, and every one of them can come out
differently in a different build.

## What the oracle does

Both builds run the same harness (`spike/level_harness.cpp`), which is compiled into the browser
module and into the native oracle:

1. The level is loaded by the engine's own loader (`LoadUtil::ReadDataFileOnHeap`), with the
   engine's own chunk filter.
2. The base class is loaded from `ingame.lvl` the way the mission script loads one: the class's
   name is hashed and the loader reads the nested level whose id is that hash
   (`LoadUtil.cpp:288-300`).
3. The soldier's own class is loaded from the side file the same way. The class's weapons, its
   ordnance, its models and its animation banks arrive with it, because they are in the same
   nested level.
4. The ground under the spawn point is found by the engine's own ray query
   (`CollisionManager::RayHit` with the filter `COLL_STATIC | COLL_TERRAIN | COLL_RIGID`).
5. The soldier is built by the engine's own factory (`EntitySoldierClass::Build`,
   `EntitySoldier.cpp:5830-5851`), which allocates it from the engine's memory pool, adds it to
   the collision manager and activates its physics. The three steps `Character::Spawn` takes
   after the build follow (`Character.cpp:1034-1039`).
6. The engine's own update loop runs for two seconds (`Thread::UpdateAll`, the call the game's
   own loop makes at `GameLoop.cpp:1299`).

The two results are compared exactly, field by field: the position, the health, the number of
weapons the class carries, the number of weapons the entity built, the number of collision
bodies, and the ground the entity stood on.

## The result

```
$ python3 tools/spawn_oracle_check.py
  spawned True at (0, 1.2, 0) on ground 1.2
  health 400/400, weapons 4/4, collision bodies 6, updates 60

PASS: the native and browser builds spawn the same soldier and agree on every number.
```

The numbers are the engine's: 1.2 is the height of the capsule the ground oracle drops (a
radius of 0.35 and a half-height of 0.85), 400 is the health the class gives a trooper, 4 is the
number of weapons the class carries, 6 is the number of collision bodies the class's own
collision scale gives the trooper.

## What had to be in place for the trooper to exist

Every one of these is the engine's own step. They are listed because each one was found by
running the code and reading where it stopped, not by reading the source and guessing.

* **The soldier's memory pool has to have room.** `EntitySoldierClass::Build` returns nothing
  when `EntitySoldier::sMemoryPool.IsFull()` (`EntitySoldier.cpp:5832`), and the pool's count
  starts at zero. The game sizes the pools in `GameLoop::Init` (`GameLoop.cpp:759-806`) and then
  sizes the soldier pool from the number of characters the mission expects
  (`SpawnManager::SetupSoldierMemoryPool`, `SpawnManager.cpp:235-242`). The harness makes the
  same calls with the same numbers, and states the mission's unit counts itself, because the
  retail game gets those from the mission's Lua script, which the harness does not run.

* **`GameLoop::Init` cannot be called as it stands.** It ends with the display managers' `Init`
  calls (`GameLoop.cpp:857-874`), and those build their text on the system's own font, which is
  a Direct3D9 texture (`RedFont::Init`, `RedFont.cpp:2335-2354`). There is no device in the
  browser. The harness therefore runs the lines of `GameLoop::Init` that the simulation needs:
  the teams, the spawn manager, the event manager, the display managers the soldier asks
  questions of, and the pool sizes.

* **The named fonts are data, not device work.** `RedVariableWidthFont::Read` reads the glyph
  table and asks for no device (`RedFont.cpp:2376-2440`), so the harness reads `core.lvl`'s font
  chunks with the engine's own loader. It also calls the engine's own glyph cache pool
  (`RedGlyphCacheElement::InitPool`, `GameState.cpp:886`).

* **The chunk filter has to name the weapon chunks.** The weapon classes are in chunks the
  engine calls `wnpc` (`LoadUtil.cpp:348`), not `wpnc`. With the wrong id the classes are
  skipped, the trooper is built with four weapon slots and no weapon objects, and the engine
  dereferences a null weapon on the first update (`EntitySoldier.cpp:3284`).

* **The low-resolution animator class has to exist before a soldier's class is read.** A
  soldier's class names its own low-resolution animation bank in its properties
  (`EntitySoldier.cpp:5591`), and the engine adds that bank to `SoldierAnimatorLowResClass`.
  The game creates that class right after it reads the file that holds the shared animation
  banks (`LuaCallbacks_Mission.cpp:619-622`), so the harness does the same: the base classes
  from `ingame.lvl` first, the soldier's own class second.

* **The lock-on managers have to exist.** A soldier asks the lock-on manager to drop its target
  every time it is updated (`EntitySoldier.cpp:6233`), and the managers are created in the
  engine's `GameLoop::PostLoad` (`GameLoop.cpp:946`).

* **The local player's input state has to exist.** A soldier asks the network layer whether the
  player is in mouse mode on every update (`EntitySoldier.cpp:6571`), and that reads the host
  joystick, which `NetGame::Create` makes (`NetGame.cpp:1955-1956`). The harness calls it
  because the engine's mission start-up calls it (`GameState.cpp:929`). It is not the
  multiplayer stack: the network is off in this build, so the call creates the local player's
  input and the game-state tables and opens no socket.

## Two things the retail data says that are worth knowing

* **The mission scripts in the data files were compiled with a four-byte Lua number (`float`).**
  `LuaFL/include/lua.h` includes `./luser_number.h`, which defines `#define LUA_NUMBER float`
  (4 bytes), matching the header of every precompiled script in `mission.lvl` and the
  `TEST_NUMBER` (`31415926.0f`, `lundump.h:19`). The spawn harness does not run any mission
  script yet—the numbers a mission script would have set are stated by the harness instead—so
  that the spawn oracle isolates entity construction from Lua execution. (An earlier revision of
  this note misread the `#ifndef LUA_NUMBER` fallback on `lua.h:52` as active; `sizeof(lua_Number)`
  is verified to be `4` at compile time.)

* **The mission script is what sizes the entity pools, and it does so at load time.** The
  script's `SetMemoryPoolSize` calls are in its main body, which runs when the script is opened
  (`LuaHelper::OpenScript` calls `lua_dobuffer`, `LuaHelper.cpp:68-92`), which is before
  `GameLoop::Init`. The engine's pool sizes are therefore the engine's defaults, with the
  script's overrides applied on top, and the soldier pool ends up sized from the character
  count.

## What comes next

The input/camera oracle (docs/input-camera-oracle.md) drives the trooper through walking,
jumping and rolling, with the engine's chase camera. The combat oracle
(docs/combat-oracle.md) fires real ordnance at engine-native target bots and measures hits and
kills. The browser mission check (browser/mission_check.mjs) then proves the same things in
headless Chrome, including the model being rasterised and a target taking damage.
