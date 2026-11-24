#!/usr/bin/env python3
"""Collision data (coll) - what the bytes actually allow, tested across every container.

There is no matching loader code for `coll` in this tree: nothing in vendor/swbf2decomp calls a chunk
child by name, so the shape below is read from the data and tested against the data. It is not a
format fact confirmed by code, and it is written that way.

What is tested, as hypotheses that can fail:

  H1  NAME and NODE are strings (printable bytes, NUL terminated)
  H2  INFO is 4 uint32 then 6 float32
  H3  INFO word 0 is the number of POSI entries, and POSI is float triples -> size == word0 * 12
  H4  the six floats in INFO are a box: min then max, componentwise min <= max
  H5  every POSI coordinate lies inside that box
  H6  INFO word 1 / word 2 are the number of NODE / LEAF children in TREE
  H7  each LEAF begins with a count, and the counts of all leaves sum to word 0
  H8  the values inside a LEAF are indices into POSI, so they are all < word 0

H4 is tested both ways round on purpose. If the box were max-then-min, H4 would pass and the
single test would not tell which order it is.

Usage: python3 tools/coll_probe.py [--json]
"""
import collections
import json
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from lvl_reader import walk  # noqa: E402


def is_string(payload):
    return len(payload) > 1 and payload[-1] == 0 and all(32 <= c < 127 for c in payload[:-1])


def probe_file(path):
    data = open(path, 'rb').read()
    if data[:4] != b'ucfb':
        return None
    total = int.from_bytes(data[4:8], 'little')
    root = walk(data, 8, 8 + total)
    if root is None:
        return None

    out = []
    stack = list(root)
    while stack:
        node = stack.pop()
        cid, size, pstart, kids = node
        if cid != b'coll':
            if kids:
                stack.extend(kids)
            continue

        by_name = {c[0]: c for c in (kids or [])}
        rec = {'file': os.path.basename(path)}
        out.append(rec)

        for name in (b'NAME', b'NODE', b'INFO', b'POSI', b'TREE', b'MASK'):
            rec['has_' + name.decode()] = name in by_name

        info = by_name.get(b'INFO')
        if info is None or info[1] < 40:
            rec['H2'] = False
            continue

        payload = data[info[2]:info[2] + info[1]]
        words = struct.unpack('<4I', payload[:16])
        floats = struct.unpack('<6f', payload[16:40])
        rec['H2'] = info[1] == 40
        rec['word0'] = words[0]
        rec['word1'] = words[1]
        rec['word2'] = words[2]
        rec['word3'] = words[3]

        pos = by_name.get(b'POSI')
        npos = pos[1] // 12 if pos else 0
        rec['H3'] = pos is not None and pos[1] == words[0] * 12 and pos[1] % 12 == 0
        if pos is None or pos[1] % 12:
            continue

        lo, hi = floats[:3], floats[3:6]
        rec['H4_min_then_max'] = all(lo[i] <= hi[i] for i in range(3))
        rec['H4_max_then_min'] = all(floats[i + 3] <= floats[i] for i in range(3))

        verts = [struct.unpack('<3f', data[pos[2] + i * 12:pos[2] + i * 12 + 12])
                 for i in range(npos)]
        rec['H5'] = all(lo[i] <= v[i] <= hi[i] for v in verts for i in range(3))

        tree = by_name.get(b'TREE')
        rec['H6'] = rec['H7'] = rec['H8'] = None
        if tree and tree[3]:
            knodes = [c for c in tree[3] if c[0] == b'NODE']
            kleafs = [c for c in tree[3] if c[0] == b'LEAF']
            rec['H6'] = len(knodes) == words[1] and len(kleafs) == words[2]
            counts = [int.from_bytes(data[c[2]:c[2] + 4], 'little') for c in kleafs]
            rec['H7'] = sum(counts) == words[0]
            # the bytes after the count in a leaf: are they index-like?
            idx_ok = True
            for c in kleafs:
                body = data[c[2] + 4:c[2] + c[1]]
                vals = [int.from_bytes(body[i:i + 3], 'little') for i in range(0, len(body) - 2, 3)]
                if any(v >= words[0] for v in vals):
                    idx_ok = False
            rec['H8'] = idx_ok

        for name in (b'NAME', b'NODE'):
            c = by_name.get(name)
            rec['H1_' + name.decode()] = c is not None and is_string(data[c[2]:c[2] + c[1]])

    return out


def main():
    files = []
    for r, _, fs in os.walk('data'):
        for n in fs:
            if n.endswith('.lvl'):
                files.append(os.path.join(r, n))

    records = []
    for p in files:
        r = probe_file(p)
        if r:
            records.extend(r)

    stats = collections.Counter()
    for rec in records:
        for k in ('H1_NAME', 'H1_NODE', 'H2', 'H3', 'H4_min_then_max', 'H4_max_then_min',
                  'H5', 'H6', 'H7', 'H8'):
            v = rec.get(k)
            if v is None:
                stats[k + '_not_tested'] += 1
            elif v:
                stats[k + '_pass'] += 1
            else:
                stats[k + '_fail'] += 1

    # how many entries does a leaf actually hold, and what does a leaf look like
    leaf_shapes = collections.Counter()
    for rec in records:
        if rec.get('word0'):
            leaf_shapes[rec['word3']] += 1

    summary = {
        'containers': len(records),
        'hypotheses': dict(stats),
        'word3_value_counts_top10': leaf_shapes.most_common(10),
        'note': ('read from the data and tested against the data. No matching reader for coll exists '
                 'in this tree, so these are data-level invariants, not format facts confirmed by code.'),
    }
    if '--json' in sys.argv:
        print(json.dumps(summary, ensure_ascii=False, sort_keys=True))
        return 0

    print('coll: %d containers' % len(records))
    for k in ('H1_NAME', 'H1_NODE', 'H2', 'H3', 'H4_min_then_max', 'H4_max_then_min',
             'H5', 'H6', 'H7', 'H8'):
        passn = stats[k + '_pass']
        failn = stats[k + '_fail']
        skipped = stats[k + '_not_tested']
        print('  %-18s pass %-5d fail %-5d not tested %-5d' % (k, passn, failn, skipped))
    print('  word 3 values (top 10):', leaf_shapes.most_common(10))
    print('  ' + summary['note'])
    return 0


if __name__ == '__main__':
    sys.exit(main())
