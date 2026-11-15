"""Alpha settings of every MTD in mtd/allmaterialbnd.mtdbnd.dcx.

MTD value block as found: param name string, type string, then a block whose marker byte is 0xC5,
followed by int count and the values (int or float). We read g_BlendMode and g_AlphaRef.
Output TSV: name, shader, g_BlendMode, g_AlphaRef, description.
"""
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import flstat  # noqa: E402


def param(data, pname):
    i = data.find(pname.encode() + b'\xa3')
    if i < 0:
        return None
    j = data.find(b'\xc5', i + len(pname) + 8)
    cnt, = struct.unpack_from('<i', data, j + 4)
    return list(struct.unpack_from('<%di' % cnt, data, j + 8))


def mtd_table(dvdroot):
    files = flstat.bnd4_files(flstat.mc.dcx_read(os.path.join(dvdroot, 'mtd', 'allmaterialbnd.mtdbnd.dcx')))
    rows = {}
    for _fid, name, data in files:
        base = name.replace('\\', '/').split('/')[-1]
        spx = re.search(rb'([A-Za-z0-9_]+)\.spx', data)
        desc = re.search(rb'\[MAP\][^\xa3\x00]*', data)
        rows[base] = {'shader': spx.group(1).decode() if spx else '',
                      'blend': param(data, 'g_BlendMode'), 'alpharef': param(data, 'g_AlphaRef'),
                      'desc': desc.group().decode('shift_jis', 'replace').replace('\n', ' ') if desc else ''}
    return rows


if __name__ == '__main__':
    t = mtd_table(sys.argv[1])
    with open(sys.argv[2], 'w', encoding='utf-8', newline='\n') as f:
        f.write('mtd\tshader\tblend\talpharef\tdesc\n')
        for k, v in sorted(t.items()):
            f.write('%s\t%s\t%s\t%s\t%s\n' % (k, v['shader'], v['blend'], v['alpharef'], v['desc']))
    print(len(t), 'MTDs')
