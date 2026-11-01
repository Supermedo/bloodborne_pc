# SPDX-License-Identifier: GPL-2.0-or-later
"""R80: Windows refused bin\\bbport_dlss.dll (Smart App Control, an antivirus) and DLSS fell back to FSR 3.1
without a word, and the in-game menu then saved FSR 3.1 over DLSS for good. PLAY now tries the DLL first:
refused, the start plays with FSR 3.1 (BB_DLSS=0), says why once per error, and DLSS stays chosen."""
from paths import ROOT
import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(ROOT / 'launcher'))
import bbport_help as help_  # noqa: E402
import bbport_lang_dlss  # noqa: E402
import bbport_launcher_win as L  # noqa: E402


def bare_launcher(upscaler='dlss', frame_gen='off'):
    ui = L.Launcher.__new__(L.Launcher)
    ui.app = dict(L.APP_DEFAULTS)
    ui.ini = {'upscaler': upscaler, 'frame_gen': frame_gen}
    ui.logged = []
    ui.append = ui.logged.append
    ui.messagebox = mock.Mock()
    return ui


class DllProblemTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.port = Path(self.tmp.name)
        (self.port / 'bin').mkdir()

    def test_without_the_bridge_nothing_to_say(self):
        self.assertIsNone(help_.dlss_problem(self.port))
        self.assertIsNone(help_.dlss_problem(None))

    def test_the_causes(self):
        (self.port / 'bin' / 'bbport_dlss.dll').write_bytes(b'x')
        for error, words in ((4551, 'Smart App Control'), (4558, 'Smart App Control'), (225, 'antivirus'),
                             (126, 'missing'), (5, 'could not load')):
            with mock.patch.dict(os.environ, {'BB_TEST_DLSS_LOAD_ERROR': str(error)}):
                code, why = help_.dlss_problem(self.port)
            self.assertEqual(code, error)
            self.assertIn(words, why)

    @unittest.skipUnless(os.name == 'nt', 'Windows loader')
    def test_a_real_load(self):
        (self.port / 'bin' / 'bbport_dlss.dll').write_bytes(b'not a program')
        with mock.patch.dict(os.environ, {'BB_TEST_DLSS_LOAD_ERROR': ''}):
            self.assertEqual(help_.dlss_problem(self.port)[0], 193)  # ERROR_BAD_EXE_FORMAT
            (self.port / 'bin' / 'bbport_dlss.dll').write_bytes(Path(sys.executable).read_bytes())
            self.assertIsNone(help_.dlss_problem(self.port))  # a program Windows loads


class PlayTests(unittest.TestCase):
    def blocked(self, error=4551):
        return mock.patch.object(help_, 'dlss_problem', return_value=(error, help_.DLSS_BLOCKED[0][1]))

    def test_refused_this_start_plays_fsr3_and_says_why_once(self):
        ui = bare_launcher()
        with self.blocked():
            env = ui.dlss_check({})
            self.assertEqual(env['BB_DLSS'], '0')
            self.assertIn('DLSS cannot start (Windows error 4551). Windows blocked bbport_dlss.dll', ui.logged[0])
            self.assertIn('DLSS stays chosen', ui.logged[0])
            ui.messagebox.showwarning.assert_called_once()
            self.assertEqual(ui.dlss_check({})['BB_DLSS'], '0')
        ui.messagebox.showwarning.assert_called_once()  # the second start: in the log only
        self.assertEqual(len(ui.logged), 2)
        with self.blocked(225):
            ui.dlss_check({})
        self.assertEqual(ui.messagebox.showwarning.call_count, 2)  # another cause: said again

    def test_dlss_frame_generation_too(self):
        ui = bare_launcher(upscaler='fsr4', frame_gen='dlss')
        with self.blocked():
            self.assertEqual(ui.dlss_check({})['BB_DLSS'], '0')

    def test_without_dlss_chosen_the_dll_is_not_tried(self):
        ui = bare_launcher(upscaler='fsr3')
        with mock.patch.object(help_, 'dlss_problem') as tried:
            self.assertEqual(ui.dlss_check({'A': '1'}), {'A': '1'})
        tried.assert_not_called()

    def test_loading_it_leaves_the_start_alone(self):
        ui = bare_launcher()
        with mock.patch.object(help_, 'dlss_problem', return_value=None):
            self.assertNotIn('BB_DLSS', ui.dlss_check({}))
        ui.messagebox.showwarning.assert_not_called()

    def test_translated(self):
        for language, texts in bbport_lang_dlss.TEXTS.items():
            self.assertIn('{}', texts[0], language)
        for _codes, why in help_.DLSS_BLOCKED:
            self.assertIn(why, bbport_lang_dlss.KEYS)


class GameSideTests(unittest.TestCase):
    """The game's half (gpu/): the load error in the log, and Save() keeping DLSS while FSR 3.1 stands in."""

    def test_sources(self):
        dlss = (ROOT / 'gpu/shadps4/video_core/renderer_vulkan/vk_dlss.cpp').read_text(encoding='utf-8')
        self.assertIn('LoadProblem(GetLastError())', dlss)
        settings = (ROOT / 'gpu/shim/bbport_settings.cpp').read_text(encoding='utf-8')
        self.assertIn('v.dlss_upscaler_fallback && v.upscaler == UpscalerFsr3', settings)
        self.assertIn('UpscalerName(upscaler)', settings)
        self.assertIn('FrameGenName(frame_gen)', settings)


if __name__ == '__main__':
    unittest.main()
