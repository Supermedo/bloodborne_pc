"""Map pieces whose description marks them cutscene-only / reflection-only / special, with their draw groups
(0x4C) and display groups (0x6C), against the map's other pieces.

    python -B opcut.py <dvdroot> <map>
"""
import collections
import re
import sys

import study

sys.stdout.reconfigure(encoding='utf-8')
dv, mp = sys.argv[1], sys.argv[2]
m = study.Map(dv, mp)
grp = collections.defaultdict(lambda: [0, 0, collections.Counter(), collections.Counter()])
for pc in m.pieces:
    p = pc['part']
    k = 'cutscene' if re.search(r'カットシーン', p.desc) else ('reflection-only' if re.search(r'映り込み専用', p.desc) else 'other')
    g = grp[k]
    g[0] += 1
    g[1] += sum(t[0] for _k, t in pc['meshes'])
    g[2][study.mc._hexgroups(p.groups_a)] += 1
    g[3][study.mc._hexgroups(p.groups_b)] += 1
for k, (n, t, ga, gb) in grp.items():
    print('%s: %d parts, %d L0 tris; draw groups 0x4C top %s; groups 0x6C top %s; distinct 0x4C %d' % (
        k, n, t, ga.most_common(3), gb.most_common(2), len(ga)))
