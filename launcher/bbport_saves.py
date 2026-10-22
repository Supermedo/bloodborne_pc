# SPDX-License-Identifier: GPL-2.0-or-later
"""Bloodborne saves from other PC versions: finding them, telling what is in them and copying
them into this port's saves folder.

Every host keeps the game's own files (userdata0000-0009: the ten character slots, an unused one
all zeros; userdata0010: the profile; backup00xx: the game's copies) byte for byte; only the
folder and the metadata beside it differ (checked against their source, 2026-10):

  this port, Awaken's port   <user>/savedata/1/<save title>/SPRJ0005 + SPRJ0005.sce_sys/param.bin
  shadPS4 0.16 and later     %APPDATA%/shadPS4/home/<1000..>/savedata/<save title>/SPRJ0005/sce_sys/param.sfo
  shadPS4 0.3 to 0.15        %APPDATA%/shadPS4/savedata/1/<title>/SPRJ0005 (or a portable user/ folder)
  bbhost                     <data>/saves/SPRJ0005 (sce_param.bin inside), data in %LOCALAPPDATA%/bbhost
  Apollo Save Tool (PS4)     PS4/APOLLO/<user>_CUSA00207_SPRJ0005 (sce_sys/param.sfo)

This port reads one folder only: <saves folder>/savedata/1/<the dump's INSTALL_DIR_SAVEDATA or
TITLE_ID>/SPRJ0005, so a save is copied there whatever its folder was called. The game's files are
copied through a staging folder, after the current saves were packed into
<saves folder>/save_backups/; nothing is written to the source. Folders beside the save in
<save title>/ count as saves to the game, so nothing else is ever put there.
"""
import datetime
import glob
import json
import os
import re
import shutil
import struct
import zipfile
from pathlib import Path

import bbport_character as character

SAVE_DIR = 'SPRJ0005'
DEFAULT_TITLE = 'CUSA00207'  # what the Game of the Year Edition (CUSA03173) and the EU base game use
SLOTS = [f'userdata{i:04d}' for i in range(10)]
PROFILE = 'userdata0010'
SAVE_FILES = re.compile(r'^(userdata|backup)\d{4}$')
SKIP = {'dvdroot_ps4', 'sce_module', 'cache', 'shader', 'shader_cache', 'fsr4_shaders', '_internal', 'bin',
        'mods', 'patches', 'licenses', 'tools', 'scripts', 'out', 'node_modules', '.git', 'windows', 'program files',
        # a benchmark's own character (an old build's benchmark/ folder, the game tests' scenarios in
        # assets, recorded routes): not a player's save
        'benchmark', 'recordings', 'assets'}


# ---- where this port wants them --------------------------------------------------------------
def save_title(game_dir):
    """The title the dump's saves go under (INSTALL_DIR_SAVEDATA, else TITLE_ID), as bbport does."""
    try:
        from prepare import sfo
        values = sfo((Path(game_dir) / 'sce_sys' / 'param.sfo').read_bytes())
        title = str(values.get('INSTALL_DIR_SAVEDATA') or values.get('TITLE_ID') or '').strip()
        if re.fullmatch(r'[A-Z]{4}\d{5}', title):
            return title
    except (OSError, ValueError, ImportError, TypeError):
        pass
    return DEFAULT_TITLE


def target_dir(user_dir, game_dir=''):
    return Path(user_dir) / 'savedata' / '1' / save_title(game_dir) / SAVE_DIR


# ---- what a save folder holds -------------------------------------------------------------------
class SaveSet:
    """One save folder: where it is, which version it came from, and what it holds."""

    def __init__(self, source, folder):
        self.source, self.folder = source, Path(folder)
        self.files = sorted(p for p in self.folder.iterdir() if p.is_file() and SAVE_FILES.match(p.name))
        self.modified = max((p.stat().st_mtime for p in self.files), default=0)
        self.slots = [name for name in SLOTS if used(self.folder / name)]
        self.names = [n for n in (character_name(self.folder / slot) for slot in self.slots) if n]

    @property
    def complete(self):
        """The profile and at least one slot: without userdata0010 the game crashes on loading."""
        names = {p.name for p in self.files}
        return PROFILE in names and any(slot in names for slot in SLOTS)

    def when(self):
        return datetime.datetime.fromtimestamp(self.modified).strftime('%d.%m.%Y %H:%M') if self.modified else '?'


