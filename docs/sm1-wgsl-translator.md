# Phase 3: DirectX 9 Shader Model 1.1 (`vs_1_1` / `ps_1_1`) → WGSL Pipeline

This document records the bytecode audit, runtime translator architecture, WebGPU resource/layout
upgrades, and measured headless Chrome Vulkan verification for Phase 3 of `swbf2-web`.

---

## 1. Complete `core.lvl` Shader Model 1.1 Bytecode Audit (`tools/audit_sm1_shaders.py`)

Scanning all `ucfb` chunks in `data/Star wars battlefront 2/GameData/data/_lvl_pc/core.lvl` (`7.16 MB`)
with `python3 tools/audit_sm1_shaders.py` confirms that retail *Star Wars Battlefront II (2005)* ships
**30 `SHDR` chunks** containing **100% DirectX 9 Shader Model 1.1 (`vs_1_1` / `ps_1_1`) bytecode**
(`0` Shader Model 2.0/3.0 blobs in `core.lvl`, `0` dynamic branches/loops, `0` runtime HLSL compiler
calls):

* **Total `SHDR` chunks:** `30`
* **Total `vs_1_1` (`0xFFFE0101`) shaders in `core.lvl`:** `2,332`
* **Total `ps_1_1` (`0xFFFF0101`) shaders in `core.lvl`:** `226`
* **Engine-instantiated shaders in `PIPELINE_PROG1` (`1`):** `1,042` `vs_1_1` + `146` `ps_1_1`
  * Each `SHDR` chunk contains two `PIPE` sub-chunks: `uiPipeline == 1` (`PIPELINE_PROG1`) and
    `uiPipeline == 2` (`PIPELINE_PROG2`). Both store `vs_1_1` / `ps_1_1` bytecode in `core.lvl`;
    when `pcRedRenderer` runs in `PIPELINE_PROG1`, `pcRedShaderDef::ReadPipeline`
    (`build/src_fixed/RedEngineFL/Graphics/PC/pcRedShader.cpp:265`) loads only the `uiPipeline == 1`
    `PIPE` sub-chunk (`1,042` VS + `146` PS).
  * In addition to `core.lvl`, `pcRedRenderer.cpp:226` embeds one 8-instruction `ps_2_0`
    (`0xFFFF0200`) fullscreen gamma/brightness/contrast pixel shader (`kGammaPassPS`).

### 1.1 Shader Counts by Render Type (`RTYP`)

| `#` | `RTYP` | `vs_1_1` Count | `ps_1_1` Count | Engine Shader Class |
|---:|---|---:|---:|---|
| 1 | `normal` | 528 | 20 | `pcNormalShader` |
| 2 | `Terrain2` | 17 | 3 | `pcTerrainShader` |
| 3 | `interface` | 4 | 4 | `pcInterfaceShader` |
| 4 | `particle` | 4 | 2 | `pcParticleShader` |
| 5 | `sprite` | 2 | 1 | `pcSpriteShader` |
| 6 | `skyfog` | 1 | 1 | `pcSkyFogShader` |
| 7 | `foliage` | 8 | 4 | `pcFoliageShader` |
| 8 | `lightbeam` | 2 | 1 | `pcLightBeamShader` |
| 9 | `scroll` | 288 | 9 | `pcScrollShader` |
| 10 | `flare` | 4 | 2 | `pcFlareShader` |
| 11 | `sample` | 1 | 1 | `pcSampleShader` |
| 12 | `rain` | 2 | 1 | `pcRainShader` |
| 13 | `normalmapadder` | 4 | 2 | `pcNormalMapAdderShader` |
| 14 | `prereflection` | 4 | 2 | `pcPreReflectionShader` |
| 15 | `water` | 36 | 18 | `pcWaterShader` |
| 16 | `stencilshadow` | 21 | 21 | `pcStencilShadowShader` |
| 17 | `shadowquad` | 1 | 1 | `pcShadowQuadShader` |
| 18 | `specmap` | 128 | 4 | `pcSpecMapShader` |
| 19 | `detail` | 288 | 9 | `pcDetailShader` |
| 20 | `ocean` | 4 | 2 | `pcOceanShader` |
| 21 | `refraction` | 192 | 6 | `pcRefractionShader` |
| 22 | `decal` | 3 | 3 | `pcDecalShader` |
| 23 | `filtercopy` | 7 | 7 | `pcFilterCopyShader` |
| 24 | `shield` | 1 | 1 | `pcShieldShader` |
| 25 | `bump` | 576 | 53 | `pcBumpShader` |
| 26 | `zprepass` | 6 | 2 | `pcZPrepassShader` |
| 27 | `pervertexdiffuselighting` | 28 | 7 | `pcPerVertexDiffuseLightingShader` |
| 28 | `perpixeldiffuselighting` | 108 | 27 | `pcPerPixelDiffuseLightingShader` |
| 29 | `specularlighting` | 60 | 8 | `pcSpecularLightingShader` |
| 30 | `hdr` | 4 | 4 | `pcHDRShader` |
| **Total** | **30 `SHDR` chunks** | **2,332** | **226** | |

