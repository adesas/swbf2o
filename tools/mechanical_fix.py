#!/usr/bin/env python3
"""Mechanical, behaviour-preserving fixes so the decompiled tree compiles for wasm.

Writes to build/src_fixed (the upstream clone is left untouched). Only safe, mechanical
transformations are applied — everything else is a genuine decomp issue that needs a human.

  1. pool allocator signatures: operator new(unsigned int/long) -> operator new(size_t)
     clang requires size_t as the first parameter; allocation behaviour is unchanged.
  2. bare enum forward declarations (illegal in C++): enum X; -> enum X : int; and matching
     definitions get the same underlying type.
  3. forward class declarations are replaced with the real include ONLY when that cannot create
     an include cycle (cycles break template bases such as Factory<Entity, ...>).
"""
import os, re, sys, collections

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "vendor", "swbf2decomp", "src")
OUT = os.path.join(ROOT, "build", "src_fixed")

def main():
    files = []
    for d, _, fs in os.walk(SRC):
        for f in fs:
            if f.endswith(('.c', '.cpp', '.h')):
                files.append(os.path.join(d, f))

    def rel(p):
        return os.path.relpath(p, SRC).replace(os.sep, '/')

    texts = {p: open(p, encoding='utf-8', errors='replace').read() for p in files}

    # class -> header that defines it
    defs = {}
    for p, t in texts.items():
        for m in re.finditer(r'^\s*(?:class|struct)\s+([A-Za-z_][A-Za-z0-9_]*)\s*\{', t, re.M):
            if p.endswith('.h'):
                defs.setdefault(m.group(1), rel(p))

    # enum names used as forward references
    enum_names = set()
    for t in texts.values():
        for m in re.finditer(r'\benum\s+([A-Za-z_][A-Za-z0-9_]*)\s*;', t):
            enum_names.add(m.group(1))

    graph = {rel(p): {m.group(1) for m in re.finditer(r'#include\s+"([^"]+)"', t)} for p, t in texts.items()}
    def reaches(a, b, seen=None):
        seen = set() if seen is None else seen
        for inc in graph.get(a, ()):
            if inc == b or (inc not in seen and (seen.add(inc) or reaches(inc, b, seen))):
                return True
        return False

    counts = collections.Counter()
    os.makedirs(OUT, exist_ok=True)
    for p in files:
        text = texts[p]
        counts['operator_new_size_t'] += len(re.findall(r'operator new(?:\[\])?\((unsigned (?:int|long))\s', text))
        text = re.sub(r'operator new(?:\[\])?\((unsigned (?:int|long))\s', 'operator new(size_t ', text)

        for name in enum_names:
            if re.search(r'\benum\s+' + name + r'\s*;', text):
                counts['enum_forward_decl'] += 1
                text = re.sub(r'\benum\s+' + name + r'\s*;', 'enum ' + name + ' : int;', text)
            if re.search(r'\benum\s+' + name + r'\s*\{', text):
                counts['enum_definition_typed'] += 1
                text = re.sub(r'\benum\s+' + name + r'\s*\{', 'enum ' + name + ' : int {', text)

        r = rel(p)
        for name, target in defs.items():
            if target == r or reaches(target, r) or reaches(r, target):
                counts['forward_decl_skipped_cycle'] += 1
                continue
            if re.search(r'^class\s+' + name + r'\s*;', text, re.M):
                counts['forward_decl_to_include'] += 1
                text = re.sub(r'^class\s+' + name + r'\s*;', '#include "' + target + '"', text, flags=re.M)

        dest = os.path.join(OUT, r)
        os.makedirs(os.path.dirname(dest), exist_ok=True)
        open(dest, 'w', encoding='utf-8').write(text)

    print(f"processed {len(files)} files -> {OUT}")
    print("fixes applied:", dict(counts))

if __name__ == '__main__':
    sys.exit(main())
