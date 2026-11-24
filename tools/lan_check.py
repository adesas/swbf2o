#!/usr/bin/env python3
"""Prove whether this machine really serves the LAN, instead of asserting it.

"is it up for the LAN" and "it only works from localhost" look identical from a browser, and every
previous report of this in the project history turned out to be one of: (a) nothing listening, because
the server was started as a background job of a shell that then exited, (b) a LAN client on plain http
where the page cannot have WebGPU, or (c) the wrong address being tried (Docker bridge addresses sort
before the real one if you read them in interface order). This script checks each of those in turn and
says which one it is.

A curl from this machine to its own LAN IP is not proof: locally-destined traffic skips the ingress
path a real neighbour's packets take. So when `sudo -n` works and `ip netns` is available, the last
check repeats the request from a network namespace on a different subnet, arriving over a real
interface, which is the closest thing available to testing from the next desk.

  python3 tools/lan_check.py            # against the running server
  tools/lan_serve.sh start              # start it durably first
"""
import http.client
import re
import os
import pathlib
import shutil
import socket
import ssl
import subprocess
import sys
import urllib.parse

ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import serve  # noqa: E402  (module import is side-effect free: everything is under __main__)

HTTP_PORT = int(os.environ.get("SWBF2_HTTP_PORT", "8123"))
TLS_PORT = int(os.environ.get("SWBF2_TLS_PORT", "8443"))
MP_PORT = int(os.environ.get("SWBF2_MP_PORT", "8133"))

results = []
LAN_IP = None


def report(ok, label, detail=""):
    results.append((ok, label))
    print(f"  {'PASS' if ok is True else ('SKIP' if ok is None else 'FAIL')}  {label}" + (f"  - {detail}" if detail else ""))
    return ok


def tcp_open(ip, port, timeout=2.0):
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.settimeout(timeout)
        return s.connect_ex((ip, port)) == 0


def http_request(scheme, host, port, path, headers=None, timeout=6.0):
    """One request, no redirect following. Returns (status, headers, body)."""
    if scheme == "https":
        ctx = ssl._create_unverified_context()
        conn = http.client.HTTPSConnection(host, port, context=ctx, timeout=timeout)
    else:
        conn = http.client.HTTPConnection(host, port, timeout=timeout)
    try:
        conn.request("GET", path, headers=headers or {})
        resp = conn.getresponse()
        body = resp.read(65536)
        return resp.status, {k.lower(): v for k, v in resp.getheaders()}, body
    finally:
        conn.close()


def first_game_file():
    """Any real file under the GameData dump, found without walking the whole tree forever."""
    for dirpath, dirnames, filenames in os.walk(serve.GAME):
        for name in sorted(filenames):
            p = pathlib.Path(dirpath) / name
            try:
                if p.is_file() and p.stat().st_size > 64:
                    return p
            except OSError:
                continue
        if len(dirnames) > 40:
            dirnames[:] = dirnames[:40]
    return None


