#!/usr/bin/env bash
# Build the prim shape oracle for the host (-m32), the same flags as tools/compile_native.sh.
#
# Two links, on purpose:
#   1. strict link  - prints every symbol the collision TU needs. That is the list of functions
#                     and globals with no body in this tree, and it is part of the record.
#   2. run link     - resolves nothing, because the check only calls SetSphere/SetCylinder/SetBox
#                     and none of those paths touch the missing symbols. Deviation stated here
#                     rather than hidden behind fake stubs.
set -euo pipefail
cd "$(dirname "$0")/.."

CLANG="$PWD/vendor/emsdk/upstream/bin/clang++"
SRC="$PWD/build/src_fixed"
OUT="$PWD/build/prim_shape"

FLAGS=(-std=gnu++17 -fms-extensions -m32 -O0 -g -w -fpermissive -fno-strict-aliasing
       -fno-unsafe-math-optimizations -ffp-contract=off -fno-fast-math
       -include "$PWD/spike/native/native_intrin.h"
       -include "$PWD/spike/shim/swbf2_prefix.h"
       -I"$PWD/spike/shim" -I"$SRC" -I"$SRC/Battlefront2/Source" -I"$SRC/LuaFL/include")

mkdir -p "$OUT"
"$CLANG" "${FLAGS[@]}" -c spike/prim_shape_oracle.cpp -o "$OUT/oracle.o"
"$CLANG" "${FLAGS[@]}" -c "$SRC/Battlefront2/Source/CollisionPrimitive.cpp" -o "$OUT/CollisionPrimitive.o"
"$CLANG" "${FLAGS[@]}" -c "$SRC/Battlefront2/Source/CollisionUtil.cpp" -o "$OUT/CollisionUtil.o"

echo "strict link: symbols this check would need if it called anything else"
"$CLANG" "${FLAGS[@]}" "$OUT/oracle.o" "$OUT/CollisionPrimitive.o" -o "$OUT/oracle_strict" \
  2>&1 | grep -o "undefined reference to \`[^']*'" | sed "s/undefined reference to //" | sort -u \
  | tee "$OUT/unresolved.txt" || true

# Stubs are generated from what the link still needs after the real TU is included.
python3 tools/gen_prim_shape_stubs.py
"$CLANG" "${FLAGS[@]}" -c spike/prim_shape_stubs.cpp -o "$OUT/stubs.o"
"$CLANG" "${FLAGS[@]}" -c spike/prim_shape_host.cpp -o "$OUT/host.o"
"$CLANG" "${FLAGS[@]}" "$OUT/oracle.o" "$OUT/CollisionPrimitive.o" "$OUT/CollisionUtil.o" "$OUT/stubs.o" "$OUT/host.o" \
  -o "$OUT/oracle_bin"
echo "run link ok, binary at $OUT/oracle_bin"
