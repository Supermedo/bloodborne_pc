# SPDX-License-Identifier: GPL-3.0-or-later
"""Check native storage metadata and shared messages without loading the UI."""
import ast
import importlib.util
from pathlib import Path
import re
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'launcher'))
import bbport_lang_motion as catalog


class StorageGuard(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        source = ROOT / 'launcher/bbport_launcher_win.py'
        tree = ast.parse(source.read_text(encoding='utf-8'))
        node = next(n for n in tree.body if isinstance(n, ast.FunctionDef)
                    and n.name == 'object_motion_unavailable')
        scope = {'re': re, '_': lambda english, *args: english}
        exec(compile(ast.Module(body=[node], type_ignores=[]), str(source), 'exec'), scope)
        cls.check = staticmethod(scope['object_motion_unavailable'])

    def test_rx6000_native_ssbo(self):
        self.assertEqual(self.check(dict(vendor_id=0x1002, device_id=0x73bf,
                                        object_motion_ssbo=True),
                                    'RX6800XT, discrete, 16384 MiB'), '')

    def test_unknown_or_old_probe_keeps_guard(self):
        for flag in (None, False, 'true'):
            with self.subTest(flag=flag):
                self.assertEqual(self.check(dict(vendor_id=0x1002, device_id=0x73bf,
                                                 object_motion_ssbo=flag),
                                            'RX6800XT, discrete, 16384 MiB'), catalog.KEYS['Gpu'])

    def test_low_vram_guard_retained(self):
        self.assertEqual(self.check(dict(vendor_id=0x1002, device_id=0x743f,
                                        object_motion_ssbo=True),
                                    'RX6500XT, discrete, 4096 MiB'), catalog.KEYS['Vram'])

    def test_other_gpus_unchanged(self):
        for vendor, device in ((0x1002, 0x744c), (0x1002, 0x7550),
                               (0x10de, 0x2684), (0x8086, 0x56a0)):
            with self.subTest(vendor=vendor, device=device):
                self.assertEqual(self.check(dict(vendor_id=vendor, device_id=device),
                                            'GPU, discrete, 16384 MiB'), '')
        self.assertEqual(self.check(dict(vendor_id=0x1002, device_id=0x731f),
                                    'RX5700, discrete, 8192 MiB'), catalog.KEYS['Gpu'])

    def test_explicit_override_unchanged(self):
        self.assertEqual(self.check(dict(vendor_id=0x1002, device_id=0x731f),
                                    'GPU, discrete, 4096 MiB', 'BB_OBJECT_MOTION=1'), '')

    def test_overlay_catalog_matches_launcher_in_all_languages(self):
        spec = importlib.util.spec_from_file_location('motion_ui_texts', ROOT / 'tools/motion_ui_texts.py')
        generator = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(generator)
        self.assertEqual(len(catalog.TEXTS), 13)
        self.assertTrue(all(row.keys() == catalog.KEYS.keys() and all(row.values())
                            for row in catalog.TEXTS.values()))
        self.assertEqual((ROOT / 'gpu/shim/bbport_motion_ui.h').read_text(encoding='utf-8'),
                         generator.generate())

    def generator(self):
        spec = importlib.util.spec_from_file_location('motion_ui_texts', ROOT / 'tools/motion_ui_texts.py')
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        return module

    def test_reason_mapping_survives_dictionary_reordering(self):
        generator = self.generator()
        expected = generator.generate()
        reordered = {code: dict(reversed(list(row.items()))) for code, row in catalog.TEXTS.items()}
        with mock.patch.object(catalog, 'TEXTS', reordered), \
                mock.patch.object(catalog, 'KEYS', dict(reversed(list(catalog.KEYS.items())))):
            self.assertEqual(generator.generate(), expected)

    def test_language_count_is_derived_and_english_remains_default(self):
        generator = self.generator()
        rows = dict(reversed(list(catalog.TEXTS.items())))
        rows['xx'] = dict(catalog.KEYS)
        with mock.patch.object(catalog, 'TEXTS', rows):
            result = generator.generate()
        self.assertIn('languages[] = {"en",', result)
        self.assertIn('"xx"', result)
        self.assertNotIn('texts[13]', result)
        self.assertIn('i < LanguageCount', result)

    def test_missing_reason_is_rejected(self):
        rows = {code: dict(row) for code, row in catalog.TEXTS.items()}
        del rows['de']['Limits']
        with mock.patch.object(catalog, 'TEXTS', rows), self.assertRaises(KeyError):
            self.generator().generate()

    def test_generated_file_uses_lf_on_windows_too(self):
        generator = self.generator()
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            target = root / 'gpu/shim/bbport_motion_ui.h'
            target.parent.mkdir(parents=True)
            with mock.patch.object(generator, 'ROOT', root), mock.patch.object(sys, 'argv', ['generator']):
                generator.main()
            self.assertEqual(target.read_bytes(), generator.generate().encode('utf-8'))
            self.assertNotIn(b'\r\n', target.read_bytes())

    def test_help_and_reasons_cover_the_same_languages(self):
        import bbport_lang
        self.assertEqual(catalog.HELP_TEXTS.keys(), catalog.TEXTS.keys())
        for code, row in catalog.HELP_TEXTS.items():
            with self.subTest(language=code):
                self.assertEqual(row.keys(), catalog.HELP_KEYS.keys())
                self.assertTrue(all(row.values()))
                self.assertIn('7', row['Intro'].format(7))
                for key, english in catalog.HELP_KEYS.items():
                    self.assertEqual(bbport_lang.table(code).get(english), row[key])


if __name__ == '__main__':
    unittest.main()
