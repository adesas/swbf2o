# Phase 4: Native `GameState` (`ShellState` <-> `MissionState`), OPFS `SaveDevice`, `IDirectInput8A` & Real `.mvs` Bink1 Video Playback

This document records the architecture, root-cause discoveries, and measured headless Chrome + Vulkan WebGPU verification results for Phase 4 of the *Star Wars Battlefront II (2005)* browser port.

---

## 1. Architecture Overview

Phase 4 replaces the single-map test harness with SWBF2's unmodified retail `GameState` state machine (`Battlefront2/Source/GameState.cpp`), driving the complete transition graph inside the Dedicated Worker over WebGPU (`emdawnwebgpu`) and on-demand OPFS streaming (`browser/vfs_opfs.mjs`):

1. **Native `GameState` Boot & Loop (`browser/gpu/wgpu_gamestate_bridge.cpp`):**
   - Exports `swbf2_gamestate_boot`, `swbf2_gamestate_tick`, `swbf2_gamestate_launch_mission`, `swbf2_gamestate_return_to_shell`, `swbf2_gamestate_lua_exec`, and `swbf2_gamestate_status`.
   - Configures retail programmable shaders (`pcForceFixedFunction = false`, `PIPELINE_PROG1`, `GEOMETRYMODE_COMPRESSED`, `SKINNING_HARDWARE`) and movies (`GameMovie::sUseMovies = true`), calls `RedRenderer::pcSetDisplayMode(...)`, and drives `GameState::Update()` at 60 Hz (`ShellState` -> `core.lvl`, `common.lvl`, `shell.lvl`, `shell_interface` Lua UI <-> `MissionState` -> `MISSION.lvl`, full mission Lua scripts, `FLRenderer::Render()`).

2. **OPFS-Backed `SaveDevice` (`browser/platform/savedevice_browser.cpp` + `browser/vfs_opfs.mjs`):**
   - Replaces `Battlefront2/Platform/Pc/pcSaveDevice.cpp` (Win32 `_wfindfirst`/`_wmkdir`/`_wfopen`).
   - Persists profiles and saves (`SaveGames/<name>.<profile|gc|rote>`) synchronously into OPFS `saves.bin` under `/saves/savegames/<name>` via `Module.vfsOpen`/`vfsRead`/`vfsWrite`/`vfsListSaves`, with a POSIX `./SaveGames/` fallback for Node and native oracle runs.

3. **Real Virtual COM `IDirectInput8A` Keyboard & Mouse Bridge (`spike/shim/dinput.h` + `browser/platform/dinput_browser.cpp`):**
   - Replaced `tools/shim_autogen.py`'s non-virtual template `IDirectInput8A` / `IDirectInputDevice8A` placeholders with real virtual COM interfaces in `spike/shim/dinput.h` (matching `FakeDirectInput` / `FakeInputDevice` exact parameter spellings), real DirectInput 8 structs/callbacks, distinct `GUID_SysKeyboard` / `GUID_SysMouse` / `GUID_Joystick` GUIDs, and exact Microsoft `DIK_*` scancodes (`0x01..0xED`).
   - `BrowserDirectInput8` / `BrowserInputDevice` subclass `FakeDirectInput` / `FakeInputDevice` (allocated via `malloc`/`free` outside `GameMemory::RunTimeHeap`) and dispatch `GetDeviceState(cbData, lpvData)` by `cbData` (`256` bytes = keyboard `DIK_*` scancode table; `16`/`20` bytes = `DIMOUSESTATE`/`DIMOUSESTATE2` relative deltas + buttons).
   - Fixed `ToUnicode` / `ToAscii` and `SystemParametersInfoA` in `spike/shim/swbf2_prefix.h` so `RedKeyboard::pcGetAsciiValue(DIK_RETURN)` emits `'\r'` (`13`) instead of raw scancode `28` and `RedKeyboard::Update()` initializes valid repeat delays.
   - Exports `swbf2_input_set_key`, `swbf2_input_set_mouse_delta`, `swbf2_input_set_cursor`, `swbf2_input_seam_counters`, and `swbf2_input_clear_all`.

