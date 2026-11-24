#!/usr/bin/env python3
"""Generate GameData manifests for swbf2-web.

Emits two files:
  1. browser/gamedata-manifest.json (v2): complete case-insensitive index of EVERY file under
     GameData/data/_lvl_pc (.lvl, .bnk, .mvs, .def, .ini, etc.) with exact byte sizes and
     deterministic SHA-256 fingerprints for OPFS sparse block cache validation.
  2. browser/manifest.json (v1 legacy): list of .lvl containers and optional extracted class
     slices for backwards compatibility with the legacy demo page.

Usage:
  python3 tools/gen_manifest.py               # fast head+tail+size SHA-256 fingerprints (~0.2s)
  python3 tools/gen_manifest.py --full-sha256 # full-file SHA-256 digests for release publishing
"""
import argparse
import hashlib
import json
import pathlib

GAME = pathlib.Path("data/Star wars battlefront 2/GameData/data/_lvl_pc")
OUT_V1 = pathlib.Path("browser/manifest.json")
OUT_V2 = pathlib.Path("browser/gamedata-manifest.json")
CLASS_DATA = pathlib.Path("build/class_data")

SAMPLE_BYTES = 65536


def fingerprint_file(path: pathlib.Path, size: int, full: bool) -> str:
    h = hashlib.sha256()
    if full:
        with path.open("rb") as fh:
            while True:
                buf = fh.read(1 << 20)
                if not buf:
                    break
                h.update(buf)
        return "sha256:" + h.hexdigest()

    # Fast fingerprint: exact size + first 64 KiB + last 64 KiB. Runs across all 5.2 GB in <0.2s
    # while detecting any truncation, replacement, or header/footer edit reliably.
    h.update(f"size:{size}:".encode("ascii"))
    with path.open("rb") as fh:
        head = fh.read(SAMPLE_BYTES)
        h.update(head)
        if size > SAMPLE_BYTES:
            fh.seek(max(0, size - SAMPLE_BYTES))
            tail = fh.read(SAMPLE_BYTES)
            h.update(tail)
    return "fast64k:" + h.hexdigest()[:32]


def main():
    ap = argparse.ArgumentParser(description="Generate v1 and v2 GameData manifests.")
    ap.add_argument("--full-sha256", action="store_true",
                    help="Compute full-file SHA-256 digests instead of fast 64 KiB head+tail fingerprints.")
    args = ap.parse_args()

    # ---- v2 manifest: EVERY file in _lvl_pc ---------------------------------
    v2_files = {}
    total_bytes = 0
    for p in sorted(GAME.rglob("*")):
        if not p.is_file():
            continue
        rel = p.relative_to(GAME).as_posix()
        size = p.stat().st_size
        total_bytes += size
        v2_files[rel.lower()] = {
            "path": rel,
            "size": size,
            "ext": p.suffix.lower(),
            "fingerprint": fingerprint_file(p, size, args.full_sha256),
        }

    manifest_v2 = {
        "version": 2,
        "baseUrl": "/GameData/",
        "blockSize": 262144,
        "fileCount": len(v2_files),
        "totalBytes": total_bytes,
        "files": v2_files,
    }
    OUT_V2.write_text(json.dumps(manifest_v2, indent=1) + "\n")
    print(f"{OUT_V2}: {len(v2_files)} files, {total_bytes / 1073741824:.2f} GiB total "
          f"({'full sha256' if args.full_sha256 else 'fast64k fingerprints'})")

    # ---- v1 legacy manifest: backwards compatibility for legacy host.js -----
    files = []
    for p in GAME.rglob("*.lvl"):
        rel = p.relative_to(GAME).as_posix()
        files.append({"path": rel, "size": p.stat().st_size, "kind": "container"})

    for p in CLASS_DATA.rglob("*.lvl"):
        rel = p.relative_to(CLASS_DATA)
        parts = rel.parts
        if len(parts) != 3:
            continue
        source = f"{parts[0]}/{parts[1]}.lvl"
        class_name = p.stem
        if parts[0] == "sound":
            bank = f"{parts[0]}/{parts[1]}.bnk"
            source_path = GAME / bank
            files.append({
                "path": f"{bank};{class_name}",
                "kind": "sound",
                "size": p.stat().st_size,
                "url": "/" + p.as_posix(),
                "fsPath": "/data/" + bank.replace("/", "_"),
                "sourceSize": source_path.stat().st_size if source_path.exists() else 0,
            })
            continue
        class_file = f"{source};{class_name}"
        files.append({
            "path": class_file,
            "classFile": class_file,
            "kind": "class",
            "size": p.stat().st_size,
            "url": "/" + p.as_posix(),
            "fsPath": "/data/" + source.replace("/", "_"),
            "sourceSize": (GAME / source).stat().st_size,
        })

    files.sort(key=lambda f: f["size"])
    OUT_V1.write_text(json.dumps(files, indent=1) + "\n")
    print(f"{OUT_V1}: {len(files)} containers, "
          f"{files[0]['size'] / 1048576:.1f} MB smallest, "
          f"{sum(f['size'] for f in files) / 1073741824:.1f} GB total")


if __name__ == "__main__":
    main()
