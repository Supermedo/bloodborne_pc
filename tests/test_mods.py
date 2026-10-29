from paths import ROOT
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import unittest.mock

spec = importlib.util.spec_from_file_location('bbmods', ROOT / 'scripts/mods.py')
mods = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mods)


class ModTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.game = self.root / 'CUSA03173'
        self.assets = self.game / 'dvdroot_ps4' / 'chr'
        self.assets.mkdir(parents=True)
        (self.game / 'eboot.bin').write_bytes(b'original executable')
        (self.assets / 'a.dcx').write_bytes(b'original')
        (self.assets / 'b.dcx').write_bytes(b'untouched')
        self.moddir = self.root / 'mods'

    def mod(self, name, contents=b'mod', file='a.dcx'):
        root = self.moddir / name
        folder = root / 'dvdroot_ps4' / 'chr'
        folder.mkdir(parents=True, exist_ok=True)
        (folder / file).write_bytes(contents)
        return root

    def test_a_mod_on_another_drive_is_copied_once(self):
        # Hard links from the mods folder fail (another drive than the view): a 1.9.4 player's
        # texture mod was copied whole into every view, 400 GB in out. Now: copied once, then linked.
        a = self.mod('A', b'big texture')
        real_link = os.link

        def link(source, destination):
            if Path(source).resolve().is_relative_to(self.moddir.resolve()):
                raise OSError(18, 'Not the same device')
            return real_link(source, destination)
        out = self.root / 'out'
        with unittest.mock.patch.object(mods.os, 'link', link):
            first = mods.build_overlay(self.game, out, [('A', a)])
            self.assertEqual(mods.COPIES['made'], 1)
            mods.remove_overlay(first, self.game)
            second = mods.build_overlay(self.game, out, [('A', a)])
            self.assertEqual(mods.COPIES['made'], 0)  # the copy is linked, not made again
            self.assertEqual((second / 'dvdroot_ps4' / 'chr' / 'a.dcx').read_bytes(), b'big texture')
            copies = out / mods.COPY_FOLDER
            self.assertEqual(len(list(copies.iterdir())), 1)
            mods.remove_overlay(second, self.game)
            # The mod turned off and another on: the old copy goes.
            b = self.mod('B', b'other', 'b.dcx')
            third = mods.build_overlay(self.game, out, [('B', b)])
            self.assertEqual([p.read_bytes() for p in copies.iterdir()], [b'other'])
            mods.remove_overlay(third, self.game)
        self.assertEqual((self.assets / 'a.dcx').read_bytes(), b'original')

    def test_merge_replacement_new_file_and_directory_listing(self):
        a = self.mod('A')
        self.mod('A', b'new', 'new.dcx')
        result = mods.build_overlay(self.game, self.root / 'out', [('A', a)])
        folder = result / 'dvdroot_ps4' / 'chr'
        self.assertEqual((folder / 'a.dcx').read_bytes(), b'mod')
        self.assertEqual((folder / 'b.dcx').read_bytes(), b'untouched')
        self.assertEqual((folder / 'new.dcx').read_bytes(), b'new')
        self.assertEqual({p.name for p in folder.iterdir()}, {'a.dcx', 'b.dcx', 'new.dcx'})
        self.assertEqual((self.assets / 'a.dcx').read_bytes(), b'original')
        self.assertFalse((self.assets / 'new.dcx').exists())
        self.assertEqual((result / 'eboot.bin').read_bytes(), b'original executable')

    def test_last_mod_wins_and_reordering_changes_winner(self):
        a, b = self.mod('A', b'A'), self.mod('B', b'B')
        for layers, winner in [([('A', a), ('B', b)], b'B'), ([('B', b), ('A', a)], b'A')]:
            result = mods.build_overlay(self.game, self.root / 'out', layers)
            self.assertEqual((result / 'dvdroot_ps4/chr/a.dcx').read_bytes(), winner)

    def test_selection_disable_and_new_mod(self):
        for name in ['C', 'B', 'A']:
            self.mod(name)
        config = self.root / 'mods.json'
        config.write_text(json.dumps({'order': ['B', 'A', 'deleted'], 'disabled': ['A']}))
        self.assertEqual(mods.selected(self.moddir, config), ['B', 'C'])

    def test_no_mod_returns_original_game(self):
        self.assertEqual(mods.build_overlay(self.game, self.root / 'out', []), self.game)
        self.assertFalse((self.root / 'out').exists())

    def test_directory_conflict_does_not_modify_base(self):
        a = self.mod('A')
        (a / 'dvdroot_ps4/chr/a.dcx').unlink()
        nested = a / 'dvdroot_ps4/chr/b.dcx'
        nested.mkdir()
        (nested / 'child').write_bytes(b'bad')
        with self.assertRaises(ValueError):
            mods.build_overlay(self.game, self.root / 'out', [('A', a)])
        self.assertEqual((self.assets / 'b.dcx').read_bytes(), b'untouched')
        self.assertEqual(list((self.root / 'out').iterdir()), [])

    def test_mod_symlinks_rejected(self):
        a = self.mod('A')
        (a / 'dvdroot_ps4/escape').symlink_to(self.assets, target_is_directory=True)
        with self.assertRaises(ValueError):
            mods.build_overlay(self.game, self.root / 'out', [('A', a)])

    @unittest.skipUnless(os.name == 'nt', 'Windows overlay links')
    def test_windows_overlay_files_report_their_size(self):
        # bb-probe stats with UCRT _stat64 and lists with FindFirstFileA: both report a file
        # symlink's own size (0), and the game panics loading it (FileTransferTask.cpp(865)).
        a = self.mod('A', b'replaced')
        result = mods.build_overlay(self.game, self.root / 'out', [('A', a)])
        folder = result / 'dvdroot_ps4' / 'chr'
        # DirEntry.stat(follow_symlinks=False) is FindFirstFile's data on Windows.
        sizes = {e.name: e.stat(follow_symlinks=False).st_size for e in os.scandir(folder)}
        self.assertEqual(sizes, {'a.dcx': len(b'replaced'), 'b.dcx': len(b'untouched')})
        self.assertFalse(any((folder / name).is_symlink() for name in sizes))

    def test_executable_replacement_rejected(self):
        a = self.mod('A')
        (a / 'eboot.bin').write_bytes(b'unsupported')
        with self.assertRaises(ValueError):
            mods.build_overlay(self.game, self.root / 'out', [('A', a)])

    def test_shadps4_sibling_folder_and_global_disable(self):
        legacy = Path(str(self.game) + '-mods')
        folder = legacy / 'dvdroot_ps4/chr'
        folder.mkdir(parents=True)
        (folder / 'a.dcx').write_bytes(b'legacy')
        for enabled, expected in [('1', b'legacy'), ('0', b'original')]:
            result = subprocess.run([sys.executable, str(ROOT / 'scripts/mods.py'), str(self.game),
                '--out', str(self.root / 'out'), '--mods-dir', str(self.moddir), '--enabled', enabled],
                capture_output=True, text=True, check=True)
            self.assertEqual((Path(result.stdout.strip()) / 'dvdroot_ps4/chr/a.dcx').read_bytes(), expected)

    def run_mods(self, profile=None):
        """The game view mods.py builds, as the launcher starts it (profile: mods.json)."""
        command = [sys.executable, str(ROOT / 'scripts/mods.py'), str(self.game),
                   '--out', str(self.root / 'out'), '--mods-dir', str(self.moddir)]
        if profile is not None:
            config = self.root / 'mods.json'
            config.write_text(json.dumps(profile))
            command += ['--config', str(config)]
        result = subprocess.run(command, capture_output=True, text=True, check=True)
        return Path(result.stdout.strip())

    def test_bare_dvdroot_ps4_is_one_mod_among_the_others(self):
        # A player's mods folder (Discord help): three mods in folders of their own and one
        # extracted bare as mods/dvdroot_ps4, which used to be the only one loaded.
        files = {'Backstep iframes': ['dvdroot_ps4/action/chr/c0000.hks'],
                 'default layout': ['dvdroot_ps4/menu/hud.drb.dcx', 'dvdroot_ps4/msg/engus/menu.msgbnd.dcx'],
                 'Tomb Prospector Concept Cape': ['dvdroot_ps4/parts/am_m_1000.partsbnd.dcx'],
                 'dvdroot_ps4': ['map/m21_00_00_00/m21_00_00_00.msb.dcx', 'msg/engus/menu.msgbnd.dcx']}
        game_path = lambda path: path if path.startswith('dvdroot_ps4/') else f'dvdroot_ps4/{path}'
        for name, paths in files.items():
            for path in paths:
                for target, contents in [(self.moddir / name / path, name), (self.game / game_path(path), 'original')]:
                    target.parent.mkdir(parents=True, exist_ok=True)
                    target.write_text(contents)
        self.assertEqual(mods.discover(self.moddir),
                         ['Backstep iframes', 'default layout', 'dvdroot_ps4', 'Tomb Prospector Concept Cape'])
        # map/ and msg/ go under the game's dvdroot_ps4 once, without a second one.
        self.assertEqual(sorted(p.as_posix() for p, _s in mods.mod_files(self.moddir / 'dvdroot_ps4')),
                         ['dvdroot_ps4/map/m21_00_00_00/m21_00_00_00.msb.dcx', 'dvdroot_ps4/msg/engus/menu.msgbnd.dcx'])
        shared = 'dvdroot_ps4/msg/engus/menu.msgbnd.dcx'
        for profile, winner in [(None, 'dvdroot_ps4'),  # alphabetical: dvdroot_ps4 loads last
                                ({'order': ['dvdroot_ps4', 'default layout']}, 'default layout'),
                                ({'disabled': ['dvdroot_ps4']}, 'default layout')]:
            with self.subTest(profile=profile):
                view = self.run_mods(profile)
                bare_on = 'dvdroot_ps4' not in (profile or {}).get('disabled', [])
                for name, paths in files.items():
                    for path in paths:
                        if game_path(path) != shared:
                            expected = name if bare_on or name != 'dvdroot_ps4' else 'original'
                            self.assertEqual((view / game_path(path)).read_text(), expected, path)
                self.assertEqual((view / shared).read_text(), winner)
                self.assertFalse((view / 'dvdroot_ps4/dvdroot_ps4').exists())

    def test_mods_folder_with_only_a_dvdroot_ps4_is_one_overlay(self):
        bare = self.moddir / 'dvdroot_ps4/chr/a.dcx'
        bare.parent.mkdir(parents=True)
        bare.write_bytes(b'bare')
        (self.moddir / 'Junk/stuff').mkdir(parents=True)  # not a mod: still the overlay alone
        self.assertEqual((self.run_mods() / 'dvdroot_ps4/chr/a.dcx').read_bytes(), b'bare')
        # Its switch in the launcher's list is respected too.
        view = self.run_mods({'disabled': ['dvdroot_ps4']})
        self.assertEqual((view / 'dvdroot_ps4/chr/a.dcx').read_bytes(), b'original')

    def test_app0_wrapped_mod_discovered(self):
        self.moddir.mkdir()
        (self.moddir / 'Wrapped/app0/dvdroot_ps4').mkdir(parents=True)
        self.assertEqual(mods.discover(self.moddir), ['Wrapped'])

    def test_other_case_replaces_the_game_file(self):
        root = self.moddir / 'Upper'
        (root / 'DVDROOT_PS4/Chr').mkdir(parents=True)
        (root / 'DVDROOT_PS4/Chr/A.DCX').write_bytes(b'upper')
        result = mods.build_overlay(self.game, self.root / 'out', [('Upper', root)])
        folder = result / 'dvdroot_ps4/chr'
        self.assertEqual((folder / 'a.dcx').read_bytes(), b'upper')
        self.assertEqual({p.name for p in folder.iterdir()}, {'a.dcx', 'b.dcx'})
        self.assertEqual({p.name for p in result.iterdir()}, {'dvdroot_ps4', 'eboot.bin'})

    def test_wrapped_and_bare_layouts(self):
        layouts = {'Title': 'CUSA03173/dvdroot_ps4/chr', 'Archive': 'Archive v1.2/dvdroot_ps4/chr',
                   'Bare': 'chr', 'Nested': 'Nested/app0/dvdroot_ps4/chr'}
        for name, path in layouts.items():
            folder = self.moddir / name / path
            folder.mkdir(parents=True)
            (folder / 'a.dcx').write_bytes(name.encode())
        (self.moddir / 'Archive/readme.txt').write_text('notes')
        (self.moddir / 'Junk/stuff').mkdir(parents=True)
        self.assertEqual(mods.discover(self.moddir), ['Archive', 'Bare', 'Nested', 'Title'])
        for name in layouts:
            with self.subTest(name=name):
                result = mods.build_overlay(self.game, self.root / 'out',
                                            [(name, self.moddir / name)])
                self.assertEqual((result / 'dvdroot_ps4/chr/a.dcx').read_bytes(), name.encode())

    def test_mod_site_layouts(self):
        # As mods come from mod sites: notes without a suffix and previews beside the wrapper,
        # two wrappers deep, and the game's folders next to screenshots.
        layouts = {'Readme': 'Readme v1/dvdroot_ps4/chr', 'Deep': 'Deep/Deep v2/dvdroot_ps4/chr',
                   'Shots': 'chr'}
        for name, path in layouts.items():
            folder = self.moddir / name / path
            folder.mkdir(parents=True)
            (folder / 'a.dcx').write_bytes(name.encode())
        (self.moddir / 'Readme/README').write_text('notes')
        (self.moddir / 'Readme/preview.webp').write_bytes(b'image')
        (self.moddir / 'Readme/Nexus page.url').write_text('[InternetShortcut]')
        (self.moddir / 'Shots/Screenshots').mkdir()
        (self.moddir / 'Shots/Screenshots/1.png').write_bytes(b'image')
        (self.moddir / 'Shots/readme.txt').write_text('notes')
        self.assertEqual(mods.discover(self.moddir), ['Deep', 'Readme', 'Shots'])
        for name in layouts:
            with self.subTest(name=name):
                result = mods.build_overlay(self.game, self.root / 'out',
                                            [(name, self.moddir / name)])
                self.assertEqual((result / 'dvdroot_ps4/chr/a.dcx').read_bytes(), name.encode())
                self.assertFalse((result / 'dvdroot_ps4/Screenshots').exists())
                self.assertFalse((result / 'dvdroot_ps4/readme.txt').exists())

    def test_unrecognized_folders_say_why(self):
        self.moddir.mkdir()
        (self.moddir / 'Zipped').mkdir()
        (self.moddir / 'Zipped/mod.zip').write_bytes(b'PK')
        (self.moddir / 'Two/A/dvdroot_ps4').mkdir(parents=True)
        (self.moddir / 'Two/B/dvdroot_ps4').mkdir(parents=True)
        (self.moddir / 'Two/A/dvdroot_ps4/x').write_bytes(b'x')
        (self.moddir / 'Empty').mkdir()
        found = dict(mods.unrecognized(self.moddir))
        self.assertEqual(sorted(found), ['Empty', 'Two', 'Zipped'])
        self.assertIn('archive', found['Zipped'])
        self.assertIn('several folders', found['Two'])
        self.assertEqual(found['Empty'], 'empty')

    def test_invalid_profile_fails(self):
        self.mod('A')
        config = self.root / 'mods.json'
        config.write_text('{"disabled":"A"}')
        with self.assertRaises(ValueError):
            mods.selected(self.moddir, config)

    def test_run_uses_overlay_propagates_exit_and_cleans_view(self):
        self.mod('A')
        python = self.root / 'python'
        python.write_text(f'#!{sys.executable}\nimport subprocess,sys\n'
            'if sys.argv[1] == "scripts/mods.py" or sys.argv[1] == "-c":\n'
            '    sys.exit(subprocess.call([sys.executable,*sys.argv[1:]]))\n')
        python.chmod(0o755)
        probe = self.root / 'probe'
        probe.write_text(f'#!{sys.executable}\nimport json,sys,os\nfrom pathlib import Path\n'
            'game=Path(sys.argv[sys.argv.index("--app0")+1])\n'
            'Path(os.environ["BB_DATA_DIR"],"mounted.json").write_text(json.dumps({\n'
            '"path":str(game),"content":(game/"dvdroot_ps4/chr/a.dcx").read_text()}))\n'
            'sys.exit(7)\n')
        probe.chmod(0o755)
        env = dict(os.environ, BB_PREBUILT='1', BB_PROBE=str(probe), PYTHON=str(python),
            BB_DATA_DIR=str(self.root), BB_GAME_DIR=str(self.game),
            BB_MODS_DIR=str(self.moddir), BB_MODS_ENABLED='1', BB_MODS_CONFIG=str(self.root/'mods.json'))
        result = subprocess.run(['bash', 'run.sh'], cwd=ROOT, env=env, capture_output=True, timeout=30)
        self.assertEqual(result.returncode, 7, result.stderr)
        mounted = json.loads((self.root / 'mounted.json').read_text())
        self.assertEqual(mounted['content'], 'mod')
        self.assertFalse(Path(mounted['path']).exists())
        self.assertEqual((self.assets/'a.dcx').read_bytes(), b'original')
