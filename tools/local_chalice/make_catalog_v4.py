"""Catalog v4: the v3 records with each glyph's description as a recipe of game-text references.

Usage: make_catalog_v4.py <msg dir (all languages)> <catalog.bin v3> <out catalog.bin v4> [<out test fixture>]

The DLL composes each description at run time in the game's language, from the game's own loaded
text (FMG, id), exactly as short_desc.py does (it shares short_desc.py's analysis). Only four words
of our own (Farm, Rare layout, Glitch, False depth) and the list comma are stored, per language;
the language is recognised from a hash of a few game texts.

"LCCAT005", u32 count, count records (164 bytes): glyph[8], channel u64, FormData v4 (31 u32),
  creator name (4 u32), first item u32, item count u32;
u32 item total, items (16 bytes): kind u8, layer u8 (0 = none), priority u8, order u8, fmg u16,
  extra u16 (tier / gem shape id in FMG 200), id u32, value u32 (gem percent x 100);
u32 probe count, probes (fmg u32, id u32); u32 language count, languages:
  probe hash u32 (FNV-1a 32 over the UTF-16 code units of the probe texts, concatenated),
  6 strings (u16 byte length + UTF-8): code, list comma, Farm, Rare layout, Glitch, False depth,
  u16 message count, the messages (messages.json order: the mod's own sentences, {g} glyph, {c} char).
Kinds: 1 farm (fmg/id: subject or 0), 2 false depth, 3 rune, 4 gem (id: effect line in FMG 35, 0 =
unnamed), 5 weapon, 6 game-word tag, 7 own-word tag (extra: 1 rare layout, 2 glitch), 8 boss, 9 chalice.

The fixture (for lc_desc_test): every text the recipes and probes need, per language, and the
expected description of every glyph in every language (what short_desc.py produces).
"""
import struct, sys
from pathlib import Path

MSG, V3, OUT = sys.argv[1], sys.argv[2], Path(sys.argv[3])
FIXTURE = Path(sys.argv[4]) if len(sys.argv) > 4 else None
here = Path(__file__).parent
sys.path.insert(0, str(here))
sys.argv = ['short_desc.py', MSG]
src = (here / 'short_desc.py').read_text(encoding='utf-8')
exec(compile(src[:src.index('data = Path(sys.argv[2]).read_bytes()')], 'short_desc.py', 'exec'))

KIND = {'farm': 1, 'false': 2, 'rune': 3, 'gem': 4, 'weapon': 5, 'gtag': 6, 'otag': 7, 'boss': 8, 'chalice': 9}
OWN_TAG = {'rare': 1, 'glitch': 2}
GEM_WORD = (2, 34004)
SEP_PROBE = (35, 23105)
PROBES = [(10, None), (200, 14203), (35, 26105), (2, 34004)]  # (10, chalice id) filled below

# FMG ids are one table in the game (item and menu archives do not share ids).
table('engus', 'item', 10); table('engus', 'menu', 2)
item_ids = set(tables[('engus', 'item')].keys())
menu_ids = set(tables[('engus', 'menu')].keys())
assert not item_ids & menu_ids, item_ids & menu_ids
ARC = dict({f: 'item' for f in item_ids}, **{str(f): 'menu' for f in menu_ids})
ARC = {int(k): v for k, v in ARC.items()}

def ref_of(arc_fid):
    arc, fid, i = arc_fid
    return fid, i

def encode(a):
    layer, prio, order, kind, p = a
    fmg = extra = ident = value = 0
    if kind == 'farm':
        k = 'farm'
        if p:
            fmg, ident = ref_of(GAME_WORD[p])
    elif kind == 'false':
        k = 'false'
    elif kind in ('rune', 'weapon'):
        k = kind
        (_, arc, fid, i), _, tier = p
        fmg, ident, extra = fid, i, int(tier or 0)
    elif kind == 'gem':
        k = 'gem'
        _, shape, eff, val = p
        fmg, ident = 35, eff or 0
        extra = GEM_SHAPE[shape] if shape else 0
        value = round(val * 100)
        assert abs(val * 100 - value) < 1e-6, val
    elif kind == 'tag':
        if p in OWN_TAG:
            k, extra = 'otag', OWN_TAG[p]
        else:
            k = 'gtag'
            fmg, ident = ref_of(GAME_WORD[p])
    elif kind == 'boss':
        k = 'boss'
        fmg, ident = p[2], p[3]
    elif kind == 'chalice':
        k = 'chalice'
        fmg, ident = 10, p
    assert 0 <= order < 256 and 0 <= (layer and int(layer) or 0) <= 4 and extra < 65536
    return struct.pack('<4B2H2I', KIND[k], int(layer) if layer else 0, prio, order, fmg, extra, ident, value), \
        [(fmg, ident)] if fmg and ident else []

