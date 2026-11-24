# Compiling the decompiled engine to WASM: how far it got, and what did the work

Final measurement: **637 of 638 translation units in the core scope compile to
wasm32-unknown-emscripten** (99.8%). One file fails, and it fails for a reason that belongs to the
decompilation rather than to the port.

```
scope                 files   ok   fail
Battlefront2/Source     429  428      1   (NetGame.cpp)
RedEngineFL              77   77      0
SoundFL                  61   61      0
LuaFL                    26   26      0
PebbleFL                 21   21      0
Zephyr                   12   12      0
ZlibFL (C)                9    9      0
_browser_shim (ours)      3    3      0
                      ------------------
total                                            637 / 638
```

`build/core.a` holds all 637 objects (40 MB) and links into a wasm module with
`-sERROR_ON_UNDEFINED_SYMBOLS=0`; `docs/backend-surface.md` lists the 750 symbols still unresolved
and sorts them into work that is ours, work that is a backend's, and work that is upstream's.

## The premise I had to throw away

The plan started as "reconstruct the top missing class definitions". That premise was wrong, and it
took a clean rebuild to see it.

The first audit (127/636) had been compiled against a source tree that my own earlier tooling had
already damaged: a "forward declaration → `#include`" pass had rewritten declarations inside template
bases (`Factory<Entity, ...>`) and destroyed the parse of whole modules. When I rebuilt the tree from
the pristine decompilation with only two provably safe edits, the same compiler, same flags, no new
shims: **303/636**, not 127. So most of the "missing classes" were self-inflicted wounds.

The real blockers were never the engine's class model. They were:

1. **MSVC-isms that clang rejects** — `operator new(unsigned int)`, untyped `enum X;`, `__declspec`,
   `char*` used as `va_list`, MSVC's `unsigned int(-1)` cast.
2. **The platform API vocabulary** — Direct3D9, DirectSound, DirectInput, Winsock, Win32: thousands
   of type names, constants and device methods that the *replacement* code used to define.
3. **Decompilation defects** — missing in-class `operator new`/`delete` declarations, duplicate global
   `operator new` definitions, redundant explicit destructor instantiations, a matrix subscript that
   the original source could not have contained.
4. **Cascades** — one missing `#include` in a header that 104 files depend on. `ZephyrSkeletonShared`
   alone gated 102 files.

So the work became: a conservative tree-prepare pass, a purely additive include pass, and a
converging "platform vocabulary by discovery" generator. Then, and only then, the last few hand
fixes.

## The three tools

`tools/prepare_tree.py` — copies `vendor/swbf2decomp/src` to `build/src_fixed` applying nine
transformations, each counted and printed so the diff is auditable, each justified in the file's
docstring. In short: `operator new`/`delete` size parameters to `size_t` (both sides of a
placement new/delete pair — getting only one side wrong breaks SoundFL); allocator function-pointer
types and allocator hooks to `size_t` (MSVC's `size_t` *is* `unsigned int`, emscripten's is
`unsigned long`); `char* pArgs` to `va_list` (MSVC x86 `va_list` literally is `char*`);
`unsigned int(-1)` to `(unsigned int)(-1)`; untyped enums get `: int`; a second identical global
`operator new` definition becomes `operator new[]` (the decompiler emitted MSVC's scalar and array
allocation functions as the same name); missing in-class `operator new`/`operator delete`
declarations are re-inserted into their class (a member allocator cannot be defined out of line
without one); redundant explicit destructor instantiations are dropped where the TU already
instantiates the whole class; and inside `RedEngineFL/Graphics/PC` headers the `FakeD3D*` software
fallback classes are dropped (9 of them) because they are the reference implementation of exactly the
layer the browser build replaces.

`tools/include_pass.py` — strictly additive. When clang reports an incomplete type, it finds the
header that actually *defines* it (brace-bodied `class X {`, never a forward declaration) and inserts
that include at the error site. It never deletes or rewrites anything, which is the mistake that
produced the original 127.

`tools/shim_autogen.py` — compiles the scope repeatedly and turns clang's own diagnostics into the
platform vocabulary, writing `spike/shim/generated_types.h`:

| clang says | generates |
|---|---|
| `unknown type name 'X'` | `typedef int X;` (an opaque handle that never outlives the replaced layer) |
| `use of undeclared identifier 'X'` | an enum constant with a stable value |
| `no member named 'M' in 'IDirect3DDevice9'` | a no-op template member on that interface |

