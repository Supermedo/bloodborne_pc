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
DATA_DIR = Path(os.environ.get('BB_DATA_DIR', PORT_DIR))
# Co-op test build: its own settings, and no updates (a public release would replace it).
COOP_TEST = False
CONFIG_DIR = Path(os.environ.get('APPDATA', Path.home())) / ('bbport-launcher-coop' if COOP_TEST else 'bbport-launcher')
CONFIG_FILE = CONFIG_DIR / 'settings.json'
PATCH_VERSION = '01.09'
MAX_LOG_LINES = 6000
NO_WINDOW = getattr(subprocess, 'CREATE_NO_WINDOW', 0)
# This build; GitHub release tags are windows-v<VERSION>.
VERSION = '1.8.2'
# Co-op: the community server that answers the game's own server requests.
COMMUNITY_SERVER = 'https://thehuntersdream.com'
RELEASES_API = 'https://api.github.com/repos/Supermedo/bloodborne_pc/releases/latest'
RELEASES_PAGE = 'https://github.com/Supermedo/bloodborne_pc/releases/latest'
UPDATE_DIR = Path(tempfile.gettempdir()) / 'bbport-update'
# Never copied over an installation by an update (the package does not hold them either).
USER_FILES = ('user', 'out', 'mods', 'bbport.ini', 'input.ini', 'mods.json', 'patches.json', 'last_run.log')


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


def run_command():
    """The command that starts run.py: this executable when frozen, else Python."""
    return [sys.executable, '--run'] if FROZEN else [sys.executable, str(PORT_DIR / 'run.py')]


# ---------------------------------------------------------------------------------------------
# Languages: every text is written in English with the Russian next to it; the other
# languages are in bbport_lang.py, keyed by the English text.

sys.path.insert(0, str(Path(__file__).resolve().parent))
import bbport_lang  # noqa: E402
import bbport_trophies as trophies  # noqa: E402
from bbport_input_config import (KEYBOARD_OUTPUTS, BUTTON_OUTPUTS, STICK_OUTPUTS, OUTPUT_LABELS, keysym_to_name,
                                  button_num_to_name, input_ini_path, load_input_ini, reserved_keys,
                                  save_input_ini, save_mouse_settings, wheel_to_name)  # noqa: E402

LANG = 'en'


WARN = '  ⚠'  # marks a risky choice; the text before it is translated as usual


def _(en, ru=None):
    if en.endswith(WARN):
        return _(en[:-len(WARN)], ru and ru.removesuffix(WARN)) + WARN
    if LANG == 'ru':
        return ru or bbport_lang.table('ru').get(en) or en
    return bbport_lang.table(LANG).get(en) or en


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
]
# Neural rendering (bbport_neural_settings.h): one nr_* key per setting, written to the same
# bbport.ini the in-game menu edits. Passes past the pass count are ignored by the game.
NR_PASS_COUNT = 4
NR_PASS_DEFAULTS = (('enabled', '1'), ('stage', 'post'), ('style', '0'), ('intensity', '1.00'),
                    ('structure', '1.00'), ('tone', '1.00'), ('auto_skin', '0'), ('skin_structure', '1.00'))
NR_DEFAULTS = {'nr_enabled': '0', 'nr_preset': '0', 'nr_input_scale': '1.00', 'nr_resolve': 'auto',
               'nr_ui_correction': '1', 'nr_hotkey': '86', 'nr_passes': '1',
               'nr_transfer_strength': '1.00', 'nr_colour_strength': '1.00', 'nr_max_ratio': '2.00',
               'nr_white_point': '1.00',
               **{f'nr_pass{i}_{field}': default for i in range(1, NR_PASS_COUNT + 1)
                  for field, default in NR_PASS_DEFAULTS}}
NR_FLAGS = {'nr_enabled', 'nr_ui_correction',
            *(f'nr_pass{i}_{field}' for i in range(1, NR_PASS_COUNT + 1)
              for field in ('enabled', 'auto_skin'))}
NR_FLOATS = {'nr_input_scale', 'nr_transfer_strength', 'nr_colour_strength', 'nr_max_ratio',
             'nr_white_point',
             *(f'nr_pass{i}_{field}' for i in range(1, NR_PASS_COUNT + 1)
               for field in ('intensity', 'structure', 'tone', 'skin_structure'))}


def nr_pass_add(values, count):
    """The pass chain with one more pass: the new slot starts from its defaults.

    Values is a {key: text} mapping of the nr_* settings (see nr_pass_values); the returned copy
    keeps every other key untouched. At NR_PASS_COUNT the chain is left as it is.
    """
    if count >= NR_PASS_COUNT:
        return dict(values)
    out = dict(values)
    for field, default in NR_PASS_DEFAULTS:
        out[f'nr_pass{count + 1}_{field}'] = default
    out['nr_passes'] = str(count + 1)
    return out


def nr_value_text(key, value):
    """The ini text of a pass value: flags as 1/0, floats with two decimals."""
    if key in NR_FLAGS:
        return '1' if value in (True, 1, '1') else '0'
    if key in NR_FLOATS:
        return f'{float(value):.2f}'
    return str(value)


def nr_pass_delete(values, removed, count):
    """The pass chain without pass `removed` (1-based): later passes move up and keep their
    settings, the freed last slot returns to its defaults."""
    if count <= 1 or not 1 <= removed <= count:
        return dict(values)
    out = dict(values)
    for i in range(removed, count):
        for field, _default in NR_PASS_DEFAULTS:
            out[f'nr_pass{i}_{field}'] = values[f'nr_pass{i + 1}_{field}']
    for field, default in NR_PASS_DEFAULTS:
        out[f'nr_pass{count}_{field}'] = default
    out['nr_passes'] = str(count - 1)
    return out


INI_FLAGS = {'sharpen', 'object_motion', 'show_fps', *NR_FLAGS,
             *(k for k, _t, _o in EFFECTS + EXTRAS + CHEATS + TWEAKS)}
INI_FLOATS = {'sharpness', *NR_FLOATS}
INI_DEFAULTS = {'upscaler': 'fsr4', 'frame_generation': 'off', 'fg_backend': 'dlss', 'reflex': 'on',
                'preset': '1', 'sharpen': '1', 'sharpness': '0.50',
                'object_motion': '1', 'show_fps': '1', 'output_res': '1920x1080', 'model_lod': '0',
                'live_resolution': 'auto', 'hud_position': '0', **NR_DEFAULTS,
                **{key: '1' if on else '0' for key, _t, on in EFFECTS + EXTRAS + CHEATS + TWEAKS}}
APP_DEFAULTS = {'ui_language': '', 'game_dir': str(PORT_DIR.parent / 'CUSA03173'), 'user_dir': '', 'dlc_dir': '',
                'mods_dir': '', 'mods_enabled': True, 'patches_dir': '', 'language': '1',
                'voice_language': 'auto',
                'player_name': '', 'fullscreen': False, 'hdr': False, 'present_mode': 'Mailbox',
                'fps_mode': 'uncap', 'frame_cap': '', 'draw_pipe': '', 'readbacks': '',
                'frames_ahead': '', 'frame_stats': False, 'gpu_profile': False,
                'vk_validation': False, 'extra_env': '', 'close_on_play': False,
                'check_updates': True, 'online': False, 'online_server': 'srv.shadps4.net:31313',
                'online_webapi': '', 'online_npid': '', 'online_password': '', 'online_upnp': True,
                'online_community': COMMUNITY_SERVER, 'online_pool': 'shadnet', 'bridge_live_map': False,
                'supermedo_user': '', 'supermedo_password': '', 'thd_online_id': '', 'thd_token': ''}
# bbbridge's own app reads and writes the online_* keys above: keep their names.

FG_BACKENDS = [('dlss', ('DLSS Frame Generation', 'DLSS генерация кадров')),
               ('fsr', ('FSR Frame Generation', 'FSR генерация кадров'))]
FG_MODES_DLSS = [('off', ('Off', 'Выключено')),
                 ('2x', ('2x (Double FPS)', '2x (Удвоение FPS)')),
                 ('3x', ('3x (Triple FPS)', '3x (Утроение FPS)')),
                 ('4x', ('4x (Quadruple FPS)', '4x (Четырёхкратный FPS)'))]
FG_MODES_FSR = [('off', ('Off', 'Выключено')),
                ('2x', ('2x (Double FPS)', '2x (Удвоение FPS)'))]
REFLEX_MODES = [('off', ('Off', 'Выключено')),
                ('on', ('On', 'Включено')),
                ('boost', ('On + Boost', 'Включено + Boost'))]
UPSCALERS = [('dlss', ('DLSS (NVIDIA GeForce RTX)',)),
             ('fsr4', ('FSR 4 (best quality)', 'FSR 4 (лучшее качество)')),
             ('fsr411', ('FSR 4.1.1 (needs fsr4_411 assets)', 'FSR 4.1.1 (нужны ассеты fsr4_411)')),
             ('fsr3', ('FSR 3.1 (every GPU)', 'FSR 3.1 (любая видеокарта)')),
             ('taa', ('TAA (native resolution anti-aliasing)', 'TAA (нативное сглаживание)')),
             ('off', ('Off', 'Выключен'))]
PRESETS = [('0', ('Native AA (×1.0)',)), ('1', ('Quality (×1.5)',)), ('2', ('Balanced (×1.7)',)),
           ('3', ('Performance (×2)',)), ('4', ('Ultra Performance (×3)',))]
OUTPUTS = [('1280x720', ('1280 × 720 (Steam Deck)',)), ('1920x1080', ('1920 × 1080',)),
           ('2560x1440', ('2560 × 1440',)), ('3840x2160', ('3840 × 2160 (4K)',)),
           ('1280x800', ('1280 × 800 (16:10)',)), ('2560x1080', ('2560 × 1080 (21:9)',)),
           ('3440x1440', ('3440 × 1440 (21:9)',)), ('3840x1600', ('3840 × 1600 (21:9)',)),
           ('5120x2160', ('5120 × 2160 (21:9)',)), ('3840x1080', ('3840 × 1080 (32:9)',)),
           ('5120x1440', ('5120 × 1440 (32:9)',)), ('1080x1920', ('1080 × 1920 (Portrait)',))]
HUD_POSITIONS = [('100', ('Screen edges', 'У краёв экрана')), ('75', ('75%',)), ('50', ('Halfway', 'Посередине')),
                 ('25', ('25%',)), ('0', ('Centred (16:9)', 'По центру (16:9)'))]
LIVE = [('auto', ('Auto (by graphics card)', 'Авто (по видеокарте)')), ('0', ('Off (faster)', 'Выключена (быстрее)')),
        ('1', ('On (change without restarting)', 'Включена (без перезапуска)'))]
LODS = [('0', ('As in the game', 'Как в игре')), ('-2', ('Highest (−2)', 'Максимальная (−2)')),
        ('1', ('Lower (1)', 'Ниже (1)')), ('2', ('Lowest (2)', 'Минимальная (2)'))]
# Neural rendering (DLSS NR). The hotkey values are SDL scancodes, as in bbport_neural_settings.h.
NR_PRESETS = [('0', ('Automatic',)), ('1', ('Original',)), ('2', ('Optimized',))]
NR_RESOLVE = [('auto', ('Auto (1:1 at a 100% input scale)',)),
              ('residual', ('Residual (Magpie + Lanczos3, low detail)',)),
              ('ratio', ('Ratio (luma-ratio resolve)',))]
NR_STAGES = [('pre', ('Pre-upscale',)), ('post', ('Post-upscale',)), ('present', ('Present',))]
NR_STYLES = [('0', ('Style 0',)), ('1', ('Style 1',)), ('2', ('Style 2',))]
def nr_runtime_version():
    """Read the selected model's fixed file version without loading its executable code."""
    directory = Path(os.environ.get('BB_NR_RUNTIME') or PORT_DIR / 'bin')
    if directory.suffix.lower() == '.dll':
        directory = directory.parent
    try:
        from ctypes import wintypes
        library = ctypes.WinDLL('version.dll')
        size_fn = library.GetFileVersionInfoSizeW
        size_fn.argtypes = [wintypes.LPCWSTR, ctypes.POINTER(wintypes.DWORD)]
        size_fn.restype = wintypes.DWORD
        info_fn = library.GetFileVersionInfoW
        info_fn.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p]
        info_fn.restype = wintypes.BOOL
        query_fn = library.VerQueryValueW
        query_fn.argtypes = [ctypes.c_void_p, wintypes.LPCWSTR, ctypes.POINTER(ctypes.c_void_p), ctypes.POINTER(wintypes.UINT)]
        query_fn.restype = wintypes.BOOL
        path = str(directory / 'nvngx_dlssnr.dll')
        ignored = wintypes.DWORD()
        size = size_fn(path, ctypes.byref(ignored))
        if not size:
            return None
        data = ctypes.create_string_buffer(size)
        if not info_fn(path, 0, size, data):
            return None
        value, length = ctypes.c_void_p(), wintypes.UINT()
        if not query_fn(data, '\\', ctypes.byref(value), ctypes.byref(length)) or not value.value or length.value < 52:
            return None
        info = ctypes.cast(value, ctypes.POINTER(wintypes.DWORD))
        if info[0] != 0xfeef04bd:
            return None
        return (info[2] >> 16, info[2] & 0xffff, info[3] >> 16, info[3] & 0xffff)
    except (OSError, AttributeError, ValueError):
        return None


