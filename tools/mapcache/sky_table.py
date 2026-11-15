#!/usr/bin/env python3
"""The moon or sun of every sky in the game, for the ray tracer's light direction.

    python sky_table.py <dvdroot_ps4> list            every sky part and its meshes (to read)
    python sky_table.py <dvdroot_ps4> check           which skies share draws, and how they differ
    python sky_table.py <dvdroot_ps4> header <out.h>  the table the engine compiles in
                                                       (gpu/.../rt/rt_sky_table.h)

A sky part is a map piece or object whose model has a sky, moon or sun material (the same rule
as sky_survey.py). The engine sees a sky's cloud draws with their model-space vertices; many
skies share index counts (the night sky's 672/576/576/480/216 is in six maps), so a draw is
known by its index count and the positions of four of its vertices, the ones the engine's probe
reads (rt_blas_cache.h ProbeSlots: the first vertex of triangle n*k/4, k = 0..3). The table
gives, for each sky of each map, those samples for every mesh at every detail level, and the
world centre of the moon or sun that goes with it.
"""
import glob
import math
import os
import sys

import mapcache as mc
import sky_survey as ss

# A sky whose moon is a part of its own (sky_survey.py: the map's event script enables the two
# together): (map, sky part) -> the moon's part.
PAIRS = {
    ('m21_00_00_00', 'o210030'): 'o210020',       # Hunter's Dream
    ('m21_00_00_00', 'o210031'): 'o210021',       # Hunter's Dream, last boss
    ('m28_00_00_00', 'm009000_0000'): 'o280021',  # Yahar'gul under the blood moon
}

CHALICE_SKIES = ('m006220', 'm206220', 'm216220', 'm406220', 'm416220')


class ChalicePart:
    def __init__(self, model):
        self.name = self.model = model
        self.pos, self.rot, self.scale = (0.0, 0.0, 0.0), (0.0, 0.0, 0.0), (1.0, 1.0, 1.0)
        self.type, self.entity_id = 0, -1


_flvers = {}


def load_model(dvdroot, folder, model):
    """FLVER of a map-piece or object model with the index lists of every detail level."""
    if model.startswith('m'):
        path = mc.model_path(dvdroot, folder, model)
    elif model.startswith('o'):
        path = os.path.join(dvdroot, 'obj', model + '.objbnd.dcx')
    else:
        return None
    if path not in _flvers:
        fl = None
        if os.path.exists(path):
            data = mc.dcx_read(path)
            off = data.find(b'FLVER\0')
            if off >= 0:
                fl = mc.read_flver(data[off:], os.path.basename(path), all_indices=True)
        _flvers[path] = fl
    return _flvers[path]


def is_disc(m):
    """A moon or sun mesh by name: its material, or a Moon/Sun MTD."""
    mtd = m.mtd.split('\\')[-1].lower()
    # M_Sky_RedMoon_/Evening_/Night_/daytime_ are cloud materials named after their sky.
    cloud = any(t in mtd for t in ('sky_redmoon', 'sky_evening', 'sky_night', 'sky_daytime'))
    return bool(ss.DISC_RE.search(m.material) or (ss.DISC_RE.search(mtd) and not cloud))


def mesh_centre(M, m):
    wx, wy, wz = mc._apply(M, *m.positions)
    return tuple((min(v) + max(v)) / 2 for v in (wx, wy, wz)), \
        max(max(v) - min(v) for v in (wx, wy, wz)) / 2


