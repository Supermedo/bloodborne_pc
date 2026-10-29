"""Exercise the achievement page and notification text without opening a Tk window."""
from paths import ROOT
import sys
import time
from types import SimpleNamespace
import unittest

sys.path.insert(0, str(ROOT / 'launcher'))
# The launcher itself, not a second copy: loading it again rebinds the modules it hands itself
# to (bbport_tls.bind), which other tests then patch in vain.
import bbport_launcher_win as launcher  # noqa: E402


class Widget:
    def __init__(self, parent=None, **options):
        self.children = []
        self.options = options
        if parent is not None:
            parent.children.append(self)

    def winfo_children(self):
        return self.children

    def pack(self, **options):
        pass

    def bind(self, *args):
        pass

    def place(self, **options):
        pass

    def lift(self):
        pass

    def create_rectangle(self, *args, **options):
        pass


def labels(widget):
    return ([widget.options['text']] if 'text' in widget.options else []) + [
        text for child in widget.children for text in labels(child)]


class TrophyLocalizationTests(unittest.TestCase):
    def setUp(self):
        previous = launcher.LANG
        self.addCleanup(setattr, launcher, 'LANG', previous)
        launcher.LANG = 'zh'
        self.app = object.__new__(launcher.Launcher)
        self.app.tk = SimpleNamespace(Frame=Widget, Label=Widget, Canvas=Widget)
        self.app.ttk = self.app.tk
        self.app.px = lambda value: value
        self.icon_ids = []
        def icon(parent, *args, **kwargs):
            self.icon_ids.append(kwargs.get('trophy_id'))
            return Widget(parent)
        self.app.trophy_icon = icon
        self.app.var = lambda *args: SimpleNamespace(get=lambda: 'game')
        self.app.trophy_body = Widget()
        self.app.trophy_state = None
        self.app.trophy_filter = 'all'
        self.app.tracker = SimpleNamespace(unlocked={14: 0})
        self.app.content = Widget()
        self.app.root = SimpleNamespace(after=lambda *args: None)

    def test_all_base_and_dlc_trophies_are_translated_on_the_page(self):
        self.app.render_trophies()
        text = labels(self.app.trophy_body)
        self.assertEqual([trophy for trophy in self.icon_ids if trophy is not None], list(range(40)))
        for _id, name, description, _grade, _group in launcher.trophies.TROPHIES:
            with self.subTest(trophy=_id):
                translated_name = launcher._(name)
                translated_description = launcher._(description)
                # The PSN platinum trophy intentionally retains "Bloodborne".
                self.assertIn(name, launcher.bbport_lang.table('zh'))
                self.assertNotEqual(translated_description, description)
                self.assertIn(translated_name, text)
                self.assertIn(translated_description, text)
        for expected in ('Bloodborne', '老猎人',
                         launcher._('{} of {} trophies earned').format(1, 40),
                         f"{launcher._('All')}   40",
                         f"{launcher._('Earned')}   1",
                         f"{launcher._('Locked')}   39",
                         launcher._('Locked'),
                         launcher._('Bronze'),
                         launcher._('Platinum')):
            self.assertIn(expected, text)
        self.assertIn(time.strftime('%Y年%m月%d日', time.localtime(0)), text)

    def test_multilingual_trophy_translations_coverage(self):
        supported_langs = ('zh', 'ru', 'ja', 'ko', 'de', 'fr', 'es', 'it', 'pt', 'pl', 'tr')
        for lang in supported_langs:
            table = launcher.bbport_lang.table(lang)
            for _id, name, description, _grade, _group in launcher.trophies.TROPHIES:
                with self.subTest(lang=lang, trophy=_id):
                    self.assertIn(name, table)
                    self.assertIn(description, table)
                    self.assertTrue(len(table[name]) > 0)
                    self.assertTrue(len(table[description]) > 0)

    def test_all_supported_languages_render_translated_trophy_cards(self):
        expected_gascoigne = {
            'zh': '加斯科因神父',
            'ru': 'Отец Гаскойн',
            'ja': 'ガスコイン神父',
            'ko': '개스코인 신부',
            'de': 'Pater Gascoigne',
            'fr': 'Père Gascoigne',
            'es': 'Padre Gascoigne',
            'it': 'Padre Gascoigne',
            'pt': 'Padre Gascoigne',
            'pl': 'Ojciec Gascoigne',
            'tr': 'Peder Gascoigne',
        }
        for lang, expected_name in expected_gascoigne.items():
            with self.subTest(language=lang):
                launcher.LANG = lang
                body = Widget()
                self.app.trophy_card(body, launcher.trophies.BY_ID[14], None)
                card_labels = labels(body)
                self.assertIn(expected_name, card_labels)

    def test_psn_wording_keeps_names_punctuation_and_line_breaks(self):
        # Chinese
        launcher.LANG = 'zh'
        self.assertEqual(launcher._('Bloodborne'), 'Bloodborne')
        self.assertEqual(launcher._('Yharnam Sunrise'), '亚南日出')
        self.assertEqual(launcher._('Honoring Wishes'), '履行愿望')
        self.assertEqual(launcher._('Yharnam, Pthumerian Queen'), '亚南，修梅里安女王')
        self.assertEqual(launcher._('Lady Maria of the Astral Clocktower'), '精神钟塔的玛利亚修女')
        self.assertEqual(launcher._('All trophies acquired. Hats off!'), '已获得所有战利品。脱帽!')
        self.assertEqual(
            launcher._("Captivated by the moon presence, you pledge to watch over the hunter's dream."),
            '因月亮的出现而被迷惑，\n你发誓要照看好猎人之梦。')

        # Russian
        launcher.LANG = 'ru'
        self.assertEqual(launcher._('Yharnam Sunrise'), 'Ярнамский восход')
        self.assertEqual(launcher._('All trophies acquired. Hats off!'), 'Получены все трофеи. Снимаем шляпу!')

        # Japanese
        launcher.LANG = 'ja'
        self.assertEqual(launcher._('Yharnam Sunrise'), 'ヤーナムの夜明け')
        self.assertEqual(launcher._('All trophies acquired. Hats off!'), 'すべてのトロフィーを得た証。おめでとう')

        # German
        launcher.LANG = 'de'
        self.assertEqual(launcher._('Yharnam Sunrise'), 'Yharnam-Sonnenaufgang')
        self.assertEqual(launcher._('All trophies acquired. Hats off!'), 'Alle Trophäen erhalten. Hut ab!')

        # Korean
        launcher.LANG = 'ko'
        self.assertEqual(launcher._('Yharnam Sunrise'), '야남의 새벽')
        self.assertEqual(launcher._('All trophies acquired. Hats off!'), '모든 트로피를 획득한 증표. 축하한다')

    def test_date_formatting_by_language(self):
        for lang, expected_date in (
            ('zh', time.strftime('%Y年%m月%d日', time.localtime(0))),
            ('ja', time.strftime('%Y年%m月%d日', time.localtime(0))),
            ('ko', time.strftime('%Y. %m. %d.', time.localtime(0))),
            ('en', time.strftime('%d %b %Y', time.localtime(0))),
            ('de', time.strftime('%d %b %Y', time.localtime(0))),
        ):
            with self.subTest(language=lang):
                launcher.LANG = lang
                body = Widget()
                self.app.trophy_card(body, launcher.trophies.BY_ID[14], 0)
                self.assertIn(expected_date, labels(body))

    def test_notification_and_unknown_trophy_fallback(self):
        launcher.LANG = 'zh'
        self.app.toasts = [14]
        self.app.show_toast()
        self.assertEqual(self.icon_ids[-1], 14)
        self.assertIn(launcher._("You've earned a trophy."), labels(self.app.content))
        self.assertIn('加斯科因神父', labels(self.app.content))
        self.app.content = Widget()
        self.app.toasts = [999]
        self.app.show_toast()
        self.assertEqual(self.icon_ids[-1], 999)
        self.assertIn('奖杯 999', labels(self.app.content))

    def test_empty_filters_use_chinese_messages(self):
        launcher.LANG = 'zh'
        self.app.tracker.unlocked = {}
        self.app.trophy_filter = 'earned'
        self.app.render_trophies()
        self.assertIn(launcher._('No trophies yet. They appear here as soon as the game unlocks them.'),
                      labels(self.app.trophy_body))
        self.app.trophy_body = Widget()
        self.app.tracker.unlocked = {t[0]: 0 for t in launcher.trophies.TROPHIES}
        self.app.trophy_filter = 'locked'
        self.app.render_trophies()
        self.assertIn(launcher._('Every trophy is earned. Hail, good hunter.'), labels(self.app.trophy_body))

    def test_english_and_unsupported_languages_keep_english_fallback(self):
        for language in ('en', 'unsupported'):
            with self.subTest(language=language):
                launcher.LANG = language
                body = Widget()
                self.app.trophy_card(body, launcher.trophies.BY_ID[14], None)
                self.assertIn('Father Gascoigne', labels(body))
                self.assertIn('Defeat the beast that once was Father Gascoigne.', labels(body))


if __name__ == '__main__':
    unittest.main()
