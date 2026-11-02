"""The launcher's background work never ends without a word (R85): a broken HTTP reply (http.client's BadStatusLine,
IncompleteRead, from a proxy, a virus scanner or a captive portal) is no OSError, and it ended helper threads with
"Checking…" on the Online page, a grey download button or no update answer for good. drain_output goes on after a
failing call, read_output after a failing log write (a full drive froze the game), Add to Steam waits for Steam in
a helper thread (and a window closed meanwhile waits for it), and Clear the shader cache refuses while a game runs
and says what it could not delete."""
from paths import ROOT
import gc
import http.client
import io
import os
import queue
import sys
import tempfile
import threading
import time
import types
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(ROOT / 'launcher'))
sys.path.insert(0, str(ROOT / 'scripts'))
import bbport_launcher_win as L  # noqa: E402


def broken_reply(*_args, **_kwargs):
    raise http.client.BadStatusLine('')


def bare_launcher(**attributes):
    """A Launcher without its window: only what the code under test reads."""
    app = L.Launcher.__new__(L.Launcher)
    app.ui_calls, app.output = queue.Queue(), queue.Queue()
    app.messagebox = mock.Mock()
    app.__dict__.update(attributes)
    return app


def run_ui_calls(app, wait=5.0, count=1):
    """The calls a helper thread posted, run as drain_output would (waits for the first `count`)."""
    calls = [app.ui_calls.get(timeout=wait) for _i in range(count)]
    while not app.ui_calls.empty():
        calls.append(app.ui_calls.get_nowait())
    for call in calls:
        call()
    return calls


class FakeRoot:
    def __init__(self):
        self.scheduled, self.reported, self.destroyed, self.withdrawn = [], [], False, False

    def after(self, ms, function):
        self.scheduled.append((ms, function))

    def state(self):
        return 'normal'

    def report_callback_exception(self, kind, value, trace):
        self.reported.append(f'{kind.__name__}: {value}')

    def withdraw(self):
        self.withdrawn = True

    def destroy(self):
        self.destroyed = True


def hidden_root(test):
    """A Tk root that is never shown, for the widgets a page or a window builds (skipped without Tk). Afterwards
    its helper threads are waited for and its widgets and variables collected here: collected later in another
    test's thread, Tcl ended the whole run ("Tcl_AsyncDelete: async handler deleted by the wrong thread")."""
    try:
        import tkinter
        root = tkinter.Tk()
    except Exception as error:
        test.skipTest(f'Tk session unavailable: {error}')
    root.withdraw()
    before = set(threading.enumerate())

    def settle():
        for thread in set(threading.enumerate()) - before:
            thread.join(10)
        for name in [name for name in vars(test) if not name.startswith('_')]:
            delattr(test, name)  # the page, the guide, the launcher: unittest keeps the test until the run ends
        gc.collect()
    test.addCleanup(settle)
    test.addCleanup(root.destroy)
    return root


def widgets(parent):
    """Every widget under `parent`, in the order they were made."""
    found = []
    for child in parent.winfo_children():
        found.append(child)
        found += widgets(child)
    return found


def texts(parent):
    """The texts of the labels under `parent`."""
    return [str(w.cget('text')) for w in widgets(parent) if w.winfo_class() in ('TLabel', 'Label')]


def button(parent, text, index=0):
    return [w for w in widgets(parent) if w.winfo_class() == 'TButton' and str(w.cget('text')) == text][index]


class ErrorTextTests(unittest.TestCase):
    def test_never_empty_and_named_where_the_text_says_little(self):
        self.assertEqual(L.error_text(http.client.BadStatusLine('')), 'BadStatusLine')
        self.assertEqual(L.error_text(http.client.BadStatusLine('HTTP/1.1 garbage')), 'BadStatusLine: HTTP/1.1 garbage')
        self.assertEqual(L.error_text(http.client.IncompleteRead(b'')), 'IncompleteRead(0 bytes read)')
        self.assertEqual(L.error_text(KeyError('tag_name')), "KeyError: 'tag_name'")
        self.assertEqual(L.error_text(OSError('timed out')), 'timed out')
        self.assertEqual(L.error_text(RuntimeError()), 'RuntimeError')

    def test_a_text_that_says_what_happened_stays_as_it_is(self):
        """"Download failed: FetchError: download failed: HTTP 404" and "Update failed: BadZipFile: …" read worse."""
        import struct
        import subprocess
        import zipfile
        failures = [L.fetch_fsr4vk.FetchError('download failed: HTTP 404 from https://example.net/fsr4vk.zip'),
                    zipfile.BadZipFile('File is not a zip file'), struct.error('unpack requires a buffer of 4 bytes'),
                    subprocess.TimeoutExpired(['powershell'], 300), RuntimeError('probe'),
                    http.client.RemoteDisconnected('Remote end closed connection without response')]
        for failure in failures:
            self.assertEqual(L.error_text(failure), str(failure))

    def test_the_kind_where_asked_for(self):
        self.assertEqual(L.error_text(RuntimeError('probe'), kind=True), 'RuntimeError: probe')
        self.assertEqual(L.error_text(RuntimeError(), kind=True), 'RuntimeError')

    def test_connection_error_uses_it(self):
        self.assertEqual(L.connection_error(http.client.BadStatusLine(''), L.RELEASES_API), 'BadStatusLine')


