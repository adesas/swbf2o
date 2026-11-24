#!/usr/bin/env bash
# Compile the prepared tree to native objects and archive them.
#
# This is the native twin of tools/compile_core.sh: the same translation units, the same shim
# headers, the same flags - except that the target is the host compiler instead of wasm. The
# point of having it is the oracle: a question that has one answer only under wasm is not a
# question about the engine, it is a question about the port. The native build is what the wasm
# build is compared against.
#
# The scope is build/scope.txt, which tools/compile_core.sh writes: the engine's own translation
# units minus the handful the browser replaces (the Win32 entry point, the Direct3D9 device
# layer, the DirectSound device layer, the Win32 file layer).
#
#   ./tools/compile_native.sh              # compile + archive
#   ./tools/compile_native.sh --objects-only
set -euo pipefail
cd "$(dirname "$0")/.."

OBJ=build/native/obj
RES=build/native/results.tsv
# The same compiler front-end as the wasm build (the toolchain's own clang), pointed at the host
# target instead of wasm32. That keeps the oracle's comparison narrow: the same front-end, the
# same flags, the same shim headers - the only difference is the target the code is compiled for.
CLANG="${CXX:-$PWD/vendor/emsdk/upstream/bin/clang++}"
CC="${CC:-$PWD/vendor/emsdk/upstream/bin/clang}"
# The native build is 32-bit (-m32), and that is not a convenience.
#
# The engine is a 32-bit program: it was compiled for x86 in 2005 and the decompilation keeps
# that assumption in the source. GameState.cpp:1668 is the clearest example:
#
#     ppNew = (void**)::operator new(uiCapacity * 4);
#
# The vector of void* is allocated with four bytes per element, which is sizeof(void*) on the
# target the engine was built for. On x86-64 each element is eight bytes, so the buffer is half
# the size it needs to be and the vector's writes run off the end of the block into the heap's
# free-list metadata. The native oracle then hands out a live block twice and the engine walks a
# cyclic list - a fault that has nothing to do with the code under test, and that wasm32 does
# not have because on wasm32 sizeof(void*) is four.
#
# So the native build uses ILP32: the same pointer width, the same struct layouts, the same
# block headers and the same pointer truncations as the wasm build it is compared against, and
# as the retail binary.
FLAGS=(-std=gnu++17 -fms-extensions -m32 -O0 -g -w -fpermissive
       -Wno-ignored-attributes -Wno-deprecated-non-prototype -Wno-invalid-offsetof
       -Wno-writable-strings -Wno-implicit-exception-spec-mismatch -Wno-narrowing
       -Wno-address-of-temporary -Wno-c23-extensions -Wno-unused-variable -Wno-unused-function
       -include "$PWD/spike/native/native_intrin.h"
       -include "$PWD/spike/shim/swbf2_prefix.h"
       -I"$PWD/spike/shim" -I"$PWD/build/src_fixed" -I"$PWD/build/src_fixed/LuaFL/include" -I"$PWD/build/src_fixed/LuaFL/src")

if ! command -v "$CLANG" >/dev/null 2>&1; then
  for c in g++ clang++ c++; do
    if command -v "$c" >/dev/null 2>&1; then CLANG="$c"; break; fi
  done
fi
echo "native compiler: $CLANG ($($CLANG --version | head -1)); C compiler: $CC ($($CC --version | head -1))"

mkdir -p "$OBJ"; : > "$RES"


{
  echo '#!/bin/bash'
  echo "CLANG='$CLANG'"
  echo "CC='$CC'"
  echo "FLAGS='${FLAGS[*]}'"
  echo "OBJ='$PWD/$OBJ'"
  echo "RES='$PWD/$RES'"
  cat <<'EOS'
for f in "$@"; do
  rel=$(echo "$f" | sed 's|build/src_fixed/||')
  out="$OBJ/$(echo "$rel" | sed 's|/|_|g; s|\.[^.]*$||').o"
  # An object that is newer than its source is kept: the native build takes minutes, and the
  # oracle re-runs it often.
  if [ -f "$out" ] && [ "$out" -nt "$f" ]; then
    printf 'ok\t%s\n' "$rel" >> "$RES.tmp"
    continue
  fi
  # The engine's tree holds C as well as C++: the zlib copy and a few C sources. They are C, so
  # they are compiled as C - the wasm build gets that from clang's extension handling, the native
  # build has to be told.
  case "$f" in
    *.c) compiler="$CC -std=gnu11 -m32 -O0 -g -w -fpermissive -Wno-incompatible-pointer-types -include $PWD/spike/shim/swbf2_prefix.h -I$PWD/spike/shim -I$PWD/build/src_fixed" ;;
    *)   compiler="$CLANG $FLAGS" ;;
  esac
  err=$($compiler -c "$f" -o "$out" 2>&1)
  if ! echo "$err" | grep -q 'error:'; then
    printf 'ok\t%s\n' "$rel" >> "$RES.tmp"
  else
    printf 'error\t%s :: %s\n' "$rel" "$(echo "$err" | grep -m1 'error:' | sed 's/.*error: //')" >> "$RES.tmp"
    rm -f "$out"
  fi
done
EOS
} > /tmp/compile_native_worker.sh
chmod +x /tmp/compile_native_worker.sh

rm -f "$RES.tmp"
xargs -P "$(nproc)" -n 8 /tmp/compile_native_worker.sh < build/scope.txt
mv "$RES.tmp" "$RES"

OK=$(awk -F'\t' '$1=="ok"' "$RES" | wc -l)
TOTAL=$(wc -l < "$RES")
echo "=== compiled $OK / $TOTAL ==="
awk -F'\t' '$1=="error" {print $2}' "$RES" | sed "s/'[^']*'/X/g" | sort | uniq -c | sort -rn | head -10
echo "--- translation units that still fail:"
awk -F'\t' '$1=="error" {print "   " $1 ": " $2}' "$RES" | head -20 || true
echo "objects: $(ls "$OBJ" | wc -l)"

if [ "${1:-}" = "--objects-only" ]; then exit 0; fi

rm -f build/native_core.a
ar rc build/native_core.a "$OBJ"/*.o
ranlib build/native_core.a 2>/dev/null || true
ls -la build/native_core.a
