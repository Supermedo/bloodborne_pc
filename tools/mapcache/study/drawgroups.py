"""Draw-group visibility sets: for each Collision part (the floor the player stands on), the map pieces whose
draw groups (part +0x4C, u32[8]) intersect the collision's display groups (+0x6C). Group meaning is taken
from SoulsFormats (DrawGroups / DispGroups), checked here only by the numbers below.

    python -B drawgroups.py <dvdroot> <map>
"""
import sys

import study


def bits(g):
    v = 0
    for i, x in enumerate(g):
        v |= x << (32 * i)
    return v


sys.stdout.reconfigure(encoding='utf-8')
dv, mp = sys.argv[1], sys.argv[2]
m = study.Map(dv, mp)
pcs = [(bits(pc['part'].groups_a), sum(t[0] for _k, t in pc['meshes']), sum(study.eff(t, 2)[0] for _k, t in pc['meshes']), pc)
       for pc in m.pieces if not pc['sky']]
tot = sum(x[1] for x in pcs)
rows = []
for p in m.msb.parts:
    if p.type != 5:
        continue
    disp = bits(p.groups_b)
    if not disp:
        continue
    vis = [x for x in pcs if x[0] & disp]
    rows.append((sum(x[1] for x in vis), len(vis), sum(x[2] for x in vis), p.name))
rows.sort()
no_group = sum(1 for x in pcs if x[0] == 0)
print('%s: %d collisions with display groups; pieces %d (no draw group %d); map L0 tris %d' % (
    mp, len(rows), len(pcs), no_group, tot))
if rows:
    for q in (0, 0.1, 0.5, 0.9, 1.0):
        r = rows[int(q * (len(rows) - 1))]
        print('   q%.0f%%: %s sees %d pieces, %.2f M L0 tris (%.0f%% of map), %.2f M lowest' % (
            100 * q, r[3], r[1], r[0] / 1e6, 100.0 * r[0] / tot, r[2] / 1e6))
