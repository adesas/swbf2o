#!/usr/bin/env python3
"""Static server for the demo.

Serves the repository at / and the 2005 GameData dump under /GameData/... The dump lives at
a path containing spaces and unicode, so the two prefixes are mapped here instead of
symlinked. Responses carry x-total-size so the page can show real download progress, and
nothing is cached - a stale wasm module in the browser cache is the most confusing failure
there is while iterating.

Binds 0.0.0.0 by default so other machines on the network (another desktop, a laptop, a phone
plugged into a TV) can open the page; pass --host 127.0.0.1 to keep it to this machine. Every
address it can be reached on is printed on startup, because "it works from localhost" and "I
could not find the right address" otherwise look identical.
"""
import argparse
import fcntl
import hashlib
import hmac
import http.cookies
import http.server
import json
import os
import pathlib
import secrets
import shutil
import socket
import socketserver
import ssl
import struct
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.parse
import urllib.request

ROOT = pathlib.Path(__file__).resolve().parent.parent
GAME = (ROOT / "data/Star wars battlefront 2/GameData/data/_lvl_pc").resolve()
DEV_HASH_FILE = ROOT / "build" / "dev_password.hash"
DEV_INVITES_FILE = ROOT / "build" / "dev_invites.json"
DEV_COOKIE_NAME = "swbf2_dev_session"
DEV_SESSION_TTL_SECS = 12 * 3600
DEV_RATE_WINDOW_SECS = 600
DEV_RATE_MAX_FAILS = 5
MP_DEFAULT_PORT = 8133
SERVE_GAMEDATA_ENV = os.environ.get("SWBF2_SERVE_GAMEDATA", "").strip().lower() in ("1", "true", "yes")
BYOC_PROMPT_ENV = os.environ.get("SWBF2_BYOC_PROMPT", "").strip().lower() in ("1", "true", "yes")

_DEV_LOCK = threading.Lock()
_DEV_SESSIONS = {}  # token -> {"exp": monotonic_ts, "kind": "password"|"invite", "invite_id": str}
_DEV_FAILS = {}     # client_ip -> [monotonic fail timestamps]


def _load_dev_invites():
    try:
        if DEV_INVITES_FILE.is_file():
            data = json.loads(DEV_INVITES_FILE.read_text(encoding="utf-8"))
            if isinstance(data, dict) and isinstance(data.get("invites"), list):
                return data["invites"]
    except Exception:
        pass
    return []


