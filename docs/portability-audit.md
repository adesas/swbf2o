# M0 portability audit — OpenSWBF2/swbf2decomp → Emscripten

Audited at `vendor/swbf2decomp` (shallow clone, commit as of 2026-11-15). Method: include/usage grep counts +
one real clang-for-wasm compile test + one real Emscripten build (below).

## Verdict

The tree is **1,452 `.cpp`/`.h` files of plain C++ with narrow platform seams**. It is not "a Windows program" — it is
an engine with a platform layer, and that layer is isolated in a handful of directories/files.

## Full-tree compile audit (actual numbers, 2026-11-15)

`tools/compile_audit.sh` compiled all 1,452 files with clang-for-wasm using a minimal Win32 shim. Full output in
`docs/compile-audit.txt`. With `-fms-extensions` (which is what makes `unsigned __int64` work) the pass rate is:

| module | pass | fail | dominant cause |
|---|---:|---:|---|
| `ZlibFL` | 9/9 | 0 | — |
| `PebbleFL` | 16/22 | 6 | needs `d3dx9math.h` shim, `LARGE_INTEGER.QuadPart`, `QueryPerformanceFrequency`, MSVC `_snprintf`/`_vsnprintf` |
| `Zephyr` | 7/12 | 5 | incomplete type `ZephyrSkeletonShared` (decomp artifact), `d3dx9math.h` |
| `SoundFL` | 25/80 | 55 | `mmreg.h`/`dsound.h`/`objbase.h`/`mediaobj.h`, `operator new(size_t, void*)`, bad `static_cast` (decomp artifact) |
| `RedEngineFL` | 20/117 | 97 | `d3d9.h` (62 files), `d3dx9math.h`, `dinput.h`, `unknwn.h`, custom `operator new` |
| `Battlefront2` | 56/466 | 410 | `d3d9.h`/`d3dx9math.h` + decomp artifacts: enum forward references (164), `static_cast` between unrelated types (356), custom `operator new` (1,954) |

Reading this honestly: **the failures are two separate jobs, and both are bounded.**
1. A fake-header/shim layer (`d3d9.h`, `d3dx9math.h`, `dinput.h`, `unknwn.h`, `mmreg.h`, `dsound.h`, `winsock2.h`,
   `io.h`, `direct.h`, `_snprintf`, `QuadPart`, `QueryPerformanceFrequency`, `strlcpy`, `operator new(size, pool)`) —
   mechanical, and it unblocks roughly a third of the failures.
2. A decomp-quality pass on ~500 files (enum forward refs, casts the decompiler could not reconstruct).
   `-fms-extensions` and `-Wno-ignored-attributes` handle the calling conventions.

Test compile of the MSVC-isms on `-target wasm32-unknown-emscripten`:

```
__thiscall  -> warning: calling convention not supported for this target (ignored, compiles)
__stdcall   -> warning (ignored, compiles)
#pragma pack-> fine
```

So calling-convention annotations (`__cdecl` 688, `__stdcall` 373, `__fastcall` 301, `__thiscall` 31 occurrences) are
**noise, not blockers**: correctness does not depend on them once you are no longer trying to reproduce the original
binary byte-for-byte. They will need a mechanical cleanup pass or `-Wno-ignored-attributes`.

## Seam map (what to replace, what to keep)

| Module | .cpp/.h | Coupling | Action |
|---|---:|---|---|
| `src/Battlefront2/Source` | 893 | gameplay/AI/net logic; D3D is only math+type usage (1,866 token hits, **11** real device-call files) | **keep as-is** |
| `src/Battlefront2/Source` netcode | 11 | `NetComm`, `NetPacket`, `NetGame`, `NetMatch`, `NetPredict`, `NetQOS`, `NetAdmin`, `LuaCallbacks_Net` | **keep**; swap transport only |
| `src/Battlefront2/Source/pcNetSocket.cpp` | 1 | the only file doing `socket()`/`sendto`/`recvfrom` | **swap to WebSocket/WebRTC adapter** |
| `src/RedEngineFL/Graphics` | engine-side | `RedModel`, `RedMaterial`, `RedShadingState`, `RedCamera`, `RedLight`, `RedSkeleton`, `RedPose`, `RedShadowRegion` | **keep** |
| `src/RedEngineFL/Graphics/PC` | 15+ | `pcRedRenderer`, `pcRedPixelShader`, `pcRedIndexBuffer`, `pcRedPrimitive`, `StateManager_DX` | **replace with a WebGPU/WebGL backend** (the one genuinely new renderer) |
| `src/Battlefront2/Platform/Pc` | 26 | post-processing effects (`pcSunFlare`, `pcScopeBlurEffect`, `pcHDREffect`, `pcLightBeam`, `pcFishEffect`), `pcSaveDevice`, `pcFLJoystick`, `pcPblFile` | reimplement or stub; effects can be deferred |
| `src/PebbleFL/Common` | 51 | math/containers + `PblFile` file abstraction (780 references) | **keep**, back `PblFile` with a WASM virtual filesystem |
| `src/LuaFL` | 49 | stock **Lua 5.0.2**, only coupling = 2 allocator functions | **compiles today** (see spike) |
| `src/Zephyr` | 26 | scripting + animation banks (`ZephyrAnim*`, `ZephyrAnimBank*`) | keep |
| `src/SoundFL` | 167 | FSound `.fs`; 1 SSE-intrinsic file | keep parsing, replace the mixer/output with WebAudio |
| `src/RedEngineFL/Memory` | — | `MemoryPool` used by **178 files** | one shim: arena allocator on the WASM heap |
| `src/Steam`, `src/lglcd` | 9 | optional | stub out |

