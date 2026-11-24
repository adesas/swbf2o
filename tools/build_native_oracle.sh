#!/usr/bin/env bash
# Build the native oracles: the same harness the browser runs, linked for the host.
#
# The oracles answer questions with numbers rather than adjectives: does the engine's terrain
# and collision code put a dropped capsule in the same place on the host and in the browser?
# Does the engine build the same soldier, with the same weapons and the same collision bodies,
# in both builds? Both sides run spike/level_harness.cpp over the same bytes of the same map, so
# any difference is a difference between the two builds and nothing else.
#
# The engine's own translation units come from build/native_core.a (tools/compile_native.sh),
# which is the host twin of build/core.a: the same front-end, the same shim headers, the same
# flags, a different target.
#
# The browser's platform seams are linked in as well, because they are part of the engine's
# environment on both sides: the renderer, the warning sink and the file reader are the same
# code in both builds, which is what makes the comparison a comparison of the target and not of
# two different programs.
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT="$PWD"

CLANGXX="$ROOT/vendor/emsdk/upstream/bin/clang++"
CC="${CC:-$ROOT/vendor/emsdk/upstream/bin/clang}"
[ -x "$CC" ] || CC=cc
OBJ="$ROOT/build/native_oracle/obj"

mkdir -p "$OBJ"

# The optimisation level can be overridden so the oracle can be built the way the browser build
# that is being compared is built: NATIVE_ORACLE_O=0 bash tools/build_native_oracle.sh
OPT="-${NATIVE_ORACLE_O:-O1}"

# 32-bit, like the wasm build it is compared against and like the retail binary: see the
# comment in tools/compile_native.sh about the engine's 32-bit assumptions.
FLAGS=(
  -std=gnu++17 -fms-extensions -m32 "$OPT" -g -w -fpermissive -Wno-address-of-temporary
  -ffunction-sections -fdata-sections
  -include "$ROOT/spike/native/native_intrin.h"
  -include "$ROOT/spike/shim/swbf2_prefix.h"
  -I "$ROOT/spike/shim" -I "$ROOT/build/src_fixed" -I "$ROOT/build/src_fixed/LuaFL/include" -I "$ROOT/build/src_fixed/LuaFL/src"
)

if [ ! -f "$ROOT/build/native_core.a" ]; then
  echo "build/native_core.a is missing; run tools/compile_native.sh first" >&2
  exit 1
fi

# libbinkdec for the host target - the oracle links browser/platform/bink_browser.cpp, which
# drives a real Bink decoder, so the host link needs the same decoder the wasm link gets.
BINK_LIB="$ROOT/build/native_binkdec.a"
bash "$ROOT/tools/native_binkdec.sh"

# The parts both oracles share.
COMMON_SOURCES=(
  "$ROOT/spike/level_harness.cpp"
  "$ROOT/spike/native/native_uni_va.cpp"
  "$ROOT/spike/native/native_startup.cpp"
)
for f in "$ROOT"/browser/platform/*.cpp; do
  # The browser's startup seam takes the arena from malloc; the native one has to take it from
  # a low mapping, because the engine's memory code truncates pointers to 32 bits. See
  # spike/native/native_startup.cpp.
  case "$(basename "$f")" in
    startup_browser.cpp) continue ;;
  esac
  COMMON_SOURCES+=("$f")
done

# The objects are shared between the two oracles: the harness, the startup and the seams are
# the same in both, and only the main program differs.
for f in "${COMMON_SOURCES[@]}"; do
  out="$OBJ/$(echo "${f#$ROOT/}" | sed 's|/|_|g; s|\.[^.]*$||').o"
  "$CLANGXX" "${FLAGS[@]}" -c "$f" -o "$out"
done

build_oracle() {
  local name="$1"
  local main="$2"
  local out="$ROOT/build/native_oracle/${name}_oracle"
  # Each oracle's main program has its own object directory: the two programs each define
  # main(), Log() and BrowserSceneCamera(), so they cannot be linked into the same binary.
  local main_obj_dir="$OBJ/main_$name"
  mkdir -p "$main_obj_dir"

  "$CLANGXX" "${FLAGS[@]}" -c "$main" -o "$main_obj_dir/main.o"
  "$CLANGXX" -std=gnu++17 -fms-extensions -m32 "$OPT" -w -fpermissive \
    -Wl,--gc-sections \
    "$OBJ"/*.o "$main_obj_dir/main.o" "$ROOT/build/native_core.a" "$BINK_LIB" -lm -o "$out"
  echo "native $name oracle: $out"
}

build_oracle ground "$ROOT/spike/native/native_main_ground_oracle.cpp"
build_oracle spawn "$ROOT/spike/native/native_main_spawn_oracle.cpp"