def used(path):
    """A character slot with anything in it (an unused slot is all zeros)."""
    try:
        with open(path, 'rb') as stream:
            return any(stream.read(4096))
    except OSError:
        return False


# A character's name in its slot: UTF-16 after eight zero bytes, ending in a zero character.
# Latin, Greek, Cyrillic, Japanese, Chinese and Korean letters; anything else is not a name.
NAME = re.compile('(?<=\x00\x00\x00\x00)([0-9A-Za-zÀ-ɏͰ-ӿ぀-ヿ一-鿿'
                  '가-힯 _\\-.\'!?]{2,16})(?=\x00)')


def character_name(slot):
    """The name of the character in a slot file (best effort; '' when not found)."""
    try:
        with open(slot, 'rb') as stream:
            data = stream.read(0x20000)
    except OSError:
        return ''
    return name_in(data)


def name_in(data):
    """The character's name in the first bytes of a slot ('' when not found)."""
    match = NAME.search(data.decode('utf-16-le', errors='replace'))
    if not match:
        return ''
    name = match.group(1)
    # Only the first candidate, and only when it looks like a name: better none than a wrong one.
    if name != name.strip() or len(name) < 3 or sum(ch.isalpha() for ch in name) < 2:
        return ''
    return name


def is_save_folder(folder):
    folder = Path(folder)
    return (folder / PROFILE).is_file() or (folder / SLOTS[0]).is_file()


# ---- finding them ----------------------------------------------------------------------------------
def _json(path):
    try:
        return json.loads(Path(path).read_text(encoding='utf-8'))
    except (OSError, ValueError):
        return {}


def _toml_value(path, section, key):
    """One string value of a small TOML file (no parser in the standard library of 3.10)."""
    try:
        text = Path(path).read_text(encoding='utf-8')
    except OSError:
        return ''
    current = ''
    for line in text.splitlines():
        line = line.strip()
        if line.startswith('['):
            current = line.strip('[]').strip()
        elif current == section and line.split('=')[0].strip() == key and '=' in line:
            return line.split('=', 1)[1].strip().strip('"\'')
    return ''


def label_for(folder):
    """Which version a save folder belongs to, from where it is."""
    parts = [p.lower() for p in Path(folder).parts]
    if 'saves' in parts and parts[-1] == SAVE_DIR.lower():
        return 'bbhost'
    if 'apollo' in parts:
        return 'PS4 (Apollo Save Tool)'
    if 'home' in parts and 'savedata' in parts:
        root = Path(*Path(folder).parts[:parts.index('home')])
        if root.name.lower() == 'shadps4':
            return 'shadPS4'
        while root.name.lower() in ('user', 'data') and root.parent != root:
            root = root.parent  # a portable shadPS4: <install>/DATA/user/home/...
        return f'shadPS4 ({root.name})'
    if 'savedata' in parts:
        root = Path(*Path(folder).parts[:parts.index('savedata')])
        if root.name.lower() == 'shadps4':
            return 'shadPS4'
        install = root.parent if root.name.lower() == 'user' else root
        if 'profiles' in parts:
            install = Path(*Path(folder).parts[:parts.index('profiles')])
        if any((install / name).exists() for name in ('Bloodborne Launcher.exe', 'BloodborneLauncher.exe')):
            profile = f' · {Path(folder).parts[parts.index("profiles") + 1]}' if 'profiles' in parts else ''
            return f"Awaken's port{profile}"
        if (install / 'run.py').is_file() or (install / 'Bloodborne.exe').is_file():
            return f'bbport ({install.name})'
        if (install / 'shadPS4.exe').is_file() or (install.parent / 'shadPS4.exe').is_file():
            return f'shadPS4 ({install.name})'
        return f'Backup ({install.name})'
    ancestor = next((q for q in Path(folder).parents
                     if q.name and not re.fullmatch(r'[A-Z]{4}\d{5}|\d+|savedata', q.name, re.I)), None)
    return f'Backup ({ancestor.name})' if ancestor else str(folder)


