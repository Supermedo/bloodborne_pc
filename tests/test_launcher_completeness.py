"""Is every launcher function complete? Run after every larger change (tools/check_launcher.py runs this
with the other launcher tests and prints a summary).

Statically, over launcher/bbport_*.py: every attribute a class reads from `self` (a widget, a method, a
setting) is made somewhere: assigned, defined as a method, or a class attribute. The launcher's mixins
count as one class, and what the setup guide reads from the launcher (`app.x`) is checked against it.
This is the check that finds an `update notice that packs before self.side_note` after the side bar
lost side_note.

Live, on Windows with a display, on copies only (nothing is written to the player's files): the
launcher opens every page in both views and the setup guide's steps, shows the update notice, lists
Advanced > Other versions, and keeps its background loop running past a failing call. Any error a Tk
callback raises on the way fails the check."""
from paths import ROOT
import ast
import sys
import tempfile
import time
import unittest
from pathlib import Path

sys.path.insert(0, str(ROOT / 'launcher'))
sys.path.insert(0, str(ROOT / 'scripts'))

LAUNCHER = ROOT / 'launcher'
# One object at run time: Launcher(SetupMixin, HelpMixin, SearchMixin, PadMixin).
LAUNCHER_GROUP = {'Launcher', 'SetupMixin', 'HelpMixin', 'SearchMixin', 'PadMixin'}
SKIP_FILES = {'bbport_launcher.py'}  # the Linux launcher (GTK): a program of its own


def sources():
    return [p for p in sorted(LAUNCHER.glob('bbport_*.py'))
            if not p.name.startswith('bbport_lang') and p.name not in SKIP_FILES]


def is_self(node, name='self'):
    return isinstance(node, ast.Name) and node.id == name


def app_reference(node):
    """`app` or `self.app`: the launcher, inside a helper class that keeps it as self.app."""
    return is_self(node, 'app') or (isinstance(node, ast.Attribute) and node.attr == 'app' and is_self(node.value))


class ClassInfo:
    def __init__(self, name, path):
        self.name, self.paths = name, {path}
        self.defined, self.used = set(), {}  # used: name -> 'file:line'
        self.external_bases = False
        self.keeps_app = False  # self.app is the launcher
        self.app_used = {}


def collect():
    """{class name or 'Launcher' for the group: ClassInfo}, and the launcher attributes other code sets."""
    classes, app_set = {}, set()
    for path in sources():
        tree = ast.parse(path.read_text(encoding='utf-8'), filename=str(path))
        local = {node.name for node in tree.body if isinstance(node, ast.ClassDef)}
        for node in tree.body:
            if not isinstance(node, ast.ClassDef):
                continue
            key = 'Launcher' if node.name in LAUNCHER_GROUP else f'{path.name}:{node.name}'
            info = classes.setdefault(key, ClassInfo(key, path.name))
            info.paths.add(path.name)
            for base in node.bases:
                if not (isinstance(base, ast.Name) and (base.id in local or base.id in LAUNCHER_GROUP)
                        or isinstance(base, ast.Attribute) and base.attr in LAUNCHER_GROUP):
                    if not (isinstance(base, ast.Name) and base.id == 'object'):
                        info.external_bases = True
            for item in node.body:
                if isinstance(item, (ast.FunctionDef, ast.AsyncFunctionDef)):
                    info.defined.add(item.name)
                elif isinstance(item, ast.Assign):
                    info.defined.update(t.id for t in item.targets if isinstance(t, ast.Name))
                elif isinstance(item, ast.AnnAssign) and isinstance(item.target, ast.Name):
                    info.defined.add(item.target.id)
            for sub in ast.walk(node):
                if isinstance(sub, ast.Attribute) and is_self(sub.value):
                    if isinstance(sub.ctx, (ast.Store, ast.Del)):
                        info.defined.add(sub.attr)
                        if sub.attr == 'app' and key != 'Launcher':
                            info.keeps_app = True
                    elif not sub.attr.startswith('__'):
                        info.used.setdefault(sub.attr, f'{path.name}:{sub.lineno}')
                elif isinstance(sub, ast.Attribute) and app_reference(sub.value) and key != 'Launcher':
                    if isinstance(sub.ctx, (ast.Store, ast.Del)):
                        app_set.add(sub.attr)
                    else:
                        info.app_used.setdefault(sub.attr, f'{path.name}:{sub.lineno}')
    return classes, app_set


def missing_attributes():
    """['Launcher.side_note (bbport_launcher_win.py:3750)', …]: attributes read but never made."""
    classes, app_set = collect()
    launcher = classes['Launcher']
    available = launcher.defined | app_set
    problems = [f'Launcher.{name} ({where})' for name, where in sorted(launcher.used.items())
                if name not in available]
    for key, info in sorted(classes.items()):
        if key == 'Launcher':
            continue
        if not info.external_bases:
            problems += [f'{key}.{name} ({where})' for name, where in sorted(info.used.items())
                         if name not in info.defined]
        if info.keeps_app:
            problems += [f'{key}: launcher.{name} ({where})' for name, where in sorted(info.app_used.items())
                         if name not in available]
    return problems


