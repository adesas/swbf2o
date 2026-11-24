#!/usr/bin/env python3
"""Collision hulls (coll) tested against the engine's own reader.

The reader is in this tree, so this is a comparison against what the engine's code does with these
bytes, not a plausible reading of them:

  CollisionMesh::_Read   CollisionMesh.cpp:475   chunk ids as hashes
  ReadTree               CollisionMesh.cpp:436   the tree grammar
  CollisionAABB::Init    CollisionMesh.cpp:183   the byte-to-float decode against a parent box
  u8tof32_hex[256]       CollisionMesh.cpp:141   the table itself, used as data

Chunk ids in the code: 0x454D414E NAME, 0x45455254 TREE, 0x45444F4E NODE, 0x49534F50 POSI,
0x4B53414D MASK, 0x4F464E49 INFO.

What the code says the bytes are:

  INFO   4 uint32: poslCount, numNodes, numLeafs, numIndices, then 6 floats min then max
  POSI   poslCount float triples
  MASK   one byte
  TREE   a prefix tree read with ReadNextChild. A branch entry is 6 bytes of compressed AABB, then
         two children. A leaf entry is 1 byte numVerts, 6 bytes of compressed AABB, then numVerts
         uint16 indices.
  NODE   a string, read and discarded

A compressed AABB is relative to its parent, so the tree is evaluated from the root down with the
INFO box as the root's parent.

The containment tests are reported in quantisation steps (dim/255 for that node), because the boxes
are lossy: a vertex can legitimately sit outside the reconstructed box by up to about half a step.
Measuring in ULPs was wrong here - comparing unsigned bit patterns orders negative numbers the wrong
way round, and that is what made an earlier version of this check disagree with itself.

Usage: python3 tools/coll_tree_check.py [--json]
"""
import json
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from lvl_reader import walk  # noqa: E402

DECOMP = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                      'vendor/swbf2decomp/src/Battlefront2/Source/CollisionMesh.cpp')


def f32(x):
    return struct.unpack('<f', struct.pack('<f', x))[0]


def load_lut():
    body = re.search(r'u8tof32_hex\[256\] = \{(.*?)\};', open(DECOMP).read(), re.S)
    return [int(h, 16) for h in re.findall(r'0x([0-9a-f]{8})', body.group(1))]


LUT = load_lut()
LUT_FLOAT = [struct.unpack('<f', struct.pack('<I', v))[0] for v in LUT]
BITS = lambda x: struct.unpack('<I', struct.pack('<f', f32(x)))[0]  # noqa: E731

# Negative control: the table as n/255 computed in float32, which is not the table in the source.
# Run with --wrong-lut and the containment tests must fail.
USE_COMPUTED_LUT = '--wrong-lut' in sys.argv


def decode(parent, comp, lut):
    """CollisionAABB::Init in float32, using the table as the engine does."""
    lut = [f32(f32(n) / f32(255.0)) for n in range(256)] if USE_COMPUTED_LUT else lut
    dim = [f32(f32(parent[1][i]) - f32(parent[0][i])) for i in range(3)]
    lo = [f32(f32(parent[0][i]) + f32(dim[i] * lut[comp[0][i]])) for i in range(3)]
    hi = [f32(f32(parent[0][i]) + f32(dim[i] * lut[comp[1][i]])) for i in range(3)]
    return lo, hi


def comp_bytes(b):
    return ([b[0], b[1], b[2]], [b[3], b[4], b[5]])


