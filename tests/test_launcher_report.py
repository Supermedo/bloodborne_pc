"""Help > Save a report (launcher/bbport_help.py): the BB Report Tool's zip, made without its window."""
from paths import ROOT
import importlib.util
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(ROOT / 'launcher'))
sys.path.insert(0, str(ROOT / 'scripts'))
spec = importlib.util.spec_from_file_location('bbport_help', ROOT / 'launcher/bbport_help.py')
help_ = importlib.util.module_from_spec(spec)
spec.loader.exec_module(help_)


class ReportTests(unittest.TestCase):
    """Help > Save a report: the BB Report Tool's zip in <bbport>/reports, selected in Explorer, its summary in
    the clipboard; the launcher's own zip only without the tool."""

    def setUp(self):
        import queue
        import types
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        root = Path(self.tmp.name)
        (root / 'BB-Report-Tool.bat').write_text('<# tool #>')
        launcher = types.SimpleNamespace(PORT_DIR=root, DATA_DIR=root, CONFIG_FILE=root / 'settings.json',
                                         VERSION='1.9.4', NO_WINDOW=0, _=lambda *texts: texts[0],
                                         renamed=lambda text: text)
        saved, help_.L = help_.L, launcher
        self.addCleanup(setattr, help_, 'L', saved)
        self.root = root

        class Widget:
            def __init__(self):
                self.states, self.text, self.clip = [], '', ''

            def state(self, flags):
                self.states.append(flags[0])

            def configure(self, text='', **_options):
                self.text = text

            def clipboard_clear(self):
                self.clip = ''

            def clipboard_append(self, text):
                self.clip += text

        class Window(help_.HelpMixin):
            def collect(self):
                pass
        self.window = Window()
        self.window.report_button, self.window.report_note, self.window.root = Widget(), Widget(), Widget()
        self.window.ui_calls = queue.Queue()

    def save(self, tool_makes_zip=True):
        import zipfile
        commands, opened = [], []

        def run(command, env=None, **_options):
            commands.append((command, env))
            if tool_makes_zip:
                with zipfile.ZipFile(Path(env['BBTOOL_OUT']) / 'BB-Report-20261022-120000.zip', 'w') as z:
                    z.writestr('report.txt', '```\nBB Report Tool 1.7.6\n```')

        class Thread:  # the work at once, in this thread
            def __init__(self, target, daemon=None):
                self.target = target

            def start(self):
                self.target()
        with mock.patch('subprocess.run', run), mock.patch('subprocess.Popen', lambda c: opened.append(c)), \
                mock.patch.object(help_.threading, 'Thread', Thread):
            self.window.save_report()
            self.window.ui_calls.get_nowait()()
        return commands, opened

    def test_the_tool_runs_without_its_window_into_reports(self):
        commands, opened = self.save()
        command, env = commands[0]
        self.assertEqual(command[0], 'powershell')
        self.assertEqual(env['BBTOOL'], str(self.root / 'BB-Report-Tool.bat'))
        self.assertEqual(env['BBTOOL_OUT'], str(self.root / 'reports'))  # it then asks nothing, opens nothing
        self.assertEqual(env['BBTOOL_VERSION'], '1.9.4')
        archive = self.root / 'reports' / 'BB-Report-20261022-120000.zip'
        self.assertEqual(opened, [['explorer', '/select,', str(archive)]])
        self.assertIn('BB Report Tool 1.7.6', self.window.root.clip)
        self.assertEqual(self.window.report_button.states, ['disabled', '!disabled'])

    def test_no_zip_says_so(self):
        _commands, opened = self.save(tool_makes_zip=False)
        self.assertEqual(opened, [])
        self.assertIn('The BB Report Tool made no zip.', self.window.report_note.text)
        self.assertEqual(self.window.report_button.states[-1], '!disabled')

    def test_the_tool_ships_beside_bloodborne_exe(self):
        package = (ROOT / 'packaging' / 'windows' / 'package.sh').read_text(encoding='utf-8')
        self.assertIn('cp packaging/windows/BB-Report-Tool.bat "$dest/"', package)
        tool = (ROOT / 'packaging' / 'windows' / 'BB-Report-Tool.bat').read_text(encoding='utf-8')
        for name in ('BBTOOL_PORT', 'BBTOOL_SETTINGS', 'BBTOOL_VERSION', 'BBTOOL_OUT'):
            self.assertIn(f'$env:{name}', tool)  # what save_report sets, so it asks nothing


if __name__ == '__main__':
    unittest.main()
