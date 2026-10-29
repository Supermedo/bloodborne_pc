"""Player mistakes the launcher and run.py repair on their own, or name the one thing to do where nothing can be
repaired (Discord help threads, the audit of 2026-10-24)."""
from paths import ROOT
import importlib.util
import os
import subprocess
import sys
import unittest
import uuid
from unittest import mock

sys.path.insert(0, str(ROOT / 'launcher'))
sys.path.insert(0, str(ROOT / 'scripts'))
import bbport_launcher_win as L  # noqa: E402

spec = importlib.util.spec_from_file_location('bbport_run_mistakes', ROOT / 'run.py')
run = importlib.util.module_from_spec(spec)
spec.loader.exec_module(run)


def bare_launcher(**values):
    """A Launcher without its window, for the methods that need only a few of its parts."""
    ui = L.Launcher.__new__(L.Launcher)
    ui.process = ui.job = ui.placer = None
    ui.fsr411_downloading = False
    ui.app = dict(L.APP_DEFAULTS)
    ui.logged = []
    ui.append = ui.logged.append
    ui.status = mock.Mock()
    ui.messagebox = mock.Mock()
    for key, value in values.items():
        setattr(ui, key, value)
    return ui


@unittest.skipUnless(os.name == 'nt', 'Windows named objects')
class GameRunsAlreadyTests(unittest.TestCase):
    """Play while a game started from Steam, a shortcut or the in-game restart is being prepared or runs."""

    def test_a_start_holds_the_mutex_the_launcher_looks_for(self):
        name = f'Local\\bbport-test-{uuid.uuid4().hex}'
        with mock.patch.object(L.bench, 'game_pids', return_value=[]):
            self.assertFalse(L.game_underway(name))
            self.assertTrue(run.hold_game_mutex(name))
            self.assertTrue(L.game_underway(name))
        # A second start (another process) is turned away while this one holds it.
        code = ("import importlib.util, sys; spec = importlib.util.spec_from_file_location('r', r'%s'); "
                "r = importlib.util.module_from_spec(spec); spec.loader.exec_module(r); "
                "sys.exit(0 if r.hold_game_mutex(r'%s') else 7)") % (ROOT / 'run.py', name)
        self.assertEqual(subprocess.run([sys.executable, '-c', code], timeout=60).returncode, 7)

    def test_the_game_process_counts_too(self):
        with mock.patch.object(L.bench, 'game_pids', return_value=[1234]):
            self.assertTrue(L.game_underway(f'Local\\bbport-test-{uuid.uuid4().hex}'))

    def test_play_brings_the_running_game_to_the_front(self):
        ui = bare_launcher()
        with mock.patch.object(L, 'game_underway', return_value=True), \
                mock.patch.object(L, 'front_game_window') as front, \
                mock.patch.object(L.subprocess, 'Popen') as popen:
            ui.play()
        front.assert_called_once_with()
        popen.assert_not_called()
        self.assertIn('brought to the front', ui.logged[0])
        ui.messagebox.assert_not_called()  # no question, no message

    def test_play_bloodborne_exe_does_not_start_a_second_game(self):
        with mock.patch.object(L, 'game_underway', return_value=True), \
                mock.patch.object(L, 'front_game_window') as front, \
                mock.patch.object(L, 'attach_stdio'), \
                mock.patch.object(L, 'save_ini') as save_ini, \
                mock.patch.object(L.subprocess, 'Popen') as popen:
            self.assertEqual(L.play_without_window(dict(L.APP_DEFAULTS)), 0)
        front.assert_called_once_with()
        popen.assert_not_called()
        save_ini.assert_not_called()

    def test_import_respec_and_update_wait_for_any_game(self):
        source = (ROOT / 'launcher' / 'bbport_launcher_win.py').read_text(encoding='utf-8')
        respec = source[source.index('def open_respec'):source.index('user = Path(', source.index('def open_respec'))]
        update = source[source.index('def install_version'):source.index('if not FROZEN or not url')]
        setup = (ROOT / 'launcher' / 'bbport_setup.py').read_text(encoding='utf-8')
        imports = setup[setup.index('def import_save'):setup.index('user, here =', setup.index('def import_save'))]
        for text in (respec, update, imports):
            self.assertIn('self.game_running()', text)
        ui = bare_launcher()
        with mock.patch.object(L, 'game_underway', return_value=True):
            self.assertTrue(ui.game_running())
        with mock.patch.object(L, 'game_underway', return_value=False):
            self.assertFalse(ui.game_running())


