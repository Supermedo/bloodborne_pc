# SPDX-License-Identifier: GPL-2.0-or-later
"""Automatic graphics settings across launcher starts (bbport_setup.auto_settings): only real new
hardware gets the settings for it, and once. The display, a laptop's two graphics cards taking turns
and the signature 1.9.3 and 1.9.4 stored do not count, and what the player changed by hand (in the
launcher or in the game's menu, which writes bbport.ini) is never overwritten.

A player on 1.9.3/1.9.4: "the game always launches with all graphics settings turned down ... the
launcher keeps lowering the graphics settings", whatever he set in the game or the launcher.

Each start loads settings.json and bbport.ini from a temporary folder as the launcher does, rates
the hardware and saves (as before PLAY). Tk variables live in a Tcl interpreter without a window."""
from paths import ROOT
import functools
import json
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
import bbport_hardware as hardware  # noqa: E402
import bbport_launcher_win as launcher  # noqa: E402

RADEON = 'AMD Radeon RX 7800 XT, discrete, 16368 MiB'
GEFORCE = 'NVIDIA GeForce RTX 4070, discrete, 12282 MiB'
RYZEN = 'AMD Ryzen 7 5800X 8-Core Processor'
# A laptop: the GeForce, or only the Radeon in the processor while the GeForce sleeps or is off
# (MUX switch, eco mode). bb-gpu-capabilities names the card the game would use.
LAPTOP_GEFORCE = 'NVIDIA GeForce RTX 4060 Laptop GPU, discrete, 8188 MiB'
LAPTOP_RADEON = 'AMD Radeon 780M Graphics, integrated or other, 4096 MiB'
LAPTOP_CPU = 'AMD Ryzen 7 7840HS w/ Radeon 780M Graphics'


class Status:
    """The status line: the last text it showed."""
    text = ''

    def configure(self, text='', **_options):
        self.text = text


class AutoProfileTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.folder = Path(temporary.name)
        self.ini = self.folder / 'bbport.ini'
        self.ini.write_text('fsr411_backend=vk\n', encoding='utf-8')  # no capability probe on load
        environment = patch.dict(os.environ, {'BB_CONFIG': str(self.ini), 'BB_INPUT_CONFIG': ''})
        environment.start()
        self.addCleanup(environment.stop)
        files = patch.multiple(launcher, CONFIG_DIR=self.folder, CONFIG_FILE=self.folder / 'settings.json',
                               LANG='en')  # the texts below are English on any Windows
        files.start()
        self.addCleanup(files.stop)
        tcl = tkinter.Tcl()
        self.tk = types.SimpleNamespace(TclError=tkinter.TclError, **{
            kind: functools.partial(getattr(tkinter, kind), tcl) for kind in ('BooleanVar', 'StringVar', 'DoubleVar')})

    # ---- a start of the launcher ---------------------------------------------------------------
    def start(self, gpu_line, display=(2560, 1440), cpu=RYZEN, ram=32):
        """The launcher opened on this hardware and display until the hardware is rated; .automatic
        counts the profiles auto_settings applied."""
        ui = launcher.Launcher.__new__(launcher.Launcher)
        ui.tk = self.tk
        saved = (json.loads((self.folder / 'settings.json').read_text(encoding='utf-8'))
                 if (self.folder / 'settings.json').exists() else {})
        ui.app = {**launcher.APP_DEFAULTS, **saved}
        ui.ini, ui.ini_lines = launcher.load_ini(ui.app)
        ui.vars, ui.mod_order, ui.patch_vars = {}, [], {}
        ui.remap_edits, ui.mouse_edited = {}, False
        ui.status = Status()
        ui.refresh_status = lambda: None
        ui.game_monitor_size = lambda: display
        ui.init_hardware()
        ui.gpu_line, ui.cpu, ui.ram = gpu_line, hardware.cpu_from_name(cpu), ram
        ui.automatic = 0
        apply_profile = ui.apply_profile

        def counted(entries, remember_undo=True):
            ui.automatic += 1
            apply_profile(entries, remember_undo)
        ui.apply_profile = counted
        ui.hardware_ready()
        ui.apply_profile = apply_profile
        return ui

    def set_up(self, gpu_line, **hardware_):
        """The first start: the setup guide applies the settings for this PC; saved."""
        ui = self.start(gpu_line, **hardware_)
        self.assertEqual(ui.automatic, 0)  # the guide has not run yet
        ui.apply_profile(ui.recommended()[2], remember_undo=False)
        ui.var('setup_done', 'app').set(True)
        return self.saved(ui)

    def saved(self, ui):
        ui.collect()
        return ui.current_settings()

    def game_menu(self, **values):
        """The in-game menu (Insert or L3+R3) writes bbport.ini while the launcher is closed."""
        _values, lines = launcher.load_ini()
        launcher.save_ini({key: str(value) for key, value in values.items()}, lines)

    def recommended(self, ui):
        return {key: value for _store, key, value, _why in ui.recommended()[2]}

    # ---- the cases -----------------------------------------------------------------------------
    def test_a_signature_of_1_9_4_on_the_same_pc_changes_nothing(self):
        """1.9.4 stored the display after the hardware and no record of what it applied; the player's
        own settings differ from the recommendation."""
        (self.folder / 'settings.json').write_text(json.dumps({
            'setup_done': True, 'game_dir': str(self.folder),
            'auto_hw': f'AMD Radeon RX 7800 XT 16 GB|{RYZEN}|32|1920x1080'}), encoding='utf-8')
        self.game_menu(preset='3', effect_ssao='0', model_lod='1')
        ui = self.start(RADEON, display=(2560, 1440))
        self.assertEqual(ui.automatic, 0)
        now = self.saved(ui)
        self.assertEqual((now['preset'], now['effect_ssao'], now['model_lod']), ('3', False, '1'))
        self.assertEqual(ui.app['auto_hw'], f'AMD Radeon RX 7800 XT 16 GB|{RYZEN}|32')  # without the display
        self.assertEqual(ui.app['auto_gpus'], ['AMD Radeon RX 7800 XT 16 GB'])
        self.assertEqual(self.start(RADEON).automatic, 0)

    def test_another_monitor_keeps_the_settings(self):
        after_setup = self.set_up(RADEON, display=(2560, 1440))
        for display in ((3840, 2160), (1920, 1080), (2560, 1440)):
            ui = self.start(RADEON, display=display)
            self.assertEqual(ui.automatic, 0, display)
            self.assertEqual(self.saved(ui), after_setup, display)
        # Changed by hand, then a TV is switched on: still the player's.
        ui = self.start(RADEON)
        ui.var('effect_ssao', 'ini').set(False)
        self.saved(ui)
        ui = self.start(RADEON, display=(3840, 2160))
        self.assertEqual(ui.automatic, 0)
        self.assertFalse(self.saved(ui)['effect_ssao'])

    def test_settings_changed_in_the_game_stay_on_new_hardware(self):
        self.set_up(RADEON)
        self.game_menu(preset='3', effect_ssao='0')
        ui = self.start(GEFORCE)
        self.assertEqual(ui.automatic, 0)
        self.assertIn('Your own graphics settings were kept', ui.status.text)
        now = self.saved(ui)
        self.assertEqual((now['preset'], now['effect_ssao'], now['upscaler']), ('3', False, 'fsr4'))
        self.assertNotEqual(now['upscaler'], self.recommended(ui)['upscaler'])  # offered, not applied
        ui = self.start(GEFORCE)  # said once: this card is the PC's now
        self.assertEqual((ui.automatic, ui.status.text), (0, ''))

    def test_settings_changed_in_the_launcher_stay_on_new_hardware(self):
        self.set_up(RADEON)
        ui = self.start(RADEON)
        ui.var('fps_mode', 'app').set('60')
        self.saved(ui)
        ui = self.start(GEFORCE)
        self.assertEqual(ui.automatic, 0)
        self.assertEqual(self.saved(ui)['fps_mode'], '60')

    def test_new_hardware_gets_its_settings_once(self):
        self.set_up(RADEON)
        ui = self.start(GEFORCE)
        self.assertEqual(ui.automatic, 1)
        now = self.saved(ui)
        self.assertEqual(now['upscaler'], 'dlss')
        self.assertEqual({key: now[key] for key in self.recommended(ui)}, self.recommended(ui))
        self.assertEqual(ui.app['auto_undo']['upscaler'], 'fsr4')  # Undo brings the Radeon's back
        self.assertEqual(self.start(GEFORCE).automatic, 0)
        # From now on a change in the game stays, also at the next new hardware.
        self.game_menu(preset='2')
        self.assertEqual(self.start(GEFORCE).automatic, 0)
        ui = self.start(RADEON, cpu='AMD Ryzen 9 7950X 16-Core Processor', ram=64)
        self.assertEqual(ui.automatic, 0)
        self.assertEqual(self.saved(ui)['preset'], '2')

    def test_a_new_processor_or_memory_is_new_hardware(self):
        self.set_up(RADEON, ram=16)
        x3d = 'AMD Ryzen 7 9800X3D 8-Core Processor'
        for cpu, ram, automatic in ((RYZEN, 16, 0), (RYZEN, 15, 0), (RYZEN, 32, 1), (RYZEN, 32, 0), (x3d, 32, 1),
                                    (x3d, 32, 0)):
            ui = self.start(RADEON, cpu=cpu, ram=ram)  # 15: what Windows counts without the modules' size
            self.assertEqual(ui.automatic, automatic, (cpu, ram))
            self.saved(ui)

    def test_a_laptops_two_cards_taking_turns(self):
        laptop = {'cpu': LAPTOP_CPU, 'ram': 16, 'display': (1920, 1080)}
        after_setup = self.set_up(LAPTOP_GEFORCE, **laptop)
        for card in (LAPTOP_RADEON, LAPTOP_GEFORCE) * 3:
            ui = self.start(card, **laptop)
            self.assertEqual(ui.automatic, 0, card)
            self.assertEqual(self.saved(ui), after_setup, card)
        self.assertEqual(ui.app['auto_hw'].split('|')[0], 'NVIDIA GeForce RTX 4060 Laptop GPU 8 GB')

    def test_a_laptop_set_up_while_its_geforce_slept(self):
        """The guide ran on the integrated graphics: the GeForce, when it shows, gets its settings
        once; after that the two take turns without any."""
        laptop = {'cpu': LAPTOP_CPU, 'ram': 16, 'display': (1920, 1080)}
        self.set_up(LAPTOP_RADEON, **laptop)
        ui = self.start(LAPTOP_GEFORCE, **laptop)
        self.assertEqual(ui.automatic, 1)
        geforce = self.saved(ui)
        self.assertEqual(geforce['upscaler'], 'dlss')
        for card in (LAPTOP_RADEON, LAPTOP_GEFORCE) * 3:
            ui = self.start(card, **laptop)
            self.assertEqual(ui.automatic, 0, card)
            self.assertEqual(self.saved(ui), geforce, card)

    def test_two_cards_taking_turns(self):
        """An eGPU with its cable in or out, two cards in one PC: the second one gets its settings
        once; a card seen before is no new hardware."""
        self.set_up(RADEON)
        ui = self.start(GEFORCE)
        self.assertEqual(ui.automatic, 1)
        geforce = self.saved(ui)
        for card in (RADEON, GEFORCE) * 3:
            ui = self.start(card)
            self.assertEqual(ui.automatic, 0, card)
            self.assertEqual(self.saved(ui), geforce, card)

    def test_nothing_detected_changes_nothing(self):
        """bb-gpu-capabilities found nothing this time (a driver update, a timeout) while a card is
        chosen by hand: no signature to compare, the remembered hardware stays."""
        self.set_up(RADEON)
        ui = self.start('')
        ui.var('gpu_override', 'app').set('amd-rx-7800-xt')
        ui.hardware_ready()
        self.assertIsNotNone(ui.rating)
        self.assertEqual(ui.app['auto_hw'], f'AMD Radeon RX 7800 XT 16 GB|{RYZEN}|32')
        self.assertEqual(self.start(RADEON).automatic, 0)

    def test_undo_makes_the_settings_the_players_own(self):
        self.set_up(RADEON)
        ui = self.start(GEFORCE)
        self.assertEqual(ui.automatic, 1)
        ui.undo_profile()
        self.saved(ui)
        ui = self.start('NVIDIA GeForce RTX 5080, discrete, 16303 MiB')
        self.assertEqual(ui.automatic, 0)
        self.assertEqual(self.saved(ui)['upscaler'], 'fsr4')

    def test_automatic_settings_off(self):
        self.set_up(RADEON)
        ui = self.start(RADEON)
        ui.var('auto_graphics', 'app').set(False)
        self.saved(ui)
        self.assertEqual(self.start(GEFORCE).automatic, 0)


