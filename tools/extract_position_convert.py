#!/usr/bin/env python3
"""Extract the netLOD path, the converters and the vector readers/writers from the fixed engine source
into a standalone unit.

They live in NetState.cpp, a game-layer file whose dependency closure is hundreds of entities, so
linking the whole file means stubbing most of the game. The check only calls what is listed below,
none of which reaches past a packet group and two globals, so those are copied out verbatim.

The copy is mechanical: this script finds each definition by its signature and copies the text
through unchanged, and it refuses to write anything it cannot find. The generated file records the
source path and its sha256, so what was extracted can always be checked against what was in the tree.

    tools/extract_position_convert.py    ->  build/position_convert_engine.inc

What is copied:
  NetState.cpp:74    int netLOD;                  the global the converters read
  NetState.cpp:165   int lastNetLOD;              the other one, what the wire field is compared to
  NetState.cpp:184   BeginNetLOD                  both to 0, which is where a connection starts
  NetState.cpp:290   SetNetLOD(bool)              how the engine itself sets netLOD
  NetState.cpp:296   SetNetPrecision              writes the field: one bit, or 0 plus the value in 2
  NetState.cpp:309   ReadNetLOD                   reads it back, and sets lastNetLOD
  NetState.cpp:76    NetVectorBits                netLOD == 1 ? 32 : 11, for UnitConvert
  NetState.cpp:81    ConvertValues                base class; TrackValue is empty, so nothing
  NetState.cpp:96    UnitConvert                  -1.0/1.0, bits from NetVectorBits()
  NetState.cpp:103   PositionXZConvert            -10240/10240, netLOD == 1 ? 32 : 21
  NetState.cpp:110   PositionYConvert             -1024/1024, same bit widths
  NetState.cpp:2616  WriteFixed / ReadFixed       the primary templates
  NetState.cpp:2653  the UnitConvert / PositionXZConvert / PositionYConvert instantiations
  NetState.cpp:2161  WriteUnitVector              three fields, all through UnitConvert
  NetState.cpp:2169  ReadUnitVector               the same
  NetState.cpp:2177  WritePositionVector          x through XZ, y through Y, z through XZ
  NetState.cpp:2185  ReadPositionVector           the same order
  NetState.cpp:152, 153, 154  the three converter objects
  NetState.cpp:89, 118, 125, 132, 139  QuatConvert, SwitchConvert, ThrustConvert, AngleConvert,
                       JetFuelConvert, and the objects built from them at :156-159
  NetState.cpp:426, 432, 438, 444    WriteThrust / ReadThrust, WriteAngle / ReadAngle
  NetState.cpp:3782, 3788, 3794, 3800 WriteSwitch / ReadSwitch, WriteJetFuel / ReadJetFuel
  NetState.cpp:2220, 2229             WriteQuaternion / ReadQuaternion, four fields in one order
  NetGame.cpp:4702                    NetGame::GetMyGuest, the bounds check and the one guest slot
  NetGame.cpp:4722                    NetGame::IsLocalTo, the rule MountedTurret::Write acts on

Three things in the output are not copies, and each is marked there. The accessors, because the
converter objects are file-static. The NetGame class shell and the guest table, because the real
NetGame.h has the whole game behind it: `myGuest[1]` is read from NetGame.h:528, and the nine values
seedGuests() puts in the table are an input both halves are given identically, not something either
side derives. And the harness, not the generated file, applies the assignment at NetState.cpp:1802,
because MountedTurret::Write is game layer and cannot be run here.
"""
import hashlib
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SOURCE = os.path.join(ROOT, 'build/src_fixed/Battlefront2/Source/NetState.cpp')
NETGAME = os.path.join(ROOT, 'build/src_fixed/Battlefront2/Source/NetGame.cpp')
OUT = os.path.join(ROOT, 'build/position_convert_engine.inc')

