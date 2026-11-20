"""Do two maps share geometry in one world space? Match placed meshes by (vertex count, LOD-0 tris, mesh AABB
size) and histogram the world-AABB-min offset between matches. Exact duplicates = offset (0,0,0).

    python -B shared_geo.py <dvdroot> <mapA> <mapB>
"""
import collections
import math
import sys

import study


def placed(m):
    out = collections.defaultdict(list)
    for pc in m.pieces:
        if pc['sky']:
            continue
        p = pc['part']
        M = study.mc.part_matrix(p.pos, p.rot, p.scale)
        fl = m.stats[pc['stem']]
        for k, me in enumerate(fl['meshes']):
            mn, mx = study.corner_aabb(M, me['bbox'])
            size = tuple(round(mx[c] - mn[c], 1) for c in range(3))
            out[(me['vc'], study.lod_tris(me['fs'])[0], size)].append((mn, pc))
    return out


def main(dv, a, b):
    sys.stdout.reconfigure(encoding='utf-8')
    ma, mb = study.Map(dv, a), study.Map(dv, b)
    pa, pb = placed(ma), placed(mb)
    offs = collections.Counter()
    dup_tris = 0
    dup = []
    for key, la in pa.items():
        lb = pb.get(key)
        if not lb:
            continue
        for mn_b, pcb in lb:
            best = min(la, key=lambda x: math.dist(x[0], mn_b))
            d = tuple(round(mn_b[c] - best[0][c], 1) for c in range(3))
            offs[d] += 1
            if max(abs(x) for x in d) <= 0.1:
                dup_tris += key[1]
                dup.append((best[1]['part'].name, pcb['part'].name, key[1], tuple(round(v) for v in mn_b)))
    print('%s vs %s: mesh signatures A %d, B %d, matched B meshes %d' % (a, b, len(pa), len(pb), sum(offs.values())))
    print('most common offsets B-A:', offs.most_common(6))
    print('exact duplicates (offset 0): %d meshes, %d L0 tris' % (len(dup), dup_tris))
    for r in sorted(dup, key=lambda x: -x[2])[:10]:
        print('   A %s = B %s, %d tris, at %s' % r)


if __name__ == '__main__':
    main(*sys.argv[1:4])
