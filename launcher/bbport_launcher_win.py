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
CONFIG_DIR = Path(os.environ.get('APPDATA', Path.home())) / 'bbport-launcher'
CONFIG_FILE = CONFIG_DIR / 'settings.json'
PATCH_VERSION = '01.09'
MAX_LOG_LINES = 6000
NO_WINDOW = getattr(subprocess, 'CREATE_NO_WINDOW', 0)
# This build; GitHub release tags are windows-v<VERSION>.
VERSION = '1.5'
RELEASES_API = 'https://api.github.com/repos/Supermedo/bloodborne_pc/releases/latest'
RELEASES_PAGE = 'https://github.com/Supermedo/bloodborne_pc/releases/latest'
UPDATE_DIR = Path(tempfile.gettempdir()) / 'bbport-update'
# Never copied over an installation by an update (the package does not hold them either).
USER_FILES = ('user', 'out', 'mods', 'bbport.ini', 'mods.json', 'patches.json', 'last_run.log')


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


def run_role(argv):
    """--run [args]: run.py; --script <file> [args]: a preparation script. Returns an exit code."""
    attach_stdio()
    for stream in (sys.stdout, sys.stderr):  # keep messages in order with the game's output
        try:
            stream.reconfigure(line_buffering=True)
        except (AttributeError, ValueError):
            pass
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

LANG = 'en'


WARN = '  ⚠'  # marks a risky choice; the text before it is translated as usual


def _(en, ru=None):
    if en.endswith(WARN):
        return _(en[:-len(WARN)], ru and ru.removesuffix(WARN)) + WARN
    if LANG == 'ru':
        return ru or en
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
]
INI_FLAGS = {'sharpen', 'object_motion', 'show_fps', *(k for k, _t, _o in EFFECTS + EXTRAS + CHEATS + TWEAKS)}
INI_DEFAULTS = {'upscaler': 'fsr4', 'preset': '1', 'sharpen': '1', 'sharpness': '0.50',
                'object_motion': '1', 'show_fps': '1', 'output_res': '1920x1080', 'model_lod': '0',
                'live_resolution': 'auto',
                **{key: '1' if on else '0' for key, _t, on in EFFECTS + EXTRAS + CHEATS + TWEAKS}}
APP_DEFAULTS = {'ui_language': '', 'game_dir': str(PORT_DIR.parent / 'CUSA03173'), 'user_dir': '',
                'mods_dir': '', 'mods_enabled': True, 'patches_dir': '', 'language': '1',
                'player_name': '', 'fullscreen': False, 'hdr': False, 'present_mode': 'Mailbox',
                'fps_mode': 'uncap', 'frame_cap': '', 'draw_pipe': '', 'readbacks': '',
                'frames_ahead': '', 'frame_stats': False, 'gpu_profile': False,
                'vk_validation': False, 'extra_env': '', 'close_on_play': False,
                'check_updates': True}

UPSCALERS = [('dlss', ('DLSS (NVIDIA GeForce RTX)',)),
             ('fsr4', ('FSR 4 (best quality)', 'FSR 4 (лучшее качество)')),
             ('fsr411', ('FSR 4.1.1 (needs fsr4_411 assets)', 'FSR 4.1.1 (нужны ассеты fsr4_411)')),
             ('fsr3', ('FSR 3.1 (every GPU)', 'FSR 3.1 (любая видеокарта)')),
             ('taa', ('TAA (native resolution anti-aliasing)', 'TAA (нативное сглаживание)')),
             ('off', ('Off', 'Выключен'))]
PRESETS = [('0', ('Native AA (×1.0)',)), ('1', ('Quality (×1.5)',)), ('2', ('Balanced (×1.7)',)),
           ('3', ('Performance (×2)',)), ('4', ('Ultra Performance (×3)',))]
OUTPUTS = [('1280x720', ('1280 × 720 (Steam Deck)',)), ('1920x1080', ('1920 × 1080',)),
           ('2560x1440', ('2560 × 1440',)), ('3840x2160', ('3840 × 2160 (4K)',))]
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
             ('5', ('Italian', 'Итальянский'))]
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