class ServerCheckTests(unittest.TestCase):
    """The Online page's "Test connection": every address gets its line, also after a broken reply."""

    def setUp(self):
        patcher = mock.patch.object(L, 'LANG', 'en')
        patcher.start()
        self.addCleanup(patcher.stop)

    def test_a_broken_reply_is_an_answer(self):
        with mock.patch.object(L.bbport_tls, 'urlopen', side_effect=broken_reply), \
                mock.patch('socket.create_connection', side_effect=http.client.LineTooLong('header line')):
            text = L.server_check('np.example.net:31313', 'http://np.example.net:31315', 'https://game.example.net')
        lines = text.splitlines()
        self.assertEqual(len(lines), 3)
        self.assertIn('Game server https://game.example.net: not reachable (BadStatusLine)', lines)
        self.assertTrue(all('not reachable' in line for line in lines), lines)

    def test_the_hunters_dream_too(self):
        with mock.patch.object(L.bbport_tls, 'urlopen', side_effect=broken_reply):
            text = L.server_check_hunters_dream()
        self.assertEqual(text.count('not reachable (BadStatusLine)'), 2)

    def test_the_player_count_is_none_after_a_broken_reply(self):
        reply = mock.MagicMock()
        reply.__enter__.return_value.read.side_effect = http.client.IncompleteRead(b'{"active')
        with mock.patch.object(L.bbport_tls, 'urlopen', return_value=reply):
            self.assertIsNone(L.hunters_online())

    def test_the_player_count_is_asked_again_after_any_failure(self):
        """hunters_asking stayed True when the thread died: the count was never asked again."""
        app = bare_launcher(process=None, hunters=None, hunters_when=0, hunters_asking=False,
                            root=types.SimpleNamespace(state=lambda: 'normal'), show_hunters=mock.Mock(),
                            var=lambda key, store: types.SimpleNamespace(get=lambda: 'huntersdream'))
        errors = []
        # The Hunter's Dream's count is only asked while it is offered (BB_HUNTERS_DREAM=1 since 1.9.7).
        with mock.patch.object(L, 'hunters_online', side_effect=RuntimeError('a bug')), \
                mock.patch.object(L, 'HUNTERS_DREAM_POOL', True), \
                mock.patch.object(threading, 'excepthook', lambda args: errors.append(args.exc_type)):
            app.ask_hunters()
            self.assertTrue(app.hunters_asking)
            run_ui_calls(app)
            for _i in range(100):  # the thread's own end, still inside the patched hook
                if errors:
                    break
                time.sleep(0.02)
        self.assertFalse(app.hunters_asking)
        self.assertEqual(errors, [RuntimeError])
        app.show_hunters.assert_called_once()


class UpdateCheckTests(unittest.TestCase):
    def setUp(self):
        patcher = mock.patch.object(L, 'LANG', 'en')
        patcher.start()
        self.addCleanup(patcher.stop)

    def test_the_button_always_answers(self):
        app = bare_launcher()
        with mock.patch.object(L.bbport_tls, 'urlopen', side_effect=broken_reply):
            L.Launcher.check_update(app, manual=True)
        run_ui_calls(app, wait=0)
        title, text = app.messagebox.showerror.call_args.args
        self.assertEqual(text, 'Could not check for updates: BadStatusLine')

    def test_the_check_at_start_stays_quiet(self):
        app = bare_launcher()
        with mock.patch.object(L.bbport_tls, 'urlopen', side_effect=broken_reply):
            L.Launcher.check_update(app)
        self.assertTrue(app.ui_calls.empty())

    def test_a_beta_still_asks_for_the_next_beta(self):
        with mock.patch.object(L, 'BETA', 3), mock.patch.object(L, 'latest_release', side_effect=broken_reply), \
                mock.patch.object(L, 'newer_beta', return_value=('1.9.6 beta 4', 'zip', 'page')):
            self.assertEqual(L.update_to_offer(), ('1.9.6 beta 4', 'zip', 'page'))
        with mock.patch.object(L, 'BETA', 3), mock.patch.object(L, 'latest_release', side_effect=broken_reply), \
                mock.patch.object(L, 'newer_beta', side_effect=OSError('offline')):
            with self.assertRaises(http.client.BadStatusLine):  # the first error is the one shown
                L.update_to_offer()


