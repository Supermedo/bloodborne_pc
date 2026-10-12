"""Windows launcher persistence and the actual Tk controls page."""
from paths import ROOT
import importlib.util
import io
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import Mock, patch

spec = importlib.util.spec_from_file_location('launcher_win', ROOT / 'launcher/bbport_launcher_win.py')
launcher = importlib.util.module_from_spec(spec)
spec.loader.exec_module(launcher)


class WindowsLauncherTests(unittest.TestCase):
    def test_renderer_choice_sets_game_mode_without_changing_saved_graphics(self):
        for renderer, expected in [('vulkan', '0'), ('remix', '1')]:
            settings = {**launcher.APP_DEFAULTS, 'renderer': renderer, 'hdr': True}
            with patch.dict(os.environ, {'BB_RTX_REMIX': '1'}, clear=True):
                env = launcher.game_environment(settings)
            self.assertEqual(env['BB_RTX_REMIX'], expected)
            self.assertTrue(settings['hdr'])

    def test_play_role_preserves_native_status_and_unicode_log(self):
        with tempfile.TemporaryDirectory() as directory:
            settings = {**launcher.APP_DEFAULTS, 'user_dir': directory}
            for code in (7, 0xc0000005, 0xc0000409, 0xffffffff):
                process = Mock(stdout=io.BytesIO('Crash dump: путь/краш.dmp\n'.encode('utf-8')))
                process.wait.return_value = code
                output = io.StringIO()
                with patch.object(launcher, 'attach_stdio'), \
                     patch.object(launcher.subprocess, 'Popen', return_value=process), \
                     patch.object(launcher.sys, 'stdout', output):
                    result = launcher.play_without_window(settings)
                expected = code if code < 0x80000000 else code-0x100000000
                self.assertEqual(result, expected)
                self.assertIn('путь/краш.dmp', output.getvalue())
                self.assertIn('путь/краш.dmp', (Path(directory)/'last_run.log').read_text(encoding='utf-8'))

    def test_frozen_script_role_preserves_all_windows_status_bits(self):
        for code in (0, 23, 0xc0000005, 0xc0000409, 0xc0000374, 0xffffffff, -1073741819, None, 'failed'):
            for args in (['--run'], ['--script', 'fixture.py']):
                with patch.object(launcher, 'attach_stdio'), \
                     patch.object(launcher.sys, 'argv', ['entry']), \
                     patch.object(launcher.runpy, 'run_path', side_effect=SystemExit(code)):
                    result = launcher.run_role(args)
                expected = (code & 0xffffffff) if isinstance(code, int) else (0 if code is None else 1)
                if expected >= 0x80000000:
                    expected -= 0x100000000
                self.assertEqual(result, expected)

    def test_version_comparison(self):
        self.assertEqual(launcher.VERSION, (ROOT / 'VERSION.txt').read_text().strip())
        self.assertEqual(launcher.version_tuple('windows-v2'), launcher.version_tuple('2.0.0'))
        self.assertLess(launcher.version_tuple('1.5'), launcher.version_tuple(launcher.VERSION))
        self.assertGreater(launcher.version_tuple('2.1'), launcher.version_tuple(launcher.VERSION))
    def test_mouse_settings_normalize_bad_manual_values(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'bbport.ini'
            with patch.dict(os.environ, {'BB_CONFIG': str(path)}):
                for value, expected in [('nan', '100.00'), ('inf', '100.00'), ('bad', '100.00'),
                                        ('0', '1.00'), ('9999', '400.00'), ('125.5', '125.50')]:
                    path.write_text(f'mouse_sensitivity={value}\n', encoding='utf-8')
                    self.assertEqual(launcher.load_ini()[0]['mouse_sensitivity'], expected)
    def test_walk_binding_defaults_remapping_and_unassigned_persist(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'bbport.ini'
            with patch.dict(os.environ, {'BB_CONFIG': str(path)}):
                path.write_text('# existing installation\nkey.move_up=T\n', encoding='utf-8')
                values, lines = launcher.load_ini()
                self.assertEqual(values['key.walk'], 'Left Alt, Right Alt')
                self.assertNotIn('pad.walk', values)
                for binding in ('Left Ctrl, Mouse X1', ''):
                    values['key.walk'] = binding
                    launcher.save_ini(values, lines)
                    values, lines = launcher.load_ini()
                    self.assertEqual(values['key.walk'], binding)
                    self.assertEqual(values['key.move_up'], 'T')
                    self.assertIn('# existing installation', lines)
        for code, _label in launcher.bbport_lang.LANGUAGE_NAMES:
            with patch.object(launcher, 'LANG', code):
                label = launcher._('Walk (hold)', 'Ходьба шагом (удерживать)')
                self.assertTrue(label)
                if code not in ('', 'en'):
                    self.assertNotEqual(label, 'Walk (hold)')
    def test_environment_passes_controller_language_and_logs(self):
        settings = {**launcher.APP_DEFAULTS, 'gamepad': 'saved-guid', 'save_log': True,
                    'fsr411_dir': 'D:/my assets/fsr4_411'}
        with patch.dict(os.environ, {}, clear=True), patch.object(launcher, 'LANG', 'ru'):
            env = launcher.game_environment(settings)
        self.assertEqual(env['BB_GAMEPAD'], 'saved-guid')
        self.assertEqual(env['BB_MENU_LANGUAGE'], 'ru')
        self.assertEqual(env['BB_SAVE_LOG'], '1')
        self.assertEqual(env['BB_FSR411_DIR'], 'D:/my assets/fsr4_411')

    def test_pages_selection_and_persistence(self):
        try:
            import tkinter as tk
            from tkinter import ttk, filedialog, messagebox
        except ImportError as error:
            self.skipTest(str(error))
        try:
            root = tk.Tk()
        except tk.TclError as error:
            self.skipTest(str(error))
        root.withdraw()
        try:
            with tempfile.TemporaryDirectory() as directory:
                data = Path(directory)
                (data / 'bbport.ini').write_text('dlss_preset=k\n', encoding='utf-8')
                result = subprocess.CompletedProcess([], 0, 'guid-one\tVirtual pad\n', '')
                with patch.object(launcher, 'DATA_DIR', data), \
                     patch.object(launcher, 'CONFIG_DIR', data), \
                     patch.object(launcher, 'CONFIG_FILE', data / 'settings.json'), \
                     patch.object(launcher.threading.Thread, 'start'), \
                     patch.object(launcher.subprocess, 'run', return_value=result), \
                     patch.dict(os.environ, {'BB_CONFIG': str(data / 'bbport.ini')}):
                    app = launcher.Launcher(root, tk, ttk, filedialog, messagebox)
                    app.show('controls')
                    root.update_idletasks()
                    self.assertEqual(root.title(), 'Bloodborne — bbport v2')
                    app.show('graphics')
                    app.var('renderer', 'app').set('remix')
                    app.collect()
                    self.assertEqual(launcher.load_json(data/'settings.json',{})['renderer'],'remix')
                    self.assertIn('RTX Remix',app.summary())
                    app.show('controls')
                    self.assertEqual(app.binding_buttons[0].winfo_class(), 'TButton')
                    self.assertEqual(app.var('key.walk', 'ini').get(), 'Left Alt, Right Alt')
                    app.gamepad_box.current(1)
                    app.gamepad_box.event_generate('<<ComboboxSelected>>')
                    self.assertEqual(app.var('gamepad', 'app').get(), 'guid-one')
                    self.assertEqual(app.gamepad_box.current(), 1)
                    app.var('key.cross', 'ini').set('X, Space')
                    app.var('mouse_camera', 'ini').set(True)
                    app.var('mouse_invert_y', 'ini').set(True)
                    app.var('mouse_sensitivity', 'ini').set(125.5)
                    app.collect()
                    self.assertIn('key.cross=X, Space', (data / 'bbport.ini').read_text())
                    saved = (data / 'bbport.ini').read_text(encoding='utf-8')
                    for setting in ('mouse_camera=1', 'mouse_invert_y=1', 'mouse_sensitivity=125.50'):
                        self.assertIn(setting, saved)
                    app.refresh_gamepads()
                    self.assertEqual(app.gamepad_box.current(), 1)
                    # Drive the actual capture buttons and completion/cancel paths without
                    # opening an external window in the Tk unit test.
                    (data / 'bin').mkdir()
                    (data / 'bin/bb-gpu-capabilities.exe').touch()
                    class Capture:
                        def __init__(self, output=b'', code=0):
                            self.output, self.code = output, code
                            self.returncode = None
                            self.pid = 0
                        def poll(self):
                            return self.returncode
                        def communicate(self, timeout=None):
                            return self.output, b'capture error' if self.returncode else b''
                        def terminate(self):
                            self.returncode = -1
                    def finish(process, key='key.cross', append=False):
                        root.after_cancel(app.capture_after)
                        process.returncode = process.code
                        app.poll_capture(key, append)
                    with patch.object(launcher, 'PORT_DIR', data), \
                         patch.object(launcher.subprocess, 'Popen') as start, \
                         patch.object(messagebox, 'showinfo') as info, \
                         patch.object(messagebox, 'showerror') as error:
                        captured = Capture(b'key Mouse Right\n'); start.return_value = captured
                        app.binding_buttons[0].invoke()
                        self.assertTrue(app.binding_buttons[0].instate(['disabled']))
                        self.assertIn('--read-input', start.call_args.args[0])
                        self.assertEqual(start.call_args.kwargs['env']['BB_GAMEPAD'], 'guid-one')
                        finish(captured)
                        self.assertEqual(app.var('key.cross', 'ini').get(), 'Mouse Right')
                        self.assertIn('key.cross=Mouse Right\n', (data / 'bbport.ini').read_text())
                        for expected in ('Mouse Right, Comma', 'Mouse Right, Comma'):
                            captured=Capture(b'key Comma\n'); start.return_value=captured
                            app.binding_buttons[1].invoke(); finish(captured, append=True)
                            self.assertEqual(app.var('key.cross', 'ini').get(), expected)
                        captured=Capture(); start.return_value=captured
                        app.binding_buttons[0].invoke(); finish(captured)
                        self.assertEqual(app.var('key.cross', 'ini').get(), 'Mouse Right, Comma')
                        captured=Capture(b'key Escape\n'); start.return_value=captured
                        app.binding_buttons[0].invoke(); app.cancel_capture()
                        self.assertIsNone(app.capture_process)
                        self.assertIsNone(app.capture_after)
                        self.assertEqual(app.var('key.cross', 'ini').get(), 'Mouse Right, Comma')
                        captured=Capture(code=1); start.return_value=captured
                        app.binding_buttons[0].invoke(); finish(captured)
                        error.assert_called_once()
                        self.assertEqual(app.var('key.cross', 'ini').get(), 'Mouse Right, Comma')
                        app.var('key.cross', 'ini').set('Q, W, E, R')
                        start.reset_mock(); app.binding_buttons[1].invoke()
                        start.assert_not_called(); info.assert_called_once()
                        app.binding_buttons[2].invoke()
                        self.assertEqual(app.var('key.cross', 'ini').get(), '')
                        walk_index = next(i for i, control in enumerate(launcher.CONTROLS) if control[0] == 'walk') * 3
                        captured=Capture(b'key Left Ctrl\n'); start.return_value=captured
                        app.binding_buttons[walk_index].invoke(); finish(captured, key='key.walk')
                        self.assertEqual(app.var('key.walk', 'ini').get(), 'Left Ctrl')
                        self.assertEqual(launcher.load_ini()[0]['key.walk'], 'Left Ctrl')
                        app.binding_buttons[walk_index+2].invoke()
                        self.assertEqual(app.var('key.walk', 'ini').get(), '')
                    app.reset_controls()
                    self.assertEqual(app.var('key.cross', 'ini').get(), 'Space')
                    self.assertEqual(app.var('key.walk', 'ini').get(), 'Left Alt, Right Alt')
                    self.assertFalse(app.var('mouse_camera', 'ini').get())
                    self.assertEqual(app.var('mouse_sensitivity', 'ini').get(), 100.)
                    app.show('graphics')
                    root.update_idletasks()
                    self.assertEqual(app.var('dlss_preset', 'ini').get(), 'K')
                    self.assertTrue(app.dlss_preset_box.instate(['disabled']))
                    app.var('upscaler', 'ini').set('dlss')
                    self.assertTrue(app.dlss_preset_box.instate(['readonly', '!disabled']))
                    for index, (preset, _label) in enumerate(launcher.DLSS_PRESETS):
                        app.dlss_preset_box.current(index)
                        app.dlss_preset_box.event_generate('<<ComboboxSelected>>')
                        self.assertEqual(app.var('dlss_preset', 'ini').get(), preset)
                        app.collect()
                        self.assertIn(f'dlss_preset={preset}\n',
                                      (data / 'bbport.ini').read_text(encoding='utf-8'))
                    app.var('upscaler', 'ini').set('fsr3')
                    self.assertTrue(app.dlss_preset_box.instate(['disabled']))
                    self.assertEqual(app.var('dlss_preset', 'ini').get(), 'L')
        finally:
            root.destroy()


if __name__ == '__main__':
    unittest.main()
