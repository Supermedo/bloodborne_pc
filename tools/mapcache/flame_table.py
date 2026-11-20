"""flame_table: every flame point of every map's map pieces, from the game files.

Candle flames are effects the engine spawns on map-piece model dummies (FLVER dummy points):
the Cathedral hall's candles are map piece m000086, dummy reference id 703 at (0, 0.675, -0.256)
in model space; all 67 candle lights the game drew there sit on those points (within 0.01 m).
The game draws their lights only within ~23 m; rt_flame_table.h lets the ray tracer light them
from any distance (vk_ray_tracing.cpp, CandleClones).

A flame point is lit only where stock lights it (the user, 2026-11-19: "only light where the lamp
on stock has the color on the lamp mesh, or real light on stock"):
- a map light (map/<id>/<id>_0000.btl.dcx, the BTL list of placed lights) within 1 m of that
  very point: its own colour, size and position, never copied to other copies of the model
  (copying the 2 lit Yharnam posts lit all 230; a plaza light lit lamps cyan);
- the candle light on CANDLE_MODELS, where the game was measured drawing it.
Every other flame point (lamp posts with only a glow, the Hunter's Dream gravestones) stays dark.

BTL light (200-byte rows after the name block): +16 name offset, +24 type (0 point), +28 on,
+29 diffuse RGB, +32 diffuse power, +36 specular RGB, +40 specular power, +56 position, +88
radius. The game's colour is (rgb / 255)^2 x power: (249, 194, 145) x 3.1 is the drawn
LegacyDPointLight3 (2.956, 1.794, 1.002), specular (0.572, 0.347, 0.194) at 0.6 (m24_01).

python flame_table.py <dvdroot_ps4> <out.h>
"""
import glob
import math
import os
import struct
import sys

import mapcache

# Dummy reference ids of the flame family (700-710). 703 = candle (measured live, m26).
FLAME_REFS = set(range(700, 711))
# Models whose flame effect carries the candle light (the game's own light, measured live).
CANDLE_MODELS = {'m000086'}
CANDLE = 0xFFFE  # FlameSeed.light: the candle light
# Lamp parts whose heads stock draws lit with no game light, checked one by one in the stock
# frame (harvest/glow.py; Cathedral Ward bridge double lamps: 255 of 255). Not the whole model:
# other m009703 copies (the posts at m009703_8300-8305) are dark in stock (user, 2026-11-19).
GLOW_PARTS = {('m24_01_00_00', 'm009703_8003'), ('m24_01_00_00', 'm009703_8004'),
              ('m24_01_00_00', 'm009703_8005')}
GLOW_PTS = []
GLOW_FIRST = 0  # FlameLights from here on are glowing lamps (rt: their halos)


def dummies(data):
    ndum = struct.unpack_from('<i', data, 0x14)[0]
    out = []
    for i in range(ndum):
        o = 0x80 + 0x40 * i
        pos = struct.unpack_from('<3f', data, o)
        ref = struct.unpack_from('<h', data, o + 0x1C)[0]
        out.append((ref, pos))
    return out


def read_btl(path):
    """Point lights of a BTL file: (colour, specular, radius, position)."""
    d = mapcache.dcx_read(path)
    count, names_len = struct.unpack_from('<II', d, 8)
    size, = struct.unpack_from('<I', d, 20)
    base = len(d) - count * size
    out = []
    for i in range(count):
        e = d[base + i * size:base + (i + 1) * size]
        if struct.unpack_from('<i', e, 24)[0] != 0 or not e[28]:
            continue
        power, = struct.unpack_from('<f', e, 32)
        spower, = struct.unpack_from('<f', e, 40)
        col = tuple(round((c / 255.0) ** 2 * power, 4) for c in e[29:32])
        spec = tuple(round((c / 255.0) ** 2 * spower, 4) for c in e[36:39])
        radius, = struct.unpack_from('<f', e, 88)
        if radius <= 0.0 or sum(col) <= 0.0:
            continue
        out.append((col, spec, round(radius, 3), struct.unpack_from('<3f', e, 56)))
    return out


EFFECT_LIGHTS = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'effect_lights.txt')