class DownloadTests(unittest.TestCase):
    def setUp(self):
        patcher = mock.patch.object(L, 'LANG', 'en')
        patcher.start()
        self.addCleanup(patcher.stop)
        self.folder = tempfile.TemporaryDirectory()
        self.addCleanup(self.folder.cleanup)

    def test_the_fsr4_button_comes_back(self):
        app = bare_launcher(fsr4_button=mock.Mock(), refresh_fsr4=mock.Mock(), refresh_status=mock.Mock())
        with mock.patch.object(L, 'PORT_DIR', Path(self.folder.name)), \
                mock.patch.object(L, 'fsr4_missing', return_value=['fsr4_a.spv']), \
                mock.patch.object(L.bbport_tls, 'urlopen', side_effect=http.client.IncompleteRead(b'')):
            app.download_fsr4()
            self.assertTrue(app.downloading)
            run_ui_calls(app)
        self.assertFalse(app.downloading)
        self.assertIn('IncompleteRead', app.messagebox.showerror.call_args.args[1])

    def test_play_comes_back_after_a_broken_fsr4vk_download(self):
        app = bare_launcher(process=None, fsr411_downloading=False, play_button=mock.Mock(),
                            refresh_fsr411=mock.Mock(), refresh_status=mock.Mock(), fsr411_progress=mock.Mock(),
                            fsr411_folder=lambda: Path(self.folder.name))
        with mock.patch.object(L.fetch_fsr4vk, 'fetch', side_effect=http.client.IncompleteRead(b'')):
            app.download_fsr411()
            self.assertTrue(app.fsr411_downloading)
            run_ui_calls(app)
        self.assertFalse(app.fsr411_downloading)  # refresh_fsr411 enables Play again from it
        self.assertIn('IncompleteRead', app.messagebox.showerror.call_args.args[1])

    def test_the_fsr4vk_answer_reads_as_before(self):
        """Its own error, translated: never with "FetchError: " in front of it."""
        app = bare_launcher(process=None, fsr411_downloading=False, play_button=mock.Mock(),
                            refresh_fsr411=mock.Mock(), refresh_status=mock.Mock(), fsr411_progress=mock.Mock(),
                            fsr411_folder=lambda: Path(self.folder.name))
        with mock.patch.object(L.fetch_fsr4vk, 'fetch'), \
                mock.patch.object(L.fetch_fsr4vk, 'files_ok', return_value=False):
            app.download_fsr411()
            run_ui_calls(app)
        self.assertEqual(app.messagebox.showerror.call_args.args[1],
                         'Download failed: Vulkan (fsr4vk): files missing or need updating.')


class InstallVersionTests(unittest.TestCase):
    """Advanced > Other versions and the update offer: a download that breaks gives the buttons back."""

    def setUp(self):
        folder = tempfile.TemporaryDirectory()
        self.addCleanup(folder.cleanup)
        for patcher in (mock.patch.object(L, 'LANG', 'en'), mock.patch.object(L, 'FROZEN', True),
                        mock.patch.object(L, 'UPDATE_DIR', Path(folder.name) / 'update')):
            patcher.start()
            self.addCleanup(patcher.stop)
        self.app = bare_launcher(game_running=lambda: False, test_running=lambda: False, root=FakeRoot(),
                                 updating=False, installing=False, update_buttons=mock.Mock())
        self.app.messagebox.askyesno.return_value = True
        self.progress, self.failed = mock.Mock(), mock.Mock()

    def install(self, count=1, **urlopen):
        with mock.patch.object(L.bbport_tls, 'urlopen', **urlopen):
            self.assertTrue(self.app.install_version('99.0.0', 'https://example.net/BBPort.zip', 'page',
                                                     self.progress, self.failed))
            run_ui_calls(self.app, count=count)
        self.failed.assert_called_once_with()
        self.assertFalse(self.app.root.destroyed)
        return self.app.messagebox.showerror.call_args.args[1]

    def test_a_broken_download(self):
        self.assertEqual(self.install(side_effect=http.client.IncompleteRead(b'')),
                         'Update failed: IncompleteRead(0 bytes read)')

    def test_a_download_that_is_no_zip(self):
        class Reply(io.BytesIO):
            headers = {'Content-Length': '9'}
        # Its progress (100 %), "Checking the download…", then the answer.
        self.assertEqual(self.install(3, return_value=Reply(b'not a zip')),
                         'Update failed: the download is damaged (File is not a zip file). Try again.')
        self.progress.assert_called()


class OtherVersionsTests(unittest.TestCase):
    def test_the_list_always_answers(self):
        """"Loading the list of versions…" stayed for good after a broken reply."""
        import tkinter
        from tkinter import ttk
        root = hidden_root(self)

        def toplevel(master):
            window = tkinter.Toplevel(master)
            window.withdraw()  # never on the screen
            return window
        app = bare_launcher(root=root, tk=types.SimpleNamespace(Toplevel=toplevel), ttk=ttk, dpi=1, version_window=None)
        with mock.patch.object(L, 'LANG', 'en'), \
                mock.patch.object(L, 'installable_releases', side_effect=http.client.BadStatusLine('')):
            app.choose_version()
            run_ui_calls(app)
        self.assertIn('Could not load the list of versions: BadStatusLine', texts(app.version_window))


