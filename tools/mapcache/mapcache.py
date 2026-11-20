#!/usr/bin/env python3
"""mapcache: read Bloodborne (PS4 1.09) map files and place static map-piece geometry in world space.

Library:
    dcx_read(path) -> bytes
    read_flver(data, name) -> Flver
    read_msb(data) -> Msb
    part_matrix(position, rotation_deg, scale) -> 3x4 row-major list

CLI:
    python mapcache.py dump <dvdroot_ps4> <map id> <out dir> [--obj]

Pure standard library. All formats little-endian except the DCX header (big-endian).
See README.md for the formats as found in the files.
"""
import array
import math
import os
import struct
import sys
import time
import zlib

# --------------------------------------------------------------------------- DCX


class FormatError(Exception):
    pass


def dcx_read(path):
    """Return the decompressed payload of a DCX file (or the raw file if it is not DCX)."""
    with open(path, 'rb') as f:
        b = f.read()
    if b[:4] != b'DCX\0':
        return b
    # Big-endian header: DCX\0 ver hdrsize ... DCS\0 usize csize DCP\0 'DFLT' ... DCA\0 8 <zlib>
    ver, = struct.unpack_from('>I', b, 4)
    if b[0x18:0x1C] != b'DCS\0' or b[0x24:0x28] != b'DCP\0' or b[0x44:0x48] != b'DCA\0':
        raise FormatError('%s: unexpected DCX layout (version %08x)' % (path, ver))
    usize, csize = struct.unpack_from('>II', b, 0x1C)
    fmt = b[0x28:0x2C]
    if fmt != b'DFLT':
        raise FormatError('%s: DCX compression %r not supported' % (path, fmt))
    ca_size, = struct.unpack_from('>I', b, 0x48)
    start = 0x44 + ca_size  # DCA block size (8) includes its own magic + size
    data = zlib.decompress(b[start:start + csize])
    if len(data) != usize:
        raise FormatError('%s: DCX size mismatch %d != %d' % (path, len(data), usize))
    return data


def _wstr(b, o):
    """UTF-16LE zero-terminated string at o."""
    e = o
    while b[e:e + 2] != b'\0\0':
        e += 2
    return b[o:e].decode('utf-16le')


def _astr(b, o):
    return b[o:b.index(b'\0', o)].decode('shift_jis', 'replace')

# --------------------------------------------------------------------------- FLVER2

# Vertex member type -> size in bytes (SoulsFormats FLVER.LayoutType).
LAYOUT_TYPE_SIZE = {
    0x01: 8,   # Float2
    0x02: 12,  # Float3
    0x03: 16,  # Float4
    0x10: 4,   # Byte4A
    0x11: 4,   # Byte4B
    0x12: 4,   # Short2toFloat2
    0x13: 4,   # Byte4C
    0x15: 4,   # UV (2 x short)
    0x16: 8,   # UVPair (4 x short)
    0x18: 8,   # ShortBoneIndices
    0x1A: 8,   # Short4toFloat4A
    0x2E: 8,   # Short4toFloat4B
    0x2F: 4,   # Byte4E
}
SEMANTICS = {0: 'Position', 1: 'BoneWeights', 2: 'BoneIndices', 3: 'Normal', 5: 'UV',
             6: 'Tangent', 7: 'Bitangent', 10: 'VertexColor'}

FS_LOD1 = 0x01000000
FS_LOD2 = 0x02000000
FS_EDGE = 0x40000000
FS_MOTIONBLUR = 0x80000000


class FaceSet:
    __slots__ = ('flags', 'strip', 'cull', 'index_size', 'index_count', 'indices')

    def label(self):
        return '%08x/%s%d' % (self.flags, 'strip' if self.strip else 'list', self.index_count)


class Mesh:
    __slots__ = ('index', 'dynamic', 'material', 'mtd', 'default_bone', 'vertex_count',
                 'positions', 'facesets', 'lod0', 'tris', 'bbox', 'layout', 'error', 'raw', 'node')

    def __init__(self):
        self.error = None
        self.raw = None    # positions as stored (the game's vertex buffer), before the node
        self.node = None   # the node matrix applied to them, None = identity
        self.tris = array.array('I')


class Flver:
    def __init__(self):
        self.version = 0
        self.bones = []      # dicts: name, t, r, s, parent
        self.meshes = []
        self.bbox = None


