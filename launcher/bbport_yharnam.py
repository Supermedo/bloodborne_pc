"""SuperMedo Seamless co-op: BBSC: Yharnam Together 1.0.0's co-op (github.com/CLSAMG22/BBSC, GPL-2.0) in this
port's own game, started by this launcher on supermedo.com's seamless server.

From 1.0.0 BBSC is a port of its own (a fork of this one from 1.6) with co-op built into its engine. Here the game is
this port's own: its seamless engine (bin\\bb-probe-seamless.exe: this port's engine built with BBSC's co-op and
network code, a program of its own; the normal bb-probe.exe and normal online never change), started by this port's
own run.py with every setting of this launcher (game_environment). From BBSC's release (the `seamless` folder,
scripts/fetch_yharnam.py) only its event tool and reference are used. Before a co-op start:

- a view of the player's game folder (folder links and hard links, nothing copied) with the 1.09 update over it
  when it is dumped beside the game, in seamless\\coop-game\\CUSA03173 (or under %LOCALAPPDATA% where folder links
  cannot go);
- the co-op event files made from the player's own game by BBSC's tools\\party_boss_events.py, in
  coop-game\\CUSA03173-mods (this port's mods.py layers a game's "-mods" folder over it);
- the game's own server pointed at the seamless server's backend (user\\host_overrides.custom.json);
- the seamless server with the SuperMedo account and the host rules chosen on the Seamless co-op page.

Only SuperMedo's server is used: its sign-in checks SuperMedo accounts, and the party made in this launcher is
the game's co-op team there. Its characters stay in seamless\\user\\savedata: co-op changes the character it plays,
and the normal saves must never see that.
"""
import json
import os
from pathlib import Path
import re
import shutil
import socket
import subprocess
import sys

SERVER = 'seamless.supermedo.com'
FOLDER_NAME = 'seamless'
VERSION = '1.0.0'
GAME_VERSION = '01.09'
TITLE = 'CUSA03173'
# BBSC 1.0.0's fixed ports (bbport_coop.py): the link (UDP), the WebAPI and the game backend.
LINK_PORT, WEBAPI_PORT, BACKEND_PORT = 31314, 31315, 20443
GAME_SERVER = 'https://ss4.scej-network.jp:20443'  # the game's own server, sent to the backend
NO_WINDOW = getattr(subprocess, 'CREATE_NO_WINDOW', 0)
ENGINE = 'bb-probe-seamless.exe'

# The enemies (SHADPS4_BLOODBORNE_HARDER_ENEMIES: HP, damage and poise multipliers, BBSC's presets) and 'auto'
# (BBSC's host rule 'scale': stronger with each hunter on the same map, up to Harder for a full team).
ENEMIES = {'auto': None, 'off': 'off', 'bit': '1.5,1.25,1.5', 'on': '3,1.5,3', 'much': '5,2,5'}
BOSS_TEMPOS = ('', 'aggressive', 'relentless')
# This launcher's settings for it (APP_DEFAULTS): BBSC's own defaults.
DEFAULTS = {'yt_enemies': 'off', 'yt_rot': False, 'yt_travel': False, 'yt_vote': False, 'yt_tempo': '',
            # Custom enemies (testers, 2026-11-06): the player's own multipliers, 1 to 10 each (the engine's range).
            'yt_custom_hp': '2', 'yt_custom_damage': '1.5', 'yt_custom_poise': '2',
            # Revival (testers, 2026-11-06): off is the engine's 'norevive' host rule, a fallen hunter dies for real.
            'yt_revive': True}
CUSTOM_ENEMIES = ('yt_custom_hp', 'yt_custom_damage', 'yt_custom_poise')


def custom_enemies(settings):
    """SHADPS4_BLOODBORNE_HARDER_ENEMIES for Custom: the three multipliers, each kept within 1 to 10."""
    values = []
    for key in CUSTOM_ENEMIES:
        try:
            value = float(str(settings.get(key, DEFAULTS[key])).strip().replace(',', '.'))
        except ValueError:
            value = float(DEFAULTS[key])
        value = min(10.0, max(1.0, value))
        values.append(f'{value:g}')
    return ','.join(values)