class HardwareCheckTests(unittest.TestCase):
    def test_a_check_that_stops_still_ends(self):
        """detect_gpu: "Checking the graphics card…" and "checking support…" stayed when it stopped."""
        names = ('append', 'refresh_fsr411', 'refresh_status', 'hardware_ready', 'refresh_motion', 'refresh_fg401')
        app = bare_launcher(app={}, fsr411_support=None, fg401_support=None, **{n: mock.Mock() for n in names})
        with mock.patch.object(L, 'LANG', 'en'), mock.patch.object(L, 'vulkan_verdict', return_value=None), \
                mock.patch.object(L.subprocess, 'run', side_effect=FileNotFoundError('bb-gpu-capabilities.exe')), \
                mock.patch.object(L.bbport_fsr41, 'query', side_effect=RuntimeError('probe')):
            L.Launcher.detect_gpu(app)
            run_ui_calls(app, wait=0)
        self.assertEqual(app.fsr411_support['vk']['reason'], 'check')
        self.assertEqual(L.bbport_fsr412.method_status('vk', app.fsr411_support)['reason'], 'check')
        self.assertEqual(app.fg401_support['fsr401']['reason'], 'check')
        app.append.assert_called_once_with('The check of the graphics card stopped: RuntimeError: probe\n')
        for name in names[2:]:
            getattr(app, name).assert_called()

    def test_check_again_ends_too(self):
        """The "Check again" buttons of FSR 4.1 and frame generation: "check failed", not "checking…" for good."""
        app = bare_launcher(app={}, **{n: mock.Mock() for n in ('append', 'refresh_fsr411', 'refresh_status',
                                                                 'refresh_fg401')})
        with mock.patch.object(L, 'LANG', 'en'), \
                mock.patch.object(L.bbport_fsr41, 'query', side_effect=http.client.BadStatusLine('')), \
                mock.patch.object(L.bbport_fg401, 'query_versions', side_effect=RuntimeError('probe')):
            app.recheck_fsr411()
            self.assertIsNone(app.fsr411_support)
            run_ui_calls(app, count=2)
            app.recheck_fg401()
            self.assertIsNone(app.fg401_support)
            run_ui_calls(app, count=2)
        self.assertEqual(app.fsr411_support['vk']['reason'], 'check')
        self.assertEqual(app.fsr411_support['dx12']['detail'], 'BadStatusLine')
        self.assertEqual(app.fg401_support, L.fg401_check_failed())
        self.assertEqual([c.args[0] for c in app.append.call_args_list],
                         ['The check of the graphics card stopped: BadStatusLine\n',
                          'The check of the graphics card stopped: RuntimeError: probe\n'])
        app.refresh_fg401.assert_called()


class DrainTests(unittest.TestCase):
    """drain_output plans its next round whatever fails in this one."""

    def drained(self, **attributes):
        app = bare_launcher(**{'process': None, 'custom_frame': None, 'root': FakeRoot(), **attributes})
        app._collected = time.monotonic()
        return app

    def test_a_failing_call_is_reported_once_and_the_loop_goes_on(self):
        app = self.drained()
        done = []

        def failing():
            raise ValueError('broken')
        for call in (failing, lambda: done.append(1), failing):
            app.ui_calls.put(call)
        app.drain_output()
        self.assertEqual(done, [1])
        self.assertEqual(app.root.reported, ['ValueError: broken'])  # the same error once
        self.assertEqual([function for _ms, function in app.root.scheduled], [app.drain_output])

    def test_a_failing_part_does_not_stop_the_others(self):
        app = self.drained(append=mock.Mock(), status=mock.Mock())
        app.tidy_up = mock.Mock(side_effect=OSError('gone'))
        app.ui_calls.put(lambda: app.output.put('a line\n'))
        app.drain_output()
        app.append.assert_called_once_with('a line\n')
        self.assertEqual(app.root.reported, ['OSError: gone'])
        self.assertEqual(len(app.root.scheduled), 1)

    def game_end(self, **attributes):
        steps = ('append', 'sync_ini', 'refresh_fsr411', 'refresh_saves', 'stop_button', 'forget_refused_sign_in',
                 'refresh_status', 'bring_back', 'show', 'drop_tray')
        app = self.drained(process=mock.Mock(), job=None, placer=None, hidden=False, follow_shared_ini=mock.Mock(),
                           setting=lambda key: False, **{name: mock.Mock() for name in steps})
        app.__dict__.update(attributes)
        app.output.put((0,))
        return app

    def test_play_comes_back_when_a_step_of_the_game_end_fails(self):
        app = self.game_end(sync_ini=mock.Mock(side_effect=PermissionError('bbport.ini is locked')))
        with mock.patch.object(L, 'LANG', 'en'):
            app.drain_output()
        self.assertIsNone(app.process)
        app.stop_button.pack_forget.assert_called_once()
        app.refresh_status.assert_called_once()
        self.assertEqual(app.root.reported, ['PermissionError: bbport.ini is locked'])
        self.assertEqual(len(app.root.scheduled), 2)  # after_exit, and the next round

    def test_no_next_round_once_the_window_closed_with_the_game(self):
        app = self.game_end(hidden=True)
        with mock.patch.object(L, 'LANG', 'en'):
            app.drain_output()
        self.assertTrue(app.root.destroyed)
        self.assertEqual(app.root.scheduled, [])

    def test_a_line_whose_trophy_fails_still_reaches_the_log_page(self):
        """The line the helpers look for: a trophy toast that raised took it from the page."""
        def toast(_new):
            raise RuntimeError('toast')  # a new one each time, as the real one would be
        app = self.drained(append=mock.Mock(), tracker=mock.Mock(), trophies_changed=toast)
        for line in ('Runtime: trophy 7 unlocked\n', 'Runtime: trophy 8 unlocked\n', 'a line\n'):
            app.output.put(line)
        app.drain_output()
        self.assertEqual([c.args[0] for c in app.append.call_args_list],
                         ['Runtime: trophy 7 unlocked\n', 'Runtime: trophy 8 unlocked\n', 'a line\n'])
        self.assertEqual(app.root.reported, ['RuntimeError: toast'])
        self.assertEqual(len(app.root.scheduled), 1)


