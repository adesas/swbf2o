#!/usr/bin/env python3
"""Read SWBF2 `segm` segments straight out of a .lvl and check what the engine says they hold.

The schema here is not guessed. It is transcribed from the engine's own reader,
RedEngineFL/Graphics/PC/pcRedSegment.cpp::RedSegment::Read, which dispatches on child chunk id,
and from pcRedVertexFormat.cpp::CreateVertexDeclaration, which defines the RVF vertex-format bits.
Keeping both in one place matters: the stride is derived from the format bits, then compared against
the per-vertex size the file states. If those two agree, the format decode is right; if they do not,
something above was transcribed wrong - and that is the answer this script is looking for.

Usage: python3 tools/modl_probe.py <file.lvl> [...] [-v]
"""
import struct
import sys
import pathlib
import collections
import json

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import lvl_reader as L

# Child ids are the four-character codes compared as little-endian uint32 in the switch,
# e.g. case 0x46554256 == 'IBUF' read little-endian.
IDS = {
    0x4C52544D: 'MTRL', 0x50595452: 'RTYP', 0x4D414E54: 'TNAM', 0x54414456: 'VDAT',
    0x46554256: 'VBUF', 0x46554249: 'IBUF', 0x4F464E49: 'INFO', 0x4E494B53: 'SKIN',
    0x50414D42: 'BMAP', 0x4D414E42: 'BNAM', 0x4D414E4D: 'MNAM', 0x584F4242: 'BBOX',
}


def rvf_stride(fmt):
    """Bytes per vertex, per pcRedVertexFormat::CreateVertexDeclaration (buffer side).

    The engine computes the declaration's final offset from both a buffer RVF and an input RVF;
    for stored data the two are the same, so input==buffer is passed throughout here.
    """
    off = 0
    if fmt & 3:
        buf_c = (fmt & 3) + 1
        in_c = buf_c
        if fmt & 0x1000:
            # packed positions: SHORT4 when more than two components, else SHORT2
            off += 8 if in_c > 2 else 4
        else:
            if in_c > 1:
                pass  # FLOATn declaration emitted here; offset is derived from buffer components
            off += buf_c * 4
    if fmt & 0x1C:
        buf_w = (fmt >> 2) & 7
        in_w = min(buf_w, buf_w)
        if buf_w > 1:
            if fmt & 0x2000:
                off += 4
            else:
                off += buf_w * 4 - 4
        off += 4
    if fmt & 0x20:
        off += 4 if (fmt & 0x4000) else 12
    if fmt & 0x40:
        off += 8 if (fmt & 0x4000) else 24
    if fmt & 0x80:
        off += 4
    if fmt & 0x100:
        off += 4
    if fmt & 0x600:
        n = (fmt >> 9) & 3
        off += n * (4 if (fmt & 0x8000) else 8)
    return off


def rvf_describe(fmt):
    """Human-readable component list, in the order the engine lays the elements out."""
    parts = []
    off = 0

    def add(name, size):
        nonlocal off
        parts.append('%s@%d' % (name, off))
        off += size

    if fmt & 3:
        c = (fmt & 3) + 1
        if fmt & 0x1000:
            add('pos:short%d' % (4 if c > 2 else 2), 8 if c > 2 else 4)
        else:
            add('pos:float%d' % c, c * 4)
    if fmt & 0x1C:
        w = (fmt >> 2) & 7
        if w > 1:
            add('weights:%s%d' % ('color' if (fmt & 0x2000) else 'float', w), 4 if (fmt & 0x2000) else w * 4 - 4)
        add('bindices:color', 4)
    if fmt & 0x20:
        add('normal:%s' % ('color' if (fmt & 0x4000) else 'float3'), 4 if (fmt & 0x4000) else 12)
    if fmt & 0x40:
        add('tan+binormal', 8 if (fmt & 0x4000) else 24)
    if fmt & 0x80:
        add('diffuse:color', 4)
    if fmt & 0x100:
        add('specular:color', 4)
    if fmt & 0x600:
        n = (fmt >> 9) & 3
        add('texcoord x%d:%s' % (n, 'short2' if (fmt & 0x8000) else 'float2'), n * (4 if (fmt & 0x8000) else 8))
    return parts


def children_of(node):
    """lvl_reader gives (id, size, payload_start, children-or-None); the top level is a plain list."""
    items = node if isinstance(node, list) else node[3]
    return items or []


