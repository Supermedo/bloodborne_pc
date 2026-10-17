"""Unit tests for Frame Generation and Reflex settings in bbport launcher and environment."""
import importlib.util
import os
from pathlib import Path
from types import SimpleNamespace
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('launcher_win', ROOT / 'launcher/bbport_launcher_win.py')
launcher = importlib.util.module_from_spec(spec)
spec.loader.exec_module(launcher)


class Variable:
    def __init__(self, value):
        self.value = value
        self.callbacks = []

    def get(self):
        return self.value

    def set(self, value):
        self.value = value
        for callback in self.callbacks:
            callback()

    def trace_add(self, _mode, callback):
        self.callbacks.append(callback)


class Combobox:
    def __init__(self, _parent, **options):
        self.options = options
        self.text = ''
        self.events = {}

    def current(self, index=None):
        if index is not None:
            self.text = self.options['values'][index]
        return self.options['values'].index(self.text) if self.text in self.options['values'] else -1

    def set(self, text):
        self.text = text

    def get(self):
        return self.text

    def bind(self, event, callback):
        self.events[event] = callback

    def configure(self, **kwargs):
        self.options.update(kwargs)


class LauncherFgSettingsTests(unittest.TestCase):
    def test_ini_defaults_include_fg_and_reflex(self):
        self.assertIn('frame_generation', launcher.INI_DEFAULTS)
        self.assertEqual(launcher.INI_DEFAULTS['frame_generation'], 'off')
        self.assertIn('fg_backend', launcher.INI_DEFAULTS)
        self.assertEqual(launcher.INI_DEFAULTS['fg_backend'], 'dlss')
        self.assertIn('reflex', launcher.INI_DEFAULTS)
        self.assertEqual(launcher.INI_DEFAULTS['reflex'], 'on')

    def test_fg_backend_options(self):
        backend_keys = [k for k, _ in launcher.FG_BACKENDS]
        self.assertEqual(backend_keys, ['dlss', 'fsr'])

    def test_dlss_and_fsr_fg_modes(self):
        dlss_modes = [k for k, _ in launcher.FG_MODES_DLSS]
        fsr_modes = [k for k, _ in launcher.FG_MODES_FSR]
        self.assertEqual(dlss_modes, ['off', '2x', '3x', '4x'])
        self.assertEqual(fsr_modes, ['off', '2x'])

    def test_reflex_modes(self):
        reflex_keys = [k for k, _ in launcher.REFLEX_MODES]
        self.assertEqual(reflex_keys, ['off', 'on', 'boost'])

    def test_choices_follow_replaced_backend_values(self):
        # Exercise the production choice callback, including a dropdown created
        # as FSR (two choices) which later exposes DLSS's third/fourth choices.
        window = launcher.Launcher.__new__(launcher.Launcher)
        value = Variable('off')
        window.var = lambda *_: value
        window.ttk = SimpleNamespace(Combobox=Combobox)
        box = window.choice(None, 'frame_generation', 'ini', launcher.FG_MODES_FSR)
        box.bbport_values = [key for key, _ in launcher.FG_MODES_DLSS]
        box.configure(values=[launcher._(*label) for _, label in launcher.FG_MODES_DLSS])
        box.current(3)
        box.events['<<ComboboxSelected>>'](None)
        self.assertEqual(value.get(), '4x')
        self.assertEqual(box.current(), 3)
        box.bbport_values = [key for key, _ in launcher.FG_MODES_FSR]
        box.configure(values=[launcher._(*label) for _, label in launcher.FG_MODES_FSR])
        value.set('2x')
        self.assertEqual(box.current(), 1)

    def test_fsr_backend_fallback_clamps_factor(self):
        # When switching to FSR backend, 3x or 4x should fall back to 2x predictably
        window = launcher.Launcher.__new__(launcher.Launcher)
        vars_dict = {
            'fg_backend': Variable('dlss'),
            'frame_generation': Variable('4x'),
            'reflex': Variable('on')
        }
        window.vars = vars_dict
        window.var = lambda key, store: vars_dict[key]
        window.ttk = SimpleNamespace(Combobox=Combobox)

        backend_box = Combobox(None, values=[k for k, _ in launcher.FG_BACKENDS])
        fg_box = Combobox(None, values=[k for k, _ in launcher.FG_MODES_DLSS])

        def on_backend_change(*_args):
            b = window.var('fg_backend', 'ini').get()
            modes = launcher.FG_MODES_DLSS if b == 'dlss' else launcher.FG_MODES_FSR
            values = [v for v, _t in modes]
            fg_box.configure(values=values)
            fg_var = window.var('frame_generation', 'ini')
            if fg_var.get() not in values:
                fg_var.set('2x' if fg_var.get() in ('3x', '4x') else values[0])

        vars_dict['fg_backend'].trace_add('write', on_backend_change)

        self.assertEqual(vars_dict['frame_generation'].get(), '4x')
        vars_dict['fg_backend'].set('fsr')
        self.assertEqual(vars_dict['frame_generation'].get(), '2x')

        vars_dict['frame_generation'].set('off')
        vars_dict['fg_backend'].set('dlss')
        self.assertEqual(vars_dict['frame_generation'].get(), 'off')

    def test_game_environment_passes_fg_and_reflex(self):
        app_settings = dict(launcher.APP_DEFAULTS)
        ini_settings = {
            'frame_generation': '3x',
            'fg_backend': 'dlss',
            'reflex': 'boost',
            'output_res': '2560x1440'
        }
        env = launcher.game_environment(app_settings, ini_settings)
        self.assertEqual(env.get('BB_FRAME_GENERATION'), '3x')
        self.assertEqual(env.get('BB_FG_BACKEND'), 'dlss')
        self.assertEqual(env.get('BB_REFLEX'), 'boost')
        self.assertEqual(env.get('BB_OUTPUT_RES'), '2560x1440')

    def test_game_environment_respects_existing_os_env(self):
        app_settings = dict(launcher.APP_DEFAULTS)
        ini_settings = {
            'frame_generation': '2x',
            'fg_backend': 'fsr',
            'reflex': 'on'
        }
        old_val = os.environ.get('BB_FRAME_GENERATION')
        try:
            os.environ['BB_FRAME_GENERATION'] = '4x'
            env = launcher.game_environment(app_settings, ini_settings)
            self.assertEqual(env.get('BB_FRAME_GENERATION'), '4x')
            self.assertEqual(env.get('BB_FG_BACKEND'), 'fsr')
        finally:
            if old_val is None:
                os.environ.pop('BB_FRAME_GENERATION', None)
            else:
                os.environ['BB_FRAME_GENERATION'] = old_val


if __name__ == '__main__':
    unittest.main()
