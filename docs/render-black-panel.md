# The flat black panel in live missions

Status: **FIXED.**
The original root-cause candidate (`MapD3DDeclTypeToVtxFmt` types > 7) turned out to be a red herring —
`pcRedVertexFormat.cpp` only emits types `0..7` (`SHORT4` compressed position + `D3DCOLOR` normal +
`D3DCOLOR` color = 16 bytes on draw 38). The real root cause was an upstream decompilation bug in
`pcPerPixelDiffuseLightingShader::GetState_Prog1` (`RedEngineFL/Graphics/PC/Shaders/pcPerPixelDiffuseLightingShader.cpp`):

- `core.lvl`'s `perpixeldiffuselighting_shader.xml` PROG1 table has **18 states (`0..17`)**:
  - `0..5`: regular mesh, no bump map
  - `6..11`: regular mesh with bump map + tangent/binormal
  - `12..17`: terrain (`materialFlags & 0x10000000`) with bump map, no tangent/binormal
- `GetState_Prog1` ran two separate `if`s (`+= 12` for terrain AND `+= 6` for `bumpMapHash`), returning
  out-of-range states `18..20` (`>= 18`) on bumped terrain! The bounds guard in `pcRedShader.cpp:185`
  then bound `VS = NULL, PS = NULL` (FFP) for draws `32..37`. FFP rendered raw `SHORT4` compressed
  terrain coordinates without decompression, leaving the lighting pass black `(0,0,0)` so draw `38`
  (`pcTerrainShader2`, multiplicative `SRCBLEND=DESTCOLOR, DESTBLEND=ZERO`) painted the ground solid black.

**Fix (recorded in `tools/prepare_tree.py` `KNOWN_ARTIFACTS`):**
```cpp
if (pMaterial->bumpMapHash)
{
    iState += (pMaterial->materialFlags & 0x10000000) ? 12 : 6;
}
```
Verified on `tat2g_eli` (`phase4_gamestate_check.mjs` 29/29): `blackPct` dropped from **27.21% -> 0.42%**
with fully bump-mapped, lit Mos Eisley sand ground.

---

### Original investigation notes (archived)

## Symptom

In live gameplay the ground is overwritten by a large flat quad that renders near-pure black. It is
easiest to see in Hero Assault (`tat2g_eli`), where it is fully reproducible; on Tatooine Conquest
(`tat2g_con`) it appears at some spawn poses and not others, so it is not the right place to measure.

Measured in the 800x600 harness frame, in the rectangle `x 0.35..0.65 w, y 0.75..0.92 h`:

| run | mean luminance (sum of RGB) | pixels near black |
| --- | --- | --- |
| `s0` | 1.3 | 99.9% |
| `s3` | 1.3 | 99.9% |

The surrounding ground renders textured and, if anything, over-bright. Nothing logs a texture or
streaming failure during the run, so this is not a missing asset.

## What it is not

* **Not a hole / not the clear colour.** Forcing every `D3DCLEAR_TARGET` to `(0.3, 0, 0.3)`
  (`swbf2_wgpu_debug_set_clear_colour`) moves whole-frame black only from 26.7% to 19.3%, and paints
  just ~3% of the frame with the forced colour. The panel is unaffected by the clear, so geometry is
  covering it. (A bright clear is useless here: the engine's `HDREffect` bright-pass saturates a
  full-intensity clear and washes out the entire frame, which is what the first attempt at this read
  as "everything went white".)
* **Not a missing texture.** Unbound D3D9 stages bind a 1x1 opaque white fallback
  (`swbf2_white_1x1`), which is the correct D3D9 behaviour, and the culprit draws have textures bound
  at every active stage anyway.
* **Not degenerate geometry.** `ps=2` (force opaque white) removes the panel: the geometry is
  submitted and rasterised.

## What it is

`ps=1` (output `fract(stage-0 UV)`) shows the panel as a smooth ramp with **one coordinate covering
the full range and the other pinned near a constant**: along `y=470`, `u` runs 0.020, 0.149, 0.282,
0.412, 0.545, 0.675, 0.804, 0.937 while `v` stays exactly 16/255 = 0.0625, rising only to ~0.3 far
down the quad. The correctly-rendered ground next to it has `v` around 0.94-1.0. So the surface maps
into a **thin horizontal band of its texture page** - a band that is black there - rather than into
its own sub-texture. One UV coordinate is effectively missing.

## Which draw, and its state

`tools/black_panel_bisect.py` bisects the frame by hiding draws by per-frame id
(`swbf2_wgpu_debug_skip_draws`), scoring the panel rectangle from the screenshot:

```
baseline:              meanLum=  1.3 blackPct= 99.9   (draws=297..307)
hide [0,148)  -> PANEL GONE        hide [0,37)  -> still there
hide [37,55)  -> PANEL GONE        hide [37,38) -> still there (meanLum 1.4)
hide [37,39)  -> PANEL GONE        hide [39,40) -> gone (ids drift by +-1 between runs)
```

So a **single draw of 50 triangles** paints the whole panel; hiding it takes the rectangle from
meanLum 1.3 to 765 (the ground underneath, correctly lit). Its captured record:

