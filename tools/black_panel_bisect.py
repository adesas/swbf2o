#!/usr/bin/env python3
"""Find which draw paints the flat black panel seen in live missions, by hiding draws by id range.

The panel is reproduced deterministically in tat2g_eli (Hero Assault, Mos Eisley): a deep rectangle
inside it measures mean luminance ~1.3 with ~99.9% of pixels near black, run after run, while the
ground around it renders normally. That makes it a clean test statistic: hide a range of draw ids
(swbf2_wgpu_debug_skip_draws, driven by the `skip=lo-hi` page query) and, if the panel disappears,
the draw responsible is inside the hidden range. Bisecting a ~300-draw frame is ~9 runs.

Why hiding draws rather than the ps-mode-4 id overlay: the overlay changes every generated fragment
shader, and doing that reliably crashes Lua GC during mission script load (see docs), so the overlay
cannot currently carry the measurement. Hiding a draw touches no shader at all.

Usage:
  tools/black_panel_bisect.py [totalDraws]      # default total 307
"""
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
NODE = os.path.join(ROOT, 'vendor/emsdk/node/24.19.0_64bit/bin/node')
CHECK = os.path.join(ROOT, 'browser/phase4_gamestate_check.mjs')
PNG = 'phase4_mission_tat2_eli.png'


def panel_score(png_path):
    import numpy as np
    from PIL import Image
    a = np.asarray(Image.open(png_path).convert('RGB')).astype(int)
    h, w, _ = a.shape
    reg = a[int(h * 0.75):int(h * 0.92), int(w * 0.35):int(w * 0.65)]
    lum = reg.sum(2)
    return float(lum.mean()), float((lum < 30).mean() * 100.0)


def run_once(skip, tag):
    out = os.path.join('/tmp/black_panel_bisect', tag)
    os.makedirs(out, exist_ok=True)
    env = dict(os.environ, SWBF2_SCREENSHOT_DIR=out)
    if skip:
        env['SWBF2_PAGE_QUERY'] = f'skip={skip[0]}-{skip[1]}'
    log = os.path.join(out, 'log')
    with open(log, 'w') as fh:
        rc = subprocess.run([NODE, CHECK], cwd=ROOT, env=env, stdout=fh, stderr=subprocess.STDOUT,
                            timeout=1800).returncode
    txt = open(log, errors='replace').read()
    draws = None
    for line in txt.splitlines():
        if 'tat2g_eli state=' in line and 'draws=' in line:
            draws = int(line.split('draws=')[1].split()[0])
            break
    png = os.path.join(out, PNG)
    if not os.path.exists(png):
        return None, draws, rc, f'no screenshot (rc={rc})'
    mean_lum, black_pct = panel_score(png)
    return black_pct, draws, rc, f'meanLum={mean_lum:6.1f} blackPct={black_pct:5.1f}'


def main():
    total = int(sys.argv[1]) if len(sys.argv) > 1 else 307
    print(f'baseline (no suppression), total draws assumed {total}', flush=True)
    base_black, draws, rc, desc = run_once(None, 'baseline')
    print(f'  baseline: {desc} draws={draws}', flush=True)
    if base_black is None or base_black < 50:
        sys.exit('baseline does not show the panel - nothing to bisect')
    if draws:
        total = draws

    lo, hi = 0, total
    step = 0
    while hi - lo > 1:
        mid = (lo + hi) // 2
        step += 1
        black, draws2, rc, desc = run_once((lo, mid), f'step{step}_{lo}-{mid}')
        gone = black is not None and black < 50
        print(f'  hide [{lo},{mid}) -> {desc} {"PANEL GONE" if gone else "panel still there"}', flush=True)
        if black is None:
            print('  run did not produce a screenshot; stopping bisection', flush=True)
            break
        if gone:
            hi = mid          # culprit is inside the hidden half
        else:
            lo = mid          # culprit is elsewhere
    print(f'\nculprit draw id: {hi} (hiding [{hi},{hi + 1}) should clear the panel)', flush=True)
    black, draws3, rc, desc = run_once((hi, hi + 1), f'confirm_{hi}')
    print(f'confirm hide [{hi},{hi + 1}) -> {desc}', flush=True)


if __name__ == '__main__':
    main()
