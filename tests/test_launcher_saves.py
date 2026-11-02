"""Importing saves of other PC versions (launcher/bbport_saves.py), on temporary folders."""
from paths import ROOT
import hashlib
import importlib.util
import os
from pathlib import Path
import sys
import tempfile
import time
import types
import unittest
from unittest import mock
import zipfile

sys.path.insert(0, str(ROOT / 'scripts'))
sys.path.insert(0, str(ROOT / 'launcher'))  # bbport_character
spec = importlib.util.spec_from_file_location('bbport_saves', ROOT / 'launcher/bbport_saves.py')
saves = importlib.util.module_from_spec(spec)
spec.loader.exec_module(saves)

SLOT, PROFILE = 1310720, 262144


def make_save(folder, name='Hunter', slots=(0,), extra=()):
    """A save folder like the game writes: ten slots (unused ones zero) and the profile."""
    folder.mkdir(parents=True, exist_ok=True)
    for i in range(10):
        data = bytearray(SLOT)
        if i in slots:
            data[:16] = bytes(range(1, 17))
            text = name.encode('utf-16-le') + b'\0\0'
            data[0x8000:0x8008] = bytes(8)
            data[0x8008:0x8008 + len(text)] = text
        (folder / f'userdata{i:04d}').write_bytes(bytes(data))
    (folder / 'userdata0010').write_bytes(b'\x1a' + bytes(PROFILE - 1))
    (folder / 'backup0000').write_bytes(bytes(SLOT))
    for name_ in extra:
        (folder / name_).write_bytes(b'meta')
    return folder


class SaveTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.user = self.root / 'port' / 'user'
        (self.root / 'port').mkdir()

    def tearDown(self):
        self.temp.cleanup()

    def test_what_a_save_holds(self):
        save = saves.SaveSet('test', make_save(self.root / 'SPRJ0005', 'Gehrman', slots=(0, 3)))
        self.assertTrue(save.complete)
        self.assertEqual(save.slots, ['userdata0000', 'userdata0003'])
        self.assertEqual(save.names, ['Gehrman', 'Gehrman'])

    def test_without_the_profile_a_save_is_not_offered(self):
        folder = make_save(self.root / 'SPRJ0005')
        (folder / 'userdata0010').unlink()
        self.assertFalse(saves.SaveSet('test', folder).complete)

    def test_found_where_the_other_versions_keep_them(self):
        appdata, local = self.root / 'Roaming', self.root / 'Local'
        make_save(appdata / 'shadPS4' / 'home' / '1000' / 'savedata' / 'CUSA00207' / 'SPRJ0005')
        make_save(appdata / 'shadPS4' / 'savedata' / '1' / 'CUSA03173' / 'SPRJ0005')
        make_save(local / 'bbhost' / 'data' / 'saves' / 'SPRJ0005')
        make_save(self.root / 'BB.Other' / 'user' / 'savedata' / '1' / 'CUSA00207' / 'SPRJ0005')
        (self.root / 'BB.Other' / 'run.py').write_text('')
        mine = make_save(saves.target_dir(self.user))
        with mock.patch.dict(os.environ, {'APPDATA': str(appdata), 'LOCALAPPDATA': str(local)}):
            found = saves.scan(self.root / 'port', exclude=[mine])
        self.assertEqual(sorted(s.source for s in found), ['bbhost', 'bbport (BB.Other)', 'shadPS4', 'shadPS4'])

    def test_seamless_coops_own_characters_stay_apart(self):
        """BBSC changes the items of the characters it plays: its own (user/seamless-coop) are never offered to
        the normal saves, and Seamless co-op is offered the normal characters, first, to copy in."""
        normal = make_save(saves.target_dir(self.user), 'Normal')
        coop = make_save(saves.target_dir(self.user / saves.COOP_SAVES), 'Coop')
        with mock.patch.dict(os.environ, {'APPDATA': '', 'LOCALAPPDATA': ''}):
            self.assertEqual(saves.scan(self.root / 'port', exclude=[normal]), [])
            found = saves.scan(self.root / 'port', exclude=[coop], extra=[('Your normal characters', normal)])
            self.assertEqual(saves.misplaced(self.root / 'empty'), ([], []))
        self.assertEqual([(s.source, s.folder) for s in found], [('Your normal characters', normal)])

    def test_bbsc_copies_are_only_offered_to_seamless_coop(self):
        """BBSC's copies before it changes a character (bbsc/save-backups/<moment>/save-dir/SPRJ0005), here and in
        an older installation, hold its items: the setup guide offered them for the normal saves (2026-11-02)."""
        mine = make_save(saves.target_dir(self.user), 'Normal')
        copy = make_save(self.root / 'port' / 'bbsc' / 'save-backups' / 'before-duel-bell-1' / 'save-dir' / 'SPRJ0005',
                         'Coop')
        old = make_save(self.root / 'BB.Old' / 'bbsc' / 'save-backups' / 'before-duel-bell-2' / 'save-dir' /
                        'SPRJ0005', 'Coop')
        (self.root / 'BB.Old' / 'run.py').write_text('')
        self.assertTrue(saves.from_seamless(copy) and saves.from_seamless(saves.target_dir(self.user / 'seamless-coop')))
        self.assertFalse(saves.from_seamless(mine))
        with mock.patch.dict(os.environ, {'APPDATA': '', 'LOCALAPPDATA': ''}):
            for_normal = saves.scan(self.root / 'port', exclude=[mine])
            for_coop = saves.scan(self.root / 'port', exclude=[mine], seamless=True)
        self.assertEqual(for_normal, [])
        self.assertEqual(sorted(s.folder for s in for_coop), sorted([copy, old]))

    def test_benchmark_characters_are_not_offered(self):
        """A port build's bundled benchmark character (benchmark/savedata/…) and recorded test routes are
        not a player's save."""
        make_save(self.root / 'port' / 'benchmark' / 'savedata' / '1' / 'CUSA00207' / 'SPRJ0005', 'Hunter')
        make_save(self.root / 'BB.Other' / 'benchmark' / 'savedata' / '1' / 'CUSA00207' / 'SPRJ0005', 'Hunter')
        make_save(self.root / 'port' / 'launcher_src' / 'recordings' / 'sewers' / 'save' / 'SPRJ0005', 'Hunter')
        make_save(self.user / 'savedata.benchmark' / '1' / 'CUSA00207' / 'SPRJ0005', 'Hunter')  # put aside at a start
        make_save(self.user / 'savedata.benchmark 2026-10-30 12.00.00' / '1' / 'CUSA00207' / 'SPRJ0005', 'Hunter')
        player = make_save(self.root / 'BB.Other' / 'user' / 'savedata' / '1' / 'CUSA00207' / 'SPRJ0005', 'Zed')
        # The player's own save, kept there while an old build's test ran and not put back: still offered.
        kept = make_save(self.root / 'BB.Old' / 'user' / 'savedata.benchmark-backup' / '1' / 'CUSA00207' / 'SPRJ0005',
                         'Ann')
        for folder in ('BB.Other', 'BB.Old'):
            (self.root / folder / 'run.py').write_text('')
        with mock.patch.dict(os.environ, {'APPDATA': str(self.root / 'Roaming'), 'LOCALAPPDATA': str(self.root / 'Local')}):
            found = saves.scan(self.root / 'port')
        self.assertEqual(sorted(s.folder for s in found), sorted([player, kept]))

    def test_import_backs_up_copies_and_restores(self):
        mine = make_save(saves.target_dir(self.user), 'Old')
        other = saves.SaveSet('bbhost', make_save(self.root / 'data' / 'saves' / 'SPRJ0005', 'New',
                                                  extra=('sce_param.bin', 'icon0.png')))
        archive = saves.import_set(other, self.user)
        self.assertTrue(archive.is_file())
        self.assertEqual(saves.SaveSet('', mine).names, ['New'])
        meta = mine.with_name('SPRJ0005.sce_sys')
        self.assertEqual((meta / 'param.bin').read_bytes(), b'meta')  # bbhost's sce_param.bin
        self.assertFalse((mine / 'sce_param.bin').exists())  # only the game's own files go into the save
        self.assertFalse((self.user / 'save_import').exists())
        self.assertEqual(sorted(p.name for p in mine.parent.iterdir()), ['SPRJ0005', 'SPRJ0005.sce_sys'])
        saves.restore(archive, self.user)
        self.assertEqual(saves.SaveSet('', mine).names, ['Old'])

    def test_achievements_come_along_from_another_installation(self):
        """Testers brought their characters into a beta's own folder; the achievements stayed behind."""
        old_user = self.root / 'BB.Other' / 'user'
        other = saves.SaveSet('bbport (BB.Other)', make_save(old_user / 'savedata' / '1' / 'CUSA00207' / 'SPRJ0005'))
        (old_user / 'trophies.log').write_text('14 100\n15 200\n', encoding='utf-8')
        saves.import_set(other, self.user)
        self.assertEqual(saves.bring_trophies(other, self.user), 2)
        self.assertEqual((self.user / 'trophies.log').read_text(encoding='utf-8'), '14 100\n15 200\n')
        self.assertEqual(saves.bring_trophies(other, self.user), 0)  # imported again: nothing twice
        self.assertEqual((old_user / 'trophies.log').read_text(encoding='utf-8'), '14 100\n15 200\n')  # source as it was

    def test_no_achievements_from_elsewhere_or_from_this_folder(self):
        bbhost = saves.SaveSet('bbhost', make_save(self.root / 'data' / 'saves' / 'SPRJ0005'))
        (self.root / 'data' / 'trophies.log').write_text('14 100\n', encoding='utf-8')
        self.assertEqual(saves.bring_trophies(bbhost, self.user), 0)
        mine = saves.SaveSet('', make_save(saves.target_dir(self.user)))
        (self.user / 'trophies.log').write_text('14 100\n', encoding='utf-8')
        self.assertEqual(saves.bring_trophies(mine, self.user), 0)
        self.assertEqual((self.user / 'trophies.log').read_text(encoding='utf-8'), '14 100\n')

    def test_achievements_of_other_installations_are_found(self):
        """R86: as the saves of other versions are found: another port folder beside this one, shadPS4 in
        %APPDATA% and portable beside the port, the Windows before a new installation (Windows.old)."""
        xml = '<title-name>Bloodborne</title-name><trophy id="014" unlockstate="true" timestamp="1790000000"/>'
        (self.user).mkdir(parents=True)
        (self.user / 'trophies.log').write_text('14 100\n', encoding='utf-8')  # this port's own
        (self.root / 'BB.Old' / 'user').mkdir(parents=True)
        (self.root / 'BB.Old' / 'run.py').write_text('')
        (self.root / 'BB.Old' / 'user' / 'trophies.log').write_text('15 200\n', encoding='utf-8')
        lists = Path('TrophyFiles') / 'trophy00' / 'Xml'
        portable = self.root / 'shadPS4-portable' / 'user' / 'game_data' / 'CUSA03173' / lists
        appdata = self.root / 'Roaming' / 'shadPS4' / 'home' / '1000' / 'trophy' / 'NPWR07150_00' / 'Xml'
        roaming = self.root / 'Windows.old' / 'Users' / 'nick' / 'AppData' / 'Roaming'
        old = roaming / 'shadPS4' / 'game_data' / 'CUSA00207' / lists
        for folder in (portable, appdata, old):
            folder.mkdir(parents=True)
            (folder / 'TROP.XML').write_text(xml, encoding='utf-8')
        (portable / 'TROP_00.XML').write_text(xml, encoding='utf-8')  # one installation: listed once
        # This port's own Seamless co-op trophies and the game test's copy of the saves in %TEMP%: no other
        # installation's.
        for own in (self.user / 'seamless-coop', self.root / 'Temp' / 'bbport-test' / 'user'):
            own.mkdir(parents=True)
            (own / 'trophies.log').write_text('16 300\n', encoding='utf-8')
        folders = {'APPDATA': str(self.root / 'Roaming'), 'LOCALAPPDATA': str(self.root / 'Local'),
                   'SystemDrive': str(self.root)}
        with mock.patch.dict(os.environ, folders):
            found = saves.trophy_records(self.root / 'port', exclude=[self.user / 'trophies.log'])
        self.assertEqual(sorted(where for where, _path in found),
                         ['bbport (BB.Old)', 'shadPS4', 'shadPS4', 'shadPS4 (shadPS4-portable)'])
        self.assertIn(portable, [path for _where, path in found])  # its Xml folder
        self.assertNotIn(self.user / 'trophies.log', [path for _where, path in found])
        picked = saves.trophy_records_in(self.root / 'BB.Old')  # a folder the player picks
        self.assertEqual(picked, [('bbport (BB.Old)', self.root / 'BB.Old' / 'user' / 'trophies.log')])

    def test_achievements_on_the_drive_of_an_old_pc(self):
        """The drive of the old PC, picked by the player (a new SSD with a new Windows: the old drive is D:).
        shadPS4's lists are ten folders down in AppData, a portable one in Downloads is as deep: the known places
        and a data folder met on the way are looked into, Windows' and the programs' folders are not walked."""
        xml = '<title-name>Bloodborne</title-name><trophy id="014" unlockstate="true" timestamp="1790000000"/>'
        drive = self.root / 'D'
        lists = Path('TrophyFiles') / 'trophy00' / 'Xml'
        roaming = drive / 'Users' / 'nick' / 'AppData' / 'Roaming'
        places = [roaming / 'shadPS4' / 'game_data' / 'CUSA03173' / lists,
                  roaming / 'shadPS4-qt' / 'home' / '1000' / 'trophy' / 'NPWR07150_00' / 'Xml',
                  drive / 'Windows.old' / 'Users' / 'nick' / 'AppData' / 'Roaming' / 'shadPS4' / 'game_data' /
                  'CUSA00207' / lists,
                  drive / 'Users' / 'nick' / 'Downloads' / 'Emulators' / 'shadPS4-win64' / 'user' / 'game_data' /
                  'CUSA03173' / lists]
        for folder in places:
            folder.mkdir(parents=True)
            (folder / 'TROP.XML').write_text(xml, encoding='utf-8')
        port = drive / 'Users' / 'nick' / 'Desktop' / 'Games' / 'BB.Port' / 'user'
        port.mkdir(parents=True)
        (port / 'trophies.log').write_text('15 200\n', encoding='utf-8')
        for skipped in (drive / 'ProgramData' / 'x' / 'user', drive / '$Recycle.Bin' / 'S-1-5' / 'user'):
            skipped.mkdir(parents=True)
            (skipped / 'trophies.log').write_text('15 200\n', encoding='utf-8')
        found = saves.trophy_records_in(drive)
        self.assertEqual(sorted(path for _where, path in found), sorted(places + [port / 'trophies.log']))
        self.assertEqual(sorted(where for where, _path in found),
                         ['Backup (BB.Port)', 'shadPS4', 'shadPS4', 'shadPS4 (shadPS4-qt)', 'shadPS4 (shadPS4-win64)'])
        # The player picked the user's folder, or shadPS4's own.
        self.assertEqual(len(saves.trophy_records_in(drive / 'Users' / 'nick')), 4)
        self.assertEqual([path for _where, path in saves.trophy_records_in(roaming / 'shadPS4')], places[:1])

    def test_a_profile_folder_with_brackets(self):
        """A Windows user name like Nick[DE]: glob takes [DE] for a set of letters, and shadPS4 there was not found."""
        roaming = self.root / 'Users' / 'Nick[DE]' / 'AppData' / 'Roaming'
        lists = roaming / 'shadPS4' / 'game_data' / 'CUSA03173' / 'TrophyFiles' / 'trophy00' / 'Xml'
        lists.mkdir(parents=True)
        (lists / 'TROP.XML').write_text('<title-name>Bloodborne</title-name><trophy id="014" unlockstate="true"/>',
                                        encoding='utf-8')
        with mock.patch.dict(os.environ, {'APPDATA': str(roaming), 'LOCALAPPDATA': '', 'SystemDrive': str(self.root)}):
            found = saves.trophy_records(self.root / 'port')
        self.assertEqual(found, [('shadPS4', lists)])
        self.assertEqual(saves.trophy_records_in(self.root / 'Users' / 'Nick[DE]'), [('shadPS4', lists)])

    def test_backups_never_overwrite_each_other(self):
        make_save(saves.target_dir(self.user))
        first, second = saves.backup(self.user), saves.backup(self.user)
        self.assertNotEqual(first, second)
        self.assertTrue(zipfile.ZipFile(first).namelist())

    def test_encrypted_console_backups_are_recognised(self):
        (self.root / 'usb' / 'PS4' / 'SAVEDATA' / '1' / 'CUSA00207' / 'sdimg_SPRJ0005').mkdir(parents=True)
        found, encrypted = saves.find_in(self.root / 'usb')
        self.assertEqual((found, len(encrypted)), ([], 1))