# The game's log line for a server key it trusted on first use (BBSC's bbport_coop.py LEARNED_KEY_LINE).
LEARNED_KEY_LINE = "Server link: the server's key, trusted on first use: "
OTHER_KEY_LINE = 'Server link: the server says another key than the one pinned'
REFUSED_LINE = 'Server link: the server refused this game: '

VIEW_MARK = '.yharnam-view'  # BBSC's own mark: a view either launcher made is known as one
# What the launcher makes in seamless\ for the player (BBSC's characters and logs, the co-op game view of links into
# the game, the co-op events, the picture settings): never shipped, and a package or an update never replaces it.
PLAYER_FILES = ('user', 'coop-game', 'event-patch', 'bbport.ini', 'mods-view', 'mods.json')


def folder(port_dir):
    return Path(port_dir) / FOLDER_NAME


def engine(port_dir):
    """This port's engine with BBSC's co-op (its own build, beside the normal bb-probe.exe, which it never
    changes)."""
    return Path(port_dir) / 'bin' / ENGINE


def event_tool(port_dir):
    return folder(port_dir) / 'tools' / 'party_boss_events.py'


def user_dir(port_dir):
    return folder(port_dir) / 'user'


def available(port_dir):
    """The seamless engine and BBSC's event tool are beside this launcher."""
    return engine(port_dir).is_file() and event_tool(port_dir).is_file()


# --- what this launcher keeps for it -------------------------------------------------------------------------

def state_path(port_dir):
    return user_dir(port_dir) / 'supermedo.json'


def load_state(port_dir):
    try:
        state = json.loads(state_path(port_dir).read_text(encoding='utf-8'))
    except (OSError, ValueError):
        return {}
    return state if isinstance(state, dict) else {}


def save_state(port_dir, state):
    path = state_path(port_dir)
    path.parent.mkdir(parents=True, exist_ok=True)
    part = path.with_suffix('.part')
    part.write_text(json.dumps(state, indent=2) + '\n', encoding='utf-8')
    part.replace(path)


def valid_link_key(value):
    return isinstance(value, str) and re.fullmatch(r'[0-9a-f]{64}', value) is not None


def link_key(port_dir, host=SERVER):
    """The server key kept for `host` (learned at the first link), '' when none: the game trusts the first one."""
    key = (load_state(port_dir).get('server_keys') or {}).get(host.lower(), '')
    return key if valid_link_key(key) else ''


def learned_key(line):
    """The key a line of the game's own log says it trusted on first use, or None (only the game's line: its level,
    file:line and function before the words, never one carried inside other text)."""
    found = re.search(r'(?:^|\] )<Info> (?:\([^()]*\) )?\S+:\d+ \w+: ' + re.escape(LEARNED_KEY_LINE) +
                      r'([0-9a-f]{64})\s*$', line)
    return found.group(1) if found else None


def keep_link_key(port_dir, key, host=SERVER):
    """Keeps the game's first-use key for `host`; a key already kept is never replaced from the log. True when kept."""
    if not valid_link_key(key) or link_key(port_dir, host):
        return False
    state = load_state(port_dir)
    keys = dict(state.get('server_keys') or {})
    keys[host.lower()] = key
    state['server_keys'] = keys
    save_state(port_dir, state)
    return True


def learn_from_log(port_dir, host=SERVER):
    """The first-use key the last run's game output (user\\log\\console.txt; BBSC's own engine wrote
    shad_log.txt) names, kept for `host` (keep_link_key): its later starts pin it. True when one was kept."""
    for name in ('console.txt', 'shad_log.txt'):
        try:
            text = (user_dir(port_dir) / 'log' / name).read_text(encoding='utf-8', errors='replace')
        except OSError:
            continue
        for line in text.splitlines():
            if LEARNED_KEY_LINE in line:
                key = learned_key(line)
                if key:
                    return keep_link_key(port_dir, key, host)
    return False


# --- the game view -------------------------------------------------------------------------------------------