def parse_segment(seg_bytes_at, data, start, end):
    """Parse one `segm` payload [start,end) by chunk id. Returns a dict."""
    pos = start
    out = {'textures': {}, 'streams': [], 'strings': {}}
    while pos + 8 <= end:
        cid = data[pos:pos + 4]
        size = struct.unpack_from('<I', data, pos + 4)[0]
        p = pos + 8
        if p + size > end:
            out['error'] = 'child %r size %d overruns segment at %d' % (cid, size, pos)
            return out
        name = IDS.get(struct.unpack('<I', cid)[0], cid.decode('latin1'))
        body = data[p:p + size]
        try:
            if name == 'INFO' and size >= 12:
                out['primType'], out['numVertices'], out['primCount'] = struct.unpack_from('<III', body, 0)
            elif name in ('VDAT', 'VBUF') and size >= 12:
                count, vsize, fmt = struct.unpack_from('<III', body, 0)
                out['streams'].append({'kind': name, 'count': count, 'size': vsize, 'fmt': fmt,
                                       'payload': p + 12, 'declared': size - 12})
            elif name == 'IBUF' and size >= 4:
                out['numIndices'] = struct.unpack_from('<I', body, 0)[0]
                out['indexPayload'] = p + 4
                out['indexBytes'] = size - 4
            elif name == 'BBOX' and size >= 24:
                out['bbox'] = struct.unpack_from('<6f', body, 0)
            elif name == 'MTRL' and size >= 28:
                flags, c0, c1, power, d0, d1 = struct.unpack_from('<IIIfBB', body, 0) if False else (0,) * 6
                flags, col_d, col_s, power = struct.unpack_from('<IIIf', body, 0)
                out['material'] = {'flags': flags, 'diffuse': col_d, 'specular': col_s,
                                   'power': power, 'raw': body[:28].hex()}
                out['strings']['MTRL_tail'] = body[24:60]
            elif name == 'TNAM' and size >= 4:
                idx = struct.unpack_from('<I', body, 0)[0]
                out['textures'][idx] = body[4:68]
            elif name in ('RTYP', 'MNAM', 'BNAM'):
                out['strings'][name] = body[:40]
        except Exception as exc:                                  # keep going: report, don't crash
            out.setdefault('errors', []).append('%s: %s' % (name, exc))
        pos = p + size
        pos += (-pos) & 3
    return out


def find_chunks(node, want, depth=0, out=None, path=''):
    if out is None:
        out = []
    for kid in children_of(node):
        cid, size, pstart, kids = kid
        name = cid.decode('latin1')
        if name in want:
            out.append((name, pstart, pstart + size, path + '/' + name))
        if kids:
            find_chunks(kid, want, depth + 1, out, path + '/' + name)
    return out


def collect_nodes(root, want, data, out=None):
    """Every node whose id is in `want`, as (name, node) in file order."""
    if out is None:
        out = []
    for kid in children_of(root):
        name = kid[0].decode('latin1')
        if name in want:
            out.append((name, kid))
        out.extend(collect_nodes(kid, want, data, []))
    return out


def host_view(root, data):
    """The counts as the browser host derives them, stated here as an independent check.

    The selection rule is the host's: the first usable vertex stream, replaced by a packed one
    when the segment carries both. Indices are what IBUF declares, and the draw count is
    INFO's primCount (falling back to numIndices), not the even-padded allocation. Segments
    without a usable vertex stream are not counted as segments by the host, so they are counted
    separately here rather than silently folded in.
    """
    acc = {'models': 0, 'segments_with_stream': 0, 'segments_without_stream': 0,
           'vertices': 0, 'indices': 0, 'draw_total': 0, 'packed_streams': 0, 'plain_only': 0}
    models = collect_nodes(root, {'modl'}, data)
    acc['models'] = len(models)
    for name, node in collect_nodes(root, {'segm'}, data):
        seg = parse_segment(None, data, node[2], node[2] + node[1])
        if 'error' in seg:
            continue
        sel = None
        for st in seg['streams']:
            if st['count'] > 0 and st['size'] > 0 and (sel is None or (st['fmt'] & 0x1000)):
                sel = st
        if sel is None:
            acc['segments_without_stream'] += 1
            continue
        acc['segments_with_stream'] += 1
        acc['packed_streams'] += 1 if (sel['fmt'] & 0x1000) else 0
        acc['plain_only'] += 0 if (sel['fmt'] & 0x1000) else 1
        acc['vertices'] += sel['count']
        if 'numIndices' in seg:
            acc['indices'] += seg['numIndices']
        draw = seg.get('primCount') or seg.get('numIndices', 0)
        acc['draw_total'] += draw
    return acc


