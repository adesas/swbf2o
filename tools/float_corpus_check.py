#!/usr/bin/env python3
"""Build the same float corpus three ways and compare: native with -ffp-contract=off, native with
-ffp-contract=fast, and wasm. If a host rounds differently from the file, a two-client desync is a
rounding difference and nothing in the packet format will explain it.

The comparison is between builds of one implementation, so it cannot tell you whether the model is
right - it tells you whether the same code behaves the same on both sides of the boundary you are
actually shipping to. -ffp-contract=off is what makes that comparison meaningful: with fast, the
compiler is allowed to fuse a multiply-add, and that is the difference this check is looking for.

Usage: python3 tools/float_corpus_check.py
"""
import json, os, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, 'spike/float_corpus.cpp')
MAIN = os.path.join(ROOT, 'spike/native/native_main_float_corpus.cpp')
OUT = os.path.join(ROOT, 'build/float_corpus')
os.makedirs(OUT, exist_ok=True)


def run(cmd):
    # The harness sets NODE_OPTIONS to a module import that some Node versions choke on.
    env = {k: v for k, v in os.environ.items() if k != 'NODE_OPTIONS'}
    r = subprocess.run(cmd, capture_output=True, text=True, env=env)
    if r.returncode:
        sys.exit('run failed: ' + ' '.join(cmd) + '\n' + (r.stderr or '').strip())
    return r.stdout.strip()


def main():
    print('the same corpus, three builds')
    for flags, exe in ((['-O2', '-ffp-contract=off', '-fno-fast-math'], 'corpus_off'),
                       (['-O2', '-ffp-contract=fast', '-mfma'], 'corpus_fast')):
        subprocess.run(['g++', '-std=gnu++17'] + flags + [SRC, MAIN, '-o', os.path.join(OUT, exe), '-lm'],
                       check=True)

    a = json.loads(run([os.path.join(OUT, 'corpus_off')]))
    b = json.loads(run([os.path.join(OUT, 'corpus_fast')]))

    emcc = os.path.join(ROOT, 'vendor/emsdk/upstream/emscripten/emcc')
    if not os.path.exists(emcc):
        sys.exit('emcc not found; the wasm side is the point of this check')
    subprocess.run([emcc, '-O2', '-std=gnu++17', SRC, '-sMODULARIZE', '-sEXPORT_NAME=Corpus',
                    '-sENVIRONMENT=node', '-sEXPORTED_FUNCTIONS=_float_corpus_report',
                    '-sEXPORTED_RUNTIME_METHODS=ccall', '-o', os.path.join(OUT, 'corpus_wasm.js')],
                   check=True)
    js = ("import F from '" + os.path.join(OUT, 'corpus_wasm.js') + "';"
         "const M = await F();"
         "console.log(M.ccall('float_corpus_report', 'string', [], []));")
    c = json.loads(run(['node', '--input-type=module', '-e', js]))

    def compare(name, key):
        diff = [(x, y, z) for x, y, z in zip(a[key], b[key], c[key]) if x != y or x != z]
        print(f'{len(a[key])} {key}: native-fast differs in {sum(1 for d in diff if d[0] != d[1])}, '
              f'wasm differs in {sum(1 for d in diff if d[0] != d[2])}')
        for d in diff[:5]:
            if d[0] != d[1]: print('  fast changes it:', d[0], '->', d[1])
            if d[0] != d[2]: print('  wasm changes it:', d[0], '->', d[2])
        return not diff

    ok = all(compare(k, k) for k in ('cases', 'height_path', 'roundtrip'))

    print()
    n_div = sum(1 for x in a['cases'] if x['div_ne_mul'])
    n_even = sum(1 for x in a['cases'] if x['up_ne_even'])
    n_fma = sum(1 for x in a['cases'] if x['fma_ne_plain'])
    print(f'within one build: division and reciprocal-multiply differ on {n_div} cases, '
          f'half-up and half-even on {n_even}, fused and unfused on {n_fma}')
    print('the pinned model divides. A host that multiplies by the reciprocal, or that rounds with '
          'floor(x+0.5) under -ffp-contract=fast, is not doing the same arithmetic.')

    if not ok:
        print('\nthe builds disagree. Before comparing two clients, pick one: the native build with '
              '-ffp-contract=off is the one gdb can walk.')
        return 1
    print('\nagree (0 failed)')
    return 0


if __name__ == '__main__':
    sys.exit(main())
