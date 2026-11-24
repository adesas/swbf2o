#!/usr/bin/env python3
"""Build a self-contained single-file static HTML Bring-Your-Own-Copy (BYOC) edition.

Bundles browser/play.html, all ES modules (play.mjs, worker.mjs, vfs_opfs.mjs, byoc_store.mjs,
webaudio.mjs, webaudio-worklet.mjs, input_bridge.mjs, touch_controls.mjs, net_client.mjs,
maps_catalog.mjs, site_config.mjs), favicon/background assets, and gzip-compressed
build/browser/swbf2.{js,wasm} into ONE standalone HTML file:

  build/static/swbf2-byoc.html

Also copies build/static/coi-serviceworker.js (for static hosts like GitHub Pages or plain
`python3 -m http.server` that cannot set COOP/COEP headers) and writes build/static/_headers
(for Cloudflare Pages / Netlify).

Zero copyrighted retail game assets are included — players select their own local Steam, GOG,
or Disc GameData folder directly in the browser.
"""
import argparse
import base64
import gzip
import json
import pathlib
import re
import shutil
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent


def read_text(rel: str) -> str:
    return (ROOT / rel).read_text(encoding="utf-8")


def read_bytes(rel: str) -> bytes:
    return (ROOT / rel).read_bytes()


def safe_json_for_script(obj) -> str:
    # Escape </ so </script> inside JS/JSON never terminates the enclosing HTML <script> tag.
    return json.dumps(obj, separators=(",", ":")).replace("</", "<\\/")


def patch_worker_for_static(src: str) -> str:
    # Replace /build/browser/swbf2.{js,wasm} network fetches with inlined gzipped blobs
    # decoded natively inside the Dedicated Worker via DecompressionStream('gzip').
    target = """    // Load build/browser/swbf2.js (UMD/MODULARIZE global SWBF2 factory) and stream swbf2.wasm
    // with real chunk-by-chunk byte progress before compiling via Emscripten's factory.
    emitLoadProgress({ phase: 'js', label: 'Downloading engine runtime…', step: 2, totalSteps: 5 });
    const jsBytes = await fetchWithProgress('/build/browser/swbf2.js', {
      phase: 'js',
      label: 'Downloading engine runtime…',
      step: 2,
      totalSteps: 5,
    });
    const jsCode = new TextDecoder().decode(jsBytes);
    const factory = new Function(`${jsCode}\\nreturn SWBF2;`)();

    const wasmBytes = await fetchWithProgress('/build/browser/swbf2.wasm', {
      phase: 'wasm',
      label: 'Downloading Battlefront II engine…',
      step: 2,
      totalSteps: 5,
    });"""

    replacement = """    // Static single-HTML edition: decompress inlined gzipped swbf2.js and swbf2.wasm natively.
    const gunzipB64 = async (b64) => {
      const bin = atob(b64);
      const u8 = new Uint8Array(bin.length);
      for (let i = 0; i < bin.length; ++i) u8[i] = bin.charCodeAt(i);
      if (typeof DecompressionStream === 'function') {
        const ds = new DecompressionStream('gzip');
        const stream = new Blob([u8]).stream().pipeThrough(ds);
        return new Uint8Array(await new Response(stream).arrayBuffer());
      }
      throw new Error('This browser does not support DecompressionStream(gzip).');
    };
    emitLoadProgress({ phase: 'js', label: 'Unpacking embedded engine runtime…', step: 2, totalSteps: 5 });
    const jsBytes = await gunzipB64(self.__SWBF2_STATIC_JS_GZ_B64__);
    const jsCode = new TextDecoder().decode(jsBytes);
    const factory = new Function(`${jsCode}\\nreturn SWBF2;`)();

    emitLoadProgress({ phase: 'wasm', label: 'Unpacking WebAssembly engine…', step: 2, totalSteps: 5 });
    const wasmBytes = await gunzipB64(self.__SWBF2_STATIC_WASM_GZ_B64__);
    const wasmBlobUrl = URL.createObjectURL(new Blob([wasmBytes], { type: 'application/wasm' }));"""

    if target not in src:
        raise RuntimeError("Could not find expected swbf2.{js,wasm} fetch block in browser/worker.mjs")
    src = src.replace(target, replacement, 1)
    src = src.replace("locateFile: (f) => '/build/browser/' + f,", "locateFile: (f) => (f.endsWith('.wasm') ? wasmBlobUrl : f),", 1)
    return src


