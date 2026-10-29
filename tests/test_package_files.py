"""scripts/package_files.py: the program files of one version. A test build unpacked over an older version stopped
the game at a black screen ("bad patch entry"); the list of the package's files finds that before."""
from paths import ROOT
import importlib.util
import json
import sys
import tempfile
import unittest
from pathlib import Path

import package_files as P

sys.path.insert(0, str(ROOT / 'launcher'))
spec = importlib.util.spec_from_file_location('bbport_help_files', ROOT / 'launcher/bbport_help.py')
help_ = importlib.util.module_from_spec(spec)
spec.loader.exec_module(help_)
run_spec = importlib.util.spec_from_file_location('bbport_run_files', ROOT / 'run.py')
run = importlib.util.module_from_spec(run_spec)
run_spec.loader.exec_module(run)


class PackageFilesTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.port = Path(self.tmp.name)
        (self.port / 'bin').mkdir()
        (self.port / 'scripts' / '__pycache__').mkdir(parents=True)
        (self.port / 'bin' / 'bb-probe.exe').write_bytes(b'probe 1.9.4')
        (self.port / 'bin' / 'SDL3.dll').write_bytes(b'sdl')
        (self.port / 'bin' / 'nvngx_dlss.dll').write_bytes(b'dlss')
        (self.port / 'run.py').write_text('run')
        (self.port / 'scripts' / 'patches.py').write_text('patches 1.9.4')
        (self.port / 'scripts' / '__pycache__' / 'patches.pyc').write_bytes(b'cache')
        self.assertEqual(P.write(self.port, '1.9.4'), 4)

    def test_the_list(self):
        files = json.loads((self.port / P.MANIFEST).read_text(encoding='utf-8'))['files']
        self.assertEqual(sorted(files), ['bin/SDL3.dll', 'bin/bb-probe.exe', 'run.py', 'scripts/patches.py'])
        self.assertEqual(P.check(self.port), ('1.9.4', []))

    def test_an_older_program_under_newer_scripts(self):
        (self.port / 'bin' / 'bb-probe.exe').write_bytes(b'probe 1.9.3')
        self.assertEqual(P.check(self.port, quick=True), ('1.9.4', ['bin/bb-probe.exe']))
        note = run.files_note(self.port)
        self.assertIn('Files: 1 differ from the package 1.9.4: bin/bb-probe.exe', note)
        self.assertEqual(help_.hints_in_log(note), ['mixed_versions'])
        self.assertEqual(help_.hints_in_log('ERROR: bad patch entry'), ['mixed_versions'])

    def test_what_a_player_may_change_or_what_a_start_does_not_need(self):
        (self.port / 'bin' / 'nvngx_dlss.dll').write_bytes(b'a newer DLSS')  # not in the list
        (self.port / 'bin' / 'SDL3.dll').write_bytes(b'other')  # in the list, but not read at every start
        self.assertEqual(P.check(self.port, quick=True), ('1.9.4', []))
        self.assertEqual(P.check(self.port), ('1.9.4', ['bin/SDL3.dll']))
        (self.port / 'scripts' / 'patches.py').unlink()
        self.assertEqual(P.check(self.port, quick=True), ('1.9.4', ['scripts/patches.py']))

    def test_a_build_from_the_sources_has_no_list(self):
        (self.port / P.MANIFEST).unlink()
        self.assertEqual(P.check(self.port), (None, []))
        self.assertIsNone(run.files_note(self.port))

    def test_play_asks_first(self):
        import types
        from unittest import mock
        (self.port / 'scripts' / 'patches.py').write_text('patches 1.9.5')
        asked, opened = [], []

        class Box:
            def __init__(self, answer):
                self.answer = answer

            def askyesno(self, title, text, **_options):
                asked.append(text)
                return self.answer

        class Window(help_.HelpMixin):
            def __init__(self, answer):
                self.messagebox, self.app = Box(answer), {}

            def var(self, key, store):
                return types.SimpleNamespace(get=lambda: '1920x1080')

            def open_topic(self, key):
                opened.append(key)
        launcher = types.SimpleNamespace(PORT_DIR=self.port, _=lambda *texts: texts[0], renamed=lambda text: text)
        with mock.patch.object(help_, 'L', launcher), mock.patch.object(help_, 'memory_problem', lambda *_a: None):
            self.assertFalse(Window(False).preflight())
            self.assertEqual(opened, ['mixed_versions'])
            self.assertTrue(Window(True).preflight())
        self.assertIn('scripts\\patches.py', asked[0])

    def test_helps_lines_and_the_topic(self):
        self.assertIn('mixed_versions', help_.TOPICS)
        self.assertIn('mixed_versions', help_.TOPIC_ORDER)
        self.assertEqual(help_.files_text(['bin/bb-probe.exe', 'run.py', 'scripts/a.py', 'scripts/b.py']),
                         'bin\\bb-probe.exe, run.py, scripts\\a.py …')


if __name__ == '__main__':
    unittest.main()
