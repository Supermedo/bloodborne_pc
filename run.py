#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Windows counterpart of run.sh: prepares the game image and starts bb-probe.exe.

Same environment variables as run.sh (BB_GAME_DIR, BB_DATA_DIR, BB_FPS, BB_RENDER_RES,
BB_LIVE_RES, BB_PATCHES, BB_MODS_*, BB_USER_DIR, ...). Extra arguments go to bb-probe.
The in-game "Apply and restart" runs this script again through BB_RESTART_COMMAND;
`--after PID` waits for the previous game process to end first (its GPU device and memory).
"""
import ctypes
import io
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time

PORT = Path(__file__).resolve().parent


def prepare_driver_environment(env):
    """The direct/frozen runner needs the same automatic driver lookup as the GUI."""
    sys.path.insert(0, str(PORT / 'launcher'))
    import bbport_driver_runtime
    return bbport_driver_runtime.configure_environment(env)


# Co-op: the community service that answers Bloodborne's own game-server requests
# (messages, bloodstains, sign-in); summon requests go to the shadNet server instead.
COMMUNITY_SERVER = 'https://game.supermedo.com'
HUNTERS_DREAM_SERVER = 'https://thehuntersdream.com'  # The Hunter's Dream pool's own (BB_HUNTERS_DREAM=1)
GAME_SERVER = 'https://ss4.scej-network.jp:20443'
# The server's ss.info sends the game to this port for its API (summons among them).
COMMUNITY_API_PORT = 18671


def seamless_coop(env):
    """BBSC (seamless co-op) starts the game: its bbsc-launch.exe is the game program, from this
    launcher's co-op choice or from BBSC's own scripts."""
    return Path(env.get('BB_PROBE', '')).name.lower() == 'bbsc-launch.exe'


def bbsc_overrides(path):
    """user/host_overrides.custom.json as BBSC's play-bbport.ps1 writes it (and leaves behind):
    the summon messenger on BBSC's WebAPI port."""
    try:
        entries = json.loads(path.read_text(encoding='utf-8-sig'))
    except (OSError, ValueError):
        return False
    return isinstance(entries, dict) and any('summon_messenger' in key and str(value).rstrip('/').endswith(':31315')
                                             for key, value in entries.items())


def online_overrides(env, user_dir):
    """Writes user/host_overrides.json for this launch. A player who needs other addresses
    puts them in user/host_overrides.custom.json, which is then used as it is."""
    custom = user_dir / 'host_overrides.custom.json'
    if custom.exists() and not seamless_coop(env) and bbsc_overrides(custom):
        # BBSC's scripts leave it pointing the game server at BBSC's: online without BBSC stays
        # on the player's own server.
        print("Co-op: user/host_overrides.custom.json is BBSC's (seamless co-op), not used without BBSC")
        custom = None
    path = custom if custom and custom.exists() else user_dir / 'host_overrides.json'
    if path != custom:
        community = (env.get('BB_COMMUNITY_SERVER') or COMMUNITY_SERVER).rstrip('/')
        if '://' not in community:
            community = 'https://' + community
        scheme, host = community.split('://', 1)
        # The game's own server functions (messages, bloodstains, summon signs) go over HTTP to
        # the game server only, as bbhost and Awaken's port send them; never through a shadNet
        # WebAPI (Metr1k, The Hunter's Dream). Play logs go to its API port instead of
        # FromSoftware's old storage buckets.
        overrides = {GAME_SERVER: community}
        overrides.update({f'https://{bucket}:443': f'{scheme}://{host}:{COMMUNITY_API_PORT}'
                          for bucket in ('bb-playlog-prod.s3.amazonaws.com',
                                         'bb-playlog-test.s3.amazonaws.com')})
        print(f'Co-op: game server {community}')
        path.write_text(json.dumps(overrides, indent=2) + '\n', encoding='utf-8')
    env['SHADPS4_HTTP_HOST_OVERRIDES_JSON'] = str(path)


# The Hunter's Dream co-op pool (BB_NET_BACKEND=thd, with bbhost players): the engine's own
# backend for it (gpu/bbnet/thd, bbhost's protocol, from Awaken's port). The game's servers and
# play logs go to the game server over HTTP; the account comes from the launcher's Discord sign-in.
LIVE_MAP_PATCH = "Live map (The Hunter's Dream)"
PLAY_LOG_HOSTS = ('bb-playlog-prod.s3.amazonaws.com', 'bb-playlog-test.s3.amazonaws.com')


def hunters_dream(env, user_dir):
    if not env.get('BB_THD_TOKEN') or not env.get('BB_THD_ONLINE_ID'):
        fail("The Hunter's Dream: not signed in. Sign in with Discord on the launcher's Online page.")
    community = (env.get('BB_COMMUNITY_SERVER') or HUNTERS_DREAM_SERVER).rstrip('/')
    if '://' not in community:
        community = 'https://' + community
    base = community
    overrides = {'*.scej-network.jp': base}
    overrides.update({host: f'{base}:{COMMUNITY_API_PORT}' for host in PLAY_LOG_HOSTS})
    path = user_dir / 'host_overrides.json'
    path.write_text(json.dumps(overrides, indent=2) + '\n', encoding='utf-8')
    for key in ('BB_SHADNET_SERVER', 'BB_SHADNET_WEBAPI', 'BB_SHADNET_NPID', 'BB_SHADNET_PASSWORD',
                'BB_SHADNET_TOKEN'):
        env.pop(key, None)
    env.update(SHADPS4_HTTP_HOST_OVERRIDES_JSON=str(path), BB_THD_SERVER=f'{base}:{COMMUNITY_API_PORT}',
               BB_THD_AUTH=base)
    env.setdefault('BB_THD_RULESET', 'vanilla')
    env.setdefault('BB_P2P_PORT', '3658')
    env.setdefault('BB_SIGN_TIMEOUT', '30')
    print(f"Co-op: The Hunter's Dream as {env['BB_THD_ONLINE_ID']} ({base})")


