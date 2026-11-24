# The combat oracle: a blaster fight, in two builds

The ground oracle (docs/ground-oracle.md) showed that the two builds put a capsule in the same
place. The spawn oracle (docs/spawn-oracle.md) showed that they build the same trooper. The
input/camera oracle (docs/input-camera-oracle.md) showed that they drive it the same way. None
of those fires a shot: a trooper can be built identically and still not shoot, because a weapon
is a state machine, a trigger, an ordnance class, a collision manager and a damage system, and
every one of those can come out differently in a different build.

This one asks the last question: **does the engine's own blaster fight the same fight in both
builds?**

## What the oracle does

Both builds run the same harness (`spike/combat_harness.cpp`), which is compiled into the
native oracle and into the wasm module, and both run the same fight script
(`CombatHarnessRun`, in the harness). The two reports are therefore two builds' readings of one
fight, not two scripts that agree.

The fight:

1. **The player's trooper.** Spawned and attached to the engine's own controller by the shared
   level harness (`LevelHarnessSpawn`, `LevelHarnessControlAttach`), on the ground the engine's
   own ray query found. The harness puts it on the player's team (the level harness's own spawn
   leaves it on the neutral team 0) with the two lines `Character::Spawn` would have done
   (`Character.cpp:1043-1049`).

