"""What the Saves page tells about a character (launcher/bbport_character.py): level, attributes,
weapons and beaten bosses from a slot file, named with the game's own texts; on made-up slots,
backups and text archives."""
from paths import ROOT
import os
import shutil
import struct
import sys
import tempfile
import unittest
from unittest import mock
import zlib
from pathlib import Path

sys.path.insert(0, str(ROOT / 'launcher'))
import bbport_character as character  # noqa: E402
import bbport_saves as saves  # noqa: E402

SLOT, PROFILE = 1310720, 262144
CLERIC_BEAST, GASCOIGNE, SHADOW = 500000, 271000, 212010
SAW_CLEAVER, SAW_SPEAR, HUNTER_PISTOL, BARE_FISTS = 7000000, 7100000, 14000000, 1000000


def make_slot(name='Gehrmanling', attributes=(17, 12, 9, 15, 7, 8), level=None, bosses=(CLERIC_BEAST, GASCOIGNE),
              weapons=(SAW_CLEAVER + 200, HUNTER_PISTOL, SAW_SPEAR, BARE_FISTS), face=0x14000):
    """A slot as the game writes it, with what the reader looks at; the rest zeros."""
    data = bytearray(SLOT)
    struct.pack_into('<I', data, 0x08, 8385323)  # 2:19:45 played
    data[face:face + 4] = b'FACE'
    at = face - character.FACE_TO_NAME
    text = name.encode('utf-16-le')
    data[at:at + len(text)] = text
    for (_key, offset), value in zip(character.ATTRIBUTES, attributes):
        struct.pack_into('<I', data, at + offset, value)
    struct.pack_into('<I', data, at + character.LEVEL, sum(attributes) - 50 if level is None else level)
    struct.pack_into('<I', data, at + character.ECHOES, 5311)
    struct.pack_into('<I', data, at + character.INSIGHT, 6)
    struct.pack_into('<I', data, at + character.FLAGS + character.DEATHS, 6)
    for npc, offset, bits in character.BOSSES:
        data[at + character.FLAGS + offset] = bits if npc in bosses else bits >> 1  # neighbours set: no matter
    entry = at + character.INVENTORY
    data[entry:entry + 16] = bytes.fromhex('40f0ffff 520000b0 e8030040 05000000')  # an item (5 Blood Vials)
    data[entry + 16:entry + 32] = bytes.fromhex('41f0ffff 53000080 e8030010 01000000')  # a piece of armour
    for number, ident in enumerate(weapons):
        entry = at + character.INVENTORY + 32 + 16 * number
        struct.pack_into('<IIII', data, entry, 0x3A9BC049 + number, 0x80800058 + number, ident, 1)
    return bytes(data)


def fmg(strings):
    """An FMG (version 2) with {id: text}."""
    ids = sorted(strings)
    groups = []
    for string_id in ids:
        if groups and groups[-1][2] == string_id - 1:
            groups[-1][2] = string_id
        else:
            groups.append([sum(g[2] - g[1] + 1 for g in groups), string_id, string_id])
    offsets_at = 0x28 + 16 * len(groups)
    text_at = offsets_at + 8 * len(ids)
    texts, slots = bytearray(), []
    for string_id in ids:
        slots.append(text_at + len(texts))
        texts += strings[string_id].encode('utf-16-le') + b'\0\0'
    out = bytearray(0x28)
    struct.pack_into('<I', out, 0x0C, len(groups))
    struct.pack_into('<Q', out, 0x18, offsets_at)
    for group in groups:
        out += struct.pack('<iiii', *group, 0)
    for slot in slots:
        out += struct.pack('<q', slot)
    return bytes(out + texts)


def dcx_bnd4(files):
    """A DFLT DCX holding a BND4 with {id: bytes}."""
    body = bytearray(0x40 + 0x24 * len(files))
    body[:4] = b'BND4'
    struct.pack_into('<I', body, 0x0C, len(files))
    struct.pack_into('<Q', body, 0x20, 0x24)
    for i, (file_id, data) in enumerate(sorted(files.items())):
        entry = 0x40 + 0x24 * i
        struct.pack_into('<Q', body, entry + 0x08, len(data))
        struct.pack_into('<II', body, entry + 0x18, len(body), file_id)
        body += data
    header = bytearray(0x4C)
    header[:4], header[0x28:0x2C] = b'DCX\0', b'DFLT'
    return bytes(header) + zlib.compress(bytes(body))


def make_game(root, folder, weapons, npcs, menu):
    msg = Path(root) / 'dvdroot_ps4' / 'msg' / folder
    msg.mkdir(parents=True)
    (msg / 'item.msgbnd.dcx').write_bytes(dcx_bnd4({character.WEAPON_NAMES: fmg(weapons),
                                                    character.NPC_NAMES: fmg(npcs)}))
    (msg / 'menu.msgbnd.dcx').write_bytes(dcx_bnd4({character.MENU_TEXTS: fmg(menu),
                                                    character.COMMON_TEXTS: fmg({character.PLAY_TIME_TEXT: 'Spielzeit'})}))


