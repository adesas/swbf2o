#!/usr/bin/env python3
"""prim shape construction: the engine's Set functions against an independent float32 model.

Records come from prim/DATA, read the way CollisionBodyPrimitive::Read reads them
(CollisionBody.cpp:236): uint32 type, then three floats. What the code does with them:

    file type 0, 1, 3 -> SetSphere(f0)            mType = SPHERE (0)
    file type 2        -> SetCylinder(f0, f1)      mType = CYLINDER (4)
    file type 4        -> SetBox(f0, f1, f2, -1.0) mType = BOX (2)

The file type is not the enum value. PrimitiveType is SPHERE=0, BUBBLE=1, BOX=2,
AXIS_ALIGNED_CYLINDER=3, CYLINDER=4, so file type 2 becomes 4, file type 4 becomes 2, and file type
3 becomes 0 because the switch routes it to SetSphere. Type 1 never appears in the data.

Fields written, per type, taken from the bodies:
    SetSphere    mType, mRadius, mMeshCollisionRadius, mHeight = r * 2.0f
    SetCylinder  mType, mRadius, mHeight,
                 mMeshCollisionRadius = sqrtf((h/2)^2 + r^2 + r^2)     <- two r^2 terms
    SetBox       mType, mWidth, mHeight, mDepth, and because fRadius < 0:
                 mRadius             = sqrtf(w^2 + d^2 + h^2)
                 mMeshCollisionRadius = sqrtf(w^2 + h^2 + d^2)
                 two different expressions in a different order, modelled separately

CollisionPrimitive() (CollisionPrimitive.cpp:42) initialises only mName, so mWidth and mDepth on a
sphere or cylinder keep whatever the object had. The harness fills them with a sentinel before each
call, and the check asserts the sentinel survived - that is the falsifiable form of "the engine does
not write that field".

The model rounds every operation to float32 in the source's order and takes sqrt as
correctly-rounded-to-float32, which is what sqrtf does. Whether double-sqrt-then-round differs from
that is measured separately, not assumed.

Controls that must fail:
  double_accum               products and sum computed in double
  single_r2_cylinder         the second r^2 missing from SetCylinder
  no_sphere_double_height    mHeight = r instead of 2r for spheres
  type3_as_aacyl             file type 3 read as AXIS_ALIGNED_CYLINDER instead of SetSphere

Usage: python3 tools/prim_shape_check.py [--json]
"""
import json
import os
import struct
import subprocess
import sys
from decimal import Decimal, getcontext
from fractions import Fraction
from math import isqrt

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from lvl_reader import walk  # noqa: E402

getcontext().prec = 60
SENTINEL = 0xAAAAAAAA
FIELDS = ['mRadius', 'mHeight', 'mWidth', 'mDepth', 'mMeshCollisionRadius']
CONTROL_NAMES = ['double_accum', 'single_r2_cylinder', 'no_sphere_double_height', 'type3_as_aacyl']


def f32(x):
    return struct.unpack('<f', struct.pack('<f', float(x)))[0]


def bits(x):
    return struct.unpack('<I', struct.pack('<f', f32(x)))[0]


def neighbours(x):
    out = [x]
    b = bits(x)
    nb = b + 1 if x >= 0 else b - 1
    out.append(struct.unpack('<f', struct.pack('<I', nb & 0xFFFFFFFF))[0])
    pb = b - 1 if x >= 0 else b + 1
    out.append(struct.unpack('<f', struct.pack('<I', pb & 0xFFFFFFFF))[0])
    return out


def sqrtf_exact(value):
    """Correctly-rounded float32 sqrt, decided by comparing candidates against the exact value."""
    if value == 0.0:
        return 0.0
    target = Decimal(value)
    s = target.sqrt()
    best, best_dist = None, None
    for c in neighbours(f32(float(s))):
        d = abs(Decimal(c) - s)
        if best is None or d < best_dist:
            best, best_dist = c, d
        elif d == best_dist and bits(c) % 2 == 0:
            best = c
    return best


