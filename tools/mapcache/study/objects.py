"""Object parts of a map: models, placed triangles (obj/*.objbnd.dcx), and ObjectParam row of each model
(gameparam ObjectParam.param; row id = model number, first field read as hp s16 as in DS1's OBJECT_PARAM_ST,
-1 = unbreakable; this field layout is assumed, not verified for Bloodborne).

    python -B objects.py <dvdroot> <map>...
"""
import collections
import os
import re
import struct
import sys

import flstat
import toggles


def object_param(dv):
    fs = flstat.bnd4_files(flstat.mc.dcx_read(os.path.join(dv, 'param', 'gameparam', 'gameparam.parambnd.dcx')))
    b = [d for _i, n, d in fs if n.endswith('ObjectParam.param')][0]
    cnt, = struct.unpack_from('<H', b, 0x0A)
    rows = {}
    for i in range(cnt):
        rid, _p, doff, _n = struct.unpack_from('<iiqq', b, 0x40 + 0x18 * i)
        rows[rid] = b[doff:doff + 0x20]
    return rows


def main(dv, maps):
    sys.stdout.reconfigure(encoding='utf-8')
    op = object_param(dv)
    print('ObjectParam rows', len(op), 'hp values (s16 @0):', collections.Counter(struct.unpack_from('<h', r, 0)[0] for r in op.values()).most_common(8))
    for mp in maps:
        msb = flstat.mc.read_msb(flstat.mc.dcx_read(os.path.join(dv, 'map', 'mapstudio', mp + '.msb.dcx')))
        objs = [p for p in msb.parts if p.type == 1]
        models = collections.Counter(p.model for p in objs)
        placed = uniq = 0
        cat = collections.Counter()
        cat_t = collections.Counter()
        for p in objs:
            t = toggles.obj_tris(dv, p.model)
            n = t[0] if t and isinstance(t[0], int) else 0
            placed += n
            row = op.get(int(p.model[1:]))
            hp = struct.unpack_from('<h', row, 0)[0] if row else None
            ent = p.entity_id not in (-1, 0)
            name = p.desc
            if re.search(r'扉|ドア|門', name):
                k = 'door'
            elif re.search(r'エレベーター|リフト|昇降', name):
                k = 'lift'
            elif re.search(r'霧壁', name):
                k = 'fog wall'
            elif re.search(r'宝箱|宝死体|ボディジェム|アイテム', name):
                k = 'item/chest'
            elif hp is not None and hp > 0:
                k = 'breakable (hp>0)'
            elif re.search(r'常駐|o_layout', name):
                k = 'resident layout (static)'
            else:
                k = 'other'
            cat[(k, ent)] += 1
            cat_t[(k, ent)] += n
        for m in models:
            t = toggles.obj_tris(dv, m)
            uniq += t[0] if t and isinstance(t[0], int) else 0
        print('\n## %s objects: %d parts, %d models, placed L0 tris %d, unique-model L0 tris %d' % (mp, len(objs), len(models), placed, uniq))
        for (k, ent), v in sorted(cat.items()):
            print('   %-26s entity id %-5s parts %5d  placed tris %d' % (k, ent, v, cat_t[(k, ent)]))


if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2:])
