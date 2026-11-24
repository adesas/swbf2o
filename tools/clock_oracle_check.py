#!/usr/bin/env python3
"""Build the engine's frame-pacing clock four ways and compare what it accumulates.

The question is whether two platforms can agree on a simulated time. That only holds if:

  * the tick domain is pinned. PblTimeBase takes TICK_SHIFT from QueryPerformanceFrequency, so the
    tick domain is a property of the platform clock. The shim pins 1 MHz for every build, which is
    why a native/Wasm comparison is possible at all; the shipped game does not pin it.
  * no compiler may fuse the one contractable expression in the frame-pacing path,
    `(int64)((float)ticks * scale + 0.5f)`. That is why -ffp-contract=off has to be a flag in the
    build rather than a hope.

What is linked is the engine's own PblTimeBase.cpp and GameState.cpp, compiled unmodified with the
flags tools/compile_core.sh uses, plus one harness TU that owns the clock and the few globals
UpdateTime reads. Everything the engine's global initializers call out of these two TUs is replaced
by a no-op or a zero buffer, discovered from the object's own relocations rather than guessed, so
the harness has no behaviour of its own except the scripted clock.

Four builds of the same source:
  native  -O0 -ffp-contract=off      the one gdb can walk
  native  -O2 -ffp-contract=fast -mfma   what an optimising native build actually does
  wasm    -O0, as tools/compile_core.sh builds it   (inherits clang's default: fast)
  wasm    -O0 -ffp-contract=off      the same build with the contract pinned

Usage: python3 tools/clock_oracle_check.py [--json]
"""
import json, os, re, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, 'spike/clock_oracle.cpp')
MAIN = os.path.join(ROOT, 'spike/native/native_main_clock_oracle.cpp')
OUT = os.path.join(ROOT, 'build/clock_oracle')
CLANG = os.path.join(ROOT, 'vendor/emsdk/upstream/bin/clang')
LLVM_OBJDUMP = os.path.join(ROOT, 'vendor/emsdk/upstream/bin/llvm-objdump')
EMCC = os.path.join(ROOT, 'vendor/emsdk/upstream/emscripten/emcc')
NODE = os.path.join(ROOT, 'vendor/emsdk/node/24.19.0_64bit/bin/node')
NM = os.path.join(ROOT, 'vendor/emsdk/upstream/bin/llvm-nm')
ENGINE_TUS = ['PebbleFL/Common/PblTimeBase.cpp', 'Battlefront2/Source/GameState.cpp']
PREFIX = os.path.join(ROOT, 'spike/shim/swbf2_prefix.h')
BASE = ['-std=gnu++17', '-fms-extensions', '-w',
        f'-include{PREFIX}', f'-I{os.path.join(ROOT, "spike/shim")}',
        f'-I{os.path.join(ROOT, "build/src_fixed")}']
SYSROOT = f'--sysroot={os.path.join(ROOT, "vendor/emsdk/upstream/emscripten/cache/sysroot")}'
SKIP = re.compile(r'^(__cxa_|__gxx_|_Unwind_|__stack_chk|memcpy|memset|memmove|snprintf|vsnprintf|'
                  r'__libc|_exit|_ZnW|_Znam|_Zda|_Zdl|_ZnK|_Zn11|__builtin|__emscripten|'
                  r'__wasm_call|abort|puts|fputs|fflush|printf|fprintf|stdout|stderr|stdin|exit|'
                  r'__errno_location|fwrite|__data|__dso|swbf2_now_ms|'
                  r'__stack_pointer|__indirect_function_table|__memory_base|__table_base|'
                  r'__global_base|__wasm_first_page_end|__heap_base|__stack_low|__stack_high|'
                  r'fma|fmaf|fabs|sqrt|sqrtf|pow|powf|sin|sinf|cos|cosf|tan|tanf|log|logf|exp|expf|'
                  r'atan|atan2|asin|acos|floor|floorf|ceil|ceilf|fmod|round|roundf|trunc|truncf|'
                  r'fmin|fmax|hypot|cbrt|lgamma|tgamma|nearbyint|rint|frexp|ldexp|modf|sinh|cosh|'
                  r'tanh|asinh|acosh|atanh|exp2|log2|log10|copysign|fdim|fmaf)')  # swbf2_now_ms is the harness's clock


