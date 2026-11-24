#!/usr/bin/env python3
"""An independent implementation of the net packet wire format, read from the engine's code.

This is the oracle half of the check; the other half (spike/net_wire.cpp, run in node) calls the
engine's own functions. Agreement is evidence about the format because they are two separate readings
of the same source. A round-trip on its own would not be: an encoder and a decoder that are wrong in
the same way still round-trip.

The rules, each traced to the function it came from:

  NetPacket::BeginWrite     clears buffer bytes 0..7, presets the group bytes to 0xff, sets data
                            index 14 and the byte cursor to 14, and sets the limit to 116 for a
                            small packet or 1012 for a big one.
  NetPktGroup::WriteBits    LSB-first inside a 32-bit word; the word is flushed when it fills and
                            the value is shifted after each part, so a value crossing a word boundary
                            is still recovered. A field under 32 bits therefore usually starts
                            mid-word.
  NetPktGroup::WritePacked  allocates the packet on the first flush, so a group of only bit writes
                            has no packet until a word completes; writes the accumulated word as a
                            whole word, and on overflow closes the current packet and starts a new
                            one, so splitting happens on word boundaries.
  NetPktGroup::WriteBytes   pads the current word with zero bits first, so raw bytes always begin at
                            a word boundary, then writes them at the byte cursor.
  NetPktGroup::ReadBytes    discards the rest of the word already read, then reads raw bytes.
  NetPktGroup::WriteFixed   >= 32 bits or doHiRes: raw IEEE bits. Otherwise 0 for <= fMin,
                            (1<<bits)-1 for >= fMax, else trunc(((1<<bits)-2)*(f-fMin)/(fMax-fMin)
                            + 0.5). ReadFixed maps 0 -> fMin and (1<<bits)-1 -> fMax and divides by
                            (1<<bits)-2, which is why those codes are reserved for the endpoints.
  PackHeader                the wire header is 5 bytes: type, group as two bytes little-endian, idx,
                            count | (compressed << 7). The 8-byte PktHeader is a view over the buffer,
                            not the wire layout.
  UnpackHeader              rebuilds the in-memory header from the 5 wire bytes and moves the data
                            index to just after them.
  MakeReceivedPacket        copies the datagram to buffer + prefix, prefix = 3, or 0 when the first
                            byte - the type - is >= 0x1e; comm index = prefix.
  NetComm::Send(iDest,uType,pGroup)  numbers the group and turns each packet in the group into its
                            own datagram with count = packets in the group and idx = its position.
  RecvGroup                 reassembles by (group sequence, type, src): one packet is complete at
                            once; otherwise fragments may arrive in any order, are reordered by idx,
                            and the group is complete only when all count fragments are present.
  Deliver                   what goes on the wire is buffer + comm index .. max index. In this tree
                            Encrypt/Decrypt return the packet unchanged, so the wire bytes are the
                            bytes as written.

Usage: python3 tools/net_wire_check.py [--json]
browser/net_wire_check.mjs compares this against the engine's own code.
"""
import json
import math
import struct
import sys

MASK32 = 0xFFFFFFFF
MAX_BIG = 0x100
MAX_SMALL = 0x100

BOOL, BITS, FIXED, BYTES, FLOATBITS = 0, 1, 2, 3, 4

TYPE = 2      # PKT_QUERY: below 0x1e, so the receive prefix is 3
SRC = 64      # host destination, so Send numbers the group
SEQ = 7       # what nextGroupNum would give

