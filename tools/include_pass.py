#!/usr/bin/env python3
"""Purely additive include pass.

Reads clang diagnostics and, for every 'unknown type name X' / 'use of undeclared identifier X'
/ 'member access into incomplete type X' where some header in the tree defines X, inserts
    #include "<that header>"
next to the file's existing includes. It never deletes or rewrites anything, so it is safe to
iterate and easy to audit (git diff style: only added lines).

Why this is needed: the decompiled tree was built by MSVC with a precompiled header and lax
two-phase name lookup, so plenty of headers use types they never include. Clang is stricter.

Usage: tools/include_pass.py [rounds]
"""
import os, re, subprocess, sys, collections
from concurrent.futures import ThreadPoolExecutor

ROOT   = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC    = f"{ROOT}/build/src_fixed"
SHIM   = f"{ROOT}/spike/shim"
CLANG  = f"{ROOT}/vendor/emsdk/upstream/bin/clang"
SYSROOT= f"{ROOT}/vendor/emsdk/upstream/emscripten/cache/sysroot"
EXCLUDE= ('Graphics/PC', 'Platform/Pc', 'PebbleFL/pc', 'SoundFL/source/win', 'Battlefront2/Source/pcMain.cpp', '/Steam/', 'lglcd')
FLAGS  = ["--sysroot=" + SYSROOT, "-target", "wasm32-unknown-emscripten", "-fms-extensions", "-O0",
          "-Wno-ignored-attributes", "-Wno-deprecated-non-prototype", "-Wno-invalid-offsetof",
          "-Wno-writable-strings", "-Wno-implicit-exception-spec-mismatch",
          "-Wno-address-of-temporary", "-Wno-incompatible-pointer-types",
          "-include", f"{SHIM}/swbf2_prefix.h"]
INC    = ["-I" + SHIM, "-I" + SRC, "-I" + SRC + "/LuaFL/include", "-I" + SRC + "/LuaFL/src"]

ERR_PATTERNS = [
    r"member access into incomplete type '([A-Za-z_][A-Za-z0-9_]*)'",
    r"unknown type name '([A-Za-z_][A-Za-z0-9_]*)'",
    r"use of undeclared identifier '([A-Za-z_][A-Za-z0-9_]*)'",
    r"unknown class name '([A-Za-z_][A-Za-z0-9_]*)'",
    r"no template named '([A-Za-z_][A-Za-z0-9_]*)'",
    r"allocation of incomplete type '([A-Za-z_][A-Za-z0-9_]*)'",
    r"'([A-Za-z_][A-Za-z0-9_]*)':'\{'",
]

def headers():
    for d, _, fs in os.walk(SRC):
        for f in fs:
            if f.endswith('.h'):
                yield os.path.join(d, f)

def sources():
    for d, _, fs in os.walk(SRC):
        for f in fs:
            if f.endswith(('.c', '.cpp')):
                p = os.path.join(d, f)
                if not any(x in p for x in EXCLUDE):
                    yield p

def defs_map():
    """class/struct/enum/union/template name -> header that defines it (first hit)."""
    d = {}
    for h in headers():
        t = open(h, encoding='utf-8', errors='replace').read()
        rel = os.path.relpath(h, SRC).replace(os.sep, '/')
        # a definition REQUIRES a body brace - matching `class X;` here is what previously
        # made the pass insert the wrong header
        base = r'^[ \t]*(?:template\s*<[^>]*>[ \t]*)?(?:class|struct|enum|union)[ \t]+([A-Za-z_][A-Za-z0-9_]*)[ \t]*(?::[^;{\n]*)?[ \t]*'
        pats = [base + r'\{', base + r'\n[ \t]*\{']
        for pat in pats:
            for m in re.finditer(pat, t, re.M):
                d.setdefault(m.group(1), rel)
    return d

def insert_include(path, header):
    cur = open(path, encoding='utf-8', errors='replace').read()
    inc = f'#include "{header}"'
    if inc in cur or f'#include <{header}>' in cur:
        return False
    m = list(re.finditer(r'^[ \t]*#include\s+[<"][^">"]+[">]', cur, re.M))
    if m:
        pos = m[-1].end()
        cur = cur[:pos] + "\n" + inc + cur[pos:]
    else:
        m2 = re.search(r'#pragma once\s*\n', cur)
        pos = m2.end() if m2 else 0
        cur = cur[:pos] + "\n" + inc + cur[pos:]
    open(path, 'w', encoding='utf-8').write(cur)
    return True

def main():
    rounds = int(sys.argv[1]) if len(sys.argv) > 1 else 3
    for r in range(rounds):
        d = defs_map()
        ok = 0
        added = collections.Counter()
        paths = list(sources())
        def scan_one(p):
            return subprocess.run([CLANG] + FLAGS + INC + ["-fsyntax-only", p],
                                  capture_output=True, text=True).stderr
        with ThreadPoolExecutor(max_workers=8) as ex:
            errs = list(ex.map(scan_one, paths))
        for err in errs:
            if not re.search(r'\berror:', err):
                ok += 1
                continue
            # repair at the ERROR SITE (usually a shared header), not at the translation
            # unit: one include added to a header fixes every file that includes it.
            for m in re.finditer(r'^(/?[^:\n]+?):(\d+):\d+: (?:fatal )?error: (.*?)(?:\n|$)', err, re.M):
                site, _, msg = m.group(1), m.group(2), m.group(3)
                if not site.startswith(SRC):
                    continue
                names = {n for pat in ERR_PATTERNS for n in re.findall(pat, msg)}
                for n in sorted(names):
                    t = d.get(n)
                    if not t or t == os.path.relpath(site, SRC).replace(os.sep, '/'):
                        continue
                    if insert_include(site, t):
                        added[n] += 1
        print(f"round {r+1}: syntax-clean files = {ok}   includes added = {sum(added.values())} "
              f"(top: {', '.join(f'{k}:{v}' for k, v in added.most_common(6))})")
        if not added:
            break

if __name__ == '__main__':
    main()
