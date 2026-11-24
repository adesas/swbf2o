#!/usr/bin/env bash
# Build the combat oracle: the blaster, the bots and the fight, in two builds.
#
# The native half is the same harness the browser module carries (spike/combat_harness.cpp and
# spike/level_harness.cpp), linked for the host as ILP32: the same front-end, the same engine
# objects, the same browser platform seams, a different target. That is the same contract
# tools/build_native_oracle.sh and tools/build_input_camera_oracle.sh have.
#
# The wasm half is a standalone module, linked from the same sources with the same flags as
# tools/build_browser.sh, because the browser module cannot call this harness yet: its source
# list and its exported-function list are both fixed (tools/build_browser.sh), and adding to them
# is a change to a file this track does not own. The standalone module is what makes the
# wasm32 measurement possible today; it is the browser module with the combat harness and its
# three exports added, and nothing else.
#
# Nothing here weakens the browser build: tools/build_browser.sh is not touched, and the module
# this script produces is a separate artifact under build/combat_oracle/.
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

bash "$ROOT/tools/build_binkdec_wasm.sh"

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

# ------------------------------------------------------------------ the native oracle (ILP32)
COMBAT_OBJ="$OBJ/spike_combat_harness"
mkdir -p "$COMBAT_OBJ"
"$CLANGXX" "${FLAGS[@]}" -c "$ROOT/spike/combat_harness.cpp" -o "$COMBAT_OBJ/combat_harness.o"

MAIN_OBJ="$OBJ/main_combat"
mkdir -p "$MAIN_OBJ"
"$CLANGXX" "${FLAGS[@]}" -c "$ROOT/spike/native/native_main_combat_oracle.cpp" -o "$MAIN_OBJ/main.o"

OUT="$ROOT/build/native_oracle/combat_oracle"
"$CLANGXX" -std=gnu++17 -fms-extensions -m32 "$OPT" -w -fpermissive \
  -Wl,--gc-sections \
  "$OBJ"/*.o "$COMBAT_OBJ/combat_harness.o" "$MAIN_OBJ/main.o" "$ROOT/build/native_core.a" "$ROOT/build/native_binkdec.a" -lm -o "$OUT"
echo "native combat oracle: $OUT"

# ------------------------------------------------------------------ the wasm oracle (wasm32)
#
# The flags below are tools/build_browser.sh's flags, copied because they have to stay in step
# with it: the prefix header neutralises MSVC's __declspec(align(16)) on PblMatrix, and without
# it every class containing a matrix gets different padding in the two builds.
if [ "${COMBAT_ORACLE_SKIP_WASM:-0}" = "1" ]; then
  echo "wasm combat oracle: skipped (COMBAT_ORACLE_SKIP_WASM=1)"
  exit 0
fi

export PATH="$ROOT/vendor/emsdk:$ROOT/vendor/emsdk/upstream/emscripten:$ROOT/vendor/emsdk/node/24.19.0_64bit/bin:$PATH"
mkdir -p "$ROOT/build/combat_oracle"

WASM_EXPORTS='_main,_malloc,_free,_swbf2_combat_run,_swbf2_combat_report,_swbf2_log_line'

emcc \
  -O1 \
  -std=gnu++17 \
  -target wasm32-unknown-emscripten \
  -fms-extensions \
  -Wno-ignored-attributes -Wno-deprecated-non-prototype -Wno-invalid-offsetof \
  -Wno-writable-strings -Wno-implicit-exception-spec-mismatch -Wno-narrowing \
  -Wno-address-of-temporary -Wno-c23-extensions -Wno-unused-variable -Wno-unused-function \
  -include "$ROOT/spike/shim/swbf2_prefix.h" \
  -I "$ROOT/spike/shim" \
  -I "$ROOT/build/src_fixed" \
  -I "$ROOT/build/src_fixed/LuaFL/include" \
  -I "$ROOT/build/src_fixed/LuaFL/src" \
  "$ROOT/spike/native/native_main_combat_oracle_wasm.cpp" \
  "$ROOT/spike/level_harness.cpp" \
  "$ROOT/spike/combat_harness.cpp" \
  "$ROOT/browser/platform/pblfile_browser.cpp" \
  "$ROOT/browser/platform/savedevice_browser.cpp" \
  "$ROOT/browser/platform/sndfile_browser.cpp" \
  "$ROOT/browser/platform/redmemory_mi_browser.cpp" \
  "$ROOT/browser/platform/pccrash_browser.cpp" \
  "$ROOT/browser/platform/startup_browser.cpp" "$ROOT/browser/platform/pc_globals.cpp" \
  "$ROOT/browser/platform/redambientlight_browser.cpp" \
  "$ROOT/browser/platform/pcRedRenderer_browser.cpp" \
  "$ROOT/browser/platform/dsound_browser.cpp" "$ROOT/browser/platform/com_browser.cpp" \
  "$ROOT/browser/platform/dinput_browser.cpp" \
  "$ROOT/browser/platform/steam_browser.cpp" "$ROOT/browser/platform/bink_browser.cpp" \
  "$ROOT/browser/platform/galaxy_browser.cpp" \
  "$ROOT/browser/platform/d3dx_browser.cpp" "$ROOT/browser/platform/pcseam_browser.cpp" \
  "$ROOT/browser/platform/redwarning_browser.cpp" \
  "$ROOT/browser/platform/winsock_browser.cpp" "$ROOT/browser/platform/winsock_c_browser.cpp" \
  -sMODULARIZE=1 \
  -sEXPORT_NAME=SWBF2COMBAT \
  -sEXPORTED_RUNTIME_METHODS=ccall,cwrap,callMain,FS,FS_createDataFile,getValue,setValue \
  -sALLOW_MEMORY_GROWTH=1 \
  -sINITIAL_MEMORY=134217728 \
  -sSTACK_SIZE=8388608 \
  -sASSERTIONS=1 \
  -sINVOKE_RUN=0 \
  -sEXIT_RUNTIME=0 \
  -sENVIRONMENT=web,node \
  -sSUPPORT_LONGJMP=1 \
  -sEXPORTED_FUNCTIONS="$WASM_EXPORTS" \
  -lc++abi-noexcept \
  -lc++ \
  "$ROOT/build/binkdec.a" \
  -Wl,--whole-archive "$ROOT/build/core.a" -Wl,--no-whole-archive \
  -o "$ROOT/build/combat_oracle/swbf2_combat.js"
echo "wasm combat oracle: $ROOT/build/combat_oracle/swbf2_combat.js"