class AntivirusLockTests(unittest.TestCase):
    """An antivirus scanning a file the port has just written holds it for a moment."""

    def result(self, code, errors=b'', output=b''):
        return subprocess.CompletedProcess([], code, output, errors)

    def test_a_locked_file_is_tried_again(self):
        locked = self.result(1, b"PermissionError: [WinError 32] The process cannot access the file 'boot.bin'\n")
        with mock.patch.object(run.subprocess, 'run', side_effect=[locked, locked, self.result(0)]) as calls, \
                mock.patch.object(run.time, 'sleep') as sleep, mock.patch.object(run, 'echo_errors'):
            self.assertIsNone(run.run_script('prepare.py', 'game'))
        self.assertEqual(calls.call_count, 3)
        self.assertEqual(sleep.call_count, 2)

    def test_errno_13_counts_and_the_start_fails_after_about_two_seconds(self):
        locked = self.result(1, b"PermissionError: [Errno 13] Permission denied: 'out\\\\patches.bin'\n")
        with mock.patch.object(run.subprocess, 'run', return_value=locked) as calls, \
                mock.patch.object(run.time, 'sleep') as sleep, mock.patch.object(run, 'echo_errors'):
            with self.assertRaises(SystemExit):
                run.run_script('patches.py', '--out', 'out')
        self.assertEqual(calls.call_count, run.LOCK_TRIES)
        self.assertLessEqual(sum(c.args[0] for c in sleep.call_args_list), 2.5)

    def test_other_failures_are_not_repeated(self):
        broken = self.result(1, b'prepare failed: out of bounds: 0x5000+0x1000 / 0x4000\n')
        with mock.patch.object(run.subprocess, 'run', return_value=broken) as calls, \
                mock.patch.object(run, 'echo_errors'):
            self.assertIsNone(run.run_script('mods.py', 'game', capture=True, optional=True))
            self.assertFalse(run.run_script('quit_menu.py', 'game', optional=True))
        self.assertEqual(calls.call_count, 2)

    def test_captured_output_is_text_as_before(self):
        with mock.patch.object(run.subprocess, 'run', return_value=self.result(0, output=b'C:\\out\\mod-game-1\r\n')), \
                mock.patch.object(run, 'echo_errors'):
            self.assertEqual(run.run_script('mods.py', 'game', capture=True), 'C:\\out\\mod-game-1\n')

    def test_the_errors_are_passed_on(self):
        import io
        stream = io.StringIO()
        with mock.patch.object(run.sys, 'stderr', stream):
            run.echo_errors('Mods: Prüfung: 3 files\n'.encode('utf-8'))
        self.assertEqual(stream.getvalue(), 'Mods: Prüfung: 3 files\n')

    def test_a_save_import_waits_for_the_lock(self):
        import bbport_saves as saves
        steps = iter([PermissionError(13, 'Permission denied'), PermissionError(32, 'in use'), 'done'])

        def step():
            item = next(steps)
            if isinstance(item, Exception):
                raise item
            return item
        with mock.patch.object(saves.time, 'sleep') as sleep:
            self.assertEqual(saves.retried(step), 'done')
        self.assertEqual(sleep.call_count, 2)
        with mock.patch.object(saves.time, 'sleep'), self.assertRaises(PermissionError):
            saves.retried(mock.Mock(side_effect=PermissionError(13, 'Permission denied')))

    def test_what_still_fails_is_named_as_the_antivirus(self):
        import bbport_help as help_
        for line in ("PermissionError: [Errno 13] Permission denied: 'C:\\\\BBPort\\\\out\\\\boot-libc.bin'",
                     'bb-probe.exe is missing from C:\\BBPort\\bin: an antivirus has probably moved it'):
            self.assertEqual(help_.hints_in_log(line), ['antivirus'], line)
        source = (ROOT / 'run.py').read_text(encoding='utf-8')
        self.assertIn("fail(f'bb-probe.exe is missing from", source)  # the player's text when packaged


class SettingsNotWritableTests(unittest.TestCase):
    """A port folder without write permission or a full drive: Play did nothing, without a word."""

    def test_play_names_the_file_and_the_folder(self):
        ui = bare_launcher()
        ui.var = lambda key, store: mock.Mock(get=mock.Mock(return_value=False if key == 'online' else ''))
        full = OSError(28, 'There is not enough space on the disk', r'D:\Games\BBPort\bbport.ini')
        with mock.patch.object(L, 'game_underway', return_value=False), \
                mock.patch.object(L.Launcher, 'collect', side_effect=full), \
                mock.patch.object(L.subprocess, 'Popen') as popen:
            ui.play()
        popen.assert_not_called()
        text = ui.messagebox.showerror.call_args.args[1]
        self.assertIn('bbport.ini', text)
        self.assertIn(r'D:\Games\BBPort', text)
        self.assertIn('There is not enough space on the disk', text)

    def test_an_unexpected_error_is_shown_once_and_logged(self):
        import tkinter as tk
        root = tk.Tk()
        root.withdraw()
        self.addCleanup(root.destroy)
        ui = bare_launcher(root=root)
        root.report_callback_exception = ui.callback_error

        def broken():
            raise ValueError('could not convert string to float: 0,5')
        import io
        with mock.patch.object(L.sys, 'stderr', io.StringIO()):
            for _ in range(3):
                root.after(0, broken)
                root.update()
        self.assertEqual(ui.messagebox.showerror.call_count, 1)
        self.assertIn('0,5', ui.messagebox.showerror.call_args.args[1])
        self.assertEqual(len(ui.logged), 3)
        self.assertIn('ValueError', ui.logged[0])
        source = (ROOT / 'launcher' / 'bbport_launcher_win.py').read_text(encoding='utf-8')
        main = source[source.index('\ndef main():'):]
        self.assertIn('root.report_callback_exception = launcher.callback_error', main)


