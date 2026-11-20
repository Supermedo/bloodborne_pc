#!/usr/bin/env python3
"""Read-only survey of sky / moon / sun geometry in every Bloodborne MSB.

    python sky_survey.py <dvdroot_ps4> <out.md>

Sky parts = MapPiece / Object / DummyObject parts whose model has a mesh with
'sky', 'moon' or 'sun' in its MTD path or material name. For each one: part fields,
EMEVD references of its entity id, and per mesh the face sets, world AABB, distance and
angular size seen from the map centre (median of non-sky map-piece mesh centres).
"""
import glob
import math
import os
import re
import struct
import sys
import time

import mapcache as mc

SKY_RE = re.compile(r'sky|moon|sun', re.I)
DISC_RE = re.compile(r'moon|sun', re.I)
_flver_cache = {}


def load_model(dvdroot, folder, model):
    """FLVER of a map-piece (m...) or object (o...) model, or None."""
    if not model:
        return None
    if model.startswith('m'):
        path = mc.model_path(dvdroot, folder, model)
        off = 0
    elif model.startswith('o'):
        path = os.path.join(dvdroot, 'obj', model + '.objbnd.dcx')
    else:
        return None
    if path in _flver_cache:
        return _flver_cache[path]
    fl = None
    if os.path.exists(path):
        data = mc.dcx_read(path)
        off = data.find(b'FLVER\0')   # objbnd = BND4 holding one FLVER (+ textures)
        if off >= 0:
            fl = mc.read_flver(data[off:], os.path.basename(path))
    _flver_cache[path] = fl
    return fl


def corner_aabb(M, bb):
    pts = [(bb[0] if i & 1 else bb[3], bb[1] if i & 2 else bb[4], bb[2] if i & 4 else bb[5]) for i in range(8)]
    w = [[M[r][0] * p[0] + M[r][1] * p[1] + M[r][2] * p[2] + M[r][3] for r in range(3)] for p in pts]
    return [min(v[c] for v in w) for c in range(3)], [max(v[c] for v in w) for c in range(3)]


