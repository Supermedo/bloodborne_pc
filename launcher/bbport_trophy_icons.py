# SPDX-License-Identifier: GPL-2.0-or-later
"""Read the game's unencrypted trophy PNGs in place; no extraction or network access."""
from pathlib import Path
import struct


class TrophyIcons:
    def __init__(self, game_dir):
        self.path = Path(game_dir) / 'sce_sys' / 'trophy' / 'trophy00.trp'
        self.signature = None
        self.entries = {}
        self.images = {}

    def get(self, trophy_id):
        try:
            stat = self.path.stat()
            signature = (stat.st_mtime_ns, stat.st_size)
            if signature != self.signature:
                self.signature = signature
                self.entries = {}
                self.images = {}
                with self.path.open('rb') as archive:
                    header = archive.read(96)
                    if len(header) != 96:
                        return None
                    magic, version, length, count, stride = struct.unpack('>IIQII', header[:24])
                    if (magic != 0xDCA24D00 or version != 3 or length != stat.st_size
                            or stride != 64 or count > 4096 or 96 + count * stride > length):
                        return None
                    data_start = 96 + count * stride
                    for _ in range(count):
                        entry = archive.read(stride)
                        if len(entry) != stride:
                            return None
                        name = entry[:32].split(b'\0', 1)[0]
                        offset, size, flags = struct.unpack('>QQI', entry[32:52])
                        if (flags == 0 and data_start <= offset <= length
                                and 0 < size <= min(length - offset, 4 * 1024 * 1024)):
                            self.entries[name] = (offset, size)
            name = f'TROP{trophy_id:03d}.PNG'.encode('ascii')
            if name not in self.images:
                if name not in self.entries:
                    return None
                offset, size = self.entries[name]
                with self.path.open('rb') as archive:
                    archive.seek(offset)
                    image = archive.read(size)
                self.images[name] = image if len(image) == size and image.startswith(b'\x89PNG\r\n\x1a\n') else None
            return self.images[name]
        except (OSError, ValueError, struct.error):
            self.signature = None
            self.entries = {}
            self.images = {}
            return None