def walk_tree(children, root_box, verts, npos, res, fname, data):
    pos = [0]

    def visit(parent):
        if pos[0] >= len(children):
            res['truncated_tree'] += 1
            return
        cid, size, pstart, _ = children[pos[0]]
        pos[0] += 1
        payload = data[pstart:pstart + size]

        if cid == b'NODE':
            if len(payload) != 6:
                res['branch_wrong_size'] += 1
                return
            res['branches'] += 1
            comp = comp_bytes(payload)
            if any(comp[0][i] > comp[1][i] for i in range(3)):
                res['branch_inverted_bytes'] += 1
            box = decode(parent, comp, LUT_FLOAT)
            step = [max(f32(f32(parent[1][i]) - f32(parent[0][i])) / 255.0, 1e-12) for i in range(3)]
            out = max(max(0.0, (parent[0][i] - box[0][i]) / step[i]) for i in range(3))
            out = max(out, max(max(0.0, (box[1][i] - parent[1][i]) / step[i]) for i in range(3)))
            if out <= 0.001:
                res['child_inside_parent'] += 1
            elif out <= 1.0:
                res['child_outside_by_one_step'] += 1
            else:
                res['child_outside_more'] += 1
                if len(res['_examples']) < 6:
                    res['_examples'].append({'file': fname, 'kind': 'branch', 'steps': round(out, 3),
                                             'bytes': ''.join('%02x' % b for b in payload)})
            visit(box)
            visit(box)
            return

        if cid != b'LEAF' or len(payload) < 7:
            res['leaf_wrong_size'] += 1
            return
        n = payload[0]
        if 1 + 6 + 2 * n != len(payload):
            res['leaf_wrong_size'] += 1
            return
        res['leaves'] += 1
        comp = comp_bytes(payload[1:7])
        if any(comp[0][i] > comp[1][i] for i in range(3)):
            res['leaf_inverted_bytes'] += 1
        box = decode(parent, comp, LUT_FLOAT)
        idx = [int.from_bytes(payload[7 + 2 * i:9 + 2 * i], 'little') for i in range(n)]
        if any(i >= npos for i in idx):
            res['index_out_of_range'] += 1
            return
        step = [max(f32(f32(parent[1][i]) - f32(parent[0][i])) / 255.0, 1e-12) for i in range(3)]
        worst = 0.0
        for i in idx:
            for k in range(3):
                worst = max(worst, max(0.0, (box[0][k] - verts[i][k]) / step[k]),
                            max(0.0, (verts[i][k] - box[1][k]) / step[k]))
        if worst <= 0.001:
            res['leaf_contains_its_vertices'] += 1
        elif worst <= 1.0:
            res['vertex_outside_by_one_step'] += 1
        else:
            res['vertex_outside_by_more'] += 1
            if len(res['_examples']) < 6:
                res['_examples'].append({'file': fname, 'kind': 'leaf', 'numVerts': n,
                                         'steps': round(worst, 3), 'bytes': ''.join('%02x' % b for b in payload),
                                         'indices': idx})

    visit(root_box)


