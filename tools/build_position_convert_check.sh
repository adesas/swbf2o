#!/bin/bash
# Build the position quantisation check: the engine's own position converters and its own
# WritePositionVector / ReadPositionVector, linked against the packet layer.
#
#   tools/build_position_convert_check.sh
#
# NetState.cpp and NetGame.cpp are game-layer files: their dependency closure is the whole game. The
# check calls the eight converter classes, the two templates, the eight vector and scalar write/read
# functions, the four netLOD functions, and NetGame's two rules - none of which reach past a packet
# group, two globals and one 64-entry table - so those are copied out by the extractor and the rest of
# the closure is not linked at all.
#
# What is not stubbed: the packet layer, the converters, the quantisation, the readers, and IsLocalTo
# and GetMyGuest, which are the engine's own text. The only stand-ins are the one stub in
# spike/position_convert_stubs.cpp, which the check never reads, and the marked class shell and guest
# table in the generated file, which are a data structure and not a rule.
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
EMCC=$ROOT/vendor/emsdk/upstream/emscripten/emcc
export PATH="$ROOT/vendor/emsdk:$PATH"
OBJ=$ROOT/build/obj

python3 $ROOT/tools/extract_position_convert.py
cp $ROOT/build/position_convert_engine.inc $ROOT/build/position_convert_engine.cpp

$EMCC --sysroot=$ROOT/vendor/emsdk/upstream/emscripten/cache/sysroot \
  -target wasm32-unknown-emscripten -fms-extensions -O1 \
  -Wno-ignored-attributes -Wno-invalid-offsetof -Wno-writable-strings -Wno-narrowing \
  -Wno-unused-variable -include $ROOT/spike/shim/swbf2_prefix.h \
  -I$ROOT/spike/shim -I$ROOT/build/src_fixed -I$ROOT/build/src_fixed/Battlefront2/Source \
  -I$ROOT/build/src_fixed/LuaFL/include \
  $ROOT/spike/position_convert_check.cpp $ROOT/build/position_convert_engine.cpp \
  $ROOT/spike/position_convert_stubs.cpp \
  $OBJ/Battlefront2_NetPacket.cpp.o $OBJ/Battlefront2_NetComm.cpp.o \
  $OBJ/Battlefront2_Source_NetDrag.o $OBJ/PebbleFL_Common_PblTimeBase.o \
  -o $ROOT/build/position_convert.js \
  -sMODULARIZE -sEXPORT_NAME=PositionConvert -sENVIRONMENT=node \
  -sEXPORTED_FUNCTIONS=_position_report \
  -sEXPORTED_RUNTIME_METHODS=HEAPU8,ccall
