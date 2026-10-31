"""The start's prerequisites beyond the graphics card: a clean eboot.bin put back from a copy on the PC
(scripts/clean_eboot.py), the game files against the clean 1.09's checksums (scripts/game_files.py), a game folder
in OneDrive, the Visual C++ runtime AMD's DirectX 12 files need, and a firewall rule that blocks the game
(launcher/bbport_help.py)."""
from paths import ROOT
import ctypes
import gzip
import importlib.util
import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(ROOT / 'launcher'))
import clean_eboot as C  # noqa: E402
import game_files as G  # noqa: E402

spec = importlib.util.spec_from_file_location('bbport_help_ready', ROOT / 'launcher/bbport_help.py')
help_ = importlib.util.module_from_spec(spec)
spec.loader.exec_module(help_)


class Folder(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)


class CleanEbootTests(Folder):
    """A 60 FPS or otherwise patched eboot.bin: the clean one comes back from a copy on the PC. Made-up files stand
    for the 93 MB eboot: b'clean' is clean."""

    def setUp(self):
        super().setUp()
        self.game = self.root / 'Games' / 'CUSA03173'
        self.update = self.root / 'Games' / 'CUSA03173-UPDATE'
        for folder in (self.game, self.update):
            folder.mkdir(parents=True)
        (self.game / 'eboot.bin').write_bytes(b'patched')
        (self.update / 'eboot.bin').write_bytes(b'clean')
        for patch in (mock.patch.object(C, 'is_clean', lambda path: Path(path).read_bytes() == b'clean'),
                      mock.patch.object(C, 'app_version', lambda game: '01.09'),
                      mock.patch.object(C, 'SIZES', range(0, 100))):
            patch.start()
            self.addCleanup(patch.stop)

    def restore(self):
        return C.restore(self.game, places=[(self.game.parent, 2)])

    def test_the_update_folder_beside_the_game(self):
        source, backup = self.restore()
        self.assertEqual(source, self.update / 'eboot.bin')
        self.assertEqual((self.game / 'eboot.bin').read_bytes(), b'clean')
        self.assertEqual(backup.read_bytes(), b'patched')  # nothing of the player's is lost
        self.assertEqual(backup.name, 'eboot.bin.modified')
        self.assertIsNone(self.restore())  # clean now: nothing more to do
        self.assertIn('the old one is kept as eboot.bin.modified', C.note((source, backup)))

    def test_a_backup_beside_it_and_an_earlier_backup(self):
        (self.update / 'eboot.bin').unlink()
        (self.game / 'eboot_original.bin').write_bytes(b'clean')
        (self.game / 'eboot.bin.modified').write_bytes(b'from before')
        source, backup = self.restore()
        self.assertEqual(source.name, 'eboot_original.bin')
        self.assertEqual(backup.name, 'eboot.bin.modified-2')
        self.assertEqual((self.game / 'eboot.bin.modified').read_bytes(), b'from before')

    def test_nothing_found_leaves_everything(self):
        (self.update / 'eboot.bin').write_bytes(b'also patched')
        self.assertIsNone(self.restore())
        self.assertEqual((self.game / 'eboot.bin').read_bytes(), b'patched')
        self.assertEqual(sorted(p.name for p in self.game.iterdir()), ['eboot.bin'])

    def test_only_a_1_09_game_with_an_eboot(self):
        with mock.patch.object(C, 'app_version', lambda game: '01.00'):
            self.assertIsNone(self.restore())  # the base game needs the whole update
        (self.game / 'eboot.bin').unlink()
        self.assertIsNone(self.restore())  # no eboot.bin: not a game folder this fills
        self.assertFalse((self.game / 'eboot.bin').exists())

    def test_a_copy_that_went_wrong_replaces_nothing(self):
        with mock.patch.object(C.shutil, 'copyfile', lambda src, dst: Path(dst).write_bytes(b'cut')):
            self.assertIsNone(self.restore())
        self.assertEqual((self.game / 'eboot.bin').read_bytes(), b'patched')
        self.assertEqual(sorted(p.name for p in self.game.iterdir()), ['eboot.bin'])

    def test_not_into_the_ports_game_views(self):
        view = self.root / 'Games' / 'out' / 'mod-game-1'
        view.mkdir(parents=True)
        (self.update / 'eboot.bin').unlink()
        (view / 'eboot.bin').write_bytes(b'clean')
        self.assertIsNone(self.restore())

    def test_shadps4s_game_folders(self):
        appdata = self.root / 'AppData'
        (appdata / 'shadPS4').mkdir(parents=True)
        games = (self.root / 'PS4').as_posix()
        (appdata / 'shadPS4' / 'config.toml').write_text(f'[GUI]\ninstallDirs = ["{games}"]\n', encoding='utf-8')
        with mock.patch.dict(os.environ, {'APPDATA': str(appdata)}):
            self.assertEqual(C.shadps4_folders(), [Path(games)])

    def test_the_start_puts_it_back(self):
        source = (ROOT / 'run.py').read_text(encoding='utf-8')
        start = source.index("if env.get('BB_SKIP_GAME_CHECK') != '1':")
        self.assertLess(start, source.index('restored = restore_eboot(game)'))
        self.assertLess(source.index('restored = restore_eboot(game)'), source.index('prepatched = digest =='))


