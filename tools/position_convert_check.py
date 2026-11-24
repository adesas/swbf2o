#!/usr/bin/env python3
"""An independent model of the engine's converters, of how netLOD reaches the reader, and of the
per-client decision that changes it.

The engine's half of this check is spike/position_convert_check.cpp, which calls the engine's own
BeginNetLOD, SetNetLOD, SetNetPrecision, ReadNetLOD, NetGame::IsLocalTo and NetGame::GetMyGuest,
NetVectorBits, the converter classes, WriteUnitVector, ReadUnitVector, WritePositionVector,
ReadPositionVector, WriteThrust, ReadThrust, WriteAngle, ReadAngle, WriteJetFuel, ReadJetFuel,
WriteSwitch, ReadSwitch, WriteQuaternion, ReadQuaternion and the bit packing. This file is a second
reading of the same source written from scratch, so the two can disagree - which a round trip cannot
show, because a writer and a reader that are wrong in the same way still round trip.

The rules, each traced to the source they came from:

  NetGame::IsLocalTo          NetGame.cpp:4722  iOther < 0 -> false; iOther == iPlayer -> true; then
                                               the one guest slot through GetMyGuest
  NetGame::GetMyGuest         NetGame.cpp:4702  iPlayer < 0 or >= 64 or iSlot != 0 -> -1, otherwise
                                               netPlayers[iPlayer].myGuest[iSlot]
  NetState.cpp:1798, :1802    MountedTurret::Write keeps what netLOD was as oldPrecision, then sets
                                               netLOD from the rule. The assignment is inside a game
                                               class, so this check cannot run it; it compares the
                                               consequence, and the harness applies the assignment
  BeginNetLOD                 NetState.cpp:184   netLOD = 0, lastNetLOD = 0
  SetNetLOD(bool)             NetState.cpp:290   netLOD = precision. A bool, so 0 or 1.
  SetNetPrecision             NetState.cpp:296   one bit when netLOD == lastNetLOD, otherwise 0 and
                                                 the value in 2 bits, then lastNetLOD = netLOD
  ReadNetLOD                  NetState.cpp:309   read the 1 bit; if it is 0 read 2 bits and set
                                                 lastNetLOD; then netLOD = lastNetLOD
  NetVectorBits               NetState.cpp:76    netLOD == 1 ? 32 : 11   (UnitConvert only)
  UnitConvert                 NetState.cpp:96    -1.0f .. 1.0f, bits = NetVectorBits()
  PositionXZConvert           NetState.cpp:103   -10240.0f .. 10240.0f, netLOD == 1 ? 32 : 21
  PositionYConvert            NetState.cpp:110   -1024.0f .. 1024.0f,   netLOD == 1 ? 32 : 21
  SwitchConvert               NetState.cpp:124   -1.0f .. 1.0f, 4 bits. Not gated by netLOD.
  ThrustConvert               NetState.cpp:131   -1.0f .. 1.0f, netLOD == 1 ? 32 : 7
  AngleConvert                NetState.cpp:138   -pi .. pi, netLOD == 1 ? 32 : 9
  JetFuelConvert              NetState.cpp:145   0.0f .. 1.0f, 11 bits. Not gated by netLOD.
  QuatConvert                 NetState.cpp:89    -1.0f .. 1.0f, 13 bits. Not gated by netLOD.
  WriteUnitVector / Read      NetState.cpp:2161, 2169
  WritePositionVector / Read  NetState.cpp:2177, 2185  x and z through XZ, y through Y
  WriteQuaternion / Read      NetState.cpp:2220, 2229  X, Y, Z, S in that order
  WriteThrust / ReadThrust    NetState.cpp:426, 432
  WriteAngle / ReadAngle      NetState.cpp:438, 444
  WriteSwitch / ReadSwitch    NetState.cpp:3782, 3788
  WriteJetFuel / ReadJetFuel  NetState.cpp:3794, 3800
  WriteFixed / ReadFixed      NetState.cpp:2617, 2625  call TrackValue, then the group's fixed field
  ConvertValues::TrackValue   NetState.cpp:86    empty. Nothing is recorded, no clamping happens.
  NetPktGroup::WriteFixed     NetPacket.cpp:420  doHiRes or iBits >= 32: raw IEEE bits. Otherwise 0
      for f <= fMin, 0xffffffff for f >= fMax, else trunc((float)(unsigned int)((1<<bits)-2) *
      (f - fMin) / (fMax - fMin) + 0.5f).
  NetPktGroup::ReadFixed      NetPacket.cpp:442  code 0 -> fMin, code (1<<bits)-1 -> fMax, and only
      interior codes divide by (1<<bits)-2. That is why the endpoints are reserved.

Every arithmetic step here is float32, including the rounding, because the engine does it in float32.
`--risk` counts the interior codes where a model computing in double lands on a different float32
value, for each of the bit widths.

`--wrong-bits N` puts a deliberately wrong bit width in this model and nothing else.
`--gate-everything` makes this model apply the raw-IEEE path to every converter when netLOD is 1, which
is the mistake a host would make for JetFuel, Switch and Quat: three of these converters are not gated.
browser/position_convert_check.mjs runs both.

Usage: python3 tools/position_convert_check.py [--json] [--risk] [--wrong-bits N] [--gate-everything]
"""
import json
import math
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from net_wire_check import (Group, Pool, c_float, double_model_divergence, fbits,  # noqa: E402
                            fixed_code, fixed_decode, reassemble)