class BackupTests(unittest.TestCase):
    """The save manager's backups: named for what they are, listed, renamed, made before a start,
    put back, and taken from a file."""

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.user = self.root / 'user'
        self.save = make_save(saves.target_dir(self.user), 'Nick', slots=(0, 3))

    def tearDown(self):
        self.temp.cleanup()

    def test_a_backup_says_what_it_is(self):
        made = saves.backup(self.user, reason='import', detail='shadPS4')
        self.assertRegex(made.name, r'^\d{4}-\d{2}-\d{2} \d{2}\.\d{2} - Before an import - shadPS4 - Nick, Nick\.zip$')
        item = saves.backups(self.user)[0]
        self.assertEqual((item.reason, item.detail, item.names, item.ok), ('import', 'shadPS4', ['Nick', 'Nick'], True))

    def test_backups_from_before_still_show(self):
        old = saves.backup_dir(self.user) / 'saves_2026-10-12_101500.zip'
        old.parent.mkdir(parents=True)
        with zipfile.ZipFile(old, 'w') as zipped:
            for path in self.save.iterdir():
                zipped.write(path, f'savedata/1/CUSA00207/SPRJ0005/{path.name}')
        item = saves.Backup(old)
        self.assertEqual((item.reason, item.created.isoformat(), item.names), ('old', '2026-10-12T10:15:00', ['Nick', 'Nick']))

    def test_rename(self):
        made = saves.backup(self.user)
        files = saves.archive_files(made)
        renamed = saves.rename_backup(made, 'Before Ludwig')
        self.assertFalse(made.exists())
        self.assertIn('Before Ludwig', renamed.name)
        self.assertEqual(saves.Backup(renamed).label, 'Before Ludwig')
        self.assertEqual(saves.archive_files(renamed), files)  # the same files, only the name is new

    def test_automatic_only_after_playing_and_only_the_last_ones(self):
        first = saves.auto_backup(self.user)
        self.assertIsNotNone(first)
        self.assertIsNone(saves.auto_backup(self.user))  # nothing played since
        for number in range(4):
            stamp = time.time() + 120 * (number + 1)
            os.utime(self.save / 'userdata0000', (stamp, stamp))
            with mock.patch.object(saves.datetime, 'datetime', wraps=saves.datetime.datetime) as clock:
                clock.now.return_value = saves.datetime.datetime.fromtimestamp(stamp + 1)
                saves.auto_backup(self.user, keep=3)
        mine = saves.backup(self.user, reason='manual')
        kinds = [b.reason for b in saves.backups(self.user)]
        self.assertEqual(kinds.count('auto'), 3)
        self.assertTrue(mine.exists())

    def test_a_named_automatic_backup_is_kept(self):
        """Renamed by the player ("before Orphan"), an automatic backup is theirs: it was deleted ten starts later
        like any other (2026-11-02)."""
        first = saves.auto_backup(self.user)
        named = saves.rename_backup(first, 'Before Orphan')
        for number in range(5):
            stamp = time.time() + 120 * (number + 1)
            os.utime(self.save / 'userdata0000', (stamp, stamp))
            with mock.patch.object(saves.datetime, 'datetime', wraps=saves.datetime.datetime) as clock:
                clock.now.return_value = saves.datetime.datetime.fromtimestamp(stamp + 1)
                saves.auto_backup(self.user, keep=2)
        self.assertTrue(named.exists())
        unnamed = [b for b in saves.backups(self.user) if b.reason == 'auto' and not b.label]
        self.assertEqual(len(unnamed), 2)

    def test_a_backup_added_from_a_file_does_not_stop_the_next_automatic_one(self):
        """A friend's zip added after playing is dated now but holds another save: the session played before it
        still gets its automatic backup at the next start (2026-11-02)."""
        saves.auto_backup(self.user)
        stamp = time.time() + 120
        os.utime(self.save / 'userdata0000', (stamp, stamp))  # played
        friend = self.root / 'friend.zip'
        with zipfile.ZipFile(friend, 'w') as zipped:
            for path in self.save.iterdir():
                zipped.write(path, f'savedata/1/CUSA00207/SPRJ0005/{path.name}')
        with mock.patch.object(saves.datetime, 'datetime', wraps=saves.datetime.datetime) as clock:
            clock.now.return_value = saves.datetime.datetime.fromtimestamp(stamp + 60)
            saves.add_backup_file(friend, self.user)
            made = saves.auto_backup(self.user)
        self.assertIsNotNone(made)

    def test_restore_takes_only_the_games_files(self):
        archive = self.root / 'other.zip'
        with zipfile.ZipFile(archive, 'w') as zipped:
            zipped.writestr('../../evil.txt', b'no')
            zipped.writestr('notes/readme.txt', b'no')
            zipped.writestr('userdata0010', b'\x1a' + bytes(10))  # a zip of the save's files themselves
            zipped.writestr('userdata0000', b'\x01' + bytes(4095))
            zipped.writestr('SPRJ0005.sce_sys/param.bin', b'meta')
        before = saves.restore(archive, self.user)
        self.assertTrue(before.is_file())
        self.assertEqual(sorted(p.name for p in self.save.iterdir()), ['userdata0000', 'userdata0010'])
        self.assertEqual((self.save.with_name('SPRJ0005.sce_sys') / 'param.bin').read_bytes(), b'meta')
        self.assertFalse((self.root / 'evil.txt').exists())
        self.assertFalse(any(self.user.rglob('readme.txt')))

    def test_a_file_without_a_whole_save_is_refused(self):
        archive = self.root / 'half.zip'
        with zipfile.ZipFile(archive, 'w') as zipped:
            zipped.writestr('userdata0000', b'\x01')
        with self.assertRaises(OSError):
            saves.restore(archive, self.user)
        with self.assertRaises(OSError):
            saves.add_backup_file(archive, self.user)
        self.assertEqual(saves.SaveSet('', self.save).names, ['Nick', 'Nick'])  # untouched

    def test_a_backup_from_a_file_joins_the_list(self):
        made = saves.backup(self.user)
        copy = self.root / 'from a friend.zip'
        copy.write_bytes(made.read_bytes())
        added = saves.add_backup_file(copy, self.user)
        item = saves.Backup(added)
        self.assertEqual((item.reason, item.detail, item.names), ('file', 'from a friend', ['Nick', 'Nick']))


