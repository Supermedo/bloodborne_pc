"""Which EMEVD instructions name entity ids of map pieces / objects / collisions of a map."""
import collections
import os
import struct
import sys

here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, here)
sys.path.insert(0, os.path.join(here, '..'))
import mapcache as mc  # noqa: E402
import emevd  # noqa: E402

dv, mp = sys.argv[1], sys.argv[2]
msb = mc.read_msb(mc.dcx_read(os.path.join(dv, 'map', 'mapstudio', mp + '.msb.dcx')))
ids = {}
for p in msb.parts:
    if p.entity_id not in (-1, 0):
        ids[p.entity_id] = p
ev = emevd.read_emevd(mc.dcx_read(os.path.join(dv, 'event', mp + '.emevd.dcx')))
common = emevd.read_emevd(mc.dcx_read(os.path.join(dv, 'event', 'common.emevd.dcx')))
print('events', len(ev), 'common', len(common))
hits = collections.defaultdict(set)
for eid, caller, ins in emevd.instances(ev, [common]):
    for bank, iid, a in ins:
        if bank == 2000:
            continue
        for k in range(0, len(a) - 3, 4):
            v, = struct.unpack_from('<i', a, k)
            if v in ids:
                p = ids[v]
                hits[(mc.PART_TYPES.get(p.type, p.type), '%d[%02d]' % (bank, iid), k)].add(v)
for k, v in sorted(hits.items()):
    print(k, len(v), sorted(v)[:12])
