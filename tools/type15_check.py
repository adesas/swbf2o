#!/usr/bin/env python3
"""The independent model for PKT_GAME_REPLICA_UPDATE (type 15).

The engine's readers and writers for this packet live in NetGame.cpp. This file is a second reading
of the same rules, written from the source, so that the two can be compared. If they agree, the
format is pinned; if they disagree, one of the two readings is wrong - which a round trip cannot
show, because a writer and a reader that are wrong in the same way still round trip.

The rules, from the source:

  destination mask    32 bits, then 32 bits - the mask is 64 bits wide
  turn                11 bits, with 0x7ff meaning "no turn" and 0x7fe meaning escape plus one bit,
                      then a rollover correction against the previous turn
  client turn         the same encoding again
  lag                 8 bits, and 128 is subtracted
  switch response     one bit, then a turn
  moves               one bit "same" (copy the previous inputs), then one bit "end", then four fixed
                      fields - stick x and y at 7 bits in [-1,1], mouse x and y at 11 bits in
                      [-16,16] - then the buttons as 30 bits and 2 bits

The fixed fields reserve their endpoints: code 0 means the minimum and code (1 << bits) - 1 means the
maximum. Only interior codes are decoded as code / ((1 << bits) - 2) * span + min, and that
arithmetic is float32, because that is what the engine does.

Usage: python3 tools/type15_check.py [--json]
"""
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from net_wire_check import (Group, Pool, fixed_code, fixed_decode, fbits, c_float,  # noqa: E402
                        make_received)

SEQ = 77
TYPE = 15          # PKT_GAME_REPLICA_UPDATE
SRC = 3

SR_NONE, SR_CAN, SR_CANT = 0, 1, 2


# ---------------------------------------------------------------- writers

def write_turn_low_bits(g, turn):
    """WriteTurnLowBits: a turn whose low 11 bits are 0x7ff or 0x7fe need the escape form."""
    bLast = False
    uTurn = turn & 0xFFFFFFFF
    if uTurn != 0xFFFFFFFF:
        if (uTurn & 0x7FF) == 0x7FF:
            bLast = False
            uTurn = 0xFFFFFFFE
        elif (uTurn & 0x7FF) == 0x7FE:
            bLast = True
            uTurn = 0xFFFFFFFE
    g.write_bits(uTurn, 11)
    if uTurn == 0xFFFFFFFE:
        g.write_bits(1 if bLast else 0, 1)


def write_switch_response(g, iResponse, iTurn):
    """WriteSwitchResponse: a response of 0 means no turn at all."""
    bCan = 1 if iResponse == 1 else 0
    if iResponse == 0:
        iTurn = 0xFFFFFFFF
    g.write_bits(bCan, 1)
    write_turn_low_bits(g, iTurn)


def inputs_key(inputs):
    """IsSame compares the mouse mode, the 30 button bits and the four floats."""
    x, y, mx, my, buttons, mouse_mode = inputs
    return (fbits(x), fbits(y), fbits(mx), fbits(my), buttons & 0x3FFFFFFF, mouse_mode & 3)


def write_move(g, prev, cur):
    """WriteMove: "same" copies the previous inputs; a null current ends the list."""
    if cur is None:
        g.write_bits(0, 1)
        g.write_bits(1, 1)
        return
    if inputs_key(prev) == inputs_key(cur):
        g.write_bits(1, 1)
        return
    g.write_bits(0, 1)
    g.write_bits(0, 1)
    g.write_bits(fixed_code(cur[0], -1.0, 1.0, 7), 7)
    g.write_bits(fixed_code(cur[1], -1.0, 1.0, 7), 7)
    g.write_bits(fixed_code(cur[2], -16.0, 16.0, 11), 11)
    g.write_bits(fixed_code(cur[3], -16.0, 16.0, 11), 11)
    # the button word is 30 bits of mBoolInputs and 2 bits of mMouseMode
    g.write_bits(cur[4] & 0x3FFFFFFF, 30)
    g.write_bits(cur[5] & 3, 2)


# ---------------------------------------------------------------- readers

def read_turn_low_bits(g, turn_high_bits):
    """ReadTurnLowBits: 0x7ff is "no turn", 0x7fe escapes with one bit, and the low bits are
    corrected against the high bits of the previous turn."""
    low = g.read_bits(11)
    if low == 0x7FF:
        return 0xFFFFFFFF
    if low == 0x7FE:
        escape = g.read_bits(1)
        low = 0x7FE if escape else 0x7FF
    turn = (turn_high_bits & 0xFFFFF800) | low
    previous_low = turn_high_bits & 0x7FF
    if previous_low >= 0x400:
        if low < previous_low - 0x400:
            turn += 0x800
    elif low > previous_low + 0x400:
        turn -= 0x800
    return turn & 0xFFFFFFFF


def read_switch_response(g, turn, turn_high_bits):
    can = g.read_bits(1)
    response_turn = read_turn_low_bits(g, turn_high_bits)
    if can:
        return SR_CAN if response_turn == turn else SR_NONE
    if response_turn != 0xFFFFFFFF and response_turn == turn:
        return SR_CANT
    return SR_NONE