def contents(folder):
    """{file name: SHA-256} of a folder's files (a failed comparison of the files' bytes takes minutes to show)."""
    return {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(Path(folder).iterdir()) if p.is_file()}


class SwapTests(unittest.TestCase):
    """An import or a restore deleted the save's files and then moved the new ones in: a file that would not move
    left half of one save and half of another. The files in use go aside first and come back on any error."""

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.user = self.root / 'user'
        self.mine = make_save(saves.target_dir(self.user), 'Old', slots=(0, 2))
        (self.mine / 'backup0001').write_bytes(b'only here')  # a file the new save has not
        self.meta = self.mine.with_name('SPRJ0005.sce_sys')
        self.meta.mkdir()
        (self.meta / 'param.bin').write_bytes(b'old meta')
        self.before = contents(self.mine), contents(self.meta)
        self.other = saves.SaveSet('bbhost', make_save(self.root / 'data' / 'saves' / 'SPRJ0005', 'New',
                                                       slots=(0, 5), extra=('backup0002',)))

    def tearDown(self):
        self.temp.cleanup()

    def as_before(self):
        self.assertEqual((contents(self.mine), contents(self.meta)), self.before)
        self.assertFalse((self.user / saves.REPLACED_DIR).exists())
        self.assertFalse((self.user / 'save_import').exists())

    @staticmethod
    def failing(name):
        """os.replace that fails for the new file `name` moved in from the staging folder (a full drive)."""
        real = os.replace

        def replace(source, target):
            if Path(target).name == name and 'save_import' in Path(source).parts:
                raise OSError(28, 'No space left on device')
            return real(source, target)
        return replace

    def test_the_new_files_replace_the_old_ones_and_nothing_stays_aside(self):
        saves.import_set(self.other, self.user)
        self.assertEqual(saves.SaveSet('', self.mine).names, ['New', 'New'])
        self.assertNotIn('backup0001', contents(self.mine))  # the old save's own extra file went with it
        self.assertIn('backup0002', contents(self.mine))
        self.assertFalse((self.user / saves.REPLACED_DIR).exists())

    def test_an_import_that_fails_halfway_leaves_the_save_as_it_was(self):
        # The profile is the last file moved in: every other new one is in place by then and goes out again.
        with mock.patch.object(saves.os, 'replace', side_effect=self.failing('userdata0010')):
            with self.assertRaises(OSError):
                saves.import_set(self.other, self.user)
        self.as_before()

    def test_a_restore_whose_metadata_cannot_follow_puts_the_save_back_too(self):
        archive = saves.backup(self.user)
        (self.mine / 'userdata0000').write_bytes(b'\x01' + bytes(4095))  # played since
        self.before = contents(self.mine), contents(self.meta)
        with mock.patch.object(saves.os, 'replace', side_effect=self.failing('param.bin')):
            with self.assertRaises(OSError):
                saves.restore(archive, self.user)
        self.as_before()

    @unittest.skipUnless(os.name == 'nt', 'only Windows keeps a file that is open from being moved')
    def test_a_file_held_open_leaves_the_save_as_it_was(self):
        """A sync tool or an antivirus holding one of the save's files while the others were already gone."""
        with open(self.mine / 'userdata0005', 'rb'), mock.patch.object(saves.time, 'sleep') as waited:
            with self.assertRaises(PermissionError):
                saves.import_set(self.other, self.user)
        self.assertEqual(waited.call_count, saves.LOCK_TRIES - 1)  # tried again first
        self.as_before()

    def test_a_file_that_cannot_go_back_is_named_with_its_folder(self):
        moving_in = self.failing('userdata0010')

        def replace(source, target):
            if Path(target) == self.mine / 'userdata0002' and saves.REPLACED_DIR in Path(source).parts:
                raise OSError(13, 'Access is denied')
            return moving_in(source, target)
        with mock.patch.object(saves.os, 'replace', side_effect=replace):
            with self.assertRaises(OSError) as raised:
                saves.import_set(self.other, self.user)
        aside = next((self.user / saves.REPLACED_DIR).iterdir())
        self.assertIn('userdata0002', str(raised.exception))
        self.assertIn(str(aside), str(raised.exception))
        self.assertEqual(contents(aside / 'save'), {'userdata0002': self.before[0]['userdata0002']})  # kept
        self.assertEqual({**contents(self.mine), 'userdata0002': self.before[0]['userdata0002']}, self.before[0])

    def test_a_new_file_that_cannot_come_out_is_named_and_no_empty_folder_stays(self):
        """Only a file the old save has not (backup0002) would not go back to staging: every file from before is
        in place again, so the error names the new one left in the save folder, not an empty aside folder."""
        moving_in = self.failing('userdata0010')

        def replace(source, target):
            if Path(source) == self.mine / 'backup0002':
                raise OSError(13, 'Access is denied')
            return moving_in(source, target)
        with mock.patch.object(saves.os, 'replace', side_effect=replace), mock.patch.object(saves.time, 'sleep'):
            with self.assertRaises(OSError) as raised:
                saves.import_set(self.other, self.user)
        self.assertIn(f'backup0002 of the new save stayed in {self.mine}', str(raised.exception))
        self.assertNotIn(saves.REPLACED_DIR, str(raised.exception))
        self.assertFalse((self.user / saves.REPLACED_DIR).exists())
        left = contents(self.mine)
        self.assertEqual(left.pop('backup0002'), contents(self.other.folder)['backup0002'])
        self.assertEqual(left, self.before[0])

    def test_put_aside_saves_are_not_taken_for_misplaced_ones(self):
        for name in sorted(self.mine.iterdir()):
            name.unlink()  # this port's own save folder empty: misplaced() looks around
        make_save(self.user / saves.REPLACED_DIR / '2026-10-30 12.00.00' / 'save', 'Old')
        make_save(self.user / 'savedata.benchmark' / '1' / 'CUSA00207' / 'SPRJ0005', 'Hunter')
        make_save(self.user / 'savedata.benchmark 2026-10-30 12.00.00' / '1' / 'CUSA00207' / 'SPRJ0005', 'Hunter')
        self.assertEqual(saves.misplaced(self.user), ([], []))