# The operations, in wire order. spike/net_wire.cpp encodes the same list. The last two force a
# second packet, which is where the count/idx numbering and the reassembly matter.
OPS = [
    {'kind': BOOL, 'value': 1, 'bits': 1},
    {'kind': BITS, 'value': 0xDEADBEEF, 'bits': 20},
    {'kind': BITS, 'value': 12345, 'bits': 32},
    {'kind': FIXED, 'f': 0.25, 'min': -1.0, 'max': 1.0, 'bits': 10},
    {'kind': FIXED, 'f': 9.0, 'min': 0.0, 'max': 10.0, 'bits': 10},   # interior, not above max
    {'kind': FIXED, 'f': -1.0, 'min': 0.0, 'max': 10.0, 'bits': 10},
    {'kind': FIXED, 'f': 10.0, 'min': 0.0, 'max': 10.0, 'bits': 10},
    # code 17 of 126: a model computing in double decodes this to 0xbf3aebaf, the engine to 0xbf3aebae
    {'kind': FIXED, 'f': -0.7380899786949158, 'min': -1.0, 'max': 1.0, 'bits': 7},
    {'kind': FIXED, 'f': 1.5, 'min': -1.0, 'max': 1.0, 'bits': 7, 'hires': True},
    {'kind': BITS, 'value': 7, 'bits': 3},
    {'kind': BYTES, 'n': 4},
    {'kind': FLOATBITS, 'f': 1.5, 'bits': 32},
    {'kind': BYTES, 'n': 100},
    {'kind': BITS, 'value': 0x1234, 'bits': 12},
]


def pattern(n):
    """The raw bytes, generated by the same rule on both sides."""
    return bytes(((i * 3 + 1) & 0xFF) for i in range(n))


def fbits(f):
    return struct.unpack('<I', struct.pack('<f', f))[0]


def c_float(x):
    return struct.unpack('<f', struct.pack('<f', x))[0]


def op_field(op):
    kind = op['kind']
    value = fbits(op['f']) if kind in (FIXED, FLOATBITS) else (0 if kind == BYTES else op['value'] & MASK32)
    declared = op.get('bits', 0)
    effective = 32 if (op.get('hires') or declared >= 32) else declared
    return {'kind': kind, 'bits': declared, 'value': value,
            'min': fbits(op['min']) if kind == FIXED else 0,
            'max': fbits(op['max']) if kind == FIXED else 0,
            'bytes_count': op.get('n', 0),
            'bytes': list(pattern(op['n'])) if kind == BYTES else [],
            'hires': 1 if op.get('hires') else 0,
            'effective_bits': effective}


def pattern_value_hex(op):
    return ''.join(f'{b:02x}' for b in pattern(op['n']))


def c_uint(x):
    return int(math.floor(x)) & MASK32 if x >= 0 else (-int(math.floor(-x))) & MASK32


def fixed_code(f, fmin, fmax, bits, hires=False):
    """Step by step as the engine writes it, in float32. Not "the same maths in a stronger type":
    the engine does every step in float32, and that is what makes it reproducible. Doing it in
    double agrees only when the last bit happens to land the same way."""
    if hires or bits >= 32:
        return fbits(f)
    f, fmin, fmax = c_float(f), c_float(fmin), c_float(fmax)
    if f <= fmin:
        return 0
    if f >= fmax:
        return 0xFFFFFFFF
    span = c_float(float((1 << bits) - 2))
    t = c_float(c_float(f - fmin) / c_float(fmax - fmin))
    v = c_float(c_float(span) * t)
    v = c_float(v + 0.5)
    return int(v)                      # the C cast truncates toward zero


def fixed_decode(code, fmin, fmax, bits, hires=False):
    if hires or bits >= 32:
        return code                    # raw IEEE bits, no quantisation
    umax = (1 << bits) - 1
    if code == 0:
        return fbits(fmin)
    if code == umax:
        return fbits(fmax)
    a = c_float(c_float(float(code)) / c_float(float(umax - 1)))
    b = c_float(a * c_float(fmax - fmin))
    return fbits(c_float(b + c_float(fmin)))


def double_model_divergence(bits, fmin, fmax):
    """How many interior codes a model that computes in double would get wrong."""
    umax = (1 << bits) - 1
    n = 0
    for code in range(1, umax):
        d = c_float(code / (umax - 1) * (fmax - fmin) + fmin)
        a = c_float(c_float(float(code)) / c_float(float(umax - 1)))
        f = c_float(c_float(a * c_float(fmax - fmin)) + c_float(fmin))
        if d != f:
            n += 1
    return n


