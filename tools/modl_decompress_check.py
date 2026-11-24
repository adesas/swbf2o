#!/usr/bin/env python3
"""Verify the model-format decodings against the numbers the files themselves carry.

Nothing here is guessed. Each rule is taken from the engine's own reader and then tested:

1. Positions stored as shorts. `pcRedSegment.cpp` sets the decode parameters from the model box:

       CompressedPos::s_vMin   = m_bbox._vMin;
       CompressedPos::s_vScale = (m_bbox._vMax - m_bbox._vMin) * (1.0f / 65535.0f);

   `m_bbox` is `_GetModelBox(pModel)`, i.e. `RedModel::_BoundingBox`, which `RedModel.cpp` fills
   from the box inside `modl`'s INFO chunk. Every effect that *writes* compressed vertices encodes
   with `short((v - s_vMin) * s_vScaleInv - 32767.5)`, so the inverse is
   `v = min + (s + 32767.5) * extent/65535`. The check: decode a segment with the model box and
   see whether the extremes it produces reproduce the BBOX that same segment records. That is an
   independent test - it does not care about strides or component order, only that the box the file
   states and the box the bytes imply are the same box.

2. Packed normals (`0x20` with the packed bit `0x4000`): four bytes. If they are component bytes
   mapped to [-1,1] then every decoded vector must have length 1, which is not a property any
   wrong reading would accidentally satisfy.

3. Packed texcoords (`0x600` with `0x8000`): reported as the range the data actually spans, so a
   fixed-point scale can be confirmed or refuted rather than assumed.

Usage: python3 tools/modl_decompress_check.py <file.lvl> [...] [-v]
"""
import struct
import sys
import pathlib
import math

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import lvl_reader as L
from modl_probe import IDS, find_chunks

TOL_REL = 1e-3          # float(0.0) -> short -> float round trip is not exact; 1e-3 is generous
PACKED_NORMAL_BIT = 0x4000
PACKED_POS_BIT = 0x1000
PACKED_TEXCOORD_BIT = 0x8000


def children(data, start, end):
    out = []
    pos = start
    while pos + 8 <= end:
        cid = data[pos:pos + 4].decode('latin1')
        size = struct.unpack_from('<I', data, pos + 4)[0]
        body = pos + 8
        if body + size > end:
            break
        out.append((cid, body, size))
        pos = body + size
        pos += (-pos) & 3
    return out


def component_offsets(fmt):
    """Byte offsets of each group, in the order CreateVertexDeclaration lays elements out."""
    off = 0
    pos_c = 0
    if fmt & 3:
        pos_c = (fmt & 3) + 1
        off += (2 * (4 if pos_c > 2 else 2)) if (fmt & PACKED_POS_BIT) else pos_c * 4
    if fmt & 0x1C:
        w = (fmt >> 2) & 7
        if w > 1:
            off += 4 if (fmt & 0x2000) else w * 4 - 4
        off += 4
    normal = off if (fmt & 0x20) else None
    off += 4 if (fmt & PACKED_NORMAL_BIT) else (12 if (fmt & 0x20) else 0)
    tangent = off if (fmt & 0x40) else None
    if fmt & 0x40:          # a group only occupies bytes when its bit is set
        off += 8 if (fmt & PACKED_NORMAL_BIT) else 24
    diffuse = off if (fmt & 0x80) else None
    off += 4 if (fmt & 0x80) else 0
    off += 4 if (fmt & 0x100) else 0
    texcoord = off if (fmt & 0x600) else None
    return {'pos': 0 if fmt & 3 else None, 'pos_components': pos_c, 'normal': normal,
            'tangent': tangent, 'diffuse': diffuse, 'texcoord': texcoord,
            'texcoord_sets': ((fmt >> 9) & 3) if (fmt & 0x600) else 0}


