"""run.py remove_stale_views: the game views with mods (mod-game-*) a game left behind are removed at the
next start, while no other game runs; the game folder they link into stays as it was."""
from paths import ROOT
import importlib.util
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(ROOT / 'scripts'))
import mods  # noqa: E402

spec = importlib.util.spec_from_file_location('bbport_run_stale_views', ROOT / 'run.py')
run = importlib.util.module_from_spec(spec)
spec.loader.exec_module(run)


class StaleViewTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        root = Path(self.tmp.name)
        self.game, self.out, mod = root / 'CUSA03173', root / 'out', root / 'mods' / 'a mod'
        (self.game / 'dvdroot_ps4' / 'chr').mkdir(parents=True)
        (self.game / 'eboot.bin').write_bytes(b'eboot')
        (self.game / 'dvdroot_ps4' / 'chr' / 'c0000.chrbnd.dcx').write_bytes(b'original')
        (self.game / 'dvdroot_ps4' / 'other.bin').write_bytes(b'other')
        (mod / 'dvdroot_ps4' / 'chr').mkdir(parents=True)
        (mod / 'dvdroot_ps4' / 'chr' / 'c0000.chrbnd.dcx').write_bytes(b'modded')
        # A view as a game leaves it when run.py never reached its cleanup: links into the game.
        self.view = mods.build_overlay(self.game, self.out, [('a mod', mod)])
        mods.LINKED.clear()  # another run made it
        self.assertTrue(self.view.name.startswith('mod-game-'))

    def game_files(self):
        return {path.relative_to(self.game).as_posix(): path.read_bytes()
                for path in self.game.rglob('*') if path.is_file()}

    def test_removed_when_no_game_runs(self):
        before = self.game_files()
        with mock.patch.object(run, 'game_running', return_value=False):
            run.remove_stale_views(self.out, self.game)
        self.assertFalse(self.view.exists())
        self.assertEqual(self.game_files(), before)

    def test_kept_while_a_game_runs(self):
        with mock.patch.object(run, 'game_running', return_value=True):
            run.remove_stale_views(self.out, self.game)
        self.assertTrue(self.view.is_dir())

    def test_only_its_own_folders(self):
        (self.out / 'boot-linked.bin').write_bytes(b'kept')
        (self.game.parent / 'mod-game-notes.txt').write_text('a file, not a view')
        with mock.patch.object(run, 'game_running', return_value=False):
            run.remove_stale_views(self.out, self.game)
        self.assertTrue((self.out / 'boot-linked.bin').is_file())
        self.assertTrue((self.game.parent / 'mod-game-notes.txt').is_file())
        self.assertTrue((self.game / 'eboot.bin').is_file())


if __name__ == '__main__':
    unittest.main()
