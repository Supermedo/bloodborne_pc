from paths import ROOT
import importlib.util
from pathlib import Path
import struct
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('bbport_respec', ROOT / 'launcher/bbport_respec.py')
respec = importlib.util.module_from_spec(spec)
spec.loader.exec_module(respec)

START = {'vitality': 11, 'endurance': 10, 'strength': 12, 'skill': 10, 'bloodtinge': 9, 'arcane': 8}


def block(attributes, name='Hunter', echoes=535):
    """An attribute block as the game writes it (bbport_respec's layout)."""
    data = bytearray(0x90)
    for a, o in zip(respec.ATTRIBUTES, respec.OFFSETS):
        struct.pack_into('<I', data, o, attributes[a])
    struct.pack_into('<II', data, respec.LEVEL, sum(attributes.values()) - 50, echoes)
    encoded = name.encode('utf-16-le')
    data[respec.NAME:respec.NAME + len(encoded)] = encoded
    return bytes(data)


def save_file(attributes, **kwargs):
    """Two copies of the block in a file of noise-free padding, like userdata0000."""
    data = bytearray(0x4000)
    one = block(attributes, **kwargs)
    data[0x1000:0x1000 + len(one)] = one
    data[0x3000:0x3000 + len(one)] = one
    return bytes(data)


class RespecTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.user = Path(self.temp.name)
        self.folder = self.user / 'savedata/1/CUSA00207/SPRJ0005'
        self.folder.mkdir(parents=True)
        (self.folder / 'userdata0000').write_bytes(save_file(START))
        (self.folder / 'backup0000').write_bytes(save_file(START))
        (self.folder / 'userdata0001').write_bytes(save_file(START, name='Other'))
        (self.folder / 'userdata0010').write_bytes(bytes(0x100))  # the system data: no character

    def tearDown(self):
        self.temp.cleanup()

    def test_finds_the_characters(self):
        self.assertEqual(respec.save_folders(self.user), [self.folder])
        found = respec.characters(self.folder)
        self.assertEqual([(slot, who['name'], who['level']) for slot, who in found],
                         [(0, 'Hunter', 10), (1, 'Other', 10)])
        self.assertEqual(found[0][1]['attributes'], START)
        self.assertEqual(respec.find_blocks(save_file(START)), [0x1000, 0x3000])

    def test_respec_keeps_the_level_and_rewrites_every_copy(self):
        new = {**START, 'vitality': 15, 'strength': 10, 'bloodtinge': 7}
        backup = respec.respec(self.folder, 0, new, now=0)
        for name in ('userdata0000', 'backup0000'):
            data = (self.folder / name).read_bytes()
            self.assertEqual([respec.read_block(data, at)['attributes'] for at in respec.find_blocks(data)],
                             [new, new])
            self.assertEqual(respec.read_block(data, 0x1000)['level'], 10)
            self.assertEqual(respec.read_block(data, 0x1000)['echoes'], 535)
        # The other character and the backup copy of the old saves are untouched.
        self.assertEqual(respec.characters(self.folder)[1][1]['attributes'], START)
        self.assertEqual(backup.parent.parent, self.user / 'respec-backups')
        self.assertEqual(respec.characters(backup)[0][1]['attributes'], START)
        self.assertEqual(respec.save_folders(self.user), [self.folder])

    def test_rejects_a_changed_sum_or_values_under_the_floors(self):
        with self.assertRaisesRegex(ValueError, 'add up to 60'):
            respec.respec(self.folder, 0, {**START, 'vitality': 20})
        with self.assertRaisesRegex(ValueError, 'Bloodtinge must be between 5'):
            respec.respec(self.folder, 0, {**START, 'bloodtinge': 4, 'vitality': 16})
        with self.assertRaisesRegex(ValueError, 'no character'):
            respec.respec(self.folder, 10, START)  # userdata0010: the system data
        self.assertFalse((self.user / 'respec-backups').exists())  # nothing written

    def test_the_saves_from_before_in_the_backups(self):
        """The launcher's way: a backup on the Saves page ("Before a respec"), no respec-backups folder."""
        import sys
        sys.path.insert(0, str(ROOT / 'launcher'))
        sys.path.insert(0, str(ROOT / 'scripts'))
        import bbport_saves as saves
        if saves.target_dir(self.user, '') != self.folder:
            self.folder.rename(saves.target_dir(self.user, ''))
            self.folder = saves.target_dir(self.user, '')
        made = saves.backup(self.user, '', reason='respec', detail='Hunter')
        new = {**START, 'vitality': 15, 'strength': 10, 'bloodtinge': 7}
        self.assertIsNone(respec.respec(self.folder, 0, new, copy=False))
        self.assertFalse((self.user / 'respec-backups').exists())
        found = saves.backups(self.user)
        self.assertEqual([(b.path, b.reason) for b in found], [(made, 'respec')])
        self.assertIn('Before a respec', made.name)
        import zipfile
        with zipfile.ZipFile(made) as zipped:
            before = next(zipped.read(n) for n in zipped.namelist() if n.endswith('/userdata0000'))
        self.assertEqual(respec.read_block(before, respec.find_blocks(before)[0])['attributes'], START)


if __name__ == '__main__':
    unittest.main()
