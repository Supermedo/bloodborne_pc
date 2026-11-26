"""Korean in the launcher (launcher/bbport_lang_ko.py and the code around it): the table wins for Korean only and every key
is a string in the launcher's code; search keeps Hangul whole and leaves particles out; dates and the game's own words."""
from paths import ROOT
import ast
import datetime
import importlib.util
import json
import re
import string
import sys
import types
import unittest

LAUNCHER = ROOT / 'launcher'
sys.path.insert(0, str(LAUNCHER))
import bbport_launcher_win as L  # noqa: E402  (loads every translation module)
import bbport_character as character  # noqa: E402
import bbport_lang  # noqa: E402
import bbport_lang_ko  # noqa: E402
import bbport_saves as saves  # noqa: E402


def load(name):
    spec = importlib.util.spec_from_file_location(name, LAUNCHER / f'{name}.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


search = load('bbport_search')
OVERRIDES = bbport_lang_ko.KO_OVERRIDES


class TableTests(unittest.TestCase):
    def test_the_table_wins_for_korean_only(self):
        korean = bbport_lang.table('ko')
        for key, value in OVERRIDES.items():
            self.assertEqual(korean.get(key), value, key)
        for code, _name in bbport_lang.LANGUAGE_NAMES:
            if code not in ('', 'en', 'ko'):
                table = bbport_lang.table(code)
                self.assertFalse([k for k in OVERRIDES if re.search('[\uac00-\ud7a3]', table.get(k) or '')], code)

    def test_every_key_is_a_text_the_launcher_has(self):
        constants = set()
        for path in sorted(LAUNCHER.glob('bbport_*.py')):
            if not path.name.startswith('bbport_lang') and path.name != 'bbport_launcher.py':
                constants |= {node.value for node in ast.walk(ast.parse(path.read_text(encoding='utf-8')))
                              if isinstance(node, ast.Constant) and isinstance(node.value, str)}
        for path in (LAUNCHER / 'assets' / 'tests').glob('*.json'):  # the scenarios' names and descriptions
            scenario = json.loads(path.read_text(encoding='utf-8'))
            constants |= {scenario.get('name'), scenario.get('description')}
        self.assertEqual([k for k in OVERRIDES if k not in constants], [])

    def test_placeholders_stay(self):
        fields = string.Formatter()
        for key, value in OVERRIDES.items():
            english = [name for _l, name, _s, _c in fields.parse(key) if name is not None]
            korean = [name for _l, name, _s, _c in fields.parse(value) if name is not None]
            self.assertEqual(len(english), len(korean), key)
            value.format(*['x'] * len(english))

    def test_the_games_words(self):
        for word, game in {'Blood Echoes': '\ud53c\uc758 \uc720\uc9c0', 'Insight': '\uacc4\ubabd',
                           'Bloodtinge': '\ud608\uc9c8'}.items():
            self.assertEqual(OVERRIDES[word], game)


class SearchTests(unittest.TestCase):
    def test_fold_keeps_hangul_whole(self):
        self.assertEqual(search.fold('\ub85c\uadf8\uc778'), '\ub85c\uadf8\uc778')
        self.assertEqual(search.fold('St\u00fcrzt AB'), 'sturzt ab')

    def test_a_particle_is_left_out(self):
        self.assertEqual(search.near('\ud574\uc0c1\ub3c4\ub97c', '\ud574\uc0c1\ub3c4 \ucd9c\ub825'), '\ud574\uc0c1\ub3c4')
        self.assertEqual(search.near('dlss\ub97c', 'dlss x'), 'dlss')
        self.assertIsNone(search.near('\ub85c\uadf8\uc778', '\ub85c\uadf8\uc5d0 x'))
        self.assertEqual(search.near('details', 'detail x'), 'detail')


class WordsTests(unittest.TestCase):
    def test_dates(self):
        moment = datetime.datetime(2026, 11, 1, 12, 33)
        before = saves.LANGUAGE
        try:
            saves.LANGUAGE = 'ko'
            self.assertEqual(saves.date_text(moment), '2026. 11. 01. 12:33')
            saves.LANGUAGE = 'en'
            self.assertEqual(saves.date_text(moment), '01.11.2026 12:33')
        finally:
            saves.LANGUAGE = before

    def test_key_names(self):
        before = L.LANG
        try:
            L.LANG = 'ko'
            self.assertEqual(L.key_text('Delete'), 'Delete')  # what is written on the key
            self.assertRegex(L.key_text('Space'), '[\uac00-\ud7a3]')
            L.LANG = 'en'
            self.assertEqual(L.key_text('Space'), 'Space')
        finally:
            L.LANG = before

    def test_the_cards_words_go_through_the_launcher(self):
        before = character.TRANSLATE
        try:
            character.TRANSLATE = 'T:{}'.format
            names = character.Names(menu={character.LEVEL_TEXT: 'x', character.CONTINUE_TEXT: 'y'})
            self.assertEqual(names.word(character.LEVEL_TEXT), 'T:x')
            self.assertEqual(names.word(character.CONTINUE_TEXT), 'y')  # as the title menu writes it
        finally:
            character.TRANSLATE = before

    def test_a_korean_launcher_reads_the_korean_words(self):
        values = {'game_dir': 'game', 'language': '1'}
        app = types.SimpleNamespace(var=lambda name, _group: types.SimpleNamespace(get=lambda: values[name]))
        before = L.LANG, character.names
        try:
            character.names = lambda folder, language: (folder, language)
            L.LANG = 'ko'
            self.assertEqual(L.game_names(app), ('game', '9'))
            L.LANG = 'de'
            self.assertEqual(L.game_names(app), ('game', '1'))
        finally:
            L.LANG, character.names = before

    def test_the_korean_message_folder(self):
        self.assertEqual(character.MSG_FOLDERS.get('9'), 'korkr')
        self.assertEqual(character.Names(menu={character.LEVEL_TEXT: 'x'}).word(character.LEVEL_TEXT), 'x')


if __name__ == '__main__':
    unittest.main()