class Packet:
    def __init__(self, num, small, pool):
        self.num = num
        self.small = small
        self.pool = pool
        self.data = bytearray(1024)
        self.next_pkt = -1
        self.prev_pkt = -1
        self.cur_index = 0
        self.max_index = 0
        self.comm_index = 0
        self.data_index = 0

    def limit(self):
        return 116 if self.small else 1012

    def begin_write(self):
        self.data[0:8] = bytes(8)
        self.data[0] = self.data[1] = 0xFF
        self.data_index = 14
        self.max_index = self.limit()
        self.cur_index = 14

    def end_write(self):
        self.max_index = self.cur_index

    def begin_read(self):
        self.cur_index = self.data_index

    def write_u32(self, v):
        if self.cur_index + 4 > self.max_index:
            return False
        self.data[self.cur_index:self.cur_index + 4] = (v & MASK32).to_bytes(4, 'little')
        self.cur_index += 4
        return True

    def write_u8(self, v):
        if self.cur_index + 1 > self.max_index:
            return False
        self.data[self.cur_index] = v & 0xFF
        self.cur_index += 1
        return True

    def write_u16(self, v):
        if self.cur_index + 2 > self.max_index:
            return False
        self.data[self.cur_index:self.cur_index + 2] = (v & 0xFFFF).to_bytes(2, 'little')
        self.cur_index += 2
        return True

    def write_raw(self, raw):
        end = min(self.cur_index + len(raw), self.max_index)
        n = end - self.cur_index
        self.data[self.cur_index:end] = raw[:n]
        self.cur_index += n
        return n

    def read_u32(self):
        if self.cur_index + 4 > self.max_index:
            return None
        v = int.from_bytes(self.data[self.cur_index:self.cur_index + 4], 'little')
        self.cur_index += 4
        return v

    def read_u8(self):
        if self.cur_index + 1 > self.max_index:
            return None
        v = self.data[self.cur_index]
        self.cur_index += 1
        return v

    def read_u16(self):
        if self.cur_index + 2 > self.max_index:
            return None
        v = int.from_bytes(self.data[self.cur_index:self.cur_index + 2], 'little')
        self.cur_index += 2
        return v

    def read_raw(self, n):
        end = min(self.cur_index + n, self.max_index)
        out = bytes(self.data[self.cur_index:end])
        self.cur_index += len(out)
        return out

    def header(self):
        b = bytes(self.data[0:8])
        first = int.from_bytes(b[0:4], 'little')
        second = int.from_bytes(b[4:8], 'little')
        return {'type': (first >> 16) & 0xFF, 'group': first & 0xFFFF, 'src': (first >> 24) & 0xFF,
                'idx': second & 0xFF, 'count': (second >> 8) & 0xFF,
                'compressed': (second >> 16) & 0xFF}

    def header_bytes(self):
        """PackHeader: the wire header as the engine writes it from the header view."""
        h = self.header()
        first = (h['group'] & 0xFFFF) | (h['type'] << 16) | (h['src'] << 24)
        second = h['idx'] | (h['count'] << 8) | (h['compressed'] << 16)
        out = bytearray()
        out.append((first >> 16) & 0xFF)
        out += (first & 0xFFFF).to_bytes(2, 'little')
        out.append(second & 0xFF)
        out.append(((second >> 8) | (0x80 if (second >> 16) & 0xFF else 0)) & 0xFF)
        return bytes(out)


class Pool:
    """Mirrors NetPacket::Create / Alloc / Get: two free lists, 0..255 big and 256..511 small."""

    def __init__(self):
        self.packets = {}
        self.free = {True: [], False: []}
        for i in range(MAX_BIG):
            self.packets[i] = Packet(i, False, self)
            self.free[False].append(i)
        for i in range(MAX_SMALL):
            n = MAX_BIG + i
            self.packets[n] = Packet(n, True, self)
            self.free[True].append(n)

    def get(self, num):
        return self.packets.get(num)

    def alloc(self, small):
        if not self.free[small]:
            return None
        p = self.packets[self.free[small].pop(0)]
        p.cur_index = 2
        return p