def is_junction(path):
    path = Path(path)
    try:
        return path.is_junction() if hasattr(path, 'is_junction') else bool(os.lstat(path).st_file_attributes & 0x400)
    except (OSError, AttributeError):
        return False


def is_link(path):
    return is_junction(path) or Path(path).is_symlink()


def make_junction(link, target):
    if sys.platform == 'win32':
        import _winapi
        _winapi.CreateJunction(str(target), str(link))
    else:
        os.symlink(target, link, target_is_directory=True)


def remove_link(path):
    """A link goes as a link, never what it points at."""
    os.rmdir(path) if is_junction(path) else Path(path).unlink()


def link_file(link, target):
    try:
        os.link(target, link)
    except OSError:  # another drive, or a file system without hard links
        shutil.copy2(target, link)


def remove_view(path):
    """Removes a view made of links. Links go as links (never what they point at); a plain file goes only in a view
    one of the launchers marked (its hard links and copies of the game's files) or when another hard link keeps its
    data. Anything else stays, and so does the folder holding it."""
    path = Path(path)
    if is_link(path):
        remove_link(path)
        return
    if path.exists():
        _remove_view(path, (path / VIEW_MARK).is_file())


def _remove_view(path, ours):
    for entry in list(path.iterdir()):
        if is_link(entry):
            remove_link(entry)
        elif entry.is_dir():
            _remove_view(entry, ours)
        elif ours or entry.stat().st_nlink > 1:
            entry.unlink()
    try:
        path.rmdir()
    except OSError:
        if ours or not any(path.iterdir()):
            raise



def clear_program(path):
    """seamless\\ emptied of BBSC's program before a newer one is copied in (the package's refresh of
    dist\\bbport-windows): PLAYER_FILES stay, and a link goes as a link, never what it points at (rm -rf walked into
    the game through a junction once)."""
    path = Path(path)
    if is_link(path) or not path.is_dir():
        return
    for entry in list(path.iterdir()):
        if entry.name.lower() in PLAYER_FILES:
            continue
        if is_link(entry):
            remove_link(entry)
        elif entry.is_dir():
            shutil.rmtree(entry)  # Python 3.8 and newer remove a junction inside as a link
        else:
            entry.unlink()

def overlaps(one, other):
    try:
        one, other = Path(one).resolve(), Path(other).resolve()
    except OSError:
        return False
    return one == other or one.is_relative_to(other) or other.is_relative_to(one)


def build_view(game, update, view):
    """`view`: the game with `update` over it. A folder only one of them has is a link to it, a folder both have is
    merged, a file is the update's when it has one. The player's folders are never changed."""
    view, game = Path(view), Path(game)
    update = Path(update) if update else None
    for source in (game, update):
        if source and overlaps(source, view):
            raise RuntimeError(f'The game folder {source} is inside the Seamless co-op game folder {view} (or holds '
                               'it): choose your own game folder on the Game files page.')
    if view.exists() or is_link(view):
        remove_view(view)
    if view.exists():
        raise RuntimeError(f'{view} holds files that are not links to your game: move them away and press Play '
                           'again.')
    _merge(game, update, view)
    (view / VIEW_MARK).write_text('Made by the launcher from links to your game; it removes it again.\n',
                                  encoding='utf-8')


def _merge(game, update, view):
    view.mkdir(parents=True, exist_ok=True)
    entries = {}
    for entry in game.iterdir():
        entries[entry.name.lower()] = [entry, None]
    if update and update.is_dir():
        for entry in update.iterdir():
            entries.setdefault(entry.name.lower(), [None, None])[1] = entry
    for own, newer in entries.values():
        if own and newer and own.is_dir() and newer.is_dir():
            _merge(own, newer, view / own.name)
            continue
        entry = newer or own
        if entry.is_dir():
            make_junction(view / entry.name, entry)
        else:
            link_file(view / entry.name, entry)


def in_onedrive(path):
    try:
        path = Path(path).resolve()
    except OSError:
        return False
    for name in ('OneDrive', 'OneDriveConsumer', 'OneDriveCommercial'):
        root = os.environ.get(name)
        if root:
            try:
                if path.is_relative_to(Path(root).resolve()):
                    return True
            except OSError:
                continue
    return False


