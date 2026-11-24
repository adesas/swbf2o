#!/usr/bin/env bash
# One command to run the demo: build the browser module if it is missing, refresh the file
# manifest, and serve. Everything reads the real GameData dump - nothing is preloaded.
set -euo pipefail
cd "$(dirname "$0")/.."
PORT="${1:-8123}"
HOST="${2:-0.0.0.0}"   # pass 127.0.0.1 as the 2nd argument to keep this off the network

[ -f build/browser/swbf2.js ] || ./tools/build_browser.sh
# The demo's two factions are staged as individual class records rather than 101 MB and
# 148 MB side files. The original data is never modified; extraction is repeatable.
[ -f build/class_data/side/cis/cis_inf_rifleman.lvl ] || \
  python3 tools/extract_class_level.py side/cis.lvl cis_inf_rifleman
[ -f build/class_data/side/rep/rep_inf_ep3_rifleman.lvl ] || \
  python3 tools/extract_class_level.py side/rep.lvl rep_inf_ep3_rifleman
[ -f build/class_data/side/imp/imp_inf_rifleman.lvl ] || \
  python3 tools/extract_class_level.py side/imp.lvl imp_inf_rifleman
[ -f build/class_data/sound/common/sample_2eb95650.lvl ] || \
  python3 tools/extract_sound_sample.py sound/common.bnk --hash 0x2EB95650
python3 tools/gen_manifest.py
command -v fuser >/dev/null && fuser -k "${PORT}/tcp" 2>/dev/null || true
# serve.py prints every address it can be reached on; a LAN URL printed here too would
# be a second, lazier guess at the same information.
exec python3 tools/serve.py "${PORT}" --host "${HOST}"
