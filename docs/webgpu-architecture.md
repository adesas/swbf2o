# WebGPU Renderer Architecture (`emdawnwebgpu` + D3D9 / SM 1.1)

This document captures the architectural design, measured engine GPU surface audit, and Phase 0
hardware WebGPU verification for `swbf2-web`.

## Locked Architectural Decisions

1. **WebGPU-only graphics backend** via Emscripten 7.0.0 `--use-port=emdawnwebgpu` (cached locally
   in `vendor/emsdk/upstream/emscripten/cache/ports/emdawnwebgpu/`, package `v20260423.175430`).
   No dual WebGL2 renderer is maintained in the production path; WebGPU provides native D3D9
   `[0, 1]` NDC depth, direct BC1/BC2/BC3 (`texture-compression-bc`) block-compressed textures,
   explicit pipeline state objects, and zero-overhead `OffscreenCanvas` rendering inside a Dedicated
   Worker.
2. **Software BC1/BC2/BC3 → RGBA8 fallback** when `adapter.HasFeature(wgpu::FeatureName::TextureCompressionBC)`
   is false (e.g., mobile GPUs or software adapters without BC support).
3. **Dedicated Worker + `OffscreenCanvas`** execution model: the WASM engine runs in a Dedicated
   Worker (`browser/worker.mjs`) owning both `OffscreenCanvas` WebGPU presentation (bound via
   Emscripten `specialHTMLTargets['!canvas'] = Module['canvas']`) and synchronous OPFS
   `FileSystemSyncAccessHandle` I/O — with zero `-sASYNCIFY` overhead and zero main-thread jank.

## Measured Engine Direct3D9 Call Surface

Auditing every translation unit in `vendor/openswbf2/src` (`RedEngineFL/Graphics/PC/*`,
`RedEngineFL/movie/pc/*`, `Battlefront2/Platform/Pc/*`) shows that SWBF2 (2005) uses a compact,
well-bounded Direct3D 9 subset (~45 distinct `IDirect3DDevice9` methods):

| Category | Methods called by engine |
|---|---|
| Resource creation | `CreateTexture`, `CreateCubeTexture`, `CreateVolumeTexture`, `CreateVertexBuffer`, `CreateIndexBuffer`, `CreateVertexDeclaration`, `CreateVertexShader`, `CreatePixelShader`, `CreateRenderTarget`, `CreateDepthStencilSurface`, `CreateOffscreenPlainSurface`, `CreateStateBlock`, `CreateQuery` |
| Render targets & clear | `GetBackBuffer`, `GetRenderTarget`, `SetRenderTarget`, `GetDepthStencilSurface`, `SetDepthStencilSurface`, `StretchRect`, `ColorFill`, `Clear`, `SetViewport`, `SetScissorRect` |
| Pipeline & state | `SetRenderState`, `GetRenderState`, `SetTextureStageState`, `GetTextureStageState`, `SetSamplerState`, `GetSamplerState`, `SetTexture`, `GetTexture`, `SetFVF`, `GetFVF`, `SetVertexDeclaration`, `SetVertexShader`, `SetPixelShader`, `SetVertexShaderConstantF`, `SetPixelShaderConstantF` |
| Fixed-function state | `SetTransform`, `GetTransform`, `SetMaterial`, `SetLight`, `LightEnable`, `SetClipPlane` |
| Draw & frame | `BeginScene`, `EndScene`, `Present`, `SetStreamSource`, `SetIndices`, `DrawPrimitive`, `DrawIndexedPrimitive`, `DrawPrimitiveUP`, `DrawIndexedPrimitiveUP`, `TestCooperativeLevel`, `Reset` |

### Key WebGPU Compatibility Facts

* **NDC depth range:** Direct3D 9 and WebGPU both use `[0, 1]` clip-space depth (unlike OpenGL's
  `[-1, 1]`), so SWBF2's projection matrices (`RedCamera::ComputeProjMatrix`) work without Z
  remapping.
* **Vertex topology:** SWBF2 segments use `D3DPT_TRIANGLESTRIP` (`5`) and `D3DPT_TRIANGLELIST`
  (`4`), plus occasional `D3DPT_TRIANGLEFAN` (`6`, converted to triangle list on index upload) and
  `D3DPT_LINELIST` / `D3DPT_POINTLIST`. For `TriangleStrip` with `uint16` indices in WebGPU,
  `primitive.stripIndexFormat = wgpu::IndexFormat::Uint16` is set on the pipeline (SWBF2 uses
  degenerate triangles rather than `0xFFFF` restart indices, so strip index buffers must remap any
  `0xFFFF` or convert strips cleanly when needed).
* **Surface presentation:** In `emdawnwebgpu`, `wgpuSurfacePresent` aborts intentionally because
  browser swapchains present automatically when the `requestAnimationFrame` callback returns after
  `queue.Submit()`.
* **Bind group layouts:** `emdawnwebgpu` / Dawn enforces strict pipeline layout compatibility —
  pipelines sharing bind groups (such as offscreen render-target passes vs live swapchain passes)
  must use explicit `wgpu::BindGroupLayout` + `wgpu::PipelineLayout` objects rather than `auto`
  default layouts.

