"""Read-only lookup of texts in a Bloodborne *.msgbnd.dcx (DCX DFLT -> BND4 -> FMG v2)."""
import struct, sys, zlib
from pathlib import Path


def dcx(data):
    assert data[:4] == b'DCX\0'
    p = data.find(b'DCA\0')
    hdr = struct.unpack_from('>I', data, p + 4)[0]
    return zlib.decompress(data[p + hdr:])


def bnd4(data):
    assert data[:4] == b'BND4'
    count = struct.unpack_from('<I', data, 0x0c)[0]
    entry_size = struct.unpack_from('<q', data, 0x20)[0]
    unicode = data[0x30]
    fmt = data[0x31]
    out = {}
    pos = 0x40
    for _ in range(count):
        flags = data[pos]
        size, = struct.unpack_from('<q', data, pos + 8)
        q = pos + 0x10
        if fmt & 0x20 or fmt & 0x04:  # uncompressed size present
            q += 8
        off, = struct.unpack_from('<I', data, q); q += 4
        fid, = struct.unpack_from('<i', data, q); q += 4
        noff, = struct.unpack_from('<I', data, q)
        name = data[noff:data.index(b'\0\0', noff) + 1].decode('utf-16le', 'replace') if unicode else ''
        out[fid] = (name, data[off:off + size])
        pos += entry_size
    return out


def fmg(data):
    groups, strings = struct.unpack_from('<ii', data, 0x0c)
    soff, = struct.unpack_from('<q', data, 0x18)
    table = {}
    for g in range(groups):
        idx, first, last = struct.unpack_from('<iii', data, 0x28 + g * 16)
        for i in range(last - first + 1):
            ptr, = struct.unpack_from('<q', data, soff + (idx + i) * 8)
            if ptr:
                end = ptr
                while data[end:end + 2] != b'\0\0':
                    end += 2
                table[first + i] = data[ptr:end].decode('utf-16le', 'replace')
    return table


if __name__ == '__main__':
    files = bnd4(dcx(Path(sys.argv[1]).read_bytes()))
    for spec in sys.argv[2:]:
        f, i = map(int, spec.split(':'))
        name, blob = files[f]
        print(f, i, name.rsplit('\\', 1)[-1], repr(fmg(blob).get(i)))