Central shims to write (all small): `winshim.h` (a few typedefs: `DWORD`, `HWND`, `LARGE_INTEGER`,
`QueryPerformanceCounter`, `GetTickCount`), `d3dx_math.h` (`D3DXVec3*` 774 uses, `D3DXMatrixMultiply` 337,
`D3DXQuaternionNormalize`, `D3DXMatrixRotation*`), `MemoryPool`, `RedWarning`, `PblFile` backend.

No inline asm, no threads, 9 files with `__declspec`. The `.package`/`.zone`/`.model`/`.anim` reading is already the
game's own chunk reader (`PblChunk`, `CheckChunkId`, `GamePath`, `_lvl_pc`) — a WASM build can read the original
game data directly, no asset converter required.

## Spike result (done, works)

`spike/lua_wasm` builds the decompiled LuaFL to WASM with **two** tiny shims and runs a mission-style script:

```
$ ./spike/build.sh
built OK -> spike/lua_wasm/lua_wasm.js
$ node spike/lua_wasm/lua_wasm.js
compiled Lua version: Lua 5.0.2
[game-lua] spawning sp_cl_pi hp=100
ok: lua VM ran the mission-style script in wasm
```

Two shims (`spike/shim/Battlefront2/Source/LuaMemory.h`, `spike/shim/RedEngineFL/Util/RedWarning.h`) were enough.
That is the proof of the whole approach: the decompiled source is portable, the game logic needs no rewrite.

## Unknowns / next things to measure

1. Does the tree compile as one unit? Expect the first real wall at `RedEngineFL/Graphics/PC` + `winshim`.
2. `MemoryPool` semantics (fixed pools with `MemPoolInfo` counts) — need behaviour-compatible arena or must be
   preserved for Lua-memory accounting (`LuaMemory::CreatePools(bMission)` suggests mission-vs-shell pools).
3. Float determinism between WASM client and native server: assume *not* identical → keep the server authoritative.
4. The upstream README warns the decomp is "not byte-accurate... particularly when playing online" — so
   netcode behaviour may have latent mismatches; the headless server build is how we find them.
5. The game's Lua scripts are pre-compiled by `LuaCompiler` in this repo — decide whether to keep that pipeline or
   run `.lua` source directly.

## Suggested next milestone

M1 headless: get `Zephyr + Battlefront2 + PebbleFL + ZlibFL + LuaFL` compiling to WASM with a no-op renderer and a
virtual filesystem, load one `.zone` + mission Lua + unit tables, run the sim at 30 Hz. That single build is both the
game server and the client core — everything else (WebGPU backend, WebAudio, input) plugs into seams already listed.

---

## STATUS (superseding the numbers in this file)

The 127/636 and 143/636 figures above are obsolete and were measured against a source tree that an
earlier automated "forward declaration → include" rewrite had already damaged. Rebuilt from the
pristine decompilation, the core scope now compiles **637 / 638** translation units to
wasm32-unknown-emscripten; `build/core.a` holds 637 objects.

The premise that motivated the follow-up work ("the top ~10 class definitions are missing") turned
out to be false: `RedPose`, `Entity`, `ZephyrAnim`, `RedLight`, `ZephyrSkeletonShared`, `Trackable`,
`FLEffectClass`, `Transport` and `Factory` are all defined in the decomp tree. The blockers were
MSVC-isms clang rejects, the vocabulary of the platform APIs the browser build replaces, genuine
decompilation defects, and include cascades. Read `docs/shim-layer-results.md` for the method and the
honest failure log, and `docs/backend-surface.md` for the 750 symbols that still stand between
`core.a` and a linked module. `docs/BUILD.md` reproduces the build.

M1 (headless core) is now a linking-and-adapters problem, not a compiling problem: only
`Battlefront2/Source/NetGame.cpp` fails, and it fails on a missing `BblList<NetEntity, 512>` class
specialization that has to be reconstructed from the binary.
