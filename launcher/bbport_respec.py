#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Respec: spread a character's attribute points again (launcher, Cheats page), game 1.09 saves.

A character's six attributes are 32-bit values 8 bytes apart in its save file (userdata0000 to
userdata0009, kept unencrypted by the port), with an unused slot after Endurance:

    +0x00 Vitality  +0x08 Endurance  (+0x10 unused)  +0x18 Strength  +0x20 Skill
    +0x28 Bloodtinge  +0x30 Arcane  ...  +0x50 level  +0x54 Blood Echoes  +0x68 name (UTF-16)

The level is always the attributes' sum minus 50, so a respec keeps the sum: the level and the
Blood Echoes stay, and nothing has to be refunded. The game works out HP, stamina and attack
from the attributes when it loads the save (checked: Vitality 11 -> 20 shows 594 / 793 HP; the
current HP refills at a lamp). The file holds two copies of the block and the game's own backup
file (backup0000) another; every copy of the character is rewritten.

No attribute goes below the lowest any origin starts with (FLOORS), so a respec cannot make a
character stronger than a fresh one of the same level could be. The launcher puts the save in
its backups first (Saves, "Before a respec"); without one, a copy of the save folder is made
before the write, outside savedata (<user folder>/respec-backups/<time>/SPRJ0005)."""
import array
import shutil
import struct
import sys
import time
from pathlib import Path

ATTRIBUTES = ('vitality', 'endurance', 'strength', 'skill', 'bloodtinge', 'arcane')
OFFSETS = (0x00, 0x08, 0x18, 0x20, 0x28, 0x30)  # from Vitality
LEVEL, ECHOES, NAME = 0x50, 0x54, 0x68
UNUSED = 0x10
LEVEL_BASE = 50  # level = sum of the attributes - 50
MAX = 99
# The lowest each attribute starts at among the origins (Noble Scion: Vitality 7, Endurance 8;
# Cruel Fate: Bloodtinge 5; Military Veteran: Arcane 6; Strength and Skill 9).
FLOORS = {'vitality': 7, 'endurance': 8, 'strength': 9, 'skill': 9, 'bloodtinge': 5, 'arcane': 6}
SLOTS = 10


def _u32(data, at):
    return struct.unpack_from('<I', data, at)[0]


def find_blocks(data):
    """Offsets of every character attribute block in a save file's bytes."""
    words = array.array('I', bytes(data[:len(data) // 4 * 4]))
    if sys.byteorder != 'little':
        words.byteswap()
    slots = [o // 4 for o in OFFSETS]
    found = []
    for i in range(len(words) - (NAME + 34) // 4):
        if not 1 <= words[i] <= MAX or words[i + 1] or words[i + UNUSED // 4]:
            continue
        values = [words[i + s] for s in slots]
        # The gaps after the attributes are zero; the level matches their sum.
        if not all(1 <= v <= MAX for v in values) or any(words[i + s + 1] for s in slots):
            continue
        if words[i + LEVEL // 4] == sum(values) - LEVEL_BASE:
            found.append(i * 4)
    return found


def read_block(data, at):
    name = data[at + NAME:at + NAME + 34].decode('utf-16-le', errors='replace').split('\0')[0]
    return {'name': name, 'level': _u32(data, at + LEVEL), 'echoes': _u32(data, at + ECHOES),
            'attributes': {a: _u32(data, at + o) for a, o in zip(ATTRIBUTES, OFFSETS)}}


def save_folders(user_dir):
    """The game's save folders under the port's user folder (savedata/<user>/<title>/SPRJ0005)."""
    root = Path(user_dir) / 'savedata'
    return sorted(p for p in root.glob('*/*/SPRJ*') if p.is_dir() and '.' not in p.name) \
        if root.is_dir() else []


def characters(folder):
    """[(slot, character)] of the save folder: the first block in each userdata file."""
    out = []
    for slot in range(SLOTS):
        path = Path(folder) / f'userdata{slot:04d}'
        if not path.is_file():
            continue
        data = path.read_bytes()
        blocks = find_blocks(data)
        if blocks:
            out.append((slot, read_block(data, blocks[0])))
    return out


def check(old, new):
    """Raises ValueError unless `new` ({attribute: value}) is a respec of `old`."""
    if set(new) != set(ATTRIBUTES):
        raise ValueError('all six attributes are needed')
    for a in ATTRIBUTES:
        if not FLOORS[a] <= new[a] <= MAX:
            raise ValueError(f'{a.capitalize()} must be between {FLOORS[a]} and {MAX}')
    if sum(new.values()) != sum(old.values()):
        raise ValueError(f'the attributes must add up to {sum(old.values())} '
                         f'(now {sum(new.values())}), so the level stays the same')


def respec(folder, slot, new, now=None, copy=True):
    """Writes `new` attributes for the character in `slot`. Returns the backup folder (copy=False: the
    caller made a backup, None)."""
    folder = Path(folder)
    path = folder / f'userdata{slot:04d}'
    data = bytearray(path.read_bytes())
    blocks = find_blocks(data)
    if not blocks:
        raise ValueError(f'no character in slot {slot + 1}')
    old = read_block(data, blocks[0])
    check(old['attributes'], new)
    # <user>/savedata/<user id>/<title>/SPRJ0005 -> <user>/respec-backups/<time>/SPRJ0005
    stamp = time.strftime('%Y%m%d-%H%M%S', time.localtime(now))
    backup = folder.parents[3] / 'respec-backups' / stamp / folder.name if copy else None
    if backup and not backup.exists():
        shutil.copytree(folder, backup)

    def rewrite(data, blocks):
        for at in blocks:
            for a, o in zip(ATTRIBUTES, OFFSETS):
                struct.pack_into('<I', data, at + o, new[a])

    rewrite(data, blocks)
    path.write_bytes(bytes(data))
    # The game's backup files hold a copy of the character last saved: the same one only if
    # its name, level and attributes match.
    for other in sorted(folder.glob('backup*')):
        copy = bytearray(other.read_bytes())
        same = [at for at in find_blocks(copy) if read_block(copy, at) == old]
        if same:
            rewrite(copy, same)
            other.write_bytes(bytes(copy))
    return backup
