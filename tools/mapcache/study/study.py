#!/usr/bin/env python3
"""Geometry-cache sizing study (read-only on the game files). Writes plain-text tables to stdout.

    python -B study.py <dvdroot_ps4> <section> [args]
sections: lod <map>...   near <map> x y z   maps   parts <map>...   connect   shared <mapA> <mapB>
Uses flstat.py's per-folder JSON cache (study/cache/flstat_<folder>.json).
"""
import collections
import math
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, '..'))
import mapcache as mc  # noqa: E402
import flstat  # noqa: E402

CACHE = os.path.join(HERE, 'cache')
SKY_RE = re.compile(r'sky|moon|sun', re.I)
L1, L2 = 0x01000000, 0x02000000
RADII = (25, 50, 100, 150, 250)


def folder_of(map_id):
    return 'm29_00_00_00' if map_id.startswith('m29') else map_id[:6] + '_00_00'


def corner_aabb(M, bb):
    xs, ys, zs = (bb[0], bb[3]), (bb[1], bb[4]), (bb[2], bb[5])
    w = [[M[r][0] * x + M[r][1] * y + M[r][2] * z + M[r][3] for r in range(3)] for x in xs for y in ys for z in zs]
    return [min(v[c] for v in w) for c in range(3)], [max(v[c] for v in w) for c in range(3)]


def lod_tris(fs):
    """face sets -> (t0, t1 or None, t2 or None)"""
    t = {f[0]: f[5] for f in fs}
    return t.get(0, 0), t.get(L1), t.get(L2)


def eff(t, level):
    """triangles at a level with fallback to the next finer level that exists."""
    t0, t1, t2 = t
    if level == 0:
        return t0, 0
    if level == 1:
        return (t1, 1) if t1 is not None else (t0, 0)
    if t2 is not None:
        return t2, 2
    return (t1, 1) if t1 is not None else (t0, 0)


class Map:
    def __init__(self, dv, map_id):
        self.id = map_id
        self.folder = folder_of(map_id)
        mp = os.path.join(dv, 'map', 'mapstudio', map_id + '.msb.dcx')
        if not os.path.exists(mp):
            mp = os.path.join(dv, 'map', 'mapstudio', map_id[:6] + '_00_00', map_id + '.msb.dcx')
        self.raw = mc.dcx_read(mp)
        self.msb = mc.read_msb(self.raw)
        self.stats = flstat.folder_stats(dv, self.folder, CACHE)
        self.ent_raw, self.type_raw = {}, {}
        for pname, _v, offs in mc._msb_params(self.raw):
            if pname == 'PARTS_PARAM_ST':
                for i, e in enumerate(offs):
                    eo, to = struct.unpack_from('<qq', self.raw, e + 0xB0)
                    self.ent_raw[i] = self.raw[e + eo:e + eo + 0x14]
                    self.type_raw[i] = self.raw[e + to:e + to + 0x20]
        self.pieces = []   # dicts
        self.missing = set()
        for p in self.msb.parts:
            if p.type != 0:
                continue
            stem = '%s_%s' % (self.folder, p.model[1:])
            fl = self.stats.get(stem)
            if fl is None or 'error' in fl:
                self.missing.add(p.model)
                continue
            M = mc.part_matrix(p.pos, p.rot, p.scale)
            meshes = []
            mn, mx = [math.inf] * 3, [-math.inf] * 3
            sky = False
            for k, m in enumerate(fl['meshes']):
                a, b = corner_aabb(M, m['bbox'])
                for c in range(3):
                    mn[c] = min(mn[c], a[c])
                    mx[c] = max(mx[c], b[c])
                meshes.append((k, lod_tris(m['fs'])))
                if SKY_RE.search(m['mtd'] + ' ' + m['mat']):
                    sky = True
            self.pieces.append({'part': p, 'stem': stem, 'meshes': meshes, 'min': mn, 'max': mx, 'sky': sky,
                                'lodid': self.ent_raw[p.index][0x0D]})
        lodbank_rows(dv)

    def models(self):
        return {pc['stem'] for pc in self.pieces}