class FullDrive(io.StringIO):
    """A log file on a drive without room: every write fails, and so does closing it (its last line)."""

    def write(self, text):
        raise OSError(28, 'No space left on device')

    def close(self):
        if not self.closed:
            super().close()
            raise OSError(28, 'No space left on device')


class ReadOutputTests(unittest.TestCase):
    """last_run.log on a full drive: the reader goes on, so the game's output pipe never fills up."""

    def setUp(self):
        patcher = mock.patch.object(L, 'LANG', 'en')
        patcher.start()
        self.addCleanup(patcher.stop)
        self.folder = tempfile.TemporaryDirectory()
        self.addCleanup(self.folder.cleanup)

    def read(self, opener, before=''):
        app = bare_launcher(app={'user_dir': self.folder.name})
        process = types.SimpleNamespace(stdout=io.BytesIO(b'one\r\ntwo\nthree\n'), wait=lambda: 0)
        real = L.RunLog
        with mock.patch.object(L, 'RunLog', lambda path, note: real(path, note, opener=opener)):
            app.read_output(process, before)
        items = []
        while not app.output.empty():
            items.append(app.output.get_nowait())
        return items

    def test_a_failing_write_stops_the_logging_not_the_reading(self):
        items = self.read(lambda *a, **k: FullDrive(), before='repaired\n')
        notes = [item for item in items if isinstance(item, str) and 'could not be written' in item]
        self.assertEqual(len(notes), 1)
        self.assertIn('No space left on device', notes[0])
        self.assertIn('last_run.log', notes[0])
        self.assertEqual([item for item in items if item in ('one\r\n', 'two\n', 'three\n')],
                         ['one\r\n', 'two\n', 'three\n'])
        self.assertEqual(items[-1], (0,))  # the game's end reaches the window

    def test_a_log_that_cannot_be_opened_is_said_too(self):
        items = self.read(mock.Mock(side_effect=PermissionError(13, 'Access is denied')))
        self.assertEqual(sum('could not be written' in item for item in items if isinstance(item, str)), 1)
        self.assertEqual(items[-1], (0,))

    def test_whatever_stops_the_forwarding_the_pipe_is_read_to_its_end(self):
        """An unread pipe freezes the game: the rest is read before the wait for the game's end."""
        app = bare_launcher(app={'user_dir': self.folder.name}, root=FakeRoot())
        stdout = io.BytesIO(b'one\n' + b'x' * 300_000 + b'\nlast\n')

        def forward(process, log):
            process.stdout.readline()
            raise RuntimeError('a bug')
        app.forward_output = forward
        app.read_output(types.SimpleNamespace(stdout=stdout, wait=lambda: 0))
        self.assertEqual(stdout.read(), b'')
        self.assertEqual(app.output.get_nowait(), (0,))
        run_ui_calls(app, wait=0)
        self.assertEqual(app.root.reported, ['RuntimeError: a bug'])

    def test_the_log_as_before_when_the_drive_has_room(self):
        self.read(open, before='repaired\n')
        self.assertEqual((Path(self.folder.name) / 'last_run.log').read_text(encoding='utf-8'),
                         'repaired\none\ntwo\nthree\n')


