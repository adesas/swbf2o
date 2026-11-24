#!/usr/bin/env python3
"""Close the platform-vocabulary gap by discovery, not guesswork.

The browser build replaces Direct3D9 / DirectInput / DirectSound / Winsock / Win32, but the
retained game code (and the DX-specific inline wrappers that survived decompilation) still names
their types, constants and device methods. Instead of hand-guessing that vocabulary, this tool
compiles the core scope repeatedly and turns clang's own diagnostics into declarations:

    unknown type name 'X'                     -> typedef int X;
    use of undeclared identifier 'X'          -> enum constant (stable value)
    no member named 'M' in 'IDirect3DDevice9' -> no-op template member on that interface

Output goes to spike/shim/generated_types.h, which both the game code and the new backends
include - so a constant here has the same value on both sides of the boundary. Generated device
members are no-ops: the WebGPU backend owns the device layer and is never reached through them.

Names our own shim headers already declare are never regenerated (denylist), otherwise a
placeholder typedef would shadow the real struct - that mistake is easy to make and very loud.

Usage: tools/shim_autogen.py [max_rounds] [scope-subset]
"""
import os
import re
import subprocess
import sys
import collections
from concurrent.futures import ThreadPoolExecutor

ROOT   = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC     = f"{ROOT}/build/src_fixed"
SHIM    = f"{ROOT}/spike/shim"
GEN     = f"{SHIM}/generated_types.h"
CLANG   = f"{ROOT}/vendor/emsdk/upstream/bin/clang"
SYSROOT = f"{ROOT}/vendor/emsdk/upstream/emscripten/cache/sysroot"
EXCLUDE = ('pcRedRenderer.cpp', 'Platform/Pc', 'PebbleFL/pc', 'SoundFL/source/win', 'Battlefront2/Source/pcMain.cpp', 'Battlefront2/Source/pcCrash.cpp', 'RedEngineFL/Memory/RedMemoryMi.cpp', '/Steam/', 'lglcd')

FLAGS = ["--sysroot=" + SYSROOT, "-target", "wasm32-unknown-emscripten", "-fms-extensions",
         "-O0", "-fsyntax-only", "-Wno-ignored-attributes", "-Wno-deprecated-non-prototype",
         "-Wno-invalid-offsetof", "-Wno-writable-strings", "-Wno-unused-variable",
         "-Wno-unused-parameter", "-Wno-address-of-temporary", "-ferror-limit=200",
         "-include", f"{SHIM}/swbf2_prefix.h"]
INC   = ["-I" + SHIM, "-I" + SRC, "-I" + SRC + "/LuaFL/include", "-I" + SRC + "/LuaFL/src"]

# only invent names that belong to the platform APIs we replace
ALLOW = re.compile(
    r"^_?(D3D|IDirect|D3DX|DX|DI8?D|DInput|DIDEV|DIDATA|DIDFT|DIDIM|DIEFF|DIJOY|DIPROP|DIPERF"
    r"|DIBUTTON|DINPUT|DIENUM|DIERR|DIFC|DSOUND|DSB|DS3D|DSCAPS|DSE|DSFX|DINPUTEFFECT"
    r"|IXAudio2|IMF|IMedia|IKs|KS(?!_)|WAVE|MMTIME|MM(?!_)|XINPUT|XInput|SOCKET|WSA|FD_|AF_|SO_"
    r"|IP_|INADDR|ERROR_|STATUS_|WAIT_|GMEM|LPC|LPD|LPTR|LPDIRECT|LPGUID|LPKNOWN|HFONT|HGDIOBJ"
    r"|HICON|HCURSOR|HBRUSH|HDC|HMENU|HBITMAP|HMONITOR|HPEN|HPALETTE|HSZ|MSG|POINT|RECT|SIZE"
    r"|SYSTEMTIME|TIMEVAL|LARGE_|VARIANT|BSTR|CREATE_|FILE_|THREAD_|PROCESS_|DEBUG_|SUBLANG"
    r"|LANG_|MAX_PATH|INFINITE|VK_|WM_|CF_|EM_|BS_|WS_|SW_|CDERR|CDD|VIDEODEV|DV(?!ID)|STG|OF_|DIK_)")

# the COM-ish interfaces whose members we may synthesise (parent must be listed first)
# Note: Direct3D 9, DirectInput 8, and DirectSound 8 interfaces are real virtual COM interfaces
# defined in spike/shim/d3d9.h, spike/shim/dinput.h, and spike/shim/dsound.h.
IFACES = [
    ('IUnknown', None),
]
IFACE_SET = {n for n, _ in IFACES}


def shim_macros():
    """#define names in the shim headers: a generated enum entry with one of these names would be
    macro-expanded inside the enum body, which is a syntax error rather than a redefinition."""
    names = set()
    for f in sorted(os.listdir(SHIM)):
        if not f.endswith('.h') or f == 'generated_types.h':
            continue
        text = open(os.path.join(SHIM, f), encoding='utf-8', errors='replace').read()
        names |= set(re.findall(r'^#[ \t]*define[ \t]+([A-Za-z_][A-Za-z0-9_]*)', text, re.M))
    return names


