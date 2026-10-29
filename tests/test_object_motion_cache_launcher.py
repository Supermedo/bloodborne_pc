# SPDX-License-Identifier: GPL-2.0-or-later
import copy
import io
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import Mock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'launcher'))
import bbport_launcher_win as launcher
import bbport_lang_motion_cache as translations


class CacheEnvironmentTest(unittest.TestCase):
    def test_existing_profile_defaults_to_original_path(self):
        settings = dict(launcher.APP_DEFAULTS)
        self.assertEqual(launcher.INI_DEFAULTS['object_motion_cache'], '0')
        with patch.dict('os.environ', {'BB_OBJECT_MOTION_CACHE': '1'}, clear=True):
            env = launcher.game_environment(settings, launcher.INI_DEFAULTS)
        self.assertEqual(env['BB_OBJECT_MOTION_CACHE'], '0')

    def test_checkbox_controls_each_fsr_mode_and_overrides_extra_variables(self):
        for upscaler in ('fsr3', 'fsr4', 'fsr411'):
            for enabled in (False, True):
                settings = {**launcher.APP_DEFAULTS,
                            'extra_env': f'BB_OBJECT_MOTION_CACHE={int(not enabled)}',
                            'frame_cap': '90'}
                original = copy.deepcopy(settings)
                with patch.dict('os.environ', {}, clear=True):
                    env = launcher.game_environment(settings, {'upscaler': upscaler, 'object_motion': '1',
                        'object_motion_cache': str(int(enabled)), 'fsr411_backend': 'dx12'})
                self.assertEqual(env['BB_OBJECT_MOTION_CACHE'], str(int(enabled)))
                self.assertEqual(env['BB_FSR411_BACKEND'], 'dx12')
                self.assertEqual(env['BB_FPS_LIMIT'], '90')
                self.assertEqual(settings, original)

    def test_other_upscalers_and_disabled_motion_use_original_path(self):
        settings = dict(launcher.APP_DEFAULTS)
        for upscaler in ('dlss', 'taa', 'off', 'fsr3', 'fsr4', 'fsr411'):
            for motion in ('0', '1'):
                graphics = {'upscaler': upscaler, 'object_motion': motion, 'object_motion_cache': '1'}
                env = launcher.game_environment(settings, graphics)
                expected = upscaler in ('fsr3', 'fsr4', 'fsr411') and motion == '1'
                self.assertEqual(env['BB_OBJECT_MOTION_CACHE'], str(int(expected)))
                self.assertEqual(graphics['object_motion_cache'], '1')

    def test_shortcut_reads_current_ini_and_preserves_saved_choice(self):
        with tempfile.TemporaryDirectory() as temporary:
            folder = Path(temporary)
            ini = folder / 'bbport.ini'
            settings = {**launcher.APP_DEFAULTS, 'object_motion_cache': True, 'user_dir': str(folder)}
            process = Mock(stdout=io.BytesIO(b''))
            process.wait.return_value = 0
            with patch.dict('os.environ', {'BB_CONFIG': str(ini)}), \
                    patch.object(launcher, 'attach_stdio'), \
                    patch.object(launcher, 'game_underway', return_value=False), \
                    patch.object(launcher.trophies, 'Tracker'), \
                    patch.object(launcher.bbport_fsr411, 'default_backend', return_value='vk'), \
                    patch.object(launcher.bbport_fsr411, 'query', return_value={}), \
                    patch.object(launcher.bbport_fsr411, 'method_status', return_value={'supported': True}), \
                    patch.object(launcher.subprocess, 'Popen', return_value=process) as start:
                for upscaler, expected in (('fsr411', '1'), ('taa', '0'), ('fsr3', '1')):
                    ini.write_text(f'upscaler={upscaler}\nobject_motion=1\n')
                    self.assertEqual(launcher.play_without_window(settings), 0)
                    self.assertEqual(start.call_args.kwargs['env']['BB_OBJECT_MOTION_CACHE'], expected)
            self.assertIs(settings['object_motion_cache'], True)

    def test_translations_cover_supported_languages(self):
        languages = {code for code, _text in launcher.UI_LANGUAGES if code}
        self.assertEqual(languages, set(translations.TRANSLATIONS))
        for language in languages:
            self.assertTrue(all(launcher.bbport_lang.table(language).get(key) for key in translations.KEYS))


