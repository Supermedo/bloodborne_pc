"""Help > Save a report (launcher/bbport_help.py): the BB Report Tool's zip, made without its window; the
tool's own hints for a log."""
from paths import ROOT
import datetime
import importlib.util
import json
import os
import subprocess
import sys
import tempfile
import unittest
import zipfile
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
                                         saves_folder=lambda s: Path(s.get('user_dir') or root / 'user'),
                                         VERSION='1.9.4', VERSION_TEXT='1.9.4', NO_WINDOW=0, _=lambda *texts: texts[0],
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
        self.window.app = {'user_dir': str(root / 'saves')}
        self.window.report_button, self.window.report_note, self.window.root = Widget(), Widget(), Widget()
        self.window.ui_calls = queue.Queue()

    def save(self, tool_makes_zip=True):
        import zipfile
        commands, opened = [], []

        def run(command, env=None, **_options):
            commands.append((command, env))
            if tool_makes_zip:
                with zipfile.ZipFile(Path(env['BBTOOL_OUT']) / 'BB-Report-20261022-120000.zip', 'w') as z:
                    z.writestr('report.txt', '```\nBB Report Tool 1.7.7\n```')

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
        # this launcher's own log (its saves folder), not the newest of any installation the tool finds
        self.assertEqual(env['BBTOOL_LOG'], str(self.root / 'saves' / 'last_run.log'))
        archive = self.root / 'reports' / 'BB-Report-20261022-120000.zip'
        self.assertEqual(opened, [['explorer', '/select,', str(archive)]])
        self.assertIn('BB Report Tool 1.7.7', self.window.root.clip)
        self.assertEqual(self.window.report_button.states, ['disabled', '!disabled'])

    def test_the_report_button_beside_play(self):
        """The bar's Report button (instead of Log) makes the same zip and says how it went in the status line."""
        status, button = self.window.report_note.__class__(), self.window.report_button.__class__()
        self.window.report_note = self.window.report_button = None  # the Help page not built yet

        def save():
            with mock.patch('subprocess.run', lambda c, env=None, **o: zipfile.ZipFile(
                    Path(env['BBTOOL_OUT']) / 'BB-Report-20261022-130000.zip', 'w').close()),                     mock.patch('subprocess.Popen', lambda c: None),                     mock.patch.object(help_.threading, 'Thread', type('Thread', (), {
                        '__init__': lambda s, target, daemon=None: setattr(s, 't', target),
                        'start': lambda s: s.t()})):
                self.window.save_report(button, status)
                self.window.ui_calls.get_nowait()()
        save()
        self.assertEqual(button.states, ['disabled', '!disabled'])
        self.assertIn('BB-Report-20261022-130000.zip', status.text)
        source = (ROOT / 'launcher' / 'bbport_launcher_win.py').read_text(encoding='utf-8')
        self.assertIn("self.save_report(self.bar_report, self.status)", source)
        self.assertNotIn("command=lambda: self.show('log')).pack(", source)

    def test_no_zip_says_so(self):
        _commands, opened = self.save(tool_makes_zip=False)
        self.assertEqual(opened, [])
        self.assertIn('The BB Report Tool made no zip.', self.window.report_note.text)
        self.assertEqual(self.window.report_button.states[-1], '!disabled')

    def test_the_tool_ships_beside_bloodborne_exe(self):
        package = (ROOT / 'packaging' / 'windows' / 'package.sh').read_text(encoding='utf-8')
        self.assertIn('cp packaging/windows/BB-Report-Tool.bat "$dest/"', package)
        tool = (ROOT / 'packaging' / 'windows' / 'BB-Report-Tool.bat').read_text(encoding='utf-8')
        for name in ('BBTOOL_PORT', 'BBTOOL_SETTINGS', 'BBTOOL_VERSION', 'BBTOOL_OUT', 'BBTOOL_LOG'):
            self.assertIn(f'$env:{name}', tool)  # what save_report sets, so it asks nothing


# The end of a normal run, as in players' logs of 1.8.2 and 1.9.4: every run has a few '== Stall during
# rendering' lines, and after the last one only save data and statistics lines came before the player quit.
NORMAL_RUN = '\n'.join([
    "Runtime: guest thread 'CSClothThread5' created (stack=65536, prio=700)",
    '== Stall during rendering at flush 1',
    "Runtime: save data 'SPRJ0005' mounted at /savedata0 (existing)",
    'Upscaler: scene texture LOD bias -0.58',
    'DLSS: DLSS 1280x720 -> 1920x1080, quality 1, preset 0, HDR input',
    '== Stall during rendering at flush 154',
    "Runtime: save data 'SPRJ0005' mounted at /savedata0 (existing)",
    'Memory: VRAM 2946 MiB; textures 900 (774 MiB), game memory backing 508 MiB; process 5922 MiB committed',
]) + '\n'