data = Path(V3).read_bytes()
assert data[:8] == b'LCCAT003'
count = struct.unpack_from('<I', data, 8)[0]
blob = data[12 + 164 * count:]
records, items_bin, needed, expected = [], [], set(), []
first_chalice = None
for n in range(count):
    r = data[12 + 164 * n: 12 + 164 * (n + 1)]
    glyph = r[:8].decode()
    chalice = struct.unpack_from('<31I', r, 16)[3]
    first_chalice = first_chalice or chalice
    off, ln = struct.unpack_from('<II', r, 156)
    items = atoms(analyse(blob[off:off + ln].decode('utf-8'), chalice))
    start = len(items_bin)
    for a in items:
        b, refs = encode(a)
        items_bin.append(b)
        needed.update(refs)
        if a[3] == 'gem':
            needed.add(GEM_WORD if not a[4][2] else (35, a[4][2]))
            if a[4][1]:
                needed.add((200, GEM_SHAPE[a[4][1]]))
    records.append(r[:156] + struct.pack('<II', start, len(items)))
    expected.append((glyph, {lang: fit(items, lang)[0] for lang in LANGS}))

PROBES[0] = (10, first_chalice)
needed |= set(PROBES) | {SEP_PROBE, GEM_WORD}

def utf16_units(s):
    b = s.encode('utf-16-le')
    return struct.unpack(f'<{len(b) // 2}H', b)

def fnv(units):
    h = 0x811c9dc5
    for u in units:
        h = ((h ^ u) * 0x01000193) & 0xffffffff
    return h

def probe_hash(lang):
    return fnv([u for f, i in PROBES for u in utf16_units(text(lang, ARC[f], f, i))])

hashes = {lang: probe_hash(lang) for lang in LANGS}
own_set = {lang: tuple(OWN[k][lang] for k in ('farm', 'rare', 'glitch', 'false')) for lang in LANGS}
for a in LANGS:
    for b in LANGS:
        assert a == b or hashes[a] != hashes[b] or own_set[a] == own_set[b], (a, b)

def s16(s):
    b = s.encode('utf-8')
    return struct.pack('<H', len(b)) + b

import json, re
M = json.loads((here / 'messages.json').read_text(encoding='utf-8'))
def messages(lang):
    return M['text'][M['same_as'].get(lang, lang)]
for lang in LANGS:
    ms = messages(lang)
    assert len(ms) == len(M['order']), lang
    for en, tr, name in zip(messages('engus'), ms, M['order']):
        assert sorted(re.findall(r'\{[a-z]\}', en)) == sorted(re.findall(r'\{[a-z]\}', tr)), (lang, name)
        assert tr.count(chr(10)) == en.count(chr(10)), (lang, name)

langs_bin = b''
for lang in LANGS:  # English (US) first: the fallback
    comma = '、' if lang in ('jpnjp', 'zhocn', 'zhotw') else ', '
    langs_bin += struct.pack('<I', hashes[lang]) + b''.join(s16(x) for x in (lang, comma) + own_set[lang])
    langs_bin += struct.pack('<H', len(M['order'])) + b''.join(s16(x) for x in messages(lang))
out = (b'LCCAT005' + struct.pack('<I', count) + b''.join(records) +
       struct.pack('<I', len(items_bin)) + b''.join(items_bin) +
       struct.pack('<I', len(PROBES)) + b''.join(struct.pack('<II', f, i) for f, i in PROBES) +
       struct.pack('<I', len(LANGS)) + langs_bin)
OUT.write_bytes(out)
print(f'{OUT}: {count} glyphs, {len(items_bin)} items, {len(needed)} game texts referenced, {len(out)} bytes')

if FIXTURE:
    # "LCFIX001", u32 language count; per language: code (s16), u32 n, n x (fmg u32, id u32, s16 text);
    # then u32 glyph count, per glyph: glyph[8], per language s16 expected text.
    fx = b'LCFIX001' + struct.pack('<I', len(LANGS))
    for lang in LANGS:
        rows = [(f, i, text(lang, ARC[f], f, i)) for f, i in sorted(needed)]
        rows = [r for r in rows if r[2]]
        fx += s16(lang) + struct.pack('<I', len(rows)) + b''.join(struct.pack('<II', f, i) + s16(t) for f, i, t in rows)
    fx += struct.pack('<I', len(expected))
    for glyph, by_lang in expected:
        fx += glyph.encode() + b''.join(s16(by_lang[lang]) for lang in LANGS)
    FIXTURE.write_bytes(fx)
    print(f'{FIXTURE}: {len(fx)} bytes')