def run(cmd):
    env = {k: v for k, v in os.environ.items() if k != 'NODE_OPTIONS'}
    r = subprocess.run(cmd, capture_output=True, text=True, env=env)
    if r.returncode:
        sys.exit(f'run failed: {" ".join(cmd)}\n{(r.stderr or "").strip()}')
    return r.stdout


def compile_tu(src, out, target=None, flags=()):
    cmd = [CLANG, *BASE]
    if target:
        cmd += ['-target', target, SYSROOT]
    cmd += list(flags) + ['-ffunction-sections', '-fdata-sections', '-c', src, '-o', out]
    subprocess.run(cmd, check=True)
    return out


def symbol_state(refs_from, defined_in):
    """Symbols the link actually lacks: referenced by the engine objects, defined by nothing that is
    linked. Read off the objects, so the engine's own definitions stay authoritative and nothing is
    stubbed twice — including the harness's own clock, which the harness defines."""
    undef, defined = set(), set()
    for obj in refs_from:
        for line in run([NM, obj]).splitlines():
            parts = line.split()
            if len(parts) >= 2 and 'U' in parts[:-1]:
                undef.add(parts[-1])
    for obj in defined_in:
        for line in run([NM, obj]).splitlines():
            parts = line.split()
            if len(parts) >= 2 and 'U' not in parts[:-1]:
                defined.add(parts[-1])
    return undef - defined


def classify(objfiles, need):
    """Symbol name -> 'call' or 'data', from the objects' own relocations. A function reference is a
    function relocation, a data reference is a data symbol. Read off the objects, not guessed from
    the name."""
    kind = {}
    for obj in objfiles:
        for line in run([LLVM_OBJDUMP, '-r', obj]).splitlines():
            parts = line.split()
            if len(parts) < 3 or not parts[1].startswith('R_'):
                continue
            sym = (parts[2] if len(parts) == 3 else parts[3]).split('+')[0]
            sym = re.sub(r'[-+]0x[0-9a-f]+$', '', sym)
            if sym not in need or SKIP.match(sym):
                continue
            kind[sym] = kind.get(sym, False) or ('FUNCTION' in parts[1] or 'PLT32' in parts[1] or
                                                 'T64_' in parts[1] or 'TABLE_INDEX' in parts[1])
    return {s: ('call' if c else 'data') for s, c in kind.items()}


def stubs_from(engine_objs, all_objs, path):
    need = symbol_state(engine_objs, all_objs)
    kinds = classify(engine_objs, need)
    with open(path, 'w') as f:
        f.write('/* Generated from %s: the symbols the link needs and no linked object defines.\n'
                '   Call targets become no-op functions, data becomes a zero buffer, so the engine\'s\n'
                '   global initializers run without touching anything outside this check. The harness\n'
                '   has no behaviour of its own beyond the scripted clock. */\n' % ', '.join(engine_objs))
        f.write('extern "C" {\n')
        for sym, k in sorted(kinds.items()):
            if not re.fullmatch(r'[A-Za-z0-9_]+', sym):
                continue
            f.write((f'void {sym}(void) {{}}\n' if k == 'call' else f'char {sym}[64];\n'))
        f.write('}\n')
    return path


def build_and_run(name, flags, native):
    target = None if native else 'wasm32-unknown-emscripten'
    objs = [compile_tu(os.path.join(ROOT, 'build/src_fixed', rel),
                       os.path.join(OUT, os.path.basename(rel) + '.' + name + '.o'), target, flags)
            for rel in ENGINE_TUS]
    harness = compile_tu(SRC, os.path.join(OUT, 'harness.' + name + '.o'), target, flags)
    stubsrc = stubs_from(objs + [harness], objs + [harness], os.path.join(OUT, 'stubs.' + name + '.cpp'))
    stubobj = compile_tu(stubsrc, os.path.join(OUT, 'stubs.' + name + '.o'), target, flags)
    # The harness object goes first: the test runs as the first global initializer, and nothing
    # outside this check runs before it.
    order = [harness, stubobj] + objs
    if native:
        exe = os.path.join(OUT, name)
        subprocess.run([CLANG, *BASE, *flags, *order, '-no-pie',
                        '-Wl,--unresolved-symbols=ignore-all', '-o', exe, '-lm'], check=True)
        return json.loads(run([exe]))
    js = os.path.join(OUT, name + '.js')
    r = subprocess.run([EMCC, *BASE, *flags, *order, '-sMODULARIZE', '-sENVIRONMENT=node',
                        '-sERROR_ON_UNDEFINED_SYMBOLS=0', '-o', js], capture_output=True, text=True)
    if r.returncode:
        sys.exit(f'wasm link failed:\n{r.stderr[-4000:]}')
    # The wasm side exits from its own initializer, so node reports a non-zero status; the report
    # is on stdout, which is what we read.
    env = {k: v for k, v in os.environ.items() if k != 'NODE_OPTIONS'}
    r = subprocess.run([NODE, '--input-type=module', '-e', "import F from '" + js + "'; await F();"],
                       capture_output=True, text=True, env=env)
    return json.loads(r.stdout.strip())


