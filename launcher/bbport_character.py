# SPDX-License-Identifier: GPL-2.0-or-later
"""A character as the player knows it: level, attributes, weapons and the bosses it has beaten,
read from its slot file (userdata0000-0009) and named with the game's own texts.

The slot's layout, as the Bloodborne Save Editor (github.com/Noxde/Bloodborne-save-editor)
documents it and checked against the characters on this PC (2026-10): the appearance block
("FACE") lies 34496 bytes after the character's name, and the rest is counted from the name:

  level, the six attributes, echoes, insight   u32s before the name
  the inventory                                1984 entries of 16 bytes from name + 468
  the event flags                              bytes from name + 68544; a beaten boss sets bits
  deaths                                       u32 at the event flags + 360
  play time                                    u32 milliseconds at 0x08 of the file

A character is only read when its level is what the game makes of its attributes (level =
attributes - 50): better nothing than wrong numbers.
"""
import struct
import zlib
from functools import lru_cache
from pathlib import Path

FACE_TO_NAME = 34496
NAME_BYTES = 34  # 16 UTF-16 characters and the zero after them
LEVEL, ECHOES, INSIGHT = -24, -20, -36
ATTRIBUTES = (('vitality', -104), ('endurance', -96), ('strength', -80), ('skill', -72),
              ('bloodtinge', -64), ('arcane', -56))
INVENTORY, INVENTORY_SLOTS = 468, 1984
FLAGS, DEATHS, JOURNEY = 68544, 360, 68830

# The bosses in the order they are met: (their name in the game's NPC names, the byte after the
# event flags' start, the bits that are set once it is beaten). The Old Hunters' five come last.
BOSSES = (
    (500000, 21714, 0x08),  # Cleric Beast
    (271000, 21727, 0x80),  # Father Gascoigne
    (209000, 21477, 0x80),  # Blood-starved Beast
    (502000, 21602, 0x80),  # Vicar Amelia
    (210000, 21352, 0x80),  # The Witch of Hemwick
    (212010, 22227, 0x80),  # Shadow of Yharnam
    (508000, 21464, 0x08),  # Darkbeast Paarl
    (510000, 22602, 0x80),  # Rom, the Vacuous Spider
    (507000, 22352, 0x80),  # The One Reborn
    (232000, 21977, 0x80),  # Martyr Logarius
    (512000, 22727, 0x80),  # Amygdala
    (257000, 21839, 0x08),  # Celestial Emissary
    (251000, 21852, 0x80),  # Ebrietas, Daughter of the Cosmos
    (899000, 22108, 0x3F),  # Micolash, Host of the Nightmare
    (551000, 22102, 0x80),  # Mergo's Wet Nurse
    (804000, 21102, 0x80),  # Gehrman, the First Hunter
    (540000, 21108, 0x20),  # Moon Presence
    (451000, 22852, 0x80),  # Ludwig
    (403000, 22983, 0x20),  # Living Failures
    (452000, 22977, 0x80),  # Lady Maria of the Astral Clocktower
    (450000, 22858, 0x20),  # Laurence, the First Vicar
    (453000, 23102, 0x80),  # Orphan of Kos
)
DLC_BOSSES = 5

# A weapon's id: its kind (a multiple of 100000), Uncanny (+10000) or Lost (+20000), its level
# (x 100). Trick weapons first, then the firearms, shields and torches of the left hand.
TRICK_WEAPONS = (2000000, 4000000, 5000000, 5100000, 7000000, 7100000, 8000000, 8100000, 9000000, 10000000,
                 10100000, 11000000, 12000000, 13000000, 22000000, 23000000, 24000000, 25000000, 26000000,
                 27000000, 28000000, 29000000, 30000000, 31000000, 32000000, 34000000, 38000000)
LEFT_HAND = (6000000, 6100000, 14000000, 14100000, 14200000, 15000000, 18000000, 18100000, 19000000,
             19100000, 20000000, 20100000, 33000000, 35000000, 36000000)
WEAPON_KINDS = TRICK_WEAPONS + LEFT_HAND