# D3DPRIMITIVETYPE values, so the prim type seen in the data is named rather than reported as a
# number. primType 5 is D3DPT_TRIANGLESTRIP; the arithmetic that confirms it is that a strip of n
# indices has n-2 primitives, which is what primCount states for every segment tested.
PRIMTYPES = {1: 'D3DPT_POINTLIST', 2: 'D3DPT_LINELIST', 3: 'D3DPT_LINESTRIP',
            4: 'D3DPT_TRIANGLELIST', 5: 'D3DPT_TRIANGLESTRIP', 6: 'D3DPT_TRIANGLEFAN'}


def segment_facts(root, data):
    """Per-segment counts as the file states them, plus the strip arithmetic."""
    facts = []
    for name, node in collect_nodes(root, {'segm'}, data):
        seg = parse_segment(None, data, node[2], node[2] + node[1])
        if 'error' in seg or 'primCount' not in seg:
            continue
        facts.append((node, seg))
    return facts


def instance_links(root, data):
    """inst records and whether the host could place them.

    A record is placeable when its TYPE names a model in the same container and it carries a full
    transform (48 bytes: four rows of three floats). Both halves are facts about the file.

    The count is split by how the record is reached. The host's documented walk descends only through
    ids the format uses as parents (ucfb, lvl_), so an instance inside a group id that policy does not
    recognise - `lvl_/@Sc*/wrld/inst` in tat2 - is outside what the host can see. Saying which side of
    that line each record falls on is the point: a difference that is explained is not a disagreement.
    """
    names = set()
    for name, node in collect_nodes(root, {'modl'}, data):
        for kid in children_of(node):
            if kid[0].decode('latin1') == 'NAME':
                raw = data[kid[2]:kid[2] + kid[1]].split(b'\x00', 1)[0]
                names.add(raw.decode('latin1', 'replace'))

    def placeable(node):
        t = None
        xfrm = 0
        for kid in children_of(node):
            if kid[0].decode('latin1') != 'INFO':
                continue
            for g in children_of(kid):
                if g[0].decode('latin1') == 'TYPE':
                    t = data[g[2]:g[2] + g[1]].split(b'\x00', 1)[0].decode('latin1', 'replace')
                elif g[0].decode('latin1') == 'XFRM':
                    xfrm = g[1]
        return t, xfrm

    counts = {'inst_through_walkable_wrld': 0, 'inst_under_unrecognised_group': 0,
             'inst_placeable_by_host': 0, 'inst_with_transform': 0, 'inst_no_type': 0}

    def walk(node, path):
        for kid in children_of(node):
            nm = kid[0].decode('latin1')
            if nm == 'inst':
                through_policy = len(path) >= 2 and path[-1] == 'wrld' and path[-2] in ('ucfb', 'lvl_')
                counts['inst_through_walkable_wrld' if through_policy else 'inst_under_unrecognised_group'] += 1
                t, xfrm = placeable(kid)
                if xfrm >= 48:
                    counts['inst_with_transform'] += 1
                if t is None:
                    counts['inst_no_type'] += 1
                elif t in names and xfrm >= 48 and through_policy:
                    counts['inst_placeable_by_host'] += 1
            walk(kid, path + [nm])

    # parse_file hands back the top level as a plain list, so the root's own id is not carried;
    # the tool only gets here for a ucfb container, which is what the path is anchored on.
    walk(root, ['ucfb'])

    # The totals the host would reach through placeable instances, computed here from the file so
    # that the browser's draw path can be checked against an independent tally rather than against
    # its own bookkeeping. An instanced model is reached once per instance, so these totals are
    # larger than the per-segment sums - that difference is the point.
    modelSegs = {}
    for name, node in collect_nodes(root, {'modl'}, data):
        nm = None
        for kid in children_of(node):
            if kid[0].decode('latin1') == 'NAME':
                nm = data[kid[2]:kid[2] + kid[1]].split(b'\x00', 1)[0].decode('latin1', 'replace')
        segs = []
        for kid in children_of(node):
            if kid[0].decode('latin1') == 'segm':
                segs.append(parse_segment(None, data, kid[2], kid[2] + kid[1]))
        if nm:
            modelSegs[nm] = segs
    placed = {'placed_segments': 0, 'placed_prim_total': 0, 'placed_index_total': 0}
    for name, node in collect_nodes(root, {'inst'}, data):
        t, xfrm = placeable(node)
        if t is None or xfrm < 48 or t not in names:
            continue
        segs = modelSegs.get(t, [])
        placed['placed_segments'] += len(segs)
        for s in segs:
            placed['placed_prim_total'] += s.get('primCount', 0)
            placed['placed_index_total'] += s.get('numIndices', 0)
    counts.update(placed)
    return counts


