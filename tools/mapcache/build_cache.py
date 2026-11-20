"""Builds the geometry cache of one map for the ray tracer: every static map piece's placement
and the lowest-detail triangles of its model, in one little-endian file the engine loads
(gpu/shadps4/video_core/renderer_vulkan/rt/rt_geo_cache.h reads the same layout).

    python build_cache.py <dvdroot_ps4> <map id> <out dir>

File `<map id>.bbgeo`:
    header   8s magic "BBGEO1\\0\\0", u32 version, u32 models, u32 meshes, u32 placements,
             u32 collisions, u32 0, u64 vertex data offset, u64 index data offset, u64 file size
    models   8s name, u32 first mesh, u32 mesh count
    meshes   u32 first vertex, u32 vertex count, u32 first index, u32 index count, u32 flags,
             3 x u32 index counts of the game's LOD0, LOD1, LOD2 draws (0 = none), 6f model-space box
    places   u32 model, u32 flags, 12f world matrix (3 rows of 4), 6f world box, 8 x u32 draw groups
    collisions  8 x u32 display groups, one row per distinct set among the map's collisions
    vertices float32 x y z, only the vertices the kept triangles use
    indices  u32, triangle lists, relative to the mesh's first vertex

Mesh flags: 1 alpha-tested (cut-out), 2 blended or additive (never a caster), 4 moon or sun
material, 8 sky material. Placement flags: 1 an event script may show or hide it (entity id),
2 its model is a sky model.
"""
import array
import os
import struct
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, 'study'))
import mapcache  # noqa: E402
import alpha  # noqa: E402

MAGIC = b'BBGEO1\0\0'
VERSION = 3
MESH_ALPHA_TEST, MESH_BLEND, MESH_MOON, MESH_SKY = 1, 2, 4, 8
PLACE_EVENT, PLACE_SKY = 1, 2


def lowest_faceset(mesh):
    """The coarsest plain triangle list of a mesh: LOD2, else LOD1, else LOD0."""
    best = None
    for fs in mesh.facesets:
        if fs.flags & (mapcache.FS_MOTIONBLUR | mapcache.FS_EDGE) or fs.indices is None:
            continue
        level = 2 if fs.flags & mapcache.FS_LOD2 else 1 if fs.flags & mapcache.FS_LOD1 else 0
        if fs.flags & ~(mapcache.FS_LOD1 | mapcache.FS_LOD2):
            continue
        if best is None or level > best[0]:
            best = (level, fs)
    return best[1] if best else None


def lod_counts(mesh):
    """Index counts of the plain LOD0, LOD1 and LOD2 face sets (0 when there is none): what the
    game's draw of that detail level has in its index count."""
    counts = [0, 0, 0]
    for fs in mesh.facesets:
        if fs.flags & ~(mapcache.FS_LOD1 | mapcache.FS_LOD2):
            continue
        level = 2 if fs.flags & mapcache.FS_LOD2 else 1 if fs.flags & mapcache.FS_LOD1 else 0
        counts[level] = fs.index_count
    return counts


def mesh_flags(mesh, amap):
    kind = alpha.alpha_class(mesh.mtd or '', amap)
    flags = 0
    if kind.startswith('test'):
        flags |= MESH_ALPHA_TEST
    elif not kind.startswith('opaque'):
        flags |= MESH_BLEND
    name = (mesh.material or '') + ' ' + (mesh.mtd or '')
    if 'moon' in name.lower() or 'sun' in name.lower():
        flags |= MESH_MOON
    if 'sky' in name.lower():
        flags |= MESH_SKY
    return flags


def world_box(m, box):
    lo, hi = box[:3], box[3:]
    xs, ys, zs = [], [], []
    for cx in (lo[0], hi[0]):
        for cy in (lo[1], hi[1]):
            for cz in (lo[2], hi[2]):
                xs.append(m[0][0] * cx + m[0][1] * cy + m[0][2] * cz + m[0][3])
                ys.append(m[1][0] * cx + m[1][1] * cy + m[1][2] * cz + m[1][3])
                zs.append(m[2][0] * cx + m[2][1] * cy + m[2][2] * cz + m[2][3])
    return (min(xs), min(ys), min(zs), max(xs), max(ys), max(zs))