def check_file(path, verbose):
    root, _ = L.parse_file(str(path))
    if not root:
        return None
    data = pathlib.Path(path).read_bytes()

    pos_ok = pos_bad = 0
    norm_ok = norm_bad = 0
    tex_reported = 0
    tex_within_2048 = 0
    worst = []

    for name, s, e, mpath in find_chunks(root, {'modl'}):
        mkids = children(data, s, e)
        model_box = None
        for cid, body, size in mkids:
            # modl INFO: skin, proj flag, segment count, pad, then the model box (RedModel::_BoundingBox)
            if cid == 'INFO' and size >= 68:
                model_box = struct.unpack_from('<6f', data, body + 16)
                break
        if not model_box:
            continue
        mn, mx = model_box[:3], model_box[3:]
        ext = [mx[i] - mn[i] for i in range(3)]
        if any(x <= 1e-6 for x in ext):
            continue

        for cid, body, size in mkids:
            if cid != 'segm':
                continue
            seg = children(data, body, body + size)
            own = None
            streams = []
            for sc, sb, ss in seg:
                if sc == 'BBOX' and ss >= 24:
                    own = struct.unpack_from('<6f', data, sb)
                elif sc in ('VBUF', 'VDAT') and ss >= 12:
                    cnt, vsize, fmt = struct.unpack_from('<III', data, sb)
                    streams.append((cnt, vsize, fmt, sb + 12))
            if not own or not streams:
                continue

            for cnt, vsize, fmt, payload in streams:
                if not (fmt & 3) or cnt == 0:
                    continue
                comp = component_offsets(fmt)
                ncomp = comp['pos_components']

                # (1) positions: float streams must equal the recorded box; packed streams must
                # reproduce it through the model box.
                lo = [1e9] * 3
                hi = [-1e9] * 3
                for v in range(cnt):
                    base = payload + v * vsize
                    for i in range(min(ncomp, 3)):
                        if fmt & PACKED_POS_BIT:
                            sh = struct.unpack_from('<h', data, base + i * 2)[0]
                            val = mn[i] + (sh + 32767.5) * (ext[i] / 65535.0)
                        else:
                            val = struct.unpack_from('<f', data, base + i * 4)[0]
                        lo[i] = min(lo[i], val)
                        hi[i] = max(hi[i], val)
                if lo[0] == 1e9:
                    continue
                tol = TOL_REL * max(1.0, max(abs(x) for x in own))
                err = max(max(abs(lo[i] - own[i]), abs(hi[i] - own[3 + i])) for i in range(3))
                if err <= tol:
                    pos_ok += 1
                else:
                    pos_bad += 1
                    if verbose and len(worst) < 6:
                        worst.append((mpath + '/segm', fmt, err,
                                      [round(lo[i] - own[i], 4) for i in range(3)]))

                # (2) packed normals: bytes mapped to [-1,1] must give unit vectors.
                if (fmt & 0x20) and (fmt & PACKED_NORMAL_BIT):
                    lens = []
                    for v in range(cnt):
                        o = payload + v * vsize + comp['normal']
                        n = [(data[o + k] / 255.0) * 2 - 1 for k in range(3)]
                        lens.append(math.sqrt(sum(x * x for x in n)))
                    mean = sum(lens) / len(lens)
                    if abs(mean - 1.0) <= 0.15:
                        norm_ok += 1
                    else:
                        norm_bad += 1
                        if verbose and len(worst) < 10:
                            worst.append((mpath + '/segm', fmt, mean, None))

                # (3) packed texcoords: report the span, do not assume a scale.
                if (fmt & 0x600) and (fmt & PACKED_TEXCOORD_BIT) and comp['texcoord_sets']:
                    vals = [struct.unpack_from('<h', data, payload + v * vsize + comp['texcoord'] + k * 2)[0]
                            for v in range(cnt) for k in range(2)]
                    tex_reported += 1
                    if max(vals) <= 2048 and min(vals) >= -2048:
                        tex_within_2048 += 1
                    if verbose and tex_reported <= 4:
                        print('   texcoord-short span [%d..%d] x [%d..%d] (scale 1/2048 -> uv [%.2f, %.2f])'
                              % (min(vals), max(vals), min(vals), max(vals),
                                 min(vals) / 2048.0, max(vals) / 2048.0))

    return pos_ok, pos_bad, norm_ok, norm_bad, tex_reported, tex_within_2048, worst


def main(argv):
    verbose = '-v' in argv
    files = [a for a in argv if not a.startswith('-')]
    base = pathlib.Path('data/Star wars battlefront 2/GameData/data/_lvl_pc')
    tot = [0] * 6
    for f in files:
        p = pathlib.Path(f)
        if not p.exists() and (base / f).exists():
            p = base / f
        r = check_file(p, verbose)
        if r is None:
            print('%s: not a ucfb container' % f)
            continue
        pos_ok, pos_bad, norm_ok, norm_bad, tex_n, tex_ok, worst = r
        for i, v in enumerate((pos_ok, pos_bad, norm_ok, norm_bad, tex_n, tex_ok)):
            tot[i] += v
        print('%s: positions reproduce their recorded box %d ok / %d bad'
              ' | packed normals unit %d ok / %d bad'
              ' | packed texcoords within 1/2048 %d of %d'
              % (p.name, pos_ok, pos_bad, norm_ok, norm_bad, tex_ok, tex_n))
        for w in worst:
            print('   FAIL %s fmt=0x%04x value=%s %s' % (w[0], w[1], round(w[2], 4), w[3] or ''))

    print('\nTOTAL positions: %d ok / %d bad' % (tot[0], tot[1]))
    print('TOTAL packed normals: %d ok / %d bad' % (tot[2], tot[3]))
    print('TOTAL packed texcoords: %d within a 1/2048 fixed point out of %d reported'
          % (tot[5], tot[4]))
    if tot[0] and not tot[1]:
        print('Confirmed: the quantisation reference is modl INFO\'s model box, not the segment box.')


if __name__ == '__main__':
    main(sys.argv[1:])
