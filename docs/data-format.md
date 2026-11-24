# Data format: `.lvl` = `ucfb` chunk containers (verified against retail GameData)

Verified against retail PC `GameData/data/_lvl_pc/` assets (5.2 GB, 196 files). Tools: `tools/lvl_reader.py`, `tools/lvl_survey.py`.

## The format

```
magic  'ucfb'
uint32 total_payload_size
then a chunk list, each chunk: 4-byte ASCII id + uint32 size + payload
child boundaries are 4-byte aligned (matches PblFileChunk::HasMoreChildren in the decomp source)
```

A payload is a container only if its children exactly consume it — otherwise it is raw data. With that rule the
reader resolves **153 of 185** `.lvl` files; every one of the 185 is a `ucfb` container, so the 32 exceptions are the
`sound/*.lvl` banks, which use their own compressed/inline encoding (`SoundFL`, `common.bnk`, `sndstream*`). That is a
real, scoped subproblem — not a format misunderstanding.

The engine already implements this reader (`PebbleFL/Common/PblChunk.h`, `Zephyr/Source/CheckChunkId.cpp`), so a WASM
build reads the original game data directly. The Python reader exists to verify behaviour and to drive an optional
pre-baked web bundle.

## Chunk ids found in the real data (153 files)

| id | chunks | bytes | what it is |
|---|---:|---:|---|
| `tex_` | 2515 | 959.8 MB | textures (with `INFO`, `FMT_`, `FACE`, `LVL_` = mip levels; DXT3/DXT1) |
| `lvl_` | 560 | 812.9 MB | LOD / level geometry blocks |
| `modl` | 2368 | 119.5 MB | models (meshes) |
| `tern` | 28 | 25.3 MB | terrain |
| `coll` | 1003 | 7.6 MB | collision |
| `fx__` | 546 | 4.0 MB | particle / effect systems |
| `wrld` | 198 | 3.0 MB | world/scene assembly |
| `SHDR` | 30 | 3.0 MB | shaders |
| `Locl` | 6 | 2.7 MB | localisation / sound-loc? (needs confirmation) |
| `path` | 351 | 2.1 MB | **AI path networks** (route data used by AIGoal/AIUtil) |
| `scr_` | 165 | 1.3 MB | **precompiled Lua bytecode** (`NAME` + `INFO` + `BODY` starting with `\x1bLua`) |
| `prim` | 752 | 0.9 MB | primitives |
| `plan` | 31 | 0.9 MB | plant/plane data |
| `hud_` | 5 | 0.5 MB | HUD elements |
| `skel` | 1725 | 0.5 MB | skeletons |
| `entc` | 1349 | 0.4 MB | entity classes |
| `font` | 5 | 0.4 MB | fonts |
| `zaf_` / `zaa_` | 39 / 45 | 0.3 / 1.4 MB | Zephyr animation sets / anim banks |
| `CLTH` | 17 | 0.2 MB | cloth |
| `gmod` | 1643 | 0.2 MB | game-model bindings |
| `port` | 14 | 0.2 MB | portals (region/visibility) |
| `lght` | 181 | 0.1 MB | lights |

## Two concrete confirmations

1. **Mission scripts live inside the game data as compiled Lua bytecode.** Example from `mission.lvl` → `scr_` chunk
   `setup_teams`, whose `BODY` starts with the Lua bytecode signature and contains `SetupTeams`, `soldier`, `pilot`,
   `assault`. So the browser build must either run the game's own Lua 5.0 VM (`LuaFL`, already compiling) or the
   `LuaCompiler` pipeline in the repo — this is why "swap in Fengari" was ruled out.
2. **AI route data is data-driven** (`path`, `wrld`, `port`, `coll`, `entc`), so a headless sim needs real data — the
   M1 build will exercise it directly.

## Commands

```bash
python3 tools/lvl_reader.py "data/.../\_lvl_pc/mission.lvl" --tree 3
python3 tools/lvl_reader.py "data/.../\_lvl_pc/fpm/all/allcldcr.lvl" --ids
python3 tools/lvl_survey.py "data/.../\_lvl_pc"
node browser/net_wire_check.mjs
python3 tools/native_wire_check.py   # the same engine code, compiled natively, against the same model        # net wire format, engine code against an independent model
python3 tools/coll_tree_check.py     # coll read the way CollisionMesh reads it; --wrong-lut is the control     # collision hulls
python3 tools/prim_shape_check.py    # prim/DATA driven through the engine's own Set functions; controls must fail  # collision shapes
```

## `tex_` layout, verified byte by byte (2026-11-16)

The earlier summary line above ("INFO, FMT_, FACE, LVL_ = mip levels") was a guess and was wrong
in a way that mattered: reading a FACE's payload as pixels produced an image shifted by the length
of its own header, which looks plausible and is incorrect. The real structure, read from
`core.lvl` and `yav/yav1.lvl`:

```
tex_
  NAME   NUL-terminated texture name
  INFO   8 bytes: engine flags, then a count that matches the number of FMT_ blocks
  FMT_   one per candidate encoding of the same image (the PC build kept alternatives)
    INFO   16 bytes:
           [0] encoding id - a real Direct3D 9 format value (see table below)
           [1] width | (height << 16)
           [2] mip count << 16 | depth-ish (0x00080001 = 8 mips for a 128x128)
           [3] usage flags
    FACE   one per cube face; for a 2D texture, one per mip level, largest first
      LVL_  a nested container (id 'LVL_' = 0x5f4c564c), NOT a pixel blob
        INFO  8 bytes
        BODY  the pixels of exactly one level, one image, uncompressed or block-compressed
```

`BODY` holds exactly `w*h*bpp` or `ceil(w/4)*ceil(h/4)*blockBytes` — verified against `white`
(4×4 `L8` → 16 bytes) and `default_spotlight` (128×128 `DXT1` → 8192, then 2048, 512, … as
further FACEs). A `BODY` much larger than that is a volume or texture array (`attenuation_volume`:
32×32 `A8R8G8B8` with a 131072-byte BODY = 32 slices), which the 2D path refuses and says so,
instead of uploading slice zero and calling the picture correct.

### Encoding ids are `D3DFMT_*` values

On PC these go straight into D3DX, so the id is the D3D9 enum. Confirmed from real data, not
assumed:

| id | format | how it was confirmed |
|---|---|---|
| 21 | `A8R8G8B8` | `cube_normalizationmap` 128×128, BODY = 65536 = 128·128·4 |
| 23 | `R5G6B5` | `hemisphere_normalizationmap` 256×256, BODY = 131072 = 2·w·h |
| 50 | `L8` | `white` 4×4, BODY = 16 |
| `'DXT1'` = 0x31545844 | DXT1 | `default_spotlight` 128×128, BODY = 8192 = blocks·8 |
| `'DXT3'` = 0x33545844 | DXT3 | `specularcubemap` 128×128, BODY = 16384 = blocks·16 |
| 60 | `V8U8` | `water_bumpmap_0..2` in `yav1.lvl`, 128×128, BODY = 32768 = 2 bytes/texel, and a bump map is exactly what a signed two-channel 16-bit format is for |