class Group:
    def __init__(self, pool, small):
        self.pool = pool
        self.is_small = small
        self.first = -1
        self.last = -1
        self.count = 0
        self.pkt_ptr = None
        self.cur_bit = 0
        self.pkt_bits = 0

    def reset(self):
        self.count = 0
        self.last = -1
        self.first = -1

    def begin_write(self):
        self.reset()
        self.pkt_ptr = None
        self.cur_bit = 0
        self.pkt_bits = 0

    def push_back(self, p):
        p.prev_pkt = self.last
        if self.count == 0:
            self.first = p.num
        else:
            self.pool.get(self.last).next_pkt = p.num
        self.last = p.num
        p.next_pkt = -1
        self.count += 1

    def pop_front(self):
        if self.count == 0:
            return None
        p = self.pool.get(self.first)
        self.first = p.next_pkt
        p.next_pkt = -1
        self.count -= 1
        if self.count == 0:
            self.last = -1
        return p

    def remove(self, p):
        prev = self.pool.get(p.prev_pkt) if p.prev_pkt >= 0 else None
        nxt = self.pool.get(p.next_pkt) if p.next_pkt >= 0 else None
        if prev:
            prev.next_pkt = nxt.num if nxt else -1
        else:
            self.first = nxt.num if nxt else -1
        if nxt:
            nxt.prev_pkt = prev.num if prev else -1
        else:
            self.last = prev.num if prev else -1
        p.next_pkt = p.prev_pkt = -1
        self.count -= 1

    def set(self, p):
        self.first = p.num
        self.count = 1
        self.last = self.first
        q = p
        while q.next_pkt != -1:
            self.last = q.next_pkt
            q = self.pool.get(self.last)
            self.count += 1

    def begin_read(self):
        self.pkt_ptr = self.pool.get(self.first)
        self.pkt_ptr.begin_read()
        self.pkt_bits = 0
        self.cur_bit = 32

    def write_packed(self):
        if self.pkt_ptr is None:
            self.pkt_ptr = self.pool.alloc(self.is_small)
            self.push_back(self.pkt_ptr)
            self.pkt_ptr.begin_write()
        if not self.pkt_ptr.write_u32(self.pkt_bits):
            self.pkt_ptr.end_write()
            self.pkt_ptr = self.pool.alloc(self.is_small)
            self.push_back(self.pkt_ptr)
            self.pkt_ptr.begin_write()
            self.pkt_ptr.write_u32(self.pkt_bits)

    def read_packed(self):
        if self.pkt_ptr is not None:
            v = self.pkt_ptr.read_u32()
            if v is not None:
                self.pkt_bits = v
                return True
        self.pkt_ptr = self.pool.get(self.pkt_ptr.next_pkt) if self.pkt_ptr else None
        if self.pkt_ptr:
            self.pkt_ptr.begin_read()
            v = self.pkt_ptr.read_u32()
            if v is None:
                self.pkt_bits = 0
                return False
            self.pkt_bits = v
            return True
        self.pkt_bits = 0
        return False

    def write_bits(self, value, n):
        while True:
            cnt = min(32 - self.cur_bit, n)
            part = value if cnt == 32 else value & ((1 << cnt) - 1)
            self.pkt_bits = (self.pkt_bits | (part << self.cur_bit)) & MASK32
            self.cur_bit += cnt
            if self.cur_bit == 32:
                self.write_packed()
                self.cur_bit = 0
                self.pkt_bits = 0
            n -= cnt
            if n == 0:
                return
            value >>= cnt

    def read_bits(self, n):
        if self.cur_bit == 32:
            self.read_packed()
            self.cur_bit = 0
        value = 0
        shift = 0
        while True:
            cnt = min(32 - self.cur_bit, n)
            mask = MASK32 if cnt == 32 else (1 << cnt) - 1
            part = self.pkt_bits >> self.cur_bit
            self.cur_bit += cnt
            value = (value | ((part & mask) << shift)) & MASK32
            n -= cnt
            if n == 0:
                return value
            shift += cnt
            if not self.read_packed():
                return value
            self.cur_bit = 0

    def write_bytes(self, raw):
        if self.pkt_ptr is None:
            self.pkt_ptr = self.pool.alloc(self.is_small)
            self.push_back(self.pkt_ptr)
            self.pkt_ptr.begin_write()
        if self.cur_bit > 0:
            self.write_bits(0, 32 - self.cur_bit)
        n = self.pkt_ptr.write_raw(raw)
        while n != len(raw):
            raw = raw[n:]
            self.pkt_ptr.end_write()
            self.pkt_ptr = self.pool.alloc(self.is_small)
            self.push_back(self.pkt_ptr)
            self.pkt_ptr.begin_write()
            n = self.pkt_ptr.write_raw(raw)
            if n == 0:
                break

    def read_bytes(self, n):
        if self.cur_bit > 0:
            self.cur_bit = 32
            self.pkt_bits = 0
        out = b''
        while self.pkt_ptr:
            chunk = self.pkt_ptr.read_raw(n - len(out))
            out += chunk
            if len(out) == n:
                return out
            self.pkt_ptr = self.pool.get(self.pkt_ptr.next_pkt)
            if not self.pkt_ptr:
                return out
            self.pkt_ptr.begin_read()
        return out

    def end_write(self):
        self.write_packed()
        if self.pkt_ptr:
            self.pkt_ptr.end_write()