class SignatureTests(unittest.TestCase):
    def test_the_signature_is_the_hardware_only(self):
        cpu = hardware.cpu_from_name(RYZEN)
        self.assertEqual(hardware.signature(RADEON, cpu, 32), f'AMD Radeon RX 7800 XT 16 GB|{RYZEN}|32')
        self.assertEqual(hardware.signature('', cpu, 32), '')
        self.assertEqual(hardware.signature(RADEON, hardware.Cpu('', None), 16), '')  # not detected

    def test_stored_signatures_of_every_version(self):
        now = f'AMD Radeon RX 7800 XT 16 GB|{RYZEN}|32'
        self.assertEqual(hardware.stored_signature(now + '|2560x1440'), now)
        self.assertEqual(hardware.stored_signature(now), now)
        for unreadable in ('', None, {}, 'garbage', 'card|cpu|32'):
            self.assertEqual(hardware.stored_signature(unreadable), '', unreadable)

    def test_what_is_new_hardware(self):
        before = f'AMD Radeon RX 7800 XT 16 GB|{RYZEN}|32|2560x1440'
        new = lambda now, seen=(), integrated=False: hardware.new_hardware(before, now, seen, integrated)
        self.assertFalse(new(f'AMD Radeon RX 7800 XT 16 GB|{RYZEN}|32'))
        self.assertFalse(new(f'AMD Radeon RX 7800 XT 15 GB|{RYZEN}|32'))  # what the driver reports
        self.assertFalse(new(f'Radeon RX 7800 XT 16 GB|{RYZEN}|32'))  # another driver's wording
        self.assertFalse(new(f'AMD Radeon RX 7800 XT 16 GB|{RYZEN}|31'))
        self.assertFalse(new(f'AMD Radeon RX 7800 XT 16 GB|{RYZEN}|0'))  # memory unknown
        self.assertTrue(new(f'AMD Radeon RX 7800 XT 16 GB|{RYZEN}|64'))
        self.assertTrue(new(f'AMD Radeon RX 7800 XT 16 GB|AMD Ryzen 7 9800X3D 8-Core Processor|32'))
        self.assertTrue(new(f'NVIDIA GeForce RTX 4070 12 GB|{RYZEN}|32'))
        self.assertTrue(new(f'NVIDIA GeForce RTX 4060 Ti 16 GB|{RYZEN}|32'))
        self.assertFalse(new(f'NVIDIA GeForce RTX 4070 12 GB|{RYZEN}|32', seen=['NVIDIA GeForce RTX 4070 12 GB']))
        self.assertFalse(new(f'AMD Radeon(TM) Graphics 2 GB|{RYZEN}|32', integrated=True))
        self.assertFalse(new(''))

    def test_the_tools_integrated_cards(self):
        """bb-gpu-capabilities says 'integrated or other' for every card that is not discrete."""
        self.assertTrue(hardware.parse_gpu_line(LAPTOP_RADEON)[1])
        self.assertTrue(hardware.parse_gpu_line('AMD Radeon(TM) Graphics, integrated, 512 MiB')[1])
        self.assertFalse(hardware.parse_gpu_line(LAPTOP_GEFORCE)[1])
        self.assertEqual(hardware.best_gpu_line(['Intel(R) UHD Graphics, integrated or other, 16000 MiB',
                                                 LAPTOP_GEFORCE]), LAPTOP_GEFORCE)


class TextTests(unittest.TestCase):
    def test_the_note_is_translated_everywhere(self):
        import bbport_lang
        note = 'New hardware detected. Your own graphics settings were kept; the ones for it are on Picture & performance.'
        for language in [code for code, _name in bbport_lang.LANGUAGE_NAMES if code not in ('', 'en')]:
            with self.subTest(language=language):
                self.assertTrue(bbport_lang.table(language).get(note))


if __name__ == '__main__':
    unittest.main()