### 1.2 Complete `vs_1_1` Opcode & Declaration Surface

* **14 distinct `vs_1_1` opcodes across all 2,332 shaders:**
  `add` (`9,283`), `dcl` (`9,574`), `dp3` (`25,282`), `dp4` (`34,149`), `lit` (`794`),
  `mad` (`31,617`), `max` (`12,824`), `min` (`3,275`), `mov` (`19,657`), `mul` (`26,754`),
  `rcp` (`1,611`), `rsq` (`3,352`), `sge` (`1,035`), `slt` (`1,472`).
  *(Macro opcodes `m4x4`, `m4x3`, `m3x4`, `m3x3`, `m3x2`, `mova`, `frc`, `exp`, `expp`, `log`,
  `logp`, `dst`, `abs` are also supported by the translator).*
* **Relative addressing (`c[N + a0.x]`):** `7,413` operands across hardware-skinned vertex shaders.
  In `vs_1_1`, writing to `a0.x` uses `D3DSIO_MOV` with destination register type `D3DSPR_ADDR` (`3`).
* **Constant register range:** Static index up to `c53`, plus hardware bone palette `c51..c95`
  (`MaxVertexShaderConst = 96`).
* **19 distinct `dcl` semantic → input register (`v0..v8`) bindings:**
  * `position0 -> v0` (`2,332`)
  * `blendweight0 -> v1` (`8`), `blendindices0 -> v1` (`8`), `blendindices0 -> v2` (`1,109`)
  * `normal0 -> v2` (`4`), `normal0 -> v3` (`2,226`)
  * `position1 -> v2` (`4`), `position1 -> v3` (`4`), `position2 -> v3` (`4`), `position2 -> v6` (`4`)
  * `tangent0 -> v4` (`206`), `blendweight1 -> v4` (`8`)
  * `binormal0 -> v5` (`206`), `blendindices1 -> v5` (`8`)
  * `color0 -> v6` (`1,156`)
  * `texcoord0 -> v7` (`2,278`), `blendweight2 -> v7` (`4`)
  * `texcoord1 -> v8` (`1`), `blendindices2 -> v8` (`4`)

### 1.3 Complete `ps_1_1` Opcode Surface

* **17 distinct `ps_1_1` opcodes across all 226 shaders:**
  `add` (`45`), `cnd` (`49`), `def` (`115`), `dp3` (`156`), `lrp` (`48`), `mad` (`202`),
  `mov` (`158`), `mul` (`479`), `sub` (`1`), `tex` (`447`), `texbem` (`4`), `texcoord` (`20`),
  `texm3x2pad` (`1`), `texm3x2tex` (`1`), `texm3x3pad` (`20`), `texm3x3tex` (`8`),
  `texm3x3vspec` (`2`).
  *(Additional `ps_1_1..1_3` opcodes `texkill`, `texbeml`, `texreg2ar`, `texreg2gb`, `texm3x3diff`,
  `texm3x3spec`, `cmp`, `bem`, `dp4` are also implemented in the translator).*
* **Co-issued vector + alpha instructions (`+` bit `0x40000000`):** `342` co-issued instructions,
  with **`0` read-after-write hazards** between the RGB and Alpha pipe instructions in every pair
  (sequential emission is 100% equivalent).
* **Constant register range:** `c0..c7` (`8 × vec4<f32>`), updated via `SetPixelShaderConstantF`
  and overridden locally when `def cN, f0, f1, f2, f3` is present in the shader bytecode.

---

## 2. Translator & Pipeline Architecture (`browser/gpu/dx9_sm1_wgsl.cpp`)