def encode(pool):
    g = Group(pool, True)
    g.begin_write()
    for i, op in enumerate(OPS):
        kind = op['kind']
        if kind == BYTES:
            g.write_bytes(pattern(op['n']))
        elif kind == FIXED:
            code = fixed_code(op['f'], op['min'], op['max'], op['bits'], op.get('hires', False))
            g.write_bits(code, 32 if op.get('hires') else op['bits'])
        elif kind == FLOATBITS:
            g.write_bits(fbits(op['f']), 32)
        elif kind == BOOL:
            g.write_bits(1 if op['value'] else 0, 1)
        else:
            g.write_bits(op['value'] & MASK32, op['bits'])
    g.end_write()

    count = g.count
    datagrams = []
    for i in range(count):
        p = g.pop_front()
        # The header fields NetComm::Send(iDest, uType, pGroup) sets for each packet in the group.
        p.data[0:8] = bytes(8)
        p.data[0] = SEQ & 0xFF
        p.data[1] = (SEQ >> 8) & 0xFF
        p.data[2] = TYPE
        p.data[3] = SRC
        p.data[4] = i
        p.data[5] = count
        p.data[6] = p.data[7] = 0
        datagrams.append({'header': p.header(), 'bytes': p.header_bytes() + bytes(p.data[14:p.max_index])})
    return datagrams


def make_received(pool, raw):
    prefix = 0 if raw[0] >= 0x1E else 3
    p = pool.alloc(len(raw) <= 116 - prefix)
    if p is None:
        return None, prefix
    p.begin_write()
    p.data[prefix:prefix + len(raw)] = raw
    p.comm_index = prefix
    p.max_index = len(raw) + prefix
    # UnpackHeader: rebuild the in-memory header view in buffer bytes 0..7 and move the data index.
    p.cur_index = p.comm_index
    b0 = p.read_u8()
    w1 = p.read_u16()
    b1 = p.read_u8()
    b2 = p.read_u8()
    u_first = (w1 & 0xFFFF) | (b0 << 16) | (p.data[3] << 24)
    u_second = b1 | ((b2 & 0x7F) << 8) | ((b2 >> 7) << 16)
    p.data[0:4] = (u_first & MASK32).to_bytes(4, 'little')
    p.data[4:8] = (u_second & MASK32).to_bytes(4, 'little')
    p.data_index = p.cur_index
    return p, prefix


def read_values(group):
    group.begin_read()
    values = []
    for i, op in enumerate(OPS):
        kind = op['kind']
        if kind == BYTES:
            values.append(''.join(f'{b:02x}' for b in group.read_bytes(op['n'])))
            continue
        code = group.read_bits(1 if kind == BOOL else (32 if op.get('hires') else op.get('bits', 32)))
        if kind == BOOL:
            values.append(1 if code else 0)
        elif kind in (BITS, FLOATBITS):
            values.append(code)
        else:
            values.append(fixed_decode(code, op['min'], op['max'], op['bits'], op.get('hires', False)))
    return values


