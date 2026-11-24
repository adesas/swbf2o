#!/usr/bin/env python3
"""ucfb (.lvl / .package) chunk-container reader for SWBF / SWBF2 game data.

Matches the engine's own reader semantics from the decompiled source
(`PebbleFL/Common/PblChunk.h`, `Zephyr/Source/CheckChunkId.cpp`):
  - 4-byte ASCII chunk ids, uint32 size
  - child boundaries aligned to 4 bytes (`HasMoreChildren`: next + ((-next)&3) < end)

A payload is a container only if its children exactly consume it (with 4-byte padding);
otherwise it is a leaf (raw data). That is what makes the walk reliable.

Usage:
  python3 tools/lvl_reader.py <file.lvl> [--ids] [--tree] [--maxdepth N]
"""
import sys

def is_id(b):
    return len(b) == 4 and all(32 <= c < 127 for c in b)

def walk(data, start, end):
    """Return entries [(id, size, payload_start, children|None)] consuming [start,end), else None."""
    entries, pos = [], start
    while pos + 8 <= end:
        cid = data[pos:pos+4]
        if not is_id(cid):
            return None
        size = int.from_bytes(data[pos+4:pos+8], 'little')
        pstart = pos + 8
        if pstart + size > end:
            return None
        kids = walk(data, pstart, pstart + size)
        entries.append((cid, size, pstart, kids))
        pos = pstart + size
        pos += (-pos) & 3
        if pos > end:
            return None
    return entries if pos == end else None

def print_tree(node, depth=0, maxdepth=6):
    cid, size, _, kids = node
    print(f"{'  ' * depth}[{cid}] size={size}{' (container)' if kids else ' (data)'}")
    if kids and depth < maxdepth:
        for k in kids:
            print_tree(k, depth + 1, maxdepth)

def collect(node, stats, depth=0):
    cid, size, _, kids = node
    key = cid.decode() if depth == 1 else None
    if key:
        s = stats.setdefault(key, [0, 0]); s[0] += 1; s[1] += size
    if kids:
        for k in kids:
            collect(k, stats, depth + 1)

def parse_file(path):
    data = open(path, 'rb').read()
    if data[:4] != b'ucfb':
        return None, data[:4]
    total = int.from_bytes(data[4:8], 'little')
    root = walk(data, 8, 8 + total)
    return root, data[:4]

def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    if not args:
        print(__doc__); return 2
    path = args[0]
    flags = [a for a in sys.argv[1:] if a.startswith('--')]
    show_ids = '--ids' in flags
    show_tree = '--tree' in flags or not show_ids
    maxdepth = int(args[1]) if len(args) > 1 else 6
    root, magic = parse_file(path)
    if root is None:
        print(f"{path}: magic={magic} — top level is not a clean chunk list"); return 1
    total = int.from_bytes(open(path,'rb').read()[4:8], 'little')
    print(f"{path}: magic=ucfb total={total} chunks={len(root)}")
    if show_tree:
        for n in root:
            print_tree(n, 0, maxdepth)
    if show_ids:
        stats = {}
        for n in root:
            collect(n, stats, 0)
        print("\n--- chunk ids ---")
        for cid, (count, nbytes) in sorted(stats.items(), key=lambda kv: -kv[1][1]):
            print(f"  {cid}: {count} chunks, {nbytes} bytes")
    return 0

if __name__ == '__main__':
    sys.exit(main())
