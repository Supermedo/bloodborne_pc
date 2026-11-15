"""Event-script toggles of map pieces (2012[01] map part enable) and objects (2005[xx]) per map.

    python -B toggles.py <dvdroot> <map>...
For each map: every instruction instance (event parameters substituted, common.emevd events included)
whose args name a MapPiece / Object / DummyObject / Collision entity id of that map's MSB.
"""
import collections
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, '..'))
import mapcache as mc  # noqa: E402
import emevd  # noqa: E402
import emedf  # noqa: E402
import flstat  # noqa: E402

_obj = {}


def obj_tris(dv, model):
    if model in _obj:
        return _obj[model]
    p = os.path.join(dv, 'obj', model + '.objbnd.dcx')
    r = None
    if os.path.exists(p):
        b = mc.dcx_read(p)
        o = b.find(b'FLVER\0')
        if o >= 0:
            try:
                st = flstat.stat_flver(b[o:], model)
                r = (sum(f[5] for m in st['meshes'] for f in m['fs'] if f[0] == 0), st['meshes'][0]['mtd'] if st['meshes'] else '')
            except Exception as e:  # noqa: BLE001
                r = ('err %s' % e, '')
    _obj[model] = r
    return r


def map_tris(dv, folder, model):
    st = flstat.folder_stats(dv, folder, os.path.join(HERE, 'cache')).get('%s_%s' % (folder, model[1:]))
    if not st or 'error' in st:
        return None
    return sum(f[5] for m in st['meshes'] for f in m['fs'] if f[0] == 0)


def main(dv, maps):
    names = emedf.names(dv)
    common = emevd.read_emevd(mc.dcx_read(os.path.join(dv, 'event', 'common.emevd.dcx')))
    for mp in maps:
        folder = mp[:6] + '_00_00'
        msb = mc.read_msb(mc.dcx_read(os.path.join(dv, 'map', 'mapstudio', mp + '.msb.dcx')))
        ids = {p.entity_id: p for p in msb.parts if p.entity_id not in (-1, 0) and p.type in (0, 1, 5, 9, 11)}
        ev = emevd.read_emevd(mc.dcx_read(os.path.join(dv, 'event', mp + '.emevd.dcx')))
        area = os.path.join(dv, 'event', mp[:3] + '.emevd.dcx')
        extra = [common] + ([emevd.read_emevd(mc.dcx_read(area))] if os.path.exists(area) else [])
        hits = collections.defaultdict(lambda: collections.defaultdict(set))   # eid -> instr -> {(event, state)}
        for eid, caller, ins in emevd.instances(ev, extra):
            for bank, iid, a in ins:
                if bank == 2000 or len(a) < 4:
                    continue
                for k in range(0, len(a) - 3, 4):
                    v, = struct.unpack_from('<i', a, k)
                    if v in ids:
                        st = a[k + 4] if len(a) > k + 4 else None
                        hits[v]['%d[%02d]' % (bank, iid)].add((eid, st))
        print('\n### %s: entity-id parts MapPiece %d, Object %d, DummyObject %d, Collision %d, ConnectCollision %d' % (
            mp, *[sum(1 for p in ids.values() if p.type == t) for t in (0, 1, 9, 5, 11)]))
        by_instr = collections.Counter()
        for v, d in hits.items():
            for k in d:
                by_instr[(mc.PART_TYPES[ids[v].type], k)] += 1
        print('instruction use (part type, instruction, name, distinct entity ids):')
        for (t, k), n in sorted(by_instr.items()):
            b_, i_ = k.split('[')[0], k.split('[')[1][:-1]
            print('   %s %s %s %d' % (t, k, names.get((int(b_), int(i_)), ('', '?'))[1], n))
        print('map pieces named by events:')
        for v, p in sorted(ids.items()):
            if p.type != 0:
                continue
            d = hits.get(v, {})
            print('   %d %s %s "%s" L0 tris %s pos (%.0f,%.0f,%.0f): %s' % (
                v, p.name, p.model, p.desc, map_tris(dv, folder, p.model), *p.pos,
                '; '.join('%s x%d (events %s)' % (k, len(s), sorted({e for e, _ in s})[:4]) for k, s in d.items()) or 'NOT referenced'))
        print('objects/dummy objects toggled by 2005[03] (enable), 2005[01]/[08] (destroy), 2005[07] (anim replay):')
        rows = collections.defaultdict(list)
        for v, d in sorted(hits.items()):
            p = ids[v]
            if p.type not in (1, 9):
                continue
            ks = [k for k in d if k in ('2005[03]', '2005[01]', '2005[08]', '2005[07]', '2005[02]')]
            if ks:
                rows[(p.model, p.desc)].append((v, ','.join(sorted(ks))))
        for (model, desc), lst in sorted(rows.items()):
            print('   %s "%s" tris %s: %s' % (model, desc, obj_tris(dv, model), ' '.join('%d:%s' % x for x in lst)))


if __name__ == '__main__':
    sys.stdout.reconfigure(encoding='utf-8')
    main(sys.argv[1], sys.argv[2:])
