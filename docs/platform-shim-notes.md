# Platform (PC) shim pass — what was added, and what still does not compile

Scope of this pass: make the 100 previously-excluded PC translation units in `/tmp/excl2.txt`
compile for `wasm32-unknown-emscripten` by extending the headers in `spike/shim/` only. Nothing
under `build/src_fixed/`, `vendor/`, or `tools/` was touched, and no engine identifier was masked
with a macro.

## Measured result

| measurement | command | before this pass | after this pass |
|---|---|---|---|
| platform scope (100 TUs from `/tmp/excl2.txt`) | `bash /tmp/measure_pc.sh` | `OK: 36  FAIL: 64  TOTAL: 100` | **`OK: 82  FAIL: 18  TOTAL: 100`** |
| core scope (the standing measurement) | `bash tools/measure.sh` | `TOTAL ok: 373 / 638` (first measured just after this pass's first edit, i.e. not a pre-pass baseline) | **`TOTAL ok: 373 / 638`** |
| shim autogen convergence | `python3 tools/shim_autogen.py 8` | `converged: 372/638 files syntax-clean` | `converged: 372/638 files syntax-clean` (byte-identical `generated_types.h`) |

Both numbers were measured in this session against the final state of the shim, with
`/tmp/measure_pc.sh` (which compiles each platform TU through `tools/compile_one.sh`, i.e. with
exactly the core build's flags and object output) and with `tools/measure.sh`.

Note on the core number: `docs/shim-layer-results.md` records 637/638 as of 2026-11-15 21:55. The
`build/src_fixed` tree was regenerated after that (tree mtime 2026-11-15 23:59,
`tools/prepare_tree.py` mtime 2026-11-16 00:05), and the regenerated tree measures **373/638**
before and after this pass. Every remaining core error is inside engine code — the top kinds are
`use of undeclared identifier 'SoldierAnimationBank'` (150), `member access into incomplete type
'ZephyrSkeletonShared'` (43), `expected ';' after class` at `Battlefront2/Source/HUDItem.h:64` (42),
`variable/field type 'Thread' is an abstract class` (25), `unknown type name 'Combo'` (3). None of
them names a shim header or a shim symbol, and no core error is attributable to the shim: a scan of
all 638 core diagnostics for errors located inside `spike/shim/**` or mentioning any symbol added
here returns nothing. Restoring 637/638 is a question for whoever owns `tools/prepare_tree.py`, not
for the shim layer.

## Files changed / created

| file | change |
|---|---|
| `spike/shim/d3d9.h` | Direct3D9 vocabulary: 158 constants, corrected pre-existing values, `D3DGAMMARAMP` made the Microsoft aggregate, added descriptor structs/fields, added `Direct3DCreate9`. |
| `spike/shim/d3dx9.h` | **new.** Stands in for the D3DX9 umbrella header (`pcRedTexture.cpp` includes it) and declares the texture-requirement checks. |
| `spike/shim/d3dx9math.h` | `D3DXMATRIXA16` made a real 16-byte-aligned matrix type, plus `D3DXVec4Transform` / `D3DXVec4TransformArray`. |
| `spike/shim/dsound.h` | DirectSound 8 vocabulary: 87 constants, descriptor structs, capability-bit fields, interface identifiers, creation/enumeration entry points, `DSDENUMCALLBACK`. |
| `spike/shim/mediaobj.h` | `IMediaObject` / `IMediaObjectInPlace` interfaces, their interface identifiers, `DMO_SET_TYPEF_TEST_ONLY`, and `dmo.h`'s `DMO_MEDIA_TYPE` / `DMO_OUTPUT_DATA_BUFFER` layouts corrected. |
| `spike/shim/objbase.h` | **new.** Stands in for `<objbase.h>`: `CLSCTX_`, `CLSID`, `CoInitialize`, `CoCreateInstance`, and the COM identifiers the DirectSound path passes around. |
| `spike/shim/swbf2_prefix.h` | Win32 host vocabulary the PC files still name: `winerror.h` codes, file/handle API, registry, thread priorities, the user32/gdi32 calls the window and Logitech-LCD code makes, wide CRT path helpers, `guiddef.h` GUID comparison, `IKsPropertySet` signature fix. |
| `spike/shim/generated_types.h` | Only through `tools/shim_autogen.py`: 87 further interface members in the tool's own permissive form. |

Everything added is inside the headers' existing `#ifdef __cplusplus` guard where it models C++ COM
APIs; the Win32 constants/functions added to the prefix are plain C so the prefix stays includable
from C.

## Why the shim needed this

The retained PC code is the D3D9 device layer (`RedEngineFL/Graphics/PC`), its effect/shader
wrappers (`Graphics/PC/Shaders`), the DirectSound backend (`SoundFL/source/win`), the PC save
device / PBL file loader, the window bootstrap and the Logitech LCD driver. None of it was compiled
before this pass, so none of its vocabulary had been harvested. The gap was closed in the order the
build made it visible: first the includes that could not be found at all (`<d3dx9.h>`,
`<objbase.h>`), then the constants the code names, then the COM members it calls, then the Win32
host API it calls.

`tools/shim_autogen.py` was re-run after every batch (that is the project's documented flow). It
now converges with **zero** pending additions and rewrites `generated_types.h` byte-identically
(md5 `2baec0ec85836b0668e98afa1421240f` across three runs), so everything below is stable: the tool
will not re-invent a name a shim header declares, and the headers do not restate anything the tool
owns.

## Symbols added, and why

### `spike/shim/d3d9.h`

Value corrections to declarations that already existed (the old values were not Microsoft's, so a
backend decoding them would have mapped the wrong state id): `D3DFMT_A8R8G8B8`→21,
`D3DFMT_X8R8G8B8`→22, `D3DFMT_R5G6B5`→23, `D3DFMT_D24S8`→75, `D3DFMT_G16R16F`→112,
`D3DFMT_A16B16G16R16F`→113, `D3DMULTISAMPLE_NONMASKABLE`→1, `D3DTOP_ADD`→7, `D3DTOP_ADDSIGNED`→8,
`D3DTOP_BLENDTEXTUREALPHA`→13, `D3DTA_CONSTANT`→0x00000006, `D3DSAMP_MAXANISOTROPY`→10,
`D3DRS_SHADEMODE`→9, `D3DRS_ALPHAREF`→24, `D3DRS_STENCILENABLE`→52, `D3DRS_STENCILFUNC`→56,
`D3DRS_COLORWRITEENABLE`→168, `D3DRS_BLENDOP`→171, `D3DRS_SCISSORTESTENABLE`→174,
`D3DRS_DEPTHBIAS`→195, `D3DRS_CLIPPING`→136, `D3DUSAGE_WRITEONLY`→0x00000008,
`D3DUSAGE_DEPTHSTENCIL`→0x00000002, the `D3DCMP_*` and `D3DFOG_*` orderings.

Types:

| symbol | why |
|---|---|
| `D3DGAMMARAMP` (now `struct { unsigned short red[256], green[256], blue[256]; }`) | `Platform/Pc/pcColorControl.cpp` writes `ramp.red[i]`, `ramp.green[i]`, `ramp.blue[i]` and hands the address to `SetDeviceGammaRamp`. As `float[3]` the member accesses were errors. This is also Microsoft's actual shape (`d3d9types.h`). |
| `D3DBOX`, `D3DLOCKED_BOX`, `D3DCLIPSTATUS9`, `D3DRASTER_STATUS`, `D3DDEVICE_CREATION_PARAMETERS`, `D3DRECTPATCH_INFO`, `D3DTRIPATCH_INFO`, `D3DRECT9`, `D3DFONT` | descriptor/parameter structs the PC renderer and the retained `pcRedRenderer.h` name. |
| `D3DVERTEXBUFFER_DESC::FVF`, `D3DPRESENT_PARAMETERS::{hDeviceWindow, Windowed, EnableAutoDepthStencil, ...}`, `D3DVOLUME_DESC::Usage`, extra `D3DCAPS9` fields (`StencilCaps`, `TextureOpCaps`, …) | the platform files fill these structs in field-by-field; each missing field was a separate `no member named` error. |
| `IDirect3DVertexDeclaration9`, `IDirect3DVolumeTexture9`, `IDirect3DBaseTexture9`, `IDirect3DIndexBuffer9`, `IDirect3DQuery9`, `IDirect3DCubeTexture9` forward declarations | `pcRedVertexFormat.cpp`, `pcRedTexture.cpp` and the shader wrappers hold pointers to them. |

Macros / functions:

| symbol | why |
|---|---|
| `MAKEFOURCC`, `D3DCOLOR_ARGB`, `D3DPS_VERSION`, `D3DVS_VERSION`, `D3DDECL_END()`, `D3DTS_WORLDMATRIX(i)` + `D3DTS_WORLD`, `D3D_SDK_VERSION`, `MAKE_HRESULT`, `_FACD3D`, `MAKE_D3DHRESULT`, `D3D_OK`, `D3DERR_DEVICELOST`, `D3DERR_DEVICENOTRESET`, `D3DERR_DRIVERINTERNALERROR` | each one appears verbatim in the platform sources (`pcRedVertexFormat.cpp` builds declarations with `D3DDECL_END()`, `pcMain.cpp`/`pcRedRenderer.cpp` compare `hr` against `D3D_OK` and the device-lost codes). |
| `IDirect3D9* Direct3DCreate9(unsigned sdkVersion)` | the only entry point the retained code calls on the SDK object itself. Declared, not defined: the WebGPU backend owns it. |

158 further constants were added (all values are Microsoft's, cross-checked against the enum bodies
as carried in Wine's `d3d9types.h` / `d3d9caps.h`, because these numbers are the contract the WebGPU
backend will decode). They are listed name-by-name in Appendix A. Families and what asked for them:

| family | asked for by |
|---|---|
| `D3DFMT_*` | `pcRedTexture.cpp`, `pcRedVertexBuffer.cpp` (`D3DFMT_VERTEXDATA`), `pcRedSurface`, `pcRedDeviceEnumerator.cpp` |
| `D3DDECLTYPE_*` / `D3DDECLUSAGE_*` / `D3DDECLMETHOD_*` / `MAX_D3DVERTEXELEMENTS` | `pcRedVertexFormat.cpp` (vertex declarations) |
| `D3DRS_*`, `D3DCOLORWRITEENABLE_*` | `StateManager_DX.cpp`, `pcRedStencilShadow.cpp`, `Shaders/*` |
| `D3DTSS_*` / `D3DTSS_TCI_*` / `D3DSAMP_*` / `D3DTTFF_*` / `D3DTA_*` / `D3DTOP_*` / `D3DMCS_*` | `pcRedShader.cpp`, `pcNormalShader.cpp`, `pcWaterShader.cpp`, `pcStencilShadowShader.cpp` |
| `D3DFVF_*` | `pcRedPrimitive.cpp`, `pcRedVertexBuffer.cpp` |
| `D3DUSAGE_*`, `D3DLOCK_*`, `D3DSWAPEFFECT_*`, `D3DPRESENT_INTERVAL_*`, `D3DMULTISAMPLE_*` | device creation / present parameters in `pcRedRenderer.cpp`, `pcRedDeviceEnumerator.cpp` |
| `D3DCAPS_*`, `D3DDEVCAPS_*`, `D3DPMISCCAPS_*`, `D3DPTEXTURECAPS_*`, `D3DTEXOPCAPS_*`, `D3DSTENCILCAPS_*` | capability queries in `pcRedAutoSettings.cpp` and `pcRedDeviceEnumerator.cpp` |
| `D3DTS_*`, `D3DZB_*`, `D3DSTENCILOP_*`, `D3DCMP_*`, `D3DPT_*`, `D3DFILL_*` | fixed-function state plumbing in `StateManager_DX.cpp` |

### `spike/shim/d3dx9.h` (new)

| symbol | why |
|---|---|
| the header itself | `RedEngineFL/Graphics/PC/pcRedTexture.cpp` does `#include <d3dx9.h>`; without the file the TU died before reaching anything else. It includes `d3d9.h` and `d3dx9math.h`. |
| `D3DXCheckTextureRequirements`, `D3DXCheckCubeTextureRequirements`, `D3DXCheckVolumeTextureRequirements` | `pcRedTexture.cpp` asks the device what texture size/mip count/format is acceptable before creating it. Declared with Microsoft's signature; the WebGPU backend answers the question. The two cube/volume variants are here because they are the same call in the same file family and the header would otherwise be inconsistent — they cost nothing and are named by the same include. |

D3DX9's maths library is *not* shimmed as a library: `d3dx9math.h` already supplies inline
implementations (that is what `spike/shim/dxmath_c.cpp` backs), which is why only these entry
points needed declaring.

### `spike/shim/d3dx9math.h`

| symbol | why |
|---|---|
| `D3DXMATRIXA16` as `struct D3DXMATRIXA16 : D3DXMATRIX` (was `typedef D3DXMATRIX D3DXMATRIXA16`) | `Graphics/PC/pcRedSegment.cpp` and `pcRedStencilShadow.cpp` declare `static const D3DXMATRIXA16 _IdentityMatrix(1.0f, 0.0f, ... /* 16 floats */)`, which a typedef could not express: `no matching constructor for initialization of 'const D3DXMATRIXA16'`. Microsoft's own type is a 16-byte-aligned derived class, so this is the same shape. |
| its constructors: 16 floats, `const D3DXMATRIX&`, `const float*`, scalar fill; and `operator=(const D3DXMATRIX&)` | the 16-float form is the one the two files above use; the others keep the existing call sites (assigning a `D3DXMATRIX` to a `D3DXMATRIXA16`, and `pcWaterShader.cpp` writing through `D3DXMatrixMultiply(&_IdentityMatrix, ...)` — that name is const-looking but is mutated, so the type must stay mutable there). Derived→base conversion is implicit, so no conversion operator was added; clang warns that such an operator would never be used. |
| `D3DXVec4Transform`, `D3DXVec4TransformArray` | called by `Shaders/pcWaterShader.cpp` / the effect code. Implemented inline next to the other `D3DXVec*` helpers. |

### `spike/shim/objbase.h` (new)

| symbol | why |
|---|---|
| the header itself | `SoundFL/source/win/snddsdmo.cpp` and friends include `<objbase.h>`. |
| `CLSCTX_INPROC_SERVER` (0x1), `CLSCTX_INPROC_HANDLER` (0x2), `CLSCTX_INPROC` (their OR) | `sndengine.cpp:941` passes `CLSCTX_INPROC_SERVER`, `snddsdmo.cpp:82` passes `CLSCTX_INPROC` — both were `use of undeclared identifier`. |
| `typedef GUID CLSID` | `CoCreateInstance`'s first parameter, and named directly in `SoundFL`. |
| `CoInitialize`, `CoUninitialize`, `CoCreateInstance` | called by the DirectSound/DMO setup path. Declared only: there is no COM runtime on wasm32, and the DMO effects `CoCreateInstance` would build are the WebAudio backend's job. |

Interface identifiers declared (not defined) here and in `mediaobj.h`: `IID_IMediaObject`,
`IID_IMediaObjectInPlace`, `IID_IMediaBuffer`, `IID_IUnknown`, `IID_IKsPropertySet`. Microsoft
declares these in headers and defines them once in `uuid.lib`; they stay declarations until the code
that owns the COM/audio boundary is linked in. Nothing in this spike links them yet, so no GUID
bytes were invented — see "Open items".

### `spike/shim/dsound.h`

Structs added so the win/ backends can fill them in (field names and order follow `dsound.h`):
`DSBCAPS`, `DSCBUFFERDESC`, `DSCBUFFERDESC1`, `DSCEFFECTDESC`, `DSFXI3DL2Reverb` +
`typedef DSFXI3DL2Reverb DSFX_I3DL2REVERB`.
Structs extended: `DSCAPS` (Microsoft's capability/memory fields, on top of the four legacy ones),
`DSBUFFERDESC` (`GUID guid3DAlgorithm`), `DS3DBUFFER` (cone/distance fields
`dwInsideConeAngle`, `dwOutsideConeAngle`, `vConeOrientation`, `lConeOutsideVolume`, …).

| symbol | why |
|---|---|
| `_FACDS` (0x878), `MAKE_DSHRESULT`, `DSERR_*` (19 codes) | `snddsbuffer.cpp`, `snddsbufferrenderer.cpp`, `sndvoice.cpp` test `hr` against them. |
| `DSBCAPS_*`, `DSBPLAY_*`, `DSBSTATUS_*`, `DSBVOLUME_MIN/MAX`, `DSCAPS_*`, `DSCBCAPS_WAVEMAPPED`, `DSCBSTART_LOOPING`, `DSSCL_*`, `DSSPEAKER_*`, `DS3D_IMMEDIATE`/`DS3D_DEFERRED`, `DSFX_I3DL2REVERB_*` (23 range constants) | buffer creation flags, `Play`/`Lock`/`SetPosition` arguments, `GetStatus` tests, the EAX/I3DL2 reverb parameter clamps in the effect code. All 87 values are Microsoft's (checked against `dsound.h` line by line: the only differences left are the four `DSERR_*` that the shim had invented, now fixed). |
| `SPEAKER_*` (18 channel-mask bits) | `WAVEFORMATEXTENSIBLE::dwChannelMask` is written with them in `snddsbuffer.cpp`. |
| `DSDENUMCALLBACK` typedef + `DirectSoundCreate`, `DirectSoundCreate8`, `DirectSoundCaptureCreate8`, `DirectSoundEnumerateA`, `DirectSoundCaptureEnumerateA`, `GetDeviceID` | `snddsdeviceenumerator.cpp` passes `BOOL (LPGUID, LPCSTR, LPCSTR, LPVOID)` callbacks; the parameter type is exactly that, so the typedef's first parameter is `GUID*`, not `const GUID*` (DirectSound's own enumeration callback is non-const). `DirectSoundCreate8` etc. are declared for the device objects; the WebAudio backend supplies them. |
| `IID_IDirectSound8`, `IID_IDirectSound3DListener`, `IID_IDirectSound3DBuffer`, `IID_IDirectSoundFXI3DL2Reverb8`, `DSDEVID_DefaultPlayback`, `DSDEVID_DefaultVoicePlayback`, `DS3DALG_DEFAULT`, `GUID_DSFX_STANDARD_*` (8) and `GUID_DSFX_WAVES_REVERB` | named by name in the effect/device code (`IDirectSoundCreate8(&DSDEVID_DefaultPlayback, ...)`, `CreateEffect(GUID_DSFX_STANDARD_I3DL2REVERB, ...)`). |

### `spike/shim/mediaobj.h`

| symbol | why |
|---|---|
| `IMediaObject` (+24 members), `IMediaObjectInPlace` (+3 members) | `snddsdmo.cpp` / `sndstreamdsdmo.cpp` drive the DMO effect pipeline (`mObject->ProcessInput(...)`, `SetInputType(...)`). Until now `IMediaObject` was `typedef int` from the generator, so every member access was an error. Members are in `generated_types.h`'s permissive no-op form (`template <class... _A> long Method(_A&&...) { return 0; }`) — the browser build replaces the pipeline with WebAudio nodes. |
| `IID_IMediaObject`, `IID_IMediaObjectInPlace`, `IID_IMediaBuffer` | `QueryInterface`/`CoCreateInstance` arguments in the same files. |
| `DMO_SET_TYPEF_TEST_ONLY` (0x00000200) | `snddsdmo.cpp` passes it to `SetInputType`. |
| `GetInputSizeInfo`, `GetInputBuffering` on `IMediaObject` | `sndstreamdsdmo.cpp` sizes the input stream before allocating (`no member named 'GetInputSizeInfo' in 'IMediaObject'`). |
| `DMO_MEDIA_TYPE::cbFormat` (added) and `IMediaBuffer* pUnk` → `IUnknown* pUnk` | `MediaType` in `snddsdmo.h` derives from `DMO_MEDIA_TYPE` and `memset`s it by `sizeof`, and `snddsdmo.cpp:74` writes the inherited `cbFormat` — which only type-checked once the field existed. |
| `DMO_OUTPUT_DATA_BUFFER` given its fourth field (`rtTimelength`) and `pBuffer` renamed to Microsoft's | `snddsdmo.cpp:35` aggregate-initialises four elements: `DMO_OUTPUT_DATA_BUFFER output = { pMediaBuffer, 0, 0, 0 };` |

### `spike/shim/generated_types.h` (through `tools/shim_autogen.py` only)

87 interface members were harvested from the platform diagnostics and written in the generator's own
format, so a plain rerun reproduces them exactly. Per interface:

| interface | members |
|---|---|
| `IDirect3D9` | `CheckDepthStencilMatch`, `CheckDeviceFormat`, `CheckDeviceMultiSampleType`, `CheckDeviceType`, `CreateDevice`, `EnumAdapterModes`, `GetAdapterCount`, `GetAdapterIdentifier`, `GetAdapterModeCount`, `GetDeviceCaps` |
| `IDirect3DDevice9` | `ColorFill`, `CreateCubeTexture`, `CreateDepthStencilSurface`, `CreateIndexBuffer`, `CreateOffscreenPlainSurface`, `CreatePixelShader`, `CreateQuery`, `CreateTexture`, `CreateVertexBuffer`, `CreateVertexDeclaration`, `CreateVertexShader`, `CreateVolumeTexture`, `DrawIndexedPrimitive`, `DrawPrimitive`, `GetAvailableTextureMem`, `GetBackBuffer`, `GetDeviceCaps`, `GetDisplayMode`, `GetFVF`, `GetFrontBufferData`, `GetRenderState`, `GetRenderTarget`, `GetSamplerState`, `GetTexture`, `GetTextureStageState`, `GetTransform`, `LightEnable`, `SetGammaRamp`, `SetLight`, `SetMaterial`, `SetPixelShaderConstantF`, `SetTransform`, `StretchRect` |
| `IDirect3DSurface9` | `GetDesc`, `LockRect`, `UnlockRect` |
| `IDirect3DBaseTexture9` | `GetLevelCount`, `GetType` |
| `IDirect3DTexture9` | `GetLevelDesc`, `GetSurfaceLevel`, `LockRect`, `UnlockRect` |
| `IDirect3DCubeTexture9` | `GetLevelDesc`, `LockRect`, `UnlockRect` |
| `IDirect3DVolumeTexture9` | `GetLevelDesc`, `LockBox`, `UnlockBox` |
| `IDirect3DVertexBuffer9` / `IDirect3DIndexBuffer9` | `Lock`, `Unlock` |
| `IDirect3DQuery9` | `GetData`, `Issue` |
| `IDirectSound` | `CreateSoundBuffer`, `SetCooperativeLevel` |
| `IDirectSound8` | `GetCaps`, `GetSpeakerConfig`, `Initialize`, `SetCooperativeLevel`, `SetSpeakerConfig` |
| `IDirectSoundBuffer8` | `AcquireResources`, `GetCaps`, `GetCurrentPosition`, `GetFormat`, `GetStatus`, `Lock`, `Play`, `Restore`, `SetCurrentPosition`, `SetFormat`, `SetFrequency`, `SetPan`, `SetVolume`, `Stop`, `Unlock` |
| `IDirectSoundCapture8` | `CreateCaptureBuffer` |
| `IDirectSoundCaptureBuffer8` | `GetCurrentPosition`, `Lock`, `Start`, `Stop`, `Unlock` |
| `IDirectSound3DListener` | `CommitDeferredSettings`, `SetDistanceFactor`, `SetDopplerFactor`, `SetRolloffFactor` |
| `IDirectSound3DBuffer` | `SetAllParameters`, `SetConeAngles`, `SetConeOrientation`, `SetConeOutsideVolume`, `SetMaxDistance`, `SetMinDistance`, `SetPosition`, `SetVelocity` |
| `IDirectSoundFXI3DL2Reverb8` | `SetAllParameters` |
| `IDirectInput8A` / `IDirectInputDevice8A` | `CreateDevice`, `EnumDevices` / `Acquire`, `EnumObjects`, `GetDeviceState`, `GetProperty`, `Poll`, `SetCooperativeLevel`, `SetDataFormat`, `SetProperty`, `Unacquire` |

### `spike/shim/swbf2_prefix.h`

Win32 scalar codes (`winerror.h`, `winnt.h`): `MAKE_HRESULT`, `ERROR_SUCCESS`,
`ERROR_INVALID_FUNCTION`, `ERROR_FILE_NOT_FOUND`, `ERROR_PATH_NOT_FOUND`, `ERROR_ACCESS_DENIED`,
`ERROR_INVALID_HANDLE`, `ERROR_NOT_ENOUGH_MEMORY`, `ERROR_INVALID_PARAMETER`,
`ERROR_CALL_NOT_IMPLEMENTED`, `ERROR_SERVICE_NOT_ACTIVE`, `ERROR_ALREADY_INITIALIZED`,
`RPC_S_SERVER_UNAVAILABLE`, `RPC_S_CALL_FAILED`, `E_UNEXPECTED`, `E_FAIL`, `E_NOTIMPL`, `E_POINTER`,
`E_ABORT`, `E_ACCESSDENIED`, `E_OUTOFMEMORY`, `E_INVALIDARG`, `CLASS_E_NOAGGREGATION`, and the
structured-exception codes `EXCEPTION_ACCESS_VIOLATION`, `EXCEPTION_DATATYPE_MISALIGNMENT`,
`EXCEPTION_FLT_DIVIDE_BY_ZERO`, `EXCEPTION_FLT_INVALID_OPERATION`, `EXCEPTION_STACK_OVERFLOW`.
*Why:* the PC crash handler (`pcMain.cpp`) and the DSound path compare against these by name; the
values are Microsoft's so that a code a backend has to translate means the same thing on both
sides.

Files/handles (`winbase.h`, `winnt.h`) — `INVALID_HANDLE_VALUE`, `GENERIC_*`, `FILE_SHARE_*`,
`CREATE_NEW`/`CREATE_ALWAYS`/`OPEN_EXISTING`/`OPEN_ALWAYS`/`TRUNCATE_EXISTING`, `FILE_BEGIN`/
`FILE_CURRENT`/`FILE_END`, `FILE_ATTRIBUTE_*`, `FILE_FLAG_*`, `FILETIME`, `ULARGE_INTEGER`,
`WIN32_FIND_DATAA`/`W`, and the calls `CreateFileA`, `ReadFile`, `WriteFile`, `GetFileSize`,
`SetFilePointer`, `SetEndOfFile`, `FlushFileBuffers`, `DeleteFileA`, `GetDiskFreeSpaceA`,
`GetDiskFreeSpaceExA`, `GetDiskFreeSpaceExW`, `FindFirstFileA`/`W`, `FindNextFileA`/`W`,
`FindClose`, `CreateDirectoryA`
*Why:* `Battlefront2/Platform/Pc/pcSaveDevice.cpp` (save slots, free-space check),
`Platform/Pc/pcPblFile.cpp` (loose-file search) and `SoundFL/source/win/sndfileplatform.cpp`
(all of it), which each do a full Win32 file loop.

Registry (`winreg.h`) — `HKEY_LOCAL_MACHINE`/`HKEY_CURRENT_USER`/`HKEY_USERS`/`HKEY_CLASSES_ROOT`/
`HKEY_CURRENT_CONFIG`, `KEY_*` access masks, `REG_*` value types, `RegOpenKeyExA`, `RegCreateKeyExA`,
`RegQueryValueExA`, `RegSetValueExA`, `RegCloseKey`.
*Why:* `RedEngineFL/Graphics/PC/pcRedAutoSettings.cpp` reads the auto-detected quality settings, and
`lglcd/lgLcd.cpp` reads its device configuration. On the browser build every one of them reports
"not present", which is the same branch the game takes on a machine with no registry key.

Thread priorities (`winnt.h`) — `THREAD_BASE_PRIORITY_*`, `THREAD_PRIORITY_IDLE` …
`THREAD_PRIORITY_TIME_CRITICAL`, `GetThreadPriority`, `SetThreadPriority`.
*Why:* `Battlefront2/Platform/Pc/pcPblFile.cpp` and `SoundFL/source/win/sndthread.cpp` raise the
loader/mixer thread priority. There is no thread scheduler to raise here; the value is remembered so
the call succeeds.

user32/gdi32 — `SetRect`, `SW_*`, `SWP_*`, `PM_*`, `ICM_*`, `FW_*`, `BI_*`, `DIB_*`, `LOGFONTA`,
`WINDOWPLACEMENT`, `AdjustWindowRect`, `GetClientRect`, `SetWindowPos`, `GetFocus`, `PeekMessageA`,
`TranslateMessage`, `DispatchMessageA`, `SetICMMode`, `GetDeviceGammaRamp`, `SetTextColor`,
`SetBkColor`, `CreateFontIndirectA`, `CreateDIBSection`, `GdiFlush`, `LoadLibraryA`, `FreeLibrary`,
`GetExceptionCode`.
*Why:* `Battlefront2/Source/pcMain.cpp` keeps its message loop and gamma-ramp code;
`lglcd/lgLcd.cpp` draws the LCD applet with GDI. Each of these was a `use of undeclared identifier`
or `no matching function`. `SetRect` and `SetRectEmpty` are deliberately **not** macros: the real
`windows.h` defines them as macros, but `RedEngineFL/Interface/RedHotSpot.h:18` declares a member
`void SetRect(float, float, float, float)`, and a macro would be expanded inside that declaration
(this cost 45 core files until it was fixed, so it is worth keeping as a rule). Plain functions give
the call sites the same meaning without touching engine identifiers.

Wide CRT (`_MAX_PATH`, `_MAX_DRIVE`/`_MAX_DIR`/`_MAX_FNAME`/`_MAX_EXT`, `_wfinddata_t`,
`_wfindfirst`/`_wfindnext`/`_wfindclose`, `_wsplitpath`, `_wmakepath`, `_wfullpath`, `_wfopen`,
`_wremove`, `_wrename`, `_strdup`) and the `lstrcpyA`/`lstrcatA`/`lstrcmpA`/`lstrcpynA` family.
*Why:* `pcSaveDevice.cpp` walks save slots with `_wfindfirst` and splits names with `_wsplitpath`;
`lgLcd.cpp` uses the `lstr*` spellings; `_strdup` exists because MSVC spells it that way and musl
does not.

COM plumbing: `REFIID`/`REFCLSID`/`REFGUID` changed to `const GUID&` in C++ (pointer in C), `GUID`
comparison operators (`guiddef.h`), `IID_IUnknown` / `IID_IKsPropertySet` declarations,
`IKsPropertySet` re-based on `IUnknown`.
*Why:* `snddsbuffer.cpp:79` does `QueryInterface(IID_X, ...)` with a GUID *object*; with `REFIID` as
a pointer that could not bind. Microsoft's own definition is a reference in C++ and a pointer in C,
so the header now matches per-language. `operator==` is needed by
`snddsmediabuffer.cpp`/`snddsdeviceenumerator.cpp` comparing device ids.

Corrections to declarations that already existed in the prefix:
* `ERROR_ALREADY_INITIALIZED` 1174 → 1247 (1174 is `ERROR_APPLICATION_VERIFICATION`).
* `CreateDIBSection` returned `int` → returns `HBITMAP` (it is assigned to an `HBITMAP` in
  `lgLcd.cpp:CreateDib`); `GetForegroundWindow` returned `void*` → returns `HWND` (`sndengine.cpp:991`
  assigns it to an `HWND`).
* `CreateFontIndirectA` returned `int` → returns `HFONT`.
* `WINDOWPLACEMENT`'s made-up fields replaced by Microsoft's (`length`, `flags`, `showCmd`,
  `ptMinPosition`, `ptMaxPosition`, `rcNormalPosition`, `rcDevice`) — `pcMain.cpp` sets
  `placement.length` and reads `showCmd`.
* `IKsPropertySet::Get`/`Set` had the parameters the wrong way round and `Set` one argument short,
  so `snddsbuffer.cpp:795` (`Set(propSet, id, 0, 0, data, len)`) could not match. The interface is
  now `QuerySupport(const GUID&, unsigned long, unsigned long*)`,
  `Get(const GUID&, unsigned long, void*, unsigned long, void*, unsigned long, unsigned long*)`,
  `Set(const GUID&, unsigned long, void*, unsigned long, void*, unsigned long)` — ksproxy.h's shape,
  as also carried by Wine. A `Query` method that is not part of `IKsPropertySet` in any version was
  **removed** rather than left behind: a phantom virtual would shift the vtable of any real
  implementation, which is exactly the kind of drift this shim has to avoid.
* `DSERR_GENERIC`/`DSERR_UNSUPPORTED`/`DSERR_BUFFERLOST`/`DSERR_INVALIDPARAM` (invented values
  -1/-2/-4/-5) and `DSCAPS_CONTINUOUSRATE` (0) replaced with Microsoft's values.
* GDI stock-object ids `BLACK_BRUSH` (3→4) and `DKGRAY_BRUSH` (4→3) were swapped, `WHITE_PEN` (1→6)
  and `BLACK_PEN` (2→7) were off by five, and `SC_SCREENSAVE` (0xF010→0xF140) was wrong. The engine
  passes these as opaque ids (`GameLoop.cpp:314` `GetStockObject(WHITE_BRUSH)`,
  `FLInputManager.cpp:806` `GetStockObject(WHITE_PEN)`), so nothing broke, but a backend that maps
  them onto real GDI/WebGPU resources would have drawn the wrong object.

## Verification

* `bash /tmp/measure_pc.sh` — 82/100, log in `/tmp/FINAL_pc.log`, and the per-file first-error list it writes is `/tmp/pc_result.txt`.
* `bash tools/measure.sh` — `TOTAL ok: 373 / 638`, unchanged from the start of
  the pass; error-kind histogram above.
* `python3 tools/shim_autogen.py 8` then again `4` — both `converged: 372/638 files syntax-clean`,
  `+0 types, +0 names`, `generated_types.h` md5 unchanged (`2baec0ec…`) → the hand-written shim
  content and the generator agree, and nothing added here will be re-invented or fight the tool.
* Diagnostics scan (`/tmp/coreerrs/*.txt`, 638 files): no core error is located inside a shim header
  and none names a symbol added in this pass.
* Value check of every constant added here against the official enum/`#define` bodies as mirrored in
  Wine and mingw-w64 (`/tmp/dxref/`): 382 names are shared with the reference headers and every
  remaining difference is a spelling of the same number (`__MSABI_LONG(...)`,
  `_HRESULT_TYPEDEF_(...)`, `MAKEINTRESOURCE(...)`, `READ_CONTROL` written as a name). The
  substantive disagreements that check turned up — `ERROR_ALREADY_INITIALIZED`, the four `DSERR_*`,
  `DSCAPS_CONTINUOUSRATE`, and the GDI ids above — were all fixed.
* Nothing was added that the retained tree does not name: 66 constants and 16 functions
  (`D3DERR_*` codes, cubemap faces, `D3DSBT_*`, `DSBUFFERDESC1`, `CoTaskMemAlloc`, …) that turned out
  to have no reference anywhere in `build/src_fixed` were removed again.
* Late in the pass the shim-side gaps that were *hidden* behind the `Thread` failures were dug out of
  the full (uncapped) diagnostics and fixed too — `DirectSoundCreate8` was declared but never
  written, `GetForegroundWindow` returned `void*` where `sndengine.cpp:991` wants `HWND`,
  `IMediaObject` was missing `GetInputSizeInfo`, and `DMO_MEDIA_TYPE`/`DMO_OUTPUT_DATA_BUFFER` were
  missing a field each. They do not move the pass/fail count (those files are blocked by `Thread`
  regardless) but they are the reason the remaining errors in `sndengine.cpp`, `snddsdmo.cpp` and
  `sndstreamdsdmo.cpp` are now *only* `Thread`.

## The 18 platform files that still fail

First error is from the final `measure_pc.sh` run; "root cause" is from the full error list of the
same file (`-ferror-limit=0`). **None of them can be fixed from `spike/shim/`** — each needs a
decision about the retained tree, listed after the table.

| file | first error | root cause |
|---|---|---|
| `Battlefront2/Platform/Pc/pcPblFile.cpp` | `variable type 'Thread' is an abstract class` | see *Thread* below |
| `Battlefront2/Source/pcMain.cpp` | `variable type 'Thread' is an abstract class` | *Thread*, and a second, independent blocker: `SEH '__try' is not supported on this target` (`__try/__except/GetExceptionCode` around the game loop; emsdk clang has no SEH lowering for wasm32) |
| `Battlefront2/Platform/Pc/pcFirstPerson.cpp` | `member access into incomplete type 'ZephyrSkeletonShared'` (×2) | the TU uses `ZephyrSkeletonShared::` without including the header that defines it (it is defined in `Battlefront2/Source/*.h` siblings) |
| `Battlefront2/Platform/Pc/pcSpawnDisplay.cpp` | `member access into incomplete type 'ZephyrSkeletonShared'` | 914 further errors: `unknown type name 'Property'`, and `m_pEntityElement`, `mOwner`, `mPlayerIdx`, `mMode`, `mTextSpawnTimer`, `mTeamNumber`, `mClassIndex`, … undeclared — the retained `SpawnDisplay` header does not declare the members the retained `.cpp` uses |
| `Battlefront2/Platform/Pc/pcOrdnanceGrapplingHook.cpp` | `use of undeclared identifier 'SoldierAnimationBank'` | missing include (`Battlefront2/Source/SoldierAnimationBank.h` exists) plus `Combo` incomplete (`no member named 'uiNumAnimations'/'uiNameHash' in 'Combo'`) — the same `Combo` gap that breaks 4 core files |
| `RedEngineFL/Graphics/PC/pcRedRenderer.cpp` | `use of undeclared identifier 'FakeD3D'` | `pcRedRenderer.h` was retained with the software-fallback classes commented out (`/* class FakeD3D … omitted in the browser build */`) while the `.cpp` still defines all of their members: 326 errors, all `FakeD3D*` |
| `SoundFL/source/win/sndengine.cpp` | `variable type 'Thread' is an abstract class` | *Thread* |
| `SoundFL/source/win/sndstream.cpp` | `variable type 'Thread' is an abstract class` | *Thread* |
| `SoundFL/source/win/sndvoice.cpp` | `field type 'Thread' is an abstract class` | *Thread* |
| `SoundFL/source/win/sndsoftoutput.cpp` | `field type 'Thread' is an abstract class` | *Thread* |
| `SoundFL/source/win/sndstreamrenderer.cpp` | `field type 'Thread' is an abstract class` | *Thread* |
| `SoundFL/source/win/sndstreamauxrenderer.cpp` | `field type 'Thread' is an abstract class` | *Thread* |
| `SoundFL/source/win/snddsbufferrenderer.cpp` | `field type 'Thread' is an abstract class` | *Thread* |
| `SoundFL/source/win/sndstreamcapturesource.cpp` | `field type 'Thread' is an abstract class` | *Thread*, plus engine-side typing: the TU declares the capture object as `IDirectSoundCapture*` and passes `&`of it to `DirectSoundCaptureCreate8`, whose out parameter is `IDirectSoundCapture8**` (so is Microsoft's) — the decompiler picked the base type |
| `SoundFL/source/win/snddsdmo.cpp` | `field type 'Thread' is an abstract class` | *Thread* only |
| `SoundFL/source/win/sndstreamdsdmo.cpp` | `field type 'Thread' is an abstract class` | *Thread* |
| `SoundFL/source/win/sndthread.cpp` | `cannot define or redeclare 'Start' here because namespace 'Snd' does not enclose namespace 'Thread'` | `sndthread.cpp:9` defines `bool Thread::Start(void*)` inside `namespace Snd`, while `Thread` is the global-scope `Battlefront2/Source/Thread.h` class — decompiled namespace mismatch, and the same *Thread* problem underneath |
| `SoundFL/source/win/snddsbuffer.cpp` | `use of undeclared identifier 'Samples'` | `SampleInfo::_SetFormat` writes `Samples.wValidBitsPerSample`, `SubFormat`, `dwChannelMask` bare: the decompiler split one local `WAVEFORMATEXTENSIBLE` into per-field variables, so the retained source no longer has a name for the struct |

### The `Thread` blocker (12 of the 18, and 10 of them nothing else)

`build/src_fixed/Battlefront2/Source/Thread.h:88` declares

```cpp
class Thread {
public:
    Thread();
    virtual ~Thread();
    virtual bool Update(float fDeltaTime) = 0;   // <-- line 94
    ...
};
```

and twelve platform TUs hold a `Thread` **by value** (`Thread mThread;` in `sndthreadupdate.h:50`,
`static Thread smFileThread;` in `sndfile.h:84` — which is also what `pcPblFile.cpp` trips over,
since it includes that header, …). A
class with a pure virtual cannot be instantiated, so those declarations are ill-formed — and the
same thing breaks 25 core files. The shipped game evidently *does* construct a `Thread`, so the
decompiled `= 0` on `Update` is almost certainly wrong.

This cannot be repaired from the shim: `Thread` is an engine class, and the only ways to make a
value of it exist are (a) editing `Thread.h` to give `Update` a body (or drop `= 0`), or (b)
changing those members, or (c) excluding these TUs from the platform scope. (a)–(c) are engine-tree
decisions and outside this pass's mandate (`spike/shim/` + `docs/` only), so the 12 files are
reported rather than hacked around. `pcMain.cpp` would additionally need its
`__try/__except(EXCEPTION_…)` crash guard rewritten (or the crash handler left to
`setjmp`/wasm traps) even once `Thread` is settled.

## Open items

1. `IID_IUnknown`, `IID_IKsPropertySet`, `IID_IMediaObject`, `IID_IMediaObjectInPlace`,
   `IID_IMediaBuffer`, the `IID_IDirectSound*` objects and the `DSDEVID_*` / `GUID_DSFX_*` identifiers
   are declarations only. Somebody has to provide Microsoft's actual GUID bytes (a `guids.cpp`,
   i.e. the `uuid.lib` role) before these TUs link; inventing them here would have been worse than
   leaving them undeclared, since a wrong GUID silently binds to the wrong interface.
2. Restoring the core measurement to 637/638 needs `build/src_fixed` regenerated from whatever
   produced the 2026-11-15 21:55 tree — or the 373/638 tree accepted as the new baseline.
3. The `Thread` decision above, then the missing includes / `Combo` / `ZephyrSkeletonShared` /
   `SpawnDisplay` members / `FakeD3D` retention / `WAVEFORMATEXTENSIBLE` split-locals above.
   Fixing *only* `Thread::Update` takes the platform scope from 82 to 92/100: exactly ten of the
   eighteen failures (`pcPblFile`, `sndvoice`, `sndsoftoutput`, `sndstream`, `sndstreamrenderer`,
   `sndstreamauxrenderer`, `snddsbufferrenderer`, `sndengine`, `snddsdmo`, `sndstreamdsdmo`) have no
   other error left in them after this pass.
4. `pcMain.cpp`'s SEH usage (`__try`/`__except`) needs a wasm-side plan.
5. `spike/shim/guids.cpp` does not exist; if the audio backend is built for real, that is where the
   GUID definitions and the `DirectSoundCreate8` / `CoCreateInstance` / `GetDeviceGammaRamp` bodies
   belong.

## Appendix A — the constant vocabulary added

`d3d9.h` (158) and `dsound.h` (87); values are Microsoft's. Generated from the headers themselves,
so this list cannot drift from the file.

## d3d9.h: 158 constants
- D3DRS [22]: D3DRS_ALPHAFUNC (25), D3DRS_AMBIENT (139), D3DRS_AMBIENTMATERIALSOURCE (147), D3DRS_CCW_STENCILFUNC (189), D3DRS_CCW_STENCILPASS (188), D3DRS_CCW_STENCILZFAIL (187), D3DRS_COLORVERTEX (141), D3DRS_DIFFUSEMATERIALSOURCE (145), D3DRS_EMISSIVEMATERIALSOURCE (148), D3DRS_FILLMODE (8), D3DRS_LIGHTING (137), D3DRS_NORMALIZENORMALS (143), D3DRS_SPECULARENABLE (29), D3DRS_SPECULARMATERIALSOURCE (146), D3DRS_STENCILMASK (58), D3DRS_STENCILPASS (55), D3DRS_STENCILREF (57), D3DRS_STENCILWRITEMASK (59), D3DRS_STENCILZFAIL (54), D3DRS_TEXTUREFACTOR (60), D3DRS_TWOSIDEDSTENCILMODE (185), D3DRS_ZFUNC (23)
- D3DFMT [21]: D3DFMT_A16B16G16R16 (36), D3DFMT_A1R5G5B5 (25), D3DFMT_A2R10G10B10 (35), D3DFMT_A4L4 (52), D3DFMT_A8B8G8R8 (32), D3DFMT_A8L8 (51), D3DFMT_A8P8 (40), D3DFMT_A8R3G3B2 (29), D3DFMT_D15S1 (73), D3DFMT_D24X4S4 (79), D3DFMT_D32 (71), D3DFMT_G16R16 (34), D3DFMT_INDEX16 (101), D3DFMT_L16 (81), D3DFMT_P8 (41), D3DFMT_R3G3B2 (27), D3DFMT_R8G8B8 (20), D3DFMT_VERTEXDATA (100), D3DFMT_X1R5G5B5 (24), D3DFMT_X4R4G4B4 (30), D3DFMT_X8B8G8R8 (33)
- D3DFVF [12]: D3DFVF_DIFFUSE (0x0040), D3DFVF_LASTBETA_UBYTE4 (0x1000), D3DFVF_NORMAL (0x0010), D3DFVF_SPECULAR (0x0080), D3DFVF_TEX1 (0x0100), D3DFVF_TEX2 (0x0200), D3DFVF_XYZ (0x0002), D3DFVF_XYZB1 (0x0006), D3DFVF_XYZB2 (0x0008), D3DFVF_XYZB3 (0x000a), D3DFVF_XYZB4 (0x000c), D3DFVF_XYZRHW (0x0004)
- D3DMULTISAMPLE [12]: D3DMULTISAMPLE_10_SAMPLES (10), D3DMULTISAMPLE_11_SAMPLES (11), D3DMULTISAMPLE_12_SAMPLES (12), D3DMULTISAMPLE_13_SAMPLES (13), D3DMULTISAMPLE_14_SAMPLES (14), D3DMULTISAMPLE_15_SAMPLES (15), D3DMULTISAMPLE_3_SAMPLES (3), D3DMULTISAMPLE_5_SAMPLES (5), D3DMULTISAMPLE_6_SAMPLES (6), D3DMULTISAMPLE_7_SAMPLES (7), D3DMULTISAMPLE_8_SAMPLES (8), D3DMULTISAMPLE_9_SAMPLES (9)
- D3DDECLUSAGE [9]: D3DDECLUSAGE_BINORMAL (7), D3DDECLUSAGE_BLENDINDICES (2), D3DDECLUSAGE_BLENDWEIGHT (1), D3DDECLUSAGE_COLOR (10), D3DDECLUSAGE_NORMAL (3), D3DDECLUSAGE_POSITION (0), D3DDECLUSAGE_POSITIONT (9), D3DDECLUSAGE_TANGENT (6), D3DDECLUSAGE_TEXCOORD (5)
- D3DDECLTYPE [8]: D3DDECLTYPE_D3DCOLOR (4), D3DDECLTYPE_FLOAT1 (0), D3DDECLTYPE_FLOAT2 (1), D3DDECLTYPE_FLOAT3 (2), D3DDECLTYPE_FLOAT4 (3), D3DDECLTYPE_SHORT2 (6), D3DDECLTYPE_SHORT4 (7), D3DDECLTYPE_UNUSED (17)
- D3DTOP [8]: D3DTOP_ADDSMOOTH (11), D3DTOP_BLENDCURRENTALPHA (16), D3DTOP_BLENDDIFFUSEALPHA (12), D3DTOP_BLENDFACTORALPHA (14), D3DTOP_BUMPENVMAP (22), D3DTOP_MODULATE2X (5), D3DTOP_MODULATECOLOR_ADDALPHA (19), D3DTOP_MULTIPLYADD (25)
- D3DTSS [7]: D3DTSS_ALPHAARG0 (27), D3DTSS_BUMPENVMAT01 (8), D3DTSS_BUMPENVMAT10 (9), D3DTSS_BUMPENVMAT11 (10), D3DTSS_COLORARG0 (26), D3DTSS_CONSTANT (32), D3DTSS_TEXCOORDINDEX (11)
- D3DPRESENT_INTERVAL [6]: D3DPRESENT_INTERVAL_DEFAULT (0x00000000), D3DPRESENT_INTERVAL_FOUR (0x00000008), D3DPRESENT_INTERVAL_IMMEDIATE (0x80000000), D3DPRESENT_INTERVAL_ONE (0x00000001), D3DPRESENT_INTERVAL_THREE (0x00000004), D3DPRESENT_INTERVAL_TWO (0x00000002)
- D3DRTYPE [6]: D3DRTYPE_CUBETEXTURE (5), D3DRTYPE_INDEXBUFFER (7), D3DRTYPE_SURFACE (1), D3DRTYPE_TEXTURE (3), D3DRTYPE_VERTEXBUFFER (6), D3DRTYPE_VOLUMETEXTURE (4)
- other [6]: D3DDEVTYPE_HAL (1), D3DFILL_SOLID (3), D3DFILL_WIREFRAME (2), D3D_OK (S_OK), D3D_SDK_VERSION (32), _FACD3D (0x876)
- D3DTTFF [4]: D3DTTFF_COUNT2 (2), D3DTTFF_COUNT3 (3), D3DTTFF_DISABLE (0), D3DTTFF_PROJECTED (256)
- D3DCOLOR [3]: D3DCOLORWRITEENABLE_BLUE (((1)<<2)), D3DCOLORWRITEENABLE_GREEN (((1)<<1)), D3DCOLORWRITEENABLE_RED (((1)<<0))
- D3DERR [3]: D3DERR_DEVICELOST (MAKE_D3DHRESULT(2152)), D3DERR_DEVICENOTRESET (MAKE_D3DHRESULT(2153)), D3DERR_DRIVERINTERNALERROR (MAKE_D3DHRESULT(2087))
- D3DPT [3]: D3DPTEXTURECAPS_NONPOW2CONDITIONAL ((0x00000100)), D3DPTEXTURECAPS_NOPROJECTEDBUMPENV ((0x00200000)), D3DPTEXTURECAPS_POW2 ((0x00000002))
- D3DSTENCILOP [3]: D3DSTENCILOP_DECR (8), D3DSTENCILOP_INCR (7), D3DSTENCILOP_KEEP (1)
- D3DTS [3]: D3DTS_PROJECTION (3), D3DTS_VIEW (2), D3DTS_WORLD (D3DTS_WORLDMATRIX(0))
- D3DCREATE [2]: D3DCREATE_HARDWARE_VERTEXPROCESSING ((0x00000040)), D3DCREATE_SOFTWARE_VERTEXPROCESSING ((0x00000020))
- D3DDEVCAPS [2]: D3DDEVCAPS_HWTRANSFORMANDLIGHT (0x0010000), D3DDEVCAPS_PUREDEVICE (0x0100000)
- D3DMCS [2]: D3DMCS_COLOR1 (1), D3DMCS_MATERIAL (0)
- D3DSAMP [2]: D3DSAMP_MAXMIPLEVEL (9), D3DSAMP_MIPMAPLODBIAS (8)
- D3DTA [2]: D3DTA_ALPHAREPLICATE (0x00000020), D3DTA_SPECULAR (0x00000004)
- D3DTEXOPCAPS [2]: D3DTEXOPCAPS_BUMPENVMAP (0x0200000), D3DTEXOPCAPS_MULTIPLYADD (0x1000000)
- D3DTSS_TCI [2]: D3DTSS_TCI_CAMERASPACENORMAL (0x10000), D3DTSS_TCI_CAMERASPACEPOSITION (0x20000)
- D3DZB [2]: D3DZB_FALSE (0), D3DZB_TRUE (1)
- D3DBACKBUFFER_TYPE [1]: D3DBACKBUFFER_TYPE_MONO (0)
- D3DCMP [1]: D3DCMP_NOTEQUAL (6)
- D3DDECLMETHOD [1]: D3DDECLMETHOD_DEFAULT (0)
- D3DLOCK [1]: D3DLOCK_NOOVERWRITE (0x1000)
- D3DPMISCCAPS [1]: D3DPMISCCAPS_PERSTAGECONSTANT ((0x00008000))
- D3DSTENCILCAPS [1]: D3DSTENCILCAPS_TWOSIDED (0x100)

## dsound.h: 87 constants
- DSFX_I3DL2REVERB [23]: DSFX_I3DL2REVERB_DECAYHFRATIO_MAX (2.0f), DSFX_I3DL2REVERB_DECAYHFRATIO_MIN (0.1f), DSFX_I3DL2REVERB_DECAYTIME_MAX (20.0f), DSFX_I3DL2REVERB_DECAYTIME_MIN (0.1f), DSFX_I3DL2REVERB_DENSITY_MAX (100.0f), DSFX_I3DL2REVERB_DENSITY_MIN (0.0f), DSFX_I3DL2REVERB_DIFFUSION_MAX (100.0f), DSFX_I3DL2REVERB_DIFFUSION_MIN (0.0f), DSFX_I3DL2REVERB_HFREFERENCE_MAX (20000.0f), DSFX_I3DL2REVERB_HFREFERENCE_MIN (20.0f), DSFX_I3DL2REVERB_REFLECTIONSDELAY_MIN (0.0f), DSFX_I3DL2REVERB_REFLECTIONS_MAX (1000), DSFX_I3DL2REVERB_REFLECTIONS_MIN ((-10000)), DSFX_I3DL2REVERB_REVERBDELAY_MAX (0.1f), DSFX_I3DL2REVERB_REVERBDELAY_MIN (0.0f), DSFX_I3DL2REVERB_REVERB_MAX (2000), DSFX_I3DL2REVERB_REVERB_MIN ((-10000)), DSFX_I3DL2REVERB_ROOMHF_MAX (0), DSFX_I3DL2REVERB_ROOMHF_MIN ((-10000)), DSFX_I3DL2REVERB_ROOMROLLOFFFACTOR_MAX (10.0f), DSFX_I3DL2REVERB_ROOMROLLOFFFACTOR_MIN (0.0f), DSFX_I3DL2REVERB_ROOM_MAX (0), DSFX_I3DL2REVERB_ROOM_MIN ((-10000))
- DSERR [19]: DSERR_ACCESSDENIED (E_ACCESSDENIED), DSERR_ALLOCATED (MAKE_DSHRESULT(10)), DSERR_ALREADYINITIALIZED (MAKE_DSHRESULT(130)), DSERR_BADFORMAT (MAKE_DSHRESULT(100)), DSERR_BADSENDBUFFERGUID (MAKE_DSHRESULT(210)), DSERR_BUFFERTOOSMALL (MAKE_DSHRESULT(180)), DSERR_CONTROLUNAVAIL (MAKE_DSHRESULT(30)), DSERR_DS8_REQUIRED (MAKE_DSHRESULT(190)), DSERR_FXUNAVAILABLE (MAKE_DSHRESULT(220)), DSERR_INVALIDCALL (MAKE_DSHRESULT(50)), DSERR_NOAGGREGATION (CLASS_E_NOAGGREGATION), DSERR_NODRIVER (MAKE_DSHRESULT(120)), DSERR_NOINTERFACE (E_NOINTERFACE), DSERR_OBJECTNOTFOUND (MAKE_DSHRESULT(4449)), DSERR_OTHERAPPHASPRIO (MAKE_DSHRESULT(160)), DSERR_OUTOFMEMORY (E_OUTOFMEMORY), DSERR_PRIOLEVELNEEDED (MAKE_DSHRESULT(70)), DSERR_SENDLOOP (MAKE_DSHRESULT(200)), DSERR_UNINITIALIZED (MAKE_DSHRESULT(170))
- SPEAKER [8]: SPEAKER_BACK_LEFT (0x00000010), SPEAKER_BACK_RIGHT (0x00000020), SPEAKER_FRONT_CENTER (0x00000004), SPEAKER_FRONT_LEFT (0x00000001), SPEAKER_FRONT_LEFT_OF_CENTER (0x00000040), SPEAKER_FRONT_RIGHT (0x00000002), SPEAKER_FRONT_RIGHT_OF_CENTER (0x00000080), SPEAKER_LOW_FREQUENCY (0x00000008)
- DSSPEAKER [7]: DSSPEAKER_5POINT1 (6), DSSPEAKER_7POINT1 (7), DSSPEAKER_HEADPHONE (1), DSSPEAKER_MONO (2), DSSPEAKER_QUAD (3), DSSPEAKER_STEREO (4), DSSPEAKER_SURROUND (5)
- DSBCAPS [6]: DSBCAPS_CTRLFREQUENCY (0x00000020), DSBCAPS_CTRLPAN (0x00000040), DSBCAPS_LOCDEFER (0x00040000), DSBCAPS_LOCHARDWARE (0x00000004), DSBCAPS_LOCSOFTWARE (0x00000008), DSBCAPS_PRIMARYBUFFER (0x00000001)
- DSCAPS [6]: DSCAPS_PRIMARYMONO (0x00000001), DSCAPS_PRIMARYSTEREO (0x00000002), DSCAPS_SECONDARY16BIT (0x00000800), DSCAPS_SECONDARY8BIT (0x00000400), DSCAPS_SECONDARYMONO (0x00000100), DSCAPS_SECONDARYSTEREO (0x00000200)
- DSBSTATUS [5]: DSBSTATUS_LOCHARDWARE (0x00000008), DSBSTATUS_LOCSOFTWARE (0x00000010), DSBSTATUS_LOOPING (0x00000004), DSBSTATUS_PLAYING (0x00000001), DSBSTATUS_TERMINATED (0x00000020)
- DSBPLAY [4]: DSBPLAY_LOCHARDWARE (0x00000002), DSBPLAY_LOCSOFTWARE (0x00000004), DSBPLAY_TERMINATEBY_PRIORITY (0x000000020), DSBPLAY_TERMINATEBY_TIME (0x00000008)
- DS3D [2]: DS3D_DEFERRED (0x00000001), DS3D_IMMEDIATE (0x00000000)
- DSBVOLUME [2]: DSBVOLUME_MAX (0), DSBVOLUME_MIN (-10000)
- DSSCL [2]: DSSCL_EXCLUSIVE (3), DSSCL_NORMAL (1)
- DSCBCAPS [1]: DSCBCAPS_WAVEMAPPED (0x80000000)
- DSCBSTART [1]: DSCBSTART_LOOPING (0x00000001)
- _FACDS [1]: _FACDS (0x878)


---

## Two silent bugs in the shim, found by wiring the 3D camera

Both were found in the same debugging session, both are in files this pass owns (`spike/shim/`),
and neither produced a compile or link error. Each produced *wrong numbers* instead, which is why
they are recorded here rather than buried in a commit.

### 1. The shim and the host did not agree on what `__declspec(align(16))` means

`PblMatrix` is declared by the decompiler as `class __declspec(align(16)) PblMatrix`. Engine TUs
are compiled with `-include spike/shim/swbf2_prefix.h`, and that header contains
`#define __declspec(x)` - so for them the attribute is erased and the class is 4-byte aligned.
`browser/main.cpp` was compiled with `-fms-extensions` and **without** the prefix header, so it
honoured the attribute and got 16-byte alignment.

Same headers, different flags, different layout: the host read `_Matrix` at offset 48 while the
engine wrote it at offset 40, so every matrix the host handed to the engine arrived permuted.
Measured directly (`cam=448 _Matrix@48` before, `cam=428 _Matrix@40` after).

The fix is structural, not a cast: `tools/build_browser.sh` now repeats `tools/compile_core.sh`'s
flags verbatim, including the forced prefix include, with a comment saying why they must stay in
step. There is one ABI in this program, and it is defined by one flag list.

Symptom, for the record, because it is unrecognisable from the outside: a camera matrix written by
the host came back from the engine as
`0,0,0,0.98 | -0.18,0,0,0.18 | 0.98,0,0,4.65 | 25.58,1,1,0` - the intended values, shuffled, and
with `m[3][3]` zero despite being set to one.

### 2. `D3DXMATRIX`'s named fields were sixteen names for the same float

`spike/shim/d3dx9math.h` declared

```cpp
union { float m[16]; float _11, _12, _13, _14, _21, ... _44; };
```

In a union every member starts at offset 0, so `_12` through `_44` were all aliases of `m[0]`
(`offsetof` returned 0 for every one of them). Every helper that reads `_ij` - `D3DXVec3TransformNormal`,
`TransformCoord`, `Vec4Transform`, the matrix multiplies - multiplied all three components by the
same number, and the constructor's `m[0] = 1; _11 = _22 = _33 = _44 = 1.0f` set `m[0]` four times
instead of building an identity matrix.

Reproduced natively in under a second, which is the lesson of this section - the wasm/browser
layer was nowhere near the cause:

```sh
g++ -std=gnu++17 -I spike/shim -I build/src_fixed -include spike/shim/swbf2_prefix.h \
    /tmp/invtest.cpp build/src_fixed/PebbleFL/pc/pcPblMatrix.cpp spike/shim/dxmath_c.cpp -o /tmp/invtest -lm
```

`PblMatrix::GetInverseFast` - real engine code, called by cloth, traps, vehicles, fish effects and
every camera - returned a translation row of `-30.235, -30.235, -30.235` (namely
`(0 + 4.655 + 25.58) x m[0]`, three times). After putting the fields in a nested `struct`, as
`spike/shim/d3d9.h` already did for `D3DMATRIX`, it returns `0.000, -0.002, -26.004`, which is the
correct inverse of a rigid camera 26 units from the origin.

### Related rename

`swbf2_prefix.h` used to declare `double emscripten_get_now(void);` with C++ linkage. That is a real
Emscripten runtime function, declared `extern "C"` by `<emscripten.h>`, so the two declarations
collided in any TU that needed both - which the browser host does, for its WebGL context and main
loop. The shim's clock is now `swbf2_now_ms()`, its own name, defined in
`browser/platform/pcRedRenderer_browser.cpp`; the 16 engine objects that call it were rebuilt.
