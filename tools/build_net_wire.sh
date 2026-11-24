#!/bin/bash
# Build the net wire check module: the engine's own packet layer, plus the check entry points.
#
#   tools/build_net_wire.sh
#
# The objects it links are the wire layer's real dependency closure, which is itself a result:
# NetPacket needs the QOS/drag queues (defined in NetDrag), the time base, and the game-layer
# packet release. Everything except the packet layer comes from the built tree; the game-layer
# callback is stubbed in spike/net_wire_stubs.cpp because the check does not run the game.
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
EMCC=$ROOT/vendor/emsdk/upstream/emscripten/emcc
export PATH="$ROOT/vendor/emsdk:$PATH"
OBJ=$ROOT/build/obj

$EMCC --sysroot=$ROOT/vendor/emsdk/upstream/emscripten/cache/sysroot \
  -target wasm32-unknown-emscripten -fms-extensions -O0 \
  -Wno-ignored-attributes -Wno-invalid-offsetof -Wno-writable-strings -Wno-narrowing \
  -Wno-unused-variable -include $ROOT/spike/shim/swbf2_prefix.h \
  -I$ROOT/spike/shim -I$ROOT/build/src_fixed -I$ROOT/build/src_fixed/Battlefront2/Source \
  -I$ROOT/build/src_fixed/LuaFL/include \
  $ROOT/spike/net_wire.cpp $ROOT/spike/net_wire_stubs.cpp \
  $OBJ/Battlefront2_NetPacket.cpp.o $OBJ/Battlefront2_NetComm.cpp.o \
  $OBJ/Battlefront2_Source_NetDrag.o $OBJ/PebbleFL_Common_PblTimeBase.o \
  -o $ROOT/build/net_wire.js \
  -sMODULARIZE -sEXPORT_NAME=NetWire -sENVIRONMENT=node \
  -sEXPORTED_FUNCTIONS=_wire_report \
  -sEXPORTED_RUNTIME_METHODS=HEAPU8,HEAPU32,ccall