def aabb_dist(pt, mn, mx):
    return math.sqrt(sum(max(mn[c] - pt[c], 0, pt[c] - mx[c]) ** 2 for c in range(3)))


_LODBANK = {}


def lodbank_rows(dv):
    """LodBank row id -> (lv01 border dist, lv12 border dist) from default_LodBank.param."""
    if not _LODBANK:
        fs = flstat.bnd4_files(mc.dcx_read(os.path.join(dv, 'param', 'drawparam', 'default_drawparam.parambnd.dcx')))
        b = fs[0][2]
        cnt, = struct.unpack_from('<H', b, 0x0A)
        for i in range(cnt):
            rid, _p, doff, _n = struct.unpack_from('<iiqq', b, 0x40 + 0x18 * i)
            d01, _p01, d12, _p12 = struct.unpack_from('<4f', b, doff)
            _LODBANK[rid] = (d01, d12)
    return _LODBANK


def game_level(d, pc):
    """LOD level the game's LodBank would pick at distance d (part LodParamID = entity byte +0x0D, 255 = row 0)."""
    rid = pc['lodid'] if pc['lodid'] != 255 else 0
    d01, d12 = _LODBANK.get(rid, _LODBANK[0])
    return 0 if d < d01 else (1 if d < d12 else 2)


def mixes():
    return {
        'L0': lambda d, pc: 0,
        'L1': lambda d, pc: 1,
        'lowest': lambda d, pc: 2,
        'L0<50,L1': lambda d, pc: 0 if d < 50 else 1,
        'L0<30,L1<100,lowest': lambda d, pc: 0 if d < 30 else (1 if d < 100 else 2),
        'game LodBank': game_level,
    }


def tally(pieces, fn_dist, mix):
    placed, uniq = 0, {}
    for pc, d in pieces:
        lv = mix(d, pc)
        for k, t in pc['meshes']:
            n, el = eff(t, lv)
            placed += n
            uniq[(pc['stem'], k, el)] = n
    return placed, sum(uniq.values()), len({(s, el) for (s, _k, el) in uniq})


def sec_near(dv, map_id, pt, label=''):
    m = Map(dv, map_id)
    pcs = [(pc, aabb_dist(pt, pc['min'], pc['max'])) for pc in m.pieces if not pc['sky']]
    print('\n## near %s %s point (%.1f, %.1f, %.1f); map pieces %d (+%d sky parts excluded)' % (
        map_id, label, *pt, len(pcs), sum(pc['sky'] for pc in m.pieces)))
    print('columns per mix: placed tris M / unique-model tris M / BLAS count (model x level)')
    print('radius | parts | models | ' + ' | '.join(mixes()))
    for r in RADII + (1e9,):
        sel = [(pc, d) for pc, d in pcs if d <= r]
        cells = []
        for name, f in mixes().items():
            pl, un, nb = tally(sel, None, f)
            cells.append('%.2f / %.2f / %d' % (pl / 1e6, un / 1e6, nb))
        print('%s | %d | %d | %s' % ('all' if r > 1e8 else r, len(sel), len({pc['stem'] for pc, _ in sel}), ' | '.join(cells)))