@unittest.skipUnless(sys.platform == 'win32', 'Windows launcher widgets')
class CacheWidgetTest(unittest.TestCase):
    def setUp(self):
        import tkinter as tk
        from tkinter import ttk
        self.root = tk.Tk()
        self.root.withdraw()
        self.addCleanup(self.root.destroy)
        self.ui = launcher.Launcher.__new__(launcher.Launcher)
        self.ui.root, self.ui.tk, self.ui.ttk, self.ui.dpi = self.root, tk, ttk, 1
        self.ui.vars = {}
        self.ui.app = dict(launcher.APP_DEFAULTS)
        self.ui.ini, self.ui.ini_lines = dict(launcher.INI_DEFAULTS), []
        self.ui.mod_order, self.ui.patch_vars = [], {}
        self.ui.remap_edits, self.ui.mouse_edited = {}, False
        self.frame = ttk.Frame(self.root)
        self.button = self.ui.motion_cache_check(self.frame)

    def test_widget_default_and_availability_retain_choice(self):
        selected = self.ui.var('object_motion_cache', 'ini')
        self.assertFalse(selected.get())
        self.button.invoke()
        self.assertTrue(selected.get())
        for upscaler in ('fsr3', 'fsr4', 'fsr411', 'taa', 'dlss', 'off', 'fsr411'):
            self.ui.vars['upscaler'].set(upscaler)
            self.assertEqual(self.button.instate(['disabled']), upscaler in ('taa', 'dlss', 'off'))
            self.assertTrue(selected.get())
        self.ui.vars['object_motion'].set(False)
        self.assertTrue(self.button.instate(['disabled']))
        self.button.invoke()
        self.assertTrue(selected.get())
        self.ui.vars['object_motion'].set(True)
        self.assertFalse(self.button.instate(['disabled']))
        self.button.invoke()
        self.assertFalse(selected.get())

    def test_collect_saves_boolean_and_preserves_other_settings(self):
        with tempfile.TemporaryDirectory() as temporary:
            folder = Path(temporary)
            profile = folder / 'settings.json'
            self.ui.app['user_dir'] = 'existing-save-folder'
            self.ui.var('fsr411_backend', 'ini').set('dx12')
            with patch.object(launcher, 'CONFIG_DIR', folder), \
                    patch.object(launcher, 'CONFIG_FILE', profile), \
                    patch.dict('os.environ', {'BB_CONFIG': str(folder / 'bbport.ini')}):
                for selected in (True, False):
                    self.ui.vars['object_motion_cache'].set(selected)
                    self.ui.collect()
                    saved = launcher.load_json(profile, {})
                    self.assertNotIn('object_motion_cache', saved)
                    self.assertNotIn('fsr411_backend', saved)
                    self.assertEqual(saved['user_dir'], 'existing-save-folder')
                    self.assertEqual(launcher.load_ini()[0]['object_motion_cache'], str(int(selected)))
                    self.assertEqual(launcher.load_ini()[0]['fsr411_backend'], 'dx12')
                    self.assertEqual(launcher.load_ini()[0]['object_motion'], '1')

    def test_translated_widgets_and_trace_cleanup(self):
        dependencies = [self.ui.vars[key] for key in ('upscaler', 'object_motion')]
        self.button.destroy()
        baseline = [len(var.trace_info()) for var in dependencies]
        try:
            for language in translations.TRANSLATIONS:
                launcher.LANG = language
                button = self.ui.motion_cache_check(self.frame)
                self.assertEqual(button.cget('text'), translations.TRANSLATIONS[language][translations.KEYS[0]])
                button.destroy()
                self.assertEqual([len(var.trace_info()) for var in dependencies], baseline)
                self.ui.vars['upscaler'].set('fsr3')
        finally:
            launcher.LANG = 'en'


if __name__ == '__main__':
    unittest.main()