class PkgToolLayoutTests(unittest.TestCase):
    """PKG tools such as PkgEditor extract sce_sys as Sc0 (inside the game folder, or Image0 and Sc0 side by
    side); a dump without sce_module or sce_sys failed with a path inside the game view."""

    def setUp(self):
        import tempfile
        from pathlib import Path
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        self.game, self.sc0, self.out = self.root / 'Image0', self.root / 'Sc0', self.root / 'out'
        for folder in (self.game / 'sce_module', self.game / 'dvdroot_ps4' / 'chr', self.game / 'sce_sys' / 'about',
                       self.sc0 / 'trophy'):
            folder.mkdir(parents=True)
        (self.game / 'eboot.bin').write_bytes(b'eboot')
        (self.game / 'dvdroot_ps4' / 'chr' / 'c0000.chrbnd.dcx').write_bytes(b'original')
        (self.game / 'sce_sys' / 'about' / 'right.sprx').write_bytes(b'about')  # the image's own part of sce_sys
        (self.sc0 / 'param.sfo').write_bytes(b'sfo')
        (self.sc0 / 'trophy' / 'trophy00.trp').write_bytes(b'trophies')

    def files(self, folder):
        return {p.relative_to(folder).as_posix(): p.read_bytes() for p in folder.rglob('*') if p.is_file()}

    def test_sc0_is_found_inside_or_beside_the_game_folder(self):
        import prepare
        self.assertEqual(prepare.system_folder(self.game), self.sc0)
        inside = self.game / 'Sc0'
        self.sc0.rename(inside)
        self.assertEqual(prepare.system_folder(self.game), inside)
        (self.game / 'sce_sys' / 'param.sfo').write_bytes(b'sfo')
        self.assertEqual(prepare.system_folder(self.game), self.game / 'sce_sys')
        self.assertEqual(prepare.system_folder(self.root / 'nothing'), self.root / 'nothing' / 'sce_sys')

    def test_a_view_of_its_own_maps_sc0_and_leaves_the_dump_alone(self):
        import mods
        before, sc0 = self.files(self.game), self.files(self.sc0)
        view = run.system_view(self.game, None, self.sc0, self.out)
        self.assertTrue(view.name.startswith('mod-game-'))
        self.assertEqual(self.files(view / 'sce_sys'), {'about/right.sprx': b'about', 'param.sfo': b'sfo',
                                                         'trophy/trophy00.trp': b'trophies'})
        self.assertEqual((view / 'eboot.bin').read_bytes(), b'eboot')
        self.assertEqual((view / 'dvdroot_ps4' / 'chr' / 'c0000.chrbnd.dcx').read_bytes(), b'original')
        mods.remove_overlay(view, self.game)
        self.assertFalse(view.exists())
        self.assertEqual(self.files(self.game), before)
        self.assertEqual(self.files(self.sc0), sc0)

    def test_the_view_made_for_mods_gets_sce_sys(self):
        import mods
        mod = self.root / 'mods' / 'a mod' / 'dvdroot_ps4' / 'chr'
        mod.mkdir(parents=True)
        (mod / 'c0000.chrbnd.dcx').write_bytes(b'modded')
        view = mods.build_overlay(self.game, self.out, [('a mod', mod.parent.parent)])
        before = self.files(self.game)
        self.assertIs(run.system_view(self.game, view, self.sc0, self.out), view)
        self.assertEqual((view / 'sce_sys' / 'param.sfo').read_bytes(), b'sfo')
        self.assertEqual((view / 'dvdroot_ps4' / 'chr' / 'c0000.chrbnd.dcx').read_bytes(), b'modded')
        self.assertEqual(self.files(self.game), before)  # only the view's link to sce_sys went, not its files
        mods.remove_overlay(view, self.game)
        self.assertEqual(self.files(self.game), before)

    def test_a_missing_folder_is_named_before_the_start(self):
        import shutil
        import bbport_help as help_
        self.assertIsNone(help_.system_problem(self.game))  # Sc0 serves
        shutil.rmtree(self.game / 'sce_module')
        self.assertIn('sce_module', help_.system_problem(self.game))
        shutil.rmtree(self.sc0)
        self.assertIn('sce_module / sce_sys', help_.system_problem(self.game))
        with mock.patch.object(help_, 'scan_game', return_value={'files': 28800, 'bytes': 30 * 1024 ** 3,
                                                                 'empty': [], 'shaders': True}), \
                mock.patch.object(help_, 'eboot_state', return_value=('clean', 93_825_704)):
            checks, _eboot = help_.run_checks(str(self.game), '', 'C:\\Games\\BBPort', '1920x1080')
        self.assertIn(('bad', None, help_.system_problem(self.game)), checks)
        self.assertEqual(L.start_problem({'game_dir': str(self.game)}), (help_.system_problem(self.game), 'game'))
        ui = bare_launcher()
        ui.app['game_dir'], ui.ini = str(self.game), dict(L.INI_DEFAULTS)
        ui.var = lambda key, store: mock.Mock(get=mock.Mock(return_value=False if key == 'online' else ''))
        ui.show = mock.Mock()
        with mock.patch.object(L, 'game_underway', return_value=False), \
                mock.patch.object(L.Launcher, 'save_settings', return_value=True), \
                mock.patch.object(L.subprocess, 'Popen') as popen:
            ui.play()
        popen.assert_not_called()
        self.assertIn('sce_module', ui.messagebox.showerror.call_args.args[1])
        ui.show.assert_called_with('game')