```json
{"id":38,"tris":50,"stride":16,"declId":6148,"vsId":5097,"psId":5107,"vtxKey":"0x0000000000005570",
 "vp":"800x600","z":{"enable":1,"func":4},"blend":{"enable":1,"src":9,"dst":1},
 "stages":[{"uvIdx":0,"colorOp":4,"texId":6144,"texW":512,"texH":512,"texFmt":"0x33545844"},
           {"uvIdx":1,"colorOp":4,"texId":6145,"texW":256,"texH":256,"texFmt":"0x31545844"}, ...]}
```

50 quads, **stride 16**, one shared declaration (`declId 6148`, also used by draws 36 and 37 which
are the same batch kind unblended), alpha blending on, one vertex shader and one pixel shader per
batch. `POSITION` as `FLOAT3` is 12 bytes, so the whole remaining vertex is **4 bytes** - i.e. the
texture coordinates are packed (two half-floats, or a normalised short pair), not `FLOAT2`.

## Root-cause candidate

`MapD3DDeclTypeToVtxFmt` (`browser/gpu/d3d9_resources_wgpu.cpp`) handles only D3D9 declaration types
0..7 and silently drops everything else:

```c
    case D3DDECLTYPE_SHORT4:   *outBytes = 8;  return WGPU_VTX_SHORT4;
    default:                   *outBytes = 4;  return WGPU_VTX_NONE;
```

D3D9 continues past 7 with `UBYTE4N` (8), `SHORT2N` (9), `SHORT4N` (10), `USHORT2N` (11), `USHORT4N`
(12), `UDEC3` (13), `DEC3N` (14), `FLOAT16_2` (15), `FLOAT16_4` (16). The caller then does

```c
    if (fmt == WGPU_VTX_NONE) continue;
```

so such an element is **dropped without a word**: no attribute reaches `FfpVertexLayout`, the varying
it should feed is left at its default (hence a pinned `v`), and `minStride` never accounts for its
bytes. That matches both the 16-byte stride with packed UVs and the single surviving UV coordinate.

**To fix:** map types 8..16 to WebGPU vertex formats (`UNORM8x4`/`SNORM8x4`/`SNORM16x2`/`UNORM16x2`/
`FLOAT16x2`/`FLOAT16x4`), feed the right component count and normalisation through the FFP vertex
shader and the SM 1.1 input-type key, and add them to the pipeline cache key. `UDEC3`/`DEC3N` are
10-10-10 packed and have no direct WebGPU format - expand on the CPU or reject loudly. Do not leave
a `default:` that silently discards a vertex element: it turns a format gap into a rendering artefact
that looks like a texture problem. Worth asserting at load time that every element of a declaration
is mapped, and logging the ones that are not.

## Second, separate defect found on the way

Two perturbations of the build crashed the engine in two unrelated places:

* with `ps=4` (paint every draw with its id - recompiles every fragment shader): `luaC_collectgarbage`
  walking a table during `LuaScript::LoadScript`, i.e. during mission script load; reproducible;
* with per-draw record capture backed by a `std::vector`: a **null indirect call** inside
  `FirstPersonRenderable::RenderSoldier`.

Both crashed only when something perturbed the build; the untouched build is green run after run. The
record path was moved to fixed static storage (`g_dbgRecs[2][4096]`) and the crash went away, which
points at a **latent out-of-bounds write whose victim depends on heap/global layout**, i.e. the same
family as the recently-fixed hardcoded `pRaw + 0x20bc` MSVC byte offset in
`PlayerProfile::tGameOpt::ApplySettings`. A bounded next step: build the core with
`-fsanitize=address` (or with `GLOBAL_BASE`/heap canaries) and run the Phase 4 check under it to
catch the write in the act. That is likely to matter more than any individual artefact, because the
same corruption can be silently producing others.

## Instruments (all default-off)

| knob | how | what it answers |
| --- | --- | --- |
| `swbf2_wgpu_debug_set_clear_colour(on, r, g, b)` | page query `clear=0.3,0,0.3` | is this black region geometry, or nothing drawn at all? |
| `swbf2_wgpu_debug_set_ps_mode(n)` | page query `ps=1\|2` (`4` currently unusable, see above) | 1: show stage-0 UVs; 2: force opaque white (is geometry submitted here?) |
| `swbf2_wgpu_debug_skip_draws(lo, hi)` | page query `skip=lo-hi` | which draw produces a given region; bisect it |
| `swbf2_wgpu_debug_record_draws(1)` | page query `record=1&ids=25-50` | dump the submitted state (textures, decl, stride, blend, z, vs/ps ids) for draw ids in a range |
| `swbf2_wgpu_debug_frame_texture_summary()` | logged with `record=1` | every texture bound in the last frame, with draw and triangle counts |
| `tools/black_panel_bisect.py` | `python3 tools/black_panel_bisect.py [draws]` | automated bisection of the panel to one draw id |

The query reaches the worker as `SWBF2_PAGE_QUERY` on `browser/phase4_gamestate_check.mjs`, e.g.

```bash
SWBF2_PAGE_QUERY='skip=38-39' SWBF2_SCREENSHOT_DIR=/tmp/out \
  vendor/emsdk/node/24.19.0_64bit/bin/node browser/phase4_gamestate_check.mjs
```