def shim_owned_names():
    """Names our own shim headers declare - the autogen must never redefine these."""
    names = set()
    for f in sorted(os.listdir(SHIM)):
        if not f.endswith('.h') or f == 'generated_types.h':
            continue
        text = open(os.path.join(SHIM, f), encoding='utf-8', errors='replace').read()
        for pat in (r'\b(?:struct|class|enum|union)\s+([A-Za-z_][A-Za-z0-9_]*)',
                    r'\b([A-Za-z_][A-Za-z0-9_]{2,})\s*\(',
                    r'\btypedef\s+[^;{}]*?\b([A-Za-z_][A-Za-z0-9_]*)\s*[;{]',
                    r'^#define\s+([A-Za-z_][A-Za-z0-9_]*)'):
            for m in re.finditer(pat, text, re.M):
                names.add(m.group(1))
    return names


def sources():
    out = []
    for d, _, fs in os.walk(SRC):
        for f in fs:
            if f.endswith(('.c', '.cpp')):
                p = os.path.join(d, f)
                if not any(x in p for x in EXCLUDE):
                    out.append(p)
    return sorted(out)


def write_gen(types, consts, iface_members, owned=frozenset()):
    L = []
    add = L.append
    add('#pragma once')
    add('/* AUTO-GENERATED by tools/shim_autogen.py - do not edit by hand.')
    add('')
    add('   Vocabulary of the platform APIs the browser build REPLACES (Direct3D9 / DirectInput /')
    add('   DirectSound / Winsock / Win32) that the RETAINED game code still names. Game code and the')
    add('   new backends include this single header, so a constant has the same value on both sides.')
    add('   Types are placeholders: such handles never outlive the replaced layer. Device members are')
    add('   no-ops because the WebGPU backend owns the device and is never reached through them.')
    add('*/')
    add('#include "swbf2_prefix.h"')
    add('')
    add('#ifdef __cplusplus')
    add('#include "d3d9.h"')
    add('#include "dinput.h"')
    add('#include "dsound.h"')
    add('')
    add('#ifndef SWBF2_GENERATED_TYPES_H')
    add('#define SWBF2_GENERATED_TYPES_H')
    add('')
    MACROS = shim_macros()
    # drop anything a shim header now declares by hand - the generated file must never fight the shims
    live_consts = {n: v for n, v in consts.items() if n not in MACROS and n not in owned}
    types = {n for n in types if n not in MACROS and n not in owned}
    if live_consts:
        add('enum _swbf2_gen_const : int {')
        for n, v in sorted(live_consts.items(), key=lambda kv: (kv[1], kv[0])):
            add(f'  {n} = {v},')
        add('};')
        add('')
    live_types = sorted(n for n in types if n not in MACROS)
    for n in live_types:
        add(f'typedef int {n};')
    if live_types:
        add('')
    for name, parent in IFACES:
        if name == 'IUnknown':
            continue
        members = sorted(iface_members.get(name, set()))
        add(f'struct {name} : {parent} {{')
        if name == 'IDirect3DDevice9':
            # Zero out-pointers and return E_NOTIMPL for stub resource factories until Phase 2's
            # real WebGPU IDirect3DDevice9 replaces these templates, so callers like pcRedTexture
            # never assign uninitialized stack pointers to m_pTexture or LockRect.pBits.
            add('  long CreateTexture(unsigned int, unsigned int, unsigned int, unsigned int, D3DFORMAT, D3DPOOL, IDirect3DTexture9** pp, void**) { if (pp) *pp = 0; return E_FAIL; }')
            add('  long CreateCubeTexture(unsigned int, unsigned int, unsigned int, D3DFORMAT, D3DPOOL, IDirect3DCubeTexture9** pp, void**) { if (pp) *pp = 0; return E_FAIL; }')
            add('  long CreateVolumeTexture(unsigned int, unsigned int, unsigned int, unsigned int, unsigned int, D3DFORMAT, D3DPOOL, IDirect3DVolumeTexture9** pp, void**) { if (pp) *pp = 0; return E_FAIL; }')
            add('  long CreateVertexBuffer(unsigned int, unsigned int, unsigned int, D3DPOOL, IDirect3DVertexBuffer9** pp, void**) { if (pp) *pp = 0; return E_FAIL; }')
            add('  long CreateIndexBuffer(unsigned int, unsigned int, D3DFORMAT, D3DPOOL, IDirect3DIndexBuffer9** pp, void**) { if (pp) *pp = 0; return E_FAIL; }')
            add('  long CreateDepthStencilSurface(unsigned int, unsigned int, D3DFORMAT, D3DMULTISAMPLE_TYPE, unsigned int, int, IDirect3DSurface9** pp, void**) { if (pp) *pp = 0; return E_FAIL; }')
            add('  long CreateRenderTarget(unsigned int, unsigned int, D3DFORMAT, D3DMULTISAMPLE_TYPE, unsigned int, int, IDirect3DSurface9** pp, void**) { if (pp) *pp = 0; return E_FAIL; }')
            add('  long CreateOffscreenPlainSurface(unsigned int, unsigned int, D3DFORMAT, D3DPOOL, IDirect3DSurface9** pp, void**) { if (pp) *pp = 0; return E_FAIL; }')
            add('  long CreateStateBlock(D3DSTATEBLOCKTYPE, IDirect3DStateBlock9** pp) { if (pp) *pp = 0; return E_FAIL; }')
            add('  long CreateQuery(D3DQUERYTYPE, IDirect3DQuery9** pp) { if (pp) *pp = 0; return E_FAIL; }')
            add('  long CreateVertexDeclaration(const D3DVERTEXELEMENT9*, IDirect3DVertexDeclaration9** pp) { if (pp) *pp = 0; return E_FAIL; }')
            add('  long CreateVertexShader(const unsigned int*, IDirect3DVertexShader9** pp) { if (pp) *pp = 0; return E_FAIL; }')
            add('  long CreatePixelShader(const unsigned int*, IDirect3DPixelShader9** pp) { if (pp) *pp = 0; return E_FAIL; }')
            skip_dev = {'CreateTexture', 'CreateCubeTexture', 'CreateVolumeTexture', 'CreateVertexBuffer',
                        'CreateIndexBuffer', 'CreateDepthStencilSurface', 'CreateRenderTarget',
                        'CreateOffscreenPlainSurface', 'CreateStateBlock', 'CreateQuery',
                        'CreateVertexDeclaration', 'CreateVertexShader', 'CreatePixelShader'}
            members = [m for m in members if m not in skip_dev]
        if not members and name != 'IDirect3DDevice9':
            add('  int _no_members_required = 0;')
        for m in members:
            add(f'  template <class... _A> long {m}(_A&&...) {{ return 0; }}')
        add('};')
    add('')
    add('#endif  /* SWBF2_GENERATED_TYPES_H */')
    add('#endif  /* __cplusplus */')
    with open(GEN, 'w') as fh:
        fh.write("\n".join(L) + "\n")


