# SPDX-License-Identifier: GPL-3.0-or-later
"""Check native storage metadata and shared messages without loading the UI."""
import ast
import importlib.util
from pathlib import Path
import re
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'launcher'))
import bbport_lang_vram as catalog


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
                                            'RX6800XT, discrete, 16384 MiB'), catalog.KEYS[2])

    def test_low_vram_guard_retained(self):
        self.assertEqual(self.check(dict(vendor_id=0x1002, device_id=0x743f,
                                        object_motion_ssbo=True),
                                    'RX6500XT, discrete, 4096 MiB'), catalog.KEYS[3])

    def test_other_gpus_unchanged(self):
        for vendor, device in ((0x1002, 0x744c), (0x1002, 0x7550),
                               (0x10de, 0x2684), (0x8086, 0x56a0)):
            with self.subTest(vendor=vendor, device=device):
                self.assertEqual(self.check(dict(vendor_id=vendor, device_id=device),
                                            'GPU, discrete, 16384 MiB'), '')
        self.assertEqual(self.check(dict(vendor_id=0x1002, device_id=0x731f),
                                    'RX5700, discrete, 8192 MiB'), catalog.KEYS[2])

    def test_explicit_override_unchanged(self):
        self.assertEqual(self.check(dict(vendor_id=0x1002, device_id=0x731f),
                                    'GPU, discrete, 4096 MiB', 'BB_OBJECT_MOTION=1'), '')

    def test_overlay_catalog_matches_launcher_in_all_languages(self):
        spec = importlib.util.spec_from_file_location('motion_ui_texts', ROOT / 'tools/motion_ui_texts.py')
        generator = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(generator)
        self.assertEqual(len(catalog.TEXTS), 13)
        self.assertTrue(all(len(row) == len(catalog.KEYS) and all(row)
                            for row in catalog.TEXTS.values()))
        self.assertEqual((ROOT / 'gpu/shim/bbport_motion_ui.h').read_text(encoding='utf-8'),
                         generator.generate())


if __name__ == '__main__':
    unittest.main()