def sources(port_dir, game_dir=''):
    """Folder globs where other versions keep Bloodborne saves on this PC (see the module text)."""
    port_dir = Path(port_dir)
    appdata, local = os.environ.get('APPDATA', ''), os.environ.get('LOCALAPPDATA', '')
    globs = []
    if appdata:
        shad = Path(appdata) / 'shadPS4'
        globs += [shad / 'home' / '*' / 'savedata' / '*' / SAVE_DIR, shad / 'savedata' / '*' / '*' / SAVE_DIR]
        home = (_json(shad / 'config.json').get('General') or {}).get('home_dir')
        if home:
            globs.append(Path(home) / '*' / 'savedata' / '*' / SAVE_DIR)
        saved = _toml_value(shad / 'config.toml', 'GUI', 'saveDataPath')
        if saved:
            globs.append(Path(saved) / '*' / '*' / SAVE_DIR)
        # Awaken's launcher shares %APPDATA%/bbport-launcher/settings.json and adds port_dir.
        other = _json(Path(appdata) / 'bbport-launcher' / 'settings.json').get('port_dir')
        if other:
            globs += [Path(other) / 'user' / 'savedata' / '1' / '*' / SAVE_DIR,
                      Path(other) / 'profiles' / '*' / 'savedata' / '1' / '*' / SAVE_DIR]
        data = _toml_value(Path(appdata) / 'bbhost' / 'bbhost.toml', 'paths', 'data')
        if data:
            globs.append(Path(data) / 'saves' / SAVE_DIR)
    if local:
        globs.append(Path(local) / 'bbhost' / 'data' / 'saves' / SAVE_DIR)
    if game_dir:
        globs.append(Path(game_dir) / 'data' / 'saves' / SAVE_DIR)  # bbhost beside the dump
    return [str(g) for g in globs]


def walk(roots, depth=9, limit=30000):
    """SPRJ0005 save folders under `roots` (other installations beside this one), skipping the
    big folders of dumps and ports."""
    found, seen = [], 0
    level = [Path(r) for r in roots if r and Path(r).is_dir()]
    for _ in range(depth + 1):
        following = []
        for folder in level:
            try:
                children = [p for p in folder.iterdir() if p.is_dir()]
            except OSError:
                continue
            for child in children:
                seen += 1
                name = child.name.lower()
                if name == SAVE_DIR.lower() and is_save_folder(child):
                    found.append(child)
                elif name not in SKIP and not name.endswith('.sce_sys') and not name.startswith('sdimg_'):
                    following.append(child)
            if seen > limit:
                return found
        level = following
    return found


def scan(port_dir, game_dir='', exclude=()):
    """[SaveSet] of every complete save on this PC that is not this port's own, newest first."""
    seen = set()
    for path in exclude:
        try:
            seen.add(os.path.normcase(str(Path(path).resolve())))
        except OSError:
            pass
    folders = [Path(name) for pattern in sources(port_dir, game_dir) for name in glob.glob(pattern)]
    folders += walk([Path(port_dir).parent] + ([Path(game_dir).parent] if game_dir else []))
    sets = []
    for folder in folders:
        try:
            key = os.path.normcase(str(folder.resolve()))  # shadPS4 0.16 may leave symlinks behind
        except OSError:
            continue
        if key in seen or not folder.is_dir():
            continue
        seen.add(key)
        try:
            save = SaveSet(label_for(folder), folder)
        except OSError:
            continue
        if save.complete:
            sets.append(save)
    return sorted(sets, key=lambda s: -s.modified)


