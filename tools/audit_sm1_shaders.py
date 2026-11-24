#!/usr/bin/env python3
"""
Audit & disassemble all DirectX 9 Shader Model 1.1 (vs_1_1 / ps_1_1) shaders in SWBF2's core.lvl.

Usage:
  python3 tools/audit_sm1_shaders.py
  python3 tools/audit_sm1_shaders.py --json
"""

import struct
import sys
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CORE_LVL = ROOT / "data/Star wars battlefront 2/GameData/data/_lvl_pc/core.lvl"

OP_NAMES = {
    0: "nop", 1: "mov", 2: "add", 3: "sub", 4: "mad", 5: "mul", 6: "rcp", 7: "rsq",
    8: "dp3", 9: "dp4", 10: "min", 11: "max", 12: "slt", 13: "sge", 14: "exp", 15: "log",
    16: "lit", 17: "dst", 18: "lrp", 19: "frc", 20: "m4x4", 21: "m4x3", 22: "m3x4",
    23: "m3x3", 24: "m3x2", 31: "dcl", 35: "abs", 46: "mova",
    64: "texcoord", 65: "texkill", 66: "tex", 67: "texbem", 68: "texbeml", 69: "texreg2ar",
    70: "texreg2gb", 71: "texm3x2pad", 72: "texm3x2tex", 73: "texm3x3pad", 74: "texm3x3tex",
    75: "texm3x3diff", 76: "texm3x3spec", 77: "texm3x3vspec", 78: "expp", 79: "logp",
    80: "cnd", 81: "def", 88: "cmp", 89: "bem",
}

USAGE_NAMES = {
    0: "position", 1: "blendweight", 2: "blendindices", 3: "normal", 4: "psize",
    5: "texcoord", 6: "tangent", 7: "binormal", 8: "tessfactor", 9: "positiont",
    10: "color", 11: "fog", 12: "depth", 13: "sample",
}


def scan_core_shaders(buf: bytes):
    pos = 8
    end = len(buf)
    shdrs = []
    all_vs = []
    all_ps = []

    while pos + 8 <= end:
        cid = buf[pos:pos + 4]
        sz = struct.unpack_from("<I", buf, pos + 4)[0]
        payload = pos + 8
        cend = payload + sz
        if cid == b"SHDR":
            rtyp = ""
            p = payload
            while p + 8 <= cend:
                ccid = buf[p:p + 4]
                csz = struct.unpack_from("<I", buf, p + 4)[0]
                if ccid == b"RTYP":
                    rtyp = buf[p + 8:p + 8 + csz].split(b"\x00")[0].decode("latin1")
                    break
                p = (p + 8 + csz + 3) & ~3

            vs_list = []
            ps_list = []
            i = payload
            while i + 8 <= cend:
                w = struct.unpack_from("<I", buf, i)[0]
                if w in (0xFFFE0101, 0xFFFF0101):
                    j = i + 4
                    while j + 4 <= cend:
                        tok = struct.unpack_from("<I", buf, j)[0]
                        if (tok & 0xFFFF) == 0xFFFE:
                            j += 4 + ((tok >> 16) & 0x7FFF) * 4
                            continue
                        if tok == 0x0000FFFF:
                            words = struct.unpack_from(f"<{(j + 4 - i) // 4}I", buf, i)
                            if w == 0xFFFE0101:
                                vs_list.append(words)
                                all_vs.append((rtyp, words))
                            else:
                                ps_list.append(words)
                                all_ps.append((rtyp, words))
                            i = j + 4
                            break
                        j += 4
                    else:
                        i += 4
                else:
                    i += 4

            shdrs.append({
                "rtyp": rtyp,
                "vsCount": len(vs_list),
                "psCount": len(ps_list),
            })
        pos = (cend + 3) & ~3

    return shdrs, all_vs, all_ps


def summarize_opcodes(shader_pairs):
    ops = {}
    dcls = {}
    rel_addr = 0
    coissue = 0
    max_const = -1
    for _, words in shader_pairs:
        k = 1
        while k < len(words):
            tok = words[k]
            if tok == 0x0000FFFF:
                break
            op = tok & 0xFFFF
            if op == 0xFFFE:
                k += 1 + ((tok >> 16) & 0x7FFF)
                continue
            if tok & 0x40000000:
                coissue += 1
            op_name = OP_NAMES.get(op, f"op_{op}")
            ops[op_name] = ops.get(op_name, 0) + 1
            k += 1
            if op == 81:
                k += 5
                continue
            if op == 31:
                ut = words[k]
                rt = words[k + 1]
                u_name = f"{USAGE_NAMES.get(ut & 0x1F, str(ut & 0x1F))}{(ut >> 16) & 0xF}->v{rt & 0x7FF}"
                dcls[u_name] = dcls.get(u_name, 0) + 1
                k += 2
                continue
            while k < len(words) and (words[k] & 0x80000000):
                pt = words[k]
                rtype = ((pt >> 28) & 7) | (((pt >> 11) & 3) << 3)
                rnum = pt & 0x7FF
                if rtype == 2 and rnum > max_const:
                    max_const = rnum
                if pt & 0x00002000:
                    rel_addr += 1
                k += 1
    return {
        "count": len(shader_pairs),
        "opcodes": dict(sorted(ops.items())),
        "declarations": dict(sorted(dcls.items())),
        "relativeAddressingCount": rel_addr,
        "coIssueCount": coissue,
        "maxConstantRegister": max_const,
    }


def main():
    buf = CORE_LVL.read_bytes()
    shdrs, all_vs, all_ps = scan_core_shaders(buf)
    report = {
        "file": str(CORE_LVL.relative_to(ROOT)),
        "shdrChunkCount": len(shdrs),
        "vs_1_1": summarize_opcodes(all_vs),
        "ps_1_1": summarize_opcodes(all_ps),
        "shadersByRenderType": shdrs,
    }
    if "--json" in sys.argv:
        print(json.dumps(report, indent=2))
    else:
        print(f"core.lvl SHDR chunks: {report['shdrChunkCount']}")
        print(f"Total vs_1_1 shaders: {report['vs_1_1']['count']} (max const: c{report['vs_1_1']['maxConstantRegister']}, rel_addr: {report['vs_1_1']['relativeAddressingCount']})")
        print(f"  vs_1_1 opcodes: {report['vs_1_1']['opcodes']}")
        print(f"  vs_1_1 dcls:    {report['vs_1_1']['declarations']}")
        print(f"Total ps_1_1 shaders: {report['ps_1_1']['count']} (max const: c{report['ps_1_1']['maxConstantRegister']}, co_issue: {report['ps_1_1']['coIssueCount']})")
        print(f"  ps_1_1 opcodes: {report['ps_1_1']['opcodes']}")


if __name__ == "__main__":
    main()
