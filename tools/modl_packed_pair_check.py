#!/usr/bin/env python3
"""Compare the packed vertex streams against the float streams in the same segment.

The PC build stored two vertex streams per segment for most models: one packed (bits 0x1000/0x2000/
0x4000/0x8000) and one uncompressed, for the two vertex formats the hardware could take. They hold
the same vertices in the same order. That makes the packed encodings testable against the file's
own answer, with no assumption at all: for each vertex, compare what the packed form implies with
what the uncompressed form states.

This settles three things that would otherwise have to be assumed:

  * positions   - the quantisation reference. `pcRedSegment.cpp` sets CompressedPos::s_vMin /
                  s_vScale from `m_bbox`, which is `RedModel::_BoundingBox`, filled from the box
                  inside `modl`'s INFO chunk. Every segment also records its own BBOX (its extents).
                  The comparison says which box the shorts were quantised against.
  * normals     - whether the 4 bytes are component bytes, and in which order. D3DCOLOR is a DWORD,
                  so in memory its bytes are B,G,R,A; `MTRL` colours in this same format confirm
                  that reading (RedSegment::Read unpacks `.b`, `.g`, `.r`, `.a` from the low bytes).
  * texcoords   - the fixed-point scale, by dividing the float UV by the short.

Usage: python3 tools/modl_packed_pair_check.py [file.lvl ...]
"""
import struct
import sys
import pathlib
import collections

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import lvl_reader as L
from modl_probe import find_chunks
from modl_decompress_check import children

PACKED_POS = 0x1000
PACKED_WEIGHTS = 0x2000
PACKED_NORMAL = 0x4000
PACKED_TEXCOORD = 0x8000


def offsets(fmt):
    """Byte offsets of each component group inside one vertex record, in the order
    CreateVertexDeclaration lays elements out."""
    off = 0
    npos = (fmt & 3) + 1 if fmt & 3 else 0
    pos = off if fmt & 3 else None
    off += (2 * (4 if npos > 2 else 2)) if (fmt & PACKED_POS) else npos * 4

    if fmt & 0x1C:
        w = (fmt >> 2) & 7
        if w > 1:
            off += 4 if (fmt & PACKED_WEIGHTS) else w * 4 - 4
        off += 4

    normal = off if (fmt & 0x20) else None
    off += 4 if (fmt & PACKED_NORMAL) else (12 if (fmt & 0x20) else 0)

    tangent = off if (fmt & 0x40) else None
    # A group only occupies bytes when its bit is set. Adding the packed tangent size to a
    # vertex format that has no tangent group shifts everything after it by 8 bytes, which is
    # what made the texcoord comparison read garbage.
    if fmt & 0x40:
        off += 8 if (fmt & PACKED_NORMAL) else 24

    diffuse = off if (fmt & 0x80) else None
    off += 4 if (fmt & 0x80) else 0
    off += 4 if (fmt & 0x100) else 0

    texcoord = off if (fmt & 0x600) else None
    sets = ((fmt >> 9) & 3) if (fmt & 0x600) else 0
    return {'pos': pos, 'pos_components': npos, 'normal': normal, 'tangent': tangent,
            'diffuse': diffuse, 'texcoord': texcoord, 'sets': sets}


def stream_pairs(data, seg_start, seg_end):
    st = []
    for cid, body, size in children(data, seg_start, seg_end):
        if cid in ('VBUF', 'VDAT') and size >= 12:
            hdr = struct.unpack_from('<III', data, body)
            st.append((hdr, body + 12))
    packed = [x for x in st if x[0][2] & PACKED_POS]
    plain = [x for x in st if not (x[0][2] & PACKED_POS)]
    if not packed or not plain:
        return None
    if packed[0][0][0] != plain[0][0][0]:
        return None
    return packed[0], plain[0]