Two lessons recorded here because they were expensive:

* **`0x83f1` is `COMPRESSED_RGBA_S3TC_DXT1_EXT` (8 bytes per block), not DXT3.** A private
  numbering of DXT enums made a DXT3 payload get uploaded as DXT1, and WebGL rejected it with
  `GL_INVALID_VALUE` (1281) because the length did not match the format's block size.
* **`D3DFMT_A8R8G8B8` is BGRA in memory.** Converting 32-bit art without swapping gives an image
  that looks almost right and is wrong in every channel.

`GLForD3DFormat` in `browser/main.cpp` lists only ids confirmed from containers, with a comment
saying why: a missing encoding produces an honest
`no browser-side encoding (file offers: …)` in `tools/audit_textures.mjs`, while a wrong id
renders the wrong picture silently.

---

# `modl` / `segm`: level geometry, as decoded from the engine's own reader

Recovered 2026-11-17 by reading the engine rather than guessing at the bytes, then checked against
every file that holds the chunk. `tools/modl_probe.py` re-runs the check; `--json` prints one
parseable record per file, which is what `browser/node_test.mjs` and `browser/cdp_check.mjs` compare
the host's reader against. In that mode the human summary goes to stderr so stdout stays a single record.

The schema is transcribed from two functions and nowhere else:
`RedEngineFL/Graphics/PC/pcRedSegment.cpp::RedSegment::Read` (dispatches on child chunk id) and
`pcRedVertexFormat.cpp::CreateVertexDeclaration` (defines the RVF vertex-format bits that set the
stride). That is the discipline that made the result trustworthy: the stride is *derived* from the
format bits, then compared against the per-vertex size the file itself states.

```
ucfb
 ├─ modl  : NAME VRTX NODE INFO shdw segm… SPHR
 │   └─ segm : INFO MTRL RTYP TNAM×4 BBOX IBUF VBUF×1-3 BNAM
 ├─ tern  : NAME INFO LTEX DTEX DTLX SCAL AXIS ROTN HEXP HGT8 CUTS LUMI PCHS DCAL
 │   └─ PCHS : COMN PTCH×64                 (8×8 grid; each patch 64 units, 9×9 = 81 verts)
 ├─ wrld  : NAME TNAM SNAM INFO regn×n inst×712 BARR Hint
 │   └─ inst : INFO { XFRM(48) … } PROP      (XFRM = 4 rows of 3 floats = LoadUtil::ReadTransform)
 │                                          (in tat2, 2 further inst records sit inside
 │                                           `lvl_/@Sc*/wrld` - a group id the walk policy
 │                                           does not treat as a parent, so the host cannot reach them)
 └─ tex_  : NAME FMT_ FACE                    (already decoded; NAME is the join key)
```

## The two record bodies that carry the geometry

`VBUF` (and `VDAT` for the inline case) - self-describing, which is why nothing here is guessed:

| field | meaning |
|---|---|
| `uint32` | vertex count |
| `uint32` | **bytes per vertex** - stated by the file |
| `uint32` | RVF format bits (below) |
| bytes | `count * size` of vertex data |

`IBUF` is `uint32 numIndices` then that many `uint16` indices. `INFO` is
`(uint32 primType, uint32 numVertices, uint32 primCount)`.

`primType` is 5 in every segment of every container - **`D3DPT_TRIANGLESTRIP`**, not a triangle list.
The arithmetic says so on its own: for all 185 containers, `primCount == numIndices - 2`, which is
exactly the primitive count of a strip of `numIndices` indices. `primCount` is what to draw, and
`numIndices` is the allocation; the two differ by 2 per strip, so drawing `numIndices` reads padding.
Drawing the same index buffer as a triangle list produces a different mesh from the same bytes - and
both still look like a picture, which is why this had to be settled by arithmetic rather than by eye.

## RVF format bits

| bits | component | size |
|---|---|---|
| `0x0003` | position, `(v & 3) + 1` components | `4*components`, or 4/8 if `0x1000` (SHORT2/SHORT4) |
| `0x001C` | blend weight count `(v >> 2) & 7` | packed colour 4 if `0x2000` else `4*w - 4`; plus 4 for blend indices |
| `0x0020` | normal | 4 if `0x4000` (packed) else 12 |
| `0x0040` | tangent + binormal | 8 if `0x4000` else 24 |
| `0x0080` | diffuse colour | 4 |
| `0x0100` | specular colour | 4 |
| `0x0600` | texcoord set count `(v >> 9) & 3` | 8 each, or 4 if `0x8000` (SHORT2) |

The `0x1000 / 0x2000 / 0x4000 / 0x8000` bits mean "stored packed" per group.

## Evidence, not assertion

```
tat/tat2.lvl   vertex streams stride+payload agree 551, disagree 0   index streams 225 / 225 fit
yav/yav1.lvl   vertex streams stride+payload agree 292, disagree 0   index streams 130 / 130 fit
across every container that has segments (34 files)
               vertex streams agree 13,852, disagree 0                index streams 6,698 / 6,698
segment float-position min/max equal the stored BBOX: 225 of 225 (tat2)
```

The bbox check is the independent one: it does not care about strides, only that `pos:float3` sits
at offset 0 and is read in little-endian - if positions were misaligned, the extremes would not
reproduce the box the file records.

## The packed encodings, answered by the files themselves

The PC build stored two vertex streams for most segments: one packed, one uncompressed, holding the
same vertices in the same order. That lets the packed form be checked against the file's own answer
with no assumption at all. `tools/modl_packed_pair_check.py` and `tools/modl_decompress_check.py` do
it over all 185 containers:

| question | what the data answers |
|---|---|
| which box quantised positions | `modl`'s INFO model box, for 7,174 segments, against 1,486 for the segment's own recorded BBOX. Decode: `world = min + (short + 32767.5) * extent / 65535`, the inverse of the encode every effect uses (`short((v - s_vMin) * s_vScaleInv - 32767.5)`), with `s_vMin`/`s_vScale` taken from `RedModel::_BoundingBox`. 14,804 of 14,819 stream records reproduce the box the segment records. |
| packed normals | four bytes read as `z,y,x` - the memory order of a `D3DCOLOR` (`B,G,R,A`), the same reading `RedSegment::Read` uses for `MTRL` colours - mapped `byte/255 * 2 - 1`. Matches the file's own float normals on all 7,267 comparable segment pairs. |
| packed texcoords | fixed point at `1/2048` (12-bit). 7,235 of 7,236 comparable segments reproduce their own float UVs within one step; 1,188 vertices have UVs outside `±16`, which a 12-bit fixed point cannot hold, so they are reported as not checkable rather than counted as wrong. |
| vertex-group offsets | a group only occupies bytes when its bit is set. Adding tangent bytes when the tangent bit is absent shifts everything after it, which is how a texcoord decode can look plausible and read garbage. |