class ReadTests(unittest.TestCase):
    def test_what_a_character_is(self):
        info = character.read(make_slot())
        self.assertEqual(info.name, 'Gehrmanling')
        self.assertEqual(info.level, 18)
        self.assertEqual(list(info.attributes.values()), [17, 12, 9, 15, 7, 8])
        self.assertEqual((info.echoes, info.insight, info.deaths, info.played), (5311, 6, 6, 8385))
        self.assertEqual(info.bosses, [CLERIC_BEAST, GASCOIGNE])

    def test_weapons_trick_weapons_first_without_the_bare_fists_or_items(self):
        info = character.read(make_slot())
        self.assertEqual(info.weapons, [SAW_CLEAVER + 200, SAW_SPEAR, HUNTER_PISTOL])

    def test_a_weapon_kind_once_at_its_best_level(self):
        info = character.read(make_slot(weapons=(SAW_CLEAVER + 100, SAW_CLEAVER + 500, SAW_CLEAVER + 10000)))
        self.assertEqual(info.weapons, [SAW_CLEAVER + 500, SAW_CLEAVER + 10000])  # the Uncanny one is another

    def test_the_slot_may_be_laid_out_further_on(self):
        self.assertEqual(character.read(make_slot(face=0x1F000)).level, 18)

    def test_numbers_that_do_not_add_up_are_not_shown(self):
        self.assertIsNone(character.read(make_slot(level=40)))
        self.assertIsNone(character.read(make_slot(attributes=(0, 0, 0, 0, 0, 50), level=0)))
        self.assertIsNone(character.read(bytes(SLOT)))
        self.assertIsNone(character.read(b'FACE'))

    def test_two_states_of_a_character_compared(self):
        now, before = character.read(make_slot()), character.read(make_slot(bosses=(CLERIC_BEAST,)))
        self.assertEqual(now.summary(), character.read(make_slot()).summary())
        self.assertNotEqual(now.summary(), before.summary())


class NameTests(unittest.TestCase):
    def setUp(self):
        self.game = tempfile.TemporaryDirectory()
        self.addCleanup(self.game.cleanup)
        make_game(self.game.name, 'deude', {SAW_CLEAVER: 'Sägehackbeil', SAW_CLEAVER + 200: 'Sägehackbeil +2',
                                            HUNTER_PISTOL: 'Jägerpistole'},
                  {CLERIC_BEAST: 'Kleriker-Bestie'}, {character.LEVEL_TEXT: 'Stufe', 10400: 'Vitalität'})
        make_game(self.game.name, 'engus', {SAW_CLEAVER: 'Saw Cleaver'}, {CLERIC_BEAST: 'Cleric Beast'},
                  {character.LEVEL_TEXT: 'Level'})
        character.names.cache_clear()

    def test_the_games_own_names_in_its_text_language(self):
        words = character.names(self.game.name, '4')
        self.assertEqual(words.weapon(SAW_CLEAVER + 200), 'Sägehackbeil +2')
        self.assertEqual(words.weapon(SAW_CLEAVER + 300), 'Sägehackbeil +3')  # from the plain name
        self.assertEqual(words.boss(CLERIC_BEAST), 'Kleriker-Bestie')
        self.assertEqual(words.word(character.LEVEL_TEXT), 'Stufe')
        self.assertEqual(words.attribute('vitality'), 'Vitalität')
        self.assertEqual(words.attribute('arcane'), 'Arcane')  # not in the file: English
        self.assertEqual(words.word(character.PLAY_TIME_TEXT), 'Spielzeit')

    def test_english_when_the_language_is_missing_and_nothing_without_the_game(self):
        self.assertEqual(character.names(self.game.name, '8').weapon(SAW_CLEAVER), 'Saw Cleaver')
        empty = character.names(str(Path(self.game.name) / 'nowhere'), '4')
        self.assertIsNone(empty.weapon(SAW_CLEAVER))
        self.assertEqual(empty.word(character.LEVEL_TEXT), 'Level')


def make_profile(names, played=0):
    """A profile with the Load Game menu's block (name, then what the menu shows) for each slot."""
    data = bytearray(PROFILE)
    data[:4] = b'\x1a\0\0\0'
    for slot, name in names.items():
        data[saves.PROFILE_LISTED + slot] = 1
        start = saves.PROFILE_SLOTS + slot * saves.PROFILE_SLOT_SIZE
        data[start:start + 2 * len(name)] = name.encode('utf-16-le')
        struct.pack_into('<I', data, start + 0x26, played)
    data[0x2100:0x2104] = struct.pack('<I', played)  # outside the blocks: must stay as it is now
    return bytes(data)


