# The browser demo: what runs, and how to check it yourself

Open <http://127.0.0.1:8123/> after `./tools/demo.sh`.

What you are looking at is the decompiled 2005 engine compiled to WebAssembly, reading the real
game files from the 2005 `GameData` dump, drawing through WebGL2, and running the game's own Lua
bytecode on the engine's own Lua 5.0.2 VM. Nothing is reimplemented for the demo: the container
walk is `PblFile`/`PblFileChunk` from `PebbleFL`, the VM is `LuaFL`, the texture reader decodes the
bytes the engine's own `FMT_` chunks describe.

The texture/model viewers remain available, but there is now also an interactive single-player combat slice:
open <http://127.0.0.1:8123/?mission=1>. An Imperial stormtrooper and four CIS battle droids stand
on `tat/tat2`. The engine builds the player and targets, moves the player, runs the chase camera,
creates and simulates real `OrdnanceLaser` blaster bolts, and applies genuine collision and damage.
Visual 3D glowing laser bolts (blue for the Empire, red for CIS droids) are rendered live in WebGL2,
dead droids fall to the sand, and the HUD tracks health, ammo, and remaining targets across waves.

Mission controls:
* `WASD`: Move trooper
* Pointer-lock mouse: Look and steer
* `Space`: Jump (or respawn when defeated)
* `Ctrl` or `C`: Roll
* `Shift`: Sprint
* `LMB` (Left Mouse Button): Fire weapon
* `1`, `2`, `3`: Switch weapons (Blaster Rifle, Blaster Pistol, Thermal Detonator)
* `R`: Next Wave (when droids cleared) or Respawn (when defeated)

Click the canvas to capture pointer lock. The first user gesture starts WebAudio for blaster sound effects.

The original texture/model demo is still below. It is not a playable game by itself; what it
proves is the data path, format readers, script VM and GPU upload path on real game content.

## Run it

```sh
./tools/demo.sh                # builds if needed, regenerates the manifest, serves on :8123
```

Controls: `←/→` or `A/D` page through textures, `M` builds a montage, `3` opens the 3D view, `G`
switches the 3D view between real model geometry and the resident textures, `Esc` returns to the
flat view, `P` walks a container's chunks, `L` runs its scripts. In the 3D view the same keys drive
the camera instead: drag or arrows orbit, wheel or `Q`/`Z` zoom. Clicking any file in the list
stages it -
files stream over HTTP on demand and are written into the wasm VFS under `/data`; nothing is
preloaded, and only `core.lvl`'s 7 MB or so is resident by default.

The 3D view is not a reimplementation of a camera: the host posts a camera-to-world matrix to
`RedCamera::SetMatrix`, asks the engine for its view transform and frustum, and lets
`RedCamera::ClassifySphere` decide what is visible. The HUD reports those counts (`drawn` and
`culled` are the engine's answers, not the host's). What the host does add is the projection
matrix in GL's form - the engine's own `ComputeProjMatrix` is written for D3D9, taking clip `w`
from view-space `z` and mapping the near plane to 0 - built from the engine's `GetFrustum`, so
field of view and clip planes stay the engine's.

Query parameters: `?file=tat/tat2.lvl` picks the container staged at startup, `?probe=1` logs the
chunk layout of the next three textures, `?quiet=1` skips the startup demo.

## Model geometry (`modl` / `segm`)

`swbf2_build_models(path)` is the host's reader for level geometry: it walks `modl` and its `segm`
children, decodes each segment's vertex stream through the format bits the file states, reads `IBUF`
indices, `MTRL`, `TNAM` and `BBOX`, and places instances by resolving `inst/INFO/TYPE` to a model in
the same container. The decode rules are the engine's - see `docs/data-format.md` - and the drawing
path is the engine's camera: `RedCamera::GetFrustum`, `ComputeProjMatrix` and `ClassifySphere` decide
what is drawn, exactly as in the billboard view.

Two things it does not pretend to have: a light list (there is none in this data path, so shading is
one fixed direction plus ambient and the report says so), and the engine's material pipeline (`BMAP`,
`SKIN`, tangent sets are read as bytes and not used). Segments with no normal group are flat-shaded
from the screen-space derivative pair, which is exact for a triangle and needs no invented vertex data.

