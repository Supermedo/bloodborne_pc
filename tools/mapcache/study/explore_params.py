"""List files of the param binders (look for LOD-related params)."""
import glob
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import flstat  # noqa: E402

dv = sys.argv[1]
for p in [os.path.join(dv, 'param', 'gameparam', 'gameparam.parambnd.dcx'),
          os.path.join(dv, 'param', 'drawparam', 'default_drawparam.parambnd.dcx')] + \
        sorted(glob.glob(os.path.join(dv, 'param', 'drawparam', 'm24_01_0000.gparambnd.dcx'))):
    fs = flstat.bnd4_files(flstat.mc.dcx_read(p))
    print(os.path.basename(p), len(fs))
    for fid, name, data in fs:
        print('  ', fid, name.replace('\\', '/').split('/')[-1], len(data))
