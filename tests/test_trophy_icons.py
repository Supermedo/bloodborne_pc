"""Archive bounds, refreshes and actual Tk image/fallback rendering."""
from paths import ROOT
import importlib.util
from io import BytesIO
from pathlib import Path
import struct
import sys
import tempfile
from types import SimpleNamespace
import unittest

sys.path.insert(0, str(ROOT / 'launcher'))
from bbport_trophy_icons import TrophyIcons


def archive(path, entries):
    offset = 96 + len(entries) * 64
    table, payload = bytearray(), bytearray()
    for name, image, flags in entries:
        table.extend(struct.pack('>32sQQI12x', name, offset, len(image), flags))
        payload.extend(image)
        offset += len(image)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(struct.pack('>IIQII72x', 0xDCA24D00, 3, offset, len(entries), 64) + table + payload)


class TrophyArchiveTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.icons = TrophyIcons(self.temp.name)

    def test_missing_then_installed_and_replaced_archive(self):
        self.assertIsNone(self.icons.get(0))
        first = b'\x89PNG\r\n\x1a\nfirst'
        archive(self.icons.path, [(b'TROP000.PNG', first, 0)])
        self.assertEqual(self.icons.get(0), first)
        self.assertIsNone(self.icons.get(1))
        second = first + b'replaced'
        archive(self.icons.path, [(b'TROP000.PNG', second, 0)])
        self.assertEqual(self.icons.get(0), second)
        self.icons.path.unlink()
        self.assertIsNone(self.icons.get(0))

    def test_encrypted_non_png_and_out_of_bounds_entries_are_ignored(self):
        png = b'\x89PNG\r\n\x1a\nimage'
        archive(self.icons.path, [(b'TROP000.PNG', png, 3), (b'TROP001.PNG', b'not png', 0),
                                  (b'TROP002.PNG', png, 0)])
        data = bytearray(self.icons.path.read_bytes())
        struct.pack_into('>Q', data, 96 + 2 * 64 + 32, len(data) + 1)
        self.icons.path.write_bytes(data)
        for trophy in range(3):
            self.assertIsNone(self.icons.get(trophy))

    def test_truncated_header_and_invalid_table(self):
        self.icons.path.parent.mkdir(parents=True)
        self.icons.path.write_bytes(b'broken')
        self.assertIsNone(self.icons.get(0))
        self.icons.path.write_bytes(struct.pack('>IIQII72x', 0xDCA24D00, 3, 96, 4097, 64))
        self.assertIsNone(self.icons.get(0))


class TrophyIconRenderingTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        try:
            import tkinter as tk
            from PIL import Image, ImageTk
        except ImportError as error:
            raise unittest.SkipTest(str(error))
        try:
            cls.root = tk.Tk()
        except tk.TclError as error:
            raise unittest.SkipTest(str(error))
        cls.root.withdraw()
        cls.tk, cls.Image, cls.ImageTk = tk, Image, ImageTk
        spec = importlib.util.spec_from_file_location('icon_launcher', ROOT / 'launcher/bbport_launcher_win.py')
        cls.launcher = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(cls.launcher)

    @classmethod
    def tearDownClass(cls):
        cls.root.destroy()

    def test_game_language_menu_preserves_chinese_selection(self):
        from tkinter import ttk
        previous = self.launcher.LANG
        self.addCleanup(setattr, self.launcher, 'LANG', previous)
        self.launcher.LANG = 'zh'
        app = object.__new__(self.launcher.Launcher)
        app.tk, app.ttk = self.tk, ttk
        app.vars = {}
        app.app = {'language': '11'}
        box = app.choice(self.root, 'language', 'app', self.launcher.LANGUAGES)
        self.assertIn('简体中文', box['values'])
        self.assertIn('繁體中文', box['values'])
        self.assertIn('韩语', box['values'])
        self.assertEqual(app.vars['language'].get(), '11')
        self.assertEqual(box.get(), '简体中文')
        app.vars['language'].set('10')
        self.assertEqual(box.get(), '繁體中文')
        box.destroy()

    def test_artwork_size_dimming_cache_and_game_folder_change(self):
        with tempfile.TemporaryDirectory() as folder:
            stream = BytesIO()
            self.Image.new('RGBA', (240, 240), (200, 100, 50, 255)).save(stream, format='PNG')
            archive(TrophyIcons(folder).path, [(b'TROP014.PNG', stream.getvalue(), 0)])
            app = object.__new__(self.launcher.Launcher)
            app.tk = self.tk
            selected = [folder]
            app.var = lambda *args: SimpleNamespace(get=lambda: selected[0])
            options = (self.root, 'bronze', True, 46, '#171717')
            earned = app.trophy_icon(*options, trophy_id=14)
            locked = app.trophy_icon(self.root, 'bronze', False, 46, '#171717', trophy_id=14)
            toast = app.trophy_icon(self.root, 'bronze', True, 38, '#171717', trophy_id=14)
            again = app.trophy_icon(*options, trophy_id=14)
            self.assertEqual((earned.image.width(), earned.image.height()), (46, 46))
            self.assertEqual(toast.image.width(), 38)
            self.assertIs(again.image, earned.image)
            self.assertEqual(self.ImageTk.getimage(earned.image).getpixel((20, 20)), (200, 100, 50, 255))
            self.assertEqual(self.ImageTk.getimage(locked.image).getpixel((20, 20)), (80, 40, 20, 255))
            for trophy in (0, 999, None):
                widget = app.trophy_icon(*options, trophy_id=trophy)
                self.assertEqual(widget.winfo_class(), 'Canvas')
                widget.destroy()
            archive(TrophyIcons(folder).path, [(b'TROP014.PNG', b'\x89PNG\r\n\x1a\ninvalid', 0)])
            damaged = app.trophy_icon(*options, trophy_id=14)
            self.assertEqual(damaged.winfo_class(), 'Canvas')
            damaged.destroy()
            selected[0] = str(Path(folder) / 'another-game')
            missing = app.trophy_icon(*options, trophy_id=14)
            self.assertEqual(missing.winfo_class(), 'Canvas')
            for widget in (earned, locked, toast, again, missing):
                widget.destroy()