# The header. Inputs both sides set identically. The type is not a claim about which packet carries
# positions: type 15 carries input, and positions come from prediction. It is below 0x1e, so the
# receive prefix is 3, as the wire check pinned.
TYPE = 12
SRC = 3
SEQ = 77

BEGIN, SET, PRECISION, WIRE, READ, VEC_POS, VEC_UNIT = range(7)
FIELD, GET = 7, 8                      # write one field / read one field back
#   for those two the argument is converter * 8 + value index, so both halves read the same number
#   and the existing scenarios' steps are unchanged
DECISION = 9                           # run the rule the engine's own IsLocalTo implements
GROUP = 10                               # a group boundary: the state has to survive across it
UNIT, PXZ, PY, SWITCH, THRUST, ANGLE, JETFUEL, QUAT = range(8)
PI = -3.1415927410125732

# `gated` is the part of each class that reads netLOD. Three of them do not, and that difference is
# the thing this file is checking: a model that gates everything is wrong for those three.
CONVERTERS = {
    'unit': {'min': -1.0, 'max': 1.0, 'base': 11, 'gated': True},
    'positionXZ': {'min': -10240.0, 'max': 10240.0, 'base': 21, 'gated': True},
    'positionY': {'min': -1024.0, 'max': 1024.0, 'base': 21, 'gated': True},
    'switch': {'min': -1.0, 'max': 1.0, 'base': 4, 'gated': False},
    'thrust': {'min': -1.0, 'max': 1.0, 'base': 7, 'gated': True},
    'angle': {'min': PI, 'max': -PI, 'base': 9, 'gated': True},
    'jetfuel': {'min': 0.0, 'max': 1.0, 'base': 11, 'gated': False},
    'quat': {'min': -1.0, 'max': 1.0, 'base': 13, 'gated': False},
}
# Which converter each field goes through - the model's reading of the source. The engine half states
# the same reading; the bytes are what decides it, because the converters have different ranges.
ORDER = {
    'pos': ['positionXZ', 'positionY', 'positionXZ'],
    'unit': ['unit', 'unit', 'unit'],
    'quat': ['quat', 'quat', 'quat', 'quat'],
    'thrust': ['thrust'], 'angle': ['angle'], 'jetfuel': ['jetfuel'], 'switch': ['switch'],
    'positionXZ': ['positionXZ'], 'positionY': ['positionY'],
}

def enc(c, v):
    return c * 8 + v

WRONG_BITS = None                      # set by --wrong-bits, the negative control
GATE_EVERYTHING = False                # set by --gate-everything, the second negative control


def bits_for(netLOD, converter):
    if WRONG_BITS is not None:
        return WRONG_BITS
    c = CONVERTERS[converter]
    if GATE_EVERYTHING:
        return 32 if netLOD == 1 else c['base']
    return 32 if (netLOD == 1 and c['gated']) else c['base']


def to_float(bits):
    """Reinterpret the bits as a float32. That is what the engine's float field is, and it is not the
    same as converting an integer to a float."""
    return struct.unpack('<f', struct.pack('<I', int(bits) & 0xFFFFFFFF))[0]


def masked(code, bits):
    """What actually reaches the wire: NetPacket.cpp:371 masks each part of a field."""
    return code if bits >= 32 else code & ((1 << bits) - 1)


# The guest table. It is an input both halves are given identically, not something either side
# derives; what is compared is each side's own reading of the rule that reads it.
GUESTS = {5: [-1], 12: [44], 20: [31]}


def get_my_guest(player, slot):
    """This model's reading of NetGame.cpp:4693."""
    if player < 0 or player >= 64 or slot != 0:
        return -1
    return GUESTS.get(player, [-1])[slot]