# Each entry is either a definition to copy ('def', signature, whether the capture starts at the line
# before it for a "template <class CONVERT>" header) or a single line to copy verbatim ('line').
# The order is the order the generated file needs: the globals first, then the bit width, then the
# classes, then the converter objects, then the templates and their instantiations, then the netLOD
# path, then the functions that call it.
TARGETS = [
    ('line', 'int netLOD;', False, 'state'),
    ('line', 'int lastNetLOD;', False, 'state'),
    ('def', 'static int NetVectorBits()', False, 'state'),
    ('def', 'class ConvertValues', False, 'state'),
    ('def', 'class UnitConvert : public ConvertValues', False, 'state'),
    ('def', 'class PositionXZConvert : public ConvertValues', False, 'state'),
    ('def', 'class PositionYConvert : public ConvertValues', False, 'state'),
    ('line', 'static UnitConvert unitConvert;', False, 'state'),
    ('line', 'static PositionXZConvert positionXZConvert;', False, 'state'),
    ('line', 'static PositionYConvert positionYConvert;', False, 'state'),
    ('def', 'void WriteFixed(CONVERT& convert, NetPktGroup& group, float fValue)', True, 'state'),
    ('def', 'void ReadFixed(CONVERT& convert, NetPktGroup& group, float& fValue)', True, 'state'),
    ('line', 'template void WriteFixed<UnitConvert>(UnitConvert&, NetPktGroup&, float);', False, 'state'),
    ('line', 'template void ReadFixed<UnitConvert>(UnitConvert&, NetPktGroup&, float&);', False, 'state'),
    ('line', 'template void WriteFixed<PositionXZConvert>(PositionXZConvert&, NetPktGroup&, float);', False, 'state'),
    ('line', 'template void ReadFixed<PositionXZConvert>(PositionXZConvert&, NetPktGroup&, float&);', False, 'state'),
    ('line', 'template void WriteFixed<PositionYConvert>(PositionYConvert&, NetPktGroup&, float);', False, 'state'),
    ('line', 'template void ReadFixed<PositionYConvert>(PositionYConvert&, NetPktGroup&, float&);', False, 'state'),
    ('def', 'void BeginNetLOD()', False, 'state'),
    ('def', 'void SetNetLOD(bool precision)', False, 'state'),
    ('def', 'void SetNetPrecision(NetPktGroup& group)', False, 'state'),
    ('def', 'void ReadNetLOD(NetPktGroup& group)', False, 'state'),
    ('def', 'void WriteUnitVector(NetPktGroup& group, const PblVector3& value)', False, 'state'),
    ('def', 'void ReadUnitVector(NetPktGroup& group, PblVector3& value)', False, 'state'),
    ('def', 'void WritePositionVector(NetPktGroup& group, const PblVector3& value)', False, 'state'),
    ('def', 'void ReadPositionVector(NetPktGroup& group, PblVector3& value)', False, 'state'),    ('def', 'class QuatConvert : public ConvertValues', False, 'state'),
    ('def', 'class SwitchConvert : public ConvertValues', False, 'state'),
    ('def', 'class ThrustConvert : public ConvertValues', False, 'state'),
    ('def', 'class AngleConvert : public ConvertValues', False, 'state'),
    ('def', 'class JetFuelConvert : public ConvertValues', False, 'state'),
    ('line', 'static QuatConvert quatConvert;', False, 'state'),
    ('line', 'static SwitchConvert switchConvert;', False, 'state'),
    ('line', 'static ThrustConvert thrustConvert;', False, 'state'),
    ('line', 'static AngleConvert angleConvert;', False, 'state'),
    ('line', 'static JetFuelConvert jetFuelConvert;', False, 'state'),
    ('line', 'template void WriteFixed<QuatConvert>(QuatConvert&, NetPktGroup&, float);', False, 'state'),
    ('line', 'template void ReadFixed<QuatConvert>(QuatConvert&, NetPktGroup&, float&);', False, 'state'),
    ('line', 'template void WriteFixed<SwitchConvert>(SwitchConvert&, NetPktGroup&, float);', False, 'state'),
    ('line', 'template void ReadFixed<SwitchConvert>(SwitchConvert&, NetPktGroup&, float&);', False, 'state'),
    ('line', 'template void WriteFixed<ThrustConvert>(ThrustConvert&, NetPktGroup&, float);', False, 'state'),
    ('line', 'template void ReadFixed<ThrustConvert>(ThrustConvert&, NetPktGroup&, float&);', False, 'state'),
    ('line', 'template void WriteFixed<AngleConvert>(AngleConvert&, NetPktGroup&, float);', False, 'state'),
    ('line', 'template void ReadFixed<AngleConvert>(AngleConvert&, NetPktGroup&, float&);', False, 'state'),
    ('line', 'template void WriteFixed<JetFuelConvert>(JetFuelConvert&, NetPktGroup&, float);', False, 'state'),
    ('line', 'template void ReadFixed<JetFuelConvert>(JetFuelConvert&, NetPktGroup&, float&);', False, 'state'),
    ('text', 'pbl_quaternion', False, 'state'),
    ('def', 'void WriteThrust(NetPktGroup& group, float value)', False, 'state'),
    ('def', 'void ReadThrust(NetPktGroup& group, float& value)', False, 'state'),
    ('def', 'void WriteAngle(NetPktGroup& group, float value)', False, 'state'),
    ('def', 'void ReadAngle(NetPktGroup& group, float& value)', False, 'state'),
    ('def', 'void WriteSwitch(NetPktGroup& group, float value)', False, 'state'),
    ('def', 'void ReadSwitch(NetPktGroup& group, float& value)', False, 'state'),
    ('def', 'void WriteJetFuel(NetPktGroup& group, float value)', False, 'state'),
    ('def', 'void ReadJetFuel(NetPktGroup& group, float& value)', False, 'state'),
    ('def', 'void WriteQuaternion(NetPktGroup& group, const PblQuaternion& value)', False, 'state'),
    ('def', 'void ReadQuaternion(NetPktGroup& group, PblQuaternion& value)', False, 'state'),
    ('text', 'netgame_shell', False, 'state'),
    ('def', 'int NetGame::GetMyGuest(int iPlayer, int iSlot)', False, 'game'),
    ('def', 'bool NetGame::IsLocalTo(int iPlayer, int iOther)', False, 'game'),
]


