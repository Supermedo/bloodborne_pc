"""Final-save snapshots through the existing backup browser, without a game dump."""
from paths import ROOT
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock
import zipfile

sys.path.insert(0, str(ROOT / 'launcher'))
import bbport_exit_backup as exits
import bbport_saves as saves
import bbport_launcher_win as launcher

class ExitBackupTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.user = Path(self.temp.name)
        self.folder = saves.target_dir(self.user)
        self.folder.mkdir(parents=True)
        self.slot = self.folder / 'userdata0000'
        self.slot.write_bytes(b'initial')
        (self.folder / saves.PROFILE).write_bytes(b'profile')

    def process(self, value=b'final', code=0):
        def wait():
            self.slot.write_bytes(value)
            return code
        return mock.Mock(pid=123, wait=mock.Mock(side_effect=wait))

    def test_final_write_is_captured_and_native_exit_metadata_is_kept(self):
        self.assertEqual(exits.wait_and_backup(self.process(code=9), self.user, enabled=True), 9)
        item, = saves.backups(self.user)
        self.assertTrue(item.ok)
        self.assertEqual(item.reason, 'exit')
        with zipfile.ZipFile(item.path) as zipped:
            slot = next(name for name in zipped.namelist() if name.endswith('/userdata0000'))
            self.assertEqual(zipped.read(slot), b'final')
            meta = json.loads(zipped.read(saves.BACKUP_META))
            self.assertEqual(meta['game_pid'], 123)
            self.assertEqual(meta['exit_code'], 9)
            self.assertEqual(meta['sha256'][slot], hashlib.sha256(b'final').hexdigest())

    def test_every_exit_keeps_a_separate_snapshot(self):
        for index in range(3):
            exits.wait_and_backup(self.process(str(index).encode()), self.user, enabled=True)
        items = saves.backups(self.user)
        self.assertEqual(len(items), 3)
        self.assertEqual(len({item.path for item in items}), 3)
        self.assertTrue(all(item.reason == 'exit' for item in items))

    def test_disabled_option_never_copies_saves(self):
        with mock.patch.object(saves, 'backup') as backup:
            self.assertEqual(exits.wait_and_backup(self.process(code=7), self.user), 7)
        backup.assert_not_called()

    def test_backup_failure_does_not_change_game_exit_code(self):
        with mock.patch.object(saves, 'backup', side_effect=OSError('disk full')):
            self.assertEqual(exits.wait_and_backup(self.process(code=5), self.user, enabled=True, log=lambda line: None), 5)

    def test_bad_sha256_is_rejected_before_restore_or_import(self):
        exits.wait_and_backup(self.process(), self.user, enabled=True)
        item, = saves.backups(self.user)
        with zipfile.ZipFile(item.path) as zipped:
            files = {info.filename: zipped.read(info.filename) for info in zipped.infolist()}
        slot = next(name for name in files if name.endswith('/userdata0000'))
        files[slot] = b'changed with a valid ZIP CRC but wrong SHA256'
        with zipfile.ZipFile(item.path, 'w') as zipped:
            for name, data in files.items(): zipped.writestr(name, data)
        self.slot.write_bytes(b'current progress')
        with self.assertRaisesRegex(OSError, 'SHA-256'):
            saves.restore(item.path, self.user)
        with self.assertRaisesRegex(OSError, 'SHA-256'):
            saves.add_backup_file(item.path, self.user / 'another-user')
        self.assertEqual(self.slot.read_bytes(), b'current progress')

    def test_failed_verification_never_publishes_a_partial_backup(self):
        original = saves.write_backup
        def changed(target, files, meta):
            self.slot.write_bytes(b'changed while being copied')
            return original(target, files, meta)
        with mock.patch.object(saves, 'write_backup', side_effect=changed):
            exits.wait_and_backup(self.process(), self.user, enabled=True, log=lambda line: None)
        self.assertEqual(list(saves.backup_dir(self.user).iterdir()), [])

    def test_real_subprocess_finishes_before_backup(self):
        code = 'from pathlib import Path; import sys; Path(sys.argv[1]).write_bytes(b"last write"); sys.exit(7)'
        result = exits.run_with_backup([sys.executable, '-c', code, str(self.slot)], self.user,
            creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
        self.assertEqual(result, 7)
        item, = saves.backups(self.user)
        with zipfile.ZipFile(item.path) as zipped:
            slot = next(name for name in zipped.namelist() if name.endswith('/userdata0000'))
            self.assertEqual(zipped.read(slot), b'last write')

    def test_game_and_ui_languages_and_backup_preference_are_independent(self):
        self.assertTrue({'10', '11'} <= {code for code, _label in launcher.LANGUAGES})
        settings = {**launcher.APP_DEFAULTS, 'ui_language': 'zh', 'language': '10', 'exit_backup': True}
        env = launcher.game_environment(settings)
        self.assertEqual(env['BB_UI_LANGUAGE'], 'zh')
        self.assertEqual(env['BB_LANGUAGE'], '10')
        self.assertEqual(env['BB_EXIT_BACKUP'], '1')
        self.assertFalse(launcher.APP_DEFAULTS['exit_backup'])

if __name__ == '__main__': unittest.main()
