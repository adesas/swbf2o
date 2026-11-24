#!/usr/bin/env bash
# Measure what the browser backends still owe the engine.
#
# The shipped module links with zero undefined symbols only because archive linking pulls in the
# slice the host actually calls. The honest size of the remaining backend work is what the engine
# demands when *everything* it was compiled from has to resolve, so this links the whole archive
# with undefined symbols tolerated and counts them.
#
# How the count is taken matters. wasm-ld reports the symbols it could not resolve as warnings
# in the link log, and turns them into imports in the wasm. `llvm-nm -u` does not see those
# imports - it reports zero for a module with unresolved symbols - so counting with nm would
# report a vacuous "0 unresolved" for a link that in fact left symbols open. The list is therefore
# taken from the linker's own warnings, which name every symbol and say which object wanted it.
#
# The browser's platform files are the same ones tools/build_browser.sh links, so the number
# describes the state of the build that ships rather than an older slice of it.
set -uo pipefail
cd "$(dirname "$0")/.."
export PATH="$PWD/vendor/emsdk:$PWD/vendor/emsdk/upstream/emscripten:$PWD/vendor/emsdk/node/24.19.0_64bit/bin:$PATH"

OUT=build/backend-surface.txt
PROBE_LOG=build/browser/probe-link.log

# Kept in step with tools/build_browser.sh's COMMON list: the same browser sources, the same
# engine headers, the same shim.
emcc -O1 -std=gnu++17 -fms-extensions -Wno-invalid-offsetof -Wno-ignored-attributes \
  -Wno-writable-strings -Wno-narrowing -Wno-unused-variable -Wno-unused-function -Wno-address-of-temporary \
  -I spike/shim -I build/src_fixed -I build/src_fixed/LuaFL/include -I build/src_fixed/LuaFL/src \
  -include spike/shim/swbf2_prefix.h \
  browser/main.cpp spike/level_harness.cpp spike/combat_harness.cpp browser/platform/*.cpp \
  -sMODULARIZE=1 -sEXPORT_NAME=SWBF2 -sENVIRONMENT=web,node -sINVOKE_RUN=0 \
  -sERROR_ON_UNDEFINED_SYMBOLS=0 -sALLOW_MEMORY_GROWTH=1 \
  -sSUPPORT_LONGJMP=1 -sUSE_WEBGL2=1 -sFULL_ES3=1 -sMAX_WEBGL_VERSION=2 \
  -Wl,--whole-archive build/core.a -Wl,--no-whole-archive \
  -lc++abi-noexcept -lc++ \
  -o build/browser/whole-archive-probe.js > "$PROBE_LOG" 2>&1
LINK_RC=$?

# The linker's own words: every symbol it could not resolve, with the object that wanted it.
grep -oE "undefined symbol: [A-Za-z0-9_]+" "$PROBE_LOG" | sed 's/undefined symbol: //' \
  | sort -u > "$OUT"

if [ "$LINK_RC" -ne 0 ]; then
  # A failed probe says more than a count: duplicates have to be fixed first, and the errors name
  # what went wrong. Report them, do not report the count alone.
  echo "probe link failed (rc $LINK_RC) - see $PROBE_LOG"
  grep -E "error:" "$PROBE_LOG" | head -8
  echo "  distinct unresolved symbols named by the linker:"
  echo "    $(wc -l < "$OUT") (written to $OUT)"
  echo "  duplicate symbols:"
  grep -A2 "duplicate symbol" "$PROBE_LOG" | grep -oE "defined in .*" | sort -u | head -6
  exit 1
fi

TOTAL=$(wc -l < "$OUT")
cat <<EOF

whole-archive link: $TOTAL unresolved symbols (build/backend-surface.txt has the list)

  by kind:
$(awk '
  /__cxa_|_Unwind|__gxx|__emut|^_Zdl|^_Znw|operator/ {a++; next}
  /^_ZN?(Pbl|Red|Bbl|Zap|Fl|Gal|Lua|lua)/           {e++; next}
  /WSA|socket|connect|bind|send|recv|select|inet_|gethost|htons|ntohs|closesock|__Net|NetLink|Net/ {n++; next}
  /^_?(bink|Bink)/                                   {b++; next}
  /^_?(GLog|GS2|GServerBase|GClientBase|GPlayerBase|GServer|GClient|GPS|MasterServer|devpub|qr_)/ {g++; next}
  /^_?lglcd|^Lglcd|Logi|LCD/                         {l++; next}
  /_ZN.*Engine|_ZN.*Renderer|_ZN.*Sound|_ZN.*Device|Semaphore|_ZN.*Stream/ {k++; next}
  {o++}
  END {
    printf "    engine namespaces (Pbl/Red/Bbl/...) : %d\n", e+0
    printf "    network / winsock                   : %d\n", n+0
    printf "    C++ runtime / new-delete / unwinding: %d\n", a+0
    printf "    Bink video                          : %d\n", b+0
    printf "    GameSpy / Galaxy                    : %d\n", g+0
    printf "    Logitech LCD                        : %d\n", l+0
    printf "    renderer/device/sound classes       : %d\n", k+0
    printf "    other                               : %d\n", o+0
  }' "$OUT")

  most-demanded symbols:
$(grep -oE "undefined symbol: [A-Za-z0-9_]+.*" "$PROBE_LOG" | sort | uniq -c | sort -rn | head -12)
EOF
