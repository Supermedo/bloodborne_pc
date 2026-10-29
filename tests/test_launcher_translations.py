"""The launcher's translations (launcher/bbport_lang*.py): every translation formats with the same
values as its English text, keeps the status symbol the launcher reads, and the side bar, the
goal and the guide's steps are translated in every language."""
from paths import ROOT
import re
import string
import sys
import unittest

sys.path.insert(0, str(ROOT / 'launcher'))
sys.path.insert(0, str(ROOT / 'scripts'))
import bbport_launcher_win as L  # noqa: E402  (loads every translation module)
import bbport_lang  # noqa: E402
import bbport_setup  # noqa: E402

LANGUAGES = [code for code, _name in bbport_lang.LANGUAGE_NAMES if code not in ('', 'en')]
FORMAT = string.Formatter()


def launcher_texts():
    """[(file, line, English, Russian or None)] of every _() call with a written-out text in the launcher's
    windows (not the language modules, nor the Linux launcher)."""
    import ast
    found = []
    for path in sorted((ROOT / 'launcher').glob('bbport_*.py')):
        if path.name.startswith('bbport_lang') or path.name == 'bbport_launcher.py':
            continue
        for node in ast.walk(ast.parse(path.read_text(encoding='utf-8'))):
            if isinstance(node, ast.Call) and isinstance(node.func, ast.Name) and node.func.id == '_' \
                    and node.args and isinstance(node.args[0], ast.Constant) and isinstance(node.args[0].value, str) \
                    and node.args[0].value.strip():
                russian = node.args[1].value if len(node.args) > 1 and isinstance(node.args[1], ast.Constant) else None
                found.append((path.name, node.lineno, node.args[0].value, russian))
    return found


def sample(text):
    """Values for a text's fields in its order: a number where the format needs one."""
    return [1234.5 if spec and (spec[-1] in 'fdeg%' or ',' in spec) else 'x'
            for _l, name, spec, _c in FORMAT.parse(text) if name is not None]


class TranslationTests(unittest.TestCase):
    def test_placeholders(self):
        for language in LANGUAGES:
            for en, text in bbport_lang.table(language).items():
                if text and '{' in en:
                    with self.subTest(language=language, text=en):
                        values = sample(en)
                        en.format(*values)
                        text.format(*values)

    def test_status_symbols(self):
        for language in LANGUAGES:
            for en, text in bbport_lang.table(language).items():
                if text and en[:1] in '✓•⚠✗':
                    with self.subTest(language=language, text=en):
                        self.assertEqual(text[:1], en[:1])

    def test_the_main_texts_are_translated_everywhere(self):
        main = [title[0] for _name, title in L.NAV_SIMPLE + L.NAV_EXPERT]
        inline = {title[0]: title[1] for _name, title in L.NAV_SIMPLE + L.NAV_EXPERT if len(title) > 1}
        main += [f'{short} (goal)' for short, _long in L.Launcher.GOAL_TEXTS.values()]  # their own keys
        main += [plain for _key, _lore, plain in bbport_setup.STEPS]
        main += ['Expert view', 'Search…', 'Your goal', 'Made for your PC', 'Measure on this PC',
                 'Only with your own copy of the game']  # the piracy notice must be readable everywhere
        for language in LANGUAGES:
            table = bbport_lang.table(language)
            for text in main:
                if text in ('Mods',):  # the same word in many languages
                    continue
                with self.subTest(language=language, text=text):
                    # Russian may come with the text in the code: _('Play', 'Играть')
                    found = table.get(text) or (inline.get(text) if language == 'ru' else None)
                    self.assertTrue(found, f'{text!r} has no {language} translation')

    def test_every_text_is_translated_everywhere(self):
        """Every text the launcher shows through _('English'[, 'Russian']) and Help's topics, in every
        language: a new text without its translations shows English to everyone else (the Russian may
        stand in the call)."""
        import bbport_help
        # Help's topics are translated where they are shown: their texts count as well.
        topics = [(f'bbport_help.py TOPICS[{key}]', 0, text, None)
                  for key, (title, why, steps) in bbport_help.TOPICS.items() for text in (title, why, *steps)]
        topics += [('bbport_help.py TOPIC_ACTIONS', 0, text, None)
                   for actions in bbport_help.TOPIC_ACTIONS.values() for text, _method in actions]
        missing = {}
        for path, line, text, russian in launcher_texts() + topics:
            for language in LANGUAGES:
                if not bbport_lang.table(language).get(text) and not (language == 'ru' and russian):
                    missing.setdefault(f'{path}:{line} {text[:60]!r}', []).append(language)
        self.assertFalse(missing, 'texts without a translation (add them to a bbport_lang module):\n' +
                         '\n'.join(f'  {where}: {", ".join(languages)}' for where, languages in missing.items()))

    def test_the_processor_texts_are_translated_everywhere(self):
        import bbport_lang_cpu
        self.assertEqual(set(bbport_lang_cpu.TEXTS) - {'en'}, set(LANGUAGES))
        for language in LANGUAGES:
            for text in bbport_lang_cpu.KEYS:
                with self.subTest(language=language, text=text[:30]):
                    self.assertTrue(bbport_lang.table(language).get(text))

    def test_the_certificate_texts_are_translated_everywhere(self):
        """bbport_tls.explain's two texts, as the player reads them (Russian is beside the English)."""
        import bbport_lang_tls
        import bbport_tls
        from datetime import datetime, timezone
        from unittest import mock
        self.assertEqual(set(bbport_lang_tls.TEXTS) - {'en'}, set(LANGUAGES) - {'ru'})
        now = datetime(2026, 10, 22, 12, tzinfo=timezone.utc)
        clock = bbport_tls.Facts(datetime(2027, 1, 1, tzinfo=timezone.utc), datetime(2027, 4, 1, tzinfo=timezone.utc),
                                 "Let's Encrypt (YE2)")
        issuer = bbport_tls.Facts(datetime(2026, 1, 1, tzinfo=timezone.utc), datetime(2027, 1, 1, tzinfo=timezone.utc),
                                  'Test Antivirus Web Shield Root')
        english = {}
        for language in ['en'] + LANGUAGES:
            for name, facts in (('clock', clock), ('issuer', issuer)):
                with self.subTest(language=language, text=name), mock.patch.object(L, 'LANG', language), \
                        mock.patch.object(bbport_tls, 'certificate_facts', return_value=facts):
                    text = bbport_tls.explain([b'leaf'], 'thehuntersdream.com', now=now)
                    if language == 'en':
                        english[name] = text
                    else:
                        self.assertNotEqual(text, english[name])
                    self.assertIn(now.astimezone().strftime('%Y-%m-%d') if name == 'clock' else
                                  'Test Antivirus Web Shield Root', text)


if __name__ == '__main__':
    unittest.main()


class PiracyNoticeTests(unittest.TestCase):
    def test_every_language_names_rule_2_and_the_ban(self):
        source = (ROOT / 'launcher' / 'bbport_setup.py').read_text(encoding='utf-8')
        start = source.index('tk.Label(words, text=_("The port does not come with the game')
        end = source.index(')', source.index('can get you banned', start))
        text = ''.join(a or b for a, b in re.findall(r"'([^']*)'|\"([^\"]*)\"", source[start:end]))
        self.assertIn("can't give out game files", text)
        self.assertIn('rule 2', text)
        for language in LANGUAGES:
            with self.subTest(language=language):
                self.assertTrue(re.search(r'2', bbport_lang.table(language).get(text, '')), language)

