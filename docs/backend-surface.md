# Engine symbol closure — where the real work actually starts

**Status (supersedes the 750-symbol analysis that used to be here).**

`build/core.a` contains 732 objects, which is every translation unit in the prepared tree:

```
=== compiled 732 / 732 ===
```

Linking the **whole archive** into the browser module resolves every symbol:

```sh
./tools/measure_backends.sh
# whole-archive link: 0 unresolved symbols
```

That is the number the old version of this file said would take months. It was wrong for a
specific reason worth recording: it counted *demands* (symbols the archive references) instead of
*unresolved demands*, and it predated three changes that closed almost all of the gap:

1. **The platform directories are compiled, not excluded.** Graphics/PC, Platform/Pc, PebbleFL/pc,
   SoundFL/source/win all compile against the shim headers, so the code that *is* the backend
   contract stopped being a hole and became ordinary engine code that defines its own symbols.
2. **`NetGame.cpp` compiles.** It alone defined the `net*` registry globals referenced across the
   engine; while it failed, every one of those showed up as an unresolved symbol elsewhere.
3. The C++ runtime pieces the decompilation expects (`__cxa_guard_acquire`, sized `operator
   delete`) come from `-lc++abi-noexcept`, which matches how the engine objects are built.

Reproduce with `./tools/measure_backends.sh`. It links `-Wl,--whole-archive build/core.a` with
`ERROR_ON_UNDEFINED_SYMBOLS=0` and counts what is left, because the shipped module links clean for
a different reason: archive linking only pulls in the objects a caller actually reaches, so a
"0 undefined" build of the demo says nothing about the rest of the engine. Forcing every object in
is the only honest measurement. If the link fails, the script says so and writes the linker's
unresolved list to `build/backend-surface.txt` rather than reporting a vacuous zero.

**The count itself was the blind spot, until the game loop was linked in.** The script used to take
its number from `llvm-nm -u` on the probe's wasm. `llvm-nm` does not report a Wasm module's
function imports, so a link that left 206 symbols unresolved was counted as zero. The list is now
taken from the linker's own `warning: undefined symbol:` lines, which name every symbol and say
which object wanted it, and the browser's platform files are the same ones
`tools/build_browser.sh` links - so the number describes the build that ships. The zero above is
the first one this file has been able to state honestly.

## What the browser layer supplies to get there

These are the pieces the engine demands that the removed `pcMain.cpp` used to own. Each one is a
decision, not a stub, and each says why in its own file:

| what | where | why it is correct rather than a fudge |
|---|---|---|
| `hGlobalWnd`, `hGlobalInstance` | `browser/platform/pc_globals.cpp` | null. There is no Win32 window or instance handle. Fabricating one would let code act on a handle that cannot exist; null fails visibly. |
| `pcWindowed`, `pcWidth`, `pcHeight` | same | the page is always windowed; the host keeps the two sizes equal to the canvas drawing buffer. |
| `gAppActive`, `gIsQuitting`, `pcEnableLogging`, `pcPostInstall`, `pcForceFixedFunction`, `gAltMeleeHitEffects`, `gbMutexValid` | same | engine-shipped defaults; the Win32 single-instance mutex and post-install step have no browser analogue. |
| 21 DirectSound / Media Object / KS GUID constants | `spike/shim/win_guids.cpp` | all zero, on purpose. They belong to subsystems the browser replaces (WebAudio owns mixing, the media element owns decoding). Zero names no interface, which is true; published values would advertise interfaces nothing implements. |
| `RedAmbientLight::DebugDraw/IsVisible/SphereIntersect/AABBIntersect` | `browser/platform/redambientlight_browser.cpp` | the only bodies the decompilation never produced, and the last symbol in the way of a clean link. An ambient light has no position, direction or range, so no frustum/sphere/AABB test can reject it: the answers are `true`, matching what the engine itself does for the other position-less light (`RedDirectionalLight::IsVisible` returns true with no bounding region). `DebugDraw` is empty because it drew through the D3D9 debug renderer. |

## The game-loop seam, as the linker defined it (2026-11-18)

The host calling `GameState::UpdateTime()` is what brought the engine's game loop into the link,
and with it the whole engine: `GameState.o` reaches `GameLoop.o`, which reaches the entity, AI,
sound and renderer code. The link then named **149 symbols** the browser layer did not have. They
were not one problem but five, and each was measured before it was answered:

| group | count | where it is answered | what the answer is |
|---|---|---|---|
| `RedRenderer::` | 63 | `browser/platform/pcRedRenderer_browser.cpp` | state the game sets and reads back (fog, shadow length, camera, time base) is stored and returned; the defaults are the ones `pcRedRenderer.cpp` initialises the same fields to (shadow intensity 1.0, stencil shadow length 500.0, fog off, fog colour black); device-shaped calls answer "no device" (zero display modes, no depth/stencil match, no occlusion query). |
| `Snd::DSBuffer`, `Snd::StreamCaptureSource` | 25 | `browser/platform/dsound_browser.cpp` | there is no DirectSound device; `DirectSoundCreate8` fails with `DSERR_NODRIVER`, which is the path the engine's own sound engine already handles (it drops the mix config to `mixConfig_Disabled`), and a buffer that does not exist reports "not playing". |
| DirectSound / DirectInput / COM / D3DX | 9 | `browser/platform/dsound_browser.cpp`, `dinput_browser.cpp`, `com_browser.cpp`, `d3dx_browser.cpp` | "no such device": an enumerator that enumerates nothing, a COM apartment that initialises (one thread, one apartment) and an in-process server that cannot exist, a texture-requirements check that cannot be answered without a device. |
| Steam / Bink / Galaxy | 26 | `browser/platform/steam_browser.cpp`, `bink_browser.cpp`, `galaxy_browser.cpp` | the answers the engine already handles for "the platform SDK is not running": a zero pipe and user handle, a null client, a null movie, an offline Galaxy. |
| the engine's own PC entry points | 7 | `browser/platform/pcseam_browser.cpp` | `ToggleSoundEnabled`, `ToggleSoundPitch` and `ToggleAltMeleeHitEffectsCheat` are reproduced with the engine's own bodies and the engine's own flags (`sPauseMenuSoundEnabled`, `Snd::Source3D::sPitchMultiplier`); `PumpWaitingMessages` is empty because the browser's pump is the page's animation-frame loop; `std::_Xbad_alloc`, `std::_Xlength_error` and `__cxa_throw` are the MSVC CRT's helpers the decompiled source declares for itself (`GameState.cpp:219`) and report-and-stop, because the engine has no catch block anywhere. |
| C++ runtime | 9 | `-lc++` on the link line | `std::string`'s allocating members, `__hash_memory`, `__next_prime` and `this_thread::sleep_for` live in libc++, not in libc++abi; the engine's own calls to them were the unresolved ones. |
| Winsock | 1 | `browser/platform/winsock_c_browser.cpp` | the engine's `NetGame.cpp:8473` declares `closesocket` with C linkage and an `unsigned int` parameter, which is a different symbol from the C++-linkage `closesocket(int)` the shim declares; both are provided, and the C one does the same work. |

Two of those answers are worth naming because they are the ones a reader would otherwise suspect
of being fudges:

* **The renderer state is stored, not invented.** `SetFogRange` followed by `GetFogRange` returns
  the range that was set, because the browser layer keeps the same fields the D3D9 path keeps in
  `RedRenderer`'s statics. A browser that answered "0" to every getter would be indistinguishable
  from a game that had turned fog off.
* **The frame number counts frames.** `BeginFrame` increments it, so `GetFrameNumber` is the engine's
  own counter of frames presented rather than a constant.

After these, the strict whole-archive link reports **0 unresolved symbols** and the shipped
browser module links with the engine's game loop inside it:

```sh
bash tools/build_browser.sh          # -> build/browser/swbf2.{js,wasm}
bash tools/measure_backends.sh       # -> whole-archive link: 0 unresolved symbols
vendor/emsdk/node/24.19.0_64bit/bin/node browser/node_test.mjs
```

## What is actually left

Symbol closure is not behaviour. The engine can now be *linked*; nothing yet makes it *run a
match*. The remaining work is implementing the seams the engine calls into, and they are now
enumerated by call site rather than by guesswork:

* **Renderer.** `RedRenderer`/`RedDevice`/primitives and the shader effect classes need a WebGL2
  implementation behind the interfaces the engine already calls. The D3D9 device layer
  (`RedEngineFL/Graphics/PC/pcRedRenderer.cpp`) is the only TU excluded as the renderer's other
  half; its HLSL has no mechanical GLSL translation, which is why the seam is implemented rather
  than converted.
* **Audio.** `Voice`, `StreamAuxRenderer`, the DSound device plumbing and the DMO effect chain
  map onto WebAudio + the media element. Two sound TUs (`snddsbuffer.cpp`,
  `sndstreamcapturesource.cpp`) are excluded: decompiler defects plus the device plumbing the
  browser replaces.
* **File/save layer.** `PblFile` has a browser implementation (`browser/platform/pblfile_browser.cpp`)
  reading the real VFS; `FilePlatform`, `SaveDevice` and `Stream` still need one, which is what
  standing up a profile or a save game will exercise.