class ShaderCacheTests(unittest.TestCase):
    def setUp(self):
        patcher = mock.patch.object(L, 'LANG', 'en')
        patcher.start()
        self.addCleanup(patcher.stop)
        self.folder = tempfile.TemporaryDirectory()
        self.addCleanup(self.folder.cleanup)
        self.cache = Path(self.folder.name) / 'cache'
        (self.cache / 'pipelines').mkdir(parents=True)
        (self.cache / 'pipelines' / 'a.bin').write_bytes(b'x')
        self.app = bare_launcher(process=None, app={'cache_dir': self.folder.name})

    def test_not_while_a_game_runs_from_elsewhere(self):
        """Started through Steam or a shortcut: the launcher has no process, the game holds the start mutex."""
        with mock.patch.object(L, 'game_underway', return_value=True):
            self.app.clear_cache()
        self.assertIn('Close the game first', self.app.messagebox.showinfo.call_args.args[1])
        self.assertTrue((self.cache / 'pipelines' / 'a.bin').exists())

    def test_cleared(self):
        with mock.patch.object(L, 'game_underway', return_value=False):
            self.app.clear_cache()
        self.assertFalse(self.cache.exists())
        self.assertIn('Shader cache cleared', self.app.messagebox.showinfo.call_args.args[1])

    @unittest.skipUnless(os.name == 'nt', 'Windows keeps an open file from being deleted')
    def test_a_failed_delete_is_said(self):
        with open(self.cache / 'pipelines' / 'a.bin', 'rb'), mock.patch.object(L, 'game_underway', return_value=False):
            self.app.clear_cache()
        self.app.messagebox.showinfo.assert_not_called()
        self.assertIn('Part of the shader cache could not be deleted', self.app.messagebox.showerror.call_args.args[1])


class SteamTests(unittest.TestCase):
    """Add to Steam: the wait for Steam to close (up to half a minute) runs off the Tk thread."""

    def setUp(self):
        import bbport_steam
        folder = tempfile.TemporaryDirectory()
        self.addCleanup(folder.cleanup)
        self.steam, self.closed = Path(folder.name), threading.Event()
        patchers = [mock.patch.object(L, 'LANG', 'en'),
                    mock.patch.object(bbport_steam, 'steam_folder', return_value=self.steam),
                    mock.patch.object(bbport_steam, 'user_configs', return_value=[self.steam / 'config']),
                    mock.patch.object(bbport_steam, 'steam_running', side_effect=lambda: not self.closed.is_set()),
                    mock.patch.object(bbport_steam, 'add_shortcut'), mock.patch.object(L.subprocess, 'Popen'),
                    mock.patch.object(L.os, 'startfile', create=True)]
        started = []
        for patcher in patchers:
            started.append(patcher.start())
            self.addCleanup(patcher.stop)
        self.add, self.popen, self.startfile = started[-3:]
        self.app = bare_launcher(collect=mock.Mock(), process=None, tray=None, root=FakeRoot())
        self.app.messagebox.askyesno.return_value = True

    def test_the_window_does_not_wait_for_steam(self):
        app = self.app
        start = time.monotonic()
        app.add_to_steam()
        self.assertLess(time.monotonic() - start, 0.4)
        app.add_to_steam()  # a second click while it waits: said, nothing more
        self.assertIn('still being added to Steam', app.messagebox.showinfo.call_args.args[1])
        self.assertEqual(app.messagebox.askyesno.call_count, 1)
        self.assertTrue(app.ui_calls.empty())
        self.add.assert_not_called()
        self.closed.set()
        run_ui_calls(app, wait=10)
        self.popen.assert_called_once()
        self.assertEqual(self.add.call_args.args[0], self.steam / 'config' / 'shortcuts.vdf')
        self.startfile.assert_called_once_with('steam://open/games')
        self.assertIn('Bloodborne is in your Steam library', app.messagebox.showinfo.call_args.args[1])
        self.assertEqual(app.messagebox.showinfo.call_count, 2)
        self.assertFalse(app.steam_adding)
        self.assertFalse(app.root.destroyed)

    def test_closed_while_it_waits_it_finishes_first(self):
        """Ended at once, Steam stayed closed and a cut-off shortcuts.vdf lost every non-Steam game in it."""
        app = self.app
        app.add_to_steam()
        app.close()
        self.assertTrue(app.root.withdrawn)
        self.assertFalse(app.root.destroyed)
        self.add.assert_not_called()
        self.closed.set()
        run_ui_calls(app, wait=10)
        self.add.assert_called_once()
        self.startfile.assert_called_once_with('steam://open/games')  # Steam's library with the game: the answer
        app.messagebox.showinfo.assert_not_called()
        self.assertTrue(app.root.destroyed)

    def test_opened_again_meanwhile_it_stays_open(self):
        app = self.app
        app.root.deiconify = app.root.lift = app.root.focus_force = lambda: None
        app.root.state = lambda *_a: 'normal'
        app.add_to_steam()
        app.close()
        app.bring_back()  # a second start of the launcher brings this one to the front
        self.closed.set()
        run_ui_calls(app, wait=10)
        self.assertFalse(app.root.destroyed)
        app.messagebox.showinfo.assert_called_once()