# The 1.09 executable with Lance McDonald's 60 fps patch applied to it, as some dumps ship it
# (seen on US CUSA00900): one of its edits inverts a branch in the menu code (+0xf6d90d) and the
# gestures menu crashes the game. The port has its own 60 fps; the clean 1.09 eboot.bin is needed.
LANCE_60FPS_IMAGE = 'bbc91f4dff6bc3118b039464289e39878a8fdfc802a07fab9d5fe84d935a676f'


def eboot_image(game):
    """eboot.bin's loaded image (its loadable segments at their addresses): the same whatever
    tool dumped it, unlike the SELF around it."""
    sys.path.insert(0, str(PORT / 'scripts'))
    from prepare import parse_self, span
    elf, _header, ph, _segments, _missing = parse_self((Path(game) / 'eboot.bin').read_bytes())
    loads = [p for p in ph if p['type'] in (1, 0x61000010)]
    image = bytearray(max(p['vaddr'] + p['memsz'] for p in loads))
    for p in loads:
        image[p['vaddr']:p['vaddr'] + p['filesz']] = span(elf, p['offset'], p['filesz'])
    return image


def image_sha256(game):
    import hashlib
    return hashlib.sha256(eboot_image(game)).hexdigest()


def fail(message):
    print(message, file=sys.stderr)
    sys.exit(1)


def wait_for(pid):
    kernel32 = ctypes.windll.kernel32
    SYNCHRONIZE = 0x00100000
    handle = kernel32.OpenProcess(SYNCHRONIZE, False, pid)
    if handle:
        kernel32.WaitForSingleObject(handle, 30000)
        kernel32.CloseHandle(handle)


def game_running():
    """Whether a game (bb-probe.exe) runs now; unknown counts as yes."""
    try:
        result = subprocess.run(['tasklist', '/FI', 'IMAGENAME eq bb-probe.exe', '/FO', 'CSV', '/NH'],
                                capture_output=True, timeout=15, creationflags=no_console())
    except (OSError, subprocess.SubprocessError):
        return True
    # Bytes: its "no tasks" line is in the console's code page and language.
    return result.returncode != 0 or b'bb-probe.exe' in result.stdout.lower()


def remove_stale_views(out, game):
    """The game views with mods (mods.py's mod-game-* folders in out or beside the game) that a
    game left behind when it did not end through run.py (ended in the Task Manager, the PC
    turned off): removed before a new one is made, while no other game runs. They hold links
    into the game folder; mods.remove_overlay deletes the links, never what they point to."""
    import tempfile  # mods.py's game view for a drive without links (exFAT, FAT32)
    places = {Path(out).resolve(), Path(game).parent, Path(tempfile.gettempdir()) / 'bbport-game-view'}
    stale = [path for place in places if place.is_dir() for path in place.glob('mod-game-*')
             if path.is_dir() and not path.is_symlink() and not (hasattr(path, 'is_junction') and path.is_junction())]
    if not stale or game_running():
        return
    from mods import remove_overlay
    removed = 0
    for path in stale:
        try:
            remove_overlay(path, game)
            removed += 1
        except OSError as error:
            print(f'Mods: {path} could not be removed: {error}')
    if removed:
        print(f'Mods: removed {removed} game view(s) an earlier game left behind')


# Held while a start prepares and runs the game: the launcher and Play Bloodborne.exe bring that game's
# window to the front instead of starting a second one (bbport_launcher_win.game_underway).
GAME_MUTEX = 'Local\\bbport-game'


def hold_game_mutex(name=GAME_MUTEX):
    """Opens the game's named mutex for the rest of this process; False when another start has it."""
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    kernel.CreateMutexW.restype = ctypes.c_void_p
    global _GAME_MUTEX_HANDLE
    _GAME_MUTEX_HANDLE = kernel.CreateMutexW(None, False, name)
    return ctypes.get_last_error() != 183  # ERROR_ALREADY_EXISTS


def no_console():
    """Started without a console (the launcher): console programs must not open their own."""
    return 0 if ctypes.windll.kernel32.GetConsoleWindow() else subprocess.CREATE_NO_WINDOW


def files_note(port):
    """A line for the log when the program files are of different versions (scripts/package_files.py): the game
    then stops with "bad patch entry" (a black screen), and the launcher's Help names the fix. None when they
    match, or without the package's list (a build from the sources)."""
    try:
        sys.path.insert(0, str(Path(port) / 'scripts'))
        from package_files import check
        version, differ = check(port, quick=True)
    except Exception:  # a check: never in the way of a start
        return None
    if not differ:
        return None
    return f'Files: {len(differ)} differ from the package {version}: {", ".join(differ[:5])}'


