#!/usr/bin/env python3
"""Drop a capsule in the engine's own world, in two builds, and compare where it comes to rest.

The question is whether the browser build and the native build put an object in the same place.
That only holds if the engine's own terrain and collision code is doing the work in both:

  * the level is loaded by the engine's own loader (LoadUtil::ReadDataFile), not by a browser-side
    reader, so both builds see the same chunks;
  * the ground is found by the engine's own ray (CollisionManager::RayHit with the filter
    COLL_STATIC | COLL_TERRAIN | COLL_RIGID, the same filter the engine uses for the chunks);
  * the capsule falls by the engine's own gravity (-9.8, integrated the way EntityItem::Update
    integrates it) and settles where the ray says the ground is.

The two builds are the same source: spike/level_harness.cpp is compiled into the browser module
and into the native oracle, and both link the engine's own translation units with the same
front-end (the Emscripten clang). The only difference is the target: wasm32 against i386.

The native oracle is built as ILP32 on purpose. The engine was written for 32-bit Windows and
still assumes it in places - GameState.cpp:1668 allocates four bytes per pointer in a vector of
pointers, and RedMemory.cpp truncates heap pointers to unsigned int - so a 64-bit native build
corrupts its own heap (measured: the write was caught with a hardware watchpoint at
GameState.cpp:1668, overflowing the block into the next heap tag). wasm32 and i386 have the same
pointer width and the same struct layouts, so the comparison is between two builds that can
agree, and the browser's wasm32 build is one of them.

Usage: python3 tools/ground_oracle_check.py [--json] [--skip-build] [--level tat/tat2]
"""
import argparse, json, os, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ORACLE = os.path.join(ROOT, 'build/native_oracle/ground_oracle')
MODULE = os.path.join(ROOT, 'build/browser/swbf2.js')
DATA = os.path.join(ROOT, 'data/Star wars battlefront 2/GameData/data/_lvl_pc')
NODE = os.path.join(ROOT, 'vendor/emsdk/node/24.19.0_64bit/bin/node')
MARKER = 'GROUND_ORACLE_JSON '


def run(cmd, env=None):
    e = {k: v for k, v in os.environ.items() if k != 'NODE_OPTIONS'}
    if env:
        e.update(env)
    r = subprocess.run(cmd, capture_output=True, text=True, env=e)
    if r.returncode:
        sys.exit(f'run failed: {" ".join(cmd)}\n{(r.stderr or "").strip()[-2000:]}')
    return r.stdout


def build(script):
    print(f'building {os.path.basename(script)} ...', flush=True)
    run(['bash', os.path.join(ROOT, 'tools', script)])


def parse(text):
    for line in text.splitlines():
        if line.startswith(MARKER):
            return json.loads(line[len(MARKER):])
    sys.exit('no result line in the output:\n' + text[-2000:])


def native_result(level):
    return parse(run([ORACLE, level], env={'SWBF2_DATA_ROOT': DATA + '/'}))


def browser_result(level):
    return parse(run([NODE, os.path.join(ROOT, 'browser/ground_probe.mjs'), level],
                      env={'SWBF2_MODULE': MODULE}))


def compare(a, b):
    """The two results must agree exactly. The numbers are floats printed with enough digits to
    round-trip, so an exact comparison is a comparison of the two builds' exact values."""
    problems = []
    if a['level'] != b['level']:
        problems.append(f"level: native {a['level']} vs browser {b['level']}")
    if a['collision_meshes'] != b['collision_meshes']:
        problems.append(f"collision meshes: native {a['collision_meshes']} vs "
                        f"browser {b['collision_meshes']}")
    if len(a['samples']) != len(b['samples']):
        problems.append(f"samples: native {len(a['samples'])} vs browser {len(b['samples'])}")
        return problems
    for na, nb in zip(a['samples'], b['samples']):
        where = f"({na['x']:g}, {na['z']:g})"
        for key in ('x', 'z', 'ok', 'terrain', 'ground', 'rest', 'steps'):
            if na[key] != nb[key]:
                problems.append(f'{where} {key}: native {na[key]!r} vs browser {nb[key]!r}')
    return problems


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--json', action='store_true')
    ap.add_argument('--skip-build', action='store_true')
    ap.add_argument('--level', default='tat/tat2')
    args = ap.parse_args()

    if not args.skip_build:
        if not os.path.exists(os.path.join(ROOT, 'build/native_core.a')):
            build('compile_native.sh')
        build('build_native_oracle.sh')
        if not os.path.exists(MODULE):
            build('build_browser.sh')
    for path, what in ((ORACLE, 'the native oracle'), (MODULE, 'the browser module')):
        if not os.path.exists(path):
            sys.exit(f'{what} is missing at {path}; build it first (drop --skip-build)')

    native = native_result(args.level)
    browser = browser_result(args.level)
    problems = compare(native, browser)

    report = {
        'level': args.level,
        'samples': len(native['samples']),
        'collision_meshes': native['collision_meshes'],
        'identical': not problems,
        'problems': problems,
        'native': native,
        'browser': browser,
    }
    if args.json:
        print(json.dumps(report, indent=1))
    else:
        for s in native['samples']:
            print(f"  ({s['x']:g}, {s['z']:g}) terrain {s['terrain']:g} ground {s['ground']:g} "
                  f"rest {s['rest']:g} steps {s['steps']}")
        if problems:
            print('\nFAIL: the two builds do not agree:')
            for p in problems:
                print('  ' + p)
        else:
            print(f"\nPASS: {len(native['samples'])} samples identical in the native and browser "
                  f"builds (collision meshes {native['collision_meshes']}).")
    sys.exit(1 if problems else 0)


if __name__ == '__main__':
    main()
