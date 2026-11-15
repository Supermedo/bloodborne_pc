"""Draw groups: is "a map piece is drawn when its DrawGroups share a bit with the DispGroups of
the collision the player stands on" consistent with what the game draws on the bridge?
usage: groups_check.py <dvdroot_ps4> <map id> near_part,... far_part,..."""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
import mapcache  # noqa: E402

dv, map_id = sys.argv[1], sys.argv[2]
near = sys.argv[3].split(',')
far = sys.argv[4].split(',')
msb = mapcache.read_msb(mapcache.dcx_read(os.path.join(dv, 'map', 'mapstudio', map_id + '.msb.dcx')))


def bits(g):
    v = 0
    for i, x in enumerate(g):
        v |= x << (32 * i)
    return v


def show(v):
    return ','.join(str(i) for i in range(256) if v >> i & 1) or '-'


parts = {p.name: p for p in msb.parts}
for name in near + far:
    p = parts.get(name)
    if p is None:
        print(name, 'not found')
        continue
    print(f'{name:16s} type {p.type} draw[{show(bits(p.groups_a))}] disp[{show(bits(p.groups_b))}] back[{show(bits(p.groups_c))}]')
cols = [p for p in msb.parts if p.type == 5]
pieces = [p for p in msb.parts if p.type == 0]
print(len(cols), 'collisions,', len(pieces), 'map pieces')
for which, key in (('pieces.draw vs collision.disp', 'groups_b'), ('pieces.draw vs collision.draw', 'groups_a')):
    print('--', which)
    for c in cols:
        cg = bits(getattr(c, key))
        if not cg:
            continue
        ok_near = all(bits(parts[n].groups_a) & cg for n in near if n in parts)
        if not ok_near:
            continue
        hit_far = [n for n in far if n in parts and bits(parts[n].groups_a) & cg]
        vis = sum(1 for p in pieces if bits(p.groups_a) & cg)
        print(f'   {c.name:14s} at ({c.pos[0]:.0f} {c.pos[1]:.0f} {c.pos[2]:.0f}) groups[{show(cg)}] shows {vis} pieces; far ones shown: {hit_far or "none"}')