def model_record(rec, control=None):
    t, f0, f1, f2 = rec
    a0, a1, a2 = f32(f0), f32(f1), f32(f2)
    roots = []   # the float32 sums a sqrt was taken of, so the two readings can be compared

    def root(sum_value, double_path):
        roots.append(sum_value)
        if double_path or control == 'double_accum':
            return f32(float(Decimal(sum_value).sqrt()))
        return sqrtf_exact(sum_value)

    mtype, fields = None, {}
    if t in (0, 1, 3):
        if t == 3 and control == 'type3_as_aacyl':
            mtype = 3
            fields = {'mRadius': a0, 'mHeight': a1, 'mMeshCollisionRadius': f32(a1 * f32(0.5))}
        else:
            mtype = 0
            fields = {'mRadius': a0, 'mMeshCollisionRadius': a0,
                      'mHeight': a0 if control == 'no_sphere_double_height' else f32(a0 * f32(2.0))}
    elif t == 2:
        mtype = 4
        hf = f32(a1 * f32(0.5))
        r2 = f32(a0 * a0)
        if control == 'single_r2_cylinder':
            total = f32(f32(hf * hf) + r2)
        else:
            total = f32(f32(f32(hf * hf) + r2) + r2)
        if control == 'double_accum':
            total = float(Decimal(a0) * Decimal(a0) * 2 + Decimal(a1) / 2 * Decimal(a1) / 2)
        fields = {'mRadius': a0, 'mHeight': a1, 'mMeshCollisionRadius': root(total, control == 'double_accum')}
    elif t == 4:
        mtype = 2
        r_total = f32(f32(f32(a0 * a0) + f32(a2 * a2)) + f32(a1 * a1))          # source order
        m_total = f32(f32(f32(a0 * a0) + f32(a1 * a1)) + f32(a2 * a2))          # different order
        if control == 'double_accum':
            r_total = float(Decimal(a0) * Decimal(a0) + Decimal(a2) * Decimal(a2) + Decimal(a1) * Decimal(a1))
            m_total = r_total
        fields = {'mWidth': a0, 'mHeight': a1, 'mDepth': a2,
                  'mRadius': root(r_total, control == 'double_accum'),
                  'mMeshCollisionRadius': root(m_total, control == 'double_accum')}
    else:
        raise ValueError('type %d is not in the switch' % t)
    return mtype, fields, roots


def extract_records():
    recs = []
    for r, _, fs in os.walk('data'):
        for n in fs:
            if not n.endswith('.lvl'):
                continue
            p = os.path.join(r, n)
            data = open(p, 'rb').read()
            if data[:4] != b'ucfb':
                continue
            total = int.from_bytes(data[4:8], 'little')
            root = walk(data, 8, 8 + total)
            if root is None:
                continue
            stack = list(root)
            while stack:
                cid, size, pstart, kids = stack.pop()
                if cid != b'prim':
                    if kids:
                        stack.extend(kids)
                    continue
                for c in (kids or []):
                    if c[0] != b'DATA':
                        continue
                    pay = data[c[2]:c[2] + c[1]]
                    if len(pay) == 16:
                        recs.append((os.path.basename(p), struct.unpack('<I', pay[:4])[0],
                                     struct.unpack('<3f', pay[4:16])))
    return recs


def stub_inventory():
    path = 'spike/prim_shape_stubs.cpp'
    if not os.path.exists(path):
        return {}
    text = open(path).read()
    funcs = [l.split('(')[0].replace('void ', '') for l in text.splitlines() if l.startswith('void _Z')]
    data = [l.split('=')[0].replace('int ', '').strip() for l in text.splitlines() if l.startswith('int _Z')]
    return {'functions_with_no_body_in_this_tree': funcs,
            'globals_with_no_definition_in_this_tree': data,
            'note': 'each stub aborts if called; nothing in SetSphere/SetCylinder/SetBox reaches them. '
                    'PblMatrix four-row constructor is host-side because this TU has a file-scope '
                    'object built with it, which runs before main.'}