def main():
    max_rounds = int(sys.argv[1]) if len(sys.argv) > 1 else 12
    subset = sys.argv[2] if len(sys.argv) > 2 else None
    OWNED = shim_owned_names() | shim_macros()
    files = sources()
    if subset:
        files = [f for f in files if subset in f]

    types, consts, iface_members = set(), {}, collections.defaultdict(set)
    if os.path.exists(GEN):
        t = open(GEN).read()
        types |= set(re.findall(r'^typedef int ([A-Za-z_][A-Za-z0-9_]*);', t, re.M))
        for n, v in re.findall(r'^  ([A-Za-z_][A-Za-z0-9_]*) = (\d+),$', t, re.M):
            consts[n] = int(v)
        for blk in re.finditer(r'struct ([A-Za-z_][A-Za-z0-9_]*) : \w+ \{(.*?)\};', t, re.S):
            for m in re.finditer(r'long ([A-Za-z_][A-Za-z0-9_]*)\(_A&&', blk.group(2)):
                iface_members[blk.group(1)].add(m.group(1))
    value = (max(consts.values()) + 1) if consts else 0x1000

    def scan(paths):
        def one(p):
            return subprocess.run([CLANG] + FLAGS + INC + ["-c", p, "-o", "/dev/null"],
                                  capture_output=True, text=True).stderr
        with ThreadPoolExecutor(max_workers=8) as ex:
            return list(ex.map(one, paths))

    def harvest(errs):
        nonlocal value
        nt = nc = 0
        for err in errs:
            for m in re.finditer(r"unknown type name '([A-Za-z_][A-Za-z0-9_]*)'", err):
                n = m.group(1)
                if ALLOW.match(n) and n not in types and n not in consts and n not in OWNED:
                    types.add(n); nt += 1
            for m in re.finditer(r"use of undeclared identifier '([A-Za-z_][A-Za-z0-9_]*)'", err):
                n = m.group(1)
                if ALLOW.match(n) and n not in consts and n not in types and n not in OWNED:
                    consts[n] = value; value += 1; nc += 1
            for m in re.finditer(r"no member named '([A-Za-z_][A-Za-z0-9_]*)' in '([^']*)'", err):
                mem, iface = m.group(1), m.group(2)
                iface = iface.replace('const ', '').split(' ')[-1].rstrip('&*')
                if iface in IFACE_SET and mem not in iface_members[iface] and mem not in OWNED:
                    iface_members[iface].add(mem); nc += 1
        return nt, nc

    for r in range(max_rounds):
        new_t, new_c = harvest(scan(files))
        write_gen(types, consts, iface_members, OWNED)
        print(f"round {r+1}: +{new_t} types, +{new_c} names (types={len(types)}, "
              f"consts={len(live_consts) if 'live_consts' in dir() else len(consts)}, dx-members={sum(len(v) for v in iface_members.values())})")
        if not new_t and not new_c:
            break

    def ok_count():
        return sum(1 for e in scan(files) if not re.search(r'\berror:', e))
    print(f"converged: {ok_count()}/{len(files)} files syntax-clean")


if __name__ == '__main__':
    main()