class SetupGuideTests(unittest.TestCase):
    """The guide's sign-ins (its online step) answer after the guide closed or moved to another step: its widgets
    are gone then (a code.pack_forget() raised before the sign-in was saved)."""

    def setUp(self):
        import bbport_setup
        import tkinter
        from tkinter import ttk
        root = hidden_root(self)
        for patcher in (mock.patch.object(L, 'LANG', 'en'), mock.patch.object(L, 'HUNTERS_DREAM_POOL', True),
                        mock.patch('webbrowser.open')):
            patcher.start()
            self.addCleanup(patcher.stop)
        variables = {}

        def var(key, _store):
            if key not in variables:
                variables[key] = tkinter.BooleanVar(root) if key == 'online' else tkinter.StringVar(root)
            return variables[key]
        self.app = bare_launcher(tk=tkinter, ttk=ttk, px=int, var=var, collect=mock.Mock(),
                                 saves_card=lambda body, **_options: tkinter.Frame(body))
        guide = self.guide = bbport_setup.SetupGuide.__new__(bbport_setup.SetupGuide)
        guide.app, guide.tk, guide.ttk, guide.px = self.app, tkinter, ttk, int
        guide.origin, guide.online_offered, guide.first = types.SimpleNamespace(get=lambda: 'new'), True, False
        self.body = tkinter.Frame(root)

    def step(self, pool):
        self.app.var('online_pool', 'app').set(pool)
        self.guide.step_carry(self.body)
        return button(self.body, 'Sign in with Discord')

    def test_the_hunters_dream_sign_in_is_saved_after_the_guide_closed(self):
        def sign_in(on_code):
            on_code('https://example.net/link', 'ABCD-1234')
            return 'Hunter', 'token'
        sign = self.step('huntersdream')
        with mock.patch.object(L, 'thd_sign_in', side_effect=sign_in):
            sign.invoke()
            self.body.destroy()
            run_ui_calls(self.app, count=2)  # the code to enter, then the account
        self.assertEqual(self.app.var('thd_token', 'app').get(), 'token')
        self.app.collect.assert_called_once()

    def test_its_failure_after_the_guide_closed(self):
        sign = self.step('huntersdream')
        with mock.patch.object(L, 'thd_sign_in', side_effect=http.client.BadStatusLine('')):
            sign.invoke()
            self.body.destroy()
            run_ui_calls(self.app)
        self.app.collect.assert_not_called()

    def test_its_failure_on_the_step_shown(self):
        sign = self.step('huntersdream')
        with mock.patch.object(L, 'thd_sign_in', side_effect=http.client.BadStatusLine('')):
            sign.invoke()
            run_ui_calls(self.app)
        self.assertIn('Sign-in failed: BadStatusLine', texts(self.body))

    def test_the_supermedo_sign_in_after_the_guide_closed(self):
        def sign_in(_server, on_page):
            on_page('https://example.net/discord')
            return 'Hunter', 'secret'
        sign = self.step('supermedo')
        with mock.patch.object(L, 'discord_sign_in', side_effect=sign_in):
            sign.invoke()
            self.body.destroy()
            run_ui_calls(self.app, count=2)
        self.assertEqual(self.app.var('supermedo_npid', 'app').get(), 'Hunter')
        self.app.collect.assert_called_once()