def collect(root, use_effects=True):
    maps, rows = [], []
    table = []
    for f in sorted(glob.glob(os.path.join(root, 'map', 'MapStudio', '*.msb.dcx'))):
        mid = os.path.basename(f)[:12]
        msb = mapcache.read_msb(mapcache.dcx_read(f))
        btl = os.path.join(root, 'map', mid, mid + '_0000.btl.dcx')
        lights = read_btl(btl) if os.path.exists(btl) else []
        cache = {}
        index = None
        start = len(rows)
        for p in msb.parts:
            if p.type != 0 or not p.model:
                continue
            if p.model not in cache:
                path = mapcache.model_path(root, mid, p.model)
                try:
                    cache[p.model] = [d for d in dummies(mapcache.dcx_read(path))
                                      if d[0] in FLAME_REFS] if os.path.exists(path) else []
                except Exception:
                    cache[p.model] = []
            if not cache[p.model]:
                continue
            if index is None:
                index = len(maps)
                maps.append(mid)
            m = mapcache.part_matrix(p.pos, p.rot, p.scale)
            for ref, d in cache[p.model]:
                w = [m[r][0] * d[0] + m[r][1] * d[1] + m[r][2] * d[2] + m[r][3] for r in range(3)]
                rows.append([index, ref, w, CANDLE if p.model in CANDLE_MODELS else 0xFFFF])
                if (mid, p.name) in GLOW_PARTS:
                    GLOW_PTS.append(tuple(round(x, 2) for x in w))
        # each map light goes to its nearest flame point of this map, within 1 m
        for col, spec, radius, pos in lights:
            near = [(math.dist(rows[i][2], pos), i) for i in range(start, len(rows))]
            near = [x for x in near if x[0] < 1.0]
            if near:
                rows[min(near)[1]][3] = len(table)
                table.append((col, spec, radius, pos))
    # the lamp lights the flame effects spawn, recorded in game (effect_lights.py)
    if os.path.exists(EFFECT_LIGHTS) and use_effects:
        cells = {}
        for i, r in enumerate(rows):
            cells.setdefault((int(r[2][0] // 4), int(r[2][2] // 4)), []).append(i)
        for line in open(EFFECT_LIGHTS):
            if line.startswith('#') or not line.strip():
                continue
            v = [float(x) for x in line.split()]
            pos, radius, col, spec = tuple(v[0:3]), v[3], tuple(v[4:7]), tuple(v[7:10])
            near = [(math.dist(rows[i][2], pos), i)
                    for dx in (-1, 0, 1) for dz in (-1, 0, 1)
                    for i in cells.get((int(pos[0] // 4) + dx, int(pos[2] // 4) + dz), [])]
            free = [x for x in near if x[0] < 0.6 and rows[x[1]][3] >= CANDLE]
            if free:
                rows[min(free)[1]][3] = len(table)
            else:
                # a lamp whose effect hangs on an object, not a map piece dummy: its own point,
                # in the map of the nearest flame point
                mi = min(near)[1] if near else min(range(len(rows)), key=lambda i: math.dist(rows[i][2], pos))
                rows.append([rows[mi][0], 703, list(pos), len(table)])
            table.append((col, spec, radius, pos))
    global GLOW_FIRST
    GLOW_FIRST = len(table)
    if use_effects:
        glow_lights(rows, table)
    return maps, rows, table


# Brightest stock pixel (0-255) around a flame point that counts as a lit lamp head (harvest/glow.py:
# glowing lamp heads read 200-255, dark Yharnam posts 9-20).
GLOW_MIN = 200
# No added lamp light within this of a light the game places (m).
GLOW_GAME_NEAR = 2.5
# A recorded glow sprite marks the flame point within this (m; measured 0.05).
GLOW_MATCH = 0.3
# The game's lamp light in m24_01 (BTL LegacyDPointLight3), for a ref with no recorded light.
LAMP = ((2.956, 1.794, 1.002), (0.572, 0.347, 0.194), 5.0)
# Flame dummies of lamp heads (703 street lamps, 705 bridge double lamps); a recorded glow on any
# other flame dummy is a candle group and gets the candle light.
LAMP_HEAD_REFS = {703, 705}


def glow_lights(rows, table):
    """A flame point the stock frame draws lit (harvest/glow_<map>.txt) but the game gives no light
    (the Cathedral Ward bridge lamps, m009703): the user's rule, 2026-11-19, "light where the lamp
    on stock has the color on the lamp mesh". Colour and radius: the median light the game spawns
    on that dummy ref elsewhere, else LAMP. One light per lamp head (points within 1 m share)."""
    glow = {k: True for k in GLOW_PTS}
    for path in glob.glob(os.path.join(os.path.dirname(EFFECT_LIGHTS), 'harvest', 'glow_*.txt')):
        if path.endswith('_here.txt'):
            continue
        if os.path.basename(path).startswith('glow_lamps_'):
            continue
        for line in open(path):
            v = line.split()
            if len(v) >= 5 and float(v[4]) >= GLOW_MIN:
                glow[tuple(round(float(x), 2) for x in v[:3])] = True
    # The game's glow sprites, recorded once in game (BB_RT_GLOW=1, BB_RT_GLOW_DUMP): each marks
    # the flame point it sits on (0.05 m) as a lamp stock draws lit.
    recorded = []
    for path in glob.glob(os.path.join(os.path.dirname(EFFECT_LIGHTS), 'harvest', 'glow_lamps_*.txt')):
        for line in open(path):
            v = line.split()
            if len(v) >= 3 and not line.startswith('#'):
                recorded.append(tuple(float(x) for x in v[:3]))
    if recorded:
        cells = {}
        for r in rows:
            cells.setdefault((int(r[2][0] // 2), int(r[2][2] // 2)), []).append(r)
        for q in recorded:
            near = [r for dx in (-1, 0, 1) for dz in (-1, 0, 1)
                    for r in cells.get((int(q[0] // 2) + dx, int(q[2] // 2) + dz), [])]
            near = [r for r in near if math.dist(r[2], q) < GLOW_MATCH]
            if near:
                r = min(near, key=lambda r: math.dist(r[2], q))
                glow[tuple(round(x, 2) for x in r[2])] = True
    if not glow:
        return
    by_ref = {}
    for r in rows:
        if r[3] < CANDLE:
            by_ref.setdefault(r[1], []).append(table[r[3]])
    def median_light(ref):
        ls = by_ref.get(ref)
        if not ls:
            return LAMP
        ls = sorted(ls, key=lambda l: sum(l[0]))
        col, spec, radius, _ = ls[len(ls) // 2]
        return col, spec, radius
    # Where the game already lights something (its candles on the dark-headed Cathedral Ward
    # posts, m009703_8300-8305, 1.3 m from the dummy) the lamp head is dark in stock: no light
    # (user, 2026-11-19: "another light too bright ... in stock theres no lamp").
    lit = [t[3] for t in table]
    added = candles = 0
    for r in rows:
        if r[3] != 0xFFFF or tuple(round(x, 2) for x in r[2]) not in glow:
            continue
        if any(math.dist(r[2], p) < GLOW_GAME_NEAR for p in lit):
            continue
        if r[1] not in LAMP_HEAD_REFS:
            # A glowing candle group, not a lamp (m33 m000201 at the shrine arches, 2026-11-19:
            # the lamp light flooded the cave orange where stock shows a few small candles).
            r[3] = CANDLE
            lit.append(r[2])
            candles += 1
            continue
        col, spec, radius = LAMP  # the ref's other lights include cool fill lights (came out blue)
        r[3] = len(table)
        table.append((col, spec, radius, tuple(r[2])))
        lit.append(r[2])
        added += 1
    print('%d glowing lamp heads lit, %d glowing candle groups (stock glow >= %d)'
          % (added, candles, GLOW_MIN))


def main(argv):
    root, out = argv[1], argv[2]
    maps, rows, table = collect(root)
    with open(out, 'w', newline='\n') as f:
        f.write('// SPDX-License-Identifier: GPL-2.0-or-later\n')
        f.write('// Generated by tools/mapcache/flame_table.py from the map files of Bloodborne 1.09;\n')
        f.write('// do not edit. Every flame dummy (reference id 700-710) of every placed map piece, in\n')
        f.write('// world space (the live game frame). vk_ray_tracing.cpp CandleClones lights them.\n')
        f.write('#pragma once\n\n#include <cstdint>\n\nnamespace Vulkan::Rt {\n\n')
        f.write('struct FlameSeed {\n    float pos[3];\n    uint16_t ref;\n    uint8_t map;\n'
                '    uint16_t light; // FlameLights index, 0xFFFE candle, 0xFFFF none\n};\n\n')
        f.write('inline constexpr uint16_t FlameCandle = 0x%X; // FlameSeed.light: the candle light\n'
                % CANDLE)
        f.write('// FlameLights from this index on are lamps stock draws glowing with no game light\n'
                '// (flame_table.py glow_lights): they get a halo.\n')
        f.write('inline constexpr uint16_t FlameGlowFirst = %d;\n\n' % GLOW_FIRST)
        f.write("// The map lights (BTL) at flame points, in the game's light terms.\n")
        f.write('struct FlameLight {\n    float color[3];\n    float spec[3];\n    float radius;\n'
                '    float pos[3];\n};\n\n')
        f.write('inline constexpr FlameLight FlameLights[] = {\n')
        for col, spec, radius, pos in table:
            f.write('    {{%.4ff, %.4ff, %.4ff}, {%.4ff, %.4ff, %.4ff}, %.3ff, {%.3ff, %.3ff, %.3ff}},\n'
                    % (col + spec + (radius,) + tuple(pos)))
        if not table:
            f.write('    {{0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, 0.0f, {0.0f, 0.0f, 0.0f}},\n')
        f.write('};\n\n')
        f.write('inline constexpr const char* FlameMaps[] = {\n')
        for m in maps:
            f.write('    "%s",\n' % m)
        f.write('};\n\ninline constexpr FlameSeed FlameSeeds[] = {\n')
        for idx, ref, w, light in rows:
            f.write('    {{%.3ff, %.3ff, %.3ff}, %d, %d, %d},\n' % (w[0], w[1], w[2], ref, idx, light))
        f.write('};\n\n} // namespace Vulkan::Rt\n')
    print('%d flame points in %d maps: %d with their own map light, %d candles -> %s' %
          (len(rows), len(maps), sum(r[3] < CANDLE for r in rows), sum(r[3] == CANDLE for r in rows), out))


if __name__ == '__main__':
    main(sys.argv)
