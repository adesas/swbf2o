#!/usr/bin/env python3
"""Survey every .lvl in GameData with the ucfb reader: how many parse, and which chunk ids exist.
This validates the format understanding against the whole dataset, not one file."""
import sys, os, collections
sys.path.insert(0, os.path.dirname(__file__))
from lvl_reader import parse_file

root = sys.argv[1]
files = [os.path.join(d,f) for d,_,fs in os.walk(root) for f in fs if f.endswith('.lvl')]
ok = bad = 0
ids = collections.Counter(); idbytes = collections.Counter()
examples = {}
for p in files:
    data = open(p,'rb').read()
    if data[:4] != b'ucfb': bad += 1; continue
    total = int.from_bytes(data[4:8],'little')
    r, m = parse_file(p)
    data = open(p,"rb").read()
    if not r: bad += 1; continue
    ok += 1
    for cid, size, _, kids in r:
        ids[cid.decode()] += 1; idbytes[cid.decode()] += size
        examples.setdefault(cid.decode(), p)
print(f"parsed {ok} / {len(files)} .lvl files (ucfb containers)")
print("\nchunk ids by total bytes:")
for cid, b in idbytes.most_common(25):
    print(f"  {cid:6s} {ids[cid]:6d} chunks  {b:12d} bytes   e.g. {examples[cid]}")
