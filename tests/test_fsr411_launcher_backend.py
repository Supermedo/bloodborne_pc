# SPDX-License-Identifier: GPL-2.0-or-later
import copy
import json
from pathlib import Path
import sys
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'launcher'))
import bbport_launcher_win as launcher
import bbport_lang_fsr411 as translations


class BackendSettingsTest(unittest.TestCase):
    def test_existing_profile_retains_vulkan(self):
        settings = dict(launcher.APP_DEFAULTS)
        settings.pop('fsr411_backend')
        with patch.dict('os.environ', {}, clear=True):
            env = launcher.game_environment(settings)
        self.assertEqual((env['BB_FSR411_BACKEND'], env['BB_FSR411_NATIVE']), ('vk', '0'))

    def test_selection_overrides_inherited_backend_without_changing_other_settings(self):
        for selected, native in (('vk', '0'), ('dx12', '1')):
            settings = {**launcher.APP_DEFAULTS, 'fsr411_backend': selected,
                        'fps_mode': '90', 'frame_cap': '90', 'present_mode': 'Mailbox',
                        'fullscreen': True, 'user_dir': 'copied-saves'}
            original = copy.deepcopy(settings)
            with patch.dict('os.environ', {'BB_FSR411_BACKEND': 'wrong', 'BB_FSR411_NATIVE': 'wrong'}, clear=True):
                env = launcher.game_environment(settings)
            self.assertEqual((env['BB_FSR411_BACKEND'], env['BB_FSR411_NATIVE']), (selected, native))
            self.assertEqual((env['BB_FPS'], env['BB_FPS_LIMIT'], env['BB_PRESENT_MODE']), ('90', '90', 'Mailbox'))
            self.assertEqual(env['BB_USER_DIR'], 'copied-saves')
            self.assertEqual(settings, original)

    def test_all_supported_languages_preserve_stored_selection(self):
        languages = {code for code, _ in launcher.UI_LANGUAGES if code}
        self.assertEqual(languages, set(translations.TRANSLATIONS))
        self.assertEqual(len(languages), 13)
        try:
            for language in languages:
                launcher.LANG = language
                table = launcher.bbport_lang.table(language)
                self.assertTrue(all(table.get(key) for key in translations.KEYS))
                self.assertNotEqual(launcher._('AMD DX12'), launcher._('Vulkan (fsr4vk)'))
                for selected in ('dx12', 'vk'):
                    saved = json.loads(json.dumps({'ui_language': language, 'fsr411_backend': selected}))
                    self.assertEqual(launcher.fsr411_backend(saved), selected)
        finally:
            launcher.LANG = 'en'


if __name__ == '__main__':
    unittest.main()
