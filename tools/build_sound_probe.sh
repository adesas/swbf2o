#!/usr/bin/env bash
# Build the sound-property probe, linked from the same native objects as the other oracles.
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT="$PWD"
CLANGXX="$ROOT/vendor/emsdk/upstream/bin/clang++"
OBJ="$ROOT/build/native_oracle/obj"
if [ ! -f "$OBJ/spike_level_harness.o" ]; then
  echo "run tools/build_native_oracle.sh first" >&2
  exit 1
fi
FLAGS=(
  -std=gnu++17 -fms-extensions -m32 "-${NATIVE_ORACLE_O:-O1}" -g -w -fpermissive
  -ffunction-sections -fdata-sections
  -include "$ROOT/spike/native/native_intrin.h"
  -include "$ROOT/spike/shim/swbf2_prefix.h"
  -I "$ROOT/spike/shim" -I "$ROOT/build/src_fixed" -I "$ROOT/build/src_fixed/LuaFL/include"
)
MAIN_OBJ="$OBJ/main_sound_probe"
mkdir -p "$MAIN_OBJ"
"$CLANGXX" "${FLAGS[@]}" -c "$ROOT/spike/native/native_main_sound_probe.cpp" -o "$MAIN_OBJ/main.o"
OUT="$ROOT/build/native_oracle/sound_probe"
"$CLANGXX" -std=gnu++17 -fms-extensions -m32 "-${NATIVE_ORACLE_O:-O1}" -w -fpermissive \
  -Wl,--gc-sections "$OBJ"/*.o "$MAIN_OBJ/main.o" "$ROOT/build/native_core.a" -o "$OUT"
echo "native sound probe: $OUT"