* **Input.** DirectInput device enumeration (`pcRedInputManager`) versus browser events.
* **Netcode.** The `net*` registry variables now exist (NetGame.cpp links), so the replication
  layer is present as *code*; the transport (`NetLink`/Winsock/GameSpy) needs WebSockets/WebRTC
  under it. The seam is `pcNetSocket.cpp`, which is the only place the engine actually touches a
  socket: `StartSockets`, `OpenBroadcastSockets`, `OpenDirectSocket`, `CloseDirectSocket`,
  `SendSocket`, `SendVoiceSocket`, `RecvSockets`, `RecvGalaxySockets`, `GetAddr`, `SetAddr`. A
  browser adapter has to satisfy those, nothing more. The packet layer above it links on its own -
  its dependency closure is `NetPacket` + `NetComm` + `NetDrag` (the QOS queues live in NetDrag) +
  `PblTimeBase`, plus one game-layer callback (`NetGameP::FreePackets`) that a check stubs. The wire
  format itself is pinned by `browser/net_wire_check.mjs` (see data-format.md); what is missing is a
  way to put those bytes on a wire.
* **Third party.** Bink video, GameSpy Galaxy, Logitech LCD: stubbed as linkable, unimplemented
  as behaviour. Bink matters (movies gate some UI), the LCD panel does not.
* **Model format.** The decompiled tree has no model-file reader - `RedModel.cpp` holds runtime
  structures and no TU parses `segm` - so the vertex layout had to come from the data, read through
  the two functions that do define it (`pcRedSegment.cpp::RedSegment::Read` for the chunk dispatch,
  `pcRedVertexFormat.cpp::CreateVertexDeclaration` for the RVF bits and the stride). The reader lives in the browser host (`browser/main.cpp`), not in the engine, and is
  checked against an independent oracle (`tools/modl_probe.py --json`) plus against the files
  themselves (`swbf2_model_selfcheck`, which decodes each segment twice: the packed stream and the
  uncompressed copy of the same vertices).
  What the host deliberately does *not* reimplement: the view matrix, the projection and the frustum
  test are the engine's `RedCamera`, and the instance transform is read as `LoadUtil::ReadTransform`
  reads it. What is still host-side is shading - one fixed light plus ambient, because there is no
  light list in this data path yet, and the engine's per-material pipeline (`MTRL` flags beyond the
  lit bit, `BMAP`, `SKIN`, tangent sets) is read but not used.

Two engine symbols still carry deliberate semantic choices instead of real implementations:
`hGlobalWnd` (no window exists) and `RedAmbientLight::DebugDraw` (no debug renderer exists). Both
are inert rather than wrong, and both are named in the files above.

## The renderer seam, as the linker defined it

`pcRedRenderer.cpp` is the one engine unit held out of the build - it *is* the Direct3D9 device.
While nothing reached for it, that cost nothing. The moment the engine's own `RedCamera` was used
from the browser host, its dependency closure (`pcRedPrimitive`, `pcRedVertexBuffer`,
`pcRedIndexBuffer`, `pcRedVertexFormat`, `pcWaterShader`, `PerformanceTimer`) made the demand
concrete, and the linker enumerated 36 symbols the browser layer then has to answer. Those answers
live in `browser/platform/pcRedRenderer_browser.cpp`, and each is one of exactly three kinds:

* **A real value, from a real source.** The viewport comes from the canvas drawing buffer; the
  camera and projection come from the `RedCamera` the scene is drawn with; the time base is a real
  `PblTimeBase`; `TransformWorldPointToProjectionSpace` delegates to the
  camera's own method; the render options are `RenderOptionsT`'s own defaults.
* **Null, where a device, surface, texture, shader or register block would have to exist.** Callers
  that use the result fail immediately and visibly. A fabricated pointer would let them get further
  and produce a scene that looks rendered and is not.
* **A no-op with one log line**, for state-setting entry points the engine calls hundreds of times a
  frame. The log is emitted once, so the absence is visible in the page log without drowning it.

Capability queries (`pcGetRenderCaps`, `pcGetDeviceCaps`, `pcGetAdaptorIdentifier`) return
zero-filled structs: literally accurate when there is no device to report capabilities, and unlike
optimistic values they cannot make the engine take a path that then fails.

Two conventions the browser layer had to learn from the code rather than assume, both now covered
by an assertion in `browser/cdp_check.mjs`:

* **The camera's Z axis points away from what it looks at.** `RedCamera::ClassifySphere` computes
  `fZ = -(Zrow . (point - eye))` and treats `fZ` as forward depth. Setting Z along the look
  direction puts every object behind the near plane, and the engine's own culling then rejects the
  whole scene - reported as `drawn 0`, which looks like a rendering bug and is not one.
* **`RedCamera::ComputeProjMatrix` is D3D9-shaped** (row-major in the decompiler's layout, clip `w`
  taken from view-space `z`, near plane to `z_ndc` 0) and cannot be uploaded to GL as-is. The host
  builds the GL projection from the engine's `GetFrustum`, so the field of view and clip planes stay
  the engine's and only the matrix form changes - which is the device layer's job, not a redefinition.

