#!/bin/bash
# Build the transport check module: the engine's socket code, the browser transport, and the check.
#
#   tools/build_net_transport_check.sh
#
# The engine's pcNetSocket.cpp is linked as it was compiled, so the check measures the engine's own
# send and receive paths, not a reimplementation of them. Two things the linker insists on, and both
# are results worth keeping:
#   - the socket layer's closure is not just the packet layer: it needs the game layer (FindSource,
#     IsAdminPacketAuthorized, NetGameP::GetKey), the Galaxy API, and the QOS queues. The check does
#     not run those paths, so undefined symbols are tolerated; the names are written to
#     build/net_transport_undefined.txt by the first (strict) link below.
#   - the Winsock functions must come from the browser transport, not from Emscripten's socket layer,
#     or the check would measure a UDP proxy instead of the seam a browser has to implement. That is
#     why setsockopt/getsockopt are no longer no-op stubs in the shim.
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
EMCC=$ROOT/vendor/emsdk/upstream/emscripten/emcc
export PATH="$ROOT/vendor/emsdk:$ROOT/vendor/emsdk/upstream/emscripten:$PATH"
OBJ=$ROOT/build/obj
NM=$ROOT/vendor/emsdk/upstream/bin/llvm-nm

FLAGS="-O0 -std=gnu++17 -fms-extensions -Wno-ignored-attributes -Wno-invalid-offsetof \
  -Wno-writable-strings -Wno-narrowing -Wno-unused-variable -Wno-parentheses \
  -include $ROOT/spike/shim/swbf2_prefix.h -I$ROOT/spike/shim -I$ROOT/browser/platform \
  -I$ROOT/build/src_fixed -I$ROOT/build/src_fixed/Battlefront2/Source \
  -I$ROOT/build/src_fixed/LuaFL/include"

SOURCES="$ROOT/spike/net_transport_check.cpp $ROOT/spike/net_transport_stubs.cpp $ROOT/browser/platform/winsock_browser.cpp
  $OBJ/Battlefront2_pcNetSocket.cpp.o $OBJ/Battlefront2_NetPacket.cpp.o
  $OBJ/Battlefront2_NetComm.cpp.o $OBJ/Battlefront2_Source_NetDrag.o
  $OBJ/PebbleFL_Common_PblTimeBase.o"

# First link: strict, so the closure is named.
$EMCC $FLAGS $SOURCES -sMODULARIZE -sEXPORT_NAME=NetTransport -sENVIRONMENT=node \
  -sEXPORTED_FUNCTIONS=_net_transport_report -sEXPORTED_RUNTIME_METHODS=HEAPU8,ccall,addFunction -sALLOW_TABLE_GROWTH \
  -lc++abi-noexcept -o $ROOT/build/net_transport_strict.js > $ROOT/build/net_transport_link.log 2>&1
grep -oE "undefined symbol: .*" $ROOT/build/net_transport_link.log | sed 's/undefined symbol: //' \
  | sort -u > $ROOT/build/net_transport_undefined.txt
echo "strict link: $(wc -l < $ROOT/build/net_transport_undefined.txt) unresolved (build/net_transport_undefined.txt)"

# Second link: tolerant, so the check can run the paths that do not need them.
$EMCC $FLAGS $SOURCES -sMODULARIZE -sEXPORT_NAME=NetTransport -sENVIRONMENT=node \
  -sEXPORTED_FUNCTIONS=_net_transport_report -sEXPORTED_RUNTIME_METHODS=HEAPU8,ccall,addFunction -sALLOW_TABLE_GROWTH \
  -sERROR_ON_UNDEFINED_SYMBOLS=0 -lc++abi-noexcept -o $ROOT/build/net_transport.js