The model-box row identifies the **box**, not the rounding: it compares the decode against the file's
own float copy with a tolerance of one quantisation step, so it cannot see a half-step difference. The
terrain section below is exact vertex by vertex and is what pins the integer convention (q over
0..65535, offset 32768). Taken as an exact per-vertex test over all 185 containers, the model path
reproduces its own float positions for about half the compared vertices under any single convention
(2,430,470 of 4,958,793 with the model box, 976,943 with the segment box), so for models the box is
established and the rounding is not.

The same comparison runs in the host, in C, against the same decoder it uses for drawing:
`swbf2_model_selfcheck` decodes both streams of each segment with `DecodeVertex` and reports the
worst deviation per category in quantisation steps. On `tat2.lvl` and `yav1.lvl` it reports zero
disagreements in every category. That is the check that caught the two ways this was wrong the first
time it ran: normalising the position step as `1/65535` instead of `extent/65535` (which calls every
correctly decoded vertex bad whenever the box is wider than one unit), and normalising the byte step
as `1/255` instead of `2/255` (a signed range spans 256 values, so one step is `2/255`).

## Terrain patches: the scale and bias, pinned (2026-11-18)

Terrain patches hold the same vertices twice - a compressed stream (position as four shorts) and a
flat float stream (28 B: position + normal + specular) - so the quantisation can be tested as an
**encode**: quantise the float value with a candidate base and step and compare against the stored
short. A wrong base or step fails on most vertices, not on average. Over the 5,638 patches in the 10
terrain containers that carry patches which are a single 9x9 vertex group - 456,678 vertices - every
vertex of every one of them is reproduced on all three axes. `tools/tern_probe.py` does this run.

`tern INFO`, in the order `RedTerrain::Read` reads it:

```nasm
float fMetersPerPoint, fHeightScale, fHeightMin, fHeightMax
int16 iPointsPerDimension, iPatchPointsPerDimension, iHeightPatchPointsPerDimension,
      iNumLayers, iNumDecalTextures, iNumDecalTiles
```

and a `PTCH` record's INFO, per `pcRedTerrain.cpp::TerrainRenderable::Read`: `u8 numLayers`, that
many layer ids, then `int16 heightMin, heightMax, heightShift`.

```
patch record order   patch index in PCHS = iZ * patchesPerDimension + iX
vertex order         row-major (patchPoints+1)^2 grid
X, Z                 stored = round_half_up(v_local / (patchPoints * metersPerPoint) * 65535) - 32768
Y                    stored = (heightInt - patchHeightMin) * 2^patchHeightShift - 32768
                     heightInt from HGT8 + HEXP
```

The offset is 32768, not the `32767.5` in the decompiled encode. `q` runs over `0..65535` and the
stored shorts are exactly `q - 32768`; the Y row is exact integer arithmetic, so it shows the
convention directly. Rounding to nearest is what stores the half-step case as 32768. This data cannot
separate half-up from half-even: the only exact half-step value that occurs here rounds the same way
under both, so that part is not claimed.

The height lookup needs the **clamped** grid coordinate. `GetHeightUnscaled` masks the coordinate,
which resolves the last row or column of patches to point 0 of its height patch. Counted both ways on
the same vertices: clamped reproduces all 456,678; masked reproduces 453,654 and disagrees only on
the wrapped edge.

The three field order confirmations that make this not an assumption: `LUMI` is exactly
`iPointsPerDimension^2` bytes, `HEXP` is exactly `8 * iNumHeightPatches`, `PTCH` count is exactly
`iNumPatches`, `LTEX` holds exactly `iNumLayers` names, `DCAL` is empty exactly when
`iNumDecalTiles` is 0, and `HGT8` run-length data consumes exactly its chunk on every container.

Two things the model does not cover, reported rather than fitted: 570 patches are not a single 9x9
group (they carry seam vertices and several vertex groups, so pairing the two streams by index is not
defined for them), and the fourth short of the position group is 0 for 98.3% of vertices and is not
decoded here. The values that do occur are 0..255, which is the shape of a per-vertex weight, not a
position component - but that is not established.

## Net packet wire format, pinned from two implementations (2026-11-18)

`tools/net_wire_check.py` implements the packet rules from the source, and `spike/net_wire.cpp` runs
the engine's own functions in wasm. `browser/net_wire_check.mjs` compares them field by field and
exits non-zero on any disagreement. The point of two implementations is that a round-trip cannot
prove a format: an encoder and a decoder that are wrong in the same way still round-trip.

What the source states, and what the check now confirms both sides agree on:

* **The datagram is 5 header bytes then the payload**: `type`, `group` as two bytes little-endian,
  `idx`, `count | (compressed << 7)`. The 8-byte `PktHeader` is a view over the first bytes of the
  buffer, not the wire layout - on the wire `compressed` is one bit inside the count byte.
* **Buffer indices.** `BeginWrite` sets data index 14 and the byte cursor to 14; `PackHeader` sets
  `mCommIndex = data index - 5 = 9`; `Deliver` sends `mData + mCommIndex .. mMaxIndex`. A receiver
  places the datagram at `buffer + prefix` where prefix is 3, or 0 when the first byte - the type -
  is `>= 0x1e`, and `UnpackHeader` moves the data index to just after the 5 header bytes.
* **Bits are packed LSB-first inside 32-bit words**, the word is flushed when it fills, and the value
  is shifted after each part, so a field that crosses a word boundary is still read back correctly.
  Fields below 32 bits are therefore usually not word aligned - a reader that assumes alignment is
  wrong.
* **The byte cursor is shared** between the bit writer and raw bytes. `WriteBytes` pads the current
  word with zero bits first, so raw bytes always begin at a word boundary; `ReadBytes` discards the
  rest of the word it has already read before taking bytes. `WritePacked` allocates the packet on the
  first flush, so a group of only bit writes has no packet until a word completes.
* **Fixed fields reserve their endpoints**: code 0 means `fMin` and `(1<<bits)-1` means `fMax`, and an
  interior value can only reach `(1<<bits)-2`, which is the denominator `ReadFixed` divides by. A
  field of 32 bits or more is the raw IEEE bits instead (`doHiRes` forces that for all fields).
* **A group is one datagram per packet.** The packet limit is 116 bytes for a small packet and 1012
  for a big one; when a word does not fit, the current packet is closed and a new one starts, so
  splitting happens on word boundaries. `Send` numbers the group (`nextGroupNum`) and sets `count` to
  the number of packets and `idx` to each packet's position.
* **Fragments may arrive out of order.** `RecvGroup` matches on `(group, type, src)` and only
  completes when all `count` fragments are present; until then it holds them, and the reader gets them
  ordered by `idx`. The check exercises a two-packet group in both arrival orders, and reading raw
  bytes across the packet split.

The check's test vector is 11 operations: a 1-bit field, a 20-bit value that crosses a word boundary,
a full word, three fixed fields (interior, above max, below min), a 3-bit field that leaves a partial
word, raw bytes after it, a raw IEEE float, 100 raw bytes that force the packet split, then a 12-bit
field read across the split. Run:

```bash
./tools/build_net_wire.sh && node browser/net_wire_check.mjs
python3 tools/native_wire_check.py   # the same engine code, compiled natively, against the same model
```

