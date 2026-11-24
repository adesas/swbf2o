# Class staging: read one trooper, not the whole faction

A soldier's authoritative definition is in `side/<faction>.lvl`. The two files the demo needs
are large:

| file | whole-file size |
| --- | ---: |
| `side/cis.lvl` | 101,132,344 bytes |
| `side/rep.lvl` | 147,884,048 bytes |

The browser host used to stage a whole file into the wasm VFS. That is both a large download
and a large resident allocation, and most of it belongs to classes the demo never uses.

## What the file actually contains

A side file is an ordinary `ucfb` chunk sequence. Its top-level chunks are `lvl_` records. Each
record's payload is:

```
uint32 class_name_hash
uint32 nested_level_size
byte[nested_level_size] nested_chunks
```

`LoadUtil::ReadDataFileChunk` selects a record by comparing its id with the hash of the class's
name (`LoadUtil.cpp:288-300`). The selected record already contains the class's `entc`, `wnpc`,
`ordc`, `expc`, models, skeletons, animation banks and textures. The class's base chain is in the
same record; `com_inf_default` comes separately from `ingame.lvl`.

So the smallest useful staging unit is the complete top-level `lvl_` record, not a new per-class
format and not a browser-side reimplementation of the class reader.

## The extraction

`tools/extract_class_level.py` writes a new `ucfb` header and copies the complete top-level
record, byte for byte. The engine's loader, chunk filter and hash selection are unchanged.

```
python3 tools/extract_class_level.py side/cis.lvl cis_inf_rifleman
python3 tools/extract_class_level.py side/rep.lvl rep_inf_ep3_rifleman
```

Measured results:

| class | extracted size | whole side file | fetched fraction |
| --- | ---: | ---: | ---: |
| `cis_inf_rifleman` | 4,981,024 bytes | 101,132,344 bytes | 4.93% |
| `rep_inf_ep3_rifleman` | 3,461,796 bytes | 147,884,048 bytes | 2.34% |
| `imp_inf_rifleman` (Stormtrooper) | 3,406,400 bytes | 100,463,052 bytes | 3.39% |

The CIS download is 96,151,320 bytes smaller; the Republic download is 144,422,252 bytes
smaller; the Imperial download is 97,056,652 bytes smaller. The extracted records retain their
models and textures because the renderer needs those as well as the simulation.

The tool validates the `ucfb` size, every top-level chunk boundary, the nested-size relation,
and duplicate class hashes. It never modifies the game data dump. Output goes to
`build/class_data/`.

## How the host uses it

`tools/gen_manifest.py` adds an entry for each extracted record. An entry carries:

* `classFile`: the original engine name, for example `side/cis.lvl;cis_inf_rifleman`;
* `url`: the small file under `/build/class_data/`;
* `fsPath`: the original side file's VFS name, `/data/side_cis.lvl`.

`browser/host.js` fetches `url` but writes the bytes to `fsPath`. The engine therefore opens
exactly the name it would open in the full game, while the page downloads only the selected
record. `window.__stageClass(classFile)` stages it and `window.__loadClass(classFile)` also
calls the engine's class loader.

The original full side files remain in the manifest and can still be staged deliberately.
Only one extracted record per original side file can occupy its VFS alias at a time; staging
another class from the same faction replaces that file before the new class is loaded. Classes
already registered with the engine are unaffected.

`tools/demo.sh` extracts the demo's CIS and Republic rifleman records if they are missing and
then regenerates the manifest.

## Verification

The spawn oracle accepts `--extracted`, which uses the small record in both builds and compares
the engine's results exactly:

```
python3 tools/spawn_oracle_check.py --extracted
python3 tools/spawn_oracle_check.py --extracted --class rep_inf_ep3_rifleman
```

All three pass. The CIS trooper has 400 health, four built weapons and six collision bodies;
the Republic and Imperial troopers have 400 health, five built weapons and six collision
bodies. They stand at the same ground-rest height and survive 60 engine updates in the native
ILP32 and browser wasm32 builds.