class MisplacedSaveTests(unittest.TestCase):
    """Saves copied into the saves folder where the game does not read them: another title's folder, shadPS4's
    savedata/1000, a folder level too many. The Play page said "Saves found", the game showed none."""

    def setUp(self):
        import tempfile
        from pathlib import Path
        import bbport_saves as saves
        self.saves = saves
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.user = Path(temp.name) / 'user'
        self.target = saves.target_dir(self.user, '')  # savedata/1/CUSA00207/SPRJ0005

    def save(self, folder, names=('userdata0000', 'userdata0010'), data=b'Maria'):
        folder.mkdir(parents=True, exist_ok=True)
        for name in names:
            (folder / name).write_bytes(data)
        return folder

    def test_the_one_save_found_is_copied_to_where_the_game_reads_it(self):
        for wrong in ('savedata/1/CUSA00900/SPRJ0005', 'savedata/1000/CUSA00207/SPRJ0005',
                      'savedata/CUSA00207/SPRJ0005', 'Bloodborne save/SPRJ0005'):
            with self.subTest(wrong=wrong):
                import shutil
                shutil.rmtree(self.user, ignore_errors=True)
                source = self.save(self.user / wrong)
                self.assertFalse(L.has_complete_save(self.user, ''))
                line, encrypted = L.misplaced_saves({'user_dir': str(self.user), 'game_dir': ''})
                self.assertIn('copied to where the game reads them', line)
                self.assertIsNone(encrypted)
                self.assertEqual((self.target / 'userdata0010').read_bytes(), b'Maria')
                self.assertEqual((source / 'userdata0010').read_bytes(), b'Maria')  # copied, never moved
                self.assertTrue(L.has_complete_save(self.user, ''))
                self.assertEqual(self.saves.misplaced(self.user, ''), ([], []))  # done: nothing next time

    def test_what_was_there_is_backed_up_first(self):
        self.save(self.target, names=('userdata0000',), data=b'half')  # no profile: the game crashes on it
        self.save(self.user / 'savedata/1/CUSA00900/SPRJ0005')
        adopted, _encrypted = self.saves.adopt_misplaced(self.user, '')
        self.assertTrue(adopted)
        import zipfile
        backups = list(self.saves.backup_dir(self.user).glob('*.zip'))
        self.assertEqual(len(backups), 1)
        with zipfile.ZipFile(backups[0]) as zipped:
            self.assertEqual(zipped.read('savedata/1/CUSA00207/SPRJ0005/userdata0000'), b'half')
        self.assertEqual((self.target / 'userdata0000').read_bytes(), b'Maria')

    def test_nothing_is_copied_when_it_is_not_clear(self):
        self.save(self.user / 'savedata/1/CUSA00900/SPRJ0005')
        self.save(self.user / 'savedata/1000/CUSA00207/SPRJ0005')
        self.assertEqual(self.saves.adopt_misplaced(self.user, '')[0], None)  # two: which one is the player's?
        self.assertFalse(self.target.exists())

    def test_the_ports_own_save_and_its_copies_are_left_alone(self):
        self.save(self.target, data=b'own')
        self.save(self.user / 'savedata/1/CUSA00900/SPRJ0005')
        self.assertEqual(self.saves.adopt_misplaced(self.user, ''), (None, []))
        self.assertEqual((self.target / 'userdata0000').read_bytes(), b'own')
        import shutil
        shutil.rmtree(self.user)
        for copy in ('respec-backups/20261024-120000/SPRJ0005', 'save-backups/20261024-120000/savedata/1/CUSA00207/'
                     'SPRJ0005', 'save_import'):
            self.save(self.user / copy)
        self.assertEqual(self.saves.misplaced(self.user, ''), ([], []))

    def test_an_encrypted_ps4_save_is_named_once(self):
        folder = self.user / 'savedata/PS4/SAVEDATA/1a2b3c/CUSA00207'
        folder.mkdir(parents=True)
        (folder / 'sdimg_SPRJ0005').write_bytes(b'encrypted')
        settings = {'user_dir': str(self.user), 'game_dir': ''}
        self.assertEqual(L.misplaced_saves(settings), (None, str(folder)))
        settings['encrypted_told'] = str(folder)
        self.assertEqual(L.misplaced_saves(settings), (None, None))
        ui = bare_launcher()
        ui.app.update(user_dir=str(self.user), game_dir='')
        ui.refresh_saves = mock.Mock()
        line, encrypted = L.misplaced_saves(ui.app)
        self.assertIsNone(line)
        source = (ROOT / 'launcher' / 'bbport_launcher_win.py').read_text(encoding='utf-8')
        play = source[source.index('    def play(self):'):source.index('    def auto_backup(self):')]
        self.assertIn("self.app['encrypted_told'] = encrypted", play)
        self.assertIn('Apollo Save Tool', play)

    def test_the_play_page_counts_only_a_save_the_game_can_load(self):
        self.save(self.user / 'savedata/1/CUSA00207/SPRJ0005', names=('userdata0000',))
        self.assertFalse(L.has_complete_save(self.user, ''))
        self.save(self.target)
        self.assertTrue(L.has_complete_save(self.user, ''))