class Character:
    def __init__(self, name, level, attributes, echoes, insight, deaths, journey, played, weapons, bosses):
        self.name, self.level, self.attributes = name, level, attributes
        self.echoes, self.insight, self.deaths, self.journey = echoes, insight, deaths, journey
        self.played = played  # seconds
        self.weapons = weapons  # weapon ids, trick weapons first
        self.bosses = bosses  # NPC name ids of the beaten bosses, in BOSSES' order

    def summary(self):
        """What tells two states of a character apart."""
        return self.level, tuple(self.attributes.values()), tuple(self.weapons), tuple(self.bosses), self.echoes


def read(data):
    """The Character in a slot file's bytes; None when it holds none, or one this cannot read."""
    face = data.find(b'FACE', 0xF000)
    at = face - FACE_TO_NAME
    if face < 0 or at < 0x80 or at + FLAGS + DEATHS + 4 > len(data) or at + JOURNEY >= len(data):
        return None
    name = data[at:at + NAME_BYTES].decode('utf-16-le', errors='replace').split('\0')[0].strip()

    def u32(offset):
        return struct.unpack_from('<I', data, at + offset)[0]
    attributes = {key: u32(offset) for key, offset in ATTRIBUTES}
    level = u32(LEVEL)
    if not name or '�' in name or not all(1 <= v <= 99 for v in attributes.values()) \
            or level != sum(attributes.values()) - 50:
        return None
    flags = at + FLAGS
    bosses = [npc for npc, offset, bits in BOSSES if data[flags + offset] & bits == bits]
    journey = data[at + JOURNEY]
    return Character(name, level, attributes, u32(ECHOES), u32(INSIGHT), u32(FLAGS + DEATHS),
                     journey if journey <= 8 else 0, struct.unpack_from('<I', data, 0x08)[0] // 1000,
                     weapons_in(data, at + INVENTORY), bosses)


