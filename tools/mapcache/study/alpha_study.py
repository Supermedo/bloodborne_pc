"""Alpha-tested (cut-out) materials in map pieces: share of meshes / triangles, clusters, UV layout,
and where their diffuse textures live (map/mXX/*.tpfbhd name lists).

    python -B alpha_study.py <dvdroot> <map>...
"""
import collections
import os
import re
import struct
import sys

import alpha
import study


def bhf4_names(path):
    b = open(path, 'rb').read()
    if b[:4] != b'BHF4':
        return []
    cnt, = struct.unpack_from('<i', b, 0x0C)
    fhs, = struct.unpack_from('<q', b, 0x20)
    uni = b[0x30]
    out = []
    for i in range(cnt):
        o = 0x40 + fhs * i
        noff, = struct.unpack_from('<i', b, o + fhs - 4)
        csize, = struct.unpack_from('<q', b, o + 8)
        out.append((study.mc._wstr(b, noff) if uni else study.mc._astr(b, noff), csize))
    return out


def tex_index(dv, area):
    """texture base name (lower, no extension) -> (tpfbhd file name, stored size), for map/<area>/*.tpfbhd."""
    idx = {}
    d = os.path.join(dv, 'map', area)
    for f in sorted(os.listdir(d)):
        if f.endswith('.tpfbhd'):
            for n, sz in bhf4_names(os.path.join(d, f)):
                idx.setdefault(n.lower().split('.')[0], (f, sz))
    return idx


def cluster(mat, mtd, tex):
    s = (mat + ' ' + mtd + ' ' + tex).lower()
    for k, pat in (('grass', r'grass|kusa|草|weed'), ('tree/leaf', r'tree|leaf|leaves|branch|wood_?l|ivy|木|葉|枝|plant|bush'),
                   ('fence/grille/railing', r'fence|grill|grid|rail|lattice|iron|bar|chain|net|柵|鉄格子'),
                   ('cloth/rope/hair', r'cloth|rope|cobweb|web|hair|curtain|flag|布|縄'),
                   ('decal/dirt', r'decal|dirt|blood|moss|crack|stain|汚')):
        if re.search(pat, s):
            return k
    return 'other'


def main(dv, maps):
    sys.stdout.reconfigure(encoding='utf-8')
    amap = alpha.mtd_alpha(dv)
    for mp in maps:
        m = study.Map(dv, mp)
        texidx = tex_index(dv, mp[:3])
        cls_u = collections.Counter()
        cls_ut = collections.Counter()
        cls_p = collections.Counter()
        cls_pt = collections.Counter()
        clus = collections.Counter()
        clus_t = collections.Counter()
        uvs = collections.Counter()
        mtds = collections.Counter()
        texfound = collections.Counter()
        tpf_used = collections.Counter()
        uv_used = collections.Counter()
        samples = collections.defaultdict(list)
        utex = {}
        seen = set()
        for pc in m.pieces:
            fl = m.stats[pc['stem']]
            for k, me in enumerate(fl['meshes']):
                c = alpha.alpha_class(me['mtd'], amap)
                t0 = study.lod_tris(me['fs'])[0]
                cls_p[c] += 1
                cls_pt[c] += t0
                key = (pc['stem'], k)
                if key in seen:
                    continue
                seen.add(key)
                cls_u[c] += 1
                cls_ut[c] += t0
                if not c.startswith('test'):
                    continue
                diff = [t[1] for t in me['tex'] if t[0] == 'g_DiffuseTexture']
                dname = diff[0].replace('\\', '/').split('/')[-1] if diff else ''
                cl = cluster(me['mat'], me['mtd'], dname)
                clus[cl] += 1
                clus_t[cl] += t0
                if len(samples[cl]) < 4:
                    samples[cl].append('%s/%s' % (me['mat'], dname))
                mtds[me['mtd'].replace('\\', '/').split('/')[-1]] += 1
                uvm = [(t, i) for (_o, t, s, i) in me['layout'] if s == 5]
                uvs[' '.join('%x' % t for t, _i in uvm)] += 1
                base = dname.lower().split('.')[0]
                f = texidx.get(base)
                texfound['found' if f else 'missing'] += 1
                if f:
                    tpf_used[f[0]] += 1
                    utex[base] = f[1]
                if diff:
                    uv_used[diff[0].replace('\\', '/').rsplit('/', 2)[-2] if '/' in diff[0].replace('\\', '/') else '-'] += 1
        tot_u, tot_ut = sum(cls_u.values()), sum(cls_ut.values())
        tot_p, tot_pt = sum(cls_p.values()), sum(cls_pt.values())
        print('\n## %s alpha classes (unique meshes / unique L0 tris / placed meshes / placed L0 tris)' % mp)
        for c in sorted(cls_u):
            print('   %-10s %5d (%.1f%%) | %9d (%.1f%%) | %6d (%.1f%%) | %9d (%.1f%%)' % (
                c, cls_u[c], 100.0 * cls_u[c] / tot_u, cls_ut[c], 100.0 * cls_ut[c] / tot_ut,
                cls_p[c], 100.0 * cls_p[c] / tot_p, cls_pt[c], 100.0 * cls_pt[c] / tot_pt))
        print('   alpha-test clusters (unique meshes, L0 tris):', [(k, clus[k], clus_t[k]) for k, _ in clus.most_common()])
        for k, v in samples.items():
            print('      %s e.g. %s' % (k, '; '.join(v)))
        print('   alpha-test MTDs:', mtds.most_common(10))
        print('   alpha-test UV member types (per mesh):', uvs.most_common(6))
        print('   diffuse texture found in map/%s/*.tpfbhd: %s; by binder: %s' % (mp[:3], dict(texfound), tpf_used.most_common(6)))
        print('   diffuse texture source folder in path:', uv_used.most_common(4))
        print('   unique alpha-test diffuse textures %d, stored (DCX TPF) size %.1f MB' % (len(utex), sum(utex.values()) / 1e6))


if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2:])
