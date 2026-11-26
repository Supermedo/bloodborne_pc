# SPDX-License-Identifier: GPL-2.0-or-later
"""Offline Chalice Glyphs (docs/LOCAL_CHALICE.md): the shipped catalog and the mod's own sentences."""
import json
import re
import struct
import unittest

from paths import ROOT

CATALOG = ROOT / 'data' / 'local_chalice' / 'catalog.bin'
MESSAGES = ROOT / 'tools' / 'local_chalice' / 'messages.json'
KINDS = range(1, 10)  # lc_desc.h Kind


def read_catalog(data):
    """(records, items, probes, languages) of a LCCAT005 catalog (tools/local_chalice/make_catalog_v4.py)."""
    assert data[:8] == b'LCCAT005', data[:8]
    p = 8
    count, = struct.unpack_from('<I', data, p); p += 4
    records = []
    for _ in range(count):
        glyph = data[p:p + 8].decode('ascii')
        form = struct.unpack_from('<31I', data, p + 16)
        first, n = struct.unpack_from('<II', data, p + 156)
        records.append((glyph, form, first, n))
        p += 164
    total, = struct.unpack_from('<I', data, p); p += 4
    items = [struct.unpack_from('<4B2H2I', data, p + 16 * i) for i in range(total)]
    p += 16 * total
    probe_count, = struct.unpack_from('<I', data, p); p += 4
    probes = [struct.unpack_from('<II', data, p + 8 * i) for i in range(probe_count)]
    p += 8 * probe_count
    language_count, = struct.unpack_from('<I', data, p); p += 4

    def text():
        nonlocal p
        n, = struct.unpack_from('<H', data, p)
        s = data[p + 2:p + 2 + n].decode('utf-8')
        p += 2 + n
        return s
    languages = []
    for _ in range(language_count):
        probe, = struct.unpack_from('<I', data, p); p += 4
        code, comma, farm, rare, glitch, false_depth = (text() for _ in range(6))
        n, = struct.unpack_from('<H', data, p); p += 2
        messages = [text() for _ in range(n)]
        languages.append((probe, code, comma, (farm, rare, glitch, false_depth), messages))
    assert p == len(data), (p, len(data))
    return records, items, probes, languages


class CatalogTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.records, cls.items, cls.probes, cls.languages = read_catalog(CATALOG.read_bytes())

    def test_glyphs(self):
        glyphs = [r[0] for r in self.records]
        self.assertEqual(len(glyphs), 2351)
        self.assertEqual(glyphs, sorted(set(glyphs)), 'sorted, no duplicates (the mod binary-searches it)')
        for glyph, form, _, _ in self.records:
            self.assertRegex(glyph, r'^[0-9a-z]{8}$')
            self.assertEqual(form[:2], (4, 0x7C), glyph)          # FormData version 4, 124 bytes
            self.assertEqual(form[2] >> 24, 0x1D, glyph)           # a chalice dungeon map (m29)

    def test_descriptions(self):
        for glyph, _, first, n in self.records:
            self.assertLessEqual(first + n, len(self.items), glyph)
            self.assertLessEqual(n, 64, glyph)
        for kind, layer, _, _, _, _, _, _ in self.items:
            self.assertIn(kind, KINDS)
            self.assertLessEqual(layer, 4)

    def test_languages(self):
        codes = [lang[1] for lang in self.languages]
        self.assertEqual(codes[0], 'engus', 'English first: the fallback')
        self.assertEqual(len(codes), 21)
        english = self.languages[0][4]
        for probe, code, _, own, messages in self.languages:
            self.assertTrue(all(own), code)
            self.assertEqual(len(messages), len(english), code)
            for en, tr in zip(english, messages):
                self.assertEqual(sorted(re.findall(r'\{[a-z]\}', en)), sorted(re.findall(r'\{[a-z]\}', tr)), code)
        # Languages with different words of our own must be told apart by the probe hash.
        by_probe = {}
        for probe, code, _, own, _ in self.languages:
            by_probe.setdefault(probe, set()).add(own)
        self.assertTrue(all(len(v) == 1 for v in by_probe.values()))

    def test_messages_source(self):
        source = json.loads(MESSAGES.read_text(encoding='utf-8'))
        for code, messages in source['text'].items():
            self.assertEqual(len(messages), len(source['order']), code)


if __name__ == '__main__':
    unittest.main()
