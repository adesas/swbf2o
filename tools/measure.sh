#!/bin/bash
# Rebuild every object from build/src_fixed and print pass/fail + top error kinds.
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
: > build/results.tsv; rm -rf build/obj; mkdir -p build/obj
find build/src_fixed -type f \( -name '*.c' -o -name '*.cpp' \) | grep -vE 'Graphics/PC|Platform/Pc|PebbleFL/pc|SoundFL/source/win|/Steam/|lglcd|Battlefront2/Source/pcMain.cpp' > /tmp/core_list.txt
xargs -P 8 -n 40 bash -c 'for f in "$@"; do "$ROOT/tools/compile_one.sh" "$f"; done' _ < /tmp/core_list.txt
echo "scope: $(wc -l < /tmp/core_list.txt) files"
awk -F'\t' '{c[$1" "$2]++} END{for(k in c) print "  " k, c[k]}' build/results.tsv | sort
echo "  TOTAL ok: $(grep -c $'\tok' build/results.tsv) / $(wc -l < build/results.tsv)"
echo "  top remaining errors:"
awk -F'\t' '$2=="error"{print $3}' build/results.tsv | sort | uniq -c | sort -rn | head -10 | sed 's/^/    /'
