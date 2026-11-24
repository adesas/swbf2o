#!/usr/bin/env bash
# Build the vendored libbinkdec (Bink1 BIKf..BIKi decoder) for wasm32: build/binkdec.a
#
# Compiled without spike/shim/swbf2_prefix.h on purpose: these are third-party C FFT/DCT sources
# and a C++ decoder, and the engine's Win32/D3D prefix breaks them. It is a separate archive so
# it stays cached - it never changes, and rebuilding it on every browser link is pure waiting.
#
# The native twin is tools/native_binkdec.sh; both are linked because browser/platform/
# bink_browser.cpp drives a real decoder on both targets, and a seam that only exists on one
# side stops the oracles from comparing targets at all.
#
#   ./tools/build_binkdec_wasm.sh        # build (cached) -> build/binkdec.a
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT="$PWD"

EMCC="$ROOT/vendor/emsdk/upstream/emscripten/emcc"
EMXX="$ROOT/vendor/emsdk/upstream/emscripten/em++"
[ -x "$EMCC" ] || EMCC=$(command -v emcc)
[ -x "$EMXX" ] || EMXX=$(command -v em++)
AR="$ROOT/vendor/emsdk/upstream/bin/llvm-ar"
[ -x "$AR" ] || AR=$(command -v llvm-ar || true)
[ -n "${AR:-}" ] || { echo "no llvm-ar found" >&2; exit 1; }

INC=(-I browser/platform/binkdec/include -I browser/platform/binkdec/interface)
LIB=build/binkdec.a
OBJ=build/binkdec_obj

need=0
if [ ! -f "$LIB" ]; then
  need=1
else
  for f in browser/platform/binkdec/src/* browser/platform/binkdec/include/* \
           browser/platform/binkdec/interface/*; do
    if [ "$f" -nt "$LIB" ]; then need=1; break; fi
  done
fi
[ "$need" = "1" ] || exit 0

echo "building $LIB..."
mkdir -p "$OBJ"
objs=()
for f in browser/platform/binkdec/src/*.c; do
  o="$OBJ/$(basename "$f").o"
  "$EMCC" -O2 -target wasm32-unknown-emscripten "${INC[@]}" -Wno-unused-function -c "$f" -o "$o"
  objs+=("$o")
done
for f in browser/platform/binkdec/src/*.cpp; do
  o="$OBJ/$(basename "$f").o"
  "$EMXX" -O2 -std=gnu++17 -fno-exceptions -fno-rtti -target wasm32-unknown-emscripten \
    "${INC[@]}" -Wno-multichar -Wno-unused-variable -Wno-unused-function -c "$f" -o "$o"
  objs+=("$o")
done
rm -f "$LIB"
"$AR" rcs "$LIB" "${objs[@]}"
ls -la "$LIB"