def _bone_local(t, r, s):
    """SoulsFormats FLVER bone: v * S * Rx * Rz * Ry * T (row vectors), r in radians.
    Returned as a column-vector 3x4: world = M * [x y z 1]."""
    return _compose(t, r, s, 'XZY', 1.0)


def _mat_mul(a, b):
    """3x4 * 3x4 (affine, column vectors)."""
    out = []
    for i in range(3):
        row = []
        for j in range(4):
            v = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j]
            if j == 3:
                v += a[i][3]
            row.append(v)
        out.append(row)
    return out


def _is_identity(m, eps=1e-6):
    ident = ((1, 0, 0, 0), (0, 1, 0, 0), (0, 0, 1, 0))
    return all(abs(m[i][j] - ident[i][j]) <= eps for i in range(3) for j in range(4))


def read_flver(data, name='?', want_positions=True, all_indices=False):
    """Parse a FLVER2 (little-endian). Each mesh gets model-space positions with its node
    (bone) transform applied, and LOD-0 triangle indices. A mesh that uses a feature this
    parser does not handle gets .error set (and no geometry) instead of guessed data."""
    b = data
    if b[:6] != b'FLVER\0':
        raise FormatError('%s: not a FLVER' % name)
    if b[6:8] != b'L\0':
        raise FormatError('%s: big-endian FLVER not supported' % name)
    (ver, doff, _dlen, ndum, nmat, nbone, nmesh, nvb) = struct.unpack_from('<8i', b, 8)
    if ver >> 16 != 2:
        raise FormatError('%s: FLVER version %08x is not FLVER2' % (name, ver))
    bbmin = struct.unpack_from('<3f', b, 0x28)
    bbmax = struct.unpack_from('<3f', b, 0x34)
    hdr_index_size = b[0x48]
    unicode = b[0x49]
    nfs, nlay, _ntex = struct.unpack_from('<3i', b, 0x50)
    rd_str = _wstr if unicode else _astr

    fl = Flver()
    fl.version = ver
    fl.bbox = (bbmin, bbmax)
    o = 0x80 + 0x40 * ndum   # dummies skipped (not geometry)

    mats = []
    for i in range(nmat):
        name_off, mtd_off = struct.unpack_from('<2i', b, o + 0x20 * i)
        mats.append((rd_str(b, name_off), rd_str(b, mtd_off)))
    o += 0x20 * nmat

    for i in range(nbone):
        f = struct.unpack_from('<3fi3fhh3fhh', b, o + 0x80 * i)
        fl.bones.append({'name': rd_str(b, f[3]), 't': f[0:3], 'r': f[4:7], 'parent': f[7],
                         's': f[9:12]})
    o += 0x80 * nbone
    # Node world transforms: local transforms chained through parents.
    world = []
    for i, bone in enumerate(fl.bones):
        m = _bone_local(bone['t'], bone['r'], bone['s'])
        p, guard = bone['parent'], 0
        while 0 <= p < nbone and guard < 256:
            pb = fl.bones[p]
            m = _mat_mul(_bone_local(pb['t'], pb['r'], pb['s']), m)
            p, guard = pb['parent'], guard + 1
        world.append(m)

    mesh_hdr = [struct.unpack_from('<B3x11i', b, o + 0x30 * i) for i in range(nmesh)]
    o += 0x30 * nmesh
    fs_hdr = [struct.unpack_from('<IBBhiiiiii', b, o + 0x20 * i) for i in range(nfs)]
    o += 0x20 * nfs
    vb_hdr = [struct.unpack_from('<8i', b, o + 0x20 * i) for i in range(nvb)]
    o += 0x20 * nvb
    layouts = []
    for i in range(nlay):
        cnt, _z0, _z1, moff = struct.unpack_from('<4i', b, o + 0x10 * i)
        layouts.append([struct.unpack_from('<iiIIi', b, moff + 0x14 * j) for j in range(cnt)])

    for mi, mh in enumerate(mesh_hdr):
        (dyn, mat_i, _z0, _z1, def_bone, bone_cnt, bb_off, _bone_off,
         fs_cnt, fs_off, vb_cnt, vb_off) = mh
        m = Mesh()
        m.index = mi
        m.dynamic = dyn
        m.material, m.mtd = mats[mat_i] if 0 <= mat_i < nmat else ('?', '?')
        m.default_bone = def_bone
        m.bbox = struct.unpack_from('<6f', b, bb_off) if bb_off else None
        m.facesets = []
        m.positions = None
        m.vertex_count = 0
        m.lod0 = None
        fl.meshes.append(m)
        try:
            # --- vertex buffer / layout
            if vb_cnt != 1:
                raise FormatError('%d vertex buffers per mesh not handled' % vb_cnt)
            vbi, = struct.unpack_from('<i', b, vb_off)
            (_bidx, lay_i, vsize, vcount, _z2, _z3, blen, boff) = vb_hdr[vbi]
            m.vertex_count = vcount
            lay = layouts[lay_i]
            end = 0
            for (_u, soff, typ, sem, _idx) in lay:
                if typ not in LAYOUT_TYPE_SIZE:
                    raise FormatError('vertex member type 0x%x (semantic %d) not handled' % (typ, sem))
                if soff != end:
                    raise FormatError('layout gap at %d (expected %d)' % (soff, end))
                end = soff + LAYOUT_TYPE_SIZE[typ]
            if end != vsize:
                raise FormatError('layout size %d != vertex size %d' % (end, vsize))
            if blen != vsize * vcount:
                raise FormatError('buffer length %d != %d*%d' % (blen, vsize, vcount))
            m.layout = ' '.join('%s:%x@%d' % (SEMANTICS.get(sem, 'sem%d' % sem), typ, soff)
                                for (_u, soff, typ, sem, _idx) in lay)
            pos = [x for x in lay if x[3] == 0]
            if len(pos) != 1:
                raise FormatError('%d position members' % len(pos))
            _u, poff, ptyp, _s, _i = pos[0]
            if ptyp not in (0x02, 0x03):
                raise FormatError('position type 0x%x not handled' % ptyp)
            if dyn:
                raise FormatError('dynamic (skinned) mesh not handled for map pieces')
            # --- node transform. All map-piece meshes seen are static (dynamic=0, no bone
            # list); their vertices are stored relative to one node. With a single bone that
            # node is bone 0; otherwise use DefaultBoneIndex.
            if nbone == 1:
                node = world[0]
            elif 0 <= def_bone < nbone:
                node = world[def_bone]
            elif nbone == 0:
                node = None
            else:
                raise FormatError('default bone %d out of range (%d bones)' % (def_bone, nbone))
            if want_positions:
                start = doff + boff
                if vsize % 4 == 0 and poff % 4 == 0:
                    a = array.array('f')
                    a.frombytes(b[start:start + blen])
                    if sys.byteorder != 'little':
                        a.byteswap()
                    s4, p4 = vsize // 4, poff // 4
                    xs, ys, zs = a[p4::s4], a[p4 + 1::s4], a[p4 + 2::s4]
                else:
                    raise FormatError('unaligned vertex size %d / position offset %d' % (vsize, poff))
                m.raw = (xs, ys, zs)
                if node is not None and not _is_identity(node):
                    m.node = node
                    xs, ys, zs = _apply(node, xs, ys, zs)
                m.positions = (xs, ys, zs)
            # --- face sets
            fs_ids = struct.unpack_from('<%di' % fs_cnt, b, fs_off)
            for k in fs_ids:
                (flags, strip, cull, _u6, icount, ioff, ilen, _z4, isize, _z5) = fs_hdr[k]
                fs = FaceSet()
                fs.flags, fs.strip, fs.cull = flags, strip, cull
                fs.index_size = isize or hdr_index_size
                fs.index_count = icount
                fs.indices = None
                if fs.index_size not in (16, 32):
                    raise FormatError('index size %d' % fs.index_size)
                if ilen != icount * fs.index_size // 8:
                    raise FormatError('index length %d != %d*%d' % (ilen, icount, fs.index_size // 8))
                if flags & FS_EDGE:
                    raise FormatError('edge-compressed face set not handled')
                m.facesets.append(fs)
                if flags == 0 or all_indices:
                    ia = array.array('H' if fs.index_size == 16 else 'I')
                    ia.frombytes(b[doff + ioff:doff + ioff + ilen])
                    if sys.byteorder != 'little':
                        ia.byteswap()
                    fs.indices = ia
                if flags == 0:
                    if m.lod0 is not None:
                        raise FormatError('two LOD-0 face sets')
                    m.lod0 = fs
            if m.lod0 is None:
                raise FormatError('no LOD-0 face set (flags %s)' % [hex(f.flags) for f in m.facesets])
            m.tris = triangulate(m.lod0.indices, m.lod0.strip, m.lod0.index_size, vcount)
        except (FormatError, struct.error, IndexError) as e:
            m.error = '%s mesh %d: %s' % (name, mi, e)
            m.positions = None
    return fl


def triangulate(idx, strip, index_size, vcount):
    """Flat triangle list from a face set. Strips are expanded with alternating winding and
    restart on the all-ones index; degenerate triangles are dropped."""
    restart = 0xFFFF if index_size == 16 else 0xFFFFFFFF
    out = array.array('I')
    if strip:
        flip = False
        for i in range(len(idx) - 2):
            a, b_, c = idx[i], idx[i + 1], idx[i + 2]
            if a == restart or b_ == restart or c == restart:
                flip = False
                continue
            if a != b_ and b_ != c and a != c:
                out.extend((c, b_, a) if flip else (a, b_, c))
            flip = not flip
    else:
        if len(idx) % 3:
            raise FormatError('triangle list length %d not a multiple of 3' % len(idx))
        for i in range(0, len(idx), 3):
            a, b_, c = idx[i], idx[i + 1], idx[i + 2]
            if a != b_ and b_ != c and a != c:
                out.extend((a, b_, c))
    if out and max(out) >= vcount:
        raise FormatError('index %d >= vertex count %d' % (max(out), vcount))
    return out


def _apply(m, xs, ys, zs):
    (a, b, c, tx), (d, e, f, ty), (g, h, i, tz) = m
    wx = [a * x + b * y + c * z + tx for x, y, z in zip(xs, ys, zs)]
    wy = [d * x + e * y + f * z + ty for x, y, z in zip(xs, ys, zs)]
    wz = [g * x + h * y + i * z + tz for x, y, z in zip(xs, ys, zs)]
    return wx, wy, wz

# --------------------------------------------------------------------------- MSB (Bloodborne)

PART_TYPES = {0: 'MapPiece', 1: 'Object', 2: 'Enemy', 4: 'Player', 5: 'Collision',
              8: 'Navmesh', 9: 'DummyObject', 10: 'DummyEnemy', 11: 'ConnectCollision'}
MODEL_TYPES = {0: 'MapPiece', 1: 'Object', 2: 'Enemy', 4: 'Player', 5: 'Collision', 6: 'Navmesh'}


class Part:
    __slots__ = ('index', 'name', 'desc', 'instance_id', 'type', 'type_index', 'model_index',
                 'model', 'sib', 'pos', 'rot', 'scale', 'groups_a', 'groups_b', 'groups_c', 'entity_id')


class Msb:
    def __init__(self):
        self.params = []    # (name, version, count)
        self.models = []    # dicts
        self.parts = []     # Part


def _msb_params(b):
    o = 0x10
    while o:
        ver, cnt, name_off = struct.unpack_from('<iiq', b, o)
        offs = struct.unpack_from('<%dq' % cnt, b, o + 16)
        yield _wstr(b, name_off), ver, list(offs[:-1])
        o = offs[-1]


def read_msb(data):
    """Bloodborne MSB: 'MSB ' header (0x10 bytes), then a linked chain of params
    (MODEL_PARAM_ST, EVENT_PARAM_ST, POINT_PARAM_ST, PARTS_PARAM_ST, ...)."""
    b = data
    if b[:4] != b'MSB ':
        raise FormatError('not an MSB')
    msb = Msb()
    for pname, ver, offs in _msb_params(b):
        msb.params.append((pname, ver, len(offs)))
        if pname == 'MODEL_PARAM_ST':
            for e in offs:
                name_off, typ, tidx, sib_off, inst = struct.unpack_from('<qiiqi', b, e)
                msb.models.append({'name': _wstr(b, e + name_off), 'type': typ, 'type_index': tidx,
                                   'sib': _wstr(b, e + sib_off), 'instances': inst})
        elif pname == 'PARTS_PARAM_ST':
            for i, e in enumerate(offs):
                f = struct.unpack_from('<qqiiiiq9f', b, e)
                p = Part()
                p.index = i
                p.desc = _wstr(b, e + f[0])
                p.name = _wstr(b, e + f[1])
                p.instance_id, p.type, p.type_index, p.model_index = f[2:6]
                p.sib = _wstr(b, e + f[6])
                p.pos, p.rot, p.scale = f[7:10], f[10:13], f[13:16]
                g = struct.unpack_from('<24I', b, e + 0x4C)
                p.groups_a, p.groups_b, p.groups_c = g[0:8], g[8:16], g[16:24]
                ent_off, = struct.unpack_from('<q', b, e + 0xB0)
                p.entity_id, = struct.unpack_from('<i', b, e + ent_off)  # EMEVD target id, -1 = none
                p.model = msb.models[p.model_index]['name'] if 0 <= p.model_index < len(msb.models) else None
                msb.parts.append(p)
    return msb


def _compose(t, r, s, order, sign):
    """Column-vector 3x4 for: scale, then the rotations about the axes in `order` (first
    letter applied first), then translation. Angles in radians, right-hand-rule matrices on
    the raw coordinates (numerically System.Numerics CreateRotationX/Y/Z)."""
    def rx(a):
        c, s_ = math.cos(a), math.sin(a)
        return [[1, 0, 0, 0], [0, c, -s_, 0], [0, s_, c, 0]]

    def ry(a):
        c, s_ = math.cos(a), math.sin(a)
        return [[c, 0, s_, 0], [0, 1, 0, 0], [-s_, 0, c, 0]]

    def rz(a):
        c, s_ = math.cos(a), math.sin(a)
        return [[c, -s_, 0, 0], [s_, c, 0, 0], [0, 0, 1, 0]]
    rot = {'X': lambda: rx(sign * r[0]), 'Y': lambda: ry(sign * r[1]), 'Z': lambda: rz(sign * r[2])}
    m = [[s[0], 0, 0, 0], [0, s[1], 0, 0], [0, 0, s[2], 0]]
    for axis in order:
        m = _mat_mul(rot[axis](), m)
    m[0][3] += t[0]
    m[1][3] += t[1]
    m[2][3] += t[2]
    return m


# MSB rotation convention. Verified on m24_00_00_00 (see README): Euler degrees, applied
# X then Z then Y (row-vector S*Rx*Rz*Ry*T, same as FLVER bones in SoulsFormats).
MSB_ROT_ORDER = 'XZY'
MSB_ROT_SIGN = 1.0


def part_matrix(position, rotation_deg, scale, order=None, sign=None):
    r = [math.radians(a) for a in rotation_deg]
    return _compose(position, r, scale, order or MSB_ROT_ORDER, MSB_ROT_SIGN if sign is None else sign)

# --------------------------------------------------------------------------- map dump


def model_path(dvdroot, map_id, model_name):
    """Map-piece model 'm009001' of map m24_00_00_00 -> map/m24_00_00_00/m24_00_00_00_009001.flver.dcx"""
    return os.path.join(dvdroot, 'map', map_id, '%s_%s.flver.dcx' % (map_id, model_name[1:]))


def load_map(dvdroot, map_id, log=print):
    """Returns (msb, pieces, flvers, failures). pieces = map-piece Parts, flvers = model name -> Flver."""
    msb = read_msb(dcx_read(os.path.join(dvdroot, 'map', 'mapstudio', map_id + '.msb.dcx')))
    pieces = [p for p in msb.parts if p.type == 0]
    flvers, failures = {}, []
    for name in sorted({p.model for p in pieces}):
        path = model_path(dvdroot, map_id, name)
        if not os.path.exists(path):
            failures.append('%s: model file missing %s' % (name, path))
            continue
        try:
            fl = read_flver(dcx_read(path), os.path.basename(path))
        except (FormatError, struct.error, zlib.error, IndexError) as e:
            failures.append('%s: %s' % (name, e))
            continue
        flvers[name] = fl
        failures.extend(m.error for m in fl.meshes if m.error)
    return msb, pieces, flvers, failures


def _hexgroups(g):
    return ''.join('%08x' % x for x in reversed(g)).lstrip('0') or '0'


def dump(dvdroot, map_id, out_dir, write_obj=False, near=None):
    """near = (x, y, z, r): OBJ only gets parts whose position is within r (the whole map is ~6 GB)."""
    t0 = time.time()
    msb, pieces, flvers, failures = load_map(dvdroot, map_id)
    t_parse = time.time() - t0
    os.makedirs(out_dir, exist_ok=True)
    n_mesh_inst = n_tri = 0
    gmin = [math.inf] * 3
    gmax = [-math.inf] * 3
    objf = open(os.path.join(out_dir, map_id + '.obj'), 'w', newline='\n') if write_obj else None
    vbase = 1
    with open(os.path.join(out_dir, map_id + '.pieces.tsv'), 'w', encoding='utf-8', newline='\n') as tsv, \
            open(os.path.join(out_dir, map_id + '.parts.tsv'), 'w', encoding='utf-8', newline='\n') as ptsv:
        tsv.write('part\tmodel\tmesh\tverts\ttris_lod0\tfacesets\tmaterial\tmtd\t'
                  'min_x\tmin_y\tmin_z\tmax_x\tmax_y\tmax_z\tv0_x\tv0_y\tv0_z\n')
        ptsv.write('index\tpart\tdesc\tinstance_id\tentity_id\tmodel\tpos_x\tpos_y\tpos_z\trot_x\trot_y\trot_z\t'
                   'scale_x\tscale_y\tscale_z\tgroups_0x4c\tgroups_0x6c\tgroups_0x8c\tsib\n')
        for p in pieces:
            ptsv.write('%d\t%s\t%s\t%d\t%d\t%s\t%s\t%s\t%s\t%s\t%s\n' % (
                p.index, p.name, p.desc, p.instance_id, p.entity_id, p.model,
                '\t'.join('%.4f' % v for v in p.pos), '\t'.join('%.4f' % v for v in p.rot),
                '\t'.join('%.4f' % v for v in p.scale),
                '\t'.join(_hexgroups(g) for g in (p.groups_a, p.groups_b, p.groups_c)), p.sib))
            fl = flvers.get(p.model)
            if fl is None:
                continue
            M = part_matrix(p.pos, p.rot, p.scale)
            to_obj = objf and (near is None or math.dist(p.pos, near[:3]) <= near[3])
            if to_obj:
                objf.write('o %s\n' % p.name)
            for m in fl.meshes:
                fsdesc = ' '.join(f.label() for f in m.facesets)
                if m.error:
                    tsv.write('%s\t%s\t%d\t%d\tERROR\t%s\t%s\t%s\t%s\n' % (
                        p.name, p.model, m.index, m.vertex_count, fsdesc, m.material, m.mtd,
                        m.error.replace('\t', ' ')))
                    continue
                wx, wy, wz = _apply(M, *m.positions)
                mn = (min(wx), min(wy), min(wz))
                mx = (max(wx), max(wy), max(wz))
                for c in range(3):
                    gmin[c] = min(gmin[c], mn[c])
                    gmax[c] = max(gmax[c], mx[c])
                nt = len(m.tris) // 3
                n_mesh_inst += 1
                n_tri += nt
                tsv.write('%s\t%s\t%d\t%d\t%d\t%s\t%s\t%s\t%s\t%s\t%s\n' % (
                    p.name, p.model, m.index, m.vertex_count, nt, fsdesc, m.material, m.mtd,
                    '\t'.join('%.3f' % v for v in mn), '\t'.join('%.3f' % v for v in mx),
                    '%.3f\t%.3f\t%.3f' % (wx[0], wy[0], wz[0])))
                if to_obj:
                    objf.write(''.join('v %.4f %.4f %.4f\n' % v for v in zip(wx, wy, wz)))
                    t = m.tris
                    objf.write(''.join('f %d %d %d\n' % (t[i] + vbase, t[i + 1] + vbase, t[i + 2] + vbase)
                                       for i in range(0, len(t), 3)))
                    vbase += len(wx)
    if objf:
        objf.close()
    skipped = {}
    for p in msb.parts:
        if p.type != 0:
            k = PART_TYPES.get(p.type, 'type%d' % p.type)
            skipped[k] = skipped.get(k, 0) + 1
    n_models = len({p.model for p in pieces})
    n_meshes = sum(len(f.meshes) for f in flvers.values())
    print('map %s: %d map-piece parts, %d map-piece models (%d parsed), %d meshes, '
          '%d placed meshes, %d LOD-0 triangles placed' % (
              map_id, len(pieces), n_models, len(flvers), n_meshes, n_mesh_inst, n_tri))
    print('skipped parts: ' + ', '.join('%s %d' % kv for kv in sorted(skipped.items())))
    print('world AABB: min (%.2f, %.2f, %.2f) max (%.2f, %.2f, %.2f)' % (*gmin, *gmax))
    print('parse %.1fs, total %.1fs, failures %d' % (t_parse, time.time() - t0, len(failures)))
    for f in failures:
        print('  FAIL ' + f)
    return failures


def main(argv):
    if len(argv) >= 4 and argv[0] == 'dump':
        near = None
        if '--near' in argv:
            near = tuple(float(v) for v in argv[argv.index('--near') + 1].split(','))
        fails = dump(argv[1], argv[2], argv[3], write_obj='--obj' in argv[4:], near=near)
        return 1 if fails else 0
    print(__doc__)
    return 2


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
