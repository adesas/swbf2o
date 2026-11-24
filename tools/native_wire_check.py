#!/usr/bin/env python3
"""Run the engine's own net code natively and compare it with the independent model.

Same C++ as build/net_wire.js (the wasm side), compiled with g++ instead of emscripten. That matters
for two reasons: the agreement is then between three readings of the source rather than two, and a
desync can be debugged in a real debugger instead of in a browser console.

Only two things are needed to make the decomp compile natively:
  -D__int64="long long"   g++ has no __int64; the size is the same, so layouts are unaffected
  a native shim           a clock, one flag, and the game-layer entry points the code calls

Usage:
  python3 tools/native_wire_check.py
"""
import json, os, shutil, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, 'build', 'src_fixed')
OBJ = os.path.join(ROOT, 'build', 'native', 'obj')
BIN = os.path.join(ROOT, 'build', 'native', 'net_wire_native')
NEED = [
    'Battlefront2/Source/NetPacket.cpp',
    'Battlefront2/Source/NetComm.cpp',
    'Battlefront2/Source/NetDrag.cpp',
    'PebbleFL/Common/PblTimeBase.cpp',
]
TUS = [
    'spike/net_wire.cpp',
    'spike/native/native_shim.cpp',
    'spike/native/native_main.cpp',
]
FLAGS = ['-std=gnu++17', '-fms-extensions', '-O0', '-D__int64=long long',
         '-include', 'spike/shim/swbf2_prefix.h',
         '-Ispike/shim', f'-I{SRC}', f'-I{SRC}/Battlefront2/Source', f'-I{SRC}/LuaFL/include']


def build():
    os.makedirs(OBJ, exist_ok=True)
    objs = []
    for rel in NEED:
        out = os.path.join(OBJ, os.path.basename(rel) + '.o')
        if not os.path.exists(out):
            r = subprocess.run(['g++', *FLAGS, '-c', os.path.join(SRC, rel), '-o', out],
                               capture_output=True, text=True, cwd=ROOT)
            if r.returncode:
                sys.exit(f'native compile failed for {rel}:\n{r.stderr.strip()}')
        objs.append(out)
    r = subprocess.run(['g++', *FLAGS, *[os.path.join(ROOT, t) for t in TUS], *objs,
                        '-no-pie', '-Wl,--unresolved-symbols=ignore-all', '-o', BIN],
                       capture_output=True, text=True, cwd=ROOT)
    if r.returncode:
        sys.exit(f'native link failed:\n{r.stderr.strip()}')


def norm_bytes(b):
    return b if isinstance(b, str) else ''.join(f'{x:02x}' for x in b)


def norm_value(v):
    return v if isinstance(v, str) else hex(v)


def main():
    if not shutil.which('g++'):
        sys.exit('g++ is not available')
    build()
    run = subprocess.run([BIN], capture_output=True, text=True, cwd=ROOT)
    if run.returncode:
        sys.exit(f'native run failed:\n{run.stderr.strip()}')
    oracle = json.loads(subprocess.run(['python3', os.path.join(ROOT, 'tools/net_wire_check.py'),
                                       '--json'], capture_output=True, text=True).stdout)

    failed = 0
    for line in run.stdout.strip().split('\n'):
        eng = json.loads(line)
        order = eng['decode']['order']
        label = 'decode_send_order' if order == list(range(len(oracle['datagrams']))) else 'decode_scrambled'
        o = oracle[label]
        checks = [
            ('datagram bytes', [norm_bytes(d['bytes']) for d in eng['datagrams']],
                               [norm_bytes(d['bytes']) for d in oracle['datagrams']]),
            ('header', eng['datagrams'][0]['header'], oracle['datagrams'][0]['header']),
            ('values', [norm_value(v) for v in eng['decode']['values']],
                       [norm_value(v) for v in o['values']]),
            ('group completion', eng['decode']['complete_after_each'], o['complete_after_each']),
        ]
        print(f'native engine, receive order {order}')
        for name, a, b in checks:
            ok = a == b
            failed += 0 if ok else 1
            print(f"  {'ok  ' if ok else 'FAIL'} {name}")

    print('\nagree (0 failed)' if not failed else f'\n{failed} failed')
    print('the wasm run of the same code and the same model already agree (browser/net_wire_check.mjs);')
    print('this is the third reading of the source, and the one you can debug with gdb.')
    return 0 if not failed else 1


if __name__ == '__main__':
    sys.exit(main())
