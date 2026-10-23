# SPDX-License-Identifier: GPL-2.0-or-later
"""Exercise profile migration, overlay/launcher saves, and inherited restart environments."""
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'launcher'))
import bbport_launcher_win as launcher
spec = importlib.util.spec_from_file_location('bbport_run_shared_test', ROOT / 'run.py')
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)


class SharedSettingsTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.folder = Path(self.temporary.name)
        self.ini = self.folder / 'bbport.ini'
        self.environment = patch.dict(os.environ, {'BB_CONFIG': str(self.ini)})
        self.environment.start()
        self.addCleanup(self.environment.stop)
        self.ini_path = patch.object(launcher, 'ini_path', return_value=self.ini)
        self.ini_path.start()
        self.addCleanup(self.ini_path.stop)

    def test_migrate_legacy_profile_once_preserves_unrelated_lines(self):
        original = '# existing config\nupscaler=fsr411\nobject_motion=1\nsharpness=0.75\n'
        self.ini.write_text(original)
        legacy = {'fsr411_backend': 'dx12', 'object_motion_cache': True, 'user_dir': 'kept'}
        graphics, _ = launcher.load_ini(legacy)
        self.assertEqual((graphics['fsr411_backend'], graphics['object_motion_cache']), ('dx12', '1'))
        self.assertTrue(self.ini.read_text().startswith(original))
        self.assertEqual(legacy['user_dir'], 'kept')
        self.ini.write_text('fsr411_backend=vk\nobject_motion_cache=0\n')
        self.assertEqual(launcher.load_ini(legacy)[0]['object_motion_cache'], '0')
        self.assertEqual(launcher.load_ini(legacy)[0]['fsr411_backend'], 'vk')

    def test_partial_migration_and_safe_defaults(self):
        self.ini.write_text('fsr411_backend=vk\n')
        graphics, _ = launcher.load_ini({'fsr411_backend': 'dx12', 'object_motion_cache': True})
        self.assertEqual((graphics['fsr411_backend'], graphics['object_motion_cache']), ('vk', '1'))
        self.ini.write_text('fsr411_backend=bad\nobject_motion_cache=true\n')
        graphics, _ = launcher.load_ini({'fsr411_backend': 'dx12', 'object_motion_cache': True})
        self.assertEqual((graphics['fsr411_backend'], graphics['object_motion_cache']), ('vk', '0'))

    def test_shortcut_migrates_and_managed_environment_follows_ini(self):
        self.ini.write_text('upscaler=fsr411\nobject_motion=1\n')
        settings = {**launcher.APP_DEFAULTS, 'fsr411_backend': 'dx12', 'object_motion_cache': True,
                    'extra_env': 'BB_FSR411_BACKEND=vk BB_OBJECT_MOTION_CACHE=0 BB_SHARED_GRAPHICS_CONFIG=0'}
        environment = launcher.game_environment(settings)
        self.assertEqual(environment['BB_FSR411_BACKEND'], 'dx12')
        self.assertEqual(environment['BB_FSR411_NATIVE'], '1')
        self.assertEqual(environment['BB_OBJECT_MOTION_CACHE'], '1')
        self.assertEqual(environment['BB_SHARED_GRAPHICS_CONFIG'], '1')
        self.assertEqual(launcher.load_ini()[0]['object_motion_cache'], '1')

    def test_restart_reloads_both_directions_and_retains_inactive_preference(self):
        env = {'BB_SHARED_GRAPHICS_CONFIG': '1', 'BB_FSR411_BACKEND': 'dx12',
               'BB_FSR411_NATIVE': '1', 'BB_OBJECT_MOTION_CACHE': '1'}
        for backend, cache, upscaler, motion, effective in (
                ('vk', '0', 'fsr411', '1', '0'), ('dx12', '1', 'fsr3', '1', '1'),
                ('vk', '1', 'taa', '1', '0'), ('dx12', '1', 'fsr411', '0', '0')):
            with self.subTest(backend=backend, cache=cache, upscaler=upscaler, motion=motion):
                text = (f'upscaler={upscaler}\nobject_motion={motion}\n'
                        f'fsr411_backend={backend}\nobject_motion_cache={cache}\n')
                self.ini.write_text(text)
                runner.refresh_shared_graphics(env, self.ini)
                self.assertEqual(env['BB_FSR411_BACKEND'], backend)
                self.assertEqual(env['BB_FSR411_NATIVE'], '1' if backend == 'dx12' else '0')
                self.assertEqual(env['BB_OBJECT_MOTION_CACHE'], effective)
                self.assertEqual(self.ini.read_text(), text)

    def test_direct_experiments_retain_explicit_environment(self):
        self.ini.write_text('fsr411_backend=vk\nobject_motion_cache=0\n')
        env = {'BB_FSR411_BACKEND': 'dx12', 'BB_OBJECT_MOTION_CACHE': '1'}
        before = dict(env)
        runner.refresh_shared_graphics(env, self.ini)
        self.assertEqual(env, before)

    def test_native_overlay_save_roundtrip(self):
        fixture = ROOT / 'out/gpu/shared-graphics-test.exe'
        if not fixture.is_file():
            self.skipTest('Build shared-graphics-test for the native/Python roundtrip')
        self.ini.write_text('upscaler=fsr411\nobject_motion=1\nsharpness=0.75\n')
        launcher.load_ini({'fsr411_backend': 'dx12', 'object_motion_cache': True})
        for backend, cache in (('vk', '0'), ('dx12', '1')):
            result = subprocess.run([str(fixture), 'save', str(self.ini), backend, cache],
                                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            self.assertEqual(result.returncode, 0, result.stdout)
            graphics, _ = launcher.load_ini({'fsr411_backend': 'vk', 'object_motion_cache': False})
            self.assertEqual((graphics['fsr411_backend'], graphics['object_motion_cache']), (backend, cache))
            self.assertEqual(graphics['sharpness'], '0.75')
            env = launcher.game_environment(launcher.APP_DEFAULTS)
            runner.refresh_shared_graphics(env, self.ini)
            self.assertEqual(env['BB_FSR411_BACKEND'], backend)
            self.assertEqual(env['BB_OBJECT_MOTION_CACHE'], cache)


@unittest.skipUnless(sys.platform == 'win32', 'Windows launcher widgets')
class OpenLauncherTests(unittest.TestCase):
    def setUp(self):
        SharedSettingsTests.setUp(self)
        import tkinter as tk
        from tkinter import ttk
        self.root = tk.Tk()
        self.root.withdraw()
        self.addCleanup(self.root.destroy)
        self.ui = launcher.Launcher.__new__(launcher.Launcher)
        self.ui.root, self.ui.tk, self.ui.ttk, self.ui.dpi = self.root, tk, ttk, 1
        self.ui.app = dict(launcher.APP_DEFAULTS)
        self.ui.ini, self.ui.ini_lines = dict(launcher.INI_DEFAULTS), []
        self.ui.vars = {}
        self.ui.fsr411_refreshers = []
        self.ui.mod_order, self.ui.patch_vars = [], {}
        self.ui.remap_edits, self.ui.mouse_edited = {}, False
        frame = ttk.Frame(self.root)
        self.ui.motion_cache_check(frame)
        self.ui.fsr411_choice(frame)
        self.profile_patch = patch.multiple(launcher, CONFIG_DIR=self.folder, CONFIG_FILE=self.folder / 'settings.json')
        self.profile_patch.start()
        self.addCleanup(self.profile_patch.stop)

    def test_overlay_edit_survives_collect_and_updates_widgets(self):
        self.ui.app.update(fsr411_backend='vk', object_motion_cache=False)
        self.ini.write_text('fsr411_backend=dx12\nobject_motion_cache=1\n')
        self.ui.collect()
        self.assertEqual(self.ui.vars['fsr411_backend'].get(), 'dx12')
        self.assertTrue(self.ui.vars['object_motion_cache'].get())
        self.assertEqual(launcher.load_ini()[0]['fsr411_backend'], 'dx12')
        profile = json.loads((self.folder / 'settings.json').read_text())
        self.assertNotIn('fsr411_backend', profile)
        self.assertNotIn('object_motion_cache', profile)

    def test_explicit_launcher_edit_wins_over_earlier_overlay_edit(self):
        self.ini.write_text('fsr411_backend=vk\nobject_motion_cache=0\n')
        self.ui.var('fsr411_backend', 'ini').set('dx12')
        self.ui.var('object_motion_cache', 'ini').set(True)
        self.ui.collect()
        graphics, _ = launcher.load_ini()
        self.assertEqual((graphics['fsr411_backend'], graphics['object_motion_cache']), ('dx12', '1'))


if __name__ == '__main__':
    unittest.main()