TEXT = {
    'pbl_quaternion': (
        '/* The only line the generated file adds for the quaternion path: the header for\n'
        ' * PblQuaternion, which WriteQuaternion and ReadQuaternion take. Nothing in it is a model.\n'
        ' */\n'
        '#include "PebbleFL/Common/PblQuaternion.h"\n\n'),
    'netgame_shell': (
        '/* Not a copy. NetGame.h cannot be included here: its dependency closure is the whole game.\n'
        ' * This is the smallest shell that lets the two functions below be the engine\u2019s own, copied\n'
        ' * verbatim from NetGame.cpp. `myGuest[1]` is what NetGame.h:528 declares.\n'
        ' *\n'
        ' * The guest table is an input, not a rule: the nine values seedGuests() writes are the same\n'
        ' * nine values tools/position_convert_check.py is given, and the other 55 entries are -1 on both\n'
        ' * sides. What is compared is each side\u2019s own reading of the rule that reads the table.\n'
        ' */\n'
        'struct NetPlayerStub { int myGuest[1]; };\n'
        'struct NetGame\n'
        '{\n'
        'public:\n'
        '    static NetPlayerStub netPlayers[64];\n'
        '    static int GetMyGuest(int iPlayer, int iSlot);\n'
        '    static bool IsLocalTo(int iPlayer, int iOther);\n'
        '};\n'
        'NetPlayerStub NetGame::netPlayers[64];\n'
        'void seedGuests()\n'
        '{\n'
        '    for (int i = 0; i < 64; ++i) NetGame::netPlayers[i].myGuest[0] = -1;\n'
        '    NetGame::netPlayers[5].myGuest[0] = -1;\n'
        '    NetGame::netPlayers[12].myGuest[0] = 44;\n'
        '    NetGame::netPlayers[20].myGuest[0] = 31;\n'
        '}\n\n'),
}

