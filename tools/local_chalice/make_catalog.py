"""Builds LocalChalice-Offline/catalog.bin from the v0.5 catalog.json (+ Noxde creator names).

Usage: make_catalog.py <v0.5 catalog.json> <Noxde dungeons.json> <out catalog.bin> [Reborn catalogue.json]

"LCCAT003", u32 count, then count records (164 bytes, little endian), sorted by glyph:
  glyph[8] ASCII, channel u64 (v0.5 local_chalice.py id()), FormData v4 (31 u32),
  creator name: record words 27..30 (4 u32, zero when unknown),
  description: u32 offset, u32 length (UTF-8, in the table after the records; 0/0 when none).
Descriptions come from Reborn's catalogue (first) or Noxde's records.
FormData words 22..25 are record words 23..26: the creator's online ID where the source has it.
Creator names exist only in Noxde's records (save-record words 0..30); a name is taken only when
Noxde's record is the same dungeon as the catalog entry (words 3..26 equal).
"""
import base64, hashlib, json, struct, sys
from pathlib import Path

catalog = json.loads(Path(sys.argv[1]).read_text(encoding='utf-8'))
noxde = json.loads(Path(sys.argv[2]).read_text(encoding='utf-8'))
out = Path(sys.argv[3])
reborn = json.loads(Path(sys.argv[4]).read_text(encoding='utf-8')) if len(sys.argv) > 4 else {}

names, descs = {}, {}
for rows in noxde.values():
    for row in rows:
        raw = bytes(int(x, 16) if isinstance(x, str) else x for x in row['bytes'])
        if row.get('desc', '').strip():
            descs.setdefault(row['glyph'].lower(), row['desc'].strip())
        if len(raw) != 124:
            continue
        words = struct.unpack('>31I', raw)  # save-record words 0..30
        names[row['glyph'].lower()] = (words[3:27], words[27:31])
for rows in reborn.values():
    for row in rows:
        if row.get('desc', '').strip():
            descs[row['glyph'].lower()] = row['desc'].strip()  # Reborn first

rows, named, with_id, described = [], 0, 0, 0
table = bytearray()
for glyph, form in sorted(catalog.items()):
    raw = base64.b64decode(form, validate=True)
    words = struct.unpack('<31I', raw)
    assert len(glyph) == 8 and glyph.isascii() and glyph == glyph.lower(), glyph
    assert words[:2] == (4, 0x7c) and words[2] >> 24 == 0x1d, glyph
    channel = (int.from_bytes(hashlib.sha256(form.encode('ascii')).digest()[:7], 'big') >> 4) or 1
    assert channel < 1 << 63
    name = (0, 0, 0, 0)
    if glyph in names and tuple(names[glyph][0]) == tuple(words[2:26]):
        name = tuple(names[glyph][1])
    named += any(name)
    with_id += any(words[22:26])
    text = descs.get(glyph, '').encode('utf-8')[:1000]
    while text and (text[-1] & 0xc0) == 0x80:  # never cut inside a character
        text = text[:-1]
    if text and text[-1] >= 0xc0:
        text = text[:-1]
    offset = len(table) if text else 0
    table += text
    described += bool(text)
    rows.append(glyph.encode('ascii') + struct.pack('<Q', channel) + raw + struct.pack('<4I', *name) +
                struct.pack('<II', offset, len(text)))
data = b'LCCAT003' + struct.pack('<I', len(rows)) + b''.join(rows) + bytes(table)
out.write_bytes(data)
print(described, 'with description;', len(table), 'bytes of descriptions')


def text(ws):
    return b''.join(struct.pack('<I', w) for w in ws).split(b'\0')[0].decode('latin1')


print(len(rows), 'entries;', named, 'with creator name;', with_id, 'with online ID;', out.stat().st_size, 'bytes')
for g in ('cummmfpk', 'wxncg3sg', '83ss4pyq'):
    r = next(r for r in rows if r[:8] == g.encode())
    form = struct.unpack_from('<31I', r, 16)
    off, length = struct.unpack_from('<II', r, 156)
    print(g, 'online ID', repr(text(form[22:26])), 'name', repr(text(struct.unpack_from('<4I', r, 140))),
          'desc', repr(bytes(table[off:off + length]).decode('utf-8')[:70]))
