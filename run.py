#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Windows counterpart of run.sh: prepares the game image and starts bb-probe.exe.

Same environment variables as run.sh (BB_GAME_DIR, BB_DATA_DIR, BB_FPS, BB_RENDER_RES,
BB_LIVE_RES, BB_PATCHES, BB_MODS_*, BB_USER_DIR, ...). Extra arguments go to bb-probe.
The in-game "Apply and restart" runs this script again through BB_RESTART_COMMAND;
`--after PID` waits for the previous game process to end first (its GPU device and memory).
"""
import ctypes
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time

PORT = Path(__file__).resolve().parent


# Co-op: the community service that answers Bloodborne's own game-server requests
# (messages, bloodstains, sign-in); summon requests go to the shadNet server instead.
COMMUNITY_SERVER = 'https://thehuntersdream.com'
GAME_SERVER = 'https://ss4.scej-network.jp:20443'
# The server's ss.info sends the game to this port for its API (summons among them).
COMMUNITY_API_PORT = 18671


def online_overrides(env, user_dir):
    """Writes user/host_overrides.json for this launch. A player who needs other addresses
    puts them in user/host_overrides.custom.json, which is then used as it is."""
    custom = user_dir / 'host_overrides.custom.json'
    path = custom if custom.exists() else user_dir / 'host_overrides.json'
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
    community = (env.get('BB_COMMUNITY_SERVER') or COMMUNITY_SERVER).rstrip('/')
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


def run_script(name, *args, capture=False, optional=False):
    """Runs scripts/<name>. optional: a failure returns False instead of stopping the launch."""
    # Inside the packaged Bloodborne.exe (PyInstaller) the executable runs scripts itself.
    script = ['--script'] if getattr(sys, 'frozen', False) else []
    command = [sys.executable, *script, str(PORT / 'scripts' / name), *map(str, args)]
    if optional and not capture:
        try:
            return subprocess.run(command, stdin=subprocess.DEVNULL, creationflags=no_console()).returncode == 0
        except OSError:
            return False
    if capture:
        result = subprocess.run(command, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, text=True,
                                creationflags=no_console())
        if result.returncode:
            if optional:
                return None
            sys.exit(result.returncode)
        return result.stdout
    # stdin given: the output handles are passed explicitly (a windowed Bloodborne.exe child would
    # get none otherwise).
    result = subprocess.run(command, stdin=subprocess.DEVNULL, creationflags=no_console())
    if result.returncode:
        sys.exit(result.returncode)
    return None


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
        for line in Path(config).read_text(encoding='utf-8').splitlines():
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
        values.get('upscaler', 'fsr4') in ('fsr3', 'fsr4', 'fsr411')
    env['BB_OBJECT_MOTION_CACHE'] = '1' if cache else '0'


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
LOW_VRAM_MB = 4608
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


def main():
    args = sys.argv[1:]
    restarted = len(args) >= 2 and args[0] == '--after'
    if restarted:
        wait_for(int(args[1]))
        args = args[2:]
    # As run.sh: relative paths (BB_GAME_DIR, BB_DATA_DIR, fsr4_shaders/) are from the port.
    os.chdir(PORT)
    env = os.environ
    note = files_note(PORT)
    if note:
        print(note)
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
                         image_groups, output_size, read_settings)
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
    text_layer = out / 'quit_text'
    shutil.rmtree(text_layer, ignore_errors=True)
    if ini_value(config, 'quit_to_desktop') != '0' and (game_app_version(game) in (None, '01.09') or env.get('BB_FORCE_PATCHES')):
        # Optional: when its files cannot be written ("[WinError 5] Access is denied", an
        # antivirus or the folder's permissions), the game starts without it.
        if not run_script('quit_menu.py', game, '--out', text_layer, optional=True):
            shutil.rmtree(text_layer, ignore_errors=True)
            env['BB_QUIT_TO_DESKTOP'] = '0'  # patches.py: no menu entries without their texts
            print('Quit to Desktop: its menu texts could not be written; the game starts without it')
    # Rebirth at the Doll (bbport.ini rebirth, patches.py): the Doll's talk script and its texts
    # (on top of Quit to Desktop's, which edit the same menu text archives), a layer below the mods.
    rebirth_layer = out / 'rebirth'
    shutil.rmtree(rebirth_layer, ignore_errors=True)
    if ini_value(config, 'rebirth') != '0' and (game_app_version(game) in (None, '01.09') or env.get('BB_FORCE_PATCHES')):
        if not run_script('rebirth.py', game, '--out', rebirth_layer, '--source', text_layer, optional=True):
            shutil.rmtree(rebirth_layer, ignore_errors=True)
            env['BB_REBIRTH'] = '0'  # patches.py: no eboot hooks without the Doll's script
            print("Rebirth: the Doll's script could not be written; the game starts without it")
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
                      '--layer', fov_layer, capture=True, optional=True)
    if view is None:
        # The game view could not be made with the port's own layers (Quit to Desktop's texts,
        # the HUD layout); without them the game still starts, as it did before they existed.
        print('Mods: the game view could not be made; starting without Quit to Desktop and the '
              'HUD layout')
        env['BB_QUIT_TO_DESKTOP'] = '0'
        env['BB_REBIRTH'] = '0'
        view = run_script('mods.py', *mod_args, capture=True)
    game = Path(view.strip())
    mod_view = game if game.resolve() != original_game else None
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
            # Ultrawide: the camera and HUD need the startup patch (patches.py hud_canvas).
            from patches import OUTPUT_SIZE, hud_canvas
            canvas = hud_canvas(tuple(int(v) for v in scaled_output.split('x')))
            if patched and canvas != OUTPUT_SIZE:
                live = '0'
                env['BB_UI_RES'] = f'{canvas[0]}x{canvas[1]}'
                print(f'Output {scaled_output}: ultrawide, HUD {env["BB_UI_RES"]}')
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
        run_script('patches.py', '--out', out, '--fps', fps, '--extra', extra,
                   '--settings', config, '--game-dir', game, '--render-res', env.get('BB_RENDER_RES', ''),
                   '--output-res', env.get('BB_OUTPUT_RES', ''),
                   '--patches-dir', env.get('BB_PATCHES_DIR', data / 'patches'),
                   '--patches-config', env.get('BB_PATCHES_CONFIG', data / 'patches.json'))
        if not env.get('BB_VBLANK_HZ'):
            env['BB_VBLANK_HZ'] = {'uncap': '0', '90': '90'}.get(fps, '60')

        # The in-game restart starts this script again once this process is gone.
        # GPU caches (shaders, pipelines) beside the saves; read before main(), so set here.
        user_dir = Path(env.get('BB_USER_DIR', data / 'user')).resolve()
        user_dir.mkdir(parents=True, exist_ok=True)
        env.setdefault('BB_GPU_USER_DIR', str(user_dir))
        if env.get('BB_SAVE_BACKUPS') == '1' and not restarted:  # not again for an in-game restart
            backup_saves(user_dir)
        if env.get('BB_ONLINE') == '1' and env.get('BB_NET_BACKEND') == 'thd':
            hunters_dream(env, user_dir)
        elif env.get('BB_ONLINE') == '1':
            online_overrides(env, user_dir)
        this = ['--run'] if getattr(sys, 'frozen', False) else [str(Path(__file__).resolve())]
        restart = [sys.executable, *this, '--after', str(os.getpid()), *args]
        env['BB_RESTART_COMMAND'] = subprocess.list2cmdline(restart)
        command = [str(probe), str(out / 'boot-linked.bin'), '--content-profile', str(out / 'content.bin'),
                   '--patches', str(out / 'patches.bin'), '--app0', str(game),
                   '--user', str(user_dir),
                   '--timeout', env.get('BB_TIMEOUT', '0'), *args]
        game_over = None
        if env.get('BB_THERMAL_LOG') == '1':
            import threading
            game_over = threading.Event()
            threading.Thread(target=thermal_watch, args=(game_over,), daemon=True).start()
        try:
            # No stdin: an inherited pipe (shells such as Git Bash) cost the game its console output.
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
