#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Change appearance: one character takes another's look (launcher, Cheats page), game 1.09 saves.

The game has no way to change a hunter's appearance after character creation (the Hunter's Dream
mirror that would have done it was cut), so the look comes from another character: the player
makes a new one in an empty slot with the look they want, then copies it onto their hunter here.

What is copied is what character creation sets, and nothing else:
- the appearance block, 0xF0 bytes from b'FACE' (version 3, size 0xF0): body and face sliders,
  hair, eyes, colours. Each slot file (userdata0000 to userdata0009, unencrypted in the port) holds
  one; the profile (userdata0010) holds one per slot in the block the Load Game menu shows.
- gender and voice: one byte each, 0x22 and 0x23 after the character's name (the origin, 0x26, stays).
Checked against the characters on this PC (2026-11); the Bloodborne Save Editor
(github.com/Noxde/Bloodborne-save-editor) finds the same block and bytes.

The level, attributes, items, origin and progress are untouched. The game's own backup files
(backup0000, backup0010) get the change too where they hold the same character. The launcher puts
the saves in its backups first (Saves, "Before an appearance change"); without one, a copy of the
save folder is made before the write (<user folder>/appearance-backups/<time>/SPRJ0005)."""
import shutil
import struct
import time
from pathlib import Path

import bbport_character as character
import bbport_textfile as textfile

MAGIC, VERSION, SIZE = b'FACE', 3, 0xF0
GENDER, VOICE = 0x22, 0x23  # after the name
SLOTS = 10
PROFILE = 'userdata0010'
# The profile's block per slot (bbport_saves.PROFILE_SLOTS): the name first, the appearance from +0x3A.
PROFILE_SLOTS, PROFILE_SLOT_SIZE, PROFILE_FACE = 0x1096, 0x192, 0x3A


def face_at(data, at):
    """True when an appearance block as the game writes it starts at `at`."""
    return data[at:at + 4] == MAGIC and struct.unpack_from('<II', data, at + 4) == (VERSION, SIZE)


def find_face(data):
    """The offset of a slot file's appearance block, None when it holds no character this can read."""
    if not character.read(data):
        return None
    at = data.find(MAGIC, 0xF000)  # as bbport_character.read finds it
    return at if face_at(data, at) else None


def name_at(face):
    return face - character.FACE_TO_NAME


def read(data):
    """(appearance block, gender, voice) of a slot file's character, None without one."""
    face = find_face(data)
    if face is None:
        return None
    name = name_at(face)
    return bytes(data[face:face + SIZE]), data[name + GENDER], data[name + VOICE]


def write(data, look):
    """Puts `look` (read()'s result) into a slot file's bytes (a bytearray)."""
    face = find_face(data)
    if face is None:
        raise ValueError('no character in this save file')
    block, gender, voice = look
    data[face:face + SIZE] = block
    data[name_at(face) + GENDER] = gender
    data[name_at(face) + VOICE] = voice


def save_folders(user_dir):
    """The game's save folders under the port's user folder (savedata/<user>/<title>/SPRJ0005)."""
    root = Path(user_dir) / 'savedata'
    return sorted(p for p in root.glob('*/*/SPRJ*') if p.is_dir() and '.' not in p.name) \
        if root.is_dir() else []


def characters(folder):
    """[(slot, Character)] of the save folder's slot files that hold a character."""
    out = []
    for slot in range(SLOTS):
        path = Path(folder) / f'userdata{slot:04d}'
        who = character.read_file(path) if path.is_file() else None
        if who and find_face(path.read_bytes()) is not None:
            out.append((slot, who))
    return out


def profile_face(profile, slot, name):
    """The offset of `slot`'s appearance block in the profile's bytes, when the block is that of `name`."""
    start = PROFILE_SLOTS + slot * PROFILE_SLOT_SIZE
    if start + PROFILE_SLOT_SIZE > len(profile):
        return None
    listed = profile[start:start + character.NAME_BYTES].decode('utf-16-le', errors='replace').split('\0')[0]
    face = start + PROFILE_FACE
    return face if listed.strip() == name and face_at(profile, face) else None


def check(folder, donor, target):
    """(target's slot file, donor's look, target's look) when slot `donor`'s look can go to slot `target`;
    ValueError when there is nothing to copy. Writes nothing."""
    if donor == target:
        raise ValueError('choose two different characters')
    files = {}
    for slot in (donor, target):
        path = Path(folder) / f'userdata{slot:04d}'
        files[slot] = bytearray(path.read_bytes()) if path.is_file() else bytearray()
    look, old = read(files[donor]), read(files[target])
    if look is None:
        raise ValueError(f'no character in slot {donor + 1}')
    if old is None:
        raise ValueError(f'no character in slot {target + 1}')
    if look == old:
        raise ValueError(f'{character.read(files[target]).name} already looks like this')
    return files[target], look, old


def copy(folder, donor, target, now=None, backup=True):
    """Gives the character in slot `target` the appearance of the one in slot `donor`. Returns the backup
    folder (backup=False: the caller made a backup, None). ValueError when there is nothing to copy."""
    folder = Path(folder)
    data, look, old = check(folder, donor, target)
    path = folder / f'userdata{target:04d}'
    who = character.read(data)
    # <user>/savedata/<user id>/<title>/SPRJ0005 -> <user>/appearance-backups/<time>/SPRJ0005
    stamp = time.strftime('%Y%m%d-%H%M%S', time.localtime(now))
    kept = folder.parents[3] / 'appearance-backups' / stamp / folder.name if backup else None
    if kept and not kept.exists():
        shutil.copytree(folder, kept)

    write(data, look)
    # Whole or not at all (a full drive, a crash halfway): a save cut off loses the character.
    textfile.write_bytes(path, bytes(data))
    # The game's backup of a slot file: only one that holds this character as it was last saved.
    for other in sorted(folder.glob('backup000[0-9]')):
        data = bytearray(other.read_bytes())
        same = character.read(data)
        if same and same.summary() == who.summary() and read(data) == old:
            write(data, look)
            textfile.write_bytes(other, bytes(data))
    # The Load Game menu's picture of the character: the profile's block for the slot, and its backup.
    for name in (PROFILE, 'backup0010'):
        path = folder / name
        if not path.is_file():
            continue
        profile = bytearray(path.read_bytes())
        face = profile_face(profile, target, who.name)
        if face is not None and profile[face:face + SIZE] == old[0]:
            profile[face:face + SIZE] = look[0]
            textfile.write_bytes(path, bytes(profile))
    return kept
