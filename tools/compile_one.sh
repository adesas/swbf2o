#!/bin/bash
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CLANG=$ROOT/vendor/emsdk/upstream/bin/clang
SYSROOT=$ROOT/vendor/emsdk/upstream/emscripten/cache/sysroot
SRC=$ROOT/build/src_fixed
OBJ=$ROOT/build/obj
RES=$ROOT/build/results.tsv
FLAGS="--sysroot=$SYSROOT -target wasm32-unknown-emscripten -fms-extensions -O0 -Wno-ignored-attributes -Wno-deprecated-non-prototype -Wno-invalid-offsetof -Wno-writable-strings -Wno-implicit-exception-spec-mismatch -Wno-narrowing -Wno-address-of-temporary -Wno-c23-extensions -Wno-unused-variable -include $ROOT/spike/shim/swbf2_prefix.h"
INC="-I$ROOT/spike/shim -I$SRC -I$SRC/LuaFL/include"
f="$1"
rel=$(echo "$f" | sed "s|.*/src_fixed/||")
mod=$(echo "$rel" | sed "s|/.*||")
out="$OBJ/$(echo "$rel" | sed 's|/|_|g; s|\.[^.]*$||').o"
err=$($CLANG $FLAGS $INC -c "$f" -o "$out" 2>&1)
if ! echo "$err" | grep -q 'error:'; then
  printf '%s\tok\t\n' "$mod" >> "$RES"
else
  printf '%s\terror\t%s\n' "$mod" "$(echo "$err" | grep -m1 'error:' | sed 's/.*error: //')" >> "$RES"
  rm -f "$out"
fi