def nr_optimized_supported(version):
    return version is not None and version >= (310, 9, 16, 0)


_nr_sdl = None
# Windows set-1 positions of printable keys. Translate the physical position rather than
# the layout's glyph: e.g. German Z/Y must bind the key that was actually pressed.
NR_SCAN_NAMES = {**dict(zip(range(0x10, 0x1a), 'QWERTYUIOP')),
                 **dict(zip(range(0x1e, 0x27), 'ASDFGHJKL')),
                 **dict(zip(range(0x2c, 0x33), 'ZXCVBNM')),
                 **dict(zip(range(0x02, 0x0c), '1234567890')),
                 0x0c: '-', 0x0d: '=', 0x1a: '[', 0x1b: ']', 0x27: ';',
                 0x28: "'", 0x29: '`', 0x2b: '\\', 0x33: ',', 0x34: '.', 0x35: '/'}


def nr_keyboard():
    """Use SDL's own key names/scancodes, without initializing video or the GPU."""
    global _nr_sdl
    if _nr_sdl is None:
        path = PORT_DIR / 'bin' / 'SDL3.dll'
        if not path.is_file():
            path = next((Path(folder) / 'SDL3.dll' for folder in os.get_exec_path()
                         if (Path(folder) / 'SDL3.dll').is_file()), path)
        library = ctypes.CDLL(str(path))
        library.SDL_GetScancodeFromName.argtypes = [ctypes.c_char_p]
        library.SDL_GetScancodeFromName.restype = ctypes.c_int
        library.SDL_GetScancodeName.argtypes = [ctypes.c_int]
        library.SDL_GetScancodeName.restype = ctypes.c_char_p
        _nr_sdl = library
    return _nr_sdl


def nr_scancode(keysym, keycode=None):
    if os.name == 'nt' and keycode is not None:
        scan = ctypes.windll.user32.MapVirtualKeyW(int(keycode), 4)
        if scan in NR_SCAN_NAMES:
            return nr_keyboard().SDL_GetScancodeFromName(NR_SCAN_NAMES[scan].encode('ascii'))
    name = keysym_to_name(keysym)
    aliases = {'enter': 'Return', 'pgup': 'PageUp', 'pgdown': 'PageDown',
               'lshift': 'Left Shift', 'rshift': 'Right Shift', 'lctrl': 'Left Ctrl',
               'rctrl': 'Right Ctrl', 'lalt': 'Left Alt', 'ralt': 'Right Alt',
               'lwin': 'Left GUI', 'rwin': 'Right GUI', 'lmeta': 'Left GUI', 'rmeta': 'Right GUI',
               'kpperiod': 'Keypad .', 'kpcomma': 'Keypad ,', 'kpslash': 'Keypad /',
               'kpasterisk': 'Keypad *', 'kpminus': 'Keypad -', 'kpplus': 'Keypad +',
               'kpequals': 'Keypad =', 'kpenter': 'Keypad Enter', 'grave': '`', 'minus': '-',
               'equals': '=', 'lbracket': '[', 'rbracket': ']', 'backslash': '\\',
               'semicolon': ';', 'apostrophe': "'", 'comma': ',', 'period': '.', 'slash': '/',
               'pausebreak': 'Pause'}
    if name and re.fullmatch(r'kp\d', name):
        name = 'Keypad ' + name[-1]
    elif name:
        name = aliases.get(name, name)
    elif re.fullmatch(r'F(?:1\d|2[0-4])', keysym):
        name = keysym
    if not name:
        return 0
    return nr_keyboard().SDL_GetScancodeFromName(name.encode('utf-8'))


def nr_hotkey_name(value):
    try:
        code = int(value)
    except (TypeError, ValueError):
        code = 0
    if code == 0:
        return _('(none)', '(нет)')
    if code == 86:
        return _('Keypad minus', 'Минус на цифровой клавиатуре')
    try:
        name = nr_keyboard().SDL_GetScancodeName(code)
        if name:
            name = name.decode('utf-8')
            if os.name == 'nt' and name in NR_SCAN_NAMES.values():
                scan = next(k for k, v in NR_SCAN_NAMES.items() if v == name)
                vk = ctypes.windll.user32.MapVirtualKeyW(scan, 3)
                glyph = ctypes.windll.user32.MapVirtualKeyW(vk, 2) & 0x7fffffff
                if glyph and chr(glyph).isprintable():
                    return chr(glyph)
            return name
    except OSError:
        pass
    return _('Key {}', 'Клавиша {}').format(code)

# Slider ranges of the neural pass and of the Ratio resolve. The style parameters are verified at
# 0..1 and the Ratio shader clamps transfer and colour to 0..1, so a wider slider would do nothing.
NR_STYLE_MAX = 1.0
NR_SKIN_MIN = -1.0
NR_SKIN_MAX = 1.0
NR_TRANSFER_MAX = 1.0
NR_COLOUR_MAX = 1.0
NR_MAX_RATIO = (1.0, 8.0)
NR_WHITE_POINT = (0.01, 100.0)
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


def load_ini():
    values, lines = dict(INI_DEFAULTS), []
    try:
        lines = ini_path().read_text(encoding='utf-8').splitlines()
    except OSError:
        pass
    for line in lines:
        if '=' in line and not line.lstrip().startswith('#'):
            key, value = line.split('=', 1)
            values[key.strip()] = value.strip()
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
# The SuperMedo Server co-op pool: a public shadNet server. Accounts are made at
# SUPERMEDO_SIGNUP. The launcher shows the pool's name, not its address.
SUPERMEDO_SERVER = 'www.supermedo.com:31313'
SUPERMEDO_SIGNUP = 'https://www.supermedo.com'


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


def game_environment(s, ini=None):
    env = dict(os.environ)
    if ini:
        for ini_k, env_k in (('frame_generation', 'BB_FRAME_GENERATION'),
                             ('fg_backend', 'BB_FG_BACKEND'),
                             ('reflex', 'BB_REFLEX'),
                             ('upscaler', 'BB_UPSCALER'),
                             ('output_res', 'BB_OUTPUT_RES')):
            if ini_k in ini and not env.get(env_k):
                env[env_k] = str(ini[ini_k])
    env['BB_GAME_DIR'] = s['game_dir']
    if s['user_dir']:
        env['BB_USER_DIR'] = s['user_dir']
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
    for key, name in (('draw_pipe', 'BB_DRAW_PIPE'), ('readbacks', 'BB_READBACKS'), ('frames_ahead', 'BB_FRAMES_AHEAD')):
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
    if s.get('online'):
        server = str(s['online_server']).strip() or APP_DEFAULTS['online_server']
        env['BB_ONLINE'] = '1'
        env['BB_SHADNET_SERVER'] = server
        env['BB_SHADNET_WEBAPI'] = webapi_setting(s)
        env['BB_SHADNET_NPID'] = str(s['online_npid']).strip()
        env['BB_SHADNET_PASSWORD'] = str(s['online_password'])
        env['BB_UPNP'] = '1' if s['online_upnp'] else '0'
        env['BB_COMMUNITY_SERVER'] = community_address(s['online_community'])
        if s.get('online_pool') == 'supermedo':
            env['BB_SHADNET_SERVER'] = SUPERMEDO_SERVER
            env['BB_SHADNET_WEBAPI'] = webapi_address(SUPERMEDO_SERVER)
            env['BB_POOL_NAME'] = 'SuperMedo Server'
            env['BB_SHADNET_NPID'] = str(s.get('supermedo_user', '')).strip()
            env['BB_SHADNET_PASSWORD'] = str(s.get('supermedo_password', ''))
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
    env.setdefault('BB_CLIENT_VERSION', f'bbport {VERSION}')
    env['PYTHONUNBUFFERED'] = '1'
    env['PYTHONIOENCODING'] = 'utf-8'
    return env


# ---------------------------------------------------------------------------------------------
# The window.

BG, PANEL, CARD, LINE = '#0e0c0b', '#151210', '#1c1815', '#2e2722'
TEXT, MUTED, GOLD, BLOOD, BLOOD_HI = '#e9e2d6', '#9a8f80', '#c8a96a', '#7c1717', '#9e2222'
# Trophy glyphs only: the grade colours of the console, toned to the launcher's palette.
TROPHY_COLORS = {trophies.PLATINUM: '#c4ccd8', trophies.GOLD: GOLD, trophies.SILVER: '#adb1b7',
                 trophies.BRONZE: '#b0774f'}
TROPHY_LOCKED = '#3b332c'
GRADE_NAMES = {trophies.PLATINUM: ('Platinum', 'Платиновый'), trophies.GOLD: ('Gold', 'Золотой'),
               trophies.SILVER: ('Silver', 'Серебряный'), trophies.BRONZE: ('Bronze', 'Бронзовый')}