class CleanImageTests(unittest.TestCase):
    """CLEAN_IMAGE is the clean 1.09 eboot's: checked where a game folder is at hand (BB_GAME_DIR)."""

    @unittest.skipUnless(os.environ.get('BB_GAME_DIR') and
                         (Path(os.environ.get('BB_GAME_DIR', '')) / 'eboot.bin').is_file(), 'no game folder')
    def test_the_clean_eboot(self):
        import bbport_help as help_module
        game = Path(os.environ['BB_GAME_DIR'])
        if help_module.eboot_state(game)[0] == 'clean':
            self.assertTrue(C.is_clean(game / 'eboot.bin'))


class GameFilesTests(Folder):
    def setUp(self):
        super().setUp()
        self.game = self.root / 'game'
        for name, data in (('dvdroot_ps4/map/m32.msb', b'lake'), ('dvdroot_ps4/chr/c0000.bnd', b'hunter'),
                           ('sce_module/libc.prx', b'libc'), ('sce_sys/param.sfo', b'not checked')):
            (self.game / name).parent.mkdir(parents=True, exist_ok=True)
            (self.game / name).write_bytes(data)
        self.list = self.root / 'list.txt.gz'
        self.assertEqual(G.make(self.game, self.list), 3)  # sce_sys differs between dump tools: not in it
        self.clean = G.read_list(self.list)

    def test_a_clean_folder(self):
        seen = []
        result = G.check(self.game, lambda done, total: seen.append((done, total)), clean=self.clean)
        self.assertEqual(result, {'checked': 3, 'differ': [], 'missing': []})
        self.assertEqual(seen[-1], (14, 14))

    def test_a_changed_a_cut_and_a_missing_file(self):
        (self.game / 'dvdroot_ps4/map/m32.msb').write_bytes(b'LAKE')  # same size, other bytes
        (self.game / 'dvdroot_ps4/chr/c0000.bnd').write_bytes(b'hu')
        (self.game / 'sce_module/libc.prx').unlink()
        result = G.check(self.game, clean=self.clean)
        self.assertEqual(result['differ'], ['dvdroot_ps4/chr/c0000.bnd', 'dvdroot_ps4/map/m32.msb'])
        self.assertEqual(result['missing'], ['sce_module/libc.prx'])

    def test_the_list_of_the_clean_game(self):
        clean = G.read_list()
        self.assertGreater(len(clean), 28000)
        self.assertIn('dvdroot_ps4/shader/gxshader.shaderbnd.dcx', clean)
        self.assertNotIn('eboot.bin', clean)
        self.assertLess(G.LIST.stat().st_size, 2 << 20)  # checksums only, no game data
        with gzip.open(G.LIST, 'rt', encoding='utf-8') as stream:
            size, digest, name = stream.readline().rstrip('\n').split('\t')
        self.assertEqual((len(digest), int(size) > 0), (32, True))

    def test_the_list_has_the_clean_menu_textures(self):
        clean = G.read_list()
        self.assertEqual(clean['dvdroot_ps4/menu/common.tpf.dcx'], (9415151, 'f70fe66b2b7106c8be0e5a552c19c693'))
        self.assertFalse([name for name in clean if name.endswith('.bak')])
        self.assertEqual(len(clean), 28804)

    def test_the_full_result_is_written(self):
        (self.game / 'dvdroot_ps4/chr/c0000.bnd').write_bytes(b'hu')
        (self.game / 'sce_module/libc.prx').unlink()
        out = self.root / 'user' / 'game_files_check.txt'
        G.write_result(G.check(self.game, clean=self.clean), out)
        self.assertEqual(out.read_text(encoding='utf-8').splitlines(),
                         ['3 files: 1 differ, 1 missing', 'differ\tdvdroot_ps4/chr/c0000.bnd',
                          'missing\tsce_module/libc.prx'])