class HandEditedFileTests(unittest.TestCase):
    """bbport.ini and input.ini saved in ANSI or with sharpness=0,5 kept the launcher from opening; a broken
    settings.json silently became the defaults (the game folder and the sign-in gone)."""

    def setUp(self):
        import tempfile
        from pathlib import Path
        import bbport_textfile as textfile
        self.textfile = textfile
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.folder = Path(temp.name)
        for patch in (mock.patch.dict(os.environ, {'BB_CONFIG': str(self.folder / 'bbport.ini')}),
                      mock.patch.multiple(L, CONFIG_DIR=self.folder / 'launcher',
                                          CONFIG_FILE=self.folder / 'launcher' / 'settings.json'),
                      mock.patch.object(textfile, 'REPAIRS', [])):
            patch.start()
            self.addCleanup(patch.stop)

    def test_bbport_ini_edited_in_notepad(self):
        ini = self.folder / 'bbport.ini'
        original = b'# Sch\xe4rfe: 0,5 ist gut\r\nupscaler=fsr3\r\nsharpness=0,5\r\nshow_fps=0\r\nfov=wide\r\n'
        ini.write_bytes(original)
        values, lines = L.load_ini({})
        self.assertEqual((values['sharpness'], values['show_fps'], values['fov'], values['upscaler']),
                         ('0.50', '0', '43', 'fsr3'))
        text = ini.read_bytes().decode('utf-8')  # UTF-8 now
        self.assertIn('# Sch?rfe: 0,5 ist gut', text)  # the comment stays, the unreadable letter as ?
        self.assertIn('sharpness=0.50', text)
        self.assertEqual((self.folder / 'bbport.ini.bad').read_bytes(), original)
        self.assertEqual(len(self.textfile.REPAIRS), 1)
        self.assertIn('bbport.ini.bad', self.textfile.REPAIRS[0])
        self.assertEqual(L.load_ini({})[0]['sharpness'], '0.50')  # nothing more to repair
        self.assertEqual(len(self.textfile.REPAIRS), 1)
        self.assertEqual(L.Launcher.ini_text('sharpness', float(values['sharpness'])), '0.50')

    def test_good_values_are_left_as_they_are(self):
        ini = self.folder / 'bbport.ini'
        ini.write_bytes('\ufeffsharpness=0.5\nshow_fps=1\noutput_res=1920x1200\nfov=60\n'.encode('utf-8'))
        before = ini.read_bytes()
        values = L.load_ini({})[0]
        self.assertEqual((values['sharpness'], values['output_res']), ('0.5', '1920x1200'))
        self.assertFalse((self.folder / 'bbport.ini.bad').exists())
        self.assertEqual(self.textfile.REPAIRS, [])
        self.assertTrue(ini.read_bytes().endswith(before[-20:]))

    def test_a_broken_settings_json_comes_back_from_the_last_good_start(self):
        import json
        good = dict(game_dir=r'D:\Games\CUSA03173', thd_token='secret', thd_online_id='Maria')
        L.write_settings(good)
        self.assertEqual(L.load_settings(), good)  # a good load keeps a copy
        backup = L.CONFIG_FILE.with_name('settings.json.bak')
        self.assertEqual(json.loads(backup.read_text(encoding='utf-8')), good)
        L.CONFIG_FILE.write_text('{"game_dir": "D:\\Games\\CUSA03173", "thd_token": ', encoding='utf-8')  # cut off
        self.assertEqual(L.load_settings(), good)
        self.assertEqual(json.loads(L.CONFIG_FILE.read_text(encoding='utf-8')), good)  # written back
        self.assertIn('thd_token', L.CONFIG_FILE.with_name('settings.json.bad').read_text(encoding='utf-8'))
        self.assertIn('settings.json.bad', self.textfile.REPAIRS[-1])

    def test_without_a_copy_the_defaults_and_the_broken_file_is_kept(self):
        L.CONFIG_DIR.mkdir()
        L.CONFIG_FILE.write_text('{ broken', encoding='utf-8')
        self.assertEqual(L.load_settings(), {})
        self.assertEqual(L.CONFIG_FILE.with_name('settings.json.bad').read_text(encoding='utf-8'), '{ broken')
        L.remember_window((1600, 900))  # never the window size alone over settings that could not be read
        self.assertEqual(L.CONFIG_FILE.read_text(encoding='utf-8'), '{ broken')

    def test_input_ini_in_another_encoding(self):
        import bbport_input_config as config
        path = self.folder / 'input.ini'
        original = b'# Steuerung f\xfcr Maria\ncross = space\n'
        path.write_bytes(original)
        bindings = config.load_input_ini(path)[0]
        self.assertEqual(bindings['cross'], ['space'])
        self.assertEqual(path.read_bytes().decode('utf-8').replace('\r\n', '\n'), '# Steuerung f?r Maria\ncross = space\n')
        self.assertEqual((self.folder / 'input.ini.bad').read_bytes(), original)
        self.assertIn('input.ini.bad', self.textfile.REPAIRS[0])

    def test_a_write_is_whole_or_not_at_all(self):
        path = self.folder / 'settings.ini'
        path.write_text('old', encoding='utf-8')
        with mock.patch.object(self.textfile.os, 'replace', side_effect=OSError(28, 'No space left on device')):
            with self.assertRaises(OSError):
                self.textfile.write(path, 'new')
        self.assertEqual(path.read_text(encoding='utf-8'), 'old')
        self.assertEqual(sorted(p.name for p in self.folder.iterdir()), ['settings.ini'])  # no temp left
        self.textfile.write(path, 'new')
        self.assertEqual(path.read_text(encoding='utf-8'), 'new')
        self.assertEqual(self.textfile.keep_bad(path).name, 'settings.ini.bad')
        self.assertEqual(self.textfile.keep_bad(path).name, 'settings.ini.bad2')  # never over an earlier one

    def test_run_py_reads_a_hand_edited_ini(self):
        ini = self.folder / 'bbport.ini'
        ini.write_bytes(b'# Sch\xe4rfe\nfsr411_backend=dx12\n')
        env = {'BB_SHARED_GRAPHICS_CONFIG': '1'}
        run.refresh_shared_graphics(env, ini)
        self.assertEqual(env['BB_FSR411_BACKEND'], 'dx12')

    def test_the_repairs_go_into_the_next_starts_log(self):
        source = (ROOT / 'launcher' / 'bbport_launcher_win.py').read_text(encoding='utf-8')
        play = source[source.index('    def play(self):'):source.index('    def auto_backup(self):')]
        self.assertIn('for line in textfile.REPAIRS:', play)
        self.assertIn("args=(self.process, self.log.get('1.0', 'end-1c'))", play)
        self.textfile.REPAIRS.append('Settings: repaired')
        with mock.patch.object(L, 'game_underway', return_value=False), mock.patch.object(L, 'attach_stdio'), \
                mock.patch.object(L, 'load_ini', return_value=(dict(L.INI_DEFAULTS), [])), \
                mock.patch.object(L.display, 'Placer'), mock.patch.object(L.trophies, 'Tracker'), \
                mock.patch.object(L.subprocess, 'Popen') as popen:
            popen.return_value.stdout.readline.return_value = b''
            popen.return_value.wait.return_value = 0
            settings = dict(L.APP_DEFAULTS, user_dir=str(self.folder / 'user'), auto_backup=False)
            self.assertEqual(L.play_without_window(settings), 0)
        self.assertEqual((self.folder / 'user' / 'last_run.log').read_text(encoding='utf-8'), 'Settings: repaired\n')


