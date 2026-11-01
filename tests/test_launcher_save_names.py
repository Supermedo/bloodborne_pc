"""Test of launcher/bbport_saves.py's names and bbport_character.py's reader: a slot the game's layout reader (bbport_character)
reads is named by it, also when the eight bytes before the name are not all zero, where the search
the launcher had took the first name-like text after zero bytes (another field's bytes). A slot that
reader cannot read (no appearance block, a level that does not fit the attributes, cut short, or a
reader that raises) is named by that search as before; an empty slot has no name. A slot cut short
before the boss bits, as the first 128 KiB the launcher read of a slot was, reads as none (it raised
IndexError, in the launcher's character cards too). character_name and SaveSet, which names a
backup, read whole slot files; a backup without its list of names is named from the slots in it, and
so is a backup taken from a file.
"""
from paths import ROOT
import json
import struct
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path

sys.path.insert(0, str(ROOT / 'launcher'))
import bbport_character as character  # noqa: E402
import bbport_saves as saves  # noqa: E402

AT = 0xB6F0  # where the name is in the slots these were found in
SIZE = 0x140000  # a slot file
DECOY = 'ĩ肀ŕ肀Ɓ肀Ů肀'  # what the search took: a field 0x184 bytes after the name
ATTRIBUTES = {'vitality': 20, 'endurance': 12, 'strength': 15, 'skill': 18, 'bloodtinge': 9, 'arcane': 8}


def slot(name, before=b'\0' * 8, face=True, size=SIZE, level=None, decoy=None):
    data = bytearray(size)
    struct.pack_into('<I', data, 0x08, 3_600_000)  # play time: a used slot has bytes in its first 4 KiB
    if decoy is not None:
        at = AT + 0x184
        data[at:at + len(decoy) * 2] = decoy.encode('utf-16-le')  # zero bytes before and after it
    data[AT - 8:AT] = before
    data[AT:AT + len(name) * 2] = name.encode('utf-16-le')
    for key, offset in character.ATTRIBUTES:
        struct.pack_into('<I', data, AT + offset, ATTRIBUTES[key])
    struct.pack_into('<I', data, AT + character.LEVEL,
                     sum(ATTRIBUTES.values()) - 50 if level is None else level)
    if face and AT + character.FACE_TO_NAME + 4 <= size:
        data[AT + character.FACE_TO_NAME:AT + character.FACE_TO_NAME + 4] = b'FACE'
    return bytes(data)



class SaveNameTests(unittest.TestCase):
    def test_names(self):
        problems = []

        def check(ok, what):
            if not ok:
                problems.append(what)

        dirty = b'\x01\x00\x00\x00\x02\x00\x00\x00'
        check(saves.name_in(slot('Maria')) == 'Maria', 'a slot as the game writes it')
        check(saves.name_in(slot('Maria', before=dirty, face=False, decoy=DECOY)) == DECOY,
              'the case the fix is for: the search alone takes the other field')
        check(saves.name_in(slot('Maria', before=dirty, decoy=DECOY)) == 'Maria', 'the fix: the layout names it')
        check(saves.name_in(slot('Maria', face=False)) == 'Maria', 'no appearance block: the search, as before')
        check(saves.name_in(slot('Maria', level=1)) == 'Maria', 'a level the attributes do not give: the search')
        check(saves.name_in(slot('Maria', size=AT + character.FACE_TO_NAME + 64)) == 'Maria',
              'cut short after the appearance block: the search')
        short = slot('Maria', size=AT + character.FLAGS + character.DEATHS + 8)  # past the deaths, before the boss bits
        check(character.read(short) is None, 'cut short before the boss bits: none, no IndexError')
        check(character.read(slot('Maria')[:0x20000]) is None, 'the first 128 KiB of a slot: none')
        check(saves.name_in(short) == 'Maria', 'cut short before the boss bits: the search')
        check(saves.name_in(slot('Maria', before=dirty, face=False)) == '', 'neither finds it: no name')
        check(saves.name_in(bytes(SIZE)) == '', 'an empty slot')
        check(saves.name_in(b'') == '', 'no bytes')
        real_read = character.read
        try:
            def broken(data):
                raise IndexError('a layout this reader does not know')
            character.read = broken
            check(saves.name_in(slot('Maria')) == 'Maria', 'a reader that raises: the search')
        finally:
            character.read = real_read

        with tempfile.TemporaryDirectory() as tmp:
            folder = Path(tmp) / 'SPRJ0005'
            folder.mkdir()
            (folder / 'userdata0000').write_bytes(slot('Maria', before=dirty, decoy=DECOY))
            (folder / 'userdata0001').write_bytes(slot('Hunter'))
            (folder / 'userdata0010').write_bytes(bytes(0x40000))
            (folder / 'userdata0002').write_bytes(short)
            check(character.read_file(folder / 'userdata0002') is None, 'read_file of a slot cut short')
            check(sorted(saves.characters(folder)) == ['userdata0000', 'userdata0001'],
                  'the character cards skip a slot cut short')
            (folder / 'userdata0002').unlink()
            check(saves.character_name(folder / 'userdata0000') == 'Maria', 'character_name reads the whole file')
            check(saves.character_name(folder / 'missing') == '', 'character_name of a missing file')
            check(saves.SaveSet('', folder).names == ['Maria', 'Hunter'], 'the names a backup is named with')

            old = Path(tmp) / 'saves_2026-10-20_153200.zip'  # an old backup: no list of names in it
            with zipfile.ZipFile(old, 'w') as z:
                for name in ('userdata0000', 'userdata0001', 'userdata0010'):
                    z.write(folder / name, f'savedata/1/CUSA00207/SPRJ0005/{name}')
            check(saves.Backup(old).names == ['Maria', 'Hunter'], 'a backup without its names, named from its slots')

            user = Path(tmp) / 'user'
            made = saves.add_backup_file(old, user)
            with zipfile.ZipFile(made) as z:
                meta = json.loads(z.read(saves.BACKUP_META))
            check(meta['characters'] == ['Maria', 'Hunter'] and 'Maria, Hunter' in made.name,
                  f'a backup taken from a file: {made.name}, {meta["characters"]}')
        self.assertEqual(problems, [])


if __name__ == '__main__':
    unittest.main()