def patch_webaudio_for_static(src: str) -> str:
    old = "const DEFAULT_WORKLET_URL = new URL('./webaudio-worklet.mjs', import.meta.url).href;"
    new = "const DEFAULT_WORKLET_URL = globalThis.__SWBF2_WORKLET_URL__ || '';"
    if old not in src:
        raise RuntimeError("Could not find DEFAULT_WORKLET_URL in browser/webaudio.mjs")
    return src.replace(old, new, 1)


def patch_play_for_static(src: str, mp_default: str) -> str:
    # 1. Point Worker to inlined Blob URL
    old_w = "worker = new Worker('/browser/worker.mjs', { type: 'module' });"
    new_w = "worker = new Worker(globalThis.__SWBF2_WORKER_URL__, { type: 'module' });"
    if old_w not in src:
        raise RuntimeError("Could not find Worker('/browser/worker.mjs') in browser/play.mjs")
    src = src.replace(old_w, new_w, 1)

    # 2. Register sibling ./coi-serviceworker.js (and reload once activated on plain static hosts)
    old_sw = """if (!self.crossOriginIsolated && 'serviceWorker' in navigator && location.protocol !== 'file:') {
  navigator.serviceWorker.register('/browser/coi-serviceworker.js').then((reg) => {
    if (reg.active && !navigator.serviceWorker.controller) {
      location.reload();
    }
  }).catch(() => {});
}"""
    new_sw = """if (!self.crossOriginIsolated && 'serviceWorker' in navigator && location.protocol !== 'file:') {
  navigator.serviceWorker.register('./coi-serviceworker.js').then((reg) => {
    const reloadOnce = () => {
      if (!sessionStorage.getItem('swbf2_coi_reloaded')) {
        sessionStorage.setItem('swbf2_coi_reloaded', '1');
        location.reload();
      }
    };
    if (reg.active && !navigator.serviceWorker.controller) {
      reloadOnce();
    } else if (reg.installing || reg.waiting) {
      const sw = reg.installing || reg.waiting;
      sw.addEventListener('statechange', () => {
        if (sw.state === 'activated' && !self.crossOriginIsolated) reloadOnce();
      });
    }
  }).catch(() => {});
}"""
    if old_sw not in src:
        raise RuntimeError("Could not find coi-serviceworker registration block in browser/play.mjs")
    src = src.replace(old_sw, new_sw, 1)

    # 3. Always run in pure static BYOC mode (no /api/server-config fetch 404 noise)
    old_cfg = """async function initByocState() {
  try {
    const res = await fetch('/api/server-config', { cache: 'no-store' });
    if (res.ok) {
      const cfg = await res.json();
      serverGamedataHosted = !!cfg.gamedataHosted;
      serverByocPrompt = typeof cfg.byocPrompt === 'boolean' ? cfg.byocPrompt : !serverGamedataHosted;
    } else {
      serverGamedataHosted = !SITE_CONFIG.byocDefault;
      serverByocPrompt = !serverGamedataHosted;
    }
  } catch {
    serverGamedataHosted = !SITE_CONFIG.byocDefault;
    serverByocPrompt = !serverGamedataHosted;
  }"""
    new_cfg = """async function initByocState() {
  // Static single-HTML edition: always pure Bring-Your-Own-Copy (no backend server required).
  serverGamedataHosted = false;
  serverByocPrompt = true;"""
    if old_cfg not in src:
        raise RuntimeError("Could not find initByocState in browser/play.mjs")
    src = src.replace(old_cfg, new_cfg, 1)

    # 4. Point optional online multiplayer signaling to external relay (or ?mp=wss://...)
    old_mp = """const DEFAULT_MP_SERVER = params.get('mp') || `${wsProto}//${location.host || '127.0.0.1:8123'}`;
const DEFAULT_MP_HTTP = params.get('mp')
  ? DEFAULT_MP_SERVER.replace(/^ws:/i, 'http:').replace(/^wss:/i, 'https:')
  : location.origin;"""
    if mp_default:
        new_mp = f"""const DEFAULT_MP_SERVER = params.get('mp') || {json.dumps(mp_default)};
const DEFAULT_MP_HTTP = DEFAULT_MP_SERVER.replace(/^ws:/i, 'http:').replace(/^wss:/i, 'https:');"""
    else:
        new_mp = """const DEFAULT_MP_SERVER = params.get('mp') || '';
const DEFAULT_MP_HTTP = DEFAULT_MP_SERVER ? DEFAULT_MP_SERVER.replace(/^ws:/i, 'http:').replace(/^wss:/i, 'https:') : '';"""
    if old_mp not in src:
        raise RuntimeError("Could not find DEFAULT_MP_SERVER block in browser/play.mjs")
    src = src.replace(old_mp, new_mp, 1)

    # 5. Guard /api/rooms fetch when no multiplayer signaling server is configured (zero 404s)
    old_rooms_fetch = "    const res = await fetch(`${DEFAULT_MP_HTTP}/api/rooms`, { cache: 'no-store' });"
    new_rooms_fetch = "    const res = DEFAULT_MP_HTTP ? await fetch(`${DEFAULT_MP_HTTP}/api/rooms`, { cache: 'no-store' }) : { ok: false };"
    if old_rooms_fetch not in src:
        raise RuntimeError("Could not find refreshRooms fetch in browser/play.mjs")
    src = src.replace(old_rooms_fetch, new_rooms_fetch, 1)

    # 6. When offline-only (no --mp-server and no ?mp=...), hide Play Online and promote Single Player
    if not mp_default:
        src += (
            "\nif (!DEFAULT_MP_SERVER) {\n"
            "  const btnOnline = document.getElementById('btnPlayOnline');\n"
            "  if (btnOnline) btnOnline.style.display = 'none';\n"
            "  const btnSp = document.getElementById('btnSinglePlayer');\n"
            "  if (btnSp) { btnSp.classList.remove('secondary'); btnSp.classList.add('primary'); }\n"
            "}\n"
        )

    return src


