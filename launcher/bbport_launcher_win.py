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
VERSION = '1.9.4'
# Co-op: the community server that answers the game's own server requests.
COMMUNITY_SERVER = 'https://thehuntersdream.com'
RELEASES_API = 'https://api.github.com/repos/Supermedo/bloodborne_pc/releases/latest'
RELEASES_PAGE = 'https://github.com/Supermedo/bloodborne_pc/releases/latest'
# The community site (install guide, FAQ, forum), linked from the side bar.
COMMUNITY_SITE = 'https://bbport.de/'
RELEASES_LIST_API = 'https://api.github.com/repos/Supermedo/bloodborne_pc/releases?per_page=100'
# The oldest version Advanced > Other versions offers: older ones cannot install themselves the
# same way (--install-update) or read today's settings.
OLDEST_INSTALLABLE = '1.8.2'
UPDATE_DIR = Path(tempfile.gettempdir()) / 'bbport-update'
# Never copied over an installation by an update (the package does not hold them either).
USER_FILES = ('user', 'out', 'mods', 'fsr4vk', 'bbport.ini', 'input.ini', 'mods.json', 'patches.json', 'last_run.log')


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
import bbport_character as character  # noqa: E402
import bbport_tray  # noqa: E402
import bbport_lang_setup  # noqa: E402,F401 (the setup guide's texts)
import bbport_lang_help  # noqa: E402,F401 (the Help page's and the game tests' texts)
import bbport_lang_ui  # noqa: E402,F401 (the regrouped pages, the side bar, the guide)
import bbport_lang_world  # noqa: E402,F401 (the new texts in the other eleven languages)
import bbport_lang_port  # noqa: E402,F401 (the 1.9.3 texts and the Controls page's key names)
import bbport_hardware as hardware  # noqa: E402
import bbport_frame  # noqa: E402
import bbport_display as display  # noqa: E402
import bbport_saves as saves  # noqa: E402
import bbport_pad  # noqa: E402
import bbport_help  # noqa: E402
import bbport_search  # noqa: E402
import bbport_setup  # noqa: E402
import bbport_trophies as trophies  # noqa: E402
import bbport_theme as theme  # noqa: E402
from bbport_input_config import (STICK_OUTPUTS, OUTPUT_LABELS, ACTION_GROUPS, SLOTS, keysym_to_name,
                                  button_num_to_name, input_ini_path, load_input_ini, reserved_keys,
                                  save_input_ini, save_mouse_settings, wheel_to_name, MAX_COMBO,
                                  MODIFIER_NAMES, apply_layout, slot_conflicts, binding_parts,
                                  display_name, CONTROLLER_OUTPUTS, DEFAULT_CONTROLLER,
                                  controller_conflicts, controller_display_name,
                                  load_controller_bindings, save_controller_bindings,
                                  keyboard_layout)  # noqa: E402
import bbport_gamepad  # noqa: E402
import bbport_respec as respec  # noqa: E402

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
    ('rebirth', ("Rebirth at the Doll: back to your origin's attributes for 5 Insight, levels regained free",
                 'Перерождение у Куклы: характеристики происхождения за 5 озарений, уровни возвращаются бесплатно'), True),
]
# Effects that are taste, not performance: on Picture & performance (the others on All graphics).
TASTE_EFFECTS = ('effect_chromatic_aberration', 'effect_dof', 'effect_motion_blur')
INI_FLAGS = {'sharpen', 'object_motion', 'object_motion_cache', 'show_fps', 'mute_unfocused', 'frame_gen_fit_display',
             *(k for k, _t, _o in EFFECTS + EXTRAS + CHEATS + TWEAKS)}
INI_DEFAULTS = {'upscaler': 'fsr4', 'preset': '1', 'sharpen': '1', 'sharpness': '0.50',
                'object_motion': '1', 'show_fps': '1', 'output_res': '1920x1080', 'model_lod': '0',
                'fsr411_backend': 'vk', 'object_motion_cache': '0',
                'live_resolution': 'auto', 'hud_position': '100', 'fov': '43', 'mute_unfocused': '0',
                'button_prompts': 'auto',
                'frame_gen': 'off', 'frame_gen_multiplier': '2', 'frame_gen_fit_display': '0',
                **{key: '1' if on else '0' for key, _t, on in EFFECTS + EXTRAS + CHEATS + TWEAKS}}
APP_DEFAULTS = {'ui_language': '', 'game_dir': str(PORT_DIR.parent / 'CUSA03173'), 'user_dir': '', 'cache_dir': '',
                'dlc_dir': '',
                'mods_dir': '', 'mods_enabled': True, 'patches_dir': '', 'language': '1',
                'voice_language': 'auto',
                'player_name': '', 'fullscreen': False, 'hdr': False, 'present_mode': 'Mailbox',
                'fps_mode': 'uncap', 'frame_cap': '', 'draw_pipe': '', 'readbacks': '', 'low_vram': '',
                'frames_ahead': '', 'frame_stats': False, 'gpu_profile': False,
                'vk_validation': False, 'extra_env': '', 'close_on_play': False,
                'on_play': 'stay', 'reopen_after_game': False, 'minimize_to_tray': False,
                'check_updates': True, 'save_backups': False, 'online': False, 'online_server': 'srv.shadps4.net:31313',
                'online_webapi': '', 'online_npid': '', 'online_password': '', 'online_upnp': True,
                'online_custom_servers': False,
                'online_community': COMMUNITY_SERVER, 'online_pool': 'huntersdream', 'bridge_live_map': False,
                'thd_online_id': '', 'thd_token': '',
                # The setup guide ran (or the launcher was set up before it existed); a graphics card
                # chosen by hand when the detected one is not in the hardware check's list.
                'setup_done': False, 'gpu_override': '',
                # Graphics settings made for the hardware (bbport_setup.auto_settings): on/off, the
                # hardware they were made for, and what they replaced (Undo).
                'auto_graphics': True, 'auto_hw': '', 'auto_undo': {},
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
                'seen_version': '',  # the version whose news the Play page has shown
                # Game tests (System & performance): the kind, the scenario, its options.
                'test_mode': 'performance', 'test_scenario': 'hunters-view', 'test_compare': 'preset',
                'test_repeats': '1', 'test_minutes': '10', 'test_saves': {}}  # the last check of eboot.bin (bbport_help), kept while the file is unchanged
# bbbridge's own app reads and writes the online_* keys above: keep their names.

UPSCALERS = [('dlss', ('DLSS (NVIDIA GeForce RTX)',)),
             ('fsr4', ('FSR 4 (best quality)', 'FSR 4 (лучшее качество)')),
             ('fsr411', ('FSR 4.1.1 (experimental)', 'FSR 4.1.1 (экспериментально)')),
             ('fsr3', ('FSR 3.1 (every GPU)', 'FSR 3.1 (любая видеокарта)')),
             ('taa', ('TAA (native resolution anti-aliasing)', 'TAA (нативное сглаживание)')),
             ('off', ('Off', 'Выключен'))]
# Frames generated between two of the game's (vk_frame_generation.cpp).
FRAME_GENERATION = [('off', ('Off', 'Выключена')),
                    ('fsr3', ('AMD FSR 3.1 Frame Generation (any GPU)', 'AMD FSR 3.1 (любая видеокарта)')),
                    ('fsr401', ('AMD ML-FG 4.0.1 (RX 9000, 2× SDR, experimental)',)),
                    ('dlss', ('NVIDIA DLSS Frame Generation (GeForce RTX 40/50)',
                              'NVIDIA DLSS (GeForce RTX 40/50)'))]
FRAME_GEN_MULTIPLIERS = [('2', ('2× (one generated frame)', '2× (один кадр)')),
                         ('3', ('3× (DLSS on GeForce RTX 50)', '3× (DLSS на GeForce RTX 50)')),
                         ('4', ('4× (DLSS on GeForce RTX 50)', '4× (DLSS на GeForce RTX 50)'))]
PRESETS = [('0', ('Native AA (×1.0)',)), ('1', ('Quality (×1.5)',)), ('2', ('Balanced (×1.7)',)),
           ('3', ('Performance (×2)',)), ('4', ('Ultra Performance (×3)',))]
FSR411_BACKENDS = [('vk', ('Vulkan (fsr4vk)',)), ('dx12', ('AMD DX12',))]
SHARED_GRAPHICS_KEYS = ('fsr411_backend', 'object_motion_cache')


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
             ('5', ('Italian', 'Итальянский')), ('17', ('Português (Brasil)',))]
VOICE_LANGUAGES = [('auto', ('As in the game',)), ('eng', ('English',)),
                   ('ptb', ('Português (Brasil)',)), ('ded', ('Deutsch',)),
                   ('esa', ('Español (América)',)), ('ese', ('Español (España)',)),
                   ('frf', ('Français',)), ('iti', ('Italiano',)), ('jaj', ('日本語',))]
DRAW_PIPE = [('', ('Auto (8+ threads)', 'Авто (8+ потоков)')), ('1', ('On', 'Включён')),
             ('0', ('Off (more stable)', 'Выключен (стабильнее)'))]
READBACKS = [('', ('Relaxed (default)', 'Relaxed (по умолчанию)')), ('0', ('Off', 'Выключены')),
             ('2', ('Precise',))]
# What the launcher does when the game starts (close_on_play of earlier launchers: 'close').
ON_PLAY = [('stay', ('Keep it open',)), ('minimize', ('Minimize it',)), ('close', ('Close it',))]
# BB_LOW_VRAM (run.py's low-VRAM profile): '' leaves it to the card's memory.
LOW_VRAM = [('', ('Auto (4 GB cards)', 'Авто (карты на 4 ГБ)')), ('1', ('On', 'Включён')),
            ('0', ('Off', 'Выключен'))]
# Frame cap of the unlocked mode (BB_FPS_LIMIT). '' leaves the port's own: the display refresh,
# at most 120, because the game's movement timing breaks above about 120 FPS.
FRAME_CAPS = [('', ('Auto: display refresh, max 120 (recommended)', 'Авто: частота монитора, макс. 120 (рекомендуется)')),
              ('60', ('60',)), ('90', ('90',)), ('120', ('120',)), ('144', ('144  ⚠',)), ('165', ('165  ⚠',)),
              ('240', ('240  ⚠',)), ('0', ('No limit  ⚠', 'Без ограничения  ⚠'))]
FRAMES_AHEAD = [('', ('1 (default)', '1 (по умолчанию)')), ('2', ('2',)), ('0', ('Unbounded', 'Без ограничения'))]
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


def ini_path():
    return Path(os.environ.get('BB_CONFIG', DATA_DIR / 'bbport.ini'))


