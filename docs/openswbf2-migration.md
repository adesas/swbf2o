# Migrating Upstream Decompilation: `vendor/swbf2decomp` → `vendor/openswbf2`

This document records the measured migration of `swbf2-web` from the older `vendor/swbf2decomp`
snapshot (`1,461` files under `src/`) to the newer `vendor/openswbf2` tree (`OpenSWBF2/OpenSWBF2`,
`1,585` non-mimalloc files under `src/`, `135` added files, `11` consolidated/removed headers),
verifying 100% compilation across both `wasm32-unknown-emscripten` and 32-bit native (`i386`)
targets, `0` unresolved symbols on a strict `--whole-archive` link, and exact native-vs-WASM
parity on every simulation oracle.

## Why `vendor/openswbf2` was promoted

`vendor/openswbf2` tracks a newer sync of `Gametoast/swbf2decomp` (`0c1bbd3e`) plus substantial
upstream stability work:

* **Full `RedMemory` / `RedMi` heap management** (`RedEngineFL/Memory/RedMemoryMi.h`) replacing
  raw arena corruption risks with explicit per-heap ownership tracking and clean teardown.
* **Expanded `RedEngineFL/Graphics/PC` & `DisplayLogic`** (`pcDisplayLogic.cpp`,
  `pcDisplaySettings.cpp`, `pcGammaPass.cpp`, `pcWindow.cpp`, `pcFrameLimiter.cpp`, and an
  expanded 8,365-line `pcRedRenderer.cpp` vs 6,592 lines in `swbf2decomp`).
* **Completed inline engine helpers** (`PblFile::SeekAlign`/`Mem*`, `RedAmbientLight` virtuals,
  `RedLight::RedLight(TypeT)`, `RedLight::GetProjectionMatrix()`, `dx::StateManager_DX::FlushState()`,
  `RedMissionLog.cpp`), allowing us to retire several browser-side workarounds.
* **Retail-accurate API fixes**, including `ProcessedInputs::Set(eType, bLocked, fLastValue, fValue)`
  matching the retail x86 parameter order (`FLJoystick.cpp:180`), `D3DXMATRIX : public D3DMATRIX`
  with true `float m[4][4]` layout, and `PblFileChunk::IsDone()` / `CollisionMesh::_HashTable.GetNumEntries()`.

`vendor/SWBF2Phoenix` (`LibSWBF2` + `lua5.0-swbf2-x64`) remains in `vendor/` strictly as a
read-only format reference.

## Measured compilation & link results

| Metric | Previous (`vendor/swbf2decomp`) | Current (`vendor/openswbf2`) |
|---|---|---|
| Source files copied (`prepare_tree.py`) | ~1,461 | `1,574` (`.c/.cpp/.h/.inl`, excl. `mimalloc/`) |
| WASM32 TUs compiled (`tools/compile_core.sh`) | 637 / 638 → ~740 | **772 / 772 (100.0%)** (`build/core.a`, `49,770,312` B) |
| Native i386 TUs compiled (`tools/compile_native.sh`) | ~740 | **772 / 772 (100.0%)** (`build/native_core.a`, `86,434,434` B) |
| Explicitly replaced / skipped TUs | 5 | `8` (see table below) |
| Whole-archive unresolved symbols (`measure_backends.sh`) | 0 | **0** |
| Browser module (`build/browser/swbf2.wasm`) | ~6.4 MB | `6,679,065` B |
| `sizeof(lua_Number)` (`LuaFL/include/luser_number.h`) | 4 (`float`) | **4 (`float`)**, enforced via `static_assert` |

### Skipped / replaced translation units (8 total)

