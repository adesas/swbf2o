#!/usr/bin/env python3
"""Extract the type-15 readers and writers from the fixed engine source into a standalone unit.

The functions live in NetGame.cpp, a game-layer file whose global constructors drag in a few hundred
symbols and trap at module start. Linking the whole file works only with a large stub set, and a
large stub set is where a check stops being a check.

So the functions are copied out verbatim instead. The copy is mechanical: this script finds each
definition by its signature and copies the text through unchanged, and it refuses to write anything
it cannot find. The generated file records the source path and its sha256, so what was extracted can
always be checked against what was in the tree.

    tools/extract_type15.py    ->  build/type15_engine.inc

Usage: python3 tools/extract_type15.py
"""
import hashlib
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SOURCE = os.path.join(ROOT, 'build/src_fixed/Battlefront2/Source/NetGame.cpp')
OUT = os.path.join(ROOT, 'build/type15_engine.inc')

# Each entry: the text that identifies the definition, and whether the capture should start at the
# line before it (for "template <>" headers, which sit on their own line).
TARGETS = [
    ('void WriteDestMask(NetPktGroup& group, unsigned __int64 mask)', False),
    ('void ReadDestMask(NetPktGroup& group, unsigned __int64& mask)', False),
    ('void WriteTurnLowBits(NetPktGroup* pGroup, unsigned int uTurn)', False),
    ('void ReadTurnLowBits(NetPktGroup& group, unsigned int& turn)', False),
    ('void WriteSwitchResponse(NetPktGroup* pGroup, int iResponse, int iTurn)', False),
    ('SwitchResponse ReadSwitchResponse(NetPktGroup& group, unsigned int turn, unsigned int)', False),
    ('void WriteMove(NetPktGroup* pGroup, ProcessedInputs* pPrev, ProcessedInputs* pCur)', False),
    ('bool ReadMove(NetPktGroup& group, const ProcessedInputs& previous, ProcessedInputs& current)', False),
    ('bool IsSame(ProcessedInputs* first, ProcessedInputs* second)', False),
    ('void WriteFixed<StickAxisConvert>(', True),
    ('void WriteFixed<MouseAxisConvert>(', True),
    ('void ReadFixed<StickAxisConvert>(', True),
    ('void ReadFixed<MouseAxisConvert>(', True),
]

GLOBALS = [
    'StickAxisConvert stickAxisConvert;',
    'MouseAxisConvert mouseAxisConvert;',
]


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
            continue                      # a declaration, not a definition
        start = i
        if include_header:
            # the "template <>" line sits above the signature
            if start == 0 or 'template' not in lines[start - 1]:
                continue
            start -= 1
        # the signature may wrap over several lines; find the line that opens the body
        j = i
        while j < len(lines):
            text = lines[j].split('//')[0].strip()
            if text.startswith('{'):
                break
            if text.endswith(';'):
                return None               # a declaration after all
            j += 1
        else:
            continue
        depth = 0
        while j < len(lines):
            text = lines[j].split('//')[0]
            depth += text.count('{') - text.count('}')
            if depth == 0:
                return ''.join(lines[start:j + 1])
            j += 1
        continue
    return None


def main():
    if not os.path.exists(SOURCE):
        sys.exit('source not found: %s' % SOURCE)
    text = open(SOURCE, 'r', errors='replace').read()
    lines = text.splitlines(keepends=True)
    digest = hashlib.sha256(text.encode('utf-8', 'replace')).hexdigest()

    pieces = []
    globals_text = []
    missing = []
    for needle, include_header in TARGETS:
        body = find_definition(lines, needle, include_header)
        if body is None:
            missing.append(needle)
            continue
        pieces.append('/* from NetGame.cpp: %s */\n%s\n' % (needle.split('(')[0], body))

    for glob in GLOBALS:
        if glob not in text:
            missing.append(glob)
            continue
        globals_text.append('/* from NetGame.cpp: %s */\n%s\n' % (glob, glob))

    if missing:
        sys.exit('could not extract:\n  ' + '\n  '.join(missing))

    prototypes = (
        'extern unsigned int turnHighBits;\n'
        '/* the primary template is declared in NetGame.cpp, not in a header */\n'
        'template <class Convert>\n'
        'void WriteFixed(Convert&, NetPktGroup&, float);\n'
        '/* the specializations are declared before the code that calls them, as in the source */\n'
        'template <>\n'
        'void WriteFixed<StickAxisConvert>(StickAxisConvert&, NetPktGroup&, float);\n'
        'template <>\n'
        'void WriteFixed<MouseAxisConvert>(MouseAxisConvert&, NetPktGroup&, float);\n'
        'void WriteDestMask(NetPktGroup& group, unsigned __int64 mask);\n'
        'void ReadDestMask(NetPktGroup& group, unsigned __int64& mask);\n'
        'void WriteTurnLowBits(NetPktGroup* pGroup, unsigned int uTurn);\n'
        'void ReadTurnLowBits(NetPktGroup& group, unsigned int& turn);\n'
        'void WriteSwitchResponse(NetPktGroup* pGroup, int iResponse, int iTurn);\n'
        'SwitchResponse ReadSwitchResponse(NetPktGroup& group, unsigned int turn, unsigned int);\n'
        'void WriteMove(NetPktGroup* pGroup, ProcessedInputs* pPrev, ProcessedInputs* pCur);\n'
        'bool ReadMove(NetPktGroup& group, const ProcessedInputs& previous, ProcessedInputs& current);\n'
        'bool IsSame(ProcessedInputs* first, ProcessedInputs* second);\n'
        '\n')

    header = (
        '/* Generated by tools/extract_type15.py - do not edit.\n'
        ' *\n'
        ' * Copied verbatim from %s\n'
        ' * source sha256 %s\n'
        ' *\n'
        ' * The readers and writers for PKT_GAME_REPLICA_UPDATE. They are copied rather than linked\n'
        ' * because the global constructors in NetGame.cpp reach into game code that the check does\n'
        ' * not need, and linking the file means stubbing hundreds of names - at which point the\n'
        ' * check is measuring the stubs. The copy is mechanical: the extractor copies the source text\n'
        ' * through unchanged and records the hash above. */\n'
        '#include "NetPacket.h"\n'
        '#include "NetGame.h"\n'
        '#include "FLJoystick.h"\n'
        '\n' + prototypes) % (os.path.relpath(SOURCE, ROOT), digest)

    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, 'w') as f:
        f.write(header)
        f.write('\n'.join(globals_text))
        f.write('\n'.join(pieces))

    print('wrote %s' % os.path.relpath(OUT, ROOT))
    print('source %s sha256 %s' % (os.path.relpath(SOURCE, ROOT), digest[:16]))
    print('extracted %d definitions' % len(pieces))
    return 0


if __name__ == '__main__':
    sys.exit(main())