### 2.1 Unified Program Generation (`GenerateCombinedWGSL`)

Because Direct3D 9 binds vertex shaders (`SetVertexShader`) and pixel shaders (`SetPixelShader`)
independently — and frequently mixes a programmable `vs_1_1` with the fixed-function fragment stage
or a fixed-function/RHW vertex stage with a programmable `ps_1_1` — `GenerateCombinedWGSL` in
`browser/gpu/dx9_sm1_wgsl.cpp` emits a single self-contained WGSL module (`vs_main` + `fs_main`)
cached by `ProgramCacheKey { vsId, psId, vsKey, texTypes }`:

1. **Shared `VsOut` Interpolant Interface:**
   * `@builtin(position) pos: vec4<f32>`
   * `@location(0) color0: vec4<f32>` (`oD0`, clamped to `[0, 1]` per DX9 SM 1.x spec)
   * `@location(1) color1: vec4<f32>` (`oD1`, clamped to `[0, 1]`)
   * `@location(2)..@location(5) tex0..tex3: vec4<f32>` (`oT0..oT3`, full `vec4<f32>` preserved for
     3D/Cube directions, `texm3x3vspec` eye-vector `.w` components, and `D3DTTFF_PROJECTED`
     divides)
   * `@location(6) fog: f32` (`oFog`, saturated to `[0, 1]`)
2. **Vertex Stage (`vs != nullptr` vs FFP fallback):**
   * When `vs != nullptr`, `EmitSm1VertexStageWGSL` decodes the `vs_1_1` token stream.
   * When `vs == nullptr`, the Phase 2 FFP vertex stage (`d3d9_ffp_wgsl.cpp`) is emitted with the
     same `VsOut` struct.
3. **Fragment Stage (`ps != nullptr` vs FFP fallback):**
   * When `ps != nullptr`, `EmitSm1FragmentStageWGSL` decodes `ps_1_1` or `ps_2_0` (`kGammaPassPS`),
     followed by the shared alpha-test (`u.alphaTest`) and linear fog (`u.fogParams`, `u.fogColor`)
     epilogue.
   * When `ps == nullptr`, the 4-stage D3D9 FFP texture combiner (`D3DTSS_COLOROP`/`ALPHAOP`) runs
     against `in.tex0..3`, `in.color0`, and `in.color1`.

### 2.2 Compressed Vertex Attribute Bridge (`vsInputTypeKey`)

When `pcForceFixedFunction == false`, `RedRenderer` enables:
* `m_renderCaps.pipeline = PIPELINE_PROG1` (`1`)
* `m_renderCaps.geometry = GEOMETRYMODE_COMPRESSED` (`1`)
* `m_renderCaps.skinning = SKINNING_HARDWARE` (`1`)

In `GEOMETRYMODE_COMPRESSED`, `pcRedVertexFormat` creates vertex declarations using compressed D3D9
types (`D3DDECLTYPE_SHORT4`, `D3DDECLTYPE_SHORT2`, `D3DDECLTYPE_D3DCOLOR`, `D3DDECLTYPE_UBYTE4`)
and uploads per-primitive decompression scale/bias constants to `c12..c15`
(`pcRedRenderer.cpp:7361-7401`):
* **`c12`:** `[normalScale (2.0 or 1.0), normalBias (-1.0 or 0.0), texScale (1/2048 or 1.0), 1.0]`
* **`c13`:** `[(bbox.max - bbox.min) / 65534.0, 0.0]`
* **`c14`:** `[(bbox.max + bbox.min) * 0.5, 1.0]`

In WebGPU, `VertexFormat::Sint16x2` (`SHORT2`) and `VertexFormat::Sint16x4` (`SHORT4`) require
`vec2<i32>` / `vec4<i32>` inputs in WGSL, whereas `Float32x*`, `Unorm8x4`, and `Uint8x4` require
`f32` / `vec*<f32>` / `vec4<u32>`. At draw time (`WgpuD3D9Device::emitDraw`), we match each `dcl`
in the active `WgpuVertexShader` against the bound `WgpuVertexDecl` by `(Usage, UsageIndex)`, pack
the 4-bit `WgpuVtxFmt` per input register `v0..v15` into a 64-bit `vsInputTypeKey`, and emit exact
WGSL attribute types and `read_v(idx)` conversions:
* `WGPU_VTX_UBYTE4N` (`D3DCOLOR` stored in memory as `[B, G, R, A]`): swizzled via `.zyxw` to logical
  `(R, G, B, A)` in `[0, 1]`. Note that `pcRedVertexFormat` encodes compressed normals, tangents,
  binormals, blend weights, and 1-bone blend indices as `D3DCOLOR`!