4. **OPFS Streaming for `SoundFL` & `RedMoviePlayer` (`browser/platform/sndfile_browser.cpp` + `SoundFL/source/sndfile.cpp`):**
   - Replaces `SoundFL/source/win/sndfileplatform.cpp` (`CreateFileA`/`ReadFile`/`SetFilePointer`) with a `PblFile`-backed `Snd::FilePlatform`, streaming `.mvs` movies (`movies/shell.mvs`, `crawl.mvs`, `ingame.mvs`, `training.mvs`) and `.lvl` audio banks (`sound/shell.lvl`, `sound/global.lvl`, `sound/tat.lvl`, `sound/spa.lvl`) on demand from OPFS in 256 KiB sparse blocks.
   - Patched `Snd::File::Read` (`SoundFL/source/sndfile.cpp`) to call `CompleteFileOp()` synchronously (matching `Snd::File::Open`), since single-threaded WASM has no background Win32 `FileThread`.

5. **Real `.mvs` Bink1 (`BIKf`..`BIKi`) Video Decoder (`browser/platform/bink_browser.cpp` + `browser/platform/binkdec/`):**
   - Vendored a self-contained C++17 adaptation of `libbinkdec` (LGPL-2.1, compiled into `build/binkdec.a`) implementing the RAD Bink SDK C ABI (`BinkSetIO`, `BinkOpen`, `BinkClose`, `BinkWait`, `BinkDoFrame`, `BinkNextFrame`, `BinkCopyToBuffer`, `BinkDX9SurfaceType`, `BinkSetVolume`).
   - Reads raw `BIKi` streams directly from `RedMoviePlayer`'s `BINKIO` ring buffer (`BinkFileReadHeader` / `BinkFileReadFrame`) in `<= 16 KiB` chunks so frames larger than half of `RedMovie`'s 256 KiB circular buffer (e.g. `shell.mvs` frame 0 at `123,848` bytes) never deadlock `BinkFileReadFrame`.
   - Converts decoded YUV420p planes (`Y`, `Cb`, `Cr` + optional alpha) via integer BT.601 math directly into the locked `D3DFMT_X8R8G8B8` (`BGRA8`) `RedTexture` surface uploaded to WebGPU and composited behind SWBF2's Main Menu UI by `RedMoviePlayerRenderer`.

---

## 2. Engine & Memory Bugs Found and Fixed (`tools/prepare_tree.py`)

Bringing up full `ShellState <-> MissionState` transitions exposed five subtle engine/decompiler bugs when running under 32-bit Clang/WASM:

| # | File | Root Cause | Fix Recorded in `tools/prepare_tree.py` |
|---|---|---|---|
| 1 | `SoundFL/source/sndenginebase.cpp` | `Snd::EngineBase::Open()` looped `uiOffset < 0xA800` for `Voice` and `uiOffset < 0x1446A68` for `Stream`. Under Clang `sizeof(Voice) == 1308` and `sizeof(Stream) == 3,543,420` (slightly smaller than MSVC), causing an extra 33rd/7th placement-new that overflowed `smStreamStorage` in `.bss` by **3.54 MB** and wiped out `smVoiceVirtualManager`. | Changed loop bounds to `uiOffset + sizeof(Voice) <= 0xA800` and `uiOffset + sizeof(Stream) <= 0x1446A68`. |
| 2 | `RedEngineFL/Memory/RedMemory.cpp` + `browser/gpu/*` | `emdawnwebgpu`'s C++ `RefCounted` wrappers and `libbinkdec` containers call global `::operator new`, which `RedMemory.cpp` routed into `GameMemory::RunTimeHeap`. When `ShellState::Exit()` or `MissionState::Exit()` destroyed `RunTimeHeap`, live WebGPU objects (`wgpu::Sampler`, `wgpu::BindGroupLayout`, `WgpuTexture2D::mipViews`/`locks`) were freed underneath Dawn, triggering `Assertion failed: oldRefCount >= 1` in `wgpuSamplerAddRef`. | Added sentinel heap ID `-2` in `RedMemory.cpp::operator new`/`new[]` routing to system `malloc` (`RedFreeToHeap` already routes non-RedMi pointers to `free`), wrapped WebGPU & Bink decoder creation sites in RAII `WgpuSysHeapGuard`/`BinkSysHeapGuard`, and switched `WgpuTexture2D` vectors to `SysAllocator`. |
| 3 | `RedEngineFL/movie/pc/pcRedMoviePlayer.cpp` | `RedMoviePlayer::Update()` called `CreateBinkDecoder()` (which dereferences `mBink->Width`) unconditionally even when `BinkOpen()` returned `NULL`. | Guarded `if (mBink) { CreateBinkDecoder(); }`. |
| 4 | `RedEngineFL/Graphics/PC/pcRedShader.cpp` | `pcPerPixelDiffuseLightingShader::GetState_Prog1` can return state indices `18..23` when a material combines `materialFlags & 0x10000000` (`+12`) and `bumpMapHash` (`+6`), whereas `core.lvl`'s `perpixeldiffuselighting_shader.xml` defines `m_uiNumStates = 18` for `PIPELINE_PROG1`. Additionally, `pcRedShaderDef::Reset()` did not zero `m_uiPixelShaderPoolSize` / `m_uiVertexShaderPoolSize`. | Added bounds/null guard in `pcRedShaderDef::LoadVertexAndPixelShader` and zeroed both shader pool sizes in `pcRedShaderDef::Reset()`. |
| 5 | `Battlefront2/Source/PlayerProfile.cpp` | `PlayerProfile::tGameOpt::ApplySettings()` wrote keyboard bindings, flip, and sensitivity fields into `RawControllerInputs` using hardcoded MSVC x86 byte offsets (`pRaw + 0x20bc`, `pRaw[0x000c]`, `pRaw[0x2638/0x263d/0x2642]`). On MSVC x86 a polymorphic class containing a `double` inserts 4 bytes of padding after the vptr (`mCurKeyboardBindings` at `0x20bc`); under Clang/Itanium C++ ABI (`wasm32` and Linux `i386`) `mCurKeyboardBindings` starts at `0x20b8`, so `iMode = 4` overflowed 4 bytes into `m_pGuiInputs`, truncating its low 16 bits to `0x0000` (`0x005a5xxx -> 0x005a0000`). Every subsequent `RawControllerInputs::ReadLatest()` then `memset`-zeroed `0x005a0000` (silently corrupting whatever `.bss` global landed there — previously `FLEffect::s_EffectFactories` / `s_EffectClasses`, which caused spurious `FLEffect::CleanupAll()` crashes!) and dropped all GUI mouse/button state in `MissionState`. | Replaced all hardcoded `pRaw` byte-offset writes with the real C++ member names (`pJoystick->mCurKeyboardBindings[iMode]`, `mFlip`, `mJoystickSensitivityX/Y`, `mMouseAssist`). Pristine `FLEffect::CleanupAll()` now runs unmodified with zero workarounds. |

Additionally, three critical platform/shim bugs were found and fixed outside `prepare_tree.py`:

- **Real Virtual COM `IDirectInput8A` / `IDirectInputDevice8A` + Real `DIK_*` Scancodes (`spike/shim/dinput.h`, `spike/shim/generated_types.h`, `tools/shim_autogen.py`):** `generated_types.h` previously synthesized `IDirectInput8A` / `IDirectInputDevice8A` as non-virtual variadic templates (silently hiding `BrowserDirectInput8` overrides) AND assigned `DIK_* = 4149..4292` (causing `RedKeyboard::StaticInit()` to overflow 256-byte `sAsciiCodeMap`/`sRedKeyMap` arrays by 8 KiB in `.bss`). Replaced with real virtual COM interfaces and exact Microsoft `0x01..0xED` scancodes.
- **OPFS `SaveDevice` Prefix & Case-Insensitive Lookup (`browser/vfs_opfs.mjs`, `browser/platform/savedevice_browser.cpp`):** Normalized `OpfsVfs.listSaves()` prefixes identically to `_resolve()` (stripping leading `/` and `saves/`), switched `SaveDevice::FindFileInList()` to case-insensitive UTF-16 comparison (`L"Player 1"` vs lowercased OPFS key `"player 1"`), and changed `SAVE_DIRECTORY`/`SFTPrefix` literals from 32-bit `L"..."` to 16-bit UTF-16 `u"..."`.
- **Ephemeral CDP Port in `browser/phase4_gamestate_check.mjs`:** Derived `CDP_PORT` from `process.pid` so stale headless Chrome processes on fixed port `9251` can never hijack a check run.

> **Note on `movies/*.mvs`:** Our `libbinkdec` integration decodes unmodified retail Steam, GOG, and DVD PC `.mvs` Bink1 (`BIKi`) menu flythroughs and campaign cutscenes directly into rotating WebGPU textures.

---

## 3. Headless Chrome + Hardware Vulkan Verification (`browser/phase4_gamestate_check.mjs`)

Executed via `./vendor/emsdk/node/24.19.0_64bit/bin/node browser/phase4_gamestate_check.mjs` on NVIDIA Pascal Vulkan (`hasTextureCompressionBC: true`, `pipeline = 1 (PROG1)`, `geometryMode = 1 (COMPRESSED)`, `skinningMode = 1 (HARDWARE)`):

- **Result: `29 / 29 PASS`, `0` WebGPU errors (`8.48 s` total wall time).** All five screenshots were visually inspected:
  1. **`ShellState` Boot (`ifs_login`):** Booted `core.lvl` + `common.lvl` + `shell.lvl` + `shell_interface` in `627 ms` (`28` draws, `278` tris, `82.05%` non-black, `movieOpen = true`). Screenshot shows the retail *Profile Management* screen (`Player 1`, `Select Difficulty: Normal`, `Accept`, `Star Wars Battlefront II v. 1.1`).
  2. **DirectInput8 Profile Login & OPFS `SaveDevice` Persistence (VERIFIED):** A single DirectInput8 `DIK_RETURN` on `ifs_login` (`diCreations = 1`, `diDevices = 2`, `giEq = 1`) natively created `savegames/player 1.profile` via `ifs_saveop -> SaveProfile`, persisted it into OPFS across a fresh second-worker reload (`persistedProfilesAfterReload = ["savegames/player 1.profile"]`), and transitioned naturally (zero Lua screen bypass) into the PC main menu (`ifs_sp_campaign`, `Profile Name: Player 1`).
  3. **Main Menu (`ifs_sp_campaign`) + Live Animated `.mvs` Bink1 Playback:** `libbinkdec` opened `shell.mvs` segment 0 (`800x600`, `2,576` frames @ `30/1 fps`), decoded `9` frames into rotating `D3DFMT_X8R8G8B8` WebGPU textures (`draws = 64`, `nonBlackPct = 86.79%`), with **`4.91%`** pixel delta measured between early and late menu frames proving live Bink1 animation behind the retail UI.
  4. **Instant Action — Ground Conquest (`tat2g_con`):** `ShellState -> MissionState` (`2,534 ms`), clicked Team 1 (`Empire`) on `ifs_sideselect1` -> `ifs_pc_SpawnSelect` -> clicked `SPAWN` via real DirectInput8 mouse, spawned into live first-person gameplay (`playerAlive = true`, `currentScreen = ""`, `491` draws, `67,437` tris, `2` hardware-skinned draws / `3,016` skinned tris, `0` WebGPU errors). Screenshot shows 1st-person E-11 Blaster Rifle, Thermal Detonators, 150 vs 150 tickets, minimap, crosshair, FX-7 medical droid, and Mos Eisley geometry.
  5. **Round-trip #1 -> Hero Assault (`tat2g_eli`):** Clean `MissionState::Exit()` -> `ShellState::Enter()` (`300 ms`, back to `ifs_sp_campaign`) -> `tat2g_eli` (`1,914 ms`), clicked Team 1 + `SPAWN` via DirectInput8 mouse (`playerAlive = true`, `currentScreen = ""`, `307` draws, `47,913` tris, `5` skinned draws / `5,406` skinned tris, `0` WebGPU errors). Screenshot shows Han Solo holding his DL-44 Blaster + Detpacks beside Mace Windu with an ignited glowing purple lightsaber and `0/180` Hero Assault HUD.
  6. **Round-trip #2 -> Space Assault (`spa1g_ass`):** Clean round-trip -> `spa1g_ass` (`1,666 ms`), clicked Team 1 + `SPAWN` via DirectInput8 mouse (`playerAlive = true`, `currentScreen = ""`, `213` draws, `86,643` tris, `23` skinned draws / `32,141` skinned tris, `0` WebGPU errors). Screenshot shows the Imperial Star Destroyer hangar with Commando Pistol, Time Bombs, TIE Bomber, walking Imperial Pilot, and Capital Ship Systems HUD.