def main(argv):
    verbose = '-v' in argv
    json_mode = '--json' in argv
    # Human output goes to stderr in --json mode so stdout stays a single parseable record.
    say = (lambda *a, **k: None) if json_mode else print
    files = [a for a in argv if not a.startswith('-')]
    base = pathlib.Path('data/Star wars battlefront 2/GameData/data/_lvl_pc')
    for f in files:
        p = pathlib.Path(f)
        if not p.exists() and (base / f).exists():
            p = base / f
        root, magic = L.parse_file(str(p))
        if not root:
            say('%s: not a ucfb container (magic %r)' % (f, magic))
            continue
        data = p.read_bytes()
        found = find_chunks(root, {'segm', 'modl', 'VBUF'})
        segs = [x for x in found if x[0] == 'segm']
        say('=== %s: %d modl, %d segm, %d VBUF' %
              (p.name, sum(1 for x in found if x[0] == 'modl'), len(segs),
               sum(1 for x in found if x[0] == 'VBUF')))

        fmt_seen = collections.Counter()
        stride_ok = stride_bad = 0
        primtypes = collections.Counter()
        badidx = 0
        idx_ok = 0
        parsed = 0
        strip_ok = strip_bad = 0
        texnames = collections.Counter()
        for name, s, e, path in segs:
            seg = parse_segment(None, data, s, e)
            if 'error' in seg:
                say('   !! %s' % seg['error'])
                continue
            parsed += 1
            for st in seg['streams']:
                want = rvf_stride(st['fmt'])
                fmt_seen[st['fmt']] += 1
                if want == st['size'] and want * st['count'] == st['declared']:
                    stride_ok += 1
                else:
                    stride_bad += 1
                    if verbose and stride_bad <= 8:
                        say('   stride mismatch fmt=0x%04x size=%d computed=%d count=%d payload=%d/%d'
                              % (st['fmt'], st['size'], want, st['count'], st['declared'],
                                 want * st['count']))
            if 'numIndices' in seg:
                if seg['indexBytes'] >= seg['numIndices'] * 2:
                    idx_ok += 1
                else:
                    badidx += 1
            if 'primType' in seg:
                primtypes[seg['primType']] += 1
                if 'numIndices' in seg and seg['primCount'] == seg['numIndices'] - 2:
                    strip_ok += 1
                elif 'numIndices' in seg:
                    strip_bad += 1
            for i, raw in seg['textures'].items():
                texnames[raw.split(b'\x00', 1)[0]] += 1
        say('   vertex streams: stride agrees %d, disagrees %d' % (stride_ok, stride_bad))
        say('   index streams: numIndices fits the payload %d, does not %d' % (idx_ok, badidx))
        say('   primitive types seen: %s' % dict(primtypes.most_common(6)))
        say('   vertex formats seen (fmt: count):')
        for fmt, k in fmt_seen.most_common(8):
            say('      0x%04x x%-4d -> %s' % (fmt, k, ' '.join(rvf_describe(fmt))))
        sample = list(texnames.items())[:6]
        say('   texture names bound by segments: %d distinct, e.g. %s' %
              (len(texnames), ['%s:%d' % (n.decode('latin1', 'replace'), c) for n, c in sample]))
        if '--json' in argv:
            print(json.dumps({
                'file': p.name,
                'path': str(f),
                'modl': sum(1 for x in found if x[0] == 'modl'),
                'segm': len(segs),
                'vbuf': sum(1 for x in found if x[0] == 'VBUF'),
                'segments_parsed': parsed,
                'stride_ok': stride_ok, 'stride_bad': stride_bad,
                'idx_ok': idx_ok, 'idx_bad': badidx,
                'primtypes': {str(k): (PRIMTYPES.get(k, 'unknown'), v) for k, v in primtypes.most_common()},
                'strip_arithmetic': strip_ok, 'strip_arithmetic_bad': strip_bad,
                'distinct_tex_names': len(texnames),
                'host_view': host_view(root, data),
                'instances': instance_links(root, data),
            }))
            continue
        if verbose and segs:
            name, s, e, path = segs[0]
            seg = parse_segment(None, data, s, e)
            say('   first segment: %s' % {k: v for k, v in seg.items()
                                             if k in ('primType', 'numVertices', 'primCount',
                                                      'numIndices', 'bbox')})
            for st in seg['streams']:
                say('      stream %s count=%d size=%d fmt=0x%04x' %
                      (st['kind'], st['count'], st['size'], st['fmt']))


if __name__ == '__main__':
    main(sys.argv[1:])