* `WGPU_VTX_SHORT2` / `WGPU_VTX_SHORT4`: converted via `vec2<f32>(in.vN)` / `vec4<f32>(in.vN)` so
  the `vs_1_1` instructions (`mad r0.xyz, v0, c13, c14` and `mul oT0, v7, c12.zzzz`) decompress
  positions and UVs identically to Direct3D 9.

### 2.3 Dynamic 2D / Cube / 3D Texture Bind Group Layouts (`texTypes`)

 Programmable shaders in `core.lvl` sample 2D textures (`D3DRTYPE_TEXTURE`), environment/reflection
and normalization cube maps (`D3DRTYPE_CUBETEXTURE`, e.g. `normalization_cube_map`), and 3D
attenuation volume textures (`D3DRTYPE_VOLUMETEXTURE`, e.g. `attenuation_volume`).
* `WgpuCubeTexture` creates a 6-layer `wgpu::Texture` with `TextureViewDimension::Cube`.
* `WgpuVolumeTexture` creates a 3D `wgpu::Texture` (`TextureDimension::e3D`, `TextureViewDimension::e3D`).
* `WgpuGpuContext` pre-creates `1×1` white fallback views for all three dimensions (`whiteView`,
  `whiteCubeView`, `whiteVolView`) and caches `BindGroupLayout` and `PipelineLayout` objects keyed by
  an 8-bit `texTypes` mask (`2 bits × 4 stages`: `0 = 2D`, `1 = Cube`, `2 = 3D`).
* `EmitTextureDeclWGSL` and `EmitSampleStageWGSL` emit `texture_2d<f32>`, `texture_cube<f32>`, or
  `texture_3d<f32>` bindings and sample them accordingly (including `D3DTTFF_PROJECTED` divide by
  `.w` when enabled on the stage).

---

## 3. Measured Verification (`browser/webgpu_render_check.mjs`)

Run the automated headless Chrome verification suite:

```sh
./tools/build_browser.sh
vendor/emsdk/node/24.19.0_64bit/bin/node browser/webgpu_render_check.mjs
```

### Measured Output (Chrome `152.0.7977.82`, NVIDIA Pascal Vulkan)

```json
{
  "ok": true,
  "elapsedMs": 9319,
  "loadMs": 872,
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
    "pipeline": 1,
    "geometryMode": 1,
    "skinningMode": 1,
    "texturesLoaded": 320,
    "terrainLayers": 3,
    "collisionMeshes": 68,
    "trooperSpawned": true,
    "vertexShadersCreated": 1042,
    "pixelShadersCreated": 146,
    "vsTranslated": 21,
    "psTranslated": 21,
    "lastFrameDraws": 519,
    "lastFrameTriangles": 173057,
    "lastFrameSkinnedDraws": 6,
    "lastFrameSkinnedTriangles": 8894,
    "wgpuErrors": 0,
    "hasBC": true
  },
  "shaderValidation": {
    "ok": true,
    "vsTotal": 2332,
    "vsCompiled": 2332,
    "psTotal": 226,
    "psCompiled": 226,
    "engineVsTotal": 1042,
    "engineVsCompiled": 1042,
    "enginePsTotal": 146,
    "enginePsCompiled": 146,
    "shaderCompileErrors": 0
  },
  "frameA": {
    "ok": true,
    "width": 640,
    "height": 360,
    "nonBlackPixels": 229268,
    "nonBlackPct": 99.51,
    "distinctColors12bit": 295,
    "meanLuma": 149.81,
    "quadrantLumas": [152.5, 134.88, 160.44, 151.41],
    "centerRgba": "0x918269ff",
    "draws": 560,
    "triangles": 175507,
    "skinnedDraws": 6,
    "skinnedTriangles": 8894,
    "wgpuErrors": 0
  },
  "frameAHidden": {
    "ok": true,
    "width": 640,
    "height": 360,
    "nonBlackPixels": 230400,
    "nonBlackPct": 100,
    "distinctColors12bit": 276,
    "meanLuma": 153.15,
    "quadrantLumas": [152.49, 138.38, 160.44, 161.31],
    "draws": 554,
    "triangles": 162130,
    "skinnedDraws": 2,
    "skinnedTriangles": 630,
    "wgpuErrors": 0
  },
  "frameB": {
    "ok": true,
    "width": 640,
    "height": 360,
    "nonBlackPixels": 230376,
    "nonBlackPct": 99.99,
    "distinctColors12bit": 249,
    "meanLuma": 139.88,
    "quadrantLumas": [136.18, 139.21, 138.07, 146.07],
    "centerRgba": "0xaa9775ff",
    "draws": 519,
    "triangles": 173057,
    "skinnedDraws": 6,
    "skinnedTriangles": 8894,
    "wgpuErrors": 0
  },
  "frameDiffPct": 86.44,
  "trooperDiffPct": 4.81,
  "trooperDiffPx": 11080
}
```