def run_script(name, *args, capture=False, optional=False, timeout=None):
    """Runs scripts/<name>. optional: a failure returns False instead of stopping the launch.
    timeout (seconds, with capture and optional): a script still running then is stopped, and that
    counts as a failure."""
    # Inside the packaged Bloodborne.exe (PyInstaller) the executable runs scripts itself.
    script = ['--script'] if getattr(sys, 'frozen', False) else []
    command = [sys.executable, *script, str(PORT / 'scripts' / name), *map(str, args)]
    for attempt in range(1, LOCK_TRIES + 1):
        # stdin given: the output handles are passed explicitly (a windowed Bloodborne.exe child would
        # get none otherwise). The errors are read for a locked file, then passed on.
        try:
            result = subprocess.run(command, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE if capture else None,
                                    stderr=subprocess.PIPE, creationflags=no_console(),
                                    timeout=timeout if capture and optional else None)
        except subprocess.TimeoutExpired as expired:
            echo_errors(expired.stderr or b'')
            print(f'{name}: still running after {timeout} s; stopped')
            return None
        except OSError:
            if optional and not capture:
                return False
            raise
        echo_errors(result.stderr)
        if not result.returncode or attempt == LOCK_TRIES or \
                not LOCKED.search(result.stderr.decode('utf-8', errors='replace')):
            break
        print(f'{name}: a file was locked (an antivirus scanning it?); trying again', flush=True)
        time.sleep(LOCK_WAIT)
    if optional and not capture:
        return result.returncode == 0
    if result.returncode:
        if optional:
            return None
        sys.exit(result.returncode)
    # Decoded as text=True did: the locale's encoding, universal newlines.
    return io.TextIOWrapper(io.BytesIO(result.stdout)).read() if capture else None


# An antivirus that scans a file the port has just written holds it for a moment, and opening it fails
# ("[WinError 32]", "[WinError 5]", "[Errno 13] Permission denied"). run_script runs such a step again,
# LOCK_TRIES times in all over about two seconds, before the start fails.
LOCKED = re.compile(r'WinError (?:32|5)\b|PermissionError|Errno 13\b')
LOCK_TRIES, LOCK_WAIT = 4, 0.6


def echo_errors(data):
    """A script's error output, passed on as it came."""
    if not data:
        return
    try:
        sys.stderr.flush()
        sys.stderr.buffer.write(data)
        sys.stderr.buffer.flush()
    except (AttributeError, OSError, ValueError):
        sys.stderr.write(data.decode('utf-8', errors='replace'))


def auto_live_blurs_dof(output, config):
    """live_resolution=auto keeps the startup patch for outputs above 1080p with depth of field on:
    the live path renders such scenes into host targets larger than the guest's 1920x1080, and
    the game's depth of field composite, built for its own half-resolution targets, blurs them
    (Native at 4K came out softer than Performance)."""
    width, height = (int(v) for v in output.split('x'))
    return (width > 1920 or height > 1080) and ini_value(config, 'effect_dof') != '0'


def ini_value(path, key):
    try:
        for line in Path(path).read_text(encoding='utf-8', errors='replace').splitlines():
            match = re.fullmatch(rf'{re.escape(key)}=(.*)', line.strip())
            if match:
                return match.group(1)
    except OSError:
        pass
    return None


def refresh_shared_graphics(env, config):
    """Re-read launcher/overlay startup choices after an in-game restart."""
    if env.get('BB_SHARED_GRAPHICS_CONFIG') != '1':
        return  # direct engine/script experiments retain their explicit overrides
    values = {}
    try:
        for line in Path(config).read_text(encoding='utf-8-sig', errors='replace').splitlines():
            if '=' in line and not line.lstrip().startswith(('#', ';')):
                key, value = line.split('=', 1)
                values[key.strip()] = value.strip()
    except OSError:
        pass
    backend = 'dx12' if values.get('fsr411_backend') == 'dx12' else 'vk'
    env['BB_FSR411_BACKEND'] = backend
    env['BB_FSR411_NATIVE'] = '1' if backend == 'dx12' else '0'
    cache = values.get('object_motion_cache') == '1' and \
        values.get('object_motion', '1') == '1' and \
        values.get('upscaler', 'fsr4') in ('fsr3', 'fsr4', 'fsr411', 'fsr412')
    env['BB_OBJECT_MOTION_CACHE'] = '1' if cache else '0'
    # Frame rate mode and cap: the launcher and the game's PC Settings page share them.
    if values.get('fps_mode') in ('30', '60', '90', 'uncap'):
        env['BB_FPS'] = values['fps_mode']
    if 'frame_cap' in values:
        if values['frame_cap'].isdigit():
            env['BB_FPS_LIMIT'] = values['frame_cap']
        else:
            env.pop('BB_FPS_LIMIT', None)


def find_executable(name):
    """bin/ (packaged), else out/ (built with build.sh)."""
    for candidate in (PORT / 'bin' / name, PORT / 'out' / name):
        if candidate.is_file():
            return candidate
    return None


# Low-VRAM profile (from a player's 4 GB mod; measured on an RTX 3050 Ti Laptop, i5-11300H, Central
# Yharnam, 1080p output with an upscaler): the texture cache's own marks had it evict and upload
# again ~30 textures a second with 2.7 GB in use, and the live path's native-size game targets
# cost ~700 MiB more than the startup patch (2.8 GB at 640x360 against 2.1 GB at 960x540).
# Discrete GPUs up to LOW_VRAM_MB; BB_LOW_VRAM=1 forces the profile, 0 turns it off.
LOW_VRAM_MB = 6656  # 4 and 6 GB cards (6144 MiB with room for what a driver reports), not 8 GB
# Left to the desktop, browsers and the driver.
VRAM_RESERVE_MB = 600


def low_vram_mb(env, caps):
    """The card's memory in MiB when the low-VRAM profile applies, else 0. caps is
    bb-gpu-capabilities, which picks the GPU the game uses (BB_GPU_ID included)."""
    setting = env.get('BB_LOW_VRAM', '')
    if setting == '0':
        return 0
    vram, discrete = 0, False
    try:
        result = subprocess.run([str(caps), '--vram'], stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                stderr=subprocess.DEVNULL, text=True, timeout=20, creationflags=no_console())
        size, kind = result.stdout.split()
        vram, discrete = int(size), kind == '1'
    except (OSError, ValueError, subprocess.SubprocessError):
        pass
    if setting == '1':
        return vram or 4096
    # Integrated GPUs follow the driver's budget already (texture_cache.cpp).
    return vram if discrete and 0 < vram <= LOW_VRAM_MB else 0


