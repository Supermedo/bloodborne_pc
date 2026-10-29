#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The game files the port itself changes or reads at every start, by feature: one table for run.py (which
mods' files the port's layers are built on, and which feature goes off when one cannot take the change),
the launcher's Mods page (what a mod touches) and the tests (no layer writes a file that is not here).

Each feature names game paths (fnmatch patterns under the game folder, compared case-insensitively) and
what the port does with them:
- 'adds': it only adds entries (the menu texts get new ids): a mod's version always takes them.
- 'edits': it changes the file (the menus get rows, the Doll's script an entry, the camera table its field
  of view): done on a mod's version when a mod brings one; when that fails, the feature is off.
- 'reads': it reads the file (the weapons' requirements, the names for the Discord status): a mod's
  version is the one read.

A mod's files win over the game's, and the port's layers come last: they are built from the mods' files
(mods.py --sources), so a mod and the port both keep what they change."""
from collections import namedtuple
from fnmatch import fnmatchcase
from pathlib import Path, PurePosixPath

MENU_TEXTS = 'dvdroot_ps4/msg/*/menu.msgbnd.dcx'
ITEM_TEXTS = 'dvdroot_ps4/msg/*/item.msgbnd.dcx'
SYSTEM_MENU = 'dvdroot_ps4/menu/optionsetting.gfx'
TITLE_MENUS = ('dvdroot_ps4/menu/title.gfx', 'dvdroot_ps4/menu/title_dlc.gfx', 'dvdroot_ps4/menu/title_dlc_eu.gfx')
MENU_MOVIES = 'dvdroot_ps4/menu/*.gfx'
DOLL_SCRIPTS = 'dvdroot_ps4/script/talk/m21_00_00_00.talkesdbnd.dcx'
GAME_PARAMS = 'dvdroot_ps4/param/gameparam/gameparam.parambnd.dcx'

# key: bbport.ini's switch as well; layer: the folder in out/ it writes (None: it writes no game file);
# switch: the variable that turns its eboot patches off (patches.py), so no menu row is left without its
# text; title: for the log.
Feature = namedtuple('Feature', 'key layer switch title files')
FEATURES = (
    Feature('quit_to_desktop', 'quit_text', 'BB_QUIT_TO_DESKTOP', 'Quit to Desktop',
            ((MENU_TEXTS, 'adds'), (SYSTEM_MENU, 'edits')) + tuple((menu, 'edits') for menu in TITLE_MENUS)),
    Feature('pc_menu', 'quit_text', 'BB_PC_MENU', 'PC Settings in the System menu',
            ((MENU_TEXTS, 'adds'), (SYSTEM_MENU, 'edits'))),
    Feature('rebirth', 'rebirth', 'BB_REBIRTH', 'Rebirth at the Doll',
            ((MENU_TEXTS, 'adds'), (DOLL_SCRIPTS, 'edits'), (GAME_PARAMS, 'reads'))),
    Feature('fov', 'fov', None, 'Field of view', ((GAME_PARAMS, 'edits'),)),
    Feature('hud_layout', 'hud_layout', None, 'Ultrawide HUD layout', ((MENU_MOVIES, 'edits'),)),
    Feature('discord', None, None, 'Discord status', ((MENU_TEXTS, 'reads'), (ITEM_TEXTS, 'reads'))),
)
BY_KEY = {feature.key: feature for feature in FEATURES}
# The port's layers, in the order they are built and applied (each one above the one before: Rebirth's
# texts are built on Quit to Desktop's, the HUD layout's menus on Quit to Desktop's rows).
LAYERS = ('quit_text', 'rebirth', 'hud_layout', 'fov')


def game_path(relative):
    """A game path as the table compares it: '/'-separated and case-folded."""
    return PurePosixPath(*Path(relative).parts).as_posix().replace('\\', '/').casefold()


def matches(relative, pattern):
    return fnmatchcase(game_path(relative), pattern.casefold())


def uses(relative):
    """[(feature, how)] of the features that change or read this game path."""
    return [(feature, how) for feature in FEATURES for pattern, how in feature.files if matches(relative, pattern)]


def watched(relative):
    """Whether the port changes or reads this game path at all (the files mods.py --sources keeps)."""
    return bool(uses(relative))


def in_layer(layer, relative):
    """Whether `layer` may write this game path (the tests hold every layer to it)."""
    return any(feature.layer == layer and how != 'reads' and matches(relative, pattern)
               for feature in FEATURES for pattern, how in feature.files)


def find(folder, relative):
    """The file at the game path `relative` under `folder`, matched case-insensitively (mods made on
    Windows often spell the game's names otherwise), or None."""
    path = Path(folder)
    for part in Path(relative).parts:
        if not path.is_dir():
            return None
        exact = path / part
        if exact.exists():
            path = exact
            continue
        path = next((entry for entry in path.iterdir() if entry.name.casefold() == part.casefold()), None)
        if path is None:
            return None
    return path if path.is_file() else None


def source(game, relative, *layers):
    """The file the port builds on for the game path `relative`: the first of `layers` that has it (the
    port's own layer below, then the mods' files: mods.py --sources), else the game's."""
    for layer in layers:
        found = find(layer, relative) if layer else None
        if found:
            return found
    return Path(game) / relative


def from_layer(path, game):
    """Whether `path` (a source() result) is not the game's own file."""
    try:
        Path(path).resolve().relative_to(Path(game).resolve())
        return False
    except ValueError:
        return True