def make_folder(user, slots, profile):
    folder = saves.target_dir(user)
    folder.mkdir(parents=True, exist_ok=True)
    for i in range(10):
        (folder / f'userdata{i:04d}').write_bytes(slots.get(i, bytes(SLOT)))
    (folder / 'userdata0010').write_bytes(profile)
    return folder


class RestoreCharacterTests(unittest.TestCase):
    """One character back to a backup's state; the other characters and the rest of the profile stay."""

    def setUp(self):
        self.user = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.user, ignore_errors=True)
        self.then = {0: make_slot('Maria', bosses=()), 2: make_slot(bosses=(CLERIC_BEAST,))}
        make_folder(self.user, self.then, make_profile({0: 'Maria', 2: 'Gehrmanling'}, played=100))
        self.archive = saves.backup(self.user)
        self.now = {0: make_slot('Maria', bosses=(CLERIC_BEAST, GASCOIGNE, SHADOW)), 2: make_slot()}
        self.folder = make_folder(self.user, self.now, make_profile({0: 'Maria', 2: 'Gehrmanling'}, played=900))

    def test_only_that_character_goes_back(self):
        before = saves.restore_character(self.archive, 'userdata0002', self.user)
        self.assertEqual((self.folder / 'userdata0002').read_bytes(), self.then[2])
        self.assertEqual((self.folder / 'userdata0000').read_bytes(), self.now[0])
        profile = (self.folder / 'userdata0010').read_bytes()
        old, new = make_profile({0: 'Maria', 2: 'Gehrmanling'}, 100), make_profile({0: 'Maria', 2: 'Gehrmanling'}, 900)
        self.assertEqual(saves.profile_block(profile, 'userdata0002'), saves.profile_block(old, 'userdata0002'))
        self.assertEqual(saves.profile_block(profile, 'userdata0000'), saves.profile_block(new, 'userdata0000'))
        self.assertEqual(profile[0x2100:0x2104], new[0x2100:0x2104])
        self.assertEqual(saves.Backup(before).reason, 'restore')  # the state before is kept
        self.assertFalse((self.user / 'save_import').exists())

    def test_another_character_in_the_slot_is_refused(self):
        (self.folder / 'userdata0002').write_bytes(make_slot('Djura'))
        with self.assertRaises(OSError):
            saves.restore_character(self.archive, 'userdata0002', self.user)
        self.assertEqual((self.folder / 'userdata0002').read_bytes(), make_slot('Djura'))

    def test_a_profile_laid_out_otherwise_is_refused(self):
        (self.folder / 'userdata0010').write_bytes(make_profile({0: 'Maria'}))  # slot 2's block unnamed
        with self.assertRaises(OSError):
            saves.restore_character(self.archive, 'userdata0002', self.user)
        self.assertEqual((self.folder / 'userdata0002').read_bytes(), self.now[2])

    def test_the_slot_goes_back_when_the_profile_cannot_follow(self):
        """The slot and the profile were replaced one after the other: a profile that would not move left the
        slot of one state and the Load Game menu's block of another."""
        def state():
            return {p.name: zlib.crc32(p.read_bytes()) for p in self.folder.iterdir()}
        before = state()
        real = os.replace

        def replace(source, target):
            if Path(target).name == 'userdata0010' and 'save_import' in Path(source).parts:
                raise OSError(28, 'No space left on device')
            return real(source, target)
        with mock.patch.object(saves.os, 'replace', side_effect=replace):
            with self.assertRaises(OSError):
                saves.restore_character(self.archive, 'userdata0002', self.user)
        self.assertEqual(state(), before)
        self.assertFalse((self.user / saves.REPLACED_DIR).exists())


class BackupTests(unittest.TestCase):
    def test_the_characters_in_a_backup(self):
        with tempfile.TemporaryDirectory() as user:
            folder = saves.target_dir(user)
            folder.mkdir(parents=True)
            for i in range(10):
                (folder / f'userdata{i:04d}').write_bytes(make_slot() if i == 2 else bytes(SLOT))
            (folder / 'userdata0010').write_bytes(make_profile({2: 'Gehrmanling'}))
            self.assertEqual(list(saves.characters(folder)), ['userdata0002'])
            item = saves.Backup(saves.backup(user))
            found = saves.backup_characters(item)
            self.assertEqual(list(found), ['userdata0002'])
            self.assertEqual(found['userdata0002'].level, 18)

    def test_a_character_deleted_in_the_game_is_neither_here_nor_in_backups(self):
        """The game keeps the slot file of a deleted character and clears its byte in the profile."""
        with tempfile.TemporaryDirectory() as user:
            folder = make_folder(user, {0: make_slot('Maria'), 2: make_slot()}, make_profile({0: 'Maria'}))
            self.assertEqual(list(saves.characters(folder)), ['userdata0000'])
            self.assertEqual(list(saves.backup_characters(saves.Backup(saves.backup(user)))), ['userdata0000'])


if __name__ == '__main__':
    unittest.main()