class OwnOutputSizeTests(unittest.TestCase):
    """1920x1200, 2560x1600, 1366x768: the game's menu knows ten sizes, and its first save wrote 1920x1080."""

    def setUp(self):
        import tempfile
        from pathlib import Path
        import bbport_textfile as textfile
        self.textfile = textfile
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.folder = Path(temp.name)
        self.ini = self.folder / 'bbport.ini'
        for patch in (mock.patch.dict(os.environ, {'BB_CONFIG': str(self.ini)}),
                      mock.patch.multiple(L, CONFIG_DIR=self.folder / 'launcher',
                                          CONFIG_FILE=self.folder / 'launcher' / 'settings.json'),
                      mock.patch.object(textfile, 'REPAIRS', [])):
            patch.start()
            self.addCleanup(patch.stop)

    def test_what_is_remembered(self):
        self.assertEqual(L.custom_output('1920x1200', {}), '1920x1200')
        self.assertEqual(L.custom_output('2560x1440', {'output_custom': '1920x1200'}), '')  # chosen in the menu
        self.assertEqual(L.custom_output('1920x1080', {'output_custom': '1920x1200'}), '1920x1200')
        self.assertEqual(L.custom_output('1920x1200', {'output_display': True}), '')
        self.assertEqual(L.own_output({'output_custom': '1920x1200'}, '1920x1080'), '1920x1200')
        self.assertIsNone(L.own_output({'output_custom': '1920x1200'}, '2560x1440'))
        self.assertIsNone(L.own_output({'output_custom': '1920x1200', 'output_display': True}, '1920x1080'))
        self.assertIsNone(L.own_output({}, '1920x1080'))
        # A listed size or the monitor's chosen on the launcher's page ends the own size (1920x1080 too).
        source = (ROOT / 'launcher' / 'bbport_launcher_win.py').read_text(encoding='utf-8')
        choice = source[source.index('    def output_choice'):source.index('    def select_fsr411_backend')]
        self.assertEqual(choice.count("self.app['output_custom'] = ''"), 2)

    def collect(self, ui):
        ui.sync_ini = lambda: None  # what the game wrote is in ui.ini already
        ui.collect()

    def test_the_launcher_sets_it_again_at_every_save(self):
        ui = bare_launcher(vars={}, remap_edits={}, mouse_edited=False, mod_order=[], patch_vars={})
        ui.ini, ui.ini_lines = dict(L.INI_DEFAULTS, output_res='1920x1200'), []
        self.collect(ui)
        self.assertEqual(ui.app['output_custom'], '1920x1200')
        ui.ini['output_res'] = '1920x1080'  # the game's menu saved
        self.collect(ui)
        self.assertEqual(L.load_ini({})[0]['output_res'], '1920x1200')
        self.assertIn('1920x1200', self.textfile.REPAIRS[0])
        ui.ini['output_res'] = '2560x1440'  # the player chose another size in the menu
        self.collect(ui)
        self.assertEqual((ui.app['output_custom'], L.load_ini({})[0]['output_res']), ('', '2560x1440'))

    def test_play_bloodborne_exe_sets_it_again(self):
        self.ini.write_text('output_res=1920x1080\n', encoding='utf-8')
        settings = dict(L.APP_DEFAULTS, output_custom='2560x1600', user_dir=str(self.folder / 'user'),
                        auto_backup=False)
        with mock.patch.object(L, 'game_underway', return_value=False), mock.patch.object(L, 'attach_stdio'), \
                mock.patch.object(L.display, 'Placer') as placer, mock.patch.object(L.trophies, 'Tracker'), \
                mock.patch.object(L.subprocess, 'Popen') as popen:
            popen.return_value.stdout.readline.return_value = b''
            popen.return_value.wait.return_value = 0
            self.assertEqual(L.play_without_window(settings), 0)
        self.assertEqual(L.load_ini({})[0]['output_res'], '2560x1600')
        self.assertEqual(placer.call_args.args[1], (2560, 1600))
        self.assertIn('2560x1600', (self.folder / 'user' / 'last_run.log').read_text(encoding='utf-8'))


class RefusedSignInTests(unittest.TestCase):
    """The Hunter's Dream refused the saved token: the game said so only in its log and played offline, and the
    launcher still showed "Signed in"."""
    LINE = ("THD: the server refused this PC's sign-in as Maria (401 token revoked): sign in again on the launcher's "
            "Online page; until then nobody can summon or invite this player\n")

    def setUp(self):
        import tempfile
        from pathlib import Path
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.folder = Path(temp.name)
        self.user = self.folder / 'user'
        self.user.mkdir()
        patch = mock.patch.multiple(L, CONFIG_DIR=self.folder / 'launcher',
                                    CONFIG_FILE=self.folder / 'launcher' / 'settings.json',
                                    HUNTERS_DREAM_POOL=True)  # off by default since SuperMedo Server
        patch.start()
        self.addCleanup(patch.stop)
        self.settings = dict(L.APP_DEFAULTS, user_dir=str(self.user), online=True, online_pool='huntersdream',
                             thd_online_id='Maria', thd_token='old-token', game_dir='D:\\CUSA03173')
        L.write_settings(self.settings)

    def test_the_refusal_is_found_in_the_log(self):
        self.assertFalse(L.sign_in_refused(self.settings))  # no log yet
        (self.user / 'last_run.log').write_text('Runtime: started\n' + self.LINE, encoding='utf-8')
        self.assertTrue(L.sign_in_refused(self.settings))
        L.forget_sign_in()
        saved = L.load_settings()
        self.assertEqual((saved['thd_token'], saved['thd_online_id'], saved['game_dir']), ('', 'Maria', 'D:\\CUSA03173'))
        self.assertTrue(L.needs_sign_in(saved))
        self.assertFalse(L.needs_sign_in(dict(saved, online_pool='shadnet')))

    def test_after_the_launchers_game(self):
        import types
        (self.user / 'last_run.log').write_text(self.LINE, encoding='utf-8')
        token = types.SimpleNamespace(value='old-token')
        token.get, token.set = (lambda: token.value), (lambda value: setattr(token, 'value', value))
        ui = bare_launcher()
        ui.app.update(self.settings)
        ui.var = lambda key, store: token
        ui.forget_refused_sign_in()
        self.assertEqual((token.value, ui.app['thd_token'], L.load_settings()['thd_token']), ('', '', ''))
        self.assertIn('sign in again', ui.logged[0])
        ui.logged.clear()
        ui.forget_refused_sign_in()  # nothing more to forget
        self.assertEqual(ui.logged, [])
        source = (ROOT / 'launcher' / 'bbport_launcher_win.py').read_text(encoding='utf-8')
        self.assertIn("thd_token.trace_add('write', lambda *_a: show_accounts())", source)  # "Not signed in"

    def test_after_play_bloodborne_exe_and_the_next_start_asks(self):
        line = self.LINE.encode('utf-8')
        with mock.patch.object(L, 'game_underway', return_value=False), mock.patch.object(L, 'attach_stdio'), \
                mock.patch.object(L, 'load_ini', return_value=(dict(L.INI_DEFAULTS), [])), \
                mock.patch.object(L.display, 'Placer'), mock.patch.object(L.trophies, 'Tracker'), \
                mock.patch.object(L.subprocess, 'Popen') as popen:
            popen.return_value.stdout.readline.side_effect = [line, b'']
            popen.return_value.wait.return_value = 0
            self.assertEqual(L.play_without_window(dict(self.settings, auto_backup=False)), 0)
        self.assertEqual(L.load_settings()['thd_token'], '')
        self.assertIn('the next start asks to sign in', (self.user / 'last_run.log').read_text(encoding='utf-8'))
        # The next Play Bloodborne.exe opens the launcher, whose Play asks: sign in now or play offline.
        import tempfile
        from pathlib import Path
        with tempfile.TemporaryDirectory() as temp:
            game = Path(temp)
            (game / 'eboot.bin').touch()
            for folder in ('sce_module', 'sce_sys'):
                (game / folder).mkdir()
            (game / 'sce_sys' / 'param.sfo').touch()
            root, ui = mock.Mock(), mock.Mock()
            import tkinter as tk
            # main() sets the language from Windows: put back for the other tests
            with mock.patch.object(L, 'LANG', L.LANG), mock.patch.object(L.saves, 'WORDS', dict(L.saves.WORDS)), \
                    mock.patch.object(L.sys, 'argv', ['Bloodborne.exe', '--play']), mock.patch.object(L, 'FROZEN', False), \
                    mock.patch.object(L, 'load_settings', return_value=dict(self.settings, thd_token='', game_dir=temp)), \
                    mock.patch.object(L, 'play_without_window') as play, mock.patch.object(L, 'one_launcher', return_value=True), \
                    mock.patch.object(L, 'bring_to_front_on_request'), mock.patch.object(tk, 'Tk', return_value=root), \
                    mock.patch.object(L, 'Launcher', return_value=ui):
                L.main()
            play.assert_not_called()
            root.after.assert_any_call(500, ui.play)