def load_ini(legacy_settings=None):
    values, lines = dict(INI_DEFAULTS), []
    try:
        lines = ini_path().read_text(encoding='utf-8').splitlines()
    except OSError:
        pass
    for line in lines:
        if '=' in line and not line.lstrip().startswith('#'):
            key, value = line.split('=', 1)
            values[key.strip()] = value.strip()
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
        save_ini(migrated, lines)
        return load_ini(legacy_settings)
    if 'fsr411_backend' not in present:
        values['fsr411_backend'] = bbport_fsr411.default_backend(PORT_DIR, legacy_settings)
    values['fsr411_backend'] = fsr411_backend(values)
    values['object_motion_cache'] = '1' if values.get('object_motion_cache') == '1' else '0'
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
    ini_path().write_text('\n'.join(out) + '\n', encoding='utf-8')


def game_info(folder):
    """(title, app version) of a game folder, or None without eboot.bin."""
    folder = Path(folder or '.')
    if not (folder / 'eboot.bin').is_file():
        return None
    try:
        from prepare import sfo
        values = sfo((folder / 'sce_sys/param.sfo').read_bytes())
        return values.get('TITLE', 'Bloodborne').replace('™', '').strip(), values.get('APP_VER', '?')
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
        from prepare import sfo
        return str(sfo((Path(folder or '.') / 'sce_sys/param.sfo').read_bytes()).get('TITLE_ID', '')).strip()
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


def webapi_setting(s):
    """The WebAPI field, unless it holds the game server's address (a common mix-up): then the
    co-op server's own WebAPI is used."""
    webapi = str(s['online_webapi']).strip()
    community_host = community_address(s['online_community']).split('://', 1)[1].split(':')[0]
    if webapi and community_host in webapi:
        webapi = ''
    return webapi or webapi_address(str(s['online_server']).strip() or APP_DEFAULTS['online_server'])


def server_check(server, webapi, community):
    """A line about each online address: reachable, or why not (runs off the Tk thread)."""
    import socket
    lines = []
    try:
        with urllib.request.urlopen(community + ':20443/bb-eu/ss.info', timeout=6) as reply:
            reply.read(64)
        lines.append(_('Game server {0}: reachable').format(community))
    except (OSError, ValueError) as error:
        lines.append(_('Game server {0}: not reachable ({1})').format(community, error))
    host, _sep, port = server.rpartition(':')
    try:
        socket.create_connection((host or server, int(port or 31313)), timeout=4).close()
        lines.append(_('Server {0}: reachable').format(server))
    except (OSError, ValueError) as error:
        lines.append(_('Server {0}: not reachable ({1})').format(server, error))
    try:
        with urllib.request.urlopen(webapi.rstrip('/') + '/status', timeout=4) as reply:
            ok = b'"ok":true' in reply.read(512).replace(b' ', b'')
        lines.append(_('WebAPI {0}: reachable').format(webapi) if ok else
                     _('WebAPI {0}: answered, but it is not a shadNet server').format(webapi))
    except (OSError, ValueError) as error:
        lines.append(_('WebAPI {0}: not reachable ({1})').format(webapi, error))
    return '\n'.join(lines)


# The Hunter's Dream pool: the engine's own backend for it (BB_NET_BACKEND=thd, bbhost's
# protocol, from Awaken's port). BB_HUNTERS_DREAM=0 hides it.
HUNTERS_DREAM_POOL = os.environ.get('BB_HUNTERS_DREAM', '1') != '0'


def thd_call(url, body):
    """A call to The Hunter's Dream's account service (bbhost's): its reply, or ValueError with
    the server's reason."""
    request = urllib.request.Request(url, data=json.dumps(body).encode(), method='POST',
                                     headers={'Content-Type': 'application/json'})
    try:
        with urllib.request.urlopen(request, timeout=10) as response:
            text = response.read()
    except urllib.error.HTTPError as error:
        text = error.read()
    try:
        reply = json.loads(text)
    except ValueError:
        reply = None
    if not isinstance(reply, dict) or 'ResKind' not in reply:
        raise ValueError(f'{url} has no account service')
    if reply['ResKind'] != 0:
        raise ValueError(reply.get('Message') or 'refused')
    return reply


def thd_sign_in(game_server, on_code):
    """Discord sign-in the way bbhost does it: a code to approve on the website. on_code(page,
    code) shows it. Returns (online id, token); raises ValueError."""
    auth = community_address(game_server)
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
            urllib.request.urlopen(url, timeout=6).close()
            lines.append(_('{0}: reachable').format(name))
        except urllib.error.HTTPError:
            lines.append(_('{0}: reachable').format(name))   # it answered
        except (OSError, ValueError) as error:
            lines.append(_('{0}: not reachable ({1})').format(name, error))
    return '\n'.join(lines)


def motion_cache_available(graphics):
    return graphics.get('upscaler', INI_DEFAULTS['upscaler']) in ('fsr3', 'fsr4', 'fsr411') and \
        graphics.get('object_motion', INI_DEFAULTS['object_motion']) in ('1', True)


HUNTERS_ONLINE_EVERY = 600  # seconds between two looks at the count: the answer is about 140 KB


def hunters_online():
    """(players online, bbhost, shadNet) on The Hunter's Dream, from the public statistics its own
    start page shows (thehuntersdream.com/api/stats, active_sessions); None when it does not answer."""
    request = urllib.request.Request(COMMUNITY_SERVER + '/api/stats', headers={'User-Agent': f'bbport-launcher/{VERSION}'})
    try:
        with urllib.request.urlopen(request, timeout=8) as response:
            sessions = json.load(response).get('active_sessions') or {}
        if not sessions.get('server_online'):
            return None
        clients = sessions.get('by_client') or {}
        return int(sessions.get('count') or 0), int(clients.get('bbhost') or 0), int(clients.get('shadps4') or 0)
    except (OSError, ValueError, TypeError, AttributeError):
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


def cache_folder(s):
    """Where the shader and pipeline caches, the logs and the temporary files go: the cache folder the
    player chose, else the saves folder; a saves folder in a cloud folder keeps them on this PC
    (every PC builds the shaders for its own graphics card, and they are big)."""
    if s.get('cache_dir'):
        return Path(s['cache_dir'])
    saves = saves_folder(s)
    return DATA_DIR / 'user' if synced_folder(saves) else saves


def game_environment(s, graphics=None):
    env = {k: v for k, v in os.environ.items() if not k.startswith(('BB_FG_', 'BB_FG401_'))}
    graphics = graphics if graphics is not None else load_ini(s)[0]
    env['BB_GAME_DIR'] = s['game_dir']
    if s['user_dir']:
        env['BB_USER_DIR'] = s['user_dir']
    if cache_folder(s) != saves_folder(s):  # else run.py puts them beside the saves, as before
        env['BB_GPU_USER_DIR'] = str(cache_folder(s))
    if s['dlc_dir']:
        env['BB_DLC_DIR'] = s['dlc_dir']
    env['BB_MODS_DIR'] = s['mods_dir'] or str(DATA_DIR / 'mods')
    env['BB_MODS_CONFIG'] = str(DATA_DIR / 'mods.json')
    env['BB_MODS_ENABLED'] = '1' if s['mods_enabled'] else '0'
    env['BB_PATCHES_DIR'] = s['patches_dir'] or str(DATA_DIR / 'patches')
    env['BB_PATCHES_CONFIG'] = str(DATA_DIR / 'patches.json')
    env['BB_LANGUAGE'] = s['language']
    env['BB_VOICE_LANGUAGE'] = s['voice_language']
    if str(s['player_name']).strip():
        env['BB_USER_NAME'] = str(s['player_name']).strip()
    env['BB_FULLSCREEN'] = '1' if s['fullscreen'] else '0'
    env['BB_PRESENT_MODE'] = s['present_mode']
    if s['hdr']:
        env['BB_HDR'] = '1'
    env['BB_FPS'] = s['fps_mode']
    if s.get('frame_cap', ''):
        env['BB_FPS_LIMIT'] = s['frame_cap']
    for key, name in (('draw_pipe', 'BB_DRAW_PIPE'), ('readbacks', 'BB_READBACKS'), ('frames_ahead', 'BB_FRAMES_AHEAD'),
                      ('low_vram', 'BB_LOW_VRAM')):
        if s[key]:
            env[name] = s[key]
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
        server = str(s['online_server']).strip() or APP_DEFAULTS['online_server']
        env['BB_ONLINE'] = '1'
        env['BB_SHADNET_SERVER'] = server
        env['BB_SHADNET_WEBAPI'] = webapi_setting(s)
        env['BB_SHADNET_NPID'] = str(s['online_npid']).strip()
        env['BB_SHADNET_PASSWORD'] = str(s['online_password'])
        env['BB_UPNP'] = '1' if s['online_upnp'] else '0'
        env['BB_COMMUNITY_SERVER'] = community_address(s['online_community'])
        if s.get('online_pool') != 'shadnet' and HUNTERS_DREAM_POOL:  # also a pool the launcher no longer has
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
    env['PYTHONUNBUFFERED'] = '1'
    env['PYTHONIOENCODING'] = 'utf-8'
    return env


# ---------------------------------------------------------------------------------------------
# The window.

# The colours of bbport_theme (bbport.de): BG is the side bar, PANEL the pages.
BG, PANEL, CARD, LINE = theme.DEEP, theme.BG, theme.CARD, theme.BORDER
TEXT, MUTED, GOLD, BLOOD, BLOOD_HI = theme.TEXT, theme.MUTED, theme.GOLD_LIGHT, theme.BLOOD, theme.BLOOD_HOVER
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


