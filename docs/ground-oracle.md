# The ground oracle: where the engine puts a capsule down

Plan item 3 asks for a collision hull and a terrain ground clamp, checked against the engine's own
answer. This note records that check: the same level, the same drop, in two builds, compared
number by number.

Reproduce:

```sh
sudo apt-get install libc6-dev-i386 g++-multilib   # the 32-bit C library, once
python3 tools/ground_oracle_check.py
```

It builds the native oracle (`tools/build_native_oracle.sh`), builds the browser module
(`tools/build_browser.sh`), loads `tat/tat2` in each through the engine's own loader, drops a
capsule at seventeen points across the world, and compares the two sets of results exactly.

## What is actually being measured

Nothing in the harness computes a ground height of its own:

* the level is read by `LoadUtil::ReadDataFile` (`Battlefront2/Source/LoadUtil.cpp:524`), the
  engine's own loader, with an explicit list of the chunk ids the simulation needs
  (`entc`, `wrld`, `prp_`, `ordc`, `comb`, `expc`, `wpnc`, `coll`, `prim`, `tern`, `path`, `plan`,
  `plnp`, `port`). Models, textures, fonts, HUD and scripts are left out: the browser reads models
  itself, the fonts need a Direct3D9 device, the HUD has its own chunk reader in the browser, and
  the mission script needs the Lua mission flow this build does not run.
* the ground is found by `CollisionManager::RayHit` with the filter `COLL_STATIC | COLL_TERRAIN |
  COLL_RIGID` — the filter the engine itself uses when it drops a chunk onto the world
  (`Chunk.cpp:590`).
* the terrain height is `RedTerrain::GetInterpolatedHeight` (`RedTerrain.cpp:193`), the engine's
  own bilinear interpolation of the height patches.
* the capsule falls with the engine's own gravity (−9.8) integrated the way `EntityItem::Update`
  integrates it (`EntityItem.cpp:367`): half a step of velocity, then a step of position.

The harness (`spike/level_harness.cpp`) is compiled into both builds, so there is one piece of
harness code and two targets.

## The result

Seventeen points, three builds, identical:

| build | terrain | ground | rest | steps |
|---|---:|---:|---:|---:|
| native i386 | 0 | 0 | 1.2000000476837158 | 382 |
| browser wasm32, `-O1` | 0 | 0 | 1.2000000476837158 | 382 |
| browser wasm32, `-O0` | 0 | 0 | 1.2000000476837158 | 382 |

and at (25, 25), where the level's collision geometry is above the terrain:

| build | terrain | ground | rest | steps |
|---|---:|---:|---:|---:|
| native i386 | 0 | 14.966317176818848 | 16.166316986083984 | 367 |
| browser wasm32, `-O1` | 0 | 14.966317176818848 | 16.166316986083984 | 367 |
| browser wasm32, `-O0` | 0 | 14.966317176818848 | 16.166316986083984 | 367 |

The rest height on flat ground is 1.2, which is the capsule's radius (0.35) plus its half-height
(0.85) — the arithmetic the harness is supposed to be doing.

Both builds report 37 collision meshes in the engine's own table.

## Why the native oracle is built as ILP32

The engine was written for 32-bit Windows and still assumes it in two places that matter:

* `GameState.cpp:1668` allocates `uiCapacity * 4` bytes for a vector of `void*`. On a 64-bit
  target that is half the space the vector needs, and the overflow lands in the engine's own heap
  metadata. This was measured, not inferred: a hardware watchpoint on the corrupted free-list tag
  caught the write in `InsertStatsLeaderboardBuckets`, at `GameState.cpp:1668`.
* `RedMemory.cpp:306`, `:307`, `:381` and `:382` truncate heap pointers to `unsigned int`.

On x86-64 the first of these corrupts the heap during static initialisation, which is what made
the first native oracle spin: the console's group list became a cycle and
`RedCommandConsole::Group::List::Find` (`RedCommandConsole.cpp:38`) walked it forever, on one
thread, at 100 % CPU. The 64-bit build is not a valid oracle for this engine.

`wasm32` and `i386` have the same pointer width, the same `PblList` node sizes and the same
truncation behaviour, so the native oracle is built with `-m32` and the two builds can agree.
The engine's own allocator was checked separately on the 64-bit build: 300 allocations with no
frees, then every other block freed and 50 re-allocations, gave zero duplicate blocks. The
allocator is sound; the engine's 32-bit assumptions are not.

## The two seam decisions this needed

**The file reader answers for a layout the engine asks for.** The engine's `LoadUtil` builds the
path `data\_lvl_pc\<name>` (`LoadUtil.cpp:468`) and the data on disk is a tree
(`tat/tat2.lvl`), while the browser's virtual file system is flat (`tat_tat2.lvl`). The seam
(`browser/platform/pblfile_browser.cpp`) tries the tree form and the flat form, and resolves the
name case-insensitively because the engine asks for `MISSION.lvl` and the file is `mission.lvl`.
Both builds use the same seam, so both see the same bytes.

**A read with no destination is skipped, not obeyed.** The terrain's renderable asks the renderer
for an index buffer (`pcRedIndexBuffer.cpp:41`); the browser's renderer seam answers honestly
that there is no Direct3D9 device, so there is no buffer. The engine then reads the index bytes
into that null buffer (`pcRedIndexBuffer.cpp:199`) and crashes. The seam skips the bytes and
keeps the file position (`fseek` past them), so the file stays in step with the engine's own
expectations. This is the only place where the browser declines to give the engine what it asked
for, and it is recorded rather than hidden.

## What this does not settle

The oracle checks the ground: the terrain interpolation, the collision ray and the fall. It does
not yet check a moving capsule against a hull, a slope, a step or a jump — that is the rest of
plan item 3, and the same harness is where it should be measured.
