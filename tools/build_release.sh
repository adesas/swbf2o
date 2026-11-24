#!/usr/bin/env bash
# Phase 7 Release Build Pipeline:
# Compiles all 771 core TUs + browser WebGPU/WASM + Node WASM with -O2 -ffp-contract=off -msimd128
# and reports raw, gzip, and brotli binary sizes.
set -euo pipefail
cd "$(dirname "$0")/.."

export SWBF2_RELEASE=1
echo "=== [1/3] Compiling core archive (SWBF2_RELEASE=1: -O2 -ffp-contract=off -msimd128) ==="
./tools/compile_core.sh
echo "=== [2/3] Linking browser WebGPU WASM ==="
./tools/build_browser.sh browser
echo "=== [3/3] Linking Node headless oracle WASM ==="
./tools/build_browser.sh node

python3 - <<'PY'
import gzip, pathlib, subprocess

for p in [pathlib.Path("build/browser/swbf2.wasm"), pathlib.Path("build/browser/swbf2.js")]:
    if not p.exists():
        continue
    raw = p.read_bytes()
    gz = gzip.compress(raw, compresslevel=9)
    br_str = "n/a"
    try:
        res = subprocess.run(["brotli", "-q", "6", "-c"], input=raw, capture_output=True, check=True)
        br_str = f"{len(res.stdout) / (1024*1024):.2f} MiB"
    except Exception:
        pass
    print(f"  {p}: raw={len(raw)/(1024*1024):.2f} MiB | gzip={len(gz)/(1024*1024):.2f} MiB | brotli={br_str}")
PY
