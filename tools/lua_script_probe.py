#!/usr/bin/env python3
"""Read the calls a compiled Lua script makes, using the engine's own Lua format.

The engine ships Lua 5.0 (build/src_fixed/LuaFL/src).  Its undump format is
not the stock one: the header carries the operand widths (lundump.cpp:260-282)
and the function prototype puts the constants before the code
(lundump.cpp:214-231).  This tool follows those files rather than guessing.

    python3 tools/lua_script_probe.py <file.lvl> [function name]

It prints the calls the script makes, in order, with their arguments.
"""

import struct
import sys

LUA_SIGNATURE = b"\x1bLua"
LUA_TNIL = 0
LUA_TNUMBER = 3
LUA_TSTRING = 4

# lopcodes.h: the opcode numbers, in order.
OP_MOVE = 0
OP_LOADK = 1
OP_LOADBOOL = 2
OP_GETGLOBAL = 5
OP_SETGLOBAL = 7
OP_CALL = 25
OP_CLOSURE = 34

# lopcodes.h: the instruction layout.
SIZE_OP = 6
SIZE_C = 9
SIZE_B = 9
SIZE_A = 8
POS_C = SIZE_OP
POS_B = POS_C + SIZE_C
POS_A = POS_B + SIZE_B
MASK_Bx = (1 << (SIZE_C + SIZE_B)) - 1


def all_chunks(data, pos=8, end=None, depth=0, path=""):
    """Every chunk in the file, at any depth, with the path of chunk names."""
    for cid, size, start in chunk_children(data, pos, end):
        label = struct.pack("<I", cid).decode("latin1")
        here = "%s/%s" % (path, label) if path else label
        yield cid, size, start, here
        for found in all_chunks(data, start, start + size, depth + 1, here):
            yield found


def chunk_children(data, pos=8, end=None):
    """The four-character chunk sequence of a .lvl file."""
    if end is None:
        total = struct.unpack("<I", data[4:8])[0]
        end = 8 + total
    while pos + 8 <= end:
        cid = struct.unpack("<I", data[pos : pos + 4])[0]
        size = struct.unpack("<I", data[pos + 4 : pos + 8])[0]
        start = pos + 8
        if start + size > end:
            return
        yield cid, size, start
        pos = (start + size + 3) & ~3


class Reader:
    def __init__(self, data, pos=0):
        self.data = data
        self.pos = pos

    def byte(self):
        v = self.data[self.pos]
        self.pos += 1
        return v

    def int(self):
        v = struct.unpack("<i", self.data[self.pos : self.pos + 4])[0]
        self.pos += 4
        return v

    def size(self):
        v = struct.unpack("<I", self.data[self.pos : self.pos + 4])[0]
        self.pos += 4
        return v

    def number(self):
        # The retail scripts were dumped with a four-byte lua_Number: the header says so
        # (the size byte) and the test number is 31415926.0f, which is the engine's
        # TEST_NUMBER - pi times ten to the seventh - rounded to a float (lundump.h:19).
        v = struct.unpack("<f", self.data[self.pos : self.pos + 4])[0]
        self.pos += 4
        return v

    def string(self):
        size = self.size()
        if size == 0:
            return None
        raw = self.data[self.pos : self.pos + size]
        self.pos += size
        return raw.split(b"\0")[0].decode("latin1")