def physical_cores():
    """CPU cores, not threads (GetLogicalProcessorInformation); os.cpu_count() when unknown."""
    try:
        from ctypes import wintypes
        size = wintypes.DWORD(0)
        ctypes.windll.kernel32.GetLogicalProcessorInformation(None, ctypes.byref(size))
        buffer = ctypes.create_string_buffer(size.value)
        if not ctypes.windll.kernel32.GetLogicalProcessorInformation(buffer, ctypes.byref(size)):
            raise OSError
        # SYSTEM_LOGICAL_PROCESSOR_INFORMATION: ULONG_PTR mask, then the relationship (0 = a core).
        entry = ctypes.sizeof(ctypes.c_void_p) * 2 + 16
        cores = sum(1 for offset in range(0, size.value - entry + 1, entry)
                    if int.from_bytes(buffer.raw[offset + ctypes.sizeof(ctypes.c_void_p):][:4], 'little') == 0)
        if cores:
            return cores
    except (AttributeError, OSError):
        pass
    return os.cpu_count() or 8


def thermal_watch(stop):
    """BB_THERMAL_LOG=1 (debugging): notes in the log when the NVIDIA driver starts or stops
    slowing the card down to cool it (nvidia-smi's thermal slowdown reasons), checked every 20 s.
    On a laptop that is what a frame rate falling after some minutes of play usually is."""
    smi = shutil.which('nvidia-smi') or str(Path(os.environ.get('SystemRoot', r'C:\Windows'))
                                            / 'System32' / 'nvidia-smi.exe')
    if not Path(smi).is_file():
        print('Thermal: no nvidia-smi (NVIDIA only)', flush=True)
        return
    slowed = None
    while not stop.wait(20):
        try:
            line = subprocess.run([smi, '--query-gpu=temperature.gpu,power.draw,clocks_throttle_reasons.active',
                                   '--format=csv,noheader,nounits'], stdin=subprocess.DEVNULL,
                                  stdout=subprocess.PIPE, text=True, timeout=10,
                                  creationflags=no_console()).stdout.splitlines()[0]
            temperature, watts, reasons = (value.strip() for value in line.split(','))
            now = bool(int(reasons, 16) & 0x60)  # software or hardware thermal slowdown
        except (OSError, ValueError, IndexError, subprocess.SubprocessError):
            return
        if now != slowed:
            slowed = now
            print(f'Thermal: GPU {temperature} C, {watts} W: '
                  + ('the driver is slowing the card down to cool it' if now else 'no thermal limit'), flush=True)


SAVE_BACKUPS_KEPT = 10


# Seamless co-op's own saves: <user>/seamless-coop is the game's user folder while BBSC plays
# (its saves, its rebirth targets), so co-op never writes the player's normal characters.
COOP_SAVES = 'seamless-coop'


def coop_user_dir(env, user_dir, game):
    """The user folder for a BBSC start. The GPU caches stay in the normal one, the DLC the normal
    one has is reported installed here too, and BBSC is told where the character's save is (it
    otherwise looks in <working folder>/user, the normal saves)."""
    coop = user_dir / COOP_SAVES
    coop.mkdir(parents=True, exist_ok=True)
    if not env.get('BB_DLC_DIR'):
        titles = sorted(p for p in (user_dir / 'addcont').glob('*') if p.is_dir() and any(p.iterdir()))
        if titles:
            env['BB_DLC_DIR'] = str(titles[0])  # its <label> folders, as the launcher's DLC folder
    title = 'CUSA00207'
    try:
        sys.path.insert(0, str(PORT / 'scripts'))
        from prepare import sfo
        values = sfo((Path(game) / 'sce_sys' / 'param.sfo').read_bytes())
        found = str(values.get('INSTALL_DIR_SAVEDATA') or values.get('TITLE_ID') or '').strip()
        if re.fullmatch(r'[A-Z]{4}\d{5}', found):
            title = found
    except (OSError, ValueError, ImportError, TypeError):
        pass
    env.setdefault('BBSC_SAVE_DIR', str(coop / 'savedata' / '1' / title / 'SPRJ0005'))
    print(f'Seamless co-op: its own saves in {coop} (the normal saves are not used)')
    return coop


def backup_saves(user_dir, keep=SAVE_BACKUPS_KEPT, now=None):
    """BB_SAVE_BACKUPS=1 (launcher: Play page, "Back up saves at start"): a copy of the saves
    in <user>/save-backups/<time>/savedata before the game starts, the newest `keep` kept.
    A failed copy is reported and the game starts anyway. Returns the copy, or None."""
    saves = Path(user_dir) / 'savedata'
    if not saves.is_dir() or not any(saves.rglob('userdata*')):
        return None
    root = Path(user_dir) / 'save-backups'
    target = root / time.strftime('%Y%m%d-%H%M%S', time.localtime(now)) / 'savedata'
    try:
        if not target.exists():
            shutil.copytree(saves, target)
        for old in sorted(p for p in root.iterdir() if p.is_dir())[:-keep]:
            shutil.rmtree(old, ignore_errors=True)
    except OSError as error:
        print(f'Save backup: {error}; the game starts without one')
        return None
    print(f'Save backup: {target.parent}')
    return target


