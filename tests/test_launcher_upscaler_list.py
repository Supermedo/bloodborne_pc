# SPDX-License-Identifier: GPL-2.0-or-later
"""The upscaler and frame generation lists (R64): FSR 4 is named after the cards it is made for, DLSS
cannot be picked without a GeForce RTX (it became FSR 3.1 without a word) nor DLSS frame generation
without an RTX 40/50, each with the reason, and the upscaler the automatic settings pick is marked."""
from paths import ROOT
import functools
import os
import sys
import tempfile
import tkinter
import types
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(ROOT / 'launcher'))
sys.path.insert(0, str(ROOT / 'scripts'))
import bbport_fsr41_ui  # noqa: E402
import bbport_hardware as hardware  # noqa: E402
import bbport_lang  # noqa: E402
import bbport_launcher_win as launcher  # noqa: E402

RADEON = 'AMD Radeon RX 7800 XT, discrete, 16368 MiB'
ARC = 'Intel(R) Arc(TM) B580 Graphics, discrete, 12216 MiB'
RTX_3070 = 'NVIDIA GeForce RTX 3070, discrete, 8192 MiB'
RTX_4070 = 'NVIDIA GeForce RTX 4070, discrete, 12282 MiB'
GTX_1080 = 'NVIDIA GeForce GTX 1080, discrete, 8192 MiB'
RYZEN = 'AMD Ryzen 7 5800X 8-Core Processor'
NOTE = ' — not on this graphics card'


def card(line):
    name, _integrated, vram = hardware.parse_gpu_line(line)
    return name, hardware.match_gpu(name, vram)


class UnavailableTests(unittest.TestCase):
    def test_dlss_only_on_a_geforce_rtx(self):
        for line, upscaler, frame_gen in ((RADEON, {'dlss'}, {'dlss'}), (ARC, {'dlss'}, {'dlss'}),
                                          (GTX_1080, {'dlss'}, {'dlss'}), (RTX_3070, set(), {'dlss'}),
                                          (RTX_4070, set(), set())):
            with self.subTest(line=line):
                self.assertEqual(hardware.unavailable(*card(line)), {'upscaler': upscaler, 'frame_gen': frame_gen})

    def test_a_card_missing_from_the_table_by_its_name(self):
        self.assertEqual(hardware.unavailable('NVIDIA GeForce RTX 6090'), {'upscaler': set(), 'frame_gen': set()})
        self.assertEqual(hardware.unavailable('NVIDIA GeForce RTX 3050 A'), {'upscaler': set(), 'frame_gen': {'dlss'}})
        self.assertEqual(hardware.unavailable('AMD Radeon Graphics'), {'upscaler': {'dlss'}, 'frame_gen': {'dlss'}})
        # an unknown maker: nothing is taken away
        self.assertEqual(hardware.unavailable('Virtual GPU'), {'upscaler': set(), 'frame_gen': set()})

    def test_fsr4_is_named_after_its_cards_in_every_language(self):
        english = dict(launcher.UPSCALERS)['fsr4'][0]
        self.assertEqual(english, 'FSR 4 (Radeon RX 7000/9000)')
        for language, _name in bbport_lang.LANGUAGE_NAMES:
            if language in ('', 'en'):
                continue
            with self.subTest(language=language), patch.object(launcher, 'LANG', language):
                self.assertIn('Radeon RX 7000/9000', launcher._(english))
                self.assertNotIn('best', launcher._(english))


