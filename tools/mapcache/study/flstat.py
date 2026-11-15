#!/usr/bin/env python3
"""Header-level FLVER2 statistics (no vertex decode) + BND4 reader + cache of stats per map folder.

    stat_flver(data) -> dict (meshes: material, mtd, textures, bbox, vertex count, layout, face sets)
    bnd4_files(data) -> [(id, name, bytes)]
    folder_stats(dvdroot, folder, cache_dir) -> {model file stem: stat dict}  (cached as JSON)
"""
import json
import os
import struct
import sys
import time
import zlib

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
import mapcache as mc  # noqa: E402


def _str(b, o, uni):
    return mc._wstr(b, o) if uni else mc._astr(b, o)


def stat_flver(b, name='?'):
    if b[:6] != b'FLVER\0' or b[6:8] != b'L\0':
        raise mc.FormatError('%s: not a little-endian FLVER' % name)
    (ver, doff, _dlen, ndum, nmat, nbone, nmesh, nvb) = struct.unpack_from('<8i', b, 8)
    hdr_isize = b[0x48]
    uni = b[0x49]
    nfs, nlay, ntex = struct.unpack_from('<3i', b, 0x50)
    o = 0x80 + 0x40 * ndum
    mats = [struct.unpack_from('<6i', b, o + 0x20 * i) for i in range(nmat)]
    o += 0x20 * nmat + 0x80 * nbone
    mesh_hdr = [struct.unpack_from('<B3x11i', b, o + 0x30 * i) for i in range(nmesh)]
    o += 0x30 * nmesh
    fs_hdr = [struct.unpack_from('<IBBhiiiiii', b, o + 0x20 * i) for i in range(nfs)]
    o += 0x20 * nfs
    vb_hdr = [struct.unpack_from('<8i', b, o + 0x20 * i) for i in range(nvb)]
    o += 0x20 * nvb
    layouts = []
    for i in range(nlay):
        cnt, _a, _b, moff = struct.unpack_from('<4i', b, o + 0x10 * i)
        layouts.append([struct.unpack_from('<iiIIi', b, moff + 0x14 * j)[1:] for j in range(cnt)])
    o += 0x10 * nlay
    texs = []
    for i in range(ntex):
        p, t = struct.unpack_from('<2i', b, o + 0x20 * i)
        texs.append((_str(b, t, uni), _str(b, p, uni)))
    out = {'ver': ver, 'nbone': nbone, 'meshes': []}
    for mh in mesh_hdr:
        (dyn, mat_i, _z0, _z1, _db, _bc, bb_off, _bo, fs_cnt, fs_off, vb_cnt, vb_off) = mh
        nm, mtd, tc, ti, mflags, _gx = mats[mat_i]
        me = {'dyn': dyn, 'mat': _str(b, nm, uni), 'mtd': _str(b, mtd, uni),
              'tex': texs[ti:ti + tc],
              'bbox': list(struct.unpack_from('<6f', b, bb_off)) if bb_off else None}
        vbs = struct.unpack_from('<%di' % vb_cnt, b, vb_off) if vb_cnt else ()
        me['vc'] = vb_hdr[vbs[0]][3] if vbs else 0
        me['vsize'] = sum(vb_hdr[v][2] for v in vbs)
        me['layout'] = [m for v in vbs for m in layouts[vb_hdr[v][1]]]  # (struct off, type, semantic, index)
        fss = []
        for k in struct.unpack_from('<%di' % fs_cnt, b, fs_off):
            (flags, strip, cull, _u, icount, ioff, ilen, _z4, isize, _z5) = fs_hdr[k]
            isize = isize or hdr_isize
            if strip:
                ia = memoryview(b[doff + ioff:doff + ioff + ilen]).cast('H' if isize == 16 else 'I')
                tris = len(mc.triangulate(ia, 1, isize, 1 << 32))
                tris //= 3
            else:
                tris = icount // 3
            fss.append((flags, strip, cull, icount, isize, tris))
        me['fs'] = fss
        if me['bbox'] is None:
            me['bbox'] = None
        out['meshes'].append(me)
    return out


def bnd4_files(b):
    """BND4 container -> [(id, name, data)]."""
    if b[:4] != b'BND4':
        raise mc.FormatError('not BND4')
    cnt, = struct.unpack_from('<i', b, 0x0C)
    fhs, = struct.unpack_from('<q', b, 0x20)
    uni = b[0x30]
    fmt = b[0x31]
    out = []
    for i in range(cnt):
        o = 0x40 + fhs * i
        csize, = struct.unpack_from('<q', b, o + 8)
        p = o + 16
        if fmt & 0x20 or fmt & 0x04:   # uncompressed size present
            p += 8
        if fhs >= 0x24:
            doffs, = struct.unpack_from('<q', b, p) if fhs == 0x2C else struct.unpack_from('<i', b, p)
        else:
            doffs, = struct.unpack_from('<i', b, p)
        p += 8 if fhs == 0x2C else 4
        fid, noff = struct.unpack_from('<ii', b, p)
        name = mc._wstr(b, noff) if uni else mc._astr(b, noff)
        out.append((fid, name, b[doffs:doffs + csize]))
    return out


def folder_stats(dvdroot, folder, cache_dir):
    path = os.path.join(cache_dir, 'flstat_%s.json' % folder)
    if os.path.exists(path):
        with open(path, encoding='utf-8') as f:
            return json.load(f)
    d = os.path.join(dvdroot, 'map', folder)
    res = {}
    for fn in sorted(os.listdir(d)):
        if not fn.endswith('.flver.dcx'):
            continue
        stem = fn[:-len('.flver.dcx')]
        try:
            res[stem] = stat_flver(mc.dcx_read(os.path.join(d, fn)), fn)
            res[stem]['bytes'] = os.path.getsize(os.path.join(d, fn))
        except (mc.FormatError, struct.error, zlib.error, IndexError, UnicodeDecodeError) as e:
            res[stem] = {'error': str(e)}
    os.makedirs(cache_dir, exist_ok=True)
    with open(path, 'w', encoding='utf-8') as f:
        json.dump(res, f)
    return res


if __name__ == '__main__':
    t = time.time()
    r = folder_stats(sys.argv[1], sys.argv[2], sys.argv[3])
    print(sys.argv[2], len(r), 'files', '%.1fs' % (time.time() - t))
