#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""A game folder whose eboot.bin is not the clean 1.09 one (a 60 FPS eboot, a patched one, one cut off) gets the
clean one back when it is still on this PC: the update folder it was copied from (shadPS4's CUSA03173-UPDATE), a
backup a patch tool left beside it, another installation, Downloads. A copy counts only when its loaded image is
the clean 1.09 one, byte for byte (CLEAN_IMAGE). The eboot.bin it replaces is kept as eboot.bin.modified.

Nothing of the game is in here: without a clean copy on the PC, the game folder stays as it is.

    python clean_eboot.py GAME_DIR      replaces eboot.bin when a clean copy is found"""
import hashlib
import os
import shutil
import sys
from pathlib import Path

CLEAN_IMAGE = '071df19c8880086d97182dbc057bc8cb37badaca57d9112683836b24a0444c0a'  # 1.09's loaded image (SHA-256)
SIZES = range(80 << 20, 120 << 20)  # the clean eboot.bin has 93,825,704 bytes; other dump tools wrap it a bit apart
BACKUP_NAMES = ('.bak', '.orig', '.original', '.backup', '.old', '.clean')
REPARSE_POINT = 0x400  # FILE_ATTRIBUTE_REPARSE_POINT: a junction (mod views link into the game folder)
LIMIT = 4000  # folders looked into at most


def image_hash(path):
    """SHA-256 of an eboot's loaded image (its loadable segments at their addresses, as run.py's eboot_image),
    None when it is not a readable PS4 executable."""
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    from prepare import parse_self, span
    try:
        elf, _header, ph, _segments, _missing = parse_self(Path(path).read_bytes())
        loads = [p for p in ph if p['type'] in (1, 0x61000010)]
        image = bytearray(max(p['vaddr'] + p['memsz'] for p in loads))
        for p in loads:
            image[p['vaddr']:p['vaddr'] + p['filesz']] = span(elf, p['offset'], p['filesz'])
    except (OSError, ValueError, KeyError, IndexError):
        return None
    return hashlib.sha256(image).hexdigest()


def is_clean(path):
    return image_hash(path) == CLEAN_IMAGE


def app_version(game_dir):
    """APP_VER of the game folder's param.sfo ('01.09'), None when unreadable. Game of the Year dumps say
    APP_VER 01.00 over the 1.09 code: patches.game_app_version counts them as 01.09."""
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    try:
        from patches import game_app_version
        return game_app_version(game_dir)
    except ImportError:
        return None


def _folders(top, depth):
    """top and its folders down to depth, without junctions, links or the port's own game views."""
    found, stack = [], [(Path(top), 0)]
    while stack and len(found) < LIMIT:
        folder, level = stack.pop()
        found.append(folder)
        if level >= depth:
            continue
        try:
            entries = list(os.scandir(folder))
        except OSError:
            continue
        for entry in entries:
            try:
                if not entry.is_dir() or entry.is_symlink() or \
                        getattr(entry.stat(follow_symlinks=False), 'st_file_attributes', 0) & REPARSE_POINT:
                    continue
            except OSError:
                continue
            name = entry.name.lower()
            if name.startswith(('mod-game-', '$recycle', 'dvdroot_ps4', 'sce_')) or name in ('out', 'windows'):
                continue
            stack.append((Path(entry.path), level + 1))
    return found


def shadps4_folders():
    """The game folders shadPS4's settings name (installDirs): players who came from shadPS4 keep the update there."""
    found = []
    for config in (Path(os.environ.get('APPDATA', '')) / 'shadPS4' / 'config.toml',):
        try:
            import tomllib
            data = tomllib.loads(config.read_text(encoding='utf-8'))
        except (OSError, ValueError, ImportError):
            continue
        dirs = (data.get('GUI') or {}).get('installDirs') or []
        found += [Path(d if isinstance(d, str) else d.get('path', '')) for d in dirs]
    return [d for d in found if str(d)]


def candidates(game_dir, places=None):
    """eboot files that may be a clean copy, nearest first: backups beside eboot.bin, the game folder's neighbours
    (the update it was copied from), shadPS4's game folders, Downloads, the port's neighbours."""
    game = Path(game_dir)
    if places is None:
        home = Path(os.environ.get('USERPROFILE') or Path.home())
        port = Path(__file__).resolve().parent.parent
        places = [(game.parent, 2)] + [(d, 2) for d in shadps4_folders()] + [(home / 'Downloads', 3),
                                                                            (port.parent, 2)]
    seen = set()
    try:
        with os.scandir(game) as listing:
            beside = [Path(entry.path) for entry in listing if entry.is_file()]
    except OSError:
        beside = []
    for path in beside:
        name = path.name.lower()
        if name != 'eboot.bin' and not name.startswith('eboot.bin.modified') and \
                (name.startswith('eboot') or name.endswith(BACKUP_NAMES)):
            yield path
    for top, depth in places:
        for folder in _folders(top, depth):
            path = folder / 'eboot.bin'
            try:
                key = os.path.normcase(str(path.resolve()))
            except OSError:
                continue
            if key in seen or folder == game:
                continue
            seen.add(key)
            if path.is_file():
                yield path


def find(game_dir, places=None):
    """The first clean 1.09 eboot among the candidates, or None. Only files of the right size are read."""
    for path in candidates(game_dir, places):
        try:
            if path.stat().st_size not in SIZES:
                continue
        except OSError:
            continue
        if is_clean(path):
            return path
    return None


def restore(game_dir, places=None):
    """eboot.bin put back from a clean copy: (source, backup) or None when eboot.bin is clean, missing elsewhere,
    or cannot be replaced. The old eboot.bin stays as eboot.bin.modified (…modified-2, … when that exists)."""
    game = Path(game_dir)
    eboot = game / 'eboot.bin'
    # Only a 1.09 game folder (its param.sfo): the base game 1.00 needs the whole update, not its eboot.bin.
    if not eboot.is_file() or app_version(game) != '01.09' or is_clean(eboot):
        return None
    source = find(game, places)
    if not source:
        return None
    backup = game / 'eboot.bin.modified'
    number = 2
    while backup.exists():
        backup, number = game / f'eboot.bin.modified-{number}', number + 1
    temporary = game / 'eboot.bin.restoring'
    try:
        shutil.copyfile(source, temporary)
        if not is_clean(temporary):  # a copy that went wrong never replaces anything
            temporary.unlink()
            return None
        if eboot.exists():
            os.replace(eboot, backup)
        os.replace(temporary, eboot)
    except OSError:
        try:
            temporary.unlink()
        except OSError:
            pass
        if not eboot.exists() and backup.exists():
            os.replace(backup, eboot)
        return None
    return source, backup if backup.exists() else None


def note(result):
    """The log line for a restore(): what was replaced, from where, and where the old one is."""
    source, backup = result
    return (f'Game files: eboot.bin was not the clean 1.09 one; the clean copy from {source.parent} replaced it'
            + (f', the old one is kept as {backup.name}' if backup else ''))


if __name__ == '__main__':
    done = restore(sys.argv[1]) if len(sys.argv) == 2 else None
    print(note(done) if done else 'eboot.bin: nothing to do, or no clean copy found')