| File in `vendor/openswbf2/src` | Reason excluded | Replacement |
|---|---|---|
| `Battlefront2/Source/pcMain.cpp` | Win32 `WinMain`, message pump, SEH `__try/__except` | `browser/main.cpp`, `browser/platform/startup_browser.cpp`, `pcseam_browser.cpp` |
| `Battlefront2/Source/pcCrash.cpp` | Win32 SEH `MiniDumpWriteDump` + watchdog thread | `browser/platform/pccrash_browser.cpp` |
| `Battlefront2/Platform/Pc/pcPblFile.cpp` | Win32 `CreateFile`/`ReadFile` + overlapped I/O | `browser/platform/pblfile_browser.cpp` |
| `RedEngineFL/Memory/RedMemoryMi.cpp` | Windows `mimalloc` + `HeapWalk`/`VirtualQuery` | `browser/platform/redmemory_mi_browser.cpp` |
| `RedEngineFL/Graphics/PC/pcRedRenderer.cpp` | Direct3D9 device creation / Win32 swapchain (Phase 2 restores orchestration over WebGPU) | `browser/platform/pcRedRenderer_browser.cpp` (transitional) |
| `SoundFL/source/win/snddsbuffer.cpp` | DirectSound buffer implementation (Phase 5 replaces with WebAudio mixer) | `browser/platform/dsound_browser.cpp` |
| `SoundFL/source/win/sndstreamcapturesource.cpp` | DirectSound capture stream | `browser/platform/dsound_browser.cpp` |
| `ZlibFL/adler32.c` | Duplicate `adler32` also defined in `Battlefront2/Source/unzip.cpp` (only skipped by MSVC static-lib linkage; collides under `--whole-archive`) | `Battlefront2/Source/unzip.cpp` |

## Key engineering changes required

### 1. Portable `RedMi` 16-slot heap backend (`browser/platform/redmemory_mi_browser.cpp`)

`vendor/openswbf2` routes `RedMemory.cpp` through `RedMemoryMi.h` (up to 16 heaps, `RedMiCreate`,
`RedMiDestroy`, `RedMiAlloc`, `RedMiFree`, `RedMiRealloc`, `RedMiOwner`, `RedMiUsableSize`,
`RedMiHeapStats`, `RedMiPurge`, `RedMiConfigure`). Upstream's `RedMemoryMi.cpp` embeds `mimalloc`
and calls Win32 `HeapWalk` / `VirtualQuery`.

Our browser/native replacement (`browser/platform/redmemory_mi_browser.cpp`) implements the exact
`RedMemoryMi.h` contract identically on `wasm32` and `i386`:

* Each allocation carries an intrusive header (`magic`, `heap`, `reqSize`, `rawPtr`, doubly-linked
  per-heap list pointers `prev`/`next`).
* A dynamic open-addressing pointer hash table tracks every live `RedMi` allocation in `O(1)`.
  This lets `RedMiOwner(const void* p)` safely return `-1` for arbitrary CRT, stack, or global
  pointers without dereferencing unknown memory (matching `mi_heap_contains_block` semantics).
* `RedMiDestroy(heap)` walks only that heap's intrusive list, frees every block cleanly, and resets
  its stats and table entries.

### 2. True Direct3D9 matrix hierarchy & `.inl` headers

* `vendor/openswbf2` uses `PebbleFL/pc/pcPblMatrix.inl` and `PebbleFL/pc/pcPblVector.inl`, which
  `tools/prepare_tree.py` previously did not copy (`.inl` added to copy filter).
* `PblMatrix` derives from `D3DXMATRIXA16` and indexes `m[row][col]`. `spike/shim/d3dx9math.h` and
  `spike/shim/d3d9.h` were updated to Microsoft's true `struct D3DMATRIX` (`float m[4][4]` unioned
  with `_11.._44`), `struct D3DXMATRIX : public D3DMATRIX`, `struct _D3DXMATRIXA16 : D3DXMATRIX`,
  `D3DXVec3Lerp`, and `D3DXVECTOR4` comparison/arithmetic operators. The obsolete `FreeCamera.cpp`
  flat-index workaround in `prepare_tree.py` was retired.

### 3. Startup & input contract alignment (bugs caught by verification)

Three subtle upstream/shim traps were caught and fixed during verification:

1. **`Startup_RedInitHeap` WASM signature mismatch:** `openswbf2`'s `RedMemory.cpp:902` calls
   `extern "C" void Startup_RedInitHeap(int, int)`, whereas `startup_browser.cpp` and
   `native_startup.cpp` still declared `Startup_RedInitHeap(void)`. Updated both to
   `Startup_RedInitHeap(int, int)` calling `RedMiConfigure(0)` before heap setup.
