#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Bloodborne launcher for Windows (Tkinter; launcher/bbport_launcher.py is the Linux one).

Every setting of the port in one window: the game folder and saves, bbport.ini (upscaler,
preset, output, effects), start-up options passed to run.py as environment variables
(frame rate, presentation, HDR, ...), mods, third-party patches and the FSR 4 assets.
Launcher options live in %APPDATA%/bbport-launcher/settings.json.

Frozen with PyInstaller (packaging/windows/package.sh) the same Bloodborne.exe also runs the
game without the window (`--play`), run.py (`--run`) and the preparation scripts (`--script`),
so a packaged port needs no Python installation.
"""
import ctypes
import gc
import json
import os
from pathlib import Path
import queue
import re
import runpy
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import urllib.request
import webbrowser
import zipfile

FROZEN = getattr(sys, 'frozen', False)
PORT_DIR = Path(sys.executable).resolve().parent if FROZEN else Path(__file__).resolve().parent.parent
sys.path.insert(0, str(PORT_DIR / 'scripts'))
import fetch_fsr4vk
DATA_DIR = Path(os.environ.get('BB_DATA_DIR', PORT_DIR))
# Co-op test build: its own settings, and no updates (a public release would replace it).
COOP_TEST = False
CONFIG_DIR = Path(os.environ.get('APPDATA', Path.home())) / ('bbport-launcher-coop' if COOP_TEST else 'bbport-launcher')
CONFIG_FILE = CONFIG_DIR / 'settings.json'
PATCH_VERSION = '01.09'
MAX_LOG_LINES = 6000
NO_WINDOW = getattr(subprocess, 'CREATE_NO_WINDOW', 0)
# This build; GitHub release tags are windows-v<VERSION>.
VERSION = '1.9.7'
# A beta of VERSION (its number), or 0: shown as 'v1.9.5 beta 1', and the release of VERSION itself is
# offered as its update.
BETA = 0
VERSION_TEXT = f'{VERSION} beta {BETA}' if BETA else VERSION
BETA_PAGE = 'https://github.com/emurphy04/bloodborne_pc/releases/tag/v1.9.5-beta.10'
# The port's Discord application (discord.com/developers: its Application ID; an art asset named
# "bloodborne" is the picture). Empty: no Discord status, and its setting is not shown.
DISCORD_APPLICATION_ID = '1563260197835907232'
# Co-op: the community server that answers the game's own server requests.
COMMUNITY_SERVER = 'https://game.supermedo.com'
# The Hunter's Dream, for its own pool (off since 1.9.7; BB_HUNTERS_DREAM=1 shows it again): its sign-in, player
# count and game server.
HUNTERS_DREAM_SERVER = 'https://thehuntersdream.com'
# SuperMedo Server: the shadNet server (accounts, bells and summons) and the game server above,
# both supermedo.com's own.
SUPERMEDO_SERVER, SUPERMEDO_WEBAPI = 'np.supermedo.com:31313', 'http://np.supermedo.com:31315'
RELEASES_API = 'https://api.github.com/repos/Supermedo/bloodborne_pc/releases/latest'
RELEASES_PAGE = 'https://github.com/Supermedo/bloodborne_pc/releases/latest'
# The community site (install guide, FAQ, forum), linked from the side bar.
COMMUNITY_SITE = 'https://bbport.de/'
RELEASES_LIST_API = 'https://api.github.com/repos/Supermedo/bloodborne_pc/releases?per_page=100'
# The betas (pre-releases v<VERSION>-beta.<N>): a beta asks here too, for the next one (update_to_offer).
BETAS_API = 'https://api.github.com/repos/emurphy04/bloodborne_pc/releases?per_page=30'
# The oldest version Advanced > Other versions offers: older ones cannot read today's settings. (A copy of
# this launcher installs it, whatever version: update_installer.)
OLDEST_INSTALLABLE = '1.8.2'
# The downloads, one folder per update (install_version); cleared at a start without an update underway.
UPDATE_DIR = Path(tempfile.gettempdir()) / 'bbport-update'
# Never put in place by an update (bbport_update.stage), whatever the package holds.
USER_FILES = ('user', 'out', 'mods', 'fsr4vk', 'bbport.ini', 'input.ini', 'mods.json', 'patches.json', 'last_run.log')
# The GitHub asset's digest ("sha256:…") by download URL, noted by game_zip: the download is checked against it.
ASSET_DIGESTS = {}


# ---------------------------------------------------------------------------------------------
# Command line roles of the frozen executable.

def attach_stdio():
    """A windowed executable starts without sys.stdout; inherited pipes or a console still exist."""
    import msvcrt
    for name, std in (('stdout', -11), ('stderr', -12)):
        if getattr(sys, name) is not None:
            continue
        stream = None
        handle = ctypes.windll.kernel32.GetStdHandle(std)
        if handle and handle != ctypes.c_void_p(-1).value:
            try:
                stream = open(msvcrt.open_osfhandle(handle, os.O_WRONLY), 'w', encoding='utf-8',
                              errors='replace', buffering=1)
            except OSError:
                stream = None
        setattr(sys, name, stream or open(os.devnull, 'w'))


class LossyStream:
    """Output whose reader may be gone (a launcher that was closed): on Windows a write to such a
    pipe fails with OSError [Errno 22], which stopped the game while it was being prepared."""

    def __init__(self, stream):
        self.stream = stream

    def write(self, text):
        try:
            return self.stream.write(text)
        except (OSError, ValueError):
            return len(text)

    def flush(self):
        try:
            self.stream.flush()
        except (OSError, ValueError):
            pass

    def __getattr__(self, name):
        return getattr(self.stream, name)


def run_role(argv):
    """--run [args]: run.py; --script <file> [args]: a preparation script. Returns an exit code."""
    attach_stdio()
    for stream in (sys.stdout, sys.stderr):  # keep messages in order with the game's output
        try:
            stream.reconfigure(line_buffering=True)
        except (AttributeError, ValueError):
            pass
    sys.stdout, sys.stderr = LossyStream(sys.stdout), LossyStream(sys.stderr)
    if argv[0] == '--script':
        path, sys.argv = argv[1], argv[1:]
    else:
        path = str(PORT_DIR / 'run.py')
        sys.argv = [path, *argv[1:]]
    try:
        runpy.run_path(path, run_name='__main__')
    except SystemExit as stop:
        return stop.code if isinstance(stop.code, int) else (0 if stop.code is None else 1)
    return 0


# One launcher window at a time (one_launcher): every installation shares its settings, and two windows would
# overwrite each other's and could start two games. A second start brings the open window to the front.
ONE_LAUNCHER = 'Local\\bbport-launcher'
TO_FRONT = 'Local\\bbport-launcher-front'


def one_launcher(name=ONE_LAUNCHER, front=TO_FRONT):
    """True for the first launcher window (it holds a named mutex while it runs); a second start asks that one to
    come to the front (bring_to_front_on_request) and gets False."""
    from ctypes import wintypes
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    kernel.CreateMutexW.restype = kernel.OpenEventW.restype = wintypes.HANDLE
    global _ONE_LAUNCHER_HANDLE
    _ONE_LAUNCHER_HANDLE = kernel.CreateMutexW(None, False, name)
    if ctypes.get_last_error() != 183:  # ERROR_ALREADY_EXISTS
        return True
    event = kernel.OpenEventW(0x0002, False, front)  # EVENT_MODIFY_STATE
    if event:
        kernel.SetEvent(wintypes.HANDLE(event))
        kernel.CloseHandle(wintypes.HANDLE(event))
    return False


def bring_to_front_on_request(on_request, front=TO_FRONT):
    """Calls on_request (from a helper thread) each time another start asks for the open window."""
    from ctypes import wintypes
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    kernel.CreateEventW.restype = wintypes.HANDLE
    event = kernel.CreateEventW(None, False, False, front)
    if not event:
        return None

    def wait():
        while kernel.WaitForSingleObject(wintypes.HANDLE(event), 0xFFFFFFFF) == 0:
            on_request()
    threading.Thread(target=wait, daemon=True).start()
    return event


def wait_for_exit(pid, seconds=15):
    """--after PID: the launcher this one replaces (Apply restarts it) closes first, with its window."""
    kernel = ctypes.windll.kernel32
    handle = kernel.OpenProcess(0x00100000, False, int(pid))  # SYNCHRONIZE
    if handle:
        kernel.WaitForSingleObject(handle, seconds * 1000)
        kernel.CloseHandle(handle)


# run.py holds this named mutex while it prepares and runs the game (run.py GAME_MUTEX).
GAME_MUTEX = 'Local\\bbport-game'


def game_underway(mutex=GAME_MUTEX):
    """Whether a game is being prepared or runs: from this launcher, Play Bloodborne.exe (Steam, a shortcut) or
    the in-game restart. A second one would share its saves and prepared files."""
    kernel = ctypes.windll.kernel32
    kernel.OpenMutexW.restype = ctypes.c_void_p
    handle = kernel.OpenMutexW(0x00100000, False, mutex)  # SYNCHRONIZE
    if handle:
        kernel.CloseHandle(ctypes.c_void_p(handle))
        return True
    return bool(bench.game_pids())


def front_game_window():
    """The running game's window to the front (restored when minimized); False when it has none yet (still
    being prepared)."""
    user32 = ctypes.windll.user32
    for pid in bench.game_pids():
        hwnd = bench.game_window(pid)
        if hwnd:
            if user32.IsIconic(hwnd):
                user32.ShowWindow(hwnd, 9)  # SW_RESTORE
            user32.SetForegroundWindow(hwnd)
            return True
    return False


def run_command():
    """The command that starts run.py: this executable when frozen, else Python."""
    return [sys.executable, '--run'] if FROZEN else [sys.executable, str(PORT_DIR / 'run.py')]


# ---------------------------------------------------------------------------------------------
# Languages: every text is written in English with the Russian next to it; the other
# languages are in bbport_lang.py, keyed by the English text.

sys.path.insert(0, str(Path(__file__).resolve().parent))
import bbport_lang  # noqa: E402
import bbport_fg401  # noqa: E402
import bbport_fsr411  # noqa: E402
import bbport_fsr412  # noqa: E402
import bbport_fsr41
import bbport_fsr41_ui
import bbport_character as character  # noqa: E402
import bbport_tray  # noqa: E402
import bbport_lang_setup  # noqa: E402,F401 (the setup guide's texts)
import bbport_lang_help  # noqa: E402,F401 (the Help page's and the game tests' texts)
import bbport_lang_ui  # noqa: E402,F401 (the regrouped pages, the side bar, the guide)
import bbport_lang_world  # noqa: E402,F401 (the new texts in the other eleven languages)
import bbport_lang_port  # noqa: E402,F401 (the 1.9.3 texts and the Controls page's key names)
import bbport_lang_195  # noqa: E402,F401 (the 1.9.5 texts: the shadNet server that turns the port away)
import bbport_lang_pools  # noqa: E402,F401 (the co-op pools' texts with The Hunter's Dream back)
import bbport_lang_mistakes  # noqa: E402,F401 (the repairs of player mistakes, in the other eleven languages)
import bbport_lang_vram  # noqa: E402,F401 (the video memory warning's switch)
import bbport_lang_latency  # noqa: E402,F401 (the low latency options)
import bbport_lang_gpu  # noqa: E402,F401 (a graphics card Windows has and Vulkan does not see)
import bbport_lang_ready  # noqa: E402,F401 (the start's other prerequisites: runtime, folders, firewall)
import bbport_lang_audit1  # noqa: E402,F401 (the launcher's own update: its checks and the installer's window)
import bbport_lang_audit2  # noqa: E402,F401 (the wrong and stale texts of the launcher check, rewritten)
import bbport_lang_audit3  # noqa: E402,F401 (background work that no longer ends without a word, the shader cache)
import bbport_lang_audit4  # noqa: E402,F401 (the search: the side bar's buttons, notes for other choices)
import bbport_lang_audit5  # noqa: E402,F401 (Help's new order: asking for help above the problems)
import bbport_lang_audit8  # noqa: E402,F401 (sharpening with FSR 4, achievements from another installation)
import bbport_lang_news  # noqa: E402,F401 (the What's new window)
import bbport_lang_thd  # noqa: E402,F401 (1.9.7: The Hunter's Dream no longer offered)
import bbport_lang_readouts  # noqa: E402,F401 (the FPS counter's lines)
# Last of the texts: it builds bbport_lang's tables (table()) to add its own, so a module registering
# EXTRA_TRANSLATIONS after it would not be in them.
import bbport_fsr41_translations  # noqa: E402,F401
import bbport_hardware as hardware  # noqa: E402
import bbport_frame  # noqa: E402
import bbport_display as display  # noqa: E402
import bbport_saves as saves  # noqa: E402
import bbport_exit_backup  # noqa: E402,F401 (bundled for run.py)
import bbport_benchmark as bench  # noqa: E402 (the game's process and window)
import bbport_pad  # noqa: E402
import bbport_help  # noqa: E402
import bbport_search  # noqa: E402
import bbport_setup  # noqa: E402
import bbport_news  # noqa: E402
import bbport_trophies as trophies  # noqa: E402
import bbport_theme as theme  # noqa: E402
import bbport_tls  # noqa: E402 (every HTTPS request: no expired certificates of the stores, Windows' trust)
import bbport_update as updater  # noqa: E402 (the launcher's own update: checked, swapped in by renaming)
from bbport_trophy_icons import TrophyIcons  # noqa: E402
from bbport_input_config import (STICK_OUTPUTS, OUTPUT_LABELS, ACTION_GROUPS, SLOTS, keysym_to_name,
                                  button_num_to_name, input_ini_path, load_input_ini, reserved_keys,
                                  save_input_ini, save_mouse_settings, wheel_to_name, MAX_COMBO,
                                  MODIFIER_NAMES, apply_layout, slot_conflicts, binding_parts,
                                  display_name, CONTROLLER_OUTPUTS, DEFAULT_CONTROLLER,
                                  controller_conflicts, controller_display_name,
                                  load_controller_bindings, save_controller_bindings,
                                  keyboard_layout, windows_mouse_poller)  # noqa: E402
import bbport_gamepad  # noqa: E402
import bbport_respec as respec  # noqa: E402
import bbport_textfile as textfile  # noqa: E402

LANG = 'en'


WARN = '  ⚠'  # marks a risky choice; the text before it is translated as usual


def _(en, ru=None):
    if en.endswith(WARN):
        return _(en[:-len(WARN)], ru and ru.removesuffix(WARN)) + WARN
    if LANG == 'ru':
        return ru or bbport_lang.table('ru').get(en) or en
    return bbport_lang.table(LANG).get(en) or en


def renamed(text):
    """A translated text that names the page with the game folder by an older name: Game & effects
    (the original launcher) or Game & language (until the folders moved to the expert page Game
    files; the game's languages are on Gameplay)."""
    return text.replace(_('Game & effects', 'Игра и эффекты'), _('Game files')).replace(
        _('Game & language'), _('Game files'))


def windows_language():
    try:
        primary = ctypes.windll.kernel32.GetUserDefaultUILanguage() & 0x3ff
    except (AttributeError, OSError):
        return 'en'
    return bbport_lang.WINDOWS_LANGUAGES.get(primary, 'en')


# ---------------------------------------------------------------------------------------------
# Settings. bbport.ini keys (the game reads them, the in-game menu edits them) and the
# launcher's own settings.json (passed to run.py as environment variables).

EFFECTS = [
    ('effect_chromatic_aberration', ('Chromatic aberration', 'Хроматическая аберрация'), True),
    ('effect_dof', ('Depth of field', 'Глубина резкости (DoF)'), True),
    ('effect_motion_blur', ('Motion blur', 'Размытие в движении'), True),
    ('effect_ssao', ('Ambient occlusion (SSAO)', 'Затенение SSAO'), True),
    ('effect_game_aa', ("The game's own anti-aliasing", 'Собственное сглаживание игры'), True),
    ('effect_dynamic_shadows', ('Shadows of dynamic lights', 'Тени от динамических источников'), True),
    ('effect_ssr', ('Screen-space reflections (not in the original)', 'Отражения SSR (не было в игре)'), False),
]
EXTRAS = [
    ('skip_intro', ('Skip the intro logos and movie', 'Пропуск заставок при запуске'), False),
    ('debug_camera', ('Free camera (hold Cross + L3; keyboard Space + Z)', 'Свободная камера (Cross + L3 / Space + Z)'), False),
    ('debug_menu', ('Game debug menu (left touchpad / Tab; needs the debug fonts)',
                    'Debug menu (левый touchpad / Tab; нужны шрифты)'), False),
]
# Cheats and gameplay tweaks: game patches for 1.09 (patches.py EFFECTS), off by default.
CHEATS = [
    ('cheat_no_death', ('Never die (health stops at 1 HP)', 'Бессмертие (здоровье не ниже 1 HP)'), False),
    ('cheat_stealth', ('Enemies do not see you (unless attacked)', 'Враги не видят вас (пока не атакованы)'), False),
    ('cheat_silent', ('Enemies do not hear you', 'Враги не слышат вас'), False),
    ('cheat_rally_no_decay', ('Rally never fades', 'Rally не угасает'), False),
    ('cheat_enemy_control', ('Control the targeted enemy (R3; L3 to go back; not with the free camera)',
                             'Управление выбранным врагом (R3; L3 — назад; не вместе со свободной камерой)'), False),
]
TWEAKS = [
    ('tweak_no_rally', ('No Rally (hits do not give health back)', 'Без Rally (удары не возвращают здоровье)'), False),
    ('tweak_camera_distance', ('Camera further from the character', 'Камера дальше от персонажа'), False),
    ('tweak_no_camera_rotation', ('No camera auto-rotation while moving', 'Без автоповорота камеры при движении'), False),
    ('tweak_easy_run', ('Run with less stick tilt (about 70%)', 'Бег при меньшем наклоне стика (около 70%)'), False),
    ('tweak_ragdoll', ('Dark Souls-style ragdoll physics (corpses fly further)',
                       'Физика тел как в Dark Souls (тела отлетают дальше)'), False),
    ('tweak_circle_confirm', ('Japanese/Asian buttons: Circle confirms and interacts, Cross dodges',
                              'Японская раскладка: Круг — подтверждение и действие, Крест — уклонение'), False),
    ('quit_to_desktop', ('Quit to Desktop in the title menu and in the System menu',
                         'Пункт «Выйти на рабочий стол» в главном меню и в меню «Система»'), True),
    ('pc_menu', ('PC Settings in the System menu: graphics, frame rate, effects and camera in the game',
                 'Пункт «Настройки ПК» в меню «Система»: графика, частота кадров, эффекты и камера в игре'), True),
    ('rebirth', ("Rebirth at the Doll: back to your origin's attributes for 5 Insight, levels regained free",
                 'Перерождение у Куклы: характеристики происхождения за 5 озарений, уровни возвращаются бесплатно'), True),
    ('steady_cloth', ('Cloth as on PS4 at higher frame rates: capes and coats hang and move as at 30 FPS',), True),
]
# The FPS counter's lines (gpu/shim/bbport_overlay.cpp FpsCounter), one switch each in bbport.ini; FPS alone on
# by default. Render latency is its own line: under FPS it was taken for the frame time.
READOUTS = [('show_fps', ('FPS',)), ('show_frame_times', ('Frame time',)), ('show_latency', ('Render latency',)),
            ('show_vram', ('Video memory (VRAM)',)), ('show_gpu', ('GPU usage',)), ('show_cpu', ('CPU usage',)),
            ('show_ram', ('RAM usage',)), ('show_fps_graph', ('Frame time graph',))]
# The counter's values: green / amber / red by how they stand, or plain grey (bbport_overlay.cpp PlainCounter).
COUNTER_COLORS = [('1', ('Colour-coded (green, orange, red)',)), ('0', ('Plain (grey)',))]
# Effects that are taste, not performance: on Picture & performance (the others on All graphics).
TASTE_EFFECTS = ('effect_chromatic_aberration', 'effect_dof', 'effect_motion_blur')
INI_FLAGS = {'sharpen', 'object_motion', 'object_motion_cache', 'fsr_compatibility_options', 'show_fps', 'mute_unfocused', 'frame_gen_fit_display',
             'sharp_ui', *(key for key, _t in READOUTS),
             *(k for k, _t, _o in EFFECTS + EXTRAS + CHEATS + TWEAKS)}
INI_DEFAULTS = {'upscaler': 'fsr3', 'preset': '1', 'sharpen': '1', 'sharpness': '0.50',
                'object_motion': '1', **{key: '0' for key, _t in READOUTS}, 'show_fps': '1', 'counter_colors': '1',
                'output_res': '1920x1080', 'model_lod': '0',
                'fsr411_backend': 'vk', 'fsr41_version': 'fsr412', 'object_motion_cache': '0', 'fsr_compatibility_options': '0',
                'live_resolution': 'auto', 'hud_position': '100', 'sharp_ui': '0', 'fov': '43', 'mute_unfocused': '0',
                'button_prompts': 'auto',
                'frame_gen': 'off', 'frame_gen_multiplier': '2', 'frame_gen_fit_display': '0',
                **{key: '1' if on else '0' for key, _t, on in EFFECTS + EXTRAS + CHEATS + TWEAKS}}
APP_DEFAULTS = {'ui_language': '', 'game_dir': str(PORT_DIR.parent / 'CUSA03173'), 'user_dir': '', 'cache_dir': '',
                'dlc_dir': '',
                'mods_dir': '', 'mods_enabled': True, 'patches_dir': '', 'language': '1',
                'voice_language': 'auto',
                'player_name': '', 'fullscreen': False, 'exclusive_fullscreen': False, 'hdr': False, 'present_mode': 'Mailbox',
                'fps_mode': 'uncap', 'frame_cap': '', 'draw_pipe': '', 'readbacks': '', 'low_vram': '', 'vram_warning': True,
                'frames_ahead': '', 'low_latency': 'off', 'async_pipelines': '', 'vrr': '', 'early_submit': '', 'frame_stats': False, 'gpu_profile': False,
                'vk_validation': False, 'extra_env': '', 'close_on_play': False,
                'on_play': 'stay', 'reopen_after_game': False, 'minimize_to_tray': False, 'discord_status': True,
                'menu_key': 'Insert', 'menu_pad': 'l3+r3',
                'check_updates': True, 'save_backups': False, 'online': False, 'online_server': 'srv.shadps4.net:31313',
                'online_webapi': '', 'online_npid': '', 'online_password': '', 'online_upnp': True,
                'online_custom_servers': False, 'supermedo_npid': '', 'supermedo_pass': '',
                'online_community': COMMUNITY_SERVER, 'online_pool': 'supermedo', 'bridge_live_map': False,
                'thd_online_id': '', 'thd_token': '',
                # The setup guide ran (or the launcher was set up before it existed); a graphics card
                # chosen by hand when the detected one is not in the hardware check's list.
                'setup_done': False, 'gpu_override': '',
                # Graphics settings made for the hardware (bbport_setup.auto_settings): on/off, the
                # hardware they were made for, and what they replaced (Undo); the settings right
                # after they were applied (a later difference is the player's own) and the hardware
                # their upscaler was chosen for (one chosen by hand for another card is no choice for a
                # new one); the graphics cards this PC has shown (a laptop's two are no new hardware when
                # they take turns).
                'auto_graphics': True, 'auto_hw': '', 'auto_undo': {}, 'auto_applied': {}, 'auto_applied_hw': '',
                'auto_gpus': [],
                'bench_goal': '60',  # the performance test's frame rate goal
                'eboot_check': {},
                # The launcher's view: simple (the pages a player needs) or expert (every page); empty
                # until decided at the first start (bbport_launcher_win.Launcher.__init__).
                'ui_mode': '',
                # The player's goal (Play page): smooth, balanced or quiet (bbport_hardware.GOALS).
                'goal': 'smooth',
                # What the player plays with: 'controller' or 'keyboard' (and mouse); empty until
                # chosen, then taken from mouse look (Launcher.__init__).
                'play_with': '',
                # A hint of what lies behind the window (window_chrome); off by default, easiest to read.
                # A new key: the first test build saved 'translucent' on for everyone.
                'translucent_window': False,
                'windows_title_bar': False,  # Windows' caption instead of the launcher's own (bbport_frame)
                # The game's window (bbport_display): its monitor ('' = the main one), its size in a
                # window ('output', 'last' or 'custom'), the player's own size and the last one it had.
                'game_monitor': '', 'window_size': 'output', 'window_custom': '1600x900', 'window_last': '',
                'output_display': False,  # output resolution: the game monitor's own, set at every start
                'auto_backup': True,  # the saves backed up before every start (bbport_saves.auto_backup)
                'exit_backup': False,  # and after the game exits (run.py, bbport_exit_backup)
                'seen_version': '',  # the version (VERSION_TEXT) whose What's new was shown (bbport_news)
                # Game tests (System & performance): the kind, the scenario, its options.
                'test_mode': 'performance', 'test_scenario': 'hunters-view', 'test_compare': 'preset',
                'test_repeats': '1', 'test_minutes': '10', 'test_saves': {}}  # the last check of eboot.bin (bbport_help), kept while the file is unchanged
# bbbridge's own app reads and writes the online_* keys above: keep their names.

UPSCALERS = [('dlss', ('DLSS (NVIDIA GeForce RTX)',)),
             ('fsr4', ('FSR 4 (best quality)', 'FSR 4 (лучшее качество)')),
             ('fsr412', ('FSR 4.1.2 (experimental)',)),
             ('fsr411', ('FSR 4.1.1 (experimental)', 'FSR 4.1.1 (экспериментально)')),
             ('fsr3', ('FSR 3.1 (every GPU)', 'FSR 3.1 (любая видеокарта)')),
             ('taa', ('TAA (native resolution anti-aliasing)', 'TAA (нативное сглаживание)')),
             ('off', ('Off', 'Выключен'))]
# Frames generated between two of the game's (vk_frame_generation.cpp).
FRAME_GENERATION = [('off', ('Off', 'Выключена')),
                    ('fsr3', ('AMD FSR 3.1 Frame Generation (any GPU)', 'AMD FSR 3.1 (любая видеокарта)')),
                    ('fsr401', ('AMD ML-FG 4.0.1 (RX 9000, 2× SDR, experimental)',)),
                    ('fsr402', ('AMD ML-FG 4.0.2 (RX 9000, 2× SDR, experimental)',)),
                    ('dlss', ('NVIDIA DLSS Frame Generation (GeForce RTX 40/50)',
                              'NVIDIA DLSS (GeForce RTX 40/50)'))]
FRAME_GEN_MULTIPLIERS = [('2', ('2× (one generated frame)', '2× (один кадр)')),
                         ('3', ('3× (DLSS on GeForce RTX 50)', '3× (DLSS на GeForce RTX 50)')),
                         ('4', ('4× (DLSS on GeForce RTX 50)', '4× (DLSS на GeForce RTX 50)'))]
PRESETS = [('0', ('Native AA (×1.0)',)), ('1', ('Quality (×1.5)',)), ('2', ('Balanced (×1.7)',)),
           ('3', ('Performance (×2)',)), ('4', ('Ultra Performance (×3)',))]
FSR411_BACKENDS = [('vk', ('Vulkan (fsr4vk)',)), ('dx12', ('AMD DX12',))]
SHARED_GRAPHICS_KEYS = ('fsr411_backend', 'object_motion_cache')
# Launcher settings the game's PC Settings page also changes (bbport_pc_menu.cpp): kept in
# settings.json as before and mirrored to bbport.ini, where the newer value is.
MIRRORED_KEYS = ('fps_mode', 'frame_cap')


def mirrored(app, ini):
    """`app` with the mirrored settings bbport.ini holds (the game may have changed them)."""
    return {**app, **{key: ini[key] for key in MIRRORED_KEYS if key in ini}}


def fsr411_backend(settings):
    # The persisted value is independent of the translated display label.
    return 'dx12' if settings.get('fsr411_backend') == 'dx12' else 'vk'


def native_fsr411_present():
    # Packaged beside bin/bb-probe.exe; a source build copies it to out/ (docs/FSR411_WINDOWS.md).
    return any(dll.is_file() and dll.stat().st_size > 0
               for dll in (PORT_DIR / folder / 'amd_fidelityfx_upscaler_dx12.dll' for folder in ('bin', 'out')))


OUTPUTS = [('1280x720', ('1280 × 720 (Steam Deck)',)), ('1920x1080', ('1920 × 1080',)),
           ('2560x1440', ('2560 × 1440',)), ('3840x2160', ('3840 × 2160 (4K)',)),
           ('2560x1080', ('2560 × 1080 (21:9)',)), ('3440x1440', ('3440 × 1440 (21:9)',)),
           ('3840x1600', ('3840 × 1600 (21:9)',)), ('5120x2160', ('5120 × 2160 (21:9)',)),
           ('3840x1080', ('3840 × 1080 (32:9)',)), ('5120x1440', ('5120 × 1440 (32:9)',))]
# No field of view here: bbport.ini fov (scripts/fov.py) scales the camera table LockCamParam, but
# measured in the game 90 degrees gives about 48 (2026-10-20). Back once it works.
# The game's button icons (gpu/shim/bbport_prompts.cpp): with the device in use, or always one kind.
PROMPT_MODES = [('auto', ('Follow the device in use', 'По используемому устройству')),
                ('playstation', ('PlayStation', 'PlayStation')),
                ('keyboard', ('Keyboard and mouse', 'Клавиатура и мышь')),
                ('xbox', ('Xbox', 'Xbox')), ('switch', ('Nintendo Switch', 'Nintendo Switch'))]
HUD_POSITIONS = [('100', ('Screen edges', 'У краёв экрана')), ('75', ('75%',)), ('50', ('Halfway', 'Посередине')),
                 ('25', ('25%',)), ('0', ('Centred (16:9)', 'По центру (16:9)'))]
LIVE = [('auto', ('Auto (by graphics card)', 'Авто (по видеокарте)')), ('0', ('Off (faster)', 'Выключена (быстрее)')),
        ('1', ('On (change without restarting)', 'Включена (без перезапуска)'))]
LODS = [('0', ('As in the game', 'Как в игре')), ('-2', ('Highest (−2)', 'Максимальная (−2)')),
        ('1', ('Lower (1)', 'Ниже (1)')), ('2', ('Lowest (2)', 'Минимальная (2)'))]
FPS_MODES = [('uncap', ('Unlocked (frame-time patch)', 'Без ограничения (патч)')), ('60', ('60 FPS',)),
             ('90', ('90 FPS',)), ('30', ('30 FPS (as on PS4)', '30 FPS (как на PS4)'))]
PRESENT_MODES = [('Mailbox', ('Mailbox (low latency, no tearing)', 'Mailbox (без разрывов)')),
                 ('Fifo', ('FIFO (VSync)',)), ('FifoRelaxed', ('FIFO Relaxed',)),
                 ('Immediate', ('Immediate (tearing)', 'Immediate (с разрывами)'))]
LANGUAGES = [('1', ('English', 'Английский')), ('8', ('Russian', 'Русский')), ('0', ('Japanese', 'Японский')),
             ('2', ('French', 'Французский')), ('3', ('Spanish', 'Испанский')), ('4', ('German', 'Немецкий')),
             ('5', ('Italian', 'Итальянский')), ('9', ('Korean', 'Корейский')),
             ('10', ('Traditional Chinese (繁體中文)', 'Традиционный китайский (繁體中文)')),
             ('11', ('Simplified Chinese (简体中文)', 'Упрощенный китайский (简体中文)')),
             ('17', ('Português (Brasil)',))]
VOICE_LANGUAGES = [('auto', ('As in the game',)), ('eng', ('English',)),
                   ('ptb', ('Português (Brasil)',)), ('ded', ('Deutsch',)),
                   ('esa', ('Español (América)',)), ('ese', ('Español (España)',)),
                   ('frf', ('Français',)), ('iti', ('Italiano',)), ('jaj', ('日本語',))]
DRAW_PIPE = [('', ('Auto (8+ threads)', 'Авто (8+ потоков)')), ('1', ('On', 'Включён')),
             ('0', ('Off (more stable)', 'Выключен (стабильнее)'))]
# Off (0) skipped the copy FaceGen needs and exploded the hunter's face (PR #99): the game treats
# it as Relaxed, and a saved Off becomes the default (Launcher.__init__).
READBACKS = [('', ('Relaxed (default)', 'Relaxed (по умолчанию)')), ('2', ('Precise',))]
# What opens the in-game menu (BB_MENU_KEY / BB_MENU_PAD, gpu/shim/bbport_overlay.cpp): a key's name as
# SDL knows it (not F7, F8 or F9: input.ini's mouse and reload keys, pad recording), or none; a
# controller combination, or off. The launcher has every setting either way.
MENU_KEYS = [('Insert', ('Insert',)), ('Home', ('Home',)), ('End', ('End',)), ('Delete', ('Delete',)),
             ('PageUp', ('Page Up',)), ('PageDown', ('Page Down',))] + \
            [(f'F{n}', (f'F{n}',)) for n in range(1, 13) if n not in (7, 8, 9)] + [('none', ('No key',))]
MENU_PADS = [('l3+r3', ('L3 + R3 (press both sticks)',)), ('back+start', ('Select + Start (View + Menu)',)),
             ('off', ('Off',))]
# What the launcher does when the game starts (close_on_play of earlier launchers: 'close').
ON_PLAY = [('stay', ('Keep it open',)), ('minimize', ('Minimize it',)), ('close', ('Close it',))]
# BB_LOW_VRAM (run.py's low-VRAM profile): '' leaves it to the card's memory.
LOW_VRAM = [('', ('Auto (cards up to 6 GB)', 'Авто (карты до 6 ГБ)')), ('1', ('On', 'Включён')),
            ('0', ('Off', 'Выключен'))]
# Frame cap of the unlocked mode (BB_FPS_LIMIT). '' leaves the port's own: the display refresh,
# at most 120, because the game's movement timing breaks above about 120 FPS.
FRAME_CAPS = [('', ('Auto: display refresh, max 120 (recommended)', 'Авто: частота монитора, макс. 120 (рекомендуется)')),
              ('60', ('60',)), ('90', ('90',)), ('120', ('120',)), ('144', ('144  ⚠',)), ('165', ('165  ⚠',)),
              ('240', ('240  ⚠',)), ('0', ('No limit  ⚠', 'Без ограничения  ⚠'))]
FRAMES_AHEAD = [('', ('1 (default)', '1 (по умолчанию)')), ('2', ('2',)), ('0', ('Unbounded', 'Без ограничения'))]
# gpu/shadps4/video_core/renderer_vulkan/vk_low_latency.cpp: NVIDIA Reflex or AMD Anti-Lag 2.
LOW_LATENCY = [('off', ('Off', 'Выкл.')), ('on', ('On', 'Вкл.')), ('boost', ('On + Boost', 'Вкл. + Boost'))]
# vk_pipeline_cache.cpp BB_ASYNC_PIPELINES: new pipelines built on worker threads, their draws skipped meanwhile.
# gpu/shim/core/emulator_settings.h BB_VRR: V-Sync (FIFO) and a frame cap just below the refresh rate.
VRR_MODES = [('', ('Off', 'Выкл.')), ('1', ('On (G-Sync / FreeSync)', 'Вкл. (G-Sync / FreeSync)'))]
# vk_rasterizer.cpp BB_EARLY_SUBMIT: the frame goes to the GPU in parts while it is prepared.
EARLY_SUBMIT = [('', ('Off', 'Выкл.')), ('1', ('On (about 2 ms less lag)', 'Вкл. (примерно на 2 мс меньше задержка)'))]
ASYNC_PIPELINES = [('', ('Wait for each (exact)', 'Ждать каждый (точно)')),
                   ('1', ('In the background (no stutter)', 'В фоне (без фризов)'))]
UI_LANGUAGES = bbport_lang.LANGUAGE_NAMES

FSR4_COMMIT = 'ae8d628fae208813172446d1e49ed94150b04658'
FSR4_BASE = f'https://raw.githubusercontent.com/FireBurn/Q2RTX/{FSR4_COMMIT}/baseq2/fsr4_shaders'


def fsr4_files():
    """The FSR 4 v07 asset set of tools/fetch_fsr4_assets.sh (1080 and 2160 tiers)."""
    files = ['LICENSE-FSR4-v07.txt', 'rcas.spv', 'spd_auto_exposure.spv']
    for model in ('native', 'quality', 'balanced', 'performance', 'ultraperf', 'drs'):
        files += [f'fsr4_model_v07_i8_{model}_initializers.bin', f'fsr4_model_v07_i8_{model}_pre_weights.bin',
                  f'fsr4_model_v07_i8_{model}_shader_manifest.json']
        for tier in ('1080', '2160'):
            files += [f'fsr4_model_v07_i8_{model}_{tier}_pre.spv', f'fsr4_model_v07_i8_{model}_{tier}_post.spv']
            files += [f'fsr4_model_v07_i8_{model}_{tier}_pass{n}.spv' for n in range(1, 13)]
    return files


def fsr4_missing():
    folder = PORT_DIR / 'fsr4_shaders'
    return [name for name in fsr4_files() if not (folder / name).is_file() or not (folder / name).stat().st_size]


def load_json(path, default):
    try:
        return json.loads(Path(path).read_text(encoding='utf-8'))
    except (OSError, ValueError):
        return default


def load_settings():
    """settings.json. A broken one (a hand edit, a write cut off) silently became the defaults: the game folder
    and the sign-in were gone. It is kept as .bad and the copy from the last good load (.bak) is used and written
    back. Each good load refreshes that copy."""
    backup = CONFIG_FILE.with_name(CONFIG_FILE.name + '.bak')
    try:
        text, _clean = textfile.read(CONFIG_FILE)
        settings = json.loads(text)
        if not isinstance(settings, dict):
            raise ValueError('not a JSON object')
    except FileNotFoundError:
        return {}
    except (OSError, ValueError) as error:
        broken = textfile.keep_bad(CONFIG_FILE) if isinstance(error, ValueError) else None
        try:
            settings = json.loads(textfile.read(backup)[0])
            if not isinstance(settings, dict):
                raise ValueError('not a JSON object')
        except (OSError, ValueError):
            textfile.REPAIRS.append(f'Settings: {CONFIG_FILE} cannot be read ({error}) and has no backup; '
                                    f'the defaults are used' + (f', the old file is {broken.name}' if broken else ''))
            return {}
        if broken:
            try:
                write_settings(settings)
            except OSError:
                pass
        textfile.REPAIRS.append(f'Settings: {CONFIG_FILE} cannot be read ({error}); the copy from the last good '
                                f'start is used' + (f', the old file is {broken.name}' if broken else ''))
        return settings
    try:
        if not backup.is_file() or textfile.read(backup)[0] != text:
            textfile.write(backup, text)
    except OSError:
        pass
    return settings


def write_settings(settings):
    """settings.json, whole or not at all."""
    CONFIG_DIR.mkdir(parents=True, exist_ok=True)
    textfile.write(CONFIG_FILE, json.dumps(settings, indent=2, ensure_ascii=False))


def ini_path():
    return Path(os.environ.get('BB_CONFIG', DATA_DIR / 'bbport.ini'))


def ini_repairs(values):
    """{key: value} for hand edits the launcher cannot use: a decimal comma (sharpness=0,5) read as meant (the
    game took it for 0), a number or size that is none back to its default. Switches need none: anything but 1
    is off, for the game too."""
    fixed = {}
    for key, value in values.items():
        good = value
        if key == 'sharpness':
            try:
                number = float(value.replace(',', '.'))
                good = value if ',' not in value and 0 <= number <= 2 else f'{min(2.0, max(0.0, number)):.2f}'
            except ValueError:
                good = INI_DEFAULTS[key]
        elif key in ('preset', 'model_lod', 'hud_position', 'fov', 'frame_gen_multiplier'):
            good = value if re.fullmatch(r'-?\d+', value) else INI_DEFAULTS[key]
        elif key == 'output_res':
            good = value if re.fullmatch(r'\d+x\d+', value) else INI_DEFAULTS[key]
        if good != value:
            fixed[key] = good
    return fixed


def load_ini(legacy_settings=None):
    values, lines, clean = dict(INI_DEFAULTS), [], True
    try:
        text, clean = textfile.read(ini_path())
        lines = text.splitlines()
    except OSError:
        pass
    for line in lines:
        if '=' in line and not line.lstrip().startswith('#'):
            key, value = line.split('=', 1)
            values[key.strip()] = value.strip()
    # Edited by hand (another encoding, a decimal comma): rewritten clean, the old file kept as .bad.
    fixed = ini_repairs(values)
    if fixed or not clean:
        values.update(fixed)
        bad = textfile.keep_bad(ini_path())
        try:
            save_ini(fixed, lines)
        except OSError as error:
            # Read-only or locked: the file is as it was, so its copy goes again (each load made one more), and
            # the repaired values hold for this start.
            if bad:
                try:
                    bad.unlink()
                except OSError:
                    pass
            ini_not_written(error, fixed)
            return values, lines
        what = ', '.join(f'{key}={value}' for key, value in fixed.items()) or 'its text as UTF-8'
        textfile.REPAIRS.append(f'Settings: {ini_path()} repaired by hand edits ({what}); '
                                f'the old file is {bad.name if bad else "not kept"}')
        return load_ini(legacy_settings)
    # Move existing launcher preferences once. Explicit INI values always win,
    # including vk/0; a later overlay edit must never be replaced by the old JSON.
    present = {line.split('=', 1)[0].strip() for line in lines
               if '=' in line and not line.lstrip().startswith('#')}
    migrated = {}
    for key in SHARED_GRAPHICS_KEYS:
        if key not in present and legacy_settings is not None and key in legacy_settings:
            old = legacy_settings[key]
            migrated[key] = fsr411_backend(legacy_settings) if key == 'fsr411_backend' else \
                '1' if old is True or old == '1' else '0'
    if migrated:
        try:
            save_ini(migrated, lines)
        except OSError as error:
            # A read-only or locked bbport.ini kept the launcher from opening: the moved values hold for this
            # start, and the next one tries again.
            values.update(migrated)
            present |= set(migrated)
            ini_not_written(error, migrated)
        else:
            return load_ini(legacy_settings)
    if 'upscaler' not in present:
        env = bbport_fsr411.environment(PORT_DIR, legacy_settings or {})
        support = bbport_fsr41.query(PORT_DIR, env)
        states = bbport_fsr41.method_statuses(PORT_DIR, DATA_DIR, support, fetch_fsr4vk, env)
        choice = bbport_fsr41.preferred_route(states,
            backend=values['fsr411_backend'] if 'fsr411_backend' in present else None)
        if choice is not None:
            values['upscaler'], values['fsr411_backend'] = bbport_fsr41.ROUTES[choice][:2]
    if 'fsr411_backend' not in present:
        if 'upscaler' in present and values['upscaler'] in ('fsr411', 'fsr412'):
            env = bbport_fsr411.environment(PORT_DIR, legacy_settings or {})
            support = bbport_fsr41.query(PORT_DIR, env)
            states = bbport_fsr41.method_statuses(PORT_DIR, DATA_DIR, support, fetch_fsr4vk, env)
            choice = bbport_fsr41.preferred_route(states, version=values['upscaler'])
            values['fsr411_backend'] = bbport_fsr41.ROUTES[choice][1] if choice is not None else 'vk'
        elif values['upscaler'] not in ('fsr411', 'fsr412'):
            values['fsr411_backend'] = bbport_fsr411.default_backend(PORT_DIR, legacy_settings)
    values['fsr411_backend'] = fsr411_backend(values)
    values['fsr41_version'] = values['upscaler'] if values['upscaler'] in ('fsr411', 'fsr412') else \
        'fsr411' if values.get('fsr41_version') == 'fsr411' else 'fsr412'
    values['object_motion_cache'] = '1' if values.get('object_motion_cache') == '1' else '0'
    values['fsr_compatibility_options'] = '1' if values.get('fsr_compatibility_options') == '1' else '0'
    return values, lines


def save_ini(values, lines):
    """Rewrites the edited keys in place, appends missing ones, keeps comments and other keys."""
    written, out = set(), []
    for line in lines:
        if '=' in line and not line.lstrip().startswith('#'):
            key = line.split('=', 1)[0].strip()
            if key in values:
                out.append(f'{key}={values[key]}')
                written.add(key)
                continue
        out.append(line)
    if not lines:
        out.append('# bbport settings (in-game menu: Insert / L3+R3)')
    out += [f'{key}={value}' for key, value in values.items() if key not in written]
    textfile.write(ini_path(), '\n'.join(out) + '\n')


def ini_not_written(error, values):
    """A line for the log (once) when bbport.ini cannot be written (read-only, held by another program): the
    launcher goes on with `values` in memory instead of stopping."""
    line = (f'Settings: {ini_path()} cannot be written ({error}); this start goes on without saving '
            + (', '.join(f'{key}={value}' for key, value in values.items()) or 'its text as UTF-8'))
    if line not in textfile.REPAIRS:
        textfile.REPAIRS.append(line)


def game_info(folder):
    """(title, app version) of a game folder, or None without eboot.bin."""
    folder = Path(folder or '.')
    if not (folder / 'eboot.bin').is_file():
        return None
    try:
        from prepare import sfo, system_folder
        from patches import game_app_version
        values = sfo((system_folder(folder) / 'param.sfo').read_bytes())
        # game_app_version: Game of the Year dumps say APP_VER 01.00 over the 1.09 code.
        version = game_app_version(folder) or values.get('APP_VER', '?')
        return values.get('TITLE', 'Bloodborne').replace('™', '').strip(), version
    except (OSError, ValueError, ImportError):
        return 'Bloodborne', '?'


def monitor_options(chosen=''):
    """(value, (label,)) of the game monitor choice: '' (Windows' main monitor) first, then the
    others; a chosen monitor that is not connected stays listed."""
    found = display.monitors()
    main = next((m for m in found if m.primary), None)
    words = lambda m: (f' · {m.name}' if m.name else '') + ' · {} × {}'.format(*m.size)
    options = [('', (_('Main monitor') + (words(main) if main else ''),))]
    for m in found:
        if m is main:
            continue
        dx, dy = (m.rect[0] - main.rect[0], m.rect[1] - main.rect[1]) if main else (0, 0)
        side = (_('on the left') if dx < 0 else _('on the right')) if abs(dx) >= abs(dy) else \
            (_('above') if dy < 0 else _('below'))
        options.append((m.device, (_('Monitor {}').format(m.number) + words(m) + (f' · {side}' if main else ''),)))
    if chosen and chosen not in [value for value, _t in options]:
        number = display.Monitor(chosen, (0, 0, 0, 0), (0, 0, 0, 0), False).number
        options.append((chosen, (_('Monitor {} (not connected: the main monitor is used)').format(number),)))
    return options


OLD_HUNTERS = 'SPEXPANSIONDLC03'  # The Old Hunters' entitlement label: its folder's name


def title_id(folder):
    """The dump's TITLE_ID (sce_sys/param.sfo); '' when unknown."""
    try:
        from prepare import sfo, system_folder
        return str(sfo((system_folder(folder or '.') / 'param.sfo').read_bytes()).get('TITLE_ID', '')).strip()
    except (OSError, ValueError, ImportError, TypeError):
        return ''


def add_ons(folder):
    """The add-ons the port reports as installed from one folder (src/runtime_content.c: every
    subfolder, its name the label, up to 16 characters)."""
    try:
        return sorted(p.name for p in Path(folder).iterdir()
                      if p.is_dir() and len(p.name) <= 16 and not p.name.startswith('.'))
    except OSError:
        return []


def dlc_status(game_dir, dlc_dir, user_dir):
    """('ok' | 'warn' | 'none', text): does the game get The Old Hunters?"""
    edition = title_id(game_dir) == 'CUSA03173'  # the Game of the Year Edition has it
    if dlc_dir:
        folder = Path(dlc_dir)
        if not folder.is_dir():
            return 'warn', _('The DLC folder does not exist.')
        if (folder / 'eboot.bin').is_file():
            return 'warn', _('This is the game folder. The DLC folder is the one that contains {}.').format(OLD_HUNTERS)
        if (folder / 'sce_sys').is_dir():
            return 'warn', _('This is the add-on itself: choose the folder that contains {}.').format(folder.name)
        found = add_ons(folder)
        if OLD_HUNTERS in found:
            return 'ok', _('The Old Hunters found ({}).').format(OLD_HUNTERS)
        near = next((name for name in found if name.upper() == OLD_HUNTERS), None)
        if near:
            return 'warn', _('Rename the folder {} to {}: the game looks for that exact name.').format(near, OLD_HUNTERS)
        if not edition:
            return 'warn', _('No add-on in this folder: The Old Hunters is a folder named {} in it.').format(OLD_HUNTERS)
    addcont = Path(user_dir) / 'addcont'  # shadPS4's layout, in the saves folder
    if addcont.is_dir() and any(OLD_HUNTERS in add_ons(title) for title in addcont.iterdir()):
        return 'ok', _('The Old Hunters found in the saves folder (addcont).')
    if edition:
        return 'ok', _('Your edition (Game of the Year, CUSA03173) already includes The Old Hunters.')
    return 'none', _('The Old Hunters: not found. Only needed if you own it; without it its areas stay closed.')


def webapi_address(server):
    """The shadNet WebAPI of a server address host:port (the server's default port 31315)."""
    host = server.rsplit(':', 1)[0] if server.count(':') == 1 else server
    return f'http://{host}:31315'


def community_address(value):
    """The game server (messages, bloodstains, ghosts) as https://host, The Hunter's Dream by default."""
    value = str(value).strip() or COMMUNITY_SERVER
    scheme, _sep, rest = value.rpartition('://')
    host = rest.split('/')[0].split(':')[0]  # the game picks the ports (20443, 18671)
    return f'{scheme or "https"}://{host}'


def game_server(s):
    """The game server the game uses: the address field only where the Online page shows it (Own server,
    online_custom_servers), The Hunter's Dream for its own pool, the SuperMedo game server otherwise. The Hunter's
    Dream's sign-in and backend exist on The Hunter's Dream
    only, and other tools write their own address into the field (a co-op tool's: "has no account
    service" at the sign-in)."""
    if s.get('online_pool') == 'shadnet' and s.get('online_custom_servers'):
        return community_address(s.get('online_community', ''))
    if s.get('online_pool') == 'huntersdream' and HUNTERS_DREAM_POOL:
        return HUNTERS_DREAM_SERVER
    return COMMUNITY_SERVER


def shadnet_servers(s):
    """(server, WebAPI) of shadNet the game uses: the address fields only with own servers (online_custom_servers:
    the Own server pool, where the Online page shows them), the public server otherwise. A field another tool or an earlier setup left behind
    (a WebAPI with https://, where shadNet speaks http) then cannot break the public one."""
    if s.get('online_custom_servers'):
        return str(s.get('online_server', '')).strip() or APP_DEFAULTS['online_server'], webapi_setting(s)
    server = APP_DEFAULTS['online_server']
    return server, webapi_address(server)


def has_own_servers(s):
    """For settings saved before online_custom_servers existed: on when they hold other addresses (a
    private server, a friend's), which the game used then."""
    return any(str(s.get(key, '')).strip() not in ('', APP_DEFAULTS[key])
               for key in ('online_server', 'online_webapi', 'online_community'))


def started_by_bbsc(env=None):
    """BBSC's own "OPEN BBPORT LAUNCHER + BBSC" (tools/play-bbport.ps1) opened this launcher, with its
    bbsc-launch.exe as the game program (BB_PROBE) and its server in the shadNet boxes."""
    env = os.environ if env is None else env
    return Path(env.get('BB_PROBE', '')).name.lower() == 'bbsc-launch.exe'


def settled_pool(s, env=None):
    """The co-op pool a launcher start keeps (online_custom_servers already settled): SuperMedo Server (the
    default) or The Hunter's Dream while it is on. Since 1.9.5 beta 10 the launcher offers no other server:
    players who chose shadPS4's public one (which refuses the port) or another own server were confused
    about where they played, so they move to SuperMedo Server. Only Seamless co-op's own start (started_by_bbsc)
    keeps its shadNet server."""
    pool = s.get('online_pool')
    if pool == 'huntersdream' and HUNTERS_DREAM_POOL:
        return pool
    if started_by_bbsc(env) and ((pool == 'shadnet' and s.get('online_custom_servers')) or bbsc_server_saved(s, env)):
        return 'shadnet'
    return 'supermedo'


def bbsc_server_saved(s, env=None):
    """BBSC's start names the server its script saved in the shadNet boxes (BB_SHADNET_SERVER = online_server):
    its pool again, though a normal launcher start since moved the pool to SuperMedo Server and saved that."""
    env = os.environ if env is None else env
    theirs = env.get('BB_SHADNET_SERVER', '').strip().partition(':')[0].lower()
    ours = str(s.get('online_server', '')).strip().partition(':')[0].lower()
    return bool(theirs) and theirs == ours


def settled_online(app, saved):
    """The online settings a start keeps, in the launcher window and in a start without it (--play: Steam, a
    shortcut, where the window's settled pool never ran): own servers (online_custom_servers) on for settings
    from before that setting when they hold other addresses, then the co-op pool (settled_pool). Up to 1.9.5 beta 8 SuperMedo
    Server's account was in the shadNet boxes, which a co-op tool writes too: one saved on SuperMedo Server
    becomes its own account, and the boxes stay shadNet's."""
    custom = saved['online_custom_servers'] if 'online_custom_servers' in saved else has_own_servers(app)
    pool = settled_pool({**app, 'online_custom_servers': custom})
    settled = {'online_custom_servers': custom or pool == 'shadnet', 'online_pool': pool}  # BBSC's server counts
    if 'supermedo_npid' not in saved and saved.get('online_pool') == 'supermedo':
        settled.update(supermedo_npid=app.get('online_npid', ''), supermedo_pass=app.get('online_password', ''))
    return settled


def chosen_pool(s):
    """The pool a start plays on: The Hunter's Dream's players play on SuperMedo Server while it is not offered
    (since 1.9.7), before the window or settled_pool has rewritten their choice."""
    pool = s.get('online_pool', 'supermedo')
    return 'supermedo' if pool == 'huntersdream' and not HUNTERS_DREAM_POOL else pool


def pool_account(s):
    """The account the game signs in with on the chosen pool, '' without one. Each pool keeps its own, so a
    change of pool (or a co-op tool writing the shadNet boxes) never signs another one out: SuperMedo Server's
    account, The Hunter's Dream's Discord sign-in, the shadNet boxes."""
    pool = chosen_pool(s)
    if pool == 'huntersdream':
        return str(s.get('thd_online_id', '')).strip() if s.get('thd_token') else ''
    if pool == 'supermedo':
        return str(s.get('supermedo_npid', '')).strip() if s.get('supermedo_pass') else ''
    return str(s.get('online_npid', '')).strip()


def signed_out_server(s):
    """With online on: the name of the chosen server when the launcher has no account for it (SuperMedo Server
    or The Hunter's Dream), else ''. A Seamless co-op start (shadnet) brings its own."""
    pool = chosen_pool(s)
    if not s.get('online') or pool not in ('supermedo', 'huntersdream') or pool_account(s):
        return ''
    return "The Hunter's Dream" if pool == 'huntersdream' else 'SuperMedo Server'


# The Online page's settings in settings.json. A co-op tool (Seamless co-op) writes shadNet's there; the pool and
# the SuperMedo Server and The Hunter's Dream sign-ins are the launcher's own.
SHADNET_KEYS = ('online_server', 'online_webapi', 'online_community', 'online_npid', 'online_password',
                'online_custom_servers')
OWN_ONLINE_KEYS = ('online_pool', 'supermedo_npid', 'supermedo_pass', 'thd_online_id', 'thd_token')


def outside_online_changes(written, on_disk):
    """What another program changed in settings.json since the launcher read or wrote it (`written`): the
    shadNet settings it set ({key: value}, taken), and the launcher's own it changed ([key], kept as they were)."""
    taken = {key: on_disk[key] for key in SHADNET_KEYS if key in on_disk and on_disk[key] != written.get(key)}
    kept = [key for key in OWN_ONLINE_KEYS if key in on_disk and on_disk[key] != written.get(key)]
    return taken, kept


def webapi_setting(s):
    """The WebAPI field, unless it holds the game server's address (a common mix-up): then the
    co-op server's own WebAPI is used."""
    webapi = str(s['online_webapi']).strip()
    community_host = community_address(s['online_community']).split('://', 1)[1].split(':')[0]
    if webapi and community_host in webapi:
        webapi = ''
    return webapi or webapi_address(str(s['online_server']).strip() or APP_DEFAULTS['online_server'])


def server_check(server, webapi, community):
    """A line about each online address: reachable, or why not (runs off the Tk thread). Every error counts as an
    answer: a broken HTTP reply (BadStatusLine from a proxy or a virus scanner) is no OSError, and it ended the
    test's thread with "Checking…" left on the Online page for good."""
    import socket
    lines = []
    try:
        with bbport_tls.urlopen(community + ':20443/bb-eu/ss.info', timeout=6) as reply:
            reply.read(64)
        lines.append(_('Game server {0}: reachable').format(community))
    except Exception as error:
        lines.append(_('Game server {0}: not reachable ({1})').format(community, error_text(error)))
    host, _sep, port = server.rpartition(':')
    try:
        socket.create_connection((host or server, int(port or 31313)), timeout=4).close()
        lines.append(_('Server {0}: reachable').format(server))
    except Exception as error:
        lines.append(_('Server {0}: not reachable ({1})').format(server, error_text(error)))
    try:
        with bbport_tls.urlopen(webapi.rstrip('/') + '/status', timeout=4) as reply:
            ok = b'"ok":true' in reply.read(512).replace(b' ', b'')
        lines.append(_('WebAPI {0}: reachable').format(webapi) if ok else
                     _('WebAPI {0}: answered, but it is not a shadNet server').format(webapi))
    except Exception as error:
        lines.append(_('WebAPI {0}: not reachable ({1})').format(webapi, error_text(error)))
    return '\n'.join(lines)


# The Hunter's Dream pool: the engine's own backend for it (BB_NET_BACKEND=thd, bbhost's
# protocol, from Awaken's port). Not offered since 1.9.7: its players move to SuperMedo Server (settled_pool,
# show_pool) and are told once (bbport_news). BB_HUNTERS_DREAM=1 shows it again.
HUNTERS_DREAM_POOL = os.environ.get('BB_HUNTERS_DREAM', '0') == '1'


SHADNET_ERRORS = {3: 'The name or the email address is not valid.', 4: 'Too soon: try again in a minute.',
                  11: 'This name is taken.', 12: 'This email provider is not allowed.',
                  13: 'An account with this email address exists already.', 23: 'Sign-up is closed.'}


def shadnet_register(address, npid, password, email, timeout=15):
    """Creates an account on a shadNet server (its Create command: the RegistrationRequest
    protobuf, framed as shadNet's documentation describes). Returns None, or why it failed."""
    import socket
    import struct

    def varint(n):
        out = b''
        while True:
            byte, n = n & 0x7F, n >> 7
            out += bytes([byte | (0x80 if n else 0)])
            if not n:
                return out

    def text(field, value):
        data = value.encode('utf-8')
        return varint(field << 3 | 2) + varint(len(data)) + data if data else b''

    def read(sock, n):
        data = b''
        while len(data) < n:
            chunk = sock.recv(n - len(data))
            if not chunk:
                raise OSError('the server closed the connection')
            data += chunk
        return data
    host, _, port = address.rpartition(':')
    proto = text(1, npid) + text(2, password) + text(4, email)
    payload = struct.pack('<I', len(proto)) + proto
    with socket.create_connection((host, int(port)), timeout=timeout) as sock:
        info = read(sock, 15)
        read(sock, struct.unpack_from('<I', info, 3)[0] - 15)  # ServerInfo: the protocol version
        sock.sendall(struct.pack('<BHIQ', 0, 2, 15 + len(payload), 1) + payload)
        reply = read(sock, 16)
    error = reply[15]
    return None if error == 0 else SHADNET_ERRORS.get(error, f'The server said no (error {error}).')


def thd_open(request):
    return bbport_tls.urlopen(request, timeout=10)


def discord_sign_in(server, on_page, timeout=600):
    """Sign in with Discord on SuperMedo Server (its /auth pages): opens the sign-in page through
    on_page(url), waits for it and returns (Online ID, password), or raises ValueError (why)."""
    def call(path, data=None):
        request = urllib.request.Request(server + path, data=data, method='POST' if data is not None else 'GET')
        with bbport_tls.urlopen(request, timeout=15) as response:
            return json.loads(response.read())
    started = call('/auth/start', b'')
    on_page(started['url'])
    deadline = time.time() + timeout
    while time.time() < deadline:
        time.sleep(2)
        reply = call('/auth/poll?session=' + started['session'])
        if reply.get('status') == 'done':
            return reply['npid'], reply['password']
        if reply.get('status') == 'failed':
            raise ValueError(reply.get('message') or 'the sign-in failed')
        if reply.get('status') == 'expired':
            break
    raise ValueError('the sign-in took too long; start it again')


def thd_call(url, body, tries=3):
    """A call to The Hunter's Dream's account service (bbhost's): its reply, or ValueError with
    the server's reason. What is no answer of the service (an error page, a timeout, a dropped
    connection) is asked again, up to `tries` times: testers saw "has no account service" at the
    sign-in, and it passed after a few clicks."""
    request = urllib.request.Request(url, data=json.dumps(body).encode(), method='POST',
                                     headers={'Content-Type': 'application/json'})
    status = None
    for attempt in range(tries):
        if attempt:
            time.sleep(attempt)  # 1 s, then 2 s
        try:
            with thd_open(request) as response:
                status, text = getattr(response, 'status', None), response.read()
        except urllib.error.HTTPError as error:
            status, text = error.code, error.read()
        except OSError:  # a timeout, a dropped connection, a certificate: the last one is shown
            if attempt == tries - 1:
                raise
            continue
        try:
            reply = json.loads(text)
        except ValueError:
            reply = None
        if isinstance(reply, dict) and 'ResKind' in reply:
            if reply['ResKind'] != 0:
                raise ValueError(reply.get('Message') or 'refused')
            return reply
    host = url.split('://', 1)[-1].split('/')[0]
    raise ValueError(f'{host} did not answer properly (HTTP {status}); try again in a minute')


def thd_sign_in(on_code):
    """Discord sign-in the way bbhost does it: a code to approve on The Hunter's Dream's website.
    on_code(page, code) shows it. Returns (online id, token); raises ValueError."""
    auth = HUNTERS_DREAM_SERVER
    start = thd_call(auth + '/auth/device/start', {'DeviceName': f'bbport on {os.environ.get("COMPUTERNAME", "PC")}'})
    device, user_code = start.get('DeviceCode', ''), start.get('UserCode', '')
    if not device or not user_code:
        raise ValueError('no code in the reply')
    interval = max(1, int(start.get('Interval') or 5))
    deadline = time.monotonic() + int(start.get('ExpiresIn') or 600)
    on_code(start.get('VerificationUri') or auth + '/link', user_code)
    while time.monotonic() < deadline:
        time.sleep(interval)
        poll = thd_call(auth + '/auth/device/poll', {'DeviceCode': device})
        status = poll.get('Status')
        if status == 'approved':
            if not poll.get('OnlineId') or not poll.get('Token'):
                raise ValueError('no token in the reply')
            return poll['OnlineId'], poll['Token']
        if status == 'slow_down':
            interval += 5
        elif status == 'denied':
            raise ValueError('the sign-in was denied')
        elif status == 'expired':
            break
    raise ValueError('the code expired; try again')


def server_check_hunters_dream():
    """Whether The Hunter's Dream's game server and bbhost service answer (off the Tk thread)."""
    lines = []
    for title, url in ((_('Game server {0}'), COMMUNITY_SERVER + ':20443/bb-eu/ss.info'),
                       (_('Co-op service {0}'), COMMUNITY_SERVER + ':18671/')):
        name = title.format(url.split('://', 1)[1].split('/')[0])
        try:
            bbport_tls.urlopen(url, timeout=6).close()
            lines.append(_('{0}: reachable').format(name))
        except urllib.error.HTTPError:
            lines.append(_('{0}: reachable').format(name))   # it answered
        except Exception as error:  # a broken reply too (BadStatusLine): see server_check
            lines.append(_('{0}: not reachable ({1})').format(name, error_text(error)))
    return '\n'.join(lines)


def vulkan_verdict(env):
    """scripts/vulkan_check.py's answer for this PC: what the game's renderer needs, asked of the graphics
    driver, in a process of its own (a broken driver ends that one, not the launcher). A process the driver
    took down: {'problem': 'crashed', 'code': its exit code}. None when it could not ask."""
    script = PORT_DIR / 'scripts' / 'vulkan_check.py'
    if not script.is_file():
        return None
    command = [sys.executable, '--script', str(script)] if FROZEN else [sys.executable, str(script)]
    try:
        result = subprocess.run(command, capture_output=True, text=True, timeout=30, env=env,
                                creationflags=NO_WINDOW)
    except (OSError, subprocess.TimeoutExpired):
        return None
    try:
        return json.loads(result.stdout.strip().splitlines()[-1])
    except (ValueError, IndexError):
        # Ended by Windows (an exception code such as 0xC0000005) inside the driver: worth saying. Any other
        # end without an answer is the check's own failure, not the PC's.
        crashed = result.returncode < 0 or result.returncode >= 0xC0000000
        return {'ok': False, 'problem': 'crashed', 'code': f'0x{result.returncode & 0xffffffff:08X}'} \
            if crashed else None


def fsr41_check_failed(error):
    """bbport_fsr41.query's answer when the check stopped with an error it does not expect: "check failed" with
    why (bbport_fsr411.query's own shape for a failed probe), not "checking support…" for good."""
    detail = error_text(error)
    return {'vendor_id': None, **{method: dict(supported=False, reason='check', detail=detail,
                                               checks=[dict(id='probe', state='fail', detail=detail)])
                                  for method in ('vk', 'dx12')}}


def fg401_check_failed():
    """bbport_fg401.query_versions' answer when the check stopped with an error it does not expect."""
    return {key: dict(supported=False, reason='check', detail='') for key in ('fsr401', 'fsr402')}


def motion_cache_available(graphics, unavailable=''):
    return not unavailable and \
        graphics.get('upscaler', INI_DEFAULTS['upscaler']) in ('fsr3', 'fsr4', 'fsr411', 'fsr412') and \
        graphics.get('object_motion', INI_DEFAULTS['object_motion']) in ('1', True)


def early_submit_unavailable(vendor, extra_env=''):
    """Why sending frames in parts is off on this card whatever the setting (vk_rasterizer.cpp: slower on AMD,
    BB_EARLY_SUBMIT=force overrides it); '' when the setting decides."""
    if vendor == 0x1002 and 'BB_EARLY_SUBMIT=force' not in str(extra_env).split():
        return _('Not available on AMD Radeon: there it adds lag and costs FPS.',
                 'Недоступно на AMD Radeon: там добавляет задержку и снижает FPS.')
    return ''


def object_motion_unavailable(gpu, gpu_line, extra_env=''):
    """Why the game runs without object motion vectors on this card whatever the setting says, as
    vk_object_motion.cpp decides (BB_OBJECT_MOTION=1 overrides it); '' when the setting decides.
    gpu: bb-gpu-capabilities' 'gpu' entry; gpu_line: 'Name, discrete, 16304 MiB'."""
    if 'BB_OBJECT_MOTION=1' in str(extra_env).split():
        return ''
    gpu = gpu or {}
    if gpu.get('vendor_id') == 0x1002 and isinstance(gpu.get('device_id'), int) and gpu['device_id'] < 0x7440:
        return _('Not available on RX 6000 and older Radeon cards: they lose the device with it.',
                 'Недоступно на Radeon RX 6000 и более старых: с ним видеокарта теряет устройство.')
    parts = [part.strip() for part in str(gpu_line or '').split(',')]
    vram = next((int(m.group(1)) for part in parts for m in [re.match(r'(\d+) MiB', part)] if m), 0)
    if 'discrete' in parts and 0 < vram < 6144 - 256:
        return _('Not available under 6 GB of video memory: textures need it more.',
                 'Недоступно при видеопамяти меньше 6 ГБ: она нужнее текстурам.')
    return ''


HUNTERS_ONLINE_EVERY = 600  # seconds between two looks at the count: the answer is about 140 KB


def hunters_online():
    """(players online, bbhost, shadNet) on The Hunter's Dream, from the public statistics its own
    start page shows (thehuntersdream.com/api/stats, active_sessions); None when it does not answer."""
    request = urllib.request.Request(HUNTERS_DREAM_SERVER + '/api/stats', headers={'User-Agent': f'bbport-launcher/{VERSION}'})
    try:
        with bbport_tls.urlopen(request, timeout=8) as response:
            sessions = json.load(response).get('active_sessions') or {}
        if not sessions.get('server_online'):
            return None
        clients = sessions.get('by_client') or {}
        return int(sessions.get('count') or 0), int(clients.get('bbhost') or 0), int(clients.get('shadps4') or 0)
    except Exception:  # a broken reply too (IncompleteRead): no count, never a dead helper thread
        return None


def moved(order, source, to):
    """`order` with the item at `source` dropped before the item now at `to` (len: at the end)."""
    order = list(order)
    if to not in (source, source + 1):
        order.insert(to - 1 if to > source else to, order.pop(source))
    return order


# Folders a sync client (OneDrive, Dropbox, Google Drive, iCloud, Nextcloud) keeps in step with the cloud.
SYNCED_NAMES = ('onedrive', 'dropbox', 'google drive', 'googledrive', 'icloud drive', 'iclouddrive', 'nextcloud')


def synced_folder(path):
    """Whether a folder lies in a cloud sync folder (by OneDrive's own variables, or by its name)."""
    text = str(path).lower().replace('\\', '/').rstrip('/') + '/'
    roots = [os.environ.get(name, '') for name in ('OneDrive', 'OneDriveConsumer', 'OneDriveCommercial')]
    if any(root and text.startswith(root.lower().replace('\\', '/').rstrip('/') + '/') for root in roots):
        return True
    # the folder itself or a variant of it: 'OneDrive - Firma', 'Dropbox (Personal)'; not 'OneDriveBackup'
    return any(re.search(rf'/{re.escape(name)}(?=[/ (-])', text) for name in SYNCED_NAMES)


def saves_folder(s):
    return Path(s.get('user_dir') or DATA_DIR / 'user')


def saves_in_other_installation(s):
    """Whether the saves folder lies in another installation of the port (bbport_help.other_installations)."""
    saves = {'user_dir': str(saves_folder(s))}
    return any(key == 'user_dir' for key, _name, _other in bbport_help.other_installations(saves, PORT_DIR, DATA_DIR))


def cache_folder(s):
    """Where the shader and pipeline caches, the logs and the temporary files go: the cache folder the
    player chose, else this installation's user folder, wherever the saves are. Saves may be shared (a
    cloud folder, a folder several installations use); shaders belong to one build and one graphics
    card: another build's shaders turned testers' pictures dark ("stale pipelines" in the log, gone
    with an own cache)."""
    if s.get('cache_dir'):
        return Path(s['cache_dir'])
    return DATA_DIR / 'user'


# What BBSC's script hands the launcher it opens (game_environment).
BBSC_SCRIPT_VARIABLES = ('BB_PROBE', 'BBSC_EMULATOR', 'BB_ONLINE', 'BB_SHADNET_SERVER', 'BB_SHADNET_WEBAPI',
                         'BB_SHADNET_NPID', 'BB_SHADNET_PASSWORD', 'BB_COMMUNITY_SERVER', 'BB_USER_NAME')


def game_environment(s, graphics=None):
    env = {k: v for k, v in os.environ.items() if not k.startswith(('BB_FG_', 'BB_FG401_'))}
    # BBSC's own "OPEN BBPORT LAUNCHER + BBSC" (tools/play-bbport.ps1) opens this launcher with
    # BBSC as the game program and its server: kept for a start on that server only (the shadNet
    # choice it points there). Offline, The Hunter's Dream or another server start without BBSC.
    if started_by_bbsc(env):
        theirs = env.get('BB_SHADNET_SERVER', '').strip().partition(':')[0].lower()
        ours = str(s.get('online_server', '')).strip().partition(':')[0].lower()
        if not (s.get('online') and s.get('online_pool') == 'shadnet' and theirs and theirs == ours):
            for key in BBSC_SCRIPT_VARIABLES:
                env.pop(key, None)
    import bbport_driver_runtime
    bbport_driver_runtime.configure_environment(env, replace=True)
    graphics = graphics if graphics is not None else load_ini(s)[0]
    env['BB_UI_LANGUAGE'] = s.get('ui_language') or LANG
    env['BB_GAME_DIR'] = s['game_dir']
    if s['user_dir']:
        env['BB_USER_DIR'] = s['user_dir']
    if cache_folder(s) != saves_folder(s):
        env['BB_GPU_USER_DIR'] = str(cache_folder(s))
    if s['dlc_dir']:
        env['BB_DLC_DIR'] = s['dlc_dir']
    env['BB_MODS_DIR'] = s['mods_dir'] or str(DATA_DIR / 'mods')
    env['BB_MODS_CONFIG'] = str(DATA_DIR / 'mods.json')
    env['BB_MODS_ENABLED'] = '1' if s['mods_enabled'] else '0'
    env['BB_PATCHES_DIR'] = s['patches_dir'] or str(DATA_DIR / 'patches')
    env['BB_PATCHES_CONFIG'] = str(DATA_DIR / 'patches.json')
    env['BB_LANGUAGE'] = s['language']
    env['BB_DISCORD'] = '1' if DISCORD_APPLICATION_ID and s.get('discord_status', True) else '0'  # src/runtime_discord.c
    if DISCORD_APPLICATION_ID:
        env['BB_DISCORD_APP_ID'] = DISCORD_APPLICATION_ID
    env['BB_MENU_KEY'] = str(s.get('menu_key') or 'Insert')  # the in-game menu's key and combination
    env['BB_MENU_PAD'] = str(s.get('menu_pad') or 'l3+r3')
    env['BB_EXIT_BACKUP'] = '1' if s.get('exit_backup', False) else '0'
    env['BB_VOICE_LANGUAGE'] = s['voice_language']
    if str(s['player_name']).strip():
        env['BB_USER_NAME'] = str(s['player_name']).strip()
    env['BB_FULLSCREEN'] = '1' if s['fullscreen'] else '0'
    if s['fullscreen'] and s.get('exclusive_fullscreen'):
        env['BB_WINDOW_MODE'] = 'fullscreen'  # gpu/shim/window.cpp: the display mode is the game's
    env['BB_PRESENT_MODE'] = s['present_mode']
    if s['hdr']:
        env['BB_HDR'] = '1'
    s = mirrored(s, graphics)
    env['BB_FPS'] = s['fps_mode']
    if s.get('frame_cap', ''):
        env['BB_FPS_LIMIT'] = s['frame_cap']
    for key, name in (('draw_pipe', 'BB_DRAW_PIPE'), ('readbacks', 'BB_READBACKS'), ('frames_ahead', 'BB_FRAMES_AHEAD'),
                      ('low_vram', 'BB_LOW_VRAM'), ('low_latency', 'BB_LOW_LATENCY'),
                      ('async_pipelines', 'BB_ASYNC_PIPELINES'), ('vrr', 'BB_VRR'),
                      ('early_submit', 'BB_EARLY_SUBMIT')):
        if s[key]:
            env[name] = s[key]
    if not s.get('vram_warning', True):
        env['BB_VRAM_WARN_MB'] = '0'  # texture_cache.cpp: no "Video memory is running low" banner
    for key, name in (('frame_stats', 'BB_FRAME_STATS'), ('gpu_profile', 'BB_GPU_PROFILE'),
                      ('vk_validation', 'BB_VK_VALIDATION')):
        if s[key]:
            env[name] = '1'
    for item in str(s['extra_env']).split():
        if '=' in item:
            key, value = item.split('=', 1)
            env[key] = value
    # run.py refreshes these derived values from the shared file on every start,
    # including an in-game restart. The saved preference remains independent of
    # whether the current upscaler/object-motion selection can use the cache.
    env['BB_SHARED_GRAPHICS_CONFIG'] = '1'
    env['BB_FSR411_BACKEND'] = fsr411_backend(graphics)
    env['BB_FSR411_NATIVE'] = '1' if fsr411_backend(graphics) == 'dx12' else '0'
    cache = graphics.get('object_motion_cache', '0') == '1' and motion_cache_available(graphics)
    env['BB_OBJECT_MOTION_CACHE'] = '1' if cache else '0'
    if s.get('online'):
        server, webapi = shadnet_servers(s)
        env['BB_ONLINE'] = '1'
        env['BB_SHADNET_SERVER'] = server
        env['BB_SHADNET_WEBAPI'] = webapi
        env['BB_SHADNET_NPID'] = str(s['online_npid']).strip()
        env['BB_SHADNET_PASSWORD'] = str(s['online_password'])
        env['BB_UPNP'] = '1' if s['online_upnp'] else '0'
        env['BB_COMMUNITY_SERVER'] = game_server(s)
        if s.get('online_pool', 'supermedo') == 'supermedo':  # SuperMedo Server: its own addresses and account
            env['BB_SHADNET_SERVER'] = SUPERMEDO_SERVER
            env['BB_SHADNET_WEBAPI'] = SUPERMEDO_WEBAPI
            env['BB_SHADNET_NPID'] = str(s.get('supermedo_npid', '')).strip()
            env['BB_SHADNET_PASSWORD'] = str(s.get('supermedo_pass', ''))
        if s.get('online_pool') == 'huntersdream' and HUNTERS_DREAM_POOL:
            # The engine's Hunter's Dream backend with the Discord-linked account (run.py sets
            # the game server and the backend's addresses).
            env['BB_NET_BACKEND'] = 'thd'
            env['BB_THD_ONLINE_ID'] = str(s.get('thd_online_id', ''))
            env['BB_THD_TOKEN'] = str(s.get('thd_token', ''))
            env['BB_BRIDGE_LIVE_MAP'] = '1' if s.get('bridge_live_map') else '0'
            for key in ('BB_SHADNET_SERVER', 'BB_SHADNET_WEBAPI', 'BB_SHADNET_NPID', 'BB_SHADNET_PASSWORD'):
                env.pop(key, None)
    # Which build a server sees (shadNet's client version, The Hunter's Dream's client field).
    # The servers tell clients apart by this User-Agent: name/version (bbport/1.9.1).
    env.setdefault('BB_CLIENT_VERSION', f'bbport/{VERSION}')
    # Started from Steam, the game gets Steam's Vulkan layers. Its shader pre-caching (Fossilize) cannot record
    # the port's pipelines ("Fossilize ERROR: Cannot copy unknown pNext"), and a tester's AMD driver crashed
    # right after; the port keeps its own pipeline cache. Steam's overlay stays.
    env['DISABLE_VK_LAYER_VALVE_steam_fossilize_1'] = '1'
    env['PYTHONUNBUFFERED'] = '1'
    env['PYTHONIOENCODING'] = 'utf-8'
    return env


# ---------------------------------------------------------------------------------------------
# The window.

# The colours of bbport_theme (bbport.de): BG is the side bar, PANEL the pages.
BG, PANEL, CARD, LINE = theme.DEEP, theme.BG, theme.CARD, theme.BORDER
TEXT, MUTED, GOLD, BLOOD, BLOOD_HI = theme.TEXT, theme.MUTED, theme.GOLD_LIGHT, theme.BLOOD, theme.BLOOD_HOVER
DISCORD_BLURPLE, DISCORD_BLURPLE_HOVER = '#5865f2', '#4752c4'  # Discord's own colour: the button stands out
# Trophy glyphs only: the grade colours of the console, toned to the launcher's palette.
TROPHY_COLORS = {trophies.PLATINUM: '#c4ccd8', trophies.GOLD: GOLD, trophies.SILVER: '#adb1b7',
                 trophies.BRONZE: '#b0774f'}
TROPHY_LOCKED = '#34373c'
GRADE_NAMES = {trophies.PLATINUM: ('Platinum', 'Платиновый'), trophies.GOLD: ('Gold', 'Золотой'),
               trophies.SILVER: ('Silver', 'Серебряный'), trophies.BRONZE: ('Bronze', 'Бронзовый')}


# The side bar: the pages a player needs, then the expert's (shown in the expert view only).
NAV_SIMPLE = [('play', ('Play', 'Играть')), ('picture', ('Picture & performance',)),
              ('controls', ('Controls', 'Управление')), ('gameplay', ('Gameplay', 'Игровой процесс')),
              ('online', ('Online play',)), ('saves', ('Saves',)),
              ('mods', ('Mods',)), ('language', ('Launcher customization', 'Настройки лаунчера')),
              ('trophies', ('Achievements', 'Достижения')), ('help', ('Help', 'Помощь'))]
# The game tests (Measure on this PC, the Game tests on System & tests) are off for now, until they play
# a fixed benchmark character instead of the player's last save. True brings them back with their
# page name.
GAME_TESTS = False
# The game folder is set once (usually by the setup guide); a problem with it links there.
NAV_EXPERT = [('graphics', ('All graphics',)), ('game', ('Game files',)), ('cheats', ('Cheats', 'Читы')),
              ('system', ('System & tests',) if GAME_TESTS else ('Your PC',)),
              ('advanced', ('Advanced', 'Дополнительно'))]
EXPERT_PAGES = {name for name, _t in NAV_EXPERT} | {'log'}


def nav_plan(room, savings, count, group, px, steps=(37, 26, 23, 22)):
    """The side bar's entries for the `room` (pixels) their list has under the full head: (their height in
    points, the head, whether the list scrolls). Head 0 is the full one, 1 has narrower gaps (`savings[0]` pixels
    more for the list), 2 has no by-line either (`savings[1]`). Each head takes entries from its height in `steps`
    down to the next one's, never taller than the head before, so more room never makes them flatter; with less
    room than even the flattest need, the list scrolls. Each entry takes px(height) and a pixel's gap, `group` is
    the EXPERT label's (0 in the simple view). The entries had stopped at 26 points before: at 700 pixels high
    the last ones were behind the Expert view switch. The by-line goes last: dropped with the gaps for entries
    under 26 points, it gave them a point or two and left most of its room empty."""
    def tallest(space, most, fewest):
        return next((h for h in range(most, fewest - 1, -1) if group + count * (px(h) + px(1)) <= space), None)
    for head, (most, fewest) in enumerate(zip(steps, steps[1:])):
        height = tallest(room + (0, *savings)[head], most, fewest)
        if height is not None:
            return height, head, False
    return steps[-1], len(savings), True


class Launcher(bbport_setup.SetupMixin, bbport_help.HelpMixin, bbport_search.SearchMixin, bbport_pad.PadMixin):
    def __init__(self, root, tk, ttk, filedialog, messagebox):
        self.tk, self.ttk, self.filedialog, self.messagebox = tk, ttk, filedialog, messagebox
        self.root = root
        saved = load_settings()
        self.app = {**APP_DEFAULTS, **saved}
        if 'thehuntersdream' in str(self.app.get('online_community', '')):  # the game server is supermedo.com's
            self.app['online_community'] = COMMUNITY_SERVER
        if self.app.get('readbacks') == '0':  # Off left the launcher (READBACKS)
            self.app['readbacks'] = ''
        self.app.update(settled_online(self.app, saved))  # dead ends to SuperMedo Server
        self.online_written = {key: saved.get(key) for key in SHADNET_KEYS + OWN_ONLINE_KEYS}
        for key in ('supermedo_user', 'supermedo_password'):  # 1.9.2's account, the password in plain text
            self.app.pop(key, None)
        if self.app.get('ui_mode') not in ('simple', 'expert'):
            # Set up with an earlier launcher: every page as before (expert). New: the simple view.
            self.app['ui_mode'] = 'expert' if saved.get('setup_done') or saved.get('game_dir') else 'simple'
        # A folder without bbport.ini yet (a new installation, a test build beside the old one) gets the
        # settings for this PC once it is rated (auto_settings), not the fixed defaults.
        self.ini_new = not ini_path().exists()
        self.ini, self.ini_lines = load_ini(self.app)
        self.app = mirrored(self.app, self.ini)  # the game's PC Settings page may have changed them
        self.remap_ini_path = input_ini_path(ini_path())
        self.remap_bindings, self.remap_lines, self.remap_toggle_key, self.remap_reload_key, \
            self.mouse_settings = load_input_ini(self.remap_ini_path)
        # {output: [binding or None per slot]}, edited by the remap page and
        # applied on top of self.remap_bindings in collect(); separate from it so re-opening the
        # Controls page after a cancelled edit shows what is actually on disk, not a half-edit.
        self.remap_edits = {}
        self.mouse_edited = False  # set by any mouse widget's command/trace; gates the save in collect()
        if self.app.get('play_with') not in ('controller', 'keyboard'):
            self.app['play_with'] = 'keyboard' if self.mouse_settings['enabled'] else 'controller'
        # The Controller section: {output: controller button or None}, edits apart as above.
        self.controller_bindings = load_controller_bindings(self.remap_lines)
        self.controller_edits = {}
        self.pads = None  # bbport_gamepad.Pads, opened the first time a controller slot listens
        self.fg401_support = None
        self.fsr411_support = None
        self.fsr411_refreshers = []
        self.motion_refreshers = []  # the object motion boxes, greyed out on cards that run without it
        self.fsr411_upscaler_refreshers = []
        self.fg401_refreshers = []
        self.vars = {}
        self.process = self.job = self.placer = None
        self.hidden = False  # closed while the game runs: it exits with the game
        self.tray = None  # bbport_tray.Tray once the launcher went to the notification area
        if self.app.get('on_play') not in [v for v, _t in ON_PLAY]:
            self.app['on_play'] = 'stay'
        if 'on_play' not in saved and saved.get('close_on_play'):
            self.app['on_play'] = 'close'
        self.downloading = False
        self.fsr411_downloading = False
        self.output = queue.Queue()
        self.gpu_text = _('• Checking the graphics card…', '• Проверка видеокарты…')
        self.vulkan = None  # scripts/vulkan_check.py's answer for this PC (asked before PLAY)
        self.banner_image = None
        self.ui_calls = queue.Queue()  # work for the Tk thread from helper threads
        # Tk objects in reference cycles (a picker's watched variables, a hint's window) may only be
        # freed on the Tk thread: Python's collector runs on whichever thread allocates, and from a
        # helper thread it aborts Tcl ("Tcl_AsyncDelete: async handler deleted by the wrong thread").
        # drain_output collects instead, every 2 s.
        gc.disable()
        self._collected = time.monotonic()
        self.mod_order, self.mod_vars, self.patch_vars = [], {}, {}
        self.tracker = trophies.Tracker(self.trophy_dir())
        self.init_hardware()
        self.trophies_loaded, self.toasts = False, []
        root.title('Bloodborne — bbport')
        root.configure(bg=BG)
        self.dpi = root.winfo_fpixels('1i') / 96.0
        # Never taller or wider than the screen (a 1080p laptop at 150 % has about 1280 × 670 points).
        free_w, free_h = root.winfo_screenwidth() - self.px(40), root.winfo_screenheight() - self.px(80)
        root.geometry(f'{min(self.px(1180), free_w)}x{min(self.px(760), free_h)}')
        root.minsize(min(self.px(1000), free_w), min(self.px(680), free_h))
        self.compact = min(self.px(760), free_h) < self.px(740)  # a short screen: smaller banner and emblem
        theme.load_fonts(root)
        self.theme = theme.Theme(root, self.dpi)
        self.set_icon()
        self.style()
        self.build()
        self.show('play')
        root.after(50, self.window_chrome)
        self.maybe_open_setup()
        self.maybe_show_news()
        root.protocol('WM_DELETE_WINDOW', self.close)
        root.after(100, self.drain_output)
        root.after(300, self.poll_trophies)
        self.hunters, self.hunters_when, self.hunters_asking = None, 0, False
        root.after(3000, self.poll_hunters)
        root.after(800, self.temporary_warning)
        threading.Thread(target=self.detect_gpu, daemon=True).start()
        if self.app.get('check_updates', True) and not COOP_TEST:
            threading.Thread(target=self.check_update, daemon=True).start()

    def temporary_warning(self):
        """Once per start of a port opened from inside a zip or rar, its saves in it (saves_in_temporary_folder):
        they would be deleted with that folder, which cannot be helped from here."""
        if not saves_in_temporary_folder(self.app):
            return
        self.messagebox.showwarning('Bloodborne', _(
            'The port runs from a temporary folder ({}), as when it is started from inside a zip or rar file. That '
            'folder is deleted when the archive closes, and your saves with it.\n\nExtract the port to a folder '
            'first (like C:\\Games\\BBPort) and start Bloodborne.exe from there.',
            'Порт запущен из временной папки ({}), как при запуске прямо из zip- или rar-архива. Эта папка '
            'удаляется, когда архив закрывается, а вместе с ней и ваши сохранения.\n\nСначала распакуйте порт '
            'в папку (например, C:\\Games\\BBPort) и запускайте Bloodborne.exe оттуда.').format(PORT_DIR))

    def callback_error(self, kind, value, trace):
        """Tk's handler for an error in a button's or a timer's code. Its own printed to a console the packaged
        launcher does not have, and Play then did nothing without a word: now the error goes to the Log page and
        a message names it (once per error, a timer may repeat it)."""
        import traceback
        text = ''.join(traceback.format_exception(kind, value, trace))
        try:
            sys.stderr.write(text)
        except (AttributeError, OSError, ValueError):
            pass
        try:
            self.append(text)
        except Exception:  # the Log page may not exist yet
            pass
        shown = getattr(self, 'errors_shown', set())
        self.errors_shown = shown
        if text in shown:
            return
        shown.add(text)
        self.messagebox.showerror('Bloodborne', _('Something went wrong in the launcher: {}',
                                                  'В лаунчере произошла ошибка: {}').format(value or kind.__name__) +
                                  '\n\n' + _('The details are in the log (Help > "Show the log"). Help > "Save a '
                                             'report" makes the zip to attach when you ask for help.',
                                             'Подробности в журнале («Помощь» > «Показать журнал»). «Помощь» > '
                                             '«Сохранить отчёт» создаёт zip-файл, который нужно приложить, когда '
                                             'просите о помощи.'))

    def save_settings(self):
        """collect() before PLAY; False with a message when they cannot be written (a folder without write
        permission, a full drive): the game would start with other settings, or not at all."""
        try:
            self.collect()
        except OSError as error:
            path = Path(error.filename or ini_path())
            self.messagebox.showerror('Bloodborne', _(
                'Your settings could not be saved, so the game cannot start with them.\n\nFile: {}\nFolder: {}\n'
                'Windows says: {}\n\nFree up space on that drive, or move the port to a folder you can write to '
                '(like C:\\Games\\BBPort).',
                'Не удалось сохранить настройки, поэтому игра не может запуститься с ними.\n\nФайл: {}\nПапка: {}\n'
                'Windows сообщает: {}\n\nОсвободите место на этом диске или переместите порт в папку, доступную '
                'для записи (например, C:\\Games\\BBPort).').format(path.name, path.parent, error.strerror or error))
            return False
        return True

    def px(self, size):
        return int(size * self.dpi)

    def set_icon(self):
        try:
            self.root.iconbitmap(default=str(PORT_DIR / 'launcher' / 'bloodborne.ico'))
            return
        except self.tk.TclError:
            pass
        for icon in (PORT_DIR / 'launcher' / 'bloodborne.png', Path(self.app['game_dir']) / 'sce_sys' / 'icon0.png'):
            try:
                self.icon = self.tk.PhotoImage(file=str(icon))
                if self.icon.width() > 128:
                    self.icon = self.icon.subsample(self.icon.width() // 64)
                self.root.iconphoto(True, self.icon)
                return
            except (self.tk.TclError, OSError):
                continue

    # ---- look --------------------------------------------------------------------------------
    def style(self):
        """bbport_theme's ttk style: rounded buttons and fields, switches, cards."""
        s = self.ttk.Style(self.root)
        self.theme.style(s)
        # FSR 4.1.1's provider menu (Graphics)
        s.configure('Fsr411.TMenubutton', background=CARD, arrowcolor=GOLD, padding=4)
        s.map('Fsr411.TMenubutton', foreground=[('disabled', MUTED)], background=[('active', CARD)])
        s.configure('Unavailable.Fsr411.TMenubutton', foreground=MUTED)

    # ---- widget helpers ----------------------------------------------------------------------
    def var(self, key, store):
        """The Tk variable of a setting (one per setting, shared by every widget that shows it)."""
        tk = self.tk
        if key not in self.vars:
            if store == 'ini':
                value = self.ini.get(key, INI_DEFAULTS[key])
                if key in INI_FLAGS:
                    v = tk.BooleanVar(value=value == '1')
                elif key == 'sharpness':
                    v = tk.DoubleVar(value=float(value or 0.5))
                else:
                    v = tk.StringVar(value=value)
            else:
                value = self.app.get(key, APP_DEFAULTS[key])
                if isinstance(APP_DEFAULTS[key], bool):
                    v = tk.BooleanVar(value=bool(value))
                else:
                    v = tk.StringVar(value=str(value))
            v.store = store
            self.vars[key] = v
        return self.vars[key]

    def choice(self, parent, key, store, options, width=38, style='TCombobox'):
        """A combobox over (value, (english, russian)) pairs, kept in sync with its variable."""
        if key == 'upscaler':
            return bbport_fsr41_ui.upscaler_choice(self, parent, options, width, _)
        var = self.var(key, store)
        values = [v for v, _t in options]
        box = self.ttk.Combobox(parent, values=[_(*text) for _v, text in options], state='readonly', width=width,
                               style=style)
        if var.get() not in values:
            var.set(values[0])

        def show(*_args):
            current = var.get()
            box.current(values.index(current) if current in values else 0)
        show()
        box.bind('<<ComboboxSelected>>', lambda _event: var.set(values[box.current()]))
        var.trace_add('write', show)
        box.setting_key = key  # for the search (row() indexes it)
        return box

    def fg401_choice(self, parent, width=52):
        var = self.var('frame_gen', 'ini')
        box = self.ttk.Combobox(parent, state='readonly', width=width)
        values = []
        def refresh(*_args):
            def ready(value):
                status = bbport_fg401.status_for(self.fg401_support, value)
                return bool(status and status.get('supported'))
            options = [(v,t) for v,t in FRAME_GENERATION if v not in ('fsr401','fsr402') or ready(v)]
            values[:] = [v for v,t in options]
            box.configure(values=[_(*t) for v,t in options])
            if var.get() in ('fsr401','fsr402') and not ready(var.get()):
                # Display the saved choice without offering it as a new unsupported choice.
                version = '4.0.2' if var.get() == 'fsr402' else '4.0.1'
                key = 'ML-FG 4.0.1: checking support…' if self.fg401_support is None else bbport_fg401.KEYS[0]
                box.set(_(key.replace('4.0.1', version)))
            else:
                box.current(values.index(var.get()) if var.get() in values else values.index('off'))
            self.refresh_fg401_status()
        box.bind('<<ComboboxSelected>>', lambda _e: var.set(values[box.current()]))
        var.trace_add('write', refresh)
        self.fg401_refreshers.append(refresh)
        refresh()
        box.setting_key = 'frame_gen'  # for the search (row() indexes it)
        return box

    def refresh_fg401(self):
        for refresh in self.fg401_refreshers:
            refresh()
        self.refresh_fg401_status()

    def refresh_fg401_status(self):
        if hasattr(self, 'fg401_status'):
            selected = self.var('frame_gen', 'ini').get()
            text = bbport_fg401.message(bbport_fg401.status_for(self.fg401_support, selected), _,
                hdr=bool(self.var('hdr', 'app').get()), selected=selected in ('fsr401','fsr402'),
                version='4.0.2' if selected == 'fsr402' else '4.0.1') if selected in ('fsr401','fsr402') else ''
            self.fg401_status.configure(text=text)
            for widget in (self.fg401_status, getattr(self, 'fg401_recheck', None)):
                if widget is not None:
                    widget.grid() if text else widget.grid_remove()

    def recheck_fg401(self):
        self.fg401_support = None
        self.refresh_fg401()
        def work():
            try:
                support = bbport_fg401.query_versions(PORT_DIR, bbport_fsr411.environment(PORT_DIR, self.app))
            except Exception as error:  # never "checking support…" for good
                support = fg401_check_failed()
                self.ui_calls.put(lambda e=error_text(error, kind=True): self.append(
                    _('The check of the graphics card stopped: {}').format(e) + '\n'))
            def done():
                self.fg401_support = support
                self.refresh_fg401()
            self.ui_calls.put(done)
        threading.Thread(target=work, daemon=True).start()

    def fsr411_choice(self, parent, width=38):
        return bbport_fsr41_ui.settings_button(self, parent, _)

    def open_fsr41_settings(self):
        return bbport_fsr41_ui.open_settings(self, _)

    def size_entry(self, parent, text, used, note):
        """An entry for a size like 2400x1080: used once it is a size (when left or confirmed);
        otherwise `note` says what it takes."""
        entry = self.ttk.Entry(parent, textvariable=text, width=12)

        def done(_e=None):
            size = display.parse_size(text.get())
            if size:
                text.set(display.size_text(size))
                used(size)
            else:
                note.configure(text=_('Width × height, e.g. 2400x1080 (from 640x360 to 7680x4320).'),
                               foreground=theme.AMBER)
        entry.bind('<FocusOut>', done)
        entry.bind('<Return>', done)
        return entry

    def game_monitor_size(self):
        m = display.monitor(self.var('game_monitor', 'app').get())
        return m.size if m else (self.root.winfo_screenwidth(), self.root.winfo_screenheight())

    def output_choice(self, parent):
        """Output resolution: the sizes of OUTPUTS, the game monitor's own (set again at every start)
        or any size of the player's (the port takes any; bbport.ini keeps it)."""
        ttk = self.ttk
        res, match = self.var('output_res', 'ini'), self.var('output_display', 'app')
        sizes = [value for value, _t in OUTPUTS]
        values = ['display', *sizes, 'custom']
        holder = ttk.Frame(parent)
        box = ttk.Combobox(holder, state='readonly', width=30,
                           values=[_('Same as the monitor'), *[_(*text) for _v, text in OUTPUTS], _('Custom size…')])
        box.pack(side='left')
        own = self.tk.StringVar(value=res.get())
        note = ttk.Label(holder, style='Muted.TLabel', wraplength=self.px(240), justify='left')
        note.pack(side='left', padx=(10, 0))
        picked = {'custom': res.get() not in sizes}

        def typed(size):
            text = display.size_text(size)
            if text in sizes:
                self.app['output_custom'] = ''
            res.set(text)
        entry = self.size_entry(holder, own, typed, note)

        def show(*_a):
            pick = 'display' if match.get() else 'custom' if picked['custom'] or res.get() not in sizes else res.get()
            box.current(values.index(pick))
            if pick == 'custom':
                try:
                    typing = self.root.focus_get() is entry
                except KeyError:  # the focus is in a combobox's list
                    typing = False
                if not typing:
                    own.set(res.get())
                entry.pack(side='left', padx=(8, 0), before=note)
            else:
                entry.pack_forget()
            size = display.parse_size(res.get())
            text = _('now {} × {}').format(*self.game_monitor_size()) if pick == 'display' else \
                _('Narrower than 16:9: the picture may look stretched.') if size and display.narrower_than_16_9(size) \
                else ''
            note.configure(text=text, foreground=theme.AMBER if pick != 'display' and text else theme.MUTED)

        def choose(_e):
            pick = values[box.current()]
            picked['custom'] = pick == 'custom'
            if pick != 'custom':  # chosen here: 1920x1080 too is the player's choice then
                self.app['output_custom'] = ''
            match.set(pick == 'display')
            if pick == 'display':
                res.set(display.size_text(self.game_monitor_size()))
            elif pick != 'custom':
                res.set(pick)
            show()
            if pick == 'custom':
                entry.focus_set()
                entry.select_range(0, 'end')

        def changed(*_a):
            # another page (the guide, automatic settings) chose a size: that one counts now
            if match.get() and res.get() != display.size_text(self.game_monitor_size()):
                match.set(False)
            if res.get() in sizes:
                picked['custom'] = False
            show()

        def monitor_changed(*_a):
            if match.get():
                res.set(display.size_text(self.game_monitor_size()))
            show()
        box.bind('<<ComboboxSelected>>', choose)
        res.trace_add('write', changed)
        match.trace_add('write', show)
        self.var('game_monitor', 'app').trace_add('write', monitor_changed)
        show()
        holder.setting_key = 'output_res'
        return holder

    def select_fsr411_backend(self, method):
        if self.var('upscaler', 'ini').get() not in ('fsr411','fsr412') or getattr(self, 'fsr411_downloading', False):
            return
        backend = self.var('fsr411_backend', 'ini')
        status = self.fsr41_status(method)
        if status['supported']:
            backend.set(method)
        elif self.var('upscaler','ini').get() == 'fsr411' and method == 'vk' and not getattr(self, 'process', None) and bbport_fsr411.can_download(
                status, getattr(self, 'fsr411_support', None)):
            if not self.messagebox.askyesno(bbport_fsr411.DOWNLOAD_TITLE, _(bbport_fsr411.DOWNLOAD_QUESTION), parent=self.root):
                return
            previous = backend.get()
            def activate():
                if self.var('upscaler', 'ini').get() == 'fsr411' and backend.get() == previous:
                    backend.set('vk')
            self.download_fsr411(on_success=activate)

    def readouts_section(self, f):
        """The FPS counter's lines, one box each in two columns (also in the in-game menu)."""
        self.section(f, _('FPS counter'))
        boxes = self.ttk.Frame(f)
        boxes.grid(row=self.next_row(f), column=0, columnspan=2, sticky='w')
        for index, (key, title) in enumerate(READOUTS):
            box = self.ttk.Checkbutton(boxes, text=_(*title), variable=self.var(key, 'ini'))
            box.grid(row=index // 2, column=index % 2, sticky='w', padx=(0, self.px(32)), pady=(self.px(4), 0))
            self.index_entry(box, _(*title), None, key)
        self.row(f, _('Value colours'), self.choice(f, 'counter_colors', 'ini', COUNTER_COLORS),
                 _('Colour-coded shows at a glance how each value stands; plain keeps the counter quiet.'),
                 key='counter_colors')
        self.note(f, _('One box per line of the counter in the top right; also in the in-game menu. Render '
                       'latency is the time from the game starting a frame to it being shown, which Low latency '
                       'lowers; frame time is the time between two frames.'), top=6)

    def detected_vendor(self):
        """The graphics card's PCI vendor id (bb-gpu-capabilities), None until the check answered."""
        support = getattr(self, 'fsr411_support', None) or {}
        return (support.get('gpu') or {}).get('vendor_id')

    def motion_unavailable(self):
        support = getattr(self, 'fsr411_support', None) or {}
        return object_motion_unavailable(support.get('gpu'), getattr(self, 'gpu_line', ''),
                                         self.app.get('extra_env', ''))

    def refresh_motion(self):
        for refresh in self.motion_refreshers:
            refresh()

    def next_row(self, parent):
        return parent.grid_size()[1]

    def motion_cache_check(self, parent):
        upscaler = self.var('upscaler', 'ini')
        object_motion = self.var('object_motion', 'ini')
        button = self.ttk.Checkbutton(parent, text=_('Motion-vector cache (experimental)'),
                                     variable=self.var('object_motion_cache', 'ini'))
        row = self.next_row(parent)
        button.grid(row=row, column=0, columnspan=2, sticky='w', pady=(8, 0))
        self.wrap_within(self.ttk.Label(parent, text=_(
            'May solve general FSR FPS drops; uses extra VRAM. Requires object motion vectors. '
            'Applies at next game start.'), style='Muted.TLabel', justify='left', anchor='w'), 640).grid(
            row=row + 1, column=0, columnspan=2, sticky='we', padx=(26, 0))
        self.index_entry(button, _('Motion-vector cache (experimental)'), key='object_motion_cache')

        def refresh(*_args):
            available = motion_cache_available({'upscaler': upscaler.get(),
                                                'object_motion': object_motion.get()}, self.motion_unavailable())
            button.state(['!disabled' if available else 'disabled'])
        button.refresh = refresh

        traces = [(var, var.trace_add('write', refresh)) for var in (upscaler, object_motion)]

        def cleanup(event):
            if event.widget == button:
                for var, trace in traces:
                    var.trace_remove('write', trace)

        button.bind('<Destroy>', cleanup)
        refresh()
        return button

    def row(self, parent, title, widget, hint=None, key=None):
        ttk = self.ttk
        r = self.next_row(parent)
        label = ttk.Label(parent, text=title)
        label.grid(row=r, column=0, sticky='w', padx=(0, 18), pady=(6, 0))
        widget.grid(row=r, column=1, sticky='w', pady=(6, 0))
        if hint:
            self.wrap_within(ttk.Label(parent, text=hint, style='Muted.TLabel', justify='left', anchor='w'), 560).grid(
                row=r + 1, column=1, sticky='we', pady=(2, 2))
        self.index_entry(widget, title, hint, key or self.setting_key_of(widget), mark=label)
        return widget

    def check(self, parent, key, store, title, hint=None):
        ttk = self.ttk
        r = self.next_row(parent)
        box = ttk.Checkbutton(parent, text=title, variable=self.var(key, store))
        box.grid(row=r, column=0, columnspan=2, sticky='w', pady=(4, 0))
        self.index_entry(box, title, hint, key)
        if hint:
            self.wrap_within(ttk.Label(parent, text=hint, style='Muted.TLabel', justify='left', anchor='w'), 640).grid(
                row=r + 1, column=0, columnspan=2, sticky='we', padx=(26, 0))
        return box

    def wrap_within(self, label, wrap):
        """`label` wraps at `wrap` points, or narrower where it has less room: a fixed width ran its lines past
        the right edge of a narrow window, cut off. Its room is what its parent gives it (packed with fill='x',
        gridded sticky 'we'), less a few pixels for the label's own border. A label as wide as it asks for has
        no other room to go by: it stays as it is (else it would narrow itself step by step)."""
        px = self.px

        def fit(event):
            if event.width != label.winfo_reqwidth():
                label.configure(wraplength=max(px(120), min(px(wrap), event.width - px(8))))
        label.configure(wraplength=px(wrap))
        label.bind('<Configure>', fit, add='+')
        return label

    def note(self, parent, text, top=12):
        self.wrap_within(self.ttk.Label(parent, text=text, style='Muted.TLabel', justify='left', anchor='w'), 680).grid(
            row=self.next_row(parent), column=0, columnspan=2, sticky='we', pady=(top, 0))

    def fold(self, parent, title, texts, top=10):
        """Details most players never need: a gold link that shows or hides the notes `texts`."""
        link = self.tk.Label(parent, text=title + '  ›', bg=PANEL, fg=GOLD, cursor='hand2', font=(theme.BODY_MEDIUM, 9))
        link.grid(row=self.next_row(parent), column=0, columnspan=2, sticky='w', pady=(top, 0))
        notes = []
        for text in texts:
            label = self.wrap_within(self.ttk.Label(parent, text=text, style='Muted.TLabel', justify='left', anchor='w'),
                                     680)
            label.grid(row=self.next_row(parent), column=0, columnspan=2, sticky='we', pady=(6, 0))
            label.grid_remove()
            notes.append(label)

        def toggle(_e=None):
            shown = bool(notes[0].winfo_manager())
            for label in notes:
                label.grid_remove() if shown else label.grid()
            link.configure(text=title + ('  ›' if shown else '  ⌄'))
        link.bind('<Button-1>', toggle)

    def section(self, parent, title, top=18):
        """A heading in Cinzel with a hairline running to the right edge."""
        head = self.ttk.Frame(parent)
        head.grid(row=self.next_row(parent), column=0, columnspan=2, sticky='we', pady=(top + 6, 6))
        self.ttk.Label(head, text=title, style='Section.TLabel').pack(side='left')
        self.tk.Frame(head, bg=LINE, height=1).pack(side='left', fill='x', expand=True, padx=(14, 0), pady=(4, 0))

    def folder(self, parent, key, title, prompt, hint=None, on_change=None):
        ttk = self.ttk
        var = self.var(key, 'app')
        holder = ttk.Frame(parent)
        ttk.Entry(holder, textvariable=var, width=54).pack(side='left')

        def browse():
            chosen = self.filedialog.askdirectory(title=prompt, initialdir=var.get() or str(PORT_DIR))
            if chosen:
                var.set(str(Path(chosen)))
        ttk.Button(holder, text=_('Browse…', 'Обзор…'), command=browse).pack(side='left', padx=(6, 0))
        ttk.Button(holder, text=_('Open', 'Открыть'), command=lambda: self.open_path(var.get(), key)).pack(
            side='left', padx=(6, 0))
        if on_change:
            var.trace_add('write', lambda *_a: on_change())
        holder.setting_key = key
        return self.row(parent, title, holder, hint)

    def page_header(self, parent, title, subtitle):
        """A page's head: its title over the cathedral art, fading into the page."""
        c = self.tk.Canvas(parent, height=self.px(118), bg=PANEL, highlightthickness=0, bd=0)

        def draw():
            c.pending = None
            w, h = max(c.winfo_width(), 200), int(c['height'])
            c.delete('all')
            c.art = self.theme.cached(('header', w, h), lambda: theme.header((w, h), PANEL))
            c.create_image(0, 0, image=c.art, anchor='nw')
            c.create_text(self.px(32), self.px(48), text=title, anchor='w', fill=theme.TEXT_STRONG,
                          font=(theme.HEAD, 21))
            if subtitle:
                # As wide as the page's cards (its padding and scroll bar lie at the right): it reached the window's
                # edge. A narrow window wraps it into more lines, and the head grows with them (drawn again).
                text = c.create_text(self.px(33), self.px(72), text=subtitle, anchor='nw', fill=theme.TEXT_SOFT,
                                     font=(theme.BODY, 9), width=w - self.px(80))
                need = max(self.px(118), c.bbox(text)[3] + self.px(14))
                if need != h:
                    c.configure(height=need)

        def schedule(event):
            if getattr(c, 'pending', None):
                c.after_cancel(c.pending)
                c.pending = None
            if (event.width, event.height) == getattr(c, 'drawn', None):
                return  # shown again at the same size (a page change, back from the task bar)
            c.drawn = (event.width, event.height)
            if ('header', max(event.width, 200), int(c['height'])) in self.theme.cache:
                draw()  # a size every page has: at once, so the page never shows without it
            else:
                c.pending = c.after(30, draw)  # while the window is being resized: once it rests
        c.bind('<Configure>', schedule)
        return c

    def scrolled_page(self, name, title, subtitle):
        """A settings page: a heading and content that scrolls vertically."""
        tk, ttk = self.tk, self.ttk
        outer = ttk.Frame(self.content)
        self.page_header(outer, title, subtitle).pack(fill='x')
        canvas = tk.Canvas(outer, bg=PANEL, highlightthickness=0, bd=0)
        bar = ttk.Scrollbar(outer, orient='vertical', command=canvas.yview)
        inner = ttk.Frame(canvas, padding=(self.px(32), 0, self.px(32), self.px(28)))
        inner.columnconfigure(1, weight=1)
        window = canvas.create_window(0, 0, window=inner, anchor='nw')
        inner.bind('<Configure>', lambda _e: canvas.configure(scrollregion=canvas.bbox('all')))
        canvas.bind('<Configure>', lambda e: canvas.itemconfigure(window, width=e.width))
        canvas.configure(yscrollcommand=bar.set, yscrollincrement=1)  # scrolled in pixels (wheel())
        bar.pack(side='right', fill='y', padx=(0, self.px(4)), pady=(0, self.px(8)))
        canvas.pack(side='left', fill='both', expand=True)
        outer.scroll = lambda pixels: canvas.yview_scroll(pixels, 'units') if inner.winfo_height() > canvas.winfo_height() else None
        # A canvas maps the page in it only when it draws, and it draws when it is seen: asked to now,
        # the page is built under the cover (Launcher.cover), not after it goes.
        outer.redisplay = lambda: canvas.itemconfigure(window, state='normal')
        self.pages[name] = outer
        return inner

    # ---- layout ------------------------------------------------------------------------------
    def build(self):
        tk, ttk, px = self.tk, self.ttk, self.px
        self.custom_frame = None  # bbport_frame.Frame once installed (window_chrome), False if it failed
        self.titlebar = None if self.app.get('windows_title_bar') else self.build_titlebar()
        self.root.bind('<Map>', self.restored, add='+')
        self.root.bind('<Unmap>', self.minimized, add='+')
        side = tk.Frame(self.root, bg=BG, width=px(248))
        side.pack(side='left', fill='y')
        side.pack_propagate(False)
        tk.Frame(self.root, bg=LINE, width=1).pack(side='left', fill='y')
        self.side = side
        brand = tk.Frame(side, bg=BG)
        brand.pack(fill='x', padx=px(20), pady=(px(20), px(4)))
        self.brand = brand
        mark = theme.emblem(px(48), glow=False)
        if mark is not None:
            self.brand_mark = self.theme.photo(mark, BG)
            tk.Label(brand, image=self.brand_mark, bg=BG, bd=0).pack(side='left', padx=(0, px(10)))
        words = tk.Frame(brand, bg=BG)
        words.pack(side='left', fill='x')
        tk.Label(words, text='BLOODBORNE', bg=BG, fg=theme.TEXT_STRONG, font=(theme.HEAD_BOLD, 14)).pack(anchor='w')
        tk.Label(words, text='PC PORT', bg=BG, fg=GOLD, font=(theme.HEAD, 8)).pack(anchor='w')
        self.byline = tk.Label(side, text=_('by Super Medo') + f'  ·  v{VERSION_TEXT}' +
                               ('\n' + _('co-op test build') if COOP_TEST else ''),
                               bg=BG, fg=MUTED, justify='left', font=(theme.BODY, 9))
        self.byline_pads = ((px(2), px(8)), (0, px(4)))  # its gaps under the full head and the narrower ones
        self.byline.pack(anchor='w', padx=px(22), pady=self.byline_pads[0])
        self.build_search(side)  # bbport_search: settings and help by the player's own words
        self.nav, self.current_page = {}, None
        # Shorter entries on a short screen, so the expert view (13 pages) still fits.
        self.nav_h = 37 if not self.compact else 32
        self.nav_pills = {'on': self.theme.box(224, self.nav_h, 8, CARD, LINE, bg=BG),
                          'hover': self.theme.box(224, self.nav_h, 8, theme.SURFACE, bg=BG)}
        accent = self.theme.box(3, 18, 1.5, theme.BLOOD_LIGHT, bg=CARD)
        # The entries lie in a list of their own, laid at the top of the room between the search and the switch:
        # moved up and down by the wheel when even the flattest entries do not fit (fit_nav, nav_scroll).
        self.nav_list = tk.Frame(side, bg=BG)
        self.nav_list.pack(fill='both', expand=True)
        self.nav_inner = tk.Frame(self.nav_list, bg=BG)
        self.nav_inner.place(x=0, y=0, relwidth=1)
        self.nav_scrolls, self.nav_offset, self.nav_head = False, 0, 0
        # Shown while entries lie above or below the list (nav_scroll): a hairline across its cut edge and a gold
        # chevron (a link's colour: a click moves the list) in the gutter right of the entries, where it covers no
        # pill. A small grey caret alone went unseen.
        self.nav_arrows, self.nav_edges = [], []
        for glyph, step in (('', -80), ('', 80)):  # ChevronUpMed, ChevronDownMed
            arrow = tk.Label(self.nav_list, text=glyph, bg=BG, fg=GOLD, font=(theme.ICONS, 8), bd=0, padx=0,
                             pady=px(3), cursor='hand2')
            arrow.bind('<Button-1>', lambda _e, step=step: self.nav_scroll(px(step)))
            arrow.bind('<Enter>', lambda _e, arrow=arrow: arrow.configure(fg=theme.GOLD_LIGHT))
            arrow.bind('<Leave>', lambda _e, arrow=arrow: arrow.configure(fg=GOLD))
            self.nav_arrows.append(arrow)
            self.nav_edges.append(tk.Frame(self.nav_list, bg=LINE, height=1))
        self.nav_group = tk.Label(self.nav_inner, text=_('EXPERT'), bg=BG, fg=MUTED, font=(theme.BODY_SEMI, 8),
                                  anchor='w')
        for name, title in NAV_SIMPLE + NAV_EXPERT:
            if name == NAV_EXPERT[0][0]:
                self.nav_group.pack(fill='x', padx=px(24), pady=(px(8), px(2)))
            c = tk.Canvas(self.nav_inner, width=px(224), height=px(self.nav_h), bg=BG, highlightthickness=0, bd=0,
                          cursor='hand2')
            c.pack(padx=px(12), pady=(0, px(1)))
            mid = px(self.nav_h / 2)
            items = {'pill': c.create_image(0, 0, anchor='nw', state='hidden'),
                     'bar': c.create_image(px(1), mid, anchor='w', image=accent, state='hidden'),
                     'icon': c.create_text(px(22), mid, text=theme.ICON[name], font=(theme.ICONS, 12)),
                     'label': c.create_text(px(44), mid, text=_(*title), anchor='w', font=(theme.BODY_MEDIUM, 10))}
            c.bind('<Button-1>', lambda _e, n=name: self.show(n))
            c.bind('<Enter>', lambda _e, n=name: self.nav_paint(n, hover=True))
            c.bind('<Leave>', lambda _e, n=name: self.nav_paint(n))
            self.nav[name] = (c, items)
            self.nav_paint(name)
        self.update_box = None
        self.version_window = None  # Advanced > Other versions
        self.versions_listed = False  # its list arrived: its Install may be used
        self.updating = False  # an update is being downloaded or started: one at a time (update_buttons)
        self.installing = False  # its installer is starting and this launcher closes for it (update_installing)
        # The view: the switch never changes a setting, only which pages are shown.
        expert = tk.BooleanVar(value=self.app['ui_mode'] == 'expert')
        self.expert_var = expert
        # Packed ahead of the search and the entries: the switch and the link stay at the bottom
        # however short the window gets (the entries get flatter instead, fit_nav).
        # The community's Discord and website as two buttons in the corner (they were small gold links, and
        # most players never found the Discord): the Discord in its own blurple, the site as a card.
        site = COMMUNITY_SITE.split('//')[1].strip('/')
        self.community_buttons = []
        for icon, text, url, fill, hover, outline, colour in (
                ('\ue8f2', _('Join our Discord'), bbport_help.DISCORD, DISCORD_BLURPLE, DISCORD_BLURPLE_HOVER,
                 None, '#ffffff'),
                ('\ue774', site, COMMUNITY_SITE, CARD, theme.SURFACE, LINE, TEXT)):
            pills = (self.theme.box(224, 36, 8, fill, outline, bg=BG), self.theme.box(224, 36, 8, hover, outline, bg=BG))
            c = tk.Canvas(side, width=px(224), height=px(36), bg=BG, highlightthickness=0, bd=0, cursor='hand2')
            c.create_image(0, 0, anchor='nw', image=pills[0], tags='pill')
            c.create_text(px(20), px(18), text=icon, fill=colour, font=(theme.ICONS, 11))
            label = c.create_text(px(40), px(18), text=text, fill=colour, anchor='w', font=(theme.BODY_SEMI, 10))
            for size in (9, 8):  # a long translation (German, Polish) ran into the arrow: a size smaller then
                if c.bbox(label)[2] <= px(198):
                    break
                c.itemconfigure(label, font=(theme.BODY_SEMI, size))
            c.create_text(px(208), px(18), text='\u2197', fill=colour, font=(theme.BODY, 10))
            c.bind('<Button-1>', lambda _e, u=url: webbrowser.open(u))
            c.bind('<Enter>', lambda _e, c=c, p=pills[1]: c.itemconfigure('pill', image=p))
            c.bind('<Leave>', lambda _e, c=c, p=pills[0]: c.itemconfigure('pill', image=p))
            c.pills = pills  # kept: Tk does not hold the images
            self.community_buttons.append(c)
        # Packed from the bottom: the site last, the Discord above it.
        self.community_buttons[1].pack(side='bottom', padx=px(12), pady=(px(4), px(14)), before=self.search_box)
        self.community_buttons[0].pack(side='bottom', padx=px(12), pady=(px(6), 0), before=self.search_box)
        self.mode_switch = ttk.Checkbutton(side, text=_('Expert view'), variable=expert, command=self.toggle_mode,
                                           style='Bar.TCheckbutton')  # drawn on the side bar's colour
        self.mode_switch.pack(side='bottom', anchor='w', padx=px(22), pady=(px(4), px(6)), before=self.search_box)
        # Found by the search too ("expert view", "website"): the jump marks them where they are.
        self.index_entry(self.mode_switch, _('Expert view'), key='button:expert')
        self.index_entry(self.community_buttons[1], site, _('Install guide and FAQ on bbport.de'), key='button:site')
        # A short window narrows these gaps (their pady: full, narrow) and then takes the by-line away before the
        # entries get too flat to read (fit_nav).
        self.head_pads = [(brand, (px(20), px(4)), (px(8), px(2))), (self.search_box, (0, px(10)), (0, px(6))),
                          (self.mode_switch, (px(4), px(6)), (px(2), px(3))),
                          (self.community_buttons[1], (px(4), px(14)), (px(4), px(8)))]
        # The list's room changes with the window and with the update notice above the switch.
        self.nav_list.bind('<Configure>', lambda _e: self.root.after_idle(self.fit_nav))
        self.apply_mode()

        right = tk.Frame(self.root, bg=PANEL)
        right.pack(side='left', fill='both', expand=True)
        bar = tk.Frame(right, bg=BG, height=px(80))
        bar.pack(fill='x', side='bottom')
        bar.pack_propagate(False)
        tk.Frame(right, bg=LINE, height=1).pack(fill='x', side='bottom')
        self.play_button = ttk.Button(bar, text=_('PLAY', 'ИГРАТЬ'), style='Play.TButton', command=self.play)
        self.play_button.pack(side='right', padx=(px(10), px(24)))
        # Stop only while the game runs (beside Play, game_running): a greyed-out button the rest of the time
        self.stop_button = ttk.Button(bar, text=_('Stop', 'Остановить'), style='Bar.TButton', command=self.stop,
                                      state='disabled')
        # The BB Report Tool's zip (Help > Save a report) right here, with this port folder's log; the log
        # itself is on Help > Show the log.
        self.bar_report = ttk.Button(bar, text=_('Report'), style='Bar.TButton',
                                     command=lambda: self.save_report(self.bar_report, self.status))
        self.bar_report.pack(side='right', padx=(0, px(8)))
        self.status = tk.Label(bar, text='', bg=BG, fg=MUTED, font=(theme.BODY, 9), anchor='w', justify='left')
        self.status.pack(side='left', fill='x', expand=True, padx=(px(28), px(16)))
        self.status.bind('<Configure>', lambda e: self.status.configure(wraplength=max(px(120), e.width)))
        self.content = tk.Frame(right, bg=PANEL)
        self.content.pack(fill='both', expand=True)

        self.pages = {}
        self.build_play()
        self.build_picture()
        self.build_controls()
        self.build_online()
        self.build_saves()
        self.build_mods()
        self.build_language()
        self.build_trophies()
        self.build_help()
        self.build_graphics()
        self.build_game()
        self.build_gameplay()
        self.build_cheats()
        self.build_system()
        self.build_advanced()
        self.build_log()
        self.search_ready()
        self.show_goal()
        self.root.bind_all('<MouseWheel>', self.wheel)
        self.root.bind_class('TCombobox', '<MouseWheel>', self.combobox_wheel)
        self.refresh_status()

    def toggle_mode(self):
        self.app['ui_mode'] = 'expert' if self.expert_var.get() else 'simple'
        self.apply_mode()
        if self.current_page in EXPERT_PAGES and self.app['ui_mode'] == 'simple':
            self.show('play')
        self.refresh_status()

    def apply_mode(self):
        """Shows the expert's pages in the side bar, or hides them (simple view)."""
        expert = self.app.get('ui_mode') == 'expert'
        px = self.px
        self.nav_group.pack_forget()
        for name, _title in NAV_EXPERT:
            self.nav[name][0].pack_forget()
        if expert:  # after the simple view's entries in the list
            self.nav_group.pack(fill='x', padx=px(24), pady=(px(8), px(2)))
            for name, _title in NAV_EXPERT:
                self.nav[name][0].pack(padx=px(12), pady=(0, px(1)))
        self.root.after_idle(self.fit_nav)

    def fit_nav(self):
        """The side bar's entries as tall as the window allows, so that every entry and the switch below them
        stay visible (nav_plan): 37 points, 26 on a short window or a high scaling; shorter still, the gaps of
        the head narrow for entries down to 23 points, and only then does the by-line go, for 22. Only where even
        that is too tall (a 768 pixels high screen at 125 %, an update notice on the smallest window) does the
        list scroll, with the page shown kept in view."""
        px = self.px
        try:
            if self.side.winfo_height() < px(200):
                return  # not laid out yet
            shown = [name for name in self.nav if self.nav[name][0].winfo_manager()]
            group = self.nav_group.winfo_reqheight() + px(8) + px(2) if self.nav_group.winfo_manager() else 0
            full, narrow = (sum(pads) for pads in self.byline_pads)
            gaps = full - narrow + sum(sum(wide) - sum(tight) for _widget, wide, tight in self.head_pads)
            savings = (gaps, gaps + narrow + self.byline.winfo_reqheight())
            # the room the list would have under the full head, also while another one is shown
            room = self.nav_list.winfo_height() - (0, *savings)[self.nav_head]
        except self.tk.TclError:
            return
        if not shown:
            return
        height, head, self.nav_scrolls = nav_plan(room, savings, len(shown), group, px)
        if head != self.nav_head:
            self.nav_head = head
            for widget, wide, tight in self.head_pads:
                widget.pack_configure(pady=tight if head else wide)
            if head == 2:
                self.byline.pack_forget()
            else:
                self.byline.pack(anchor='w', padx=px(22), pady=self.byline_pads[head], after=self.brand)
        if height != self.nav_h:
            self.nav_h = height
            self.nav_pills = {'on': self.theme.box(224, height, 8, CARD, LINE, bg=BG),
                              'hover': self.theme.box(224, height, 8, theme.SURFACE, bg=BG)}
            mid = px(height / 2)
            for name, (c, items) in self.nav.items():
                c.configure(height=px(height))
                c.coords(items['pill'], 0, 0)
                c.coords(items['bar'], px(1), mid)
                c.coords(items['icon'], px(22), mid)
                c.coords(items['label'], px(44), mid)
                self.nav_paint(name)
        self.root.after_idle(lambda: self.nav_scroll(reveal=self.current_page))  # the page shown in view

    def nav_scroll(self, pixels=0, reveal=None):
        """Moves the side bar's list by `pixels` while it scrolls, so that the entry `reveal` is in view if given;
        at its top while every entry fits. A hairline and a chevron at an edge say that entries lie beyond it."""
        offset = most = 0
        try:
            if self.nav_scrolls:
                room = self.nav_list.winfo_height()
                most = max(0, self.nav_inner.winfo_reqheight() - room)
                offset = self.nav_offset + pixels
                entry = self.nav.get(reveal, (None,))[0]
                if entry is not None and entry.winfo_manager():
                    self.root.update_idletasks()  # where the entries lie in the list now
                    top = entry.winfo_y()
                    offset = min(max(offset, top + entry.winfo_reqheight() - room), top)
                offset = min(most, max(0, int(offset)))
        except self.tk.TclError:
            return
        if offset != self.nav_offset:
            self.nav_offset = offset
            self.nav_inner.place_configure(y=-offset)
        above, below = offset > 0, offset < most - self.px(2)  # the last entry's gap under it is no entry
        for arrow, edge, more, rely, side in ((self.nav_arrows[0], self.nav_edges[0], above, 0, 'n'),
                                              (self.nav_arrows[1], self.nav_edges[1], below, 1, 's')):
            if more:
                edge.place(x=self.px(12), rely=rely, anchor=side + 'w', width=self.px(224), height=1)
                arrow.place(relx=1, rely=rely, anchor=side + 'e')
                edge.lift()
                arrow.lift()
            else:
                edge.place_forget()
                arrow.place_forget()

    TITLE_ICONS = {'minimize': '\ue921', 'maximize': '\ue922', 'restore': '\ue923', 'close': '\ue8bb'}

    def build_titlebar(self):
        """The launcher's own title bar: a drag area in the side bar's colour and the window's three
        buttons. Windows' caption goes once the window exists (window_chrome, bbport_frame)."""
        tk, px = self.tk, self.px
        bar = tk.Frame(self.root, bg=BG, height=px(32))
        bar.pack(side='top', fill='x')
        bar.pack_propagate(False)
        self.title_buttons = {}
        for name, command, hover in (('close', self.close, theme.BLOOD),
                                     ('maximize', self.toggle_maximized, theme.SURFACE),
                                     ('minimize', self.minimize, theme.SURFACE)):
            cell = tk.Frame(bar, bg=BG, width=px(46), height=px(32))
            cell.pack(side='right')
            cell.pack_propagate(False)
            glyph = tk.Label(cell, text=self.TITLE_ICONS[name], bg=BG, fg=theme.TEXT_SOFT, font=(theme.ICONS, 9))
            glyph.pack(fill='both', expand=True)

            def paint(on, cell=cell, glyph=glyph, hover=hover, name=name):
                cell.configure(bg=hover if on else BG)
                glyph.configure(bg=hover if on else BG,
                                fg=theme.TEXT_STRONG if on and name == 'close' else theme.TEXT if on else theme.TEXT_SOFT)
            for widget in (cell, glyph):
                widget.bind('<Enter>', lambda _e, paint=paint: paint(True))
                widget.bind('<Leave>', lambda _e, paint=paint: paint(False))
                widget.bind('<ButtonRelease-1>', lambda e, command=command, widget=widget, cell=cell, glyph=glyph:
                            command() if widget.winfo_containing(e.x_root, e.y_root) in (cell, glyph) else None)
            self.title_buttons[name] = glyph
        # The window's own changes only: bound to the root, <Configure> comes for every element in it too.
        self.root.bind('<Configure>', lambda e: self.title_state() if e.widget is self.root else None, add='+')
        return bar

    def toggle_maximized(self):
        self.root.state('normal' if self.root.state() == 'zoomed' else 'zoomed')

    def title_state(self):
        """The maximize button's glyph follows the window's state; Windows learns where the bar is."""
        if not getattr(self, 'title_buttons', None):
            return
        zoomed = self.root.state() == 'zoomed'
        glyph = self.TITLE_ICONS['restore' if zoomed else 'maximize']
        if self.title_buttons['maximize'].cget('text') != glyph:
            self.title_buttons['maximize'].configure(text=glyph)
        if self.custom_frame:  # where Windows finds the caption and the buttons (physical pixels)
            self.custom_frame.bar = self.titlebar.winfo_height()
            self.custom_frame.buttons = sum(cell.master.winfo_width() for cell in self.title_buttons.values())
            self.custom_frame.edge, self.custom_frame.corner = self.px(5), self.px(10)

    def titlebar_height(self):
        """The height the launcher's title bar takes at the top (0 with Windows' caption)."""
        return self.titlebar.winfo_reqheight() if self.titlebar and self.custom_frame is not False else 0

    def window_chrome(self):
        """The title bar in the launcher's colours: dark, and on Windows 11 acrylic (a hint of
        what lies behind it); the whole window very slightly translucent when chosen (Launcher customization,
        Advanced). Off by default: a dark window lets bright windows behind it show through clearly."""
        translucent = bool(self.app.get('translucent_window', False))
        try:
            hwnd = int(self.root.wm_frame(), 16)
            if hwnd == self.root.winfo_id() and getattr(self, 'chrome_waits', 0) < 100:
                # Tk has not wrapped the window yet (not mapped): its caption is not there to take off.
                self.chrome_waits = getattr(self, 'chrome_waits', 0) + 1
                self.root.after(50, self.window_chrome)
                return
            dwm = ctypes.windll.dwmapi

            def attribute(number, value):
                v = ctypes.c_int(value)
                return dwm.DwmSetWindowAttribute(hwnd, number, ctypes.byref(v), ctypes.sizeof(v))
            if attribute(20, 1):  # DWMWA_USE_IMMERSIVE_DARK_MODE; 19 before Windows 10 20H1
                attribute(19, 1)
            attribute(38, 3 if translucent else 2)  # DWMWA_SYSTEMBACKDROP_TYPE: acrylic or mica (Windows 11)
            attribute(34, 0x00141210)  # DWMWA_BORDER_COLOR (Windows 11): the side bar's dark, not the accent
            if self.titlebar is not None and getattr(self, 'custom_frame', None) is None:
                frame = bbport_frame.Frame(hwnd, [self.root.winfo_id(), self.titlebar.winfo_id()])
                self.custom_frame = frame if frame.install() else False
                if self.custom_frame:
                    frame.cover_on_minimize = True
                if not self.custom_frame:
                    self.titlebar.pack_forget()
                    guide = getattr(self, 'guide', None)
                    if guide is not None and not getattr(guide, 'closed', True):
                        guide.frame.place_configure(y=0, height=0)  # the guide over the whole window again
                self.title_state()
            ctypes.windll.user32.SetWindowPos(hwnd, 0, 0, 0, 0, 0, 0x1 | 0x2 | 0x4 | 0x20)  # redraw the frame
        except (AttributeError, OSError, ValueError, self.tk.TclError):
            pass
        try:
            self.root.attributes('-alpha', 0.985 if translucent else 1.0)
        except self.tk.TclError:
            pass

    def appearance_check(self, parent):
        """The switch for the see-through window (on two pages, one setting)."""
        self.check(parent, 'translucent_window', 'app', _('Slightly translucent window'), _('A hint of what lies behind the launcher: a blurred title bar (Windows 11) and the window at 98.5 %. Off: fully opaque, easiest to read.'))
        if not getattr(self, 'appearance_traced', False):
            self.appearance_traced = True
            self.var('translucent_window', 'app').trace_add('write', lambda *_a: (self.app.__setitem__(
                'translucent_window', self.var('translucent_window', 'app').get()), self.window_chrome()))

    def nav_paint(self, name, hover=False):
        """A side bar entry: a card behind the current page, a softer one under the mouse."""
        c, items = self.nav[name]
        on = name == self.current_page
        pill = self.nav_pills['on'] if on else self.nav_pills['hover'] if hover else ''
        c.itemconfigure(items['pill'], image=pill, state='normal' if pill else 'hidden')
        c.itemconfigure(items['bar'], state='normal' if on else 'hidden')
        c.itemconfigure(items['icon'], fill=GOLD if on else theme.TEXT_SOFT if hover else MUTED)
        c.itemconfigure(items['label'], fill=theme.TEXT_STRONG if on else TEXT if hover else theme.TEXT_SOFT)

    def wheel(self, event):
        """Scrolls the page in pixels (touchpads send small steps), once per redraw however many
        wheel events came in meanwhile. Over the side bar's entries while they scroll: those."""
        if self.nav_scrolls and str(event.widget).startswith(str(self.nav_list)):
            self.nav_scroll(int(-event.delta / 120 * self.px(40)))
            return
        page = self.pages.get(self.current_page)
        if not hasattr(page, 'scroll') or isinstance(event.widget, self.tk.Text) or 'popdown' in str(event.widget):
            return
        self.wheel_pixels = getattr(self, 'wheel_pixels', 0.0) - event.delta / 120 * self.px(64)
        if not getattr(self, 'wheel_pending', None):
            self.wheel_pending = self.root.after_idle(self.flush_wheel)

    def flush_wheel(self):
        self.wheel_pending = None
        pixels = int(self.wheel_pixels)
        self.wheel_pixels -= pixels
        page = self.pages.get(self.current_page)
        if pixels and hasattr(page, 'scroll'):
            page.scroll(pixels)

    def combobox_wheel(self, event):
        """The wheel over a closed drop-down scrolls the page instead of changing its value
        (while a Controls slot listens, the wheel goes on to be bound)."""
        if getattr(self, 'remap_listening', False):
            return None
        (self.guide.wheel if getattr(self, 'guide', None) else self.wheel)(event)
        return 'break'

    def cover(self, widget):
        """A still picture of what `widget` shows now, laid over it (None when there is none to take).
        Tk on Windows maps every element of a page on its own, about 1 ms each: built under the
        picture, a page appears at once when the picture goes (uncover) instead of piece by piece."""
        if not widget.winfo_viewable() or widget.winfo_width() < 2:
            return None
        image = picture_of(widget)
        if image is None:
            return None
        from PIL import ImageTk
        label = self.tk.Label(self.root, bd=0, highlightthickness=0, background=PANEL)
        label.photo = ImageTk.PhotoImage(image, master=self.root)
        label.configure(image=label.photo)
        label.place(in_=widget, x=0, y=0, relwidth=1, relheight=1)
        label.lift()
        self.root.update()  # the picture is on the screen before anything changes under it
        return label

    def uncover(self, label):
        """The picture goes once what was built under it is laid out and drawn."""
        if label is not None:
            self.built_under_cover()
            label.destroy()

    def built_under_cover(self):
        """Lays out, maps and draws what changed under a cover, before the cover goes."""
        self.root.update_idletasks()  # the page is mapped (a canvas that is not ignores the call below)
        page = self.pages.get(getattr(self, 'current_page', None))
        if hasattr(page, 'redisplay'):
            page.redisplay()
        self.root.update()

    def minimize(self):
        """To the task bar with a picture of the window over it: back from there, the window shows
        that at once while Tk maps its elements under it (else it would build up piece by piece).
        The task bar button and Win+Down lay the picture from the window procedure (bbport_frame)."""
        if self.custom_frame:
            self.custom_frame.take_cover()
        elif getattr(self, 'restore_cover', None) is None:
            self.restore_cover = self.cover(self.root)
        self.root.iconify()

    def setting(self, key):
        """A launcher setting as shown now (its switch), before collect() saves it to self.app."""
        return self.vars[key].get() if key in self.vars else self.app.get(key)

    def minimized(self, event):
        """Minimized, with "Minimize to the system tray" on: off the task bar, an icon in the tray."""
        if event.widget is self.root and self.setting('minimize_to_tray'):
            self.root.after(1, self.to_tray)

    def to_tray(self):
        if self.root.state() != 'iconic':
            return
        try:
            if self.tray is None:
                self.tray = bbport_tray.Tray(PORT_DIR / 'launcher' / 'bloodborne.ico', 'Bloodborne',
                                             self.bring_back, self.quit_from_tray, (_('Open'), _('Quit')),
                                             root=self.root)
            if self.tray.show():
                self.root.withdraw()
        except OSError:
            pass  # no tray: it stays on the task bar

    def bring_back(self):
        """The launcher in front again: from the tray, the task bar or hidden."""
        self.__dict__.pop('close_after_steam', None)  # opened again while Add to Steam finished: it stays open
        if self.tray:
            self.tray.hide()
        self.root.deiconify()
        self.root.state('normal')
        self.root.lift()
        self.root.focus_force()

    def quit_from_tray(self):
        self.bring_back()
        self.close()

    def drop_tray(self):
        if self.tray:
            self.tray.close()
            self.tray = None

    def restored(self, event):
        """Back from the task bar: the picture goes once the window is built under it."""
        if event.widget is self.root:
            self.root.after(1, self.uncover_window)

    def uncover_window(self):
        label, self.restore_cover = getattr(self, 'restore_cover', None), None
        frame = self.custom_frame if self.custom_frame and self.custom_frame.cover else None
        if label is not None or frame is not None:
            self.built_under_cover()
            if label is not None:
                label.destroy()
            if frame is not None:
                frame.drop_cover()

    def show(self, name):
        if getattr(self, 'showing', False):
            self.page_wanted = name  # clicked while the page before is still being built: next
            return
        self.showing, self.page_wanted = True, None
        try:
            # Only a page shown for the first time maps its elements one by one: the others stay
            # mapped in the stack (show_page) and come up at once.
            first = not getattr(self.pages.get(name), 'stacked', False)
            cover = self.cover(self.content) if first and name != getattr(self, 'current_page', None) else None
            self.show_page(name)
            self.uncover(cover)
        finally:
            self.showing = False
        if self.page_wanted and self.page_wanted != name:
            self.show(self.page_wanted)
            return
        if name == 'online':
            self.check_online_routes()
        if name == 'controls':  # SDL starts after the page is there (it takes a moment)
            self.root.after(60, lambda: self.pad_start() if self.current_page == 'controls' else None)

    def show_page(self, name):
        if name != 'controls':
            self.pad_stop()
        if name in EXPERT_PAGES and name != 'log' and self.app.get('ui_mode') != 'expert' and \
                getattr(self, 'expert_var', None) is not None:
            self.expert_var.set(True)  # the page and its place in the side bar, not a page out of nowhere
            self.toggle_mode()
            self.status.configure(text=_('The expert view is on now, to show this. The switch is at the bottom of '
                                         'the side bar.'), fg=GOLD)
        # The pages lie on top of each other and the shown one is raised: unpacking one and packing
        # the next made Tk unmap and map every element of both, about 40 ms a change (16 ms now).
        page = self.pages[name]
        if not getattr(page, 'stacked', False):
            page.place(x=0, y=0, relwidth=1, relheight=1)
            page.stacked = True
        page.tkraise()
        self.current_page = name
        for n in self.nav:
            self.nav_paint(n)
        self.nav_scroll(reveal=name)  # its entry in view while the side bar's list scrolls
        if name == 'mods':
            self.refresh_lists(only_if_changed=True)
        elif name == 'graphics':
            self.refresh_fsr4()
            self.refresh_fsr411()
        elif name == 'play':
            self.refresh_status()
            self.show_rating_check()
        elif name == 'picture':
            self.update_rating()
            if getattr(self, 'profile_view', None):
                self.profile_view.redraw()
        elif name == 'trophies':
            self.render_trophies()
        elif name == 'saves':
            self.refresh_saves(only_if_changed=True)
        elif name == 'system':
            self.update_rating()

    def build_play(self):
        tk, ttk, px = self.tk, self.ttk, self.px
        page = tk.Frame(self.content, bg=PANEL)
        self.pages['play'] = page
        self.banner = tk.Canvas(page, height=px(200 if self.compact else 286), bg=PANEL, highlightthickness=0, bd=0)
        self.banner.pack(fill='x')
        self.banner.bind('<Configure>', lambda e: self.schedule_banner()
                         if (e.width, e.height) != getattr(self, 'banner_size', None) else None)
        self.banner.bind('<Button-1>', lambda e: self.banner_spot(e) and self.show('game'))
        self.banner.bind('<Motion>', lambda e: self.banner.configure(cursor='hand2' if self.banner_spot(e) else ''))
        body = ttk.Frame(page, padding=(px(28), 0, px(28), px(22)))
        body.pack(fill='both', expand=True)
        body.columnconfigure(0, weight=1, uniform='cards')
        body.columnconfigure(1, weight=1, uniform='cards')
        body.rowconfigure(0, weight=1)

        info = ttk.Frame(body, style='Card.TFrame', padding=(px(22), px(18)))
        info.grid(row=0, column=0, sticky='nsew', padx=(0, px(10)))
        ttk.Label(info, text=_('Ready check', 'Проверка'), style='Card.Section.TLabel').pack(anchor='w', pady=(0, px(8)))
        # One line per thing that matters; a line that is not fine leads to its fix with a click.
        self.checks, self.check_lines, self.check_targets = {}, {}, {}
        for key in ('news', 'system', 'gpu', 'runtime', 'settings', 'game', 'files', 'saves', 'online', 'firewall',
                    'pool', 'cheats', 'fsr4'):
            line = ttk.Frame(info, style='Inner.TFrame')
            line.pack(fill='x', pady=px(4))
            icon = tk.Label(line, text='', bg=CARD, fg=MUTED, font=(theme.ICONS, 11))
            icon.pack(side='left', anchor='n', pady=(px(3), 0))
            text = ttk.Label(line, text='', style='Card.TLabel', justify='left', wraplength=px(330))
            text.pack(side='left', fill='x', padx=(px(10), 0))
            go = tk.Label(line, text='›', bg=CARD, fg=GOLD, font=(theme.BODY_SEMI, 12))
            self.checks[key] = (icon, text)
            self.check_lines[key] = (line, go)
            for widget in (line, icon, text, go):
                widget.bind('<Button-1>', lambda _e, k=key: self.follow_check(k))
        info.bind('<Configure>', lambda e: [text.configure(wraplength=max(px(160), e.width - px(96)))
                                            for _icon, text in self.checks.values()])
        self.rating_views.append(self.show_rating_check)

        quick = ttk.Frame(body, style='Card.TFrame', padding=(px(22), px(18)))
        quick.grid(row=0, column=1, sticky='nsew', padx=(px(10), 0))
        quick.columnconfigure(0, weight=1)
        ttk.Label(quick, text=_('Your goal'), style='Card.Section.TLabel').grid(row=0, column=0, sticky='w',
                                                                               pady=(0, px(8)))
        goal = self.goal_switch(quick)
        goal.grid(row=1, column=0, sticky='w')
        self.index_entry(goal, _('Your goal'), ', '.join(self.goal_name(g) for g in ('smooth', 'balanced', 'quiet')),
                         'goal')
        self.goal_note = tk.Label(quick, text='', bg=CARD, fg=theme.TEXT_SOFT, font=(theme.BODY, 9), justify='left',
                                  anchor='w')
        self.goal_note.grid(row=2, column=0, sticky='we', pady=(px(8), 0))
        self.goal_notes = getattr(self, 'goal_notes', []) + [self.goal_note]
        quick.bind('<Configure>', lambda e: self.goal_note.configure(wraplength=max(px(160), e.width - px(50))))
        self.expect_label = tk.Label(quick, text='', bg=CARD, fg=MUTED, font=(theme.BODY_MEDIUM, 9), cursor='hand2')
        self.expect_label.grid(row=3, column=0, sticky='w', pady=(px(6), 0))
        self.expect_label.bind('<Button-1>', lambda _e: self.show('picture'))
        tk.Frame(quick, bg=LINE, height=1).grid(row=4, column=0, sticky='we', pady=(px(14), px(10)))
        toggles = ttk.Frame(quick, style='Inner.TFrame')
        toggles.grid(row=5, column=0, sticky='we')
        device = ttk.Frame(toggles, style='Inner.TFrame')
        device.pack(fill='x', pady=(0, px(10)))
        label = ttk.Label(device, text=_('You play with'), style='Card.TLabel')
        label.pack(side='left')
        switch = self.device_switch(device)
        switch.pack(side='left', padx=(px(12), 0))

        def fit_device(_event=None):
            # A narrow card (a small window, a longer language) shows the switch without its label: beside it, the
            # card's edge cut "Keyboard and mouse" off, and under it the card had no room left for Play online.
            # Its two choices say what it is.
            wide = device.winfo_width() >= label.winfo_reqwidth() + switch.winfo_reqwidth() + px(16)
            if wide == getattr(device, 'wide', None):
                return
            device.wide = wide
            label.pack_forget()
            switch.pack_forget()
            if wide:
                label.pack(side='left')
            switch.pack(side='left', padx=(px(12), 0) if wide else 0)
        device.bind('<Configure>', fit_device, add='+')
        self.index_entry(switch, _('You play with'), '{}, {}'.format(_('Controller', 'Геймпад'), _(
            'Keyboard and mouse', 'Клавиатура и мышь')), 'play_with')
        ttk.Checkbutton(toggles, text=_('Fullscreen', 'Полный экран'), variable=self.var('fullscreen', 'app'),
                        style='Card.TCheckbutton').pack(anchor='w')
        online = ttk.Frame(toggles, style='Inner.TFrame')
        online.pack(fill='x', pady=(px(6), 0))
        ttk.Checkbutton(online, text=_('Play online'), variable=self.var('online', 'app'),
                        style='Card.TCheckbutton').pack(side='left')
        self.online_note = tk.Label(online, text='', bg=CARD, fg=MUTED, font=(theme.BODY, 9), cursor='hand2')
        self.online_note.pack(side='left', padx=(px(12), 0))
        self.online_note.bind('<Button-1>', lambda _e: self.show('online'))
        self.hunters_note = tk.Label(online, text='', bg=CARD, fg=theme.TEXT_SOFT, font=(theme.BODY, 9), cursor='hand2')
        self.hunters_note.pack(side='right')
        self.hunters_note.bind('<Button-1>', lambda _e: webbrowser.open(HUNTERS_DREAM_SERVER))
        for key in ('fps_mode', 'upscaler', 'output_res'):
            self.var(key, 'app' if key == 'fps_mode' else 'ini').trace_add('write', lambda *_a: self.refresh_status())
        self.vars['output_res'].trace_add('write', lambda *_a: self.show_rating_check())
        self.var('online', 'app').trace_add('write', lambda *_a: self.refresh_status())
        self.show_rating_check()

    GOAL_TEXTS = {'smooth': ('Smooth', 'As many frames as the game allows, up to 120 FPS; the picture made for your PC.'),
                  'balanced': ('Balanced', 'A steady 60 FPS with the best picture your graphics card has room for.'),
                  'quiet': ('Quiet', '60 FPS and a little less detail: the graphics card works less, stays cooler '
                                     'and quieter (laptops).')}

    def goal_switch(self, parent, bg=None):
        """Three buttons in a row: smooth, balanced, quiet (the player's goal)."""
        tk, px = self.tk, self.px
        bg = bg or CARD
        holder = tk.Frame(parent, bg=LINE, padx=1, pady=1)
        var = self.var('goal', 'app')
        for value in ('smooth', 'balanced', 'quiet'):
            tk.Radiobutton(holder, text=self.goal_name(value), variable=var, value=value, indicatoron=False,
                           command=lambda v=value: self.set_goal(v), bg=theme.SURFACE, fg=theme.TEXT_SOFT,
                           selectcolor=theme.BLOOD, activebackground=theme.SURFACE, activeforeground=theme.TEXT_STRONG,
                           relief='flat', offrelief='flat', bd=0, highlightthickness=0, cursor='hand2',
                           font=(theme.BODY_MEDIUM, 10), padx=px(14), pady=px(5)).pack(side='left', padx=(0, 1))
        var.trace_add('write', lambda *_a: self.show_goal())
        return holder

    def goal_name(self, goal):
        """A goal's button text. Its key has ' (goal)' after the English, because "Balanced" alone
        also names an FSR/DLSS preset, which is not translated."""
        english = self.GOAL_TEXTS.get(goal, self.GOAL_TEXTS['smooth'])[0]
        text = _(f'{english} (goal)')
        return english if text == f'{english} (goal)' else text

    def show_goal(self):
        goal = self.var('goal', 'app').get()
        for note in getattr(self, 'goal_notes', []):
            note.configure(text=_(self.GOAL_TEXTS.get(goal, self.GOAL_TEXTS['smooth'])[1]))

    def set_goal(self, goal):
        """The player chose a goal: the settings for this PC and that goal, at once (Undo on
        Picture & performance); an upscaler chosen by hand for this card stays (goal_entries)."""
        self.var('goal', 'app').set(goal)
        chosen = self.recommended()
        if chosen:
            self.apply_profile(self.goal_entries(chosen[2]))
        self.show_goal()
        self.refresh_status()

    def follow_check(self, key):
        target = self.check_targets.get(key)
        if target:
            target()

    def schedule_banner(self):
        if getattr(self, 'banner_pending', None):
            self.root.after_cancel(self.banner_pending)
        self.banner_pending = self.root.after(30, self.draw_banner)

    def draw_banner(self):
        """The hero: Yharnam at night, the emblem, the title and which dump is selected."""
        self.banner_pending = None
        c, px = self.banner, self.px
        self.banner_size = (c.winfo_width(), c.winfo_height())
        w, h = max(c.winfo_width(), 400), int(c['height'])
        c.delete('all')
        x, y = px(40), int(h * 0.27)
        info = game_info(self.var('game_dir', 'app').get())
        if info:
            first = (_('CUSA03173 · game version {}', 'CUSA03173 · версия игры {}').format(info[1]), GOLD, theme.GOLD, ())
        else:
            first = (renamed(_('Choose your game folder (Game & effects)', 'Выберите папку игры («Игра и эффекты»)')),
                     theme.TEXT_STRONG, theme.BLOOD_LIGHT, ('choose',))
        # The chips are drawn into the art itself: one opaque image (see bbport_theme).
        from tkinter import font as tkfont
        font = (theme.BODY_MEDIUM, 9)
        measure = tkfont.Font(root=self.root, font=font).measure
        chips, left, chip_h, chip_y = [], x, px(28), y + px(92)
        for text, fg, line, tags in (first, (f'v{VERSION_TEXT}', theme.TEXT_SOFT, theme.BORDER_STRONG, ())):
            chip_w = measure(text) + px(26)
            chips.append((left, chip_w, text, fg, line, tags))
            left += chip_w + px(8)

        def art():
            image = theme.hero((w, h), PANEL)
            for cx, cw, _t, _f, line, _g in chips:
                pill = theme.rounded((cw, chip_h), chip_h // 2, (8, 9, 10, 190), line, self.dpi)
                image.alpha_composite(pill, (cx, chip_y - chip_h // 2))
            return image
        key = ('hero', w, h) + tuple((cx, cw, line) for cx, cw, _t, _f, line, _g in chips)
        self.banner_image = self.theme.cached(key, art)
        c.create_image(0, 0, image=self.banner_image, anchor='nw')
        c.create_text(x, y, text=_('native port · Windows', 'нативный порт · Windows').upper(), anchor='w',
                      fill=GOLD, font=(theme.HEAD, 10))
        c.create_text(x - px(2), y + px(42), text='Bloodborne', anchor='w', fill=theme.TEXT_STRONG,
                      font=(theme.HEAD_BOLD, 40))
        self.banner_spots = []
        for cx, cw, text, fg, _line, tags in chips:
            c.create_text(cx + px(13), chip_y, text=text, anchor='w', fill=fg, font=font, tags=tags)
            if tags:
                self.banner_spots.append((cx, chip_y - chip_h // 2, cx + cw, chip_y + chip_h // 2))

    def banner_spot(self, event):
        """The chip under the mouse that opens Game & effects (no game folder yet)."""
        return any(x0 <= event.x <= x1 and y0 <= event.y <= y1 for x0, y0, x1, y1 in getattr(self, 'banner_spots', []))

    def build_graphics(self):
        """Expert: every graphics setting on one page (fullscreen, HDR and the taste effects are on
        Picture & performance)."""
        ttk = self.ttk
        f = self.scrolled_page('graphics', _('All graphics'),
                               _('Every graphics and frame rate setting. Stored in bbport.ini; the in-game menu '
                                 '(Insert or L3+R3) changes the same values.'))
        self.version_warning(f)
        self.link_row(f, _('Fullscreen, HDR and the taste effects are on Picture & performance.'),
                      'picture')
        self.section(f, _('Upscaling', 'Апскейлинг'), top=4)
        self.row(f, _('Upscaler', 'Апскейлер'), self.choice(f, 'upscaler', 'ini', UPSCALERS),
                 _("Temporal upscaling with the game's own motion vectors. FSR 4 needs its files (below) and "
                   'a GPU with INT8 dot products; otherwise the game falls back to FSR 3.1 by itself.',
                   'Временной апскейлинг с векторами движения игры. FSR 4 нужны ассеты (ниже) и GPU с INT8; '
                   'иначе игра сама переключится на FSR 3.1.'))
        before = set(f.grid_slaves())
        self.row(f, '', self.fsr411_choice(f))
        fsr411_rows = list(set(f.grid_slaves()) - before)
        self.row(f, _('Quality preset', 'Пресет'), self.choice(f, 'preset', 'ini', PRESETS),
                 _('Render scale per axis: Quality renders at 1/1.5 of the output size.',
                   'Масштаб рендера по каждой оси: Quality рисует в 1/1.5 размера вывода.'))
        self.row(f, _('Output resolution', 'Разрешение вывода'), self.output_choice(f),
                 _('What the upscaler produces; the HUD is drawn at this size too.',
                   'Что выдаёт апскейлер; интерфейс рисуется в этом же размере.'))
        self.row(f, _('HUD position (ultrawide)', 'Положение интерфейса (21:9)'),
                 self.choice(f, 'hud_position', 'ini', HUD_POSITIONS),
                 _('21:9 outputs: health, items and Blood Echoes at the screen edges, or in the centred '
                   '16:9 area as on a 16:9 screen.',
                   'Для 21:9: здоровье, предметы и Отголоски крови у краёв экрана или в центральной '
                   'области 16:9, как на обычном экране.'))
        # Sharp UI: Awaken's idea from the Lantern Engine (patches.py ui_viewport, UiComposition).
        self.check(f, 'sharp_ui', 'ini', _('Sharp UI', 'Чёткий интерфейс'),
                   _('Menus, HUD and text drawn at the output resolution instead of 1080p stretched up: much '
                     'sharper at 1440p and 4K. From the next start, with live resolution changes off; not on '
                     'ultrawide screens yet. An idea from Awaken\'s Lantern Engine.',
                     'Меню, интерфейс и текст рисуются в разрешении вывода, а не растягиваются из 1080p: '
                     'намного чётче в 1440p и 4K. Со следующего запуска, при выключенной смене разрешения '
                     'на лету; пока не на сверхшироких экранах. Идея из Lantern Engine от Awaken.'))
        self.row(f, _('Live resolution changes', 'Смена разрешения на лету'), self.choice(f, 'live_resolution', 'ini', LIVE),
                 _('Off: outputs other than 1080p are set by a patch at start (fastest; changing them in the '
                   'game restarts it). On: change output and preset in the game without a restart, at a cost.',
                   'Выкл.: разрешения кроме 1080p задаются патчем при запуске (быстрее). Вкл.: менять в игре '
                   'без перезапуска, но медленнее.'))
        self.check(f, 'sharpen', 'ini', _('Sharpening (RCAS)', 'Резкость (RCAS)'),
                   _('Off with FSR 4: it sharpens by itself, and sharpening on top of it brings flaws along edges.'))
        holder = ttk.Frame(f)
        ttk.Scale(holder, from_=0.0, to=2.0, variable=self.var('sharpness', 'ini'), length=300).pack(side='left')
        value = ttk.Label(holder, width=5)
        value.pack(side='left', padx=10)
        show = lambda *_a: value.configure(text=f'{self.vars["sharpness"].get():.2f}')
        self.vars['sharpness'].trace_add('write', show)
        show()
        self.row(f, _('Sharpness', 'Сила резкости'), holder)
        motion = self.check(f, 'object_motion', 'ini', _('Object motion vectors', 'Векторы движения объектов'),
                            _('Less ghosting on characters, cloth and weapons; costs about 5–10% FPS.',
                              'Меньше гостинга на персонажах и одежде; стоит около 5–10% FPS.'))
        # Off on some cards whatever the box says (vk_object_motion.cpp): greyed out, with the reason.
        reason = self.wrap_within(self.ttk.Label(f, style='Muted.TLabel', justify='left', anchor='w'), 640)
        reason.grid(row=self.next_row(f), column=0, columnspan=2, sticky='we', padx=(26, 0))
        cache = self.motion_cache_check(f)

        def refresh_motion():
            if not motion.winfo_exists():
                return
            why = self.motion_unavailable()
            motion.state(['disabled' if why else '!disabled'])
            reason.configure(text=why)
            reason.grid() if why else reason.grid_remove()
            cache.refresh()
        self.motion_refreshers.append(refresh_motion)
        refresh_motion()
        self.section(f, _('Effects', 'Эффекты'))
        for key, title, _on in EFFECTS:
            if key not in TASTE_EFFECTS:
                self.check(f, key, 'ini', _(*title))
        self.row(f, _('Model detail (LOD)', 'Детализация моделей'), self.choice(f, 'model_lod', 'ini', LODS),
                 _('A game patch (game version 1.09). "Highest" can break armor and capes: parts go missing or '
                   'look wrong.',
                   'Патч игры (версия 1.09). «Максимальная» может ломать броню и плащи: части пропадают или '
                   'выглядят неправильно.'))
        self.note(f, _('Effects are game patches for version 1.09, applied at start.'), top=6)
        self.section(f, _('Frame rate', 'Частота кадров'))
        self.row(f, _('Frame rate', 'Режим'), self.choice(f, 'fps_mode', 'app', FPS_MODES),
                 _("Community patches for game version 1.09. Unlocked makes the game use the real frame time. "
                   'Other game versions always run at 30 FPS (the patches would corrupt them).',
                   'Патчи сообщества для версии 1.09. «Без ограничения» — игра использует реальное время кадра. '
                   'Другие версии всегда работают в 30 FPS.'))
        self.row(f, _('Frame cap (unlocked mode)', 'Ограничение FPS (режим без ограничения)'),
                 self.choice(f, 'frame_cap', 'app', FRAME_CAPS, 46),
                 _("Above about 120 FPS the game's movement timing breaks (running and rolling get slower, "
                   'physics and animations can glitch): higher caps are at your own risk.',
                   'Выше ~120 FPS ломается тайминг движения игры (бег и перекаты замедляются, возможны '
                   'сбои физики и анимаций): более высокие значения — на ваш риск.'))
        self.row(f, _('Frames ahead of the GPU', 'Кадров впереди GPU'), self.choice(f, 'frames_ahead', 'app', FRAMES_AHEAD),
                 _('1 keeps frame pacing even; more can raise FPS when the graphics card is the limit.',
                   '1 — ровная подача кадров; больше может поднять FPS, если упирается в видеокарту.'))
        self.row(f, _('Presentation', 'Режим показа кадров'), self.choice(f, 'present_mode', 'app', PRESENT_MODES))
        self.row(f, _('Low latency (Reflex / Anti-Lag 2)', 'Низкая задержка (Reflex / Anti-Lag 2)'),
                 self.choice(f, 'low_latency', 'app', LOW_LATENCY),
                 _('Less input lag: the game starts each frame just in time instead of queuing frames for the '
                   'display. NVIDIA Reflex on GeForce, AMD Anti-Lag 2 on Radeon (current drivers); other GPUs ignore '
                   'it. Boost keeps the GPU clocks up.',
                   'Меньше задержка ввода: игра начинает кадр вовремя, а не копит кадры в очереди. NVIDIA Reflex на '
                   'GeForce, AMD Anti-Lag 2 на Radeon (свежие драйверы); на других GPU не действует. Boost держит '
                   'частоты GPU высокими.'))
        early = self.row(f, _('Send frames to the GPU in parts', 'Отправлять кадр в GPU частями'),
                 self.choice(f, 'early_submit', 'app', EARLY_SUBMIT),
                 _('The graphics card starts drawing a frame while the rest of it is still being prepared, '
                   'so it is finished about 2 ms sooner. Uses a little more CPU. New: turn it off if you see '
                   'flickering or missing objects.',
                   'Видеокарта начинает рисовать кадр, пока остальное ещё готовится, и заканчивает примерно на '
                   '2 мс раньше. Немного больше нагрузка на CPU. Новое: выключите, если видите мерцание или '
                   'пропавшие объекты.'))
        # Off on Radeon cards whatever the box says (vk_rasterizer.cpp MaybeSubmitEarly): greyed out, with why.
        early_reason = self.wrap_within(self.ttk.Label(f, style='Muted.TLabel', justify='left', anchor='w'), 560)
        early_reason.grid(row=self.next_row(f), column=1, sticky='we', pady=(0, 2))

        def refresh_early():
            if not early.winfo_exists():
                return
            why = early_submit_unavailable(self.detected_vendor(), self.app.get('extra_env', ''))
            early.state(['disabled' if why else '!disabled'])
            early_reason.configure(text=why)
            early_reason.grid() if why else early_reason.grid_remove()
        self.motion_refreshers.append(refresh_early)
        refresh_early()
        self.row(f, _('Variable refresh rate (VRR)', 'Переменная частота (VRR)'),
                 self.choice(f, 'vrr', 'app', VRR_MODES),
                 _('For a G-Sync or FreeSync display: the display follows the frames of the game, without tearing '
                   'and without the lag of V-Sync. Uses V-Sync (Presentation is ignored) and keeps the frame rate a '
                   'little below the refresh rate (117 FPS at 120 Hz, at most 120). Turn on G-Sync/FreeSync in '
                   'the NVIDIA/AMD control panel; fullscreen works best.',
                   'Для мониторов G-Sync/FreeSync: монитор следует за кадрами игры, без разрывов и без задержки '
                   'V-Sync. Использует V-Sync (режим показа игнорируется) и держит FPS чуть ниже частоты (117 '
                   'при 120 Гц, максимум 120). Включите G-Sync/FreeSync в панели NVIDIA/AMD; лучше в полноэкранном '
                   'режиме.'))
        self.row(f, _('New shaders while playing', 'Новые шейдеры во время игры'),
                 self.choice(f, 'async_pipelines', 'app', ASYNC_PIPELINES),
                 _('In the background: a place seen for the first time does not stutter while its shaders are '
                   'compiled; things drawn with them appear a moment later instead. Each is compiled once and kept.',
                   'В фоне: новое место не подтормаживает при компиляции шейдеров, зато объекты с ними появляются '
                   'чуть позже. Каждый шейдер компилируется один раз и сохраняется.'))
        self.readouts_section(f)
        self.section(f, _('Frame generation', 'Генерация кадров'))
        self.row(f, _('Frame generation', 'Генерация кадров'),
                 self.fg401_choice(f),
                 _('Frames made between two of the game\'s: 60 FPS on screen becomes 120. Needs a display with '
                   'that refresh rate (120 Hz or more at 60 FPS); it pauses when the display cannot show the extra '
                   'frames. Adds a little input latency. Menus and movies are not generated. DLSS needs a GeForce '
                   'RTX 40 or newer, otherwise FSR 3.1 is used.',
                   'Кадры между двумя кадрами игры: 60 FPS на экране становятся 120. Нужен монитор с такой '
                   'частотой (120 Гц и выше при 60 FPS); иначе генерация приостанавливается. Немного добавляет '
                   'задержку ввода. Меню и ролики не генерируются. DLSS нужна GeForce RTX 40 или новее, иначе '
                   'используется FSR 3.1.'))
        self.fg401_status = self.wrap_within(self.ttk.Label(f, justify='left', anchor='w', style='Muted.TLabel'), 640)
        self.fg401_status.grid(row=self.next_row(f), column=1, sticky='we', pady=(4, 0))
        self.fg401_recheck = ttk.Frame(f)
        self.fg401_recheck.grid(row=self.next_row(f), column=1, sticky='w', pady=(6, 0))
        ttk.Button(self.fg401_recheck, text=_('Recheck support'), command=self.recheck_fg401).pack(side='left')
        self.refresh_fg401()
        self.var('hdr', 'app').trace_add('write', lambda *_a: self.refresh_fg401_status())
        self.var('frame_gen', 'ini').trace_add('write', lambda *_a: self.refresh_fg401_status())
        self.row(f, _('Multiplier', 'Множитель'),
                 self.choice(f, 'frame_gen_multiplier', 'ini', FRAME_GEN_MULTIPLIERS),
                 _('FSR 3.1 makes one frame (2×); DLSS on a GeForce RTX 50 up to three (4×).',
                   'FSR 3.1 делает один кадр (2×); DLSS на GeForce RTX 50 — до трёх (4×).'))
        self.check(f, 'frame_gen_fit_display', 'ini', _('Fit to display refresh', 'Под частоту монитора'),
                   _('On: no more frames than the display shows (an uncapped game gets fewer generated frames, and '
                     'the GPU does less). Off: the multiplier as chosen.',
                     'Вкл.: не больше кадров, чем показывает монитор (при снятом ограничении FPS генерируется меньше, '
                     'видеокарта меньше нагружена). Выкл.: множитель как выбран.'))
        self.section(f, _('FSR 4 assets', 'Ассеты FSR 4'))
        self.fsr4_label = self.wrap_within(ttk.Label(f, text='', justify='left', anchor='w', style='Muted.TLabel'), 640)
        self.fsr4_label.grid(row=self.next_row(f), column=0, columnspan=2, sticky='we')
        holder = ttk.Frame(f)
        holder.grid(row=self.next_row(f), column=0, columnspan=2, sticky='w', pady=(8, 0))
        self.fsr4_button = ttk.Button(holder, text=_('Download FSR 4 assets', 'Скачать ассеты FSR 4'),
                                      command=self.download_fsr4)
        self.fsr4_button.pack(side='left')
        self.index_entry(self.fsr4_button, _('Download FSR 4 assets', 'Скачать ассеты FSR 4'), key='button:fsr4')
        self.fsr4_progress = ttk.Progressbar(holder, length=280, maximum=len(fsr4_files()))
        self.fsr4_progress.pack(side='left', padx=12)
        self.note(f, _("From FireBurn/Q2RTX on GitHub (built from AMD's MIT-licensed FidelityFX source), "
                       'about 30 MB, into the fsr4_shaders folder of the port.',
                       'С GitHub FireBurn/Q2RTX (собраны из MIT-исходников AMD FidelityFX), около 30 МБ, '
                       'в папку fsr4_shaders порта.'), top=6)
        before = set(f.grid_slaves())
        self.section(f, _('FSR 4.1.1 (experimental)', 'FSR 4.1.1 (экспериментально)'))
        def muted():
            return self.wrap_within(ttk.Label(f, text='', justify='left', anchor='w', style='Muted.TLabel'), 640)
        self.fsr411_label, self.fsr411_native_label = muted(), muted()
        self.fsr411_label.grid(row=self.next_row(f), column=0, columnspan=2, sticky='we')
        self.fsr411_native_label.grid(row=self.next_row(f), column=0, columnspan=2, sticky='we')
        holder = ttk.Frame(f)
        holder.grid(row=self.next_row(f), column=0, columnspan=2, sticky='w', pady=(8, 0))
        self.fsr411_button = ttk.Button(holder, text=_('Download fsr4vk'),
                                        command=self.download_fsr411)
        self.fsr411_button.pack(side='left')
        self.index_entry(self.fsr411_button, _('Download fsr4vk'), key='button:fsr4vk')
        self.fsr411_progress = ttk.Progressbar(holder, length=280, maximum=100)
        self.fsr411_progress.pack(side='left', padx=12)
        self.ttk.Button(holder, text=_('Recheck support'), command=self.recheck_fsr411).pack(side='left')
        self.note(f, _('Download fsr4vk here. AMD DX12 is included.'), top=6)
        fsr411_asset_rows = list(set(f.grid_slaves()) - before)
        self.index_only(fsr411_asset_rows, _('only with FSR 4.1.1'), 'upscaler')  # found, and the jump marks Upscaler

        def show_fsr411(*_a):
            for widget in fsr411_rows:
                widget.grid()
            for widget in fsr411_asset_rows:
                widget.grid() if self.var('upscaler', 'ini').get() == 'fsr411' else widget.grid_remove()
        self.var('upscaler', 'ini').trace_add('write', show_fsr411)
        show_fsr411()
        self.refresh_fsr411()
        self.vars['fsr411_backend'].trace_add('write', lambda *_a: (self.refresh_fsr411(), self.refresh_status()))

    def link_row(self, parent, text, page, link=None, command=None):
        """A muted line with a link to another page (or to `command`)."""
        row = self.ttk.Frame(parent)
        row.grid(row=self.next_row(parent), column=0, columnspan=2, sticky='we', pady=(4, 0))
        words = self.ttk.Label(row, text=text, style='Muted.TLabel', justify='left')
        words.pack(side='left')
        label = self.tk.Label(row, text=(link or _('Open')) + '  ›', bg=PANEL, fg=GOLD, cursor='hand2',
                              font=(theme.BODY_MEDIUM, 9))
        label.pack(side='left', padx=(8, 0))
        label.bind('<Button-1>', lambda _e: command() if command else self.show(page))
        # The text wraps where the link would no longer fit beside it: a long translation (Russian) cut it off.
        row.bind('<Configure>', lambda e: words.configure(
            wraplength=max(self.px(120), e.width - label.winfo_reqwidth() - 8 - self.px(8))))
        return label

    def build_game(self):
        """Expert: the game folder and the DLC folder (set once, usually by the setup guide)."""
        f = self.scrolled_page('game', _('Game files'),
                               _('Your game files and their add-ons. Set once, usually by the setup guide.'))
        self.section(f, _('Game', 'Игра'), top=4)
        self.folder(f, 'game_dir', _('Game folder', 'Папка игры'),
                    _('Choose the folder with eboot.bin', 'Выберите папку с eboot.bin'),
                    _('Your own game files of CUSA03173 (eboot.bin, sce_module, sce_sys, dvdroot_ps4); '
                      'version 1.09 for the community patches.',
                      'Ваши собственные файлы игры CUSA03173 (eboot.bin, sce_module, sce_sys, dvdroot_ps4); '
                      'версия 1.09 для патчей сообщества.'), on_change=self.game_changed)
        self.folder(f, 'dlc_dir', _('DLC folder'), _('Choose the DLC folder'),
                    _('Your add-ons, one folder each named like SPEXPANSIONDLC03 (The Old Hunters). Only '
                      'needed with the base game alone; the Game of the Year edition (CUSA03173) has it.'))
        dlc = self.wrap_within(self.tk.Label(f, text='', bg=PANEL, font=(theme.BODY, 9), justify='left', anchor='w'),
                               560)
        dlc.grid(row=self.next_row(f), column=1, sticky='we', pady=(4, 2))

        def dlc_shown(*_a):
            kind, text = dlc_status(self.var('game_dir', 'app').get(), self.var('dlc_dir', 'app').get().strip(),
                                    self.var('user_dir', 'app').get() or DATA_DIR / 'user')
            mark = {'ok': '✓', 'warn': '!', 'none': '–'}[kind]
            dlc.configure(text=f'{mark}  {text}',
                          fg={'ok': theme.GOLD_LIGHT, 'warn': theme.AMBER, 'none': MUTED}[kind])
        for key in ('game_dir', 'dlc_dir', 'user_dir'):
            self.var(key, 'app').trace_add('write', lambda *_a: self.root.after_idle(dlc_shown))
        dlc_shown()
        self.dlc_note = dlc
        self.link_row(f, _('Is the game folder all right? Help checks it.'), 'help')
        self.link_row(f, _('Set everything up again, step by step.'), None, _('Repeat the setup'), self.open_setup)
        self.piracy_notice(f, PANEL, wrap=720).grid(row=self.next_row(f), column=0, columnspan=2, sticky='we',
                                                    pady=(14, 0))

    def build_language(self):
        """Launcher customization (the page key stays 'language'): the launcher's language and look,
        what it does when the game starts, the shortcuts. The game's own settings are on Gameplay."""
        ttk = self.ttk
        f = self.scrolled_page('language', _('Launcher customization'),
                               _("The launcher's language and look, what it does while you play, and shortcuts to "
                                 'the game.'))
        self.section(f, _('Launcher'), top=4)
        before = self.var('ui_language', 'app').get()
        language = ttk.Frame(f)
        self.choice(language, 'ui_language', 'app', UI_LANGUAGES, 24).pack(side='left')
        restart = ttk.Button(language, text=_('Apply (restarts the launcher)'), command=self.relaunch)
        self.var('ui_language', 'app').trace_add('write', lambda *_a: restart.pack(side='left', padx=(8, 0))
                                                 if self.var('ui_language', 'app').get() != before else
                                                 restart.pack_forget())
        self.row(f, _('Launcher language', 'Язык лаунчера'), language,
                 _('Applies when the launcher opens again.', 'Применится при следующем открытии лаунчера.'))
        self.appearance_check(f)  # Advanced > Appearance has the title bar (expert)
        self.section(f, _('While you play'))
        self.row(f, _('When the game starts'), self.choice(f, 'on_play', 'app', ON_PLAY, 24),
                 _('Closed, it ends with the game; minimized, it waits on the task bar (or in the tray).'))
        self.check(f, 'reopen_after_game', 'app', _('Open the launcher again when the game closes'),
                   _('After a crash it always comes back, with the log.'))
        if DISCORD_APPLICATION_ID:  # without the port's Discord application there is nothing to show
            self.check(f, 'discord_status', 'app', _("Show what I'm playing on Discord"),
                       _('Your area, the boss you fight and your level on your Discord profile, while the Discord '
                         'app runs.'))
        self.check(f, 'minimize_to_tray', 'app', _('Minimize to the system tray'),
                   _('Minimized, the launcher leaves the task bar for an icon next to the clock: click it to '
                     'open the launcher again.'))
        self.section(f, _('Shortcuts'))
        shortcuts = ttk.Frame(f)
        button = ttk.Button(shortcuts, text=_('Desktop shortcut', 'Ярлык на рабочем столе'), command=self.shortcut)
        button.pack(side='left')
        self.index_entry(button, _('Desktop shortcut', 'Ярлык на рабочем столе'), key='button:shortcut')
        button = ttk.Button(shortcuts, text=_('Add to Steam'), command=self.add_to_steam)
        button.pack(side='left', padx=(6, 0))
        self.index_entry(button, _('Add to Steam'), key='button:steam')
        self.row(f, '', shortcuts,
                 _('The desktop shortcut opens this launcher; the Steam entry starts the game straight away '
                   'with these settings.'), key='button:shortcut')

    def build_gameplay(self):
        """The game's own settings for everyone (the simple view too): its text and voices, how it starts
        and sounds, and the gameplay tweaks (game patches, not cheats)."""
        f = self.scrolled_page('gameplay', _('Gameplay'),
                               _("The game's language and sound, and changes to how it plays and its menus."))
        self.section(f, _('Language'), top=4)
        self.row(f, _('Text language', 'Язык текста'), self.choice(f, 'language', 'app', LANGUAGES))
        self.row(f, _('Voice language', 'Язык озвучки'), self.choice(f, 'voice_language', 'app', VOICE_LANGUAGES),
                 _('As in the game uses the original voice setting; other choices use available tracks.'))
        self.row(f, _('Player name', 'Имя игрока'), self.ttk.Entry(f, textvariable=self.var('player_name', 'app'), width=30),
                 _('Where the game shows the PSN name; empty: the default.', 'Где игра показывает имя PSN; пусто — по умолчанию.'))
        self.section(f, _('Starting the game and sound'))
        self.check(f, 'skip_intro', 'ini', _(*dict((k, t) for k, t, _o in EXTRAS)['skip_intro']))
        self.check(f, 'mute_unfocused', 'ini', _('Mute when the game is in the background',
                                                 'Без звука, когда игра в фоне'))
        self.section(f, _('Gameplay tweaks', 'Изменения игрового процесса'))
        self.note(f, _('Game patches for version 1.09, applied at start.'), top=0)
        self.version_warning(f)
        for key, title, _on in TWEAKS:
            self.check(f, key, 'ini', _(*title))

    def build_cheats(self):
        f = self.scrolled_page('cheats', _('Cheats', 'Читы'),
                               _('Game patches for version 1.09, applied at start. Leave them off for a normal '
                                 'play-through.', 'Патчи игры для версии 1.09, применяются при запуске. Для обычного '
                                 'прохождения оставьте их выключенными.'))
        self.version_warning(f)
        self.section(f, _('Cheats', 'Читы'), top=4)
        for key, title, _on in CHEATS:
            self.check(f, key, 'ini', _(*title))
        self.section(f, _('Debug'))
        for key, title, _on in EXTRAS:
            if key != 'skip_intro':
                self.check(f, key, 'ini', _(*title))
        # Enemy control and the free camera share their buttons: one at a time.
        control, camera = self.var('cheat_enemy_control', 'ini'), self.var('debug_camera', 'ini')
        control.trace_add('write', lambda *_a: control.get() and camera.set(False))
        camera.trace_add('write', lambda *_a: camera.get() and control.set(False))
        for key, _title, _on in CHEATS:
            self.var(key, 'ini').trace_add('write', lambda *_a: self.refresh_status())
        self.section(f, _('Respec', 'Перераспределение'))
        holder = self.ttk.Frame(f)
        self.ttk.Button(holder, text=_('Respec a character…', 'Перераспределить характеристики…'),
                        command=self.open_respec).pack(side='left')
        self.index_entry(holder, _('Respec a character…', 'Перераспределить характеристики…'), key='button:respec')
        self.row(f, '', holder,
                 _("Spread a character's attribute points again: same level, same Blood Echoes. Changes the save "
                   'while the game is closed; the saves from before go into your backups (Saves).',
                   'Заново распределить очки характеристик персонажа: тот же уровень и Отголоски крови. Меняет '
                   'сохранение, пока игра закрыта; прежние сохранения попадают в ваши резервные копии (Сохранения).'))

    def open_respec(self):
        """The respec dialog (bbport_respec): a character, its six attributes, Apply."""
        tk, ttk = self.tk, self.ttk
        title = _('Respec', 'Перераспределение')
        if self.game_running():
            self.messagebox.showinfo(title, _('Close the game first: it would save over the change.',
                                              'Сначала закройте игру: она запишет сохранение поверх.'))
            return
        user = Path(self.var('user_dir', 'app').get() or DATA_DIR / 'user')
        found = [(folder, slot, who) for folder in respec.save_folders(user)
                 for slot, who in respec.characters(folder)]
        if not found:
            self.messagebox.showinfo(title, _('No characters found in {}.', 'Персонажи не найдены в {}.').format(user))
            return
        win = tk.Toplevel(self.root)
        win.title(title)
        win.configure(bg=PANEL)
        win.transient(self.root)
        win.resizable(False, False)
        body = ttk.Frame(win, padding=16)
        body.pack(fill='both', expand=True)
        labels = [_('Slot {}: {} (level {})', 'Ячейка {}: {} (уровень {})').format(slot + 1, who['name'] or '—', who['level'])
                  for _folder, slot, who in found]
        chosen = tk.StringVar(value=labels[0])
        ttk.Combobox(body, textvariable=chosen, values=labels, state='readonly', width=36).grid(
            row=0, column=0, columnspan=2, sticky='w', pady=(0, 10))
        # the attributes as the game names them in its text language (as on the Saves page)
        words = character.names(self.var('game_dir', 'app').get(), self.var('language', 'app').get())
        values = {a: tk.IntVar() for a in respec.ATTRIBUTES}
        for row, a in enumerate(respec.ATTRIBUTES, start=1):
            ttk.Label(body, text=words.attribute(a)).grid(row=row, column=0, sticky='w', padx=(0, 16), pady=2)
            ttk.Spinbox(body, from_=respec.FLOORS[a], to=respec.MAX, textvariable=values[a], width=6).grid(
                row=row, column=1, sticky='w', pady=2)
        left = ttk.Label(body, style='Muted.TLabel')
        left.grid(row=8, column=0, columnspan=2, sticky='w', pady=(10, 0))
        buttons = ttk.Frame(body)
        buttons.grid(row=9, column=0, columnspan=2, sticky='e', pady=(12, 0))

        def current():
            return found[labels.index(chosen.get())]

        def load(*_a):
            for a in respec.ATTRIBUTES:
                values[a].set(current()[2]['attributes'][a])

        def points(*_a):
            try:
                spent = sum(values[a].get() for a in respec.ATTRIBUTES)
            except tk.TclError:
                left.configure(text='')
                return
            remaining = sum(current()[2]['attributes'].values()) - spent
            left.configure(text=_('Points to place: {}', 'Осталось распределить: {}').format(remaining) if remaining >= 0
                           else _('{} points too many', 'Лишних очков: {}').format(-remaining))
            apply.state(['!disabled'] if remaining == 0 else ['disabled'])

        def save():
            folder, slot, who = current()
            try:
                new = {a: values[a].get() for a in respec.ATTRIBUTES}
                respec.check(who['attributes'], new)
                # The saves from before in the backups (Saves: restorable there) when the backups cover
                # this folder; else respec keeps a copy beside them.
                game = self.var('game_dir', 'app').get()
                made = saves.backup(user, game, reason='respec', detail=who.get('name', '')) \
                    if saves.target_dir(user, game).resolve() == Path(folder).resolve() else None
                backup = respec.respec(folder, slot, new, copy=made is None)
            except (ValueError, OSError, tk.TclError) as error:
                self.messagebox.showerror(title, str(error), parent=win)
                return
            who['attributes'] = new
            self.refresh_saves()  # the new backup in the list
            self.messagebox.showinfo(title, _('Done. A copy of the saves from before is in {}.',
                                              'Готово. Копия прежних сохранений: {}.').format(made or backup.parent),
                                     parent=win)
            win.destroy()

        ttk.Button(buttons, text=_('Reset', 'Сбросить'), command=load).pack(side='left', padx=(0, 8))
        apply = ttk.Button(buttons, text=_('Apply', 'Применить'), command=save)
        apply.pack(side='left')
        chosen.trace_add('write', load)
        for var in values.values():
            var.trace_add('write', points)
        load()
        points()
        win.grab_set()

    def build_online(self):
        ttk = self.ttk
        f = self.scrolled_page('online', _('Online play'),
                               _('Messages, bloodstains and ghosts come from the game server; bells and summons '
                                 'go through a shadNet server.'))
        self.check(f, 'online', 'app', _('Play online'),
                   _('Off: the game stays offline, as before.'))
        self.var('online', 'app').trace_add('write', lambda *_a: self.refresh_status())
        self.section(f, _('Co-op pool'))
        pool = self.var('online_pool', 'app')
        pool.trace_add('write', lambda *_a: (self.refresh_status(), self.root.after_idle(self.ask_hunters)))
        # SuperMedo Server first (the default), The Hunter's Dream (the previous server) for the time being. An own
        # server only for Seamless co-op's own start (settled_pool): other servers confused players about where
        # they played. Each keeps its own account (pool_account): switching back and forth signs none out.
        pools = ttk.Frame(f)
        ttk.Radiobutton(pools, text=_('SuperMedo Server (recommended)') if HUNTERS_DREAM_POOL else
                        _('SuperMedo Server'), variable=pool, value='supermedo').pack(side='left', padx=(0, 16))
        if HUNTERS_DREAM_POOL:
            ttk.Radiobutton(pools, text=_("The Hunter's Dream (previous server)"), variable=pool,
                            value='huntersdream').pack(side='left', padx=(0, 16))
        elif pool.get() == 'huntersdream':
            pool.set('supermedo')
        bbsc = started_by_bbsc()
        if bbsc:
            ttk.Radiobutton(pools, text=_("Own server (Seamless co-op, a friend's)"), variable=pool,
                            value='shadnet').pack(side='left')
        elif pool.get() == 'shadnet':
            pool.set('supermedo')
        pools.setting_key = 'online_pool'  # the search's words (bbport_search.WORDS)
        self.row(f, _('Play with'), pools, ' '.join(
            [_("SuperMedo Server (recommended): supermedo.com's own servers, with a free account made below.")] +
            ([_("The Hunter's Dream: the previous server, still here for now; connections there often drop.")]
             if HUNTERS_DREAM_POOL else []) +
            ([_("Own server: Seamless co-op's or a friend's private server, with its address and account.")]
             if bbsc else [])))
        self.hunters_line = ttk.Label(f, text='', style='Muted.TLabel')
        self.hunters_line.grid(row=self.next_row(f), column=1, sticky='w', pady=(2, 0))
        # Each pool shows only its own settings (show_pool below).
        groups = {}
        before = set(f.grid_slaves())

        def group(name):
            nonlocal before
            now = set(f.grid_slaves())
            groups[name] = groups.get(name, []) + list(now - before)
            before = now

        self.section(f, _("The Hunter's Dream account"))
        # The previous server, for the time being: its players lose the connection often, so the page sends
        # them on to SuperMedo Server.
        self.note(f, _("The Hunter's Dream is the previous server and stays only for the time being. Connections "
                       'there often drop, so SuperMedo Server is recommended. Your sign-ins on both stay when you '
                       'switch.'), top=0)
        self.row(f, '', ttk.Button(f, text=_('Switch to SuperMedo Server'), style='Accent.TButton',
                                   command=lambda: (pool.set('supermedo'), self.collect())))
        account = ttk.Label(f, text='', justify='left')

        thd_id, thd_token = self.var('thd_online_id', 'app'), self.var('thd_token', 'app')

        def show_accounts():
            name = thd_id.get() if thd_token.get() else ''
            account.configure(text=_('Signed in as {}').format(name) if name else _('Not signed in'))
        show_accounts()
        thd_token.trace_add('write', lambda *_a: show_accounts())  # also forgotten after a refusal
        self.row(f, _('Account'), account, key='thd_online_id')
        signing = self.wrap_within(ttk.Label(f, text='', style='Muted.TLabel', justify='left', anchor='w'), 560)
        # The code to type on the website, large, with a button that copies it.
        code_box = ttk.Frame(f)
        code_label = ttk.Label(code_box, text='', style='Code.TLabel')
        code_label.pack(side='left')

        def copy_code():
            self.root.clipboard_clear()
            self.root.clipboard_append(code_label.cget('text'))
        ttk.Button(code_box, text=_('Copy'), command=copy_code).pack(side='left', padx=(14, 0))

        def show_code(code=''):
            code_label.configure(text=code)
            if code:
                code_box.grid()
            else:
                code_box.grid_remove()

        def sign_in_thd():  # its own name: tests/test_launcher_findable.py finds this page's button by it
            signing.configure(text=_('Starting the sign-in…'))
            show_code()

            def on_code(page, code):
                text = _('In the page that opened ({0}), sign in with Discord and enter this code:').format(page)
                self.ui_calls.put(lambda: (signing.configure(text=text), show_code(code)))
                webbrowser.open(page)

            def work():
                try:
                    name, token = thd_sign_in(on_code)
                except Exception as error:  # a broken reply too (BadStatusLine): the line always answers
                    self.ui_calls.put(lambda e=connection_error(error, COMMUNITY_SERVER): (
                        signing.configure(text=_('Sign-in failed: {}').format(e)), show_code()))
                    return
                self.ui_calls.put(lambda: (thd_id.set(name), thd_token.set(token), signing.configure(text=''),
                                           show_code(), show_accounts(), self.collect()))
            threading.Thread(target=work, daemon=True).start()
        button = self.row(f, '', ttk.Button(f, text=_('Sign in with Discord'), command=sign_in_thd))
        if HUNTERS_DREAM_POOL:  # hidden otherwise: the search must not lead to it
            self.index_entry(button, _('Sign in with Discord'), _("The Hunter's Dream account"),
                             key='button:signin_thd')
        signing.grid(row=self.next_row(f), column=1, sticky='we', pady=(6, 0))
        code_box.grid(row=self.next_row(f), column=1, sticky='w', pady=(4, 0))
        self.check(f, 'bridge_live_map', 'app', _('Show me on the live map'),
                   _("thehuntersdream.com/map shows where players are and which bells ring: the game sends "
                     "your position more often while you play."))
        credit = ttk.Label(f, text=_("The connection to The Hunter's Dream is Awaken's port of bbhost's network "
                                     "code; bbhost and The Hunter's Dream are by droogie."),
                           style='Muted.TLabel', cursor='hand2')
        credit.grid(row=self.next_row(f), column=0, columnspan=2, sticky='w', pady=(8, 0))
        credit.bind('<Button-1>', lambda _e: webbrowser.open('https://github.com/Awakenchan/BloodBorneButPc'))
        group('huntersdream')
        self.section(f, _('Account'))
        group('account')
        self.row(f, _('Online ID (NPID)'), ttk.Entry(f, textvariable=self.var('online_npid', 'app'), width=40),
                 _('Your account name on that server, not your email.'))
        self.row(f, _('Password'), ttk.Entry(f, textvariable=self.var('online_password', 'app'), width=40, show='•'),
                 _('Saved on this PC with the other launcher settings.'))
        # The own server's addresses below always count on its pool (online_custom_servers on): the public
        # shadNet server, the only other one, turned the port away.
        custom = self.var('online_custom_servers', 'app')
        group('shadnet')
        # SuperMedo Server: the account in use (Sign out empties it), and a new account made on the
        # server from here with its own boxes; once made, it is the account in use. Its own settings,
        # not the shadNet boxes (a co-op tool writes those).
        npid_var, password_var = self.var('supermedo_npid', 'app'), self.var('supermedo_pass', 'app')
        self.section(f, _('SuperMedo account'))
        signed = ttk.Label(f, text='', justify='left')
        buttons = ttk.Frame(f)

        def show_signed(*_args):
            name = pool_account({'online_pool': 'supermedo', 'supermedo_npid': npid_var.get(),
                                 'supermedo_pass': password_var.get()})
            signed.configure(text=_('Signed in as {}').format(name) if name
                             else _('Not signed in: sign in with Discord, or create an account below.'))
            # Sign out only with an account to sign out of: without one the button did nothing.
            sign_out_button.state(['!disabled' if name else 'disabled'])
        npid_var.trace_add('write', show_signed)
        password_var.trace_add('write', show_signed)

        def sign_out():
            npid_var.set('')
            password_var.set('')
            self.collect()
        discord_line = self.wrap_within(ttk.Label(f, text='', style='Muted.TLabel', justify='left', anchor='w'), 560)

        def sign_in_discord():
            discord_line.configure(text=_('Starting the sign-in…'))

            def on_page(page):
                webbrowser.open(page)
                self.ui_calls.put(lambda: discord_line.configure(
                    text=_('Sign in with Discord in the page that opened, then come back here.')))

            def work():
                try:
                    npid, password = discord_sign_in(COMMUNITY_SERVER, on_page)
                except Exception as error:  # a broken reply too (BadStatusLine): the line always answers
                    self.ui_calls.put(lambda e=connection_error(error, COMMUNITY_SERVER): discord_line.configure(
                        text=_('Sign-in failed: {}').format(e)))
                    return
                self.ui_calls.put(lambda: (npid_var.set(npid), password_var.set(password),
                                           discord_line.configure(text=''), self.collect()))
            threading.Thread(target=work, daemon=True).start()
        self.row(f, _('Account'), signed, key='supermedo_npid')
        button = ttk.Button(buttons, text=_('Sign in with Discord'), command=sign_in_discord)
        button.pack(side='left')
        hint = _('Discord makes your SuperMedo account the first time, named after your Discord name; nothing to '
                 'remember. Sign in again on any PC.')
        self.index_entry(button, _('Sign in with Discord'), hint, key='button:signin_supermedo')
        sign_out_button = ttk.Button(buttons, text=_('Sign out'), command=sign_out)
        sign_out_button.pack(side='left', padx=(14, 0))
        self.index_entry(sign_out_button, _('Sign out'), _('SuperMedo account'), key='button:signout')
        show_signed()
        self.row(f, '', buttons, hint)
        discord_line.grid(row=self.next_row(f), column=1, sticky='we', pady=(6, 0))
        self.section(f, _('New SuperMedo account'))
        sign_up = self.wrap_within(ttk.Label(f, text='', style='Muted.TLabel', justify='left', anchor='w'), 560)
        new_npid, new_password, email = (self.tk.StringVar(value='') for _n in range(3))

        def create_account():
            # All three read here: a Tk variable read in the worker thread raised there (no account, and
            # "Creating the account…" stayed).
            npid, password, mail = new_npid.get().strip(), new_password.get(), email.get().strip()
            if not npid or not password or '@' not in mail:
                sign_up.configure(text=_('Enter a new Online ID, a password and your email address.'))
                return
            sign_up.configure(text=_('Creating the account…'))

            def work():
                try:
                    problem = shadnet_register(SUPERMEDO_SERVER, npid, password, mail)
                except OSError as error:
                    problem = _('SuperMedo Server could not be reached: {}').format(error_text(error))
                except Exception as error:
                    # An answer that is not shadNet's (struct.error): never "Creating the account…" for good.
                    problem = _('The account could not be created: {}').format(error_text(error))
                if problem:
                    self.ui_calls.put(lambda: sign_up.configure(text=problem))
                    return
                text = _('Account {} created and signed in. Press Play to go online.').format(npid)
                self.ui_calls.put(lambda: (npid_var.set(npid), password_var.set(password),
                                           sign_up.configure(text=text), self.collect()))
            threading.Thread(target=work, daemon=True).start()
        self.row(f, _('New Online ID'), ttk.Entry(f, textvariable=new_npid, width=40),
                 _('1 to 16 letters, numbers, _ or -. Other players see this name.'), key='supermedo_npid')
        self.row(f, _('New password'), ttk.Entry(f, textvariable=new_password, width=40, show='•'),
                 key='supermedo_npid')
        self.row(f, _('Email address'), ttk.Entry(f, textvariable=email, width=40),
                 _('Only to make the account.'), key='supermedo_npid')
        button = self.row(f, '', ttk.Button(f, text=_('Create account'), command=create_account))
        self.index_entry(button, _('Create account'), _('New SuperMedo account'), key='button:create_account')
        sign_up.grid(row=self.next_row(f), column=1, sticky='we', pady=(6, 0))
        group('supermedo')
        self.section(f, _('Game server (messages, bloodstains, ghosts)'))
        community = self.var('online_community', 'app')
        self.row(f, _('Game server'), ttk.Entry(f, textvariable=community, width=40),
                 _('game.supermedo.com by default. Only the name is needed; the game picks the ports.'))
        self.section(f, _('Co-op server (bells and summons)'))
        server = self.var('online_server', 'app')
        self.row(f, _('Server address'), ttk.Entry(f, textvariable=server, width=40),
                 _('host:port from the person who runs the server (Seamless co-op: its own address).'))
        self.row(f, _('WebAPI address'), ttk.Entry(f, textvariable=self.var('online_webapi', 'app'), width=40),
                 _('Of the shadNet server, not the game server. Leave empty: the server address with '
                   'port 31315 is used.'))
        group('servers')
        self.section(f, _('Connection'))
        self.check(f, 'online_upnp', 'app', _('UPnP (open a port on the router automatically)'),
                   _('Turn it off on Tailscale or another VPN.'))
        group('upnp')
        result = ttk.Label(f, text='', style='Muted.TLabel', justify='left')

        def check():
            values = {key: self.var(key, 'app').get()
                      for key in ('online_server', 'online_webapi', 'online_community')}
            online_on = self.var('online', 'app').get()
            pool_name = pool.get()
            own = custom.get()
            address, webapi = (SUPERMEDO_SERVER, SUPERMEDO_WEBAPI) if pool_name == 'supermedo' else \
                shadnet_servers({**values, 'online_custom_servers': own})
            game = game_server({**values, 'online_pool': pool_name, 'online_custom_servers': own})
            supermedo_name = npid_var.get().strip() if password_var.get() else ''
            hunters_dream = pool_name == 'huntersdream'
            signed_in = thd_id.get() if thd_token.get() else ''
            result.configure(text=_('Checking…'))

            def work():
                try:
                    if hunters_dream:
                        text = server_check_hunters_dream()
                        text += '\n' + (_('Signed in as {}').format(signed_in) if signed_in else
                                        _('Not signed in: use "Sign in with Discord" above.'))
                    else:
                        text = server_check(address, webapi, game)
                    if pool_name == 'supermedo':
                        text += '\n' + (_('Signed in as {}').format(supermedo_name) if supermedo_name else
                                        _('Not signed in: sign in with Discord, or create an account below.'))
                    if pool_name == 'shadnet' and own and values['online_webapi'].strip() \
                            and values['online_webapi'].strip() != webapi:
                        text += '\n' + _('The WebAPI box holds the game server address; it is ignored.')
                    if not online_on:
                        text += '\n' + _('"Play online" is off: the game starts offline.')
                except Exception as error:  # the line never stays at "Checking…" (R85); only a bug gets here
                    text = _('The check stopped: {}').format(error_text(error, kind=True))
                self.ui_calls.put(lambda: result.configure(text=text))
            threading.Thread(target=work, daemon=True).start()
        button = self.row(f, '', ttk.Button(f, text=_('Test connection'), command=check))
        self.index_entry(button, _('Test connection'), _('Connection'), key='button:test_connection')
        result.grid(row=self.next_row(f), column=1, sticky='w', pady=(6, 0))
        self.note(f, _('Everyone in a session needs the same game version (1.09) and the same server. '
                       'Cheats change the game for the other players too: turn them off when you play together.'))
        show_code()  # hidden until a sign-in shows one; its row stays reserved (rows are counted above)
        shown = {'shadnet': {'account', 'shadnet', 'servers', 'upnp'},
                 'huntersdream': {'huntersdream', 'upnp'},
                 'supermedo': {'supermedo', 'upnp'}}

        def show_pool(*_args):
            if pool.get() == 'huntersdream' and not HUNTERS_DREAM_POOL:
                pool.set('supermedo')  # runs show_pool again
                return
            want = set(shown.get(pool.get(), shown['shadnet']))
            if pool.get() == 'shadnet' and not custom.get():
                custom.set(True)  # an own server: its addresses count
                if server.get().strip() == APP_DEFAULTS['online_server']:
                    server.set('')  # shadPS4's public one, which turns the port away: to be filled in
            for name, widgets in groups.items():
                for widget in widgets:
                    if widget is not code_box:
                        widget.grid() if name in want else widget.grid_remove()
            if 'huntersdream' in want:
                show_code(code_label.cget('text'))
            else:
                code_box.grid_remove()
        pool.trace_add('write', show_pool)
        custom.trace_add('write', show_pool)
        show_pool()
        # The other pools' rows stay findable ("sign in" while The Hunter's Dream is chosen): the search says
        # which pool they need and marks "Play with". An own server's only while it can be chosen.
        only = {'supermedo': _('only with SuperMedo Server'),
                'huntersdream': _("only with The Hunter's Dream") if HUNTERS_DREAM_POOL else None,
                'shadnet': _('only with an own server') if bbsc else None}
        for name, widgets in groups.items():
            choices = [p for p, wanted in shown.items() if name in wanted]  # (pools is the "Play with" frame)
            if len(choices) == 1 and only[choices[0]]:
                self.index_only(widgets, only[choices[0]], 'online_pool')

    def build_mods(self):
        f = self.scrolled_page('mods', _('Mods & patches', 'Моды и патчи'),
                               _('The game files are never changed: mods are layered over them at start.',
                                 'Файлы игры не меняются: моды накладываются при запуске.'))
        self.section(f, _('Mods', 'Моды'), top=4)
        self.check(f, 'mods_enabled', 'app', _('Load mods', 'Загружать моды'),
                   _('Loose-file mods, each in its own folder (with dvdroot_ps4, or chr\\, parts\\ … directly).',
                     'Моды из файлов, каждый в своей папке (с dvdroot_ps4 или chr\\, parts\\ … напрямую).'))
        self.folder(f, 'mods_dir', _('Mods folder', 'Папка модов'), _('Choose the mods folder', 'Выберите папку модов'),
                    _('Empty: {}', 'Пусто: {}').format(DATA_DIR / 'mods'), on_change=self.refresh_lists)
        self.mods_frame = self.ttk.Frame(f)
        self.mods_frame.grid(row=self.next_row(f), column=0, columnspan=2, sticky='we', pady=(8, 0))
        self.section(f, _('Third-party patches', 'Сторонние патчи'))
        self.folder(f, 'patches_dir', _('Patches folder', 'Папка патчей'), _('Choose the patches folder', 'Выберите папку патчей'),
                    _('shadPS4/GoldHEN XML patch files for version 1.09. Empty: {}',
                      'XML-патчи shadPS4/GoldHEN для версии 1.09. Пусто: {}').format(DATA_DIR / 'patches'),
                    on_change=self.refresh_lists)
        self.patches_frame = self.ttk.Frame(f)
        self.patches_frame.grid(row=self.next_row(f), column=0, columnspan=2, sticky='we', pady=(8, 0))
        self.ttk.Button(f, text=_('Refresh', 'Обновить'), command=self.refresh_lists).grid(
            row=self.next_row(f), column=0, sticky='w', pady=(14, 0))

    def build_trophies(self):
        f = self.scrolled_page('trophies', _('Achievements', 'Достижения'),
                               _('Trophies the game unlocks while you play; updated live, kept with your saves.',
                                 'Трофеи, которые игра открывает во время игры; обновляются сразу, хранятся с '
                                 'сохранениями.'))
        self.trophy_filter, self.trophy_state = 'all', None
        # A new PC or Windows: the trophies of the old installation (a player merged two trophies.log by hand).
        holder = self.ttk.Frame(f)
        holder.grid(row=self.next_row(f), column=0, columnspan=2, sticky='we', pady=(4, 8))
        button = self.ttk.Button(holder, text=_('Bring achievements from another installation'),
                                 command=self.import_trophies)
        button.pack(side='left')
        hint = _('From an older port folder, the drive of your old PC or shadPS4: added to yours, none removed.')
        self.trophy_import_note = self.ttk.Label(holder, text=hint, style='Muted.TLabel', justify='left', anchor='w')
        self.wrap_within(self.trophy_import_note, 460).pack(side='left', fill='x', expand=True, padx=10)
        self.trophy_import_note.hint = hint
        self.index_entry(button, _('Bring achievements from another installation'), hint, key='button:trophies')
        self.trophy_body = self.ttk.Frame(f)
        self.trophy_body.grid(row=self.next_row(f), column=0, columnspan=2, sticky='we')

    def trophy_icon(self, parent, grade, earned, size, bg, trophy_id=None):
        """Game trophy artwork, or a grade cup for totals and missing artwork."""
        if trophy_id in trophies.BY_ID:
            game_dir = self.var('game_dir', 'app').get() or '.'
            path = Path(game_dir) / 'sce_sys' / 'trophy' / 'trophy00.trp'
            if not hasattr(self, 'trophy_art') or self.trophy_art.path != path:
                self.trophy_art = TrophyIcons(game_dir)
                self.trophy_images = {}
                self.trophy_art_signature = None
            data = self.trophy_art.get(trophy_id)
            if self.trophy_art_signature != self.trophy_art.signature:
                self.trophy_images.clear()
                self.trophy_art_signature = self.trophy_art.signature
            if data:
                try:
                    from io import BytesIO
                    from PIL import Image, ImageEnhance, ImageTk
                    key = (trophy_id, earned, size)
                    if key not in self.trophy_images:
                        with Image.open(BytesIO(data)) as source:
                            image = source.convert('RGBA').resize((size, size), Image.Resampling.LANCZOS)
                        if not earned:
                            image = ImageEnhance.Brightness(image).enhance(0.4)
                        self.trophy_images[key] = ImageTk.PhotoImage(image, master=parent)
                    icon = self.tk.Label(parent, image=self.trophy_images[key], bg=bg, bd=0)
                    icon.image = self.trophy_images[key]
                    return icon
                except ImportError:
                    pass
                except (OSError, ValueError, Image.DecompressionBombError):
                    pass
        c = self.tk.Canvas(parent, width=size, height=size, bg=bg, highlightthickness=0, bd=0)
        color = TROPHY_COLORS.get(grade, MUTED) if earned else TROPHY_LOCKED
        p = lambda *xy: [v * size for v in xy]
        c.create_oval(*p(.02, .02, .98, .98), fill=PANEL, outline=LINE if earned else PANEL)
        handle = max(2, round(size / 18))
        c.create_arc(*p(.24, .27, .42, .47), start=90, extent=180, style='arc', outline=color, width=handle)
        c.create_arc(*p(.58, .27, .76, .47), start=270, extent=180, style='arc', outline=color, width=handle)
        c.create_polygon(*p(.33, .25, .67, .25, .66, .44, .58, .55, .42, .55, .34, .44), fill=color, outline=color)
        c.create_rectangle(*p(.47, .55, .53, .65), fill=color, outline='')
        c.create_rectangle(*p(.39, .65, .61, .70), fill=color, outline='')
        c.create_rectangle(*p(.35, .71, .65, .76), fill=color, outline='')
        return c

    def render_trophies(self):
        """Rebuilds the page when the unlocks or the filter changed."""
        unlocked = dict(self.tracker.unlocked)
        state = (tuple(sorted(unlocked.items())), self.trophy_filter, self.var('game_dir', 'app').get())
        if state == self.trophy_state:
            return
        self.trophy_state = state
        tk, body = self.tk, self.trophy_body
        for widget in body.winfo_children():
            widget.destroy()
        known = trophies.TROPHIES
        earned = [t for t in known if t[0] in unlocked]
        percent = len(earned) * 100 // len(known)

        card = self.ttk.Frame(body, style='Card.TFrame', padding=2)
        card.pack(fill='x', pady=(4, 0))
        left = tk.Frame(card, bg=CARD)
        left.pack(side='left', padx=22, pady=16)
        tk.Label(left, text=f'{percent}%', bg=CARD, fg=TEXT, font=(theme.HEAD, 26)).pack(anchor='w')
        width, height = self.px(260), max(3, self.px(4))
        bar = tk.Canvas(left, width=width, height=height, bg=LINE, highlightthickness=0, bd=0)
        bar.pack(anchor='w', pady=(2, 8))
        if earned:
            bar.create_rectangle(0, 0, max(height, width * len(earned) // len(known)), height, fill=GOLD, outline='')
        tk.Label(left, text=_('{} of {} trophies earned', '{} из {} трофеев получено').format(len(earned), len(known)),
                 bg=CARD, fg=MUTED, font=(theme.BODY, 9)).pack(anchor='w')
        grades = tk.Frame(card, bg=CARD)
        grades.pack(side='right', padx=(0, 22))
        for grade in trophies.GRADES:
            have = sum(1 for t in earned if t[3] == grade)
            total = sum(1 for t in known if t[3] == grade)
            cell = tk.Frame(grades, bg=CARD)
            cell.pack(side='left', padx=self.px(9))
            self.trophy_icon(cell, grade, True, self.px(34), CARD).pack()
            tk.Label(cell, text=f'{have} / {total}', bg=CARD, fg=TEXT if have else MUTED,
                     font=(theme.BODY, 9)).pack(pady=(4, 0))

        tabs = tk.Frame(body, bg=PANEL)
        tabs.pack(fill='x', pady=(18, 2))
        for name, title, count in (('all', _('All', 'Все'), len(known)),
                                   ('earned', _('Earned', 'Получены'), len(earned)),
                                   ('locked', _('Locked', 'Не получены'), len(known) - len(earned))):
            on = name == self.trophy_filter
            tab = tk.Frame(tabs, bg=PANEL, cursor='hand2')
            tab.pack(side='left', padx=(0, 24))
            label = tk.Label(tab, text=f'{title}   {count}', bg=PANEL, fg=TEXT if on else MUTED, cursor='hand2',
                             font=(theme.BODY, 10, 'bold' if on else 'normal'))
            label.pack()
            tk.Frame(tab, bg=GOLD if on else PANEL, height=2).pack(fill='x', pady=(4, 0))
            for widget in (tab, label):
                widget.bind('<Button-1>', lambda _e, n=name: self.set_trophy_filter(n))

        keep = {'all': lambda t: True, 'earned': lambda t: t[0] in unlocked,
                'locked': lambda t: t[0] not in unlocked}[self.trophy_filter]
        shown = 0
        for group in (trophies.BASE, trophies.DLC):
            members = [t for t in known if t[4] == group]
            items = [t for t in members if keep(t)]
            if not items:
                continue
            shown += len(items)
            head = tk.Frame(body, bg=PANEL)
            head.pack(fill='x', pady=(16, 8))
            tk.Label(head, text=_(group), bg=PANEL, fg=GOLD, font=(theme.HEAD, 13)).pack(side='left')
            tk.Label(head, text='{} / {}'.format(sum(1 for t in members if t[0] in unlocked), len(members)),
                     bg=PANEL, fg=MUTED, font=(theme.BODY, 9)).pack(side='left', padx=10, pady=(3, 0))
            for trophy in items:
                self.trophy_card(body, trophy, unlocked.get(trophy[0]))
        if not shown:
            tk.Label(body, text=_('No trophies yet. They appear here as soon as the game unlocks them.',
                                  'Трофеев пока нет. Они появятся здесь, как только игра их откроет.')
                     if self.trophy_filter == 'earned' else _('Every trophy is earned. Hail, good hunter.',
                                                              'Все трофеи получены. Славной охоты.'),
                     bg=PANEL, fg=MUTED, font=(theme.BODY, 10)).pack(anchor='w', pady=(22, 0))

    def trophy_card(self, parent, trophy, when):
        tk = self.tk
        _id, name, text, grade, _group = trophy
        earned = when is not None
        card = self.ttk.Frame(parent, style='Card.TFrame', padding=2)
        card.pack(fill='x', pady=(0, 6))
        self.trophy_icon(card, grade, earned, self.px(46), CARD, trophy_id=_id).pack(
            side='left', padx=(12, 14), pady=10)
        side = tk.Frame(card, bg=CARD)
        side.pack(side='right', padx=(12, 18))
        tk.Label(side, text=_(*GRADE_NAMES[grade]), bg=CARD, fg=TROPHY_COLORS[grade] if earned else MUTED,
                 font=(theme.BODY, 9, 'bold')).pack(anchor='e')
        date_format = '%Y年%m月%d日' if LANG in ('zh', 'ja') else '%Y. %m. %d.' if LANG == 'ko' else '%d %b %Y'
        tk.Label(side, text=time.strftime(date_format, time.localtime(when)) if earned else _('Locked', 'Не получен'),
                 bg=CARD, fg=MUTED, font=(theme.BODY, 9)).pack(anchor='e', pady=(2, 0))
        middle = tk.Frame(card, bg=CARD)
        middle.pack(side='left', fill='x', expand=True, pady=10)
        tk.Label(middle, text=_(name), bg=CARD, fg=TEXT if earned else MUTED,
                 font=(theme.BODY, 11, 'bold' if earned else 'normal')).pack(anchor='w')
        self.wrap_within(tk.Label(middle, text=_(text), bg=CARD, fg=MUTED, font=(theme.BODY, 9), justify='left',
                                  anchor='w'), 520).pack(fill='x', pady=(1, 0))

    def set_trophy_filter(self, name):
        self.trophy_filter = name
        self.render_trophies()

    def trophy_dir(self):
        return Path(self.var('user_dir', 'app').get() or DATA_DIR / 'user')

    def import_trophies(self, folder=None):
        """Achievements: the trophies of other installations (another port folder, shadPS4 and its forks, those of
        the Windows before, or under a folder the player picks) added to this port's, after a copy of its
        trophies.log (bbport_trophies.add_unlocks). The search walks folders: in a helper thread."""
        if getattr(self, 'trophy_searching', False) or self.saves_folder_gone():
            return
        self.trophy_searching = True
        self.trophy_import_note.configure(text=_('Looking for achievements on this PC…'))
        target, game = self.trophy_dir() / trophies.LOG_NAME, self.var('game_dir', 'app').get()

        def work():
            try:
                records = (saves.trophy_records_in(folder, [target]) if folder else
                           saves.trophy_records(PORT_DIR, game, [target]))
                found = [(where, path, trophies.missing(trophies.read_unlocks(path), target))
                         for where, path in records]
            except Exception:  # a search must never take the launcher down
                found = []
            self.ui_calls.put(lambda: self.offer_trophies(found, target, folder))
        threading.Thread(target=work, daemon=True).start()

    def saves_folder_gone(self):
        """A saves folder of the player's own that is not there (missing_saves_folder): said, and True. The
        achievements are never added to one made anew in its place."""
        problem = missing_saves_folder({'user_dir': self.var('user_dir', 'app').get()})
        if problem:
            self.messagebox.showerror('Bloodborne', problem)
        return bool(problem)

    def offer_trophies(self, found, target, folder=None):
        """The search's result: the installations with trophies that are not here yet, added on Yes; else a
        folder to look in."""
        self.trophy_searching = False
        self.trophy_import_note.configure(text=self.trophy_import_note.hint)
        have = trophies.read_log(target)
        found = [(where, path, unlocks) for where, path, unlocks in found if set(unlocks) - set(have)]
        if not found:
            text = (_('No achievements in {} that are not here yet.').format(folder) if folder else
                    _('No other installation on this PC has achievements that are not here yet.'))
            if self.messagebox.askyesno('Bloodborne', text + '\n\n' + _(
                    'Choose a folder to look in? An old port folder, a shadPS4 folder or the drive of your old PC.')):
                chosen = self.filedialog.askdirectory(title=_('Choose the folder with the achievements'),
                                                      initialdir=str(Path.home()))
                if chosen:
                    self.import_trophies(Path(chosen))
            return
        lines = '\n'.join('• ' + _('{}: {} new here').format(where, len(set(unlocks) - set(have))) + f'\n   {path}'
                          for where, path, unlocks in found)
        if not self.messagebox.askyesno('Bloodborne', _(
                'Achievements found:\n\n{}\n\nAdd them to yours? Your trophies.log is copied first, and no trophy is '
                'removed.').format(lines)):
            return
        union = {}
        for _where, _path, unlocks in found:
            for trophy, when in unlocks.items():
                union[trophy] = min(when, union.get(trophy, when))
        if self.saves_folder_gone():  # its drive went away during the search
            return
        try:
            if not self.var('user_dir', 'app').get():  # only this port's own user/ before a first start, as run.py
                target.parent.mkdir(parents=True, exist_ok=True)
            added, copy = trophies.add_unlocks(union, target)
        except OSError as error:
            self.messagebox.showerror('Bloodborne', _('Could not add the achievements: {}').format(error))
            return
        if Path(self.tracker.user_dir) == target.parent:
            # earned before, not just now: shown on the page without the "You've earned a trophy" notice
            self.trophies_changed(self.tracker.refresh_log() + self.tracker.award_platinum(), announce=False)
        done = _('Achievements added: {} new.').format(added)
        if copy:
            done += '\n\n' + _('Your previous list is kept as {}.').format(copy)
        self.messagebox.showinfo('Bloodborne', done)

    def poll_trophies(self):
        """Every 2 s: trophies.log (written by the game) and last_run.log (a game started elsewhere)."""
        folder = self.trophy_dir()
        if folder != self.tracker.user_dir:
            self.tracker, self.trophies_loaded = trophies.Tracker(folder), False
        self.trophies_changed(self.tracker.poll(run_log=not self.process), announce=self.trophies_loaded)
        self.trophies_loaded = True
        self.root.after(2000, self.poll_trophies)

    def trophies_changed(self, new, announce=True):
        if self.current_page == 'trophies':
            self.render_trophies()
        if announce:
            for trophy in new:
                self.toasts.append(trophy)
                if len(self.toasts) == 1:
                    self.show_toast()

    def show_toast(self):
        """The console's 'You've earned a trophy' notice, top right, one at a time."""
        if not self.toasts:
            return
        tk = self.tk
        trophy = self.toasts[0]
        _id, name, _text, grade, _group = trophies.BY_ID.get(
            trophy, (trophy, _('Trophy {}', 'Трофей {}').format(trophy), '', trophies.BRONZE, trophies.BASE))
        box = tk.Frame(self.content, bg=CARD, highlightthickness=1, highlightbackground=LINE, cursor='hand2')
        icon = self.trophy_icon(box, grade, True, self.px(38), CARD, trophy_id=_id)
        icon.pack(side='left', padx=(12, 12), pady=10)
        words = tk.Frame(box, bg=CARD)
        words.pack(side='left', padx=(0, 20), pady=8)
        caption = tk.Label(words, text=_("You've earned a trophy.", 'Вы получили трофей.'), bg=CARD, fg=MUTED,
                           font=(theme.BODY, 9))
        caption.pack(anchor='w')
        title = tk.Label(words, text=_(name), bg=CARD, fg=TEXT, font=(theme.BODY, 11, 'bold'))
        title.pack(anchor='w')
        for widget in (box, icon, words, caption, title):
            widget.bind('<Button-1>', lambda _e: self.show('trophies'))
        box.place(relx=1.0, x=-self.px(20), y=self.px(18), anchor='ne')
        box.lift()

        def done():
            box.destroy()
            self.toasts.pop(0)
            self.show_toast()
        self.root.after(5500, done)
    def current_slots(self, output):
        """`output`'s bindings, one per slot (None for an empty one): pending edits, else what
        input.ini has. A file with more than SLOTS keeps the extra ones at the end."""
        if output in self.remap_edits:
            return self.remap_edits[output]
        values = list(self.remap_bindings.get(output) or [])
        return values + [None] * (SLOTS - len(values))

    def build_controls(self):
        ttk, tk = self.ttk, self.tk
        f = self.scrolled_page('controls', _('Controls', 'Управление'),
                               _('Keyboard, mouse and controller: each has its own buttons, and both work '
                                 'side by side.',
                                 'Клавиатура, мышь и геймпад: у каждого свои кнопки, и они работают '
                                 'одновременно.'))
        self.remap_rows = {}  # output -> ([slot buttons], conflict label)
        self.remap_clear_marks = {}  # (output, slot) -> the small × that clears that slot
        self.remap_listening = False
        self.mouse_vars = {}  # mouse_settings key -> its widget's variable (apply_layout_preset)

        self.row(f, _('You play with'), self.device_switch(f),
                 _('Keyboard and mouse turns the mouse camera on and uses the Dark Souls III layout '
                   '(asked first if you changed keys yourself). Controller turns the mouse camera off, so '
                   'the game leaves the pointer alone. Keys and controller buttons keep working either way.'),
                 key='play_with')
        self.row(f, _('Button prompts', 'Подсказки кнопок'), self.choice(f, 'button_prompts', 'ini', PROMPT_MODES),
                 _("The game's button icons: your keyboard and mouse keys while you play with them, "
                   "the controller's (PlayStation, Xbox or Switch) while you use it.",
                   'Значки кнопок в игре: ваши клавиши и мышь, пока вы играете ими, и кнопки '
                   'геймпада (PlayStation, Xbox или Switch), пока вы используете его.'))
        self.row(f, _('In-game menu key'), self.choice(f, 'menu_key', 'app', MENU_KEYS, 16),
                 _('Opens the settings menu during play. No key: the menu only opens with the controller, or '
                   'not at all; the launcher has every setting either way. A key you move the menu off can be '
                   'bound to the game.'))
        self.row(f, _('In-game menu on the controller'), self.choice(f, 'menu_pad', 'app', MENU_PADS, 28),
                 _("Off if pressing both sticks or Select + Start opens it by accident."))
        # Only the settings of the device chosen in "You play with" (no switch of its own: one choice,
        # not two that look alike). A search jump shows the other device's part without changing it.
        played = self.var('play_with', 'app')
        tab = tk.StringVar(value=played.get() or 'keyboard')
        played.trace_add('write', lambda *_a: played.get() in ('controller', 'keyboard') and tab.set(played.get()))
        tabs, before = {}, set(f.grid_slaves())
        indexed = len(getattr(self, 'search_settings', []))

        def tab_rows(name):
            nonlocal before, indexed
            now = set(f.grid_slaves())
            tabs[name] = list(now - before)
            before = now
            for entry in getattr(self, 'search_settings', [])[indexed:]:  # a search jump opens its tab
                entry['reveal'] = lambda n=name, inner=entry.get('reveal'): (tab.set(n), inner and inner())
            indexed = len(getattr(self, 'search_settings', []))

        self.section(f, _('Controller test'))
        self.pad_card = self.controller_card(f)
        self.pad_card.grid(row=self.next_row(f), column=0, columnspan=2, sticky='we', pady=(self.px(4), self.px(6)))
        self.index_entry(self.pad_card, _('Controller test'), _('Press the buttons: what lights up here reaches the '
                                                                  'game.'), 'controller_test')
        self.build_controller_bindings(f)
        tab_rows('controller')

        preset = ttk.Frame(f)
        ttk.Button(preset, text=_('Use the Dark Souls III layout', 'Раскладка Dark Souls III'),
                   command=self.apply_layout_preset).pack(side='left')
        preset.setting_key = 'mouse:layout'
        self.row(f, _('Layout', 'Раскладка'), preset,
                 _('WASD, Space to dodge, left click / Shift + left click to attack, right click / '
                   'Shift + right click (or Ctrl) for the off hand, E interact, R item, F vial, Q lock '
                   'on, Tab or Esc menu, and the 1:1 mouse camera. Replaces the keyboard and mouse '
                   'bindings; the controller keeps its own.',
                   'WASD, пробел — уклонение, левый клик / Shift + левый клик — атака, правый клик / '
                   'Shift + правый клик (или Ctrl) — левая рука, E — действие, R — предмет, F — флакон, '
                   'Q — захват цели, Tab или Esc — меню и камера мыши 1:1. Заменяет привязки клавиатуры и '
                   'мыши; у геймпада остаются свои.'))
        self.build_mouse_look(f)
        self.note(f, _('Click a slot, then press a key, click a mouse button or turn the wheel; '
                       'Esc cancels. Right-click a slot to clear it. Movement and camera '
                       'directions take keys only.',
                       'Щёлкните ячейку и нажмите клавишу, кнопку мыши или прокрутите колесо; '
                       'Esc отменяет. Правый клик по ячейке очищает её. Направления движения и '
                       'камеры принимают только клавиши.'), top=10)
        # One grid for every group, so the slot columns line up down the page.
        table = ttk.Frame(f)
        table.grid(row=self.next_row(f), column=0, columnspan=2, sticky='w')
        for c in range(1, SLOTS + 1):
            table.columnconfigure(c, uniform='slot', minsize=self.px(150))
        for (group, group_ru), actions in ACTION_GROUPS:
            ttk.Label(table, text=_(group, group_ru), style='Section.TLabel').grid(
                row=table.grid_size()[1], column=0, columnspan=SLOTS + 2, sticky='w', pady=(14, 4))
            for output, (label, label_ru) in actions:
                r = table.grid_size()[1]
                ttk.Label(table, text=_(label, label_ru)).grid(row=r, column=0, sticky='w', padx=(0, 18), pady=2)
                buttons = []
                for slot in range(SLOTS):
                    btn = ttk.Button(table, style='Slot.TButton',
                                     command=lambda o=output, i=slot: self.start_remap_capture(o, i))
                    btn.grid(row=r, column=slot + 1, sticky='ew', padx=2, pady=2)
                    btn.bind('<Button-3>', lambda _e, o=output, i=slot: self.clear_remap(o, i))
                    buttons.append(btn)
                    # A small × in the slot's top-right corner clears it, like a right-click.
                    # refresh_remap_row places it over the slot only while the slot has a binding.
                    mark = tk.Label(table, text='×', bg=CARD, fg=MUTED, cursor='hand2',
                                    font=('Segoe UI', 10))
                    mark.bind('<Button-1>', lambda _e, o=output, i=slot: self.clear_remap(o, i))
                    mark.bind('<Enter>', lambda _e, m=mark: m.configure(bg=LINE, fg=TEXT))
                    mark.bind('<Leave>', lambda _e, m=mark: m.configure(bg=CARD, fg=MUTED))
                    # Keep the × the same colour as the slot under it while the slot is hovered.
                    btn.bind('<Enter>', lambda _e, m=mark: m.configure(bg=LINE), add='+')
                    btn.bind('<Leave>', lambda _e, m=mark: m.configure(bg=CARD), add='+')
                    self.remap_clear_marks[(output, slot)] = mark
                warn = ttk.Label(table, style='Muted.TLabel')
                warn.grid(row=r, column=SLOTS + 1, sticky='w', padx=(8, 0))
                self.remap_rows[output] = (buttons, warn)
        self.refresh_remap_row(None)
        self.fold(f, _('Key combinations and reserved keys'), [_('Combinations: hold Shift, Ctrl or Alt while pressing the key or mouse '
                       'button (up to three inputs, such as Shift + left click). A held '
                       'combination wins over the bindings on its parts, so left click and Shift + '
                       'left click can be two actions. ⚠ marks an input another action uses too. A '
                       'key that is both an action of its own and part of a combination is pressed '
                       'on the way into it: with Shift on Circle, Shift + left click starts a '
                       'sprint that ends as a dodge.',
                       'Сочетания: удерживайте Shift, Ctrl или Alt, нажимая клавишу или кнопку мыши '
                       '(до трёх вводов, например Shift + левый клик). Удерживаемое сочетание '
                       'важнее привязок его частей, поэтому левый клик и Shift + левый клик могут '
                       'быть разными действиями. ⚠ отмечает ввод, который использует и другое '
                       'действие. Клавиша, которая сама является действием и частью сочетания, '
                       'нажимается по пути к нему: если Shift — это Круг, Shift + левый клик '
                       'начинает бег, который заканчивается уклонением.'),
                       _('Reserved: Insert/Escape (the port\'s menu), F9 (route recording), and '
                       'whichever key toggles mouse look or reloads input.ini (F7/F8 by '
                       'default).',
                       'Зарезервированы: Insert/Escape (меню порта), F9 (запись маршрута), а '
                       'также клавиши переключения мыши и перезагрузки input.ini (по умолчанию '
                       'F7/F8).')])
        tab_rows('keyboard')

        def show_tab(*_a):
            for name, widgets in tabs.items():
                for widget in widgets:
                    widget.grid() if name == tab.get() else widget.grid_remove()
        tab.trace_add('write', show_tab)
        # The label column as wide as the wider tab needs: switching never moves the rows beside it.
        f.update_idletasks()
        f.columnconfigure(0, minsize=f.grid_bbox(0, 0)[2])
        show_tab()

    def build_mouse_look(self, f):
        """Mouse look: on or off, what the mouse moves, the camera mode and its sliders."""
        ttk, tk = self.ttk, self.tk
        self.section(f, _('Mouse look', 'Обзор мышью'))
        ms = self.mouse_settings

        def mark_mouse_edited(*_a):
            self.mouse_edited = True

        # Built manually (not via self.check) because mouse_settings is its own dict, not
        # self.ini/self.app -- self.var()'s ini/app bookkeeping does not apply to it.
        enabled_var = self.mouse_vars['enabled'] = tk.BooleanVar(value=ms['enabled'])
        enabled_var.trace_add('write', lambda *_a: (ms.__setitem__('enabled', enabled_var.get()), mark_mouse_edited()))
        box = ttk.Checkbutton(f, text=_('Enable mouse look', 'Включить обзор мышью'), variable=enabled_var)
        box.grid(row=self.next_row(f), column=0, columnspan=2, sticky='w', pady=(4, 0))
        self.index_entry(box, _('Enable mouse look', 'Включить обзор мышью'), key='mouse:enabled')
        self.note(f, _('F7 also toggles this in-game, instantly; F8 reloads these settings '
                       'without restarting.', 'F7 также переключает это прямо в игре мгновенно; '
                       'F8 применяет эти настройки без перезапуска.'), top=2)

        stick_var = self.mouse_vars['stick'] = tk.StringVar(value=ms['stick'])
        stick_var.trace_add('write', lambda *_a: (ms.__setitem__('stick', stick_var.get()), mark_mouse_edited()))
        stick_box = ttk.Frame(f)
        ttk.Radiobutton(stick_box, text=_('Camera (right stick)', 'Камера (правый стик)'),
                       variable=stick_var, value='right').pack(side='left')
        ttk.Radiobutton(stick_box, text=_('Movement (left stick)', 'Движение (левый стик)'),
                       variable=stick_var, value='left').pack(side='left', padx=(16, 0))
        self.row(f, _('Mouse controls', 'Мышь управляет'), stick_box)

        def slider_row(key, title, hint, lo, hi, fmt='{:.2f}'):
            holder = ttk.Frame(f)
            var = self.mouse_vars[key] = tk.DoubleVar(value=ms[key])
            ttk.Scale(holder, from_=lo, to=hi, variable=var, length=280).pack(side='left')
            value_label = ttk.Label(holder, width=6)
            value_label.pack(side='left', padx=10)
            show = lambda *_a: value_label.configure(text=fmt.format(var.get()))
            var.trace_add('write', lambda *_a: (ms.__setitem__(key, round(var.get(), 3)), show(), mark_mouse_edited()))
            show()
            self.row(f, title, holder, hint)

        # deadzone_offset ("Smoothness") used to gate a startup floor/ramp in mouse_to_axis;
        # that formula was replaced by an output-value lerp with a fixed time constant
        # (src/runtime_pad.c), which deadzone_offset no longer affects, so the slider for it was
        # removed here rather than leave a control with no real effect. The value is still read/
        # written in input.ini by bbport_input_config.py for files that already set it.
        camera_var = self.mouse_vars['camera'] = tk.StringVar(value=ms['camera'])
        camera_var.trace_add('write', lambda *_a: (ms.__setitem__('camera', camera_var.get()), mark_mouse_edited()))
        camera_box = ttk.Frame(f)
        ttk.Radiobutton(camera_box, text=_('Direct (1:1, like a PC game)', 'Прямая (1:1, как в ПК-играх)'),
                       variable=camera_var, value='direct').pack(side='left')
        ttk.Radiobutton(camera_box, text=_('Stick emulation', 'Эмуляция стика'),
                       variable=camera_var, value='stick').pack(side='left', padx=(16, 0))
        self.row(f, _('Camera', 'Камера'), camera_box,
                 _('Direct turns the game\'s camera by exact angles: no stick acceleration or '
                   'turn-speed cap. While locked on, moving the mouse switches target.',
                   'Прямая поворачивает камеру игры на точные углы: без ускорения стика и '
                   'предела скорости поворота. При захвате цели движение мыши переключает цель.'))
        # Only the sliders of the chosen camera mode: the other mode's do nothing meanwhile.
        modes, before = {}, set(f.grid_slaves())
        indexed = len(getattr(self, 'search_settings', []))

        def mode_rows(name):
            nonlocal before, indexed
            now = set(f.grid_slaves())
            modes[name] = list(now - before)
            before = now
            for entry in getattr(self, 'search_settings', [])[indexed:]:  # found by the search: shown
                entry['reveal'] = lambda rows=modes[name]: [widget.grid() for widget in rows]
            indexed = len(getattr(self, 'search_settings', []))
        slider_row('camera_sensitivity', _('Sensitivity (direct)', 'Чувствительность (прямая)'),
                   _('How far the direct camera turns per mouse movement (1.00 = 900 counts per '
                     'radian).', 'Насколько поворачивается прямая камера при движении мыши (1.00 = '
                     '900 отсчётов на радиан).'), 0.1, 5.0)
        mode_rows('direct')
        slider_row('speed', _('Sensitivity (stick)', 'Чувствительность (стик)'),
                   _('How fast the camera turns for a given mouse movement in stick emulation.',
                     'Насколько быстро поворачивается камера при движении мыши в эмуляции стика.'), 0.1, 5.0)
        slider_row('speed_offset', _('Minimum turn speed (stick)', 'Минимальная скорость поворота (стик)'),
                   _('A constant added on top of sensitivity; raises the floor for slow '
                     'movements without affecting fast ones as much.',
                     'Добавляется поверх чувствительности; поднимает минимальную скорость для '
                     'медленных движений, почти не влияя на быстрые.'), 0.0, 1.0)
        mode_rows('stick')

        def show_mode(*_a):
            for name, widgets in modes.items():
                for widget in widgets:
                    widget.grid() if name == camera_var.get() else widget.grid_remove()
        camera_var.trace_add('write', show_mode)
        show_mode()
        slider_row('sensitivity_y', _('Vertical sensitivity', 'Вертикальная чувствительность'),
                   _('Up and down as a multiple of left and right (1.00 = the same).',
                     'Вверх и вниз относительно влево и вправо (1.00 = одинаково).'), 0.1, 3.0)

        def invert_check(key, text):
            var = self.mouse_vars[key] = tk.BooleanVar(value=ms[key])
            var.trace_add('write', lambda *_a: (ms.__setitem__(key, var.get()), mark_mouse_edited()))
            box = ttk.Checkbutton(f, text=text, variable=var)
            box.grid(row=self.next_row(f), column=0, columnspan=2, sticky='w', pady=(4, 0))
            self.index_entry(box, text, key=f'mouse:{key}')

        invert_check('invert_x', _('Invert horizontal', 'Инвертировать по горизонтали'))
        invert_check('invert_y', _('Invert vertical', 'Инвертировать по вертикали'))
        self.note(f, _('Vertical sensitivity and inverting apply to both camera modes.',
                       'Вертикальная чувствительность и инверсия действуют в обоих режимах камеры.'), top=2)

        self.note(f, _('Mouse buttons and the scroll wheel are bound in the slots below: click a '
                       'slot, then click or turn the wheel. They work while the mouse is captured '
                       '(mouse look on).',
                       'Кнопки мыши и колесо назначаются в ячейках ниже: щёлкните ячейку, затем '
                       'щёлкните или прокрутите колесо. Они работают, пока мышь захвачена '
                       '(обзор мышью включён).'), top=10)
        self.note(f, _('Changes apply the next time the game starts, or press F8 in-game to '
                       'reload input.ini without restarting.',
                       'Изменения применяются при следующем запуске игры, либо нажмите F8 в '
                       'игре, чтобы перечитать input.ini без перезапуска.'), top=6)

    def build_controller_bindings(self, f):
        """The Controller section: one controller button per game action, chosen by pressing it."""
        ttk = self.ttk
        self.section(f, _('Controller', 'Геймпад'))
        self.note(f, _('Click a slot, then press the controller button for that action; Esc cancels. '
                       'Right-click a slot to put back its default. A button on two actions presses '
                       'both. The sticks always move and turn the camera.',
                       'Щёлкните ячейку и нажмите кнопку геймпада для этого действия; Esc отменяет. '
                       'Правый клик возвращает кнопку по умолчанию. Кнопка на двух действиях нажимает '
                       'оба. Стики всегда отвечают за движение и камеру.'), top=2)
        table = ttk.Frame(f)
        table.grid(row=self.next_row(f), column=0, columnspan=2, sticky='w')
        table.columnconfigure(1, minsize=self.px(150))
        self.controller_rows = {}
        self.controller_listening = False
        for (group, group_ru), actions in ACTION_GROUPS:
            outputs = [(o, label) for o, label in actions if o in CONTROLLER_OUTPUTS]
            if not outputs:
                continue
            ttk.Label(table, text=_(group, group_ru), style='Section.TLabel').grid(
                row=table.grid_size()[1], column=0, columnspan=3, sticky='w', pady=(14, 4))
            for output, (label, label_ru) in outputs:
                r = table.grid_size()[1]
                ttk.Label(table, text=_(label, label_ru)).grid(row=r, column=0, sticky='w', padx=(0, 18), pady=2)
                btn = ttk.Button(table, style='Slot.TButton', command=lambda o=output: self.start_controller_capture(o))
                btn.grid(row=r, column=1, sticky='ew', padx=2, pady=2)
                btn.bind('<Button-3>', lambda _e, o=output: self.set_controller(o, DEFAULT_CONTROLLER.get(o)))
                warn = ttk.Label(table, style='Muted.TLabel')
                warn.grid(row=r, column=2, sticky='w', padx=(8, 0))
                self.controller_rows[output] = (btn, warn)
        reset = ttk.Frame(f)
        button = ttk.Button(reset, text=_('Reset controller buttons', 'Сбросить кнопки геймпада'),
                            command=lambda: [self.set_controller(o, DEFAULT_CONTROLLER.get(o))
                                             for o in CONTROLLER_OUTPUTS])
        button.pack(side='left')
        self.row(f, '', reset)
        self.index_entry(button, _('Reset controller buttons', 'Сбросить кнопки геймпада'),
                         key='button:reset_controller')
        self.refresh_controller_rows()

    def current_controller(self, output):
        return self.controller_edits.get(output, self.controller_bindings.get(output))

    def refresh_controller_rows(self):
        current = {o: self.current_controller(o) for o in self.controller_rows}
        conflicts = controller_conflicts(current)
        for output, (btn, warn) in self.controller_rows.items():
            name = current[output]
            text = controller_display_name(name, LANG == 'ru') if name else '—'
            btn.configure(text=text + ('  ⚠' if output in conflicts else ''), style='Slot.TButton')
            others = conflicts.get(output, [])
            warn.configure(text='⚠ ' + _('also {}', 'также {}').format(
                ', '.join(_(*OUTPUT_LABELS[o]) for o in others)) if others else '')

    def set_controller(self, output, name):
        if self.controller_listening:
            return
        self.controller_edits[output] = name
        self.refresh_controller_rows()

    def start_controller_capture(self, output):
        """Waits for a controller button (read through the game's SDL3, bbport_gamepad) and binds
        it to `output`. Buttons already held when it starts count only once released and pressed
        again. Esc, or 10 seconds without a press, cancels."""
        if self.controller_listening:
            return
        if self.pads is None:
            self.pads = bbport_gamepad.Pads(PORT_DIR)
        if not self.pads.connected():
            self.messagebox.showwarning(
                _('Controller', 'Геймпад'),
                _('No controller found. Connect one, then click the slot again.',
                  'Геймпад не найден. Подключите его и снова щёлкните ячейку.') +
                (f'\n\n{self.pads.problem}' if self.pads.problem else ''))
            return
        self.controller_listening = True
        btn = self.controller_rows[output][0]
        for other, _warn in self.controller_rows.values():
            other.state(['disabled'])
        btn.configure(text=_('Press a button…', 'Нажмите кнопку…'), style='Listen.TButton')
        ignored = set(self.pads.pressed())
        deadline = time.monotonic() + 10

        def finish(name=None):
            self.root.unbind_all('<KeyPress-Escape>')
            for other, _warn in self.controller_rows.values():
                other.state(['!disabled'])
            self.controller_listening = False
            if name:
                self.controller_edits[output] = name
            self.refresh_controller_rows()

        def poll():
            if not self.controller_listening:
                return
            held = self.pads.pressed()
            ignored.intersection_update(held)  # released: counts again
            new = sorted(held - ignored)
            if new:
                finish(new[0])
            elif time.monotonic() > deadline:
                finish()
            else:
                self.root.after(15, poll)
        self.root.bind_all('<KeyPress-Escape>', lambda _e: finish())
        self.root.after(15, poll)

    def apply_layout_preset(self):
        if self.messagebox.askyesno(
                _('Dark Souls III layout', 'Раскладка Dark Souls III'),
                _('Replace your keyboard and mouse bindings with the Dark Souls III layout? '
                  'Controller bindings stay as they are.',
                  'Заменить привязки клавиатуры и мыши раскладкой Dark Souls III? Привязки '
                  'геймпада не изменятся.')):
            self.write_layout()

    def write_layout(self):
        """Writes the Dark Souls III layout into input.ini now (bbport_input_config.apply_layout)
        and shows it: pending edits on the Controls page are replaced by it. False when the file
        cannot be written (said in a message)."""
        lines = apply_layout(self.remap_lines)
        try:
            textfile.write(self.remap_ini_path, '\n'.join(lines) + '\n')
        except OSError as error:
            self.messagebox.showerror(_('Layout', 'Раскладка'), str(error))
            return False
        self.remap_edits = {}
        self.mouse_edited = False
        self.reload_input_ini()
        for key, var in getattr(self, 'mouse_vars', {}).items():
            var.set(self.mouse_settings[key])
        self.mouse_edited = False  # the widgets' traces mark what was just read
        if getattr(self, 'remap_rows', None):
            self.refresh_remap_row(None)
        return True

    def reload_input_ini(self):
        """input.ini read again after a save. The mouse settings are updated in place (the Controls
        page's widgets hold that dict) and keep changes not saved yet."""
        self.remap_bindings, self.remap_lines, self.remap_toggle_key, self.remap_reload_key, \
            mouse = load_input_ini(self.remap_ini_path)
        if not self.mouse_edited:
            self.mouse_settings.update(mouse)

    def device_switch(self, parent):
        """Two buttons in a row: controller, keyboard and mouse (play_with)."""
        tk, px = self.tk, self.px
        holder = tk.Frame(parent, bg=LINE, padx=1, pady=1)
        var = self.var('play_with', 'app')
        for value, text in (('controller', _('Controller', 'Геймпад')),
                            ('keyboard', _('Keyboard and mouse', 'Клавиатура и мышь'))):
            tk.Radiobutton(holder, text=text, variable=var, value=value, indicatoron=False,
                           command=lambda v=value: self.set_play_with(v), bg=theme.SURFACE, fg=theme.TEXT_SOFT,
                           selectcolor=theme.BLOOD, activebackground=theme.SURFACE, activeforeground=theme.TEXT_STRONG,
                           relief='flat', offrelief='flat', bd=0, highlightthickness=0, cursor='hand2',
                           font=(theme.BODY_MEDIUM, 10), padx=px(14), pady=px(5)).pack(side='left', padx=(0, 1))
        return holder

    def set_play_with(self, device):
        """The player plays with a controller or with keyboard and mouse. Keyboard and mouse: the
        mouse camera on and the Dark Souls III layout, at once while the keys are the shipped ones,
        asked when they were changed by hand. Controller: the mouse camera off, so the game leaves
        the pointer alone; the keys stay. Saved with the other settings (collect)."""
        self.var('play_with', 'app').set(device)
        if device == 'keyboard':
            layout = 'own' if self.remap_edits else keyboard_layout(self.remap_lines)
            if layout == 'default' or layout == 'own' and self.messagebox.askyesno(
                    _('Dark Souls III layout', 'Раскладка Dark Souls III'),
                    _('You changed the keyboard and mouse bindings yourself. Replace them with the '
                      'Dark Souls III layout? The controller\'s buttons stay.\n\nNo keeps your keys '
                      'and only turns the mouse camera on.')):
                self.write_layout()
        on = device == 'keyboard'
        if self.mouse_settings['enabled'] != on:
            if 'enabled' in getattr(self, 'mouse_vars', {}):
                self.mouse_vars['enabled'].set(on)  # its trace marks the change
            else:
                self.mouse_settings['enabled'] = on
                self.mouse_edited = True

    def refresh_remap_row(self, output):
        """Every row, since a change to one binding can add or clear another's conflict mark."""
        conflicts = slot_conflicts({o: self.current_slots(o) for o in self.remap_rows})
        russian = LANG == 'ru'
        for o, (buttons, warn) in self.remap_rows.items():
            values = self.current_slots(o)
            marked = conflicts.get(o, {})
            for i, btn in enumerate(buttons):
                key = values[i] if i < len(values) else None
                text = ' + '.join(display_name(p, russian) for p in binding_parts(key)) if key else '—'
                btn.configure(text=text + ('  ⚠' if i in marked else ''), style='Slot.TButton')
                mark = self.remap_clear_marks.get((o, i))
                if mark is not None:
                    if key:
                        mark.place(in_=btn, relx=1.0, x=-self.px(4), y=self.px(2), anchor='ne')
                    else:
                        mark.place_forget()
            others = sorted({b for slot in marked.values() for b in slot})
            warn.configure(text='⚠ ' + _('also {}', 'также {}').format(
                ', '.join(_(*OUTPUT_LABELS[c]) for c in others)) if others else '')

    def set_slot(self, output, slot, value):
        values = list(self.current_slots(output))
        values[slot] = value
        self.remap_edits[output] = values

    def clear_remap(self, output, slot):
        if not self.remap_listening:
            self.set_slot(output, slot, None)
            self.refresh_remap_row(output)

    def start_remap_capture(self, output, slot):
        """Grabs the next keypress or mouse click and binds it to `output`'s slot. Esc
        cancels; a key/button already reserved (HOT-001/HOT-002), or a mouse click on a
        STICK_OUTPUTS row (an axis has no use for a press), is rejected in place, leaving the
        previous binding untouched. bind_all (not root.bind) plus grab_set makes sure the event
        reaches this handler even if some other widget currently has focus, and that no other
        widget (e.g. the player-name Entry on another page) reacts to the same keypress. Every
        slot is disabled meanwhile, so a click on another slot binds the click instead of
        starting over there.

        The <ButtonPress> bind is installed via after_idle rather than immediately: this call
        itself runs from the slot's own click handler, so the left-button-release that
        follows (Tk doesn't fire <ButtonPress> again for that click, but belt-and-suspenders
        against any platform quirk) would otherwise risk being seen as "the user chose
        leftbutton" the instant capture starts."""
        if self.remap_listening:
            return
        self.remap_listening = True
        is_stick = output in STICK_OUTPUTS
        key_label = self.remap_rows[output][0][slot]
        for buttons, _warn in self.remap_rows.values():
            for btn in buttons:
                btn.state(['disabled'])
        key_label.configure(text=_('Press…', 'Нажмите…'), style='Listen.TButton')
        self.root.grab_set()
        # Shift, Ctrl and Alt held so far, in press order: the next input makes a combination
        # with them, and releasing one with nothing else pressed binds the modifiers alone.
        modifiers = []

        # Tk does not always turn the wheel click and the side ("macro") buttons into
        # <ButtonPress> events (it depends on the Tcl/Tk build and the mouse driver), so on
        # Windows they are also polled straight from the OS while waiting. `done` makes the
        # first of the two paths to see a press the only one that binds it.
        poller = None if is_stick else windows_mouse_poller()
        state = {'done': False, 'poll_job': None}

        # Tk on Windows sends Alt, Alt + key and F10 to the menu bar (tk::WinMenuKey, which
        # waits in the system menu's loop), not to <KeyPress>: they are taken over meanwhile.
        menu_keys = {seq: self.root.bind_all(seq) for seq in (
            '<Key-Alt_L>', '<Key-Alt_R>', '<Alt-Key>', '<Key-F10>',
            '<KeyRelease-Alt_L>', '<KeyRelease-Alt_R>', '<Alt-KeyRelease>', '<KeyRelease-F10>')}

        def finish():
            state['done'] = True
            if state['poll_job'] is not None:
                self.root.after_cancel(state['poll_job'])
                state['poll_job'] = None
            self.root.unbind_all('<KeyPress>')
            self.root.unbind_all('<KeyRelease>')
            for seq, script in menu_keys.items():
                self.root.bind_all(seq, script)
            self.root.unbind_all('<ButtonPress>')
            self.root.unbind_all('<Shift-MouseWheel>')
            self.root.bind_all('<MouseWheel>', self.wheel)  # the pages scroll again
            self.root.grab_release()
            for buttons, _warn in self.remap_rows.values():
                for btn in buttons:
                    btn.state(['!disabled'])
            self.remap_listening = False
            self.refresh_remap_row(output)

        def apply(name):
            if state['done']:
                return
            parts = (modifiers + [name])[-MAX_COMBO:] if name not in modifiers else list(modifiers)
            reserved = [p for p in parts if p in reserved_keys(self.remap_toggle_key, self.remap_reload_key)]
            if reserved:
                self.messagebox.showwarning(
                    _('Reserved', 'Зарезервировано'),
                    _('{} is reserved (menu, recording, or a hotkey) and cannot be bound to a '
                      'game action.', '{} зарезервирована (меню, запись или горячая клавиша) и '
                      'не может быть назначена игровому действию.').format(reserved[0]))
            else:
                self.set_slot(output, slot, ', '.join(parts))
            finish()

        def on_key(event):
            if event.keysym == 'Escape':
                finish()
                return
            name = keysym_to_name(event.keysym)
            if name is None:
                self.messagebox.showwarning(
                    _('Unsupported key', 'Клавиша не поддерживается'),
                    _('That key has no bbport name; pick another one.',
                      'У этой клавиши нет имени в bbport; выберите другую.'))
                return
            if name in MODIFIER_NAMES:
                if name not in modifiers and len(modifiers) < MAX_COMBO - 1:
                    modifiers.append(name)
                    key_label.configure(text=' + '.join(display_name(m, LANG == 'ru') for m in modifiers) + ' + …')
                return  # wait: another input makes a combination, a release binds it alone
            apply(name)

        def on_key_release(event):
            if modifiers and keysym_to_name(event.keysym) in modifiers:
                apply(modifiers[-1])

        def on_click(event):
            name = button_num_to_name(event.num)
            if name is None:
                return  # a button Tk reports but bbport has no name for: ignore, keep waiting
            if is_stick:
                self.messagebox.showwarning(
                    _('Keyboard only', 'Только клавиатура'),
                    _('A stick direction can only be bound to a key, not a mouse button.',
                      'Направление стика можно привязать только к клавише, а не к кнопке мыши.'))
                return
            apply(name)

        def on_wheel(event, horizontal=False):
            name = wheel_to_name(event.delta, horizontal)
            if name:
                apply(name)
            return 'break'

        def pointer_over_launcher():
            """The polled buttons are read system-wide: only a press with the pointer over the
            launcher's own window counts, not a click in another program."""
            try:
                x, y = self.root.winfo_pointerxy()
                return self.root.winfo_containing(x, y) is not None
            except KeyError:
                return True  # tkinter cannot name a combobox's popdown, but it is ours

        def poll_mouse():
            state['poll_job'] = None
            if state['done']:
                return
            name = poller.poll()  # refreshed on every tick, even when the press is ignored
            if name and pointer_over_launcher():
                apply(name)
                return
            state['poll_job'] = self.root.after(10, poll_mouse)

        self.root.bind_all('<KeyPress>', on_key)
        self.root.bind_all('<KeyRelease>', on_key_release)
        for seq in menu_keys:
            self.root.bind_all(seq, on_key_release if 'Release' in seq else on_key)
        if not is_stick:
            self.root.after_idle(lambda: self.root.bind_all('<ButtonPress>', on_click))
            # While waiting, the wheel binds instead of scrolling the page.
            self.root.bind_all('<MouseWheel>', on_wheel)
            self.root.bind_all('<Shift-MouseWheel>', lambda e: on_wheel(e, True))
            if poller is not None:
                state['poll_job'] = self.root.after(10, poll_mouse)

    def build_advanced(self):
        ttk = self.ttk
        f = self.scrolled_page('advanced', _('Advanced', 'Дополнительно'),
                               _('Launcher options, performance switches and diagnostics.',
                                 'Настройки лаунчера, производительность и диагностика.'))
        self.section(f, _('Launcher', 'Лаунчер'), top=4)
        self.check(f, 'check_updates', 'app', _('Check for updates when the launcher opens',
                                                'Проверять обновления при открытии лаунчера'))
        holder = ttk.Frame(f)
        holder.grid(row=self.next_row(f), column=0, columnspan=2, sticky='w', pady=(10, 0))
        button = ttk.Button(holder, text=_('Port folder', 'Папка порта'), command=lambda: self.open_path(DATA_DIR))
        button.pack(side='left')
        self.index_entry(button, _('Port folder', 'Папка порта'), key='button:port_folder')
        ttk.Button(holder, text=_('Launcher language', 'Язык лаунчера') + '  ›', command=lambda: self.show('language')).pack(
            side='left', padx=6)
        ttk.Button(holder, text='bbport.ini', command=lambda: self.open_path(ini_path())).pack(side='left')
        ttk.Button(holder, text=_('Repeat the setup'), command=self.open_setup).pack(side='left', padx=6)
        # The updates and the shader cache: here since the original launcher, and on Help for the simple view (the
        # search finds them there, a page of both views), the shader cache also in Help's black screen topic.
        holder = ttk.Frame(f)
        holder.grid(row=self.next_row(f), column=0, columnspan=2, sticky='w', pady=(10, 0))
        ttk.Button(holder, text=_('Check for updates', 'Проверить обновления'), command=self.check_update_now).pack(
            side='left')
        ttk.Button(holder, text=_('Other versions…', 'Другие версии…'), command=self.choose_version).pack(
            side='left', padx=(6, 0))
        ttk.Label(holder, text=f'v{VERSION_TEXT}', style='Muted.TLabel').pack(side='left', padx=10)
        holder = ttk.Frame(f)
        holder.grid(row=self.next_row(f), column=0, columnspan=2, sticky='we', pady=(10, 0))
        ttk.Button(holder, text=_('Clear the shader cache'), command=self.clear_cache).pack(side='left')
        hint = _('For a black screen, or a broken picture after a driver update. The next start stutters for a few '
                 'minutes.')
        self.wrap_within(ttk.Label(holder, text=hint, style='Muted.TLabel', justify='left', anchor='w'), 560).pack(
            side='left', fill='x', expand=True, padx=10)
        self.section(f, _('Appearance'))  # the translucent window: Launcher customization > Launcher
        self.check(f, 'windows_title_bar', 'app', _("Windows' title bar instead of the launcher's own"),
                   _('Applies when the launcher opens again.', 'Применится при следующем открытии лаунчера.'))
        self.var('windows_title_bar', 'app').trace_add('write', lambda *_a: self.app.__setitem__(
            'windows_title_bar', self.var('windows_title_bar', 'app').get()))
        self.section(f, _('Performance', 'Производительность'))
        self.row(f, _('Two-stage GPU pipeline', 'Двухстадийный конвейер GPU'), self.choice(f, 'draw_pipe', 'app', DRAW_PIPE),
                 _('20–30% faster; switch it off if the game is unstable.', 'Быстрее на 20–30%; при нестабильности выключите.'))
        self.row(f, _('GPU readbacks', 'Чтение данных GPU'), self.choice(f, 'readbacks', 'app', READBACKS),
                 _('How exactly data the GPU writes is copied back for the game.',
                   'Насколько точно данные, записанные GPU, возвращаются игре.'))
        self.row(f, _('Low-VRAM profile', 'Профиль для малого объёма видеопамяти'),
                 self.choice(f, 'low_vram', 'app', LOW_VRAM),
                 _('A larger texture budget and, at 1080p with an upscaler, a smaller scene from the start '
                   '(preset changes then need a restart).',
                   'Больший бюджет текстур и, при 1080p с апскейлером, уменьшенная сцена с запуска '
                   '(смена пресета тогда требует перезапуска).'))
        self.check(f, 'vram_warning', 'app', _('Warn when video memory runs low',
                                               'Предупреждать о нехватке видеопамяти'),
                   _('Off: no "Video memory is running low" banner in the game. Memory is freed as before; the log '
                     'still says when it ran short.',
                     'Выкл.: в игре нет баннера «Видеопамять заканчивается». Память освобождается как прежде; журнал '
                     'по-прежнему сообщает, когда её не хватило.'))
        ttk.Label(f, text=_('Low-VRAM profile by fixdag and bashiku.',
                            'Профиль для малого объёма видеопамяти: fixdag и bashiku.'),
                  style='Muted.TLabel').grid(row=self.next_row(f), column=0, columnspan=2, sticky='w', pady=(8, 0))
        self.section(f, _('Diagnostics', 'Для разработчика'))
        self.check(f, 'frame_stats', 'app', _('Frame statistics in the log (every 5 s)', 'Статистика кадров в журнале (раз в 5 с)'))
        self.check(f, 'gpu_profile', 'app', _('GPU time per pass in the log', 'Профиль GPU в журнале'))
        self.check(f, 'vk_validation', 'app', _('Vulkan validation layers (needs the Vulkan SDK; much slower)',
                                                'Слои валидации Vulkan (нужен Vulkan SDK; сильно замедляет)'))
        self.row(f, _('Extra variables', 'Доп. переменные'), ttk.Entry(f, textvariable=self.var('extra_env', 'app'), width=58),
                 _('NAME=value pairs separated by spaces (README lists them).', 'Пары ИМЯ=значение через пробел (список в README).'))
        # The Cathedral Ward benchmark for testers: with the game tests off (GAME_TESTS) it runs by
        # itself with the player's settings and gives one zip to send.
        holder = ttk.Frame(f)
        holder.grid(row=self.next_row(f), column=0, columnspan=2, sticky='we', pady=(10, 0))
        ttk.Button(holder, text=_('Benchmark for bug reports…'), command=self.open_bug_benchmark).pack(side='left')
        self.wrap_within(ttk.Label(holder, text=_('A fixed route in Cathedral Ward for testers: one zip to send.'),
                                   style='Muted.TLabel', justify='left', anchor='w'), 560).pack(
            side='left', fill='x', expand=True, padx=10)
        self.index_entry(holder, _('Benchmark for bug reports…'),
                         _('A fixed route in Cathedral Ward for testers: one zip to send.'), 'button:benchmark')

    def open_bug_benchmark(self):
        """The game tests (System) with the Cathedral Ward scenario chosen; with the game tests off,
        the benchmark by itself (run_bug_benchmark)."""
        if not GAME_TESTS:
            self.run_bug_benchmark()
            return
        self.var('test_scenario', 'app').set('cathedral-ward')
        self.show('system')

    def run_bug_benchmark(self):
        """Cathedral Ward with the player's settings: one run on the scenario's own save, offline and
        in fullscreen; at the end its report as a zip, shown in Explorer."""
        import bbport_benchmark as bench
        title = _('Benchmark for bug reports…').rstrip('…')
        if self.update_installing():
            return
        if self.process or bench.game_pids():
            self.messagebox.showinfo(title, _('Close the game first.'))
            return
        if self.test_running():
            self.messagebox.showinfo(title, _('A game test is running already.'))
            return
        scenario = next((s for s in bench.scenarios(self.tests_dir() / 'scenarios') if s.id == 'cathedral-ward'), None)
        if not scenario or not game_info(self.var('game_dir', 'app').get()):
            self.messagebox.showinfo(title, _('Choose the game folder first.', 'Сначала выберите папку игры.'))
            return
        if not self.messagebox.askokcancel(title, _(
                'The benchmark plays a fixed route in Cathedral Ward on a save of its own, with your graphics '
                'settings, in fullscreen (about 3 minutes). Your saves and settings stay as they are.\n\n'
                'Leave the mouse, keyboard and controller alone until it ends. Then a zip to send opens in '
                'Explorer.'), parent=self.root):
            return
        self.collect()
        import platform
        import time as clock
        w, h = self.display_size()
        info = {'Launcher': f'v{VERSION_TEXT}', 'Graphics card': self.gpu_line or '?',
                'Processor': self.cpu.name if self.cpu else '?', 'Memory': f'{self.ram} GB', 'Display': f'{w} × {h}',
                'Windows': platform.version(), 'Game': ' '.join(game_info(self.var('game_dir', 'app').get()) or ())}
        folder = self.tests_dir() / (clock.strftime('%Y-%m-%d_%H%M%S') + '_benchmark')
        settings = {**self.app, 'online': False, 'fullscreen': True}
        self.bench = bench.Test(PORT_DIR, run_command(), game_environment(settings), self.save_user_dir(),
                                self.var('game_dir', 'app').get(),
                                ini_path().read_text(encoding='utf-8') if ini_path().is_file() else '',
                                scenario, bench.Variants({}, [('benchmark', {})]), folder,
                                lambda kind, data: self.ui_calls.put(lambda: self.bug_benchmark_event(kind, data)),
                                kind='compare', info=info)
        self.play_button.configure(state='disabled')
        self.status.configure(text=_('Game test running: the game opens and closes by itself.'), fg=GOLD)
        self.bench.start(self.save_target())

    def bug_benchmark_event(self, kind, data):
        if kind not in ('done', 'error'):
            return
        test, self.bench = self.bench, None
        if test:
            test.finished = True
        self.refresh_status()
        title = _('Benchmark for bug reports…').rstrip('…')
        run = (data[1] or [None])[0] if kind == 'done' else None
        if kind == 'error' or not run or run.get('status') != 'ok':
            reason = data if kind == 'error' else (run or {}).get('message') or (run or {}).get('status') or '?'
            self.messagebox.showwarning(title, _('The benchmark stopped: {}').format(reason))
            return
        import bbport_report
        try:
            archive = bbport_report.export(data[2])
        except OSError as error:
            self.messagebox.showwarning(title, _('The benchmark stopped: {}').format(error))
            return
        subprocess.Popen(['explorer', '/select,', str(archive)])
        self.messagebox.showinfo(title, _('Benchmark finished. Send this file: {}').format(archive))

    def build_log(self):
        tk, ttk, px = self.tk, self.ttk, self.px
        page = ttk.Frame(self.content)
        self.pages['log'] = page
        self.page_header(page, _('Log', 'Журнал'), '').pack(fill='x')
        body = ttk.Frame(page, padding=(px(32), 0, px(24), px(22)))
        body.pack(fill='both', expand=True)
        top = ttk.Frame(body)
        top.pack(fill='x', pady=(0, px(10)))
        ttk.Button(top, text=_('Copy', 'Копировать'), command=self.copy_log).pack(side='right')
        ttk.Button(top, text=_('Clear', 'Очистить'), command=lambda: self.set_log('')).pack(side='right', padx=6)
        self.log = tk.Text(body, wrap='none', bg=theme.DEEP, fg=theme.TEXT_SOFT, insertbackground=TEXT, relief='flat',
                           font=('Consolas', 9), padx=12, pady=10, state='disabled', highlightthickness=1,
                           highlightbackground=LINE, highlightcolor=LINE, selectbackground=BLOOD,
                           selectforeground=theme.TEXT_STRONG)
        bar = ttk.Scrollbar(body, command=self.log.yview)
        self.log.configure(yscrollcommand=bar.set)
        bar.pack(side='right', fill='y', padx=(px(4), 0))
        self.log.pack(fill='both', expand=True)

    def version_warning(self, parent):
        """A banner shown while the selected game is not version 1.09 (refresh_status)."""
        label = self.ttk.Label(parent, style='Warning.TLabel', justify='left', anchor='w', text=_(
            'Your game is version {}: these options are patches for 1.09 and are not applied; the game runs '
            'at 30 FPS. Update the game to 1.09 to use them.',
            'Ваша игра версии {}: эти параметры — патчи для 1.09 и не применяются; игра работает в 30 FPS. '
            'Обновите игру до 1.09, чтобы их использовать.'))
        self.wrap_within(label, 640).grid(row=self.next_row(parent), column=0, columnspan=2, sticky='we', pady=(6, 4))
        label.template = label.cget('text')
        self.warnings = getattr(self, 'warnings', []) + [label]

    # ---- state -------------------------------------------------------------------------------
    def game_changed(self):
        self.draw_banner()
        self.set_icon()
        self.refresh_status()

    def check_line(self, key, text, target=None):
        """A line of the ready check: '✓ …', '• …', '⚠ …' or '✗ …' ('' hides it); a target makes it
        a link."""
        icon, label = self.checks[key]
        line, go = self.check_lines[key]
        if not text:
            line.pack_forget()
            return
        if not line.winfo_manager():
            order = list(self.check_lines)
            after = [self.check_lines[k][0] for k in order[order.index(key) + 1:] if self.check_lines[k][0].winfo_manager()]
            line.pack(fill='x', pady=self.px(4), **({'before': after[0]} if after else {}))
        glyph, color = {'✓': ('ok', GOLD), '•': ('info', MUTED), '⚠': ('warn', theme.AMBER),
                        '✗': ('error', theme.BLOOD_LIGHT)}.get(text[:1], (None, TEXT))
        icon.configure(text=theme.ICON[glyph] if glyph else '', fg=color)
        label.configure(text=text[1:].lstrip() if glyph else text,
                        foreground=TEXT if glyph == 'ok' else MUTED if glyph == 'info' else color)
        self.check_targets[key] = target
        cursor = 'hand2' if target else ''
        for widget in (line, icon, label, go):
            widget.configure(cursor=cursor)
        if target:
            go.pack(side='right', anchor='n')
        else:
            go.pack_forget()

    def refresh_status(self):
        info = game_info(self.var('game_dir', 'app').get())
        if not info:
            game = renamed(_('✗ No eboot.bin in the game folder (Game & effects)', '✗ В папке игры нет eboot.bin («Игра и эффекты»)'))
        elif info[1] == PATCH_VERSION:
            game = _('✓ Game version {}: every patch available', '✓ Версия игры {}: доступны все патчи').format(info[1])
        else:
            game = _('⚠ Game version {}: runs at 30 FPS without the community patches (they are for 1.09)',
                     '⚠ Версия игры {}: 30 FPS без патчей сообщества (они для 1.09)').format(info[1])
        user = Path(self.var('user_dir', 'app').get() or DATA_DIR / 'user')
        saves = [user] if has_complete_save(user, self.var('game_dir', 'app').get()) else []
        save = (_('✓ Saves found in {}', '✓ Найдены сохранения в {}').format(user) if saves
                else _('• No saves yet: bring your character from another version, or the game creates them'))
        missing = fsr4_missing()
        # Another version's eboot.bin differs anyway (the base game 1.00): its line is the version's, with
        # the steps to copy the 1.09 update over it.
        eboot = self.eboot_note() if info and info[1] == PATCH_VERSION else None
        problem = (self.app.get('eboot_check') or {}).get('state') not in (None, 'clean')
        self.check_line('game', eboot or game, (lambda: self.open_topic('eboot_modified')) if eboot and problem else
                        (lambda: self.show('game')) if not info else
                        (lambda: self.open_topic('version')) if info[1] != PATCH_VERSION else None)
        self.check_line('saves', save, lambda: self.show('saves'))
        # The start check (scripts/vulkan_check.py): only when the port cannot start on the card, or when the game
        # runs on a slower one than the PC has (Vulkan does not see it, or its driver holds it back); Help says why.
        unused = self.unused_card()
        self.check_line('gpu', _('✗ The graphics card cannot run the port: see Help') if self.gpu_problem() else
                        self.gpu_unused_line(),
                        (lambda: self.open_topic('gpu_unseen')) if unused else lambda: self.show('help'))
        # The game folder in OneDrive or another synced folder (Help's check counts the files kept online only).
        self.check_line('files', '⚠ ' + _('The game folder is in OneDrive or another synced folder: move it (Help)')
                        if info and bbport_help.synced_game(self.var('game_dir', 'app').get()) else '',
                        lambda: self.open_topic('game_location'))
        # AMD's DirectX 12 FSR 4 and frame generation could not be loaded, and the Visual C++ runtime is missing.
        self.check_line('runtime', '• ' + _('AMD FSR 4 needs the Microsoft Visual C++ Redistributable: click to '
                                            'download it') if self.runtime_problem() else '', self.open_vc_redist)
        self.check_line('fsr4', _('• FSR 4 files missing: FSR 3.1 is used meanwhile. Click to download (30 MB)')
                        if missing and self.var('upscaler', 'ini').get() == 'fsr4' else '', self.download_fsr4)
        fsr411_ready, status = True, None
        if self.var('upscaler', 'ini').get() == 'fsr411':
            status = self.fsr411_status(self.var('fsr411_backend', 'ini').get())
            fsr411_ready = status['supported']
            self.check_line('fsr4', '' if fsr411_ready else '• FSR 4.1.1: ' + bbport_fsr411.hint(status, _),
                            lambda: self.show('graphics'))
        online = self.var('online', 'app').get()
        name = self.account_name()
        signed_in = bool(name)
        self.check_line('online', ('⚠ ' + _('Online is on, but you are not signed in yet: sign in now')
                                   if online and not signed_in else ''), lambda: self.show('online'))
        # Cancel at Windows' "allow this app" question: the firewall blocks the game's incoming connections.
        self.check_line('firewall', '⚠ ' + _('Windows Firewall blocks the game: co-op can fail (Help)')
                        if online and getattr(self, 'firewall_blocked', False) else '',
                        lambda: self.open_topic('firewall'))
        previous = online and self.var('online_pool', 'app').get() == 'huntersdream'  # nudged to SuperMedo Server
        self.check_line('pool', '• ' + _("The Hunter's Dream is the previous server, and connections there often "
                                         'drop: switch to SuperMedo Server') if previous else '',
                        lambda: self.show('online'))
        if getattr(self, 'online_note', None):
            self.online_note.configure(text=(_('signed in as {}').format(name) if online and signed_in and name else
                                             _('not signed in ›') if online and not signed_in else ''),
                                       fg=MUTED if signed_in else theme.AMBER)
        self.show_hunters()
        cheats = [_(*title) for key, title, _on in CHEATS if self.var(key, 'ini').get()]
        self.check_line('cheats', ('⚠ ' + _('Cheats on: {}').format(', '.join(t.split(' (')[0] for t in cheats))
                                   if cheats else ''), lambda: self.show('cheats'))
        news = self.var('setup_done', 'app').get() and self.app.get('seen_version') != VERSION_TEXT
        self.check_line('news', '• ' + _('New in version {}: what has changed').format(VERSION_TEXT) if news else '',
                        self.open_news)
        for label in getattr(self, 'warnings', []):
            if info and info[1] != PATCH_VERSION:
                label.configure(text=label.template.format(info[1]))
                label.grid()
            else:
                label.grid_remove()
        if not self.process:
            self.play_button.configure(state='normal' if info and fsr411_ready and not self.test_running()
                                       and not self.fsr411_downloading else 'disabled')
            self.status.configure(text=bbport_fsr411.hint(status, _) if info and not fsr411_ready else
                                  self.summary() if info else _('Choose the game folder first.',
                                                                'Сначала выберите папку игры.'), fg=MUTED)

    def check_online_routes(self):
        """The Online page opened: settings.json again, in case another program wrote it meanwhile (Seamless
        co-op writes its server). The shadNet settings it set are taken; the pool and the SuperMedo Server and The
        Hunter's Dream sign-ins stay as the launcher has them, settled again (settled_pool), and are written
        back, so the page and the next start use the right sign-in."""
        written = getattr(self, 'online_written', None)
        if written is None or not CONFIG_FILE.exists():
            return
        taken, kept = outside_online_changes(written, load_settings())
        if not taken and not kept:
            return
        for key, value in taken.items():
            self.var(key, 'app').set(value)
        pool = self.var('online_pool', 'app')
        settled = settled_pool({'online_pool': pool.get(), 'online_server': self.var('online_server', 'app').get(),
                                'online_custom_servers': self.var('online_custom_servers', 'app').get()})
        if settled != pool.get():
            pool.set(settled)
        self.collect()
        self.append(_('Online: another program changed the online settings (a co-op tool?). Its shadNet '
                      'settings are used; your service and sign-ins stay as they were.') + '\n')

    def account_name(self):
        """The chosen pool's account (pool_account) as the page shows it now."""
        return pool_account({key: self.var(key, 'app').get() for key in (
            'online_pool', 'thd_online_id', 'thd_token', 'supermedo_npid', 'supermedo_pass', 'online_npid')})

    def poll_hunters(self):
        """Every minute: the count of The Hunter's Dream again when it is the chosen pool, the window is
        shown, no game runs and the last look is HUNTERS_ONLINE_EVERY old (a helper thread asks)."""
        self.root.after(60_000, self.poll_hunters)
        self.ask_hunters()

    def ask_hunters(self):
        if (not HUNTERS_DREAM_POOL or getattr(self, 'hunters_asking', True) or self.process or self.root.state() == 'iconic'
                or self.var('online_pool', 'app').get() != 'huntersdream'
                or time.time() - self.hunters_when < HUNTERS_ONLINE_EVERY):
            return
        self.hunters_asking = True

        def ask():
            found = None

            def done():
                self.hunters, self.hunters_when, self.hunters_asking = found, time.time(), False
                self.show_hunters()
            try:
                found = hunters_online()
            finally:  # hunters_asking back to False whatever happened: a thread that died left it True for good
                self.ui_calls.put(done)
        threading.Thread(target=ask, daemon=True).start()

    def show_hunters(self):
        """'● 798 online' beside Play online, and the split on the Online page; nothing when unknown."""
        known = getattr(self, 'hunters', None) if self.var('online_pool', 'app').get() == 'huntersdream' else None
        if getattr(self, 'hunters_note', None):
            self.hunters_note.configure(text='● ' + _('{} online').format(known[0]) if known else '')
        if getattr(self, 'hunters_line', None):
            self.hunters_line.configure(text=_("The Hunter's Dream: {} hunters online right now ({} bbhost · {} shadNet)")
                                        .format(*known) if known else '')

    def maybe_show_news(self):
        """At start: What's new, once per version (VERSION_TEXT: every beta counts). A new installation's
        setup guide comes first and counts as seeing it."""
        if self.app.get('seen_version') == VERSION_TEXT:
            return
        if getattr(self, 'guide', None):
            self.app['seen_version'] = VERSION_TEXT
            return
        if bbport_news.notes(VERSION_TEXT) is not None:  # else the Play page's line links the release page
            self.open_news()

    def open_news(self):
        """The What's new window (bbport_news), or the release page for a version without notes."""
        if bbport_news.notes(VERSION_TEXT) is None:
            self.app['seen_version'] = VERSION_TEXT
            webbrowser.open(BETA_PAGE if BETA else RELEASES_PAGE)
            self.refresh_status()
        elif not getattr(self, 'news_panel', None):
            self.news_panel = bbport_news.NewsPanel(self, VERSION_TEXT)

    def summary(self):
        fps = dict(FPS_MODES).get(self.var('fps_mode', 'app').get(), ('?',))
        info = game_info(self.var('game_dir', 'app').get())
        if info and info[1] != PATCH_VERSION:
            fps = ('30 FPS',)
        upscaler = dict(UPSCALERS).get(self.var('upscaler', 'ini').get(), ('?',))[0].split(' (')[0]
        selected = self.var('upscaler', 'ini').get()
        if selected in ('fsr411', 'fsr412'):
            upscaler += ' · ' + ('DX12' if self.var('fsr411_backend', 'ini').get() == 'dx12'
                                  else 'AMD Vulkan' if selected == 'fsr412' else 'fsr4vk')
        output = self.var('output_res', 'ini').get().replace('x', ' × ')
        online = _('Online') if self.var('online', 'app').get() else _('Offline')
        if self.var('online', 'app').get() and self.var('online_pool', 'app').get() == 'huntersdream':
            online = _("Online (The Hunter's Dream)")
        return f'{_(*fps)}   ·   {upscaler}   ·   {output}   ·   {online}'

    def detect_gpu(self):
        """Helper thread: the graphics card, its support checks and the processor (detect_hardware), for the Play
        page and the rating. However it ends, the window hears that it is over: an error nobody expected ended the
        thread with "Checking the graphics card…" and the support checks waiting for good. Then the error is on the
        Log page, and the checks that had not answered say "check failed"."""
        try:
            self.detect_hardware()
        except Exception as error:
            failed, why = fsr41_check_failed(error), error_text(error, kind=True)

            def not_checked():
                self.append(_('The check of the graphics card stopped: {}').format(why) + '\n')
                if self.fsr411_support is None:  # its answer, when it came, ran before this
                    self.fsr411_support = failed
                    self.refresh_fsr411()
                    self.refresh_status()
            self.ui_calls.put(not_checked)
            if self.fg401_support is None:
                self.fg401_support = fg401_check_failed()
        finally:
            self.ui_calls.put(self.refresh_status)
            self.ui_calls.put(self.hardware_ready)
            self.ui_calls.put(self.refresh_motion)
            self.ui_calls.put(self.refresh_fg401)

    def detect_hardware(self):
        exe = PORT_DIR / 'bin' / 'bb-gpu-capabilities.exe'
        if not exe.is_file():
            exe = PORT_DIR / 'out' / 'bb-gpu-capabilities.exe'
        env = dict(os.environ)
        for item in str(self.app.get('extra_env', '')).split():
            if '=' in item:
                key, value = item.split('=', 1)
                env[key] = value
        if not (exe.parent / 'SDL3.dll').is_file():
            clang64 = Path(os.environ.get('MSYS2_ROOT', r'C:\msys64')) / 'clang64' / 'bin'
            env['PATH'] = f'{clang64}{os.pathsep}{env.get("PATH", "")}'
        text = _('• Graphics card: not checked', '• Видеокарта: не проверена')
        names = []
        try:
            result = subprocess.run([str(exe), '--live-resolution'], capture_output=True, text=True, timeout=30,
                                    env=env, creationflags=NO_WINDOW)
            names = [line[5:].split(':')[0] for line in result.stderr.splitlines() if line.startswith('GPU: ')]
            if names:
                text = _('✓ Graphics card: {}', '✓ Видеокарта: {}').format(names[0])
            elif result.returncode:
                text = _('✗ No Vulkan 1.3 graphics card found (update the driver)',
                         '✗ Не найдена видеокарта с Vulkan 1.3 (обновите драйвер)')
        except (OSError, ValueError, subprocess.TimeoutExpired):  # ValueError: output in another code page
            pass
        self.gpu_text = text
        self.vulkan = vulkan_verdict(env)
        support = bbport_fsr41.query(PORT_DIR, env)
        def fsr411_done():
            self.fsr411_support = support
            self.refresh_fsr411()
            self.refresh_status()
        self.ui_calls.put(fsr411_done)
        self.fg401_support = bbport_fg401.query_versions(PORT_DIR, env)
        self.firewall_blocked = bbport_help.firewall_blocks(PORT_DIR / 'bin' / 'bb-probe.exe')
        self.gpu_line = hardware.best_gpu_line(names)  # 'Name, discrete, 16304 MiB' for the hardware check
        try:
            self.cpu, self.ram = hardware.detect_cpu(), hardware.memory_gb()
        except Exception:  # the rating is a guide: never stop the launcher over it
            self.cpu, self.ram = hardware.Cpu('', None), 16
        self.cpu_instructions = hardware.instructions()  # the System page's hard limits

    def refresh_fsr4(self):
        total, missing = len(fsr4_files()), len(fsr4_missing())
        self.fsr4_progress.configure(value=total - missing)
        if missing:  # how much is there; a full bar beside a finished download says nothing
            self.fsr4_progress.pack(side='left', padx=12)
        else:
            self.fsr4_progress.pack_forget()
        self.fsr4_label.configure(
            text=_('Installed: {} files in {}.', 'Установлены: {} файлов в {}.').format(total, PORT_DIR / 'fsr4_shaders')
            if not missing else _('{} of {} files missing in {}.', 'Нет {} из {} файлов в {}.').format(
                missing, total, PORT_DIR / 'fsr4_shaders'))
        self.fsr4_button.configure(state='normal' if missing and not self.downloading else 'disabled')

    def fsr411_status(self, method):
        return bbport_fsr411.method_status(PORT_DIR, DATA_DIR, method,
            getattr(self, 'fsr411_support', None), fetch_fsr4vk,
            bbport_fsr411.environment(PORT_DIR, self.app))

    def fsr41_status(self, method):
        if self.var('upscaler', 'ini').get() == 'fsr412':
            return bbport_fsr412.method_status(method, getattr(self, 'fsr411_support', None))
        return self.fsr411_status(method)

    def fsr411_folder(self):
        env = bbport_fsr411.environment(PORT_DIR, self.app)
        return bbport_fsr411.vulkan_folder(PORT_DIR, DATA_DIR, env)

    def recheck_fsr411(self):
        self.fsr411_support = None
        self.refresh_fsr411()
        def work():
            try:
                support = bbport_fsr41.query(PORT_DIR,
                    bbport_fsr411.environment(PORT_DIR, self.app), refresh=True)
            except Exception as error:  # never "checking support…" for good
                support = fsr41_check_failed(error)
                self.ui_calls.put(lambda e=error_text(error, kind=True): self.append(
                    _('The check of the graphics card stopped: {}').format(e) + '\n'))
            def done():
                self.fsr411_support = support
                self.refresh_fsr411()
                self.refresh_status()
            self.ui_calls.put(done)
        threading.Thread(target=work, daemon=True).start()

    def refresh_fsr411(self):
        folder = self.fsr411_folder()
        installed = fetch_fsr4vk.files_ok(folder)
        self.fsr411_verified = installed
        self.fsr411_label.configure(text=_('Vulkan (fsr4vk)') + ': ' +
            bbport_fsr411.hint(self.fsr411_status('vk'), _))
        self.fsr411_native_label.configure(text=_('AMD DX12') + ': ' +
            bbport_fsr411.message(self.fsr411_status('dx12'), _))
        for refresh in getattr(self, 'fsr411_refreshers', []):
            refresh()
        for refresh in getattr(self, 'fsr411_upscaler_refreshers', []):
            refresh()
        caps = getattr(self, 'fsr411_support', None)
        hardware_ok = caps is None or caps['vk']['supported'] or caps['vk']['reason'] == 'check'
        self.fsr411_button.configure(state='normal' if hardware_ok and not installed and not self.fsr411_downloading and not self.process else 'disabled')
        if installed:
            self.fsr411_progress.configure(value=100)
        if self.fsr411_downloading:  # the bar only while fsr4vk downloads
            beside = {'after': self.fsr411_button} if self.fsr411_button.winfo_manager() == 'pack' else {}
            self.fsr411_progress.pack(side='left', padx=12, **beside)
        else:
            self.fsr411_progress.pack_forget()

    def download_fsr411(self, on_success=None):
        if self.process or self.fsr411_downloading:
            return
        self.fsr411_downloading = True
        self.play_button.configure(state='disabled')
        self.refresh_fsr411()
        folder = self.fsr411_folder()

        def progress(done, total):
            percent = min(100, done * 100 // max(total, 1))
            self.ui_calls.put(lambda p=percent: self.fsr411_progress.configure(value=p))

        def work():
            error = None

            def done():
                self.fsr411_downloading = False
                if not error and on_success and self.fsr411_status('vk')['supported']:
                    on_success()
                self.refresh_fsr411()
                self.refresh_status()
                if error:
                    self.messagebox.showerror('FSR 4.1.1', _('Download failed: {}', 'Не удалось скачать: {}').format(error))
            try:
                fetch_fsr4vk.fetch(folder, progress)
                if not fetch_fsr4vk.files_ok(folder):
                    raise fetch_fsr4vk.FetchError(_('Vulkan (fsr4vk): files missing or need updating.'))
            except Exception as failure:  # a broken download too (IncompleteRead)
                error = error_text(failure)
            finally:  # Play and the button come back whatever happened (R85)
                self.ui_calls.put(done)
        threading.Thread(target=work, daemon=True).start()

    def download_fsr4(self):
        self.downloading = True
        self.fsr4_button.configure(state='disabled')
        folder = PORT_DIR / 'fsr4_shaders'

        def work():
            error = None

            def done():
                self.downloading = False
                self.refresh_fsr4()
                self.refresh_status()
                if error:
                    self.messagebox.showerror('FSR 4', _('Download failed: {}', 'Не удалось скачать: {}').format(error))
            try:
                folder.mkdir(parents=True, exist_ok=True)
                for name in fsr4_missing():
                    part = folder / (name + '.part')
                    with bbport_tls.urlopen(f'{FSR4_BASE}/{name}', timeout=60) as response:
                        part.write_bytes(response.read())
                    part.replace(folder / name)
                    self.ui_calls.put(self.refresh_fsr4)
            except Exception as failure:  # a broken download too (IncompleteRead)
                error = connection_error(failure, FSR4_BASE)
            finally:  # the button comes back whatever happened: it stayed grey (R85)
                self.ui_calls.put(done)
        threading.Thread(target=work, daemon=True).start()

    def refresh_lists(self, only_if_changed=False):
        """The mods and the patch files drawn anew (on a page change only when something changed)."""
        if not hasattr(self, 'patches_frame'):
            return
        if only_if_changed and self.lists_made_from_now() == getattr(self, 'lists_made_from', None):
            return
        tk, ttk = self.tk, self.ttk
        from mods import discover, unrecognized
        from patches import external_patches
        for holder in (self.mods_frame, self.patches_frame):
            for widget in holder.winfo_children():
                widget.destroy()
        root = Path(self.var('mods_dir', 'app').get() or DATA_DIR / 'mods')
        profile = load_json(DATA_DIR / 'mods.json', {})
        available = discover(root)
        if set(self.mod_order) != set(available):
            known = [n for n in profile.get('order', []) if n in available]
            self.mod_order = known + [n for n in available if n not in known]
        old = {n: v.get() for n, v in self.mod_vars.items()}
        self.mod_vars = {}
        if not available:
            self.wrap_within(ttk.Label(self.mods_frame, text=_('No mods in {} yet.', 'В {} пока нет модов.').format(
                root), style='Muted.TLabel', justify='left', anchor='w'), 620).pack(fill='x')
        self.mod_rows = []
        for index, name in enumerate(self.mod_order):
            var = tk.BooleanVar(value=old.get(name, name not in profile.get('disabled', [])))
            self.mod_vars[name] = var
            self.mod_rows.append(self.mod_row(index, name, var))
        if len(self.mod_order) > 1:
            ttk.Label(self.mods_frame, text=_('Drag a mod by its grip to change the order. Lower in the list loads '
                                              'later and wins conflicts.'),
                      style='Muted.TLabel').pack(anchor='w', pady=(6, 0))
        # Folders that are not mods are named, with why, instead of left out silently.
        for name, problem in unrecognized(root):
            self.wrap_within(ttk.Label(self.mods_frame, text=_('Not loaded: {}: {}', 'Не загружен: {}: {}').format(
                name, problem), style='Muted.TLabel', justify='left', anchor='w'), 620).pack(fill='x', pady=(4, 0))
        chosen = load_json(DATA_DIR / 'patches.json', {})
        found = external_patches(Path(self.var('patches_dir', 'app').get() or DATA_DIR / 'patches'), PATCH_VERSION)
        old = {n: v.get() for n, v in self.patch_vars.items()}
        self.patch_vars = {}
        if not found:
            ttk.Label(self.patches_frame, text=_('No patch files for version 1.09 yet.', 'Пока нет патчей для версии 1.09.'),
                      style='Muted.TLabel').pack(anchor='w')
        for key, _path, meta in found:
            on = key in chosen.get('enabled', []) or (key not in chosen.get('disabled', []) and
                                                      meta.get('isEnabled', 'false').lower() == 'true')
            var = tk.BooleanVar(value=old.get(key, on))
            self.patch_vars[key] = var
            author = meta.get('Author')
            ttk.Checkbutton(self.patches_frame, text=key + (f'  —  {author}' if author else ''), variable=var).pack(anchor='w')
        self.lists_made_from = self.lists_made_from_now()

    def lists_made_from_now(self):
        """What the mod and patch lists are drawn from: the folders, the saved choices, the order."""
        mods_dir = Path(self.var('mods_dir', 'app').get() or DATA_DIR / 'mods')
        patches_dir = Path(self.var('patches_dir', 'app').get() or DATA_DIR / 'patches')
        return (str(mods_dir), folder_entries(mods_dir), str(patches_dir), folder_entries(patches_dir),
                tuple(self.mod_order), folder_entries(DATA_DIR, ('mods.json', 'patches.json')))

    def mod_row(self, index, name, var):
        """One mod: a grip to drag it by, its place, its switch and its name (a click on the name
        switches it too)."""
        tk, ttk, px = self.tk, self.ttk, self.px
        row = tk.Frame(self.mods_frame, bg=CARD, highlightthickness=1, highlightbackground=LINE)
        row.pack(fill='x', pady=(0, px(4)))
        grip = tk.Label(row, text='\ue76f', bg=CARD, fg=MUTED, font=(theme.ICONS, 11), cursor='fleur')
        grip.pack(side='left', padx=(px(10), px(6)), pady=px(8))
        tk.Label(row, text=f'{index + 1}.', bg=CARD, fg=MUTED, font=(theme.BODY, 10), width=3, anchor='e').pack(
            side='left')
        ttk.Checkbutton(row, variable=var, style='Card.TCheckbutton').pack(side='left', padx=(px(6), 0))
        hints = []
        if name.casefold() == 'dvdroot_ps4':  # a mod extracted bare into the mods folder
            hints.append(tk.Label(row, text=_('Put this in a folder of its own (e.g. mods\\<mod name>\\dvdroot_ps4)',
                                              'Положите его в отдельную папку (например, mods\\<имя мода>\\dvdroot_ps4)'),
                                  bg=CARD, fg=MUTED, font=(theme.BODY, 9), anchor='w', justify='left'))
            self.wrap_within(hints[0], 520)
            hints[0].pack(side='bottom', fill='x', padx=(px(4), px(10)), pady=(0, px(6)))
        label = tk.Label(row, text=name, bg=CARD, fg=TEXT, font=(theme.BODY, 10), anchor='w', cursor='hand2')
        label.pack(side='left', fill='x', expand=True, padx=(px(4), px(10)))
        for widget in (row, grip, label, *hints):
            widget.bind('<ButtonPress-1>', lambda e, i=index: self.mod_drag_start(e, i))
            widget.bind('<B1-Motion>', self.mod_drag_move)
            widget.bind('<ButtonRelease-1>', lambda _e, v=var, w=widget: self.mod_drag_end(v if w is label else None))
        return row

    def mod_drag_start(self, event, index):
        self.mod_drag = {'from': index, 'y': event.y_root, 'to': None, 'line': None}

    def mod_drag_move(self, event):
        drag = getattr(self, 'mod_drag', None)
        if not drag or (drag['to'] is None and abs(event.y_root - drag['y']) < self.px(5)):
            return  # a click, not yet a drag
        rows = self.mod_rows
        if drag['to'] is None:  # the drag begins: the row lifts
            self.mod_paint(rows[drag['from']], theme.SURFACE, GOLD)
            drag['line'] = self.tk.Frame(self.mods_frame, bg=GOLD, height=self.px(2))
        # where it would land: before the first row whose middle is below the pointer
        to = sum(1 for row in rows if row.winfo_rooty() + row.winfo_height() / 2 < event.y_root)
        drag['to'] = to
        last = rows[-1]
        top = rows[to].winfo_y() - self.px(3) if to < len(rows) else last.winfo_y() + last.winfo_height() + self.px(1)
        drag['line'].place(x=0, y=max(0, top), relwidth=1, height=self.px(2))
        drag['line'].lift()

    def mod_drag_end(self, switch=None):
        """The drop: the mod moves to where the line is. Without a drag a click on the name switches
        the mod (`switch` is its variable then)."""
        drag, self.mod_drag = getattr(self, 'mod_drag', None), None
        if not drag:
            return
        if drag['to'] is None:
            if switch is not None:
                switch.set(not switch.get())
            return
        self.mod_order = moved(self.mod_order, drag['from'], drag['to'])
        self.refresh_lists()

    def mod_paint(self, row, bg, edge):
        row.configure(bg=bg, highlightbackground=edge)
        for child in row.winfo_children():
            if child.winfo_class() == 'Label':
                child.configure(bg=bg)

    @staticmethod
    def ini_text(key, value):
        return ('1' if value else '0') if key in INI_FLAGS else \
            f'{float(value):.2f}' if key == 'sharpness' else str(value)

    def sync_ini(self):
        """Takes what the in-game menu wrote to bbport.ini while the launcher was open, except where
        the launcher has an edit of its own that is not saved yet (else Play wrote the launcher's
        older values over the menu's)."""
        disk, lines = load_ini()
        for key, var in self.vars.items():
            if var.store != 'ini' or key not in disk:
                continue
            try:
                shown = self.ini_text(key, var.get())
            except (self.tk.TclError, ValueError):  # an unfinished number in a spinbox
                continue
            if shown == self.ini.get(key) and shown != disk[key]:
                var.set(disk[key] == '1' if key in INI_FLAGS else disk[key])
        # The running game writes the whole file whenever its menu saves anything. A frame rate it holds (started
        # with, or chosen in its menu since) and writes back unchanged is no choice of its own: one the launcher
        # saved since (set while the game ran) stays and is written again, else the game's old value came back and
        # the next start ran with it (R88).
        started = getattr(self, 'game_started', None) or {}  # a game started here, until its end: what it holds
        kept = {}
        for key in MIRRORED_KEYS:  # changed by the game's PC Settings page
            var = self.vars.get(key)
            if key not in disk or disk[key] == self.ini.get(key):
                continue
            if key in started and disk[key] == started[key] and self.ini.get(key) is not None:
                kept[key] = self.ini[key]
                continue
            if key in started:  # chosen in the game's menu: what the game holds from now on
                started[key] = disk[key]
            if var is None or str(var.get()) == str(self.app.get(key)):
                self.app[key] = disk[key]
                if var is not None:
                    var.set(disk[key])
        if kept:
            try:
                save_ini(kept, lines)
                disk, lines = load_ini()
            except OSError:
                pass  # written with the next save of the settings
            note = ('Settings: ' + ', '.join(f'{key}={value}' for key, value in kept.items()) + ', set in the '
                    'launcher while the game ran, kept (the game had written the value it held back)')
            if note not in textfile.REPAIRS:
                textfile.REPAIRS.append(note)
        self.ini, self.ini_lines = disk, lines

    def remember_game_start(self):
        """What the game starting now runs with: bbport.ini as run.py reads it (sync_ini compares the game's saves
        with it)."""
        self.game_started = {key: self.ini.get(key) for key in MIRRORED_KEYS}

    def game_restarts(self):
        """The game restarts itself (its settings menu). It saved bbport.ini before, and the new start reads that
        file: taken first, else a save handled after this looked like the old start's values written back, and the
        launcher's older ones replaced the choice just made in the game."""
        self.sync_ini()
        self.remember_game_start()

    def collect(self):
        """Writes settings.json, bbport.ini, mods.json and patches.json."""
        self.sync_ini()
        for key, var in self.vars.items():
            try:
                value = var.get()
            except self.tk.TclError:  # an unfinished number in a spinbox
                value = APP_DEFAULTS.get(key, INI_DEFAULTS.get(key))
            if var.store == 'ini':
                self.ini[key] = self.ini_text(key, value)
            else:
                self.app[key] = value
        for key in SHARED_GRAPHICS_KEYS:
            self.app.pop(key, None)
        own = own_output(self.app, self.ini.get('output_res'))
        if own:  # the game's menu wrote 1920x1080 over it
            self.ini['output_res'] = own
            if 'output_res' in self.vars:
                self.vars['output_res'].set(own)
            textfile.REPAIRS.append(f"Output: {own}, the player's own size, set again (the game's menu had "
                                    f"written {INI_DEFAULTS['output_res']})")
        self.app['output_custom'] = custom_output(self.ini.get('output_res'), self.app)
        write_settings(self.app)
        self.online_written = {key: self.app.get(key) for key in SHADNET_KEYS + OWN_ONLINE_KEYS}
        save_ini({**{key: self.ini[key] for key in INI_DEFAULTS},
                  **{key: str(self.app.get(key, APP_DEFAULTS[key])) for key in MIRRORED_KEYS}}, self.ini_lines)
        self.ini, self.ini_lines = load_ini()
        if self.remap_edits:
            save_input_ini(self.remap_ini_path, {key: [v for v in values if v]
                                                 for key, values in self.remap_edits.items()}, self.remap_lines)
            self.reload_input_ini()
            self.remap_edits = {}
            if getattr(self, 'remap_rows', None):
                self.refresh_remap_row(None)  # emptied slots close up, as the file has them
        if getattr(self, 'controller_edits', None):  # a Launcher built without __init__ (tests) has none
            save_controller_bindings(self.remap_ini_path, self.controller_edits, self.remap_lines)
            self.reload_input_ini()
            self.controller_bindings = load_controller_bindings(self.remap_lines)
            self.controller_edits = {}
        if self.mouse_edited:
            save_mouse_settings(self.remap_ini_path, self.mouse_settings, self.remap_lines)
            self.mouse_edited = False
            self.reload_input_ini()
        # Whole or not at all, as bbport.ini: a file cut off is read as empty, and the mods' order and the ones
        # turned off were gone.
        if self.mod_order:
            textfile.write(DATA_DIR / 'mods.json', json.dumps(
                {'order': self.mod_order, 'disabled': [n for n, v in self.mod_vars.items() if not v.get()]},
                indent=2))
        if self.patch_vars:
            chosen = load_json(DATA_DIR / 'patches.json', {})
            shown = set(self.patch_vars)
            enabled = {k for k in chosen.get('enabled', []) if k not in shown} | {k for k, v in self.patch_vars.items() if v.get()}
            disabled = {k for k in chosen.get('disabled', []) if k not in shown} | {k for k, v in self.patch_vars.items() if not v.get()}
            textfile.write(DATA_DIR / 'patches.json', json.dumps(
                {'enabled': sorted(enabled), 'disabled': sorted(disabled)}, indent=2))

    # ---- game process ------------------------------------------------------------------------
    def play(self):
        if self.process or self.fsr411_downloading or self.update_installing():
            return
        if game_underway():
            # Started from Steam, a shortcut or the in-game restart: its window comes to the front instead of a
            # second game on the same saves.
            front_game_window()
            self.append(_('The game runs already (started from Steam, a shortcut or the in-game restart): its '
                          'window was brought to the front.',
                          'Игра уже запущена (из Steam, ярлыка или после перезапуска в игре): её окно выведено '
                          'на передний план.') + '\n')
            self.status.configure(text=_('The game is running.', 'Игра запущена.'), fg=GOLD)
            return
        online, npid = self.var('online', 'app'), self.var('online_npid', 'app')
        if not online.get() and self.account_name():
            choice = self.messagebox.askyesnocancel(
                'Bloodborne', _('An online account is filled in, but "Play online" is off.\n\n'
                                'Yes: play online.   No: play offline.'))
            if choice is None:
                return
            online.set(bool(choice))
        pool_name = self.var('online_pool', 'app').get()
        hunters_dream = pool_name == 'huntersdream'
        if online.get() and hunters_dream and not self.var('thd_token', 'app').get():
            if self.messagebox.askyesno(
                    'Bloodborne', _("To play online with The Hunter's Dream, sign in with Discord first.") + '\n\n' +
                    _('Sign in now? (No: play offline this time.)')):
                self.show('online')
                return
            online.set(False)
        if online.get() and pool_name == 'supermedo' and not self.account_name():
            # Online without an account was offline without a word (a new player, the setup's default).
            if self.messagebox.askyesno(
                    'Bloodborne', _('To play online with SuperMedo Server, sign in with Discord or make a free '
                                    'account first.') + '\n\n' + _('Sign in now? (No: play offline this time.)')):
                self.show('online')
                return
            online.set(False)
        if online.get() and pool_name == 'shadnet' and '@' in npid.get():
            self.messagebox.showwarning(
                'Bloodborne', _('The Online ID is the account name (NPID) you registered on the server, '
                                'not your email address.'))
            self.show('online')
            return
        if self.var('output_display', 'app').get():
            self.var('output_res', 'ini').set(display.size_text(self.game_monitor_size()))
        if not self.save_settings():
            return
        if self.ini.get('upscaler') in ('fsr411', 'fsr412'):
            self.fsr411_support = bbport_fsr41.query(PORT_DIR,
                bbport_fsr411.environment(PORT_DIR, self.app), refresh=True)
            status = self.fsr41_status(self.ini['fsr411_backend'])
            if not status['supported']:
                self.show('graphics')
                self.refresh_fsr411()
                self.refresh_status()
                return
        if not game_info(self.app['game_dir']):
            self.messagebox.showerror('Bloodborne', _('Choose the game folder with eboot.bin (CUSA03173).',
                                                      'Выберите папку игры с eboot.bin (CUSA03173).'))
            self.show('game')
            return
        problem = start_problem(self.app)
        if problem:
            self.messagebox.showerror('Bloodborne', problem[0])
            self.show(problem[1])
            return
        if not self.preflight():  # bbport_help: a start that would fail for a reason the player can fix
            return
        self.set_log('')
        for line in textfile.REPAIRS:  # what was repaired since the last start: in this one's log
            self.append(line + '\n')
        textfile.REPAIRS.clear()
        self.pad_stop()  # the game reads the controller now
        line, encrypted = misplaced_saves(self.app)
        if line:
            self.append(line + '\n')
            self.refresh_saves()
        if encrypted:  # named once: the game starts without it
            self.app['encrypted_told'] = encrypted
            self.messagebox.showinfo('Bloodborne', _("These are a PS4's own encrypted backups (sdimg_…); they cannot "
                                                     'be used. Export the save with Apollo Save Tool on the PS4 '
                                                     'instead.') + '\n\n' + encrypted)
        self.auto_backup()
        self.remember_game_start()
        try:
            self.process = subprocess.Popen(run_command(), cwd=PORT_DIR, env=game_environment(self.app, self.ini),
                                            stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                            stderr=subprocess.STDOUT, creationflags=NO_WINDOW)
        except OSError as error:
            self.append(_('Could not start: {}', 'Не удалось запустить: {}').format(error) + '\n')
            self.process = self.game_started = None
            return
        self.job = GameJob(self.process)
        self.placer = display.Placer(self.app, display.parse_size(self.ini.get('output_res')) or (1920, 1080)).start()
        threading.Thread(target=self.read_output, args=(self.process, self.log.get('1.0', 'end-1c')),
                         daemon=True).start()
        self.play_button.configure(state='disabled')
        self.stop_button.configure(state='normal')
        self.stop_button.pack(side='right', after=self.play_button)
        self.refresh_fsr411()
        self.status.configure(text=_('Preparing the game; it opens in its own window…',
                                     'Подготовка игры; она откроется в своём окне…'), fg=GOLD)
        if self.setting('on_play') == 'close':
            self.root.after(5000, lambda: self.process and self.close())
        elif self.setting('on_play') == 'minimize':
            self.root.after(5000, lambda: self.process and self.root.state() == 'normal' and self.minimize())

    def auto_backup(self):
        """Before a start: the saves backed up when they changed since the last backup (never in the way)."""
        if not self.app.get('auto_backup', True):
            return
        notes = []
        try:
            made = saves.auto_backup(self.save_user_dir(), self.app['game_dir'], notes=notes)
        except OSError as error:
            self.append(_('The automatic backup of the saves failed: {}').format(error) + '\n')
            return
        for line in notes:
            self.append(line + '\n')
        if made:
            self.append(_('Saves backed up: {}').format(made.name) + '\n')
            self.refresh_saves()

    def window_left(self, size):
        """The game's last size in a window, for 'As I left it' next time."""
        if size and not self.app.get('fullscreen'):
            self.app['window_last'] = display.size_text(size)
            remember_window(size)

    def read_output(self, process, before=''):
        """Helper thread: the game's output to the Log page and to <saves folder>/last_run.log, after `before`
        (what the Log page said before the start: the repairs made for it). The game's end always reaches the
        window, and nothing about the log file stops the reading (RunLog): an unread pipe freezes the game."""
        log = RunLog(Path(self.app.get('user_dir') or DATA_DIR / 'user') / 'last_run.log', self.output.put)
        if before:
            log.write(before)
        try:
            self.forward_output(process, log)
        except Exception as error:
            # Nothing expected (the log file is RunLog's): the error to the window like a Tk callback's, and the rest
            # of the output read all the same before the wait below, as an unread pipe freezes the game.
            self.ui_calls.put(lambda e=error: self.root.report_callback_exception(type(e), e, e.__traceback__))
            try:
                while process.stdout.read(1 << 16):
                    pass
            except Exception:  # the pipe itself broke: nothing left to read
                pass
        finally:
            log.close()
            self.output.put((process.wait(),))

    def forward_output(self, process, log):
        """read_output's lines: to the Log page (drain_output), the eboot warnings, and the log file."""
        warned = False
        for raw in iter(process.stdout.readline, b''):
            text = raw.decode('utf-8', errors='replace')
            self.output.put(text)
            if not warned and 'eboot.bin is not the 01.09 build' in text:
                warned = True
                self.ui_calls.put(lambda: self.messagebox.showwarning('Bloodborne', _(
                    'Your eboot.bin is not the clean 1.09 one (modified, or from another release). The port '
                    'cannot patch it: the game runs at 30 FPS in 16:9 (ultrawide and the output resolution '
                    'do not apply), the effects (All graphics > Effects, Picture & performance > Taste) and the '
                    'gameplay tweaks (Gameplay) do nothing, and some scenes (like the cutscene after Rom) can '
                    'freeze.\n\nReplace eboot.bin with the one from an unmodified 1.09 update.')))
            elif not warned and 'eboot.bin is modified, not the clean 01.09 build' in text:
                # Some patch groups still apply (run.py): the line says which are off.
                warned = True
                detail = text.split(': ', 1)[-1].strip()
                self.ui_calls.put(lambda: self.messagebox.showwarning('Bloodborne', _(
                    "Your eboot.bin has been modified (for example with a 60 FPS patch already applied). "
                    "Part of the port's patches cannot be used with it:\n\n{}\n\nFor every option, "
                    "replace eboot.bin with the one from an unmodified 1.09 update.").format(detail)))
            log.write(text.replace('\r\n', '\n'))

    def drain_output(self):
        """Every 100 ms on the Tk thread: the helper threads' calls (ui_calls) and the game's output. Each part on
        its own, and the next round planned whatever happens: one failing call stopped every later answer to the
        window (a finished download, the game's end, so Play stayed disabled). A failure is reported like a Tk
        callback's (reported)."""
        closed = False
        try:
            if self.process:
                self.reported(self.follow_shared_ini)
            while not self.ui_calls.empty():
                self.reported(self.ui_calls.get_nowait())
            self.reported(self.tidy_up)
            closed = self.reported(self.take_output)
        finally:
            if not closed:  # the window closed with the game: no next round
                self.root.after(100, self.drain_output)

    def reported(self, call, *args):
        """call(*args); an error in it goes to callback_error like a Tk callback's (the Log page and a message), once
        per error: a part of drain_output that fails in every round would fill the Log page. None then."""
        try:
            return call(*args)
        except Exception:
            import traceback
            kind, value, trace = sys.exc_info()
            seen = self.__dict__.setdefault('drain_errors', set())
            text = ''.join(traceback.format_exception(kind, value, trace))
            if text not in seen:
                seen.add(text)
                self.root.report_callback_exception(kind, value, trace)
            return None

    def follow_shared_ini(self):
        """While the game runs: bbport.ini saved by the in-game menu, merged into the pages."""
        try:
            stamp = ini_path().stat().st_mtime_ns
            # Wait for a stable write before merging an overlay save into the UI.
            if stamp == getattr(self, '_shared_ini_seen', None) and stamp != getattr(self, '_shared_ini_synced', None):
                self.sync_ini()
                self._shared_ini_synced = stamp
            self._shared_ini_seen = stamp
        except OSError:
            pass

    def tidy_up(self):
        """Python's collector on the Tk thread (see gc.disable in __init__), and the picture a minimize put over the
        window taken away when the minimize did not happen."""
        if time.monotonic() - self._collected > 2:
            gc.collect()
            self._collected = time.monotonic()
        frame = self.custom_frame
        if frame and frame.cover and time.monotonic() - frame.cover_time > 1.5 and self.root.state() != 'iconic':
            self.uncover_window()  # a minimize that did not happen: the picture never stays over the window

    def take_output(self):
        """Up to 500 of the game's lines (read_output) to the Log page, and its end. True when the window closed
        with the game."""
        for _i in range(500):
            try:
                item = self.output.get_nowait()
            except queue.Empty:
                break
            if isinstance(item, tuple):
                if self.game_ended(item[0]):
                    return True
            else:
                self.reported(self.game_line, item)
        return False

    def game_ended(self, code):
        """The game's end (read_output's last item). The steps that read files or the game's leftovers each on their
        own (reported): one that failed left Play disabled and Stop shown. True when the window closed with it."""
        self.process = None
        self.append(_('\n— the game exited (code {}) —\n', '\n— игра завершилась (код {}) —\n').format(code))
        self.reported(self.sync_ini)
        self.game_started = None  # the frame rate the launcher now follows again (sync_ini)
        self.reported(self.refresh_fsr411)
        if self.job:
            self.reported(self.job.close)
        self.reported(self.refresh_saves)  # the characters' times
        if self.placer:
            placer, self.placer = self.placer, None
            placer.stop()
            self.reported(self.window_left, placer.last)
        self.stop_button.configure(state='disabled')
        self.stop_button.pack_forget()
        self.reported(self.forget_refused_sign_in)
        self.reported(self.refresh_status)
        reopen = self.setting('reopen_after_game')
        if self.hidden:
            if code == 0 and not reopen:
                self.drop_tray()
                self.root.destroy()
                return True
            # Back (asked for, or it stopped with an error: then with its log).
            self.hidden = False
            self.bring_back()
            if code != 0:
                self.show('log')
        elif reopen:
            self.bring_back()
        self.root.after(300, lambda: self.after_exit(code))  # the known causes in its log
        return False

    def game_line(self, item):
        """One line of the game's output to the Log page first, then what it changes: the status line, a notice for
        after the game, a trophy. The line is what the helpers look for: one of those that failed (a trophy toast
        that raised) took it from the page."""
        self.append(item)
        if 'Entering original x86-64 code' in item:
            self.status.configure(text=_('The game is running.', 'Игра запущена.'), fg=GOLD)
        elif 'restarting through run.py' in item:
            self.status.configure(text=_('Restarting with the new settings…', 'Перезапуск с новыми настройками…'),
                                  fg=GOLD)
            self.game_restarts()
        elif any(text in item for text, _topic in bbport_help.GAME_NOTICES):
            # Said once the game closes (after_exit).
            notices = self.__dict__.setdefault('game_notices', [])
            notices += [topic for text, topic in bbport_help.GAME_NOTICES
                        if text in item and topic not in notices]
        elif 'Runtime: trophy ' in item:
            self.trophies_changed(self.tracker.observe(item))

    def forget_refused_sign_in(self):
        """After the game: a sign-in The Hunter's Dream refused is forgotten (forget_sign_in), with a line in
        the log; the Play page and the Online page then say "not signed in", and the next Play asks."""
        token = self.var('thd_token', 'app')
        if not token.get() or not sign_in_refused(self.app):
            return
        token.set('')
        self.app['thd_token'] = ''
        try:
            forget_sign_in()
            if getattr(self, 'online_written', None) is not None:
                self.online_written['thd_token'] = ''  # the launcher's own write, not another program's
        except OSError:
            pass  # saved with the other settings later
        self.append(_("The Hunter's Dream refused this PC's sign-in: sign in again on the Online page.",
                      'The Hunter\'s Dream отклонил вход с этого ПК: войдите снова на странице «Онлайн-игра».') +
                    '\n')

    def stop(self):
        if self.job:
            self.job.terminate()

    def append(self, text):
        self.log.configure(state='normal')
        self.log.insert('end', text)
        lines = int(self.log.index('end-1c').split('.')[0])
        if lines > MAX_LOG_LINES:
            self.log.delete('1.0', f'{lines - MAX_LOG_LINES}.0')
        self.log.see('end')
        self.log.configure(state='disabled')

    def set_log(self, text):
        self.log.configure(state='normal')
        self.log.delete('1.0', 'end')
        self.log.insert('end', text)
        self.log.configure(state='disabled')

    def copy_log(self):
        self.root.clipboard_clear()
        self.root.clipboard_append(self.log.get('1.0', 'end'))

    def open_path(self, path, key=None):
        if key == 'user_dir' and not path:
            path = DATA_DIR / 'user'
        elif key == 'mods_dir' and not path:
            path = DATA_DIR / 'mods'
        elif key == 'patches_dir' and not path:
            path = DATA_DIR / 'patches'
        path = Path(path or DATA_DIR)
        if path.suffix == '.ini':
            self.collect()
        elif not path.exists():
            path.mkdir(parents=True, exist_ok=True)
        os.startfile(str(path))

    # ---- updates -------------------------------------------------------------------------------
    def check_update_now(self):
        """The Check for updates buttons (Help, Advanced): asked at once, the answer either way."""
        threading.Thread(target=self.check_update, args=(True,), daemon=True).start()

    def check_update(self, manual=False):
        """Helper thread: asks GitHub for the newest release (and a beta for the next beta) and offers it. Asked by
        the button, it always answers: a broken reply (BadStatusLine) ended the thread without a word."""
        try:
            offer = update_to_offer()
        except Exception as failure:
            if manual:
                self.ui_calls.put(lambda error=connection_error(failure, RELEASES_API): self.messagebox.showerror(
                    'Bloodborne', _('Could not check for updates: {}', 'Не удалось проверить обновления: {}').format(error)))
            return

        def show():
            if offer:
                self.offer_update(*offer)
            elif manual:
                self.messagebox.showinfo('Bloodborne', _('You have the latest version ({}).',
                                                         'У вас последняя версия ({}).').format(VERSION_TEXT))
        self.ui_calls.put(show)

    def offer_update(self, version, url, page):
        if self.update_box:
            return
        tk, ttk = self.tk, self.ttk
        text = _('Version {} is available.', 'Доступна версия {}.').format(version)
        box = tk.Frame(self.side, bg=CARD, highlightthickness=1, highlightbackground=theme.GOLD)
        self.update_label = tk.Label(box, text=text, bg=CARD, fg=GOLD, font=(theme.BODY_SEMI, 10),
                                     wraplength=self.px(180), justify='left')
        self.update_label.pack(anchor='w', padx=12, pady=(10, 8))
        buttons = tk.Frame(box, bg=CARD)
        buttons.pack(fill='x', padx=12, pady=(0, 12))
        buttons.columnconfigure((0, 1), weight=1, uniform='update')
        self.update_button = ttk.Button(buttons, text=_('Update', 'Обновить'), style='Card.Tight.Accent.TButton',
                                        command=lambda: self.install_update(version, url, page),
                                        state='disabled' if self.updating else 'normal')
        self.update_button.grid(row=0, column=0, sticky='we')
        ttk.Button(buttons, text=_("What's new", 'Что нового'), style='Card.Tight.TButton',
                   command=lambda: webbrowser.open(page)).grid(row=0, column=1, sticky='we', padx=(6, 0))
        # above the Expert view switch at the bottom of the side bar (side_note of the old launcher
        # is gone: the box failed to show, so 1.9.3 never offered an update)
        box.pack(side='bottom', fill='x', padx=self.px(14), pady=(self.px(8), self.px(4)), before=self.search_box)
        self.root.after_idle(self.fit_nav)
        self.update_box = box
        if not self.process:
            self.status.configure(text=text, fg=GOLD)

    def install_update(self, version, url, page):
        """The side box's Update button."""
        def say(text):
            self.update_label.configure(text=text)

        def failed():
            say(_('Version {} is available.', 'Доступна версия {}.').format(version))
        self.install_version(version, url, page, say, failed)

    def update_buttons(self):
        """The side box's Update and Other versions' Install, off while an update is under way: two at once shared
        one download folder, and the second one's start took the first one's files away."""
        state = 'disabled' if self.updating else 'normal'
        button = getattr(self, 'update_button', None)
        if button is not None and button.winfo_exists():
            button.configure(state=state)
        if self.versions_listed and self.version_window and self.version_window.winfo_exists():
            self.version_install.configure(state=state)

    def install_version(self, version, url, page, say, failed):
        """Downloads release `version`, checks it and starts an installer that puts it in place of this installation
        once this launcher has closed (saves, settings and mods stay; bbport_update). say(text) shows how far it is,
        failed() puts the caller's view back. One update at a time; False when nothing was started."""
        if self.updating:
            return False
        if self.game_running():
            self.messagebox.showinfo('Bloodborne', _('Close the game before updating.', 'Закройте игру перед обновлением.'))
            return False
        if self.test_running():  # its game runs from the files the update replaces
            self.messagebox.showinfo('Bloodborne', _('A game test is running. Update when it has finished.'))
            return False
        if not FROZEN or not url:  # a source tree updates with git
            webbrowser.open(page)
            return False
        if version_tuple(version) < version_tuple(VERSION):
            question = _('Install the older version {} now? The launcher closes, installs it and opens again. Saves and '
                         'settings are kept. To stay on it, switch off "Check for updates when the launcher opens".',
                         'Установить старую версию {} сейчас? Лаунчер закроется, установит её и откроется снова. '
                         'Сохранения и настройки останутся. Чтобы остаться на ней, выключите «Проверять обновления '
                         'при открытии лаунчера».')
        else:
            question = _('Install version {} now? The launcher closes, installs it and opens again. Saves and settings are kept.',
                         'Установить версию {} сейчас? Лаунчер закроется, установит её и откроется снова. Сохранения и '
                         'настройки останутся.')
        if not self.messagebox.askyesno('Bloodborne', question.format(version)):
            return False
        self.updating = True
        self.update_buttons()
        digest = ASSET_DIGESTS.get(url)

        def work():
            folder = None
            try:
                UPDATE_DIR.mkdir(parents=True, exist_ok=True)
                # A folder of its own: no leftover and no other download mixes into it.
                folder = Path(tempfile.mkdtemp(prefix=re.sub(r'[^\w.]+', '-', f'v{version}') + '-', dir=UPDATE_DIR))
                archive = folder / 'update.zip'
                request = urllib.request.Request(url, headers={'User-Agent': 'bbport-launcher'})
                with bbport_tls.urlopen(request, timeout=60) as response, open(archive, 'wb') as out:
                    total, done, shown = int(response.headers.get('Content-Length') or 0), 0, -1
                    while chunk := response.read(1 << 20):
                        out.write(chunk)
                        done += len(chunk)
                        percent = done * 100 // total if total else 0
                        if percent != shown:
                            shown = percent
                            self.ui_calls.put(lambda p=percent: say(_('Downloading version {}… {}%',
                                                                      'Загрузка версии {}… {}%').format(version, p)))
                self.ui_calls.put(lambda: say(_('Checking the download…')))
                inside = updater.check_package(archive, digest)  # before anything is unpacked or replaced
                with zipfile.ZipFile(archive) as package:
                    package.extractall(folder / 'new')
                    listed = updater.listed_files(package, inside)
                archive.unlink()
                # What the copy in the installation must hold, whatever happens to the unpacked files until then.
                (folder / 'files.json').write_text(json.dumps(listed), encoding='utf-8')
                installer = update_installer(folder)
            except Exception as failure:  # whatever it was (a broken HTTP answer is no OSError), the buttons come back
                self.ui_calls.put(lambda error=connection_error(failure, url) or type(failure).__name__:
                                  self.update_failed(failed, error, folder))
                return
            self.ui_calls.put(lambda: self.start_installer(version, installer, folder, folder / 'new' / inside, say,
                                                           failed))
        threading.Thread(target=work, daemon=True).start()
        return True

    def update_failed(self, failed, error, folder=None):
        """The update stopped before the launcher closed: the buttons come back, and the attempt's folder goes (a
        player retrying a broken download filled the system drive with them)."""
        if folder:
            shutil.rmtree(folder, ignore_errors=True)
        self.updating = self.installing = False
        self.update_buttons()
        failed()
        self.messagebox.showerror('Bloodborne', _('Update failed: {}', 'Не удалось обновить: {}').format(error))

    def update_installing(self):
        """True while the installer starts and this launcher is about to close for it (the status line says so): a
        game or a game test started now would keep the launcher open (close() only hides it while a game runs), and
        the installer would wait for their files."""
        if not getattr(self, 'installing', False):
            return False
        self.status.configure(text=_('An update is being installed. The launcher opens by itself when it is done.'),
                              fg=GOLD)
        return True

    def start_installer(self, version, installer, folder, new, say, failed):
        """The download is checked: the installer starts, and once it holds the update mutex (then a second start can
        no longer take its files away) this launcher closes the usual way, settings saved and the tray icon gone. A
        game or a game test started during the download is waited for first; from here on none starts."""
        if self.game_running() or self.test_running():
            say(_('Version {} is downloaded: it installs once the game is closed.').format(version))
            self.root.after(2000, lambda: self.start_installer(version, installer, folder, new, say, failed))
            return
        self.installing = True
        say(_('Starting the installation…'))
        env = {key: value for key, value in os.environ.items() if not key.upper().startswith('BB_UPDATE_')}
        env.update(BB_UPDATE_FROM=str(new), BB_UPDATE_VERSION=version, BB_UPDATE_FILES=str(folder / 'files.json'))
        try:
            process = subprocess.Popen([str(installer), '--install-update', str(PORT_DIR), str(os.getpid())],
                                       cwd=str(installer.parent), stdin=subprocess.DEVNULL, env=env,
                                       creationflags=NO_WINDOW)
        except OSError as failure:
            self.update_failed(failed, _('the new version did not start ({}). An antivirus program may have stopped '
                                         'it.').format(failure), folder)
            return

        def wait():
            problem = updater.wait_for_installer(process)
            if problem is None:
                self.ui_calls.put(self.close)
            else:
                self.ui_calls.put(lambda: self.update_failed(failed, _(
                    'the new version did not start ({}). An antivirus program may have stopped it.').format(problem),
                    folder))
        threading.Thread(target=wait, daemon=True).start()

    def choose_version(self):
        """Help or Advanced > Other versions: any release from OLDEST_INSTALLABLE on, chosen by the player."""
        if self.version_window and self.version_window.winfo_exists():
            self.version_window.lift()
            return
        tk, ttk = self.tk, self.ttk
        window = tk.Toplevel(self.root)
        self.version_window = window
        window.title(_('Other versions', 'Другие версии'))
        window.configure(bg=BG)
        window.transient(self.root)
        window.resizable(False, False)
        frame = ttk.Frame(window, padding=self.px(16))
        frame.pack(fill='both', expand=True)
        ttk.Label(frame, text=_('Install another version of the port, newer or older (back to {}). Saves, settings '
                                'and mods are kept.', 'Установить другую версию порта, новее или старше (до {}). '
                                'Сохранения, настройки и моды останутся.').format(OLDEST_INSTALLABLE),
                  wraplength=self.px(380), justify='left').pack(anchor='w')
        status = ttk.Label(frame, text=_('Loading the list of versions…', 'Загрузка списка версий…'),
                           style='Muted.TLabel', wraplength=self.px(380), justify='left')
        status.pack(anchor='w', pady=(10, 4))
        box = ttk.Combobox(frame, state='disabled', width=40)
        box.pack(anchor='w', fill='x')
        buttons = ttk.Frame(frame)
        buttons.pack(anchor='w', pady=(12, 0))
        install = ttk.Button(buttons, text=_('Install', 'Установить'), state='disabled')
        install.pack(side='left')
        self.version_install, self.versions_listed = install, False
        notes = ttk.Button(buttons, text=_("What's new", 'Что нового'), state='disabled')
        notes.pack(side='left', padx=6)
        ttk.Button(buttons, text=_('Close', 'Закрыть'), command=window.destroy).pack(side='left')
        found = []

        def chosen():
            index = box.current()
            return found[index] if 0 <= index < len(found) else None

        def show(versions):
            if not window.winfo_exists():
                return
            found.extend(versions)
            if not found:
                status.configure(text=_('No versions found.', 'Версии не найдены.'))
                return
            labels = []
            for version, _url, _page in found:
                label = f'v{version}'
                if version == found[0][0]:
                    label += _(' (latest)', ' (последняя)')
                if version_tuple(version) == version_tuple(VERSION):
                    label += _(' (installed)', ' (установлена)')
                labels.append(label)
            box.configure(values=labels, state='readonly')
            current = next((i for i, (v, _u, _p) in enumerate(found)
                            if version_tuple(v) == version_tuple(VERSION)), 0)
            box.current(current)
            status.configure(text=_('Installed: v{}', 'Установлена: v{}').format(VERSION))
            self.versions_listed = True
            install.configure(state='disabled' if self.updating else 'normal')  # the side box's may run
            notes.configure(state='normal')

        def failed_list(error):
            if window.winfo_exists():
                status.configure(text=_('Could not load the list of versions: {}',
                                        'Не удалось загрузить список версий: {}').format(error))

        def on_install():
            release = chosen()
            if not release:
                return
            version, url, page = release
            if version_tuple(version) == version_tuple(VERSION):
                self.messagebox.showinfo('Bloodborne', _('Version {} is already installed.',
                                                         'Версия {} уже установлена.').format(version))
                return

            def say(text):
                if window.winfo_exists():
                    status.configure(text=text)

            def failed():
                if window.winfo_exists():
                    box.configure(state='readonly')
                    status.configure(text=_('Installed: v{}', 'Установлена: v{}').format(VERSION))
            if self.install_version(version, url, page, say, failed):
                box.configure(state='disabled')

        install.configure(command=on_install)
        notes.configure(command=lambda: (release := chosen()) and webbrowser.open(release[2]))

        def fetch():
            try:
                versions = installable_releases()
            except Exception as failure:  # a broken reply too: never "Loading the list of versions…" for good
                self.ui_calls.put(lambda error=connection_error(failure, RELEASES_LIST_API): failed_list(error))
                return
            self.ui_calls.put(lambda: show(versions))
        threading.Thread(target=fetch, daemon=True).start()

    def clear_cache(self):
        """Shader and pipeline caches; they are rebuilt while playing. Not while a game runs, also one started
        through Steam or a shortcut (game_running): it holds them open. The button then did nothing, or said
        "cleared" over files it could not delete: now what was left is said."""
        if self.game_running():
            self.messagebox.showinfo('Bloodborne', _('Close the game first: it uses the shader cache while it runs.'))
            return
        cache = cache_folder(self.app) / 'cache'
        left = []
        if cache.exists():
            def failed(_function, _path, error):
                left.append(error_text(error[1] if isinstance(error, tuple) else error))
            if sys.version_info >= (3, 12):
                shutil.rmtree(cache, onexc=failed)
            else:
                shutil.rmtree(cache, onerror=failed)
        if left:
            self.messagebox.showerror('Bloodborne', _('Part of the shader cache could not be deleted: {}').format(
                left[0]))
            return
        self.messagebox.showinfo('Bloodborne', _('Shader cache cleared. The next start stutters for a few minutes while '
                                                 'it is rebuilt.', 'Кэш шейдеров очищен. Следующий запуск несколько '
                                                 'минут будет подтормаживать, пока кэш собирается заново.'))

    def shortcut(self):
        """Bloodborne.lnk on the desktop."""
        if FROZEN:
            target, arguments, icon = sys.executable, '', f'{sys.executable},0'
        else:  # a source tree: the launcher script with the windowless Python
            pythonw = Path(sys.executable).with_name('pythonw.exe')
            target = str(pythonw if pythonw.exists() else sys.executable)
            arguments, icon = Path(__file__).resolve(), PORT_DIR / 'launcher' / 'bloodborne.ico'
        script = ('$s=(New-Object -ComObject WScript.Shell).CreateShortcut([Environment]::GetFolderPath("Desktop")'
                  '+"\\Bloodborne.lnk");'
                  f'$s.TargetPath="{target}";$s.WorkingDirectory="{PORT_DIR}";$s.IconLocation="{icon}";'
                  + (f"$s.Arguments='\"{arguments}\"';" if arguments else '') + '$s.Save()')
        result = subprocess.run(['powershell', '-NoProfile', '-Command', script], capture_output=True,
                                creationflags=NO_WINDOW)
        if result.returncode == 0:
            self.messagebox.showinfo('Bloodborne', _('Shortcut created on the desktop.', 'Ярлык создан на рабочем столе.'))
        else:
            self.messagebox.showerror('Bloodborne', result.stderr.decode(errors='replace')[:400])

    def add_to_steam(self):
        """Bloodborne in the Steam library (a non-Steam game that starts Play Bloodborne.exe). Steam has to close
        for it: the wait for that (up to half a minute) and the writing run in a helper thread, as the window froze
        while it waited on the Tk thread. The window closed meanwhile only hides until it is done (close)."""
        if getattr(self, 'steam_adding', False):  # a second click while the first one waits for Steam
            self.messagebox.showinfo('Bloodborne', _('Bloodborne is still being added to Steam: Steam closes for it '
                                                     'first, which can take up to half a minute.'))
            return
        import bbport_steam
        steam = bbport_steam.steam_folder()
        configs = bbport_steam.user_configs(steam) if steam else []
        if not configs:
            self.messagebox.showerror('Bloodborne', _('Steam was not found on this PC (or nobody has signed in to it yet).'))
            return
        if FROZEN:
            exe, options = PORT_DIR / 'Play Bloodborne.exe', ''
        else:
            pythonw = Path(sys.executable).with_name('pythonw.exe')
            exe, options = (pythonw if pythonw.exists() else Path(sys.executable)), f'"{Path(__file__).resolve()}" --play'
        self.collect()  # Play Bloodborne.exe starts the game with the saved settings
        running = bbport_steam.steam_running()
        if running and not self.messagebox.askyesno('Bloodborne', _('Steam has to close while the game is added; it '
                                                                    'starts again afterwards. Close Steam now?')):
            return
        self.steam_adding = True
        problem = None

        def done():
            self.steam_adding = False
            closing = self.__dict__.pop('close_after_steam', False)  # the window was closed while it waited
            if problem:
                self.messagebox.showerror('Bloodborne', problem)
            else:
                if running:
                    os.startfile('steam://open/games')
                if not closing:  # closed, Steam's library with the game is the answer
                    self.messagebox.showinfo('Bloodborne', _('Bloodborne is in your Steam library. It starts with '
                                                             'the settings saved in this launcher.'))
            if closing:
                self.close()

        def work():
            nonlocal problem
            try:
                if running:
                    subprocess.Popen([str(steam / 'steam.exe'), '-shutdown'], creationflags=NO_WINDOW)
                    for _i in range(60):
                        time.sleep(0.5)
                        if not bbport_steam.steam_running():
                            break
                    else:
                        problem = _('Steam did not close. Close it yourself and try again.')
                        return
                    time.sleep(1)  # Steam writes its files as it exits
                for config in configs:
                    bbport_steam.add_shortcut(config / 'shortcuts.vdf', 'Bloodborne', str(exe), str(PORT_DIR),
                                              str(exe), options)
            except Exception as error:  # the button always answers
                problem = _('Could not add the game to Steam: {}').format(error_text(error))
            finally:
                self.ui_calls.put(done)
        threading.Thread(target=work, daemon=True).start()

    def close(self):
        if self.test_running():  # a game test: its game must not stay behind
            if not self.messagebox.askyesno('Bloodborne', _('A game test is running. Stop it and close the launcher?')):
                return
            self.bench.cancelled = True
            subprocess.run(['taskkill', '/IM', 'bb-probe.exe', '/F'], capture_output=True, creationflags=NO_WINDOW)
        try:
            self.collect()
        except Exception:  # never keep the window open over a settings problem
            pass
        if getattr(self, 'steam_adding', False):
            # Add to Steam waits for Steam to close or writes shortcuts.vdf: ended now, Steam stayed closed and a
            # cut-off shortcuts.vdf lost every non-Steam game in it. Hidden, the launcher closes once it is done.
            self.close_after_steam = True
            self.root.withdraw()
            return
        if self.process:
            # The game's output still comes through this process (Log page, last_run.log). An
            # exited launcher closed that pipe, and the next line a preparation script or the
            # in-game restart printed failed (OSError: [Errno 22] Invalid argument): the game
            # never started. Hidden instead, it exits with the game.
            self.hidden = True
            self.root.withdraw()
            return
        self.drop_tray()
        self.root.destroy()


class GameJob:
    """A Windows job holding run.py and everything it starts: bb-probe.exe and the launches made
    by the in-game restart (no longer descendants of the first process)."""

    def __init__(self, process):
        kernel32 = ctypes.windll.kernel32
        kernel32.CreateJobObjectW.restype = ctypes.c_void_p
        self.handle = kernel32.CreateJobObjectW(None, None)
        if self.handle:
            kernel32.AssignProcessToJobObject(ctypes.c_void_p(self.handle), ctypes.c_void_p(int(process._handle)))

    def terminate(self):
        if self.handle:
            ctypes.windll.kernel32.TerminateJobObject(ctypes.c_void_p(self.handle), 1)

    def close(self):
        if self.handle:
            ctypes.windll.kernel32.CloseHandle(ctypes.c_void_p(self.handle))
            self.handle = None


class RunLog:
    """<saves folder>/last_run.log as read_output writes it. A write that fails (a full drive) ends the logging, not
    the reading: the reader thread died with it, the game's output pipe filled up and the game froze, with the
    launcher saying "running" and Stop doing nothing. `note(text)` says it once (on the Log page)."""

    def __init__(self, path, note, opener=open):
        self.path, self.note, self.file = Path(path), note, None
        try:
            self.path.parent.mkdir(parents=True, exist_ok=True)
            self.file = opener(self.path, 'w', encoding='utf-8', buffering=1)
        except Exception as error:
            self.failed(error)

    def write(self, text):
        if self.file is None:
            return
        try:
            self.file.write(text)
        except Exception as error:
            self.failed(error)

    def failed(self, error):
        self.close()
        self.note(_("{0} could not be written ({1}): the rest of this game's log is only on this page.").format(
            self.path, error_text(error)) + '\n')

    def close(self):
        file, self.file = self.file, None
        if file is not None:
            try:
                file.close()  # its last line may still fail to reach a full drive: nothing more to do
            except Exception:
                pass


def folder_entries(folder, names=None):
    """(name, modified) of a folder's entries (only `names`, when given): what a list drawn from it
    depends on; () without the folder."""
    try:
        return tuple(sorted((e.name, e.stat().st_mtime_ns) for e in os.scandir(folder)
                            if names is None or e.name in names))
    except OSError:
        return ()


def picture_of(widget):
    """What a widget's window shows on the screen now, as a PIL image (None where it cannot be had)."""
    if os.name != 'nt':
        return None
    try:
        return bbport_frame.picture(widget.winfo_id(), widget.winfo_width(), widget.winfo_height())
    except (AttributeError, OSError, ValueError, ImportError, ctypes.ArgumentError):
        return None


def remember_window(size):
    """Writes the game window's last size into settings.json (and nothing else)."""
    settings = load_settings()
    if not settings:  # never the size alone over settings that could not be read
        return
    settings['window_last'] = display.size_text(size)
    try:
        write_settings(settings)
    except OSError:
        pass


# OpenSSL's words for a certificate outside its dates, which is what every certificate looks like to a PC whose
# clock is wrong.
CERTIFICATE_DATES = re.compile(r'certificate (?:has expired|is not yet valid)', re.I)
# Windows' own connection check answers plain HTTP with its time, where a server does not (api.github.com).
TIME_HOST = 'www.msftconnecttest.com'


def clock_off(host):
    """Seconds this PC's clock is off, by the Date of a plain HTTP answer from `host` (no certificate to check);
    None when it cannot be asked."""
    import email.utils
    import http.client
    try:
        connection = http.client.HTTPConnection(host, 80, timeout=6)
        try:
            connection.request('HEAD', '/', headers={'User-Agent': f'bbport-launcher/{VERSION}'})
            date = connection.getresponse().getheader('Date')
        finally:
            connection.close()
        return time.time() - email.utils.parsedate_to_datetime(date).timestamp()
    except (OSError, ValueError, TypeError, http.client.HTTPException):
        return None


def connection_error(error, url):
    """The words for a failed HTTPS call: "Your PC's date is wrong" when the certificate seemed expired or not yet
    valid and this PC's clock is more than a day off (the one thing to do: set it); else the error itself. Run off
    the Tk thread: it may ask the server's time."""
    if CERTIFICATE_DATES.search(str(error)):
        import urllib.parse
        off = clock_off(urllib.parse.urlsplit(url).hostname or '')
        if off is None:
            off = clock_off(TIME_HOST)
        if off is not None and abs(off) > 86400:
            return _("Your PC's date is wrong ({}), so secure connections fail. Set the date in Windows: Settings > "
                     'Time & language > Date & time, "Set time automatically".',
                     'Дата на вашем ПК неверна ({}), поэтому защищённые соединения не работают. Установите дату в '
                     'Windows: Параметры > Время и язык > Дата и время, «Установить время автоматически».').format(
                time.strftime('%d.%m.%Y'))
    return error_text(error)


def error_text(error, kind=False):
    """An error a helper thread caught, in a few words: its own text, with its kind only where the text alone says
    little (BadStatusLine('') reads "''", KeyError('tag_name') "'tag_name'", http.client's other errors a bare piece
    of the reply) or where `kind` asks for it (a Log page line about an error nobody expected); never empty. A text
    that says what happened stays as it is: "FetchError: " or "BadZipFile: " in front of it read worse to players."""
    import http.client
    name, text = type(error).__name__, str(error).strip()
    if not text or text in ("''", '""'):
        return name
    if text.startswith(name):
        return text
    quoted = len(text) > 1 and text[0] == text[-1] and text[0] in '\'"'
    bare_reply = isinstance(error, http.client.HTTPException) and not isinstance(error, OSError)
    return f'{name}: {text}' if kind or quoted or bare_reply else text


# What the game logs when The Hunter's Dream refuses the saved sign-in (gpu/bbnet/thd/thd_session.cpp MarkRefused).
SIGN_IN_REFUSED = "refused this PC's sign-in"


def sign_in_refused(settings):
    """Whether the last game's log says The Hunter's Dream refused the saved sign-in (a revoked or expired token).
    The game goes on offline and said so only there; the launcher still showed it signed in."""
    try:
        text = (saves_folder(settings) / 'last_run.log').read_text(encoding='utf-8', errors='replace')
    except OSError:
        return False
    return SIGN_IN_REFUSED in text[-400_000:]


def forget_sign_in():
    """The refused sign-in out of settings.json (nothing else changed): the Online page shows "Not signed in"
    and the next Play asks to sign in."""
    settings = load_settings()
    if settings.get('thd_token'):
        settings['thd_token'] = ''
        write_settings(settings)


def needs_sign_in(settings):
    """Online with The Hunter's Dream or SuperMedo Server without its account: run.py would stop the start, or the
    game would stay offline without a word. A start from Steam or a shortcut opens the launcher, whose Play asks."""
    if not settings.get('online') or pool_account(settings):
        return False
    return chosen_pool(settings) in ('supermedo', 'huntersdream')


def custom_output(size, settings):
    """The output size of the player's own to remember (settings.json output_custom): one the game's menu does
    not know. gpu/shim/bbport_settings.cpp has ten; its first save in the menu writes 1920x1080 instead (the
    proper fix is there: keep the size it read). '' for a listed size or the monitor's own; at 1920x1080 the
    remembered one stays, since that is what the menu writes."""
    if settings.get('output_display'):
        return ''
    if size not in {value for value, _t in OUTPUTS} and display.parse_size(size):
        return size
    return settings.get('output_custom', '') if size == INI_DEFAULTS['output_res'] else ''


def own_output(settings, size):
    """The player's own output size to set again over the game menu's 1920x1080, or None."""
    custom = settings.get('output_custom')
    if custom and size == INI_DEFAULTS['output_res'] and not settings.get('output_display') and \
            display.parse_size(custom):
        return custom
    return None


def has_complete_save(user_dir, game_dir):
    """Whether the game finds a save to load: the profile and a character where it reads them."""
    try:
        return saves.SaveSet('', saves.target_dir(user_dir, game_dir)).complete
    except OSError:
        return False


def misplaced_saves(settings):
    """Before every start: a save put in the saves folder where the game does not read it is copied to where it
    does (bbport_saves.adopt_misplaced). (A line for the log or None, the folder of an encrypted PS4 save not
    named yet or None.)"""
    user, game = saves_folder(settings), settings.get('game_dir', '')
    try:
        adopted, encrypted = saves.adopt_misplaced(user, game)
    except OSError as error:
        return _('Saves: a save in {} could not be copied to where the game reads it: {}',
                 'Сохранения: не удалось скопировать сохранение из {} туда, где игра его читает: {}').format(
            user, error), None
    if adopted:
        return _('Saves: {} copied to where the game reads them ({}); the original stays where it was.',
                 'Сохранения: {} скопированы туда, где игра их читает ({}); оригинал остался на месте.').format(
            adopted.folder, saves.target_dir(user, game)), None
    folder = str(encrypted[0].parent) if encrypted else None
    return None, folder if folder and folder != settings.get('encrypted_told') else None


def saves_in_temporary_folder(settings):
    """The port runs from an archive program's temporary folder (bbport_help.temporary_folder) and its saves are in
    there too (not in a saves folder of the player's own elsewhere)."""
    return bbport_help.temporary_folder(PORT_DIR) and \
        bbport_help.temporary_folder(saves_folder(settings))


def start_problem(settings):
    """(message, page) for what keeps the game from starting and cannot be repaired here, checked before every
    start (Play, Play Bloodborne.exe); None when nothing does."""
    problem = bbport_help.system_problem(settings.get('game_dir'))
    if problem:
        return problem, 'game'
    problem = missing_saves_folder(settings)
    return (problem, 'saves') if problem else None


def missing_saves_folder(settings):
    """The message for a saves folder of the player's own on a drive that is not connected, or renamed: an empty
    one made in its place would show no characters, and the game would start new ones there. None when it is there
    or none is set (this port's own, user/, is made at the start)."""
    user = str(settings.get('user_dir') or '').strip()
    if user and not Path(user).is_dir():
        return _('The saves folder {} is not there: its drive is not connected, or the folder was renamed or moved. '
                 'Connect the drive, or choose the saves folder again.',
                 'Папки сохранений {} нет: её диск не подключён, или папку переименовали или переместили. Подключите '
                 'диск или выберите папку сохранений заново.').format(user)
    return None


def play_without_window(settings):
    """--play: the game with the saved settings (shortcuts, Steam). Output goes to the console
    when there is one and to <saves folder>/last_run.log."""
    attach_stdio()
    if game_underway():
        # A second click while the first is being prepared, or Steam while the launcher's game runs: that game's
        # window comes to the front instead of a second game on the same saves and prepared files.
        front_game_window()
        print('Play: a game is being prepared or runs already; its window was brought to the front')
        return 0
    values, lines = load_ini(settings)
    graphics = values
    # The engine reads the same INI even when this shortcut skips the GUI.
    present = {line.split('=', 1)[0].strip() for line in lines
               if '=' in line and not line.lstrip().startswith('#')}
    missing = {key: values[key] for key in ('upscaler', 'fsr411_backend') if key not in present}
    # A read-only or locked bbport.ini stopped the start with a traceback: the game starts with what the file
    # holds, and the log says what could not be written.
    if missing:
        try:
            save_ini(missing, lines)
            lines = ini_path().read_text(encoding='utf-8').splitlines()
        except OSError as error:
            ini_not_written(error, missing)
    if settings.get('output_display'):  # the game monitor's own size, as the launcher's Play does
        m = display.monitor(settings.get('game_monitor', ''))
        if m and values.get('output_res') != display.size_text(m.size):
            values['output_res'] = display.size_text(m.size)
            try:
                save_ini({key: values[key] for key in INI_DEFAULTS}, lines)
            except OSError as error:
                ini_not_written(error, {'output_res': values['output_res']})
    own = own_output(settings, values.get('output_res'))
    if own:  # the player's own size, which the game's menu wrote as 1920x1080 (custom_output)
        values['output_res'] = own
        try:
            save_ini({key: values[key] for key in INI_DEFAULTS}, lines)
            textfile.REPAIRS.append(f"Output: {own}, the player's own size, set again (the game's menu had "
                                    f"written {INI_DEFAULTS['output_res']})")
        except OSError:
            pass
    line, encrypted = misplaced_saves(settings)
    notes = textfile.REPAIRS + ([line] if line else [])
    if encrypted:
        notes.append(f'Saves: {encrypted} holds an encrypted PS4 save (sdimg_): it cannot be used; export it with '
                     'Apollo Save Tool on the PS4')
    offline = signed_out_server(settings)
    if offline:
        # A shortcut or Steam has no Play button to ask for the sign-in: online without the chosen server's
        # account, the game was signed out and said it can't connect to the Bloodborne network (30.10.2026).
        # This start plays offline, as Play does when the player says No.
        settings = {**settings, 'online': False}
        notes.append(f"Online: not signed in to {offline}, so this start plays offline; sign in on the launcher's "
                     'Online play page to play online')
    if settings.get('auto_backup', True):
        try:
            saves.auto_backup(Path(settings.get('user_dir') or DATA_DIR / 'user'), settings['game_dir'], notes=notes)
        except OSError:
            pass  # never in the way of a start
    placer = display.Placer(settings, display.parse_size(values.get('output_res')) or (1920, 1080)).start()
    if graphics.get('upscaler') in ('fsr411', 'fsr412'):
        env = bbport_fsr411.environment(PORT_DIR, settings)
        support = bbport_fsr41.query(PORT_DIR, env, refresh=True)
        status = (bbport_fsr412.method_status(graphics['fsr411_backend'], support)
            if graphics['upscaler'] == 'fsr412' else
            bbport_fsr411.method_status(PORT_DIR, DATA_DIR, graphics['fsr411_backend'],
                support, fetch_fsr4vk, env))
        if not status['supported']:
            print('FSR 4.1: ' + _('FSR 4.1 unavailable') +
                  ' (' + status['reason'] + ': ' + status.get('detail', '') + ')')
            return None  # Show the launcher controls instead of a popup.
    log_dir = Path(settings.get('user_dir') or DATA_DIR / 'user')
    log_dir.mkdir(parents=True, exist_ok=True)
    tracker = trophies.Tracker(log_dir)
    with open(log_dir / 'last_run.log', 'w', encoding='utf-8', buffering=1) as log:
        log.writelines(note + '\n' for note in notes)
        process = subprocess.Popen(run_command(), cwd=PORT_DIR, env=game_environment(settings, graphics),
                                   stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                   stderr=subprocess.STDOUT, creationflags=NO_WINDOW)
        for raw in iter(process.stdout.readline, b''):
            text = raw.decode('utf-8', errors='replace')
            log.write(text)
            if 'Runtime: trophy ' in text:
                tracker.observe(text)
            try:
                sys.stdout.write(text)
            except (OSError, ValueError):
                pass
        code = process.wait()
    if settings.get('thd_token') and sign_in_refused(settings):
        try:
            forget_sign_in()
            with open(log_dir / 'last_run.log', 'a', encoding='utf-8') as log:
                log.write("Online: The Hunter's Dream refused the saved sign-in; it is forgotten, the next start "
                          'asks to sign in\n')
        except OSError:
            pass
    placer.stop()
    if placer.last and not settings.get('fullscreen'):
        remember_window(placer.last)
    return code


def version_tuple(text):
    return tuple(int(number) for number in re.findall(r'\d+', text or ''))


def latest_release():
    """(version, zip URL, page URL) of the newest GitHub release."""
    request = urllib.request.Request(RELEASES_API, headers={'Accept': 'application/vnd.github+json',
                                                            'User-Agent': 'bbport-launcher'})
    with bbport_tls.urlopen(request, timeout=15) as response:
        release = json.load(response)
    version = '.'.join(re.findall(r'\d+', release['tag_name']))
    return version, game_zip(release), release.get('html_url') or RELEASES_PAGE


def newer_beta():
    """(label, zip URL, page URL) of the newest published beta after this one: a later beta of this
    version or a beta of a later one. None for a release build, and when there is none."""
    if not BETA:
        return None
    request = urllib.request.Request(BETAS_API, headers={'Accept': 'application/vnd.github+json',
                                                         'User-Agent': 'bbport-launcher'})
    with bbport_tls.urlopen(request, timeout=15) as response:
        releases = json.load(response)
    best = None
    for release in releases:
        found = re.fullmatch(r'v(\d+(?:\.\d+)*)-beta\.(\d+)', release.get('tag_name', ''))
        if release.get('draft') or not found:
            continue
        key = (version_tuple(found[1]), int(found[2]))
        url = game_zip(release)
        if url and key > (version_tuple(VERSION), BETA) and (best is None or key > best[0]):
            best = (key, f'{found[1]} beta {found[2]}', url, release.get('html_url') or BETA_PAGE)
    return best[1:] if best else None


def update_to_offer():
    """(version, zip URL, page URL) the launcher offers, or None. A newer release; for a beta also its
    own version's release (the beta's goal), else the next beta, so testers move from beta to beta
    and on to the release without downloading by hand. Raises the first error when no check answered (any
    kind: a broken reply from a proxy is no OSError, and a beta still asks for the next beta then)."""
    failure = None
    try:
        version, url, page = latest_release()
        newer = version_tuple(version) > version_tuple(VERSION)
        if newer or (BETA and version_tuple(version) == version_tuple(VERSION)):
            return version, url, page
    except Exception as error:
        failure = error
    if BETA:
        try:
            return newer_beta()
        except Exception as error:
            failure = failure or error
    if failure:
        raise failure
    return None


def game_zip(release):
    """The download URL of a release's game package: Bloodborne-Windows.zip, else its one .zip that
    is not a source archive. None when it cannot tell (the release page opens instead). Up to 1.9.5
    beta 2 the launcher took the first .zip: with the FSR 4 source archive uploaded first, an update
    downloaded that and stopped ("Bloodborne.exe is missing from the download")."""
    zips = [asset for asset in release.get('assets', []) if asset.get('name', '').lower().endswith('.zip')]
    named = [asset for asset in zips if asset['name'].lower() == 'bloodborne-windows.zip']
    games = named or [asset for asset in zips
                      if not re.search(r'(^|[-_.])(src|source|sources)([-_.]|$)', asset['name'].lower())]
    if len(games) != 1:
        return None
    if games[0].get('digest'):  # GitHub lists "sha256:…" for its assets: install_version checks the download
        ASSET_DIGESTS[games[0]['browser_download_url']] = games[0]['digest']
    return games[0]['browser_download_url']


def installable_releases():
    """[(version, zip URL, page URL)] of the published releases from OLDEST_INSTALLABLE on, newest
    first (drafts, pre-releases and releases without a zip left out)."""
    request = urllib.request.Request(RELEASES_LIST_API, headers={'Accept': 'application/vnd.github+json',
                                                                 'User-Agent': 'bbport-launcher'})
    with bbport_tls.urlopen(request, timeout=15) as response:
        releases = json.load(response)
    found = {}
    for release in releases:
        tag = release.get('tag_name', '')
        if release.get('draft') or release.get('prerelease') or not tag.startswith('windows-v'):
            continue
        version = '.'.join(re.findall(r'\d+', tag))
        if not version or version_tuple(version) < version_tuple(OLDEST_INSTALLABLE):
            continue
        url = game_zip(release)
        if url and version not in found:
            found[version] = (version, url, release.get('html_url') or RELEASES_PAGE)
    return sorted(found.values(), key=lambda item: version_tuple(item[0]), reverse=True)


def update_installer(folder):
    """The program that puts a downloaded version in place: a copy of this launcher in the update's folder, for every
    version. Not the new version's own launcher: one built without bbport_update (an older release, or one from a
    branch without it) copies itself over the installation file by file and never takes the update mutex, so this
    launcher would wait for it in vain."""
    internal = Path(getattr(sys, '_MEIPASS', PORT_DIR / '_internal'))  # the frozen Python beside Bloodborne.exe
    own = folder / 'installer'
    shutil.copytree(internal, own / internal.name)
    shutil.copy2(sys.executable, own / 'Bloodborne.exe')
    return own / 'Bloodborne.exe'


def progress_window(heading, work, icon=None):
    """Runs work(say) on a helper thread under a small window: the heading and the line say(text) sets. It cannot be
    closed (that would stop a swap halfway), and a start during the work brings it to the front (updater.to_front).
    Returns (what work returned, None) or (None, what it raised, whatever it was); where Tk cannot show the window
    the work runs all the same, without it."""
    outcome, lines, fronts = [], queue.Queue(), queue.Queue()

    def run():
        try:
            outcome.append((work(lines.put), None))
        except BaseException as failure:  # the window closes and the installer says it
            outcome.append((None, failure))

    def without_window():
        run()
        return outcome[0]
    try:
        import tkinter as tk
        root = tk.Tk()
    except Exception:
        return without_window()
    try:
        root.title('Bloodborne')
        root.configure(bg=BG)
        root.resizable(False, False)
        root.protocol('WM_DELETE_WINDOW', lambda: None)
        try:
            root.iconbitmap(default=str(icon or PORT_DIR / 'launcher' / 'bloodborne.ico'))
        except tk.TclError:
            pass
        tk.Label(root, text=heading, bg=BG, fg=GOLD, font=(theme.BODY_SEMI, 12)).pack(anchor='w', padx=28,
                                                                                       pady=(22, 6))
        line = tk.Label(root, text='', bg=BG, fg=MUTED, font=(theme.BODY, 10), width=52, anchor='w', justify='left',
                        wraplength=440)
        line.pack(anchor='w', padx=28, pady=(0, 24))
        root.update_idletasks()
        root.geometry(f'+{(root.winfo_screenwidth() - root.winfo_reqwidth()) // 2}'
                      f'+{(root.winfo_screenheight() - root.winfo_reqheight()) // 3}')
        root.lift()
    except Exception:  # a window Tk cannot draw: the update goes on without it
        try:
            root.destroy()
        except Exception:
            pass
        return without_window()
    event = bring_to_front_on_request(lambda: fronts.put(True), updater.UPDATE_FRONT)  # noqa: F841 (kept open)
    worker = threading.Thread(target=run, daemon=True)
    worker.start()

    def front():
        root.deiconify()
        root.lift()
        root.attributes('-topmost', True)
        root.after(500, lambda: root.attributes('-topmost', False))
        root.focus_force()

    def poll():
        try:
            while not lines.empty():
                line.configure(text=lines.get_nowait())
            while not fronts.empty():
                fronts.get_nowait()
                front()
        except Exception:  # only the window shows less: the work goes on
            pass
        if outcome:
            root.destroy()
        else:
            root.after(100, poll)
    root.after(100, poll)
    root.mainloop()
    worker.join()
    return outcome[0] if outcome else (None, None)


def install_update(target, wait_pid):
    """--install-update TARGET PID: run by a copy of the launcher in the update's folder in %TEMP% (BB_UPDATE_FROM
    names the new files, BB_UPDATE_FILES what they must be; 1.9.6 and older start the new version itself, without
    them). Holds the update mutex, waits in a small window for the old launcher and every program started from
    TARGET, puts the new version in place by renaming (bbport_update.install), then starts TARGET's launcher: the new
    one, or the old one again when the update was undone. What happened goes into that start's log
    (BB_UPDATE_RESULT)."""
    target = Path(target)
    source = Path(os.environ.get('BB_UPDATE_FROM') or PORT_DIR)
    version = os.environ.get('BB_UPDATE_VERSION') or VERSION_TEXT
    listed = updater.read_listed(os.environ.get('BB_UPDATE_FILES'))
    launcher = updater.open_process(wait_pid)  # before the mutex: the old launcher waits for it, so it is still there
    if not updater.hold_mutex():
        updater.to_front()  # the installer that runs already shows its window
        return 1
    try:
        ctypes.windll.shcore.SetProcessDpiAwareness(1)
    except (AttributeError, OSError):
        pass
    notes, failure = progress_window(_('Installing version {}…').format(version), lambda say: updater.install(
        source, target, launcher, int(wait_pid), say, USER_FILES, listed=listed),
        icon=source / 'launcher' / 'bloodborne.ico')
    updater.release_mutex()  # the launcher started next clears what is left
    lines = list(notes or [])
    if isinstance(failure, updater.RollbackFailed):
        # Not started: its files may be half swapped. The next start puts them right (bbport_update.finish).
        updater.notice(_('Version {0} could not be installed, and the installed version could not be put back '
                         'completely ({1}).\n\nDownload the port again from the releases page, which opens now. '
                         'Saves, settings and mods are not affected.').format(version, failure), error=True)
        webbrowser.open(RELEASES_PAGE)
        return 1
    if failure:
        reason = str(failure) or type(failure).__name__
        lines.append(f'Update: version {version} was not installed, the installed version was kept ({reason})')
        updater.notice(_('Version {0} could not be installed: {1}.\n\nThe installed version was kept and works as '
                         'before.').format(version, reason), error=True)
    else:
        lines.append(f'Update: version {version} installed')
    env = {key: value for key, value in os.environ.items() if not key.upper().startswith('BB_UPDATE_')}
    env['BB_UPDATE_RESULT'] = '\n'.join(lines)
    subprocess.Popen([str(target / 'Bloodborne.exe')], cwd=str(target), env=env)
    return 1 if failure else 0


class _ThisModule:
    """bbport_setup's view of this module: its globals when used (the tests load this file under
    another name, and they and the preview tools change PORT_DIR and others after the import)."""

    def __getattr__(self, name):
        try:
            return globals()[name]
        except KeyError:
            raise AttributeError(name) from None


bbport_setup.bind(_ThisModule())
bbport_news.bind(_ThisModule())
bbport_help.bind(_ThisModule())
bbport_search.bind(_ThisModule())
bbport_pad.bind(_ThisModule())
bbport_tls.bind(_ThisModule())
updater.bind(_ThisModule())


def restore_benchmark_backup(user):
    """The route benchmark of the 1.9.3 test builds left its own save in place of the player's when it did not
    finish (closed launcher, crash): the player's comes back. The savedata in place used to be deleted first: a
    file in use (a sync tool, an antivirus) left part of it there, and the player's could not come back. It is
    renamed aside now (bbport_saves.BENCHMARK_SAVE: kept, never deleted) before the player's is renamed back, and
    goes back itself when that fails. Either way the log of the next start says so."""
    backup, live = user / 'savedata.benchmark-backup', user / 'savedata'
    try:
        if not backup.is_dir():
            return
    except OSError:  # a saves folder that cannot be reached now (a network drive): nothing to put back this start
        return
    aside, moved = None, False
    try:
        if live.exists():
            aside = user / saves.BENCHMARK_SAVE
            if aside.exists():
                aside = user / f'{saves.BENCHMARK_SAVE} {time.strftime("%Y-%m-%d %H.%M.%S")}'
            live.rename(aside)
            moved = True
        backup.rename(live)
    except OSError as error:
        line = (f'Saves: the saves from before an unfinished game test could not be put back ({error}); they stay '
                f'in {backup}, the next start tries again')
        if moved:  # the rename of the player's failed, so savedata is free again
            try:
                aside.rename(live)
            except OSError as second:
                line += f'; the saves that were in use could not go back either ({second}), they are in {aside}'
        textfile.REPAIRS.append(line)
        return
    textfile.REPAIRS.append(f'Saves: the saves from before an unfinished game test are back in {live}'
                            + (f"; the test's save is kept in {aside}" if aside is not None else ''))


def main():
    global LANG
    args = sys.argv[1:]
    if args and args[0] in ('--run', '--script'):
        sys.exit(run_role(args))
    if args[:1] == ['--after'] and len(args) >= 2:
        wait_for_exit(args[1])
        args = args[2:]
    saved = load_settings()
    settings = {**APP_DEFAULTS, **saved}
    settings.update(settled_online(settings, saved))  # also for --play: Steam and shortcuts skip the window
    LANG = settings.get('ui_language') or windows_language()
    saves.WORDS.update({key: _(text) for key, text in saves.REASONS.items()})  # backup file names
    if args[:1] == ['--install-update'] and len(args) == 3:
        sys.exit(install_update(args[1], args[2]))
    # An update is being installed: its files are being swapped, and a launcher or a game started from them would
    # hold them (the installer would wait for it, or undo the swap). The installer's window comes to the front and
    # this start exits at once; without that window, a note that closes by itself. The launcher opens by itself.
    if updater.underway():
        if not updater.to_front():
            updater.notice(_('An update is being installed. The launcher opens by itself when it is done.'), seconds=4)
        sys.exit(0)
    restore_benchmark_backup(Path(settings.get('user_dir') or DATA_DIR / 'user'))
    # Without a usable game folder there is nothing to play yet: open the launcher instead.
    page, failed, problem = 'play', None, None
    if '--play' in args and (Path(settings['game_dir'] or '.') / 'eboot.bin').is_file() and CONFIG_FILE.exists():
        # What cannot be repaired is said before the start, in the launcher's window (there is none otherwise).
        problem = start_problem(settings)
        if problem:
            page = problem[1]
        elif needs_sign_in(settings):
            page = 'sign in'  # its Play asks: sign in now, or play offline this time
        elif saves_in_temporary_folder(settings):
            pass  # the launcher opens and warns (temporary_warning); its Play starts the game anyway
        else:
            result = play_without_window(settings)
            if result == 0:
                sys.exit(0)
            # Ended with an error: the launcher opens on Help with what the log names, as after its own Play
            # (from Steam or a shortcut the game used to close without a word).
            page, failed = ('graphics', None) if result is None else ('help', result)
    try:
        ctypes.windll.shcore.SetProcessDpiAwareness(1)  # sharp text on scaled displays
    except (AttributeError, OSError):
        pass
    if not one_launcher():
        sys.exit(0)  # the open launcher comes to the front
    # What finished or failed updates left behind, only now: before, a second start deleted the files an update was
    # still copying from. clear_leftovers keeps them while an installer holds the update mutex, and finishes or
    # undoes a swap that stopped halfway. It and the installer's outcome go into the next game's log.
    if FROZEN:
        textfile.REPAIRS.extend(updater.clear_leftovers(PORT_DIR, UPDATE_DIR) or [])
    textfile.REPAIRS.extend(line for line in os.environ.pop('BB_UPDATE_RESULT', '').splitlines() if line.strip())
    import tkinter as tk
    from tkinter import filedialog, messagebox, ttk
    root = tk.Tk()
    launcher = Launcher(root, tk, ttk, filedialog, messagebox)
    root.report_callback_exception = launcher.callback_error  # never invisible (a test keeps its own)
    bring_to_front_on_request(lambda: launcher.ui_calls.put(launcher.bring_back))
    if page == 'sign in':
        root.after(500, launcher.play)
    elif page != 'play':
        launcher.show(page)
    if failed:
        root.after(500, lambda: launcher.after_exit(failed, always=True))
    if problem:
        root.after(500, lambda: messagebox.showerror('Bloodborne', problem[0]))
    root.mainloop()


if __name__ == '__main__':
    main()