class GameFolderTests(Folder):
    def test_a_synced_game_folder(self):
        self.assertTrue(help_.synced_game(r'C:\Users\Hunter\OneDrive\Games\CUSA03173'))
        self.assertFalse(help_.synced_game(r'D:\Games\CUSA03173'))
        self.assertFalse(help_.synced_game(''))

    @unittest.skipUnless(sys.platform == 'win32', "Windows' file attributes")
    def test_files_kept_online_only(self):
        game = self.root / 'game'
        (game / 'dvdroot_ps4').mkdir(parents=True)
        for name in ('a.bin', 'b.bin'):
            (game / 'dvdroot_ps4' / name).write_bytes(b'x')
        offline = str(game / 'dvdroot_ps4' / 'b.bin')
        ctypes.windll.kernel32.SetFileAttributesW(offline, 0x1000)  # FILE_ATTRIBUTE_OFFLINE, as OneDrive sets it
        self.addCleanup(ctypes.windll.kernel32.SetFileAttributesW, offline, 0x80)
        scan = help_.scan_game(game)
        self.assertEqual((scan['online_only'], scan['first_online']), (1, os.path.join('dvdroot_ps4', 'b.bin')))

    def test_the_checks_name_it(self):
        game = self.root / 'game'
        game.mkdir()
        (game / 'eboot.bin').write_bytes(b'x')
        scan = {'files': 28800, 'bytes': 30 * 1024 ** 3, 'empty': [], 'shaders': True, 'online_only': 0,
                'first_online': None}
        with mock.patch.object(help_, 'eboot_state', return_value=('clean', 93_825_704)), \
                mock.patch.object(help_, 'scan_game', return_value=scan), \
                mock.patch.object(help_, 'synced_game', return_value=True), \
                mock.patch.object(help_, 'firewall_blocks', return_value=True):
            checks, _eboot = help_.run_checks(str(game), '', str(self.root / 'port'), '1920x1080')
            topics = [topic for _state, topic, _text in checks]
            self.assertIn('game_location', topics)
            self.assertIn('firewall', topics)
            scan.update(online_only=3, first_online='dvdroot_ps4\\map\\m29.msb')
            checks, _eboot = help_.run_checks(str(game), '', str(self.root / 'port'), '1920x1080')
        line = [text for state, topic, text in checks if topic == 'game_location']
        self.assertEqual(len(line), 1)
        self.assertIn('3 files of the game are kept online only', line[0])

    def test_a_restored_eboot_is_said_once(self):
        game = self.root / 'game'
        game.mkdir()
        (game / 'eboot.bin').write_bytes(b'x')
        states = iter([('modified', 1), ('clean', 1)])
        with mock.patch.object(help_, 'eboot_state', lambda folder: next(states)), \
                mock.patch.object(help_, 'scan_game', return_value={'files': 28800, 'bytes': 30 * 1024 ** 3,
                                                                    'empty': [], 'shaders': True}), \
                mock.patch.object(help_, 'restore_eboot',
                                  return_value=(Path('D:/PS4/CUSA03173-UPDATE/eboot.bin'),
                                                game / 'eboot.bin.modified')):
            checks, eboot = help_.run_checks(str(game), '', str(self.root / 'port'), '1920x1080')
        self.assertEqual(eboot['state'], 'clean')
        self.assertIn(('ok', None, 'eboot.bin was not the clean 1.09 file: the clean copy from '
                       f'{Path("D:/PS4/CUSA03173-UPDATE")} is used now, the other one is kept as eboot.bin.modified.'),
                      checks)


