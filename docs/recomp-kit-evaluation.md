# Bounded experiment: could `recomp-kit` recompile our `BattlefrontII.exe`?

Prompt: [veritr1x/recomp-kit](https://github.com/recomp-kit) is a static recompilation kit that turns
32-bit x86 Windows games into native apps, with a browser target; [austynoconnor/most-wanted-web](https://github.com/austynoconnor/most-wanted-web) proves a 2005 D3D9 title reaches the browser that way.
The question was whether that route beats compiling the decompiled engine, and the only honest way
to answer it was to run their toolchain on our executable.

**Result: blocked, for reasons specific to this build.** Not "the kit is bad" - their translator
handles the games it was built for. This binary is a different kind of target.

Reproduce everything below from `./recomp-probe` (Ghidra 12.1.3, portable JDK 21.0.13,
`recomp-kit` clone, and a `game.toml` whose identity values were read out of the PE headers).

**That working tree has since been deleted** (2.2 GB: Ghidra 881 MB, its zip 569 MB, the JDK 208 MB,
and 24,580 exported listings). Nothing here depends on it still existing - every number quoted below
was recorded while it was present, and all of it regenerates in about four minutes. The fetch URLs
are in the reproduction section at the end.

## What ran, and how long it took

| step | result |
|---|---|
| Ghidra 12.1.3 `analyzeHeadless` import + full default analysis + listing export | **~4 minutes** on 16 cores, `MAXMEM=12G` |
| Functions discovered | **24,580** (`out/BattlefrontII.exe/summary.txt`, `functions.tsv`) |
| `recomp-kit` translator over the full image | **aborted**, `TranslateError: unparsed operand 'DR0'`, 0 C files generated |
| Same over listings scoped to the real code sections (overlay removed, 11,018 functions) | **aborted**, `unparsed operand 'DR2'` |
| Same with `--only` on 400 real game functions | **aborted identically** |

I used a listings-only variant of their `ExportProgram.java` (no per-function decompilation) because
`tools/recomp/translate.py` reads only `functions/*.asm` - the `.c` files their export writes are
pseudocode, not an input - so skipping decompilation costs the measurement nothing. Listing format
and `functions.tsv` columns were kept byte-compatible so the translator could not fail because of
my variant. Ghidra's ~4-minute run means this probe is cheap to re-run.

## Where the 24,580 functions are

| region | bytes | functions | bytes/function |
|---|---|---|---|
| first `.text` `0x00401000-0x0078a000` | 3.71 MB | 9,308 | ~400 |
| second `.text` `0x01fee000-0x0256e000` | 5.77 MB | 2,728 | ~2,100 |
| `.rld` `0x0283d000-0x029d9000` | 1.69 MB | **10,834** | ~155 |
| `.rld20`, `.init`, elsewhere | 0.26 MB | 1,700 | - |

The two extremes are the tell. 5.77 MB of code yielding 2,728 functions means Ghidra could not
find boundaries in it; 1.69 MB yielding 10,834 tiny functions in a region named `.rld` means that
region is not ordinary game code.

## What the executable actually is

- **PE32 i386, 15,005,184 bytes, link timestamp 2005-12-05, ImageBase `0x00400000`, entry `0x00750009`**
  (the entry sits in the first, clean `.text`).
- **No D3DX9.** The import table's 12 DLLs are `d3d9, dsound, dinput8, winmm, ws2_32, wsock32,
  binkw32, ole32, advapi32, gdi32, kernel32, user32` - 199 functions total. `d3d9.dll` imports
  exactly `Direct3DCreate9` and `dsound`/`dinput8` import their one COM factory each, which is
  normal for COM-heavy APIs. This **corrects an earlier claim in this project's notes**: SWBF2 does
  not import D3DX9, so "we would need a general HLSL compiler" is not the blocker it was assumed to
  be. Their `dx/d3d9_ffp.cpp` + GLSL/MSL generation would in fact cover the API surface this game
  imports.
- **Not encrypted, but obfuscated.** Section entropies: first `.text` 6.64, second `.text` 6.48
  (code-like; a packed section would sit near 7.8). The second `.text` is polymorphic rather than
  encrypted - which is exactly why Ghidra disassembles it and still cannot find function
  boundaries. This function is representative:

  ```
  021eabba  PUSH -0x1                      ; SEH frame setup
  021eabc1  MOV EAX,FS:[0x0]
  021eabd4  PUSH 0x1dc2
  021eabd9  XOR dword ptr [ESP],0x9de      ; constant decrypted at run time
  021eabe1  CALL 0x021eabec                ; call-next-line = take EIP
  021eabe8  MOV DR0,EDX                    ; hardware watchpoint
  021eac08  PUSHFD
  021eac09  MOV EAX,dword ptr [ESP]
  021eac0c  TEST AH,0x1                    ; Trap Flag: "am I being debugged"
  ```

- **`.rld` is a runtime import/hook table**, holding ASCII pairs like `d3d9.dll`/`Direct3DCreate9`,
  `USER32.dll`/`DispatchMessageA`, `WS2_32.dll`, `KERNEL32.dll`/`CreateThread`/`FindFirstFileA`/
  `DeleteFileA`, plus `SHELL32.dll` (which the real import table does **not** import) and two MD5-ish
  integrity hashes with `CUSTOM`/`DEFAULT` markers. The strings `SecuROM` and `Classic` both appear
  in the image; `Ricochet` does not. Section naming (`.rld`, `.rld20`), rebuilt imports and leftover
  anti-debug code are consistent with a post-release patch/relocation overlay rather than the
  game's own code.

So the parts of this executable a static recompiler would need are clean and small; the parts it
would choke on are large and are not game logic.

## Where their translator gave up

`tools/recomp/translate.py` refuses the whole image rather than isolating a function:

1. Full image → `unparsed operand 'DR0'` at `seh_frame_sites` (line 2029) while translating a
   function body. Exactly one function in the export contains a `MOV DRn` - the anti-debug stub at
   `0x021eabb2`, in the second `.text`.
2. Listings scoped to exclude the second `.text`/`.rld`/`.rld20` (11,018 functions, **zero**
   occurrences of `DR` anywhere in the remaining listings) → `unparsed operand 'DR2'`, raised from
   `resolve_candidate` → `seh_frame_sites(probe_fn, image)`: a **function their resolver
   synthesises** while probing candidate addresses. It decodes image bytes it decided might be code,
   lands on a debug-register move, and their operand parser has no representation for it.
3. `--only` with 400 real functions from the first `.text` → fails in the same global pre-pass,
   before function selection matters.

Their `--allow-unmodelled` switch is the designed escape hatch for instructions that cannot be
modelled, but the crash happens in a pre-pass that runs before that logic, so the switch does not
apply. Fixing this means patching their translator's resolver to tolerate unparseable candidates -
possible, but it is a contribution to their tree, and each wall like this is a day, not an hour.

## The finding that decides it: our exe and the decompilation are different builds

`vendor/swbf2decomp` documents **14,595 distinct code addresses** in the first code section (from its
`// FUNCTION: SWBF2 0x…` annotations). Against `BattlefrontII.exe`:

| | |
|---|---|
| Ghidra function starts in that range | 9,308 |
| Overlap with the decompilation's addresses | **606** |
| Decompilation addresses Ghidra did not find | 13,989 |
| Overlap after shifting by ±0x400000, ±0x1000, ±0x1000000 | 606 / 0 / 604 / 0 - no base shift explains it |
| Is the decompilation's `RedAmbientLight` ctor `0x006afca0` a function start here? | **no** - Ghidra's nearest boundaries are `0x6ae080`…`0x6ae4e0` |

Same image base, same region, almost no shared function boundaries. This executable is **not the
build `swbf2decomp` documents** - and by extension not the build the community's whole body of RE
work describes, which is what a SWBF2 port on their kit would have to start from. Their kit's
per-game bring-up (Most Wanted: 957 lines of pinned guest addresses in `game.toml`, a 1211-line
analysis document, many values still placeholders) is built on top of exactly that kind of address
knowledge.

## What this means for the two routes

| | recompile the binary (their kit) | compile the decompilation (ours) |
|---|---|---|
| renderer/models | the game's own code, including the `modl`/`segm` loaders this project cannot read | hand-written backend; `segm` vertex layout must be reverse-engineered |
| this exe | blocked behind an obfuscated overlay that their translator aborts on, and which gates startup | irrelevant - the source is self-consistent |
| build match | does not match any existing SWBF2 RE artifact, so every address hint must be re-derived | the decompilation *is* the artifact |
| multiplayer | their winsock shim is `sendto`/`recvfrom` → `s_socket_error`; ours must be written either way | same |
| today | nothing runs | 732/732 TUs link with 0 undefined symbols, 2514 textures decode, Lua scripts execute |

Recommendation unchanged, and now with measurements behind it: continue on our spine. Take their
`web/player/fetch-backend.js` (manifest digest, `Content-Range` validation, prefetch budget, OPFS
persistence) for our streaming layer, and their browser operational lessons - Start-gated launch,
one session, bounded resolution, sized initial heap - because we will meet the same failures.
Revisit their tree if a SWBF2 build that matches the community's addresses ever gets pinned by
hash, since that is the precondition for both routes sharing any work.

## Reproducing

The probe tree was deleted, so recreating it starts with these (none of it needs root; Ghidra needs
a full JDK because it compiles its Java scripts, so a JRE-only system fails):

```sh
mkdir -p ~/recomp-probe && cd ~/recomp-probe
# GitHub hides release asset links in the page body; expanded_assets exposes them
curl -sL -o ghidra.zip "https://github.com/NationalSecurityAgency/ghidra/releases/download/Ghidra_12.1.3_build/ghidra_12.1.3_PUBLIC_20260817.zip" && unzip -q ghidra.zip
curl -sL -o jdk.tar.gz "https://api.adoptium.net/v3/binary/latest/21/ga/linux/x64/jdk/hotspot/normal/eclipse" && tar xzf jdk.tar.gz
python3 -m venv .venv && .venv/bin/pip -q install pefile capstone
git clone --depth 1 https://github.com/veritr1x/recomp-kit /tmp/recomp-kit
cp "<GameData>/BattlefrontII.exe" .   # sha256 26e68f8624426694592343f005432a0cecabbe368020b9b4807b2c35cd0c73d7
```

`ExportListingsOnly.java` is the listings-only variant of their `ExportProgram.java` (no
decompilation, byte-compatible `functions.tsv` and listing format) and is described in the section
above; `game.toml` needs the ten keys their `tools/game_config.py` calls REQUIRED_GAME_KEYS, with
`image_base = 0x00400000`, `entry_point = 0x00750009` and the SHA-256 above.
```sh
# Ghidra 12.1.3 + portable JDK 21 (Ghidra needs javac; a JRE is not enough)
JAVA_HOME=~/recomp-probe/jdk-21.0.13+9 MAXMEM=12G \
  ~/recomp-probe/ghidra_12.1.3_PUBLIC/support/analyzeHeadless ~/recomp-probe/proj swbf2 \
  -import ~/recomp-probe/BattlefrontII.exe -scriptPath ~/recomp-probe \
  -postScript ExportListingsOnly.java ~/recomp-probe/out -deleteProject

# their translator, strict
~/recomp-probe/.venv/bin/python /tmp/recomp-kit/tools/recomp/translate.py \
  --game ~/recomp-probe/swbf2-recomp-scoped --out ~/recomp-probe/gen-scoped \
  --report ~/recomp-probe/report-scoped.json
```

Gotchas hit, all real: GitHub's release page hides its asset links (use
`/releases/expanded_assets/<tag>`); Ghidra's launcher dies with "Unable to prompt user for JDK
path, no TTY detected" unless `JAVA_HOME` points at a full JDK; Debian's `awk` is `mawk`, which has
no `strtonum`; and a backgrounded `cd X && nohup … &` leaves the *parent* shell in the old
directory, which makes the first poll look like the job never started.