class ImportWindowTests(unittest.TestCase):
    """Saves > From other versions > Import, as the window does it (bbport_setup.SetupMixin.import_save)."""

    def test_the_achievements_come_along_without_trophy_notices(self):
        import bbport_launcher_win as L
        import bbport_setup
        import bbport_trophies as trophies
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        root = Path(temp.name)
        (root / 'BB.Other' / 'run.py').parent.mkdir()
        (root / 'BB.Other' / 'run.py').write_text('')
        other_user, user = root / 'BB.Other' / 'user', root / 'beta' / 'user'
        save = saves.SaveSet('bbport (BB.Other)', make_save(other_user / 'savedata' / '1' / 'CUSA00207' / 'SPRJ0005'))
        (other_user / 'trophies.log').write_text('14 100\n15 200\n', encoding='utf-8')
        said, changed = [], []

        class Window(bbport_setup.SetupMixin):
            process, save_found = None, None
            tracker = trophies.Tracker(user)
            messagebox = types.SimpleNamespace(askyesno=lambda title, text: True,
                                               showinfo=lambda title, text: said.append(text),
                                               showerror=lambda title, text: said.append(text))

            def var(self, key, store):
                return types.SimpleNamespace(get=lambda: {'user_dir': str(user), 'game_dir': '', 'online': False, 'online_pool': 'supermedo'}[key])

            def trophies_changed(self, new, announce=True):
                changed.append((new, announce))

            def refresh_status(self):
                pass

            def refresh_saves(self, only_if_changed=False):
                pass
        with mock.patch.object(L, 'LANG', 'en'):
            Window().import_save(save, lambda: None)
        self.assertIn('Your achievements from there came along: 2 new here.', said[-1])
        self.assertEqual(changed, [([14, 15], False)])  # on the page, without "You've earned a trophy"
        self.assertEqual(Window.tracker.poll(), [])  # and not announced later by the 2-second poll


