#!/usr/bin/env python3
"""Extract one PCM16 sound sample, with its SoundFL metadata, into a small bank.

The PC sound containers use the same ucfb chunk grammar as the class files. A sound bank
holds a metadata chunk (0x0FB40705) and a PCM data chunk (0xD872E2A5). The metadata has a
small bank header followed by one record per sample. This tool finds a sample by its SoundFL
name hash, copies its metadata record and its exact PCM bytes, and writes a new ucfb bank.

No audio is decoded, resampled or synthesised. The sample bytes are copied unchanged. The
only new values are the bank's sample count and PCM size, which describe the smaller bank.

    python3 tools/extract_sound_sample.py sound/tat.lvl imp_weap_inf_rifle_fire
    python3 tools/extract_sound_sample.py sound/common.bnk force_push_fire

Paths are relative to _lvl_pc unless absolute. Output defaults to
build/class_data/<container parent>/<container stem>/<sample name>.lvl.
"""

import argparse
import hashlib
import json
import mmap
import os
import pathlib
import struct
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
GAME = ROOT / "data/Star wars battlefront 2/GameData/data/_lvl_pc"
OUT = ROOT / "build/class_data"

UCFB = struct.unpack("<I", b"ucfb")[0]
LVL = struct.unpack("<I", b"lvl_")[0]
BANK_OUTER = 0x5E262256
BANK_INNER = 0x9A644D15
# The raw banks embedded in sound/*.lvl have two more wrappers around the same metadata and
# data chunks (measured at the start of sound/tat.lvl).
RAW_BANK_OUTER = 0x5BB97F21
RAW_BANK_INNER = 0x5F6F6D65
BANK_METADATA = 0x0FB40705
BANK_DATA = 0xD872E2A5

BANK_HASH = 0x8D39BDE6
BANK_TEMPLATE = 0x694AAA0B
BANK_SIZE = 0x23A0D95C
BANK_NUM_SAMPLES = 0x98B889CE
BANK_FORMAT = 0xB99D8552
BANK_LOAD_SAMPLES = 0x8EBF7143

SAMPLE_BEGIN = 0x96E382A7
SAMPLE_HASH = 0x37386AE0
SAMPLE_FREQUENCY = 0x2FB31C01
SAMPLE_SIZE = 0x23A0D95C
SAMPLE_LENGTH = 0x1D48FEEF
SAMPLE_ALIAS = 0x7D268157
SAMPLE_END = 0x2E789FB4


def pblhash(name):
    value = 0x811C9DC5
    for byte in name.encode("ascii"):
        value ^= byte | 0x20
        value = (value * 0x01000193) & 0xFFFFFFFF
    return value


def children(data, pos, end):
    while pos + 8 <= end:
        cid, size = struct.unpack_from("<II", data, pos)
        payload = pos + 8
        if payload + size > end:
            raise ValueError(f"chunk at {pos} runs past its parent")
        yield cid, size, payload
        pos = (payload + size + 3) & ~3


def walk_banks(data, pos, end, depth=0):
    """Yield (metadata payload, data payload) for each bank in a ucfb/lvl_ tree."""
    if depth > 8:
        return
    for cid, size, payload in children(data, pos, end):
        if cid == BANK_METADATA:
            # The data chunk is a sibling of the metadata chunk, so find it in this parent.
            data_payload = None
            for other, other_size, other_payload in children(data, pos, end):
                if other == BANK_DATA:
                    data_payload = other_payload
                    break
            if data_payload is not None:
                yield payload, data_payload
        elif cid == LVL:
            # A lvl_ record starts with the class/record hash and the nested size.
            if size < 8:
                raise ValueError("short lvl_ record")
            _, nested_size = struct.unpack_from("<II", data, payload)
            if nested_size + 8 != size:
                raise ValueError("lvl_ nested size does not match its outer size")
            yield from walk_banks(data, payload + 8, payload + 8 + nested_size, depth + 1)
        elif cid in (BANK_OUTER, BANK_INNER, RAW_BANK_OUTER, RAW_BANK_INNER):
            yield from walk_banks(data, payload, payload + size, depth + 1)


def parse_bank(data, metadata_offset):
    """Return (header bytes, sample records, PCM size) for one metadata chunk."""
    pos = metadata_offset
    metadata_end = metadata_offset + struct.unpack_from("<I", data, metadata_offset - 4)[0]
    header = {}
    while pos + 4 <= metadata_end:
        field, value = struct.unpack_from("<II", data, pos)
        header[field] = value
        if field == BANK_LOAD_SAMPLES or value == SAMPLE_BEGIN:
            # A marker is four bytes: the next word is the first sample record. Raw stream
            # banks use a different marker id, but SAMPLE_BEGIN identifies it just as well.
            pos += 4
            break
        pos += 8
    count = header.get(BANK_NUM_SAMPLES, 0)
    if count > 1000000:
        raise ValueError("bad sample count")

    records = []
    pcm_size = 0
    while (count and len(records) < count) or (not count and pos + 4 <= metadata_end and
                                               struct.unpack_from("<I", data, pos)[0] == SAMPLE_BEGIN):
        start = pos
        begin = struct.unpack_from("<I", data, pos)[0]
        if begin != SAMPLE_BEGIN:
            raise ValueError("sample record does not start with SAMPLE_BEGIN")
        pos += 4
        record = {"hash": 0, "frequency": 0, "size": 0, "length": 0, "alias": 0,
                  "data_offset": 0, "start": start, "end": 0}
        while True:
            field = struct.unpack_from("<I", data, pos)[0]
            if field == SAMPLE_END:
                pos += 4
                break
            value = struct.unpack_from("<I", data, pos + 4)[0]
            pos += 8
            if field == SAMPLE_HASH:
                record["hash"] = value
            elif field == SAMPLE_FREQUENCY:
                record["frequency"] = value
            elif field == SAMPLE_SIZE:
                record["size"] = value
            elif field == SAMPLE_LENGTH:
                record["length"] = value
            elif field == SAMPLE_ALIAS:
                record["alias"] = value
        record["end"] = pos
        if record["alias"]:
            base = next((r for r in records if r["hash"] == record["alias"]), None)
            if base is None:
                raise ValueError("sample alias has no earlier base record")
            record["data_offset"] = base["data_offset"]
            record["size"] = base["size"]
            record["length"] = base["length"]
            record["frequency"] = base["frequency"]
        else:
            record["data_offset"] = pcm_size
            pcm_size += record["size"]
        records.append(record)

    if count and header.get(BANK_SIZE) != pcm_size:
        raise ValueError(f"bank size {header.get(BANK_SIZE)} != sum of samples {pcm_size}")
    return header, records, pcm_size


