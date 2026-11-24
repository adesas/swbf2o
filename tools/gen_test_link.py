#!/usr/bin/env python3
"""Generate, list, or revoke short-lived hashed tester invite links for tools/serve.py.

Only SHA-256 hashes of invite tokens are stored in build/dev_invites.json (gitignored, chmod 0600).
Plaintext tokens are printed once when generated and never stored on disk or in Git.

Usage:
  python3 tools/gen_test_link.py                          # Single-use link valid 24 hours
  python3 tools/gen_test_link.py --uses 4 --hours 12      # Up to 4 friends, valid 12 hours
  python3 tools/gen_test_link.py --join mos_eisley        # Link directly into a lobby
  python3 tools/gen_test_link.py --list                   # List active invite links
  python3 tools/gen_test_link.py --revoke <id>            # Revoke one link immediately
  python3 tools/gen_test_link.py --revoke-all             # Revoke all links immediately
"""
import argparse
import datetime
import hashlib
import json
import os
import pathlib
import secrets
import sys
import time
import urllib.parse

ROOT = pathlib.Path(__file__).resolve().parent.parent
INVITES_FILE = ROOT / "build" / "dev_invites.json"


def load_invites():
    if not INVITES_FILE.is_file():
        return []
    try:
        data = json.loads(INVITES_FILE.read_text(encoding="utf-8"))
        return data.get("invites", []) if isinstance(data, dict) else []
    except Exception:
        return []


def save_invites(invites):
    INVITES_FILE.parent.mkdir(parents=True, exist_ok=True)
    tmp = INVITES_FILE.with_suffix(".json.tmp")
    payload = json.dumps({"version": 1, "invites": invites}, indent=2) + "\n"
    fd = os.open(str(tmp), os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    try:
        os.write(fd, payload.encode("utf-8"))
    finally:
        os.close(fd)
    os.chmod(tmp, 0o600)
    os.replace(tmp, INVITES_FILE)
    os.chmod(INVITES_FILE, 0o600)


def fmt_ts(ts: float) -> str:
    return datetime.datetime.fromtimestamp(ts, tz=datetime.timezone.utc).strftime("%Y-%m-%d %H:%M UTC")


def main() -> int:
    ap = argparse.ArgumentParser(description="Manage hashed tester invite links.")
    ap.add_argument("--uses", type=int, default=1, help="Max redemptions for this link (default: 1).")
    ap.add_argument("--hours", type=float, default=24.0, help="Hours until link expires (default: 24).")
    ap.add_argument("--label", default="", help="Optional private note (e.g. friend's name).")
    ap.add_argument("--join", default="", help="Optional multiplayer lobby ID to append (&join=...).")
    ap.add_argument("--base-url", default="https://swbf2o.com", help="Public base URL (default: https://swbf2o.com).")
    ap.add_argument("--list", action="store_true", help="List existing tester invite links.")
    ap.add_argument("--revoke", default="", help="Revoke a specific invite by its short ID.")
    ap.add_argument("--revoke-all", action="store_true", help="Revoke and delete all tester invite links.")
    args = ap.parse_args()

    now = time.time()
    invites = load_invites()

    if args.revoke_all:
        if INVITES_FILE.exists():
            INVITES_FILE.unlink()
        print("Revoked all tester invite links (takes effect immediately).")
        return 0

    if args.revoke:
        before = len(invites)
        invites = [inv for inv in invites if inv.get("id") != args.revoke]
        if len(invites) == before:
            print(f"No invite found with id '{args.revoke}'.", file=sys.stderr)
            return 1
        save_invites(invites)
        print(f"Revoked invite '{args.revoke}' (takes effect immediately).")
        return 0

    if args.list:
        if not invites:
            print("No tester invite links configured.")
            return 0
        for inv in invites:
            exp = float(inv.get("expires_at", 0))
            uses = int(inv.get("uses", 0))
            max_uses = int(inv.get("max_uses", 1))
            status = "EXPIRED" if exp <= now else ("USED" if uses >= max_uses else "ACTIVE")
            lbl = f"  label={inv.get('label')!r}" if inv.get("label") else ""
            print(f"  [{status:7s}] id={inv.get('id')}  uses={uses}/{max_uses}  expires={fmt_ts(exp)}{lbl}")
        return 0

    # Prune already-expired invites before adding a new one
    invites = [inv for inv in invites if float(inv.get("expires_at", 0)) > now]

    token = secrets.token_urlsafe(24)  # 192 bits of entropy
    token_hash = hashlib.sha256(token.encode("utf-8")).hexdigest()
    short_id = secrets.token_hex(4)
    max_uses = max(1, int(args.uses))
    hours = max(0.1, float(args.hours))
    expires_at = now + hours * 3600.0

    invites.append({
        "id": short_id,
        "sha256": token_hash,
        "uses": 0,
        "max_uses": max_uses,
        "created_at": now,
        "expires_at": expires_at,
        "label": args.label.strip()[:64],
    })
    save_invites(invites)

    base = args.base_url.rstrip("/")
    qs = [("invite", token)]
    if args.join.strip():
        qs.append(("join", args.join.strip()))
    url = f"{base}/play?{urllib.parse.urlencode(qs)}"

    print(f"Created tester invite link (id={short_id}, uses=0/{max_uses}, expires={fmt_ts(expires_at)}):")
    print(f"\n  {url}\n")
    print(f"Active immediately on tools/serve.py (no restart required).")
    print(f"Revoke anytime with: python3 tools/gen_test_link.py --revoke {short_id}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