def main():
    lan = [ip for name, ip, _, _ in serve.reorder_for_printing(serve.interfaces(), serve.default_route_interface())
           if not name.startswith(("docker", "br-", "virbr", "veth", "lxc", "wg", "tun"))]
    if not lan:
        print("no non-loopback IPv4 interface found; this machine has no LAN address to serve")
        return 1
    global LAN_IP
    ip = LAN_IP = lan[0]
    print(f"checking LAN serving on {ip} (http {HTTP_PORT}, https {TLS_PORT}, mp {MP_PORT})")
    others = [x for x in lan[1:]]
    if others:
        print(f"  other addresses seen: {', '.join(others)} - only {ip} is the one your neighbours use")

    print("\nlisteners")
    # Connecting to the machine's own LAN address rather than 127.0.0.1 is what distinguishes a
    # 0.0.0.0 bind from a loopback-only bind, which is exactly the failure mode being diagnosed.
    http_up = tcp_open(ip, HTTP_PORT)
    report(http_up, f"http {ip}:{HTTP_PORT} accepts (not loopback-bound)", "" if http_up else "nothing listening, or bound to 127.0.0.1: run tools/lan_serve.sh start")
    tls_up = tcp_open(ip, TLS_PORT)
    report(tls_up, f"https {ip}:{TLS_PORT} accepts", "" if tls_up else "no Secure Context for LAN devices: they cannot get WebGPU/SharedArrayBuffer/OPFS over plain http")
    mp_up = tcp_open("127.0.0.1", MP_PORT)
    report(mp_up, f"multiplayer signaling 127.0.0.1:{MP_PORT} accepts", "" if mp_up else "/api and /ws will 502")

    if not http_up:
        print("\nnothing is serving on the LAN address; that alone explains 'other computers cannot reach it'")
        return finish()

    print("\nplain http on the LAN address")
    st, hdr, _ = http_request("http", ip, HTTP_PORT, "/")
    loc = hdr.get("location", "")
    report(st == 302 and loc.startswith(f"https://{ip}:{TLS_PORT}/"),
           "http redirect to https for a LAN Host header", f"got {st} Location={loc or 'none'}")
    st, hdr, _ = http_request("http", "127.0.0.1", HTTP_PORT, "/")
    report(st == 200, "localhost http is NOT redirected (this machine's workflow unchanged)", f"got {st}")

    if tls_up:
        print("\nhttps on the LAN address")
        st, hdr, body = http_request("https", ip, TLS_PORT, "/")
        coi = hdr.get("cross-origin-opener-policy") == "same-origin" and hdr.get("cross-origin-embedder-policy") == "require-corp"
        report(st == 200 and coi, "https / serves the page with COOP/COEP (SharedArrayBuffer)", f"status {st}, coop/coep {'present' if coi else 'MISSING'}")
        # getpeercert() returns nothing at all on an unverified connection, so read the certificate
        # itself and ask openssl for the SAN - otherwise this check silently passes on an empty SAN.
        san = ""
        try:
            pem = ssl.get_server_certificate((ip, TLS_PORT))
            parsed = subprocess.run(["openssl", "x509", "-noout", "-ext", "subjectAltName"],
                                    input=pem, capture_output=True, text=True, timeout=10)
            text = parsed.stdout + parsed.stderr
            found = re.findall(r"(?:IP Address:|IP:)\s*([0-9][0-9a-fA-F:.]*)", text)
            san = " ".join(found)
        except Exception as exc:  # noqa: BLE001 - a diagnostic must not die on a sub-step
            san = f"could not read certificate: {exc}"
        report(ip in san.split(), "certificate SAN covers this LAN address", f"SAN: {san or text.strip()[:80]}")
        ctx = ssl._create_unverified_context()
        st, hdr, body = http_request("https", ip, TLS_PORT, "/api/health")
        report(st == 200 and b'"ok":true' in body, "/api/health proxied to the multiplayer server", f"{st} {body[:80].decode('latin1')}")

        # A websocket upgrade is the part a hand-written proxy most often gets wrong.
        key = "dGhlIHNhbXBsZSBub25jZQ=="
        req = (f"GET /ws HTTP/1.1\r\nHost: {ip}:{TLS_PORT}\r\nUpgrade: websocket\r\n"
               f"Connection: Upgrade\r\nSec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n")
        greeting = ""
        try:
            with socket.create_connection((ip, TLS_PORT), timeout=6) as raw:
                with ctx.wrap_socket(raw, server_hostname=ip) as tls:
                    tls.sendall(req.encode())
                    greeting = tls.recv(4096).decode("latin1", "replace")
        except OSError as exc:
            greeting = f"error {exc}"
        report(greeting.startswith("HTTP/1.1 101"), "/ws upgrade proxied to the multiplayer server", greeting.splitlines()[0] if greeting else "no response")

    print("\ngame data")
    f = first_game_file()
    if f is None:
        report(None, "GameData range request", f"no files under {serve.GAME}")
    else:
        rel = f.relative_to(serve.GAME).as_posix()
        st, hdr, body = http_request("https" if tls_up else "http", ip, TLS_PORT if tls_up else HTTP_PORT,
                                     "/GameData/" + urllib.parse.quote(rel), {"Range": "bytes=0-15"})
        ok = st == 206 and hdr.get("content-range", "").startswith("bytes 0-15/") and len(body) == 16
        report(ok, "range request inside a GameData container", f"{rel}: {st} {hdr.get('content-range', 'no Content-Range')}")

    print("\nforeign client (namespace on a different subnet, arriving over a real interface)")
    report(run_namespace_check(ip), "a non-local address can complete the same requests", "see output above; skipped here is not evidence of a problem")

    return finish()