def _save_dev_invites(invites):
    DEV_INVITES_FILE.parent.mkdir(parents=True, exist_ok=True)
    tmp = DEV_INVITES_FILE.with_suffix(".json.tmp")
    payload = json.dumps({"version": 1, "invites": invites}, indent=2) + "\n"
    fd = os.open(str(tmp), os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    try:
        os.write(fd, payload.encode("utf-8"))
    finally:
        os.close(fd)
    os.chmod(tmp, 0o600)
    os.replace(tmp, DEV_INVITES_FILE)
    os.chmod(DEV_INVITES_FILE, 0o600)


def _redeem_dev_invite(token: str):
    """Verify and consume one use of a hashed invite token. Returns invite_id or None."""
    if not token or len(token) < 16 or len(token) > 128:
        return None
    digest = hashlib.sha256(token.encode("utf-8")).hexdigest()
    now_wall = time.time()
    invites = _load_dev_invites()
    matched = None
    for inv in invites:
        stored = str(inv.get("sha256") or "")
        if len(stored) == 64 and hmac.compare_digest(digest, stored):
            matched = inv
            break
    if not matched:
        return None
    if float(matched.get("expires_at", 0)) <= now_wall:
        return None
    uses = int(matched.get("uses", 0))
    max_uses = int(matched.get("max_uses", 1))
    if uses >= max_uses:
        return None
    matched["uses"] = uses + 1
    _save_dev_invites(invites)
    return str(matched.get("id") or "")


def _load_dev_password_hash():
    env_hash = os.environ.get("SWBF2_DEV_PASSWORD_HASH", "").strip()
    if env_hash:
        return env_hash
    try:
        if DEV_HASH_FILE.is_file():
            return DEV_HASH_FILE.read_text(encoding="ascii").strip()
    except OSError:
        pass
    return ""


def _verify_dev_password(password: str, stored_hash: str) -> bool:
    if not password or not stored_hash:
        return False
    parts = stored_hash.split("$")
    if len(parts) != 4 or parts[0] != "pbkdf2_sha256":
        return False
    try:
        iters = int(parts[1])
        salt = bytes.fromhex(parts[2])
        expected = bytes.fromhex(parts[3])
    except ValueError:
        return False
    if iters < 100_000 or len(salt) < 8 or len(expected) != 32:
        return False
    actual = hashlib.pbkdf2_hmac("sha256", password.encode("utf-8"), salt, iters)
    return hmac.compare_digest(actual, expected)


def _prune_dev_state(now: float):
    expired = [tok for tok, rec in _DEV_SESSIONS.items() if rec["exp"] <= now]
    for tok in expired:
        _DEV_SESSIONS.pop(tok, None)
    for ip in list(_DEV_FAILS.keys()):
        recent = [t for t in _DEV_FAILS[ip] if (now - t) < DEV_RATE_WINDOW_SECS]
        if recent:
            _DEV_FAILS[ip] = recent
        else:
            _DEV_FAILS.pop(ip, None)

# SIOCGIFADDR / SIOCGIFNETMASK: the ioctl numbers for reading an interface's IPv4 address
# without shelling out to `ip` or depending on a third-party module.
SIOCGIFADDR = 0x8915
SIOCGIFNETMASK = 0x891B
SIOCGIFBRDADDR = 0x8919


class Handler(http.server.SimpleHTTPRequestHandler):
    # HTTP/1.1, not the default 1.0. With 1.0 every request is its own connection, which on the TLS
    # listener means a fresh handshake for each one - and a LAN client fetching a 4 MB wasm module
    # plus a few hundred GameData range requests from a phone on Wi-Fi spends more time handshaking
    # than transferring, which reads as "it loads forever, or not at all". Safe to enable because
    # every response path here sets Content-Length (send_head, serve_game_file, proxy_api,
    # send_error), and the websocket hijack sets close_connection.
    protocol_version = "HTTP/1.1"

    # 0 = never redirect. The TLS instance keeps this 0; only the plain-HTTP instance is given the
    # HTTPS port, so an https origin is never redirected to itself.
    tls_redirect_port = 0

    extensions_map = {
        **http.server.SimpleHTTPRequestHandler.extensions_map,
        ".wasm": "application/wasm",
        ".json": "application/json",
        ".mjs": "text/javascript",
        ".svg": "image/svg+xml",
        ".webp": "image/webp",
    }

    mp_port = MP_DEFAULT_PORT
    serve_gamedata = False
    byoc_prompt = True
    game_dir = GAME

    def _client_ip(self) -> str:
        cf_ip = (self.headers.get("CF-Connecting-IP") or "").strip()
        if cf_ip:
            return cf_ip
        xff = (self.headers.get("X-Forwarded-For") or "").split(",")[0].strip()
        if xff:
            return xff
        return self.client_address[0] if self.client_address else "unknown"

    def _is_https_request(self) -> bool:
        if isinstance(self.connection, ssl.SSLSocket):
            return True
        if (self.headers.get("X-Forwarded-Proto") or "").lower() == "https":
            return True
        cf_vis = (self.headers.get("CF-Visitor") or "").lower()
        return '"https"' in cf_vis

    def _dev_cookie_token(self) -> str:
        raw = self.headers.get("Cookie")
        if not raw:
            return ""
        jar = http.cookies.SimpleCookie()
        try:
            jar.load(raw)
        except http.cookies.CookieError:
            return ""
        morsel = jar.get(DEV_COOKIE_NAME)
        return morsel.value.strip() if morsel else ""

    def has_valid_dev_session(self) -> bool:
        tok = self._dev_cookie_token()
        if not tok:
            return False
        now = time.monotonic()
        with _DEV_LOCK:
            rec = _DEV_SESSIONS.get(tok)
            if not rec or rec["exp"] <= now:
                if rec is not None:
                    _DEV_SESSIONS.pop(tok, None)
                return False
            kind = rec.get("kind")
            invite_id = rec.get("invite_id", "")
        if kind == "password":
            return bool(_load_dev_password_hash())
        if kind == "invite":
            # Immediately honor --revoke / --revoke-all even for already-redeemed sessions!
            now_wall = time.time()
            for inv in _load_dev_invites():
                if inv.get("id") == invite_id and float(inv.get("expires_at", 0)) > now_wall:
                    return True
            with _DEV_LOCK:
                _DEV_SESSIONS.pop(tok, None)
            return False
        return False

    def _mint_dev_cookie(self, kind: str, invite_id: str = "") -> str:
        token = secrets.token_urlsafe(32)
        with _DEV_LOCK:
            _prune_dev_state(time.monotonic())
            if len(_DEV_SESSIONS) >= 32:
                oldest = min(_DEV_SESSIONS, key=lambda k: _DEV_SESSIONS[k]["exp"])
                _DEV_SESSIONS.pop(oldest, None)
            _DEV_SESSIONS[token] = {
                "exp": time.monotonic() + DEV_SESSION_TTL_SECS,
                "kind": kind,
                "invite_id": invite_id,
            }
        secure_attr = "; Secure" if self._is_https_request() else ""
        return (
            f"{DEV_COOKIE_NAME}={token}; Path=/; HttpOnly; SameSite=Strict; "
            f"Max-Age={DEV_SESSION_TTL_SECS}{secure_attr}"
        )

    def can_stream_gamedata(self) -> bool:
        return bool(type(self).serve_gamedata) or self.has_valid_dev_session()

    def translate_game(self, path: str):
        """Resolve /GameData/<rel> inside the dump, or None when disabled, escaped, or missing."""
        if not self.can_stream_gamedata():
            return None
        base_game = type(self).game_dir
        rel = urllib.parse.unquote(path[len("/GameData/"):])
        target = (base_game / rel).resolve()
        if not str(target).startswith(str(base_game)):
            return None
        if target.is_file():
            return target
        # Case-insensitive fallback matching Windows/SWBF2 path semantics.
        cur = base_game
        for part in pathlib.PurePosixPath(rel).parts:
            if part in ("", "."):
                continue
            if part == "..":
                return None
            nxt = cur / part
            if nxt.exists():
                cur = nxt
                continue
            low = part.lower()
            hit = None
            if cur.is_dir():
                for child in cur.iterdir():
                    if child.name.lower() == low:
                        hit = child
                        break
            if hit is None:
                return None
            cur = hit
        cur = cur.resolve()
        if not str(cur).startswith(str(base_game)) or not cur.is_file():
            return None
        return cur

    def redirect_to_https(self):
        """302 a direct plain-HTTP request made to a LAN IP, or None if behind a reverse proxy or local.

        When swbf2o.com is served behind Cloudflare Tunnel, Caddy, or Nginx, TLS is terminated
        upstream on port 443 and requests arrive here over loopback HTTP with X-Forwarded-Proto /
        CF-Visitor / CF-Connecting-IP headers — those must NEVER redirect to :8443.
        """
        port = type(self).tls_redirect_port
        if not port:
            return None
        if self.headers.get("Upgrade", "").lower() == "websocket":
            return None
        if (
            self.headers.get("X-Forwarded-Proto")
            or self.headers.get("CF-Visitor")
            or self.headers.get("CF-Connecting-IP")
            or self.headers.get("X-Forwarded-For")
        ):
            return None
        host = (self.headers.get("Host") or "").split(":")[0].strip().strip("[]")
        if not host or host == "localhost" or host == "::1" or host.startswith("127."):
            return None
        return f"https://{host}:{port}{self.path}"

    @staticmethod
    def is_allowed_static_path(path: str) -> bool:
        """Strict allowlist so private repo files (.git, build/lan_tls/key.pem, tools, data, logs)
        are never exposed when serving swbf2o.com on the public internet."""
        unquoted = urllib.parse.unquote(path)
        if ".." in unquoted.split("/") or "\x00" in unquoted:
            return False
        if unquoted in ("/index.html", "/robots.txt", "/sitemap.xml"):
            return True
        if unquoted.startswith(("/browser/", "/build/browser/", "/build/class_data/")):
            parts = [p for p in unquoted.split("/") if p]
            if any(p.startswith(".") for p in parts):
                return False
            return True
        return False

    def list_directory(self, path):
        self.send_error(404, "not found")
        return None

    def do_GET(self):
        parsed = urllib.parse.urlparse(self.path)
        path = parsed.path

        target = self.redirect_to_https()
        if target is not None:
            self.send_response(302)
            self.send_header("Location", target)
            self.send_header("Content-Length", "0")
            self.end_headers()
            self.log_request(302, 0)
            self.close_connection = True
            return None

        if path.startswith("/ws") or self.headers.get("Upgrade", "").lower() == "websocket":
            if (
                self.headers.get("Upgrade", "").lower() != "websocket"
                or "upgrade" not in (self.headers.get("Connection") or "").lower()
                or not (self.headers.get("Sec-WebSocket-Key") or "").strip()
            ):
                self.close_connection = True
                self.send_error(426, "Upgrade Required: WebSocket handshake headers missing")
                return None
            return self.proxy_websocket()

        if path == "/api/server-config":
            return self.serve_server_config()

        if path.startswith("/api/"):
            return self.proxy_api()

        if path.startswith("/GameData/"):
            if not self.can_stream_gamedata():
                self.send_error(
                    404,
                    "GameData hosting disabled (Bring-Your-Own-Copy mode). Select your local GameData folder in the browser.",
                )
                return None
            return self.serve_game_file(self.translate_game(path))

        if path == "/browser/gamedata-manifest.json" and not self.can_stream_gamedata():
            self.send_error(404, "GameData manifest disabled in BYOC mode")
            return None

        if path == "/favicon.ico":
            self.path = "/browser/favicon.svg"
            path = "/browser/favicon.svg"
        elif path in ("/play", "/play/", "/", ""):
            q_pairs = urllib.parse.parse_qsl(parsed.query, keep_blank_values=True)
            invite_tok = None
            kept_pairs = []
            for k, v in q_pairs:
                if k == "invite" and invite_tok is None:
                    invite_tok = v.strip()
                else:
                    kept_pairs.append((k, v))
            if invite_tok is not None and self.command == "GET":
                ip = self._client_ip()
                now = time.monotonic()
                cookie_hdr = None
                with _DEV_LOCK:
                    _prune_dev_state(now)
                    fails = _DEV_FAILS.get(ip, [])
                    rate_limited = len(fails) >= DEV_RATE_MAX_FAILS
                    inv_id = None if rate_limited else _redeem_dev_invite(invite_tok)
                    if inv_id is None and not rate_limited:
                        _DEV_FAILS.setdefault(ip, []).append(now)
                    elif inv_id is not None:
                        _DEV_FAILS.pop(ip, None)
                if inv_id is not None:
                    cookie_hdr = self._mint_dev_cookie("invite", inv_id)
                clean_q = urllib.parse.urlencode(kept_pairs)
                dest = f"/play?{clean_q}" if clean_q else "/play"
                self.send_response(302)
                self.send_header("Location", dest)
                if cookie_hdr:
                    self.send_header("Set-Cookie", cookie_hdr)
                self.send_header("Content-Length", "0")
                self.end_headers()
                return None

            q = f"?{parsed.query}" if parsed.query else ""
            if path in ("/play", "/play/"):
                self.path = f"/browser/play.html{q}"
                path = "/browser/play.html"
            elif "mission=" in parsed.query:
                self.path = f"/browser/index.html{q}"
                path = "/browser/index.html"
            elif "join=" in parsed.query or "host=" in parsed.query:
                self.path = f"/browser/play.html{q}"
                path = "/browser/play.html"
            else:
                self.path = f"/index.html{q}"
                path = "/index.html"

        if not self.is_allowed_static_path(path):
            self.send_error(404, "not found")
            return None
        if not (ROOT / urllib.parse.unquote(path).lstrip("/")).is_file():
            self.send_error(404, "not found")
            return None
        return super().do_GET()

    do_HEAD = do_GET

    def _send_json(self, status: int, payload: dict, extra_headers=None):
        body = json.dumps(payload).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        if extra_headers:
            for k, v in extra_headers:
                self.send_header(k, v)
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(body)

    def _read_post_body(self, max_bytes: int = 2048):
        te = (self.headers.get("Transfer-Encoding") or "").lower()
        if "chunked" in te:
            out = bytearray()
            while True:
                line = self.rfile.readline(64)
                if not line:
                    return None
                try:
                    sz = int(line.split(b";", 1)[0].strip(), 16)
                except ValueError:
                    return None
                if sz == 0:
                    self.rfile.readline(64)
                    break
                if len(out) + sz > max_bytes:
                    self.close_connection = True
                    return None
                out.extend(self.rfile.read(sz))
                self.rfile.readline(64)
            return bytes(out)
        try:
            cl = int(self.headers.get("Content-Length") or "0")
        except ValueError:
            self.close_connection = True
            return None
        if cl <= 0 or cl > max_bytes:
            self.close_connection = True
            return None
        return self.rfile.read(cl)

    def do_POST(self):
        parsed = urllib.parse.urlparse(self.path)
        path = parsed.path

        if path == "/api/dev-unlock":
            stored_hash = _load_dev_password_hash()
            if not stored_hash:
                self.close_connection = True
                return self._send_json(404, {"ok": False, "error": "Developer unlock is not enabled on this server."})

            ip = self._client_ip()
            now = time.monotonic()
            with _DEV_LOCK:
                _prune_dev_state(now)
                fails = _DEV_FAILS.get(ip, [])
                if len(fails) >= DEV_RATE_MAX_FAILS:
                    self.close_connection = True
                    return self._send_json(429, {"ok": False, "error": "Too many failed attempts. Try again in 10 minutes."})

            raw = self._read_post_body(2048)
            if raw is None:
                return self._send_json(400, {"ok": False, "error": "Invalid request payload."})
            try:
                data = json.loads(raw.decode("utf-8"))
                pw = str(data.get("password") or "")
            except Exception:
                return self._send_json(400, {"ok": False, "error": "Invalid JSON body."})

            if not _verify_dev_password(pw, stored_hash):
                with _DEV_LOCK:
                    _DEV_FAILS.setdefault(ip, []).append(time.monotonic())
                return self._send_json(401, {"ok": False, "error": "Invalid developer password."})

            with _DEV_LOCK:
                _DEV_FAILS.pop(ip, None)
            cookie_hdr = self._mint_dev_cookie("password")
            return self._send_json(
                200,
                {"ok": True, "gamedataHosted": True, "devUnlocked": True},
                extra_headers=[("Set-Cookie", cookie_hdr)],
            )

        if path == "/api/dev-logout":
            tok = self._dev_cookie_token()
            if tok:
                with _DEV_LOCK:
                    _DEV_SESSIONS.pop(tok, None)
            secure_attr = "; Secure" if self._is_https_request() else ""
            cookie_hdr = f"{DEV_COOKIE_NAME}=; Path=/; HttpOnly; SameSite=Strict; Max-Age=0{secure_attr}"
            return self._send_json(
                200,
                {"ok": True, "devUnlocked": False},
                extra_headers=[("Set-Cookie", cookie_hdr)],
            )

        self.send_error(404, "not found")
        return None

    def serve_server_config(self):
        dev_unlocked = self.has_valid_dev_session()
        hosted = bool(type(self).serve_gamedata) or dev_unlocked
        if dev_unlocked:
            byoc_prompt = False
            mode = "dev"
        elif type(self).serve_gamedata:
            byoc_prompt = bool(type(self).byoc_prompt)
            mode = "byoc-optional" if byoc_prompt else "hosted"
        else:
            byoc_prompt = True
            mode = "byoc"
        return self._send_json(
            200,
            {
                "gamedataHosted": hosted,
                "byocPrompt": byoc_prompt,
                "devUnlocked": dev_unlocked,
                "mode": mode,
            },
        )

    def proxy_api(self):
        url = f"http://127.0.0.1:{self.mp_port}{self.path}"
        try:
            with urllib.request.urlopen(url, timeout=2.0) as resp:
                body = resp.read()
                self.send_response(resp.status)
                self.send_header("Content-Type", resp.headers.get("Content-Type", "application/json"))
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                if self.command != "HEAD":
                    self.wfile.write(body)
        except Exception as exc:
            body = f'{{"error":"multiplayer server unavailable: {exc}","rooms":[]}}'.encode("utf-8")
            self.send_response(502)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            if self.command != "HEAD":
                self.wfile.write(body)
        return None

    def proxy_websocket(self):
        try:
            backend = socket.create_connection(("127.0.0.1", self.mp_port), timeout=5.0)
            backend.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            backend.settimeout(None)
        except OSError:
            self.send_error(502, "multiplayer signaling server unavailable")
            return None

        client = self.connection
        try:
            client.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        except OSError:
            pass
        client.settimeout(None)

        req_lines = [f"GET {self.path} HTTP/1.1"]
        for k, v in self.headers.items():
            req_lines.append(f"{k}: {v}")
        req_lines.extend(["", ""])
        try:
            backend.sendall("\r\n".join(req_lines).encode("latin1"))
        except OSError:
            backend.close()
            self.close_connection = True
            return None

        # Read the backend's HTTP response headers before opening a raw pipe.
        # If the backend refuses the WebSocket upgrade (status != 101), we MUST NOT
        # leave a raw bidirectional pipe open on a pooled keep-alive connection from
        # Cloudflare/reverse proxies, or subsequent HTTP requests on that socket would
        # be misrouted straight to the multiplayer server (returning 404 for the whole site!).
        backend.settimeout(5.0)
        head_buf = bytearray()
        try:
            while b"\r\n\r\n" not in head_buf and len(head_buf) < 16384:
                chunk = backend.recv(4096)
                if not chunk:
                    break
                head_buf.extend(chunk)
        except OSError:
            backend.close()
            self.close_connection = True
            self.send_error(502, "multiplayer signaling handshake failed")
            return None

        sep = head_buf.find(b"\r\n\r\n")
        status_line = bytes(head_buf[:head_buf.find(b"\r\n")]) if sep != -1 else b""
        if sep == -1 or not status_line.startswith(b"HTTP/1.1 101"):
            backend.close()
            self.close_connection = True
            self.send_error(400, "WebSocket upgrade rejected by signaling server")
            return None

        backend.settimeout(None)
        try:
            client.sendall(bytes(head_buf))
        except OSError:
            backend.close()
            self.close_connection = True
            return None

        def pipe(src, dst):
            try:
                while True:
                    chunk = src.recv(65536)
                    if not chunk:
                        break
                    dst.sendall(chunk)
            except Exception:
                pass
            finally:
                try:
                    dst.shutdown(socket.SHUT_WR)
                except Exception:
                    pass

        t1 = threading.Thread(target=pipe, args=(client, backend), daemon=True)
        t2 = threading.Thread(target=pipe, args=(backend, client), daemon=True)
        t1.start()
        t2.start()
        t1.join()
        t2.join()
        try:
            backend.close()
        except Exception:
            pass
        self.close_connection = True
        return None

    def serve_game_file(self, target):
        if target is None:
            self.send_error(404, "no such game file")
            return None
        st = target.stat()
        size = st.st_size
        etag = f'"{size:x}-{int(st.st_mtime):x}"'
        last_mod = self.date_time_string(st.st_mtime)

        # Byte ranges. This exists because the interesting win is reading one texture out of a
        # 60 MB container without transferring the container: a texture is a contiguous span of
        # bytes, so a range request plus the container manifest is enough. Advertising
        # Accept-Ranges without obeying Range would tell a browser or CDN to trust a capability
        # this server does not have.
        start, end, partial = 0, size - 1, False
        spec = self.headers.get("Range", "")
        if spec.startswith("bytes="):
            lo, _, hi = spec[6:].split(",")[0].strip().partition("-")
            try:
                if size == 0:
                    raise ValueError("empty file")
                if lo == "":
                    suffix = int(hi)
                    if suffix <= 0:
                        raise ValueError("zero-length suffix range")
                    start, end = max(0, size - suffix), size - 1
                else:
                    start = int(lo)
                    end = size - 1 if hi == "" else min(int(hi), size - 1)
                if start >= size or start > end:
                    raise ValueError("unsatisfiable")
                partial = True
            except ValueError:
                self.send_error(416, "range cannot be satisfied")
                return None

        self.send_response(206 if partial else 200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(end - start + 1) if size else "0")
        self.send_header("x-total-size", str(size))
        self.send_header("Accept-Ranges", "bytes")
        self.send_header("ETag", etag)
        self.send_header("Last-Modified", last_mod)
        if partial:
            self.send_header("Content-Range", f"bytes {start}-{end}/{size}")
        self.end_headers()
        if self.command == "HEAD":
            return None
        try:
            with target.open("rb") as fh:
                if start:
                    fh.seek(start)
                left = (end - start + 1) if size else 0
                while left > 0:
                    block = fh.read(min(1 << 20, left))
                    if not block:
                        break
                    self.wfile.write(block)
                    left -= len(block)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def end_headers(self):
        # Cross-origin isolation headers required for SharedArrayBuffer (audio/input rings)
        # and high-resolution timers in the Dedicated Worker architecture.
        self.send_header("Cross-Origin-Opener-Policy", "same-origin")
        self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
        self.send_header("Cross-Origin-Resource-Policy", "same-origin")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("Referrer-Policy", "strict-origin-when-cross-origin")
        req_path = urllib.parse.urlparse(self.path).path
        if req_path.startswith("/build/browser/"):
            try:
                fp = ROOT / urllib.parse.unquote(req_path).lstrip("/")
                if fp.is_file():
                    self.send_header("x-total-size", str(fp.stat().st_size))
            except OSError:
                pass
        if req_path.startswith("/GameData/") or req_path == "/browser/gamedata-manifest.json":
            if type(self).serve_gamedata:
                self.send_header("Cache-Control", "public, max-age=2592000, immutable")
            else:
                # Private developer session unlock: NEVER allow Cloudflare/CDN to cache for public visitors!
                self.send_header("Cache-Control", "private, no-store, no-cache, must-revalidate")
                self.send_header("Vary", "Cookie")
        elif req_path.startswith("/api/"):
            self.send_header("Cache-Control", "private, no-store")
            self.send_header("Vary", "Cookie")
        elif req_path.startswith("/browser/assets/") or req_path in ("/browser/favicon.svg", "/favicon.ico"):
            self.send_header("Cache-Control", "public, max-age=86400")
        else:
            self.send_header("Cache-Control", "no-store")
        super().end_headers()

    def log_message(self, fmt, *args):
        sys.stdout.write("  " + (fmt % args) + "\n")
        sys.stdout.flush()


class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


class TlsHandler(Handler):
    """The secure origin never redirects: it is already where a LAN client needs to be."""

    tls_redirect_port = 0


class TLSServer(Server):
    def __init__(self, server_address, RequestHandlerClass, ssl_ctx):
        self.ssl_ctx = ssl_ctx
        super().__init__(server_address, RequestHandlerClass)

    def finish_request(self, request, client_address):
        try:
            tls_conn = self.ssl_ctx.wrap_socket(request, server_side=True)
        except (ssl.SSLError, OSError):
            return
        super().finish_request(tls_conn, client_address)


def ensure_lan_cert(ips):
    """Generate a self-signed certificate covering all LAN IPs + 127.0.0.1 + localhost."""
    tls_dir = ROOT / "build" / "lan_tls"
    tls_dir.mkdir(parents=True, exist_ok=True)
    cert_path = tls_dir / "cert.pem"
    key_path = tls_dir / "key.pem"
    ips_path = tls_dir / "ips.txt"

    unique_ips = []
    for ip in list(ips) + ["127.0.0.1"]:
        if ip and ip not in unique_ips:
            unique_ips.append(ip)
    san = ",".join([f"IP:{ip}" for ip in unique_ips] + ["DNS:localhost"])

    if cert_path.exists() and key_path.exists() and ips_path.exists():
        try:
            if ips_path.read_text(encoding="utf-8").strip() == san:
                return cert_path, key_path
        except OSError:
            pass

    openssl_bin = shutil.which("openssl") or "openssl"
    subprocess.run(
        [
            openssl_bin,
            "req",
            "-x509",
            "-newkey",
            "rsa:2048",
            "-nodes",
            "-keyout",
            str(key_path),
            "-out",
            str(cert_path),
            "-days",
            "3650",
            "-subj",
            "/CN=SWBF2 LAN Server",
            "-addext",
            f"subjectAltName={san}",
        ],
        check=True,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    ips_path.write_text(san + "\n", encoding="utf-8")
    return cert_path, key_path


def ensure_mp_server(mp_port, host="0.0.0.0"):
    """Start tools/multiplayer_server.mjs in the background if mp_port is not already listening."""
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
        probe.settimeout(0.25)
        if probe.connect_ex(("127.0.0.1", mp_port)) == 0:
            return None
    node_bin = ROOT / "vendor/emsdk/node/24.19.0_64bit/bin/node"
    if not node_bin.exists():
        node_bin = shutil.which("node")
    mp_script = ROOT / "tools/multiplayer_server.mjs"
    if not node_bin or not mp_script.exists():
        return None
    try:
        return subprocess.Popen(
            [str(node_bin), str(mp_script), str(mp_port), "--host", host],
            cwd=str(ROOT),
            stdout=sys.stdout,
            stderr=sys.stderr,
        )
    except OSError:
        return None


def interfaces():
    """Every UP non-loopback interface with an IPv4 address, as (name, ip, netmask, broadcast).

    Reads the addresses with SIOCGIFADDR rather than parsing `ip` output, so the printed URLs are
    the machine's actual addresses, including on hosts without `ip` installed.
    """
    found = []
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
        for index, name in socket.if_nameindex():
            if name == "lo":
                continue

            def addr_of(what):
                packed = struct.pack("256s", name.encode("utf-8")) + struct.pack(
                    "=HH", socket.AF_INET, 0
                ) + bytes(14)
                try:
                    reply = fcntl.ioctl(probe.fileno(), what, packed)
                except OSError:
                    return None
                return socket.inet_ntoa(reply[20:24])

            ip = addr_of(SIOCGIFADDR)
            if not ip:
                continue
            found.append(
                (name, ip, addr_of(SIOCGIFNETMASK) or "", addr_of(SIOCGIFBRDADDR) or "")
            )
    return found


def default_route_interface():
    """Name of the interface this machine reaches the internet through.

    connect() on a UDP socket sends nothing and needs no peer; it only makes the kernel pick a
    source address for that destination, which getsockname() then reports.
    """
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
        try:
            probe.connect(("198.51.100.1", 80))  # TEST-NET-2: never connected to, only routed
            local = probe.getsockname()[0]
        except OSError:
            return None
    for name, ip, _, _ in interfaces():
        if ip == local:
            return name
    return None


def reorder_for_printing(found, preferred):
    """Real LAN first, then everything else (docker and VM bridges sort last and get marked).

    A machine with Docker running has several routable IPv4 addresses, and the first one an
    operator tries is the one that should be the actual network address.
    """
    def rank(entry):
        name = entry[0]
        if preferred and name == preferred:
            return (0, name)
        if name.startswith(("docker", "br-", "virbr", "veth", "lxc", "wg", "tun")):
            return (2, name)
        return (1, name)

    return sorted(found, key=rank)


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[1])
    ap.add_argument("port", nargs="?", type=int, default=8123)
    ap.add_argument(
        "--host",
        default="0.0.0.0",
        help="address to bind (default 0.0.0.0 = reachable from the local network; "
        "use --host 127.0.0.1 to keep it to this machine)",
    )
    ap.add_argument(
        "--tls-port",
        type=int,
        default=None,
        help="HTTPS port for LAN Secure Context (WebGPU/SharedArrayBuffer/OPFS). "
        "Defaults to 8443 when serving on port 8123 and 0.0.0.0; pass 0 to disable.",
    )
    ap.add_argument(
        "--mp-port",
        type=int,
        default=MP_DEFAULT_PORT,
        help="Local multiplayer signaling server port to proxy /api/* and /ws to (default 8133).",
    )
    ap.add_argument(
        "--serve-gamedata",
        action="store_true",
        default=SERVE_GAMEDATA_ENV,
        help="Serve local GameData files over /GameData/ for private LAN or dev testing "
        "(disabled by default — public deployments run in Bring-Your-Own-Copy mode).",
    )
    ap.add_argument(
        "--byoc-prompt",
        action="store_true",
        default=BYOC_PROMPT_ENV,
        help="Show the Bring-Your-Own-Copy folder prompt on /play even when --serve-gamedata is enabled, "
        "allowing players to either load local files or click through to GameData streaming.",
    )
    ap.add_argument(
        "--byoc-only",
        action="store_true",
        default=False,
        help="Force strict Bring-Your-Own-Copy mode (ignore any local build/server_mode.json or env defaults).",
    )
    ap.add_argument(
        "--gamedata-dir",
        default=None,
        help="Path to local GameData/data/_lvl_pc directory when --serve-gamedata is enabled.",
    )
    args = ap.parse_args()

    os.chdir(ROOT)
    Handler.mp_port = args.mp_port

    serve_gd = False if args.byoc_only else bool(args.serve_gamedata)
    byoc_prompt = True if (args.byoc_only or not serve_gd) else bool(args.byoc_prompt)

    Handler.serve_gamedata = serve_gd
    Handler.byoc_prompt = byoc_prompt
    if args.gamedata_dir:
        Handler.game_dir = pathlib.Path(args.gamedata_dir).expanduser().resolve()
    else:
        Handler.game_dir = GAME
    all_interfaces = interfaces()
    preferred = default_route_interface()
    ordered = reorder_for_printing(all_interfaces, preferred)

    mp_proc = None
    if args.port == 8123:
        mp_proc = ensure_mp_server(args.mp_port, args.host)

    try:
        httpd = Server((args.host, args.port), Handler)
    except OSError as exc:
        print(f"cannot bind {args.host}:{args.port}: {exc}", file=sys.stderr)
        if args.host != "127.0.0.1":
            print("retrying on 127.0.0.1 only", file=sys.stderr)
            httpd = Server(("127.0.0.1", args.port), Handler)
        else:
            raise SystemExit(1)

    bound_host, bound_port = httpd.server_address[:2]

    tls_port = args.tls_port
    if tls_port is None:
        tls_port = 8443 if (bound_port == 8123 and bound_host in ("0.0.0.0", "::")) else 0

    tls_httpd = None
    bound_tls_port = 0
    if tls_port > 0:
        try:
            cert_path, key_path = ensure_lan_cert([ip for _, ip, _, _ in ordered])
            ssl_ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            ssl_ctx.load_cert_chain(certfile=str(cert_path), keyfile=str(key_path))
            tls_httpd = TLSServer((bound_host, tls_port), TlsHandler, ssl_ctx)
            bound_tls_port = tls_httpd.server_address[1]
            threading.Thread(target=tls_httpd.serve_forever, daemon=True).start()
        except Exception as exc:
            print(f"note: could not start HTTPS listener on port {tls_port}: {exc}", file=sys.stderr)

    # Only set once the listener is proven up, otherwise a client is sent to a port nothing holds.
    if bound_tls_port:
        Handler.tls_redirect_port = bound_tls_port

    print(f"root={ROOT}")
    if Handler.serve_gamedata:
        print(f"mode=hosted game={Handler.game_dir}")
    else:
        print("mode=byoc (Bring-Your-Own-Copy — /GameData/ hosting disabled)")
    print(f"listening on {bound_host}:{bound_port}" + (f" (and HTTPS {bound_host}:{bound_tls_port})" if bound_tls_port else ""))
    if bound_host in ("0.0.0.0", "::"):
        print("open one of these from any device on the same network:")
        for name, ip, _, _ in ordered:
            note = "" if name == preferred else "   (not the default route)"
            if bound_tls_port:
                print(f"    https://{ip}:{bound_tls_port}/   (LAN WebGPU + Multiplayer){note}")
            print(f"    http://{ip}:{bound_port}/{note}")
        print("    http://127.0.0.1:%d/   (this machine)" % bound_port)
        broadcasts = sorted({b for _, _, _, b in ordered if b})
        if broadcasts:
            print(f"    scan the segment: {' '.join(broadcasts)}")
        if Handler.serve_gamedata:
            print(
                "    note: --serve-gamedata is ON; keep private self-hosted GameData off public servers"
            )
    else:
        print(f"open http://127.0.0.1:{bound_port}/  (bound to {bound_host}, this machine only)")
    sys.stdout.flush()
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("stopping")
    finally:
        if tls_httpd:
            tls_httpd.server_close()
        httpd.server_close()
        if mp_proc:
            try:
                mp_proc.terminate()
            except Exception:
                pass

