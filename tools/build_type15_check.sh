#!/bin/bash
# Build the type-15 check: the engine's own readers and writers for PKT_GAME_REPLICA_UPDATE, which
# live in NetGame.cpp, linked against the packet layer.
#
#   tools/build_type15_check.sh
#
# NetGame.cpp is a game-layer file, so its dependency closure is enormous. The check only calls four
# functions from it - ReadDestMask, ReadTurnLowBits, ReadSwitchResponse, ReadMove, and their four
# writers - all of which touch nothing but the packet group and a couple of globals, so the rest of
# the closure is linked unresolved and never called.
#
# What is not stubbed: the packet layer, the writers, the readers, the turn bookkeeping. The only
# stand-in is ProcessedInputs' constructor, which lives in the joystick code this harness does not
# link; the harness only needs zeroed storage.
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
EMCC=$ROOT/vendor/emsdk/upstream/emscripten/emcc
export PATH="$ROOT/vendor/emsdk:$PATH"
OBJ=$ROOT/build/obj

# The readers and writers are copied out of NetGame.cpp by the extractor, so that the check links the
# engine's text without the game layer's global constructors.
python3 $ROOT/tools/extract_type15.py
cp $ROOT/build/type15_engine.inc $ROOT/build/type15_engine.cpp

$EMCC --sysroot=$ROOT/vendor/emsdk/upstream/emscripten/cache/sysroot \
  -target wasm32-unknown-emscripten -fms-extensions -O1 \
  -Wno-ignored-attributes -Wno-invalid-offsetof -Wno-writable-strings -Wno-narrowing \
  -Wno-unused-variable -include $ROOT/spike/shim/swbf2_prefix.h \
  -I$ROOT/spike/shim -I$ROOT/build/src_fixed -I$ROOT/build/src_fixed/Battlefront2/Source \
  -I$ROOT/build/src_fixed/LuaFL/include \
  $ROOT/spike/type15_check.cpp $ROOT/build/type15_engine.cpp $ROOT/spike/type15_stubs.cpp \
  $OBJ/Battlefront2_NetPacket.cpp.o $OBJ/Battlefront2_NetComm.cpp.o \
  $OBJ/Battlefront2_Source_NetDrag.o $OBJ/PebbleFL_Common_PblTimeBase.o \
  -o $ROOT/build/type15.js \
  -sMODULARIZE -sEXPORT_NAME=Type15 -sENVIRONMENT=node \
  -sEXPORTED_FUNCTIONS=_type15_report \
  -sEXPORTED_RUNTIME_METHODS=HEAPU8,ccall