class ListTests(unittest.TestCase):
    """The lists in a real (hidden) Tk window, on a PC rated as the launcher does."""

    def setUp(self):
        try:
            self.root = tkinter.Tk()
        except tkinter.TclError as error:
            self.skipTest(f'Tk session unavailable: {error}')
        self.root.withdraw()
        self.addCleanup(self.root.destroy)
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        folder = Path(temporary.name)
        (folder / 'bbport.ini').write_text('fsr411_backend=vk\n', encoding='utf-8')
        for patcher in (patch.dict(os.environ, {'BB_CONFIG': str(folder / 'bbport.ini'), 'BB_INPUT_CONFIG': ''}),
                        patch.multiple(launcher, CONFIG_DIR=folder, CONFIG_FILE=folder / 'settings.json', LANG='en')):
            patcher.start()
            self.addCleanup(patcher.stop)

    def open(self, gpu_line, rated=True):
        from tkinter import ttk
        ui = launcher.Launcher.__new__(launcher.Launcher)
        ui.tk, ui.ttk, ui.root = tkinter, ttk, self.root
        ui.app = dict(launcher.APP_DEFAULTS)
        ui.ini, ui.ini_lines = launcher.load_ini(ui.app)
        ui.vars, ui.mod_order, ui.patch_vars = {}, [], {}
        ui.remap_edits, ui.mouse_edited = {}, False
        ui.status = types.SimpleNamespace(configure=lambda **_k: None)
        ui.refresh_status = lambda: None
        ui.game_monitor_size = lambda: (2560, 1440)
        ui.px = lambda n: n
        ui.fsr411_refreshers, ui.fg401_refreshers, ui.fg401_support = [], [], None
        ui.fsr411_support = None
        ui.init_hardware()
        parent = ttk.Frame(self.root)
        ui.fg401_status = ttk.Label(parent)
        self.upscaler = ui.choice(parent, 'upscaler', 'ini', launcher.UPSCALERS)
        self.frame_gen = ui.fg401_choice(parent)
        if rated:
            ui.gpu_line, ui.cpu, ui.ram = gpu_line, hardware.cpu_from_name(RYZEN), 32
            with patch.object(launcher.Launcher, 'auto_settings', lambda _self: None):
                ui.hardware_ready()
        return ui

    def entries(self):
        menu = self.upscaler.menu
        return {menu.entrycget(i, 'label'): menu.entrycget(i, 'state') for i in range(menu.index('end') + 1)}

    def test_dlss_greyed_out_with_the_reason_on_a_radeon(self):
        self.open(RADEON)
        self.assertEqual(self.entries()['DLSS (NVIDIA GeForce RTX)' + NOTE], 'disabled')
        self.assertEqual(self.entries()['FSR 3.1 (every GPU)'], 'normal')

    def test_dlss_offered_and_recommended_on_a_geforce_rtx(self):
        self.open(RTX_4070)
        self.assertEqual(self.entries()['DLSS (NVIDIA GeForce RTX) — recommended for this PC'], 'normal')
        self.assertFalse([label for label in self.entries() if NOTE in label])

    def test_the_automatic_choice_is_marked_on_a_radeon(self):
        ui = self.open(RADEON)
        picked = ui.recommended_choice('upscaler')
        marked = [label for label in self.entries() if label.endswith('— recommended for this PC')]
        self.assertEqual(len(marked), 1)
        self.assertTrue(marked[0].startswith('FSR 4.1' if picked in ('fsr411', 'fsr412') else
                                             launcher._(*dict(launcher.UPSCALERS)[picked])), (picked, marked))

    def test_nothing_taken_away_before_the_detection(self):
        self.open(RADEON, rated=False)
        self.assertEqual(self.entries()['DLSS (NVIDIA GeForce RTX)'], 'normal')
        self.assertFalse([label for label in self.entries() if '—' in label])

    def test_a_saved_dlss_shows_why_on_a_radeon(self):
        ui = self.open(RADEON)
        ui.var('upscaler', 'ini').set('dlss')
        self.assertEqual(self.upscaler.button.cget('text'), 'DLSS (NVIDIA GeForce RTX)' + NOTE)

    def test_dlss_frame_generation_cannot_be_picked_without_an_rtx_40(self):
        ui = self.open(RTX_3070)
        texts = list(self.frame_gen['values'])
        dlss = next(i for i, text in enumerate(texts) if text.startswith('NVIDIA DLSS'))
        self.assertTrue(texts[dlss].endswith(NOTE))
        self.frame_gen.current(dlss)
        self.frame_gen.event_generate('<<ComboboxSelected>>')
        self.root.update()
        self.assertEqual(ui.var('frame_gen', 'ini').get(), 'off')
        self.assertEqual(self.frame_gen.get(), 'Off')

    def test_dlss_frame_generation_on_an_rtx_40(self):
        ui = self.open(RTX_4070)
        texts = list(self.frame_gen['values'])
        dlss = next(i for i, text in enumerate(texts) if text.startswith('NVIDIA DLSS'))
        self.assertNotIn(NOTE, texts[dlss])
        self.frame_gen.current(dlss)
        self.frame_gen.event_generate('<<ComboboxSelected>>')
        self.root.update()
        self.assertEqual(ui.var('frame_gen', 'ini').get(), 'dlss')


if __name__ == '__main__':
    unittest.main()
