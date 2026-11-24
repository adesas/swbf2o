#!/usr/bin/env python3
"""Drive one trooper with the same injected input in two builds, and compare every number.

The question is whether the browser build and the native build answer the same way when the host
gives them the same input. Both run the same harness (spike/level_harness.cpp), and the input
script is *in* the harness (LevelHarnessMovementScript), so the two builds cannot disagree about
what was injected - they drive the same trooper through the same turns by construction.

What is compared is everything the engine reports about that trooper, turn by turn:

  * the position, the direction it faces (the entity's own matrix) and its velocity;
  * the four control values the engine scaled from the input;
  * the soldier's own state (STAND, CROUCH, JUMP, ROLL, ...), its health and its energy;
  * the engine's own chase camera: where it is, which way it looks, its field of view and zoom;
  * the engine's own input record, so a disagreement about *what was injected* shows up as such
    rather than as a disagreement about what happened.

The numbers are floats printed with enough digits to round-trip, so the comparison is between the
two builds' exact values.

Usage: python3 tools/input_camera_check.py [--json] [--skip-build] [--level tat/tat2]
                                            [--class cis_inf_rifleman]
"""
import argparse, json, os, struct, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ORACLE = os.path.join(ROOT, 'build/native_oracle/input_camera_oracle')
MODULE = os.path.join(ROOT, 'build/browser/swbf2.js')
DATA = os.path.join(ROOT, 'data/Star wars battlefront 2/GameData/data/_lvl_pc')
NODE = os.path.join(ROOT, 'vendor/emsdk/node/24.19.0_64bit/bin/node')
MARKER = 'INPUT_CAMERA_ORACLE_JSON '

# The record's fields, in the order both builds print them: the engine's own turn counter, the
# soldier's state, the fire and special-move triggers, the engine's input record (its button bits
# and its mouse mode), then the position, the facing, the velocity, the four control values, the
# health, the camera (position, direction, field of view, zoom), the class's roll factor and the
# trooper's energy.
NAMES = ['updates', 'iState', 'iFireDown', 'iSpecialMoveDown', 'uiHostButtons', 'uiHostMouseMode',
         'x', 'y', 'z', 'dirX', 'dirY', 'dirZ', 'velX', 'velY', 'velZ', 'controlMove',
         'controlStrafe', 'controlTurn', 'controlPitch', 'fHealth', 'camX', 'camY', 'camZ',
         'camDirX', 'camDirY', 'camDirZ', 'camFov', 'camZoom', 'fRollSpeedFactor', 'fEnergy']
INT_FIELDS = {'updates', 'iState', 'iFireDown', 'iSpecialMoveDown', 'uiHostButtons',
              'uiHostMouseMode'}


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
    return parse(run([NODE, os.path.join(ROOT, 'browser/input_camera.mjs'), level, class_name,
                      class_file], env={'SWBF2_MODULE': MODULE}))


def f32(value):
    """The value as an IEEE single, which is what the engine's numbers are.

    The native oracle prints with %.9g and the browser prints the exact double of the same float.
    Nine significant digits round-trip a float exactly, so both describe the same single; rounding
    both to single is what makes the comparison a comparison of the two builds' floats rather than
    of the two builds' printing."""
    return struct.unpack('f', struct.pack('f', float(value)))[0]


def compare(a, b):
    """The two results must agree exactly, field by field, turn by turn."""
    problems = []
    for key in ('level', 'class', 'class_file', 'attached', 'ready'):
        if key in a and key in b and a[key] != b[key]:
            problems.append(f'{key}: native {a[key]!r} vs browser {b[key]!r}')
    for key in ('ground_rest', 'x', 'y', 'z'):
        if f32(a[key]) != f32(b[key]):
            problems.append(f'{key}: native {a[key]!r} vs browser {b[key]!r}')
    if a['script_turns'] != b['script_turns']:
        problems.append(f"script_turns: native {a['script_turns']} vs browser {b['script_turns']}")

    sa, sb = a['states'], b['states']
    if len(sa) != len(sb):
        problems.append(f'recorded turns: native {len(sa)} vs browser {len(sb)}')
    for i in range(min(len(sa), len(sb))):
        for j, name in enumerate(NAMES):
            va, vb = sa[i][j], sb[i][j]
            if name in INT_FIELDS:
                same = int(va) == int(vb)
            else:
                same = f32(va) == f32(vb)
            if not same:
                problems.append(f'turn {i} {name}: native {va!r} vs browser {vb!r}')
                if len(problems) > 40:
                    problems.append('... (more differences suppressed)')
                    return problems
    return problems


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--json', action='store_true')
    ap.add_argument('--skip-build', action='store_true')
    ap.add_argument('--level', default='tat/tat2')
    ap.add_argument('--class', dest='class_name', default='cis_inf_rifleman')
    ap.add_argument('--class-file', default=None,
                    help='the file the class is in, "file;class" (default side/cis.lvl;CLASS)')
    args = ap.parse_args()
    class_file = args.class_file or f'side/cis.lvl;{args.class_name}'

    if not args.skip_build:
        if not os.path.exists(os.path.join(ROOT, 'build/native_core.a')):
            build('compile_native.sh')
        build('build_native_oracle.sh')
        build('build_input_camera_oracle.sh')
        if not os.path.exists(MODULE):
            build('build_browser.sh')
    for path, what in ((ORACLE, 'the native input/camera oracle'), (MODULE, 'the browser module')):
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
        'turns': native.get('script_turns'),
        'problems': problems,
        'native': {k: v for k, v in native.items() if k != 'states'},
        'browser': {k: v for k, v in browser.items() if k != 'states'},
    }
    if args.json:
        print(json.dumps(report, indent=1))
    else:
        n = native.get('script_turns', 0)
        print(f"  attached {native.get('attached')}, ready {native.get('ready')}, "
              f"{n} driven turns")
        if native.get('states'):
            last = native['states'][-1]
            vals = dict(zip(NAMES, last))
            print(f"  final position ({vals['x']:g}, {vals['y']:g}, {vals['z']:g}), facing "
                  f"({vals['dirX']:g}, {vals['dirY']:g}, {vals['dirZ']:g})")
            print(f"  camera at ({vals['camX']:g}, {vals['camY']:g}, {vals['camZ']:g}), fov "
                  f"{vals['camFov']:g} rad")
        if problems:
            print('\nFAIL: the two builds do not agree:')
            for p in problems:
                print('  ' + p)
        else:
            print("\nPASS: the native and browser builds drive the trooper the same way and "
                  "agree on every number, turn by turn.")
    sys.exit(1 if problems else 0)


if __name__ == '__main__':
    main()