def extract(source, sample_name, output, sample_hash=None):
    wanted = sample_hash if sample_hash is not None else pblhash(sample_name)
    with source.open("rb") as fh:
        data = mmap.mmap(fh.fileno(), 0, access=mmap.ACCESS_READ)
        found = None
        for metadata, pcm in walk_banks(data, 8, len(data)):
            try:
                header, records, pcm_size = parse_bank(data, metadata)
            except (ValueError, struct.error):
                continue
            for index, record in enumerate(records):
                if record["hash"] == wanted:
                    found = (metadata, pcm, header, records, index, pcm_size)
                    break
            if found:
                break
        if not found:
            raise ValueError(f"sample {sample_name} (hash 0x{wanted:08x}) not found in {source}")

        metadata, pcm, header, records, index, pcm_size = found
        target = records[index]
        # An alias shares the base sample's bytes. Include its base record first, so the small
        # bank is self-contained and the alias resolves exactly as it did in the original.
        needed = []
        if target["alias"]:
            base = next((r for r in records[:index] if r["hash"] == target["alias"]), None)
            if base is None:
                raise ValueError("target alias has no base record")
            needed.append(base)
        needed.append(target)

        header_bytes = bytearray(data[metadata:records[0]["start"]])
        for pos in range(0, len(header_bytes), 8):
            field = struct.unpack_from("<I", header_bytes, pos)[0]
            if field == BANK_NUM_SAMPLES:
                struct.pack_into("<I", header_bytes, pos + 4, len(needed))
            elif field == BANK_SIZE:
                struct.pack_into("<I", header_bytes, pos + 4, target["size"])

        record_bytes = b"".join(data[r["start"]:r["end"]] for r in needed)
        pcm_bytes = data[pcm + target["data_offset"]:pcm + target["data_offset"] + target["size"]]
        metadata_chunk = struct.pack("<II", BANK_METADATA, len(header_bytes) + len(record_bytes))
        metadata_chunk += header_bytes + record_bytes
        data_chunk = struct.pack("<II", BANK_DATA, len(pcm_bytes)) + pcm_bytes
        outer_payload = metadata_chunk + data_chunk
        outer = struct.pack("<II", BANK_OUTER, len(outer_payload)) + outer_payload
        result = b"ucfb" + struct.pack("<I", len(outer)) + outer

        output.parent.mkdir(parents=True, exist_ok=True)
        temporary = output.with_suffix(output.suffix + ".tmp")
        temporary.write_bytes(result)
        temporary.replace(output)
        return {
            "sample": sample_name,
            "hash": f"0x{wanted:08x}",
            "path": output.relative_to(ROOT).as_posix(),
            "size": len(result),
            "source_size": source.stat().st_size,
            "frequency": target["frequency"],
            "frames": target["length"],
            "format": "PCM16",
            "pcm_sha256": hashlib.sha256(pcm_bytes).hexdigest(),
            "pcm_bytes": len(pcm_bytes),
            "aliased": bool(target["alias"]),
        }


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("source")
    ap.add_argument("sample", nargs="?", default=None)
    ap.add_argument("--hash", dest="sample_hash", default=None,
                    help="extract by the sample's SoundFL hash instead of its property name")
    ap.add_argument("--out", type=pathlib.Path, default=None)
    ap.add_argument("--json", action="store_true")
    args = ap.parse_args()
    if not args.sample and not args.sample_hash:
        ap.error("give a sample name or --hash")
    source = pathlib.Path(args.source)
    if not source.is_absolute():
        source = GAME / source
    source = source.resolve()
    if not source.is_file():
        ap.error(f"no such sound container: {source}")
    sample_name = args.sample or f"sample_{int(args.sample_hash, 0):08x}"
    output = args.out or OUT / source.parent.name / source.stem / f"{sample_name}.lvl"
    if not output.is_absolute():
        output = ROOT / output
    try:
        info = extract(source, sample_name, output,
                         int(args.sample_hash, 0) if args.sample_hash else None)
    except (ValueError, struct.error) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    if args.json:
        print(json.dumps(info, indent=1))
    else:
        print(f"{info['sample']}: {info['format']} {info['frequency']} Hz, "
              f"{info['frames']} frames, {info['pcm_bytes']} PCM bytes; "
              f"{info['size']} bytes -> {info['path']} "
              f"({info['size'] / info['source_size']:.4%} of source)")
        print(f"  PCM SHA-256 {info['pcm_sha256']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
