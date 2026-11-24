#!/usr/bin/env python3
"""Fight the same fight in two builds and compare what the engine made of it.

The question this asks is whether the browser build and the native build run the engine's own
blaster the same way. That only holds if the engine's own weapon, ordnance, collision and damage
code is doing the work in both:

  * the player's trooper is spawned and driven by the shared level harness
    (spike/level_harness.h), through the same HostInput the game itself uses;
  * the trigger the player holds is the engine's own trigger: HostInput's fire bit becomes
    Controllable::mControlFire[0] (PlayerController.cpp:463-470), and the weapon's own fire
    state machine reads that trigger (WeaponCannon.cpp:653) and fires (WeaponCannon.cpp:728);
  * the shot is the engine's own ordnance: WeaponCannon::Fire builds an OrdnanceDesc and
    OrdnanceLaserClass::Build (OrdnanceLaser.cpp:195) makes the bolt, which flies and hits by
    itself (OrdnanceBullet::Update, OrdnanceBullet.cpp:482-620);
  * the targets are the engine's own bots: Team::CreateCharacter with a negative player id
    (EntitySoldier.cpp:3719 makes their controller a UnitMobileController) and
    Character::Spawn (Character.cpp:952), on a team with the engine's own side and class;
  * the hit is the engine's own damage: OrdnanceBullet::Collide -> ApplyDamage
    (OrdnanceBullet.cpp:619) -> Damageable, and the harness only counts the health the engine's
    own damage state holds before and after.

The two builds are the same source: spike/combat_harness.cpp is compiled into the native oracle
and into the wasm module, and both run the same fight script (CombatHarnessRun, in the harness),
so the two reports are two builds' readings of one fight rather than two scripts agreeing.

The native oracle is built as ILP32 on purpose. The engine was written for 32-bit Windows and
still assumes it in places - GameState.cpp:1668 allocates four bytes per pointer in a vector of
pointers, and RedMemory.cpp truncates heap pointers to unsigned int - so a 64-bit native build
corrupts its own heap. wasm32 and i386 have the same pointer width and the same struct layouts,
so the comparison is between two builds that can agree, and the browser's wasm32 build is one of
them.

One thing the two builds do *not* share is their maths library: the native oracle links the
host's glibc and the wasm build links musl, and their atan2f and sinf differ by one unit in the
last place on some inputs. The harness therefore computes its own aim angles out of arithmetic
instead of asking the platform for them (see the comment on DetAtan2 in spike/combat_harness.cpp),
so what this check compares is the engine's behaviour and not the platform's library.

Usage: python3 tools/combat_oracle_check.py [--json] [--skip-build] [--targets 4]
                                            [--level tat/tat2]
"""
import argparse
import json
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ORACLE = os.path.join(ROOT, 'build/native_oracle/combat_oracle')
MODULE = os.path.join(ROOT, 'build/combat_oracle/swbf2_combat.js')
PROBE = os.path.join(ROOT, 'browser/combat_probe.mjs')
DATA = os.path.join(ROOT, 'data/Star wars battlefront 2/GameData/data/_lvl_pc')
NODE = os.path.join(ROOT, 'vendor/emsdk/node/24.19.0_64bit/bin/node')
MARKER = 'COMBAT_ORACLE_JSON '

PLAYER_CLASS = 'rep_inf_ep3_rifleman'
PLAYER_CLASS_FILE = 'side/rep.lvl;rep_inf_ep3_rifleman'
TARGET_CLASS = 'cis_inf_rifleman'
TARGET_CLASS_FILE = 'side/cis.lvl;cis_inf_rifleman'


def run(cmd, env=None):
    e = {k: v for k, v in os.environ.items() if k != 'NODE_OPTIONS'}
    if env:
        e.update(env)
    r = subprocess.run(cmd, capture_output=True, text=True, env=e)
    if r.returncode:
        sys.exit(f'run failed: {" ".join(cmd)}\n{(r.stderr or "").strip()[-4000:]}')
    return r.stdout


def build(script):
    print(f'building {os.path.basename(script)} ...', flush=True)
    run(['bash', os.path.join(ROOT, 'tools', script)])