def weapons_in(data, start):
    """The weapons the character carries (not the bare fists), each kind once at its best level."""
    best = {}
    for at in range(start, start + INVENTORY_SLOTS * 16, 16):
        if (data[at + 7], data[at + 11]) == (0xB0, 0x40) or data[at + 11] == 0x10:
            continue  # an item, a piece of armour
        ident = struct.unpack_from('<I', data, at + 8)[0]
        kind, imprint, level = ident // 100000 * 100000, ident % 100000 // 10000, ident % 10000 // 100
        if kind not in WEAPON_KINDS or imprint not in (0, 1, 2, 8) or level > 10 or ident % 100:
            continue
        key = kind + imprint * 10000
        if key not in best or level > best[key] % 10000 // 100:
            best[key] = ident
    return sorted(best.values(), key=lambda ident: WEAPON_KINDS.index(ident // 100000 * 100000))


def read_file(path):
    try:
        return read(Path(path).read_bytes())
    except OSError:
        return None


# ---- the game's own names (msg/<language>/item.msgbnd.dcx and menu.msgbnd.dcx) ----------------
# The game's text language (BB_LANGUAGE, the PS4 system language) and its folder.
MSG_FOLDERS = {'0': 'jpnjp', '1': 'engus', '2': 'frafr', '3': 'spaes', '4': 'deude', '5': 'itait', '8': 'rusru',
               '9': 'korkr', '17': 'porbr'}
WEAPON_NAMES, NPC_NAMES, MENU_TEXTS, COMMON_TEXTS = 11, 18, 200, 76  # the FMGs in the archives
# Menu texts: the level, the echoes, the insight, the play time, the six attributes and the title
# menu's Continue and Load Game as the game writes them
LEVEL_TEXT, ECHOES_TEXT, INSIGHT_TEXT, PLAY_TIME_TEXT = 10200, 10201, 10210, 101016
CONTINUE_TEXT, LOAD_GAME_TEXT = 401301, 401302
ATTRIBUTE_TEXTS = dict(zip((key for key, _ in ATTRIBUTES), range(10400, 10406), strict=True))
ENGLISH = {LEVEL_TEXT: 'Level', ECHOES_TEXT: 'Blood Echoes', INSIGHT_TEXT: 'Insight', PLAY_TIME_TEXT: 'Play Time',
           CONTINUE_TEXT: 'Continue', LOAD_GAME_TEXT: 'Load Game', 10400: 'Vitality', 10401: 'Endurance',
           10402: 'Strength', 10403: 'Skill', 10404: 'Bloodtinge', 10405: 'Arcane'}
TRANSLATE = str  # the launcher's _ (set by it), for the words of its cards: a table has the game's own word


def unpack_dcx(data):
    if data[:4] != b'DCX\0' or data[0x28:0x2C] != b'DFLT':
        raise ValueError('not a DFLT DCX archive')
    return zlib.decompress(data[0x4C:])


def bnd4_files(data):
    """{file id: bytes} of a BND4 archive with 0x24-byte entries."""
    if data[:4] != b'BND4' or struct.unpack_from('<Q', data, 0x20)[0] != 0x24:
        raise ValueError('not a BND4 archive this can read')
    files = {}
    for i in range(struct.unpack_from('<I', data, 0x0C)[0]):
        entry = 0x40 + i * 0x24
        size, = struct.unpack_from('<Q', data, entry + 0x08)
        offset, file_id = struct.unpack_from('<II', data, entry + 0x18)
        files[file_id] = data[offset:offset + size]
    return files


def read_fmg(data):
    """{id: text} of an FMG (version 2: 64-bit string offsets); empty strings left out."""
    groups, = struct.unpack_from('<I', data, 0x0C)
    offsets, = struct.unpack_from('<Q', data, 0x18)
    strings = {}
    for g in range(groups):
        index, first, last = struct.unpack_from('<iii', data, 0x28 + 16 * g)
        for k, string_id in enumerate(range(first, last + 1)):
            at, = struct.unpack_from('<q', data, offsets + 8 * (index + k))
            if at > 0:
                end = at
                while data[end:end + 2] != b'\0\0':
                    end += 2
                strings[string_id] = data[at:end].decode('utf-16-le')
    return strings


class Names:
    """The game's names for weapons, bosses and the menu's words in one language (English, then
    nothing, where the game's files are missing)."""

    def __init__(self, weapons=None, npcs=None, menu=None):
        self.weapons, self.npcs, self.menu = weapons or {}, npcs or {}, menu or {}

    def weapon(self, ident):
        """'Saw Cleaver +2' as the game writes it."""
        if ident in self.weapons:
            return self.weapons[ident]
        base = self.weapons.get(ident - ident % 10000)
        level = ident % 10000 // 100
        return (f'{base} +{level}' if level else base) if base else None

    def boss(self, npc):
        return self.npcs.get(npc)

    def word(self, text):
        word = self.menu.get(text) or ENGLISH.get(text, '')
        # The title menu's two words stay as that menu writes them: the launcher quotes them.
        return word if text in (CONTINUE_TEXT, LOAD_GAME_TEXT) else TRANSLATE(word)

    def attribute(self, key):
        return self.word(ATTRIBUTE_TEXTS[key])


@lru_cache(maxsize=4)
def names(game_dir, language='1'):
    """Names in the game's text language, from the game in `game_dir` (CUSA03173)."""
    msg = Path(game_dir) / 'dvdroot_ps4' / 'msg'
    for folder in (MSG_FOLDERS.get(str(language), 'engus'), 'engus'):
        try:
            item = bnd4_files(unpack_dcx((msg / folder / 'item.msgbnd.dcx').read_bytes()))
            menu = bnd4_files(unpack_dcx((msg / folder / 'menu.msgbnd.dcx').read_bytes()))
            words = {**read_fmg(menu[COMMON_TEXTS]), **read_fmg(menu[MENU_TEXTS])}
            return Names(read_fmg(item[WEAPON_NAMES]), read_fmg(item[NPC_NAMES]), words)
        except (OSError, ValueError, KeyError, struct.error, zlib.error):
            continue
    return Names()