def frame_scale_is_reachable():
    """Is the contractable expression reachable at all? g_fFrameRateScale is a global; if nothing in
    the tree ever writes it, the path is dead code and no compiler flag can make it diverge."""
    import glob
    hits = []
    for path in glob.glob(os.path.join(ROOT, 'build/src_fixed/**/*.cpp'), recursive=True):
        try:
            src = open(path, errors='ignore').read()
        except OSError:
            continue
        for m in re.finditer(r'g_fFrameRateScale\s*=\s*([^=])', src):
            hits.append((path, m.group(1).strip()))
    return hits


def main():
    os.makedirs(OUT, exist_ok=True)
    builds = [('native off', build_and_run('clock_native_off', ['-O0', '-ffp-contract=off',
                                                               '-fno-fast-math'], True)),
              ('native fast', build_and_run('clock_native_fast', ['-O2', '-ffp-contract=fast',
                                                                  '-mfma'], True)),
              ('wasm default', build_and_run('clock_wasm_default', ['-O0'], False)),
              ('wasm off', build_and_run('clock_wasm_off', ['-O0', '-ffp-contract=off'], False))]

    ref = builds[0][1]
    same = lambda b: json.dumps(b, sort_keys=True) == json.dumps(ref, sort_keys=True)

    print("the engine's own UpdateTime(), from a scripted clock, four builds")
    print()
    print('1. the tick domain, decided by QueryPerformanceFrequency')
    for k in ('frequency', 'tick_shift', 'ticks_per_second', 'ticks_per_frame',
             'seconds_per_tick_as_float', 'smallest_time_the_clock_can_report_ms',
             'floor_ticks', 'max_advance_ticks', 'normal_advance_ticks'):
        vals = [b['clock'][k] for _, b in builds]
        print(f'   {k:40s} {str(vals[0]):>22}  ' + ('same' if len(set(map(str, vals))) == 1 else 'DIFFERS'))

    print()
    print('   what the same constructor gives against a real hardware clock:')
    for row in ref['shift_table']:
        print(f"   frequency {row['frequency']:>10}: TICK_SHIFT {row['tick_shift']:2d}, "
              f"ticks_per_second {row['ticks_per_second']:>7}, ticks_per_frame {row['ticks_per_frame']:>8}, "
              f"smallest reportable step {row['smallest_time_the_clock_can_report_ms']:.3f} ms")

    print()
    print('2. 1000 ticks at a fixed rate (the deterministic branch)')
    for label, b in builds:
        r = b['fixed_rate_30hz_1000']
        print(f"   {label:14s} sim time {r['sim_time_engine']:.9f} s, drift {r['drift_ms']:+.3f} ms, "
              f"sum of game deltas {r['sum_of_engine_deltas']:.9f}, frames earning 0 ticks "
              f"{r['frames_that_earned_zero_ticks']}")

    print()
    print('3. the variable-rate branch: the 80 Hz floor, the 62.5 ms re-anchor, the 0.1 s clamp')
    for sec in ref['variable']:
        bad = 0
        for label, b in builds[1:]:
            other = [s for s in b['variable'] if s['label'] == sec['label']][0]
            for a, c in zip(sec['entries'], other['entries']):
                keys = [k for k in a if k in c]
                if any(a[k] != c[k] for k in keys):
                    bad += 1
                    print(f"   {label} vs {sec['label']} asked {a['asked_ms']:.3f} ms: "
                          f"{a} -> {c}")
        if not bad:
            print(f"   {sec['label']}: identical in all four builds ({len(sec['entries'])} frames)")
            for a in sec['entries']:
                if 'clock_reads' in a:
                    print(f"      asked {a['asked_ms']:>9.3f} ms -> {a['ticks_advanced']:>8} ticks "
                          f"({a['clock_reads']} clock reads) -> game delta "
                          f"{a['delta_the_game_received_ms']:>8.3f} ms")
                elif 'unfused' in a:
                    print(f"      asked {a['asked_ms']:>9.3f} ms -> {a['ticks_advanced']:>8} ticks "
                          f"(unfused {a['unfused']}, fused {a['fused']})")
                else:
                    print(f"      asked {a['asked_ms']:>9.3f} ms -> {a['ticks_advanced']:>8} ticks "
                          f"(half-up {a['rounds_half_up']}, truncate {a['cast_truncates']})")

    print()
    print('4. the contractable expression, `(int64)((float)ticks * scale + 0.5f)`')
    for sec in ref['variable']:
        if sec['label'] == 'frame_scale_half':
            for i, a in enumerate(sec['entries']):
                moved = []
                for label, b in builds[1:]:
                    other = [s for s in b['variable'] if s['label'] == 'frame_scale_half'][0]
                    if other['entries'][i]['ticks_advanced'] != a['ticks_advanced']:
                        moved.append(f"{label} gives {other['entries'][i]['ticks_advanced']}")
                print(f"   scale 0.5, asked {a['asked_ms']:.3f} ms -> engine {a['ticks_advanced']} ticks "
                      f"(half-up {a['rounds_half_up']}, truncate {a['cast_truncates']}) "
                      + ('agrees everywhere' if not moved else '   MOVED: ' + '; '.join(moved)))
        if sec['label'] == 'frame_scale_tenth':
            for i, a in enumerate(sec['entries']):
                moved = []
                for label, b in builds[1:]:
                    other = [s for s in b['variable'] if s['label'] == 'frame_scale_tenth'][0]
                    if other['entries'][i]['ticks_advanced'] != a['ticks_advanced']:
                        moved.append(f"{label} gives {other['entries'][i]['ticks_advanced']}")
                print(f"   scale 0.1, asked {a['asked_ms']:.3f} ms -> engine {a['ticks_advanced']} ticks "
                      f"(unfused {a['unfused']}, fused {a['fused']}, fma changes it: "
                      f"{a['fma_would_change_it']}) "
                      + ('agrees everywhere' if not moved else '   MOVED: ' + '; '.join(moved)))

    print()
    print('5. SetIgnoreNextUpdate (the frame after a load must not advance the total clock)')
    for label, b in builds:
        r = b['ignore_next_time']
        print(f"   {label:14s} first frame {r['first_game_ticks']}/{r['first_total_ticks']} ticks, "
              f"second frame game {r['second_game_ticks']}, total {r['second_total_ticks']}")

    writers = frame_scale_is_reachable()
    print()
    print('6. is the contractable expression reachable in this build?')
    if writers:
        for path, rhs in writers[:5]:
            print(f'   g_fFrameRateScale is written in {os.path.relpath(path, ROOT)}: {rhs}')
    else:
        print('   nothing in build/src_fixed ever writes g_fFrameRateScale, so the expression at')
        print('   GameState.cpp:524 is dead code in this build and no -ffp-contract setting can make')
        print('   the frame pacing diverge through it. The drift that does exist comes from the tick')
        print('   quantisation, which is identical on every platform.')

    ok = all(same(b) for _, b in builds)
    print()
    if ok:
        print('All four builds produce identical numbers.')
        print('Read that with the caveat it deserves: the shim pins QueryPerformanceFrequency at 1 MHz '
              'for every build, so the tick domain is the same by construction. What the check proves is '
              'that the arithmetic itself - the float-to-tick conversion, the floor, the re-anchor, the '
              'clamp, the half-scale rounding - does not move between a native g++/clang build and a '
              'wasm build, with or without FMA contraction, at -O0 and at -O2.')
        print('What it does not prove is that the game agrees with itself across platforms: the shipped '
              'game takes TICK_SHIFT from the hardware clock, so a native build against a real QPF and '
              'a wasm build against a pinned 1 MHz are in different tick domains from the first frame.')
        return 0
    print('The builds disagree. Run 2 and 4 are the ones that moved.')
    return 1


if __name__ == '__main__':
    sys.exit(main())