def system_view(game, view, system, out):
    """The game view for a dump whose system folder a PKG tool named Sc0 (prepare.system_folder): the view
    made for mods gets a sce_sys folder, or a view of its own is made (mod-game-*, removed after the game as a
    view with mods is). Its sce_sys holds copies of the dump's own sce_sys files with Sc0's over them: a few
    small files; the dump is never written to."""
    import tempfile
    from mods import make_link, remove_overlay
    game, made = Path(game), view is None
    if made:
        place = Path(out).resolve()
        if os.name == 'nt' and place.drive.casefold() != game.drive.casefold():
            place = game.parent  # hard links reach one drive only (mods.build_overlay)
        place.mkdir(parents=True, exist_ok=True)
        view = Path(tempfile.mkdtemp(prefix='mod-game-', dir=place))
    try:
        if made:
            for entry in game.iterdir():
                if entry.name.casefold() not in ('sce_sys', 'sc0'):
                    make_link(view / entry.name, entry)
        target = view / 'sce_sys'
        if os.path.lexists(target):
            os.rmdir(target)  # the view's link to the dump's sce_sys; a folder with files is never removed
        for source in (game / 'sce_sys', system):
            if source.is_dir():
                shutil.copytree(source, target, dirs_exist_ok=True)
    except BaseException:
        if made:
            remove_overlay(view, game)
        raise
    print(f'Game files: {system} serves as sce_sys (a PKG tool\'s layout)')
    return view


def user_folders(env, data):
    """The saves folder (BB_USER_DIR, else <data>/user), and BB_GPU_USER_DIR for the caches (beside the saves
    unless set). The player's own saves folder is never made anew: on a drive that is not connected, or renamed,
    an empty one would show no characters (the launcher says so before the start). A cache folder that cannot be
    had (its drive is gone) gives way to the saves folder; caches are made again, nothing is lost."""
    user_dir = Path(env.get('BB_USER_DIR', Path(data) / 'user')).resolve()
    if env.get('BB_USER_DIR') and not user_dir.is_dir():
        fail(f'The saves folder {user_dir} is not there: its drive is not connected, or the folder was renamed '
             'or moved. Connect the drive, or choose the saves folder again in the launcher (Saves).')
    user_dir.mkdir(parents=True, exist_ok=True)
    cache_dir = env.get('BB_GPU_USER_DIR')
    if cache_dir:
        try:
            Path(cache_dir).mkdir(parents=True, exist_ok=True)
        except OSError as error:
            print(f'Cache: {cache_dir} cannot be used ({error}); the caches go to {user_dir} this time')
            env['BB_GPU_USER_DIR'] = str(user_dir)
    env.setdefault('BB_GPU_USER_DIR', str(user_dir))
    return user_dir


def writable_saves(user_dir):
    """Save files marked read-only (copied from a backup disc or another PC, or by hand): the game
    cannot replace them, so every save fails while it ends with 0 (runtime_file.c commit_file).
    The mark is cleared before each start; returns how many files had it."""
    import stat
    cleared = 0
    for path in (Path(user_dir) / 'savedata').rglob('*'):
        try:
            if path.is_file() and not path.stat().st_mode & stat.S_IWRITE:
                os.chmod(path, stat.S_IREAD | stat.S_IWRITE)
                cleared += 1
        except OSError as error:
            print(f'Saves: {path} stays read-only: {error}')
    if cleared:
        print(f'Saves: {cleared} read-only save file(s) made writable')
    return cleared


