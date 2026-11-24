# Phase 1: Dedicated Worker + OPFS On-Demand GameData Streaming VFS

This document records the architecture, implementation, and measured Chrome verification of the
on-demand `GameData` mirror streaming filesystem (`browser/vfs_opfs.mjs` +
`browser/platform/pblfile_browser.cpp` + `browser/worker.mjs`).

## Problem Solved

The retail *Star Wars Battlefront II (2005)* PC `_lvl_pc` directory contains **196 files totaling
5.227 GB (4.87 GiB)**:

* `.lvl`: 185 containers (`3,289.89 MB`), including 101–148 MB faction containers (`side/rep.lvl`,
  `side/cis.lvl`, `side/imp.lvl`, `side/all.lvl`) and 17–75 MB map containers.
* `.mvs`: 8 Bink movie archives (`1,859.02 MB`).
* `.bnk`: 1 sound bank (`sound/common.bnk`, `77.94 MB`).
* `.def` / `.ini`: 2 metadata files (`~20 KB`).

Copying containers into Emscripten's in-RAM `MEMFS` hits browser WASM memory ceilings immediately
and wastes hundreds of megabytes downloading classes, sound samples, and level sections that
`LoadUtil::ReadDataFileOnHeap` skips over via `PblFile::Seek()`.

## Architecture

### 1. Cross-Origin Isolation (`tools/serve.py` + `browser/coi-serviceworker.js`)

`tools/serve.py` emits:

* `Cross-Origin-Opener-Policy: same-origin`
* `Cross-Origin-Embedder-Policy: require-corp`
* `Cross-Origin-Resource-Policy: same-origin`
* HTTP `Range` (`206 Partial Content`, `Accept-Ranges: bytes`, `Content-Range`, `x-total-size`)
  with case-insensitive path resolution under `/GameData/` (matching Windows/SWBF2 path rules).

For static hosts that cannot configure response headers (e.g., GitHub Pages),
`browser/coi-serviceworker.js` injects matching COOP/COEP headers via a Service Worker while
preserving HTTP `Range` requests verbatim.

### 2. Full GameData Manifest v2 (`tools/gen_manifest.py` → `browser/gamedata-manifest.json`)

Indexes all 196 files under `GameData/data/_lvl_pc/` in `< 0.2 s` with exact byte sizes,
case-insensitive lowercase lookup keys, and deterministic 64 KiB head+tail+size SHA-256
fingerprints (with `--full-sha256` available for release CDN manifests). `browser/manifest.json`
(v1) continues to be generated for backwards compatibility with the legacy demo page.

### 3. Unified Pooled OPFS Block Store (`browser/vfs_opfs.mjs`)

Inside a Dedicated Worker (`browser/worker.mjs`), `OpfsVfs` opens **three**
`FileSystemSyncAccessHandle` files **once** asynchronously during `await vfs.init()` under
`swbf2_opfs_v1/`:

| OPFS File | Role |
|---|---|
| `blocks.bin` | Pooled `256 KiB` slot store (default budget `4,096` slots = `1 GiB`, LRU recyclable) holding sparse slices of any of the 196 GameData files. |
| `index.bin` | Append-only binary journal (`24 B` records: magic `'BLK1'`, 64-bit file+fingerprint hash, `blockIdx`, `slot`, `byteLen`). Reloading reconstructs the cache map in `< 2 ms` and automatically invalidates entries if a file's manifest fingerprint changes. |
| `saves.bin` | Synchronous read-write persistent store backing `/saves/*`, `SaveGames/*`, `*.profile`, and `*.sav`. |

Opening a fixed set of three `SyncAccessHandle`s at Worker boot means:
* Every `PblFile::Open`, `OpenCreate`, `OpenReadWrite`, `Seek`, `Tell`, `ReadData`, `WriteData`,
  `Flush`, and `Close` call from C++ is **100% synchronous** with **zero `-sASYNCIFY` overhead**.
* Browser per-origin `SyncAccessHandle` limits are never exceeded regardless of how many `.lvl` /
  `.bnk` / `.mvs` files the game touches.
* On a block cache miss, contiguous missing blocks needed by a read are coalesced (up to 4 blocks =
  `1 MiB`) into a single synchronous Worker `XMLHttpRequest` (`responseType = 'arraybuffer'`,
  `Range: bytes=lo-hi`), strictly validated against `206 Partial Content` and `Content-Range`,
  written synchronously to `blocks.bin` + `index.bin`, and cached in a 32 MiB (`128 × 256 KiB`)
  in-worker LRU RAM cache.