def scrub_wasm_paths(raw: bytes) -> bytes:
    """Replace any embedded local build path strings inside the WASM data section with equal-length
    NUL-padded basenames so byte offsets and WASM section sizes stay 100% identical."""
    def _repl(m: re.Match) -> bytes:
        full = m.group(0)
        base = full.rsplit(b"/", 1)[-1]
        return base + b"\x00" * (len(full) - len(base))
    return re.sub(rb"/(?:home|Users)/[^\x00]{1,240}", _repl, raw)


ANONYMOUS_SITE_CONFIG = """export const SITE_CONFIG = {
  siteName: 'Battlefront II (2005) — Browser Edition',
  domain: '',
  tagline: 'Classic 2005 Star Wars Battlefront II — playable natively in your browser with your own game files.',
  byocDefault: true,
  discordUrl: '',
  githubUrl: '',
  iceServers: [
    { urls: 'stun:stun.cloudflare.com:3478' },
    { urls: 'stun:stun.l.google.com:19302' },
  ],
};
"""


def build_static_edition(out_dir: pathlib.Path, filename: str, mp_default: str):
    wasm_path = ROOT / "build" / "browser" / "swbf2.wasm"
    js_path = ROOT / "build" / "browser" / "swbf2.js"
    if not wasm_path.is_file() or not js_path.is_file():
        raise SystemExit(
            f"Missing {wasm_path} or {js_path}. Run ./tools/build_release.sh first."
        )

    out_dir.mkdir(parents=True, exist_ok=True)

    # Compress swbf2.js and scrubbed swbf2.wasm at max gzip level to keep the single HTML compact.
    wasm_bytes = scrub_wasm_paths(wasm_path.read_bytes())
    js_gz_b64 = base64.b64encode(gzip.compress(js_path.read_bytes(), compresslevel=9)).decode("ascii")
    wasm_gz_b64 = base64.b64encode(gzip.compress(wasm_bytes, compresslevel=9)).decode("ascii")

    # Inline favicon & background image as data URIs
    favicon_b64 = base64.b64encode(read_bytes("browser/favicon.svg")).decode("ascii")
    bg_b64 = base64.b64encode(read_bytes("browser/assets/bg_galactic_conquest.webp")).decode("ascii")

    # Collect and patch all ES modules (anonymous site_config, zero swbf2o/discord/github links)
    modules = {
        "site_config.mjs": ANONYMOUS_SITE_CONFIG,
        "maps_catalog.mjs": read_text("browser/maps_catalog.mjs"),
        "byoc_store.mjs": read_text("browser/byoc_store.mjs"),
        "input_bridge.mjs": read_text("browser/input_bridge.mjs"),
        "touch_controls.mjs": read_text("browser/touch_controls.mjs"),
        "net_client.mjs": read_text("browser/net_client.mjs"),
        "webaudio-worklet.mjs": read_text("browser/webaudio-worklet.mjs"),
        "webaudio.mjs": patch_webaudio_for_static(read_text("browser/webaudio.mjs")),
        "vfs_opfs.mjs": read_text("browser/vfs_opfs.mjs"),
        "worker.mjs": patch_worker_for_static(read_text("browser/worker.mjs")),
        "play.mjs": patch_play_for_static(read_text("browser/play.mjs"), mp_default),
    }

    html = read_text("browser/play.html")

    # Replace external links/assets in play.html with self-contained data URIs & anonymous branding
    html = html.replace(
        "<title>SWBF2O — Play in Browser</title>",
        "<title>Battlefront II (2005) — Browser Edition</title>",
    )
    html = html.replace(
        '<span class="brand-kicker">SWBF2O · Unofficial Community Project</span>',
        '<span class="brand-kicker">Browser Edition · Bring Your Own Copy</span>',
    )
    html = html.replace(
        "<h1>SWBF2O</h1>",
        "<h1>BATTLEFRONT II</h1>",
    )
    html = html.replace(
        'href="/browser/favicon.svg"',
        f'href="data:image/svg+xml;base64,{favicon_b64}"',
    )
    html = html.replace(
        "url('/browser/assets/bg_galactic_conquest.webp')",
        f"url('data:image/webp;base64,{bg_b64}')",
    )
    # Strip Home / Discord / GitHub / Developer links so the standalone HTML is fully anonymous
    html = re.sub(
        r'\s*<a href="/" class="link-btn"[^>]*>Home</a>',
        '',
        html,
    )
    html = re.sub(
        r'\s*<a id="portalDiscordLink"[^>]*>Discord</a>',
        '',
        html,
    )
    html = re.sub(
        r'\s*<a id="portalGithubLink"[^>]*>GitHub</a>',
        '',
        html,
    )
    html = re.sub(
        r'\s*<a id="btnDevToggle"[^>]*>Developer</a>',
        '',
        html,
    )
    html = re.sub(
        r'\s*<form id="devUnlockBox"[\s\S]*?</form>',
        '',
        html,
    )

    bootstrap = f"""<script id="__swbf2_modules__" type="application/json">{safe_json_for_script(modules)}</script>
<script id="__swbf2_js_gz__" type="text/plain">{js_gz_b64}</script>
<script id="__swbf2_wasm_gz__" type="text/plain">{wasm_gz_b64}</script>
<script type="module">
  // Self-contained Single-HTML Bootstrapper (Zero backend required — Bring Your Own Copy)
  const rawMods = JSON.parse(document.getElementById('__swbf2_modules__').textContent);
  const jsGzB64 = document.getElementById('__swbf2_js_gz__').textContent.trim();
  const wasmGzB64 = document.getElementById('__swbf2_wasm_gz__').textContent.trim();

  const blobUrls = new Map();
  const makeBlobUrl = (code) => URL.createObjectURL(new Blob([code], {{ type: 'text/javascript' }}));

  // Resolve ./foo.mjs relative imports recursively into in-memory Blob URLs so both main thread
  // and Dedicated Worker ES modules run with zero external .mjs requests.
  const importRe = /(from\\s+['"])\\.\\/([^'"]+\\.mjs)(['"])/g;
  function buildModule(name) {{
    if (blobUrls.has(name)) return blobUrls.get(name);
    let code = rawMods[name];
    if (typeof code !== 'string') throw new Error('Missing bundled module: ' + name);
    code = code.replace(importRe, (_, pre, dep, post) => {{
      const depUrl = buildModule(dep);
      return `${{pre}}${{depUrl}}${{post}}`;
    }});
    if (name === 'worker.mjs') {{
      code =
        `self.__SWBF2_STATIC_JS_GZ_B64__ = ${{JSON.stringify(jsGzB64)}};\\n` +
        `self.__SWBF2_STATIC_WASM_GZ_B64__ = ${{JSON.stringify(wasmGzB64)}};\\n` +
        code;
    }}
    const url = makeBlobUrl(code);
    blobUrls.set(name, url);
    return url;
  }}

  globalThis.__SWBF2_WORKLET_URL__ = buildModule('webaudio-worklet.mjs');
  globalThis.__SWBF2_WORKER_URL__ = buildModule('worker.mjs');
  const playUrl = buildModule('play.mjs');
  await import(playUrl);
</script>"""

    script_tag = '<script type="module" src="/browser/play.mjs"></script>'
    if script_tag not in html:
        raise RuntimeError("Could not find play.mjs script tag in browser/play.html")
    html = html.replace(script_tag, bootstrap, 1)

    out_html = out_dir / filename
    out_html.write_text(html, encoding="utf-8")

    # Copy coi-serviceworker.js for static hosts that don't support custom COOP/COEP headers (GitHub Pages)
    shutil.copy2(ROOT / "browser" / "coi-serviceworker.js", out_dir / "coi-serviceworker.js")

    # Write _headers for Cloudflare Pages / Netlify (enables instant SharedArrayBuffer without SW reload)
    (out_dir / "_headers").write_text(
        "/*\n"
        "  Cross-Origin-Opener-Policy: same-origin\n"
        "  Cross-Origin-Embedder-Policy: require-corp\n"
        "  Cross-Origin-Resource-Policy: same-origin\n",
        encoding="utf-8",
    )

    (out_dir / "README.txt").write_text(
        "Battlefront II (2005) — Single Static HTML Edition (Bring Your Own Copy)\n"
        "========================================================================\n\n"
        f"Main file: {filename} ({out_html.stat().st_size / 1048576:.2f} MiB)\n\n"
        "Zero backend server or copyrighted game files required.\n"
        "Players select their own legal Star Wars Battlefront II (2005) GameData folder\n"
        "(Steam, GOG, or Disc) directly in Chrome, Edge, Brave, or Opera.\n\n"
        "Deployment options:\n"
        "  1. Cloudflare Pages / Netlify:\n"
        f"     Upload `{filename}` (or rename to `index.html`) + `_headers`.\n"
        "  2. GitHub Pages / Any plain static host:\n"
        f"     Upload `{filename}` (or rename to `index.html`) + `coi-serviceworker.js`.\n"
        "  3. Test locally with zero setup:\n"
        "     python3 -m http.server 8080 --directory build/static\n"
        f"     Open http://127.0.0.1:8080/{filename}\n",
        encoding="utf-8",
    )

    print(f"Built static single-HTML BYOC edition:")
    print(f"  HTML: {out_html} ({out_html.stat().st_size / 1048576:.2f} MiB)")
    print(f"  SW:   {out_dir / 'coi-serviceworker.js'} (optional sibling for hosts without COOP/COEP headers)")
    print(f"  HDR:  {out_dir / '_headers'} (for Cloudflare Pages / Netlify)")


def main() -> int:
    ap = argparse.ArgumentParser(description="Build anonymous standalone single-HTML BYOC edition.")
    ap.add_argument("--out-dir", default=str(ROOT / "build" / "static"), help="Output directory (default: build/static)")
    ap.add_argument("--filename", default="swbf2-byoc.html", help="Output HTML filename (default: swbf2-byoc.html)")
    ap.add_argument(
        "--mp-server",
        default="",
        help="Optional WebRTC multiplayer signaling URL (default: '' offline/anonymous; pass wss://... to enable).",
    )
    args = ap.parse_args()
    build_static_edition(pathlib.Path(args.out_dir), args.filename, args.mp_server)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
