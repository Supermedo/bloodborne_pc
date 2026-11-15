"""Explore face-set flags, strips, dynamic meshes, layouts and MTD names across all cached map folders."""
import collections
import glob
import json
import os
import sys

here = os.path.dirname(os.path.abspath(__file__))
flags = collections.Counter()
combos = collections.Counter()
strips = dyn = errors = 0
uvtypes = collections.Counter()
nuv = collections.Counter()
mtds = collections.Counter()
textypes = collections.Counter()
for p in sorted(glob.glob(os.path.join(here, 'cache', 'flstat_*.json'))):
    d = json.load(open(p, encoding='utf-8'))
    for stem, fl in d.items():
        if 'error' in fl:
            errors += 1
            print('ERR', stem, fl['error'])
            continue
        for m in fl['meshes']:
            dyn += m['dyn']
            fl_ = tuple(sorted(f[0] for f in m['fs']))
            combos['%s' % ' '.join('%x' % x for x in fl_)] += 1
            for f in m['fs']:
                flags['%08x' % f[0]] += 1
                strips += f[1]
            uvs = [t for (_o, t, s, _i) in m['layout'] if s == 5]
            nuv[len(uvs)] += 1
            for t in uvs:
                uvtypes['%x' % t] += 1
            mtds[m['mtd'].split('\\')[-1]] += 1
            for t in m['tex']:
                textypes[t[0]] += 1
print('errors', errors, 'dynamic meshes', dyn, 'strip sets', strips)
print('flags', flags.most_common())
print('combos', combos.most_common(20))
print('uv member types', uvtypes.most_common(), 'uv members per mesh', sorted(nuv.items()))
print('tex types', textypes.most_common(30))
print('mtd count', len(mtds))
for k, v in mtds.most_common(400):
    print(v, k)