def junctions_work(path):
    path = Path(path)
    probe = path / '.junction-probe'
    try:
        path.mkdir(parents=True, exist_ok=True)
        if is_junction(probe):
            os.rmdir(probe)
        make_junction(probe, path)
        os.rmdir(probe)
        return True
    except OSError:
        return False


def view_root(port_dir):
    """Where the co-op game view goes: seamless\\coop-game, or under %LOCALAPPDATA% when that drive cannot hold
    folder links (exFAT, FAT32) or the launcher is in a OneDrive folder (it syncs a hard link as the game's own
    file)."""
    here = folder(port_dir) / 'coop-game'
    if not in_onedrive(port_dir) and junctions_work(here):
        return here
    return Path(os.environ.get('LOCALAPPDATA', Path.home())) / 'SuperMedo' / 'seamless-game'


def update_folder(game):
    """The update dumped beside the game (CUSA03173-patch), if any."""
    game = Path(game)
    for candidate in (game.parent / f'{game.name}-patch', game.parent / f'{TITLE}-patch'):
        if (candidate / 'eboot.bin').is_file() and not overlaps(candidate, game):
            return candidate
    return None


# --- the co-op event files -----------------------------------------------------------------------------------

def script_command(script, *args):
    """A Python script: this launcher's own Bloodborne.exe --script when frozen, else Python."""
    if getattr(sys, 'frozen', False):
        return [sys.executable, '--script', str(script), *map(str, args)]
    return [sys.executable, str(script), *map(str, args)]


def file_sha256(path):
    import hashlib
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def events_current(port_dir):
    """The event files were made by this release's event tool (its manifest names the tool's hash)."""
    manifest = folder(port_dir) / 'event-patch' / 'manifest.json'
    try:
        made = json.loads(manifest.read_text(encoding='utf-8'))
        return made.get('tool_sha256') == file_sha256(event_tool(port_dir))
    except (OSError, ValueError):
        return False


def prepared_key(game_dir):
    return json.dumps([str(Path(game_dir)), VERSION])


def needs_prepare(port_dir, game_dir):
    root = view_root(port_dir)
    return (load_state(port_dir).get('prepared') != prepared_key(game_dir)
            or not (root / TITLE / 'eboot.bin').is_file() or not events_current(port_dir))


def event_differences(made, reference):
    """The event files made from other game files than the ones this release was tested with: every hunter in a
    team needs the same ones. The game files count, not the files made: BBSC's reference was made by its own
    launcher's Python, whose compression gives other bytes for the same events (2026-11-05: the same 18 inputs,
    18 other outputs); every player here makes them with this launcher's."""
    mine, tested = made.get('files', {}), reference.get('files', {})
    return [name for name in sorted(set(mine) | set(tested))
            if name not in mine or name not in tested
            or mine[name].get('input_sha256') != tested[name].get('input_sha256')]