def main(argv):
    files = [a for a in argv if not a.startswith('-')]
    base = pathlib.Path('data/Star wars battlefront 2/GameData/data/_lvl_pc')
    if not files:
        files = [base / 'tat' / 'tat2.lvl', base / 'yav' / 'yav1.lvl']

    pos_ref_wins = collections.Counter()
    normal_orders = collections.Counter()
    normal_maps = collections.Counter()
    uv_ratios = collections.Counter()
    pairs = 0
    verts = 0
    uv_ok = uv_bad = out_of_range = 0
    worst_uv = 0.0
    worst_norm = 0.0

    for f in files:
        p = pathlib.Path(f)
        if not p.exists() and (base / f).exists():
            p = base / f
        root, _ = L.parse_file(str(p))
        if not root:
            continue
        data = p.read_bytes()
        for name, s, e, mpath in find_chunks(root, {'modl'}):
            model_box = None
            for cid, body, size in children(data, s, e):
                if cid == 'INFO' and size >= 72:
                    vals = struct.unpack_from('<IIII6f6fII', data, body)
                    model_box = list(vals[4:10])
                    break
            if not model_box:
                continue
            for cid, body, size in children(data, s, e):
                if cid != 'segm':
                    continue
                pair = stream_pairs(data, body, body + size)
                if not pair:
                    continue
                pk, fl = pair
                cnt, psize, pfmt = pk[0]
                _, fsize, ffmt = fl[0]
                po = offsets(pfmt)
                fo = offsets(ffmt)
                if po['pos_components'] != fo['pos_components']:
                    continue
                seg_box = None
                for sc, sb, ss in children(data, body, body + size):
                    if sc == 'BBOX' and ss >= 24:
                        seg_box = list(struct.unpack_from('<6f', data, sb))
                        break
                if not seg_box:
                    continue
                pairs += 1
                verts += cnt

                # positions: which reference box reproduces the float positions?
                for ref, label in ((model_box, 'modl INFO box'), (seg_box, 'segment BBOX')):
                    ext = [ref[i + 3] - ref[i] for i in range(3)]
                    if any(x <= 1e-6 for x in ext):
                        continue
                    err = 0.0
                    for v in range(cnt):
                        for i in range(min(po['pos_components'], 3)):
                            sh = struct.unpack_from('<h', data, pk[1] + v * psize + po['pos'] + i * 2)[0]
                            fv = struct.unpack_from('<f', data, fl[1] + v * fsize + fo['pos'] + i * 4)[0]
                            dec = ref[i] + (sh + 32767.5) * (ext[i] / 65535.0)
                            err = max(err, abs(dec - fv))
                    if err <= 0.05:
                        pos_ref_wins[label] += 1

                # normals
                if po['normal'] is not None and fo['normal'] is not None:
                    for order, oname in (((0, 1, 2), 'bytes x,y,z'), ((2, 1, 0), 'bytes z,y,x (B,G,R)')):
                        for mapping, mname in (
                            (lambda b: b / 255.0 * 2 - 1, 'unsigned bytes -> [-1,1] /255'),
                            (lambda b: (b if b < 128 else b - 256) / 127.0, 'signed bytes /127')):
                            mx = 0.0
                            for v in range(cnt):
                                for i in range(3):
                                    b = data[pk[1] + v * psize + po['normal'] + order[i]]
                                    fv = struct.unpack_from('<f', data, fl[1] + v * fsize + fo['normal'] + i * 4)[0]
                                    mx = max(mx, abs(mapping(b) - fv))
                            if mx <= 0.02:
                                normal_orders[oname] += 1
                                normal_maps[mname] += 1
                            else:
                                worst_norm = max(worst_norm, 0.0)

                # texcoords: the short and the float are the same component, so the ratio is the
                # fixed-point scale the exporter used. Note the stride differs per side: a short
                # component is 2 bytes, a float component is 4. Quantising to a 12-bit fixed point
                # means UVs outside [-16, 16] cannot be represented at all, so those segments are
                # reported separately instead of counted as failures.
                if po['texcoord'] is not None and fo['texcoord'] is not None and po['sets'] == fo['sets'] and po['sets'] and (pfmt & PACKED_TEXCOORD):
                    ratios = []
                    err = 0.0
                    for v in range(cnt):
                        for k in range(2):
                            sh = struct.unpack_from('<h', data, pk[1] + v * psize + po['texcoord'] + k * 2)[0]
                            fu = struct.unpack_from('<f', data, fl[1] + v * fsize + fo['texcoord'] + k * 4)[0]
                            if abs(fu) > 16.0:
                                out_of_range += 1
                                continue
                            if sh == 0:
                                continue
                            ratios.append(fu / sh)
                            err = max(err, abs(sh / 2048.0 - fu))
                    if ratios:
                        med = sorted(ratios)[len(ratios) // 2]
                        uv_ratios[round(med, 6)] += 1
                        if err <= 1.0 / 2048.0:
                            uv_ok += 1
                        else:
                            uv_bad += 1

    print('segments with a packed and an uncompressed copy of the same vertices: %d (%d vertices)'
          % (pairs, verts))
    print('\npositions - which box the shorts were quantised against:')
    for k, v in pos_ref_wins.most_common():
        print('   %-18s reproduces the float positions for %d segments' % (k, v))
    print('\nnormals - order and mapping that match the file\'s own float normals:')
    for k, v in normal_orders.most_common():
        print('   order %-22s matched %d segments' % (k, v))
    for k, v in normal_maps.most_common():
        print('   mapping %-34s matched %d segments' % (k, v))
    print('\ntexcoords - median float_uv/short per segment:')
    for k, v in uv_ratios.most_common(6):
        print('   %.5f (1/%.0f) x%d' % (k, 1.0 / k if k else 0, v))
    print('   median ratio per segment (1/2048 is the fixed point the data implies):')
    for k, v in uv_ratios.most_common(4):
        print('   %.6f (1/%.0f) x%d' % (k, 1.0 / k if k else 0, v))
    print('   segments reconstructed within one quantisation step (1/2048): %d ok / %d over'
          % (uv_ok, uv_bad))
    print('   UVs that a 12-bit fixed point cannot hold at all (|uv| > 16): %d vertices'
          % out_of_range)
    print('   worst |short/2048 - float_uv| over compared vertices: %.4f' % worst_uv)


if __name__ == '__main__':
    main(sys.argv[1:])