class WrongClockTests(unittest.TestCase):
    """A PC whose clock is wrong: its newest backup lay in the future and stopped every automatic backup, and every
    certificate looked expired."""

    def setUp(self):
        import tempfile
        from pathlib import Path
        import bbport_saves as saves
        self.saves = saves
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.user = Path(temp.name)
        target = saves.target_dir(self.user, '')
        target.mkdir(parents=True)
        for name in ('userdata0000', 'userdata0010'):
            (target / name).write_bytes(b'Maria')

    def test_a_backup_from_the_future_does_not_stop_the_automatic_ones(self):
        import datetime
        future = datetime.datetime.now() + datetime.timedelta(days=400)  # made while the clock was a year ahead
        with mock.patch.object(self.saves.datetime, 'datetime', mock.Mock(now=mock.Mock(return_value=future),
                                                                           fromisoformat=datetime.datetime.fromisoformat,
                                                                           fromtimestamp=datetime.datetime.fromtimestamp,
                                                                           strptime=datetime.datetime.strptime)):
            self.assertTrue(self.saves.backup(self.user, '', reason='auto'))
        notes = []
        made = self.saves.auto_backup(self.user, '', notes=notes)
        self.assertTrue(made)
        self.assertIn('dated in the future', notes[0])
        self.assertIsNone(self.saves.auto_backup(self.user, ''))  # nothing played since this one

    def test_a_wrong_clock_is_named_instead_of_the_certificate(self):
        error = OSError("<urlopen error [SSL: CERTIFICATE_VERIFY_FAILED] certificate verify failed: certificate has "
                        "expired (_ssl.c:1006)>")
        with mock.patch.object(L, 'clock_off', return_value=3 * 86400) as asked:
            self.assertIn("Your PC's date is wrong", L.connection_error(error, 'https://thehuntersdream.com'))
        asked.assert_called_once_with('thehuntersdream.com')
        with mock.patch.object(L, 'clock_off', return_value=30):  # the clock is right: Windows' root store, not the date
            self.assertEqual(L.connection_error(error, 'https://api.github.com/x'), str(error))
        with mock.patch.object(L, 'clock_off', side_effect=[None, -2 * 86400]) as asked:  # no time from that server
            self.assertIn("Your PC's date is wrong", L.connection_error(error, 'https://api.github.com/x'))
        self.assertEqual([c.args[0] for c in asked.call_args_list], ['api.github.com', L.TIME_HOST])
        with mock.patch.object(L, 'clock_off') as asked:
            self.assertEqual(L.connection_error(OSError('timed out'), 'https://api.github.com/x'), 'timed out')
        asked.assert_not_called()

    def test_the_server_time_comes_from_a_plain_http_answer(self):
        import email.utils
        import time
        answer = mock.Mock()
        answer.getresponse.return_value.getheader.return_value = email.utils.formatdate(time.time() - 7200, usegmt=True)
        with mock.patch('http.client.HTTPConnection', return_value=answer) as connect:
            self.assertAlmostEqual(L.clock_off('example.org'), 7200, delta=60)
        connect.assert_called_once_with('example.org', 80, timeout=6)
        with mock.patch('http.client.HTTPConnection', side_effect=OSError('offline')):
            self.assertIsNone(L.clock_off('example.org'))

    def test_the_messages_that_show_it(self):
        source = (ROOT / 'launcher' / 'bbport_launcher_win.py').read_text(encoding='utf-8')
        for call in ('connection_error(error, COMMUNITY_SERVER)', 'connection_error(failure, RELEASES_API)',
                     'connection_error(failure, url)'):
            self.assertIn(call, source)