class RuntimeTests(Folder):
    def test_missing_files(self):
        system = self.root / 'System32'
        system.mkdir()
        (system / 'vcruntime140.dll').write_bytes(b'x')
        port = self.root / 'port'
        (port / 'bin').mkdir(parents=True)
        (port / 'bin' / 'msvcp140.dll').write_bytes(b'x')  # beside the game's program counts too
        self.assertEqual(help_.missing_runtime(port, system), ['vcruntime140_1.dll'])

    def test_only_when_amds_files_failed_to_load(self):
        fsr = {'fsr411': {'dx12': {'supported': False, 'reason': 'runtime', 'detail': 'Windows error 126'}}}
        fg = {'fsr401': {'supported': False, 'reason': 'rx9000'}}
        self.assertTrue(help_.runtime_failed(fsr, fg))
        self.assertFalse(help_.runtime_failed({'fsr411': {'reason': 'ready'}}, fg, None))

    def test_the_amd_files_need_it(self):
        # The DirectX 12 files' imports (fetched for the package); the port's own programs use Windows' C runtime.
        names = {name.lower() for name in help_.VC_RUNTIME}
        self.assertEqual(names, {'msvcp140.dll', 'vcruntime140.dll', 'vcruntime140_1.dll'})
        self.assertTrue(help_.VC_REDIST.startswith('https://aka.ms/'))


class FirewallTests(unittest.TestCase):
    PROGRAM = r'C:\Games\BBPort\bin\bb-probe.exe'

    def rule(self, **fields):
        values = {'Action': 'Block', 'Active': 'TRUE', 'Dir': 'In', 'Protocol': '17', 'Profile': 'Public',
                  'App': self.PROGRAM, 'Name': 'bb-probe.exe', **fields}
        return 'v2.10|' + '|'.join(f'{key}={value}' for key, value in values.items()) + '|'

    def test_cancel_at_windows_question(self):
        self.assertTrue(help_.firewall_blocks(self.PROGRAM, [self.rule()]))
        self.assertTrue(help_.firewall_blocks(self.PROGRAM.lower(), [self.rule()]))  # Windows' paths: any case

    def test_what_does_not_block_it(self):
        for fields in ({'Action': 'Allow'}, {'Active': 'FALSE'}, {'Dir': 'Out'},
                       {'App': r'C:\Games\Other\bin\bb-probe.exe'}):
            with self.subTest(**fields):
                self.assertFalse(help_.firewall_blocks(self.PROGRAM, [self.rule(**fields)]))
        self.assertFalse(help_.firewall_blocks(self.PROGRAM, []))

    @unittest.skipUnless(sys.platform == 'win32', "Windows' firewall rules")
    def test_this_pcs_rules_can_be_read(self):
        self.assertIsInstance(help_.firewall_blocks(Path(os.environ.get('TEMP', '.')) / 'no-such.exe'), bool)


class TopicTests(unittest.TestCase):
    def test_each_new_topic_has_its_button(self):
        for topic, method in (('vc_runtime', 'open_vc_redist'), ('game_location', 'choose_game_page'),
                              ('game_files', 'check_game_files'), ('firewall', 'open_firewall_settings')):
            with self.subTest(topic=topic):
                self.assertIn(topic, help_.TOPIC_ORDER)
                self.assertEqual(help_.TOPIC_ACTIONS[topic][0][1], method)

    def test_the_play_page_has_their_lines(self):
        source = (ROOT / 'launcher' / 'bbport_launcher_win.py').read_text(encoding='utf-8')
        for key in ('files', 'runtime', 'firewall'):
            with self.subTest(key=key):
                self.assertIn(f"self.check_line('{key}',", source)
                self.assertIn(f"'{key}'", source[source.index("for key in ('news', 'system', 'gpu'"):][:200])


if __name__ == '__main__':
    unittest.main()