### 4. Zero-Boundary-Chatter `PblFile` Window (`browser/platform/pblfile_browser.cpp`)

`PblFile` stores a `BrowserFileHandle*` in `_Buffer` (preserving exact `PblFile` class layout).
When `Module.vfsOpen` is attached by the Worker:

* Each read-only VFS handle keeps a **64 KiB read-ahead window inside WASM linear memory**, so
  tens of thousands of 4-byte chunk headers (`Read32()`, `ReadFloat()`) execute as pure WASM
  `memcpy`s without crossing the WASM→JS boundary.
* Large reads (`>= 32 KiB`) copy directly from `vfs_opfs.mjs` into the caller's destination buffer.
* `Seek()` adjusts `h->pos` in C++ without issuing any JS or HTTP calls, so skipping 131 MB of
  unused faction classes in `side/rep.lvl` triggers **zero** network transfers for skipped regions.
* When `Module.vfsOpen` is absent (Node.js tests, native i386 oracles, legacy MEMFS host), `PblFile`
  falls back seamlessly to POSIX `FILE*` (`fopen`/`fread`/`fseek`).

## Measured Verification Results (Headless Chrome `152.0.7977.82`)

Reproduce:
```sh
python3 tools/gen_manifest.py
./tools/build_browser.sh
vendor/emsdk/node/24.19.0_64bit/bin/node browser/vfs_check.mjs
```

The test boots a Dedicated Worker, loads `core.lvl`, `tat/tat2.lvl`, `ingame.lvl`,
`side/rep.lvl;rep_inf_ep3_rifleman` (out of the **raw 147.88 MB** retail container), and
`sound/common.bnk` (`force_push_fire` out of the **raw 77.94 MB** retail bank), probes ground,
spawns the Republic rifleman through `EntitySoldierClass::Build`, writes `/saves/profile_test.sav`,
**terminates the Worker**, spawns a **brand-new Worker** against the same OPFS, and repeats the
entire workload warm:

| Metric | Cold Run (Fresh OPFS) | Warm Run (New Worker) | Raw Retail File Sizes |
|---|---|---|---|
| Elapsed wall time | `538 ms` | `160 ms` | — |
| `window.crossOriginIsolated` | `true` | `true` | — |
| `SharedArrayBuffer` available | `true` | `true` | — |
| `opfsPersistent` | `true` | `true` | — |
| Files copied to MEMFS `/data` | **0** | **0** | — |
| Total HTTP requests | `162` | **0** | — |
| Total HTTP bytes transferred | **48.91 MB** (`17.8%` of 274.31 MB) | **0 B** | `274.31 MB` |
| `core.lvl` HTTP bytes | `5.59 MB` (18 reqs) | `0 B` (22 OPFS hits) | `7.16 MB` |
| `tat/tat2.lvl` HTTP bytes | `14.48 MB` (47 reqs) | `0 B` (56 OPFS hits) | `26.28 MB` |
| `ingame.lvl` HTTP bytes | `11.80 MB` (44 reqs) | `0 B` (45 OPFS hits) | `15.04 MB` |
| `side/rep.lvl` HTTP bytes | **16.52 MB** (51 reqs, **11.2%**) | `0 B` (63 OPFS hits) | `147.88 MB` |
| `sound/common.bnk` HTTP bytes | **524 KiB** (2 reqs, **0.67%**) | `0 B` (2 OPFS hits) | `77.94 MB` |
| `tat2` collision meshes | `37` | `37` | — |
| Ground probes `(0,0)` / `(25,25)` | `rest 1.2` / `rest 16.166317` | identical | — |
| Spawned `rep_inf_ep3_rifleman` | `400/400 hp`, `5/5 wpns`, `6 bodies` | identical | — |
| `force_push_fire` PCM SHA-256 | `ba7b1ad5...e726391` (45,493 frames) | identical | — |
| OPFS `/saves/profile_test.sav` | written & read back | persisted across Workers | — |

Regression checks also verified after modifying `browser/platform/pblfile_browser.cpp`:
* `browser/node_test.mjs`: PASS
* `browser/audio_check.mjs --no-browser`: PASS
* `tools/ground_oracle_check.py --skip-build`: PASS
* `tools/spawn_oracle_check.py --skip-build`: PASS
