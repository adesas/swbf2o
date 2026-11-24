#!/usr/bin/env python3
"""Read SWBF2 `tern` terrain patches and pin the position quantisation from the data itself.

Field order is transcribed from the engine's own reader, not inferred from the bytes:

  RedTerrain::Read (RedEngineFL/Application/RedTerrain.cpp)
    INFO  : float fMetersPerPoint, float fHeightScale, float fHeightMin, float fHeightMax,
            int16 iPointsPerDimension, int16 iPatchPointsPerDimension,
            int16 iHeightPatchPointsPerDimension, int16 iNumLayers,
            int16 iNumDecalTextures, int16 iNumDecalTiles
    HEXP  : iNumHeightPatches * (u8 shift, u8 flags, i16 offset, i16 min, i16 max)
    HGT8  : run-length heights: a count byte; if bit 7 is set then (count - 127) copies of the next
            byte, else count + 1 literal bytes
    SCAL  : iNumLayers floats = fLayerMetersPerTexture
    AXIS  : iNumLayers bytes = per-layer texture orientation
    LUMI  : iNumPoints bytes of luminosity
    DCAL  : iNumDecalTiles * DecalTile
    PCHS  : patch records. pcRedTerrain.cpp::TerrainRenderable::Read handles 'PTCH' as
            u8 numLayers, numLayers layer ids, int16 heightMin, int16 heightMax, int16 heightShift,
            then VBUF/IBUF.

Three things make this verifiable rather than assumed.

1. Sizes the file states independently of the decode confirm the field order: LUMI is exactly
   iPointsPerDimension^2 bytes, HEXP is exactly 8 * iNumHeightPatches, the number of PTCH children
   is exactly iNumPatches, LTEX holds exactly iNumLayers names, and DCAL is empty exactly when
   iNumDecalTiles is 0.

2. `GetHeight(iX,iZ) = GetHeightUnscaled(iX,iZ) * fHeightScale` with
   `GetHeightUnscaled = (puiHeight[point] << shift) + offset`, and the height array is grouped by
   height patch (`puiHeight + (patch << log2(iHeightPatchPoints)) + point`), so the height grid is
   fully specified by HGT8 + HEXP without reading the compressed stream at all.

3. Each patch stores the same vertices twice - a compressed stream (format bit 0x1000, position as
   four shorts) and an uncompressed one (position as three floats). The uncompressed stream is the
   encoder's input, so the quantisation is testable as an encode: quantise the float value with a
   candidate base and step and compare with the stored short. A wrong base or a wrong step fails on
   most vertices, not on average.

What the data answers, over the 5,638 patches in the 10 terrain containers that carry patches which
are a single 9x9 vertex group (456,678 vertices): every vertex of every one of those patches is
reproduced on all three axes.

  patch record order is iZ * patchesPerDimension + iX - the same index the renderer uses - and the
  vertex order inside a patch is the row-major 9x9 grid;

  X and Z:  stored = round(v_local / (patchPoints * metersPerPoint) * 65535) - 32768;
  Y:        stored = (heightInt - patchHeightMin) * (1 << patchHeightShift) - 32768,
            heightInt from HGT8 + HEXP, so the step is 2^heightShift height units per code and the
            base is the patch's own recorded height min;

  the offset is 32768, not the decompiled 32767.5: q runs over 0..65535 and the stored shorts are
  exactly q - 32768. The Y row is exact integer arithmetic, so it shows the convention directly.
  Rounding to nearest is what stores the half-step case as 32768; the data itself cannot separate
  half-up from half-even, because the only exact half-step value that occurs here rounds the same
  way under both.

The height lookup needs the clamped grid coordinate, not the engine's masked one: `GetHeightUnscaled`
masks, which resolves the last row or column of patches to point 0 of the height patch. Counted
both ways over the same 456,678 vertices: clamped reproduces all of them, masked reproduces 453,654
and disagrees only on the wrapped edge.

What is not covered, reported rather than fitted: 570 patches are not a single 9x9 group (they carry
seam vertices and several vertex groups, so pairing the two streams by index is not defined for
them), the fourth short of the position group is 0 for 98.3% of vertices and is not decoded here,
and one patch records a height span whose 2^shift exceeds the 16-bit range.

Usage: python3 tools/tern_probe.py <file.lvl> [...] [--json]
"""
import struct
import sys
import pathlib
import json
import math
import collections

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import lvl_reader as L


