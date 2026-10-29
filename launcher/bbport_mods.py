# SPDX-License-Identifier: GPL-2.0-or-later
"""What the launcher tells about mods and the port (scripts/port_files.py): on the Mods page, under each
mod, which of the port's own changes its files touch and how they are combined, which files a later mod
takes from it, and what the last start had to turn off because of it (out/mod-report.json, run.py); on the
Play page, one line when a mod turned a feature off. All texts through the launcher's _()."""
import json
from pathlib import Path

L = None  # bbport_launcher_win, set by bind()


def bind(module):
    global L
    L = module


def _(*texts):
    return L._(*texts)


# The port's features as players know them (port_files.FEATURES' keys).
TITLES = {
    'quit_to_desktop': ('Quit to Desktop',),
    'pc_menu': ('PC Settings in the game menu',),
    'rebirth': ('Rebirth at the Doll',),
    'fov': ('Field of view',),
    'hud_layout': ('Ultrawide HUD layout',),
    'discord': ('Discord status',),
}


def title(key):
    return _(*TITLES.get(key, (key,)))


def report(data_dir):
    """out/mod-report.json of the last start: {feature: {'state', 'mod', 'file', 'why'}}."""
    try:
        found = json.loads((Path(data_dir) / 'out' / 'mod-report.json').read_text(encoding='utf-8'))
        return found.get('features', {}) if isinstance(found, dict) else {}
    except (OSError, ValueError):
        return {}


def touches(files):
    """The port's features a mod's game paths touch: {feature key: set of how ('adds', 'edits', 'reads')}."""
    import port_files
    found = {}
    for relative in files:
        for feature, how in port_files.uses(relative):
            found.setdefault(feature.key, set()).add(how)
    return found


def mod_paths(folder):
    """The case-folded game paths of a mod's files ([] for a folder that is not one)."""
    import mods
    try:
        return [relative.as_posix().casefold() for relative, _source in mods.mod_files(folder)]
    except (OSError, ValueError):
        return []


def notes(root, order, enabled, last=None):
    """{mod: [line]} for the Mods page. order: the mods as listed; enabled: the names switched on; last: the
    last start's report (report())."""
    paths = {name: mod_paths(Path(root) / name) for name in order}
    on = [name for name in order if name in enabled]
    lines = {}
    for name in order:
        found, said = touches(paths[name]), []
        if 'quit_to_desktop' in found or 'pc_menu' in found:
            if 'edits' in found.get('pc_menu', ()) or 'edits' in found.get('quit_to_desktop', ()):
                said.append(_('Changes the title or System menu: Quit to Desktop and PC Settings are fitted into '
                              'it, or stay off while this mod is on.'))
            else:
                said.append(_('Changes menu texts: the port adds its own (Quit to Desktop, PC Settings, Rebirth) '
                              'to them.'))
        if 'edits' in found.get('rebirth', ()):
            said.append(_("Changes the Doll's talk: Rebirth is fitted into it, or stays off while this mod is on."))
        if 'fov' in found:
            said.append(_('Changes game parameters: the field of view and Rebirth use its values.'))
        if 'hud_layout' in found and not ({'quit_to_desktop', 'pc_menu'} & set(found)):
            said.append(_('Changes menu screens: the ultrawide HUD layout is fitted to them.'))
        if name in on:
            mine = set(paths[name])
            for later in on[on.index(name) + 1:]:
                count = len(mine & set(paths[later]))
                if count:
                    said.append(_('{} (lower in the list) replaces {} of its files.').format(later, count))
            off = {}  # one line per file of this mod: what it turned off
            for key, entry in (last or {}).items():
                if entry.get('state') == 'off' and entry.get('mod') == name:
                    off.setdefault(Path(entry.get('file', '')).name, []).append(title(key))
            for file, features in off.items():
                said.append('⚠ ' + _("Off at the last start because of this mod's {}: {}.").format(
                    file, ', '.join(features)))
        if said:
            lines[name] = said
    return lines


def warning(last, enabled):
    """The Play page's line when a mod turned one of the port's features off at the last start, or ''."""
    for key, entry in (last or {}).items():
        if entry.get('state') == 'off' and entry.get('mod') in enabled:
            return '⚠ ' + _("Mod '{}': {} was off at the last start (click for why)").format(entry['mod'], title(key))
    return ''


def enabled_mods(app, data_dir):
    """The names of the mods a start loads (mods.py mod_layers), [] with mods off."""
    import mods
    if not app.get('mods_enabled', True):
        return []
    root = Path(app.get('mods_dir') or Path(data_dir) / 'mods')
    try:
        return [name for name, _folder in mods.mod_layers(app.get('game_dir') or '.', root,
                                                          Path(data_dir) / 'mods.json', True)]
    except (OSError, ValueError):
        return []
