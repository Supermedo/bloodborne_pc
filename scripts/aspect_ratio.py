"""Fingerprint loaded eboot segments, independent of ELF/SELF container padding."""
import hashlib
import struct

EBOOT_109_SEGMENTS = 'cbe97cb9e7b830195c93e7ed139cbaaeea66302da82f1b39ca31f90a3ceccc52'

def segment_fingerprint(elf):
    phoff, = struct.unpack_from('<Q', elf, 32)
    count, = struct.unpack_from('<H', elf, 56)
    digest = hashlib.sha256()
    for i in range(count):
        kind, flags, offset, address, physical, filesz, memsz, align = struct.unpack_from(
            '<IIQQQQQQ', elf, phoff + i * 56)
        if kind == 1:
            if offset + filesz > len(elf):
                raise ValueError('Truncated eboot load segment')
            digest.update(struct.pack('<QQQ', address, filesz, memsz))
            digest.update(elf[offset:offset + filesz])
    return digest.hexdigest()