def is_local_to(player, other):
    """This model's reading of NetGame.cpp:4722."""
    if other < 0:
        return False
    if other == player:
        return True
    for i in range(1):
        if other == get_my_guest(player, i):
            return True
    return False


# client id, mPlayerId, the value GetMyGuest would read, the result, netLOD before, netLOD after.
DECISIONS = [
    (5, -1),      # iOther < 0 -> false, so netLOD stays as it was
    (5, 5),       # iOther == iPlayer -> true
    (12, 44),     # the one guest slot
    (12, 45),     # a guest table that does not match
    (64, 44),     # past the array: GetMyGuest returns -1 before comparing
    (5, 5),       # same client again, from a netLOD that was already 1
]


def order_for(op, arg):
    if op == VEC_POS:
        return ORDER['pos']
    if op == VEC_UNIT:
        return ORDER['unit']
    return ORDER[CONVERTERS_NAMES[arg // 8]]


def value_row(op, arg):
    """For the field ops the argument carries both the converter and the row; for the vector ops it is
    the row itself, so the two must not be read the same way."""
    return arg if op in (VEC_POS, VEC_UNIT) else arg % 8


def field_record(netLOD, converter, value_bits):
    conv = CONVERTERS[converter]
    b = bits_for(netLOD, converter)
    code = masked(fixed_code(to_float(value_bits), conv['min'], conv['max'], b), b)
    return {'name': converter, 'bits': b, 'min': fbits(conv['min']), 'max': fbits(conv['max']),
            'code': code}


# Each scenario has a writer side and a reader side, and both halves run the same steps. The engine has
# one netLOD, so the reader continues from the state the writer left; BEGIN is the engine's own
# BeginNetLOD, which is what a new peer starts from. Values are float32 bit patterns, so both halves
# describe the same input.
V03 = [0x3E99999A]                     # 0.3, the same value through every converter
VQUAT = [0xBF7FFCB9, 0x3ECCCCCD, 0x3F333333, 0x3F800000]   # -0.99995, 0.4, 0.7, 1.0:
# the first lands on code 0 from inside the range, the last is the top code. The decoded components
# are not unit length before normalisation: they are -1.0, 0.4, 0.7, 1.0.
VROUND = [0xBF3FEC57]                  # -0.7497, which quantises to interior code 1025

SCENARIOS = [
    {'name': 'ordinary interior values',
     'w': [(SET, 0), (VEC_POS, 0), (VEC_POS, 1)], 'r': [(VEC_POS, 0), (VEC_POS, 1)],
     'values': [[0x449A5000, 0x42290000, 0xC5B17100],       # 1234.5, 42.25, -5678.125
                [0xC2C80000, 0x00000000, 0x42C80000]]},     # -100.0, 0.0, 100.0: 0.0 is an interior code
    {'name': 'the endpoints are codes, not values',
     'w': [(SET, 0), (VEC_POS, 0), (VEC_POS, 1)], 'r': [(VEC_POS, 0), (VEC_POS, 1)],
     'values': [[0xC6200000, 0xC4800000, 0xC6200000],       # exactly min on all three: code 0
                [0x46200000, 0x44800000, 0x46200000]]},     # exactly max: code (1<<21)-1
    {'name': 'values outside the range',
     'w': [(SET, 0), (VEC_POS, 0), (VEC_POS, 1)], 'r': [(VEC_POS, 0), (VEC_POS, 1)],
     'values': [[0xC63B8000, 0xC59C4000, 0xC63B8000],       # -12000, -5000, -12000: below min
                [0x463B8000, 0x459C4000, 0x463B8000]]},     # 12000, 5000, 12000: above max, y is outside
    {'name': 'an interior value that rounds onto the reserved code 0',
     'w': [(SET, 0), (VEC_POS, 0)], 'r': [(VEC_POS, 0)],
     'values': [[0xC61FFFFF, 0xC47FFFFF, 0xC61FFFFF]]},     # one float32 step inside the range
    {'name': 'float32 rounding, where a double model answers differently',
     'w': [(SET, 0), (VEC_POS, 0)], 'r': [(VEC_POS, 0)],
     'values': [[0x46180000, 0x44780004, 0xC6180000]]},     # 9728.0, 992.000244140625, -9728.00048828125
    {'name': 'netLOD 1: the same values as raw IEEE bits',
     'w': [(SET, 1), (VEC_POS, 0), (VEC_POS, 1)], 'r': [(VEC_POS, 0), (VEC_POS, 1)],
     'values': [[0x46180000, 0x44780004, 0xC6180000],
                [0xC63B8000, 0xC59C4000, 0xC63B8000]]},     # outside the range, and nothing is lost
    # Twenty vectors is 1260 bits, past the 116 byte small-packet limit, so the group splits. The
    # values come from a rule both sides apply identically, so no float arithmetic describes the input.
    {'name': 'twenty vectors: the group splits on a word boundary',
     'w': [(SET, 0)] + [(VEC_POS, i) for i in range(20)], 'r': [(VEC_POS, i) for i in range(20)],
     'values': [[0x449A5000 + i * 0x1000, 0x42290000 - i * 0x100, 0xC5B17100 + i * 0x100]
                for i in range(20)]},

    # --- UnitConvert at 11 bits, and netLOD as the reader observes it from the wire ---
    {'name': 'UnitConvert at 11 bits: interior, code 0, the top code',
     'w': [(PRECISION, 0), (VEC_UNIT, 0), (VEC_UNIT, 1), (VEC_UNIT, 2)],
     'r': [(READ, 0), (VEC_UNIT, 0), (VEC_UNIT, 1), (VEC_UNIT, 2)],
     'values': [[0x3F000000, 0xBF800000, 0x3F800000],       # 0.5, -1.0, 1.0: interior, 0, (1<<11)-1
                [0x00000000, 0xBF800000, 0x3F800000],       # 0.0 is interior
                [0xBFCCCCCD, 0xBF77EDFB, 0x3F000000]]},     # -1.6, -0.9684750437736511, 0.5
    {'name': 'the wire carries 2: UnitConvert and the position converters both take the long path',
     'w': [(WIRE, 2), (VEC_UNIT, 0), (VEC_POS, 1)], 'r': [(READ, 0), (VEC_UNIT, 0), (VEC_POS, 1)],
     'values': [[0x3F000000, 0xBF800000, 0x3F800000], [0x449A5000, 0x42290000, 0xC5B17100]]},
    {'name': 'the wire carries 3',
     'w': [(WIRE, 3), (VEC_UNIT, 0), (VEC_POS, 1)], 'r': [(READ, 0), (VEC_UNIT, 0), (VEC_POS, 1)],
     'values': [[0x3F000000, 0xBF800000, 0x3F800000], [0x449A5000, 0x42290000, 0xC5B17100]]},
    {'name': 'the second field says unchanged: the reader keeps what it has',
     'w': [(SET, 1), (PRECISION, 0), (PRECISION, 0), (VEC_UNIT, 0), (VEC_POS, 1)],
     'r': [(READ, 0), (READ, 0), (VEC_UNIT, 0), (VEC_POS, 1)],
     'values': [[0x3F000000, 0xBF800000, 0x3F800000], [0x449A5000, 0x42290000, 0xC5B17100]]},
    # A group whose first field is the "unchanged" bit can only be written after an earlier group set
    # lastNetLOD, so a reader that joins mid-connection is a multi-group question. Not modelled here.
    # What is modelled is a reader that starts from BeginNetLOD and then reads what the wire carries.
    {'name': 'a reader that starts fresh, then reads what the wire carries',
     'w': [(PRECISION, 0), (VEC_UNIT, 0)], 'r': [(BEGIN, 0), (READ, 0), (VEC_UNIT, 0)],
     'values': [[0x3F000000, 0xBF800000, 0x3F800000]]},

    # --- the per-client decision, and what it does to the same value ---
    # The assignment at NetState.cpp:1802 is inside a game class, so it is quoted here, not run. What is
    # run is the rule itself, through the engine's own IsLocalTo, and the consequence for the bytes.
    # Each of the first two scenarios has a reader that agrees with its writer, which is the normal case.
    {'name': 'not local to this client: the gated converters stay short',
     'w': [(DECISION, 0), (FIELD, enc(THRUST, 0)), (FIELD, enc(ANGLE, 0))], 'r': [(GET, enc(THRUST, 0)), (GET, enc(ANGLE, 0))],
     'values': [V03]},
    {'name': 'the same client: the gated converters take raw IEEE bits, the other two do not',
     'w': [(DECISION, 1), (FIELD, enc(THRUST, 0)), (FIELD, enc(ANGLE, 0))], 'r': [(GET, enc(THRUST, 0)), (GET, enc(ANGLE, 0))],
     'values': [V03]},
    # The same float, encoded both ways in one group, in the order the source does it: keep what netLOD
    # was, run the rule, then write. The bytes differ for the gated converters and do not for the
    # other two, which is the point of the comparison.
    {'name': 'the same float, encoded both ways in one group through the engine\'s own WriteFixed',
     'w': [(SET, 0), (DECISION, 0), (FIELD, enc(THRUST, 0)), (FIELD, enc(ANGLE, 0)),
           (FIELD, enc(JETFUEL, 0)), (FIELD, enc(SWITCH, 0)),
           (DECISION, 1), (FIELD, enc(THRUST, 0)), (FIELD, enc(ANGLE, 0)),
           (FIELD, enc(JETFUEL, 0)), (FIELD, enc(SWITCH, 0))],
     'r': [(GET, enc(THRUST, 0)), (GET, enc(ANGLE, 0)), (GET, enc(JETFUEL, 0)), (GET, enc(SWITCH, 0)),
           (GET, enc(THRUST, 0)), (GET, enc(ANGLE, 0)), (GET, enc(JETFUEL, 0)), (GET, enc(SWITCH, 0))],
     'values': [V03]},
    {'name': 'a guest slot: the rule reaches one entry and matches',
     'w': [(DECISION, 2), (FIELD, enc(THRUST, 0))], 'r': [(GET, enc(THRUST, 0))],
     'values': [V03]},
    {'name': 'a guest table that does not match: the rule says no',
     'w': [(DECISION, 3), (FIELD, enc(THRUST, 0))], 'r': [(GET, enc(THRUST, 0))],
     'values': [V03]},
    {'name': 'a player id past the array: the bounds check answers before the table',
     'w': [(DECISION, 4), (FIELD, enc(THRUST, 0))], 'r': [(GET, enc(THRUST, 0))],
     'values': [V03]},
    {'name': 'three converters that do not change with precision',
     'w': [(SET, 1), (DECISION, 5), (FIELD, enc(JETFUEL, 0)), (FIELD, enc(SWITCH, 0)), (FIELD, enc(QUAT, 1))],
     'r': [(GET, enc(JETFUEL, 0)), (GET, enc(SWITCH, 0)), (GET, enc(QUAT, 1))],
     'values': [V03, VQUAT]},

    # --- the quaternion path, 13 bits, not gated ---
    {'name': 'quaternion X, Y, Z, S in that order: code 0 and the top code, and an interior rounding',
     'w': [(FIELD, enc(QUAT, 0))], 'r': [(GET, enc(QUAT, 0))],
     'values': [VQUAT]},
    {'name': 'a quaternion component that rounds onto the reserved code 0',
     'w': [(FIELD, enc(QUAT, 0))], 'r': [(GET, enc(QUAT, 0))],
     'values': [[0xBF7FFCB9, 0x3F333333, 0x3ECCCCCD, 0xBF3FEC57]]},   # -0.99995, 0.7, 0.4, -0.7497

    # --- across a sequence of groups, through the engine's own four functions ---
    # The canonical order in the source is SetNetLOD then SetNetPrecision at the group start, then the
    # fields (NetState.cpp:1077), and ReadNetLOD at the group start then the fields (:1162). The field is
    # written at the point the writer reaches it, and the reader has to read it at the same point.
    {'name': 'three groups in order: A sets the precision, B says unchanged, C changes it again',
     'ws': 0, 'rs': 2,
     'w': [(SET, 1), (PRECISION, 0), (FIELD, enc(THRUST, 0)), (GROUP, 0),
           (PRECISION, 0), (FIELD, enc(THRUST, 0)), (GROUP, 0),
           (SET, 0), (PRECISION, 0), (FIELD, enc(THRUST, 0))],
     'r': [(READ, 0), (GET, enc(THRUST, 0)), (GROUP, 0),
           (READ, 0), (GET, enc(THRUST, 0)), (GROUP, 0),
           (READ, 0), (GET, enc(THRUST, 0))],
     'values': [V03]},
    # A group that arrives in pieces: the state can only be taken from it once it is complete, and then
    # it has to survive to the next group. Values come from the same rule both halves apply.
    {'name': 'a group that arrives in two packets, then a group that says unchanged',
     'ws': 0, 'rs': 2, 'rule': 15,
     'w': [(SET, 0), (PRECISION, 0)] + [(VEC_POS, i) for i in range(15)] + [(GROUP, 0),
           (PRECISION, 0), (VEC_POS, 15)],
     'r': [(READ, 0)] + [(VEC_POS, i) for i in range(15)] + [(GROUP, 0),
           (READ, 0), (VEC_POS, 15)],
     'values': [[0x449A5000 + i * 0x1000, 0x42290000 - i * 0x100, 0xC5B17100 + i * 0x100]
                for i in range(15)] + [[0xBF800000, 0x3F000000, 0xBF400000]]},
    # The shape MountedTurret::Write has at NetState.cpp:1798: the value is changed inside the group and
    # the fields written after it use the new value. Here the change comes after the field, so the field
    # cannot carry it, and a reader that read the field first has nothing to tell it.
    {'name': 'the change happens inside the group, after the field: the field cannot carry it',
     'ws': 0, 'rs': 2,
     'w': [(SET, 0), (PRECISION, 0), (FIELD, enc(THRUST, 0)), (DECISION, 1),
           (FIELD, enc(THRUST, 0)), (FIELD, enc(SWITCH, 0))],
     'r': [(READ, 0), (GET, enc(THRUST, 0)), (GET, enc(THRUST, 0)), (GET, enc(SWITCH, 0))],
     'values': [V03]},
    # The same group with the change before the field: the field carries it, and the reader agrees.
    {'name': 'the same group with the change before the field: the field carries it, and the reader agrees',
     'ws': 0, 'rs': 2,
     'w': [(SET, 0), (DECISION, 1), (PRECISION, 0), (FIELD, enc(THRUST, 0)), (FIELD, enc(SWITCH, 0))],
     'r': [(READ, 0), (GET, enc(THRUST, 0)), (GET, enc(SWITCH, 0))],
     'values': [V03]},
    # The same two groups read by a reader that carried its state across the run, which is what a reader
    # that joined after an earlier group actually has.
    {'name': 'the same bytes, read by a reader that carries the state from the earlier group',
     'ws': 0, 'rs': 1,
     'w': [(SET, 1), (PRECISION, 0), (FIELD, enc(THRUST, 0)), (GROUP, 0), (PRECISION, 0), (FIELD, enc(THRUST, 0))],
     'r': [(GROUP, 0), (READ, 0), (GET, enc(THRUST, 0))],
     'values': [V03]},
    # A reader that joins with no earlier group at all: the only thing it has is what its own state
    # already is, which is measured here by the engine's own BeginNetLOD, not assumed. It never saw the
    # group that established the value, so the field it does receive can only say keep what you have.
    {'name': 'the same bytes, read by a reader that joined and never saw an earlier group',
     'ws': 0, 'rs': 2,
     'w': [(SET, 1), (PRECISION, 0), (FIELD, enc(THRUST, 0)), (GROUP, 0), (PRECISION, 0), (FIELD, enc(THRUST, 0))],
     'r': [(GROUP, 0), (READ, 0), (GET, enc(THRUST, 0))],
     'values': [V03]},
]


def decision_record(state, index):
    player, other = DECISIONS[index]
    g = get_my_guest(player, 0)
    r = is_local_to(player, other)
    before = state[0]
    # The effect of NetState.cpp:1802: the model applies the same assignment; the engine half cannot
    # run the assignment itself because MountedTurret is game layer.
    state[0] = 1 if r else before
    return {'player': player, 'other': other, 'guest': g, 'result': r,
            'netLOD_before': before, 'netLOD_after': state[0]}


def split_groups(steps):
    """A group boundary is a step, so both halves walk the same structure."""
    out, cur = [], []
    for e in steps:
        if (e[0] if isinstance(e, tuple) else e) == GROUP:
            out.append(cur)
            cur = []
        else:
            cur.append(e)
    out.append(cur)
    return out


# ws: 0 = the state at the start is what BeginNetLOD gives, 1 = carry it from the run.
# rs: 0 = continue from the writer half, 1 = carry it from the run, 2 = what BeginNetLOD gives.
# rule: how many value rows this file builds from the rule both halves apply.
NO_CARRY = False
LATE_APPLY = False


def build(scenario, session=None):
    session = list(session or [0, 0])
    wg = split_groups(scenario['w'])
    rg = split_groups(scenario['r'])

    # The writer half runs first, and its state evolves from group to group. The reader half then runs
    # from a stated start and after that is changed only by this model's own reading of ReadNetLOD -
    # the engine half does the same and says so. A reader that joins mid-connection has only whatever
    # it already had, because an "unchanged" field carries no value.
    state = list(session) if scenario.get('ws') == 1 else [0, 0]
    fields_write, decisions = [], []
    datagrams, pkt_group, meta = [], [], []

    for gi, steps in enumerate(wg):
        if NO_CARRY:
            state = [0, 0]
        pool = Pool()
        g = Group(pool, True)
        g.begin_write()
        m = {'idx': gi, 'writer_before': list(state), 'first_bit': None, 'field_bits': 0,
             'reader_before': None, 'reader_after': None}
        for op, arg in steps:
            if op == BEGIN:
                state[:] = [0, 0]
                continue
            if op == SET:
                state[0] = int(bool(arg))          # SetNetLOD takes a bool: 0 or 1, nothing else
                continue
            if op == PRECISION:                    # SetNetPrecision, NetState.cpp:296
                if state[0] == state[1]:
                    g.write_bits(1, 1)
                else:
                    g.write_bits(0, 1)
                    g.write_bits(state[0], 2)
                    state[1] = state[0]
                continue
            if op == WIRE:
                # A field placed on the wire as the wire would carry it. No engine path in this tree can
                # write 2 or 3: SetNetLOD takes a bool, and ReadNetLOD only propagates what it read.
                g.write_bits(0, 1)
                g.write_bits(arg, 2)
                continue
            if op == DECISION:
                decisions.append(decision_record(state, arg))
                continue
            order = order_for(op, arg)
            for k in range(len(order)):
                r = field_record(state[0], order[k], scenario['values'][value_row(op, arg)][k])
                fields_write.append(r)
                g.write_bits(r['code'], r['bits'])
        g.end_write()
        cnt = g.count
        for i in range(cnt):
            p = g.pop_front()
            p.data[0:8] = bytes(8)
            p.data[0] = SEQ & 0xFF
            p.data[1] = (SEQ >> 8) & 0xFF
            p.data[2] = TYPE
            p.data[3] = SRC
            p.data[4] = i
            p.data[5] = cnt
            p.data[6] = p.data[7] = 0
            datagrams.append({'bytes': p.header_bytes() + bytes(p.data[14:p.max_index])})
            pkt_group.append(gi)
        meta.append(m)

    count = len(datagrams)
    completed, fields_read, observed = [], [], []
    nWrite = 0                       # the field index runs across groups, in the order both record
    reader_state = ([0, 0] if scenario.get('rs') == 2 else
                    list(session) if scenario.get('rs') == 1 else list(state))

    for gi in range(len(wg)):
        idxs = [i for i, g in enumerate(pkt_group) if g == gi]
        complete, completed_g, _ = reassemble([datagrams[i] for i in idxs], list(range(len(idxs))))
        if not complete.count:
            return {'error': 'the group never completed'}
        # The bytes the model wrote are what it reads, and the field it reads is the field the bytes
        # carry: nothing here is assumed about which branch SetNetPrecision took.
        complete.begin_read()
        first = complete.read_bits(1)
        meta[gi]['first_bit'] = first
        meta[gi]['field_bits'] = 1
        if not first:
            complete.read_bits(2)
            meta[gi]['field_bits'] = 3
        completed.extend(completed_g)
        # Pass one: the codes, read back out of the bytes with the widths the writer used.
        complete.begin_read()
        for entry in wg[gi]:
            op = entry[0]
            if op in (PRECISION, WIRE):
                if not complete.read_bits(1):
                    complete.read_bits(2)
                continue
            if op in (BEGIN, SET, DECISION):
                continue
            order = order_for(op, entry[1])
            for k in range(len(order)):
                fields_write[nWrite]['code'] = complete.read_bits(fields_write[nWrite]['bits'])
                nWrite += 1

        # Pass two: the same bytes, read with this model's own reading of ReadNetLOD and of the readers.
        if NO_CARRY:
            reader_state = [0, 0]
        st = list(reader_state)
        meta[gi]['reader_before'] = list(st)
        pending = None
        complete.begin_read()
        for entry in rg[gi]:
            op = entry[0]
            if op == BEGIN:
                st[:] = [0, 0]
                continue
            if op == READ:                         # ReadNetLOD, NetState.cpp:309
                if not complete.read_bits(1):
                    new = complete.read_bits(2)
                    if LATE_APPLY:
                        pending = new
                    else:
                        st[1] = new
                        st[0] = st[1]
                observed.append([st[0], st[1]])
                continue
            if op == DECISION:
                continue
            order = order_for(op, entry[1])
            for k in range(len(order)):
                c = CONVERTERS[order[k]]
                b = bits_for(st[0], order[k])
                code = complete.read_bits(b)
                fields_read.append({'name': order[k], 'bits': b,
                                    'decoded': fixed_decode(code, c['min'], c['max'], b)})
        if LATE_APPLY and pending is not None:
            st[1] = pending
            st[0] = st[1]
        meta[gi]['reader_after'] = list(st)
        reader_state = list(st)

    out = {
        'name': scenario['name'],
        'steps': {'w': [[e[0], e[1]] for e in scenario['w']], 'r': [[e[0], e[1]] for e in scenario['r']]},
        'values': scenario['values'],
        'packets': count,
        'bytes': [''.join('%02x' % b for b in d['bytes']) for d in datagrams],
        'complete_after_each': completed,
        'state_after_write': list(state),
        'state_after_read': list(reader_state),
        'observed': observed,
        'fields_write': fields_write,
        'fields_read': fields_read,
    }
    if decisions:
        out['decision'] = decisions
    if len(wg) > 1:
        out['groups'] = meta
    # The length of what the reader got, before anything normalises it. The engine path that would
    # normalise is D3DXQuaternionNormalize, which is not code available to this check, so this is the
    # model's own float32 arithmetic on the decoded components, and the runner says so.
    quat = [f for f in fields_read if f['name'] == 'quat']
    if quat:
        v = [to_float(f['decoded']) for f in quat]
        sumsq = c_float(c_float(c_float(c_float(v[0] * v[0]) + c_float(v[1] * v[1]))
                                 + c_float(v[2] * v[2])) + c_float(v[3] * v[3]))
        out['quat_components'] = [fbits(x) for x in v]
        out['quat_length_before_normalisation'] = fbits(c_float(math.sqrt(sumsq)))
    return out


def risk():
    """Interior codes where a model computing in double lands on a different float32 value."""
    return {
        '11 bits, -1 to 1 (UnitConvert)': double_model_divergence(11, -1.0, 1.0),
        '21 bits, -10240 to 10240 (PositionXZConvert)': double_model_divergence(21, -10240.0, 10240.0),
        '21 bits, -1024 to 1024 (PositionYConvert)': double_model_divergence(21, -1024.0, 1024.0),
        '13 bits, -1 to 1 (QuatConvert)': double_model_divergence(13, -1.0, 1.0),
        '9 bits, -pi to pi (AngleConvert)': double_model_divergence(9, PI, -PI),
        '7 bits, -1 to 1 (ThrustConvert)': double_model_divergence(7, -1.0, 1.0),
        '4 bits, -1 to 1 (SwitchConvert)': double_model_divergence(4, -1.0, 1.0),
        'note': 'the engine does every step in float32; a model in double is a stronger arithmetic '
                'and agrees only when the last bit lands the same way. JetFuel at 11 bits in [0, 1] '
                'has no such code: its divisor is a power of two.',
    }


CONVERTERS_NAMES = {0: 'unit', 1: 'positionXZ', 2: 'positionY', 3: 'switch', 4: 'thrust',
                    5: 'angle', 6: 'jetfuel', 7: 'quat'}


def main():
    session = [0, 0]
    out = {
        'input': {'type': TYPE, 'src': SRC, 'group_sequence': SEQ, 'dohires': 0,
                  'note': 'netLOD is observed by the engine through its own BeginNetLOD, SetNetLOD, '
                          'SetNetPrecision and ReadNetLOD; this model is a second reading of the same '
                          'rules. Values 2 and 3 cannot come from SetNetLOD, which takes a bool, so '
                          'the check puts them on the wire directly and says so. The guest table is an '
                          'input both halves are given; what is compared is each side\'s own reading '
                          'of the rule that reads it'},
        'scenarios': [],
    }
    for sc in SCENARIOS:
        r = build(sc, session)
        out['scenarios'].append(r)
        if 'error' not in r:
            session = list(r['state_after_read'])
    if '--risk' in sys.argv:
        out['risk'] = risk()
    if '--json' in sys.argv:
        print(json.dumps(out, ensure_ascii=False))
        return 0
    for s in out['scenarios']:
        print('%s (%d packets) netLOD after read %d' % (s['name'], s.get('packets', 1),
                                                        s['state_after_read'][0]))
        for d in s.get('decision', []):
            print('  decision: player %d, other %d, guest %d -> %s, netLOD %d -> %d' % (
                d['player'], d['other'], d['guest'], d['result'],
                d['netLOD_before'], d['netLOD_after']))
        for f in s['fields_read']:
            print('  %s in %.1f..%.1f at %d bits: -> %s' % (
                f['name'], CONVERTERS[f['name']]['min'], CONVERTERS[f['name']]['max'],
                f['bits'], to_float(f['decoded'])))
    if '--risk' in sys.argv:
        print('\nrisk:', json.dumps(out['risk'], indent=2))
    return 0


if __name__ == '__main__':
    if '--wrong-bits' in sys.argv:
        WRONG_BITS = int(sys.argv[sys.argv.index('--wrong-bits') + 1])
    if '--gate-everything' in sys.argv:
        GATE_EVERYTHING = True
    if '--no-carry' in sys.argv:
        NO_CARRY = True
    if '--late-apply' in sys.argv:
        LATE_APPLY = True
    sys.exit(main())