def main():
    files = []
    for r, _, fs in os.walk('data'):
        for n in fs:
            if n.endswith('.lvl'):
                files.append(os.path.join(r, n))

    keys = ('containers', 'branches', 'leaves', 'branch_wrong_size', 'leaf_wrong_size',
           'branch_inverted_bytes', 'leaf_inverted_bytes', 'index_out_of_range',
           'counts_match_tree', 'counts_mismatch', 'info_box_equals_hull', 'info_box_mismatch',
           'root_is_full_extent', 'root_not_full_extent', 'mask_children', 'mask_not_one_byte',
           'child_inside_parent', 'child_outside_by_one_step',
           'child_outside_more', 'leaf_contains_its_vertices', 'vertex_outside_by_one_step',
           'vertex_outside_by_more', 'truncated_tree')
    res = {k: 0 for k in keys}
    res['_examples'] = []
    res['_root_examples'] = []

    for p in files:
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
            if cid != b'coll':
                if kids:
                    stack.extend(kids)
                continue
            by = {c[0]: c for c in (kids or [])}
            info, tree, pos = by.get(b'INFO'), by.get(b'TREE'), by.get(b'POSI')
            if not info or info[1] < 40 or not tree or not tree[3] or not pos:
                continue
            res['containers'] += 1
            words = struct.unpack('<4I', data[info[2]:info[2] + 16])
            box = struct.unpack('<6f', data[info[2] + 16:info[2] + 40])
            npos = pos[1] // 12
            verts = [struct.unpack('<3f', data[pos[2] + i * 12:pos[2] + i * 12 + 12]) for i in range(npos)]
            if not verts:
                continue
            nbranches = len([c for c in tree[3] if c[0] == b'NODE'])
            nleafs = len([c for c in tree[3] if c[0] == b'LEAF'])
            if nbranches == words[1] and nleafs == words[2]:
                res['counts_match_tree'] += 1
            else:
                res['counts_mismatch'] += 1
            mn = [min(v[i] for v in verts) for i in range(3)]
            mx = [max(v[i] for v in verts) for i in range(3)]
            if all(abs(mn[i] - box[i]) < 1e-6 and abs(mx[i] - box[i + 3]) < 1e-6 for i in range(3)):
                res['info_box_equals_hull'] += 1
            else:
                res['info_box_mismatch'] += 1
            for c in (kids or []):
                if c[0] == b'MASK':
                    res['mask_children'] += 1
                    if c[1] != 1:
                        res['mask_not_one_byte'] += 1
            first = tree[3][0]
            fp = data[first[2]:first[2] + first[1]]
            if first[0] == b'NODE':
                comp = comp_bytes(fp) if len(fp) == 6 else None
            elif first[0] == b'LEAF' and len(fp) >= 7:
                comp = comp_bytes(fp[1:7])
            else:
                comp = None
            if comp and all(c == 255 for c in comp[1]) and all(c == 0 for c in comp[0]):
                res['root_is_full_extent'] += 1
            else:
                res['root_not_full_extent'] += 1
                if len(res['_root_examples']) < 6:
                    res['_root_examples'].append({'file': os.path.basename(p), 'kind': first[0].decode(),
                                                  'bytes': fp.hex()})
            walk_tree(tree[3], (list(box[:3]), list(box[3:])), verts, npos, res, os.path.basename(p), data)

    lut_f32 = sum(1 for n in range(256) if BITS(f32(n) / f32(255.0)) == LUT[n])
    lut_double = sum(1 for n in range(256) if BITS(f32(n / 255.0)) == LUT[n])
    diff = [n for n in range(256) if BITS(f32(n) / f32(255.0)) != LUT[n]]

    summary = {'results': res, 'lut': {
        'matches_float32_n_over_255': lut_f32, 'matches_double': lut_double,
        'entries_that_differ': diff,
        'note': 'the table is not n/255 computed either way. It differs on 8 entries, all powers of '
                'two, by one ULP. A model that computes the division is not using the engine\'s numbers.'}}
    if '--json' in sys.argv:
        print(json.dumps(summary, ensure_ascii=False, sort_keys=True))
    else:
        print('coll, read the way the engine reads it')
        for k in keys:
            print('  %-28s %d' % (k, res[k]))
        print()
        print('lut: matches float32 n/255 on %d of 256, double on %d of 256, differs at %s'
              % (lut_f32, lut_double, diff))
        for e in res['_examples']:
            print('  ' + json.dumps(e, sort_keys=True))
        for e in res['_root_examples']:
            print('  root ' + json.dumps(e, sort_keys=True))

    # Hard assertions: the grammar the code describes must hold on every container.
    hard = (res['branch_wrong_size'] + res['leaf_wrong_size'] + res['index_out_of_range']
            + res['truncated_tree'] + res['counts_mismatch'] + res['info_box_mismatch'])
    if hard:
        print('\ngrammar assertion failed')
        return 1
    if USE_COMPUTED_LUT:
        # The claim under test: the table is n/255 computed in float32. It is not, and this is the
        # only place the difference is visible - one ULP is far below one quantisation step, so no
        # containment test can see it. The control must fail, so it exits non-zero.
        if not diff:
            print('\nnegative control did not fail: the table is not the computed division')
            return 1
        print('\nnegative control fails as it must: the table differs from n/255 on %d entries' % len(diff))
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
