"""Map-piece model families (first 3 digits of the model number): parts, models, placed L0 / lowest tris,
share of parts at position (0,0,0) (geometry baked in world space), parts whose description says 遠景 (distant view).

    python -B families.py <dvdroot> <map>...
"""
import collections
import sys

import study

sys.stdout.reconfigure(encoding='utf-8')
dv = sys.argv[1]
for mp in sys.argv[2:]:
    m = study.Map(dv, mp)
    fam = collections.defaultdict(lambda: [0, set(), 0, 0, 0, 0, []])
    for pc in m.pieces:
        p = pc['part']
        f = fam[p.model[:4]]
        f[0] += 1
        f[1].add(p.model)
        f[2] += sum(t[0] for _k, t in pc['meshes'])
        f[3] += sum(study.eff(t, 2)[0] for _k, t in pc['meshes'])
        f[4] += all(abs(v) < 1e-3 for v in p.pos)
        f[5] += '遠景' in p.desc
        f[6].append(max(pc['max'][c] - pc['min'][c] for c in (0, 2)))
    print('\n## %s families: prefix | parts | models | placed L0 M | placed lowest M | parts at origin | 遠景 parts | median XZ extent m' % mp)
    for k, f in sorted(fam.items()):
        ext = sorted(f[6])
        print('%s | %d | %d | %.2f | %.2f | %d | %d | %.0f' % (k, f[0], len(f[1]), f[2] / 1e6, f[3] / 1e6, f[4], f[5], ext[len(ext) // 2]))
