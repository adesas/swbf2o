# Contributing to Star Wars Battlefront II: Browser Edition

Thank you for helping improve the browser port, WebGPU renderer, audio engine, and multiplayer portal!

## Ground Rules (Important)

1. **Zero Copyrighted Game Assets:** Never commit retail `.lvl`, `.bnk`, `.mvs`, `.exe`, `.dll`, ISO dumps, or pirated archive listings/manifests. Always test using your own legal Steam, GOG, or Disc copy under `data/` (gitignored).
2. **No Upstream Source Mirroring:** Do not commit files from `vendor/openswbf2` or `build/src_fixed`. Keep browser-side patches clean and auditable in `tools/prepare_tree.py` or `browser/`.
3. **No Personal Paths or Secrets:** Never hardcode personal filesystem paths (`/home/<user>/...`), API tokens, private TLS keys, or local hostnames. Always derive paths relative to the repository root.

## Development Workflow

```sh
# 1. Build release or debug WASM after C++ changes
./tools/build_release.sh      # or ./tools/build_browser.sh for debug

# 2. Run the automated verification suites (requires Chrome + local legal GameData)
./vendor/emsdk/node/24.19.0_64bit/bin/node browser/byoc_check.mjs
./vendor/emsdk/node/24.19.0_64bit/bin/node browser/phase5_input_audio_check.mjs
./vendor/emsdk/node/24.19.0_64bit/bin/node browser/phase7_release_check.mjs
```

## Submitting Pull Requests

- Keep commits focused and descriptive.
- Include screenshots or test output for visual/WebGPU, UI, audio, or multiplayer fixes.
- Verify `git status` and `git diff` contain zero personal data or binary build artifacts before opening a PR.
