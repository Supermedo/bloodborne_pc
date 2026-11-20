"""effect_lights: the lamp lights the game spawns with its flame effects, recorded in game.

Most lamp lights are not map lights (BTL): the flame effect on a lamp's flame dummy spawns its
own point light, and only within ~23 m of the camera, so far lamps pop in. The effect files
(FXR v2) have no public reader, so the lights are recorded from the game instead (the user,
2026-11-19: "you can teleport for everywhere you need to hook the light point to create the
table"): BB_RT_LIGHT_HARVEST=<file> makes bb-probe write every point light it draws, averaged
per spot, and a teleport sweep over every flame point of a map spawns each lamp's effect.

This keeps the harvested lights that sit on a flame dummy of the map files (within MAX_DIST)
and are not BTL map lights: an effect light belongs to its lamp, so torches, enemies and moving
lights fall out. Output, one light per line (effect_lights.txt, read by flame_table.py):
    x y z radius r g b sr sg sb

python effect_lights.py <dvdroot_ps4> <out.txt> <harvest.txt>...
"""
import glob
import math
import os
import sys

import flame_table

MAX_DIST = 0.6     # light to flame dummy, metres
MIN_SEEN = 10      # frames of its brightest on-screen pass (drops one-frame sparks)
MIN_LUM = 0.05     # colour sum: the dim fill lights of the BTL are not lamp lights


def read_harvest(path):
    out = []
    for line in open(path):
        v = line.split()
        if len(v) < 13:
            continue
        x, y, z, r, cr, cg, cb, sr, sg, sb = map(float, v[:10])
        out.append(((x, y, z), r, (cr, cg, cb), (sr, sg, sb), int(v[10]), v[12]))
    return out


def main(argv):
    root, out = argv[1], argv[2]
    maps, rows, table = flame_table.collect(root, use_effects=False)
    seeds = [r[2] for r in rows]
    btl = [t[3] for t in table]
    # every BTL light, not only those on flame points: none of them is an effect light
    for f in glob.glob(os.path.join(root, 'map', 'm*', 'm*_0000.btl.dcx')):
        btl += [l[3] for l in flame_table.read_btl(f)]
    cells = {}
    for i, s in enumerate(seeds):
        cells.setdefault((int(s[0] // 4), int(s[2] // 4)), []).append(s)

    def near_seed(p):
        cx, cz = int(p[0] // 4), int(p[2] // 4)
        best = 1e9
        for dx in (-1, 0, 1):
            for dz in (-1, 0, 1):
                for s in cells.get((cx + dx, cz + dz), []):
                    best = min(best, math.dist(s, p))
        return best

    kept = {}
    for path in argv[3:]:
        movers = []  # enemies, NPCs and the player at each sweep stop: their torches and lanterns
        for ents in glob.glob(os.path.join(os.path.dirname(os.path.abspath(path)), 'ents_*.txt')):
            for line in open(ents):
                v = line.split()
                movers.append(tuple(float(x) for x in v[-3:]))
        mcell = {}
        for m in movers:
            mcell.setdefault((int(m[0] // 4), int(m[2] // 4)), []).append(m)

        def near_mover(p):
            cx, cz = int(p[0] // 4), int(p[2] // 4)
            return any(math.hypot(m[0] - p[0], m[2] - p[2]) < 2.0 and abs(m[1] - p[1]) < 3.5
                       for dx in (-1, 0, 1) for dz in (-1, 0, 1) for m in mcell.get((cx + dx, cz + dz), []))

        lights = read_harvest(path)
        hcell = {}
        for l in lights:
            hcell.setdefault((int(l[0][0] // 4), int(l[0][2] // 4)), []).append(l)

        def trail(p, r):
            # a moving light (an enemy's torch) leaves an entry every 0.25 m along its path:
            # 5+ entries of its size within 1.5 m; lamps and candle groups have at most 2
            cx, cz = int(p[0] // 4), int(p[2] // 4)
            n = sum(1 for dx in (-1, 0, 1) for dz in (-1, 0, 1) for q in hcell.get((cx + dx, cz + dz), [])
                    if 0 < math.dist(q[0], p) < 1.5 and abs(q[1] - r) < 0.2)
            return n >= 4

        for pos, r, col, spec, seen, shader in lights:
            if trail(pos, r):
                continue
            if seen < MIN_SEEN or sum(col) < MIN_LUM or 'Spot' in shader:
                continue
            if any(math.dist(pos, b) < 0.3 for b in btl):
                continue
            # on a flame dummy, or a still light away from every character (lamps whose effect
            # hangs on an object, like Central Yharnam's gate lamps)
            if near_seed(pos) > MAX_DIST and near_mover(pos):
                continue
            key = tuple(round(v * 4) for v in pos)
            if key not in kept or kept[key][4] < seen:
                kept[key] = (pos, r, col, spec, seen)
    with open(out, 'w', newline='\n') as f:
        f.write('# effect lights recorded in game (tools/mapcache/effect_lights.py): x y z radius r g b sr sg sb\n')
        for pos, r, col, spec, seen in sorted(kept.values()):
            f.write('%.3f %.3f %.3f %.3f %.4f %.4f %.4f %.4f %.4f %.4f\n' % (*pos, r, *col, *spec))
    print('%d effect lights -> %s' % (len(kept), out))


if __name__ == '__main__':
    main(sys.argv)