def game_environment(s):
    env = dict(os.environ)
    env['BB_GAME_DIR'] = s['game_dir']
    if s['user_dir']:
        env['BB_USER_DIR'] = s['user_dir']
    env['BB_MODS_DIR'] = s['mods_dir'] or str(DATA_DIR / 'mods')
    env['BB_MODS_CONFIG'] = str(DATA_DIR / 'mods.json')
    env['BB_MODS_ENABLED'] = '1' if s['mods_enabled'] else '0'
    env['BB_PATCHES_DIR'] = s['patches_dir'] or str(DATA_DIR / 'patches')
    env['BB_PATCHES_CONFIG'] = str(DATA_DIR / 'patches.json')
    env['BB_LANGUAGE'] = s['language']
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
    env['PYTHONUNBUFFERED'] = '1'
    env['PYTHONIOENCODING'] = 'utf-8'
    return env


# ---------------------------------------------------------------------------------------------
# The window.

BG, PANEL, CARD, LINE = '#0e0c0b', '#151210', '#1c1815', '#2e2722'
TEXT, MUTED, GOLD, BLOOD, BLOOD_HI = '#e9e2d6', '#9a8f80', '#c8a96a', '#7c1717', '#9e2222'


class Launcher:
    def __init__(self, root, tk, ttk, filedialog, messagebox):
        self.tk, self.ttk, self.filedialog, self.messagebox = tk, ttk, filedialog, messagebox
        self.root = root
        self.app = {**APP_DEFAULTS, **load_json(CONFIG_FILE, {})}
        self.ini, self.ini_lines = load_ini()
        self.vars = {}
        self.process = self.job = None
        self.downloading = False
        self.output = queue.Queue()
        self.gpu_text = _('• Checking the graphics card…', '• Проверка видеокарты…')
        self.banner_source = self.banner_image = None
        self.ui_calls = queue.Queue()  # work for the Tk thread from helper threads
        self.mod_order, self.mod_vars, self.patch_vars = [], {}, {}
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
        threading.Thread(target=self.detect_gpu, daemon=True).start()
        if self.app.get('check_updates', True):
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
        s.configure('TCheckbutton', background=PANEL, foreground=TEXT, padding=(0, 3))
        s.map('TCheckbutton', background=[('active', PANEL)], foreground=[('disabled', MUTED)])
        s.configure('Warning.TLabel', background='#2a1d12', foreground='#e3b25a', padding=(10, 6))
        s.configure('TCombobox', arrowcolor=GOLD, foreground=TEXT, padding=4)
        s.map('TCombobox', fieldbackground=[('readonly', CARD)], foreground=[('readonly', TEXT)],
              selectbackground=[('readonly', CARD)], selectforeground=[('readonly', TEXT)])
        s.configure('TEntry', foreground=TEXT, insertcolor=TEXT, padding=4)
        s.configure('TSpinbox', foreground=TEXT, arrowcolor=GOLD, insertcolor=TEXT, padding=4)
        s.configure('TButton', background=CARD, foreground=TEXT, padding=(12, 6), borderwidth=1)
        s.map('TButton', background=[('active', LINE), ('disabled', PANEL)], foreground=[('disabled', MUTED)])
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

    def choice(self, parent, key, store, options, width=38):
        """A combobox over (value, (english, russian)) pairs, kept in sync with its variable."""
        var = self.var(key, store)
        values = [v for v, _t in options]
        box = self.ttk.Combobox(parent, values=[_(*text) for _v, text in options], state='readonly', width=width)
        if var.get() not in values:
            var.set(values[0])

        def show(*_args):
            current = var.get()
            box.current(values.index(current) if current in values else 0)
        show()
        box.bind('<<ComboboxSelected>>', lambda _e: var.set(values[box.current()]))
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
        tk.Label(side, text=_('native port · Windows', 'нативный порт · Windows') + f'  ·  v{VERSION}', bg=BG, fg=MUTED,
                 font=('Segoe UI', 9)).pack(anchor='w', padx=22, pady=(0, 20))
        self.nav, self.current_page = {}, None
        for name, title in (('play', _('Play', 'Играть')), ('graphics', _('Graphics', 'Графика')),
                            ('display', _('Display & FPS', 'Экран и FPS')), ('game', _('Game & effects', 'Игра и эффекты')),
                            ('cheats', _('Cheats', 'Читы')), ('mods', _('Mods & patches', 'Моды и патчи')),
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
        self.build_mods()
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
                 _("Render scale per axis: Quality renders at 1/1.5 of the output size. A reducing "
                   "preset sets the game's own render size by a patch at start (changing it restarts "
                   'the game).',
                   'Масштаб рендера по каждой оси: Quality рисует в 1/1.5 размера вывода. '
                   'Уменьшающий пресет задаёт собственный размер рендера игры патчем при запуске '
                   '(смена — с перезапуском).'))
        self.row(f, _('Output resolution', 'Разрешение вывода'), self.choice(f, 'output_res', 'ini', OUTPUTS),
                 _('What the upscaler produces; the HUD is drawn at this size too.',
                   'Что выдаёт апскейлер; интерфейс рисуется в этом же размере.'))
        self.row(f, _('Live resolution changes', 'Смена разрешения на лету'), self.choice(f, 'live_resolution', 'ini', LIVE),
                 _('Off: the preset reduces the game\'s own render size through a patch at start — at '
                   'every output including 1080p (fastest; changing it in the game restarts it). On: the '
                   'game stays at 1080p internally and the port scales scene targets at run time — change '
                   'output and preset without a restart, at a cost. Auto: the patch at 1080p when a preset '
                   'reduces; live changes elsewhere on strong GPUs.',
                   'Выкл.: пресет уменьшает собственный размер рендера игры патчем при запуске — при '
                   'любом выводе, включая 1080p (быстрее; смена в игре — с перезапуском). Вкл.: игра '
                   'остаётся в 1080p, порт масштабирует цели сцены на лету — менять вывод и пресет без '
                   'перезапуска, но медленнее. Авто: патч при 1080p с уменьшающим пресетом, на лету — '
                   'для остальных выводов на мощных GPU.'))
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
        self.row(f, _('Game language', 'Язык игры'), self.choice(f, 'language', 'app', LANGUAGES))
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
        output = self.var('output_res', 'ini').get().replace('x', ' × ')
        return f'{_(*fps)}   ·   {upscaler}   ·   {output}'

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
                    f'{float(value):.2f}' if key == 'sharpness' else str(value)
            else:
                self.app[key] = value
        CONFIG_DIR.mkdir(parents=True, exist_ok=True)
        CONFIG_FILE.write_text(json.dumps(self.app, indent=2, ensure_ascii=False), encoding='utf-8')
        save_ini({key: self.ini[key] for key in INI_DEFAULTS}, self.ini_lines)
        self.ini, self.ini_lines = load_ini()
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
        self.collect()
        if not game_info(self.app['game_dir']):
            self.messagebox.showerror('Bloodborne', _('Choose the game folder with eboot.bin (CUSA03173).',
                                                      'Выберите папку игры с eboot.bin (CUSA03173).'))
            self.show('game')
            return
        self.set_log('')
        try:
            self.process = subprocess.Popen(run_command(), cwd=PORT_DIR, env=game_environment(self.app),
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
            self.root.after(5000, self.root.destroy)  # the game keeps running

    def read_output(self, process):
        for raw in iter(process.stdout.readline, b''):
            self.output.put(raw.decode('utf-8', errors='replace'))
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
                else:
                    if 'Entering original x86-64 code' in item:
                        self.status.configure(text=_('The game is running.', 'Игра запущена.'), fg=GOLD)
                    elif 'restarting through run.py' in item:
                        self.status.configure(text=_('Restarting with the new settings…', 'Перезапуск с новыми настройками…'), fg=GOLD)
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

    def close(self):
        try:
            self.collect()
        except Exception:  # never keep the window open over a settings problem
            pass
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
    with open(log_dir / 'last_run.log', 'w', encoding='utf-8', buffering=1) as log:
        process = subprocess.Popen(run_command(), cwd=PORT_DIR, env=game_environment(settings),
                                   stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                   stderr=subprocess.STDOUT, creationflags=NO_WINDOW)
        for raw in iter(process.stdout.readline, b''):
            text = raw.decode('utf-8', errors='replace')
            log.write(text)
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