def load_function(r):
    """LoadFunction (lundump.cpp:214-231): source, header, lines, locals,
    upvalues, constants, code, then the nested functions."""
    source = r.string()
    line_defined = r.int()
    nups = r.byte()
    numparams = r.byte()
    is_vararg = r.byte()
    maxstack = r.byte()

    lines = [r.int() for _ in range(r.int())]

    locals_ = []
    for _ in range(r.int()):
        locals_.append((r.string(), r.int(), r.int()))

    upvalues = [r.string() for _ in range(r.int())]

    constants = []
    for _ in range(r.int()):
        tag = r.byte()
        if tag == LUA_TNIL:
            constants.append(None)
        elif tag == LUA_TNUMBER:
            constants.append(r.number())
        elif tag == LUA_TSTRING:
            constants.append(r.string())
        else:
            raise ValueError("bad constant type %d" % tag)

    # The nested functions sit between the constants and the code: the loader reads
    # them at the end of LoadConstants (lundump.cpp:202-208) and the dumper writes
    # them there too (ldump.cpp:128-129).
    children = [load_function(r) for _ in range(r.int())]

    code = [struct.unpack("<I", r.data[r.pos + 4 * i : r.pos + 4 * i + 4])[0] for i in range(r.int())]
    r.pos += 4 * len(code)

    return {
        "source": source,
        "line_defined": line_defined,
        "constants": constants,
        "code": code,
        "children": children,
        "locals": locals_,
        "upvalues": upvalues,
        "numparams": numparams,
        "maxstack": maxstack,
        "nups": nups,
        "is_vararg": is_vararg,
        "lines": lines,
    }


def load_chunk(data):
    """LoadHeader (lundump.cpp:260-282) and LoadFunction."""
    if data[:4] != LUA_SIGNATURE:
        raise ValueError("not a Lua chunk")
    r = Reader(data, 4)
    version = r.byte()
    endian = r.byte()
    widths = [r.byte() for _ in range(7)]  # int, size_t, Instruction, OP, A, B, C
    number_size = r.byte()
    test_number = r.number()  # the number format check
    widths.append(number_size)
    return version, endian, widths, test_number, load_function(r)


def calls(proto, wanted=None):
    """The calls the function makes, with their arguments.

    A call is GETGLOBAL (the function) then the arguments in registers then
    CALL (lopcodes.h).  The arguments are the constants loaded into the
    registers the call reads."""
    constants = proto["constants"]
    code = proto["code"]
    out = []

    for pc, ins in enumerate(code):
        op = ins & ((1 << SIZE_OP) - 1)
        if op != OP_GETGLOBAL:
            continue
        bx = (ins >> POS_C) & MASK_Bx
        name = constants[bx] if bx < len(constants) else None
        if not isinstance(name, str):
            continue
        if wanted is not None and name != wanted:
            continue

        a = (ins >> POS_A) & ((1 << SIZE_A) - 1)
        args = []
        for j in range(pc + 1, min(pc + 12, len(code))):
            ins2 = code[j]
            op2 = ins2 & ((1 << SIZE_OP) - 1)
            if op2 == OP_LOADK:
                bx2 = (ins2 >> POS_C) & MASK_Bx
                args.append(constants[bx2] if bx2 < len(constants) else None)
            elif op2 == OP_GETGLOBAL:
                bx2 = (ins2 >> POS_C) & MASK_Bx
                args.append(constants[bx2] if bx2 < len(constants) else None)
            elif op2 == OP_CALL:
                out.append((name, args))
                break
    return out


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    path = sys.argv[1]
    wanted = sys.argv[2] if len(sys.argv) > 2 else None
    data = open(path, "rb").read()

    scripts = []
    for cid, size, start, path in all_chunks(data):
        if cid != struct.unpack("<I", b"scr_")[0]:
            continue
        name = None
        body = None
        for c2, s2, p2, _ in all_chunks(data, start, start + size):
            payload = data[p2 : p2 + s2]
            if c2 == struct.unpack("<I", b"NAME")[0]:
                name = payload.split(b"\0")[0].decode("latin1")
            elif struct.pack("<I", c2).decode("latin1") in ("BODY", "CODE"):
                body = payload
        if body:
            scripts.append(("%s/%s" % (path, name) if path else name, body))

    for name, body in scripts:
        try:
            version, endian, widths, test_number, proto = load_chunk(body)
        except Exception as exc:  # a script the tool cannot read is reported, not hidden
            print("%s: not readable (%s)" % (name, exc))
            continue
        found = calls(proto, wanted)
        for child in proto["children"]:
            found += calls(child, wanted)
        if found:
            print("script %s (version %d, number %d bytes, test %g)" % (name, version, widths[7], test_number))
            for fname, args in found:
                print("  %s(%s)" % (fname, ", ".join(repr(a) for a in args)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