class Launcher:
    def __init__(self, root, tk, ttk, filedialog, messagebox):
        self.tk, self.ttk, self.filedialog, self.messagebox = tk, ttk, filedialog, messagebox
        self.root = root
        self.app = {**APP_DEFAULTS, **load_json(CONFIG_FILE, {})}
        self.ini, self.ini_lines = load_ini()
        self.remap_ini_path = input_ini_path(ini_path())
        self.remap_bindings, self.remap_lines, self.remap_toggle_key, self.remap_reload_key, \
            self.mouse_settings = load_input_ini(self.remap_ini_path)
        # {output: first-binding key name, or None once cleared}, edited by the remap page and
        # applied on top of self.remap_bindings in collect(); separate from it so re-opening the
        # Controls page after a cancelled edit shows what is actually on disk, not a half-edit.
        self.remap_edits = {}
        self.mouse_edited = False  # set by any mouse widget's command/trace; gates the save in collect()
        self.vars = {}
        self.process = self.job = None
        self.hidden = False  # closed while the game runs: it exits with the game
        self.downloading = False
        self.output = queue.Queue()
        self.gpu_text = _('• Checking the graphics card…', '• Проверка видеокарты…')
        self.banner_source = self.banner_image = None
        self.ui_calls = queue.Queue()  # work for the Tk thread from helper threads
        self.mod_order, self.mod_vars, self.patch_vars = [], {}, {}
        self.tracker = trophies.Tracker(self.trophy_dir())
        self.trophies_loaded, self.toasts = False, []
        root.title('Bloodborne — bbport')
        root.configure(bg=BG)
        self.dpi = root.winfo_fpixels('1i') / 96.0
        root.geometry(f'{self.px(1120)}x{self.px(740)}')
        root.minsize(self.px(980), self.px(660))
        self.set_icon()
        self.style()
        self.build()
        self.show('play')
        root.protocol('WM_DELETE_WINDOW', self.close)
        root.after(100, self.drain_output)
        root.after(300, self.poll_trophies)
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
    def check_images(self):
        """16 px (DPI-scaled) check box images: the clam theme draws a cross."""
        tk = self.tk
        n = max(14, int(16 * self.dpi))
        images = []
        for checked in (False, True):
            image = tk.PhotoImage(width=n + self.px(8), height=n)  # unset pixels stay transparent
            fill = BLOOD if checked else CARD
            image.put(GOLD if checked else '#5a4c40', to=(0, 0, n, n))
            image.put(fill, to=(1, 1, n - 1, n - 1))
            if checked:
                # Tick: down from (0.22n, 0.52n) to (0.42n, 0.72n), up to (0.78n, 0.30n).
                pts = []
                x0, y0, x1, y1, x2, y2 = .22 * n, .52 * n, .42 * n, .72 * n, .78 * n, .28 * n
                for t in range(40):
                    f = t / 39
                    pts.append((x0 + (x1 - x0) * f, y0 + (y1 - y0) * f))
                    pts.append((x1 + (x2 - x1) * f, y1 + (y2 - y1) * f))
                width = max(1, round(n / 9))
                for x, y in pts:
                    image.put('#f4ece0', to=(int(x), int(y), int(x) + width, int(y) + width))
            images.append(image)
        self.check_off, self.check_on = images

    def style(self):
        ttk = self.ttk
        s = ttk.Style(self.root)
        s.theme_use('clam')
        self.check_images()
        s.element_create('Bb.indicator', 'image', self.check_off, ('selected', self.check_on), sticky='')
        s.layout('TCheckbutton', [('Checkbutton.padding', {'sticky': 'nswe', 'children': [
            ('Bb.indicator', {'side': 'left', 'sticky': ''}),
            ('Checkbutton.focus', {'side': 'left', 'sticky': 'w', 'children': [
                ('Checkbutton.label', {'sticky': 'nswe'})]})]})])
        base = ('Segoe UI', 10)
        self.root.option_add('*TCombobox*Listbox.background', CARD)
        self.root.option_add('*TCombobox*Listbox.foreground', TEXT)
        self.root.option_add('*TCombobox*Listbox.selectBackground', BLOOD)
        self.root.option_add('*TCombobox*Listbox.font', base)
        s.configure('.', background=PANEL, foreground=TEXT, fieldbackground=CARD, bordercolor=LINE,
                    lightcolor=LINE, darkcolor=LINE, troughcolor=CARD, focuscolor=GOLD, font=base)
        s.configure('TFrame', background=PANEL)
        s.configure('TLabel', background=PANEL, foreground=TEXT)
        s.configure('Muted.TLabel', background=PANEL, foreground=MUTED, font=('Segoe UI', 9))
        s.configure('Section.TLabel', background=PANEL, foreground=GOLD, font=('Georgia', 13))
        s.configure('Code.TLabel', background=PANEL, foreground=GOLD, font=('Consolas', 22, 'bold'))
        s.configure('TCheckbutton', background=PANEL, foreground=TEXT, padding=(0, 3))
        s.map('TCheckbutton', background=[('active', PANEL)], foreground=[('disabled', MUTED)])
        s.configure('NRPreset.Toolbutton', background=CARD, foreground=MUTED, borderwidth=1, padding=(12, 6))
        s.map('NRPreset.Toolbutton', background=[('disabled', PANEL), ('selected', LINE), ('active', LINE)],
              foreground=[('disabled', MUTED), ('selected', TEXT), ('active', TEXT)])
        s.configure('Warning.TLabel', background='#2a1d12', foreground='#e3b25a', padding=(10, 6))
        s.configure('TCombobox', arrowcolor=GOLD, foreground=TEXT, padding=4)
        s.map('TCombobox', fieldbackground=[('readonly', CARD)], foreground=[('readonly', TEXT)],
              selectbackground=[('readonly', CARD)], selectforeground=[('readonly', TEXT)])
        s.configure('TEntry', foreground=TEXT, insertcolor=TEXT, padding=4)
        s.configure('TSpinbox', foreground=TEXT, arrowcolor=GOLD, insertcolor=TEXT, padding=4)
        s.configure('TButton', background=CARD, foreground=TEXT, padding=(12, 6), borderwidth=1)
        s.map('TButton', background=[('active', LINE), ('disabled', PANEL)], foreground=[('disabled', MUTED)])
        s.configure('NeuralAdd.TButton', background=LINE, foreground=TEXT)
        s.configure('NeuralDelete.TButton', background=CARD, foreground=MUTED)
        s.configure('Neural.TNotebook', background=PANEL, bordercolor=LINE, tabmargins=(0, 0, 0, 0))
        s.configure('Neural.TNotebook.Tab', background=CARD, foreground=MUTED, bordercolor=LINE,
                    lightcolor=LINE, darkcolor=LINE, padding=(12, 6))
        s.map('Neural.TNotebook.Tab', background=[('selected', LINE), ('active', LINE)],
              foreground=[('selected', TEXT), ('active', TEXT)],
              lightcolor=[('selected', LINE)], darkcolor=[('selected', LINE)])
        s.configure('Play.TButton', background=BLOOD, foreground='#f4ece0', font=('Georgia', 15, 'bold'),
                    padding=(36, 10), borderwidth=0)
        s.map('Play.TButton', background=[('active', BLOOD_HI), ('disabled', '#3a2420')],
              foreground=[('disabled', '#8a7a70')])
        s.configure('Horizontal.TProgressbar', background=GOLD, troughcolor=CARD, bordercolor=LINE)
        s.configure('Vertical.TScrollbar', background=CARD, arrowcolor=GOLD, troughcolor=PANEL, bordercolor=PANEL)

    # ---- widget helpers ----------------------------------------------------------------------
    def var(self, key, store):
        """The Tk variable of a setting (one per setting, shared by every widget that shows it)."""
        tk = self.tk
        if key not in self.vars:
            if store == 'ini':
                value = self.ini.get(key, INI_DEFAULTS[key])
                if key in INI_FLAGS:
                    v = tk.BooleanVar(value=value == '1')
                elif key in INI_FLOATS:
                    v = tk.DoubleVar(value=float(value) if str(value).strip() else
                                     (0.5 if key == 'sharpness' else 1.0))
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

    def choice(self, parent, key, store, options, width=38):
        """A combobox over (value, (english, russian)) pairs, kept in sync with its variable."""
        var = self.var(key, store)
        values = [v for v, _t in options]
        box = self.ttk.Combobox(parent, values=[_(*text) for _v, text in options],
                               state='normal' if key == 'output_res' else 'readonly', width=width)
        box.bbport_values = values
        if var.get() not in values and key != 'output_res':
            var.set(values[0])

        def show(*_args):
            current = var.get()
            if current in box.bbport_values:
                box.current(box.bbport_values.index(current))
            else:
                box.set(current)
        show()
        box.bind('<<ComboboxSelected>>', lambda _e: var.set(box.bbport_values[box.current()]))
        if key == 'output_res':
            def custom(_event):
                if box.current() >= 0:
                    return
                text = box.get().strip().lower().replace(' ', '')
                try:
                    w, h = map(int, text.split('x'))
                    if 256 <= w <= 7680 and 144 <= h <= 4320 and not (w % 2 or h % 2):
                        var.set(f'{w}x{h}')
                    else:
                        show()
                except ValueError:
                    show()
            box.bind('<FocusOut>', custom)
            box.bind('<Return>', custom)
        var.trace_add('write', show)
        return box

    def next_row(self, parent):
        return parent.grid_size()[1]

    def row(self, parent, title, widget, hint=None):
        ttk = self.ttk
        r = self.next_row(parent)
        ttk.Label(parent, text=title).grid(row=r, column=0, sticky='nw', padx=(0, 18), pady=(8, 0))
        widget.grid(row=r, column=1, sticky='w', pady=(5, 0))
        if hint:
            ttk.Label(parent, text=hint, style='Muted.TLabel', wraplength=self.px(560), justify='left').grid(
                row=r + 1, column=1, sticky='w', pady=(2, 2))
        return widget

    def check(self, parent, key, store, title, hint=None):
        ttk = self.ttk
        r = self.next_row(parent)
        ttk.Checkbutton(parent, text=title, variable=self.var(key, store)).grid(
            row=r, column=0, columnspan=2, sticky='w', pady=(4, 0))
        if hint:
            ttk.Label(parent, text=hint, style='Muted.TLabel', wraplength=self.px(640), justify='left').grid(
                row=r + 1, column=0, columnspan=2, sticky='w', padx=(26, 0))

    def note(self, parent, text, top=12):
        self.ttk.Label(parent, text=text, style='Muted.TLabel', wraplength=self.px(680), justify='left').grid(
            row=self.next_row(parent), column=0, columnspan=2, sticky='w', pady=(top, 0))

    def scale_row(self, parent, key, title, lo, hi, hint=None):
        """A slider row for a float ini key (neural rendering), with the value shown next to it."""
        ttk = self.ttk
        holder = ttk.Frame(parent)
        ttk.Scale(holder, from_=lo, to=hi, variable=self.var(key, 'ini'), length=300).pack(side='left')
        value = ttk.Label(holder, width=6)
        value.pack(side='left', padx=10)
        show = lambda *_a: value.configure(text=f'{self.vars[key].get():.2f}')
        self.vars[key].trace_add('write', show)
        show()
        return self.row(parent, title, holder, hint)

    def section(self, parent, title, top=18):
        self.ttk.Label(parent, text=title, style='Section.TLabel').grid(
            row=self.next_row(parent), column=0, columnspan=2, sticky='w', pady=(top, 2))

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
        return self.row(parent, title, holder, hint)

    def scrolled_page(self, name, title, subtitle):
        """A settings page: a heading and content that scrolls vertically."""
        tk, ttk = self.tk, self.ttk
        outer = ttk.Frame(self.content)
        head = ttk.Frame(outer, padding=(28, 22, 28, 6))
        head.pack(fill='x')
        ttk.Label(head, text=title, background=PANEL, foreground=TEXT, font=('Georgia', 20)).pack(anchor='w')
        ttk.Label(head, text=subtitle, style='Muted.TLabel').pack(anchor='w')
        canvas = tk.Canvas(outer, bg=PANEL, highlightthickness=0, bd=0)
        bar = ttk.Scrollbar(outer, orient='vertical', command=canvas.yview)
        inner = ttk.Frame(canvas, padding=(28, 0, 28, 24))
        inner.columnconfigure(1, weight=1)
        window = canvas.create_window(0, 0, window=inner, anchor='nw')
        inner.bind('<Configure>', lambda _e: canvas.configure(scrollregion=canvas.bbox('all')))
        canvas.bind('<Configure>', lambda e: canvas.itemconfigure(window, width=e.width))
        canvas.configure(yscrollcommand=bar.set)
        bar.pack(side='right', fill='y')
        canvas.pack(side='left', fill='both', expand=True)
        outer.scroll = lambda units: canvas.yview_scroll(units, 'units') if inner.winfo_height() > canvas.winfo_height() else None
        self.pages[name] = outer
        return inner

    # ---- layout ------------------------------------------------------------------------------
    def build(self):
        tk, ttk = self.tk, self.ttk
        side = tk.Frame(self.root, bg=BG, width=self.px(220))
        side.pack(side='left', fill='y')
        side.pack_propagate(False)
        tk.Label(side, text='BLOODBORNE', bg=BG, fg=GOLD, font=('Georgia', 16)).pack(anchor='w', padx=22, pady=(24, 0))
        self.side = side
        tk.Label(side, text=_('by Super Medo'), bg=BG, fg=TEXT, font=('Segoe UI', 10)).pack(anchor='w', padx=22)
        tk.Label(side, text=_('native port · Windows', 'нативный порт · Windows') + f'  ·  v{VERSION}' + ('\n' + _('co-op test build') if COOP_TEST else ''), bg=BG, fg=MUTED, justify='left',
                 font=('Segoe UI', 9)).pack(anchor='w', padx=22, pady=(0, 20))
        self.nav, self.current_page = {}, None
        for name, title in (('play', _('Play', 'Играть')), ('graphics', _('Graphics', 'Графика')),
                            ('display', _('Display & FPS', 'Экран и FPS')), ('game', _('Game & effects', 'Игра и эффекты')),
                            ('cheats', _('Cheats', 'Читы')), ('online', _('Online co-op', 'Онлайн-кооператив')),
                            ('mods', _('Mods & patches', 'Моды и патчи')),
                            ('trophies', _('Achievements', 'Достижения')),
                            ('controls', _('Controls', 'Управление')),
                            ('advanced', _('Advanced', 'Дополнительно')),
                            ('log', _('Log', 'Журнал'))):
            item = tk.Label(side, text='    ' + title, bg=BG, fg=TEXT, anchor='w', font=('Segoe UI', 11),
                            pady=10, cursor='hand2')
            item.pack(fill='x')
            item.bind('<Button-1>', lambda _e, n=name: self.show(n))
            item.bind('<Enter>', lambda _e, n=name: n != self.current_page and self.nav[n].configure(bg='#1a1613'))
            item.bind('<Leave>', lambda _e, n=name: n != self.current_page and self.nav[n].configure(bg=BG))
            self.nav[name] = item
        tk.Frame(side, bg=BG).pack(fill='both', expand=True)
        self.update_box = None
        self.side_note = tk.Label(side, text=_('In the game: Insert or L3+R3\nopens the port\'s menu.',
                                               'В игре: Insert или L3+R3\nоткрывает меню порта.'),
                                  bg=BG, fg=MUTED, font=('Segoe UI', 9), justify='left', wraplength=self.px(210))
        self.side_note.pack(anchor='w', padx=22, pady=(0, 18))

        right = tk.Frame(self.root, bg=PANEL)
        right.pack(side='left', fill='both', expand=True)
        bar = tk.Frame(right, bg=BG, height=72)
        bar.pack(fill='x', side='bottom')
        bar.pack_propagate(False)
        self.status = tk.Label(bar, text='', bg=BG, fg=MUTED, font=('Segoe UI', 10), anchor='w', justify='left')
        self.status.pack(side='left', padx=24)
        self.play_button = ttk.Button(bar, text=_('PLAY', 'ИГРАТЬ'), style='Play.TButton', command=self.play)
        self.play_button.pack(side='right', padx=(10, 24), pady=11)
        self.stop_button = ttk.Button(bar, text=_('Stop', 'Остановить'), command=self.stop, state='disabled')
        self.stop_button.pack(side='right', pady=11)
        self.content = tk.Frame(right, bg=PANEL)
        self.content.pack(fill='both', expand=True)

        self.pages = {}
        self.build_play()
        self.build_graphics()
        self.build_display()
        self.build_game()
        self.build_cheats()
        self.build_online()
        self.build_mods()
        self.build_trophies()
        self.build_controls()
        self.build_advanced()
        self.build_log()
        self.root.bind_all('<MouseWheel>', self.wheel)
        self.refresh_status()

    def wheel(self, event):
        page = self.pages.get(self.current_page)
        if hasattr(page, 'scroll') and not isinstance(event.widget, (self.tk.Text, self.ttk.Combobox)) \
                and 'popdown' not in str(event.widget):
            page.scroll(int(-event.delta / 120))

    def show(self, name):
        for page in self.pages.values():
            page.pack_forget()
        self.pages[name].pack(fill='both', expand=True)
        self.current_page = name
        for n, item in self.nav.items():
            item.configure(bg=PANEL if n == name else BG, fg=GOLD if n == name else TEXT)
        if name == 'mods':
            self.refresh_lists()
        elif name == 'graphics':
            self.refresh_fsr4()
        elif name == 'play':
            self.refresh_status()
        elif name == 'trophies':
            self.render_trophies()

    def build_play(self):
        tk, ttk = self.tk, self.ttk
        page = tk.Frame(self.content, bg=PANEL)
        self.pages['play'] = page
        self.banner = tk.Canvas(page, height=self.px(290), bg=BG, highlightthickness=0, bd=0)
        self.banner.pack(fill='x')
        self.banner.bind('<Configure>', lambda _e: self.draw_banner())
        body = ttk.Frame(page, padding=(28, 12, 28, 8))
        body.pack(fill='both', expand=True)
        body.columnconfigure(0, weight=3)
        body.columnconfigure(1, weight=2)
        info = ttk.Frame(body)
        info.grid(row=0, column=0, sticky='nw', padx=(0, 24))
        ttk.Label(info, text=_('Ready check', 'Проверка'), style='Section.TLabel').pack(anchor='w', pady=(0, 6))
        self.checks = {}
        for key in ('game', 'saves', 'gpu', 'fsr4'):
            self.checks[key] = ttk.Label(info, text='', justify='left', wraplength=self.px(440))
            self.checks[key].pack(anchor='w', pady=3)
        quick = ttk.Frame(body)
        quick.grid(row=0, column=1, sticky='nw')
        ttk.Label(quick, text=_('Quick settings', 'Основное'), style='Section.TLabel').grid(
            row=0, column=0, columnspan=2, sticky='w', pady=(0, 2))
        self.row(quick, _('Frame rate', 'Частота кадров'), self.choice(quick, 'fps_mode', 'app', FPS_MODES, 26))
        self.row(quick, _('Upscaler', 'Апскейлер'), self.choice(quick, 'upscaler', 'ini', UPSCALERS, 26))
        self.row(quick, _('Preset', 'Пресет'), self.choice(quick, 'preset', 'ini', PRESETS, 26))
        self.row(quick, _('Output', 'Разрешение'), self.choice(quick, 'output_res', 'ini', OUTPUTS, 26))
        ttk.Checkbutton(quick, text=_('Fullscreen', 'Полный экран'), variable=self.var('fullscreen', 'app')).grid(
            row=self.next_row(quick), column=1, sticky='w', pady=(8, 0))
        ttk.Checkbutton(quick, text=_('Play online'), variable=self.var('online', 'app')).grid(
            row=self.next_row(quick), column=1, sticky='w', pady=(4, 0))
        ttk.Button(quick, text=_('Add to Steam'), command=self.add_to_steam).grid(
            row=self.next_row(quick), column=1, sticky='w', pady=(10, 0))
        for key in ('fps_mode', 'upscaler', 'output_res'):
            self.vars[key].trace_add('write', lambda *_a: self.refresh_status())

    def draw_banner(self):
        """The cover art of the selected dump (sce_sys/pic1.png) under the title."""
        tk, c = self.tk, self.banner
        c.delete('all')
        w, h = max(c.winfo_width(), 400), int(c['height'])
        art = Path(self.var('game_dir', 'app').get() or '.') / 'sce_sys' / 'pic1.png'
        if self.banner_source != art:
            self.banner_source, self.banner_image, self.banner_size = art, None, None
            try:
                self.banner_image = tk.PhotoImage(file=str(art))
            except (tk.TclError, OSError):
                pass
        if self.banner_image:
            if self.banner_size != w:
                self.banner_size, self.banner_scaled = w, self.scaled_art(art, w)
            # The logo sits in the upper half of the cover: show that part.
            c.create_image(w // 2, int(h * 0.62), image=self.banner_scaled)
            for i, stipple in enumerate(('gray12', 'gray25', 'gray50', 'gray75')):
                c.create_rectangle(0, h - 120 + i * 24, w, h - 96 + i * 24, fill=PANEL, outline='', stipple=stipple)
            c.create_rectangle(0, h - 24, w, h, fill=PANEL, outline='')
        else:
            c.create_text(w // 2, h // 2 - 20, text=_('Choose your game folder (Game & effects)',
                                                       'Выберите папку игры («Игра и эффекты»)'),
                          fill=MUTED, font=('Segoe UI', 11))
        if not self.banner_image:
            c.create_text(30, h - 70, text='Bloodborne', anchor='w', fill='#f2ead9', font=('Georgia', 36))
        info = game_info(self.var('game_dir', 'app').get())
        sub = (_('CUSA03173 · game version {}', 'CUSA03173 · версия игры {}').format(info[1]) if info
               else _('Game folder not set', 'Папка игры не выбрана'))
        c.create_text(33, h - 30, text=sub, anchor='w', fill=GOLD, font=('Segoe UI', 11))

    def scaled_art(self, path, width):
        """The cover art scaled to `width`: Pillow (smooth) or Tk's integer subsampling."""
        try:
            from PIL import Image, ImageTk
            with Image.open(path) as image:
                height = round(image.height * width / image.width)
                return ImageTk.PhotoImage(image.convert('RGB').resize((width, height), Image.LANCZOS))
        except (ImportError, OSError):
            factor = max(1, round(self.banner_image.width() / max(width, 1)))
            return self.banner_image.subsample(factor) if factor > 1 else self.banner_image

    def build_graphics(self):
        ttk = self.ttk
        f = self.scrolled_page('graphics', _('Graphics', 'Графика'),
                               _('Stored in bbport.ini; the in-game menu (Insert or L3+R3) changes the same values.',
                                 'Хранится в bbport.ini; в игре меняется через меню (Insert или L3+R3).'))
        self.section(f, _('Upscaling', 'Апскейлинг'), top=4)
        self.row(f, _('Upscaler', 'Апскейлер'), self.choice(f, 'upscaler', 'ini', UPSCALERS),
                 _("Temporal upscaling with the game's own motion vectors. FSR 4 needs its assets (below) and "
                   'a GPU with INT8 dot products; otherwise the game falls back to FSR 3.1 by itself.',
                   'Временной апскейлинг с векторами движения игры. FSR 4 нужны ассеты (ниже) и GPU с INT8; '
                   'иначе игра сама переключится на FSR 3.1.'))
        self.row(f, _('Quality preset', 'Пресет'), self.choice(f, 'preset', 'ini', PRESETS),
                 _('Render scale per axis: Quality renders at 1/1.5 of the output size.',
                   'Масштаб рендера по каждой оси: Quality рисует в 1/1.5 размера вывода.'))
        self.row(f, _('Output resolution', 'Разрешение вывода'), self.choice(f, 'output_res', 'ini', OUTPUTS),
                 _('Choose a preset or type WxH (even dimensions, 256x144 through 7680x4320).',
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
                   _('Less ghosting on characters, cloth and weapons; costs about 10% FPS.',
                     'Меньше гостинга на персонажах и одежде; стоит около 10% FPS.'))
        self.section(f, _('Frame generation & Reflex', 'Генерация кадров и Reflex'), top=12)
        backend_box = self.choice(f, 'fg_backend', 'ini', FG_BACKENDS)
        self.row(f, _('Generation backend', 'Бэкенд генерации'), backend_box,
                 _('Select DLSS or FSR frame generation independently from temporal reconstruction.',
                   'Выбор генерации кадров DLSS или FSR отдельно от реконструкции.'))

        fg_var = self.var('frame_generation', 'ini')
        fg_box = self.choice(f, 'frame_generation', 'ini',
                             FG_MODES_DLSS if self.var('fg_backend', 'ini').get() == 'dlss' else FG_MODES_FSR)
        self.row(f, _('Frame generation', 'Генерация кадров'), fg_box,
                 _('DLSS supports Off/2x/3x/4x; FSR supports Off/2x only. Hardware support required; applies on restart.',
                   'DLSS поддерживает Off/2x/3x/4x; FSR только Off/2x. Требуется поддержка оборудования; применится после перезапуска.'))

        def on_backend_change(*_args):
            b = self.var('fg_backend', 'ini').get()
            modes = FG_MODES_DLSS if b == 'dlss' else FG_MODES_FSR
            values = [v for v, _t in modes]
            fg_box.bbport_values = values
            fg_box.configure(values=[_(*t) for _v, t in modes])
            if fg_var.get() not in values:
                fg_var.set('2x' if fg_var.get() in ('3x', '4x') else values[0])
            if fg_var.get() in values:
                fg_box.current(values.index(fg_var.get()))

        self.var('fg_backend', 'ini').trace_add('write', on_backend_change)

        self.row(f, _('NVIDIA Reflex', 'NVIDIA Reflex'), self.choice(f, 'reflex', 'ini', REFLEX_MODES),
                 _('Reduces latency with frame generation. Guarded by runtime capability; applies on restart.',
                   'Снижает задержку при генерации кадров. Применяется после перезапуска.'))

        self.section(f, _('Neural rendering', 'Нейронный рендеринг'), top=14)
        self.check(f, 'nr_enabled', 'ini', _('Enable neural rendering (DLSS NR); applies on restart',
                                             'Включить нейронный рендеринг (DLSS NR); после перезапуска'))
        model_options = ttk.Frame(f)
        version = nr_runtime_version()
        self.nr_preset_buttons = {}
        for preset, label in NR_PRESETS:
            button = ttk.Radiobutton(model_options, text=_(*label), value=preset,
                                     variable=self.var('nr_preset', 'ini'), style='NRPreset.Toolbutton')
            button.pack(side='left', padx=(0, 16))
            if preset == '2' and not nr_optimized_supported(version):
                button.state(['disabled'])
            self.nr_preset_buttons[preset] = button
        self.row(f, _('NR preset', 'Пресет NR'), model_options,
                 _('Automatic: uses the default model in the installed NR DLL. '
                   'Original: uses the original neural rendering model. '
                   'Optimized: uses the smaller FP8 model; requires NR 310.9.16.0 or newer.'))
        self.scale_row(f, 'nr_input_scale', _('NR input scale', 'Масштаб входа NR'), 0.25, 1.0,
                       _('1.00 uses the full image at the selected execution point; lower '
                         'values feed it a smaller image and cost less GPU time.',
                         '1.00 — нейропроход на входном разрешении апскейлера (без даунсэмплинга); ниже — '
                         'меньше изображение и меньше нагрузка на GPU.'))
        self.row(f, _('NR resolve method', 'Метод разрешения NR'),
                 self.choice(f, 'nr_resolve', 'ini', NR_RESOLVE),
                 _('Auto: 1:1 at a 100% input scale, residual below it. Residual: Magpie + Lanczos3, low '
                   'detail. Ratio: the luma-ratio resolve with the advanced controls below.'))
        self.check(f, 'nr_ui_correction', 'ini', _('NR UI correction', 'Коррекция интерфейса NR'),
                   _('Preserves the original HUD during Present passes. Frame generation always keeps HUD protection.'))
        hotkey = ttk.Frame(f)
        self.nr_hotkey_label = ttk.Label(hotkey, width=18, style='Muted.TLabel')
        self.nr_hotkey_label.pack(side='left')
        self.nr_hotkey_button = ttk.Button(hotkey, text=_('Set…', 'Назначить…'),
                                           command=self.start_nr_hotkey_capture)
        self.nr_hotkey_button.pack(side='left', padx=(10, 4))
        ttk.Button(hotkey, text=_('Clear', 'Сбросить'), command=self.clear_nr_hotkey).pack(side='left')
        self.var('nr_hotkey', 'ini').trace_add('write', lambda *_a: self.refresh_nr_hotkey())
        self.refresh_nr_hotkey()
        self.row(f, _('NR toggle hotkey', 'Горячая клавиша NR'), hotkey)
        # The pass chain: one tab per active pass, as in the reference overlay. Adding and deleting
        # reshuffles the nr_passN_* values; the widgets read the shared ini variables, so a rebuilt
        # tab shows the moved values.
        holder = ttk.Frame(f)
        holder.grid(row=self.next_row(f), column=0, columnspan=2, sticky='we', pady=(8, 0))
        self.nr_tabs = ttk.Notebook(holder, style='Neural.TNotebook')
        self.nr_tabs.pack(side='top', fill='x')
        buttons = ttk.Frame(holder)
        buttons.pack(side='top', anchor='w', pady=(6, 0))
        self.nr_add_button = ttk.Button(buttons, text=_('+ Add pass'), style='NeuralAdd.TButton',
                                       command=self.nr_add_pass)
        self.nr_add_button.pack(side='left')
        self.nr_delete_button = ttk.Button(buttons, text=_('Delete current pass'), style='NeuralDelete.TButton',
                                           command=self.nr_delete_pass)
        self.nr_delete_button.pack(side='left', padx=(8, 0))
        self.nr_count_label = ttk.Label(buttons, text='', style='Muted.TLabel')
        self.nr_count_label.pack(side='left', padx=(12, 0))
        self.nr_rebuild_tabs()
        self.note(f, _('Passes run in order within their stage; more passes cost more GPU time. A disabled '
                       'pass keeps its settings and is skipped by the game.',
                       'Проходы выполняются по порядку внутри своей стадии; больше проходов — больше '
                       'нагрузка на GPU. Отключённый проход сохраняет настройки и пропускается игрой.'), top=6)
        self.scale_row(f, 'nr_transfer_strength', _('Ratio transfer strength'), 0.0, NR_TRANSFER_MAX)
        self.scale_row(f, 'nr_colour_strength', _('Ratio colour strength'), 0.0, NR_COLOUR_MAX)
        self.scale_row(f, 'nr_max_ratio', _('Ratio maximum'), *NR_MAX_RATIO)
        self.scale_row(f, 'nr_white_point', _('Ratio white point'), *NR_WHITE_POINT)
        self.note(f, _('The four Ratio values apply to the Ratio resolve method only.',
                       'Четыре значения Ratio действуют только для метода Ratio.'), top=6)
        self.section(f, _('FSR 4 assets', 'Ассеты FSR 4'))
        self.fsr4_label = ttk.Label(f, text='', wraplength=self.px(640), justify='left')
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
        self.section(f, _('Detail', 'Детализация'))
        self.row(f, _('Model detail (LOD)', 'Детализация моделей'), self.choice(f, 'model_lod', 'ini', LODS),
                 _('A game patch (game version 1.09).', 'Патч игры (версия 1.09).'))
        self.check(f, 'show_fps', 'ini', _('Show the FPS counter', 'Показывать FPS'))

    def refresh_nr_hotkey(self):
        self.nr_hotkey_label.configure(text=nr_hotkey_name(self.var('nr_hotkey', 'ini').get()))

    def clear_nr_hotkey(self):
        cancel = getattr(self, '_nr_capture_finish', None)
        if cancel:
            cancel()
        self.var('nr_hotkey', 'ini').set('0')

    def start_nr_hotkey_capture(self):
        """Controls-style key capture. Escape cancels; Clear disables the shortcut."""
        self.nr_hotkey_label.configure(text=_('Press a key…', 'Нажмите клавишу…'))
        self.nr_hotkey_button.configure(state='disabled')
        self.root.grab_set()
        self.root.focus_set()

        def finish():
            self._nr_capture_finish = None
            self.root.unbind_all('<KeyPress>')
            self.root.grab_release()
            self.nr_hotkey_button.configure(state='normal')
            self.refresh_nr_hotkey()

        def on_key(event):
            if event.keysym == 'Escape':
                finish()
                return 'break'
            reserved = reserved_keys(getattr(self, 'remap_toggle_key', 'f7'),
                                     getattr(self, 'remap_reload_key', 'f8'))
            if keysym_to_name(event.keysym) in reserved:
                self.nr_hotkey_label.configure(text=_('Reserved; choose another key',
                                                      'Зарезервировано; выберите другую клавишу'))
                return 'break'
            try:
                code = nr_scancode(event.keysym, event.keycode)
            except OSError:
                code = 0
            if not code:
                self.nr_hotkey_label.configure(text=_('Choose another key…', 'Выберите другую клавишу…'))
                return 'break'
            self.var('nr_hotkey', 'ini').set(str(code))
            finish()
            return 'break'

        self._nr_capture_finish = finish
        self.root.bind_all('<KeyPress>', on_key)

    def nr_pass_count(self):
        """The active number of passes (1..NR_PASS_COUNT), from the shared nr_passes setting."""
        try:
            count = int(self.var('nr_passes', 'ini').get())
        except (TypeError, ValueError):
            count = 1
        return max(1, min(NR_PASS_COUNT, count))

    def nr_pass_values(self):
        """The text of every pass key and of the pass count: the edited variable when it exists,
        otherwise the ini value (or its default)."""
        values = {}
        for i in range(1, NR_PASS_COUNT + 1):
            for field, _default in NR_PASS_DEFAULTS:
                key = f'nr_pass{i}_{field}'
                var = self.vars.get(key)
                raw = var.get() if var is not None else self.ini.get(key, INI_DEFAULTS[key])
                values[key] = nr_value_text(key, raw)
        values['nr_passes'] = str(self.nr_pass_count())
        return values

    def nr_apply_values(self, values):
        """Writes moved pass values back into the Tk variables and the ini mirror."""
        for key, value in values.items():
            self.ini[key] = value
            var = self.vars.get(key)
            if var is None:
                continue
            if key in NR_FLAGS:
                var.set(value == '1')
            elif key in NR_FLOATS:
                var.set(float(value))
            else:
                var.set(value)

    def nr_rebuild_tabs(self, select=None):
        """One PASS tab per active pass. The widgets bind to the shared ini variables, so a rebuilt
        tab shows the values the add/delete shift left there."""
        for tab in self.nr_tabs.tabs():
            self.nr_tabs.forget(tab)
        count = self.nr_pass_count()
        for i in range(1, count + 1):
            frame = self.ttk.Frame(self.nr_tabs, padding=(10, 8, 10, 10))
            self.nr_tabs.add(frame, text=_('Pass {}').format(i))
            self.check(frame, f'nr_pass{i}_enabled', 'ini', _('Enabled'))
            self.row(frame, _('Execution point'),
                     self.choice(frame, f'nr_pass{i}_stage', 'ini', NR_STAGES),
                     _('Before upscaling, immediately after upscaling, or before present.'))
            self.row(frame, _('Style'), self.choice(frame, f'nr_pass{i}_style', 'ini', NR_STYLES))
            self.scale_row(frame, f'nr_pass{i}_intensity', _('Intensity'), 0.0, NR_STYLE_MAX)
            self.scale_row(frame, f'nr_pass{i}_structure', _('Structure'), 0.0, NR_STYLE_MAX)
            self.scale_row(frame, f'nr_pass{i}_tone', _('Tone'), 0.0, NR_STYLE_MAX)
            self.check(frame, f'nr_pass{i}_auto_skin', 'ini', _('Automatic skin mask'))
            self.scale_row(frame, f'nr_pass{i}_skin_structure', _('Skin structure'), NR_SKIN_MIN, NR_SKIN_MAX,
                           _('-1 follows the local structure of the skin region instead of the mask\'s own '
                             'adjustment.'))
        if select is not None and 0 <= select < count:
            self.nr_tabs.select(select)
        self.nr_add_button.state(['disabled'] if count >= NR_PASS_COUNT else ['!disabled'])
        self.nr_delete_button.state(['disabled'] if count <= 1 else ['!disabled'])
        self.nr_count_label.configure(text=_('{} of {} passes').format(count, NR_PASS_COUNT))

    def nr_add_pass(self):
        """+ Add PASS: one more tab; the new slot starts from its defaults."""
        count = self.nr_pass_count()
        if count >= NR_PASS_COUNT:
            return
        self.nr_apply_values(nr_pass_add(self.nr_pass_values(), count))
        self.nr_rebuild_tabs(select=count)

    def nr_delete_pass(self):
        """Delete current PASS: the later passes move up and keep their settings, the freed last
        slot returns to its defaults."""
        count = self.nr_pass_count()
        if count <= 1:
            return
        current = self.nr_tabs.index(self.nr_tabs.select()) if self.nr_tabs.select() else None
        removed = max(1, min(count, (current if isinstance(current, int) else 0) + 1))
        self.nr_apply_values(nr_pass_delete(self.nr_pass_values(), removed, count))
        self.nr_rebuild_tabs(select=min(removed, count - 1) - 1)

    def build_display(self):
        ttk = self.ttk
        f = self.scrolled_page('display', _('Display & FPS', 'Экран и FPS'),
                               _('Applied when the game starts.', 'Применяется при запуске игры.'))
        self.version_warning(f)
        self.section(f, _('Frame rate', 'Частота кадров'), top=4)
        self.row(f, _('Frame rate', 'Режим'), self.choice(f, 'fps_mode', 'app', FPS_MODES),
                 _("Community patches for game version 1.09. Unlocked makes the game use the real frame time. "
                   'Other game versions always run at 30 FPS (the patches would corrupt them).',
                   'Патчи сообщества для версии 1.09. «Без ограничения» — игра использует реальное время кадра. '
                   'Другие версии всегда работают в 30 FPS.'))
        self.row(f, _('Frame cap (unlocked mode)', 'Ограничение FPS (режим без ограничения)'),
                 self.choice(f, 'frame_cap', 'app', FRAME_CAPS),
                 _("Above about 120 FPS the game's movement timing breaks (running and rolling get slower, "
                   'physics and animations can glitch): higher caps are at your own risk.',
                   'Выше ~120 FPS ломается тайминг движения игры (бег и перекаты замедляются, возможны '
                   'сбои физики и анимаций): более высокие значения — на ваш риск.'))
        self.row(f, _('Frames ahead of the GPU', 'Кадров впереди GPU'), self.choice(f, 'frames_ahead', 'app', FRAMES_AHEAD),
                 _('1 keeps frame pacing even; more can raise FPS when the graphics card is the limit.',
                   '1 — ровная подача кадров; больше может поднять FPS, если упирается в видеокарту.'))
        self.section(f, _('Window', 'Окно'))
        self.check(f, 'fullscreen', 'app', _('Fullscreen', 'Полноэкранный режим'))
        self.row(f, _('Presentation', 'Режим показа кадров'), self.choice(f, 'present_mode', 'app', PRESENT_MODES))
        self.check(f, 'hdr', 'app', _('Allow HDR output', 'Разрешить HDR'),
                   _('When HDR is on in Windows and the display supports it.',
                     'Если HDR включён в Windows и монитор его поддерживает.'))

    def build_game(self):
        f = self.scrolled_page('game', _('Game & effects', 'Игра и эффекты'),
                               _('Your game dump, saves and the game patches.', 'Дамп игры, сохранения и патчи игры.'))
        self.section(f, _('Game', 'Игра'), top=4)
        self.folder(f, 'game_dir', _('Game folder', 'Папка игры'),
                    _('Choose the folder with eboot.bin', 'Выберите папку с eboot.bin'),
                    _('Your own dump of CUSA03173 (eboot.bin, sce_module, sce_sys, dvdroot_ps4); version 1.09 '
                      'for the community patches.', 'Ваш дамп CUSA03173 (eboot.bin, sce_module, sce_sys, '
                      'dvdroot_ps4); версия 1.09 для патчей сообщества.'), on_change=self.game_changed)
        self.folder(f, 'user_dir', _('Saves folder', 'Папка сохранений'),
                    _('Choose the saves folder', 'Выберите папку сохранений'),
                    _('Empty: {} (shader caches are kept there too).',
                      'Пусто: {} (там же кэш шейдеров).').format(DATA_DIR / 'user'), on_change=self.refresh_status)
        self.folder(f, 'dlc_dir', _('DLC folder'), _('Choose the DLC folder'),
                    _('Your dumped add-ons, one folder each named like SPEXPANSIONDLC03 (The Old Hunters). '
                      'Only needed with a base-game dump; the Game of the Year edition (CUSA03173) has it.'))
        self.row(f, _('Text language', '\u042f\u0437\u044b\u043a \u0442\u0435\u043a\u0441\u0442\u0430'), self.choice(f, 'language', 'app', LANGUAGES))
        self.row(f, _('Voice language', '\u042f\u0437\u044b\u043a \u043e\u0437\u0432\u0443\u0447\u043a\u0438'), self.choice(f, 'voice_language', 'app', VOICE_LANGUAGES),
                 _('As in the game uses the original voice setting; other choices use available tracks.'))
        self.row(f, _('Player name', 'Имя игрока'), self.ttk.Entry(f, textvariable=self.var('player_name', 'app'), width=30),
                 _('Where the game shows the PSN name; empty: the default.', 'Где игра показывает имя PSN; пусто — по умолчанию.'))
        self.section(f, _('Effects', 'Эффекты'))
        self.version_warning(f)
        for key, title, _on in EFFECTS:
            self.check(f, key, 'ini', _(*title))
        self.section(f, _('Extras', 'Дополнительно'))
        for key, title, _on in EXTRAS:
            self.check(f, key, 'ini', _(*title))
        self.note(f, _('Effects and extras are game patches for version 1.09, applied at start.',
                       'Эффекты и дополнения — патчи игры для версии 1.09, применяются при запуске.'))

    def build_cheats(self):
        f = self.scrolled_page('cheats', _('Cheats', 'Читы'),
                               _('Game patches for version 1.09, applied at start. Leave them off for a normal '
                                 'play-through.', 'Патчи игры для версии 1.09, применяются при запуске. Для обычного '
                                 'прохождения оставьте их выключенными.'))
        self.version_warning(f)
        self.section(f, _('Cheats', 'Читы'), top=4)
        for key, title, _on in CHEATS:
            self.check(f, key, 'ini', _(*title))
        self.section(f, _('Gameplay tweaks', 'Изменения игрового процесса'))
        for key, title, _on in TWEAKS:
            self.check(f, key, 'ini', _(*title))
        # Enemy control and the free camera share their buttons: one at a time.
        control, camera = self.var('cheat_enemy_control', 'ini'), self.var('debug_camera', 'ini')
        control.trace_add('write', lambda *_a: control.get() and camera.set(False))
        camera.trace_add('write', lambda *_a: camera.get() and control.set(False))

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
        pool.trace_add('write', lambda *_a: self.refresh_status())
        pools = ttk.Frame(f)
        ttk.Radiobutton(pools, text=_('shadNet server'), variable=pool, value='shadnet').pack(side='left')
        ttk.Radiobutton(pools, text=_('SuperMedo Server'), variable=pool,
                        value='supermedo').pack(side='left', padx=(16, 0))
        if HUNTERS_DREAM_POOL:
            ttk.Radiobutton(pools, text=_("The Hunter's Dream (with bbhost players)"), variable=pool,
                            value='huntersdream').pack(side='left', padx=(16, 0))
        elif pool.get() == 'huntersdream':
            pool.set('shadnet')
        self.row(f, _('Play with'), pools,
                 _("shadNet: a shadNet server and its account. SuperMedo Server uses an account from "
                   "supermedo.com."))
        # Each pool shows only its own settings (show_pool below).
        groups = {}
        before = set(f.grid_slaves())

        def group(name):
            nonlocal before
            now = set(f.grid_slaves())
            groups[name] = groups.get(name, []) + list(now - before)
            before = now

        self.section(f, _('SuperMedo Server account'))
        self.row(f, '', ttk.Button(f, text=_('Sign up for SuperMedo Server'),
                                   command=lambda: webbrowser.open(SUPERMEDO_SIGNUP)))
        self.row(f, _('Username'), ttk.Entry(f, textvariable=self.var('supermedo_user', 'app'), width=40),
                 _('The account you made at supermedo.com.'))
        self.row(f, _('Password'), ttk.Entry(f, textvariable=self.var('supermedo_password', 'app'), width=40,
                                             show='•'),
                 _('Saved on this PC with the other launcher settings.'))
        group('supermedo')
        self.section(f, _("The Hunter's Dream account"))
        account = ttk.Label(f, text='', justify='left')
        user_dir = self.var('user_dir', 'app')

        thd_id, thd_token = self.var('thd_online_id', 'app'), self.var('thd_token', 'app')

        def show_accounts():
            name = thd_id.get() if thd_token.get() else ''
            account.configure(text=_('Signed in as {}').format(name) if name else _('Not signed in'))
        show_accounts()
        self.row(f, _('Account'), account)
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
        self.section(f, _('Game server (messages, bloodstains, ghosts)'))
        community = self.var('online_community', 'app')
        self.row(f, _('Game server'), ttk.Entry(f, textvariable=community, width=40),
                 _('The Hunter\'s Dream by default. Only the name is needed; the game picks the ports.'))
        self.section(f, _('Co-op server (bells and summons)'))
        server = self.var('online_server', 'app')
        self.row(f, _('Server address'), ttk.Entry(f, textvariable=server, width=40),
                 _('srv.shadps4.net:31313 is the public shadPS4 server. For a private server, enter '
                   'host:port from the person who runs it.'))
        self.row(f, _('WebAPI address'), ttk.Entry(f, textvariable=self.var('online_webapi', 'app'), width=40),
                 _('Of the shadNet server, not the game server. Leave empty: the server address with '
                   'port 31315 is used.'))
        self.section(f, _('Account'))
        self.row(f, _('Online ID (NPID)'), ttk.Entry(f, textvariable=self.var('online_npid', 'app'), width=40),
                 _('Your shadNet account name, not your email. Register one at shadnet.shadps4.net, or ask the '
                   'owner of a private server.'))
        self.row(f, _('Password'), ttk.Entry(f, textvariable=self.var('online_password', 'app'), width=40, show='•'),
                 _('Saved on this PC with the other launcher settings.'))
        group('shadnet')
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
            hunters_dream, supermedo = pool_name == 'huntersdream', pool_name == 'supermedo'
            signed_in = thd_id.get() if thd_token.get() else ''
            result.configure(text=_('Checking…'))

            def work():
                if hunters_dream:
                    text = server_check_hunters_dream()
                    text += '\n' + (_('Signed in as {}').format(signed_in) if signed_in else
                                    _('Not signed in: use "Sign in with Discord" above.'))
                elif supermedo:
                    # The pool's name instead of its address.
                    text = server_check(SUPERMEDO_SERVER, webapi_address(SUPERMEDO_SERVER), game)
                    text = text.replace('http://' + SUPERMEDO_SERVER.split(':')[0] + ':31315', 'SuperMedo Server')
                    text = text.replace(SUPERMEDO_SERVER, 'SuperMedo Server')
                    text = text.replace(SUPERMEDO_SERVER.split(':')[0], 'SuperMedo Server')
                    if 'SuperMedo Server: not reachable' in text:
                        text += '\n' + _('SuperMedo Server could not be reached. Check your internet connection and try again.')
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
        shown = {'shadnet': {'shadnet', 'upnp'}, 'supermedo': {'supermedo', 'upnp'},
                 'huntersdream': {'huntersdream', 'upnp'}}

        def show_pool(*_args):
            if pool.get() == 'huntersdream' and not HUNTERS_DREAM_POOL:
                pool.set('shadnet')  # runs show_pool again
                return
            want = shown.get(pool.get(), shown['shadnet'])
            for name, widgets in groups.items():
                for widget in widgets:
                    if widget is not code_box:
                        widget.grid() if name in want else widget.grid_remove()
            if 'huntersdream' in want:
                show_code(code_label.cget('text'))
            else:
                code_box.grid_remove()
        pool.trace_add('write', show_pool)
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

        card = tk.Frame(body, bg=CARD, highlightthickness=1, highlightbackground=LINE)
        card.pack(fill='x', pady=(4, 0))
        left = tk.Frame(card, bg=CARD)
        left.pack(side='left', padx=22, pady=16)
        tk.Label(left, text=f'{percent}%', bg=CARD, fg=TEXT, font=('Georgia', 26)).pack(anchor='w')
        width, height = self.px(260), max(3, self.px(4))
        bar = tk.Canvas(left, width=width, height=height, bg=LINE, highlightthickness=0, bd=0)
        bar.pack(anchor='w', pady=(2, 8))
        if earned:
            bar.create_rectangle(0, 0, max(height, width * len(earned) // len(known)), height, fill=GOLD, outline='')
        tk.Label(left, text=_('{} of {} trophies earned', '{} из {} трофеев получено').format(len(earned), len(known)),
                 bg=CARD, fg=MUTED, font=('Segoe UI', 9)).pack(anchor='w')
        grades = tk.Frame(card, bg=CARD)
        grades.pack(side='right', padx=(0, 22))
        for grade in trophies.GRADES:
            have = sum(1 for t in earned if t[3] == grade)
            total = sum(1 for t in known if t[3] == grade)
            cell = tk.Frame(grades, bg=CARD)
            cell.pack(side='left', padx=self.px(9))
            self.trophy_icon(cell, grade, True, self.px(34), CARD).pack()
            tk.Label(cell, text=f'{have} / {total}', bg=CARD, fg=TEXT if have else MUTED,
                     font=('Segoe UI', 9)).pack(pady=(4, 0))

        tabs = tk.Frame(body, bg=PANEL)
        tabs.pack(fill='x', pady=(18, 2))
        for name, title, count in (('all', _('All', 'Все'), len(known)),
                                   ('earned', _('Earned', 'Получены'), len(earned)),
                                   ('locked', _('Locked', 'Не получены'), len(known) - len(earned))):
            on = name == self.trophy_filter
            tab = tk.Frame(tabs, bg=PANEL, cursor='hand2')
            tab.pack(side='left', padx=(0, 24))
            label = tk.Label(tab, text=f'{title}   {count}', bg=PANEL, fg=TEXT if on else MUTED, cursor='hand2',
                             font=('Segoe UI', 10, 'bold' if on else 'normal'))
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
            tk.Label(head, text=group, bg=PANEL, fg=GOLD, font=('Georgia', 13)).pack(side='left')
            tk.Label(head, text='{} / {}'.format(sum(1 for t in members if t[0] in unlocked), len(members)),
                     bg=PANEL, fg=MUTED, font=('Segoe UI', 9)).pack(side='left', padx=10, pady=(3, 0))
            for trophy in items:
                self.trophy_card(body, trophy, unlocked.get(trophy[0]))
        if not shown:
            tk.Label(body, text=_('No trophies yet. They appear here as soon as the game unlocks them.',
                                  'Трофеев пока нет. Они появятся здесь, как только игра их откроет.')
                     if self.trophy_filter == 'earned' else _('Every trophy is earned. Hail, good hunter.',
                                                              'Все трофеи получены. Славной охоты.'),
                     bg=PANEL, fg=MUTED, font=('Segoe UI', 10)).pack(anchor='w', pady=(22, 0))

    def trophy_card(self, parent, trophy, when):
        tk = self.tk
        _id, name, text, grade, _group = trophy
        earned = when is not None
        card = tk.Frame(parent, bg=CARD, highlightthickness=1, highlightbackground=LINE)
        card.pack(fill='x', pady=(0, 6))
        self.trophy_icon(card, grade, earned, self.px(46), CARD).pack(side='left', padx=(12, 14), pady=10)
        side = tk.Frame(card, bg=CARD)
        side.pack(side='right', padx=(12, 18))
        tk.Label(side, text=_(*GRADE_NAMES[grade]), bg=CARD, fg=TROPHY_COLORS[grade] if earned else MUTED,
                 font=('Segoe UI', 9, 'bold')).pack(anchor='e')
        tk.Label(side, text=time.strftime('%d %b %Y', time.localtime(when)) if earned else _('Locked', 'Не получен'),
                 bg=CARD, fg=MUTED, font=('Segoe UI', 9)).pack(anchor='e', pady=(2, 0))
        middle = tk.Frame(card, bg=CARD)
        middle.pack(side='left', fill='x', expand=True, pady=10)
        tk.Label(middle, text=name, bg=CARD, fg=TEXT if earned else MUTED,
                 font=('Segoe UI', 11, 'bold' if earned else 'normal')).pack(anchor='w')
        tk.Label(middle, text=text, bg=CARD, fg=MUTED, font=('Segoe UI', 9), justify='left',
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
                           font=('Segoe UI', 9))
        caption.pack(anchor='w')
        title = tk.Label(words, text=f'{name}', bg=CARD, fg=TEXT, font=('Segoe UI', 11, 'bold'))
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
    def current_binding(self, output):
        """The key name currently bound to `output`'s first slot: a pending edit (even "cleared
        to None"), else whatever input.ini already had, else None."""
        if output in self.remap_edits:
            return self.remap_edits[output]
        values = self.remap_bindings.get(output) or []
        return values[0] if values else None

    def build_controls(self):
        ttk, tk = self.ttk, self.tk
        f = self.scrolled_page('controls', _('Controls', 'Управление'),
                               _('Keyboard and mouse-button bindings. The controller keeps '
                                 'working side by side; edit its lines in input.ini directly — '
                                 'see docs/INPUT.md.',
                                 'Раскладка клавиатуры и кнопок мыши. Геймпад продолжает '
                                 'работать одновременно; его строки можно настроить только в '
                                 'input.ini — см. docs/INPUT.md.'))
        self.remap_rows = {}

        def add_row(parent, output):
            r = self.next_row(parent)
            label, _ru = OUTPUT_LABELS[output]
            ttk.Label(parent, text=_(label, _ru)).grid(row=r, column=0, sticky='w', padx=(0, 18), pady=4)
            key_label = ttk.Label(parent, width=14, style='Muted.TLabel')
            key_label.grid(row=r, column=1, sticky='w', pady=4)
            set_btn = ttk.Button(parent, text=_('Set…', 'Назначить…'),
                                 command=lambda o=output: self.start_remap_capture(o, key_label, set_btn))
            set_btn.grid(row=r, column=2, sticky='w', padx=(10, 4), pady=4)
            ttk.Button(parent, text=_('Clear', 'Сбросить'),
                      command=lambda o=output: self.clear_remap(o, key_label)).grid(
                row=r, column=3, sticky='w', pady=4)
            self.remap_rows[output] = key_label
            self.refresh_remap_row(output)

        self.section(f, _('Buttons', 'Кнопки'), top=4)
        for output in BUTTON_OUTPUTS:
            add_row(f, output)
        self.section(f, _('Sticks', 'Стики'))
        for output in STICK_OUTPUTS:
            add_row(f, output)
        self.note(f, _('Click "Set…" on a button row to bind a mouse button instead of a key — '
                       'left, middle, right, or the side buttons (back/forward), if the mouse '
                       'has them. Stick directions stay keyboard-only.',
                       'Нажмите «Назначить…» в строке кнопки, чтобы привязать кнопку мыши '
                       'вместо клавиши — левую, среднюю, правую или боковые (вперёд/назад), '
                       'если они есть. Направления стиков можно привязать только к клавише.'))
        self.note(f, _('Reserved: Insert/Escape (the port\'s menu), F9 (route recording), and '
                       'whichever key toggles mouse look or reloads input.ini (F7/F8 by '
                       'default). A key or button already used elsewhere is simply bound to '
                       'both actions (hold it to trigger both) — this page does not warn about '
                       'that.',
                       'Зарезервированы: Insert/Escape (меню порта), F9 (запись маршрута), а '
                       'также клавиши переключения мыши и перезагрузки input.ini (по умолчанию '
                       'F7/F8). Если клавиша или кнопка уже где-то занята, она просто '
                       'срабатывает на обе привязки — страница об этом не предупреждает.'))

        self.section(f, _('Mouse look', 'Обзор мышью'))
        ms = self.mouse_settings

        def mark_mouse_edited(*_a):
            self.mouse_edited = True

        # Built manually (not via self.check) because mouse_settings is its own dict, not
        # self.ini/self.app -- self.var()'s ini/app bookkeeping does not apply to it.
        enabled_var = tk.BooleanVar(value=ms['enabled'])
        enabled_var.trace_add('write', lambda *_a: (ms.__setitem__('enabled', enabled_var.get()), mark_mouse_edited()))
        ttk.Checkbutton(f, text=_('Enable mouse look', 'Включить обзор мышью'), variable=enabled_var).grid(
            row=self.next_row(f), column=0, columnspan=2, sticky='w', pady=(4, 0))
        self.note(f, _('F7 also toggles this in-game, instantly; F8 reloads these settings '
                       'without restarting.', 'F7 также переключает это прямо в игре мгновенно; '
                       'F8 применяет эти настройки без перезапуска.'), top=2)

        stick_var = tk.StringVar(value=ms['stick'])
        stick_var.trace_add('write', lambda *_a: (ms.__setitem__('stick', stick_var.get()), mark_mouse_edited()))
        stick_box = ttk.Frame(f)
        ttk.Radiobutton(stick_box, text=_('Camera (right stick)', 'Камера (правый стик)'),
                       variable=stick_var, value='right').pack(side='left')
        ttk.Radiobutton(stick_box, text=_('Movement (left stick)', 'Движение (левый стик)'),
                       variable=stick_var, value='left').pack(side='left', padx=(16, 0))
        self.row(f, _('Mouse controls', 'Мышь управляет'), stick_box)

        def slider_row(key, title, hint, lo, hi, fmt='{:.2f}'):
            holder = ttk.Frame(f)
            var = tk.DoubleVar(value=ms[key])
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
        slider_row('speed', _('Sensitivity', 'Чувствительность'),
                   _('How fast the camera turns for a given mouse movement.',
                     'Насколько быстро поворачивается камера при движении мыши.'), 0.1, 5.0)
        slider_row('speed_offset', _('Minimum turn speed', 'Минимальная скорость поворота'),
                   _('A constant added on top of sensitivity; raises the floor for slow '
                     'movements without affecting fast ones as much.',
                     'Добавляется поверх чувствительности; поднимает минимальную скорость для '
                     'медленных движений, почти не влияя на быстрые.'), 0.0, 1.0)

        self.note(f, _('Mouse buttons and the scroll wheel are bound in the Buttons section above: '
                       'press Set… and click or turn the wheel. They work while the mouse is captured '
                       '(mouse look on).',
                       'Кнопки мыши и колесо назначаются в разделе «Кнопки» выше: нажмите «Назначить…» '
                       'и щёлкните или прокрутите колесо. Они работают, пока мышь захвачена '
                       '(обзор мышью включён).'), top=10)
        self.note(f, _('Changes apply the next time the game starts, or press F8 in-game to '
                       'reload input.ini without restarting.',
                       'Изменения применяются при следующем запуске игры, либо нажмите F8 в '
                       'игре, чтобы перечитать input.ini без перезапуска.'), top=6)

    def refresh_remap_row(self, output):
        key = self.current_binding(output)
        self.remap_rows[output].configure(text=key if key else _('(none)', '(нет)'))

    def clear_remap(self, output, key_label):
        self.remap_edits[output] = None
        self.refresh_remap_row(output)

    def start_remap_capture(self, output, key_label, set_btn):
        """Grabs the next keypress or mouse click and binds it to `output`'s first slot. Esc
        cancels; a key/button already reserved (HOT-001/HOT-002), or a mouse click on a
        STICK_OUTPUTS row (an axis has no use for a press), is rejected in place, leaving the
        previous binding untouched. bind_all (not root.bind) plus grab_set makes sure the event
        reaches this handler even if some other widget currently has focus, and that no other
        widget (e.g. the player-name Entry on another page) reacts to the same keypress.

        The <ButtonPress> bind is installed via after_idle rather than immediately: this call
        itself runs from the "Set…" button's own click handler, so the left-button-release that
        follows (Tk doesn't fire <ButtonPress> again for that click, but belt-and-suspenders
        against any platform quirk) would otherwise risk being seen as "the user chose
        leftbutton" the instant capture starts."""
        is_stick = output in STICK_OUTPUTS
        original_text = set_btn['text']
        key_label.configure(text=_('Press a key, a mouse button or turn the wheel…',
                                   'Нажмите клавишу, кнопку мыши или прокрутите колесо…')
                            if not is_stick else _('Press a key…', 'Нажмите клавишу…'))
        set_btn.configure(state='disabled')
        self.root.grab_set()

        def finish():
            self.root.unbind_all('<KeyPress>')
            self.root.unbind_all('<ButtonPress>')
            self.root.unbind_all('<Shift-MouseWheel>')
            self.root.bind_all('<MouseWheel>', self.wheel)  # the pages scroll again
            self.root.grab_release()
            set_btn.configure(state='normal', text=original_text)
            self.refresh_remap_row(output)

        def apply(name):
            reserved = reserved_keys(self.remap_toggle_key, self.remap_reload_key)
            if name in reserved:
                self.messagebox.showwarning(
                    _('Reserved', 'Зарезервировано'),
                    _('{} is reserved (menu, recording, or a hotkey) and cannot be bound to a '
                      'game action.', '{} зарезервирована (меню, запись или горячая клавиша) и '
                      'не может быть назначена игровому действию.').format(name))
            else:
                self.remap_edits[output] = name
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
            apply(name)

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
        self.row(f, _('Launcher language', 'Язык лаунчера'), self.choice(f, 'ui_language', 'app', UI_LANGUAGES),
                 _('Applies when the launcher opens again.', 'Применится при следующем открытии лаунчера.'))
        self.check(f, 'close_on_play', 'app', _('Close the launcher when the game starts', 'Закрывать лаунчер при запуске игры'))
        self.check(f, 'check_updates', 'app', _('Check for updates when the launcher opens',
                                                'Проверять обновления при открытии лаунчера'))
        holder = ttk.Frame(f)
        holder.grid(row=self.next_row(f), column=0, columnspan=2, sticky='w', pady=(10, 0))
        ttk.Button(holder, text=_('Desktop shortcut', 'Ярлык на рабочем столе'), command=self.shortcut).pack(side='left')
        ttk.Button(holder, text=_('Add to Steam'), command=self.add_to_steam).pack(side='left', padx=(6, 0))
        ttk.Button(holder, text=_('Port folder', 'Папка порта'), command=lambda: self.open_path(DATA_DIR)).pack(side='left', padx=6)
        ttk.Button(holder, text='bbport.ini', command=lambda: self.open_path(ini_path())).pack(side='left')
        holder = ttk.Frame(f)
        holder.grid(row=self.next_row(f), column=0, columnspan=2, sticky='w', pady=(10, 0))
        ttk.Button(holder, text=_('Check for updates', 'Проверить обновления'),
                   command=lambda: threading.Thread(target=self.check_update, args=(True,), daemon=True).start()
                   ).pack(side='left')
        ttk.Label(holder, text=f'v{VERSION}', style='Muted.TLabel').pack(side='left', padx=10)
        holder = ttk.Frame(f)
        holder.grid(row=self.next_row(f), column=0, columnspan=2, sticky='w', pady=(10, 0))
        ttk.Button(holder, text=_('Clear shader cache', 'Очистить кэш шейдеров'), command=self.clear_cache).pack(side='left')
        ttk.Label(holder, text=_('If the game only shows a black screen, this usually helps.',
                                 'Если игра показывает только чёрный экран, обычно это помогает.'),
                  style='Muted.TLabel').pack(side='left', padx=10)
        self.section(f, _('Performance', 'Производительность'))
        self.row(f, _('Two-stage GPU pipeline', 'Двухстадийный конвейер GPU'), self.choice(f, 'draw_pipe', 'app', DRAW_PIPE),
                 _('20–30% faster; switch it off if the game is unstable.', 'Быстрее на 20–30%; при нестабильности выключите.'))
        self.row(f, _('GPU readbacks', 'Чтение данных GPU'), self.choice(f, 'readbacks', 'app', READBACKS),
                 _('How exactly data the GPU writes is copied back for the game.',
                   'Насколько точно данные, записанные GPU, возвращаются игре.'))
        self.section(f, _('Diagnostics', 'Для разработчика'))
        self.check(f, 'frame_stats', 'app', _('Frame statistics in the log (every 5 s)', 'Статистика кадров в журнале (раз в 5 с)'))
        self.check(f, 'gpu_profile', 'app', _('GPU time per pass in the log', 'Профиль GPU в журнале'))
        self.check(f, 'vk_validation', 'app', _('Vulkan validation layers (needs the Vulkan SDK; much slower)',
                                                'Слои валидации Vulkan (нужен Vulkan SDK; сильно замедляет)'))
        self.row(f, _('Extra variables', 'Доп. переменные'), ttk.Entry(f, textvariable=self.var('extra_env', 'app'), width=58),
                 _('NAME=value pairs separated by spaces (README lists them).', 'Пары ИМЯ=значение через пробел (список в README).'))

    def build_log(self):
        tk, ttk = self.tk, self.ttk
        page = ttk.Frame(self.content, padding=(20, 14, 20, 8))
        self.pages['log'] = page
        top = ttk.Frame(page)
        top.pack(fill='x', pady=(0, 8))
        ttk.Label(top, text=_('Log', 'Журнал'), font=('Georgia', 20)).pack(side='left')
        ttk.Button(top, text=_('Copy', 'Копировать'), command=self.copy_log).pack(side='right')
        ttk.Button(top, text=_('Clear', 'Очистить'), command=lambda: self.set_log('')).pack(side='right', padx=6)
        self.log = tk.Text(page, wrap='none', bg='#0a0908', fg='#cfc6b8', insertbackground=TEXT, relief='flat',
                           font=('Consolas', 9), padx=8, pady=6, state='disabled', highlightthickness=0)
        bar = ttk.Scrollbar(page, command=self.log.yview)
        self.log.configure(yscrollcommand=bar.set)
        bar.pack(side='right', fill='y')
        self.log.pack(fill='both', expand=True)

    def version_warning(self, parent):
        """A banner shown while the selected game is not version 1.09 (refresh_status)."""
        label = self.ttk.Label(parent, style='Warning.TLabel', wraplength=self.px(640), justify='left', text=_(
            'Your game is version {}: these options are patches for 1.09 and are not applied; the game runs '
            'at 30 FPS. Update the dump to 1.09 to use them.',
            'Ваша игра версии {}: эти настройки — патчи для 1.09 и не применяются; игра работает в 30 FPS. '
            'Обновите дамп до 1.09, чтобы их использовать.'))
        label.grid(row=self.next_row(parent), column=0, columnspan=2, sticky='we', pady=(6, 4))
        label.template = label.cget('text')
        self.warnings = getattr(self, 'warnings', []) + [label]

    # ---- state -------------------------------------------------------------------------------
    def game_changed(self):
        self.banner_source = None
        self.draw_banner()
        self.set_icon()
        self.refresh_status()

    def refresh_status(self):
        info = game_info(self.var('game_dir', 'app').get())
        if not info:
            game = _('✗ No eboot.bin in the game folder (Game & effects)', '✗ В папке игры нет eboot.bin («Игра и эффекты»)')
        elif info[1] == PATCH_VERSION:
            game = _('✓ Game version {}: every patch available', '✓ Версия игры {}: доступны все патчи').format(info[1])
        else:
            game = _('⚠ Game version {}: runs at 30 FPS without the community patches (they are for 1.09)',
                     '⚠ Версия игры {}: 30 FPS без патчей сообщества (они для 1.09)').format(info[1])
        user = Path(self.var('user_dir', 'app').get() or DATA_DIR / 'user')
        saves = list((user / 'savedata').glob('*/*/SPRJ*')) if (user / 'savedata').is_dir() else []
        save = (_('✓ Saves found in {}', '✓ Найдены сохранения в {}').format(user) if saves
                else _('• No saves yet: the game creates them in {}', '• Сохранений пока нет: игра создаст их в {}').format(user))
        missing = fsr4_missing()
        fsr4 = (_('✓ FSR 4 assets installed', '✓ Ассеты FSR 4 установлены') if not missing else
                _('• FSR 4 assets missing (Graphics); FSR 3.1 is used meanwhile',
                  '• Нет ассетов FSR 4 («Графика»); пока используется FSR 3.1'))
        for key, text in (('game', game), ('saves', save), ('gpu', self.gpu_text), ('fsr4', fsr4)):
            self.checks[key].configure(text=text, foreground=MUTED if text.startswith('•') else
                                       '#d9a441' if text.startswith('⚠') else '#d36b5c' if text.startswith('✗') else TEXT)
        for label in getattr(self, 'warnings', []):
            if info and info[1] != PATCH_VERSION:
                label.configure(text=label.template.format(info[1]))
                label.grid()
            else:
                label.grid_remove()
        if not self.process:
            self.play_button.configure(state='normal' if info else 'disabled')
            self.status.configure(text=self.summary() if info else _('Choose the game folder first.',
                                                                     'Сначала выберите папку игры.'), fg=MUTED)

    def summary(self):
        fps = dict(FPS_MODES).get(self.var('fps_mode', 'app').get(), ('?',))
        info = game_info(self.var('game_dir', 'app').get())
        if info and info[1] != PATCH_VERSION:
            fps = ('30 FPS',)
        upscaler = dict(UPSCALERS).get(self.var('upscaler', 'ini').get(), ('?',))[0].split(' (')[0]
        fg_val = self.var('frame_generation', 'ini').get()
        fg_str = f' · FG {fg_val.upper()}' if fg_val and fg_val != 'off' else ''
        output = self.var('output_res', 'ini').get().replace('x', ' × ')
        online = _('Online') if self.var('online', 'app').get() else _('Offline')
        if self.var('online', 'app').get() and self.var('online_pool', 'app').get() == 'huntersdream':
            online = _("Online (The Hunter's Dream)")
        elif self.var('online', 'app').get() and self.var('online_pool', 'app').get() == 'supermedo':
            online = _('Online (SuperMedo Server)')
        return f'{_(*fps)}   ·   {upscaler}{fg_str}   ·   {output}   ·   {online}'

    def detect_gpu(self):
        exe = PORT_DIR / 'bin' / 'bb-gpu-capabilities.exe'
        if not exe.is_file():
            exe = PORT_DIR / 'out' / 'bb-gpu-capabilities.exe'
        env = dict(os.environ)
        if not (exe.parent / 'SDL3.dll').is_file():
            clang64 = Path(os.environ.get('MSYS2_ROOT', r'C:\msys64')) / 'clang64' / 'bin'
            env['PATH'] = f'{clang64}{os.pathsep}{env.get("PATH", "")}'
        text = _('• Graphics card: not checked', '• Видеокарта: не проверена')
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
        self.ui_calls.put(self.refresh_status)

    def refresh_fsr4(self):
        total, missing = len(fsr4_files()), len(fsr4_missing())
        self.fsr4_progress.configure(value=total - missing)
        self.fsr4_label.configure(
            text=_('Installed: {} files in {}.', 'Установлены: {} файлов в {}.').format(total, PORT_DIR / 'fsr4_shaders')
            if not missing else _('{} of {} files missing in {}.', 'Нет {} из {} файлов в {}.').format(
                missing, total, PORT_DIR / 'fsr4_shaders'))
        self.fsr4_button.configure(state='normal' if missing and not self.downloading else 'disabled')

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

    def refresh_lists(self):
        if not hasattr(self, 'patches_frame'):
            return
        tk, ttk = self.tk, self.ttk
        from mods import discover
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
        for index, name in enumerate(self.mod_order):
            line = ttk.Frame(self.mods_frame)
            line.pack(fill='x', pady=1)
            var = tk.BooleanVar(value=old.get(name, name not in profile.get('disabled', [])))
            self.mod_vars[name] = var
            ttk.Button(line, text='▲', width=3, command=lambda i=index: self.move_mod(i, -1)).pack(side='left')
            ttk.Button(line, text='▼', width=3, command=lambda i=index: self.move_mod(i, 1)).pack(side='left', padx=(2, 10))
            ttk.Checkbutton(line, text=f'{index + 1}.  {name}', variable=var).pack(side='left')
        if len(self.mod_order) > 1:
            ttk.Label(self.mods_frame, text=_('Lower in the list loads later and wins conflicts.',
                                              'Ниже в списке — загружается позже и перекрывает.'),
                      style='Muted.TLabel').pack(anchor='w', pady=(4, 0))
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

    def move_mod(self, index, step):
        other = index + step
        if 0 <= other < len(self.mod_order):
            self.mod_order[index], self.mod_order[other] = self.mod_order[other], self.mod_order[index]
            self.refresh_lists()

    def collect(self):
        """Writes settings.json, bbport.ini, mods.json and patches.json."""
        for key, var in self.vars.items():
            try:
                value = var.get()
            except self.tk.TclError:  # an unfinished number in a spinbox
                value = APP_DEFAULTS.get(key, INI_DEFAULTS.get(key))
            if var.store == 'ini':
                self.ini[key] = ('1' if value else '0') if key in INI_FLAGS else \
                    f'{float(value):.2f}' if key in INI_FLOATS else str(value)
            else:
                self.app[key] = value
        CONFIG_DIR.mkdir(parents=True, exist_ok=True)
        CONFIG_FILE.write_text(json.dumps(self.app, indent=2, ensure_ascii=False), encoding='utf-8')
        save_ini({key: self.ini[key] for key in INI_DEFAULTS}, self.ini_lines)
        self.ini, self.ini_lines = load_ini()
        if self.remap_edits:
            first_bindings = {key: (self.remap_edits[key] if key in self.remap_edits else
                                    (vals[0] if vals else None))
                              for key, vals in self.remap_bindings.items()}
            save_input_ini(self.remap_ini_path, first_bindings, self.remap_lines)
            self.remap_bindings, self.remap_lines, self.remap_toggle_key, self.remap_reload_key, \
                self.mouse_settings = load_input_ini(self.remap_ini_path)
            self.remap_edits = {}
        if self.mouse_edited:
            save_mouse_settings(self.remap_ini_path, self.mouse_settings, self.remap_lines)
            self.remap_bindings, self.remap_lines, self.remap_toggle_key, self.remap_reload_key, \
                self.mouse_settings = load_input_ini(self.remap_ini_path)
            self.mouse_edited = False
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
        if self.process:
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
            self.messagebox.showwarning(
                'Bloodborne', _("To play online with The Hunter's Dream, sign in with Discord first."))
            self.show('online')
            return
        if online.get() and pool_name == 'supermedo' and not (
                self.var('supermedo_user', 'app').get().strip() and self.var('supermedo_password', 'app').get()):
            self.messagebox.showwarning(
                'Bloodborne', _('To play on SuperMedo Server, enter your supermedo.com username and password '
                                'on the Online co-op page (Sign up there if you have no account).'))
            self.show('online')
            return
        if online.get() and pool_name == 'shadnet' and '@' in npid.get():
            self.messagebox.showwarning(
                'Bloodborne', _('The Online ID is the account name (NPID) you registered on the server, '
                                'not your email address.'))
            self.show('online')
            return
        self.collect()
        if not game_info(self.app['game_dir']):
            self.messagebox.showerror('Bloodborne', _('Choose the game folder with eboot.bin (CUSA03173).',
                                                      'Выберите папку игры с eboot.bin (CUSA03173).'))
            self.show('game')
            return
        self.set_log('')
        try:
            self.process = subprocess.Popen(run_command(), cwd=PORT_DIR, env=game_environment(self.app, self.ini),
                                            stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                            stderr=subprocess.STDOUT, creationflags=NO_WINDOW)
        except OSError as error:
            self.append(_('Could not start: {}', 'Не удалось запустить: {}').format(error) + '\n')
            self.process = None
            return
        self.job = GameJob(self.process)
        threading.Thread(target=self.read_output, args=(self.process,), daemon=True).start()
        self.play_button.configure(state='disabled')
        self.stop_button.configure(state='normal')
        self.status.configure(text=_('Preparing the game; it opens in its own window…',
                                     'Подготовка игры; она откроется в своём окне…'), fg=GOLD)
        if self.app.get('close_on_play'):
            self.root.after(5000, lambda: self.process and self.close())

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
                    "Your eboot.bin is not the clean 1.09 one (modified, or from another dump). The port "
                    "cannot patch it: the game runs at 30 FPS, the Game & effects options do nothing, and "
                    "some scenes (like the cutscene after Rom) can freeze.\n\nReplace eboot.bin with the one "
                    "from an unmodified 1.09 update.")))
            if log:
                log.write(text.replace('\r\n', '\n'))
        if log:
            log.close()
        self.output.put((process.wait(),))

    def drain_output(self):
        while not self.ui_calls.empty():
            self.ui_calls.get_nowait()()
        try:
            for _i in range(500):
                item = self.output.get_nowait()
                if isinstance(item, tuple):
                    self.append(_('\n— the game exited (code {}) —\n', '\n— игра завершилась (код {}) —\n').format(item[0]))
                    self.process = None
                    if self.job:
                        self.job.close()
                    self.stop_button.configure(state='disabled')
                    self.refresh_status()
                    if self.hidden:
                        if item[0] == 0:
                            self.root.destroy()
                            return
                        # It stopped with an error: back with its log instead of vanishing.
                        self.hidden = False
                        self.root.deiconify()
                        self.show('log')
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
        box = tk.Frame(self.side, bg=CARD, highlightthickness=1, highlightbackground=GOLD)
        self.update_label = tk.Label(box, text=text, bg=CARD, fg=GOLD, font=('Segoe UI', 10, 'bold'),
                                     wraplength=self.px(160), justify='left')
        self.update_label.pack(anchor='w', padx=10, pady=(8, 6))
        buttons = tk.Frame(box, bg=CARD)
        buttons.pack(fill='x', padx=10, pady=(0, 10))
        self.update_button = ttk.Button(buttons, text=_('Update', 'Обновить'),
                                        command=lambda: self.install_update(version, url, page))
        self.update_button.pack(fill='x')
        ttk.Button(buttons, text=_("What's new", 'Что нового'), command=lambda: webbrowser.open(page)).pack(
            fill='x', pady=(4, 0))
        box.pack(fill='x', padx=(22, 18), pady=(0, 14), before=self.side_note)
        self.update_box = box
        if not self.process:
            self.status.configure(text=text, fg=GOLD)

    def install_update(self, version, url, page):
        if self.process:
            self.messagebox.showinfo('Bloodborne', _('Close the game before updating.', 'Закройте игру перед обновлением.'))
            return
        if not FROZEN or not url:  # a source tree updates with git
            webbrowser.open(page)
            return
        if not self.messagebox.askyesno('Bloodborne', _(
                'Install version {} now? The launcher closes, installs it and opens again. Saves and settings are kept.',
                'Установить версию {} сейчас? Лаунчер закроется, установит её и откроется снова. Сохранения и '
                'настройки останутся.').format(version)):
            return
        self.update_button.configure(state='disabled')

        def progress(percent):
            self.update_label.configure(text=_('Downloading version {}… {}%', 'Загрузка версии {}… {}%').format(
                version, percent))

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
                def failed(error=failure):
                    self.update_button.configure(state='normal')
                    self.update_label.configure(text=_('Version {} is available.', 'Доступна версия {}.').format(version))
                    self.messagebox.showerror('Bloodborne', _('Update failed: {}', 'Не удалось обновить: {}').format(error))
                self.ui_calls.put(failed)
        threading.Thread(target=work, daemon=True).start()

    def clear_cache(self):
        """Shader and pipeline caches; they are rebuilt while playing."""
        if self.process:
            return
        import shutil
        cache = Path(self.app['user_dir'] or DATA_DIR / 'user') / 'cache'
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


