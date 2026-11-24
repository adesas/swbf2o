#!/usr/bin/env bash
# Build the movement oracle: the third oracle over the same shared objects.
#
# tools/build_native_oracle.sh builds the ground and spawn oracles and the objects they share -
# the harness, the native start-up and the browser's platform seams, all of which this oracle also
# links. This script adds the movement oracle's own main and links it the same way, so
# build_native_oracle.sh is left alone; it depends on that script having run first.
#
# The oracle is the same harness code (spike/level_harness.cpp) the browser module carries, so the
# numbers the two print are two builds' readings of one script, not two programs' readings of each
# other's.
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT="$PWD"

CLANGXX="$ROOT/vendor/emsdk/upstream/bin/clang++"
OBJ="$ROOT/build/native_oracle/obj"

if [ ! -d "$OBJ" ] || [ ! -f "$OBJ/spike_level_harness.o" ]; then
  echo "the shared oracle objects are missing; run tools/build_native_oracle.sh first" >&2
  exit 1
fi
# The oracle links browser/platform/bink_browser.cpp, which drives a real Bink decoder
# (bdec_*), so the host link needs the same decoder the wasm link gets.
bash "$ROOT/tools/native_binkdec.sh"

if [ ! -f "$ROOT/build/native_core.a" ]; then
  echo "build/native_core.a is missing; run tools/compile_native.sh first" >&2
  exit 1
fi

OPT="-${NATIVE_ORACLE_O:-O1}"
FLAGS=(
  -std=gnu++17 -fms-extensions -m32 "$OPT" -g -w -fpermissive -Wno-address-of-temporary
  -ffunction-sections -fdata-sections
  -include "$ROOT/spike/native/native_intrin.h"
  -include "$ROOT/spike/shim/swbf2_prefix.h"
  -I "$ROOT/spike/shim" -I "$ROOT/build/src_fixed" -I "$ROOT/build/src_fixed/LuaFL/include" -I "$ROOT/build/src_fixed/LuaFL/src"
)

MAIN_OBJ="$OBJ/main_input_camera"
mkdir -p "$MAIN_OBJ"
"$CLANGXX" "${FLAGS[@]}" -c "$ROOT/spike/native/native_main_input_camera.cpp" -o "$MAIN_OBJ/main.o"

OUT="$ROOT/build/native_oracle/input_camera_oracle"
"$CLANGXX" -std=gnu++17 -fms-extensions -m32 "$OPT" -w -fpermissive \
  -Wl,--gc-sections \
  "$OBJ"/*.o "$MAIN_OBJ/main.o" "$ROOT/build/native_core.a" "$ROOT/build/native_binkdec.a" -lm -o "$OUT"
echo "native input/camera oracle: $OUT"