def prepare(port_dir, game_dir, version_of, say=print, call=subprocess.call):
    """The game view, the co-op event files from the player's own game, and the mods folder with them. `version_of`:
    a game folder's version ('01.09'), None when unknown. Returns (view root, a note for the player or '');
    raises RuntimeError with a message for the player."""
    base = folder(port_dir)
    game = Path(game_dir or '.')
    if not (game / 'eboot.bin').is_file():
        raise RuntimeError(f'No Bloodborne game folder (with eboot.bin) at {game}: choose it on the Game files page.')
    update = None
    if version_of(game) != GAME_VERSION:
        update = update_folder(game)
        if not update or version_of(update) != GAME_VERSION:
            raise RuntimeError(f'Your game is version {version_of(game) or "?"}; Seamless co-op needs the 1.09 update '
                               f'over it. Put the dumped update folder next to the game ({TITLE}-patch).')
    root = view_root(port_dir)
    say('Seamless co-op: preparing the game folder (links to your game, nothing is copied)…')
    view = root / TITLE
    build_view(game, update, view)
    if version_of(view) != GAME_VERSION:
        raise RuntimeError(f'The game is version {version_of(view) or "?"} with the update over it; Seamless co-op '
                           'needs 1.09.')
    game_events = view / 'dvdroot_ps4' / 'event'
    if not (game_events / 'common.emevd.dcx').is_file():
        raise RuntimeError('No dvdroot_ps4\\event\\common.emevd.dcx in the game folder: is it a complete dump?')
    say('Seamless co-op: making the co-op event files from your game…')
    events = base / 'event-patch'
    if events.exists():
        shutil.rmtree(events)
    log = user_dir(port_dir) / 'log' / 'event-patch.txt'
    log.parent.mkdir(parents=True, exist_ok=True)
    with open(log, 'w', encoding='utf-8') as out:
        code = call(script_command(event_tool(port_dir), events, game_events), stdout=out, stderr=subprocess.STDOUT,
                    stdin=subprocess.DEVNULL, creationflags=NO_WINDOW, cwd=str(base), env=script_environment())
    manifest = events / 'manifest.json'
    if code != 0 or not manifest.is_file():
        raise RuntimeError(f'Making the co-op event files failed (log: {log}).')
    note = ''
    reference = base / 'event-reference.json'
    if reference.is_file():
        differ = event_differences(json.loads(manifest.read_text(encoding='utf-8')),
                                   json.loads(reference.read_text(encoding='utf-8')))
        if differ:
            note = ('Your event files differ from the tested ones (' + ', '.join(differ) + '): another game version '
                    'or edition. Co-op may still work if every hunter has the same files.')
    mods = root / f'{TITLE}-mods'
    if mods.exists() or is_link(mods):
        remove_view(mods)
    copy_events(port_dir, root)
    state = load_state(port_dir)
    state['prepared'] = prepared_key(game_dir)
    save_state(port_dir, state)
    return root, note


def copy_events(port_dir, root):
    """The co-op event files into the mods folder beside the game view (every start: a newer release brings newer
    ones)."""
    target = Path(root) / f'{TITLE}-mods' / 'dvdroot_ps4' / 'event'
    target.mkdir(parents=True, exist_ok=True)
    (Path(root) / f'{TITLE}-mods' / VIEW_MARK).write_text('The co-op event files, made by the launcher.\n',
                                                          encoding='utf-8')
    for file in (folder(port_dir) / 'event-patch').glob('*'):
        if file.is_file():
            shutil.copy2(file, target / file.name)


# --- the mods: the player's, and BBSC's party HUD over them -----------------------------------------------------

MODS_VIEW = 'mods-view'
PARTY_HUD = 'zz Seamless co-op party HUD'  # a mod of its own, always the last one
HUD_FILE = ('dvdroot_ps4', 'menu', 'fe.gfx')