@unittest.skipUnless(sys.platform == 'win32', 'the BB Report Tool runs in Windows PowerShell')
class ToolHintTests(unittest.TestCase):
    """The BB Report Tool itself on a made-up port folder (nothing in it runs): the hints in its zip for a log,
    with the Windows events handed in (BBTOOL_EVENTS) instead of this PC's."""
    LOG_TIME = datetime.datetime(2026, 10, 21, 16, 49)

    def hints(self, log, events=(), settings=None):
        """hints.txt for this last_run.log; events: (Windows event id, seconds after the log's time) of bb-probe.exe."""
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        root = Path(temporary.name)
        port = root / 'port'
        (port / 'bin').mkdir(parents=True)
        (port / 'user').mkdir()
        (port / 'Bloodborne.exe').write_bytes(b'')
        (port / 'bin' / 'bb-probe.exe').write_bytes(b'')
        path = port / 'user' / 'last_run.log'
        path.write_text(log, encoding='utf-8')
        os.utime(path, (self.LOG_TIME.timestamp(), self.LOG_TIME.timestamp()))
        (root / 'settings.json').write_text(json.dumps(settings or {}), encoding='utf-8')
        (root / 'events.json').write_text(json.dumps([
            {'Id': number, 'TimeCreated': (self.LOG_TIME + datetime.timedelta(seconds=seconds)).isoformat(),
             'Properties': [{'Value': 'bb-probe.exe'}]} for number, seconds in events]), encoding='utf-8')
        (root / 'out').mkdir()
        env = {**os.environ, 'BBTOOL': str(ROOT / 'packaging' / 'windows' / 'BB-Report-Tool.bat'),
               'BBTOOL_PORT': str(port), 'BBTOOL_SETTINGS': str(root / 'settings.json'), 'BBTOOL_VERSION': '1.9.4',
               'BBTOOL_OUT': str(root / 'out'), 'BBTOOL_EVENTS': str(root / 'events.json'),
               'TEMP': str(root), 'TMP': str(root)}
        subprocess.run(['powershell', '-NoProfile', '-ExecutionPolicy', 'Bypass', '-Command',
                        'Invoke-Expression ([IO.File]::ReadAllText($env:BBTOOL))'], env=env,
                       stdin=subprocess.DEVNULL, capture_output=True, timeout=300)
        archive, = (root / 'out').glob('BB-Report-*.zip')
        with zipfile.ZipFile(archive) as z:
            return z.read('hints.txt').decode('utf-8-sig')

    def test_a_log_ending_with_a_stall_is_no_freeze(self):
        hints = self.hints(NORMAL_RUN)
        self.assertNotIn('not responding', hints)
        self.assertNotIn('waits on the GPU', hints)  # what 1.7.6 said after every such quit
        self.assertNotIn('Two-stage', hints)

    def test_a_hang_from_before_this_log_is_no_freeze_either(self):
        hints = self.hints(NORMAL_RUN, events=[(1002, -24 * 3600), (1000, -3600)])
        self.assertNotIn('not responding', hints)
        self.assertNotIn('Two-stage', hints)

    def test_a_hang_at_the_end_of_the_log_is_a_freeze(self):
        hints = self.hints(NORMAL_RUN, events=[(1002, 20)])
        self.assertIn('Windows recorded the game as not responding at the end of this log (2026-10-21 16:49): '
                      'a freeze, not a crash; it was waiting on the GPU.', hints)
        # Switching the pipeline off is one test, never a standing step; object motion is no freeze fix.
        self.assertIn('Test Advanced > Two-stage GPU pipeline Off once, and switch it back to Auto if it changes '
                      'nothing', hints)
        self.assertNotIn('Object motion', hints)

    def test_with_the_pipeline_already_off_the_freeze_hint_says_set_it_back(self):
        hints = self.hints(NORMAL_RUN, events=[(1002, 20)], settings={'draw_pipe': '0'})
        self.assertIn('Two-stage GPU pipeline is Off here: if that did not stop the freezes, set it back to Auto',
                      hints)
        self.assertNotIn('Test Advanced > Two-stage', hints)


if __name__ == '__main__':
    unittest.main()