## Measured `core.lvl` Shader Inventory (100% DX9 Shader Model 1.1)

Inspecting `data/Star wars battlefront 2/GameData/data/_lvl_pc/core.lvl` reveals **30 `SHDR`
chunks** covering every material in the game (`normal_shader.xml`, `normal_terrain_shader.xml`,
`interface_shader.xml`, `particle_shader.xml`, `water_shader.xml`, `bump_shader.xml`,
`hdr_shader.xml`, `sky_shader.xml`, `shadow_shader.xml`, `flare_shader.xml`, `refract_shader.xml`,
etc.):

* **Vertex shaders:** `2,332` compiled DX9 bytecode blobs — **100% `vs_1_1`** (`0xFFFE0101`).
* **Pixel shaders:** `226` compiled DX9 bytecode blobs — **100% `ps_1_1`** (`0xFFFF0101`).
* **Shader Model 2.0 / 3.0 count:** **0**.
* **Dynamic control flow / loops / runtime D3DX HLSL compiler calls:** **0**.

### Two-Stage Renderer Bring-Up Plan

1. **Phase 2 (Fixed-Function Bring-Up — COMPLETE & VERIFIED):** Restore `pcRedRenderer.cpp`
   orchestration (`773 / 773` engine TUs compiled into `build/core.a`) behind a real virtual COM
   `IDirect3DDevice9` → WebGPU backend (`browser/gpu/d3d9_device_wgpu.cpp`,
   `d3d9_resources_wgpu.cpp`, `d3d9_ffp_wgsl.cpp`, `wgpu_engine_bridge.cpp`). Verified in headless
   Chrome on hardware Vulkan (`browser/webgpu_render_check.mjs`: `17 / 17 PASS`, `0` WebGPU errors,
   `320` textures, `372` draws / `71,080` triangles on Tatooine Mos Eisley `tat/tat2` with
   `RedTerrain`, buildings, `SkyManager`, `rep_inf_ep3_rifleman`, and `RedFont` HUD). See
   `docs/webgpu-renderer.md` for the full Phase 2 design and measurements.
2. **Phase 3 (Full SM 1.1 → WGSL Translation — COMPLETE & VERIFIED):** Translate all `2,332 vs_1_1` +
   `226 ps_1_1` bytecode streams from `core.lvl` deterministically into WGSL
   (`browser/gpu/dx9_sm1_wgsl.cpp`), validated against Dawn/Tint (`2,332 / 2,332 vs_1_1` and
   `226 / 226 ps_1_1` compiled with `0` errors), enabling `PIPELINE_PROG1` + `GEOMETRYMODE_COMPRESSED`
   + `SKINNING_HARDWARE` (`584` draws / `172,847` triangles on `tat/tat2` with `0` WebGPU errors).
   See `docs/sm1-wgsl-translator.md` for full details.

## Phase 0 Hardware WebGPU Smoke Test Results

Files:
* `browser/webgpu_smoke.cpp`
* `browser/webgpu_smoke_worker.mjs`
* `browser/webgpu_smoke.html`
* `tools/build_webgpu_smoke.sh`
* `browser/webgpu_smoke_check.mjs`

Build & run:
```sh
./tools/build_webgpu_smoke.sh
vendor/emsdk/node/24.19.0_64bit/bin/node browser/webgpu_smoke_check.mjs
```

Measured output on host Chrome `152.0.7977.82`:
```json
{
  "browser": "Chrome/152.0.7977.82",
  "result": {
    "ok": true,
    "surfaceOk": true,
    "hasTextureCompressionBC": true,
    "bcMode": "hardware-bc1",
    "isFallbackAdapter": false,
    "adapter": {
      "vendor": "nvidia",
      "architecture": "pascal"
    },
    "depthRedNearOk": true,
    "depthGreenNearOk": true,
    "texLeftYellowOk": true,
    "texRightCyanOk": true,
    "pixels": {
      "redNear": "0xff0000ff",
      "greenNear": "0x00ff00ff",
      "texLeft": "0xffff00ff",
      "texRight": "0x00ffffff"
    },
    "error": "",
    "inDedicatedWorker": true
  },
  "pageErrors": []
}
```

Verified invariants:
* `emdawnwebgpu` C++ module compiles and links cleanly (`em++ --use-port=emdawnwebgpu`,
  `wgpu_smoke.js` 98 KB, `wgpu_smoke.wasm` 67 KB).
* `OffscreenCanvas` transferred to a Dedicated Worker attaches cleanly via `specialHTMLTargets['!canvas']`
  and acquires a hardware NVIDIA Pascal Vulkan adapter (`isFallbackAdapter: false`).
* D3D9 `[0, 1]` `Depth24PlusStencil8` (`LessEqual`) passes both red-near (`z=0.2`, `0xff0000ff`) and
  swapped green-near (`z=0.2`, `0x00ff00ff`) readback checks.
* Hardware BC1 compressed texture (`BC1RGBAUnorm`) uploads and samples exact yellow (`0xffff00ff`)
  and cyan (`0x00ffffff`) texels; software BC1 → RGBA8 fallback (`decode_bc1_block`) is bundled in
  the same path for adapters lacking `texture-compression-bc`.
