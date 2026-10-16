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


def image_sha256(game):
    """The SHA-256 of eboot.bin's loaded image (its loadable segments at their addresses): the
    same whatever tool dumped it, unlike the SELF around it."""
    import hashlib
    sys.path.insert(0, str(PORT / 'scripts'))
    from prepare import parse_self, span
    elf, _header, ph, _segments, _missing = parse_self((Path(game) / 'eboot.bin').read_bytes())
    loads = [p for p in ph if p['type'] in (1, 0x61000010)]
    image = bytearray(max(p['vaddr'] + p['memsz'] for p in loads))
    for p in loads:
        image[p['vaddr']:p['vaddr'] + p['filesz']] = span(elf, p['offset'], p['filesz'])
    return hashlib.sha256(image).hexdigest()


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


def ini_value(path, key):
    try:
        for line in Path(path).read_text(encoding='utf-8', errors='replace').splitlines():
            match = re.fullmatch(rf'{re.escape(key)}=(.*)', line.strip())
            if match:
                return match.group(1)
    except OSError:
        pass
    return None


def find_executable(name):
    """bin/ (packaged), else out/ (built with build.sh)."""
    for candidate in (PORT / 'bin' / name, PORT / 'out' / name):
        if candidate.is_file():
            return candidate
    return None


def main():
    args = sys.argv[1:]
    if len(args) >= 2 and args[0] == '--after':
        wait_for(int(args[1]))
        args = args[2:]
    # As run.sh: relative paths (BB_GAME_DIR, BB_DATA_DIR, fsr4_shaders/) are from the port.
    os.chdir(PORT)
    env = os.environ
    data = Path(env.get('BB_DATA_DIR', PORT))
    out = data / 'out'
    out.mkdir(parents=True, exist_ok=True)
    env.setdefault('BB_CONFIG', str(data / 'bbport.ini'))
    config = env['BB_CONFIG']
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
    from patches import OUTPUT_SIZE, game_app_version, hud_canvas, output_size, read_settings
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
    # Ultrawide outputs: the menu movies with full-screen backdrops widened and the HUD's corner
    # groups toward the screen edges (bbport.ini hud_position, 0 centred .. 100 edges), a layer
    # below the mods. Only for the
    # 01.09 game, whose camera and HUD canvas the patches widen (patches.py hud_canvas).
    hud_layer = out / 'hud_layout'
    shutil.rmtree(hud_layer, ignore_errors=True)
    canvas = hud_canvas(output_size(read_settings(Path(config))))
    if canvas != OUTPUT_SIZE and (game_app_version(game) in (None, '01.09') or env.get('BB_FORCE_PATCHES')):
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
    view = run_script('mods.py', *mod_args, '--layer', text_layer, '--layer', hud_layer, '--layer', fov_layer,
                      capture=True, optional=True)
    if view is None:
        # The game view could not be made with the port's own layers (Quit to Desktop's texts,
        # the HUD layout); without them the game still starts, as it did before they existed.
        print('Mods: the game view could not be made; starting without Quit to Desktop and the '
              'HUD layout')
        env['BB_QUIT_TO_DESKTOP'] = '0'
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

        # Patches exist for game version 01.09 only (patches.py applies none to others): other
        # versions keep the game's 30 FPS timing and change resolutions live.
        sys.path.insert(0, str(PORT / 'scripts'))
        from patches import eboot_matches_patches, game_app_version
        version = game_app_version(game)
        forced = bool(env.get('BB_FORCE_PATCHES'))
        patched = version in (None, '01.09') or forced
        if not patched:
            print(f'Game version {version}: community patches need 01.09; 30 FPS, no effect patches')
        elif not forced and not eboot_matches_patches(out / 'eboot.elf'):
            # param.sfo says 01.09, the code is another build's: the patches would break it.
            patched = False
            print('Game files: eboot.bin is not the 01.09 build the community patches were made for; '
                  '30 FPS, no effect patches')
        # Sizes chosen below for the previous launch are recomputed after an in-game restart.
        if env.get('BB_AUTO_RENDER_RES') == '1':
            for key in ('BB_RENDER_RES', 'BB_OUTPUT_RES', 'BB_AUTO_RENDER_RES'):
                env.pop(key, None)
        fps = env.get('BB_FPS', 'uncap') if patched else '30'
        scaled_render = scaled_output = None
        if not env.get('BB_RENDER_RES'):
            printed = run_script('patches.py', '--print-scaled', '--settings', config, capture=True).split()
            if len(printed) == 2:
                scaled_render, scaled_output = printed
        live = '0'
        if scaled_output:
            live = env.get('BB_LIVE_RES') or ini_value(config, 'live_resolution') or '0'
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
        # No stdin: an inherited pipe (shells such as Git Bash) cost the game its console output.
        return subprocess.run(command, stdin=subprocess.DEVNULL, creationflags=no_console()).returncode
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