The proof that the seam is honest is not a screenshot. `swbf2_depth_order_test` renders two
known-colour quads at two depths into an offscreen target with the depth buffer attached, in both
draw orders: the nearer colour must own the centre pixel, and swapping the depths must swap it.
A mirrored matrix, an inverted `z`, a missing translation or no depth test each fail it while still
producing a plausible picture. Current result: `redNear 0xFF0000`, `greenNear 0x00FF00`.

## The level-load seams, as the loader defined them (2026-11-19)

Loading a level through the engine's own loader (`LoadUtil::ReadDataFile`) brought two more
questions to the browser layer, and both are answered by the seam rather than by a workaround
around the engine.

**Where the file is.** The engine builds `data\_lvl_pc\<name>` (`LoadUtil.cpp:468`) and asks for
`MISSION.lvl` in upper case; the data on disk is a tree (`tat/tat2.lvl`) with a lower-case name,
and the browser's virtual file system is flat (`tat_tat2.lvl`). The seam
(`browser/platform/pblfile_browser.cpp`) tries the tree form and the flat form and resolves the
name case-insensitively one directory deep, because the engine's own file layer is case-sensitive
where the data is not. The native oracle uses the same seam over a real directory, so the two
builds read the same bytes.

**A read with no destination.** The terrain's renderable asks the renderer for an index buffer
(`pcRedIndexBuffer.cpp:41`); the renderer seam answers honestly that there is no Direct3D9 device,
so there is no buffer, and the engine reads the index bytes into it anyway
(`pcRedIndexBuffer.cpp:199`). The file seam skips those bytes and keeps the file position in step
(`fseek` past them), and says so once in the log. Without it the load dies in the middle of the
level's terrain, in both builds, which is how the seam's answer was found.

The ground oracle (`docs/ground-oracle.md`) is the check for both of these.

## The transport seam, as the check defined it (2026-11-18)

`pcNetSocket.cpp` is the only place the engine touches a socket, so the browser layer has to supply the
whole surface: `StartSockets`, `OpenBroadcastSockets`, `OpenDirectSocket`, `CloseDirectSocket`,
`SendSocket`, `SendVoiceSocket`, `RecvSockets`, `RecvGalaxySockets`, `GetAddr`, `SetAddr`. `spike/shim`
declares them; `browser/platform/winsock_browser.cpp` is the implementation the check runs against.
There is no raw UDP in a browser, so this layer maps engine socket calls to whatever channel the host
has, and it has to fabricate a stable `(ip, port)` pair per connection because `GetAddr`, `SetAddr` and
`FindSource` make that pair the peer identity.

Two conventions the check corrected rather than confirmed:

* **The send path counts any socket error; the receive path does not count `WSAEWOULDBLOCK`.**
  `SendSocket` calls `DecodeError(WSAGetLastError())` on `SOCKET_ERROR`, while `RecvSockets` tests
  `if (error != WSAEWOULDBLOCK)` before calling it. "No data" still has to be an error so the engine's
  loop ends, but it is not a counted error. The model said both were counted; the engine said 0 for the
  receive path, and the engine was right.
* **One update asks for data 101 times when 100 datagrams are waiting**: 80 that it processes, 20 that it
  discards, and one that returns `WSAEWOULDBLOCK`. Delivering everything at once is not the same as
  handing the engine 80 per update, so the queue has to belong to the transport, not to the engine.

What this check cannot measure through the socket layer, stated as a limit rather than a result: the
receive prefix for types below `0x1e` is decided by `NetCommP::RecvPacket`, which routes every real
packet type into the game layer. Without the game layer only `PKT_NONE` and above can be received, so
prefix 0 is measurable and prefix 3 is not - prefix 3 is pinned by `browser/net_wire_check.mjs`, where
the packet layer is exercised on its own.

The same net code also runs without emscripten. `python3 tools/native_wire_check.py` compiles the same
sources with g++ and compares the engine's output against the same model: three readings of the source
agree, and the native one is the one that can be debugged with gdb. Two things make the decomp compile
natively - `-D__int64="long long"` (g++ has no `__int64`; the size is the same, so layouts are
unaffected) and a small native shim holding a clock, one flag and the game-layer entry points the code
calls. Nothing about the semantics changes between the two targets, which is the point.

```bash
./tools/build_net_transport_check.sh && env -u NODE_OPTIONS node browser/net_transport_check.mjs
python3 tools/native_wire_check.py
```

The build script's first link is strict and writes the socket layer's unresolved closure to
`build/net_transport_undefined.txt`. Those names are the seam: they are what this layer owes the game
layer and the Galaxy API. `spike/net_transport_stubs.cpp` contains stand-ins for them so the check can
run, with the values the code would see with no game state - and `hostshort = 3658` is not a stand-in,
it is the engine's own default from `NetAdmin.cpp`, which the check reports as measured.