def main():
    work = 'build/prim_shape'
    binpath = os.path.join(work, 'oracle_bin')
    if not os.path.exists(binpath):
        print('build first: tools/build_prim_shape_oracle.sh')
        return 1

    recs = extract_records()
    open(os.path.join(work, 'records.bin'), 'wb').write(
        b''.join(struct.pack('<I3f', t, *v) for _, t, v in recs))
    subprocess.run([binpath, os.path.join(work, 'records.bin'), os.path.join(work, 'engine_out.bin')],
                   check=True)
    raw = open(os.path.join(work, 'engine_out.bin'), 'rb').read()
    engine = [struct.unpack_from('<iIIIIII', raw, i * 28) for i in range(len(raw) // 28)]
    if len(engine) != len(recs):
        print('the harness returned %d records for %d inputs' % (len(engine), len(recs)))
        return 1

    counts = {'records': len(recs), 'by_file_type': {},
              'type_matches_engine': 0, 'fields_compared': 0, 'field_disagreements': 0,
              'fields_left_untouched_by_the_engine_confirmed': 0,
              'fields_the_engine_wrote_anyway': 0,
              'records_where_double_rounding_would_differ': 0}
    for _, t, _ in recs:
        counts['by_file_type'][t] = counts['by_file_type'].get(t, 0) + 1

    control_fail = {c: 0 for c in CONTROL_NAMES}
    examples = []

    for (fname, t, vals), eng in zip(recs, engine):
        mtype, fields, roots = model_record((t, *vals))
        if mtype == eng[0]:
            counts['type_matches_engine'] += 1
        mask = eng[1]
        for i, name in enumerate(FIELDS):
            if not (mask >> i) & 1:
                if eng[2 + i] == SENTINEL:
                    counts['fields_left_untouched_by_the_engine_confirmed'] += 1
                else:
                    counts['fields_the_engine_wrote_anyway'] += 1
                    examples.append({'file': fname, 'type': t, 'field': name,
                                     'note': 'engine wrote a field the model does not expect'})
                continue
            counts['fields_compared'] += 1
            if bits(fields[name]) != eng[2 + i]:
                counts['field_disagreements'] += 1
                if len(examples) < 8:
                    examples.append({'file': fname, 'type': t, 'field': name,
                                     'model': hex(bits(fields[name])), 'engine': hex(eng[2 + i])})

        # measured, not assumed: correctly-rounded float32 sqrt vs double-then-round, same input
        for s in roots:
            if bits(f32(float(Decimal(s).sqrt()))) != bits(sqrtf_exact(s)):
                counts['records_where_double_rounding_would_differ'] += 1

        for c in CONTROL_NAMES:
            mt, fm, _ = model_record((t, *vals), c)
            bad = mt != eng[0]
            for i, name in enumerate(FIELDS):
                if (mask >> i) & 1 and name in fm and bits(fm[name]) != eng[2 + i]:
                    bad = True
            if bad:
                control_fail[c] += 1

    out = {'results': counts, 'controls_that_must_fail': control_fail,
           'link_stubs': stub_inventory(), 'examples': examples}
    ok = counts['field_disagreements'] == 0 and counts['type_matches_engine'] == len(recs) \
        and all(v > 0 for v in control_fail.values())
    out['passed'] = ok

    if '--json' in sys.argv:
        print(json.dumps(out, ensure_ascii=False, sort_keys=True))
    else:
        print('prim shape construction, engine code against an independent float32 model')
        for k, v in counts.items():
            print('  %-48s %s' % (k, json.dumps(v, sort_keys=True) if isinstance(v, dict) else v))
        print('\ncontrols that must fail:')
        for c, v in control_fail.items():
            print('  %-26s %6d records  %s' % (c, v, 'fails as it must' if v else 'DOES NOT FAIL'))
        inv = out['link_stubs']
        if inv:
            print('\nlink stubs (the check never calls these):')
            for n in inv['functions_with_no_body_in_this_tree']:
                print('  function  ' + n)
            for n in inv['globals_with_no_definition_in_this_tree']:
                print('  variable  ' + n)
            print('  ' + inv['note'])
        for e in examples:
            print('  ' + json.dumps(e, sort_keys=True))
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
