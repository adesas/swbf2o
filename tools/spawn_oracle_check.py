#!/usr/bin/env python3
"""Spawn a soldier in the engine's own world, in two builds, and compare what the engine made.

The question is whether the browser build and the native build build the same entity. That
only holds if the engine's own class, entity, weapon and physics code is doing the work in
both:

  * the level is loaded by the engine's own loader (LoadUtil::ReadDataFileOnHeap), not by a
    browser-side reader, so both builds see the same chunks;
  * the class is loaded the way the mission script loads one: the class's name is hashed and
    the loader reads the nested level whose id is that hash (LoadUtil.cpp:288-300), so the
    class's weapons, its ordnance and its models arrive with it;
  * the entity is built by the engine's own factory (EntitySoldierClass::Build), which allocates
    it from the engine's memory pool, adds it to the collision manager and activates its
    physics (EntitySoldier.cpp:5830-5851);
  * the ground it stands on is found by the engine's own ray query, and the entity is then
    advanced by the engine's own thread loop (Thread::UpdateAll, the call the game's own loop
    makes at GameLoop.cpp:1299).

The two builds are the same source: spike/level_harness.cpp is compiled into the browser module
and into the native oracle, and both link the engine's own translation units with the same
front-end (the Emscripten clang). The only difference is the target: wasm32 against i386.

The native oracle is built as ILP32 on purpose. The engine was written for 32-bit Windows and
still assumes it in places - GameState.cpp:1668 allocates four bytes per pointer in a vector of
pointers, and RedMemory.cpp truncates heap pointers to unsigned int - so a 64-bit native build
corrupts its own heap. wasm32 and i386 have the same pointer width and the same struct layouts,
so the comparison is between two builds that can agree, and the browser's wasm32 build is one of
them.

Usage: python3 tools/spawn_oracle_check.py [--json] [--skip-build] [--level tat/tat2]
                                            [--class cis_inf_rifleman]
"""
import argparse, json, os, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ORACLE = os.path.join(ROOT, 'build/native_oracle/spawn_oracle')
MODULE = os.path.join(ROOT, 'build/browser/swbf2.js')
DATA = os.path.join(ROOT, 'data/Star wars battlefront 2/GameData/data/_lvl_pc')
NODE = os.path.join(ROOT, 'vendor/emsdk/node/24.19.0_64bit/bin/node')
MARKER = 'SPAWN_ORACLE_JSON '


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


def native_result(level, class_name, class_file):
    return parse(run([ORACLE, level, class_name, class_file],
                     env={'SWBF2_DATA_ROOT': DATA + '/'}))


def browser_result(level, class_name, class_file):
    return parse(run([NODE, os.path.join(ROOT, 'browser/spawn_probe.mjs'), level, class_name,
                      class_file], env={'SWBF2_MODULE': MODULE}))


def compare(a, b):
    """The two results must agree exactly. The numbers are floats printed with enough digits to
    round-trip, so an exact comparison is a comparison of the two builds' exact values."""
    problems = []
    for key in ('level', 'class', 'class_file', 'spawned', 'updates'):
        if a[key] != b[key]:
            problems.append(f'{key}: native {a[key]!r} vs browser {b[key]!r}')
    for key in ('ground_rest', 'x', 'y', 'z', 'health', 'max_health'):
        if a[key] != b[key]:
            problems.append(f'{key}: native {a[key]!r} vs browser {b[key]!r}')
    for key in ('class_weapons', 'built_weapons', 'collision_bodies'):
        if a[key] != b[key]:
            problems.append(f'{key}: native {a[key]!r} vs browser {b[key]!r}')
    return problems


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--json', action='store_true')
    ap.add_argument('--skip-build', action='store_true')
    ap.add_argument('--level', default='tat/tat2')
    ap.add_argument('--class', dest='class_name', default='cis_inf_rifleman')
    ap.add_argument('--class-file', default=None,
                    help='the file the class is in, "file;class" (default side/cis.lvl;CLASS)')
    ap.add_argument('--extracted', action='store_true',
                    help='use the small class record made by tools/extract_class_level.py')
    args = ap.parse_args()
    faction = args.class_name.split('_', 1)[0]
    class_file = args.class_file or f'side/{faction}.lvl;{args.class_name}'
    if args.extracted:
        source, _, name = class_file.partition(';')
        source_path = os.path.join(DATA, source)
        extracted = os.path.join(ROOT, 'build/class_data',
                                 os.path.basename(os.path.dirname(source)),
                                 os.path.splitext(os.path.basename(source))[0],
                                 (name or args.class_name) + '.lvl')
        if not os.path.exists(extracted):
            run(['python3', os.path.join(ROOT, 'tools/extract_class_level.py'), source_path,
                 name or args.class_name])
        class_file = extracted + ';' + (name or args.class_name)

    if not args.skip_build:
        if not os.path.exists(os.path.join(ROOT, 'build/native_core.a')):
            build('compile_native.sh')
        build('build_native_oracle.sh')
        if not os.path.exists(MODULE):
            build('build_browser.sh')
    for path, what in ((ORACLE, 'the native oracle'), (MODULE, 'the browser module')):
        if not os.path.exists(path):
            sys.exit(f'{what} is missing at {path}; build it first (drop --skip-build)')

    native = native_result(args.level, args.class_name, class_file)
    browser = browser_result(args.level, args.class_name, class_file)
    problems = compare(native, browser)

    report = {
        'level': args.level,
        'class': args.class_name,
        'class_file': class_file,
        'identical': not problems,
        'problems': problems,
        'native': native,
        'browser': browser,
    }
    if args.json:
        print(json.dumps(report, indent=1))
    else:
        print(f"  spawned {native['spawned']} at ({native['x']:g}, {native['y']:g}, {native['z']:g}) "
              f"on ground {native['ground_rest']:g}")
        print(f"  health {native['health']:g}/{native['max_health']:g}, weapons "
              f"{native['built_weapons']}/{native['class_weapons']}, collision bodies "
              f"{native['collision_bodies']}, updates {native['updates']}")
        if problems:
            print('\nFAIL: the two builds do not agree:')
            for p in problems:
                print('  ' + p)
        else:
            print("\nPASS: the native and browser builds spawn the same soldier and agree on "
                  "every number.")
    sys.exit(1 if problems else 0)


if __name__ == '__main__':
    main()