class OnlinePageTests(unittest.TestCase):
    """The Online page's sign-ins, new account and connection test: each line answers whatever the worker met."""

    def setUp(self):
        import tkinter
        from tkinter import ttk
        root = hidden_root(self)
        for patcher in (mock.patch.object(L, 'LANG', 'en'), mock.patch.object(L, 'HUNTERS_DREAM_POOL', True),
                        mock.patch('webbrowser.open')):
            patcher.start()
            self.addCleanup(patcher.stop)
        app = self.app = bare_launcher(root=root, tk=tkinter, ttk=ttk, dpi=1, app=dict(L.APP_DEFAULTS), vars={},
                                       pages={}, content=ttk.Frame(root), refresh_status=mock.Mock(),
                                       ask_hunters=mock.Mock(), collect=mock.Mock())
        app.build_online()
        self.page = app.pages['online']

    def click(self, text, index=0):
        button(self.page, text, index).invoke()
        run_ui_calls(self.app)
        return texts(self.page)

    def test_the_connection_test_never_stays_at_checking(self):
        with mock.patch.object(L, 'server_check', side_effect=RuntimeError('a bug')):
            self.assertIn('The check stopped: RuntimeError: a bug', self.click('Test connection'))

    def test_the_hunters_dream_sign_in(self):
        with mock.patch.object(L, 'thd_sign_in', side_effect=http.client.BadStatusLine('')):
            self.assertIn('Sign-in failed: BadStatusLine', self.click('Sign in with Discord'))

    def test_the_supermedo_sign_in(self):
        with mock.patch.object(L, 'discord_sign_in', side_effect=http.client.IncompleteRead(b'')):
            self.assertIn('Sign-in failed: IncompleteRead(0 bytes read)', self.click('Sign in with Discord', 1))

    def test_a_new_account(self):
        import struct
        # Its boxes are folded until "No account yet? Make one" opens them (as the search's jump does).
        next(e for e in self.app.search_settings if e['key'] == 'button:create_account')['reveal']()
        for title, value in (('New Online ID', 'NewHunter'), ('New password', 'secret'),
                             ('Email address', 'hunter@example.net')):
            label, = [w for w in widgets(self.page) if w.winfo_class() == 'TLabel' and w.cget('text') == title]
            entry, = label.master.grid_slaves(row=int(label.grid_info()['row']), column=1)
            entry.insert(0, value)
        with mock.patch.object(L, 'shadnet_register', side_effect=struct.error('unpack requires a buffer of 4 bytes')):
            self.assertIn('The account could not be created: unpack requires a buffer of 4 bytes',
                          self.click('Create account'))
        self.app.collect.assert_not_called()


class ReportToolTests(unittest.TestCase):
    def test_the_button_comes_back_whatever_stopped_the_tool(self):
        """The BB Report Tool that ran past its 5 minutes: "Collecting the report…" and a grey button for good."""
        import subprocess
        folder = tempfile.TemporaryDirectory()
        self.addCleanup(folder.cleanup)
        app = bare_launcher(app={}, collect=mock.Mock())
        press, note = mock.Mock(), mock.Mock()
        with mock.patch.object(L, 'LANG', 'en'), mock.patch.object(L, 'DATA_DIR', Path(folder.name)), \
                mock.patch.object(L.bbport_help, 'report_tool', return_value=Path(folder.name) / 'tool.bat'), \
                mock.patch('subprocess.run', side_effect=subprocess.TimeoutExpired(['powershell'], 300)):
            app.save_report(press, note)
            run_ui_calls(app)
        self.assertEqual(press.state.call_args_list, [mock.call(['disabled']), mock.call(['!disabled'])])
        self.assertEqual(note.configure.call_args.kwargs['text'],
                         "Could not save it: Command '['powershell']' timed out after 300 seconds")


@unittest.skipUnless(os.name == 'nt', "the game test's monitor reads Windows' counters")
class MonitorTests(unittest.TestCase):
    def test_one_failed_sample_does_not_end_the_monitor(self):
        """A monitor that stopped read no more frame statistics: the game test took the game for frozen."""
        import bbport_benchmark

        class Quick(threading.Event):
            def wait(self, timeout=None):
                return super().wait(0.02)
        folder = tempfile.TemporaryDirectory()
        self.addCleanup(folder.cleanup)
        log = Path(folder.name) / 'last_run.log'
        log.write_text('', encoding='utf-8')
        asked = []

        def game_pids():
            asked.append(1)
            if len(asked) == 1:
                raise RuntimeError('a process list that changed while it was read')
            return []
        counters = mock.Mock(**{'sample.return_value': (None, None)})
        with mock.patch.object(bbport_benchmark, 'game_pids', side_effect=game_pids), \
                mock.patch.object(bbport_benchmark, 'GpuCounters', return_value=counters):
            monitor = bbport_benchmark.Monitor(log)
            monitor.stop_event = Quick()
            monitor.start()
            try:
                for _i in range(250):
                    if len(asked) >= 2:
                        break
                    time.sleep(0.02)
                log.write_text('Frame stats: 60.0 FPS, worst frame 20.1 ms; 3 shader/pipeline compiles, 12.0 ms, '
                               '1500 draws/frame\n', encoding='utf-8')
                for _i in range(250):
                    if monitor.frames:
                        break
                    time.sleep(0.02)
            finally:
                monitor.stop()
        self.assertGreaterEqual(len(asked), 2)
        self.assertEqual([frame['fps'] for frame in monitor.frames], [60.0])
        counters.close.assert_called_once()


class PlacerTests(unittest.TestCase):
    def test_one_failing_look_does_not_end_the_placing(self):
        import bbport_display
        import ctypes
        looks = []

        def find():
            looks.append(1)
            raise ctypes.ArgumentError('a window that is gone')
        placer = bbport_display.Placer({}, (1920, 1080), find=find).start()
        try:
            for _i in range(100):
                if len(looks) >= 2:
                    break
                time.sleep(0.05)
        finally:
            placer.stop()
        self.assertGreaterEqual(len(looks), 2)


if __name__ == '__main__':
    unittest.main()
