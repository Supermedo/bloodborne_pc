"""A new bbport.ini gets the settings for this PC (bbport_setup.auto_settings). A tester put the 1.9.5 beta in a new
folder: its bbport.ini started with the defaults, FSR 4 and every effect, and the game froze on Intel Arc graphics
and ran at 20 instead of 50 FPS. For that PC the settings are no upscaler and fewer effects."""
from paths import ROOT
import sys
import types
import unittest

sys.path.insert(0, str(ROOT / 'launcher'))
sys.path.insert(0, str(ROOT / 'scripts'))
import bbport_launcher_win  # noqa: E402,F401  (binds the setup module to the launcher)
import bbport_hardware as hardware  # noqa: E402
import bbport_setup  # noqa: E402

ARC = 'Intel(R) Arc(TM) Graphics, integrated, 23014 MiB'


class Window(bbport_setup.SetupMixin):
    def __init__(self, ini_new, auto_hw, auto_graphics=True):
        gpu = hardware.match_gpu('Intel(R) Arc(TM) Graphics', 23)
        self.gpu_line, self.cpu, self.ram = ARC, hardware.cpu_from_name('Intel(R) Core(TM) Ultra 5 125H'), 39
        self.rating = hardware.assess(gpu, self.cpu, self.ram)
        self.ini_new, self.applied, self.rating_views = ini_new, [], []
        self.status = types.SimpleNamespace(configure=lambda **_k: None)
        values = {'setup_done': True, 'auto_graphics': auto_graphics, 'goal': 'smooth'}
        self.var = lambda key, store: types.SimpleNamespace(get=lambda: values[key])
        self.app = {'auto_hw': hardware.signature(ARC, self.cpu, self.ram) if auto_hw == 'same' else auto_hw}

    def display_size(self):
        return 1920, 1080

    def current_settings(self):
        return {}

    def apply_profile(self, entries, remember_undo=True):
        self.applied = {key: value for _store, key, value, _why in entries}


class NewIniTests(unittest.TestCase):
    def test_a_new_ini_gets_this_pcs_settings(self):
        window = Window(ini_new=True, auto_hw='same')
        window.auto_settings()
        self.assertEqual(window.applied['upscaler'], 'off')  # not FSR 4 on Intel graphics
        self.assertFalse(window.applied['object_motion'])
        self.assertIn('New installation', window.auto_note)
        self.assertFalse(window.ini_new)  # once

    def test_also_with_automatic_settings_off_or_never_rated_before(self):
        for auto_hw, auto_graphics in (('same', False), ('', True)):
            window = Window(ini_new=True, auto_hw=auto_hw, auto_graphics=auto_graphics)
            window.auto_settings()
            self.assertEqual(window.applied['upscaler'], 'off', (auto_hw, auto_graphics))

    def test_an_existing_ini_is_left_alone(self):
        window = Window(ini_new=False, auto_hw='same')
        window.auto_settings()
        self.assertEqual(window.applied, [])

    def test_the_launcher_notes_it_before_reading_the_ini(self):
        source = (ROOT / 'launcher' / 'bbport_launcher_win.py').read_text(encoding='utf-8')
        self.assertLess(source.index('self.ini_new = not ini_path().exists()'),
                        source.index('self.ini, self.ini_lines = load_ini(self.app)'))


if __name__ == '__main__':
    unittest.main()