def sec_lod(dv, map_id):
    m = Map(dv, map_id)
    st = {s: m.stats[s] for s in m.models()}
    nsets = collections.Counter()
    tot = [0, 0, 0]
    have = [0, 0, 0]
    ratios1, ratios2 = [], []
    sky_t = 0
    for s, fl in st.items():
        for me in fl['meshes']:
            t = lod_tris(me['fs'])
            nsets[len(me['fs'])] += 1
            for lv in range(3):
                tot[lv] += eff(t, lv)[0]
            for lv in range(3):
                if t[lv] is not None:
                    have[lv] += t[lv]
            if t[1] is not None and t[0]:
                ratios1.append(t[1] / t[0])
            if t[2] is not None and t[0]:
                ratios2.append(t[2] / t[0])
    placed = [0, 0, 0]
    placed_nosky = [0, 0, 0]
    for pc in m.pieces:
        for _k, t in pc['meshes']:
            for lv in range(3):
                placed[lv] += eff(t, lv)[0]
                if not pc['sky']:
                    placed_nosky[lv] += eff(t, lv)[0]

    def q(v, f):
        v = sorted(v)
        return v[int(f * (len(v) - 1))] if v else float('nan')
    print('\n## LOD %s: %d placed map pieces, %d unique models, %d meshes' % (map_id, len(m.pieces), len(st), sum(nsets.values())))
    print('meshes by face-set count', dict(sorted(nsets.items())))
    print('unique tris  L0 %d | L1 eff %d | L2 eff %d | L1 only-where-present %d | L2 present %d' % (tot[0], tot[1], tot[2], have[1], have[2]))
    print('placed tris  L0 %d | L1 eff %d | L2 eff %d' % tuple(placed))
    print('placed tris without sky parts L0 %d | L1 %d | L2 %d' % tuple(placed_nosky))
    print('per-mesh LOD1/LOD0 median %.3f p10 %.3f p90 %.3f (n=%d); LOD2/LOD0 median %.3f p10 %.3f p90 %.3f (n=%d)' % (
        q(ratios1, .5), q(ratios1, .1), q(ratios1, .9), len(ratios1), q(ratios2, .5), q(ratios2, .1), q(ratios2, .9), len(ratios2)))
    # which meshes lack LODs: size of their triangle sets
    single = [eff(lod_tris(me['fs']), 0)[0] for fl in st.values() for me in fl['meshes'] if len(me['fs']) == 1]
    print('meshes with only LOD0: %d, tris %d, median %d' % (len(single), sum(single), q(single, .5) if single else 0))
    # distant-view parts
    print('parts with 遠景 / LOD / far in desc:')
    for pc in m.pieces:
        p = pc['part']
        if re.search(r'遠景|LOD|lod|far|Far', p.desc):
            t = [sum(eff(t, lv)[0] for _k, t in pc['meshes']) for lv in range(3)]
            print('   %s %s "%s" pos (%.0f, %.0f, %.0f) extent %.0f m, tris %s' % (
                p.name, p.model, p.desc, *p.pos, max(pc['max'][c] - pc['min'][c] for c in range(3)), t))
    # biggest models by L0 extent (candidate impostors)
    big = sorted(m.pieces, key=lambda pc: -max(pc['max'][c] - pc['min'][c] for c in (0, 2)))[:12]
    print('largest placed parts (XZ extent):')
    for pc in big:
        p = pc['part']
        t = [sum(eff(t, lv)[0] for _k, t in pc['meshes']) for lv in range(3)]
        print('   %s %s "%s" sky=%s extent %.0f x %.0f x %.0f, tris %s' % (
            p.name, p.model, p.desc, pc['sky'], *[pc['max'][c] - pc['min'][c] for c in range(3)], t))
    # LodParamID candidates: entity data bytes 4..0x13 of map pieces
    for off in range(4, 0x14):
        c = collections.Counter(m.ent_raw[pc['part'].index][off] for pc in m.pieces)
        if len(c) > 1:
            print('entity byte +0x%02x: %s' % (off, sorted(c.items())[:12]))


def sec_maps(dv, ids):
    print('\n## maps: id | map pieces | models | placed L0 | placed lowest | unique L0 | unique lowest | sky placed L0 | world AABB (no sky)')
    for map_id in ids:
        m = Map(dv, map_id)
        pl = [0, 0]
        sky0 = 0
        uq = {}
        mn, mx = [math.inf] * 3, [-math.inf] * 3
        for pc in m.pieces:
            for k, t in pc['meshes']:
                pl[0] += t[0]
                pl[1] += eff(t, 2)[0]
                uq[(pc['stem'], k)] = (t[0], eff(t, 2)[0])
                if pc['sky']:
                    sky0 += t[0]
            if not pc['sky']:
                for c in range(3):
                    mn[c] = min(mn[c], pc['min'][c])
                    mx[c] = max(mx[c], pc['max'][c])
        print('%s | %d | %d | %d | %d | %d | %d | %d | (%.0f,%.0f,%.0f)-(%.0f,%.0f,%.0f) missing %s' % (
            map_id, len(m.pieces), len(m.models()), pl[0], pl[1], sum(v[0] for v in uq.values()),
            sum(v[1] for v in uq.values()), sky0, *mn, *mx, sorted(m.missing)))


