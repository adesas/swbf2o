#!/usr/bin/env bash
# Build libbinkdec (Bink1 BIKf..BIKi decoder) for the host target: build/native_binkdec.a
#
# Every native oracle links the browser's platform seams, and browser/platform/bink_browser.cpp
# drives a real Bink decoder (bdec_*). Without this archive the native link fails outright.
#
# Building a stub instead would make the link easy and the oracle blind: a platform seam that
# exists on only one side stops being a comparison of targets, which is the entire reason the
# native build exists. Same sources, same front-end, same -m32; only the target differs.
#
#   ./tools/native_binkdec.sh          # build (cached) -> build/native_binkdec.a
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT="$PWD"

CLANGXX="$ROOT/vendor/emsdk/upstream/bin/clang++"
CC="${CC:-$ROOT/vendor/emsdk/upstream/bin/clang}"
[ -x "$CC" ] || CC=cc
[ -x "$CLANGXX" ] || CLANGXX=g++

INC=(-I "$ROOT/browser/platform/binkdec/include" -I "$ROOT/browser/platform/binkdec/interface")
LIB="$ROOT/build/native_binkdec.a"
OBJ="$ROOT/build/native_binkdec_obj"

need=0
if [ ! -f "$LIB" ]; then
  need=1
else
  for f in "$ROOT"/browser/platform/binkdec/src/* "$ROOT"/browser/platform/binkdec/include/* \
           "$ROOT"/browser/platform/binkdec/interface/*; do
    if [ "$f" -nt "$LIB" ]; then need=1; break; fi
  done
fi
[ "$need" = "1" ] || exit 0

echo "building $LIB..."
mkdir -p "$OBJ"
objs=()
for f in "$ROOT"/browser/platform/binkdec/src/*.c; do
  o="$OBJ/$(basename "$f").o"
  "$CC" -std=gnu11 -m32 -O2 -w "${INC[@]}" -c "$f" -o "$o"
  objs+=("$o")
done
for f in "$ROOT"/browser/platform/binkdec/src/*.cpp; do
  o="$OBJ/$(basename "$f").o"
  "$CLANGXX" -std=gnu++17 -m32 -O2 -w -fno-exceptions -fno-rtti "${INC[@]}" -c "$f" -o "$o"
  objs+=("$o")
done
rm -f "$LIB"
ar rcs "$LIB" "${objs[@]}"
ls -la "$LIB"