A normal run on the demo container:

```
models: 87 models, 225 segments, 35307 vertices, 56516 indices from /data/tat_tat2.lvl;
instances placed: 662 (50 could not be resolved), segments reachable through placed instances: 1923,
textures named: 60. Segments are triangle strips (prim type 5), drawn as strips.
```

The two checks that make this more than a picture:

* **The file answers for itself.** `swbf2_model_selfcheck` decodes each segment twice - the packed
  stream and the uncompressed copy of the same vertices that the same segment stores - with the same
  decoder, and asks whether they agree to the resolution the format can carry, one quantisation step
  per category. On `tat2.lvl` and `yav1.lvl` it reports zero disagreements in every category. This is
  the check that caught two real mistakes: a position step normalised as `1/65535` instead of
  `extent/65535`, and a signed-byte step normalised as `1/255` instead of `2/255`.
* **Two independent readers must agree.** `tools/modl_probe.py --json` parses the same containers with
  a separate implementation and tallies models, segments, vertices, indices, declared primitive counts,
  instance resolution and the totals reachable through placed instances. `browser/node_test.mjs` runs
  the reader with no browser at all (the reader needs no renderer) and compares it against that oracle;
  `browser/cdp_check.mjs` compares the same numbers from inside the browser runtime and then asserts on
  the drawing path.

The two views share the same camera, so they are exclusive: paging a texture or building a montage
leaves the model view, and `Esc` returns to the flat view. `swbf2_clear_cache` drops the model path
with the resident set, because the geometry draws against textures that no longer exist.

The drawing assertions in `cdp_check.mjs` are: the draw path visits exactly the segments the build
recorded as reachable through placed instances; with the camera pulled back nothing is culled, so the
primitives and indices it draws equal the oracle's placed totals; the strip arithmetic holds
(`indices == prims + 2 × segments`); the engine's frustum test culls some when the camera is close;
and the geometry changes the framebuffer relative to the flat view. Running the checks:

```sh
./tools/demo.sh &                                                   # serve
vendor/emsdk/node/24/bin/node browser/node_test.mjs                # reader + oracle, no browser
vendor/emsdk/node/24/bin/node browser/cdp_check.mjs --wait 160    # drawing path in real Chrome
```

Both exit 0 on a current build. `node --check` and the plain `node` on this machine are broken by an
unrelated `NODE_OPTIONS` import hook (an undici version mismatch), so the checks are run with the node
that emsdk installs: `vendor/emsdk/node/24.19.0_64bit/bin/node`.

## What a normal run shows

```
engine ready — pick a .lvl on the right
ucfb: /data/tat_tat2.lvl container, 26279448 bytes (header says 26279440), 384 chunks visited, 60 tex_, 0 scr_, 87 modl, 6 lvl_
tex 'tex_0027': 512x512 DXT3 -> GL 0x83f2 (65568 bytes)
gl: uploaded S3TC texture 512x512 fmt 0x83f2 len 65568
montage · 24 textures from tat/tat2.lvl drawn at once · encodings on screen: DXT1, DXT3, A8R8G8B8
```

The HUD's `gpu` readout is the engine sampling **its own framebuffer** with `glReadPixels` inside
the same frame it drew (`SampleFrame` in `browser/main.cpp`). Reading pixels from JavaScript after
the frame is composited returns the cleared buffer, which is why the first version of this page
reported a black canvas while it was drawing correctly.

## Verified numbers (2026-11-16)

| | |
|---|---|
| Engine translation units compiled | **732 / 732** (`build/results.tsv`) |
| Objects in `build/core.a` | 732 |
| Undefined symbols, whole-archive link | **0** (`./tools/measure_backends.sh`) |
| Textures decoded from real containers | **2514 of 2515 decodable**, 187 MB of pixel data, 180 containers ≤80 MB (`./tools/audit_textures.mjs --all`) |
| Encodings handled | DXT1 1237, DXT3 1159, V8U8 96, A8R8G8B8 19, R5G6B5 2, A1R5G5B5 1 |
| Refused honestly | 1 volume texture (`attenuation_volume`, 32×32×32) — reported as multi-slice rather than drawn wrong |
| Mission scripts executing | `mission.lvl` **18 / 18**, `ingame.lvl` 6 / 9 |
| Model reader, against the independent oracle | `tat2.lvl` 87 models / 225 segments / 35,307 vertices / 56,516 indices — agree; `yav1.lvl` 34 / 130 / 20,078 / 33,651 — agree |
| Model decode self-check | **0 disagreements** in every category on both containers (positions, normals, texcoords, diffuse) |
| Frame rate, headless Chromium + SwiftShader (software GL) | ~58–60 fps at 1169×813 |
| Containers available to stream | 185, 3.1 GB total, staged on demand |