def hud_patcher(port_dir):
    """BBSC's scripts\\hud_patch.py (its party HUD: the party list and party death's YOU ARE DOWN banner in the
    game's own HUD movie), loaded from the seamless folder; None without it."""
    path = folder(port_dir) / 'scripts' / 'hud_patch.py'
    if not path.is_file():
        return None
    import importlib.util
    spec = importlib.util.spec_from_file_location('bbsc_hud_patch', path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def chosen_mods(mods_dir, mods_config, enabled):
    """(name, folder) of the player's mods as the normal game layers them (scripts/mods.py's choice and order);
    none when mods are off."""
    if not enabled:
        return []
    import mods
    try:
        return [(name, Path(mods_dir) / name) for name in mods.selected(Path(mods_dir), Path(mods_config))]
    except (OSError, ValueError):
        return []


def hud_source(game, layers):
    """The fe.gfx the game shows with these mods: the last one that has one, else the game's own."""
    import mods
    found = Path(game).joinpath(*HUD_FILE)
    for _name, root in layers:
        try:
            for relative, source in mods.mod_files(root):
                if relative.as_posix().casefold() == '/'.join(HUD_FILE).casefold():
                    found = source
        except (OSError, ValueError):
            continue
    return found


def mods_view(port_dir, game, mods_dir, mods_config, enabled):
    """seamless\\mods-view: the player's chosen mods as folder links (nothing copied, their mods folder is only
    read) and the party HUD made from the HUD they show, as the last mod; seamless\\mods.json gives that order.
    Returns (mods folder, mods.json, a note for the player or ''). The party HUD is left out, with a note, when it
    cannot be made: nothing here stops the game from starting."""
    import struct
    view = folder(port_dir) / MODS_VIEW
    view.mkdir(parents=True, exist_ok=True)
    for entry in list(view.iterdir()):  # the last start's links go as links, never what they point at
        if is_link(entry):
            remove_link(entry)
    names = []
    chosen = chosen_mods(mods_dir, mods_config, enabled)
    for name, source in chosen:
        try:
            make_junction(view / name, Path(source).resolve())
            names.append(name)
        except OSError:
            pass
    note = ''
    party = view / PARTY_HUD
    try:
        patcher = hud_patcher(port_dir)
        if patcher is None:
            raise OSError('scripts\\hud_patch.py is missing')
        data = hud_source(game, chosen).read_bytes()
        strip = patcher.HUD_ICONS.read_bytes() if patcher.HUD_ICONS.is_file() else None
        import hashlib
        digest = hashlib.sha256(patcher.PATCH_VERSION + data + (strip or b'')).hexdigest()
        target = party.joinpath(*HUD_FILE)
        stamp = party / 'made-from.txt'
        if not (target.is_file() and stamp.is_file() and stamp.read_text(encoding='utf-8').strip() == digest):
            patched = patcher.patch_fe(data, strip)
            target.parent.mkdir(parents=True, exist_ok=True)
            partial = target.with_suffix('.part')
            partial.write_bytes(patched)
            partial.replace(target)
            stamp.write_text(digest + '\n', encoding='utf-8')
        names.append(PARTY_HUD)
    except (OSError, ValueError, IndexError, AttributeError, struct.error) as error:
        note = f'Seamless co-op: the party HUD is off for this run ({error}).'
    config = folder(port_dir) / 'mods.json'
    config.write_text(json.dumps({'order': names, 'disabled': [] if PARTY_HUD in names else [PARTY_HUD]},
                                 indent=2) + '\n', encoding='utf-8')
    return view, config, note


# --- the start -------------------------------------------------------------------------------------------------

def resolve(host=SERVER, lookup=socket.gethostbyname):
    """The server's IP address (the game links to an address, not a name); RuntimeError when it is not found."""
    try:
        return lookup(host)
    except OSError:
        raise RuntimeError(f"Can't find the server {host}: check your internet connection.") from None


def write_host_overrides(port_dir, ip):
    """user\\host_overrides.custom.json: the game's own server is the seamless server's backend."""
    path = user_dir(port_dir) / 'host_overrides.custom.json'
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps({GAME_SERVER: f'http://{ip}:{BACKEND_PORT}'}, indent=2) + '\n', encoding='utf-8')
    return path


def host_rules(settings):
    """SHADPS4_BLOODBORNE_HOST_RULES (BBSC's ParseHostRules): the rules on, 'off' for none. The vote is a part of
    travelling together."""
    words = []
    if settings.get('yt_rot'):
        words.append('rot')
    if settings.get('yt_travel'):
        words.append('travel')
        if settings.get('yt_vote'):
            words.append('vote')
    if settings.get('yt_enemies') == 'auto':
        words.append('scale')
    if settings.get('yt_tempo') in BOSS_TEMPOS[1:]:
        words.append(settings['yt_tempo'])
    if not settings.get('yt_revive', True):
        words.append('norevive')
    return ','.join(words) or 'off'


# The normal online game's settings in game_environment (its server, account, network backend and engine): the
# seamless game has its own.
NORMAL_ONLINE = ('BB_ONLINE', 'BB_NET_BACKEND', 'BB_COMMUNITY_SERVER', 'BB_BRIDGE_LIVE_MAP', 'BB_UPNP', 'BB_PROBE')
NORMAL_ONLINE_PREFIXES = ('BB_SHADNET_', 'BB_THD_', 'BBSC_', 'SHADPS4_')


