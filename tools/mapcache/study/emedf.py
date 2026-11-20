"""Instruction names from event/common.emedf.dcx (EDF\\0): {(bank, id): (class name, instruction name)}.

Layout as found: header longs at 0x10 = (class count, class offset, instr count, instr offset, arg count,
arg offset, ...), string table offset at 0x98. Class entry 0x20 = (id, instr count, instr byte offset,
name byte offset); instruction entry 0x20 = (id, arg count, arg offset, name byte offset); names UTF-16.
"""
import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
import mapcache as mc  # noqa: E402


def names(dvdroot):
    b = mc.dcx_read(os.path.join(dvdroot, 'event', 'common.emedf.dcx'))
    ccnt, coff, _icnt, ioff = struct.unpack_from('<4q', b, 0x10)
    soff, = struct.unpack_from('<q', b, 0x98)
    out = {}
    for c in range(ccnt):
        cid, n, io, no = struct.unpack_from('<4q', b, coff + 0x20 * c)
        cname = mc._wstr(b, soff + no)
        for k in range(n):
            iid, _ac, _ao, ino = struct.unpack_from('<4q', b, ioff + io + 0x20 * k)
            out[(cid, iid)] = (cname, mc._wstr(b, soff + ino))
    return out


if __name__ == '__main__':
    t = names(sys.argv[1])
    sys.stdout.reconfigure(encoding='utf-8')
    for k in sys.argv[2:]:
        bank, iid = k.split(':')
        print(k, t.get((int(bank), int(iid))))
    if len(sys.argv) == 2:
        for k, v in sorted(t.items()):
            print('%d[%02d]\t%s\t%s' % (k[0], k[1], v[0], v[1]))
