#!/usr/bin/env python3
"""Copy selected class records out of a side/*.lvl file into small, ordinary .lvl files.

A side file is not one opaque blob: it is a ucfb chunk sequence whose top-level chunks are
lvl_ records.  Each lvl_ payload starts with the class's name hash and the nested level's
size, followed by that class's chunks (entc, wnpc, ordc, models, animations, and so on).
LoadUtil selects one of those records by the hash of the class's name
(LoadUtil.cpp:288-300).

So the browser does not need a new reader and it does not need the other 96-143 MB of the
faction.  This tool copies the complete top-level lvl_ record, byte for byte, behind a new
ucfb header.  The engine's existing loader reads the result exactly as it reads the original.

Examples:

    python3 tools/extract_class_level.py side/cis.lvl cis_inf_rifleman
    python3 tools/extract_class_level.py side/rep.lvl rep_inf_ep3_rifleman --list
    python3 tools/extract_class_level.py side/cis.lvl --list

Paths are relative to the game's _lvl_pc directory unless absolute.  Output defaults to
build/class_data/<side directory>/<side stem>/<class>.lvl.  Nothing in the game data dump
is modified.
"""

import argparse
import hashlib
import json
import os
import pathlib
import struct
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
GAME = ROOT / "data/Star wars battlefront 2/GameData/data/_lvl_pc"
OUT = ROOT / "build/class_data"
LVL = struct.unpack("<I", b"lvl_")[0]


def pblhash(name):
    """The engine's class-name hash: FNV-1a with ASCII case folding (PblHashTable)."""
    value = 0x811C9DC5
    for byte in name.encode("ascii"):
        value ^= byte | 0x20
        value = (value * 0x01000193) & 0xFFFFFFFF
    return value


def records(path):
    """Yield (offset, chunk size, class hash, nested size) for each top-level lvl_ record."""
    size = path.stat().st_size
    with path.open("rb") as fh:
        header = fh.read(8)
        if len(header) != 8 or header[:4] != b"ucfb":
            raise ValueError(f"{path}: not a ucfb container")
        declared = struct.unpack("<I", header[4:])[0]
        if declared + 8 != size:
            raise ValueError(f"{path}: header says {declared + 8} bytes, file has {size}")
        pos = 8
        while pos + 8 <= size:
            fh.seek(pos)
            head = fh.read(8)
            cid, chunk_size = struct.unpack("<II", head)
            next_pos = (pos + 8 + chunk_size + 3) & ~3
            if next_pos > size:
                raise ValueError(f"{path}: chunk at {pos} runs past the end of the file")
            if cid == LVL:
                payload = fh.read(8)
                if len(payload) != 8:
                    raise ValueError(f"{path}: truncated lvl_ record at {pos}")
                class_hash, nested_size = struct.unpack("<II", payload)
                # Four bytes of hash, four of nested size, then the nested chunk sequence.
                if nested_size + 8 != chunk_size:
                    raise ValueError(
                        f"{path}: lvl_ at {pos} has outer size {chunk_size}, "
                        f"nested size {nested_size} (expected outer = nested + 8)"
                    )
                yield pos, chunk_size, class_hash, nested_size
            pos = next_pos
        if pos != size:
            raise ValueError(f"{path}: chunk sequence ends at {pos}, file ends at {size}")


def write_record(source, record, class_name, output):
    offset, chunk_size, class_hash, nested_size = record
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = output.with_suffix(output.suffix + ".tmp")
    # The complete top-level record includes its id and size.  The only new bytes are the
    # eight-byte ucfb header; everything the class reader sees is copied unchanged.
    with source.open("rb") as src, temporary.open("wb") as dst:
        dst.write(b"ucfb")
        dst.write(struct.pack("<I", chunk_size + 8))
        src.seek(offset)
        left = chunk_size + 8
        digest = hashlib.sha256()
        while left:
            block = src.read(min(1 << 20, left))
            if not block:
                raise ValueError(f"{source}: short read while copying {class_name}")
            dst.write(block)
            digest.update(block)
            left -= len(block)
    temporary.replace(output)
    return {
        "class": class_name,
        "class_file": f"{source.parent.name}/{source.name};{class_name}",
        "path": output.relative_to(ROOT).as_posix(),
        "size": output.stat().st_size,
        "source_size": source.stat().st_size,
        "class_hash": f"0x{class_hash:08x}",
        "nested_size": nested_size,
        "record_sha256": digest.hexdigest(),
    }


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("source", help="side file, relative to _lvl_pc or absolute")
    ap.add_argument("classes", nargs="*", help="class names to extract")
    ap.add_argument("--list", action="store_true", help="list the records and exit")
    ap.add_argument("--out", type=pathlib.Path, default=OUT,
                    help="output root (default build/class_data)")
    ap.add_argument("--manifest", type=pathlib.Path, default=None,
                    help="write a JSON manifest of the extracted records")
    args = ap.parse_args()

    source = pathlib.Path(args.source)
    if not source.is_absolute():
        source = GAME / source
    source = source.resolve()
    if not source.is_file():
        ap.error(f"no such side file: {source}")

    found = list(records(source))
    by_hash = {}
    for record in found:
        by_hash.setdefault(record[2], []).append(record)

    if args.list or not args.classes:
        print(f"{source.relative_to(GAME) if source.is_relative_to(GAME) else source}: "
              f"{len(found)} class records, {source.stat().st_size} bytes")
        for offset, chunk_size, class_hash, nested_size in found:
            print(f"  0x{class_hash:08x}  {chunk_size + 16:>9} bytes  "
                  f"at {offset}  (nested {nested_size})")
        if not args.classes:
            return 0

    output_root = args.out if args.out.is_absolute() else ROOT / args.out
    written = []
    for name in args.classes:
        class_hash = pblhash(name)
        matches = by_hash.get(class_hash, [])
        if not matches:
            print(f"error: {name} (hash 0x{class_hash:08x}) is not in {source}", file=sys.stderr)
            return 1
        if len(matches) != 1:
            print(f"error: {name} has {len(matches)} records in {source}; refusing to guess",
                  file=sys.stderr)
            return 1
        output = output_root / source.parent.name / source.stem / f"{name}.lvl"
        info = write_record(source, matches[0], name, output)
        written.append(info)
        saved = info["source_size"] - info["size"]
        print(f"{name}: {info['size']} bytes -> {info['path']} "
              f"({saved} bytes not fetched, {info['size'] / info['source_size']:.2%} of source)")

    if args.manifest:
        manifest = args.manifest if args.manifest.is_absolute() else ROOT / args.manifest
        manifest.parent.mkdir(parents=True, exist_ok=True)
        manifest.write_text(json.dumps(written, indent=1) + "\n")
        print(f"{manifest.relative_to(ROOT)}: {len(written)} records")
    return 0


if __name__ == "__main__":
    sys.exit(main())
