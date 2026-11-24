#!/usr/bin/env bash
# Compile the prepared tree to wasm objects and archive them.
#
# Nothing is excluded by directory. Earlier revisions skipped Graphics/PC, Platform/PC,
# SoundFL/source/win and friends wholesale, on the theory that the browser backend replaces
# them. That threw away real engine code for free: the sound engine, the save-file layer, the
# weather/grass/cloth effects, the renderer primitives. Most of those translation units compile
# as soon as the shim headers carry the vocabulary they reference, and a TU that compiles is
# worth more than a guess. What cannot be used is decided per file below; everything still
# missing shows up as an undefined symbol at link time, which is the honest work list for the
# backends.
#
#   ./tools/compile_core.sh                # compile + archive
#   ./tools/compile_core.sh --objects-only
set -euo pipefail
cd "$(dirname "$0")/.."

OBJ=build/obj
RES=build/results.tsv
CLANG="$PWD/vendor/emsdk/upstream/bin/clang"
EMCC="$PWD/vendor/emsdk/upstream/emscripten/emcc"
OPT_FLAGS=(-O0)
if [ "${SWBF2_RELEASE:-0}" = "1" ]; then
  OPT_FLAGS=(-O2 -ffp-contract=off -msimd128)
fi
FLAGS=(--sysroot="$PWD/vendor/emsdk/upstream/emscripten/cache/sysroot"
       -target wasm32-unknown-emscripten -fms-extensions "${OPT_FLAGS[@]}"
       -Wno-ignored-attributes -Wno-deprecated-non-prototype -Wno-invalid-offsetof
       -Wno-writable-strings -Wno-implicit-exception-spec-mismatch -Wno-narrowing
       -Wno-address-of-temporary -Wno-c23-extensions -Wno-unused-variable -Wno-unused-function -Wno-incompatible-pointer-types
       -include "$PWD/spike/shim/swbf2_prefix.h"
       -I"$PWD/spike/shim" -I"$PWD/build/src_fixed" -I"$PWD/build/src_fixed/LuaFL/include" -I"$PWD/build/src_fixed/LuaFL/src")

python3 - <<'PY'
import pathlib, os
skip = {
    # Win32 entry point: WinMain, the message pump, D3D9 device creation, PblRandom seeding and
    # a __try/__except crash guard. The browser host owns startup, and wasm32 clang has no SEH
    # lowering, so this file can only be replaced, never ported.
    "Battlefront2/Source/pcMain.cpp",
    # DirectSound capture/playback device plumbing. Same reasoning as above: the WebAudio
    # backend implements what the engine calls; these two files also carry decompiler defects
    # (a WAVEFORMATEXTENSIBLE whose fields lost their struct name; a capture interface
    # recorded as its base type), so they are replaced rather than repaired.
    "SoundFL/source/win/snddsbuffer.cpp",
    "SoundFL/source/win/sndstreamcapturesource.cpp",
    # The Win32 file layer behind PblFile (CreateFile/ReadFile plus the sound library's
    # overlapped-IO file thread). It compiles now that the shims carry Win32's vocabulary, but
    # the browser build owns file access: browser/platform/pblfile_browser.cpp implements
    # PblFile over the Emscripten VFS and over memory blocks handed over by the JS host, which
    # is what the level loaders need. Keeping both would be a duplicate symbol.
    "Battlefront2/Platform/Pc/pcPblFile.cpp",
    # The Win32 SaveGames directory layer (_wfindfirst/_wmkdir/_wfopen). Replaced by
    # browser/platform/savedevice_browser.cpp which persists profiles and saves synchronously
    # into OPFS /saves/savegames/ (with a POSIX ./SaveGames/ fallback for Node/native).
    "Battlefront2/Platform/Pc/pcSaveDevice.cpp",
    # Win32 CreateFileA/ReadFile/SetFilePointer file backend for SoundFL and RedMoviePlayer.
    # Replaced by browser/platform/sndfile_browser.cpp which streams through PblFile/OPFS.
    "SoundFL/source/win/sndfileplatform.cpp",
    # OpenSWBF2's mimalloc + Win32 HeapWalk/VirtualQuery heap backend. Replaced by our portable
    # browser/platform/redmemory_mi_browser.cpp which implements the exact RedMemoryMi.h contract
    # identically on wasm32 and native i386 without Win32 virtual memory APIs.
    "RedEngineFL/Memory/RedMemoryMi.cpp",
    # OpenSWBF2's Win32 SEH / MiniDumpWriteDump crash handler and watchdog thread. Replaced by
    # browser/platform/pccrash_browser.cpp which logs faults without Win32 dbghelp or SEH.
    "Battlefront2/Source/pcCrash.cpp",
    # OpenSWBF2 defines adler32 in Battlefront2/Source/unzip.cpp; ZlibFL/adler32.c is only in the
    # ZLibFL static library (never pulled in on MSVC). Under --whole-archive both would collide.
    "ZlibFL/adler32.c",
}
root = pathlib.Path("build/src_fixed")
files = sorted(str(p) for p in root.rglob("*") if p.suffix in (".c", ".cpp"))
rel = lambda f: os.path.relpath(f, root).replace(os.sep, "/")
skipped = [rel(f) for f in files if rel(f) in skip]
keep = [f for f in files if rel(f) not in skip]
pathlib.Path("build/scope.txt").write_text("\n".join(keep) + "\n")
print(f"scope: {len(keep)} translation units; skipped {len(skipped)}: {', '.join(skipped)}")
PY