2. **Uninitialized D3D9 resource out-pointers (`pcRedTexture.cpp:616`):** `RedTexture::pcReadFormat`
   passes an uninitialized local `IDirect3DTexture9* pTexture` to `CreateTexture(..., &pTexture, 0)`
   and stores `m_pTexture = pTexture` before checking `FAILED(hr)`. Stub template methods returning
   `0` (`S_OK`) left stack garbage in `m_pTexture` and `D3DLOCKED_RECT::pBits`, causing a segfault in
   `PblFile::ReadData` on `ground_oracle`. Fixed by zero-initializing `D3DLOCKED_RECT` /
   `D3DSURFACE_DESC` in `spike/shim/d3d9.h` and generating explicit zeroing `Create*` methods
   returning `E_FAIL` on `IDirect3DDevice9` in `tools/shim_autogen.py` (until Phase 2 provides the
   real WebGPU device).
3. **`ProcessedInputs::Set` argument swap (`FLJoystick.cpp:180`):** `openswbf2` corrected the
   decompiled signature from `(eType, bLocked, fValue, fLastValue)` to the retail order
   `(eType, bLocked, fLastValue, fValue)`. Before updating `spike/level_harness.cpp`, the combat
   oracle caught that the player was firing `0` shots while AI droids fired normally (`iTriggerDown = 0`),
   and the input/camera oracle stayed at `(0, -0.02, 0)`. Swapping the arguments in
   `LevelHarnessSetInput` restored full player movement and blaster combat (`7` shots, `7` hits,
   `2` kills, `975` damage dealt).

### 4. Build pipeline speedup & shim cleanliness

* Parallelized `tools/include_pass.py` with `ThreadPoolExecutor(max_workers=8)` and aligned
  `-Wno-address-of-temporary` / `-Wno-incompatible-pointer-types` across `include_pass.py`,
  `compile_core.sh`, `compile_native.sh`, and oracle scripts.
* Added `-I build/src_fixed/LuaFL/src` across all build scripts so `LuaFL` internal headers resolve
  uniformly.
* Added minimal Win32/C shims (`winsock.h`, `ws2tcpip.h`, `psapi.h`, `dbghelp.h`, portable scalar
  `_mm_*_ss` in `xmmintrin.h` with `#include_next <xmmintrin.h>` under `__SSE__`, and `winsock2.h`
  C-visible `gethostbyname` / `inet_ntop`).
* Corrected the stale 8-byte `lua_Number` note in `docs/spawn-oracle.md` and added
  `static_assert(sizeof(lua_Number) == 4)` in `browser/main.cpp`.

## Verification suite results (all passing on `vendor/openswbf2`)

```sh
bash tools/measure_backends.sh
# -> whole-archive link: 0 unresolved symbols

vendor/emsdk/node/24.19.0_64bit/bin/node browser/node_test.mjs
# -> PASS (core/common/ingame/tat2/mission ucfb walks, Lua 5.0.2 scripts, model self-check 0 bad)

env -u NODE_OPTIONS vendor/emsdk/node/24.19.0_64bit/bin/node browser/audio_check.mjs --no-browser
# -> PASS (force_push_fire 0x6aa27e0e, 45,493 frames, exact SHA-256 ba7b1ad54541c5499556de1ca8c7a30a2adaaedff879a2abb43d3e073e726391)

python3 tools/ground_oracle_check.py --skip-build
# -> PASS: 17 samples identical in the native and browser builds (collision meshes 37).

python3 tools/spawn_oracle_check.py --skip-build
# -> PASS: spawned True at (0, 1.2, 0), health 400/400, weapons 4/4, collision bodies 6, updates 60.

python3 tools/input_camera_check.py --skip-build
# -> PASS: 290 driven turns identical, final pos (-13.4076, -0.02, 20.974), camera (-11.2531, 2.55468, 18.7384).

python3 tools/combat_oracle_check.py --skip-build
# -> PASS: 4 targets spawned, player 7 shots, 7 hits, 2 kills, 975 dmg dealt, 180 dmg taken, 26 ordnance, 110 turns identical.
```