class Launcher(bbport_setup.SetupMixin, bbport_help.HelpMixin, bbport_search.SearchMixin, bbport_pad.PadMixin):
    def __init__(self, root, tk, ttk, filedialog, messagebox):
        self.tk, self.ttk, self.filedialog, self.messagebox = tk, ttk, filedialog, messagebox
        self.root = root
        saved = load_json(CONFIG_FILE, {})
        self.app = {**APP_DEFAULTS, **saved}
        if self.app.get('online_pool') not in ('huntersdream', 'shadnet'):  # SuperMedo Server left the launcher
            self.app['online_pool'] = 'huntersdream'
        for key in ('supermedo_user', 'supermedo_password'):  # its account, the password in plain text
            self.app.pop(key, None)
        if self.app.get('ui_mode') not in ('simple', 'expert'):
            # Set up with an earlier launcher: every page as before (expert). New: the simple view.
            self.app['ui_mode'] = 'expert' if saved.get('setup_done') or saved.get('game_dir') else 'simple'
        self.ini, self.ini_lines = load_ini(self.app)
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
        self.banner_image = None
        self.ui_calls = queue.Queue()  # work for the Tk thread from helper threads
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
        root.protocol('WM_DELETE_WINDOW', self.close)
        root.after(100, self.drain_output)
        root.after(300, self.poll_trophies)
        self.hunters, self.hunters_when, self.hunters_asking = None, 0, False
        root.after(3000, self.poll_hunters)
        threading.Thread(target=self.detect_gpu, daemon=True).start()
        if self.app.get('check_updates', True) and not COOP_TEST:
            threading.Thread(target=self.check_update, daemon=True).start()

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
        var = self.var(key, store)
        values = [v for v, _t in options]
        box = self.ttk.Combobox(parent, values=[_(*text) for _v, text in options], state='readonly', width=width,
                               style=style)
        if var.get() not in values:
            var.set(values[0])

        def show(*_args):
            shown = options
            caps = getattr(self, 'fsr411_support', None)
            if key == 'upscaler' and caps is not None and var.get() != 'fsr411' and not any(
                    caps[m]['supported'] or caps[m]['reason'] == 'files' for m in ('vk', 'dx12')):
                shown = [(v, t) for v, t in options if v != 'fsr411']
            values[:] = [v for v, _t in shown]
            box.configure(values=[_(*text) for _v, text in shown])
            current = var.get()
            box.current(values.index(current) if current in values else 0)
        show()
        box.bind('<<ComboboxSelected>>', lambda _e: var.set(values[box.current()]))
        var.trace_add('write', show)
        box.setting_key = key  # for the search (row() indexes it)
        if key == 'upscaler' and hasattr(self, 'fsr411_upscaler_refreshers'):
            self.fsr411_upscaler_refreshers.append(show)
        return box

    def fg401_choice(self, parent, width=52):
        var = self.var('frame_gen', 'ini')
        box = self.ttk.Combobox(parent, state='readonly', width=width)
        values = []
        def refresh(*_args):
            ready = bool(self.fg401_support and self.fg401_support.get('supported'))
            options = [(v,t) for v,t in FRAME_GENERATION if v!='fsr401' or ready]
            values[:] = [v for v,t in options]
            box.configure(values=[_(*t) for v,t in options])
            if var.get()=='fsr401' and not ready:
                # Display the saved choice without offering it as a new unsupported choice.
                box.set(_('ML-FG 4.0.1: checking support…') if self.fg401_support is None else
                        _(bbport_fg401.KEYS[0]))
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
            selected = self.var('frame_gen', 'ini').get() == 'fsr401'
            # only with ML-FG chosen: on other PCs it is not offered, and its reasons would be noise
            text = bbport_fg401.message(self.fg401_support, _, hdr=bool(self.var('hdr', 'app').get()),
                                        selected=selected) if selected else ''
            self.fg401_status.configure(text=text)
            for widget in (self.fg401_status, getattr(self, 'fg401_recheck', None)):
                if widget is not None:
                    widget.grid() if text else widget.grid_remove()

    def recheck_fg401(self):
        self.fg401_support = None
        self.refresh_fg401()
        def work():
            support = bbport_fg401.query(PORT_DIR, bbport_fsr411.environment(PORT_DIR, self.app))
            def done():
                self.fg401_support = support
                self.refresh_fg401()
            self.ui_calls.put(done)
        threading.Thread(target=work, daemon=True).start()

    def fsr411_choice(self, parent, width=38):
        backend = self.var('fsr411_backend', 'ini')
        upscaler = self.var('upscaler', 'ini')
        values = [value for value, _text in FSR411_BACKENDS]
        box = self.ttk.Frame(parent)
        # A menu can disable individual methods while keeping their names visible.
        # Negative width is a minimum: long translated captions get their natural width.
        box.button = self.ttk.Menubutton(box, width=-width, style='Fsr411.TMenubutton')
        box.button.grid(row=0, column=0, sticky='w')
        box.menu = self.tk.Menu(box.button, tearoff=False, background=CARD, foreground=TEXT,
                               activebackground=BLOOD, activeforeground=TEXT, disabledforeground=MUTED)
        box.button.configure(menu=box.menu)
        # Menu clicks are provisional until the requested method is ready.
        box.selection = self.tk.StringVar(value=backend.get())
        def choose(method):
            self.select_fsr411_backend(method)
            refresh()
        for value, text in FSR411_BACKENDS:
            box.menu.add_radiobutton(label=_(*text), variable=box.selection, value=value,
                command=lambda method=value: choose(method))
        box.hint = self.ttk.Label(box, style='Muted.TLabel', wraplength=self.px(width * 9), justify='left')
        box.hint.grid(row=1, column=0, sticky='w', pady=(3, 0))
        if backend.get() not in values:
            backend.set(values[0])

        def refresh(*_args):
            active = upscaler.get() == 'fsr411'
            statuses = {method: self.fsr411_status(method) for method in values}
            selected = statuses[backend.get()]
            downloading = getattr(self, 'fsr411_downloading', False)
            offer = not getattr(self, 'process', None) and bbport_fsr411.can_download(
                statuses['vk'], getattr(self, 'fsr411_support', None))
            labels = {method: _(*text) for method, text in FSR411_BACKENDS}
            if bbport_fsr411.needs_download(statuses['vk']):
                labels['vk'] = _(bbport_fsr411.VULKAN_DOWNLOAD_LABEL)
            for index, method in enumerate(values):
                box.menu.entryconfigure(index, label=labels[method],
                    state='normal' if active and not downloading and
                        (statuses[method]['supported'] or method == 'vk' and offer) else 'disabled')
            box.selection.set(backend.get())
            box.button.configure(state='normal' if active and not downloading else 'disabled',
                text=labels[backend.get()] if active else '—',
                style='Unavailable.Fsr411.TMenubutton' if active and not selected['supported'] and
                    not (backend.get() == 'vk' and offer) else 'Fsr411.TMenubutton')
            if active and not selected['supported'] and selected['reason'] != 'download':
                text = bbport_fsr411.hint(selected, _)
                if bbport_fsr411.needs_download(selected):
                    text = bbport_fsr411.message({k: v for k, v in selected.items() if k != 'files_problem'}, _)
            else:
                text = ''
            box.hint.configure(text=text)
            if text:
                box.hint.grid()
            else:
                box.hint.grid_remove()
        traces = [(var, var.trace_add('write', refresh)) for var in (backend, upscaler)]
        if hasattr(self, 'fsr411_refreshers'):
            self.fsr411_refreshers.append(refresh)
        def cleanup(event):
            if event.widget == box:
                for var, trace in traces:
                    var.trace_remove('write', trace)
                if refresh in getattr(self, 'fsr411_refreshers', []):
                    self.fsr411_refreshers.remove(refresh)
        box.bind('<Destroy>', cleanup)
        refresh()
        box.setting_key = 'fsr411_backend'  # for the search (row() indexes it)
        return box

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
        entry = self.size_entry(holder, own, lambda size: res.set(display.size_text(size)), note)

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
        if self.var('upscaler', 'ini').get() != 'fsr411' or getattr(self, 'fsr411_downloading', False):
            return
        backend = self.var('fsr411_backend', 'ini')
        status = self.fsr411_status(method)
        if status['supported']:
            backend.set(method)
        elif method == 'vk' and not getattr(self, 'process', None) and bbport_fsr411.can_download(
                status, getattr(self, 'fsr411_support', None)):
            if not self.messagebox.askyesno(bbport_fsr411.DOWNLOAD_TITLE, _(bbport_fsr411.DOWNLOAD_QUESTION), parent=self.root):
                return
            previous = backend.get()
            def activate():
                if self.var('upscaler', 'ini').get() == 'fsr411' and backend.get() == previous:
                    backend.set('vk')
            self.download_fsr411(on_success=activate)

    def next_row(self, parent):
        return parent.grid_size()[1]

    def motion_cache_check(self, parent):
        upscaler = self.var('upscaler', 'ini')
        object_motion = self.var('object_motion', 'ini')
        button = self.ttk.Checkbutton(parent, text=_('Motion-vector cache (experimental)'),
                                     variable=self.var('object_motion_cache', 'ini'))
        row = self.next_row(parent)
        button.grid(row=row, column=0, columnspan=2, sticky='w', pady=(8, 0))
        self.ttk.Label(parent, text=_(
            'May improve FSR performance; uses extra VRAM. Requires object motion vectors. '
            'Applies at next game start.'), style='Muted.TLabel', wraplength=self.px(640),
            justify='left').grid(row=row + 1, column=0, columnspan=2, sticky='w', padx=(26, 0))
        self.index_entry(button, _('Motion-vector cache (experimental)'), key='object_motion_cache')

        def refresh(*_args):
            available = motion_cache_available({'upscaler': upscaler.get(),
                                                'object_motion': object_motion.get()})
            button.state(['!disabled' if available else 'disabled'])

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
            ttk.Label(parent, text=hint, style='Muted.TLabel', wraplength=self.px(560), justify='left').grid(
                row=r + 1, column=1, sticky='w', pady=(2, 2))
        self.index_entry(widget, title, hint, key or self.setting_key_of(widget), mark=label)
        return widget

    def check(self, parent, key, store, title, hint=None):
        ttk = self.ttk
        r = self.next_row(parent)
        box = ttk.Checkbutton(parent, text=title, variable=self.var(key, store))
        box.grid(row=r, column=0, columnspan=2, sticky='w', pady=(4, 0))
        self.index_entry(box, title, hint, key)
        if hint:
            ttk.Label(parent, text=hint, style='Muted.TLabel', wraplength=self.px(640), justify='left').grid(
                row=r + 1, column=0, columnspan=2, sticky='w', padx=(26, 0))

    def note(self, parent, text, top=12):
        self.ttk.Label(parent, text=text, style='Muted.TLabel', wraplength=self.px(680), justify='left').grid(
            row=self.next_row(parent), column=0, columnspan=2, sticky='w', pady=(top, 0))

    def fold(self, parent, title, texts, top=10):
        """Details most players never need: a gold link that shows or hides the notes `texts`."""
        link = self.tk.Label(parent, text=title + '  ›', bg=PANEL, fg=GOLD, cursor='hand2', font=(theme.BODY_MEDIUM, 9))
        link.grid(row=self.next_row(parent), column=0, columnspan=2, sticky='w', pady=(top, 0))
        notes = []
        for text in texts:
            label = self.ttk.Label(parent, text=text, style='Muted.TLabel', wraplength=self.px(680), justify='left')
            label.grid(row=self.next_row(parent), column=0, columnspan=2, sticky='w', pady=(6, 0))
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
                c.create_text(self.px(33), self.px(72), text=subtitle, anchor='nw', fill=theme.TEXT_SOFT,
                              font=(theme.BODY, 9), width=w - self.px(66))

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
        mark = theme.emblem(px(48), glow=False)
        if mark is not None:
            self.brand_mark = self.theme.photo(mark, BG)
            tk.Label(brand, image=self.brand_mark, bg=BG, bd=0).pack(side='left', padx=(0, px(10)))
        words = tk.Frame(brand, bg=BG)
        words.pack(side='left', fill='x')
        tk.Label(words, text='BLOODBORNE', bg=BG, fg=theme.TEXT_STRONG, font=(theme.HEAD_BOLD, 14)).pack(anchor='w')
        tk.Label(words, text='PC PORT', bg=BG, fg=GOLD, font=(theme.HEAD, 8)).pack(anchor='w')
        tk.Label(side, text=_('by Super Medo') + f'  ·  v{VERSION}' + ('\n' + _('co-op test build') if COOP_TEST else ''),
                 bg=BG, fg=MUTED, justify='left', font=(theme.BODY, 9)).pack(anchor='w', padx=px(22), pady=(px(2), px(8)))
        self.build_search(side)  # bbport_search: settings and help by the player's own words
        self.nav, self.current_page = {}, None
        # Shorter entries on a short screen, so the expert view (13 pages) still fits.
        self.nav_h = 37 if not self.compact else 32
        self.nav_pills = {'on': self.theme.box(224, self.nav_h, 8, CARD, LINE, bg=BG),
                          'hover': self.theme.box(224, self.nav_h, 8, theme.SURFACE, bg=BG)}
        accent = self.theme.box(3, 18, 1.5, theme.BLOOD_LIGHT, bg=CARD)
        self.nav_group = tk.Label(side, text=_('EXPERT'), bg=BG, fg=MUTED, font=(theme.BODY_SEMI, 8), anchor='w')
        for name, title in NAV_SIMPLE + NAV_EXPERT:
            if name == NAV_EXPERT[0][0]:
                self.nav_group.pack(fill='x', padx=px(24), pady=(px(8), px(2)))
            c = tk.Canvas(side, width=px(224), height=px(self.nav_h), bg=BG, highlightthickness=0, bd=0, cursor='hand2')
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
        self.nav_filler = tk.Frame(side, bg=BG)
        self.nav_filler.pack(fill='both', expand=True)
        self.update_box = None
        self.version_window = None  # Advanced > Other versions
        # The view: the switch never changes a setting, only which pages are shown.
        expert = tk.BooleanVar(value=self.app['ui_mode'] == 'expert')
        self.expert_var = expert
        # Packed ahead of the search and the entries: the switch and the link stay at the bottom
        # however short the window gets (the entries get flatter instead, fit_nav).
        site = tk.Label(side, text=COMMUNITY_SITE.split('//')[1].strip('/') + '  \u2197', bg=BG, fg=GOLD,
                        font=(theme.BODY_MEDIUM, 9), cursor='hand2')
        site.pack(side='bottom', anchor='w', padx=px(22), pady=(0, px(16)), before=self.search_box)
        self.mode_switch = ttk.Checkbutton(side, text=_('Expert view'), variable=expert, command=self.toggle_mode,
                                           style='Bar.TCheckbutton')  # drawn on the side bar's colour
        self.mode_switch.pack(side='bottom', anchor='w', padx=px(22), pady=(px(4), px(6)), before=self.search_box)
        side.bind('<Configure>', lambda _e: self.root.after_idle(self.fit_nav))
        site.bind('<Button-1>', lambda _e: webbrowser.open(COMMUNITY_SITE))
        site.bind('<Enter>', lambda _e: site.configure(fg=theme.TEXT_STRONG))
        site.bind('<Leave>', lambda _e: site.configure(fg=GOLD))
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
        ttk.Button(bar, text=_('Log', 'Журнал'), style='Bar.TButton', command=lambda: self.show('log')).pack(
            side='right', padx=(0, px(8)))
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
        if expert:
            self.nav_group.pack(fill='x', padx=px(24), pady=(px(8), px(2)), before=self.nav_filler)
            for name, _title in NAV_EXPERT:
                self.nav[name][0].pack(padx=px(12), pady=(0, px(1)), before=self.nav_filler)
        self.root.after_idle(self.fit_nav)

    def fit_nav(self):
        """The side bar's entries as tall as the window allows (37 points, down to 26 on a short
        window or a high scaling), so that every entry and the switch below them stay visible."""
        px = self.px
        try:
            top = self.search_box.winfo_y() + self.search_box.winfo_height() + px(10)
            bottom = (self.update_box if self.update_box and self.update_box.winfo_manager() else
                      self.mode_switch).winfo_y() - px(4)
        except self.tk.TclError:
            return
        shown = [name for name in self.nav if self.nav[name][0].winfo_manager()]
        group = self.nav_group.winfo_reqheight() + px(10) if self.nav_group.winfo_manager() else 0
        if not shown or bottom - top < px(100):
            return
        height = max(26, min(37, int(((bottom - top - group) / len(shown) - px(1)) / self.dpi)))
        if height == self.nav_h:
            return
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
        self.root.bind('<Configure>', lambda _e: self.title_state(), add='+')
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
        wheel events came in meanwhile."""
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
                                             lambda: self.root.after(0, self.bring_back),
                                             lambda: self.root.after(0, self.quit_from_tray),
                                             (_('Open'), _('Quit')))
            if self.tray.show():
                self.root.withdraw()
        except OSError:
            pass  # no tray: it stays on the task bar

    def bring_back(self):
        """The launcher in front again: from the tray, the task bar or hidden."""
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
            cover = self.cover(self.content) if name != getattr(self, 'current_page', None) else None
            self.show_page(name)
            self.uncover(cover)
        finally:
            self.showing = False
        if self.page_wanted and self.page_wanted != name:
            self.show(self.page_wanted)
            return
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
        for page in self.pages.values():
            page.pack_forget()
        self.pages[name].pack(fill='both', expand=True)
        self.current_page = name
        for n in self.nav:
            self.nav_paint(n)
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
        for key in ('news', 'system', 'settings', 'game', 'saves', 'online', 'cheats', 'fsr4'):
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
        ttk.Label(device, text=_('You play with'), style='Card.TLabel').pack(side='left')
        switch = self.device_switch(device)
        switch.pack(side='left', padx=(px(12), 0))
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
        self.hunters_note.bind('<Button-1>', lambda _e: webbrowser.open(COMMUNITY_SERVER))
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
        Picture & performance)."""
        self.var('goal', 'app').set(goal)
        chosen = self.recommended()
        if chosen:
            self.apply_profile(chosen[2])
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
        for text, fg, line, tags in (first, (f'v{VERSION}', theme.TEXT_SOFT, theme.BORDER_STRONG, ())):
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
        self.row(f, _('Quality preset', 'Пресет'), self.choice(f, 'preset', 'ini', PRESETS),
                 _('Render scale per axis: Quality renders at 1/1.5 of the output size.',
                   'Масштаб рендера по каждой оси: Quality рисует в 1/1.5 размера вывода.'))
        before = set(f.grid_slaves())
        self.row(f, _('FSR 4.1.1 backend'), self.fsr411_choice(f),
                 _("AMD DX12 uses AMD's FSR library. Vulkan uses fsr4vk. Switching applies the next time the game starts."))
        fsr411_rows = list(set(f.grid_slaves()) - before)
        self.row(f, _('Output resolution', 'Разрешение вывода'), self.output_choice(f),
                 _('What the upscaler produces; the HUD is drawn at this size too.',
                   'Что выдаёт апскейлер; интерфейс рисуется в этом же размере.'))
        self.row(f, _('HUD position (ultrawide)', 'Положение интерфейса (21:9)'),
                 self.choice(f, 'hud_position', 'ini', HUD_POSITIONS),
                 _('21:9 outputs: health, items and Blood Echoes at the screen edges, or in the centred '
                   '16:9 area as on a 16:9 screen.',
                   'Для 21:9: здоровье, предметы и Отголоски крови у краёв экрана или в центральной '
                   'области 16:9, как на обычном экране.'))
        self.row(f, _('Live resolution changes', 'Смена разрешения на лету'), self.choice(f, 'live_resolution', 'ini', LIVE),
                 _('Off: outputs other than 1080p are set by a patch at start (fastest; changing them in the '
                   'game restarts it). On: change output and preset in the game without a restart, at a cost.',
                   'Выкл.: разрешения кроме 1080p задаются патчем при запуске (быстрее). Вкл.: менять в игре '
                   'без перезапуска, но медленнее.'))
        self.check(f, 'sharpen', 'ini', _('Sharpening (RCAS)', 'Резкость (RCAS)'))
        holder = ttk.Frame(f)
        ttk.Scale(holder, from_=0.0, to=2.0, variable=self.var('sharpness', 'ini'), length=300).pack(side='left')
        value = ttk.Label(holder, width=5)
        value.pack(side='left', padx=10)
        show = lambda *_a: value.configure(text=f'{self.vars["sharpness"].get():.2f}')
        self.vars['sharpness'].trace_add('write', show)
        show()
        self.row(f, _('Sharpness', 'Сила резкости'), holder)
        self.check(f, 'object_motion', 'ini', _('Object motion vectors', 'Векторы движения объектов'),
                   _('Less ghosting on characters, cloth and weapons; costs about 5–10% FPS.',
                     'Меньше гостинга на персонажах и одежде; стоит около 5–10% FPS.'))
        self.motion_cache_check(f)
        self.section(f, _('Effects', 'Эффекты'))
        for key, title, _on in EFFECTS:
            if key not in TASTE_EFFECTS:
                self.check(f, key, 'ini', _(*title))
        self.row(f, _('Model detail (LOD)', 'Детализация моделей'), self.choice(f, 'model_lod', 'ini', LODS),
                 _('A game patch (game version 1.09).', 'Патч игры (версия 1.09).'))
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
        self.fg401_status = self.ttk.Label(f, wraplength=self.px(640), justify='left', style='Muted.TLabel')
        self.fg401_status.grid(row=self.next_row(f), column=1, sticky='w', pady=(4, 0))
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
        self.fsr4_label = ttk.Label(f, text='', wraplength=self.px(640), justify='left', style='Muted.TLabel')
        self.fsr4_label.grid(row=self.next_row(f), column=0, columnspan=2, sticky='w')
        holder = ttk.Frame(f)
        holder.grid(row=self.next_row(f), column=0, columnspan=2, sticky='w', pady=(8, 0))
        self.fsr4_button = ttk.Button(holder, text=_('Download FSR 4 assets', 'Скачать ассеты FSR 4'),
                                      command=self.download_fsr4)
        self.fsr4_button.pack(side='left')
        self.fsr4_progress = ttk.Progressbar(holder, length=280, maximum=len(fsr4_files()))
        self.fsr4_progress.pack(side='left', padx=12)
        self.note(f, _("From FireBurn/Q2RTX on GitHub (built from AMD's MIT-licensed FidelityFX source), "
                       'about 30 MB, into the fsr4_shaders folder of the port.',
                       'С GitHub FireBurn/Q2RTX (собраны из MIT-исходников AMD FidelityFX), около 30 МБ, '
                       'в папку fsr4_shaders порта.'), top=6)
        before = set(f.grid_slaves())
        self.section(f, _('FSR 4.1.1 (experimental)', 'FSR 4.1.1 (экспериментально)'))
        self.fsr411_label = ttk.Label(f, text='', wraplength=self.px(640), justify='left', style='Muted.TLabel')
        self.fsr411_label.grid(row=self.next_row(f), column=0, columnspan=2, sticky='w')
        self.fsr411_native_label = ttk.Label(f, text='', wraplength=self.px(640), justify='left', style='Muted.TLabel')
        self.fsr411_native_label.grid(row=self.next_row(f), column=0, columnspan=2, sticky='w')
        holder = ttk.Frame(f)
        holder.grid(row=self.next_row(f), column=0, columnspan=2, sticky='w', pady=(8, 0))
        self.fsr411_button = ttk.Button(holder, text=_('Download fsr4vk'),
                                        command=self.download_fsr411)
        self.fsr411_button.pack(side='left')
        self.fsr411_progress = ttk.Progressbar(holder, length=280, maximum=100)
        self.fsr411_progress.pack(side='left', padx=12)
        self.ttk.Button(holder, text=_('Recheck support'), command=self.recheck_fsr411).pack(side='left')
        self.note(f, _('Download fsr4vk here. AMD DX12 is included.'), top=6)
        fsr411_rows += list(set(f.grid_slaves()) - before)

        def show_fsr411(*_a):
            for widget in fsr411_rows:
                widget.grid() if self.var('upscaler', 'ini').get() == 'fsr411' else widget.grid_remove()
        self.var('upscaler', 'ini').trace_add('write', show_fsr411)
        show_fsr411()
        self.refresh_fsr411()
        self.vars['fsr411_backend'].trace_add('write', lambda *_a: (self.refresh_fsr411(), self.refresh_status()))

    def link_row(self, parent, text, page, link=None, command=None):
        """A muted line with a link to another page (or to `command`)."""
        row = self.ttk.Frame(parent)
        row.grid(row=self.next_row(parent), column=0, columnspan=2, sticky='w', pady=(4, 0))
        self.ttk.Label(row, text=text, style='Muted.TLabel').pack(side='left')
        label = self.tk.Label(row, text=(link or _('Open')) + '  ›', bg=PANEL, fg=GOLD, cursor='hand2',
                              font=(theme.BODY_MEDIUM, 9))
        label.pack(side='left', padx=(8, 0))
        label.bind('<Button-1>', lambda _e: command() if command else self.show(page))

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
        dlc = self.tk.Label(f, text='', bg=PANEL, font=(theme.BODY, 9), justify='left', anchor='w')
        dlc.grid(row=self.next_row(f), column=1, sticky='w', pady=(4, 2))

        def dlc_shown(*_a):
            kind, text = dlc_status(self.var('game_dir', 'app').get(), self.var('dlc_dir', 'app').get().strip(),
                                    self.var('user_dir', 'app').get() or DATA_DIR / 'user')
            mark = {'ok': '✓', 'warn': '!', 'none': '–'}[kind]
            dlc.configure(text=f'{mark}  {text}', wraplength=self.px(560),
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
        self.row(f, '', holder,
                 _('Spread a character\'s attribute points again: same level, same Blood Echoes. '
                   'Changes the save while the game is closed; a copy of the saves is kept in '
                   'respec-backups beside them.',
                   'Заново распределить очки характеристик персонажа: тот же уровень и Отголоски '
                   'крови. Меняет сохранение, пока игра закрыта; копия сохранений остаётся в '
                   'respec-backups рядом с ними.'))

    def open_respec(self):
        """The respec dialog (bbport_respec): a character, its six attributes, Apply."""
        tk, ttk = self.tk, self.ttk
        title = _('Respec', 'Перераспределение')
        if self.process:
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
                backup = respec.respec(folder, slot, new)
            except (ValueError, OSError, tk.TclError) as error:
                self.messagebox.showerror(title, str(error), parent=win)
                return
            who['attributes'] = new
            self.messagebox.showinfo(title, _('Done. A copy of the saves from before is in {}.',
                                              'Готово. Копия прежних сохранений: {}.').format(backup.parent),
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
        f = self.scrolled_page('online', _('Online co-op'),
                               _("Messages, bloodstains and ghosts come from The Hunter's Dream; bells and summons "
                                 'go through a shadNet server.'))
        self.check(f, 'online', 'app', _('Play online'),
                   _('Off: the game stays offline, as before.'))
        self.var('online', 'app').trace_add('write', lambda *_a: self.refresh_status())
        self.section(f, _('Co-op pool'))
        pool = self.var('online_pool', 'app')
        pool.trace_add('write', lambda *_a: (self.refresh_status(), self.root.after_idle(self.ask_hunters)))
        pools = ttk.Frame(f)
        if HUNTERS_DREAM_POOL:
            ttk.Radiobutton(pools, text=_("The Hunter's Dream (with bbhost players)"), variable=pool,
                            value='huntersdream').pack(side='left', padx=(0, 16))
        elif pool.get() == 'huntersdream':
            pool.set('shadnet')
        ttk.Radiobutton(pools, text=_('shadNet server'), variable=pool, value='shadnet').pack(side='left')
        pools.setting_key = 'online_pool'  # the search's words (bbport_search.WORDS)
        self.row(f, _('Play with'), pools,
                 _("The Hunter's Dream: co-op together with bbhost players, after a free sign-in with Discord. "
                   'shadNet: a shadNet server of your choice and its account.'))
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
        account = ttk.Label(f, text='', justify='left')

        thd_id, thd_token = self.var('thd_online_id', 'app'), self.var('thd_token', 'app')

        def show_accounts():
            name = thd_id.get() if thd_token.get() else ''
            account.configure(text=_('Signed in as {}').format(name) if name else _('Not signed in'))
        show_accounts()
        self.row(f, _('Account'), account, key='thd_online_id')
        signing = ttk.Label(f, text='', style='Muted.TLabel', justify='left', wraplength=self.px(560))
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

        def sign_in():
            signing.configure(text=_('Starting the sign-in…'))
            show_code()
            game = self.var('online_community', 'app').get()

            def on_code(page, code):
                text = _('In the page that opened ({0}), sign in with Discord and enter this code:').format(page)
                self.ui_calls.put(lambda: (signing.configure(text=text), show_code(code)))
                webbrowser.open(page)

            def work():
                try:
                    name, token = thd_sign_in(game, on_code)
                except (OSError, ValueError) as error:
                    self.ui_calls.put(lambda e=error: (signing.configure(text=_('Sign-in failed: {}').format(e)),
                                                       show_code()))
                    return
                self.ui_calls.put(lambda: (thd_id.set(name), thd_token.set(token), signing.configure(text=''),
                                           show_code(), show_accounts(), self.collect()))
            threading.Thread(target=work, daemon=True).start()
        self.row(f, '', ttk.Button(f, text=_('Sign in with Discord'), command=sign_in))
        signing.grid(row=self.next_row(f), column=1, sticky='w', pady=(6, 0))
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
        self.row(f, _('Online ID (NPID)'), ttk.Entry(f, textvariable=self.var('online_npid', 'app'), width=40),
                 _('Your shadNet account name, not your email. Register one at shadnet.shadps4.net, or ask the '
                   'owner of a private server.'))
        self.row(f, _('Password'), ttk.Entry(f, textvariable=self.var('online_password', 'app'), width=40, show='•'),
                 _('Saved on this PC with the other launcher settings.'))
        # The servers' addresses only for those who play on their own (a private server, a friend's):
        # everyone else plays on the public ones and never needs to see a port number.
        if 'online_custom_servers' not in self.app:
            self.app['online_custom_servers'] = any(
                self.app.get(key, '').strip() not in ('', APP_DEFAULTS[key])
                for key in ('online_server', 'online_webapi', 'online_community'))
        custom = self.var('online_custom_servers', 'app')
        self.check(f, 'online_custom_servers', 'app', _('Use my own servers'),
                   _('Off: the public servers. On: a private server, with the addresses from the person who '
                     'runs it.'))
        group('shadnet')
        self.section(f, _('Game server (messages, bloodstains, ghosts)'))
        community = self.var('online_community', 'app')
        self.row(f, _('Game server'), ttk.Entry(f, textvariable=community, width=40),
                 _("The Hunter's Dream by default. Only the name is needed; the game picks the ports."))
        self.section(f, _('Co-op server (bells and summons)'))
        server = self.var('online_server', 'app')
        self.row(f, _('Server address'), ttk.Entry(f, textvariable=server, width=40),
                 _('srv.shadps4.net:31313 is the public shadPS4 server. For a private server, enter '
                   'host:port from the person who runs it.'))
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
            address = values['online_server'].strip() or APP_DEFAULTS['online_server']
            webapi, game = webapi_setting(values), community_address(values['online_community'])
            online_on = self.var('online', 'app').get()
            pool_name = pool.get()
            hunters_dream = pool_name == 'huntersdream'
            signed_in = thd_id.get() if thd_token.get() else ''
            result.configure(text=_('Checking…'))

            def work():
                if hunters_dream:
                    text = server_check_hunters_dream()
                    text += '\n' + (_('Signed in as {}').format(signed_in) if signed_in else
                                    _('Not signed in: use "Sign in with Discord" above.'))
                else:
                    text = server_check(address, webapi, game)
                if pool_name == 'shadnet' and values['online_webapi'].strip() and values['online_webapi'].strip() != webapi:
                    text += '\n' + _('The WebAPI box holds the game server address; it is ignored.')
                if not online_on:
                    text += '\n' + _('"Play online" is off: the game starts offline.')
                self.ui_calls.put(lambda: result.configure(text=text))
            threading.Thread(target=work, daemon=True).start()
        self.row(f, '', ttk.Button(f, text=_('Test connection'), command=check))
        result.grid(row=self.next_row(f), column=1, sticky='w', pady=(6, 0))
        self.note(f, _('Everyone in a session needs the same game version (1.09) and the same server. '
                       'Cheats change the game for the other players too: turn them off when you play together.'))
        show_code()  # hidden until a sign-in shows one; its row stays reserved (rows are counted above)
        shown = {'shadnet': {'shadnet', 'upnp'}, 'huntersdream': {'huntersdream', 'upnp'}}

        def show_pool(*_args):
            if pool.get() == 'huntersdream' and not HUNTERS_DREAM_POOL:
                pool.set('shadnet')  # runs show_pool again
                return
            want = set(shown.get(pool.get(), shown['shadnet']))
            if 'shadnet' in want and custom.get():
                want.add('servers')
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
        self.trophy_body = self.ttk.Frame(f)
        self.trophy_body.grid(row=self.next_row(f), column=0, columnspan=2, sticky='we')

    def trophy_icon(self, parent, grade, earned, size, bg):
        """A trophy cup in the colour of its grade (dim while locked), on a round plate."""
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
        state = (tuple(sorted(unlocked.items())), self.trophy_filter)
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
            tk.Label(head, text=group, bg=PANEL, fg=GOLD, font=(theme.HEAD, 13)).pack(side='left')
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
        self.trophy_icon(card, grade, earned, self.px(46), CARD).pack(side='left', padx=(12, 14), pady=10)
        side = tk.Frame(card, bg=CARD)
        side.pack(side='right', padx=(12, 18))
        tk.Label(side, text=_(*GRADE_NAMES[grade]), bg=CARD, fg=TROPHY_COLORS[grade] if earned else MUTED,
                 font=(theme.BODY, 9, 'bold')).pack(anchor='e')
        tk.Label(side, text=time.strftime('%d %b %Y', time.localtime(when)) if earned else _('Locked', 'Не получен'),
                 bg=CARD, fg=MUTED, font=(theme.BODY, 9)).pack(anchor='e', pady=(2, 0))
        middle = tk.Frame(card, bg=CARD)
        middle.pack(side='left', fill='x', expand=True, pady=10)
        tk.Label(middle, text=name, bg=CARD, fg=TEXT if earned else MUTED,
                 font=(theme.BODY, 11, 'bold' if earned else 'normal')).pack(anchor='w')
        tk.Label(middle, text=text, bg=CARD, fg=MUTED, font=(theme.BODY, 9), justify='left',
                 wraplength=self.px(520)).pack(anchor='w', pady=(1, 0))

    def set_trophy_filter(self, name):
        self.trophy_filter = name
        self.render_trophies()

    def trophy_dir(self):
        return Path(self.var('user_dir', 'app').get() or DATA_DIR / 'user')

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
        icon = self.trophy_icon(box, grade, True, self.px(38), CARD)
        icon.pack(side='left', padx=(12, 12), pady=10)
        words = tk.Frame(box, bg=CARD)
        words.pack(side='left', padx=(0, 20), pady=8)
        caption = tk.Label(words, text=_("You've earned a trophy.", 'Вы получили трофей.'), bg=CARD, fg=MUTED,
                           font=(theme.BODY, 9))
        caption.pack(anchor='w')
        title = tk.Label(words, text=f'{name}', bg=CARD, fg=TEXT, font=(theme.BODY, 11, 'bold'))
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
        ttk.Checkbutton(f, text=_('Enable mouse look', 'Включить обзор мышью'), variable=enabled_var).grid(
            row=self.next_row(f), column=0, columnspan=2, sticky='w', pady=(4, 0))
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
            ttk.Checkbutton(f, text=text, variable=var).grid(
                row=self.next_row(f), column=0, columnspan=2, sticky='w', pady=(4, 0))

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
        ttk.Button(reset, text=_('Reset controller buttons', 'Сбросить кнопки геймпада'),
                   command=lambda: [self.set_controller(o, DEFAULT_CONTROLLER.get(o)) for o in CONTROLLER_OUTPUTS]
                   ).pack(side='left')
        self.row(f, '', reset)
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
            Path(self.remap_ini_path).write_text('\n'.join(lines) + '\n', encoding='utf-8')
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

        # Tk on Windows sends Alt, Alt + key and F10 to the menu bar (tk::WinMenuKey, which
        # waits in the system menu's loop), not to <KeyPress>: they are taken over meanwhile.
        menu_keys = {seq: self.root.bind_all(seq) for seq in (
            '<Key-Alt_L>', '<Key-Alt_R>', '<Alt-Key>', '<Key-F10>',
            '<KeyRelease-Alt_L>', '<KeyRelease-Alt_R>', '<Alt-KeyRelease>', '<KeyRelease-F10>')}

        def finish():
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

        self.root.bind_all('<KeyPress>', on_key)
        self.root.bind_all('<KeyRelease>', on_key_release)
        for seq in menu_keys:
            self.root.bind_all(seq, on_key_release if 'Release' in seq else on_key)
        if not is_stick:
            self.root.after_idle(lambda: self.root.bind_all('<ButtonPress>', on_click))
            # While waiting, the wheel binds instead of scrolling the page.
            self.root.bind_all('<MouseWheel>', on_wheel)
            self.root.bind_all('<Shift-MouseWheel>', lambda e: on_wheel(e, True))

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
        holder = ttk.Frame(f)
        holder.grid(row=self.next_row(f), column=0, columnspan=2, sticky='w', pady=(10, 0))
        self.updates_button = ttk.Button(
            holder, text=_('Check for updates', 'Проверить обновления'),
            command=lambda: threading.Thread(target=self.check_update, args=(True,), daemon=True).start())
        self.updates_button.pack(side='left')
        self.index_entry(self.updates_button, _('Check for updates', 'Проверить обновления'), key='button:updates')
        ttk.Button(holder, text=_('Other versions…', 'Другие версии…'), command=self.choose_version).pack(
            side='left', padx=(6, 0))
        ttk.Label(holder, text=f'v{VERSION}', style='Muted.TLabel').pack(side='left', padx=10)
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
        holder.grid(row=self.next_row(f), column=0, columnspan=2, sticky='w', pady=(10, 0))
        ttk.Button(holder, text=_('Benchmark for bug reports…'), command=self.open_bug_benchmark).pack(side='left')
        ttk.Label(holder, text=_('A fixed route in Cathedral Ward for testers: one zip to send.'),
                  style='Muted.TLabel').pack(side='left', padx=10)
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
        info = {'Launcher': f'v{VERSION}', 'Graphics card': self.gpu_line or '?',
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
        label = self.ttk.Label(parent, style='Warning.TLabel', wraplength=self.px(640), justify='left', text=_(
            'Your game is version {}: these options are patches for 1.09 and are not applied; the game runs '
            'at 30 FPS. Update the game to 1.09 to use them.',
            'Ваша игра версии {}: эти параметры — патчи для 1.09 и не применяются; игра работает в 30 FPS. '
            'Обновите игру до 1.09, чтобы их использовать.'))
        label.grid(row=self.next_row(parent), column=0, columnspan=2, sticky='we', pady=(6, 4))
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
        saves = list((user / 'savedata').glob('*/*/SPRJ*')) if (user / 'savedata').is_dir() else []
        save = (_('✓ Saves found in {}', '✓ Найдены сохранения в {}').format(user) if saves
                else _('• No saves yet: bring your character from another version, or the game creates them'))
        missing = fsr4_missing()
        eboot = self.eboot_note() if info else None
        problem = (self.app.get('eboot_check') or {}).get('state') not in (None, 'clean')
        self.check_line('game', eboot or game, (lambda: self.open_topic('eboot_modified')) if eboot and problem else
                        (lambda: self.show('game')) if not info or info[1] != PATCH_VERSION else None)
        self.check_line('saves', save, lambda: self.show('saves'))
        self.check_line('fsr4', _('• FSR 4 files missing: FSR 3.1 is used meanwhile. Click to download (30 MB)')
                        if missing and self.var('upscaler', 'ini').get() == 'fsr4' else '', self.download_fsr4)
        fsr411_ready, status = True, None
        if self.var('upscaler', 'ini').get() == 'fsr411':
            status = self.fsr411_status(self.var('fsr411_backend', 'ini').get())
            fsr411_ready = status['supported']
            self.check_line('fsr4', '' if fsr411_ready else '• FSR 4.1.1: ' + bbport_fsr411.hint(status, _),
                            lambda: self.show('graphics'))
        online, pool = self.var('online', 'app').get(), self.var('online_pool', 'app').get()
        signed_in = (bool(self.var('thd_token', 'app').get()) if pool == 'huntersdream' else
                     bool(self.var('online_npid', 'app').get().strip()))
        self.check_line('online', ('⚠ ' + _('Online is on, but you are not signed in yet: sign in now')
                                   if online and not signed_in else ''), lambda: self.show('online'))
        if getattr(self, 'online_note', None):
            name = self.var('thd_online_id', 'app').get() if pool == 'huntersdream' else ''
            self.online_note.configure(text=(_('signed in as {}').format(name) if online and signed_in and name else
                                             _('not signed in ›') if online and not signed_in else ''),
                                       fg=MUTED if signed_in else theme.AMBER)
        self.show_hunters()
        cheats = [_(*title) for key, title, _on in CHEATS if self.var(key, 'ini').get()]
        self.check_line('cheats', ('⚠ ' + _('Cheats on: {}').format(', '.join(t.split(' (')[0] for t in cheats))
                                   if cheats else ''), lambda: self.show('cheats'))
        news = self.var('setup_done', 'app').get() and self.app.get('seen_version') != VERSION
        self.check_line('news', '• ' + _('New in version {}: what has changed').format(VERSION) if news else '',
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
            found = hunters_online()

            def done():
                self.hunters, self.hunters_when, self.hunters_asking = found, time.time(), False
                self.show_hunters()
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

    def open_news(self):
        self.app['seen_version'] = VERSION
        webbrowser.open(RELEASES_PAGE)
        self.refresh_status()

    def summary(self):
        fps = dict(FPS_MODES).get(self.var('fps_mode', 'app').get(), ('?',))
        info = game_info(self.var('game_dir', 'app').get())
        if info and info[1] != PATCH_VERSION:
            fps = ('30 FPS',)
        upscaler = dict(UPSCALERS).get(self.var('upscaler', 'ini').get(), ('?',))[0].split(' (')[0]
        if self.var('upscaler', 'ini').get() == 'fsr411':
            upscaler += ' · ' + ('DX12' if self.var('fsr411_backend', 'ini').get() == 'dx12' else 'fsr4vk')
        output = self.var('output_res', 'ini').get().replace('x', ' × ')
        online = _('Online') if self.var('online', 'app').get() else _('Offline')
        if self.var('online', 'app').get() and self.var('online_pool', 'app').get() == 'huntersdream':
            online = _("Online (The Hunter's Dream)")
        return f'{_(*fps)}   ·   {upscaler}   ·   {output}   ·   {online}'

    def detect_gpu(self):
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
        except (OSError, subprocess.TimeoutExpired):
            pass
        self.gpu_text = text
        support = bbport_fsr411.query(PORT_DIR, env)
        def fsr411_done():
            self.fsr411_support = support
            self.refresh_fsr411()
            self.refresh_status()
        self.ui_calls.put(fsr411_done)
        self.fg401_support = bbport_fg401.query(PORT_DIR, env)
        self.gpu_line = hardware.best_gpu_line(names)  # 'Name, discrete, 16304 MiB' for the hardware check
        try:
            self.cpu, self.ram = hardware.detect_cpu(), hardware.memory_gb()
        except Exception:  # the rating is a guide: never stop the launcher over it
            self.cpu, self.ram = hardware.Cpu('', None), 16
        self.ui_calls.put(self.refresh_status)
        self.ui_calls.put(self.hardware_ready)
        self.ui_calls.put(self.refresh_fg401)

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

    def fsr411_folder(self):
        env = bbport_fsr411.environment(PORT_DIR, self.app)
        return bbport_fsr411.vulkan_folder(PORT_DIR, DATA_DIR, env)

    def recheck_fsr411(self):
        self.fsr411_support = None
        self.refresh_fsr411()
        def work():
            support = bbport_fsr411.query(PORT_DIR, bbport_fsr411.environment(PORT_DIR, self.app), refresh=True)
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
            try:
                fetch_fsr4vk.fetch(folder, progress)
                if not fetch_fsr4vk.files_ok(folder):
                    raise fetch_fsr4vk.FetchError(_('Vulkan (fsr4vk): files missing or need updating.'))
            except (OSError, fetch_fsr4vk.FetchError) as failure:
                error = str(failure)

            def done():
                self.fsr411_downloading = False
                if not error and on_success and self.fsr411_status('vk')['supported']:
                    on_success()
                self.refresh_fsr411()
                self.refresh_status()
                if error:
                    self.messagebox.showerror('FSR 4.1.1', _('Download failed: {}', 'Не удалось скачать: {}').format(error))
            self.ui_calls.put(done)
        threading.Thread(target=work, daemon=True).start()

    def download_fsr4(self):
        self.downloading = True
        self.fsr4_button.configure(state='disabled')
        folder = PORT_DIR / 'fsr4_shaders'

        def work():
            error = None
            try:
                folder.mkdir(parents=True, exist_ok=True)
                for name in fsr4_missing():
                    part = folder / (name + '.part')
                    with urllib.request.urlopen(f'{FSR4_BASE}/{name}', timeout=60) as response:
                        part.write_bytes(response.read())
                    part.replace(folder / name)
                    self.ui_calls.put(self.refresh_fsr4)
            except OSError as failure:
                error = failure

            def done():
                self.downloading = False
                self.refresh_fsr4()
                self.refresh_status()
                if error:
                    self.messagebox.showerror('FSR 4', _('Download failed: {}', 'Не удалось скачать: {}').format(error))
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
            ttk.Label(self.mods_frame, text=_('No mods in {} yet.', 'В {} пока нет модов.').format(root),
                      style='Muted.TLabel').pack(anchor='w')
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
            ttk.Label(self.mods_frame, text=_('Not loaded: {}: {}', 'Не загружен: {}: {}').format(name, problem),
                      style='Muted.TLabel', wraplength=self.px(620), justify='left').pack(anchor='w', pady=(4, 0))
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
        label = tk.Label(row, text=name, bg=CARD, fg=TEXT, font=(theme.BODY, 10), anchor='w', cursor='hand2')
        label.pack(side='left', fill='x', expand=True, padx=(px(4), px(10)))
        for widget in (row, grip, label):
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
        self.ini, self.ini_lines = disk, lines

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
        CONFIG_DIR.mkdir(parents=True, exist_ok=True)
        CONFIG_FILE.write_text(json.dumps(self.app, indent=2, ensure_ascii=False), encoding='utf-8')
        save_ini({key: self.ini[key] for key in INI_DEFAULTS}, self.ini_lines)
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
        if self.mod_order:
            (DATA_DIR / 'mods.json').write_text(json.dumps(
                {'order': self.mod_order, 'disabled': [n for n, v in self.mod_vars.items() if not v.get()]},
                indent=2), encoding='utf-8')
        if self.patch_vars:
            chosen = load_json(DATA_DIR / 'patches.json', {})
            shown = set(self.patch_vars)
            enabled = {k for k in chosen.get('enabled', []) if k not in shown} | {k for k, v in self.patch_vars.items() if v.get()}
            disabled = {k for k in chosen.get('disabled', []) if k not in shown} | {k for k, v in self.patch_vars.items() if not v.get()}
            (DATA_DIR / 'patches.json').write_text(json.dumps(
                {'enabled': sorted(enabled), 'disabled': sorted(disabled)}, indent=2), encoding='utf-8')

    # ---- game process ------------------------------------------------------------------------
    def play(self):
        if self.process or self.fsr411_downloading:
            return
        online, npid = self.var('online', 'app'), self.var('online_npid', 'app')
        if not online.get() and npid.get().strip():
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
        if online.get() and pool_name == 'shadnet' and '@' in npid.get():
            self.messagebox.showwarning(
                'Bloodborne', _('The Online ID is the account name (NPID) you registered on the server, '
                                'not your email address.'))
            self.show('online')
            return
        if self.var('output_display', 'app').get():
            self.var('output_res', 'ini').set(display.size_text(self.game_monitor_size()))
        self.collect()
        if self.ini.get('upscaler') == 'fsr411':
            self.fsr411_support = bbport_fsr411.query(PORT_DIR, bbport_fsr411.environment(PORT_DIR, self.app), refresh=True)
            status = self.fsr411_status(self.ini['fsr411_backend'])
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
        if not self.preflight():  # bbport_help: a start that would fail for a reason the player can fix
            return
        self.set_log('')
        self.pad_stop()  # the game reads the controller now
        self.auto_backup()
        try:
            self.process = subprocess.Popen(run_command(), cwd=PORT_DIR, env=game_environment(self.app, self.ini),
                                            stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                            stderr=subprocess.STDOUT, creationflags=NO_WINDOW)
        except OSError as error:
            self.append(_('Could not start: {}', 'Не удалось запустить: {}').format(error) + '\n')
            self.process = None
            return
        self.job = GameJob(self.process)
        self.placer = display.Placer(self.app, display.parse_size(self.ini.get('output_res')) or (1920, 1080)).start()
        threading.Thread(target=self.read_output, args=(self.process,), daemon=True).start()
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
        try:
            made = saves.auto_backup(self.save_user_dir(), self.app['game_dir'])
        except OSError as error:
            self.append(_('The automatic backup of the saves failed: {}').format(error) + '\n')
            return
        if made:
            self.append(_('Saves backed up: {}').format(made.name) + '\n')
            self.refresh_saves()

    def window_left(self, size):
        """The game's last size in a window, for 'As I left it' next time."""
        if size and not self.app.get('fullscreen'):
            self.app['window_last'] = display.size_text(size)
            remember_window(size)

    def read_output(self, process):
        """Helper thread: the game's output to the Log page and to <saves folder>/last_run.log."""
        log = None
        try:
            log_dir = Path(self.app.get('user_dir') or DATA_DIR / 'user')
            log_dir.mkdir(parents=True, exist_ok=True)
            log = open(log_dir / 'last_run.log', 'w', encoding='utf-8', buffering=1)
        except OSError:
            pass
        warned = False
        for raw in iter(process.stdout.readline, b''):
            text = raw.decode('utf-8', errors='replace')
            self.output.put(text)
            if not warned and 'eboot.bin is not the 01.09 build' in text:
                warned = True
                self.ui_calls.put(lambda: self.messagebox.showwarning('Bloodborne', _(
                    'Your eboot.bin is not the clean 1.09 one (modified, or from another release). The port '
                    'cannot patch it: the game runs at 30 FPS in 16:9 (ultrawide and the output resolution '
                    'do not apply), the Game & effects options do nothing, and some scenes (like the '
                    'cutscene after Rom) can freeze.\n\nReplace eboot.bin with the one from an unmodified '
                    '1.09 update.')))
            elif not warned and 'eboot.bin is modified, not the clean 01.09 build' in text:
                # Some patch groups still apply (run.py): the line says which are off.
                warned = True
                detail = text.split(': ', 1)[-1].strip()
                self.ui_calls.put(lambda: self.messagebox.showwarning('Bloodborne', _(
                    "Your eboot.bin has been modified (for example with a 60 FPS patch already applied). "
                    "Part of the port's patches cannot be used with it:\n\n{}\n\nFor every option, "
                    "replace eboot.bin with the one from an unmodified 1.09 update.").format(detail)))
            if log:
                log.write(text.replace('\r\n', '\n'))
        if log:
            log.close()
        self.output.put((process.wait(),))

    def drain_output(self):
        if self.process:
            try:
                stamp = ini_path().stat().st_mtime_ns
                # Wait for a stable write before merging an overlay save into the UI.
                if stamp == getattr(self, '_shared_ini_seen', None) and stamp != getattr(self, '_shared_ini_synced', None):
                    self.sync_ini()
                    self._shared_ini_synced = stamp
                self._shared_ini_seen = stamp
            except OSError:
                pass
        while not self.ui_calls.empty():
            self.ui_calls.get_nowait()()
        frame = self.custom_frame
        if frame and frame.cover and time.monotonic() - frame.cover_time > 1.5 and self.root.state() != 'iconic':
            self.uncover_window()  # a minimize that did not happen: the picture never stays over the window
        try:
            for _i in range(500):
                item = self.output.get_nowait()
                if isinstance(item, tuple):
                    self.append(_('\n— the game exited (code {}) —\n', '\n— игра завершилась (код {}) —\n').format(item[0]))
                    self.process = None
                    self.sync_ini()
                    self.refresh_fsr411()
                    if self.job:
                        self.job.close()
                    self.refresh_saves()  # the characters' times
                    if self.placer:
                        self.placer.stop()
                        self.window_left(self.placer.last)
                        self.placer = None
                    self.stop_button.configure(state='disabled')
                    self.stop_button.pack_forget()
                    self.refresh_status()
                    reopen = self.setting('reopen_after_game')
                    if self.hidden:
                        if item[0] == 0 and not reopen:
                            self.drop_tray()
                            self.root.destroy()
                            return
                        # Back (asked for, or it stopped with an error: then with its log).
                        self.hidden = False
                        self.bring_back()
                        if item[0] != 0:
                            self.show('log')
                    elif reopen:
                        self.bring_back()
                    self.root.after(300, lambda code=item[0]: self.after_exit(code))  # the known causes in its log
                else:
                    if 'Entering original x86-64 code' in item:
                        self.status.configure(text=_('The game is running.', 'Игра запущена.'), fg=GOLD)
                    elif 'restarting through run.py' in item:
                        self.status.configure(text=_('Restarting with the new settings…', 'Перезапуск с новыми настройками…'), fg=GOLD)
                    elif 'Runtime: trophy ' in item:
                        self.trophies_changed(self.tracker.observe(item))
                    self.append(item)
        except queue.Empty:
            pass
        self.root.after(100, self.drain_output)

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
    def check_update(self, manual=False):
        """Helper thread: asks GitHub for the newest release and offers it when it is newer."""
        try:
            version, url, page = latest_release()
        except (OSError, ValueError, KeyError) as failure:
            if manual:
                self.ui_calls.put(lambda error=failure: self.messagebox.showerror(
                    'Bloodborne', _('Could not check for updates: {}', 'Не удалось проверить обновления: {}').format(error)))
            return

        def show():
            if version_tuple(version) > version_tuple(VERSION):
                self.offer_update(version, url, page)
            elif manual:
                self.messagebox.showinfo('Bloodborne', _('You have the latest version ({}).',
                                                         'У вас последняя версия ({}).').format(VERSION))
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
                                        command=lambda: self.install_update(version, url, page))
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
        def progress(percent):
            self.update_label.configure(text=_('Downloading version {}… {}%', 'Загрузка версии {}… {}%').format(
                version, percent))

        def failed():
            self.update_button.configure(state='normal')
            self.update_label.configure(text=_('Version {} is available.', 'Доступна версия {}.').format(version))
        if self.install_version(version, url, page, progress, failed):
            self.update_button.configure(state='disabled')

    def install_version(self, version, url, page, progress, failed):
        """Downloads release `version` and lets its launcher copy itself over this installation
        (saves, settings and mods stay). False when nothing was started."""
        if self.process:
            self.messagebox.showinfo('Bloodborne', _('Close the game before updating.', 'Закройте игру перед обновлением.'))
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

        def work():
            try:
                shutil.rmtree(UPDATE_DIR, ignore_errors=True)
                UPDATE_DIR.mkdir(parents=True, exist_ok=True)
                archive = UPDATE_DIR / 'update.zip'
                request = urllib.request.Request(url, headers={'User-Agent': 'bbport-launcher'})
                with urllib.request.urlopen(request, timeout=60) as response, open(archive, 'wb') as out:
                    total, done, shown = int(response.headers.get('Content-Length') or 0), 0, -1
                    while chunk := response.read(1 << 20):
                        out.write(chunk)
                        done += len(chunk)
                        percent = done * 100 // total if total else 0
                        if percent != shown:
                            shown = percent
                            self.ui_calls.put(lambda p=percent: progress(p))
                with zipfile.ZipFile(archive) as package:
                    package.extractall(UPDATE_DIR / 'new')
                archive.unlink()
                new = next((p.parent for p in (UPDATE_DIR / 'new').rglob('Bloodborne.exe')), None)
                if not new:
                    raise OSError('Bloodborne.exe is missing from the download')
                # The new launcher copies itself over this installation once this one has closed.
                subprocess.Popen([str(new / 'Bloodborne.exe'), '--install-update', str(PORT_DIR), str(os.getpid())],
                                 cwd=str(new), stdin=subprocess.DEVNULL, creationflags=NO_WINDOW)
                self.ui_calls.put(self.root.destroy)
            except (OSError, zipfile.BadZipFile) as failure:
                def show_failure(error=failure):
                    failed()
                    self.messagebox.showerror('Bloodborne', _('Update failed: {}', 'Не удалось обновить: {}').format(error))
                self.ui_calls.put(show_failure)
        threading.Thread(target=work, daemon=True).start()
        return True

    def choose_version(self):
        """Advanced > Other versions: any release from OLDEST_INSTALLABLE on, chosen by the player."""
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
            install.configure(state='normal')
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

            def progress(percent):
                if window.winfo_exists():
                    status.configure(text=_('Downloading version {}… {}%', 'Загрузка версии {}… {}%').format(
                        version, percent))

            def failed():
                if window.winfo_exists():
                    install.configure(state='normal')
                    box.configure(state='readonly')
                    status.configure(text=_('Installed: v{}', 'Установлена: v{}').format(VERSION))
            if self.install_version(version, url, page, progress, failed):
                install.configure(state='disabled')
                box.configure(state='disabled')

        install.configure(command=on_install)
        notes.configure(command=lambda: (release := chosen()) and webbrowser.open(release[2]))

        def fetch():
            try:
                versions = installable_releases()
            except (OSError, ValueError, KeyError, TypeError) as failure:
                self.ui_calls.put(lambda error=failure: failed_list(error))
                return
            self.ui_calls.put(lambda: show(versions))
        threading.Thread(target=fetch, daemon=True).start()

    def clear_cache(self):
        """Shader and pipeline caches; they are rebuilt while playing."""
        if self.process:
            return
        import shutil
        cache = cache_folder(self.app) / 'cache'
        shutil.rmtree(cache, ignore_errors=True)
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
        """Bloodborne in the Steam library (a non-Steam game that starts Play Bloodborne.exe)."""
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
        restart = False
        if bbport_steam.steam_running():
            if not self.messagebox.askyesno('Bloodborne', _('Steam has to close while the game is added; it starts '
                                                            'again afterwards. Close Steam now?')):
                return
            subprocess.Popen([str(steam / 'steam.exe'), '-shutdown'], creationflags=NO_WINDOW)
            for _i in range(60):
                time.sleep(0.5)
                if not bbport_steam.steam_running():
                    break
            else:
                self.messagebox.showerror('Bloodborne', _('Steam did not close. Close it yourself and try again.'))
                return
            time.sleep(1)  # Steam writes its files as it exits
            restart = True
        try:
            for config in configs:
                bbport_steam.add_shortcut(config / 'shortcuts.vdf', 'Bloodborne', str(exe), str(PORT_DIR),
                                          str(exe), options)
        except (OSError, ValueError) as error:
            self.messagebox.showerror('Bloodborne', _('Could not add the game to Steam: {}').format(error))
            return
        if restart:
            os.startfile('steam://open/games')
        self.messagebox.showinfo('Bloodborne', _('Bloodborne is in your Steam library. It starts with the settings '
                                                 'saved in this launcher.'))

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
    settings = load_json(CONFIG_FILE, {})
    settings['window_last'] = display.size_text(size)
    try:
        CONFIG_FILE.write_text(json.dumps(settings, indent=2, ensure_ascii=False), encoding='utf-8')
    except OSError:
        pass


def play_without_window(settings):
    """--play: the game with the saved settings (shortcuts, Steam). Output goes to the console
    when there is one and to <saves folder>/last_run.log."""
    attach_stdio()
    values, lines = load_ini(settings)
    graphics = values
    if settings.get('output_display'):  # the game monitor's own size, as the launcher's Play does
        m = display.monitor(settings.get('game_monitor', ''))
        if m and values.get('output_res') != display.size_text(m.size):
            values['output_res'] = display.size_text(m.size)
            save_ini({key: values[key] for key in INI_DEFAULTS}, lines)
    if settings.get('auto_backup', True):
        try:
            saves.auto_backup(Path(settings.get('user_dir') or DATA_DIR / 'user'), settings['game_dir'])
        except OSError:
            pass  # never in the way of a start
    placer = display.Placer(settings, display.parse_size(values.get('output_res')) or (1920, 1080)).start()
    if graphics.get('upscaler') == 'fsr411':
        env = bbport_fsr411.environment(PORT_DIR, settings)
        support = bbport_fsr411.query(PORT_DIR, env, refresh=True)
        status = bbport_fsr411.method_status(PORT_DIR, DATA_DIR, graphics['fsr411_backend'],
            support, fetch_fsr4vk, env)
        if not status['supported']:
            text = bbport_fsr411.message(status, _)
            print('FSR 4.1.1: ' + text)
            return None  # Show the launcher controls instead of a popup.
    log_dir = Path(settings.get('user_dir') or DATA_DIR / 'user')
    log_dir.mkdir(parents=True, exist_ok=True)
    tracker = trophies.Tracker(log_dir)
    with open(log_dir / 'last_run.log', 'w', encoding='utf-8', buffering=1) as log:
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
    with urllib.request.urlopen(request, timeout=15) as response:
        release = json.load(response)
    version = '.'.join(re.findall(r'\d+', release['tag_name']))
    url = next((asset['browser_download_url'] for asset in release.get('assets', [])
                if asset.get('name', '').lower().endswith('.zip')), None)
    return version, url, release.get('html_url') or RELEASES_PAGE


def installable_releases():
    """[(version, zip URL, page URL)] of the published releases from OLDEST_INSTALLABLE on, newest
    first (drafts, pre-releases and releases without a zip left out)."""
    request = urllib.request.Request(RELEASES_LIST_API, headers={'Accept': 'application/vnd.github+json',
                                                                 'User-Agent': 'bbport-launcher'})
    with urllib.request.urlopen(request, timeout=15) as response:
        releases = json.load(response)
    found = {}
    for release in releases:
        tag = release.get('tag_name', '')
        if release.get('draft') or release.get('prerelease') or not tag.startswith('windows-v'):
            continue
        version = '.'.join(re.findall(r'\d+', tag))
        if not version or version_tuple(version) < version_tuple(OLDEST_INSTALLABLE):
            continue
        url = next((asset['browser_download_url'] for asset in release.get('assets', [])
                    if asset.get('name', '').lower().endswith('.zip')), None)
        if url and version not in found:
            found[version] = (version, url, release.get('html_url') or RELEASES_PAGE)
    return sorted(found.values(), key=lambda item: version_tuple(item[0]), reverse=True)


def install_update(target, wait_pid):
    """--install-update TARGET PID, run by the downloaded version from its temporary folder:
    waits for the old launcher to close, copies this version over TARGET (never the saves,
    settings or mods) and starts it."""
    target = Path(target)
    kernel = ctypes.windll.kernel32
    handle = kernel.OpenProcess(0x00100000, False, int(wait_pid))  # SYNCHRONIZE
    if handle:
        kernel.WaitForSingleObject(handle, 60000)
        kernel.CloseHandle(handle)
    ignore = shutil.ignore_patterns(*USER_FILES)
    for _attempt in range(30):
        try:
            shutil.copytree(PORT_DIR, target, dirs_exist_ok=True, ignore=ignore)
            break
        except OSError:  # a file still in use: the old launcher is closing
            time.sleep(1)
    else:
        ctypes.windll.user32.MessageBoxW(None, _(
            'Could not install the update. Download it from the releases page.',
            'Не удалось установить обновление. Скачайте его со страницы релизов.'), 'Bloodborne', 0x10)
        webbrowser.open(RELEASES_PAGE)
        return 1
    subprocess.Popen([str(target / 'Bloodborne.exe')], cwd=str(target))
    return 0


class _ThisModule:
    """bbport_setup's view of this module: its globals when used (the tests load this file under
    another name, and they and the preview tools change PORT_DIR and others after the import)."""

    def __getattr__(self, name):
        try:
            return globals()[name]
        except KeyError:
            raise AttributeError(name) from None


bbport_setup.bind(_ThisModule())
bbport_help.bind(_ThisModule())
bbport_search.bind(_ThisModule())
bbport_pad.bind(_ThisModule())


def main():
    global LANG
    args = sys.argv[1:]
    if args and args[0] in ('--run', '--script'):
        sys.exit(run_role(args))
    if args[:1] == ['--after'] and len(args) >= 2:
        wait_for_exit(args[1])
        args = args[2:]
    settings = {**APP_DEFAULTS, **load_json(CONFIG_FILE, {})}
    LANG = settings.get('ui_language') or windows_language()
    saves.WORDS.update({key: _(text) for key, text in saves.REASONS.items()})  # backup file names
    if args[:1] == ['--install-update'] and len(args) == 3:
        sys.exit(install_update(args[1], args[2]))
    # The route benchmark of the 1.9.3 test builds left its own save in place of the player's when it
    # did not finish (closed launcher, crash): the player's comes back.
    try:
        user = Path(settings.get('user_dir') or DATA_DIR / 'user')
        if (user / 'savedata.benchmark-backup').is_dir():
            shutil.rmtree(user / 'savedata', ignore_errors=True)
            (user / 'savedata.benchmark-backup').rename(user / 'savedata')
    except OSError:
        pass
    if FROZEN and UPDATE_DIR not in PORT_DIR.parents:
        shutil.rmtree(UPDATE_DIR, ignore_errors=True)  # what a finished update left behind
    # Without a usable game folder there is nothing to play yet: open the launcher instead.
    page = 'play'
    if '--play' in args and (Path(settings['game_dir'] or '.') / 'eboot.bin').is_file() and CONFIG_FILE.exists():
        result = play_without_window(settings)
        if result is not None:
            sys.exit(result)
        page = 'graphics'
    try:
        ctypes.windll.shcore.SetProcessDpiAwareness(1)  # sharp text on scaled displays
    except (AttributeError, OSError):
        pass
    if not one_launcher():
        sys.exit(0)  # the open launcher comes to the front
    import tkinter as tk
    from tkinter import filedialog, messagebox, ttk
    root = tk.Tk()
    launcher = Launcher(root, tk, ttk, filedialog, messagebox)
    bring_to_front_on_request(lambda: launcher.ui_calls.put(launcher.bring_back))
    if page != 'play':
        launcher.show(page)
    root.mainloop()


if __name__ == '__main__':
    main()