def script_environment(base=None):
    """The environment a BBSC script (the event tool) runs in: none of this launcher's game settings."""
    env = {key: value for key, value in (os.environ if base is None else base).items()
           if not key.startswith(('BB_', 'BBSC_', 'SHADPS4_'))}
    env['PYTHONUNBUFFERED'] = '1'
    env['PYTHONIOENCODING'] = 'utf-8'
    return env


def environment(port_dir, root, settings, ip, key='', game_env=None, mods=None):
    """The seamless game's environment: this launcher's own game start (game_environment: the player's game,
    graphics, controls, mods and patches, every setting of the launcher) with only these changes: the seamless
    engine (bin\\bb-probe-seamless.exe, this port's engine with BBSC's co-op), the co-op game view (its -mods folder
    brings the co-op events), the seamless characters' own saves (seamless\\user), and the
    seamless server with the SuperMedo account. `game_env`: game_environment's result; `mods`: mods_view's
    (folder, mods.json)."""
    env = {name: value for name, value in (os.environ if game_env is None else game_env).items()
           if name not in NORMAL_ONLINE and not name.startswith(NORMAL_ONLINE_PREFIXES)}
    env.update({
        'BB_PROBE': str(engine(port_dir)),
        'BB_GAME_DIR': str(Path(root) / TITLE),
        'BB_USER_DIR': str(user_dir(port_dir)),
        'BB_RELEASE_VERSION': VERSION,
        'BB_ONLINE': '1',
        'BB_SHADNET_SERVER': f'{ip}:{LINK_PORT}', 'BB_SHADNET_WEBAPI': f'http://{ip}:{WEBAPI_PORT}',
        'BB_SHADNET_NPID': str(settings.get('supermedo_npid', '')).strip(),
        'BB_SHADNET_PASSWORD': str(settings.get('supermedo_pass', '')),
        'SHADPS4_BLOODBORNE_SEAMLESS_COOP': '1', 'SHADPS4_BLOODBORNE_RE_TRACE': '0',
        # The co-op code's files (flag store, portraits, crash dumps) and this engine's own shader cache stay
        # with the seamless saves, never in the normal game's user or cache folder.
        'BB_GPU_USER_DIR': str(user_dir(port_dir)),
        # The port's own Discord status (BB_DISCORD, the launcher's setting and Discord application), not BBSC's.
        'BBSC_DISCORD_STATUS': '0',
    })
    # Mods on (the view's -mods folder, the co-op events, is a layer only then), from seamless's own mods folder
    # (mods_view: the player's chosen mods as links, the party HUD last); without one, an empty one.
    view, config = mods or (folder(port_dir) / MODS_VIEW, folder(port_dir) / 'mods.json')
    env.update({'BB_MODS_ENABLED': '1', 'BB_MODS_DIR': str(view), 'BB_MODS_CONFIG': str(config)})
    # The owner's testers (2026-11-06): MOAL's map event files over the co-op ones turned floors red and broke
    # the co-op events. The co-op -mods folder goes over the player's mods: their other files still apply.
    env['BB_MODS_GAME_LAST'] = '1'
    if valid_link_key(key):
        env['BB_SHADNET_SERVER_KEY'] = key
    if (rules := host_rules(settings)) != 'off':
        env['SHADPS4_BLOODBORNE_HOST_RULES'] = rules
    harder = (custom_enemies(settings) if settings.get('yt_enemies') == 'custom'
              else ENEMIES.get(settings.get('yt_enemies')))
    if harder and harder != 'off':
        env['SHADPS4_BLOODBORNE_HARDER_ENEMIES'] = harder
    return env


def console_log(port_dir):
    return user_dir(port_dir) / 'log' / 'console.txt'


def rotate_logs(port_dir):
    """The last run's console log becomes console.prev.txt."""
    log = user_dir(port_dir) / 'log'
    log.mkdir(parents=True, exist_ok=True)
    current = log / 'console.txt'
    if current.is_file():
        shutil.copy2(current, log / 'console.prev.txt')
    return log