def reassemble(datagrams, order):
    pool = Pool()
    received = [make_received(pool, d['bytes']) for d in datagrams]
    partial = Group(pool, True)
    complete = Group(pool, True)
    completed = []
    for i in order:
        p, prefix = received[i]
        # DoRecvs sets the source before the group is reassembled.
        p.data[3] = SRC
        header = p.header()
        if header['count'] == 1 and header['idx'] == 0:
            p.next_pkt = p.prev_pkt = -1
            complete.set(p)
            completed.append(True)
            continue
        if partial.count == 0:
            partial.push_back(p)
            completed.append(False)
            continue
        first = pool.get(partial.first)
        fh = first.header()
        if not (fh['group'] == header['group'] and fh['type'] == header['type'] and fh['src'] == header['src']):
            partial.push_back(p)
            completed.append(False)
            continue
        cur = pool.get(partial.first)
        for _ in range(partial.count):
            nxt = pool.get(cur.next_pkt)
            if cur.header()['idx'] == header['idx']:
                partial.remove(cur)
            cur = nxt
        partial.push_back(p)
        if partial.count != header['count']:
            completed.append(False)
            continue
        complete.reset()
        for k in range(header['count']):
            q = partial.pop_front()
            while q.header()['idx'] != k:
                partial.push_back(q)
                q = partial.pop_front()
            complete.push_back(q)
        completed.append(True)
    return complete, completed, received


def main():
    as_json = '--json' in sys.argv
    datagrams = encode(Pool())
    out = {
        'ops': [op_field(o) for o in OPS],
        'datagrams': [{'bytes': d['bytes'].hex(), 'header': d['header'], 'len': len(d['bytes'])}
                      for d in datagrams],
        'facts': {'packet_limit_small': 116, 'packet_limit_big': 1012, 'data_index': 14,
                  'comm_index': 9, 'prefix': 3, 'prefix_when_type_ge_0x1e': 0,
                  'type': TYPE, 'src': SRC, 'group_sequence': SEQ, 'expected_packets': len(datagrams)},
    }
    for label, order in (('send_order', list(range(len(datagrams)))),
                         ('scrambled', [1, 0] if len(datagrams) > 1 else [0])):
        complete, completed, received = reassemble(datagrams, order)
        out['decode_' + label] = {
            'order': order,
            'complete_after_each': completed,
            'values': read_values(complete) if complete.count else [],
            'headers': [received[i][0].header() for i in order],
        }
    # Not a check, a measurement of the risk. A model computing in double agrees with the engine only
    # when the last bit happens to land the same way, so the model in this file must do every step in
    # float32 like the engine does.
    out['model_would_be_wrong_in_double'] = {
        'interior_codes_that_differ': {
            '7_bits_-1_to_1': double_model_divergence(7, -1.0, 1.0),
            '10_bits_0_to_10': double_model_divergence(10, 0.0, 10.0),
            '11_bits_-16_to_16': double_model_divergence(11, -16.0, 16.0),
        },
        'note': 'the engine writes and reads fixed fields in float32; a model in double is a stronger '
                'arithmetic and can only agree by luck',
    }
    if as_json:
        print(json.dumps(out, ensure_ascii=False))
    else:
        for i, d in enumerate(out['datagrams']):
            print(f"datagram {i}: {d['len']} bytes  {d['bytes']}")
            h = d['header']
            print(f"  header type {h['type']} group {h['group']} idx {h['idx']} count {h['count']}")
        for label in ('send_order', 'scrambled'):
            r = out['decode_' + label]
            print(f"{label}: order {r['order']} complete after each {r['complete_after_each']}")
            print(f"  values {[v if isinstance(v, str) else hex(v) for v in r['values']]}")
    return 0


if __name__ == '__main__':
    sys.exit(main())
