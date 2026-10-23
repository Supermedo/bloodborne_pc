# SPDX-License-Identifier: GPL-3.0-or-later
"""Compact picker interactions and the translation-import regression."""
from pathlib import Path
import subprocess
import sys
import unittest
from unittest.mock import Mock

ROOT = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT/'launcher'), str(ROOT/'scripts')]
import bbport_launcher_win as launcher
import bbport_lang
import bbport_fsr41 as model
import bbport_fsr41_ui as view


class CompactTests(unittest.TestCase):
    def test_provider_modules_do_not_cache_incomplete_language_tables(self):
        script = """
import sys
sys.path[:0] = sys.argv[1:]
import bbport_fsr41_ui
import bbport_lang
assert not bbport_lang._tables
import bbport_launcher_win
table = bbport_lang.table('de')
for key, value in {'Picture & performance':'Bild & Leistung', 'All graphics':'Grafik (alle)',
                   'Your goal':'Dein Ziel', 'Search…':'Suchen…', 'Details':'Details',
                   'Support':'Unterstützung'}.items():
    assert table[key] == value, (key, table.get(key))
"""
        result = subprocess.run([sys.executable, '-B', '-X', 'utf8', '-c', script,
            str(ROOT/'launcher'), str(ROOT/'scripts')], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)

    @unittest.skipUnless(sys.platform == 'win32', 'Windows picker')
    def test_help_and_diagnostics_preserve_selection_in_all_languages(self):
        import tkinter as tk
        from tkinter import ttk
        root = tk.Tk(); root.withdraw()
        app = launcher.Launcher.__new__(launcher.Launcher)
        app.root, app.tk, app.ttk, app.dpi = root, tk, ttk, 1
        app.app, app.ini, app.vars = dict(launcher.APP_DEFAULTS), dict(launcher.INI_DEFAULTS), {}
        app.ini.update(upscaler='fsr412', fsr411_backend='vk', frame_gen='fsr402')
        app.fsr411_refreshers = []; app.process = None
        app.recheck_fsr411 = Mock(); app.download_fsr411 = Mock()
        ready = dict(supported=True, reason='ready')
        app.fsr411_support = dict(vendor_id=0x1002, dx12_interop=ready,
            fsr412_vulkan=ready, fsr412_dx12=dict(supported=False, reason='files', detail='driver.dll'))
        app.fsr411_status = lambda method: dict(supported=False, reason='driver' if method=='dx12' else 'hardware',
                                               detail='provider unavailable' if method=='dx12' else 'AMD GPU required')
        try:
            for language in model.TEXTS:
                table = bbport_lang.table(language)
                translate = lambda key: table.get(key, key)
                before = dict(app.ini)
                window = view.open_settings(app, translate); root.update()
                self.assertEqual(window.title(), '')
                self.assertEqual(len(window.cards), 4)
                self.assertEqual(len(window.hints), 4)
                for index, ((_, radio, status), hint) in enumerate(zip(window.cards, window.hints)):
                    self.assertEqual(radio.instate(['disabled']), index != 0)
                    self.assertEqual(status.cget('text'), translate(model.STATUS_KEYS[(0, 4, 3, 1)[index]]))
                    self.assertEqual(hint.text, translate(model.HINTS[index]))
                    hint.widget.invoke()
                    self.assertTrue(hint.window.winfo_exists())
                    hint.hide()
                self.assertEqual(app.ini, before)
                self.assertEqual(app.var('upscaler', 'ini').get(), 'fsr412')
                self.assertEqual(app.var('fsr411_backend', 'ini').get(), 'vk')
                window.destroy(); root.update()
                self.assertFalse(app.fsr411_refreshers)
        finally:
            root.destroy()


if __name__ == '__main__': unittest.main()
