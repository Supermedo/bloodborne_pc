"""Explore one MSB: part types, descriptions, model name prefixes, entity ids, connect-collision type data."""
import collections
import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
import mapcache as mc  # noqa: E402

dv, mp = sys.argv[1], sys.argv[2]
raw = mc.dcx_read(os.path.join(dv, 'map', 'mapstudio', mp + '.msb.dcx'))
msb = mc.read_msb(raw)
print('params', msb.params)
ty = collections.Counter(mc.PART_TYPES.get(p.type, p.type) for p in msb.parts)
print('part types', ty)
desc = collections.Counter((mc.PART_TYPES.get(p.type, p.type), p.desc) for p in msb.parts)
for (t, d), n in desc.most_common(60):
    print('  %-16s %5d %s' % (t, n, d))
pref = collections.Counter((p.type, (p.model or '?')[:4]) for p in msb.parts if p.type == 0)
print('map piece model prefixes', sorted(pref.items()))
ent = collections.Counter((mc.PART_TYPES.get(p.type, p.type), p.entity_id not in (-1, 0)) for p in msb.parts)
print('entity id set', sorted(ent.items(), key=str))
print('models', collections.Counter(mc.MODEL_TYPES.get(m['type'], m['type']) for m in msb.models))
# connect collision type data
for p in msb.parts:
    if p.type == 11:
        pass