def find_in(root, depth=6):
    """Save folders at or under a folder the player picked: (found, encrypted ones seen)."""
    root = Path(root)
    if is_save_folder(root):
        return [root], []
    found, encrypted, level = [], [], [root]
    for _ in range(depth + 1):
        following = []
        for folder in level:
            try:
                children = [p for p in folder.iterdir() if p.is_dir()]
            except OSError:
                continue
            for child in children:
                if child.name.lower().startswith('sdimg_'):
                    encrypted.append(child)  # a console's own USB backup: encrypted, of no use here
                elif is_save_folder(child):
                    found.append(child)
                elif child.name.lower() not in SKIP:
                    following.append(child)
        level = following[:4000]
    return found, encrypted


# ---- copying ---------------------------------------------------------------------------------------
# ---- backups ---------------------------------------------------------------------------------------
# <saves folder>/save_backups/*.zip, each with the game's files as this port keeps them and
# bbport-backup.json: why it was made, a name the player gave it, when, and who is in it. The file
# name says the same ("2026-10-20 15.32 - Before playing - Nick, Maria.zip"), so a backup can be told
# apart in Explorer too. Backups from before (saves_<date>.zip) have no such file: 'old'.
BACKUP_DIR = 'save_backups'
BACKUP_META = 'bbport-backup.json'
REASONS = {'manual': 'Saved by hand', 'auto': 'Before playing', 'import': 'Before an import',
           'restore': 'Before a restore', 'respec': 'Before a respec', 'file': 'From a file',
           'old': 'Earlier backup'}
WORDS = dict(REASONS)  # the words for file names; the launcher puts its translations here
AUTO_KEEP = 10  # automatic backups kept; the player's own are never removed by the launcher
META_FILES = {'param.bin', 'icon0.png'}


def backup_dir(user_dir):
    return Path(user_dir) / BACKUP_DIR


def backup_file_name(created, reason, label='', detail='', names=()):
    """'2026-10-20 15.32 - Before playing - Nick, Maria.zip', safe for Windows, at most 120 characters."""
    words = label or WORDS.get(reason, REASONS.get(reason, reason))
    parts = [created.strftime('%Y-%m-%d %H.%M'), words, '' if label else detail, ', '.join(names)]
    text = ' - '.join(p.strip() for p in parts if p and p.strip())
    text = re.sub(r'[<>:"/\\|?*\x00-\x1f]', '_', text).strip(' .')
    return text[:116].rstrip(' .-') + '.zip'


def free_name(folder, name):
    """`name` in `folder`, with ' (2)', ' (3)' … when it is taken."""
    candidate, number = Path(folder) / name, 1
    while candidate.exists():
        number += 1
        candidate = Path(folder) / f'{name[:-4]} ({number}).zip'
    return candidate


def write_backup(target, files, meta):
    """A backup zip: `files` [(member name, path or bytes)] and the meta file, checked after writing."""
    staging = target.with_name(target.name + '.part')
    try:
        with zipfile.ZipFile(staging, 'w', zipfile.ZIP_DEFLATED) as zipped:
            for member, source in files:
                if isinstance(source, bytes):
                    zipped.writestr(member, source)
                else:
                    zipped.write(source, member)
            zipped.writestr(BACKUP_META, json.dumps(meta, ensure_ascii=False, indent=1))
        with zipfile.ZipFile(staging) as zipped:  # readable before anything relies on it
            if zipped.testzip() is not None:
                raise OSError(f'the backup {target} is damaged')
        os.replace(staging, target)
    finally:
        if staging.exists():
            staging.unlink()
    return target


def backup(user_dir, game_dir='', reason='manual', label='', detail=''):
    """Packs this port's current save (and its metadata) into save_backups/; None without one."""
    current = target_dir(user_dir, game_dir)
    parts = [p for p in (current, current.with_name(SAVE_DIR + '.sce_sys')) if p.is_dir()]
    if not parts or not any(current.glob('userdata*')):
        return None
    names = SaveSet('', current).names
    created = datetime.datetime.now()
    folder = backup_dir(user_dir)
    folder.mkdir(parents=True, exist_ok=True)
    root = Path(user_dir)
    files = [(p.relative_to(root).as_posix(), p) for part in parts for p in sorted(part.rglob('*')) if p.is_file()]
    meta = {'version': 1, 'reason': reason, 'label': label, 'detail': detail,
            'created': created.isoformat(timespec='seconds'), 'characters': names}
    return write_backup(free_name(folder, backup_file_name(created, reason, label, detail, names)), files, meta)


