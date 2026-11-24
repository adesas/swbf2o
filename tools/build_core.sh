#!/usr/bin/env bash
# The whole core build, in the order it has to happen.
#
# Ordering is not a style point here: prepare_tree.py regenerates build/src_fixed from the
# pristine decompilation, which throws away every #include that include_pass.py added and every
# platform constant shim_autogen.py harvested. Running compile_core.py on a freshly prepared
# tree therefore loses ~120 files to 'member access into incomplete type' and looks like a
# mysterious regression. It bit twice, so the stages are now one command.
#
#   ./tools/build_core.sh              # full pipeline
#   ./tools/build_core.sh --skip-prepare   # keep the current tree, just recompile
set -euo pipefail
cd "$(dirname "$0")/.."

if [ "${1:-}" != "--skip-prepare" ]; then
  echo "### 1/4 prepare: pristine decompilation -> build/src_fixed"
  python3 tools/prepare_tree.py | tail -2

  echo "### 2/4 include pass: add the header that defines each incomplete type"
  python3 tools/include_pass.py 2 | tail -2

  echo "### 3/4 shim autogen: converge the platform vocabulary"
  python3 tools/shim_autogen.py 2 | tail -1
else
  echo "### 1-3/4 skipped (--skip-prepare): compiling the tree as it stands"
fi

echo "### 4/4 compile + archive"
exec ./tools/compile_core.sh "$@"
