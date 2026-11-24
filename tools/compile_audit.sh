#!/bin/bash
# Compile every file in the decomp tree with clang-for-wasm and categorise failures.
export EMSDK_QUIET=1
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PATH="$ROOT/vendor/emsdk:$ROOT/vendor/emsdk/upstream/emscripten:$PATH"
OUT=$ROOT/docs/compile-audit.txt
: > "$OUT"
FLAGS="-fms-extensions -O0 -Wno-ignored-attributes -Wno-deprecated-non-prototype -Wno-invalid-offsetof"
for mod in ZlibFL LuaFL PebbleFL Zephyr SoundFL RedEngineFL Battlefront2 Steam lglcd; do
  ok=0; fail=0; : > /tmp/errs_$mod.txt
  for f in $(find "$ROOT/vendor/swbf2decomp/src/$mod" -type f \( -name "*.c" -o -name "*.cpp" \)); do
    [ -f "$f" ] || continue
    err=$(emcc $FLAGS -I$ROOT/spike/shim -I$ROOT/vendor/swbf2decomp/src -c "$f" -o /tmp/audit.o 2>&1)
    if [ -z "$err" ]; then ok=$((ok+1)); else fail=$((fail+1)); echo "$err" >> /tmp/errs_$mod.txt; fi
  done
  printf "%-14s ok:%4d fail:%4d\n" "$mod" "$ok" "$fail" >> "$OUT"
  echo "--- top error kinds in $mod ---" >> "$OUT"
  grep -oh 'error: .*' /tmp/errs_$mod.txt | sed 's/error: //' | sort | uniq -c | sort -rn | head -6 >> "$OUT"
  echo "missing headers in $mod:" >> "$OUT"
  grep -oh "fatal error: '[^']*' file not found" /tmp/errs_$mod.txt | sort | uniq -c | sort -rn | head -8 >> "$OUT"
done
cat "$OUT"