def archive_files(archive):
    """{'save': {file name: member}, 'meta': {file name: member}} of a backup zip, whatever folders the
    files were packed in. Only the game's own files count (userdata/backup, param.bin, icon0.png);
    raises OSError when it is no complete save or holds two."""
    found = {'save': {}, 'meta': {}}
    try:
        with zipfile.ZipFile(archive) as zipped:
            for info in zipped.infolist():
                if info.is_dir():
                    continue
                parts = info.filename.replace('\\', '/').split('/')
                name, parent = parts[-1], (parts[-2] if len(parts) > 1 else '')
                kind = 'meta' if parent.lower().endswith('sce_sys') else 'save'
                if (kind == 'save' and not SAVE_FILES.match(name)) or (kind == 'meta' and name not in META_FILES):
                    continue
                if name in found[kind]:
                    raise OSError('the file holds more than one save')
                found[kind][name] = info.filename
    except (zipfile.BadZipFile, OSError) as error:
        raise OSError(str(error) or 'not a zip file') from error
    names = set(found['save'])
    if PROFILE not in names or not any(slot in names for slot in SLOTS):
        raise OSError('no complete save: it needs userdata0010 and at least one character')
    return found


class Backup:
    """One zip in save_backups: why, when, who; `ok` is False for a file that is no usable backup."""

    def __init__(self, path):
        self.path = Path(path)
        try:
            self.size = self.path.stat().st_size
            self.modified = stamp = self.path.stat().st_mtime
        except OSError:
            self.size = self.modified = stamp = 0
        meta, names, self.ok = {}, [], True
        try:
            files = archive_files(self.path)
            with zipfile.ZipFile(self.path) as zipped:
                if BACKUP_META in zipped.namelist():
                    meta = json.loads(zipped.read(BACKUP_META).decode('utf-8'))
                if not meta.get('characters'):
                    for slot in SLOTS:
                        if slot in files['save']:
                            with zipped.open(files['save'][slot]) as stream:
                                data = stream.read(0x20000)
                            name = name_in(data) if any(data[:4096]) else ''
                            names += [name] if name else []
        except (OSError, ValueError, KeyError):
            self.ok = False
        self.reason = meta.get('reason') if meta.get('reason') in REASONS else 'old'
        self.label, self.detail = str(meta.get('label') or ''), str(meta.get('detail') or '')
        self.names = [str(n) for n in meta.get('characters') or names]
        self.created = None
        try:
            self.created = datetime.datetime.fromisoformat(meta['created'])
        except (KeyError, TypeError, ValueError):
            match = re.search(r'(\d{4}-\d{2}-\d{2})_(\d{6})', self.path.name)  # saves_2026-10-20_153200.zip
            if match:
                self.created = datetime.datetime.strptime(''.join(match.groups()), '%Y-%m-%d%H%M%S')
        if self.created is None:
            self.created = datetime.datetime.fromtimestamp(stamp)
        self.meta = meta


def characters(folder):
    """{slot: Character} of a save folder's readable characters."""
    found = {slot: character.read_file(Path(folder) / slot) for slot in SLOTS if used(Path(folder) / slot)}
    return {slot: info for slot, info in found.items() if info}


_IN_BACKUPS = {}  # (path, size, modified) -> {slot: Character}; a backup's file does not change


def backup_characters(item):
    """{slot: Character} of a backup's readable characters (each file read once)."""
    key = (str(item.path), item.size, item.modified)
    if key not in _IN_BACKUPS:
        found = {}
        try:
            members = archive_files(item.path)['save']
            with zipfile.ZipFile(item.path) as zipped:
                for slot in SLOTS:
                    if slot in members:
                        info = character.read(zipped.read(members[slot]))
                        if info:
                            found[slot] = info
        except (OSError, zipfile.BadZipFile, KeyError):
            pass
        _IN_BACKUPS[key] = found
    return _IN_BACKUPS[key]


