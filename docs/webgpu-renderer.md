# Phase 2: Real Engine Direct3D 9 → WebGPU Renderer (`RedRenderer` + `FLRenderer`)

This document records the architecture, implementation details, debugging findings, and measured
headless Chrome verification of the Phase 2 real engine WebGPU renderer in `swbf2-web`.

---

## 1. Architecture Overview

Instead of writing a separate scene viewer on the side, Phase 2 compiles and runs the retail
*Star Wars Battlefront II (2005)* renderer directly:

* **`RedEngineFL/Graphics/PC/pcRedRenderer.cpp` (`8,365` lines) compiled into `build/core.a`:**
  Brings total engine compilation from `772 / 772` to **`773 / 773` translation units (`100.0%`)**.
  This links the engine's actual `RedRenderer`, `StateManager_DX`, opaque & transparent sort heaps,
  render-state undo stacks, `internalRenderFixed` / `internalRenderProg1`, matrix & lighting caches,
  and all 25 retail shader classes (`pcTerrainShader`, `pcNormalShader`, `pcSkyShader`,
  `pcInterfaceShader`, `pcParticleShader`, `pcWaterShader`, `pcShadowShader`, etc.), plus
  `RedTexture`, `RedModel`, `RedSegment`, `RedPrimBuffer`, `RedFont`, `RedTextElement`,
  `RedInterfaceScreen`, `RedTerrain`, `SkyManager`, and `FLRenderer`.
* **Real Virtual COM Direct3D 9 ABI (`spike/shim/d3d9.h`):**
  Replaces the Phase 0/1 template no-op D3D9 stubs with real virtual COM interfaces (`IDirect3D9`,
  `IDirect3DDevice9`, `IDirect3DResource9`, `IDirect3DSurface9`, `IDirect3DBaseTexture9`,
  `IDirect3DTexture9`, `IDirect3DCubeTexture9`, `IDirect3DVolumeTexture9`, `IDirect3DVolume9`,
  `IDirect3DVertexBuffer9`, `IDirect3DIndexBuffer9`, `IDirect3DVertexDeclaration9`,
  `IDirect3DVertexShader9`, `IDirect3DPixelShader9`, `IDirect3DStateBlock9`, `IDirect3DSwapChain9`,
  `IDirect3DQuery9`) and authentic Microsoft Direct3D 9 enum/flag constants (`D3DCLEAR_*`,
  `D3DFMT_*`, `D3DRS_*`, `D3DTSS_*`, `D3DTOP_*`, `D3DTA_*`, `D3DSAMP_*`, `D3DLIGHT_*`, `D3DCULL_*`,
  `D3DCMP_*`, `D3DBLEND_*`, `D3DPT_*`, `D3DFVF_*`, `D3DDECLUSAGE_*`, `D3DDECLTYPE_*`).
* **Dual-Mode Compatibility (`g_bNoRender` vs WebGPU):**
  * When `g_bNoRender == true` (headless Node tests and native 32-bit `i386` oracles),
    `pcRedRenderer.cpp` uses its own internal `FakeD3D` / `FakeD3DDevice` subclasses (and a weak
    `Direct3DCreate9` fallback in `browser/platform/pcRedRenderer_browser.cpp`).
  * When `g_bNoRender == false` (browser WebGPU worker), `RedRenderer::InitStatic()` calls our
    strong `Direct3DCreate9()` in `browser/gpu/d3d9_device_wgpu.cpp`, which returns `WgpuD3D9` and
    creates `WgpuD3D9Device` backed by `emdawnwebgpu`.

---

## 2. Source Layout (`browser/gpu/`)

