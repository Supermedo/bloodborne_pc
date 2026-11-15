"""Hex dump of an MTD from the archive around a given string."""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import flstat  # noqa: E402

files = flstat.bnd4_files(flstat.mc.dcx_read(os.path.join(sys.argv[1], 'mtd', 'allmaterialbnd.mtdbnd.dcx')))
for fid, name, data in files:
    if name.replace('\\', '/').split('/')[-1] == sys.argv[2]:
        i = data.find(sys.argv[3].encode())
        seg = data[i - 16:i + int(sys.argv[4])]
        for k in range(0, len(seg), 16):
            print('%04x ' % (i - 16 + k) + ' '.join('%02x' % c for c in seg[k:k + 16]))