def backups(user_dir):
    """The backups in save_backups, newest first."""
    folder = backup_dir(user_dir)
    found = [Backup(p) for p in folder.glob('*.zip')] if folder.is_dir() else []
    return sorted(found, key=lambda b: b.created, reverse=True)


def rename_backup(path, label):
    """Gives a backup the player's own name ('' gives back the reason's); returns its new path."""
    item = Backup(path)
    if not item.ok:
        raise OSError('not a usable backup')
    with zipfile.ZipFile(item.path) as zipped:
        files = [(i.filename, zipped.read(i.filename)) for i in zipped.infolist()
                 if not i.is_dir() and i.filename != BACKUP_META]
    meta = {**item.meta, 'version': 1, 'reason': item.reason, 'detail': item.detail,
            'created': item.created.isoformat(timespec='seconds'), 'characters': item.names, 'label': label.strip()}
    name = backup_file_name(item.created, item.reason, meta['label'], item.detail, item.names)
    target = item.path if name == item.path.name else free_name(item.path.parent, name)
    write_backup(target, files, meta)
    if target != item.path:
        item.path.unlink()
    return target


def latest_change(user_dir, game_dir=''):
    """When the game last wrote this port's save (0 without one)."""
    current = target_dir(user_dir, game_dir)
    try:
        return max((p.stat().st_mtime for p in current.iterdir() if p.is_file() and SAVE_FILES.match(p.name)),
                   default=0)
    except OSError:
        return 0


def auto_backup(user_dir, game_dir='', keep=AUTO_KEEP):
    """Before a start: a backup when the save changed since the newest backup; the oldest automatic
    ones beyond `keep` go. Returns the new backup or None."""
    changed = latest_change(user_dir, game_dir)
    if not changed:
        return None
    existing = backups(user_dir)
    if existing and existing[0].created.timestamp() >= int(changed):
        return None  # nothing played since
    made = backup(user_dir, game_dir, reason='auto')
    automatic = [b for b in backups(user_dir) if b.reason == 'auto']
    for old in automatic[keep:]:
        try:
            old.path.unlink()
        except OSError:
            pass
    return made


def add_backup_file(path, user_dir):
    """A backup zip from elsewhere (another PC, a friend's) into save_backups, as 'From a file' with
    its file name; only the game's files are taken. Returns the new path (OSError: not a save)."""
    files = archive_files(path)
    title = DEFAULT_TITLE
    with zipfile.ZipFile(path) as zipped:
        packed = [(f'savedata/1/{title}/{SAVE_DIR}/{name}', zipped.read(member)) for name, member in
                  sorted(files['save'].items())]
        packed += [(f'savedata/1/{title}/{SAVE_DIR}.sce_sys/{name}', zipped.read(member)) for name, member in
                   sorted(files['meta'].items())]
    names = []
    for member, data in packed:
        if member.rsplit('/', 1)[-1] in SLOTS and any(data[:4096]):
            name = name_in(data[:0x20000])
            names += [name] if name else []
    created = datetime.datetime.now()
    detail = Path(path).stem[:60]
    meta = {'version': 1, 'reason': 'file', 'label': '', 'detail': detail,
            'created': created.isoformat(timespec='seconds'), 'characters': names}
    folder = backup_dir(user_dir)
    folder.mkdir(parents=True, exist_ok=True)
    return write_backup(free_name(folder, backup_file_name(created, 'file', '', detail, names)), packed, meta)