def build(dvdroot, map_id, out_dir, log=print):
    t0 = time.time()
    msb = mapcache.read_msb(mapcache.dcx_read(os.path.join(dvdroot, 'map', 'mapstudio', map_id + '.msb.dcx')))
    pieces = [p for p in msb.parts if p.type == 0]
    amap = alpha.mtd_alpha(dvdroot)
    model_records, mesh_records, model_index, model_box = [], [], {}, {}
    vertices, indices = array.array('f'), array.array('I')
    failures = []
    for name in sorted({p.model for p in pieces}):
        path = mapcache.model_path(dvdroot, map_id, name)
        if not os.path.exists(path):
            failures.append('%s: model file missing' % name)
            continue
        try:
            fl = mapcache.read_flver(mapcache.dcx_read(path), os.path.basename(path), all_indices=True)
        except (mapcache.FormatError, struct.error, IndexError, OSError) as e:
            failures.append('%s: %s' % (name, e))
            continue
        first = len(mesh_records)
        lo, hi = [3.0e38] * 3, [-3.0e38] * 3
        for mesh in fl.meshes:
            if mesh.error or mesh.positions is None:
                failures.append(mesh.error or '%s mesh %d: no positions' % (name, mesh.index))
                continue
            fs = lowest_faceset(mesh)
            if fs is None:
                failures.append('%s mesh %d: no plain face set' % (name, mesh.index))
                continue
            tris = mapcache.triangulate(fs.indices, fs.strip, fs.index_size, mesh.vertex_count)
            if not tris:
                continue
            used = sorted(set(tris))
            remap = dict(zip(used, range(len(used))))
            xs, ys, zs = mesh.positions
            vfirst, ifirst = len(vertices) // 3, len(indices)
            box = [3.0e38] * 3 + [-3.0e38] * 3
            for v in used:
                x, y, z = xs[v], ys[v], zs[v]
                vertices.extend((x, y, z))
                box[0], box[1], box[2] = min(box[0], x), min(box[1], y), min(box[2], z)
                box[3], box[4], box[5] = max(box[3], x), max(box[4], y), max(box[5], z)
            indices.extend(remap[i] for i in tris)
            for a in range(3):
                lo[a], hi[a] = min(lo[a], box[a]), max(hi[a], box[a + 3])
            mesh_records.append((vfirst, len(used), ifirst, len(tris), mesh_flags(mesh, amap),
                                 lod_counts(mesh), box))
        if len(mesh_records) == first:
            continue
        model_index[name] = len(model_records)
        model_box[name] = lo + hi
        model_records.append((name.encode('ascii')[:8].ljust(8, b'\0'), first, len(mesh_records) - first))
    place_records = []
    for p in pieces:
        if p.model not in model_index:
            continue
        m = mapcache.part_matrix(p.pos, p.rot, p.scale)
        mi = model_index[p.model]
        sky = any(mesh_records[k][4] & (MESH_SKY | MESH_MOON)
                  for k in range(model_records[mi][1], model_records[mi][1] + model_records[mi][2]))
        flags = (PLACE_EVENT if p.entity_id not in (-1, 0) else 0) | (PLACE_SKY if sky else 0)
        place_records.append((mi, flags, m, world_box(m, model_box[p.model]), tuple(p.groups_a)))
    # The display groups of the map's collisions (the floors the player can stand on): the game
    # draws a piece only while its draw groups share a bit with the display groups of the
    # collision under the player. Checked on the Central Yharnam bridge: the collisions whose
    # groups cover the pieces drawn there show 270-380 of 4,056 pieces each, and none shows the
    # far-view stand-in model that lies over the same buildings.
    collisions = sorted({tuple(p.groups_b) for p in msb.parts if p.type == 5 and any(p.groups_b)})
    header = struct.calcsize('<8sIIIIII QQQ')
    models_at = header
    meshes_at = models_at + 16 * len(model_records)
    places_at = meshes_at + 56 * len(mesh_records)
    collisions_at = places_at + 112 * len(place_records)
    vertex_at = collisions_at + 32 * len(collisions)
    index_at = vertex_at + 4 * len(vertices)
    size = index_at + 4 * len(indices)
    os.makedirs(out_dir, exist_ok=True)
    path = os.path.join(out_dir, map_id + '.bbgeo')
    with open(path + '.tmp', 'wb') as f:
        f.write(struct.pack('<8sIIIIII QQQ', MAGIC, VERSION, len(model_records), len(mesh_records),
                            len(place_records), len(collisions), 0, vertex_at, index_at, size))
        for name, first, count in model_records:
            f.write(struct.pack('<8sII', name, first, count))
        for vfirst, vcount, ifirst, icount, flags, lods, box in mesh_records:
            f.write(struct.pack('<8I6f', vfirst, vcount, ifirst, icount, flags, *lods, *box))
        for mi, flags, m, box, groups in place_records:
            f.write(struct.pack('<II12f6f8I', mi, flags, *m[0], *m[1], *m[2], *box, *groups))
        for groups in collisions:
            f.write(struct.pack('<8I', *groups))
        if sys.byteorder != 'little':
            vertices.byteswap()
            indices.byteswap()
        vertices.tofile(f)
        indices.tofile(f)
    os.replace(path + '.tmp', path)
    log('%s: %d models, %d meshes, %d placements, %.2f M vertices, %.2f M triangles, %.1f MB, %d failures, %.1f s'
        % (map_id, len(model_records), len(mesh_records), len(place_records), len(vertices) / 3e6,
           len(indices) / 3e6, size / 1e6, len(failures), time.time() - t0))
    for line in failures[:10]:
        log('  ' + line)
    return path


if __name__ == '__main__':
    if len(sys.argv) != 4:
        print(__doc__)
        sys.exit(2)
    build(sys.argv[1], sys.argv[2], sys.argv[3])
