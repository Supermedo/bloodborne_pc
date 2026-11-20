"""On-disk cache size per map folder (unique model files, no placement), from the flstat JSON cache.

Per mesh: positions = vertex count x 12 (float32) or x 6 (16-bit quantised in the mesh AABB);
indices = 3 x tris x (2 if vertex count <= 65536 else 4) for each kept LOD; alpha-tested meshes add a
UV per vertex (4 bytes half2). Variants: L0 only, all LODs, lowest only (vertices not compacted = upper bound).
Also: (lowest-only) measured fraction of vertices referenced by the lowest LOD for one folder (decodes indices).

    python -B cachesize.py <dvdroot> [folder for vertex-reference measurement]
"""
import glob
import json
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import flstat  # noqa: E402
import alpha  # noqa: E402
import study  # noqa: E402


def sizes(fl_all, amap):
    r = dict(files=0, meshes=0, verts=0, t0=0, tall=0, tlow=0, pos32=0, pos16=0, i0=0, iall=0, ilow=0, i0_32=0,
             uv=0, big=0)
    for stem, fl in fl_all.items():
        if 'error' in fl:
            continue
        r['files'] += 1
        for m in fl['meshes']:
            vc = m['vc']
            t = study.lod_tris(m['fs'])
            r['meshes'] += 1
            r['verts'] += vc
            r['pos32'] += vc * 12
            r['pos16'] += vc * 6
            ib = 2 if vc <= 65536 else 4
            r['big'] += vc > 65536
            r['t0'] += t[0]
            allt = sum(x for x in t if x)
            r['tall'] += allt
            low = study.eff(t, 2)[0]
            r['tlow'] += low
            r['i0'] += 3 * t[0] * ib
            r['i0_32'] += 3 * t[0] * 4
            r['iall'] += 3 * allt * ib
            r['ilow'] += 3 * low * ib
            if alpha.is_alpha_test(m['mtd'], amap):
                r['uv'] += vc * 4
    return r


def referenced_fraction(dv, folder):
    """lowest-LOD referenced vertices / all vertices (meshes that have a LOD2 or LOD1 set)."""
    d = os.path.join(dv, 'map', folder)
    used = total = 0
    for f in sorted(os.listdir(d)):
        if not f.endswith('.flver.dcx'):
            continue
        b = flstat.mc.dcx_read(os.path.join(d, f))
        (ver, doff, _dl, ndum, nmat, nbone, nmesh, nvb) = struct.unpack_from('<8i', b, 8)
        hdr_isize = b[0x48]
        nfs, = struct.unpack_from('<i', b, 0x50)
        o = 0x80 + 0x40 * ndum + 0x20 * nmat + 0x80 * nbone
        mesh_hdr = [struct.unpack_from('<B3x11i', b, o + 0x30 * i) for i in range(nmesh)]
        o += 0x30 * nmesh
        fs_hdr = [struct.unpack_from('<IBBhiiiiii', b, o + 0x20 * i) for i in range(nfs)]
        o += 0x20 * nfs
        vb_hdr = [struct.unpack_from('<8i', b, o + 0x20 * i) for i in range(nvb)]
        for mh in mesh_hdr:
            fs_cnt, fs_off, vb_cnt, vb_off = mh[8:12]
            sets = [fs_hdr[k] for k in struct.unpack_from('<%di' % fs_cnt, b, fs_off)]
            if len(sets) < 2:
                continue
            vc = vb_hdr[struct.unpack_from('<i', b, vb_off)[0]][3]
            low = max(sets, key=lambda s: s[0])
            isize = low[8] or hdr_isize
            ia = memoryview(b[doff + low[5]:doff + low[5] + low[6]]).cast('H' if isize == 16 else 'I')
            used += len(set(ia))
            total += vc
    return used, total


if __name__ == '__main__':
    dv = sys.argv[1]
    amap = alpha.mtd_alpha(dv)
    tot = {}
    print('folder | files | meshes | verts M | L0 tris M | all-LOD tris M | lowest tris M | pos f32 MB | pos q16 MB | '
          'idx L0 MB | idx all MB | idx lowest MB | idx L0 if all 32-bit MB | alpha UV MB | meshes >65536 verts')
    for p in sorted(glob.glob(os.path.join(HERE, 'cache', 'flstat_*.json'))):
        folder = os.path.basename(p)[7:-5]
        fl_all = json.load(open(p, encoding='utf-8'))
        if not fl_all:
            continue
        r = sizes(fl_all, amap)
        key = 'm29' if folder.startswith('m29') else 'non-m29'
        t = tot.setdefault(key, {k: 0 for k in r})
        for k in r:
            t[k] += r[k]
        print('%s | %d | %d | %.2f | %.2f | %.2f | %.2f | %.0f | %.0f | %.0f | %.0f | %.0f | %.0f | %.0f | %d' % (
            folder, r['files'], r['meshes'], r['verts'] / 1e6, r['t0'] / 1e6, r['tall'] / 1e6, r['tlow'] / 1e6,
            r['pos32'] / 2**20, r['pos16'] / 2**20, r['i0'] / 2**20, r['iall'] / 2**20, r['ilow'] / 2**20,
            r['i0_32'] / 2**20, r['uv'] / 2**20, r['big']))
    for key, r in tot.items():
        print('TOTAL %s | %d | %d | %.2f | %.2f | %.2f | %.2f | %.0f | %.0f | %.0f | %.0f | %.0f | %.0f | %.0f | %d' % (
            key, r['files'], r['meshes'], r['verts'] / 1e6, r['t0'] / 1e6, r['tall'] / 1e6, r['tlow'] / 1e6,
            r['pos32'] / 2**20, r['pos16'] / 2**20, r['i0'] / 2**20, r['iall'] / 2**20, r['ilow'] / 2**20,
            r['i0_32'] / 2**20, r['uv'] / 2**20, r['big']))
    if len(sys.argv) > 2:
        u, t = referenced_fraction(dv, sys.argv[2])
        print('%s: lowest LOD references %d of %d vertices (%.1f%%) in meshes with LODs' % (sys.argv[2], u, t, 100.0 * u / t))
