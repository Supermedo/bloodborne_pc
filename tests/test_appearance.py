"""Change appearance (launcher/bbport_appearance.py): one character takes another's appearance block,
gender and voice, in its slot file, the game's backup of it and the profile's Load Game picture;
nothing else of either character changes. On made-up saves laid out as the game writes them."""
from paths import ROOT
import struct
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(ROOT / 'launcher'))
import bbport_appearance as appearance  # noqa: E402
import bbport_character as character  # noqa: E402

SLOT, PROFILE = 1310720, 262144
FACE = 0x12824  # where the game puts it (a 1.09 save from this PC)


def look(seed):
    """An appearance block: the header, then slider bytes that tell two looks apart."""
    block = bytearray(appearance.SIZE)
    block[:12] = appearance.MAGIC + struct.pack('<II', appearance.VERSION, appearance.SIZE)
    for i in range(12, appearance.SIZE - 1):
        block[i] = (seed * 31 + i) & 0xFF
    return bytes(block)


def make_slot(name, seed, gender=1, voice=0, origin=1, attributes=(11, 14, 11, 11, 10, 7)):
    data = bytearray(SLOT)
    at = FACE - character.FACE_TO_NAME
    text = name.encode('utf-16-le')
    data[at:at + len(text)] = text
    for (_key, offset), value in zip(character.ATTRIBUTES, attributes):
        struct.pack_into('<I', data, at + offset, value)
    struct.pack_into('<I', data, at + character.LEVEL, sum(attributes) - 50)
    struct.pack_into('<I', data, at + character.ECHOES, 535)
    data[at + appearance.GENDER], data[at + appearance.VOICE], data[at + 0x26] = gender, voice, origin
    data[FACE:FACE + appearance.SIZE] = look(seed)
    return bytes(data)


def make_profile(slots):
    """The profile with a Load Game block per character: {slot: (name, seed)}."""
    data = bytearray(PROFILE)
    for slot, (name, seed) in slots.items():
        start = appearance.PROFILE_SLOTS + slot * appearance.PROFILE_SLOT_SIZE
        text = name.encode('utf-16-le')
        data[start:start + len(text)] = text
        face = start + appearance.PROFILE_FACE
        data[face:face + appearance.SIZE] = look(seed)
    return bytes(data)


class AppearanceTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.user = Path(self.temp.name)
        self.folder = self.user / 'savedata/1/CUSA00207/SPRJ0005'
        self.folder.mkdir(parents=True)
        self.write('userdata0000', make_slot('Hunter', 1, gender=1, voice=0))
        self.write('backup0000', make_slot('Hunter', 1, gender=1, voice=0))
        self.write('userdata0001', make_slot('Doll', 2, gender=0, voice=2, origin=6, attributes=(7, 8, 9, 13, 14, 9)))
        self.write('userdata0002', bytes(SLOT))  # an empty slot
        self.write('userdata0010', make_profile({0: ('Hunter', 1), 1: ('Doll', 2)}))
        self.write('backup0010', make_profile({0: ('Hunter', 1), 1: ('Doll', 2)}))

    def tearDown(self):
        self.temp.cleanup()

    def write(self, name, data):
        (self.folder / name).write_bytes(data)

    def read(self, name):
        return (self.folder / name).read_bytes()

    def test_reads_the_real_layout(self):
        self.assertEqual([(slot, who.name) for slot, who in appearance.characters(self.folder)],
                         [(0, 'Hunter'), (1, 'Doll')])
        self.assertEqual(appearance.read(self.read('userdata0001')), (look(2), 0, 2))
        self.assertIsNone(appearance.read(bytes(SLOT)))
        profile = self.read('userdata0010')
        self.assertEqual(appearance.profile_face(profile, 1, 'Doll'),
                         appearance.PROFILE_SLOTS + appearance.PROFILE_SLOT_SIZE + appearance.PROFILE_FACE)
        self.assertIsNone(appearance.profile_face(profile, 1, 'Hunter'))  # another character's block

    def test_copies_the_look_and_nothing_else(self):
        before = {name: self.read(name) for name in ('userdata0000', 'userdata0001')}
        kept = appearance.copy(self.folder, 1, 0, now=0)
        for name in ('userdata0000', 'backup0000'):
            data = self.read(name)
            self.assertEqual(appearance.read(data), (look(2), 0, 2))
            # Only the appearance block, gender and voice differ from before.
            at = FACE - character.FACE_TO_NAME
            changed = {i for i, (a, b) in enumerate(zip(before['userdata0000'], data)) if a != b}
            allowed = set(range(FACE, FACE + appearance.SIZE)) | {at + appearance.GENDER, at + appearance.VOICE}
            self.assertLessEqual(changed, allowed)
            who = character.read(data)
            self.assertEqual((who.name, who.level, data[at + 0x26]), ('Hunter', 14, 1))  # origin kept
        self.assertEqual(self.read('userdata0001'), before['userdata0001'])  # the donor stays as it was
        for name in ('userdata0010', 'backup0010'):
            profile = self.read(name)
            face = appearance.profile_face(profile, 0, 'Hunter')
            self.assertEqual(profile[face:face + appearance.SIZE], look(2))
        # The saves from before are kept outside savedata.
        self.assertEqual(kept.parent.parent, self.user / 'appearance-backups')
        self.assertEqual(appearance.read((kept / 'userdata0000').read_bytes()), (look(1), 1, 0))

    def test_a_backup_of_another_state_is_left_alone(self):
        self.write('backup0000', make_slot('Hunter', 1, attributes=(12, 14, 11, 11, 10, 7)))  # a level later
        before = self.read('backup0000')
        appearance.copy(self.folder, 1, 0, backup=False)
        self.assertEqual(self.read('backup0000'), before)

    def test_refuses_what_it_cannot_do_and_writes_nothing(self):
        before = {p.name: p.read_bytes() for p in self.folder.iterdir()}
        with self.assertRaisesRegex(ValueError, 'two different'):
            appearance.copy(self.folder, 0, 0)
        with self.assertRaisesRegex(ValueError, 'no character in slot 3'):
            appearance.copy(self.folder, 2, 0)
        with self.assertRaisesRegex(ValueError, 'no character in slot 4'):
            appearance.copy(self.folder, 0, 3)  # no file at all
        self.write('userdata0003', make_slot('Twin', 1))
        with self.assertRaisesRegex(ValueError, 'already looks like this'):
            appearance.copy(self.folder, 3, 0)
        (self.folder / 'userdata0003').unlink()
        self.assertEqual({p.name: p.read_bytes() for p in self.folder.iterdir()}, before)
        self.assertFalse((self.user / 'appearance-backups').exists())

    def test_a_write_cut_off_leaves_the_character_as_it_was(self):
        before = self.read('userdata0000')
        with mock.patch.object(appearance.textfile.os, 'replace', side_effect=OSError(28, 'No space left on device')):
            with self.assertRaises(OSError):
                appearance.copy(self.folder, 1, 0, backup=False)
        self.assertEqual(self.read('userdata0000'), before)
        self.assertFalse(list(self.folder.glob('*.tmp')))


if __name__ == '__main__':
    unittest.main()
