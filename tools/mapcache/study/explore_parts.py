"""Hex of part entity data / type data for chosen part types of one MSB."""
import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
import mapcache as mc  # noqa: E402

dv, mp, types, n = sys.argv[1], sys.argv[2], [int(x) for x in sys.argv[3].split(',')], int(sys.argv[4])
b = mc.dcx_read(os.path.join(dv, 'map', 'mapstudio', mp + '.msb.dcx'))
for pname, ver, offs in mc._msb_params(b):
    if pname != 'PARTS_PARAM_ST':
        continue
    shown = 0
    for i, e in enumerate(offs):
        typ, = struct.unpack_from('<i', b, e + 0x14)
        if typ not in types:
            continue
        name = mc._wstr(b, e + struct.unpack_from('<q', b, e + 8)[0])
        hdr = struct.unpack_from('<6q', b, e + 0xA8)
        ent_off, typ_off = struct.unpack_from('<qq', b, e + 0xB0)
        pos = struct.unpack_from('<3f', b, e + 0x28)
        print(name, typ, 'pos %.1f %.1f %.1f' % pos, 'offs', [hex(x) for x in hdr])
        print('  ent  ', b[e + ent_off:e + ent_off + 0x20].hex(' ', 4))
        print('  type ', b[e + typ_off:e + typ_off + 0x40].hex(' ', 4))
        shown += 1
        if shown >= n:
            break