### Summary of Verified Phase 3 Results (`25 / 25 PASS`)

1. **100% Dawn/Tint Shader Compilation Pass (`2,332 / 2,332 vs_1_1` + `226 / 226 ps_1_1`):**
   Every single `vs_1_1` and `ps_1_1` shader across all 30 `SHDR` chunks (`PIPELINE_PROG1` and
   `PIPELINE_PROG2`) in `core.lvl` translates to WGSL and compiles via `wgpu::Device::CreateShaderModule`
   with **`0` compilation or validation errors**.
2. **Full Programmable Pipeline + Compressed Geometry + Hardware Skinning Active:**
   `RedRenderer` runs with `pipeline = 1 (PIPELINE_PROG1)`, `geometryMode = 1 (GEOMETRYMODE_COMPRESSED)`,
   and `skinningMode = 1 (SKINNING_HARDWARE)`, more than doubling scene geometric detail on Tatooine
   Mos Eisley (`tat/tat2.lvl`) from `372` draws / `71,080` triangles (Phase 2 FFP) to **`560` draws /
   `175,507` triangles** (Camera A) and **`519` draws / `173,057` triangles** (Camera B) with **`0`
   WebGPU errors**.
3. **Hardware-Skinned 501st Clone Trooper (`rep_inf_ep3_rifleman`) End-to-End Verification:**
   Three real root causes that previously kept skinned characters invisible were fixed and verified
   both numerically (`skinnedDraws = 6`, `skinnedTriangles = 8,894`, `trooperDiffPct = 4.81%` /
   `11,080` pixels vs hidden trooper) and visually in `phase3_tat2_camA.png` and `phase3_tat2_camB.png`:
   * **`RedSegment::Read` `VDAT`/`VBUF` superseding (`tools/prepare_tree.py` -> `pcRedSegment.cpp`):**
     When a hardware-skinned compressed `VBUF` (`0xF000`) is read after `VDAT`, the `VDAT`/`SKIN`
     software-skinning pair is now discarded so the segment builds as a pure hardware-skinned
     `pcShaderSegment` instead of a mismatched `pcSoftwareSkinnedShaderSegment`.
   * **PVS dynamic registration (`browser/gpu/wgpu_engine_bridge.cpp`):**
     Removed the stale `g_trooper->SetPortalActive(false)` override that left `EntitySoldier`
     permanently PVS-culled during `FLRenderer::Render()`.
   * **Zephyr skeletal animation tick (`browser/gpu/wgpu_engine_bridge.cpp`):**
     `SoldierAnimator::SetupPose` reads `GameLoop::sClientDeltaTime` and returns early without
     evaluating the Zephyr skeleton when `sClientDeltaTime <= 0.0f` (leaving all bone matrices in
     `c51..c95` at identity). Setting `GameLoop::sGameOver = false` and `GameLoop::sClientDeltaTime = dt`
     in `swbf2_engine_render_frame` drives full skeletal animation and uploads live `4×3` bone
     palettes into `c51..c95` indexed by `a0.x`.
4. **Opaque Backbuffer Readback & Build Tooling Polish:**
   * `swbf2_wgpu_readback_frame` forces `alpha = 255` on every readback pixel to match `Present()`'s
     `fs_blit` (`D3DFMT_X8R8G8B8`), yielding `low_alpha = 0` in both PNG readbacks.
   * `tools/compile_one.sh` now derives the exact same `build/obj/<rel_underscored>.o` filename as
     `tools/compile_core.sh`, preventing stale-archive mismatches during single-TU rebuilds.