### Regression Suite (All 6 Checks Green)

| Suite | Result |
|---|---|
| `browser/phase4_gamestate_check.mjs` | **29 / 29 PASS** (`0` WebGPU errors) |
| `browser/webgpu_render_check.mjs` | **25 / 25 PASS** (`2,332/2,332` VS + `226/226` PS, `trooperDiffPct = 4.81%`, `0` WebGPU errors) |
| `tools/ground_oracle_check.py` | **PASS** (17 samples bit-identical, native ILP32 vs WASM) |
| `tools/spawn_oracle_check.py` | **PASS** (bit-identical) |
| `tools/input_camera_check.py` | **PASS** (290 driven turns bit-identical) |
| `tools/combat_oracle_check.py` | **PASS** (110 turns, 7 hits, 2 kills bit-identical) |

---

## 4. Remaining Items Deferred to Phases 5–7

1. **Interactive DOM -> Worker Input & Full 3D WebAudio (`Phase 5`):** Phase 4 drives `BrowserDirectInput8` from the worker harness (`swbf2_input_set_key` / `swbf2_input_set_cursor`) with `g_bNoSound = true`. Phase 5 wires live browser Keyboard, PointerLock/Absolute Mouse, and Gamepad events through a `SharedArrayBuffer` ring plus full 3D `SoundFL` -> WebAudio AudioWorklet mixing (including Bink movie audio tracks).
2. **HDR Bloom Over-Exposure Polish (`Phase 7`):** Live gameplay screenshots on bright outdoor Tatooine (`tat2g_con`, `tat2g_eli`) and the Star Destroyer hangar viewport (`spa1g_ass`) show `HDREffect` bloom washing out bright sky/ground areas; tune the D3D9 render-target bloom blend / `GlowThreshold` path in `browser/gpu/d3d9_device_wgpu.cpp` and `dx9_sm1_wgsl.cpp`.
3. **Bink `.mvs` Audio & Pacing:** Video decoding, texture upload, and live frame animation are verified (`800x600`, `4.91%` inter-frame diff), while Bink audio playback awaits Phase 5's WebAudio mixer.
5. **Flat black panel over the ground in live missions - diagnosed, not fixed (`Phase 5`):** see
   `docs/render-black-panel.md`. Measured, not guessed: a single 50-triangle, 16-byte-stride blended
   draw with packed texture coordinates repaints the panel; hiding that one draw takes the region from
   mean luminance 1.3 to 765 (the ground, correctly lit, underneath). It is not a hole - a forced
   `D3DCLEAR_TARGET` colour does not touch it - and not a missing texture - unbound stages bind the 1x1
   white fallback, and the culprit has textures bound at every active stage. The UV overlay shows one
   coordinate spanning the full range and the other pinned at exactly 16/255, i.e. the surface samples a
   thin black band of its page because one UV component never arrives. The candidate cause is
   `MapD3DDeclTypeToVtxFmt` in `browser/gpu/d3d9_resources_wgpu.cpp`: it maps D3D9 declaration types
   0..7 only and its `default:` returns `WGPU_VTX_NONE`, which the caller turns into `continue` - so
   `USHORT2N`, `SHORT2N`, `FLOAT16_2` and friends are dropped silently, leaving the varying at its
   default and `minStride` short. The doc records the fix and the instruments (`clear=`/`ps=`/`skip=`/
   `record=` page queries, `tools/black_panel_bisect.py`).