| File | Responsibility |
|---|---|
| `browser/gpu/d3d9_wgpu_internal.h` | Shared internal structures: `SysAllocator<T>` / `ByteVec` / `U32Vec`, `WgpuDynRing`, `FfpDrawUniforms` (`2,816` B = `11 × 256` B, including `vsConst[96][4]` and `psConst[8][4]`), `FfpVertexLayout`, `Sm1VsDclInput`, `FfpPipelineKey`, `WgpuGpuContext`, D3D9 COM resource classes, and C-linkage entry points (`swbf2_wgpu_*`, `swbf2_engine_render_*`). |
| `browser/gpu/d3d9_resources_wgpu.cpp` | WebGPU-backed D3D9 resources: `WgpuSurface`, `WgpuTexture2D`, `WgpuCubeTexture` (`TextureViewDimension::Cube`), `WgpuVolumeTexture` (`TextureDimension::e3D`), `WgpuVertexBuffer`, `WgpuIndexBuffer`, `WgpuVertexDecl`, `WgpuVertexShader`, `WgpuPixelShader`, `WgpuQuery`, plus hardware `BC1/BC2/BC3` (`BC1RGBAUnorm`, `BC2RGBAUnorm`, `BC3RGBAUnorm`) upload and software `DXT1/DXT3/DXT5` + legacy `A8R8G8B8`/`X8R8G8B8`/`R8G8B8`/`R5G6B5`/`A1R5G5B5`/`A4R4G4B4`/`A8`/`L8`/`A8L8`/`V8U8` → `RGBA8Unorm` decoding. |
| `browser/gpu/d3d9_ffp_wgsl.cpp` | Deterministic D3D9 Fixed-Function Pipeline (FFP) WGSL shader generator keyed by vertex declaration (`FfpVertexLayout`), plus fullscreen blit WGSL. Supports untransformed `XYZ` and pre-transformed `XYZRHW` screen-space vertices, `FLOAT1..4`/`SHORT2`/`SHORT4`/`UBYTE4`/`D3DCOLOR` attributes, up to 4 active D3D9 lights (directional/point/spot) + material/vertex color sources + ambient, 4-stage `D3DTSS_TCI_*` texcoord generation & matrix transforms (`D3DTTFF_*`), 4-stage color/alpha texture combiners (`D3DTOP_*`, `D3DTA_*`), alpha test (`D3DRS_ALPHAFUNC`/`ALPHAREF`), and linear vertex fog. |
| `browser/gpu/dx9_sm1_wgsl.cpp` | Phase 3 runtime DirectX 9 Shader Model 1.1 (`vs_1_1` / `ps_1_1`) and `ps_2_0` (`kGammaPassPS`) bytecode → WGSL translator (`GenerateCombinedWGSL`). Translates all `2,332 vs_1_1` and `226 ps_1_1` shaders in `core.lvl` with `0` Dawn/Tint errors, bridging compressed/uncompressed `IDirect3DVertexDeclaration9` streams via a 64-bit `vsInputTypeKey` and supporting `mova`/`c[N + a0.x]` hardware bone skinning, 2D/Cube/3D texture sampling, `texbem`, `texm3x2*`, and `texm3x3*`. See `docs/sm1-wgsl-translator.md`. |
| `browser/gpu/d3d9_device_wgpu.cpp` | `WgpuD3D9`, `WgpuD3D9Device`, `WgpuStateBlock`, async `swbf2_wgpu_init` inside the Dedicated Worker against `OffscreenCanvas` (`!canvas`), lazy `wgpu::RenderPassEncoder` lifecycle, pipeline/sampler/bind-group caches, per-frame streaming ring uploads (`16 MiB` vertex, `4 MiB` index, `8 MiB` dynamic uniform), `Clear`, `ColorFill`, `StretchRect`, `DrawPrimitive`/`DrawIndexedPrimitive`/`DrawPrimitiveUP`/`DrawIndexedPrimitiveUP` (with `D3DPT_TRIANGLEFAN` expansion), `Present` blit to swapchain, and async GPU backbuffer readback (`swbf2_wgpu_readback_frame`). |
| `browser/gpu/wgpu_engine_bridge.cpp` | Real engine render bridge driving retail `RedRenderer::InitStatic()`, `RedRenderer::Init()`, `RedTexture::Init()`, `RedFont::Init()`, `SkyManager::Init()`, `FLRenderer::Init()`, streaming `core.lvl`, `common.lvl`, `ingame.lvl;com_inf_default`, `tat/tat2.lvl`, and `side/rep.lvl;rep_inf_ep3_rifleman` over the Phase 1 OPFS sparse block VFS, activating `RedTerrain`, `SkyManager`, and `RedSceneManager`, spawning a Republic rifleman and `RedTextElement` HUD banner, and rendering frames through `FLRenderer::Render()`. |

