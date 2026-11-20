"""Dump param/drawparam/default_drawparam.parambnd.dcx -> default_LodBank.param rows (raw floats).

PARAM (as found): header with row count u16 at 0x0A; 64-bit row table at 0x40: (id i32, pad, data off i64, name off i64).
Row data size = distance between data offsets. Fields printed as float32 and int32.
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import flstat  # noqa: E402


def lodbank(dv):
    fs = flstat.bnd4_files(flstat.mc.dcx_read(os.path.join(dv, 'param', 'drawparam', 'default_drawparam.parambnd.dcx')))
    b = fs[0][2]
    return b


if __name__ == '__main__':
    b = lodbank(sys.argv[1])
    print(b[:0x40].hex(' ', 4))
    cnt, = struct.unpack_from('<H', b, 0x0A)
    rows = [struct.unpack_from('<iiqq', b, 0x40 + 0x18 * i) for i in range(cnt)]
    offs = [r[2] for r in rows]
    size = (offs[1] - offs[0]) if cnt > 1 else 0
    print('rows', cnt, 'row size', size, 'type', b[0x0C:0x2C])
    for rid, _p, doff, noff in rows:
        nm = flstat.mc._wstr(b, noff) if noff else ''
        d = b[doff:doff + size]
        fl = struct.unpack_from('<4f', d)
        print(rid, nm, ' '.join('%g' % x for x in fl), d[16:].hex())