def median(v):
    v = sorted(v)
    return v[len(v) // 2] if v else 0.0


def mesh_metrics(M, m, centre):
    wx, wy, wz = mc._apply(M, *m.positions)
    mn = (min(wx), min(wy), min(wz))
    mx = (max(wx), max(wy), max(wz))
    c = tuple((mn[i] + mx[i]) / 2 for i in range(3))
    d = math.dist(c, centre)
    inside = all(mn[i] <= centre[i] <= mx[i] for i in range(3))
    if inside or d < 1e-3:
        ang = None
    else:
        u = tuple((c[i] - centre[i]) / d for i in range(3))
        cosmin = 1.0
        for x, y, z in zip(wx, wy, wz):
            vx, vy, vz = x - centre[0], y - centre[1], z - centre[2]
            n = math.sqrt(vx * vx + vy * vy + vz * vz) or 1.0
            cosmin = min(cosmin, (vx * u[0] + vy * u[1] + vz * u[2]) / n)
        ang = 2 * math.degrees(math.acos(max(-1.0, min(1.0, cosmin))))
    # thickness along the view direction relative to the radius (flat disc -> small)
    flat = None
    if ang is not None:
        u = tuple((c[i] - centre[i]) / d for i in range(3))
        along = [(x - c[0]) * u[0] + (y - c[1]) * u[1] + (z - c[2]) * u[2] for x, y, z in zip(wx, wy, wz)]
        rad = max(math.dist((x, y, z), c) for x, y, z in zip(wx, wy, wz)) or 1.0
        flat = (max(along) - min(along)) / (2 * rad)
    return mn, mx, c, d, ang, flat


def classify(m, d, ang, flat, mn, mx):
    """dome: surrounds the centre and is tall; cloud band: a wide ring/strip of sky;
    moon/sun: material named moon/sun, a Moon/Sun MTD that is not a Sky_ MTD, or a small far object."""
    horiz = max(mx[0] - mn[0], mx[2] - mn[2])
    named = DISC_RE.search(m.material) or (DISC_RE.search(m.mtd) and not re.search(r'sky_', m.mtd, re.I))
    if named or (ang is not None and ang < 25 and d > 150):
        return 'moon/sun disc' if (flat is not None and flat < 0.6) else 'moon/sun (sphere/not flat)'
    if ang is None and horiz > 1500:
        return 'dome' if (mx[1] - mn[1]) > 0.25 * horiz else 'cloud band (ring)'
    if SKY_RE.search(m.mtd) and ang is not None and ang > 60:
        return 'cloud band'
    if SKY_RE.search(m.mtd):
        return 'sky (other)'
    return 'non-sky mesh in sky model'

# ---------------------------------------------------------------- EMEVD (BB: 'EVD\0' v0xCC, 64-bit)


def emevd_refs(path, ids):
    """{entity id: [(event id, 'bank[id]', state, gate flags)]} for instructions whose args hold the id."""
    out = {}
    if not os.path.exists(path) or not ids:
        return out
    b = mc.dcx_read(path)
    if b[:4] != b'EVD\0':
        return out
    h = struct.unpack_from('<16q', b, 0x10)
    ev_cnt, ev_off, in_off, args_off = h[0], h[1], h[3], h[13]
    for e in range(ev_cnt):
        eid, icnt, ioff = struct.unpack_from('<qqq', b, ev_off + e * 0x30)
        gates = []
        for k in range(icnt):
            bank, iid, alen, aoff = struct.unpack_from('<iiqq', b, in_off + ioff + k * 0x20)
            a = b[args_off + aoff: args_off + aoff + alen]
            if bank == 1003 and iid == 101 and len(a) >= 8:   # goto label if event flag
                gates.append('label%d<-flag%d' % (a[0], struct.unpack_from('<i', a, 4)[0]))
            if len(a) >= 8:
                v, st = struct.unpack_from('<ii', a, 0)
                if v in ids:
                    out.setdefault(v, []).append((eid, '%d[%02d]' % (bank, iid), st, ' '.join(gates)))
    return out

# ---------------------------------------------------------------- survey


def survey_msb(dvdroot, msb_path, rows, live):
    map_id = os.path.basename(msb_path).split('.')[0]
    folder = map_id[:6] + '_00_00'
    msb = mc.read_msb(mc.dcx_read(msb_path))
    sky, centres = [], []
    for p in msb.parts:
        if p.type not in (0, 1, 9):
            continue
        fl = load_model(dvdroot, folder, p.model)
        if fl is None:
            continue
        is_sky = any(SKY_RE.search(m.mtd + ' ' + m.material) for m in fl.meshes)
        if is_sky:
            sky.append((p, fl))
        elif p.type == 0:
            M = mc.part_matrix(p.pos, p.rot, p.scale)
            for m in fl.meshes:
                if m.bbox and not m.error:
                    mn, mx = corner_aabb(M, m.bbox)
                    centres.append([(mn[i] + mx[i]) / 2 for i in range(3)])
    centre = tuple(median([c[i] for c in centres]) for i in range(3))
    evp = os.path.join(dvdroot, 'event', map_id + '.emevd.dcx')
    if not os.path.exists(evp):   # variants (_01, _11) have no own script; the base map's is used
        evp = os.path.join(dvdroot, 'event', folder + '.emevd.dcx')
    ev = emevd_refs(evp, {p.entity_id for p, _ in sky if p.entity_id not in (-1, 0)})
    rows.append('\n### %s (models: map/%s)\n' % (map_id, folder))
    rows.append('Map centre (median of %d non-sky map-piece mesh centres): (%.0f, %.0f, %.0f)\n'
                % (len(centres), *centre))
    if not sky:
        rows.append('No sky parts.\n')
        return
    for p, fl in sky:
        refs = ev.get(p.entity_id, [])
        rows.append('\n**%s** model %s, %s, pos (%.1f, %.1f, %.1f) rot (%.1f, %.1f, %.1f), entity %d, desc "%s"  \n'
                    'groups@0x4C %s, groups@0x6C %s  \n' % (
                        p.name, p.model, mc.PART_TYPES.get(p.type, p.type), *p.pos, *p.rot, p.entity_id, p.desc,
                        mc._hexgroups(p.groups_a), mc._hexgroups(p.groups_b)))
        if refs:
            rows.append('EMEVD: ' + '; '.join('event %d %s state %d (%s)' % r for r in refs) + '  \n')
        rows.append('\n| mesh | tris L0 | face sets | material | MTD | world AABB min | max | dist | ang. size | class |\n'
                    '|---|---|---|---|---|---|---|---|---|---|\n')
        M = mc.part_matrix(p.pos, p.rot, p.scale)
        for m in fl.meshes:
            if m.error:
                rows.append('| %d | ERROR %s |\n' % (m.index, m.error))
                continue
            mn, mx, c, d, ang, flat = mesh_metrics(M, m, centre)
            cls = classify(m, d, ang, flat, mn, mx)
            rows.append('| %d | %d | %s | %s | %s | (%.0f, %.0f, %.0f) | (%.0f, %.0f, %.0f) | %.0f | %s | %s |\n' % (
                m.index, len(m.tris) // 3, ' '.join(f.label() for f in m.facesets), m.material,
                m.mtd.split('\\')[-1], *mn, *mx, d, 'surrounds' if ang is None else '%.1f deg' % ang, cls))
            live.append((map_id, p, m, c))


def survey_m29(dvdroot, rows):
    """Chalice dungeons: thousands of layout MSBs in mapstudio/m29_*/; only list which sky models they use."""
    rows.append('\n## Chalice dungeon MSBs (map/mapstudio/m29_*/)\n\n| folder | MSBs | MSBs with sky parts | sky models (mesh MTDs) |\n|---|---|---|---|\n')
    for d in sorted(glob.glob(os.path.join(dvdroot, 'map', 'mapstudio', 'm29_*'))):
        files = sorted(glob.glob(os.path.join(d, '*.msb.dcx')))
        folder = os.path.basename(d)
        n_sky, models = 0, {}
        for f in files:
            msb = mc.read_msb(mc.dcx_read(f))
            found = False
            for p in msb.parts:
                if p.type not in (0, 1, 9):
                    continue
                if p.model not in models:
                    fl = load_model(dvdroot, 'm29_00_00_00', p.model)   # all chalice models live in map/m29_00_00_00
                    mt = sorted({m.mtd.split('\\')[-1] for m in fl.meshes if SKY_RE.search(m.mtd + ' ' + m.material)}) if fl else []
                    models[p.model] = mt
                if models[p.model]:
                    found = True
            n_sky += found
        used = {k: v for k, v in models.items() if v}
        rows.append('| %s | %d | %d | %s |\n' % (folder, len(files), n_sky,
                                                 '; '.join('%s: %s' % (k, ', '.join(v)) for k, v in sorted(used.items())) or '-'))


def main(argv):
    dvdroot, out = argv[0], argv[1]
    t0 = time.time()
    rows, live = [], []
    for f in sorted(glob.glob(os.path.join(dvdroot, 'map', 'mapstudio', '*.msb.dcx'))):
        survey_msb(dvdroot, f, rows, live)
        print('%s done %.0fs' % (os.path.basename(f), time.time() - t0), flush=True)
    if '--no-m29' not in argv:
        survey_m29(dvdroot, rows)
    # live-fact matching
    bridge = (-216.0, -30.0, 64.0)
    rows.append('\n## Live-fact matches\n\n')
    band = sorted([264, 480, 576, 576, 576, 672])
    for map_id, p, m, c in live:
        n = [f.index_count for f in m.facesets]
        if 360 in n or 96 in n or 11904 in n:
            rows.append('- %s %s mesh %d (%s, %s): face sets %s, centre (%.1f, %.1f, %.1f), %.0f m from bridge (-216,-30,64)\n'
                        % (map_id, p.name, m.index, m.material, m.mtd.split('\\')[-1], n, *c, math.dist(c, bridge)))
    by_part = {}
    for map_id, p, m, c in live:
        by_part.setdefault((map_id, p.name), []).append(m.facesets[0].index_count if m.facesets else 0)
    for k, v in sorted(by_part.items()):
        if all(band.count(x) <= v.count(x) for x in set(band)):
            rows.append('- cloud band 264/480/576x3/672 found in %s %s (LOD-0 index counts %s)\n' % (k[0], k[1], v))
    with open(out, 'w', encoding='utf-8', newline='\n') as f:
        f.write(''.join(rows))
    print('wrote', out, '%.0fs' % (time.time() - t0))


if __name__ == '__main__':
    main(sys.argv[1:])