---

## 3. Key Technical Findings & Bugs Resolved

### 3.1 `RedMemory.cpp` Global `operator new` Override & Linear Memory Zeroing at `0x0`

* **Symptom:** During the first headless Chrome run of `swbf2_wgpu_init`, `WgpuDynRing::init` called
  `wgpu::Device::CreateBuffer(&bd)` and aborted in `_emwgpuDeviceCreateBuffer` with `devicePtr = 0`
  and `g_wgpuCtx.device` wiped to `0`.
* **Root Cause:** `RedEngineFL/Memory/RedMemory.cpp:777` replaces global `operator new(size_t)` with
  `RedAllocFromHeap(__RedCurrHeap, size, 0)`. Before `GameLoop` initializes heap budgets (or when an
  allocation exceeds the active `RedMemory` pool budget), `RedAllocFromHeap` returns `NULL`. Because
  the decompiled engine is compiled with `-fno-exceptions`, `std::allocator<uint8_t>::allocate` does
  not throw `std::bad_alloc` and returns `0x00000000` to `std::vector::assign(16 MiB, 0)`. Wasm's
  `memory.fill(0x00000000, 0, 16777216)` then zeroes the first `16 MiB` of linear memory (where `0`
  is a valid linear address in WebAssembly!), wiping the static `.data`/`.bss` segment including
  `g_wgpuCtx` at `0x005409ec`.
* **Fix:** Added `SysAllocator<T>` (and `typedef std::vector<uint8_t, SysAllocator<uint8_t>> ByteVec`)
  in `browser/gpu/d3d9_wgpu_internal.h` backed directly by libc `malloc`/`free` (which allocates
  from the Emscripten system `dlmalloc`/`sbrk` heap above `111 MiB` and aborts immediately on OOM).
  All CPU-side GPU staging buffers and texture lock buffers in `browser/gpu/` use `ByteVec`.

### 3.2 `LoadUtil::ReadDataFileOnHeap` Nested `lvl_` Filter for Faction Sub-Levels

* **Symptom:** Calling `LoadUtil::ReadDataFileOnHeap("side/rep.lvl", ..., &repHash, 1, 0)` without
  `CHUNK_ID('l', 'v', 'l', '_')` in the allowed chunk ID list and without passing the class name
  pointer (`&repName`) caused `LoadUtil` to skip the nested `lvl_` sub-container holding
  `rep_inf_ep3_rifleman`.
* **Fix:** Include `CHUNK_ID('l', 'v', 'l', '_')` in `kSharedIds`, pass `&comInfName` / `&repName`
  as the final argument to `LoadUtil::ReadDataFileOnHeap`, call `SoldierAnimationBank::Reset()` and
  `SoldierAnimatorClass::PostLoad()` before `MemoryPool::CreateAll()`, and initialize the spawned
  `EntitySoldier`'s `GameObject` team (`SetTeam(0)`) and `ent->Init()` matching `Character::Spawn`.

### 3.3 Direct3D 9 FFP Math & Winding Conventions in WebGPU

* **Matrix Layout:** `D3DMATRIX` / `PblMatrix` stores row-vector transforms (`v_row * M`), with
  translation in `m[3][0..2]`. When loaded into a WGSL column-major `mat4x4<f32>`, `m[0..3]` become
  columns `0..3`, so `M_wgsl * v_col` produces the exact same result as `v_row * M_d3d` with zero
  transposition.