Not established here: `NetPacket::Encrypt`/`Decrypt` return the packet unchanged in this tree, so the
check compares the bytes as written; whether a real build encrypts the payload is not answered by this
code. And the wire format is only the packet layer - the transport underneath it (`NetSocket`) still
has no browser implementation, so nothing here can be sent anywhere yet.

### The same code, three builds (2026-11-18)

Before two clients can be compared, the same code has to behave the same on both sides of the boundary
you actually ship to. Run:

```bash
python3 tools/float_corpus_check.py
```

102 quantisation cases, 80 height-path cases, 12 round-trips, built native with `-ffp-contract=off`,
native with `-ffp-contract=fast`, and as wasm. All three agree.

What the corpus is looking for, and what it found:

- dividing by the step and multiplying by its reciprocal agree on every case here. They are not
  guaranteed to agree in general, and the pinned model divides.
- half-up and half-even differ on 6 synthetic ties. The real terrain data contains exactly one tie and
  both rules round it the same way, which is why the data cannot pin the rule - so the check pins the
  *choice* instead: `roundf`, half away from zero, with `-ffp-contract=off`.
- one case differs between a fused multiply-add and the same expression left unfused. That is the
  reason the native build is pinned to `-ffp-contract=off`.
- the integer height path is exact in all three, which is what it has to be.

For the browser bridge: calling a JS callback through a C function pointer needs
`-sALLOW_TABLE_GROWTH`. Without it `addFunction` fails with
`RangeError: WebAssembly.Table.grow(): failed to grow table by 1`. The signature string's first
character is the return type, so `'iiiiii'` is int plus five parameters.

Two things the check now covers that it did not:

- **The fixed-field endpoints are not samples.** Code 0 means `fMin` and code `(1 << bits) - 1` means
  `fMax`; only interior codes are decoded as `code / ((1 << bits) - 2) * span + fMin`. A writer and a
  reader that map every code linearly round-trip fine and are still wrong about both endpoints.
- **`doHiRes` overrides the declared width.** With it set, `WriteFixed`/`ReadFixed` write and read a raw
  32-bit float whatever `iBits` says, so a field described as 7 bits becomes 32. Declared width and
  effective width are different facts and the check now reports both.

And a measurement rather than a check: the engine does this arithmetic in **float32**, step by step. A
model that does it in double is a stronger arithmetic and agrees only when the last bit happens to land
the same way. Counting interior codes, a double model differs on 86 of 126 for 7 bits in [-1,1], 263 of
1022 for 10 bits in [0,10], and 1110 of 2046 for 11 bits in [-16,16]. The model in this file was doing
it in double and passed because its one fixed case happened to coincide; it now computes each step in
float32, which is why code 17 decodes to `0xbf3aebae` and not `0xbf3aebaf`.

This matters for the game, not just for the check: entity positions arrive through
`ReadPositionVector`, which is three `ReadFixed` calls. So "do both clients get the same position" is a
float32 question before it is a network question.

## The type-15 replica update, pinned (2026-11-18)

`PKT_GAME_REPLICA_UPDATE` is 15. It is not in the list of types that do not route to the host, so
`FindSource` scans for it from destination 64 (measured: -1 before the peer is recorded there, 64
after).

What it carries is **input, not position**. `NetGameP::Recv` reads a destination mask, a turn, and
then per player: a client turn, 8 bits of lag, a switch response, and a list of moves. The positions
on the receiving side come from prediction, which is why the float corpus matters here before the
network does.

The layout, pinned against the engine's own readers and writers:

| field | encoding |
|---|---|
| destination mask | 32 bits then 32 bits - the mask is 64 bits wide |
| turn | 11 bits; `0x7ff` means "no turn", `0x7fe` means escape plus one bit; the low bits are then corrected against the high bits of the previous turn |
| client turn | the same encoding again |
| lag | 8 bits, and 128 is subtracted |
| switch response | 1 bit, then a turn |
| move | 1 bit "same" (copy the previous inputs), then 1 bit "end", then four fixed fields - stick x and y at 7 bits in [-1,1], mouse x and y at 11 bits in [-16,16] - then the buttons as 30 bits and 2 bits |

Two details that a self-consistent round trip would never catch:

- **The fixed fields reserve their endpoints.** Code 0 means `fMin` and code `(1 << bits) - 1` means
  `fMax`; only interior codes are decoded as `code / ((1 << bits) - 2) * span + fMin`.
- **The buttons are one word with two meanings.** `mBoolInputs` is 30 bits and `mMouseMode` is 2
  bits, and the wire carries them as 30 bits then 2 bits. Writing `0xFFFFFFFF` as if it were one
  field loses the top two bits to `mBoolInputs` being 30 bits wide - which is exactly what happened
  when this check was first written.

The check runs the engine's own code against an independent model:

```bash
./tools/build_type15_check.sh && node browser/type15_check.mjs
```

The readers and writers live in `NetGame.cpp`, whose global constructors reach into game code the
check does not need; linking the whole file means stubbing hundreds of names, and at that point the
check is measuring the stubs. So `tools/extract_type15.py` copies the eight functions out of the
fixed tree verbatim and records the source's sha256 in the generated file. The copy is mechanical: the
extractor finds each definition by its signature, copies the text through unchanged, and refuses to
write anything it cannot find.

## Position quantisation, pinned against the engine's own converters (2026-11-18)

Positions are not in the type-15 packet: type 15 carries input, and the positions on the receiving
side come from prediction. The quantisation below is the one `WritePositionVector` and
`ReadPositionVector` apply whenever the engine writes a position into a group.

The two halves are the engine's own code (`spike/position_convert_check.cpp`, run through
`browser/position_convert_check.mjs`) and a second reading of the same source
(`tools/position_convert_check.py`). What is compared is the bit width each side derives, the range
each side reads off the class, the codes each side produces, the bytes those codes make, and the
values each side decodes. The functions are copied verbatim out of `NetState.cpp` by
`tools/extract_position_convert.py`, which records the source hash; the packet layer is linked.

| converter | range | bits | used for |
|---|---|---|---|
| `PositionXZConvert` (NetState.cpp:103) | -10240.0f .. 10240.0f | `netLOD == 1 ? 32 : 21` | x and z |
| `PositionYConvert` (NetState.cpp:110) | -1024.0f .. 1024.0f | `netLOD == 1 ? 32 : 21` | y |

One vector therefore uses two different ranges: x and z go through `PositionXZConvert`, y through
`PositionYConvert`, in that order (NetState.cpp:2177 and 2185). `NetVectorBits()` (NetState.cpp:76)
belongs to `UnitConvert`, not to positions, and is not on this path.

What agreement showed, each item from engine behaviour rather than from a round trip:

- **The endpoints are codes, not values.** Code 0 decodes to `fMin` and code `(1 << bits) - 1` to
  `fMax`; interior codes divide by `(1 << bits) - 2` (NetPacket.cpp:442). A value outside the range is
  written as the endpoint code by `WriteFixed` itself (NetPacket.cpp:420), and `TrackValue` is empty
  (NetState.cpp:86) — nothing is recorded and nothing is clamped there.
