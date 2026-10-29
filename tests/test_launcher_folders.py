"""The saves folder apart from the caches (launcher/bbport_launcher_win.py cache_folder): the shaders
stay in this installation's user folder wherever the saves are (a cloud folder, another installation,
any folder), unless the player chose a cache folder; and the mods' order after a drag (moved)."""
from paths import ROOT
import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(ROOT / 'launcher'))
sys.path.insert(0, str(ROOT / 'scripts'))
import bbport_launcher_win as L  # noqa: E402

ONEDRIVE = {'OneDrive': r'C:\Users\Hunter\OneDrive', 'OneDriveConsumer': '', 'OneDriveCommercial': ''}


class CacheFolderTests(unittest.TestCase):
    def setUp(self):
        patch = mock.patch.dict(os.environ, ONEDRIVE)
        patch.start()
        self.addCleanup(patch.stop)

    def test_cloud_folders_are_recognised(self):
        for path in (r'C:\Users\Hunter\OneDrive\Bloodborne', r'C:\Users\Hunter\OneDrive', r'D:\Dropbox\BB saves',
                     r'E:\Google Drive\saves', r'C:\Users\Hunter\OneDrive - Firma\BB', r'F:\Nextcloud\bb'):
            self.assertTrue(L.synced_folder(path), path)
        for path in (r'C:\Games\BBPort\user', r'C:\Users\Hunter\OneDriveBackup', r'D:\Drop\box'):
            self.assertFalse(L.synced_folder(path), path)

    def test_saves_elsewhere_keep_the_shaders_in_this_installation(self):
        settings = {**L.APP_DEFAULTS, 'user_dir': r'D:\BB\user'}
        self.assertEqual(L.cache_folder(settings), L.DATA_DIR / 'user')
        env = self.environment(settings)
        self.assertEqual(env['BB_USER_DIR'], r'D:\BB\user')
        self.assertEqual(env['BB_GPU_USER_DIR'], str(L.DATA_DIR / 'user'))

    def test_saves_in_this_installation_keep_the_caches_beside_them(self):
        settings = {**L.APP_DEFAULTS, 'user_dir': ''}
        self.assertEqual(L.cache_folder(settings), L.saves_folder(settings))
        self.assertNotIn('BB_GPU_USER_DIR', self.environment(settings))  # run.py puts them there, as before

    def test_saves_in_a_cloud_folder_keep_the_shaders_on_this_pc(self):
        settings = {**L.APP_DEFAULTS, 'user_dir': r'C:\Users\Hunter\OneDrive\Bloodborne'}
        self.assertEqual(L.cache_folder(settings), L.DATA_DIR / 'user')
        env = self.environment(settings)
        self.assertEqual(env['BB_USER_DIR'], r'C:\Users\Hunter\OneDrive\Bloodborne')
        self.assertEqual(env['BB_GPU_USER_DIR'], str(L.DATA_DIR / 'user'))

    def test_a_cache_folder_of_the_players_choice(self):
        settings = {**L.APP_DEFAULTS, 'user_dir': r'C:\Users\Hunter\OneDrive\Bloodborne', 'cache_dir': r'D:\BB cache'}
        self.assertEqual(self.environment(settings)['BB_GPU_USER_DIR'], r'D:\BB cache')

    @staticmethod
    def environment(settings):
        with mock.patch.dict(os.environ, {'BB_GPU_USER_DIR': ''}):
            os.environ.pop('BB_GPU_USER_DIR')
            return L.game_environment(settings)


