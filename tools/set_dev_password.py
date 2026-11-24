#!/usr/bin/env python3
"""Set, generate, or disable the private developer-testing password hash for tools/serve.py.

Only a salted PBKDF2-SHA256 hash is stored in build/dev_password.hash (gitignored, chmod 0600).
The plaintext password is never stored on disk, in git, or in client JavaScript.

Usage:
  python3 tools/set_dev_password.py              # Prompt interactively for a complex password
  python3 tools/set_dev_password.py --generate   # Generate & set a strong 24-char complex password
  python3 tools/set_dev_password.py --disable    # Remove the hash and disable developer unlock
"""
import argparse
import getpass
import hashlib
import os
import pathlib
import secrets
import string
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
HASH_FILE = ROOT / "build" / "dev_password.hash"
ITERATIONS = 310_000


def generate_complex_password(length: int = 24) -> str:
    upper = string.ascii_uppercase
    lower = string.ascii_lowercase
    digits = string.digits
    symbols = "!@#$%^&*()-_=+[]{}:;,.?"
    alphabet = upper + lower + digits + symbols
    # Guarantee every character class is represented, then shuffle cryptographically.
    chars = [
        secrets.choice(upper),
        secrets.choice(upper),
        secrets.choice(lower),
        secrets.choice(lower),
        secrets.choice(digits),
        secrets.choice(digits),
        secrets.choice(symbols),
        secrets.choice(symbols),
    ] + [secrets.choice(alphabet) for _ in range(max(4, length - 8))]
    secrets.SystemRandom().shuffle(chars)
    return "".join(chars)


def hash_password(password: str) -> str:
    salt = secrets.token_bytes(16)
    dk = hashlib.pbkdf2_hmac("sha256", password.encode("utf-8"), salt, ITERATIONS)
    return f"pbkdf2_sha256${ITERATIONS}${salt.hex()}${dk.hex()}\n"


def main() -> int:
    ap = argparse.ArgumentParser(description="Manage the private developer unlock password hash.")
    ap.add_argument("--generate", action="store_true", help="Generate a random 24-character complex password.")
    ap.add_argument("--length", type=int, default=24, help="Generated password length (default 24, min 12).")
    ap.add_argument("--disable", action="store_true", help="Delete build/dev_password.hash and disable dev unlock.")
    args = ap.parse_args()

    if args.disable:
        if HASH_FILE.exists():
            HASH_FILE.unlink()
            print(f"Disabled developer unlock (removed {HASH_FILE}).")
        else:
            print("Developer unlock was already disabled.")
        return 0

    if args.generate:
        pw = generate_complex_password(max(12, args.length))
        generated = True
    else:
        pw = getpass.getpass("Enter new developer password (min 12 chars): ")
        if len(pw) < 12:
            print("Error: password must be at least 12 characters.", file=sys.stderr)
            return 1
        confirm = getpass.getpass("Confirm developer password: ")
        if pw != confirm:
            print("Error: passwords did not match.", file=sys.stderr)
            return 1
        generated = False

    HASH_FILE.parent.mkdir(parents=True, exist_ok=True)
    record = hash_password(pw)
    fd = os.open(str(HASH_FILE), os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    try:
        os.write(fd, record.encode("ascii"))
    finally:
        os.close(fd)
    os.chmod(HASH_FILE, 0o600)

    print(f"Saved salted PBKDF2-SHA256 hash to {HASH_FILE} (mode 0600, gitignored).")
    print("Active immediately on tools/serve.py (no server restart required).")
    if generated:
        print(f"\nGenerated developer password (save this now — it will not be shown again):\n  {pw}\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
