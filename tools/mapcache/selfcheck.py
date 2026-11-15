#!/usr/bin/env python3
"""Self-check for mapcache on one map: parse all FLVERs in the map folder, compare against
live-game triangle counts near a reference point, test the MSB rotation convention by
counting shared world-space vertices between neighbouring parts, and ray-cast the ground
under the reference point.

    python selfcheck.py <dvdroot_ps4> <map id> [x y z] [count ...]
"""
import glob
import math
import os
import sys
import time

import mapcache as mc

REF = (-216.0, -30.0, 64.0)
LIVE_COUNTS = [18898, 14100, 6658, 8582, 6831, 12472, 20979, 6233, 9084, 6003, 15720, 24614, 23836]


def all_flvers(dvdroot, map_id):
    t0 = time.time()
    ok, bad, tris = 0, [], 0
    for p in sorted(glob.glob(os.path.join(dvdroot, 'map', map_id, '*.flver.dcx'))):
        try:
            fl = mc.read_flver(mc.dcx_read(p), os.path.basename(p), want_positions=False)
        except Exception as e:  # report every failure with its file
            bad.append('%s: %s' % (os.path.basename(p), e))
            continue
        errs = [m.error for m in fl.meshes if m.error]
        if errs:
            bad.extend(errs)
        else:
            ok += 1
        tris += sum(len(m.tris) // 3 for m in fl.meshes if not m.error)
    print('[flver] %d files parse cleanly, %d failures, %d LOD-0 triangles (unique models), %.1fs'
          % (ok, len(bad), tris, time.time() - t0))
    for b in bad:
        print('  FAIL', b)


def aabb_dist(mn, mx, p):
    d = [max(mn[c] - p[c], 0, p[c] - mx[c]) for c in range(3)]
    return math.sqrt(sum(x * x for x in d))


def world_mesh(M, m):
    return mc._apply(M, *m.positions)


def ray_down(pieces, flvers, x, z, M_of):
    """Hits of a vertical ray at (x, z) against all placed LOD-0 triangles: list of (y, part, model, mesh)."""
    hits = []
    for p in pieces:
        fl = flvers.get(p.model)
        if not fl:
            continue
        M = M_of(p)
        for m in fl.meshes:
            if m.error:
                continue
            # cheap reject with the transformed model bbox corners
            bb = m.bbox
            cs = [(bb[0] if i & 1 else bb[3], bb[1] if i & 2 else bb[4], bb[2] if i & 4 else bb[5]) for i in range(8)]
            wc = [[M[r][0] * c[0] + M[r][1] * c[1] + M[r][2] * c[2] + M[r][3] for r in range(3)] for c in cs]
            if not (min(w[0] for w in wc) <= x <= max(w[0] for w in wc) and min(w[2] for w in wc) <= z <= max(w[2] for w in wc)):
                continue
            wx, wy, wz = world_mesh(M, m)
            t = m.tris
            for i in range(0, len(t), 3):
                a, b, c = t[i], t[i + 1], t[i + 2]
                ax, az, bx, bz, cx, cz = wx[a], wz[a], wx[b], wz[b], wx[c], wz[c]
                if x < min(ax, bx, cx) or x > max(ax, bx, cx) or z < min(az, bz, cz) or z > max(az, bz, cz):
                    continue
                d = (bz - cz) * (ax - cx) + (cx - bx) * (az - cz)
                if d == 0:
                    continue
                l1 = ((bz - cz) * (x - cx) + (cx - bx) * (z - cz)) / d
                l2 = ((cz - az) * (x - cx) + (ax - cx) * (z - cz)) / d
                l3 = 1 - l1 - l2
                if l1 >= 0 and l2 >= 0 and l3 >= 0:
                    hits.append((l1 * wy[a] + l2 * wy[b] + l3 * wy[c], p.name, p.model, m.index))
    return sorted(hits, reverse=True)


def shared_vertices(pieces, flvers, M_of, centre, radius):
    """Number of distinct 1-cm world grid points touched by vertices of >= 2 different parts."""
    owner = {}
    shared = set()
    pairs = {}
    for p in pieces:
        fl = flvers.get(p.model)
        if not fl or math.dist(p.pos, centre) > radius:
            continue
        M = M_of(p)
        seen_here = set()
        for m in fl.meshes:
            if m.error:
                continue
            wx, wy, wz = world_mesh(M, m)
            for k in zip(wx, wy, wz):
                q = (round(k[0] * 100), round(k[1] * 100), round(k[2] * 100))
                seen_here.add(q)
        for q in seen_here:
            o = owner.get(q)
            if o is None:
                owner[q] = p.name
            elif o != p.name:
                shared.add(q)
                key = tuple(sorted((o, p.name)))
                pairs.setdefault(key, q)
    return len(shared), pairs


def main(argv):
    dvdroot, map_id = argv[0], argv[1]
    ref = tuple(float(v) for v in argv[2:5]) if len(argv) >= 5 else REF
    counts = [int(v) for v in argv[5:]] or LIVE_COUNTS
    all_flvers(dvdroot, map_id)

    msb, pieces, flvers, failures = mc.load_map(dvdroot, map_id)
    print('[msb] %d map pieces, %d models parsed, %d failures' % (len(pieces), len(flvers), len(failures)))

    # 1. live triangle counts -> meshes (any face set) and where their placed instances sit
    by_count = {}
    for name, fl in flvers.items():
        for m in fl.meshes:
            for k, fs in enumerate(m.facesets):
                by_count.setdefault(fs.index_count // 3, []).append((name, m, fs))
    print('[live counts] reference point', ref)
    offsets = []
    for c in counts:
        hits = by_count.get(c, [])
        if not hits:
            print('  %6d  no mesh' % c)
            continue
        for name, m, fs in hits:
            best = None
            for p in pieces:
                if p.model != name:
                    continue
                wx, wy, wz = world_mesh(mc.part_matrix(p.pos, p.rot, p.scale), m)
                mn, mx = (min(wx), min(wy), min(wz)), (max(wx), max(wy), max(wz))
                d = aabb_dist(mn, mx, ref)
                if best is None or d < best[0]:
                    best = (d, p.name, mn, mx)
            lod = {0: 'LOD0', mc.FS_LOD1: 'LOD1', mc.FS_LOD2: 'LOD2'}.get(fs.flags, '%08x' % fs.flags)
            if best:
                d, pn, mn, mx = best
                print('  %6d  %s mesh %d %s (%s)  nearest part %s  AABB (%.1f,%.1f,%.1f)-(%.1f,%.1f,%.1f)  dist %.1f m'
                      % (c, name, m.index, lod, m.material, pn, *mn, *mx, d))
            else:
                print('  %6d  %s mesh %d %s: model not placed' % (c, name, m.index, lod))

    # 2. rotation convention: shared vertices between parts near the reference point
    print('[rotation] distinct 1-cm points shared by >=2 parts within 150 m of ref')
    results = {}
    for order in ('XZY', 'XYZ', 'ZXY', 'YXZ'):
        for sign in (1.0, -1.0):
            n, pairs = shared_vertices(pieces, flvers,
                                       lambda p: mc.part_matrix(p.pos, p.rot, p.scale, order, sign), ref, 150)
            results[(order, sign)] = (n, pairs)
            print('  order %s sign %+d: %d shared points, %d part pairs' % (order, sign, n, len(pairs)))
    n, pairs = results[(mc.MSB_ROT_ORDER, mc.MSB_ROT_SIGN)]
    byname = {p.name: p for p in pieces}
    shown = 0
    for (a, b), q in sorted(pairs.items(), key=lambda kv: kv[0]):
        pa, pb = byname[a], byname[b]
        if pa.model == pb.model or abs(pa.rot[1] - pb.rot[1]) < 1:
            continue
        print('  example: %s (%s, pos %s rotY %.1f) and %s (%s, pos %s rotY %.1f) share world vertex (%.2f, %.2f, %.2f)'
              % (a, pa.model, tuple(round(v, 2) for v in pa.pos), pa.rot[1],
                 b, pb.model, tuple(round(v, 2) for v in pb.pos), pb.rot[1], q[0] / 100, q[1] / 100, q[2] / 100))
        shown += 1
        if shown >= 4:
            break

    # 3. ground under the reference point
    hits = ray_down(pieces, flvers, ref[0], ref[2], lambda p: mc.part_matrix(p.pos, p.rot, p.scale))
    near = [h for h in hits if abs(h[0] - ref[1]) < 50]
    print('[ground] vertical ray at x=%.1f z=%.1f: %d hits, %d within 50 m of y=%.1f (others are sky domes / far LOD)'
          % (ref[0], ref[2], len(hits), len(near), ref[1]))
    for y, pn, mn, mi in near[:12]:
        print('  y=%.2f  %s (%s mesh %d)' % (y, pn, mn, mi))

    # 2b. rotation order, only parts with real X/Z rotation (order is irrelevant for pure-Y)
    tilted = [p for p in pieces if abs(p.rot[0]) + abs(p.rot[2]) > 2.0]
    tilted_names = {p.name for p in tilted}
    print('[rotation order] %d parts with |rotX|+|rotZ| > 2 deg; shared points involving them (whole map)' % len(tilted))
    subset = [p for p in pieces if p.name in tilted_names or
              any(math.dist(p.pos, t.pos) < 25 for t in tilted)][:1500]
    for order in ('XZY', 'XYZ', 'ZXY', 'YXZ', 'YZX', 'ZYX'):
        n, pairs = shared_vertices(subset, flvers,
                                   lambda p: mc.part_matrix(p.pos, p.rot, p.scale, order, 1.0), ref, 1e9)
        k = sum(1 for (a, b) in pairs if a in tilted_names or b in tilted_names)
        print('  order %s: %d part pairs touching a tilted part (%d pairs total)' % (order, k, len(pairs)))

    # 4. same model at different places
    multi = {}
    for p in pieces:
        multi.setdefault(p.model, []).append(p)
    big = sorted(multi.items(), key=lambda kv: -len(kv[1]))[:3]
    for name, ps in big:
        xs = [p.pos for p in ps]
        print('[instances] %s placed %d times, x %.1f..%.1f z %.1f..%.1f'
              % (name, len(ps), min(v[0] for v in xs), max(v[0] for v in xs), min(v[2] for v in xs), max(v[2] for v in xs)))


if __name__ == '__main__':
    main(sys.argv[1:])