rm -rf "$OBJ"; mkdir -p "$OBJ"; : > "$RES"

# The compiler command line is written *into* the worker rather than exported: exported
# variables do not reach grandchildren in this environment (script -> xargs -> worker arrives
# with an empty environment), which silently made every compile a "header not found" and
# reported 52/738 while the identical command line succeeded by hand.
{
  echo '#!/bin/bash'
  echo "CLANG='$CLANG'"
  echo "EMCC='$EMCC'"
  echo "FLAGS='${FLAGS[*]}'"
  echo "OBJ='$PWD/$OBJ'"
  echo "RES='$PWD/$RES'"
  cat <<'EOS'
for f in "$@"; do
  rel=$(echo "$f" | sed 's|build/src_fixed/||')
  out="$OBJ/$(echo "$rel" | sed 's|/|_|g; s|\.[^.]*$||').o"
  # LuaFL's ldo.cpp is the only TU in the tree that uses setjmp/longjmp, and in Emscripten
  # those are not libc symbols: the calls have to be lowered to __wasm_setjmp/emscripten_longjmp,
  # which only emcc's own compile step does (a raw clang object keeps calling plain setjmp, and
  # nothing in the sysroot provides it - the link then fails with 'undefined symbol: setjmp').
  # Compiling just this one file through emcc keeps the fix local to Lua's error unwinding.
  compiler="$CLANG"
  case "$rel" in LuaFL/src/ldo.cpp) compiler="$EMCC -sSUPPORT_LONGJMP=1" ;; esac
  err=$($compiler $FLAGS -c "$f" -o "$out" 2>&1)
  if ! echo "$err" | grep -q 'error:'; then
    printf 'ok\t%s\n' "$rel" >> "$RES.tmp"
  else
    printf 'error\t%s :: %s\n' "$rel" "$(echo "$err" | grep -m1 'error:' | sed 's/.*error: //')" >> "$RES.tmp"
    rm -f "$out"
  fi
done
EOS
} > /tmp/compile_worker.sh
chmod +x /tmp/compile_worker.sh

if [ -n "$($CLANG "${FLAGS[@]}" -fsyntax-only build/src_fixed/PebbleFL/Common/PblChunk.cpp 2>&1)" ]; then
  echo "preflight failed: the reference TU does not compile with these flags; not starting the sweep"
  exit 1
fi

rm -f "$RES.tmp"
xargs -P "$(nproc)" -n 8 /tmp/compile_worker.sh < build/scope.txt
mv "$RES.tmp" "$RES"

OK=$(awk -F'\t' '$1=="ok"' "$RES" | wc -l)
TOTAL=$(wc -l < "$RES")
echo "=== compiled $OK / $TOTAL ==="
echo "--- top error kinds:"
awk -F'\t' '$1=="error" {print $2}' "$RES" | sed "s/'[^']*'/X/g" | sort | uniq -c | sort -rn | head -10
echo "--- platform TUs that still fail:"
awk -F'\t' '$1=="error"' "$RES" | grep -E "Graphics/PC|Platform/PC|Platform/Pc|SoundFL/source/win|/Steam/|lglcd" | head -14 || true
echo "objects: $(ls "$OBJ" | wc -l)"

if [ "${1:-}" = "--objects-only" ]; then exit 0; fi

rm -f build/core.a
vendor/emsdk/upstream/bin/llvm-ar rc build/core.a "$OBJ"/*.o
vendor/emsdk/upstream/bin/llvm-ranlib build/core.a 2>/dev/null || true
ls -la build/core.a
