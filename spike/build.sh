#!/bin/bash
# M0 spike build: decompiled LuaFL + thin platform shim -> WASM
set -e
export EMSDK_QUIET=1
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
EMSDK="$ROOT/vendor/emsdk"
export PATH="$EMSDK:$EMSDK/upstream/emscripten:$PATH"
LUA=$ROOT/vendor/swbf2decomp/src/LuaFL

emcc -O2 \
  -I"$ROOT/spike/shim" \
  -I"$LUA/include" -I"$LUA/src" \
  -o "$ROOT/spike/lua_wasm/lua_wasm.js" \
  "$ROOT/spike/lua_wasm/main.cpp" "$ROOT/spike/shim/luamemory_shim.cpp" \
  "$LUA"/src/*.cpp "$LUA"/src/lib/*.cpp \
  -s ENVIRONMENT=node -s ALLOW_MEMORY_GROWTH=1

echo "built OK -> $ROOT/spike/lua_wasm/lua_wasm.js"