* **Texture Stage Coordinate Generation (`D3DTSS_TCI_*`):**
  * `pcTerrainShader::Begin_Fixed` and `pcNormalShader` set `D3DTSS_TCI_CAMERASPACEPOSITION` with a
    `4×4` texture matrix whose translation is in row 3 (`m[3][0..2]`), so camera-space positions are
    extended as `vec4<f32>(camPos, 1.0)`.
  * `D3DTSS_TCI_PASSTHRU` with `D3DTTFF_COUNT2` uses a `3×3` 2D UV transform stored in rows 0..2
    (`m[2][0..1]` translation), so 2D UVs are extended as `vec4<f32>(u, v, 1.0, 0.0)`.
* **Winding & Depth:** Both D3D9 and WebGPU use `[0, 1]` clip-space depth and top-down framebuffer
  coordinates (`FrontFace::CCW` → `D3DCULL_CW` maps to `CullMode::Back`, `D3DCULL_CCW` maps to
  `CullMode::Front`).

---

## 4. Measured Verification (`browser/webgpu_render_check.mjs`)

Run the automated headless Chrome verification suite (starts `tools/serve.py` with COOP/COEP +
`/GameData/` HTTP `Range` streaming, boots the Dedicated Worker with `OffscreenCanvas` on hardware
Vulkan, loads `core.lvl`, `common.lvl`, `ingame.lvl`, `tat/tat2.lvl`, and `side/rep.lvl` over the
OPFS sparse block cache, renders two distinct camera viewpoints across Mos Eisley, reads back the
WebGPU backbuffer, writes `phase2_tat2_camA.png` and `phase2_tat2_camB.png`, and asserts 17 invariants):

```sh
./tools/build_browser.sh
vendor/emsdk/node/24.19.0_64bit/bin/node browser/webgpu_render_check.mjs
```

### Measured Output (Chrome `152.0.7977.82`, NVIDIA Pascal Vulkan)

```json
{
  "ok": true,
  "elapsedMs": 937,
  "loadMs": 562,
  "wgpuReady": {
    "ok": true,
    "width": 640,
    "height": 360,
    "hasTextureCompressionBC": true,
    "isFallbackAdapter": false,
    "adapter": {
      "vendor": "nvidia",
      "architecture": "pascal"
    }
  },
  "initOk": true,
  "loadOk": true,
  "stats": {
    "initialized": true,
    "levelLoaded": true,
    "pipeline": 0,
    "geometryMode": 0,
    "skinningMode": 0,
    "texturesLoaded": 320,
    "terrainLayers": 3,
    "collisionMeshes": 68,
    "trooperSpawned": true,
    "lastFrameDraws": 362,
    "lastFrameTriangles": 80213,
    "wgpuErrors": 0,
    "hasBC": true
  },
  "frameA": {
    "ok": true,
    "width": 640,
    "height": 360,
    "nonBlackPixels": 230400,
    "nonBlackPct": 100,
    "distinctColors12bit": 178,
    "meanLuma": 107.54,
    "quadrantLumas": [99.18, 103.78, 113.43, 113.77],
    "centerRgba": "0x827663fe",
    "draws": 372,
    "triangles": 71080,
    "wgpuErrors": 0
  },
  "frameB": {
    "ok": true,
    "width": 640,
    "height": 360,
    "nonBlackPixels": 230399,
    "nonBlackPct": 100,
    "distinctColors12bit": 197,
    "meanLuma": 109.36,
    "quadrantLumas": [84.08, 117.31, 112.9, 123.15],
    "centerRgba": "0x776a53fe",
    "draws": 362,
    "triangles": 80213,
    "wgpuErrors": 0
  },
  "frameDiffPct": 80.89
}
```

All **17 / 17** assertions pass with **`0` WebGPU validation/device errors**, streaming only
`57.86 MB` out of `199.27 MB` of referenced `.lvl` containers (`225` 256 KiB OPFS blocks cached) in
`562 ms`, and rendering `372` draw calls / `71,080` triangles (Camera A) and `362` draw calls /
`80,213` triangles (Camera B) — including `RedTerrain`, Mos Eisley architecture, moisture
vaporators, crashed starship wreckage, `SkyManager` dome, spawned `rep_inf_ep3_rifleman`, and
`RedFont`/`RedTextElement` HUD overlay (`"STAR WARS BATTLEFRONT II - WEBGPU FFP"`).