def samples(m, fs):
    """Model-space positions of the four probe samples of one detail level, or None."""
    ia = fs.indices
    if ia is None or fs.strip or len(ia) < 3:
        return None
    prims = len(ia) // 3
    xs, ys, zs = m.raw   # as the game's vertex buffer holds them (an object's node is not applied)
    out = []
    for k in range(4):
        v = ia[(prims * k // 4) * 3]
        if v >= len(xs):
            return None
        out.append((xs[v], ys[v], zs[v]))
    return out


class Sky:
    """One sky part: its draws (index count + samples) and its own moon or sun, if it has one."""

    def __init__(self, map_id, part, fl):
        self.map_id, self.part, self.name, self.model = map_id, part, part.name, part.model
        # (index count, [4 x (x, y, z)], mesh index, detail level, model-to-world matrix)
        self.draws = []
        self.disc = None  # (world centre, radius)
        self.meshes = []
        part_matrix = mc.part_matrix(part.pos, part.rot, part.scale)
        for m in fl.meshes:
            if m.error or not m.facesets:
                continue
            # Model space to world: the part's placement times the mesh's node (an object's
            # meshes hang on nodes of their own; a map piece's node is identity).
            world = mc._mat_mul(part_matrix, m.node) if m.node is not None else part_matrix
            c, r = mesh_centre(part_matrix, m)
            disc = is_disc(m)
            self.meshes.append((m, c, r, disc))
            if disc:
                self.disc = (c, r)
            for lod, fs in enumerate(m.facesets):
                s = samples(m, fs)
                if s:
                    self.draws.append((fs.index_count, s, m.index, lod, world))

    def only_disc(self):
        return all(disc for _, _, _, disc in self.meshes)

    def keys(self):
        return {(n, tuple(round(c, 2) for p in s for c in p)) for n, s, _, _, _ in self.draws}


def collect(dvdroot):
    """The sky parts of the base maps (variants _01 and _11 hold the same ones)."""
    skies = []
    for f in sorted(glob.glob(os.path.join(dvdroot, 'map', 'mapstudio', '*.msb.dcx'))):
        map_id = os.path.basename(f).split('.')[0]
        if not map_id.endswith('_00_00'):
            continue
        msb = mc.read_msb(mc.dcx_read(f))
        for p in msb.parts:
            if p.type not in (0, 1, 9):
                continue
            fl = load_model(dvdroot, map_id, p.model)
            if fl is None:
                continue
            if any(ss.SKY_RE.search(m.mtd + ' ' + m.material) for m in fl.meshes):
                skies.append(Sky(map_id, p, fl))
    # The chalice dungeons' skies: thousands of layout MSBs place a few models of
    # map/m29_00_00_00 (sky_survey.py), none with a moon or sun. Listed at the origin (their
    # place differs per layout and is never needed: no other sky shares their meshes), so the
    # engine knows them as skies without a moon.
    for model in CHALICE_SKIES:
        fl = load_model(dvdroot, 'm29_00_00_00', model)
        if fl is not None:
            skies.append(Sky('m29_00_00_00', ChalicePart(model), fl))
    return skies


def f32(v):
    t = '%.9g' % v
    return t + ('' if ('.' in t or 'e' in t) else '.0') + 'f'


def write_header(skies, out):
    by_name = {(s.map_id, s.name): s for s in skies}
    moons_of_others = {(k[0], v) for k, v in PAIRS.items()}
    rows, table, places = [], [], []
    for s in skies:
        # The moon part of another sky is that sky's moon, not a sky of its own.
        if not s.draws or (s.map_id, s.name) in moons_of_others:
            continue
        moon = s.disc[0] if s.disc else None
        pair = PAIRS.get((s.map_id, s.name))
        if pair:
            moon = by_name[(s.map_id, pair)].disc[0]
        sky = len(table)
        table.append((s, moon))
        for n, smp, mesh, lod, world in s.draws:
            place = tuple(f32(world[r][c]) for r in range(3) for c in range(4))
            if place not in places:
                places.append(place)
            rows.append((n, sky, mesh, places.index(place), smp))
    rows.sort(key=lambda r: (r[0], r[1], r[2]))
    with open(out, 'w', encoding='utf-8', newline='\n') as f:
        f.write('// SPDX-License-Identifier: GPL-2.0-or-later\n'
                '// Generated by tools/mapcache/sky_table.py from the map files of Bloodborne 1.09;\n'
                '// do not edit. Each sky part of each map with the world centre of its moon or sun\n'
                '// (docs/GEOMETRY_CACHE_PLAN.md step 3), and for every mesh and detail level its index\n'
                '// count, the positions of the four vertices the probe samples (rt_blas_cache.h\n'
                '// ProbeSlots) as the vertex buffer holds them, and the matrix that places them in\n'
                '// the world. rt_sky_finder.h matches draws to it.\n'
                '#pragma once\n\n#include <cstdint>\n\nnamespace Vulkan::Rt {\n\n'
                'struct SkyTableSky {\n    const char* name;\n    float moon[3];\n    bool has_moon;\n};\n\n'
                '// 3x4 row-major, model space to world.\n'
                'struct SkyTablePlace {\n    float matrix[12];\n};\n\n'
                'struct SkyTableDraw {\n    uint32_t indices;\n    uint16_t sky, mesh, place;\n'
                '    float samples[12];\n};\n\n')
        f.write('inline constexpr SkyTableSky SkyTableSkies[] = {\n')
        for s, moon in table:
            mn = ', '.join(f32(v) for v in (moon or (0.0, 0.0, 0.0)))
            f.write('    {"%s %s", {%s}, %s},\n' % (s.map_id[:6], s.name, mn, 'true' if moon else 'false'))
        f.write('};\n\ninline constexpr SkyTablePlace SkyTablePlaces[] = {\n')
        for place in places:
            f.write('    {{%s}},\n' % ', '.join(place))
        f.write('};\n\n// Sorted by index count.\ninline constexpr SkyTableDraw SkyTableDraws[] = {\n')
        for n, sky, mesh, place, smp in rows:
            f.write('    {%d, %d, %d, %d, {%s}},\n' % (n, sky, mesh, place,
                                                      ', '.join(f32(c) for p in smp for c in p)))
        f.write('};\n\n} // namespace Vulkan::Rt\n')
    print('wrote %s: %d skies (%d with a moon or sun), %d draws' % (
        out, len(table), sum(1 for _, m in table if m), len(rows)))


def main(argv):
    dvdroot, mode = argv[0], argv[1]
    sys.stdout.reconfigure(encoding='utf-8')
    skies = collect(dvdroot)
    if mode == 'list':
        for s in skies:
            p = s.part
            print('%s %s model %s type %s entity %d pos (%.1f %.1f %.1f) rot (%.1f %.1f %.1f)' % (
                s.map_id, p.name, p.model, mc.PART_TYPES.get(p.type, p.type), p.entity_id, *p.pos, *p.rot))
            for m, c, r, disc in s.meshes:
                print('    mesh %d %-5s verts %-6d idx %-22s centre (%8.1f %8.1f %8.1f) radius %7.1f  %s | %s' % (
                    m.index, 'DISC' if disc else '', m.vertex_count, '/'.join(str(f.index_count) for f in m.facesets),
                    *c, r, m.material, m.mtd.split('\\')[-1]))
        return
    if mode == 'check':
        for s in skies:
            print('%s %s: %d draws, disc %s' % (s.map_id, s.name, len(s.draws),
                                                '(%.1f %.1f %.1f)' % s.disc[0] if s.disc else 'none'))
            for n, smp, mesh, lod, _ in s.draws:
                if lod == 0:
                    print('    mesh %d %5d idx  %s' % (mesh, n, ' '.join('(%.3f %.3f %.3f)' % p for p in smp)))
        print('\nSkies that share draws (index count and all four samples equal):')
        for i, a in enumerate(skies):
            for b in skies[i + 1:]:
                ka, kb = a.keys(), b.keys()
                both = ka & kb
                if not both:
                    continue
                apart = math.dist(a.disc[0], b.disc[0]) if a.disc and b.disc else float('nan')
                print('  %s %s / %s %s: %d shared, %d only in the first, %d only in the second; discs %.0f m apart' % (
                    a.map_id, a.name, b.map_id, b.name, len(both), len(ka - both), len(kb - both), apart))
        return
    if mode == 'header':
        write_header(skies, argv[2])
        return
    raise SystemExit('unknown mode ' + mode)


if __name__ == '__main__':
    main(sys.argv[1:])
