"""Small launcher fixes that work without a window: next_row keeps the row of a widget grid_remove took away, a URLError's text says why,
and Ctrl+F leaves the search field alone under What's new."""
from paths import ROOT
import ast
import importlib.util
import sys
import unittest
import urllib.error

LAUNCHER = ROOT / 'launcher'
sys.path.insert(0, str(LAUNCHER))
WIN = ast.parse((LAUNCHER / 'bbport_launcher_win.py').read_text(encoding='utf-8'))


def from_source(name, class_name=None):
    """A function by itself, from the file's text."""
    body = WIN.body
    if class_name:
        body = next(n for n in body if isinstance(n, ast.ClassDef) and n.name == class_name).body
    function = next(n for n in body if isinstance(n, ast.FunctionDef) and n.name == name)
    space = {}
    exec(compile(ast.Module([function], []), name, 'exec'), space)
    return space[name]


class Parent:
    """grid_size() as Tk counts: a widget grid_remove() took away is not in it."""

    def __init__(self):
        self.rows = 0

    def grid_size(self):
        return 1, self.rows


class SmallFixes(unittest.TestCase):
    def test_a_hidden_widget_keeps_its_row(self):
        next_row = from_source('next_row', 'Launcher')
        parent = Parent()
        first = next_row(None, parent)
        parent.rows = first + 1  # gridded, ...
        parent.rows = first  # ... and grid_remove()d
        self.assertEqual((first, next_row(None, parent)), (0, 1))
        parent.rows = 3  # a row and the hint on the row after it
        self.assertEqual(next_row(None, parent), 3)

    def test_a_urlerror_says_why(self):
        error_text = from_source('error_text')
        self.assertEqual(error_text(urllib.error.URLError(OSError('getaddrinfo failed'))), 'getaddrinfo failed')
        self.assertEqual(error_text(urllib.error.URLError('no route')), 'no route')
        http = urllib.error.HTTPError('https://x', 404, 'Not Found', None, None)
        self.assertEqual(error_text(http), 'HTTP Error 404: Not Found')
        self.assertEqual(error_text(OSError('timed out')), 'timed out')

    def test_ctrl_f_under_whats_new(self):
        spec = importlib.util.spec_from_file_location('bbport_search', LAUNCHER / 'bbport_search.py')
        search = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(search)

        class Covered:
            news_panel = object()
            guide = None
            remap_listening = False
        self.assertIsNone(search.SearchMixin.search_focus(Covered()))


if __name__ == '__main__':
    unittest.main()