def sec_parts(dv, ids):
    print('\n## parts: map | ' + ' | '.join(mc.PART_TYPES.values()) + ' | map pieces with entity id | objects with entity id')
    for map_id in ids:
        m = Map(dv, map_id)
        c = collections.Counter(p.type for p in m.msb.parts)
        pe = sum(1 for p in m.msb.parts if p.type == 0 and p.entity_id not in (-1, 0))
        oe = sum(1 for p in m.msb.parts if p.type == 1 and p.entity_id not in (-1, 0))
        print('%s | %s | %d | %d' % (map_id, ' | '.join(str(c.get(t, 0)) for t in mc.PART_TYPES), pe, oe))


def sec_connect(dv, ids):
    print('\n## connect collisions: map | part | entity | collision index | target map bytes')
    for map_id in ids:
        m = Map(dv, map_id)
        tgt = collections.Counter()
        for p in m.msb.parts:
            if p.type == 11:
                tr = m.type_raw[p.index]
                ci, = struct.unpack_from('<i', tr, 0)
                tgt['m%02d_%02d_%02d_%02d' % tuple(tr[4:8])] += 1
        print('%s | %s' % (map_id, dict(tgt)))


def sec_shared(dv, a, b):
    """Same model file bytes placed at the same world position in two maps."""
    ma, mb = Map(dv, a), Map(dv, b)
    import hashlib

    def h(folder, stem):
        p = os.path.join(dv, 'map', folder, stem + '.flver.dcx')
        return hashlib.md5(mc.dcx_read(p)).hexdigest()
    ha = {s: h(ma.folder, s) for s in ma.models()}
    hb = {s: h(mb.folder, s) for s in mb.models()}
    same_models = set(ha.values()) & set(hb.values())
    pa = collections.defaultdict(list)
    for pc in ma.pieces:
        pa[ha[pc['stem']]].append(pc)
    exact, near, tris = 0, 0, 0
    offsets = collections.Counter()
    for pc in mb.pieces:
        for qa in pa.get(hb[pc['stem']], []):
            d = [pc['part'].pos[c] - qa['part'].pos[c] for c in range(3)]
            if max(abs(x) for x in d) < 0.01 and max(abs(pc['part'].rot[c] - qa['part'].rot[c]) for c in range(3)) < 0.01:
                exact += 1
                tris += sum(t[0] for _k, t in pc['meshes'])
                break
            offsets[tuple(round(x) for x in d)] += 1
    print('\n## shared %s / %s: identical model files %d (of %d / %d); identical placements (same file, pos, rot) %d, L0 tris %d' % (
        a, b, len(same_models), len(ha), len(hb), exact, tris))
    print('most common position offsets between same-file parts (not exact):', offsets.most_common(5))
    # AABB overlap of the two maps (no sky)
    def box(m):
        mn, mx = [math.inf] * 3, [-math.inf] * 3
        for pc in m.pieces:
            if pc['sky']:
                continue
            for c in range(3):
                mn[c] = min(mn[c], pc['min'][c])
                mx[c] = max(mx[c], pc['max'][c])
        return mn, mx
    print('boxes', box(ma), box(mb))


def main(argv):
    dv, sec = argv[0], argv[1]
    sys.stdout.reconfigure(encoding='utf-8')
    if sec == 'lod':
        for mp in argv[2:]:
            sec_lod(dv, mp)
    elif sec == 'near':
        sec_near(dv, argv[2], tuple(float(x) for x in argv[3:6]), ' '.join(argv[6:]))
    elif sec == 'maps':
        sec_maps(dv, argv[2:])
    elif sec == 'parts':
        sec_parts(dv, argv[2:])
    elif sec == 'connect':
        sec_connect(dv, argv[2:])
    elif sec == 'shared':
        sec_shared(dv, argv[2], argv[3])


if __name__ == '__main__':
    main(sys.argv[1:])