class TrophyImportWindowTests(unittest.TestCase):
    """Achievements > Bring achievements from another installation, after the search, as the window does it
    (bbport_launcher_win.Launcher.offer_trophies)."""

    def setUp(self):
        import bbport_launcher_win as L
        import bbport_trophies as trophies
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        self.user = self.root / 'port' / 'user'
        self.user.mkdir(parents=True)
        self.target = self.user / 'trophies.log'
        self.target.write_text('14 100\n', encoding='utf-8')
        self.asked, self.said, self.changed, self.picked = [], [], [], []
        note = types.SimpleNamespace(hint='', configure=lambda **_options: None)
        self.window = types.SimpleNamespace(
            trophy_searching=True, trophy_import_note=note, tracker=trophies.Tracker(self.user),
            messagebox=types.SimpleNamespace(askyesno=lambda title, text: self.asked.append(text) or self.answer,
                                             showinfo=lambda title, text: self.said.append(text),
                                             showerror=lambda title, text: self.said.append(text)),
            filedialog=types.SimpleNamespace(askdirectory=lambda **_options: str(self.root)),
            import_trophies=lambda folder=None: self.picked.append(folder),
            trophies_changed=lambda new, announce=True: self.changed.append((new, announce)),
            var=lambda key, store: types.SimpleNamespace(get=lambda: {'user_dir': self.user_dir}[key]),
            saves_folder_gone=lambda: L.Launcher.saves_folder_gone(self.window))
        self.user_dir = str(self.user)  # a saves folder of the player's own
        self.window.tracker.poll()  # the page shows what is there
        self.answer = True

    def offer(self, records, folder=None):
        import bbport_launcher_win as L
        import bbport_trophies as trophies
        found = [(where, path, trophies.missing(trophies.read_unlocks(path), self.target)) for where, path in records]
        with mock.patch.object(L, 'LANG', 'en'):
            L.Launcher.offer_trophies(self.window, found, self.target, folder)

    def test_new_trophies_are_added_after_a_copy_without_notices(self):
        import bbport_trophies as trophies
        old = self.root / 'BB.Old' / 'user' / 'trophies.log'
        old.parent.mkdir(parents=True)
        old.write_text('14 50\n15 200\n', encoding='utf-8')
        self.offer([('bbport (BB.Old)', old)])
        self.assertIn('bbport (BB.Old): 1 new here', self.asked[0])
        self.assertIn('Achievements added: 1 new.', self.said[-1])
        self.assertEqual(trophies.read_log(self.target), {14: 50, 15: 200})  # the earlier time as well
        self.assertEqual([p.read_text(encoding='utf-8') for p in self.user.glob('trophies.log.*.bak')], ['14 100\n'])
        self.assertEqual(self.changed, [([15], False)])  # on the page, without "You've earned a trophy"
        self.assertEqual(self.window.tracker.poll(), [])  # and not announced later by the 2-second poll
        self.assertEqual(old.read_text(encoding='utf-8'), '14 50\n15 200\n')  # the other installation as it was

    def test_nothing_new_offers_a_folder_to_look_in(self):
        old = self.root / 'BB.Old' / 'user' / 'trophies.log'
        old.parent.mkdir(parents=True)
        old.write_text('14 50\n', encoding='utf-8')  # only an earlier time: nothing new to offer
        self.offer([('bbport (BB.Old)', old)])
        self.assertIn('No other installation on this PC has achievements that are not here yet.', self.asked[0])
        self.assertEqual(self.picked, [self.root])  # searched again there
        self.answer = False
        self.offer([], folder=self.root)
        self.assertIn(f'No achievements in {self.root} that are not here yet.', self.asked[-1])
        self.assertEqual((self.picked, self.said), ([self.root], []))
        self.assertEqual(self.target.read_text(encoding='utf-8'), '14 100\n')

    def test_a_saves_folder_that_is_gone_is_not_made_anew(self):
        """Its drive not connected, or renamed: an empty one made in its place would show no characters at the next
        start (run.py user_folders), so the achievements are not added to one."""
        old = self.root / 'BB.Old' / 'user' / 'trophies.log'
        old.parent.mkdir(parents=True)
        old.write_text('15 200\n', encoding='utf-8')
        gone = self.root / 'E' / 'Bloodborne saves'
        self.user_dir, self.target = str(gone), gone / 'trophies.log'
        self.offer([('bbport (BB.Old)', old)])
        self.assertIn(f'The saves folder {gone} is not there', self.said[-1])
        self.assertFalse(gone.exists())
        self.window.saves_folder_gone = lambda: False  # gone between the check and the writing
        self.offer([('bbport (BB.Old)', old)])
        self.assertIn('Could not add the achievements', self.said[-1])
        self.assertFalse(gone.exists())

    def test_this_ports_own_folder_before_a_first_start(self):
        """No saves folder of the player's own: this port's user/ is made, as run.py makes it at the start."""
        import bbport_trophies as trophies
        old = self.root / 'BB.Old' / 'user' / 'trophies.log'
        old.parent.mkdir(parents=True)
        old.write_text('15 200\n', encoding='utf-8')
        self.user_dir, self.target = '', self.root / 'new port' / 'user' / 'trophies.log'
        self.offer([('bbport (BB.Old)', old)])
        self.assertIn('Achievements added: 1 new.', self.said[-1])
        self.assertEqual(trophies.read_log(self.target), {15: 200})


if __name__ == '__main__':
    unittest.main()
