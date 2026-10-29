from paths import ROOT
import importlib.util
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('bbport_run', ROOT / 'run.py')
run = importlib.util.module_from_spec(spec)
spec.loader.exec_module(run)


class SaveBackupTests(unittest.TestCase):
    def test_copies_the_saves_and_keeps_the_newest(self):
        with tempfile.TemporaryDirectory() as temp:
            user = Path(temp)
            self.assertIsNone(run.backup_saves(user))  # no saves yet: nothing to copy
            save = user / 'savedata/1/CUSA00207/SPRJ0005'
            save.mkdir(parents=True)
            (save / 'userdata0000').write_bytes(b'one')
            copy = run.backup_saves(user, keep=3, now=1_000_000)
            self.assertEqual((copy / '1/CUSA00207/SPRJ0005/userdata0000').read_bytes(), b'one')
            for step in range(1, 5):
                run.backup_saves(user, keep=3, now=1_000_000 + step * 60)
            kept = sorted(p.name for p in (user / 'save-backups').iterdir())
            self.assertEqual(len(kept), 3)
            self.assertNotIn(copy.parent.name, kept)  # the oldest went first
            # The saves themselves are untouched.
            self.assertEqual((save / 'userdata0000').read_bytes(), b'one')

    def test_a_failed_copy_still_starts_the_game(self):
        with tempfile.TemporaryDirectory() as temp:
            user = Path(temp)
            (user / 'savedata/1/x').mkdir(parents=True)
            (user / 'savedata/1/x/userdata0000').write_bytes(b'one')
            (user / 'save-backups').write_text('a file where the folder goes')
            self.assertIsNone(run.backup_saves(user))


class WritableSaveTests(unittest.TestCase):
    def test_read_only_save_files_are_made_writable(self):
        import os
        import stat
        with tempfile.TemporaryDirectory() as temp:
            user = Path(temp)
            self.assertEqual(run.writable_saves(user), 0)  # no saves yet
            save = user / 'savedata/1/CUSA00207/SPRJ0005'
            meta = save.with_name('SPRJ0005.sce_sys')
            for folder in (save, meta):
                folder.mkdir(parents=True)
            for path in (save / 'userdata0000', save / 'userdata0010', meta / 'param.bin'):
                path.write_bytes(b'save')
            os.chmod(save / 'userdata0000', stat.S_IREAD)
            os.chmod(meta / 'param.bin', stat.S_IREAD)
            self.assertEqual(run.writable_saves(user), 2)
            for path in (save / 'userdata0000', meta / 'param.bin'):
                self.assertTrue(os.stat(path).st_mode & stat.S_IWRITE)
                path.write_bytes(b'saved again')  # the game can replace it now
            self.assertEqual((save / 'userdata0010').read_bytes(), b'save')  # contents untouched
            self.assertEqual(run.writable_saves(user), 0)


if __name__ == '__main__':
    unittest.main()
