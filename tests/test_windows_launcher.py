"""Windows launcher persistence and the actual Tk controls page."""
from paths import ROOT
import importlib.util
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('launcher_win', ROOT / 'launcher/bbport_launcher_win.py')
launcher = importlib.util.module_from_spec(spec)
spec.loader.exec_module(launcher)


class WindowsLauncherTests(unittest.TestCase):
    def test_environment_passes_controller_language_and_logs(self):
        settings = {**launcher.APP_DEFAULTS, 'gamepad': 'saved-guid', 'save_log': True,
                    'fsr411_dir': 'D:/my assets/fsr4_411'}
        with patch.dict(os.environ, {}, clear=True), patch.object(launcher, 'LANG', 'ru'):
            env = launcher.game_environment(settings)
        self.assertEqual(env['BB_GAMEPAD'], 'saved-guid')
        self.assertEqual(env['BB_MENU_LANGUAGE'], 'ru')
        self.assertEqual(env['BB_SAVE_LOG'], '1')
        self.assertEqual(env['BB_FSR411_DIR'], 'D:/my assets/fsr4_411')

    def test_controls_page_selection_and_persistence(self):
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
                    app.gamepad_box.current(1)
                    app.gamepad_box.event_generate('<<ComboboxSelected>>')
                    self.assertEqual(app.var('gamepad', 'app').get(), 'guid-one')
                    self.assertEqual(app.gamepad_box.current(), 1)
                    app.var('key.cross', 'ini').set('X, Space')
                    app.collect()
                    self.assertIn('key.cross=X, Space', (data / 'bbport.ini').read_text())
                    app.refresh_gamepads()
                    self.assertEqual(app.gamepad_box.current(), 1)
                    app.reset_controls()
                    self.assertEqual(app.var('key.cross', 'ini').get(), 'Space')
        finally:
            root.destroy()


if __name__ == '__main__':
    unittest.main()