def find_definition(lines, needle, include_header):
    """Return the text of the definition whose signature line contains `needle`.

    A forward declaration has the same text as the definition, so a line that ends in a semicolon
    is skipped: the real definition is the one whose body follows.
    """
    for i, line in enumerate(lines):
        if needle not in line:
            continue
        code = line.split('//')[0].strip()
        if code.endswith(';'):
            continue
        start = i
        if include_header:
            if start == 0 or 'template' not in lines[start - 1]:
                continue
            start -= 1
        j = i
        while j < len(lines):
            text = lines[j].split('//')[0].strip()
            if text.startswith('{'):
                break
            if text.endswith(';'):
                return None
            j += 1
        else:
            continue
        depth = 0
        while j < len(lines):
            text = lines[j].split('//')[0]
            depth += text.count('{') - text.count('}')
            if depth == 0:
                return start + 1, ''.join(lines[start:j + 1])
            j += 1
        continue
    return None


def main():
    if not os.path.exists(SOURCE):
        sys.exit('source not found: %s' % SOURCE)
    sources = {
        'state': open(SOURCE, 'r', errors='replace').read(),
        'game': open(NETGAME, 'r', errors='replace').read(),
    }
    digests = {k: hashlib.sha256(v.encode('utf-8', 'replace')).hexdigest() for k, v in sources.items()}
    src_lines = {k: v.splitlines(keepends=True) for k, v in sources.items()}

    pieces = []
    missing = []
    defs = copied_lines = 0
    for kind, needle, include_header, which in TARGETS:
        if kind == 'text':
            pieces.append(TEXT[needle])
            continue
        lines = src_lines[which]
        file_name = SOURCE if which == 'state' else NETGAME
        if kind == 'line':
            # A 'line' target is a declaration, so it does end in a semicolon; it is copied as it is.
            hit = [n for n, l in enumerate(lines, 1) if needle in l]
            if not hit:
                missing.append(needle)
                continue
            n = hit[0] if len(hit) == 1 else [x for x in hit if 'static' in lines[x - 1]][0]
            copied_lines += 1
            pieces.append('/* from %s:%d, copied verbatim */\n%s' % (os.path.basename(file_name), n, lines[n - 1]))
            continue
        got = find_definition(lines, needle, include_header)
        if got is None:
            missing.append(needle)
            continue
        n, body = got
        defs += 1
        pieces.append('/* from %s:%d, copied verbatim */\n%s\n' % (os.path.basename(file_name), n, body))

    if missing:
        sys.exit('not found in %s:\n  %s' % (SOURCE, '\n  '.join(missing)))

    accessors = (
        '\n/* Accessors. These are the only text in this file that is not copied from the source.\n'
        ' * They model nothing: each one returns what the engine\'s own converter reports. The check\n'
        ' * needs them because the converter objects are file-static in the source, so nothing outside\n'
        ' * this translation unit can ask one what bit width it uses.\n'
        ' */\n'
        'int unitBits() { return unitConvert.GetBits(); }\n'
        'unsigned int unitMinBits()\n'
        '{\n    float f = unitConvert.GetMin();\n    return *(const unsigned int*)&f;\n}\n'
        'unsigned int unitMaxBits()\n'
        '{\n    float f = unitConvert.GetMax();\n    return *(const unsigned int*)&f;\n}\n'
        'int posXZBits() { return positionXZConvert.GetBits(); }\n'
        'unsigned int posXZMinBits()\n'
        '{\n    float f = positionXZConvert.GetMin();\n    return *(const unsigned int*)&f;\n}\n'
        'unsigned int posXZMaxBits()\n'
        '{\n    float f = positionXZConvert.GetMax();\n    return *(const unsigned int*)&f;\n}\n'
        'int posYBits() { return positionYConvert.GetBits(); }\n'
        'unsigned int posYMinBits()\n'
        '{\n    float f = positionYConvert.GetMin();\n    return *(const unsigned int*)&f;\n}\n'
        'unsigned int posYMaxBits()\n'
        '{\n    float f = positionYConvert.GetMax();\n    return *(const unsigned int*)&f;\n}\n'
        'int quatBits() { return quatConvert.GetBits(); }\n'
        'unsigned int quatMinBits()\n'
        '{\n    float f = quatConvert.GetMin();\n    return *(const unsigned int*)&f;\n}\n'
        'unsigned int quatMaxBits()\n'
        '{\n    float f = quatConvert.GetMax();\n    return *(const unsigned int*)&f;\n}\n'
        'int switchBits() { return switchConvert.GetBits(); }\n'
        'unsigned int switchMinBits()\n'
        '{\n    float f = switchConvert.GetMin();\n    return *(const unsigned int*)&f;\n}\n'
        'unsigned int switchMaxBits()\n'
        '{\n    float f = switchConvert.GetMax();\n    return *(const unsigned int*)&f;\n}\n'
        'int thrustBits() { return thrustConvert.GetBits(); }\n'
        'unsigned int thrustMinBits()\n'
        '{\n    float f = thrustConvert.GetMin();\n    return *(const unsigned int*)&f;\n}\n'
        'unsigned int thrustMaxBits()\n'
        '{\n    float f = thrustConvert.GetMax();\n    return *(const unsigned int*)&f;\n}\n'
        'int angleBits() { return angleConvert.GetBits(); }\n'
        'unsigned int angleMinBits()\n'
        '{\n    float f = angleConvert.GetMin();\n    return *(const unsigned int*)&f;\n}\n'
        'unsigned int angleMaxBits()\n'
        '{\n    float f = angleConvert.GetMax();\n    return *(const unsigned int*)&f;\n}\n'
        'int jetFuelBits() { return jetFuelConvert.GetBits(); }\n'
        'unsigned int jetFuelMinBits()\n'
        '{\n    float f = jetFuelConvert.GetMin();\n    return *(const unsigned int*)&f;\n}\n'
        'unsigned int jetFuelMaxBits()\n'
        '{\n    float f = jetFuelConvert.GetMax();\n    return *(const unsigned int*)&f;\n}\n'
    )

    header = (
        '/* Generated by tools/extract_position_convert.py. Do not edit.\n'
        ' *\n'
        ' * Source: %s\n'
        ' * sha256: %s\n'
        ' * Source: %s\n'
        ' * sha256: %s\n'
        ' *\n'
        ' * %d definitions and %d lines, copied through unchanged. Nothing here is a model of the\n'
        ' * engine: this is the engine\'s own code. It is a separate translation unit only so that the\n'
        ' * accessors below can be reached.\n'
        ' * The two source files are hashed separately.\n'
        ' */\n'
        '#include "NetPacket.h"\n'
        '#include "PebbleFL/Common/PblVector.h"\n'
        '\n' % (os.path.relpath(SOURCE, ROOT), digests['state'], os.path.relpath(NETGAME, ROOT),
                 digests['game'], defs, copied_lines)
        + '\n'.join(pieces) + accessors)

    open(OUT, 'w').write(header)
    sys.stderr.write('wrote %s from %s (%d definitions, %d lines)\n'
                     % (os.path.relpath(OUT, ROOT), os.path.relpath(SOURCE, ROOT), defs, copied_lines))
    return 0


if __name__ == '__main__':
    sys.exit(main())