class StaticTests(unittest.TestCase):
    def test_every_attribute_read_is_made_somewhere(self):
        self.assertEqual(missing_attributes(), [])

    def test_the_check_finds_a_missing_attribute(self):
        """The check itself: an attribute only read (like side_note after the side bar changed) is found."""
        source = 'class Launcher:\n    def offer(self):\n        self.box.pack(before=self.side_note)\n' \
                 '    def build(self):\n        self.box = 1\n'
        with tempfile.TemporaryDirectory() as folder:
            (Path(folder) / 'bbport_fake.py').write_text(source, encoding='utf-8')
            global LAUNCHER
            saved, LAUNCHER = LAUNCHER, Path(folder)
            try:
                self.assertEqual([p.split(' ')[0] for p in missing_attributes()], ['Launcher.side_note'])
            finally:
                LAUNCHER = saved


@unittest.skipUnless(sys.platform == 'win32', 'the launcher window is Windows only')
class LiveTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        try:
            import tkinter as tk
            from tkinter import filedialog, messagebox, ttk
            cls.root = tk.Tk()
        except Exception:  # no display
            raise unittest.SkipTest('no display') from None
        import bbport_launcher_win as L
        cls.L = L
        cls.temp = tempfile.TemporaryDirectory()
        cls.saved = {name: getattr(L, name) for name in ('CONFIG_FILE', 'LANG', 'installable_releases')}
        L.CONFIG_FILE = Path(cls.temp.name) / 'settings.json'  # a fresh launcher; nothing of the player's
        L.LANG = 'en'
        patches = [('check_update', lambda self, manual=False: None), ('collect', lambda self: None),
                   ('maybe_open_setup', lambda self: None)]
        cls.originals = {name: getattr(L.Launcher, name) for name, _f in patches}
        for name, function in patches:
            setattr(L.Launcher, name, function)
        cls.errors = []
        cls.root.report_callback_exception = lambda kind, value, trace: cls.errors.append(f'{kind.__name__}: {value}')
        messagebox.askyesno = lambda *a, **k: False  # never confirms a dialog during the check
        cls.root.withdraw()
        cls.app = L.Launcher(cls.root, tk, ttk, filedialog, messagebox)
        cls.root.geometry('1280x900+0+0')
        cls.root.deiconify()
        cls.pump(3)

    @classmethod
    def tearDownClass(cls):
        cls.root.destroy()
        for name, function in cls.originals.items():
            setattr(cls.L.Launcher, name, function)
        for name, value in cls.saved.items():
            setattr(cls.L, name, value)
        cls.temp.cleanup()

    @classmethod
    def pump(cls, seconds):
        end = time.time() + seconds
        while time.time() < end:
            cls.root.update()
            time.sleep(0.01)

    def setUp(self):
        self.errors.clear()

    def tearDown(self):
        self.assertEqual(self.errors, [], 'errors in Tk callbacks')

    def test_every_page_opens_in_both_views(self):
        for mode in ('simple', 'expert'):
            self.app.expert_var.set(mode == 'expert')
            self.app.toggle_mode()
            for name, _title in self.L.NAV_SIMPLE + (self.L.NAV_EXPERT if mode == 'expert' else []):
                with self.subTest(view=mode, page=name):
                    self.app.show(name)
                    self.pump(0.3)
                    self.assertEqual(self.app.current_page, name)

    def test_the_update_notice_shows(self):
        self.app.offer_update('99.0', 'https://example.invalid/update.zip', 'https://example.invalid/')
        self.pump(0.3)
        self.assertTrue(self.app.update_box and self.app.update_box.winfo_ismapped())
        self.app.update_box.destroy()
        self.app.update_box = None

    def test_a_failing_background_call_does_not_stop_the_loop(self):
        ran = []
        self.app.ui_calls.put(lambda: 1 / 0)
        self.app.ui_calls.put(lambda: ran.append(True))
        self.pump(0.5)
        self.assertEqual(ran, [True])
        self.assertEqual(len(self.errors), 1)  # the failing call is reported, the next one still ran
        self.errors.clear()

    def test_other_versions_lists_them(self):
        self.L.installable_releases = lambda: [('9.9.9', 'https://example.invalid/a.zip', 'https://example.invalid/'),
                                               (self.L.VERSION, 'https://example.invalid/b.zip', 'https://example.invalid/')]
        try:
            self.app.choose_version()
            self.pump(1)
            window = self.app.version_window
            boxes = [w for w in self.widgets(window) if w.winfo_class() == 'TCombobox']
            self.assertTrue(boxes and len(boxes[0].cget('values')) == 2, 'the list did not arrive')
            window.destroy()
        finally:
            self.L.installable_releases = self.saved['installable_releases']

    def test_the_setup_guide_steps_through(self):
        import bbport_setup
        self.app.open_setup()
        self.pump(0.5)
        guide = self.app.guide
        try:
            for step in range(len(bbport_setup.STEPS)):
                with self.subTest(step=bbport_setup.STEPS[step][0]):
                    guide.show(step)
                    self.pump(0.5)
        finally:
            guide.closed = True
            guide.frame.destroy()

    def widgets(self, parent):
        for child in parent.winfo_children():
            yield child
            yield from self.widgets(child)


if __name__ == '__main__':
    unittest.main()