def play_without_window(settings):
    """--play: the game with the saved settings (shortcuts, Steam). Output goes to the console
    when there is one and to <saves folder>/last_run.log."""
    attach_stdio()
    log_dir = Path(settings.get('user_dir') or DATA_DIR / 'user')
    log_dir.mkdir(parents=True, exist_ok=True)
    tracker = trophies.Tracker(log_dir)
    with open(log_dir / 'last_run.log', 'w', encoding='utf-8', buffering=1) as log:
        process = subprocess.Popen(run_command(), cwd=PORT_DIR, env=game_environment(settings, load_ini()[0]),
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
        return process.wait()


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
    for attempt in range(30):
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


def main():
    global LANG
    args = sys.argv[1:]
    if args and args[0] in ('--run', '--script'):
        sys.exit(run_role(args))
    settings = {**APP_DEFAULTS, **load_json(CONFIG_FILE, {})}
    LANG = settings.get('ui_language') or windows_language()
    if args[:1] == ['--install-update'] and len(args) == 3:
        sys.exit(install_update(args[1], args[2]))
    if FROZEN and UPDATE_DIR not in PORT_DIR.parents:
        shutil.rmtree(UPDATE_DIR, ignore_errors=True)  # what a finished update left behind
    # Without a usable game folder there is nothing to play yet: open the launcher instead.
    if '--play' in args and (Path(settings['game_dir'] or '.') / 'eboot.bin').is_file():
        sys.exit(play_without_window(settings))
    try:
        ctypes.windll.shcore.SetProcessDpiAwareness(1)  # sharp text on scaled displays
    except (AttributeError, OSError):
        pass
    import tkinter as tk
    from tkinter import filedialog, messagebox, ttk
    root = tk.Tk()
    Launcher(root, tk, ttk, filedialog, messagebox)
    root.mainloop()


if __name__ == '__main__':
    main()
