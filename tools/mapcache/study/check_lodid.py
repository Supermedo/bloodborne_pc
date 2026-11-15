"""Cross-check: entity-data byte +0x0D of map pieces vs. description tags 【LODなし】/【LODあり】 and face-set counts."""
import collections
import sys

import study

sys.stdout.reconfigure(encoding='utf-8')
dv = sys.argv[1]
for mp in sys.argv[2:]:
    m = study.Map(dv, mp)
    c = collections.Counter()
    for pc in m.pieces:
        p = pc['part']
        tag = 'LODなし' if 'LODなし' in p.desc else ('LODあり' if 'LODあり' in p.desc else '-')
        nfs = max(1 + (t[1] is not None) + (t[2] is not None) for _k, t in pc['meshes']) if pc['meshes'] else 0
        c[(m.ent_raw[p.index][0x0D], tag, nfs)] += 1
    print(mp)
    for k, v in sorted(c.items()):
        print('   +0x0D=%3d desc %-6s max face sets %d : %d parts' % (k[0], k[1], k[2], v))
