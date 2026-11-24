#!/usr/bin/env bash
# Build the Phase 0 WebGPU smoke test (emdawnwebgpu, Dedicated Worker + OffscreenCanvas).
set -euo pipefail
cd "$(dirname "$0")/.."
export PATH="$PWD/vendor/emsdk:$PWD/vendor/emsdk/upstream/emscripten:$PWD/vendor/emsdk/node/24.19.0_64bit/bin:$PATH"

OUT="$PWD/build/webgpu_smoke"
mkdir -p "$OUT"

em++ -O1 -std=c++20 \
  --use-port=emdawnwebgpu \
  browser/webgpu_smoke.cpp \
  -sMODULARIZE=1 -sEXPORT_ES6=1 -sEXPORT_NAME=createSmoke \
  -sENVIRONMENT=web,worker \
  -sALLOW_MEMORY_GROWTH=1 \
  -sEXPORTED_FUNCTIONS='["_wgpu_smoke_run"]' \
  -sEXPORTED_RUNTIME_METHODS='["UTF8ToString"]' \
  -o "$OUT/wgpu_smoke.js"

ls -lh "$OUT/wgpu_smoke.js" "$OUT/wgpu_smoke.wasm"