5b. **Latent out-of-bounds write, victim depends on memory layout:** two build perturbations crashed
   the engine in unrelated places - a fragment-shader overlay change crashed `luaC_collectgarbage`
   during mission script load, and per-draw recording that grew a `std::vector` (so, the shared WASM
   heap) crashed on a null indirect call in `FirstPersonRenderable::RenderSoldier`. Moving the
   recorder to fixed static storage removed the crash; the untouched build is green repeatedly. Same
   family as the hardcoded `pRaw + 0x20bc` MSVC offset fixed in `8d6f866`. Next step: run the Phase 4
   check against an `-fsanitize=address` core build and catch the write.
6. **Native Oracle & Harness Maintenance (Done):** `tools/native_binkdec.sh` and `tools/build_binkdec_wasm.sh` build matching host/wasm32 `libbinkdec` archives; both browser checks reap `tools/serve.py` and Chrome via `bail()` on failure; `tools/compile_native.sh` builds all `771 / 771` TUs cleanly against the updated shims and `PlayerProfile.cpp` fix.

---

## 5. Historical Note: Resolution of the DirectInput8 & `PlayerProfile` Root Causes

Previously (`37c27b5`), Phase 4 stood at `23 / 25` because `swbf2_gamestate_gui_probe()` showed `diCreations: 1`, `diDevices: 0`, `diKeyboardStates: 0`: `tools/shim_autogen.py` had generated non-virtual variadic templates for `IDirectInput8A` / `IDirectInputDevice8A` that silently hid `BrowserDirectInput8` overrides.

Resolving that uncovered two deeper bugs that are now fixed and verified:
1. `spike/shim/generated_types.h` assigned `DIK_* = 4149..4292`, overflowing `RedKeyboard::StaticInit()`'s 256-byte `sAsciiCodeMap`/`sRedKeyMap` arrays by 8 KiB in `.bss`, and `swbf2_prefix.h`'s `ToUnicode` returned raw scancode `28` instead of `'\r'` (`13`). Defining the real COM interfaces and Microsoft `0x01..0xED` scancodes in `spike/shim/dinput.h` + fixing `ToUnicode` / `SystemParametersInfoA` + normalizing OPFS `listSaves()` / case-insensitive `FindFileInList()` made native DirectInput8 login and OPFS `.profile` persistence work end-to-end.
2. Once login succeeded and `PlayerProfile::tGameOpt::ApplySettings()` ran on tick 2, its hardcoded MSVC `+0x20bc` byte-offset write into `RawControllerInputs` overflowed by 4 bytes under Clang's Itanium ABI into `m_pGuiInputs`, corrupting `0x005a5xxx -> 0x005a0000`. That simultaneously caused `RawControllerInputs::ReadLatest()` to `memset`-zero `0x005a0000` (destroying `FLEffect::s_EffectFactories` in `.bss`) and broke all GUI mouse/button dispatch in `MissionState`. Replacing the raw byte offsets with real C++ member accesses fixed both symptoms completely and brought `browser/phase4_gamestate_check.mjs` to **`29 / 29 PASS`**.