def read_move(g, previous):
    same = g.read_bits(1)
    if same:
        return previous, True
    end = g.read_bits(1)
    if end:
        return None, False
    x = fixed_decode(g.read_bits(7), -1.0, 1.0, 7)
    y = fixed_decode(g.read_bits(7), -1.0, 1.0, 7)
    mx = fixed_decode(g.read_bits(11), -16.0, 16.0, 11)
    my = fixed_decode(g.read_bits(11), -16.0, 16.0, 11)
    low = g.read_bits(30)
    high = g.read_bits(2)
    word = (low & 0x3FFFFFFF) | ((high & 3) << 30)
    return (x, y, mx, my, word & 0x3FFFFFFF, (word >> 30) & 3), True


# ---------------------------------------------------------------- scenarios

SCENARIOS = [
    {
        'name': 'ordinary',
        'turnHighBits': 0,
        'destMask': 0x0000000000000003,
        'turn': 0x123,
        'clientTurn': 0x120,
        'lag': 200,
        'switchResponse': 1,
        'switchTurn': 0x123,
        'first': (-0.5, 0.25, -3.0, 12.0, 0x12345678, 0),
        'second': (-0.5, 0.25, -3.0, 12.0, 0x12345678, 0),
    },
    {
        'name': 'reserved',
        'turnHighBits': 0x400,
        'destMask': 0x0000000000000001,
        'turn': 0xFFFFFFFF,
        'clientTurn': 0x7FE,
        'lag': 128,
        'switchResponse': 0,
        'switchTurn': 0x123,
        'first': (0.75, -1.0, 16.0, -16.0, 0x3FFFFFFF, 3),
        'second': (0.75, -1.0, 16.0, -16.0, 0x3FFFFFFF, 3),
    },
]

ZERO = (0.0, 0.0, 0.0, 0.0, 0, 0)


def build(scenario):
    """Write the payload and read it back, exactly as the engine's handler does."""
    pool = Pool()
    g = Group(pool, True)
    g.begin_write()

    g.write_bits(scenario['destMask'] & 0xFFFFFFFF, 32)
    g.write_bits((scenario['destMask'] >> 32) & 0xFFFFFFFF, 32)
    write_turn_low_bits(g, scenario['turn'])
    write_turn_low_bits(g, scenario['clientTurn'])
    g.write_bits(scenario['lag'], 8)
    write_switch_response(g, scenario['switchResponse'], scenario['switchTurn'])
    write_move(g, ZERO, scenario['first'])
    write_move(g, scenario['first'], scenario['second'])
    write_move(g, scenario['second'], None)
    g.end_write()

    # the datagram, as NetComm::Send would number it
    count = g.count
    datagrams = []
    for i in range(count):
        p = g.pop_front()
        p.data[0:8] = bytes(8)
        p.data[0] = SEQ & 0xFF
        p.data[1] = (SEQ >> 8) & 0xFF
        p.data[2] = TYPE
        p.data[3] = SRC
        p.data[4] = i
        p.data[5] = count
        p.data[6] = p.data[7] = 0
        datagrams.append(p.header_bytes() + bytes(p.data[14:p.max_index]))

    # now read it back through the same receive path the engine uses
    pool2 = Pool()
    complete = Group(pool2, True)
    pkt, _prefix = make_received(pool2, datagrams[0])
    pkt.data[3] = SRC
    complete.set(pkt)
    complete.begin_read()

    high = scenario['turnHighBits']
    mask = complete.read_bits(32) | (complete.read_bits(32) << 32)
    turn = read_turn_low_bits(complete, high)
    client_turn = read_turn_low_bits(complete, high)
    lag = complete.read_bits(8)
    response = read_switch_response(complete, turn, high)

    previous = ZERO
    moves = []
    ended = False
    for _ in range(4):
        current, keep = read_move(complete, previous)
        if not keep:
            ended = True
            break
        moves.append(current)
        previous = current

    return {
        'name': scenario['name'],
        'bytes': ''.join('%02x' % b for b in datagrams[0]),
        'mask': mask,
        'turn': turn,
        'client_turn': client_turn,
        'lag': lag - 128,
        'switch_response': response,
        'moves': len(moves),
        'ended': 1 if ended else 0,
        'inputs': [
            {'stick_x': m[0], 'stick_y': m[1], 'mouse_x': m[2], 'mouse_y': m[3],
             'buttons': m[4] & 0x3FFFFFFF, 'mouse_mode': m[5]}
            for m in moves
        ],
    }


def main():
    out = {'scenarios': [build(s) for s in SCENARIOS]}
    if '--json' in sys.argv:
        print(json.dumps(out, ensure_ascii=False))
    else:
        for s in out['scenarios']:
            print(s['name'], s['bytes'])
            print('   mask', s['mask'], 'turn', s['turn'], 'client', s['client_turn'],
                  'lag', s['lag'], 'response', s['switch_response'],
                  'moves', s['moves'], 'ended', s['ended'])
    return 0


if __name__ == '__main__':
    sys.exit(main())
