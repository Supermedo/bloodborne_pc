"""Decode the first UV member of alpha-tested meshes of one map folder and print the raw short range,
to settle the UV scale (SoulsFormats: short / 1024, or / 2048 for FLVER >= 0x2000F).

    python -B uv_check.py <dvdroot> <folder> [max files]
"""
import array
import os
import struct
import sys

import alpha
import flstat

dv, folder = sys.argv[1], sys.argv[2]
nmax = int(sys.argv[3]) if len(sys.argv) > 3 else 40
amap = alpha.mtd_alpha(dv)
d = os.path.join(dv, 'map', folder)
lo, hi, n = [1e9, 1e9], [-1e9, -1e9], 0
spans = []
for f in sorted(os.listdir(d))[:4000]:
    if not f.endswith('.flver.dcx') or n >= nmax:
        continue
    b = flstat.mc.dcx_read(os.path.join(d, f))
    st = flstat.stat_flver(b, f)
    (ver, doff, _dl, ndum, nmat, nbone, nmesh, nvb) = struct.unpack_from('<8i', b, 8)
    nfs, = struct.unpack_from('<i', b, 0x50)
    o = 0x80 + 0x40 * ndum + 0x20 * nmat + 0x80 * nbone
    mesh_hdr = [struct.unpack_from('<B3x11i', b, o + 0x30 * i) for i in range(nmesh)]
    o += 0x30 * nmesh + 0x20 * nfs
    vb_hdr = [struct.unpack_from('<8i', b, o + 0x20 * i) for i in range(nvb)]
    for me, mh in zip(st['meshes'], mesh_hdr):
        if not alpha.is_alpha_test(me['mtd'], amap):
            continue
        uv = [x for x in me['layout'] if x[2] == 5]
        if not uv or uv[0][1] not in (0x15, 0x16):
            continue
        vbi, = struct.unpack_from('<i', b, mh[11])
        _bi, _li, vsize, vcount, _a, _b, blen, boff = vb_hdr[vbi]
        raw = b[doff + boff:doff + boff + blen]
        us, vs = [], []
        for k in range(vcount):
            u, v = struct.unpack_from('<hh', raw, k * vsize + uv[0][0])
            us.append(u)
            vs.append(v)
        lo = [min(lo[0], min(us)), min(lo[1], min(vs))]
        hi = [max(hi[0], max(us)), max(hi[1], max(vs))]
        spans.append((max(us) - min(us)) / 2048.0)
        n += 1
spans.sort()
print('%s: %d alpha-test meshes, version 0x%x, raw UV0 short range u %d..%d v %d..%d; '
      'median per-mesh u span /2048 = %.2f (/1024 = %.2f)' % (
          folder, n, ver, lo[0], hi[0], lo[1], hi[1], spans[len(spans) // 2], 2 * spans[len(spans) // 2]))