- **The two reserved codes are not symmetric.** An interior value can reach code 0 by rounding down:
  `-10239.9990234375` is one float32 step inside the XZ range and is written as code 0, so it decodes to
  exactly -10240, not to itself. The top code cannot be reached from an interior value at all: the
  interior branch multiplies by `(1 << bits) - 2` and adds 0.5, so its largest result is
  `(1 << bits) - 2`, and only a value at or above `fMax` produces `(1 << bits) - 1`. That is a property
  of the arithmetic, not of the sample: it holds for any value strictly inside the range.
- **`netLOD` decides the path, and it is an input, not a result.** With `netLOD = 1` every component is
  32 raw IEEE bits, so a y of 5000 — outside the Y range — arrives unchanged, while the same value
  under `netLOD = 0` arrives as 1024. The harness sets `netLOD` through the engine's own `SetNetLOD`
  (NetState.cpp:290); the model is handed the same number and does not derive it. `doHiRes` is false on
  both sides, so the raw-IEEE branch here is entered only through `iBits >= 32`, which the wire section
  already pinned.
- **The rounding is float32, and it matters more here than for the small fields.** For 21 bits in
  [-10240, 10240] a model computing in double disagrees with the engine on **1,159,576 of the 2,097,150
  interior codes**; in [-1024, 1024] on **1,138,686 of the same 2,097,150**. Concretely, the value 9728.0
  quantises to code 2044721, which the engine reads as 9727.998046875 and a double model reads as
  9727.9970703125 — one bit apart. The model in `tools/position_convert_check.py` does every step in
  float32 for this reason.
- **Twenty position vectors** — 1260 bits — cross the 116 byte small-packet limit, so the group splits
  on a word boundary; both halves produce the same two datagrams and the same bytes.

The check can fail, and it was made to fail: with `--wrong-bits 20` the model claims 20 bits where the
source says 21, and all seven scenarios disagree with the engine (`node browser/position_convert_check.mjs`
prints that as the negative control). Without the negative control the check only shows that two
readings happen to match.

```bash
./tools/build_position_convert_check.sh && node browser/position_convert_check.mjs
```

Stubs: `spike/position_convert_stubs.cpp` defines `NetGameP::FreePackets()`, which the packet layer
calls when it releases a packet; the check does not run the game, and nothing in the stubs is read by
it. The only other added text is a block of accessors in the generated file, which return what the
engine's own converters report — the converter objects are file-static in the source, so nothing outside
that translation unit can ask one what bit width it uses. They model nothing.

Still not pinned here: values of `netLOD` other than 0 and 1 (the wire carries two bits, so 2 and 3 can
arrive through `ReadNetLOD`, NetState.cpp:309, and this check does not exercise them); who sets
`netLOD` for a given client in the game layer (NetState.cpp:1802 sets it to 1 for local clients); the
quaternion half of `ReadMatrix` (NetState.cpp:2202), which normalises through D3DX; and
`NetPktGroupClampFixed` (NetPacket.cpp:467), which is a different function from `WriteFixed` and orders
the multiplication differently, so it is not assumed to agree.

### netLOD reached through the wire path, and the 11-bit converter (2026-11-19)
The position scenarios above are unchanged and still agree. What was added is the path that decides
the bit width, run as the engine's own code, and `UnitConvert` as its own scenario.

`netLOD` is now observed rather than handed in. The harness calls the engine's `BeginNetLOD`
(NetState.cpp:184, both globals to 0 — where a connection starts), `SetNetLOD` (NetState.cpp:290),
`SetNetPrecision` (NetState.cpp:296) and `ReadNetLOD` (NetState.cpp:309); the model implements the same
four rules as a second reading, and the check compares the state each side reaches. The field is one
bit when the writer's `netLOD` equals `lastNetLOD` — "unchanged", and the reader keeps what it has —
otherwise it is a 0 bit followed by the value in two bits, and the reader sets `lastNetLOD` from it and
then sets `netLOD = lastNetLOD`. Two fields in one group, read twice: the first carries 1 and sets the
state, the second says unchanged and the reader stays at 1 (`observed` shows `[1,1]` then `[1,1]`).

Values 2 and 3 cannot be produced by `SetNetLOD`, which takes a bool, so only 0 and 1 can be written by
the engine's own path in this tree. The check says that plainly and puts the field on the wire as the
wire would carry it. What those values do to decoding is measurable: every value other than 1 takes the
long path, so 2 and 3 behave exactly like 0 — `UnitConvert` at 11 bits and the position converters at
21. Reading 2 sets `[2,2]` and reading 3 sets `[3,3]`, and both decode the same bytes.

`UnitConvert` (NetState.cpp:96) is `-1.0f .. 1.0f` with the width from `NetVectorBits()`
(NetState.cpp:76): 32 when `netLOD == 1`, 11 otherwise. It is a separate path from the position
converters and is not reached through them. At 11 bits the endpoints are reserved as before, so the
first three fields of the vector give codes 1535, 0 and 2047 for inputs 0.5, -1.0 and 1.0: the interior
code 1535 decodes to 0.5004887580871582, not to 0.5, while codes 0 and 2047 come back as exactly
-1.0 and 1.0. An input outside the range (-1.6) is written as code 0 and comes back as -1.0. The
negative control now fails on all twelve scenarios, including these.

The last-bit risk for this path is small because the numbers are small: for 11 bits in [-1, 1] a model
computing in double disagrees on **1,110 of the 2,046 interior codes**. Concretely, -0.9684750437736511
(bits `0xBF77EDFB`) quantises to code 32, which the float32 arithmetic reads as -0.968719482421875 and
a double model reads as -0.9687194228172302 — one bit apart, the same shape as the 21-bit case above.

Two things stay separate and are not merged here. `ConvertValues::TrackValue` (NetState.cpp:86) is empty:
it records nothing and clamps nothing; the endpoint codes come from `WriteFixed` itself, which writes 0
for any `f <= fMin` and `0xffffffff` for any `f >= fMax` (NetPacket.cpp:420). `NetPktGroupClampFixed`
(NetPacket.cpp:467) is a different function and is not on this path: it multiplies in the other order —
`(f - fMin) / (fMax - fMin) * (float)((1 << iBits) - 2) + 0.5f` where `WriteFixed` multiplies before
dividing — and masks the code with `uMax` before comparing, so it is not assumed to agree.

The field-to-converter mapping remains a reading of the source, stated as such: `WritePositionVector` does
not report which converter it used, so the engine half and the model each read NetState.cpp:2177 and
:2185 independently, and the bytes are what decides it — the two converters have different ranges and
different divisors, so a wrong order would show up as different bytes. The check also has one `netLOD`, as
the source does, so the reader continues from the state the writer left; that is not presented as two
machines.

Stubs: the same single stub, `NetGameP::FreePackets()`, and the same accessor block. The extractor now
copies fifteen definitions and eleven lines from the same file — `lastNetLOD` (NetState.cpp:165),
`BeginNetLOD`, `SetNetPrecision`, `ReadNetLOD`, `NetVectorBits`, `UnitConvert` and its instantiations,
`WriteUnitVector`/`ReadUnitVector` — and the source hash is unchanged: `dd8f0e57dc957d0e532bb1325cb06c84aee7fdfcb4f6f62ae7df3f59b841fa8c`.