def main():
    args = sys.argv[1:]
    restarted = len(args) >= 2 and args[0] == '--after'
    if restarted:
        wait_for(int(args[1]))
        args = args[2:]
    # Two starts at once (a second click on Play Bloodborne.exe while the first is being prepared) would
    # prepare the same files and share the saves. The in-game restart takes over from the game it replaces.
    if not hold_game_mutex() and not restarted:
        print('Start: another start of the game is being prepared or runs; this one ends')
        return 0
    # As run.sh: relative paths (BB_GAME_DIR, BB_DATA_DIR, fsr4_shaders/) are from the port.
    os.chdir(PORT)
    env = os.environ
    note = files_note(PORT)
    if note:
        print(note)
    prepare_driver_environment(env)
    data = Path(env.get('BB_DATA_DIR', PORT))
    out = data / 'out'
    out.mkdir(parents=True, exist_ok=True)
    env.setdefault('BB_CONFIG', str(data / 'bbport.ini'))
    config = env['BB_CONFIG']
    refresh_shared_graphics(env, config)
    env.setdefault('BB_FSR4VK_DIR', str((data / 'fsr4vk').resolve()))
    if not env.get('BB_FSR411_DIR') and not (PORT / 'fsr4_411').is_dir() and (data / 'fsr4_411').is_dir():
        env['BB_FSR411_DIR'] = str(data / 'fsr4_411')

    probe = Path(env['BB_PROBE']) if env.get('BB_PROBE') else find_executable('bb-probe.exe')
    if not probe or not probe.is_file():
        if getattr(sys, 'frozen', False):
            # A packaged port always has it: an antivirus took it away (the launcher's Help names the fix).
            fail(f'bb-probe.exe is missing from {PORT / "bin"}: an antivirus has probably moved it to its '
                 'quarantine. Restore it there and add the port folder to the antivirus\'s exclusions, or '
                 'extract the port\'s zip again.')
        fail('bb-probe.exe not found: build it with build.sh (MSYS2 CLANG64) or use a packaged build.')
    # Development builds run from the MSYS2 tree: its CLANG64 DLLs (SDL3, FFmpeg, ...).
    if not (probe.parent / 'SDL3.dll').is_file():
        clang64 = Path(env.get('MSYS2_ROOT', r'C:\msys64')) / 'clang64' / 'bin'
        if clang64.is_dir():
            env['PATH'] = f'{clang64}{os.pathsep}{env.get("PATH", "")}'

    game = Path(env.get('BB_GAME_DIR', PORT.parent / 'CUSA03173'))
    if not (game / 'eboot.bin').is_file():
        fail(f'No eboot.bin in {game} (set BB_GAME_DIR).')
    original_game = game.resolve()
    # Without them the start fails later with a path inside the game view. A PKG tool's Sc0 is the
    # system folder too (prepare.system_folder); system_view maps it to sce_sys.
    sys.path.insert(0, str(PORT / 'scripts'))
    from prepare import system_folder
    system = system_folder(game)
    missing = [name for name, there in (('sce_module', (game / 'sce_module').is_dir()),
                                        ('sce_sys', (system / 'param.sfo').is_file())) if not there]
    if missing:
        fail(f'The game folder {game} has no {" / ".join(missing)}: the game was not unpacked completely. '
             'Unpack the base game into a new, empty folder again and copy the 1.09 update over it.')
    if env.get('BB_SKIP_GAME_CHECK') != '1':
        try:
            prepatched = image_sha256(game) == LANCE_60FPS_IMAGE
        except (OSError, ValueError, KeyError, IndexError):
            prepatched = False  # prepare.py reports unreadable executables itself
        if prepatched:
            fail("This eboot.bin has Lance McDonald's 60 fps patch applied to it, which crashes the "
                 'game when the gestures menu opens (the port has its own 60 fps): copy eboot.bin '
                 'from a clean dump of the 1.09 update into the game folder, replacing this one. '
                 'BB_SKIP_GAME_CHECK=1 starts it anyway.')
    sys.path.insert(0, str(PORT / 'scripts'))
    from patches import (OUTPUT_SIZE, PATCH_GROUPS, game_app_version, group_list, hud_canvas,
                         image_groups, output_size, read_settings, ui_viewport)
    remove_stale_views(out, original_game)
    # Patches exist for game version 01.09 only (patches.py applies none to others): other
    # versions keep the game's 30 FPS timing and change resolutions live. Of a 01.09 eboot.bin,
    # only the groups of patches whose code is the 01.09 build's are applied (patches.py
    # PATCH_GROUPS): checked here, before the HUD layout below, which needs the resolution group.
    version = game_app_version(game)
    if env.get('BB_FORCE_PATCHES'):
        groups = set(PATCH_GROUPS)
    elif version not in (None, '01.09'):
        groups = set()
        print(f'Game version {version}: community patches need 01.09; 30 FPS, no effect patches')
    else:
        try:
            groups = image_groups(eboot_image(game))
        except (OSError, ValueError, KeyError, IndexError):
            groups = set()  # prepare.py reports unreadable executables itself
        if not groups:
            print('Game files: eboot.bin is not the 01.09 build the community patches were made for; '
                  '30 FPS, no effect patches, no ultrawide or output resolution patch')
        elif groups != set(PATCH_GROUPS):
            print(f'Game files: eboot.bin is modified, not the clean 01.09 build: its '
                  f'{group_list(set(PATCH_GROUPS) - groups)} code differs, so those options are off; '
                  f'{group_list(groups)} work')
    # Quit to Desktop (bbport.ini quit_to_desktop, patches.py): the entries' names in the menu
    # texts of every language and six-row title and System menus, also a layer below the mods
    # (before the ultrawide one, which builds on its menu movies).
    # PC Settings (bbport.ini pc_menu, patches.py, pc_menu.py) adds its line and page texts to
    # the same layer, and a seventh System menu row.
    text_layer = out / 'quit_text'
    shutil.rmtree(text_layer, ignore_errors=True)
    quit_menu = ini_value(config, 'quit_to_desktop') != '0'
    pc_menu = ini_value(config, 'pc_menu') != '0'
    if (quit_menu or pc_menu) and (game_app_version(game) in (None, '01.09') or env.get('BB_FORCE_PATCHES')):
        # Optional: when its files cannot be written ("[WinError 5] Access is denied", an
        # antivirus or the folder's permissions), the game starts without it.
        flags = ([] if quit_menu else ['--no-quit']) + (['--pc'] if pc_menu else [])
        if not run_script('quit_menu.py', game, '--out', text_layer, *flags, optional=True):
            shutil.rmtree(text_layer, ignore_errors=True)
            env['BB_QUIT_TO_DESKTOP'] = '0'  # patches.py: no menu entries without their texts
            env['BB_PC_MENU'] = '0'
            print('Quit to Desktop: its menu texts could not be written; the game starts without it')
    # Rebirth at the Doll (bbport.ini rebirth, patches.py): the Doll's talk script and its texts
    # (on top of Quit to Desktop's, which edit the same menu text archives), a layer below the mods.
    rebirth_layer = out / 'rebirth'
    shutil.rmtree(rebirth_layer, ignore_errors=True)
    if ini_value(config, 'rebirth') != '0' and (game_app_version(game) in (None, '01.09') or env.get('BB_FORCE_PATCHES')):
        weapons = out / 'rebirth-weapons.txt'  # what the equipped weapons need: kept by a rebirth
        env['BB_REBIRTH_WEAPONS'] = str(weapons)
        if not run_script('rebirth.py', game, '--out', rebirth_layer, '--source', text_layer, '--weapons', weapons,
                          optional=True):
            shutil.rmtree(rebirth_layer, ignore_errors=True)
            env['BB_REBIRTH'] = '0'  # patches.py: no eboot hooks without the Doll's script
            print("Rebirth: the Doll's script could not be written; the game starts without it")
    # The Discord status (src/runtime_discord.c): the game's own place, boss and level texts.
    if env.get('BB_DISCORD') != '0':
        names = out / 'discord-names.txt'
        if run_script('discord_names.py', game, '--out', names, '--language', env.get('BB_LANGUAGE', '1'),
                      optional=True):
            env['BB_DISCORD_NAMES'] = str(names)
    # Ultrawide outputs: the menu movies with full-screen backdrops widened and the HUD's corner
    # groups toward the screen edges (bbport.ini hud_position, 0 centred .. 100 edges), a layer
    # below the mods. Only with the resolution patches, which widen the camera and HUD canvas
    # (patches.py hud_canvas): without them the widened menus showed stretched on a 16:9 game.
    hud_layer = out / 'hud_layout'
    shutil.rmtree(hud_layer, ignore_errors=True)
    canvas = hud_canvas(output_size(read_settings(Path(config))))
    if canvas != OUTPUT_SIZE and 'resolution' in groups:
        position = ini_value(config, 'hud_position') or '100'
        if not run_script('hud_layout.py', game, '--out', hud_layer, '--canvas-width', canvas[0],
                          '--position', max(0, min(100, int(position) if position.isdigit() else 100)) / 100,
                          '--source', text_layer, optional=True):
            shutil.rmtree(hud_layer, ignore_errors=True)
            print('HUD layout: its menu files could not be written; the HUD stays in the centre')
    # Field of view (bbport.ini fov, degrees; 43 is the game's): the camera table with every
    # camera's field of view scaled, a layer below the mods. fov.py keeps it between starts
    # while the setting and the game file are the same.
    fov_layer = out / 'fov'
    fov = ini_value(config, 'fov')
    if fov and fov.isdigit() and int(fov) != 43:
        if not run_script('fov.py', game, '--out', fov_layer, '--fov', fov, optional=True):
            shutil.rmtree(fov_layer, ignore_errors=True)
            print('Field of view: its camera table could not be written; the game keeps its own')
    else:
        shutil.rmtree(fov_layer, ignore_errors=True)
    mod_args = [game, '--out', out, '--mods-dir', env.get('BB_MODS_DIR', data / 'mods'),
                '--config', env.get('BB_MODS_CONFIG', data / 'mods.json'),
                '--enabled', env.get('BB_MODS_ENABLED', '1')]
    view = run_script('mods.py', *mod_args, '--layer', text_layer, '--layer', rebirth_layer, '--layer', hud_layer,
                      '--layer', fov_layer, capture=True, optional=True, timeout=60)
    if view is None:
        # The game view could not be made with the port's own layers (Quit to Desktop's texts,
        # the HUD layout); without them the game still starts, as it did before they existed.
        # Also when making it hangs (a player's game drive where it never finished, 1.9.4: the
        # view takes about a second); the half-made view is removed at the next start.
        print('Mods: the game view could not be made; starting without Quit to Desktop and the '
              'HUD layout')
        env['BB_QUIT_TO_DESKTOP'] = '0'
        env['BB_REBIRTH'] = '0'
        view = run_script('mods.py', *mod_args, capture=True)
    game = Path(view.strip())
    mod_view = game if game.resolve() != original_game else None
    if system.name != 'sce_sys':
        game = mod_view = system_view(original_game, mod_view, system, out)
    try:
        run_script('prepare.py', game, '--out', out)
        run_script('link_libc.py', game, '--out', out)
        run_script('link_modules.py', game, '--out', out)
        # Legacy CPU profile (scripts/cpu_compat.py): the image no longer needs BMI1, LZCNT or
        # MOVBE, and bb-probe's CPU check lets such a CPU start.
        try:
            compat = json.loads((out / 'link.json').read_text(encoding='utf-8')).get('cpu_compat', {})
        except (OSError, ValueError):
            compat = {}
        if compat.get('enabled'):
            env['BB_CPU_COMPAT'] = '1'
        else:
            env.pop('BB_CPU_COMPAT', None)
        run_script('content_profile.py', game, '--out', out, '--sku', env.get('BB_CONTENT_SKU', 'full'))

        # The groups checked above: the frame rate needs the FPS patches, the startup resolution
        # and ultrawide the resolution ones.
        patched = 'resolution' in groups
        # Sizes chosen below for the previous launch are recomputed after an in-game restart.
        if env.get('BB_AUTO_RENDER_RES') == '1':
            for key in ('BB_RENDER_RES', 'BB_OUTPUT_RES', 'BB_AUTO_RENDER_RES'):
                env.pop(key, None)
        fps = env.get('BB_FPS', 'uncap') if 'fps' in groups else '30'
        scaled_render = scaled_output = None
        if not env.get('BB_RENDER_RES'):
            printed = run_script('patches.py', '--print-scaled', '--settings', config, capture=True).split()
            if len(printed) == 2:
                scaled_render, scaled_output = printed
        live = '0'
        if scaled_output:
            live = env.get('BB_LIVE_RES') or ini_value(config, 'live_resolution') or '0'
            if live == 'auto' and auto_live_blurs_dof(scaled_output, config):
                live = '0'
                print(f'Output {scaled_output}: live resolution auto: startup patch (depth of field '
                      'blurs live scenes above 1080p; live_resolution=1 forces live changes)')
            if live == 'auto':
                caps = probe.parent / 'bb-gpu-capabilities.exe'
                result = subprocess.run([str(caps), '--live-resolution'], stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                        text=True, creationflags=no_console())
                live = result.stdout.strip() if result.returncode == 0 else '0'
            live = '1' if live == '1' or not patched else '0'
            # Ultrawide and Sharp UI: the camera, HUD and UI viewport need the startup patch
            # (patches.py hud_canvas, ui_viewport).
            output = tuple(int(v) for v in scaled_output.split('x'))
            canvas = ui_viewport(output, read_settings(Path(config)))
            if patched and canvas != OUTPUT_SIZE:
                live = '0'
                env['BB_UI_RES'] = f'{canvas[0]}x{canvas[1]}'
                if hud_canvas(output) != OUTPUT_SIZE:
                    print(f'Output {scaled_output}: ultrawide, HUD {env["BB_UI_RES"]}')
                else:
                    print(f'Output {scaled_output}: Sharp UI, menus and HUD drawn at {env["BB_UI_RES"]}')
        if live == '1' or not scaled_output:
            env.pop('BB_UI_RES', None)
        if live == '1':
            print(f'Output {scaled_output}: live resolution changes (live_resolution=0: startup patch)')
        elif scaled_output:
            env.update(BB_RENDER_RES=scaled_render, BB_OUTPUT_RES=scaled_output, BB_AUTO_RENDER_RES='1')
            env.setdefault('BB_DMEM_MB', '9152')
            print(f'Output {scaled_output}: scene {scaled_render}, direct memory {env["BB_DMEM_MB"]} MiB '
                  '(live_resolution=1: live changes)')
        vram = low_vram_mb(env, probe.parent / 'bb-gpu-capabilities.exe')
        if vram:
            env.setdefault('BB_GC_BUDGET_MB', str(max(2048, vram - VRAM_RESERVE_MB)))
            notes = [f'texture budget {env["BB_GC_BUDGET_MB"]} MiB']
            # At 1080p output the live path keeps the game's targets native; here they shrink too.
            wants_live = (env.get('BB_LIVE_RES') or ini_value(config, 'live_resolution')) == '1'
            if (patched and not scaled_output and not env.get('BB_RENDER_RES') and not wants_live
                    and ini_value(config, 'upscaler') != 'taa'):
                size = run_script('patches.py', '--print-preset-size', '--settings', config, capture=True).strip()
                if size:
                    env.update(BB_RENDER_RES=size, BB_AUTO_RENDER_RES='1')
                    notes.append(f'scene {size} patched at start (live_resolution=1: live changes)')
                    # On this path most prepared draws go unused (145494 of 182272), and on four
                    # cores the workers take time from the game: 53.0 FPS with 4 workers, 55.1
                    # with 2, 56.8 without (minimum 45.3, 48.4, 53.3; i5-11300H, 4 cores, 8
                    # threads). On 8 cores the workers win: 197 FPS without them, 234 with 4.
                    if physical_cores() <= 4 and 'BB_PREP_WORKERS' not in env:
                        env['BB_PREP_WORKERS'] = '0'
                        notes.append('no draw-preparation workers')
            print(f'Low-VRAM profile ({vram} MiB card): {", ".join(notes)}; BB_LOW_VRAM=0 turns it off')
        extra = env.get('BB_PATCHES', '')
        if env.get('BB_ONLINE') == '1' and env.get('BB_NET_BACKEND') == 'thd' and env.get('BB_BRIDGE_LIVE_MAP') == '1':
            extra = ';'.join(filter(None, (extra, LIVE_MAP_PATCH)))
        lang = env.get('BB_LANGUAGE') or read_settings(config).get('language')
        if lang in ('10', '11'):
            if 'Unlock Game Region' not in extra:
                extra = ';'.join(filter(None, (extra, 'Unlock Game Region')))
        run_script('patches.py', '--out', out, '--fps', fps, '--extra', extra,
                   '--settings', config, '--game-dir', game, '--render-res', env.get('BB_RENDER_RES', ''),
                   '--output-res', env.get('BB_OUTPUT_RES', ''),
                   '--patches-dir', env.get('BB_PATCHES_DIR', data / 'patches'),
                   '--patches-config', env.get('BB_PATCHES_CONFIG', data / 'patches.json'))
        if not env.get('BB_VBLANK_HZ'):
            env['BB_VBLANK_HZ'] = {'uncap': '0', '90': '90'}.get(fps, '60')

        # The in-game restart starts this script again once this process is gone.
        # GPU caches (shaders, pipelines) beside the saves; read before main(), so set here.
        user_dir = user_folders(env, data)
        saves_dir = coop_user_dir(env, user_dir, original_game) if seamless_coop(env) else user_dir
        writable_saves(saves_dir)
        if env.get('BB_SAVE_BACKUPS') == '1' and not restarted:  # not again for an in-game restart
            backup_saves(saves_dir)
        if env.get('BB_ONLINE') == '1' and env.get('BB_NET_BACKEND') == 'thd':
            hunters_dream(env, user_dir)
        elif env.get('BB_ONLINE') == '1':
            online_overrides(env, user_dir)
        this = ['--run'] if getattr(sys, 'frozen', False) else [str(Path(__file__).resolve())]
        restart = [sys.executable, *this, '--after', str(os.getpid()), *args]
        env['BB_RESTART_COMMAND'] = subprocess.list2cmdline(restart)
        command = [str(probe), str(out / 'boot-linked.bin'), '--content-profile', str(out / 'content.bin'),
                   '--patches', str(out / 'patches.bin'), '--app0', str(game),
                   '--user', str(saves_dir),
                   '--timeout', env.get('BB_TIMEOUT', '0'), *args]
        game_over = None
        if env.get('BB_THERMAL_LOG') == '1':
            import threading
            game_over = threading.Event()
            threading.Thread(target=thermal_watch, args=(game_over,), daemon=True).start()
        try:
            # No stdin: an inherited pipe (shells such as Git Bash) cost the game its console output.
            if env.get('BB_EXIT_BACKUP') == '1':  # the save as the game left it, also after a crash
                sys.path.insert(0, str(PORT / 'launcher'))
                from bbport_exit_backup import run_with_backup
                return run_with_backup(command, saves_dir, original_game, creationflags=no_console())
            return subprocess.run(command, stdin=subprocess.DEVNULL, creationflags=no_console()).returncode
        finally:
            if game_over:
                game_over.set()
    finally:
        if mod_view:
            # Read-only game files leave read-only links (mods.remove_overlay clears them).
            from mods import remove_overlay
            try:
                remove_overlay(mod_view, original_game)
            except OSError:
                pass


if __name__ == '__main__':
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(130)