Three rules make it safe rather than a hazard: it only invents names matching the platform APIs being
replaced; it never invents a name a shim header already declares (without that, it typedef'd
`D3DXVECTOR3` over the real struct and broke 627 files); and it drops entries a shim later declares by
hand (a generated enum entry whose name later becomes a `#define` is macro-expanded inside the enum
body, which is a syntax error, not a redefinition). Converged state: 30 placeholder typedefs, 194
constants, 29 synthesised D3D/DInput/DSound members. Game code and the new backends include the same
header, so a constant means the same thing on both sides of the boundary.

`tools/compile_one.sh` + `tools/measure.sh` compile the scope in parallel and report only real
`error:` diagnostics — treating warnings as failures is how a previous session chased 600 phantom
errors.

## Shims that had to be real code, not stubs

`spike/shim/d3dx9math.h` is a working D3DX9 math implementation (matrices, quaternions, vectors,
interpolations), because aiming, camera, vehicle and bone maths go through it in game logic.

`spike/shim/dxmath_c.cpp` exists because of something only a link run would reveal. Several modules
declare and call D3DX-named maths with **C linkage over the engine's own types**:

```c++
extern "C" PblMatrix* __stdcall D3DXMatrixMultiply(PblMatrix* pOut, const PblMatrix* pM1, const PblMatrix* pM2);
```

The original binary resolved those against the D3DX import library, so the decompilation has the
calls and no bodies — 12 functions, used by cloth, lens flare, sky dome, grass patch, aiming. They
now have exact D3DX9 implementations written against the layout the decompiled headers pin down:
`PblMatrix::m[4][4]` with `GetTranslation() == m[3][0..2]` and `GetXAxis() == m[0][0..2]`, i.e. the
same memory layout as `D3DXMATRIX`, row-vector maths, `v' = v * M`.

Two other honesty notes: `_wassert` was previously `#define`d to nothing, which silently deleted every
assertion in the GSM voice codec and produced an unparseable `(, 0)` — it is now a real function. And
`GetMapMode` as a macro broke `ReticuleDisplay.cpp`, which has its own game function of that name:
Win32 names are only added after checking the tree does not use them for something else.

## What is deliberately excluded from the core scope

`RedEngineFL/Graphics/PC`, `Platform/Pc`, `PebbleFL/pc`, `SoundFL/source/win`, `Steam/`, `lglcd`,
and `Battlefront2/Source/pcMain.cpp` — the last because the Win32 entry point is replaced by the
browser host's own main loop, which puts it in the same category as the D3D device. Their symbols
appear in `docs/backend-surface.md` category B: `SaveDevice` (26), `PblFile` (21), `Engine` (15),
`pcTerrainShader*`, `pcRedPrimitive`, `Semaphore`, the `Voice`/`Stream` sound plumbing. That list is
the backend contract, and it is short.

## The one file that still fails

`Battlefront2/Source/NetGame.cpp` defines `BblList<NetEntity, 512>::Remove` and
`::Itor::Advance` near the bottom of the TU. MSVC instantiates at end of translation unit, so that
was legal. clang instantiates on first use — and because the tree contains no declaration of
`template<> class BblList<NetEntity, 512>`, clang reads those as out-of-line definitions of an
already-instantiated primary template and refuses.

A hoist pass (move such definitions above first use) exists in `prepare_tree.py` and is **disabled on
purpose**: it reorders code, the correct insertion point depends on types declared mid-file
(`NetEntity` is defined inside the same .cpp), and it never addresses the actual gap, which is a
missing class specialization. Reconstructing `BblList<NetEntity, 512>` from the binary is upstream
Stage 1b/2 work; it is worth 404 symbols, including all of `NetGame` and `RedRenderer`.

## Numbers, honestly

| step | files OK |
|---|---|
| earlier "audit", tree already damaged by my own include rewrite | 127 |
| same audit, after that session's fixes | 143 |
| clean rebuild from pristine source, two safe edits, no new shims | 303 |
| + shim headers, forced prefix, D3DX math shim | 333 |
| + measure only real errors; C++ shims not leaked into C files | 372 → 469 |
| + prefix/shims rewritten cleanly (a scripted edit had broken `GUID` visibility) | 452 |
| + generated platform vocabulary (autogen converges) | 495 |
| + `FakeD3D*` fallback stripped, D3D interface members generated | 610 |
| + allocator/`va_list`/destructor-instantiation passes, DSound/DInput members | 626 |
| + SoundFL member allocators, `_wassert`, DInput structs, `GetMapMode` collision fixed | 633 |
| + FreeCamera correction, `pcMain` scoped out, browser shim TUs added | **637 / 638** |

Every jump came from reading the first error in each failing file rather than the last, and every
regression in this session came from a bulk scripted edit — the `HWND__` typedef clash, `DIDATAFORMAT`
defined twice, a `SetRect` macro that broke 124 files because the tree has a game function of that
name, a hoist pass that moved 216 blocks. Each is recorded here so the failure mode is visible, not
hidden.
