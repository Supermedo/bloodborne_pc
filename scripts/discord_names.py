#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The names the Discord status shows (src/runtime_discord.c, BB_DISCORD_NAMES), in the game's own
text in its language: "p <id> <place>" from item.msgbnd FMG 19 (id = area * 100 + block * 10),
"b <id> <boss>" from FMG 18 (the NPC names the boss bar shows) and "l <the word Level>" from
menu.msgbnd FMG 200. English where the language's folder lacks them."""
import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import quit_menu  # noqa: E402

# The game's text language (BB_LANGUAGE, the PS4 system language) and its folder, as
# launcher/bbport_character.py MSG_FOLDERS.
MSG_FOLDERS = {'0': 'jpnjp', '1': 'engus', '2': 'frafr', '3': 'spaes', '4': 'deude', '5': 'itait', '8': 'rusru',
               '17': 'porbr'}
NPC_NAMES, PLACE_NAMES, MENU_TEXTS, LEVEL_TEXT = 18, 19, 200, 10200


def fmgs(path, wanted):
    bnd = quit_menu.unpack_dcx(Path(path).read_bytes())
    return {file_id: quit_menu.read_fmg(bnd[o:o + n]) for _e, o, n, file_id in quit_menu.bnd4_files(bnd)
            if file_id in wanted}


def names(game, language):
    msg = Path(game) / 'dvdroot_ps4' / 'msg'
    for folder in (MSG_FOLDERS.get(str(language), 'engus'), 'engus'):
        if (msg / folder / 'item.msgbnd.dcx').is_file() and (msg / folder / 'menu.msgbnd.dcx').is_file():
            break
    items = fmgs(msg / folder / 'item.msgbnd.dcx', {NPC_NAMES, PLACE_NAMES})
    menu = fmgs(msg / folder / 'menu.msgbnd.dcx', {MENU_TEXTS})
    lines = [f'p {k} {v.strip()}' for k, v in sorted(items.get(PLACE_NAMES, {}).items()) if v and v.strip()]
    lines += [f'b {k} {v.strip()}' for k, v in sorted(items.get(NPC_NAMES, {}).items()) if v and v.strip()]
    level = menu.get(MENU_TEXTS, {}).get(LEVEL_TEXT)
    if level:
        lines.append(f'l {level.strip()}')
    return [' '.join(line.split()) for line in lines], folder


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('game', type=Path, help='game folder (with dvdroot_ps4)')
    p.add_argument('--out', type=Path, required=True, help='the names file to write')
    p.add_argument('--language', default='1', help='BB_LANGUAGE (the game text language)')
    a = p.parse_args()
    lines, folder = names(a.game, a.language)
    a.out.write_text(''.join(line + '\n' for line in lines), encoding='utf-8')
    print(f'Discord status: {len(lines)} names ({folder})')


if __name__ == '__main__':
    main()
