#!/usr/bin/env bash
# Export a clean, fresh-history public GitHub repository with zero personal paths,
# zero build/vendor/data artifacts, and zero old git history.
#
# Usage:
#   ./tools/export_public_repo.sh [target_dir]
#
# Defaults to ../swbf2-browser-edition next to the repository root.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TARGET="${1:-$(dirname "$ROOT")/swbf2-browser-edition}"
AUTHOR_NAME="${SWBF2_EXPORT_AUTHOR:-SWBF2O Maintainer}"
AUTHOR_EMAIL="${SWBF2_EXPORT_EMAIL:-maintainer@swbf2o.com}"

echo "Exporting clean public repository:"
echo "  source: $ROOT"
echo "  target: $TARGET"

rm -rf "$TARGET"
mkdir -p "$TARGET"

# Copy every clean tracked + new file while strictly excluding build/, data/, vendor/, .git/,
# and generated retail manifests or local secrets.
rsync -a \
  --exclude='.git/' \
  --exclude='build/' \
  --exclude='data/' \
  --exclude='vendor/' \
  --exclude='__pycache__/' \
  --exclude='*.pyc' \
  --exclude='browser/gamedata-manifest.json' \
  --exclude='chat1.txt' \
  --exclude='spike/lua_wasm/lua_wasm.js' \
  --exclude='spike/lua_wasm/lua_wasm.wasm' \
  --exclude='tools/restore_movies_from_iso.py' \
  --exclude='tools/restore_sound_streams_from_iso.py' \
  "$ROOT/" "$TARGET/"

cd "$TARGET"
git init -b main >/dev/null
git config user.name "$AUTHOR_NAME"
git config user.email "$AUTHOR_EMAIL"
git add -A
git commit -m "Initial public release: SWBF2 Browser Edition (WebGPU + WebRTC + BYOC)" >/dev/null

echo ""
echo "=== Privacy & Copyright Leak Audit ($TARGET) ==="
FAIL=0

if git grep -nI -E '(/home/[a-zA-Z0-9_-]+|archive\.org|201802|fullypcgames|Download Free Full Version|User=al|al@localhost|pi@local|BEGIN .*PRIVATE KEY)' -- . ':!tools/export_public_repo.sh' ; then
  echo "FAIL: Sensitive string found in exported repo!"
  FAIL=1
else
  echo "PASS: Zero personal paths, archive/rip mentions, or private keys."
fi

if git ls-files | grep -E '\.(lvl|bnk|mvs|iso|exe|dll|wasm|pem|key|crt|7z|zip|rar)$' ; then
  echo "FAIL: Disallowed binary or game asset found in exported repo!"
  FAIL=1
else
  echo "PASS: Zero game assets, binaries, or TLS keys tracked."
fi

echo "Tracked files: $(git ls-files | wc -l)"
echo "Git authors:   $(git log --format='%an <%ae>' | sort -u)"

if [ "$FAIL" -ne 0 ]; then
  exit 1
fi
echo "READY: Clean public GitHub repository generated at $TARGET"
