#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""BBSC's items outside Seamless co-op (run.py).

BBSC (Seamless co-op) gives a character its own kit: four copies of the game's bells and the
Silencing Blank in unused goods rows (Hunt Bell 4016, Kin Bell 4121, Bloodlust Bell 11201, Parting
Blank 11251) and a Duel Bell (4313), and takes the game's own out of the inventory. It names them
while it runs; without it the game has no text for those rows ("?GoodsName?" / "?GoodsInfo?") and
they do nothing, so the character has no working bells online. That happens when a character
played in Seamless co-op is played normally: a Saves folder pointing at Seamless co-op's saves, or
a character copied over.

texts: the five items' names and descriptions in every language's item texts (goods name 10,
info 20, caption 24 in msg/<language>/item.msgbnd.dcx), a layer below the mods. Only ids the
game's files leave empty are written.

repair: before a normal start, the four bells in the saves become the game's own again, in every
copy of each character (userdata and backup files, every place the item's handle or code is
stored, quick items included); a character that already has the game's item keeps BBSC's. The
Duel Bell has no counterpart and stays (named by the texts). The save folder is copied first."""
import argparse
import shutil
import struct
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import quit_menu  # noqa: E402

GOODS_NAME, GOODS_INFO, GOODS_CAPTION = 10, 20, 24
# BBSC's item: (the game's item it replaces, or None, name, info, caption).
KIT = {
    4016: (200, 'Hunt Bell', 'Seamless co-op bell. Outside it: put back as the Beckoning Bell.',
           "BBSC's bell for SuperMedo Seamless co-op. Ring it there to call other players into your game."),
    4121: (205, 'Kin Bell', 'Seamless co-op bell. Outside it: put back as the Small Resonant Bell.',
           "BBSC's bell for SuperMedo Seamless co-op. Ring it there to join another player's game."),
    11201: (225, 'Bloodlust Bell', 'Seamless co-op bell. Outside it: put back as the Sinister Resonant Bell.',
            "BBSC's bell for SuperMedo Seamless co-op. Ring it there to invade another player's world."),
    11251: (111, 'Parting Blank', 'Seamless co-op item. Outside it: put back as the Silencing Blank.',
            "BBSC's blank for SuperMedo Seamless co-op. Use it there to end co-op."),
    4313: (None, 'Duel Bell', 'Works only in SuperMedo Seamless co-op.',
           "BBSC's bell for SuperMedo Seamless co-op: rung there while hunting together, it turns on PvP "
           'between the players, and rung again turns it off. It does nothing outside Seamless co-op.'),
}
HANDLE, CODE = 0xB0000000, 0x40000000  # goods: an inventory entry's handle and item code
SLOT_FILES = [f'userdata{i:04d}' for i in range(10)] + [f'backup{i:04d}' for i in range(10)]
TEXTS_DONE = 'bbsc-items.done'


# ---- texts ------------------------------------------------------------------------------------
def item_texts(archive):
    """item.msgbnd.dcx with BBSC's item texts where the game has none, or None when nothing changes."""
    bnd = quit_menu.unpack_dcx(archive)
    files = {file_id: bnd[o:o + n] for _e, o, n, file_id in quit_menu.bnd4_files(bnd)}
    replace = {}
    for file_id, field in ((GOODS_NAME, 1), (GOODS_INFO, 2), (GOODS_CAPTION, 3)):
        if file_id not in files:
            continue
        strings = quit_menu.read_fmg(files[file_id])
        added = {item: KIT[item][field] for item in KIT if not strings.get(item)}
        if added:
            strings.update(added)
            replace[file_id] = quit_menu.write_fmg(files[file_id], strings)
    if not replace:
        return None
    return quit_menu.pack_dcx(archive, quit_menu.rebuild_bnd4(bnd, replace))


def write_texts(game, out):
    """Every language's item texts with BBSC's items; returns how many languages were written."""
    languages = [p for p in sorted((Path(game) / 'dvdroot_ps4' / 'msg').iterdir())
                 if (p / 'item.msgbnd.dcx').is_file()]
    written = 0
    for archive, folder in zip(quit_menu.in_parallel(lambda p: item_texts((p / 'item.msgbnd.dcx').read_bytes()),
                                                     languages), languages):
        if archive is None:
            continue
        target = Path(out) / 'dvdroot_ps4' / 'msg' / folder.name / 'item.msgbnd.dcx'
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(archive)
        written += 1
    return written


# ---- repair -----------------------------------------------------------------------------------
def _replace_u32(data, old, new):
    """Every 4-aligned u32 `old` in data set to `new`; how many."""
    count = 0
    old_bytes, new_bytes = struct.pack('<I', old), struct.pack('<I', new)
    at = data.find(old_bytes)
    while at != -1:
        if at % 4 == 0:
            data[at:at + 4] = new_bytes
            count += 1
        at = data.find(old_bytes, at + 1)
    return count


def repair_bytes(data):
    """The slot file's BBSC bells turned back into the game's; [(bbsc item, game item, places)]."""
    changes = []
    for item, (vanilla, *_texts) in KIT.items():
        if vanilla is None:
            continue
        handle, code = struct.pack('<I', HANDLE | item), struct.pack('<I', CODE | item)
        if handle + code not in data:
            continue  # no such inventory entry
        if struct.pack('<I', HANDLE | vanilla) + struct.pack('<I', CODE | vanilla) in data:
            continue  # the character has the game's item too: BBSC's stays (named)
        places = _replace_u32(data, HANDLE | item, HANDLE | vanilla) + _replace_u32(data, CODE | item, CODE | vanilla)
        changes.append((item, vanilla, places))
    return changes


def repair(save_dir, backups_dir, now=None):
    """Repairs every slot file in save_dir (a SPRJ0005 folder); returns ([(file, changes)], backup or None)."""
    save_dir = Path(save_dir)
    found = []
    for name in SLOT_FILES:
        path = save_dir / name
        if not path.is_file():
            continue
        data = bytearray(path.read_bytes())
        changes = repair_bytes(data)
        if changes:
            found.append((path, data, changes))
    if not found:
        return [], None
    backup = Path(backups_dir) / time.strftime('%Y%m%d-%H%M%S', time.localtime(now)) / save_dir.name
    shutil.copytree(save_dir, backup)
    for path, data, _changes in found:
        tmp = path.with_name(path.name + '.bbsc-repair')
        tmp.write_bytes(bytes(data))
        tmp.replace(path)
    return [(path.name, changes) for path, _data, changes in found], backup


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('game', nargs='?')
    parser.add_argument('--out', help='texts: the layer folder for item.msgbnd.dcx')
    parser.add_argument('--repair', help='repair: a SPRJ0005 save folder')
    parser.add_argument('--backups', help='repair: where the save folder is copied first')
    args = parser.parse_args()
    if args.repair:
        changed, backup = repair(args.repair, args.backups or Path(args.repair).parent / 'bbsc-repair-backups')
        for name, changes in changed:
            print(f'BBSC items: {name}: ' + ', '.join(f'{KIT[i][1]} back to the game\'s item {v} ({n} places)'
                                                    for i, v, n in changes))
        if backup:
            print(f'BBSC items: the saves were copied to {backup} first')
    if args.out:
        print(f'BBSC items: names in {write_texts(args.game, args.out)} languages')


if __name__ == '__main__':
    main()