class OtherInstallationCacheTests(unittest.TestCase):
    """Testers put a beta beside their port and kept its saves folder (the launcher's settings are
    shared): the beta then used that build's shaders, and one tester's picture turned dark ("962 stale
    pipelines" in the log, gone with the cache cleared). With the cache field empty, this installation
    keeps its own."""

    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.root = Path(tmp.name)
        self.old, self.beta = self.root / 'bbport', self.root / 'bbport-beta'
        for port in (self.old, self.beta):
            (port / 'bin').mkdir(parents=True)
            (port / 'run.py').write_text('run')
            (port / 'Bloodborne.exe').write_bytes(b'MZ')
            (port / 'user').mkdir()
        for patch in (mock.patch.object(L, 'PORT_DIR', self.beta), mock.patch.object(L, 'DATA_DIR', self.beta),
                      mock.patch.dict(os.environ, {'OneDrive': '', 'OneDriveConsumer': '', 'OneDriveCommercial': ''})):
            patch.start()
            self.addCleanup(patch.stop)

    def test_saves_of_another_installation_keep_the_shaders_in_this_one(self):
        settings = {**L.APP_DEFAULTS, 'user_dir': str(self.old / 'user')}
        self.assertTrue(L.saves_in_other_installation(settings))
        self.assertEqual(L.cache_folder(settings), self.beta / 'user')
        env = CacheFolderTests.environment(settings)
        self.assertEqual(env['BB_USER_DIR'], str(self.old / 'user'))  # the characters stay shared
        self.assertEqual(env['BB_GPU_USER_DIR'], str(self.beta / 'user'))

    def test_a_cache_folder_of_the_players_choice_stays(self):
        settings = {**L.APP_DEFAULTS, 'user_dir': str(self.old / 'user'), 'cache_dir': r'D:\BB cache'}
        self.assertEqual(L.cache_folder(settings), Path(r'D:\BB cache'))

    def test_own_saves_keep_the_caches_beside_them(self):
        for user in ('', str(self.beta / 'user')):
            settings = {**L.APP_DEFAULTS, 'user_dir': user}
            self.assertFalse(L.saves_in_other_installation(settings), user)
            self.assertEqual(L.cache_folder(settings), self.beta / 'user', user)
            self.assertNotIn('BB_GPU_USER_DIR', CacheFolderTests.environment(settings), user)

    def test_unrelated_saves_keep_the_shaders_in_this_installation(self):
        settings = {**L.APP_DEFAULTS, 'user_dir': str(self.root / 'my saves')}
        self.assertFalse(L.saves_in_other_installation(settings))  # no note on the Saves page
        self.assertEqual(L.cache_folder(settings), self.beta / 'user')
        self.assertEqual(CacheFolderTests.environment(settings)['BB_GPU_USER_DIR'], str(self.beta / 'user'))

    def test_for_the_old_installation_its_saves_are_its_own(self):
        settings = {**L.APP_DEFAULTS, 'user_dir': str(self.old / 'user')}
        with mock.patch.object(L, 'PORT_DIR', self.old), mock.patch.object(L, 'DATA_DIR', self.old):
            self.assertEqual(L.cache_folder(settings), self.old / 'user')


class FolderEntriesTests(unittest.TestCase):
    """What the Saves and Mods pages are drawn from: drawn again on a page change only when it changed."""

    def test_entries_change_with_the_folder(self):
        import tempfile
        with tempfile.TemporaryDirectory() as folder:
            before = L.folder_entries(folder)
            (Path(folder) / 'mods.json').write_text('{}')
            (Path(folder) / 'other').write_text('x')
            self.assertNotEqual(L.folder_entries(folder), before)
            self.assertEqual([name for name, _ in L.folder_entries(folder, ('mods.json',))], ['mods.json'])
        self.assertEqual(L.folder_entries(Path(folder) / 'gone'), ())


class ModOrderTests(unittest.TestCase):
    ORDER = ['a', 'b', 'c', 'd']

    def test_down_up_and_to_the_end(self):
        self.assertEqual(L.moved(self.ORDER, 0, 3), ['b', 'c', 'a', 'd'])
        self.assertEqual(L.moved(self.ORDER, 3, 0), ['d', 'a', 'b', 'c'])
        self.assertEqual(L.moved(self.ORDER, 1, 4), ['a', 'c', 'd', 'b'])

    def test_dropped_where_it_was(self):
        for source in range(4):
            self.assertEqual(L.moved(self.ORDER, source, source), self.ORDER)
            self.assertEqual(L.moved(self.ORDER, source, source + 1), self.ORDER)


if __name__ == '__main__':
    unittest.main()