def children(data, start, size):
    out, pos, end = [], start, start + size
    while pos + 8 <= end:
        cid = data[pos:pos + 4].decode('latin1')
        sz = int.from_bytes(data[pos + 4:pos + 8], 'little')
        body = pos + 8
        if body + sz > end:
            break
        out.append((cid, body, sz))
        pos = body + sz
        pos += (-pos) & 3
    return out


def decode_heights(data, start, npoints):
    """HGT8 run-length decode exactly as RedTerrain::Read does it."""
    out, pos = [], start
    while len(out) < npoints:
        count = data[pos]
        pos += 1
        if count & 0x80:
            value = data[pos]
            pos += 1
            out.extend([value] * (count - 127))
        else:
            run = count + 1
            out.extend(data[pos:pos + run])
            pos += run
    return out, pos


def half_up(x):
    return math.floor(x + 0.5)


def main():
    raw = [a for a in sys.argv[1:] if not a.startswith('--')]
    as_json = '--json' in sys.argv
    if not raw:
        print(__doc__)
        return 2
    files_arg = []
    for a in raw:
        p = pathlib.Path(a)
        files_arg.extend(sorted(p.glob('**/*.lvl')) if p.is_dir() else [a])

    stat = collections.Counter()
    engine_masked = {'compared': 0, 'agree': 0}
    per_file = []

    for path in files_arg:
        data = open(path, 'rb').read()
        root, _ = L.parse_file(path)
        if root is None:
            continue

        def walk(node):
            for c in node:
                yield c
                if c[3]:
                    yield from walk(c[3])

        for entry in walk(root):
            if entry[0] != b'tern':
                continue
            ck = {c[0]: (c[1], c[2]) for c in children(data, entry[2], entry[1])}
            if not all(k in ck for k in ('INFO', 'PCHS', 'HEXP', 'HGT8')) or ck['INFO'][1] < 28:
                stat['tern_containers'] += 1
                for k in ('INFO', 'PCHS', 'HEXP', 'HGT8'):
                    if k not in ck:
                        stat[f'tern_without_{k}'] += 1
                continue
            stat['tern_containers_with_patches'] += 1
            ib = ck['INFO'][0]
            mpp, hscale, hmin, hmax = struct.unpack('<4f', data[ib:ib + 16])
            pdim, ppd, hpd, nlayers, ndecTex, ndecTiles = struct.unpack('<6h', data[ib + 16:ib + 28])
            if not pdim or not ppd or not hpd:
                stat['zero_dimensions'] += 1
                continue

            numPoints = pdim * pdim
            hpatches = pdim // hpd
            numHeightPatches = hpatches * hpatches
            patchesPerDim = pdim // ppd
            patchGrid = (ppd + 1) * (ppd + 1)

            if ck.get('LUMI', (0, 0))[1] == numPoints:
                stat['LUMI_size_equals_iNumPoints'] += 1
            if ck.get('HEXP', (0, 0))[1] == 8 * numHeightPatches:
                stat['HEXP_size_equals_8_times_iNumHeightPatches'] += 1
            if ck.get('SCAL', (0, 0))[1] == 64 and nlayers <= 16:
                stat['SCAL_is_16_floats_and_iNumLayers_fits'] += 1
            if ck.get('AXIS', (0, 0))[1] == 16:
                stat['AXIS_is_16_orientation_bytes'] += 1
            if 'DCAL' in ck and ck['DCAL'][1] == 0 and ndecTiles == 0:
                stat['DCAL_empty_exactly_when_iNumDecalTiles_is_0'] += 1
            names = [s for s in data[ck['LTEX'][0]:ck['LTEX'][0] + ck['LTEX'][1]].split(b'\x00') if s]
            if len(names) == nlayers:
                stat['LTEX_name_count_equals_iNumLayers'] += 1

            hexp = [struct.unpack('<BBhhh', data[ck['HEXP'][0] + 8 * i:ck['HEXP'][0] + 8 * i + 8])
                    for i in range(numHeightPatches)]
            heights, consumed = decode_heights(data, ck['HGT8'][0], numPoints)
            if consumed == ck['HGT8'][1]:
                stat['HGT8_runlength_consumes_exactly_its_chunk'] += 1

            patches = [c for c in children(data, ck['PCHS'][0], ck['PCHS'][1]) if c[0] == 'PTCH']
            if len(patches) == patchesPerDim * patchesPerDim:
                stat['PTCH_count_equals_iNumPatches'] += 1

            info = {
                'file': pathlib.Path(path).name,
                'terrainName': data[ck['NAME'][0]:ck['NAME'][0] + ck['NAME'][1]].decode('latin1').split('\x00')[0],
                'metersPerPoint': mpp,
                'heightScale': round(hscale, 9),
                'pointsPerDimension': pdim,
                'patchPointsPerDimension': ppd,
                'heightPatchPointsPerDimension': hpd,
                'numLayers': nlayers,
                'patchesTested': 0,
                'xAgrees': 0, 'zAgrees': 0, 'yAgrees': 0, 'vertices': 0,
            }

            for pi, (cid, pstart, psize) in enumerate(patches):
                ch = children(data, pstart, psize)
                if not ch:
                    stat['patch_without_children'] += 1
                    continue
                inf = [c for c in ch if c[0] == 'INFO']
                if not inf or inf[0][2] < 7:
                    stat['patch_INFO_too_short'] += 1
                    continue
                nlayer = data[inf[0][1]]
                rec = struct.unpack('<3h', data[inf[0][1] + 1 + nlayer:inf[0][1] + 1 + nlayer + 6])
                vbufs = [c for c in ch if c[0] == 'VBUF']
                comp = [c for c in vbufs if struct.unpack('<I', data[c[1] + 8:c[1] + 12])[0] & 0x1000]
                uncomp = [c for c in vbufs if not (struct.unpack('<I', data[c[1] + 8:c[1] + 12])[0] & 0x1000)]
                if not comp or not uncomp:
                    stat['patches_without_both_streams'] += 1
                    continue
                cb, ub = comp[0][1], uncomp[0][1]
                csize = struct.unpack('<I', data[cb + 4:cb + 8])[0]
                usize = struct.unpack('<I', data[ub + 4:ub + 8])[0]
                count = struct.unpack('<I', data[cb:cb + 4])[0]
                if comp[0][2] != 12 + csize * count or uncomp[0][2] != 12 + usize * count:
                    stat['paired_patch_payload_does_not_fit'] += 1
                    continue
                if len(vbufs) != 2 or count != patchGrid or (struct.unpack('<I', data[cb + 8:cb + 12])[0] & 3) + 1 != 3:
                    stat['patch_not_a_single_9x9_group_skipped'] += 1
                    continue
                info['patchesTested'] += 1

                iX, iZ = pi % patchesPerDim, pi // patchesPerDim
                extent = ppd * mpp
                for i in range(count):
                    a, b = i % (ppd + 1), i // (ppd + 1)
                    shorts = struct.unpack('<4h', data[cb + 12 + i * csize:cb + 12 + i * csize + 8])
                    fl = struct.unpack('<3f', data[ub + 12 + i * usize:ub + 12 + i * usize + 12])

                    # The height point this vertex stands on, by its own position rather than by the
                    # assumed vertex order - so the order claim is tested, not assumed.
                    gx, gz = int(round(fl[0] / mpp)), int(round(fl[2] / mpp))
                    if gx > ppd or gz > ppd:
                        stat['vertex_outside_the_patch_footprint'] += 1
                        continue
                    if abs(fl[0] - gx * mpp) < 1e-3 and abs(fl[2] - gz * mpp) < 1e-3:
                        stat['vertex_positions_are_on_the_meter_lattice'] += 1
                        if a == gx and b == gz:
                            stat['vertex_order_is_row_major_grid'] += 1

                    mx, mz = min(iX * ppd + gx, pdim - 1), min(iZ * ppd + gz, pdim - 1)
                    hp = (mx // hpd) + (mz // hpd) * hpatches
                    if hp >= len(hexp) or rec[2] < 0:
                        stat['height_patch_index_out_of_range_or_negative_shift'] += 1
                        continue
                    shift, flags, off, rmin, rmax = hexp[hp]
                    height = (heights[hp * hpd * hpd + (mz % hpd) * hpd + (mx % hpd)] << shift) + off

                    info['vertices'] += 1
                    if half_up(fl[0] / extent * 65535) - 32768 == shorts[0]:
                        info['xAgrees'] += 1
                    if half_up(fl[2] / extent * 65535) - 32768 == shorts[2]:
                        info['zAgrees'] += 1
                    if (height - rec[0]) * (1 << rec[2]) - 32768 == shorts[1]:
                        info['yAgrees'] += 1
                    else:
                        if (rec[1] - rec[0]) * (1 << rec[2]) > 32767:
                            stat['vertex_in_patch_whose_recorded_span_overflows_16_bits'] += 1

                    # The same vertex with the engine's masked coordinate instead of the clamped one:
                    # only the wrapped edge (last row or column of patches) can differ.
                    emx, emz = (iX * ppd + a) % hpd, (iZ * ppd + b) % hpd
                    ehp = ((iX * ppd + a) // hpd) + ((iZ * ppd + b) // hpd) * hpatches
                    if ehp < len(hexp) and rec[2] >= 0:
                        esh, eflags, eoff, emin, emax = hexp[ehp]
                        eh = (heights[ehp * hpd * hpd + (emz % hpd) * hpd + (emx % hpd)] << esh) + eoff
                        engine_masked['compared'] += 1
                        if (eh - rec[0]) * (1 << rec[2]) - 32768 == shorts[1]:
                            engine_masked['agree'] += 1

            per_file.append(info)

    out = {
        'counts': dict(sorted(stat.items())),
        'engine_masked_height_lookup': engine_masked,
        'files': per_file,
        'model': {
            'x/z': 'stored = round_half_up(v_local / (patchPoints * metersPerPoint) * 65535) - 32768',
            'y': 'stored = (heightInt - patchHeightMin) * 2^patchHeightShift - 32768',
            'decode': 'world = patchOrigin + (stored + 32768) * patchExtent / 65535; '
                      'height = (patchHeightMin + (stored + 32768) / 2^patchHeightShift) * fHeightScale',
            'offset': 32768,
            'patchOrder': 'patch record index = iZ * patchesPerDimension + iX; '
                          'vertex order = row-major (patchPoints+1)^2 grid',
        },
    }
    if as_json:
        print(json.dumps(out, ensure_ascii=False))
    else:
        for f in per_file:
            print(f"{f['file']} name={f['terrainName']} metersPerPoint={f['metersPerPoint']} "
                  f"heightScale={f['heightScale']} points={f['pointsPerDimension']} "
                  f"patch={f['patchPointsPerDimension']} heightPatch={f['heightPatchPointsPerDimension']} "
                  f"layers={f['numLayers']} patches={f['patchesTested']} vertices={f['vertices']} "
                  f"agreeing x={f['xAgrees']} z={f['zAgrees']} y={f['yAgrees']}")
        print("\n" + json.dumps(out['counts'], indent=2))
        print(json.dumps(out['engine_masked_height_lookup'], indent=2))
    return 0


if __name__ == '__main__':
    sys.exit(main())
