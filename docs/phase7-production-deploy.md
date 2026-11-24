# Phase 7: Production Website (`play.html`), Release Optimization (`-O2 -msimd128`), & Deployment Guide

Status: **VERIFIED COMPLETE (`9 / 9` Phase 7 release/launcher checks PASS, `15 / 15` Phase 6 WebRTC multiplayer PASS, `20 / 20` Phase 5 input/3D WebAudio PASS, `29 / 29` Phase 4 GameState/Bink1 PASS, `25 / 25` Phase 3 SM 1.1->WGSL PASS, and all `4 / 4` native-vs-WASM determinism oracles bit-identical on the `-O2 -ffp-contract=off -msimd128` release build).**

---

## 1. Release Optimization Pipeline (`tools/build_release.sh`)

`SWBF2_RELEASE=1` (wrapped by [tools/build_release.sh](../tools/build_release.sh)) compiles all `771 / 771` engine TUs in [tools/compile_core.sh](../tools/compile_core.sh) and links both browser WebGPU WASM and Node oracle WASM in [tools/build_browser.sh](../tools/build_browser.sh) with:

- **`-O2 -ffp-contract=off -msimd128`**: full LLVM optimization + 128-bit WebAssembly SIMD while preserving IEEE-754 floating-point determinism (zero FMA contraction drift across all 4 native-vs-WASM oracles).
- **`-sASSERTIONS=0`** (strips debug assertions and `--profiling-funcs`).
- **`GameLoop.cpp` `SoldierAnimatorClass::PostLoad()` fix** recorded in [tools/prepare_tree.py](../tools/prepare_tree.py) (`KNOWN_ARTIFACTS`): removes `inline` from the out-of-line `.cpp` definition so clang `-O2` retains the external symbol for harness callers.

### Measured Binary Size Reduction (Debug -> Release)

| Artifact | Debug (`-O0`/`-O1`, `ASSERTIONS=1`) | Release (`-O2 -msimd128`, `ASSERTIONS=0`) | Gzip (`-9`) |
|---|---|---|---|
| `build/core.a` (771 TUs) | 51.12 MiB | **39.56 MiB** | — |
| `build/browser/swbf2.wasm` | 8.85 MiB | **4.34 MiB** (-51%) | **1.67 MiB** |
| `build/browser/swbf2.js` | 0.39 MiB | **0.15 MiB** (-61%) | ~0.04 MiB |
| `build/browser/swbf2_node.wasm` | 8.13 MiB | **2.90 MiB** (-64%) | — |

---

## 2. Production Web Shell & UX ([browser/play.html](../browser/play.html), [browser/play.mjs](../browser/play.mjs))

- **Start-Gated Boot Screen + System Preflight:** Probes WebGPU adapter + `texture-compression-bc` (with retry loop for cold GPU process init), `window.crossOriginIsolated` (COOP/COEP), OPFS persistent storage & cached MB, and 48 kHz WebAudio `AudioContext` before launching.
- **Quick-Launch Mode Cards:** One-click entry into the native 2005 Main Menu (`ShellState`), Mos Eisley Conquest (`tat2g_con`), Mos Eisley Hero Assault (`tat2g_eli`), or Space Yavin Assault (`spa1g_ass`).
- **Live Online Multiplayer Room Browser:** Polls `GET /api/rooms` on [tools/multiplayer_server.mjs](../tools/multiplayer_server.mjs), lists active rooms with map & player count, supports one-click Join, Host Room, Copy Invite Link (`?join=<code>`), and live WebRTC/WS status telemetry.
- **Slide-Out Settings & Storage Drawer:**
  - Live WebGPU display resolution switcher (`800×600`, `1024×768`, `1280×720`, `1600×900` via `swbf2_wgpu_resize`),
  - Audio Mute + live RMS level meter,
  - **OPFS Asset Cache Manager** (displays cached blocks/MB and clears cache via `vfsOp: clearCache`),
  - **Save Profile Backup Manager** (exports & imports all `/saves/*.profile` files as JSON via `OpfsVfs::exportSaves` / `importSaves` in [browser/vfs_opfs.mjs](../browser/vfs_opfs.mjs)).
- **In-Game Controls:** Auto-hidable HUD bar (`F2`), PointerLock recovery hint on `Escape`, and Fullscreen toggle.

---

## 3. Hosting & Deployment Checklist

To deploy on a static CDN (Cloudflare Pages, Netlify, Nginx, GitHub Pages) + signaling server:

1. **HTTPS Required:** `requestPointerLock()`, `navigator.getGamepads()`, WebGPU, and WebRTC require a secure origin (`https://` or `http://127.0.0.1`).
2. **Cross-Origin Isolation (COOP/COEP):** Configure response headers on the web host:
   ```http
   Cross-Origin-Opener-Policy: same-origin
   Cross-Origin-Embedder-Policy: require-corp
   Cross-Origin-Resource-Policy: same-origin
   ```
   On static hosts without custom header rules (e.g. GitHub Pages), [browser/coi-serviceworker.js](../browser/coi-serviceworker.js) is automatically registered by `play.mjs` when `window.crossOriginIsolated === false` and injects those headers transparently.
3. **Byte-Range Asset Mirror:** Serve `GameData/data/_lvl_pc/` with `Accept-Ranges: bytes`, `Content-Range`, and CORS (`Access-Control-Allow-Origin` + `Cross-Origin-Resource-Policy: cross-origin` if on a separate CDN origin), matching `browser/gamedata-manifest.json`.
4. **Multiplayer Signaling Server:** Run `node tools/multiplayer_server.mjs 8133` behind WSS (`wss://...`) and pass `?mp=wss://<your-signaling-host>` (or set `DEFAULT_MP_SERVER` in `browser/play.mjs`).

---

## 4. Release Verification Summary

Verified against the `-O2 -ffp-contract=off -msimd128` release build in Headless Chrome + Hardware Vulkan:

- **[browser/phase7_release_check.mjs](../browser/phase7_release_check.mjs): `9 / 9 PASS`** (`swbf2.wasm` = `4.34 MiB` raw / `1.67 MiB` gzip; `bootMs = 555 ms`; `59 fps` at `1280×720` with `0` WebGPU errors; preflight OK; live room browser discovered `galaxies_live`; OPFS save export -> clear -> import restored `.profile`; OPFS clear cache zeroed `cachedBlocks`; `coi-serviceworker.js` verified).
- **All 4 native-vs-WASM determinism oracles (`ground`, `spawn`, `input_camera`, `combat`): `4 / 4 PASS`** (bit-identical under `-O2 -ffp-contract=off -msimd128`).
- **Full End-to-End Regression on Release Build:** Phase 3 (`25 / 25`, `2332/2332 vs_1_1`, `226/226 ps_1_1`, `loadMs = 536 ms`), Phase 4 (`29 / 29`, `elapsedMs = 6920 ms`), Phase 5 (`20 / 20`, `move = 5.317 m`, `workletFrames = 296,145`), and Phase 6 two-tab WebRTC multiplayer (`15 / 15`, `host.remotePos == client.playerPos == [118.1, -0.02, 42.806]`, `movedMeters = 4.778 m`) all green with `0` WebGPU errors.