Still open on this path: a group whose first field is the "unchanged" bit can only be written after an
earlier group already set `lastNetLOD`, so a reader that joins mid-connection is a multi-group question
and is not modelled here; per-client `netLOD` in the game layer (NetState.cpp:1802 sets 1 for local
clients); and which packet type actually carries positions, since the check uses type 12 only as header
input.

### The per-client rule, and the three converters that are not gated (2026-11-19)
The position scenarios and the netLOD scenarios above are unchanged and still agree; the check now runs
21 scenarios instead of 12.

`NetGame::IsLocalTo` (NetGame.cpp:4722) is run as engine text, not restated: `iOther < 0` answers false,
`iOther == iPlayer` answers true, otherwise it compares `iOther` against the one guest slot through
`NetGame::GetMyGuest` (NetGame.cpp:4702), which returns -1 when `iPlayer < 0`, `iPlayer >= 64` or
`iSlot != 0` and otherwise reads `netPlayers[iPlayer].myGuest[iSlot]`. Five cases: player 5 with -1 →
false, player 5 with 5 → true, player 12 with 44 where the table holds 44 → true, player 12 with 45 →
false, player 64 with 44 → false because the bounds check answers before the table, and the same client
again from a `netLOD` that was already 1, which leaves it at 1.

The site that acts on the result is `MountedTurret::Write` at NetState.cpp:1798, quoted exactly:

```c
const int clientId = NetGame::GetCurDst();
const int oldPrecision = netLOD;
netLOD = NetGame::IsLocalTo(clientId, mPlayerId) ? 1 : oldPrecision;
WriteThrust(group, mControlPitch);
WriteThrust(group, mControlTurn);
``` That
assignment is quoted here and applied by the harness, because `MountedTurret` is game layer and cannot be
run in this tree; what is run is the rule itself, and what is compared is the consequence for the bytes.
Which branch the engine's `IsLocalTo` took is not observable from outside, so only the value it returns is
compared, and the guest value is asked for separately. The class shell that lets the two functions be
called is not a copy — `NetGame.h` has the whole game behind it — and the guest table is an input both
halves are given identically, not a rule either side invented.

The same float, 0.3 (bits `0x3E99999A`), through eight converters. Only five of them read `netLOD`:

| converter | bits at netLOD 0 | bits at netLOD 1 | reads netLOD |
| --- | --- | --- | --- |
| `UnitConvert` (NetState.cpp:96) | 11 | 32 | yes, through `NetVectorBits()` (:76) |
| `PositionXZConvert` (:103) | 21 | 32 | yes |
| `PositionYConvert` (:110) | 21 | 32 | yes |
| `ThrustConvert` (:125) | 7 | 32 | yes |
| `AngleConvert` (:132) | 9 | 32 | yes |
| `SwitchConvert` (:118) | 4 | 4 | **no** |
| `JetFuelConvert` (:139) | 11 | 11 | **no** |
| `QuatConvert` (:89) | 13 | 13 | **no** |

What the check encodes 0.3 through is the four scalar converters, and the same value is written twice in
one group with the rule applied between them: `ThrustConvert` gives code 82 at 7 bits and the raw bits
1050253722 at 32, which decode as 0.3015873432159424 and 0.30000001192092896; `AngleConvert` gives code
279 at 9 bits and the same raw bits at 32, which decode as 0.29567933082580566 and 0.30000001192092896;
`JetFuelConvert` gives code 614 both times and decodes 0.3000977635383606 both times; `SwitchConvert`
gives code 9 both times and decodes 0.2857142686843872 both times. The bytes differ for the gated ones
and do not for the other two — that difference is the whole point of the comparison. What a reader cannot do
is read a group that mixes paths: a reader has one `netLOD` for the whole group, so the fields written at
7 and 9 bits come back as `3777268.5` and `0.0` to a reader at netLOD 1. That is why the writer and the
reader have to agree, which is what the precision field is for.

The quaternion half is pinned only up to the four decoded components. `QuatConvert` is 13 bits in
[-1, 1] (NetState.cpp:89), `WriteQuaternion` writes X, Y, Z, S in that order (:2220) and `ReadQuaternion`
reads them in that order (:2229). Inputs -0.99995, 0.4, 0.7, 1.0 give codes 0, 5733, 6962 and 8191: the
first is the reserved low code reached from inside the range by rounding, the last is `(1 << 13) - 1`, the
top code the reader returns as exactly 1.0. An interior case that a double model gets wrong is -0.7497
(bits `0xBF3FEC57`) → code 1025, which the float32 arithmetic reads as -0.7496947050094604 and a double
model reads as -0.7496947646141052. Counted over every interior code, a model computing in double
disagrees on **5,630 of 8,190** for 13 bits, **287 of 510** for 9 bits, **86 of 126** for 7 bits, **9 of
14** for 4 bits, and **0** for `JetFuelConvert` at 11 bits in [0, 1], whose divisor is a power of two.

What follows `ReadMatrix` (NetState.cpp:2202) is `D3DXQuaternionNormalize` (:2207) and
`D3DXMatrixRotationQuaternion` (:2208). Those are not engine code available to this check, so nothing here
normalises, and the length below is the model's own float32 arithmetic on what it decoded, not an engine
observation. What the reader gets is not unit length before anything normalises it: the components
-1.0, 0.3999999761581421, 0.7001221179962158, 1.0 have length 1.6279345750808716, and the second
quaternion has length 1.4873510599136353. The endpoint component is what makes that visible — a
quantised quaternion whose S is 1.0 and whose X is -1.0 cannot be unit length.

Two negative controls, both failing. A model told 20 bits disagrees on 21/21 scenarios. A model that
applies the raw-IEEE path to every converter at netLOD 1 — the mistake a host would make for `JetFuel`,
`Switch` and `Quat` — disagrees on exactly 2 of 21, and those are the two where a converter that the
source does not gate is used at netLOD 1. The runner exits non-zero on either.

Stubs added: none in the game layer. The only new text is the marked `NetGame` class shell and the guest
table in the generated file, and the `PblQuaternion.h` include for the quaternion functions. The extractor
now copies 32 definitions and 26 lines; the `NetState.cpp` hash is unchanged
(`dd8f0e57dc957d0e532bb1325cb06c84aee7fdfcb4f6f62ae7df3f59b841fa8c`) and the `NetGame.cpp` hash is
`e1c7f5b3836efcb5fe901d87b9a810d80306a46a4436e0b0c1256c6222b4cdee`.

Still open here: the game-layer half of `MountedTurret::Write` — which entity, which `mPlayerId`, and what
`GetCurDst()` was — is not run, only the rule and the quoted assignment; `NetPktGroupClampFixed`
(NetPacket.cpp:467) as its own path; and the half of `ReadMatrix` after the four fields.