def parse(text):
    for line in text.splitlines():
        if line.startswith(MARKER):
            return json.loads(line[len(MARKER):])
    sys.exit('no result line in the output:\n' + text[-2000:])


def native_result(level, targets):
    return parse(run([ORACLE, level, PLAYER_CLASS, PLAYER_CLASS_FILE, TARGET_CLASS,
                      TARGET_CLASS_FILE, DATA + '/', str(targets)]))


def browser_result(level, targets):
    return parse(run([NODE, PROBE, level, PLAYER_CLASS, PLAYER_CLASS_FILE, TARGET_CLASS,
                      TARGET_CLASS_FILE, str(targets)],
                     env={'SWBF2_MODULE': MODULE}))


def compare(a, b, path='', problems=None):
    """The two results must agree exactly. The numbers are floats printed with nine significant
    digits, which is what it takes to name a float32 exactly (FLT_DECIMAL_DIG is 9), so an exact
    comparison is a comparison of the two builds' exact values."""
    if problems is None:
        problems = []
    if isinstance(a, dict):
        for key in a:
            if key not in b:
                problems.append(f'{path}/{key}: missing in the browser report')
                continue
            compare(a[key], b[key], f'{path}/{key}', problems)
    elif isinstance(a, list):
        if len(a) != len(b):
            problems.append(f'{path}: native {len(a)} entries vs browser {len(b)}')
        for i, (x, y) in enumerate(zip(a, b)):
            compare(x, y, f'{path}[{i}]', problems)
    elif a != b:
        problems.append(f'{path}: native {a!r} vs browser {b!r}')
    return problems


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--json', action='store_true')
    ap.add_argument('--skip-build', action='store_true')
    ap.add_argument('--targets', type=int, default=4)
    ap.add_argument('--level', default='tat/tat2.lvl')
    args = ap.parse_args()

    if not args.skip_build:
        if not os.path.exists(os.path.join(ROOT, 'build/native_core.a')):
            build('compile_native.sh')
        build('build_combat_oracle.sh')
    for path, what in ((ORACLE, 'the native combat oracle'), (MODULE, 'the wasm combat module')):
        if not os.path.exists(path):
            sys.exit(f'{what} is missing at {path}; build it first (drop --skip-build)')

    native = native_result(args.level, args.targets)
    browser = browser_result(args.level, args.targets)
    problems = compare(native, browser)

    report = {
        'level': args.level,
        'targets': args.targets,
        'identical': not problems,
        'problems': problems,
        'native': native,
        'browser': browser,
    }
    if args.json:
        print(json.dumps(report, indent=1))
    else:
        units = native.get('units', [])
        bots = [u for u in units if u.get('role') == 'target']
        print(f"  {len(bots)} targets asked for, {native.get('targets_spawned')} spawned, "
              f"on team {native.get('target_team')} (side {native.get('target_side')})")
        for u in units:
            where = ','.join(f'{v:g}' for v in u.get('asked_at', []))
            print(f"  {u.get('role', '?'):6} {u.get('name', '?'):10} {u.get('class', '?'):22} "
                  f"at ({where}) health {u.get('health_before', 0):g} -> "
                  f"{u.get('health_after', 0):g} hits {u.get('hits', 0)} "
                  f"shots {u.get('shots_fired', 0):g}"
                  f"{' killed' if u.get('killed') else ''}")
        print(f"  hits {native.get('hits')}, kills {native.get('kills')}, damage dealt "
              f"{native.get('damage_dealt', 0):g}, damage taken "
              f"{native.get('damage_taken', 0):g}")
        print(f"  ordnance created {native.get('ordnance_total')}, peak "
              f"{native.get('ordnance_peak')}, expired {native.get('ordnance_expired')}")
        print(f"  turns {native.get('turns_run')} of engine time "
              f"({native.get('sim_time', 0):g}s), {len(native.get('turns', []))} turn rows")
        if problems:
            print('\nFAIL: the two builds do not agree:')
            for p in problems[:40]:
                print('  ' + p)
            if len(problems) > 40:
                print(f'  ... ({len(problems) - 40} more differences suppressed)')
        else:
            print("\nPASS: the native and browser builds fight the same fight and agree on "
                  "every number.")
    sys.exit(1 if problems else 0)


if __name__ == '__main__':
    main()