def to_recycle_bin(path):
    """Moves a file to the Recycle Bin (Windows); True when it is gone from its folder."""
    path = Path(path)
    try:
        import ctypes
        from ctypes import wintypes

        class SHFILEOPSTRUCTW(ctypes.Structure):
            _fields_ = [('hwnd', wintypes.HWND), ('wFunc', wintypes.UINT), ('pFrom', wintypes.LPCWSTR),
                        ('pTo', wintypes.LPCWSTR), ('fFlags', ctypes.c_ushort), ('fAnyOperationsAborted', wintypes.BOOL),
                        ('hNameMappings', ctypes.c_void_p), ('lpszProgressTitle', wintypes.LPCWSTR)]
        # FO_DELETE; FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI
        operation = SHFILEOPSTRUCTW(None, 3, str(path.resolve()) + '\0', None, 0x40 | 0x10 | 0x4 | 0x400)
        ctypes.windll.shell32.SHFileOperationW(ctypes.byref(operation))
    except (AttributeError, OSError):
        return False
    return not path.exists()


def param_from_sfo(path):
    """This port's param.bin (SceSaveDataParam, 1328 bytes) from a shadPS4 / PS4 param.sfo."""
    try:
        from prepare import sfo
        values = sfo(Path(path).read_bytes())
    except (OSError, ValueError, ImportError):
        return None
    text = lambda key, size: str(values.get(key, '')).encode('utf-8')[:size - 1].ljust(size, b'\0')
    user_param = values.get('SAVEDATA_LIST_PARAM', 0)
    user_param = user_param if isinstance(user_param, int) else 0
    data = text('MAINTITLE', 128) + text('SUBTITLE', 128) + text('DETAIL', 1024) + struct.pack('<I', user_param)
    data += b'\0' * 4 + struct.pack('<q', int(Path(path).stat().st_mtime)) + b'\0' * 32
    return data if len(data) == 1328 else None


def import_set(save, user_dir, game_dir=''):
    """Copies `save` over this port's save (after a backup) and returns the backup's path."""
    if not save.complete:
        raise OSError('the save has no userdata0010 (the profile) or no character slot')
    user_dir = Path(user_dir)
    target = target_dir(user_dir, game_dir)
    archive = backup(user_dir, game_dir, reason='import', detail=save.source)
    staging = user_dir / 'save_import'  # outside savedata: the game takes every folder there for a save
    shutil.rmtree(staging, ignore_errors=True)
    staging.mkdir(parents=True)
    try:
        for path in save.files:
            copy = staging / path.name
            shutil.copyfile(path, copy)
            os.chmod(copy, 0o666)
            if copy.stat().st_size != path.stat().st_size:
                raise OSError(f'{path.name} was not copied completely')
        target.mkdir(parents=True, exist_ok=True)
        for old in target.iterdir():
            if old.is_file() and SAVE_FILES.match(old.name):
                os.chmod(old, 0o666)
                old.unlink()
        for copy in staging.iterdir():
            os.replace(copy, target / copy.name)
    finally:
        shutil.rmtree(staging, ignore_errors=True)
    meta = target.with_name(SAVE_DIR + '.sce_sys')
    meta.mkdir(exist_ok=True)  # the game's metadata writes fail silently without it
    source_meta = [save.folder / 'sce_sys', save.folder.with_name(save.folder.name + '.sce_sys'), save.folder]
    if not (meta / 'param.bin').is_file():
        for folder in source_meta:
            if (folder / 'param.bin').is_file() or (folder / 'sce_param.bin').is_file():  # bbport, bbhost
                shutil.copyfile(folder / ('param.bin' if (folder / 'param.bin').is_file() else 'sce_param.bin'),
                                meta / 'param.bin')
                break
            if (folder / 'param.sfo').is_file():  # shadPS4, Apollo
                data = param_from_sfo(folder / 'param.sfo')
                if data:
                    (meta / 'param.bin').write_bytes(data)
                break
    if not (meta / 'icon0.png').is_file():
        for folder in source_meta:
            if (folder / 'icon0.png').is_file():
                shutil.copyfile(folder / 'icon0.png', meta / 'icon0.png')
                break
    return archive


