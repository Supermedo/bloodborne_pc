"""R89: model detail "Highest" (-2) breaks some armor and capes. The automatic settings no longer choose it, and a
-2 the launcher chose before (bbport_setup.drop_automatic_highest_lod) becomes "As in the game"; one the player
chose by hand stays."""
from paths import ROOT
import sys
import types
import unittest

sys.path.insert(0, str(ROOT / 'launcher'))
sys.path.insert(0, str(ROOT / 'scripts'))
import bbport_launcher_win  # noqa: E402,F401  (binds the setup module to the launcher)
import bbport_lang  # noqa: E402
import bbport_setup  # noqa: E402


class Window(bbport_setup.SetupMixin):
    def __init__(self, lod, applied):
        self.values = {'model_lod': lod}
        self.var = lambda key, store: types.SimpleNamespace(get=lambda: self.values[key],
                                                            set=lambda v: self.values.__setitem__(key, v))
        self.app = {'auto_applied': applied}

    def refresh_status(self):
        pass


class ModelLodTests(unittest.TestCase):
    def test_a_highest_the_launcher_chose_becomes_as_in_the_game(self):
        window = Window('-2', {'model_lod': '-2', 'preset': '1'})
        window.drop_automatic_highest_lod()
        self.assertEqual(window.values['model_lod'], '0')
        self.assertEqual(window.app['auto_applied'], {'model_lod': '0', 'preset': '1'})

    def test_a_highest_the_player_chose_stays(self):
        for applied in ({'model_lod': '0'}, {}, None):
            window = Window('-2', applied)
            window.drop_automatic_highest_lod()
            self.assertEqual(window.values['model_lod'], '-2', applied)
            self.assertEqual(window.app['auto_applied'], applied)

    def test_a_highest_the_player_left_already(self):
        window = Window('1', {'model_lod': '-2'})
        window.drop_automatic_highest_lod()
        self.assertEqual(window.values['model_lod'], '1')

    def test_the_warning_beside_the_option_is_translated_everywhere(self):
        key = 'A game patch (game version 1.09). "Highest" can break armor and capes: parts go missing or look wrong.'
        self.assertIn(key, (ROOT / 'launcher' / 'bbport_launcher_win.py').read_text(encoding='utf-8')
                      .replace("'\n                   '", ''))
        for language, _name in bbport_lang.LANGUAGE_NAMES:
            if language in ('', 'en', 'ru'):
                continue
            with self.subTest(language=language):
                self.assertTrue(bbport_lang.table(language).get(key))


if __name__ == '__main__':
    unittest.main()