### State across a group boundary, and a reader that joins mid-connection (2026-11-19)

The four functions are pinned above, but they only make sense across a sequence of groups: the field a
writer writes is only "unchanged" because of what an earlier group did. So the check now runs several
groups in one run and compares what each half had when it reached each boundary. Nothing is assumed about
the state at a boundary; both halves are read from the bytes.

What the engine produced, group by group, for `SetNetLOD(1)`, then a field, then a field that says
unchanged, then `SetNetLOD(0)`, then a field:

| group | the field says | writer arrived at | reader arrived at | reader left at |
| --- | --- | --- | --- | --- |
| A | a value, 3 bits | 0, 0 | 0, 0 | 1, 1 |
| B | unchanged, 1 bit | 1, 1 | 1, 1 | 1, 1 |
| C | a value, 3 bits | 1, 1 | 1, 1 | 0, 0 |

Group B carries no value, so the reader's 1 can only have come from group A. A group that arrives in two
packets is included as well: the state can only be taken from a group once it is complete, and then it has
to survive to the next one.

The order inside a group is what decides the fields, and the check runs both orders for the same value.
With the change before the field, the field carries it and the reader agrees: code 1050253722 at 32 bits
comes back as 0.30000001192092896. With the change after the field, as `MountedTurret::Write` does at
NetState.cpp:1802, the field said unchanged before the change happened, so the reader kept 0 and read 7
bits where the writer wrote 32: the first field comes back as 0.3015873432159424 (both used 7 bits there),
the second as -0.5873016119003296 and the third as -0.5714285373687744. Neither half is wrong: the writer
used the value it had at that moment, the reader read what the bytes said. The reader simply cannot
reconstruct that group, because the value the writer used was never carried.

A reader that joins with no earlier group has only what its own state already is, and that is measured
through the engine's own `BeginNetLOD` rather than assumed: it is 0, 0. Given the same bytes as a reader
that did see the earlier group, the two answer differently — 7 bits and -0.5873016119003296 for the fresh
reader, 32 bits and 0.30000001192092896 for the one that carried its state. Same bytes, different state.

Two more controls, both failing: a model that does not carry state between groups disagrees on 8 of 27
scenarios, and a model that reads the field before applying it disagrees on 4 of 27. The runner exits
non-zero if either one agrees on the scenario it should be wrong for. All 27 agree, and the five earlier
checks still exit 0.

No stubs were added for this and the generated file is unchanged: the harness calls the same copied engine
text as before, once per group, and the group-boundary bookkeeping is in the harness, not in the copied
code. Still open here: the game-layer half of `MountedTurret::Write` is still not run —
the check runs the rule and the quoted assignment, not the entity; and what a real connection does when a
reader joins in the middle of a live session is a different thing from this, which measures the state a
reader has when it has seen nothing.

## Collision data (`coll`): what the bytes allow, tested across all 1010 containers (2026-11-19)

There is no matching reader for `coll` in this tree — nothing in `vendor/swbf2decomp` calls a chunk
child by name — so everything below is read from the data and tested against the data. It is labelled
that way, and it is not a format fact confirmed by code.

What holds for all 1010 containers (`python3 tools/coll_probe.py`):

| part | shape | tested |
|---|---|---|
| `NAME` | NUL-terminated string, the asset name | 1010/1010 |
| `NODE` | NUL-terminated string, the object class name (e.g. `cube`) | 1010/1010 |
| `INFO` | exactly 40 bytes: 4 uint32 then 6 float32 | 1010/1010 |
| `POSI` | `word0` plain float triples, size = `word0 * 12` | 1010/1010 |
| `TREE` | `word1` `NODE` children and `word2` `LEAF` children | 1010/1010 |
| the 6 floats | min then max, and they are **exactly** the min and max of the stored vertices | 1010/1010 |

The last row is the one that matters, because it could have failed. A box that is the hull of its own
points is an internal consistency check: if the six floats were authored separately, or stored in the
other order, it would break. Reading them as max-then-min fails on all 1010, so the order is settled
by the data. `word0` being the number of `POSI` entries is the same kind of check: it is not inferred
from a plausible layout, it is the only reading that makes `size == word0 * 12` hold everywhere.

And **collision positions are not quantised**: `POSI` is plain float triples. Unlike `modl` segments
and `tern` patches, there is no encoding step to verify here, so any disagreement between native and
wasm on the collision path can only come from the arithmetic after the read, not from decoding it.

What the natural reading gets wrong, and that is the useful part: a leaf read as "count, then that
many indices" fails on **1002 of 1010** containers, and only 16,029 of 147,951 leaves have a payload
of `count * 3` bytes. Values inside leaves read as 3-byte integers are out of range of the vertex
count in every container. So the leaf layout is not pinned — a host that parsed `coll` the obvious way
would be wrong on about 99% of containers. `MASK` appears in 117 containers and is not per-vertex, so
it is not pinned either.

The node and leaf payload is the part that needs matching code before it can be called a format.

That matching code exists, and it is in this tree, so the payload can now be read the way the engine
reads it. See the next section.

## Collision hulls, read the way the engine reads them (2026-11-19)

`CollisionMesh.cpp:475` names the chunks as hashes — `NAME` `0x454D414E`, `TREE` `0x45455254`,
`NODE` `0x45444F4E`, `POSI` `0x49534F50`, `MASK` `0x4B53414D`, `INFO` `0x4F464E49` — and `ReadTree`
(`CollisionMesh.cpp:436`) says what each entry is:

```text
INFO   4 uint32: poslCount, numNodes, numLeafs, numIndices, then 6 floats, min then max
POSI   poslCount float triples
MASK   one byte
TREE   a prefix tree read with ReadNextChild
       branch entry: 6 bytes of compressed AABB, then two children
       leaf entry:   1 byte numVerts, 6 bytes of compressed AABB, then numVerts uint16 indices
NODE   a string, read and discarded
```

The earlier reading (`count, then that many indices`) is not what the code does, and it failed on 1002 of
1010 containers. With the code's grammar every entry parses exactly: 1010 containers, 146,941 branches,
147,951 leaves, 0 wrong sizes, 0 truncated trees, 0 indices out of range, and `word1`/`word2` match the
child counts in every container.

A compressed AABB is relative to its parent (`CollisionAABB::Init`, `CollisionMesh.cpp:183`), so the tree
is evaluated from the root down with the INFO box as the root's parent:

```text
boxmin = parent.boxmin + dim * lut[byte]
boxmax = parent.boxmin + dim * lut[byte]
```

and the table `u8tof32_hex[256]` is **not** `n/255`. It matches float32 `n/255` on 248 entries and differs
on 8 — `1, 2, 4, 8, 16, 32, 64, 128`, all powers of two, by one ULP. A model that computes the division is
not using the engine's numbers. That difference is below the resolution of any containment test (one ULP
against a quantisation step of `dim/255`), so the table can only be pinned by comparing the table itself,
which is what `--wrong-lut` does.

Measured across all 1010 containers, distances in quantisation steps for that node:

```text
child inside its parent        146941, out of range 0
leaf contains its vertices     145251 exactly, 1016 within one step, 1684 beyond one step
root is the full extent        1004 of 1010
```

The boxes are lossy by construction, so a vertex can sit outside its own box by up to about half a step,
and 1,016 do — consistent. The 1,684 beyond one step and the 6 roots that are not the full extent are real
disagreements between the data and the code's own decode: the code uses those boxes only for culling, so
the engine would skip polygons it should have tested. No cause is claimed.

Two of my own measurements were wrong before they were fixed, and that is worth keeping as a record: the
`inverted_bytes` flag was computed at the wrong offset for leaves, and the first attempt compared values by
unsigned bit pattern, which orders negative floats the wrong way round. Three runs gave three sets of numbers
until both were corrected.

The check: `tools/coll_tree_check.py` (plain, `--json`, `--wrong-lut` as the negative control, which exits
non-zero). `tools/coll_probe.py` stays as the earlier data-level reading.

One open item from the earlier section closes itself here. `MASK` was reported as "not per-vertex, and
`len == word0` fails on all 117" — the code reads it as a single byte (`mMask = child._pFile->Read8()`), and
all 117 `MASK` children are exactly 1 byte. The first byte was never a length; it is a value. The values
are 17, 19, 1, 16, 2, so it is a flag byte, and what it selects is still not established by code.

## Collision shapes, `prim`, driven through the engine's own Set functions (2026-11-19)

`CollisionBodyPrimitive::Read` (`CollisionBody.cpp:199`) reads each record as `Read32` + three floats and
routes it by type. The file type is **not** the enum value — `PrimitiveType` is `SPHERE=0, BUBBLE=1, BOX=2,
AXIS_ALIGNED_CYLINDER=3, CYLINDER=4`, and the switch routes `0/1/3` to `SetSphere`, `2` to `SetCylinder`,
`4` to `SetBox`. So:

```text
file type 0, 1, 3 -> SetSphere(f0)              mType = 0   (type 1 never appears in the data)
file type 2        -> SetCylinder(f0, f1)        mType = 4
file type 4        -> SetBox(f0, f1, f2, -1.0)   mType = 2
```

Fields written, per type, from the bodies:

```text
SetSphere     mType, mRadius, mMeshCollisionRadius, mHeight = r * 2.0f
SetCylinder   mType, mRadius, mHeight,
              mMeshCollisionRadius = sqrtf((h/2)^2 + r^2 + r^2)      <- two r^2 terms
SetBox        mType, mWidth, mHeight, mDepth, and for fRadius < 0:
              mRadius              = sqrtf(w^2 + d^2 + h^2)
              mMeshCollisionRadius = sqrtf(w^2 + h^2 + d^2)           <- different order
```

`CollisionPrimitive()` (`CollisionPrimitive.cpp:42`) initialises only `mName`, so `mWidth`/`mDepth` on a
sphere or cylinder keep whatever the object had. The harness fills every field with a sentinel before each
call, and the check asserts the sentinel survived — that is the falsifiable form of "the engine does not
write that field", rather than a model that assumes zero.

Across all 6,023 records the engine and an independent float32 model agree on every field the engine
writes:

```text
records            6023   types 0: 59, 2: 792, 3: 52, 4: 5120
fields compared    28309  disagreements 0, type mapping agrees on 6023
untouched fields confirmed untouched   1806   written anyway: 0
```

Controls that must fail, and do: a model that accumulates in double fails on **1418** records, one that
drops the second `r^2` in `SetCylinder` on **792**, one that omits `mHeight = 2r` for spheres on **59**,
and one that reads file type 3 as `AXIS_ALIGNED_CYLINDER` instead of `SetSphere` on **52**.

One measurement corrects something I said earlier. I reported "double `sqrt` vs float `sqrtf` differ on
595" — that was not a sqrt difference; Python cannot do a true float32 `sqrt`, so both sides of that
comparison were the same operation, and what differed was where the accumulation happened. Measured
correctly, on the same float32 input, correctly-rounded-to-float32 sqrt and double-then-round agree on
**all 6,023** records. So the observable difference is accumulation, not sqrt precision, and the check
reports the two separately. This build's own object files reference `sqrtf` only, so the float path is
what this toolchain produces; what retail did is not established by the source text.

Symbols the collision TU needs but has no body for in this tree — this is what limits Step 2:

```text
CollisionUtil::CollideBoxSphere, CollideCylinderSphere, CollideSphereBox, CollideSpheres,
GetClosestPointInPolygon, IsPointInsideAxisAlignedCylinder, RayHitBubble,
FindAACenteredBoxRayIntersection, CollisionAABB ctor, PblMatrix ctors, PblMatrix::GetInverseFast,
CollisionObject::sCollidingWithMesh, CollisionObject::sCollideTrisWithAllPrimitives,
EntitySoldier::rttiHashEntitySoldier, RedStack::sMem, RedStack::sMemIdx
```

Nine `CollisionUtil` functions do have bodies and are linked as real code: `CollideSphereAABB`,
`CollideSphereRay`, `CollideSpheres2`, `CollideBoxes`, `CollideCylinderBox`, `CollideBoxCylinder`,
`CollideAxisAlignedCylinders`, `CollideSphereCylinder`, `RayHitSphere`. `RedTerrain::GetHoleHeight` and
`GetInterpolatedNormal` have bodies in `RedTerrain.cpp` and are stubbed here only because this check does
not reach them; they matter for the terrain step.

One honest exception: the TU has a file-scope object `_IdentityMatrix` built with `PblMatrix`'s four-row
constructor, which is declared but not defined here, so the dynamic initializer runs before main and the
binary cannot start without some definition. The harness supplies a labelled host-side one and the measured
path never reads it.

The check: `tools/prim_shape_check.py` (`--json`), harness `spike/prim_shape_oracle.cpp`, generated stubs
`spike/prim_shape_stubs.cpp` from `tools/gen_prim_shape_stubs.py`, build `tools/build_prim_shape_oracle.sh`.

## What is still open

* **Terrain patch drawing.** The quantisation is pinned above, so the patch geometry can be decoded;
  the host still has no `tern` reader, and the layer blending (`LTEX`/`DTLX`/`SCAL` per layer,
  `AXIS` orientation, `LUMI`) is a separate question from position.
* **The fourth short** of the compressed position group (see above).
* **Model placement.** `wrld/inst/INFO/XFRM` holds a real 4x3 transform, and an instance names its
  model through `TYPE` in its INFO. For the instances the documented walk policy reaches in tat2,
  662 of 712 name a model in the same container and are placed; the other 50 name something the
  reader cannot resolve, which goes through `gmod`/`BASE`/`PRNT` - not followed yet, so those
  instances are counted and reported rather than guessed at. Instance TYPE strings were confirmed by
  reading actual names out of the data (`tat2_bldg_medium1`, `tat2_prop_prop4`) rather than by
  assuming the mapping.
* Terrain splat textures: `LTEX` names the layers (`tat2_main_1`, `tat2_main_2`), `DTLX` the detail
  map, `DTEX` the decal - the per-patch blend weights are not yet decoded.