def run_namespace_check(ip):
    """Repeat the https request from inside a network namespace. None when not possible."""
    sudo = shutil.which("sudo")
    ipbin = shutil.which("ip")
    if not sudo or not ipbin or subprocess.run([sudo, "-n", "true"], capture_output=True).returncode != 0:
        print("    skipped: needs passwordless sudo; run 'sudo -n true' first to enable this check")
        return None
    ns, hv, cv = "swbf2-lan-check", "veth-lanchk-h", "veth-lanchk-c"
    cmds = [
        ["-n", "ip", "netns", "add", ns],
        ["-n", "ip", "link", "add", hv, "type", "veth", "peer", "name", cv],
        ["-n", "ip", "link", "set", cv, "netns", ns],
        ["-n", "ip", "addr", "add", "10.99.0.1/24", "dev", hv],
        ["-n", "ip", "link", "set", hv, "up"],
        ["-n", "ip", "netns", "exec", ns, "ip", "link", "set", "lo", "up"],
        ["-n", "ip", "netns", "exec", ns, "ip", "addr", "add", "10.99.0.2/24", "dev", cv],
        ["-n", "ip", "netns", "exec", ns, "ip", "link", "set", cv, "up"],
        ["-n", "ip", "netns", "exec", ns, "ip", "route", "add", "default", "via", "10.99.0.1"],
    ]
    started = False
    try:
        for c in cmds:
            r = subprocess.run([sudo] + c, capture_output=True, text=True)
            if r.returncode != 0 and "exists" not in (r.stderr + r.stdout).lower():
                print(f"    setup failed: ip {' '.join(c[1:])}: {(r.stderr or r.stdout).strip()[:120]}")
                return None
        started = True
        curl = shutil.which("curl")
        if curl:
            r = subprocess.run([sudo, "-n", "ip", "netns", "exec", ns, curl, "-sk", "-m", "6",
                                f"https://{ip}:{TLS_PORT}/api/health"], capture_output=True, text=True)
            out = (r.stdout or r.stderr).strip()
            print(f"    from 10.99.0.2: {out[:100]}")
            return r.returncode == 0 and '"ok":true' in out
        r = subprocess.run([sudo, "-n", "ip", "netns", "exec", ns, "sh", "-c",
                            f"exec 3<>/dev/tcp/{ip}/{TLS_PORT} && echo open"], capture_output=True, text=True)
        print(f"    from 10.99.0.2: {r.stdout.strip() or r.stderr.strip()[:100]}")
        return "open" in r.stdout
    finally:
        if started:
            subprocess.run([sudo, "-n", "ip", "netns", "exec", ns, "ip", "link", "del", cv], capture_output=True)
            subprocess.run([sudo, "-n", "ip", "netns", "del", ns], capture_output=True)
            subprocess.run([sudo, "-n", "ip", "link", "del", hv], capture_output=True)


def finish():
    bad = [label for ok, label in results if ok is False]
    print()
    if bad:
        print(f"{len(bad)} of {len(results)} checks FAILED:")
        for b in bad:
            print(f"  - {b}")
        print("\nmost likely cause, in the order this project has actually hit them:")
        print("  1. nothing listening: start it with tools/lan_serve.sh start (a plain `&` job dies with its shell)")
        print("  2. a LAN device on plain http: it must use https://<this ip>:8443/ and accept the certificate warning")
        print("  3. the wrong address: use the one on the interface facing your router, not a docker bridge")
        print("  4. only now look outward: the router (client isolation / guest network), not this machine")
        return 1
    print(f"all {len(results)} checks passed: this machine is serving the LAN")
    print(f"give your neighbours https://{LAN_IP}:{TLS_PORT}/ and expect one certificate warning")
    return 0


if __name__ == "__main__":
    sys.exit(main())
