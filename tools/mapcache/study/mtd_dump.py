"""List the MTD archive (mtd/allmaterialbnd.mtdbnd.dcx): names, and printable strings of chosen MTDs."""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import flstat  # noqa: E402

dv = sys.argv[1]
files = flstat.bnd4_files(flstat.mc.dcx_read(os.path.join(dv, 'mtd', 'allmaterialbnd.mtdbnd.dcx')))
print(len(files), 'files')
want = sys.argv[2:]
for fid, name, data in files:
    base = name.split('\\')[-1]
    if not want:
        print(fid, name, len(data), data[:4])
    elif base in want:
        print('=====', name, len(data))
        # shift-jis strings (length-prefixed in MTD) and floats/ints after param names
        for m in re.finditer(rb'[\x20-\x7e\x80-\xfc]{3,}', data):
            print('  %04x %s' % (m.start(), m.group().decode('shift_jis', 'replace')))