class MissingFolderTests(unittest.TestCase):
    """A saves or cache folder of the player's own on a drive that is not connected, or renamed: run.py made an
    empty saves folder (no characters) or stopped with a traceback."""

    def setUp(self):
        import tempfile
        from pathlib import Path
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.folder = Path(temp.name)

    def test_a_missing_saves_folder_is_named_and_never_made(self):
        gone = self.folder / 'E-drive' / 'Bloodborne saves'
        with self.assertRaises(SystemExit), mock.patch.object(run.sys, 'stderr'):
            run.user_folders({'BB_USER_DIR': str(gone)}, self.folder)
        self.assertFalse(gone.exists())
        with mock.patch.object(L.bbport_help, 'system_problem', return_value=None):
            problem = L.start_problem({'game_dir': '', 'user_dir': str(gone)})
        self.assertEqual(problem[1], 'saves')
        self.assertIn(str(gone), problem[0])
        with mock.patch.object(L.bbport_help, 'system_problem', return_value=None):
            self.assertIsNone(L.start_problem({'game_dir': '', 'user_dir': ''}))  # the port's own: made when needed

    def test_the_ports_own_saves_folder_is_made(self):
        env = {}
        self.assertEqual(run.user_folders(env, self.folder), (self.folder / 'user').resolve())
        self.assertTrue((self.folder / 'user').is_dir())
        self.assertEqual(env['BB_GPU_USER_DIR'], str((self.folder / 'user').resolve()))

    def test_a_cache_folder_that_cannot_be_had_gives_way_to_the_saves_folder(self):
        import io
        blocked = self.folder / 'a file'
        blocked.write_text('not a folder')
        env = {'BB_GPU_USER_DIR': str(blocked / 'cache')}
        out = io.StringIO()
        with mock.patch.object(run.sys, 'stdout', out):
            user = run.user_folders(env, self.folder)
        self.assertEqual(env['BB_GPU_USER_DIR'], str(user))
        self.assertIn('the caches go to', out.getvalue())
        env = {'BB_GPU_USER_DIR': str(self.folder / 'new cache')}  # missing on a drive that is there: made
        run.user_folders(env, self.folder)
        self.assertEqual(env['BB_GPU_USER_DIR'], str(self.folder / 'new cache'))
        self.assertTrue((self.folder / 'new cache').is_dir())


class TemporaryFolderTests(unittest.TestCase):
    """Started from inside a zip or rar: WinRAR and 7-Zip unpack into a temporary folder that goes when the archive
    closes, with the saves in it. Dropbox and Google Drive were not flagged as synced folders."""

    def test_an_archive_programs_folder(self):
        import tempfile
        import bbport_help as help_
        temp = tempfile.gettempdir()
        for folder in (os.path.join(temp, 'Rar$EXa12345.6789', 'BBPort'), os.path.join(temp, '7zO8C1B.tmp', 'BBPort'),
                       os.path.join(temp, 'Temp1_bbport-1.9.4.zip', 'BBPort'), r'D:\Rar$EXb99.1\BBPort'):
            self.assertTrue(help_.temporary_folder(folder), folder)
        self.assertFalse(help_.temporary_folder(r'C:\Games\BBPort'))
        # Other folders in %TEMP% are no archive's: the launcher's own update, a developer's checkout.
        self.assertFalse(help_.temporary_folder(os.path.join(temp, 'bbport-update', 'new', 'BBPort')))
        self.assertFalse(help_.temporary_folder(os.path.join(temp, 'claude', 'scratchpad', 'wt-mistakes')))
        self.assertFalse(help_.temporary_folder(r'C:\Games\Temp1 notes.zip files\BBPort'))

    def test_the_warning_once_per_start_only_with_the_saves_in_there(self):
        import tempfile
        from pathlib import Path
        port = Path(tempfile.gettempdir()) / 'Rar$EXa12.3' / 'BBPort'
        ui = bare_launcher()
        with mock.patch.multiple(L, PORT_DIR=port, DATA_DIR=port):
            ui.temporary_warning()
            self.assertIn('Extract the port to a folder first', ui.messagebox.showwarning.call_args.args[1])
            ui.messagebox.reset_mock()
            ui.app['user_dir'] = r'D:\Bloodborne saves'  # saves of their own elsewhere: nothing to lose
            ui.temporary_warning()
            ui.messagebox.showwarning.assert_not_called()
        source = (ROOT / 'launcher' / 'bbport_launcher_win.py').read_text(encoding='utf-8')
        self.assertEqual(source.count('root.after(800, self.temporary_warning)'), 1)  # once, when the window opens

    def test_dropbox_and_google_drive_are_named_like_onedrive(self):
        import bbport_help as help_
        self.assertEqual(help_.location_problem(r'C:\Users\a\Dropbox\BBPort'), 'location')
        self.assertEqual(help_.location_problem(r'G:\My Drive\..\Google Drive\BBPort'), 'location')
        self.assertIsNone(help_.location_problem(r'C:\Games\BBPort'))


class TranslationTests(unittest.TestCase):
    """The new texts in every language of the launcher (Russian beside the English in the code)."""

    def test_every_new_text_is_used_and_translated(self):
        import string
        import bbport_lang
        import bbport_lang_mistakes as texts
        source = ''.join((ROOT / 'launcher' / name).read_text(encoding='utf-8')
                         for name in ('bbport_launcher_win.py', 'bbport_help.py'))
        languages = [code for code, _name in bbport_lang.LANGUAGE_NAMES if code not in ('', 'en', 'ru')]
        self.assertEqual(sorted(texts.MISTAKES), sorted(languages))
        fields = lambda text: [f[1] for f in string.Formatter().parse(text) if f[1] is not None]  # noqa: E731
        for key in texts.MISTAKE_KEYS:
            self.assertIn(key[:40], source)  # a text the launcher shows (its start, before any line break)
            for language in languages:
                with self.subTest(language=language, text=key[:30]):
                    translated = bbport_lang.table(language)[key]
                    self.assertNotEqual(translated, key)
                    self.assertEqual(fields(translated), fields(key))  # the same {} in the same order


if __name__ == '__main__':
    unittest.main()
