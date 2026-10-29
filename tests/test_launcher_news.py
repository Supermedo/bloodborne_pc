"""What's new (launcher/bbport_news.py): notes for the version being built, so a version bump does not ship
without them, and the window shown once per version (VERSION_TEXT: every beta counts)."""
from paths import ROOT
import sys
import unittest

sys.path.insert(0, str(ROOT / 'launcher'))
import bbport_launcher_win as L  # noqa: E402  (one copy: a second would bind the shared modules to itself)
import bbport_news  # noqa: E402


class FakeApp:
    def __init__(self, seen, guide=None):
        self.app, self.guide, self.opened = {'seen_version': seen}, guide, 0

    def open_news(self):
        self.opened += 1


class NewsTests(unittest.TestCase):
    def test_this_version_has_notes(self):
        notice, sections = bbport_news.notes(L.VERSION_TEXT)
        self.assertTrue(sections and all(title and items for title, items in sections))
        self.assertTrue(notice is None or notice())

    def test_shown_once_per_version(self):
        for seen, opened in (('1.9.4', 1), ('1.9.5 beta 9', 1), ('', 1), (L.VERSION_TEXT, 0)):
            app = FakeApp(seen)
            L.Launcher.maybe_show_news(app)
            self.assertEqual(app.opened, opened, seen)

    def test_the_setup_guide_counts_as_seeing_it(self):
        app = FakeApp('', guide=object())
        L.Launcher.maybe_show_news(app)
        self.assertEqual((app.opened, app.app['seen_version']), (0, L.VERSION_TEXT))


if __name__ == '__main__':
    unittest.main()
