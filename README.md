# Star Wars Battlefront II: Browser Edition (SWBF2O)

Play the classic 2005 **Star Wars Battlefront II** natively in your web browser with **WebGPU**, **WebAssembly**, **48 kHz 3D WebAudio**, **WebRTC Multiplayer & Voice Chat**, and **Touch Controls** — using **your own legal copy** of the game.

> **Bring Your Own Copy (BYOC):** This repository and [swbf2o.com](https://swbf2o.com) distribute **zero** copyrighted game files, `.lvl`/`.bnk`/`.mvs` assets, or proprietary source code. Players point the browser directly at their own local Steam, GOG, or Disc `GameData` folder — your files stay 100% on your device and are never uploaded.

---

## Features

- **Bring-Your-Own-Copy Local File Engine** — Select your local Steam, GOG, or retail Disc `GameData` folder via the File System Access API (`showDirectoryPicker`) or folder picker fallback (`webkitdirectory`). The engine reads exact 256 KiB slices synchronously in a Dedicated Worker via `FileReaderSync` with IndexedDB handle persistence for 1-click reconnects.
- **Native WebGPU + SM 1.1 → WGSL Renderer** — Compiles Direct3D 9 fixed-function & Shader Model 1.1 (`vs_1_1` / `ps_1_1`) shaders to WGSL with hardware BCn compressed textures (`DXT1`/`DXT3`/`DXT5`), skinned hardware characters, particles, skies, and 60+ FPS rendering.
- **Low-Latency 48 kHz 3D WebAudio** — Lock-free `SharedArrayBuffer` `AudioWorklet` mixer synchronized to hardware sample rates with full SFX, music, and voiceover support.
- **Online WebRTC Multiplayer & Voice Chat** — Browse public lobbies or host custom multi-map rotations across all 24 ground & space maps (including XL, Hero Assault, Hunt, CTF, and Space Assault) with passwords, full SWBF2 host rules, lobby chat, in-game All/Team chat (`T`/`Y`), hot-join, and Push-to-Talk (`V`) or open-mic WebRTC voice chat.
- **Mobile & Tablet Touch Controls** — Adaptive virtual twin-stick + combat HUD for Android and iOS.
- **Persistent Browser Saves (Import & Export)** — Profiles and Galactic Conquest / Campaign saves persist in OPFS and can be exported/imported as `.json` bundles or native `.profile`/`.gc`/`.rote`/`.sav` files.
- **Classic Loading Screens** — Live-decodes the original map loading banners, metallic tips frame, and red loading indicators straight from your local `GameData/load/`.

---

## Quick Start: Playing in Your Browser

1. Own a legal copy of **Star Wars Battlefront II (Classic, 2005)** on [Steam](https://store.steampowered.com/app/6060/STAR_WARS_Battlefront_II_Classic_2005/), [GOG](https://www.gog.com/game/star_wars_battlefront_ii), or retail CD/DVD.
2. Open **[swbf2o.com/play](https://swbf2o.com/play)** in a WebGPU-capable browser (Chrome, Edge, Brave, or Opera).
3. Click **Select GameData Folder…** and pick your local install folder (any of the game root, `GameData`, or `GameData/data/_lvl_pc`):
   - **Steam (Windows):** `C:\Program Files (x86)\Steam\steamapps\common\Star Wars Battlefront II Classic\GameData`
   - **Steam (Linux / Steam Deck):** `~/.local/share/Steam/steamapps/common/Star Wars Battlefront II Classic/GameData`
   - **GOG Galaxy:** `C:\Program Files (x86)\GOG Galaxy\Games\Star Wars Battlefront II\GameData`
4. Enter a username and click **Play Online** or **Single Player**.

---

## Building & Self-Hosting From Source

### Prerequisites
- Linux/macOS/WSL2 with `python3`, `git`, `clang`, and [Emscripten (`emsdk`)](https://github.com/emscripten-core/emsdk)
- Upstream engine source cloned into `vendor/openswbf2` ([OpenSWBF2/OpenSWBF2](https://github.com/OpenSWBF2/OpenSWBF2))

### Build Steps

```sh
# 1. Clone external dependencies (gitignored)
git clone https://github.com/OpenSWBF2/OpenSWBF2 vendor/openswbf2
git clone https://github.com/emscripten-core/emsdk vendor/emsdk
vendor/emsdk/emsdk install latest && vendor/emsdk/emsdk activate latest

# 2. Build the WebAssembly core + WebGPU browser release bundle
./tools/build_core.sh
./tools/build_release.sh

# 3. Start the BYOC web + WebRTC multiplayer signaling server
python3 tools/serve.py 8123 --host 0.0.0.0 --mp-port 8133

# Optional: Build the standalone single-file static HTML BYOC edition (no backend required)
python3 tools/build_static_html.py
# Output: build/static/swbf2-byoc.html (+ coi-serviceworker.js / _headers for static hosts)
```

By default `tools/serve.py` runs in **Bring-Your-Own-Copy mode** and serves **zero** game assets (`/GameData/*` returns `404`). For private local development or running the automated browser test suites against your own local copy, pass `--serve-gamedata --gamedata-dir /path/to/GameData/data/_lvl_pc` (after running `python3 tools/gen_manifest.py`).

See [docs/BUILD.md](docs/BUILD.md) for full build flags, architecture notes, and verification suites.

---

## Repository Structure

| Path | Description |
|---|---|
| `index.html` | Public landing page |
| `browser/` | WebGPU/WGSL renderer, platform shims (`pblfile`, `dsound`, `dinput`, `winsock`, `bink`), OPFS + BYOC VFS, WebAudio worklet, WebRTC client, touch controls, and `/play` portal |
| `spike/shim/` | Portable Win32 / Direct3D 9 / DirectInput / DirectSound / Winsock header shims for `wasm32` |
| `tools/` | Build pipeline scripts, static BYOC server (`serve.py`), WebRTC signaling & room directory server (`multiplayer_server.mjs`), and verification harnesses |
| `deploy/` | Sample systemd, Caddy, and Cloudflare Tunnel configs |
| `docs/` | Technical architecture docs, build guide, and [Discord Community Kit](docs/DISCORD_COMMUNITY_KIT.md) |

---

## Community

- **Play Online:** [https://swbf2o.com](https://swbf2o.com)
- **Discord Community:** [https://discord.gg/WrBnZJQNtJ](https://discord.gg/WrBnZJQNtJ)
- **GitHub Repository:** [https://github.com/adesas/swbf2o](https://github.com/adesas/swbf2o)
- **Contributing:** See [CONTRIBUTING.md](CONTRIBUTING.md).

---

## Legal & Disclaimer

This is an independent, non-commercial, open-source community project. **No copyrighted game assets, textures, audio, maps, movies, or proprietary binaries are included in this repository or hosted by default.** Players must supply their own legally owned copy of *Star Wars Battlefront II (2005)*.

*Star Wars* and *Star Wars Battlefront II* are trademarks of Lucasfilm Ltd. / Disney and Pandemic Studios. No affiliation or endorsement is implied.

Original browser port, renderer, portal, and tooling code in this repository is licensed under the [MIT License](LICENSE). Third-party `libbinkdec` sources under `browser/platform/binkdec/` retain their original LGPL license (`browser/platform/binkdec/COPYING`).