def restore(archive, user_dir, game_dir=''):
    """Puts a backup from save_backups back; the current save is backed up first ('Before a
    restore'), which is returned. Only the game's own files are taken from the zip."""
    files = archive_files(archive)
    item = Backup(archive)
    user_dir = Path(user_dir)
    before = backup(user_dir, game_dir, reason='restore',
                    detail=item.label or item.created.strftime('%Y-%m-%d %H.%M'))
    target = target_dir(user_dir, game_dir)
    meta = target.with_name(SAVE_DIR + '.sce_sys')
    staging = user_dir / 'save_import'  # outside savedata: the game takes every folder there for a save
    shutil.rmtree(staging, ignore_errors=True)
    staging.mkdir(parents=True)
    try:
        with zipfile.ZipFile(archive) as zipped:
            for kind in ('save', 'meta'):
                (staging / kind).mkdir()
                for name, member in files[kind].items():
                    data = zipped.read(member)
                    (staging / kind / name).write_bytes(data)
                    if (staging / kind / name).stat().st_size != len(data):
                        raise OSError(f'{name} was not written completely')
        target.mkdir(parents=True, exist_ok=True)
        for old in target.iterdir():
            if old.is_file() and SAVE_FILES.match(old.name):
                os.chmod(old, 0o666)
                old.unlink()
        for copy in (staging / 'save').iterdir():
            os.replace(copy, target / copy.name)
        meta.mkdir(exist_ok=True)  # the game's metadata writes fail silently without it
        for copy in (staging / 'meta').iterdir():
            os.replace(copy, meta / copy.name)
    finally:
        shutil.rmtree(staging, ignore_errors=True)
    return before


# ---- one character back -------------------------------------------------------------------------
# The profile holds a block of 0x192 bytes per slot from 0x1096, the character's name first: what the
# Load Game menu shows (level, play time, place, appearance, equipment). One character goes back with
# its slot file and its block; the other slots and the rest of the profile stay as they are.
PROFILE_SLOTS, PROFILE_SLOT_SIZE = 0x1096, 0x192


def profile_block(profile, slot):
    """(offset, bytes) of a slot's block in the profile."""
    start = PROFILE_SLOTS + SLOTS.index(slot) * PROFILE_SLOT_SIZE
    return start, bytes(profile[start:start + PROFILE_SLOT_SIZE])


def block_name(block):
    return block[:character.NAME_BYTES].decode('utf-16-le', errors='replace').split('\0')[0]


def restore_character(archive, slot, user_dir, game_dir=''):
    """Puts one character back as it is in a backup: its slot file and its block in the profile; the
    other characters stay. Only for the same character in the same slot, and only when the profile
    is laid out as expected (OSError otherwise). The current saves are backed up first ('Before a
    restore'), which is returned."""
    files = archive_files(archive)
    if slot not in files['save']:
        raise OSError(f'the backup has no {slot}')
    with zipfile.ZipFile(archive) as zipped:
        old_slot, old_profile = zipped.read(files['save'][slot]), zipped.read(files['save'][PROFILE])
    user_dir = Path(user_dir)
    target = target_dir(user_dir, game_dir)
    profile = bytearray((target / PROFILE).read_bytes())
    then, now = character.read(old_slot), character.read_file(target / slot)
    if not then or not now or then.name != now.name:
        raise OSError('it is not the same character in this slot')
    start, block = profile_block(old_profile, slot)
    current = block_name(profile_block(profile, slot)[1])
    if len(profile) != len(old_profile) or block_name(block) != then.name or current != now.name:
        raise OSError('the profile is not laid out as expected')
    item = Backup(archive)
    before = backup(user_dir, game_dir, reason='restore',
                    detail=item.label or item.created.strftime('%Y-%m-%d %H.%M'))
    profile[start:start + PROFILE_SLOT_SIZE] = block
    staging = user_dir / 'save_import'  # outside savedata: the game takes every folder there for a save
    shutil.rmtree(staging, ignore_errors=True)
    staging.mkdir(parents=True)
    try:
        for name, data in ((slot, old_slot), (PROFILE, bytes(profile))):
            (staging / name).write_bytes(data)
            if (staging / name).stat().st_size != len(data):
                raise OSError(f'{name} was not written completely')
        for name in (slot, PROFILE):
            os.chmod(target / name, 0o666)
            os.replace(staging / name, target / name)
    finally:
        shutil.rmtree(staging, ignore_errors=True)
    return before