`ingame.lvl`'s three failures are real: `ifs_sideselect`, `ifs_charselect`, `ifs_mapselect` index
values that only exist once the engine's Lua bindings are registered. The host installs a fallback
`_G.__index` returning a callable/indexable "any" node so that unrelated control flow runs, but a
script that does arithmetic on a genuinely engine-provided value cannot be satisfied by a stub, and
faking it would hide the very gap the port needs to close. The failures are printed instead.

## How the headless check works

`browser/cdp_check.mjs` drives Chrome over the DevTools protocol using node's built-in WebSocket —
no npm dependencies, no extension. It loads the page, waits for the demo to finish, pages through
textures, builds the montage, and asserts on the engine's own framebuffer sample plus the page's
log:

```sh
./tools/demo.sh &                                      # serve
vendor/emsdk/node/*/bin/node browser/cdp_check.mjs --wait 160 --textures 6
# { "ok": true, "pageErrors": [], "montage": { "litPermille": 980, "colors": 427, "resident": 24 }, ... }
```

It also captures `Log.entryAdded` and `Inspector.targetCrashed`, because a renderer crash or a
second document otherwise reads as a stale DOM and wastes an hour. `tools/audit_textures.mjs` is the
same module run without WebGL to measure the readers rather than the renderer.

## Reaching it from another machine

`./tools/demo.sh` binds `0.0.0.0`, so the page is reachable from anything on the same network. On
startup the server prints every address it can be reached on, with the interface that carries the
default route listed first and Docker/VM bridges marked, so the address to type is not a guess:

```
listening on 0.0.0.0:8123
open one of these from any device on the same network:
    http://192.168.2.87:8123/
    http://172.18.0.1:8123/   (not the default route)
    http://127.0.0.1:8123/   (this machine)
```

`./tools/demo.sh 8123 127.0.0.1` (or `--host 127.0.0.1` directly) keeps it loopback-only again.
Verified: page, wasm, manifest, a 32 MB container and a `bytes=0-15` range all served over the LAN
address, and the whole demo loaded in Chrome from `http://192.168.2.87:8123/` at 60 fps.

Two things worth knowing, both learned from other people's debugging sessions rather than this one:

* **Plain `http://` is enough today, and will stop being enough when input lands.** Nothing the page
  uses is restricted to secure contexts. `requestPointerLock` (mouse-look) and
  `navigator.getGamepads()` *are* secure-context-only in Chrome, so the moment the game consumes
  mouse and gamepad, a phone or laptop reaching this over LAN on `http://` will be refused those
  APIs, and the bug will look like "the mouse just does nothing". The fix is HTTPS with a local
  certificate, not a code change - plan for it instead of discovering it.
* **A router can block this regardless of the server.** Nothing here filters traffic (`nft list
  ruleset` is empty, no firewalld, no ufw), but guest-network isolation or "AP client isolation"
  silently drops device-to-device traffic, and the symptom is indistinguishable from a bind
  mistake. Check the page from the serving machine's own LAN IP before blaming the router.

## What the demo is not

* Not the game's renderer. Model geometry is decoded and drawn through the engine's camera, but
  shading is the host's single fixed light plus ambient: there is no light list in this data path,
  and `BMAP`/`SKIN`/tangent sets are read as bytes and not used. Terrain (`tern` patches) has no
  reader yet, and animation, collision and the entity layer are not there.
* No audio, no input to the engine, no netcode transport.
* No gameplay: nothing calls into `GameLogic` yet, because the renderer and save/audio seams it
  calls through are not implemented.
