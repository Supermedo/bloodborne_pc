"""Chinese menu/font preferences do not change the game's language or other UI fonts."""
from paths import ROOT
import sys
import unittest
from unittest import mock

sys.path.insert(0, str(ROOT / 'launcher'))
import bbport_theme as theme

class ChineseFontTests(unittest.TestCase):
    def setUp(self):
        self.keys = ('HEAD', 'HEAD_BOLD', 'BODY', 'BODY_MEDIUM', 'BODY_SEMI', 'ICONS', 'LANGUAGE')
        self.old = {key: getattr(theme, key) for key in self.keys}
    def tearDown(self):
        for key, value in self.old.items(): setattr(theme, key, value)
    def test_chinese_uses_installed_yahei(self):
        with mock.patch('tkinter.font.families', return_value=['Inter', 'Microsoft YaHei UI']):
            theme.load_fonts(object(), 'zh')
        self.assertEqual(theme.BODY, 'Microsoft YaHei UI')
        self.assertEqual(theme.HEAD_BOLD, 'Microsoft YaHei UI')
    def test_english_keeps_latin_fonts(self):
        with mock.patch('tkinter.font.families', return_value=['Inter', 'Cinzel SemiBold', 'Microsoft YaHei UI']):
            theme.load_fonts(object(), 'en')
        self.assertEqual(theme.BODY, 'Inter')
        self.assertEqual(theme.HEAD, 'Cinzel SemiBold')
    def test_noto_is_a_cjk_fallback(self):
        with mock.patch('tkinter.font.families', return_value=['Inter', 'Noto Sans CJK SC']):
            theme.load_fonts(object(), 'zh')
        self.assertEqual(theme.BODY, 'Noto Sans CJK SC')

if __name__ == '__main__': unittest.main()