2. **Two teams.** `Team::CreateAll`, `Team::SetSide` (3 is the Republic, 4 is the CIS) and
   `Team::AddClass` (`Team.cpp:643`, the C++ entry point the mission script's `SetupTeams`
   calls). The player's class is `rep_inf_ep3_rifleman`, the targets' class is
   `cis_inf_rifleman`, both loaded the way the mission loads one (`LevelHarnessLoadClass`, the
   engine's own `LoadUtil` path).

3. **The target bots.** `Team::CreateCharacter` with the player id `-1`, which is what makes the
   engine's own controller a `UnitMobileController` instead of a `PlayerController`
   (`EntitySoldier::SetupController`, `EntitySoldier.cpp:3705-3724`), then
   `Character::SelectClass` and `Character::Spawn` (`Character.cpp:952`), whose own last steps
   are the class's `Build`, the team link, the character link and `Entity::Init`
   (`Character.cpp:1027-1049`). The bots are named by the engine's own
   `Team::GenerateRandomName` (`Team.cpp:871`), so the fight needs no mission script and no
   network.

4. **The player's shot.** The harness holds the fire bit in the engine's own `HostInput` and
   hands it to the engine with `LevelHarnessSetInput` / `LevelHarnessControlUpdate`. The
   engine's `PlayerController` turns that into `Controllable::mControlFire[0]`
   (`PlayerController.cpp:463-470`), the weapon's own fire state machine reads that trigger
   (`WeaponCannon::CheckFire`, `WeaponCannon.cpp:653`), and `WeaponCannon::Fire`
   (`WeaponCannon.cpp:728`) builds the ordnance. **There is no host-side raycast and no
   host-side damage anywhere in this harness**: the shot is the engine's `OrdnanceLaser`, and
   the hit is the engine's `OrdnanceBullet::Collide` -> `ApplyDamage`
   (`OrdnanceBullet.cpp:619`).

5. **The targets' shots.** The engine's own AI presses the targets' triggers
   (`AILowLevel::ProcessFire`, `AILowLevel.cpp:607-634`) - but only in the AI's own decision
   loop, which needs a `VisionManager` and a `CombatHelper` this harness does not have. The
   harness therefore presses the same trigger, on the same channel
   (`mControlFire[mWpnChannel]`), from a harness `Thread` that the engine runs at group 25 of
   its own turn list: after the AI's thread (group -10) and before the weapon's thread
   (group 30). The trigger itself is the engine's own `Trigger::Update`.

6. **The measurement.** A second harness `Thread`, at group 45, reads the engine's own ordnance
   list (`CollisionManager::mOrdnance`) once per engine turn. The bolts are created by the
   weapon at group 30, integrated at group 40 and consumed by the collision manager's own
   ordnance pass at group 60, so the probe has to run between the bolt and the collision pass
   to see a bolt at all. The hits are the engine's own damage state: a hit is a fall in a
   unit's `mCurHealth` between two of the engine's own turns, and a kill is the engine's own
   alive flag going false.

7. **The player's aim.** The engine fires a player's weapon along the camera's eye direction
   (`EntitySoldier::UpdateWeaponAndAimer`, `EntitySoldier.cpp:3040-3092`), so the harness
   updates the camera every turn (`LevelHarnessCameraUpdate`, the call the game makes at
   `GameLoop.cpp:1837`) and steers the trooper with the turn and pitch the host input carries.

## The result

```
$ python3 tools/combat_oracle_check.py
  4 targets asked for, 4 spawned, on team 2 (side 4)
  player PLAYER     rep_inf_ep3_rifleman   at (0,0,0) health 400 -> 310 hits 1 shots 7.00001
  target Super Battle Droid cis_inf_rifleman       at (-2.25,1.2,28) health 400 -> -50 hits 2 shots 2 killed
  target Super Battle Droid cis_inf_rifleman       at (-0.75,1.2,32) health 400 -> -125 hits 3 shots 5.00001 killed
  target Super Battle Droid cis_inf_rifleman       at (0.75,1.2,36) health 400 -> 400 hits 0 shots 6.00001
  target Super Battle Droid cis_inf_rifleman       at (2.25,1.2,40) health 400 -> 400 hits 0 shots 6.00001
  hits 6, kills 2, damage dealt 975, damage taken 90
  ordnance created 26, peak 5, expired 22
  turns 110 of engine time (3.66666s), 110 turn rows

PASS: the native and browser builds fight the same fight and agree on every number.
```

The numbers are the engine's: 400 is the health the classes give a trooper, 75 is the damage
the Republic rifleman's blaster carries, 90 is the damage the CIS rifleman's carries, 26 is the
number of bolts the engine's own weapon and ordnance created, and the two killed bots are the
engine's own dead flags.

The comparison is exact: the harness builds one JSON report and both drivers print it, so what
is compared is the two builds' bytes. Every float is printed with nine significant digits,
which is what it takes to name a float32 exactly (`FLT_DECIMAL_DIG` is 9).

## The two builds

* **native**: `build/native_oracle/combat_oracle`, ILP32 (i386), the same engine objects, the
  same front-end, the same browser platform seams, a different target.
* **wasm**: `build/combat_oracle/swbf2_combat.js`, wasm32, built from the same sources with
  `tools/build_browser.sh`'s flags.

The native build is ILP32 on purpose. The engine was written for 32-bit Windows and still
assumes it in places - `GameState.cpp:1668` allocates four bytes per pointer in a vector of
pointers, and `RedMemory.cpp` truncates heap pointers to `unsigned int` - so a 64-bit native
build corrupts its own heap. wasm32 and i386 have the same pointer width and the same struct
layouts, so the comparison is between two builds that can agree, and the browser's wasm32 build
is one of them.

### One thing the two builds do not share: their maths library

The native oracle links the host's glibc; the wasm build links musl. Their `atan2f` and their
`sinf` differ by one unit in the last place on some inputs - 54 of the engine's own 2048 sine
table entries differ, and the table is built with `sinf` and `cosf` (`PblMath.cpp:24-33`). A
harness that asked the platform for an angle would therefore aim a hair differently in the two
builds, and the oracle would report a difference that belongs to the platform and not to the
engine.

So the harness computes its own aim angles, out of the cross and the dot of the two directions
(`DetAtan2` in `spike/combat_harness.cpp`), and the two builds then agree to the last bit. The
accuracy is not the point: the point is that the same code gives the same bits in both builds.

## Running it

```
python3 tools/combat_oracle_check.py                 # builds both halves and compares
python3 tools/combat_oracle_check.py --skip-build    # compares what is already built
python3 tools/combat_oracle_check.py --targets 5     # five target bots instead of four
python3 tools/combat_oracle_check.py --json          # the whole report, both builds
```

The wasm half is a **standalone module**, not the browser module: `tools/build_browser.sh` fixes
the browser module's source list and its exported-function list, and adding to them is a change
to a file this track does not own. The standalone module is the browser module with the combat
harness and three exports added, and nothing else. The three exports are the same names the
browser module already uses for the same jobs:

```c
int         swbf2_combat_run(const char* level, const char* player_class,
                              const char* player_class_file, const char* target_class,
                              const char* target_class_file, int target_count);
const char* swbf2_combat_report(void);
const char* swbf2_log_line(int i);
```

`swbf2_combat_run` returns 1 when the fight ran, and the report is the harness's own JSON.
`browser/combat_probe.mjs` stages the game's files into the VFS and calls them; the check
script drives it.

## What the browser module would need

To run this fight inside `build/browser/swbf2.js` instead of in the standalone module, the
browser module needs three things, all of them in files this track does not own:

1. **`browser/main.cpp`**: the three exports above, and `spike/combat_harness.cpp` in the
   module's source list (`tools/build_browser.sh`).
2. **`tools/build_browser.sh`**: `spike/combat_harness.cpp` in the source list and
   `_swbf2_combat_run`, `_swbf2_combat_report` in `EXPORTED_FUNCTIONS`.
3. **`browser/host.js`**: the data the fight needs staged in the VFS - the level, the base
   class and the two faction classes - and a call to `swbf2_combat_run` when the fight is
   wanted.

Nothing in the harness needs changing for that: `CombatHarnessRun` is a plain function over
the shared level harness's public API, and it is already the same function in both builds.

## What had to be in place for the fight to happen

Every one of these is the engine's own step. They are listed because each one was found by
running the code and reading where it stopped, not by reading the source and guessing.

* **The soldier's pool has to have room for the bots.** `Character::CreateAll` sizes the
  character array, and `Character::GetPlayerStats` indexes it with the character's own index
  (`Character.cpp:1515`), so the harness re-sizes the array with
  `Character::CreateAll(1 + COMBAT_MAX_TARGETS)` after the player's trooper is attached.

* **A character needs a class before it can spawn.** `Character::Spawn` refuses without one
  (`Character.cpp:1010-1012`), and `Team::CreateCharacter` does not choose one, so the harness
  calls `Character::SelectClass` the way `ChangeClass` and `SelectSpawn` do.

* **A character's team pointer is set by `Character::InitialUpdate`**, not by
  `Team::CreateCharacter` (`Character.cpp:440-443`). The harness makes that one line itself,
  because `InitialUpdate` also calls `Respawn`, which would put the character where the spawn
  manager wants it, and this fight places its units itself.

* **The engine's own AI releases the trigger it was given.** `AILowLevel::ProcessFire` presses
  the weapon channel and releases the other one (`AILowLevel.cpp:607-634`), and the AI's thread
  runs at group -10 of the engine's turn list. A harness that pressed the trigger before the
  turn had it released again before the weapon's thread (group 30) saw it. The harness's fire
  thread therefore runs at group 25, inside the engine's own turn list.

* **A player's weapon fires along the camera's eye direction**, not along the trooper's facing
  (`EntitySoldier.cpp:3040-3092`). Without a camera update every turn, the trooper keeps
  aiming where it was when it spawned.

* **The engine's own services have to exist.** The harness runs no front end, so
  `StatusDisplay`, `ScreenFilterEffect` and `FlagItem` are created if they are not there: a
  soldier that takes damage asks the status display for a damage arrow
  (`StatusDisplay.cpp:175`), a soldier that takes damage asks the screen filter for a flash
  (`Damageable.cpp:203-221`), and a kill asks the flag list for the flag
  (`PlayerStats.cpp:522`).

* **A dead player's controller is freed.** `Controllable::Kill` releases it
  (`Controllable.cpp:303, 373-382`), and the level harness's cached controller would then be a
  dangling pointer, so the harness stops the fight when the player dies and says so in the
  report (`player_died`).

* **The bolt's own pool has to have room.** The engine allocates the ordnance from its own
  pool, and a pool whose item is smaller than the object is not a pool. The report carries the
  pool's counts and sizes (`diag`) so that a pool that is too small is visible in the result
  rather than in a crash.

## What the report holds

One JSON object, built by the harness, printed by both drivers:

* the setup: the level, the classes, the teams and their sides, the target count, the script;
* `units`: every unit - its role, the name the engine gave it, the class, the team, the player
  id, where it was asked to stand, where the engine's own matrix says it was before and after,
  its health before and after, its alive flags, its hits, the turn of each hit and the health
  either side of it, the weapons its class carries and the entity built, the ammo counter's
  own numbers, and where its weapon points;
* `ordnance`: every bolt the engine's list held - the turns it was seen in, the class's id,
  the unit that fired it, where it was and how fast it was going, the damage it carried, and
  the unit it hit when the harness could name one;
* `turns`: one row per engine turn - the player's weapon state and trigger, the shots the
  engine's own ammo counter says were fired, the bolts the engine's list held and lost, the
  health of the player and of the targets, what the engine's own controller did with the
  harness's turn and pitch, and where the trooper's own matrix and its aimer say it points;
* `diag`: the engine's own pool sizes, the seeds its random streams hold, and the number of
  collision meshes in the level.
