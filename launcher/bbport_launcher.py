#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Bloodborne launcher for the native port (GTK4 / libadwaita).

Picks the game folder, edits the port's settings (bbport.ini: upscaler, preset, ...) and the
start-up tweaks passed as environment variables to run.sh, starts and stops the game and shows
its output. Online play on SuperMedo Server (bbport_linux_online: the account, parties).
Launcher settings live in ~/.config/bbport-launcher/settings.json (the Linux launcher's own, not
the Windows launcher's).
Russian and English (bbport_i18n: the Russian text is the key).
"""

import json
import os
import signal
import sys
import threading
from pathlib import Path
import bbport_linux_online as online
from bbport_assets import fsr411_problem
from bbport_i18n import language, set_language, tr

import gi

gi.require_version("Gtk", "4.0")
gi.require_version("Adw", "1")
from gi.repository import Adw, Gio, GLib, Gtk  # noqa: E402

PORT_DIR = Path(__file__).resolve().parent.parent  # native_probe (or the package's copy)
sys.path.insert(0, str(PORT_DIR / 'scripts'))
from mods import discover as discover_mods  # noqa: E402
from patches import external_patches  # noqa: E402
# Packaged (AppImage): generated files, saves and bbport.ini live in BB_DATA_DIR.
PACKAGED = bool(os.environ.get("BB_PREBUILT"))
DATA_DIR = Path(os.environ.get("BB_DATA_DIR", PORT_DIR))
CONFIG_DIR = Path(os.environ.get("XDG_CONFIG_HOME", Path.home() / ".config")) / "bbport-launcher"
CONFIG_FILE = CONFIG_DIR / "settings.json"
MAX_LOG_LINES = 5000

# Choices: (label, value). The first entry is the default. Labels are translated when shown.
UI_LANGUAGES = [("Как в системе", ""), ("Русский", "ru"), ("English", "en")]
UPSCALERS = [("FSR 4", "fsr4"), ("FSR 4.1.1", "fsr411"), ("FSR 3", "fsr3"),
             ("TAA (нативное сглаживание)", "taa"), ("Выключен", "off")]
PRESETS = [("Native AA", 0), ("Quality (x1.5)", 1), ("Balanced (x1.7)", 2),
           ("Performance (x2)", 3), ("Ultra Performance (x3)", 4)]
OUTPUT_RES = [("1280×720 (Steam Deck)", "1280x720"), ("1920×1080", "1920x1080"), ("2560×1440", "2560x1440"), ("3840×2160", "3840x2160")]
# Game effects (patches applied at start): bbport.ini key, title, default.
EFFECTS = [
    ("effect_chromatic_aberration", "Хроматическая аберрация", True),
    ("effect_dof", "Глубина резкости (DoF)", True),
    ("effect_motion_blur", "Размытие в движении", True),
    ("effect_ssao", "Затенение SSAO", True),
    ("effect_game_aa", "Собственное сглаживание игры", True),
    ("effect_dynamic_shadows", "Тени от динамических источников", True),
    ("effect_ssr", "Отражения SSR (не было в игре)", False),
    ("skip_intro", "Пропуск заставок при запуске", False),
    ("quit_to_desktop", "Выход на рабочий стол в меню игры", True),
    ("pc_menu", "Настройки ПК в системном меню игры", True),
    ("rebirth", "Перерождение у Куклы", True),
    ("debug_camera", "Свободная камера (Cross + L3 / Space + Z)", False),
    ("debug_menu", "Debug menu (левый touchpad / Tab; нужны шрифты)", False),
]
MODEL_LOD = [("Как в игре", "0"), ("Максимальная (-2)", "-2"), ("Ниже (1)", "1"),
             ("Минимальная (2)", "2")]
FPS_MODES = [("Без ограничения (патч)", "uncap"), ("60", "60"), ("90", "90"),
             ("30 (как на PS4)", "30")]
PRESENT_MODES = [("Mailbox", "Mailbox"), ("FIFO (VSync)", "Fifo"),
                 ("FIFO Relaxed", "FifoRelaxed"), ("Immediate", "Immediate")]
DRAW_PIPE = [("Авто (8+ потоков)", ""), ("Включён", "1"), ("Выключен (стабильнее)", "0")]
LANGUAGES = [("Английский", "1"), ("Русский", "8"), ("Японский", "0"), ("Французский", "2"),
             ("Испанский", "3"), ("Немецкий", "4"), ("Итальянский", "5")]
LIVE_RESOLUTION = [("Авто (по видеокарте)", "auto"), ("Выключена (быстрее)", "0"), ("Включена", "1")]
READBACKS = [("Relaxed (по умолчанию)", ""), ("Выключены", "0"), ("Precise", "2")]
# BB_LOW_LATENCY (vk_low_latency.cpp): Auto is NVIDIA Reflex + Boost on NVIDIA and off elsewhere; On is AMD Anti-Lag 2
# where the driver has it.
LOW_LATENCY = [("Авто (NVIDIA: включена + Boost)", "auto"), ("Выключена", "off"), ("Включена", "on"),
               ("Включена + Boost", "boost")]
# bbport.ini hud_position on ultrawide outputs (scripts/hud_layout.py).
HUD_POSITIONS = [("У краёв экрана", "100"), ("75%", "75"), ("Посередине", "50"), ("25%", "25"),
                 ("По центру (16:9)", "0")]

DEFAULTS = {
    "ui_language": "",
    "game_dir": "" if PACKAGED else str(PORT_DIR.parent / "CUSA03173"),
    "user_dir": "",
    "mods_dir": "",
    "mods_enabled": True,
    "patches_dir": "",
    "language": "1",
    "fullscreen": False,
    "hdr": False,
    "present_mode": "Mailbox",
    "fps_mode": "uncap",
    "fps_limit": 0,
    "draw_pipe": "",
    "readbacks": "",
    "low_latency": "auto",
    "vrr": False,
    "save_backups": False,
    "mangohud": False,
    "frame_stats": False,
    "gpu_profile": False,
    "vk_validation": False,
    "extra_env": "",
    **online.DEFAULTS,
}

# bbport.ini keys the launcher edits; the rest of the file is kept.
INI_DEFAULTS = {
    "upscaler": "fsr4",
    "preset": "4",
    "sharpen": "1",
    "sharpness": "0.50",
    "object_motion": "1",
    "show_fps": "1",
    "output_res": "1920x1080",
    "model_lod": "0",
    "live_resolution": "0",
    "fov": "43",
    "hud_position": "100",
    **{key: "1" if default else "0" for key, _, default in EFFECTS},
}


def load_settings():
    settings = dict(DEFAULTS)
    try:
        settings.update(json.loads(CONFIG_FILE.read_text(encoding='utf-8')))
    except (OSError, ValueError):
        pass
    return settings


def save_settings(settings):
    CONFIG_DIR.mkdir(parents=True, exist_ok=True)
    # The SuperMedo account's password is kept here: readable by this user only.
    fd = os.open(CONFIG_FILE, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, 'w', encoding='utf-8') as file:
        file.write(json.dumps(settings, indent=2, ensure_ascii=False))
    try:
        os.chmod(CONFIG_FILE, 0o600)  # settings saved before the account was
    except OSError:
        pass


def ini_path():
    return Path(os.environ.get("BB_CONFIG", DATA_DIR / "bbport.ini"))


def load_ini():
    values = dict(INI_DEFAULTS)
    lines = []
    try:
        lines = ini_path().read_text(encoding='utf-8').splitlines()
    except OSError:
        pass
    for line in lines:
        if "=" in line and not line.lstrip().startswith("#"):
            key, value = line.split("=", 1)
            values[key.strip()] = value.strip()
    return values, lines


def save_ini(values, lines):
    """Rewrites the edited keys in place, appends missing ones, keeps comments and others."""
    written = set()
    out = []
    for line in lines:
        if "=" in line and not line.lstrip().startswith("#"):
            key = line.split("=", 1)[0].strip()
            if key in values:
                out.append(f"{key}={values[key]}")
                written.add(key)
                continue
        out.append(line)
    if not lines:
        out.append("# bbport settings (in-game menu: Insert / L3+R3)")
    for key, value in values.items():
        if key not in written:
            out.append(f"{key}={value}")
    ini_path().write_text("\n".join(out) + "\n", encoding='utf-8')


def patches_dir(settings):
    return Path(settings.get("patches_dir") or DATA_DIR / "patches").expanduser()


def user_folder(s):
    """The game's user folder (saves, shader cache): run.sh's BB_USER_DIR, user/ in the data directory."""
    return Path(s["user_dir"]).expanduser() if s["user_dir"] else DATA_DIR / "user"


def game_environment(s, log=print):
    """Environment for run.sh from the launcher settings (online: host_overrides.json written)."""
    env = dict(os.environ)
    env["BB_GAME_DIR"] = str(Path(s["game_dir"]).expanduser())
    if s["user_dir"]:
        env["BB_USER_DIR"] = s["user_dir"]
    env["BB_MODS_DIR"] = str(Path(s.get("mods_dir") or DATA_DIR / "mods").expanduser())
    env["BB_MODS_CONFIG"] = str(DATA_DIR / "mods.json")
    env["BB_MODS_ENABLED"] = "1" if s.get("mods_enabled", True) else "0"
    env["BB_PATCHES_DIR"] = str(patches_dir(s))
    env["BB_PATCHES_CONFIG"] = str(DATA_DIR / "patches.json")
    env["BB_LANGUAGE"] = s["language"]
    env["BB_FULLSCREEN"] = "1" if s["fullscreen"] else "0"
    env["BB_PRESENT_MODE"] = s["present_mode"]
    if s["hdr"]:
        env["BB_HDR"] = "1"
    env["BB_FPS"] = s["fps_mode"]
    if s["fps_limit"] > 0:
        env["BB_FPS_LIMIT"] = str(s["fps_limit"])
    if s["draw_pipe"]:
        env["BB_DRAW_PIPE"] = s["draw_pipe"]
    if s["readbacks"]:
        env["BB_READBACKS"] = s["readbacks"]
    env["BB_LOW_LATENCY"] = s["low_latency"]
    if s["vrr"]:
        env["BB_VRR"] = "1"
    env["BB_SAVE_BACKUPS"] = "1" if s["save_backups"] else "0"
    if s["mangohud"]:
        env["MANGOHUD"] = "1"
    if s["frame_stats"]:
        env["BB_FRAME_STATS"] = "1"
    if s["gpu_profile"]:
        env["BB_GPU_PROFILE"] = "1"
    if s["vk_validation"]:
        env["BB_VK_VALIDATION"] = "1"
    env.update(online.environment(s, user_folder(s), log))
    for item in s["extra_env"].split():
        if "=" in item:
            key, value = item.split("=", 1)
            env[key] = value
    return env


def flat_button(icon, tooltip, handler):
    button = Gtk.Button(icon_name=icon, valign=Gtk.Align.CENTER, tooltip_text=tooltip)
    button.add_css_class("flat")
    button.connect("clicked", handler)
    return button


def open_folder(window, path):
    """Opens `path` in the file manager (created first, so that a new saves folder opens)."""
    try:
        Path(path).mkdir(parents=True, exist_ok=True)
    except OSError:
        return
    Gtk.FileLauncher.new(Gio.File.new_for_path(str(path))).launch(window, None, None)


def text_button(label, handler):
    button = Gtk.Button(label=label, valign=Gtk.Align.CENTER)
    button.connect("clicked", handler)
    return button


def run_in_background(work, done):
    """work() off the GTK thread, then done(result or the exception it raised) on it."""
    def thread():
        try:
            result = work()
        except Exception as error:  # a broken reply is an answer too: the page never stays "Checking…"
            result = error
        GLib.idle_add(lambda: done(result) and False)
    threading.Thread(target=thread, daemon=True).start()


def combo_row(title, subtitle, choices, current):
    model = Gtk.StringList.new([tr(label) for label, _ in choices])
    row = Adw.ComboRow(title=title, model=model)
    if subtitle:
        row.set_subtitle(subtitle)
    values = [value for _, value in choices]
    row.set_selected(values.index(current) if current in values else 0)
    row.values = values
    return row


def combo_value(row):
    return row.values[row.get_selected()]


class FolderList:
    """A preferences group with a folder row and one switch row per item below it."""

    def __init__(self, group):
        self.group = group
        self.rows = []

    def clear(self):
        for _, row in self.rows:
            self.group.remove(row)
        self.rows = []

    def add(self, key, row):
        self.group.add(row)
        self.rows.append((key, row))


class LauncherWindow(Adw.ApplicationWindow):
    def __init__(self, app):
        super().__init__(application=app, title="Bloodborne")
        self.set_default_size(760, 820)
        self.settings = load_settings()
        set_language(self.settings.get("ui_language", ""))
        self.ini, self.ini_lines = load_ini()
        self.process = None
        self.stream = None
        self.run_log = None  # <saves folder>/last_run.log: the game's output, as the Windows launcher keeps it
        self.party = None  # SuperMedo Server's answer about this account's party (party_act)
        self.build()
        self.connect("close-request", self.on_close)
        # While the Online page is shown: friends who join or leave the party show up by themselves.
        GLib.timeout_add_seconds(15, self.poll_party)

    def build(self):
        """(Re)creates the window's content in the current launcher language."""
        toolbar = Adw.ToolbarView()
        header = Adw.HeaderBar()
        self.stack = Adw.ViewStack()
        switcher = Adw.ViewSwitcher(stack=self.stack, policy=Adw.ViewSwitcherPolicy.WIDE)
        header.set_title_widget(switcher)
        self.launch_button = Gtk.Button()
        self.launch_button.connect("clicked", self.on_launch)
        header.pack_end(self.launch_button)
        toolbar.add_top_bar(header)

        self.toasts = Adw.ToastOverlay()
        self.toasts.set_child(self.stack)
        toolbar.set_content(self.toasts)
        self.set_content(toolbar)

        log_text = self.log_view.get_buffer().get_text(
            *self.log_view.get_buffer().get_bounds(), False) if hasattr(self, "log_view") else ""
        self.stack.add_titled_with_icon(self.build_settings_page(), "settings", tr("Настройки"),
                                        "preferences-system-symbolic")
        self.stack.add_titled_with_icon(self.build_online_page(), "online", tr("Онлайн"),
                                        "network-workgroup-symbolic")
        self.stack.add_titled_with_icon(self.build_log_page(), "log", tr("Журнал"),
                                        "utilities-terminal-symbolic")
        self.log_view.get_buffer().set_text(log_text)
        self.update_launch_button()
        self.update_game_status()
        self.update_user_status()
        self.update_upscaler_status()
        self.update_account()
        self.show_party(self.party)
        if online.account(self.settings) and self.settings.get("party_key"):
            self.party_act("status", quiet=True)

    def update_launch_button(self):
        running = self.process is not None
        self.launch_button.set_label(tr("Остановить") if running else tr("Запустить"))
        self.launch_button.remove_css_class("destructive-action" if not running else "suggested-action")
        self.launch_button.add_css_class("destructive-action" if running else "suggested-action")

    # --- settings page -------------------------------------------------------------------

    def build_settings_page(self):
        page = Adw.PreferencesPage()

        launcher = Adw.PreferencesGroup()
        self.ui_language_row = combo_row(tr("Язык лаунчера") + " / Launcher language", None,
                                         UI_LANGUAGES, self.settings.get("ui_language", ""))
        self.ui_language_row.connect("notify::selected", self.on_ui_language)
        launcher.add(self.ui_language_row)
        page.add(launcher)

        game = Adw.PreferencesGroup(title=tr("Игра"))
        self.game_row = Adw.ActionRow(title=tr("Папка игры (CUSA03173)"))
        self.game_status = Gtk.Image()
        self.game_row.add_suffix(self.game_status)
        self.game_row.add_suffix(flat_button("folder-open-symbolic", tr("Выбрать папку с eboot.bin"),
                                             self.on_choose_game))
        self.game_row.add_suffix(flat_button("system-file-manager-symbolic",
                                             tr("Открыть в файловом менеджере"),
                                             lambda _b: open_folder(self, self.game_dir())))
        game.add(self.game_row)
        # Saves and the shader cache: user/ in the data directory unless chosen.
        self.user_row = Adw.ActionRow(title=tr("Папка сохранений"))
        self.user_status = Gtk.Image()
        self.user_row.add_suffix(self.user_status)
        self.user_row.add_suffix(flat_button("folder-open-symbolic", tr("Выбрать папку сохранений"),
                                             self.on_choose_user))
        self.user_row.add_suffix(flat_button("system-file-manager-symbolic",
                                             tr("Открыть в файловом менеджере"),
                                             lambda _b: open_folder(self, self.user_dir())))
        self.user_reset = flat_button("edit-undo-symbolic", tr("Вернуть папку по умолчанию"),
                                      self.on_reset_user)
        self.user_row.add_suffix(self.user_reset)
        game.add(self.user_row)
        self.language_row = combo_row(tr("Язык системы"), None, LANGUAGES, self.settings["language"])
        game.add(self.language_row)
        page.add(game)

        self.mods_group = Adw.PreferencesGroup(
            title=tr("Моды"), description=tr(
                "Распакуйте каждый мод в отдельную папку (с dvdroot_ps4 или сразу с chr/, parts/ и т. п.). "
                "При совпадении файлов побеждает мод ниже в списке. Применяется при запуске."))
        self.mods_enabled_row = Adw.SwitchRow(title=tr("Загружать моды"),
                                               active=self.settings["mods_enabled"])
        self.mods_group.add(self.mods_enabled_row)
        self.mods_folder_row = Adw.ActionRow(title=tr("Папка модов"))
        self.mods_folder_row.add_suffix(flat_button("folder-open-symbolic", tr("Выбрать папку модов"),
                                                    self.on_choose_mods))
        self.mods_folder_row.add_suffix(flat_button("system-file-manager-symbolic", tr("Открыть папку модов"),
                                                    lambda _b: open_folder(self, self.mods_dir())))
        self.mods_folder_row.add_suffix(flat_button("view-refresh-symbolic", tr("Обновить список"),
                                                    self.on_refresh_mods))
        self.mods_group.add(self.mods_folder_row)
        self.mod_list = FolderList(self.mods_group)
        self.refresh_mods()
        page.add(self.mods_group)

        self.patches_group = Adw.PreferencesGroup(
            title=tr("Сторонние патчи"),
            description=tr("XML-патчи в формате shadPS4 для версии 01.09 из папки патчей. "
                           "Применяются при запуске."))
        self.patches_folder_row = Adw.ActionRow(title=tr("Папка патчей"))
        self.patches_folder_row.add_suffix(flat_button("folder-open-symbolic", tr("Выбрать папку патчей"),
                                                       self.on_choose_patches))
        self.patches_folder_row.add_suffix(flat_button(
            "system-file-manager-symbolic", tr("Открыть папку патчей"),
            lambda _b: open_folder(self, patches_dir(self.settings))))
        self.patches_folder_row.add_suffix(flat_button("view-refresh-symbolic", tr("Обновить список"),
                                                       self.on_refresh_patches))
        self.patches_group.add(self.patches_folder_row)
        self.patch_list = FolderList(self.patches_group)
        self.refresh_patches()
        page.add(self.patches_group)

        screen = Adw.PreferencesGroup(title=tr("Экран"))
        self.output_row = combo_row(tr("Разрешение вывода"),
                                    tr("Апскейлер дорисовывает кадр; Steam Deck — 720p"),
                                    OUTPUT_RES, self.ini.get("output_res", "1920x1080"))
        screen.add(self.output_row)
        self.live_row = combo_row(tr("Смена разрешения на лету"),
                                  tr("Без перезапуска, но медленнее на Steam Deck и старых GPU"),
                                  LIVE_RESOLUTION, self.ini.get("live_resolution", "0"))
        screen.add(self.live_row)
        self.fullscreen_row = Adw.SwitchRow(title=tr("Полноэкранный режим"),
                                            active=self.settings["fullscreen"])
        screen.add(self.fullscreen_row)
        self.present_row = combo_row(tr("Режим показа кадров"), None, PRESENT_MODES,
                                     self.settings["present_mode"])
        screen.add(self.present_row)
        self.hdr_row = Adw.SwitchRow(title=tr("Разрешить HDR"), active=self.settings["hdr"])
        screen.add(self.hdr_row)
        self.vrr_row = Adw.SwitchRow(title="FreeSync / G-Sync (VRR)", subtitle=tr(
            "V-Sync и ограничение кадров чуть ниже частоты экрана"), active=self.settings["vrr"])
        screen.add(self.vrr_row)
        self.hud_row = combo_row(tr("Положение HUD на ультрашироком экране"), None, HUD_POSITIONS,
                                 self.ini.get("hud_position", "100"))
        screen.add(self.hud_row)
        page.add(screen)

        upscaler = Adw.PreferencesGroup(
            title=tr("Апскейлер"),
            description=tr("Хранится в bbport.ini; в игре меняется через меню (Insert или L3+R3)"))
        self.upscaler_row = combo_row(tr("Апскейлер"), None, UPSCALERS, self.ini["upscaler"])
        self.upscaler_row.connect("notify::selected", lambda *_: self.update_upscaler_status())
        upscaler.add(self.upscaler_row)
        self.preset_row = combo_row(tr("Пресет"), None, PRESETS, int(self.ini.get("preset", "4")))
        self.preset_row.connect("notify::selected", lambda *_: self.update_upscaler_status())
        self.output_row.connect("notify::selected", lambda *_: self.update_upscaler_status())
        upscaler.add(self.preset_row)
        self.sharpen_row = Adw.SwitchRow(title=tr("Резкость (RCAS)"),
                                         active=self.ini.get("sharpen") == "1")
        upscaler.add(self.sharpen_row)
        self.sharpness_row = Adw.SpinRow.new_with_range(0.0, 2.0, 0.05)
        self.sharpness_row.set_title(tr("Сила резкости"))
        self.sharpness_row.set_digits(2)
        self.sharpness_row.set_value(float(self.ini.get("sharpness", "0.5")))
        upscaler.add(self.sharpness_row)
        self.motion_row = Adw.SwitchRow(
            title=tr("Векторы движения объектов"),
            subtitle=tr("Меньше гостинга на персонажах; стоит около 10% FPS"),
            active=self.ini.get("object_motion") == "1")
        upscaler.add(self.motion_row)
        self.show_fps_row = Adw.SwitchRow(title=tr("Показывать FPS"),
                                          active=self.ini.get("show_fps") == "1")
        upscaler.add(self.show_fps_row)
        page.add(upscaler)

        effects = Adw.PreferencesGroup(title=tr("Эффекты игры"),
                                       description=tr("Патчи игры, применяются при запуске"))
        self.lod_row = combo_row(tr("Детализация моделей"), None, MODEL_LOD,
                                 self.ini.get("model_lod", "0"))
        effects.add(self.lod_row)
        self.effect_rows = {}
        for key, title, default in EFFECTS:
            row = Adw.SwitchRow(title=tr(title),
                                active=self.ini.get(key, "1" if default else "0") == "1")
            if key == "debug_menu":
                row.set_subtitle(tr("Установите DbgFont14h.ccm и DbgFont14h.tpf в dvdroot_ps4/font "
                                    "из мода Nexus #253"))
            elif key in online.OFFLINE_ONLY_EFFECTS:
                row.set_subtitle(tr("Только офлайн: в онлайн-игре остаётся выключенным"))
            self.effect_rows[key] = row
            effects.add(row)
        self.fov_row = Adw.SpinRow.new_with_range(30, 90, 1)
        self.fov_row.set_title(tr("Поле зрения"))
        self.fov_row.set_subtitle(tr("Градусы; в игре — 43"))
        fov = self.ini.get("fov", "43")
        self.fov_row.set_value(int(fov) if fov.isdigit() else 43)
        effects.add(self.fov_row)
        page.add(effects)

        frames = Adw.PreferencesGroup(title=tr("Частота кадров"))
        self.fps_row = combo_row(tr("Режим"), tr("Какой патч частоты кадров применить к игре"),
                                 FPS_MODES, self.settings["fps_mode"])
        frames.add(self.fps_row)
        self.limit_row = Adw.SpinRow.new_with_range(0, 480, 1)
        self.limit_row.set_title(tr("Ограничение FPS"))
        self.limit_row.set_subtitle(tr("0 — по частоте экрана (не выше 120 Гц); "
                                       "укажите число, чтобы ограничить иначе"))
        self.limit_row.set_value(self.settings["fps_limit"])
        frames.add(self.limit_row)
        page.add(frames)

        perf = Adw.PreferencesGroup(title=tr("Производительность"))
        self.pipe_row = combo_row(
            tr("Двухстадийный конвейер GPU"),
            tr("Быстрее на 20–30%; при нестабильности выключите"), DRAW_PIPE,
            self.settings["draw_pipe"])
        perf.add(self.pipe_row)
        self.readbacks_row = combo_row(tr("Чтение данных GPU процессором"), None, READBACKS,
                                       self.settings["readbacks"])
        perf.add(self.readbacks_row)
        self.latency_row = combo_row(tr("Низкая задержка"), None, LOW_LATENCY, self.settings["low_latency"])
        perf.add(self.latency_row)
        page.add(perf)

        saves = Adw.PreferencesGroup(title=tr("Сохранения"))
        self.backups_row = Adw.SwitchRow(title=tr("Копия сохранений при запуске"), subtitle=tr(
            "В папке save-backups рядом с сохранениями; хранятся последние 10"),
            active=self.settings["save_backups"])
        saves.add(self.backups_row)
        page.add(saves)

        dev = Adw.PreferencesGroup(title=tr("Для разработчика"))
        self.mangohud_row = Adw.SwitchRow(title="MangoHud", active=self.settings["mangohud"])
        dev.add(self.mangohud_row)
        self.stats_row = Adw.SwitchRow(title=tr("Статистика кадров в журнале"),
                                       subtitle="BB_FRAME_STATS",
                                       active=self.settings["frame_stats"])
        dev.add(self.stats_row)
        self.profile_row = Adw.SwitchRow(title=tr("Профиль GPU в журнале"),
                                         subtitle="BB_GPU_PROFILE",
                                         active=self.settings["gpu_profile"])
        dev.add(self.profile_row)
        self.validation_row = Adw.SwitchRow(title=tr("Слои валидации Vulkan"),
                                            subtitle=tr("Сильно замедляет"),
                                            active=self.settings["vk_validation"])
        dev.add(self.validation_row)
        self.extra_row = Adw.EntryRow(title=tr("Доп. переменные (ИМЯ=значение через пробел)"),
                                      text=self.settings["extra_env"])
        dev.add(self.extra_row)
        page.add(dev)
        return page

    def on_ui_language(self, row, _param):
        choice = combo_value(row)
        if choice == self.settings.get("ui_language", ""):
            return
        self.store()
        self.settings["ui_language"] = choice
        save_settings(self.settings)
        if self.process:
            # The log keeps streaming into the current page; rebuild on the next start.
            set_language(choice)
            self.toasts.add_toast(Adw.Toast(
                title=tr("Применится после перезапуска лаунчера, пока игра запущена")))
            return
        set_language(choice)
        # After this handler returns: the row that emitted the signal is replaced.
        GLib.idle_add(lambda: self.build() and False)

    def game_dir(self):
        return Path(self.settings["game_dir"]).expanduser()

    def user_dir(self):
        return user_folder(self.settings)

    def update_user_status(self):
        path = self.user_dir()
        custom = bool(self.settings["user_dir"])
        self.user_row.set_subtitle(str(path) if custom else tr("По умолчанию: {}").format(path))
        saves = path / "savedata"
        found = saves.is_dir() and any(saves.iterdir())
        self.user_status.set_from_icon_name("object-select-symbolic" if found else "document-new-symbolic")
        self.user_status.set_tooltip_text(tr("Найдены сохранения") if found
                                          else tr("Сохранений пока нет: игра создаст их здесь"))
        if online.coop_saves(path):
            # A Windows port's Seamless co-op saves: BBSC's goods stand in for the bells there.
            self.user_status.set_from_icon_name("dialog-warning-symbolic")
            self.user_status.set_tooltip_text(tr(
                "Это сохранения Seamless co-op из Windows (seamless-coop): без BBSC их предметы "
                "показываются как ?GoodsName?. Выберите обычную папку user"))
        self.user_reset.set_sensitive(custom)

    def choose_folder(self, title, current, done):
        dialog = Gtk.FileDialog(title=title)
        if Path(current).is_dir():
            dialog.set_initial_folder(Gio.File.new_for_path(str(current)))

        def finish(dialog, result):
            try:
                folder = dialog.select_folder_finish(result)
            except GLib.Error:
                return
            if folder:
                done(folder.get_path())
        dialog.select_folder(self, None, finish)

    def on_choose_user(self, _button):
        def chosen(path):
            self.settings["user_dir"] = path
            self.update_user_status()
            self.store()
        self.choose_folder(tr("Папка сохранений"), self.user_dir(), chosen)

    def on_reset_user(self, _button):
        self.settings["user_dir"] = ""
        self.update_user_status()
        self.store()

    def update_upscaler_status(self):
        value = combo_value(self.upscaler_row)
        if value == "fsr4":
            ok = (PORT_DIR / "fsr4_shaders").is_dir()
            hint = tr("Ассеты найдены") if ok else tr("Нет ассетов: tools/fetch_fsr4_assets.sh")
        elif value == "fsr411":
            directory = Path(os.environ["BB_FSR411_DIR"]) if os.environ.get("BB_FSR411_DIR") else (
                PORT_DIR / "fsr4_411" if (PORT_DIR / "fsr4_411").is_dir() else DATA_DIR / "fsr4_411")
            problem = fsr411_problem(directory, combo_value(self.output_row),
                                     int(combo_value(self.preset_row)))
            hint = tr("Ассеты для выбранного режима найдены") if not problem else (
                tr("{}. Установите полный набор fsr4_411 в {}.").format(problem, directory))
        else:
            hint = tr("Сглаживание в разрешении вывода без модели FSR") if value == "taa" else None
        self.preset_row.set_sensitive(value not in ("taa", "off"))
        self.sharpen_row.set_sensitive(value != "off")
        self.sharpness_row.set_sensitive(value != "off")
        self.upscaler_row.set_subtitle(hint or "")

    def update_game_status(self):
        path = self.game_dir()
        ok = bool(self.settings["game_dir"]) and (path / "eboot.bin").is_file()
        self.game_row.set_subtitle(str(path) if self.settings["game_dir"] else tr("не выбрана"))
        self.game_status.set_from_icon_name("object-select-symbolic" if ok else "dialog-warning-symbolic")
        self.game_status.set_tooltip_text(tr("Найден eboot.bin") if ok else tr("Нет eboot.bin в папке"))
        self.launch_button.set_sensitive(ok or self.process is not None)

    def on_choose_game(self, _button):
        def chosen(path):
            self.settings["game_dir"] = path
            self.update_game_status()
            self.store()
        self.choose_folder(tr("Папка игры (с eboot.bin)"), self.game_dir(), chosen)

    def store(self):
        s = self.settings
        s["language"] = combo_value(self.language_row)
        s["fullscreen"] = self.fullscreen_row.get_active()
        s["present_mode"] = combo_value(self.present_row)
        s["hdr"] = self.hdr_row.get_active()
        s["fps_mode"] = combo_value(self.fps_row)
        s["fps_limit"] = int(self.limit_row.get_value())
        s["draw_pipe"] = combo_value(self.pipe_row)
        s["readbacks"] = combo_value(self.readbacks_row)
        s["low_latency"] = combo_value(self.latency_row)
        s["vrr"] = self.vrr_row.get_active()
        s["save_backups"] = self.backups_row.get_active()
        s["mangohud"] = self.mangohud_row.get_active()
        s["frame_stats"] = self.stats_row.get_active()
        s["gpu_profile"] = self.profile_row.get_active()
        s["vk_validation"] = self.validation_row.get_active()
        s["extra_env"] = self.extra_row.get_text().strip()
        s["mods_enabled"] = self.mods_enabled_row.get_active()
        s["online"] = self.online_row.get_active()
        s["online_upnp"] = self.upnp_row.get_active()
        self.save_mod_profile()
        self.save_patch_profile()
        save_settings(s)
        self.ini.update({
            "upscaler": combo_value(self.upscaler_row),
            "preset": str(combo_value(self.preset_row)),
            "sharpen": "1" if self.sharpen_row.get_active() else "0",
            "sharpness": f"{self.sharpness_row.get_value():.2f}",
            "object_motion": "1" if self.motion_row.get_active() else "0",
            "show_fps": "1" if self.show_fps_row.get_active() else "0",
            "output_res": combo_value(self.output_row),
            "model_lod": combo_value(self.lod_row),
            "live_resolution": combo_value(self.live_row),
            "fov": str(int(self.fov_row.get_value())),
            "hud_position": combo_value(self.hud_row),
            **{key: "1" if row.get_active() else "0" for key, row in self.effect_rows.items()},
        })
        save_ini(self.ini, self.ini_lines)
        self.ini, self.ini_lines = load_ini()

    def environment(self):
        return game_environment(self.settings, log=lambda line: self.append_log(line + "\n"))

    # --- online page ---------------------------------------------------------------------

    def build_online_page(self):
        page = Adw.PreferencesPage()

        play = Adw.PreferencesGroup(
            title=tr("Онлайн"),
            description=tr("Призыв, послания и пятна крови других охотников. Применяется при запуске."))
        self.online_row = Adw.SwitchRow(title=tr("Играть онлайн"), active=self.settings["online"])
        play.add(self.online_row)
        # The two servers of the Windows launcher's Online page: Seamless co-op needs BBSC (Windows only).
        server = Adw.ActionRow(title="SuperMedo Server",
                               subtitle=tr("Серверы supermedo.com с бесплатным аккаунтом (ниже)"))
        server.add_prefix(Gtk.Image(icon_name="object-select-symbolic"))
        play.add(server)
        seamless = Adw.ActionRow(title=tr("SuperMedo Seamless co-op (бета)"), subtitle=tr(
            "Только в Windows: BBSC — программа для Windows, Linux-версию игры она не запускает"))
        seamless.add_prefix(Gtk.Image(icon_name="action-unavailable-symbolic"))
        seamless.set_sensitive(False)
        play.add(seamless)
        self.upnp_row = Adw.SwitchRow(title="UPnP", subtitle=tr("Роутер сам открывает порт игры другим охотникам"),
                                      active=self.settings["online_upnp"])
        play.add(self.upnp_row)
        self.check_row = Adw.ActionRow(title=tr("Соединение с сервером"))
        self.check_row.set_subtitle_selectable(True)
        self.check_row.add_suffix(text_button(tr("Проверить"), self.on_check_online))
        play.add(self.check_row)
        page.add(play)

        account = Adw.PreferencesGroup(title=tr("Аккаунт SuperMedo"))
        self.account_row = Adw.ActionRow(title=tr("Аккаунт"))
        self.account_row.set_subtitle_selectable(True)  # the sign-in page's address, for another device
        self.account_row.add_suffix(text_button(tr("Войти через Discord"), self.on_sign_in))
        self.sign_out_button = text_button(tr("Выйти"), self.on_sign_out)
        self.account_row.add_suffix(self.sign_out_button)
        account.add(self.account_row)
        # An account made elsewhere (the Windows launcher, the site): its Online ID and password, checked by the server.
        self.login_npid_row = Adw.EntryRow(title=tr("Online ID"))
        account.add(self.login_npid_row)
        self.login_password_row = Adw.PasswordEntryRow(title=tr("Пароль"))
        account.add(self.login_password_row)
        self.login_row = Adw.ActionRow(title=tr("Вход по паролю"))
        self.login_row.add_suffix(text_button(tr("Войти"), self.on_password_sign_in))
        account.add(self.login_row)
        page.add(account)

        new = Adw.PreferencesGroup(title=tr("Новый аккаунт SuperMedo"), description=tr(
            "Не нужен при входе через Discord: Discord сам создаёт аккаунт с вашим именем в Discord"))
        self.new_npid_row = Adw.EntryRow(title=tr("Новый Online ID (1–16 букв, цифр, _ или -)"))
        new.add(self.new_npid_row)
        self.new_password_row = Adw.PasswordEntryRow(title=tr("Новый пароль"))
        new.add(self.new_password_row)
        self.new_email_row = Adw.EntryRow(title=tr("Эл. почта (только для создания аккаунта)"))
        new.add(self.new_email_row)
        self.create_row = Adw.ActionRow(title=tr("Создать аккаунт"))
        self.create_row.add_suffix(text_button(tr("Создать"), self.on_create_account))
        new.add(self.create_row)
        page.add(new)

        # Party (play with friends): a party code from SuperMedo Server. While in a party, the hunter's signs
        # and Beckoning Bell find only the party's hunters (the server gives them the party's summon password).
        self.party_group = Adw.PreferencesGroup(title=tr("Отряд (игра с друзьями)"), description=tr(
            "Пока вы в отряде, ваши знаки призыва и Колокол зова находят только охотников отряда, "
            "без ограничения по уровню. До 4 охотников."))
        self.party_row = Adw.ActionRow(title=tr("Ваш отряд"))
        self.party_row.add_suffix(text_button(tr("Обновить"), lambda _b: self.party_act("status")))
        self.party_group.add(self.party_row)
        self.code_row = Adw.ActionRow(title=tr("Код отряда"), subtitle=tr(
            "Отправьте код друзьям. Новый код отменяет старый; кто уже в отряде, остаётся"))
        self.code_label = Gtk.Label(selectable=True, valign=Gtk.Align.CENTER)
        self.code_label.add_css_class("title-3")
        self.code_row.add_suffix(self.code_label)
        self.code_row.add_suffix(flat_button("edit-copy-symbolic", tr("Скопировать"), self.on_copy_code))
        self.code_row.add_suffix(text_button(tr("Новый код"), lambda _b: self.party_act("new_code")))
        self.party_group.add(self.code_row)
        self.join_row = Adw.EntryRow(title=tr("Вступить по коду друга (SM-XXXX-XXXX)"), show_apply_button=True)
        self.join_row.connect("apply", lambda row: self.party_act("join", {"code": row.get_text().strip()}))
        self.party_group.add(self.join_row)
        self.party_actions = Adw.ActionRow()
        self.party_actions.add_suffix(text_button(tr("Создать код отряда"), lambda _b: self.party_act("host")))
        self.leave_button = text_button(tr("Покинуть отряд"), lambda _b: self.party_act("leave"))
        self.party_actions.add_suffix(self.leave_button)
        self.party_group.add(self.party_actions)
        page.add(self.party_group)
        # The party's hunters with their levels; the host may kick one.
        self.members_group = Adw.PreferencesGroup()
        self.member_list = FolderList(self.members_group)
        page.add(self.members_group)
        return page

    def update_account(self):
        name = online.account(self.settings)
        self.account_row.set_subtitle(tr("Вход выполнен: {}").format(name) if name else tr(
            "Вход не выполнен: войдите через Discord или создайте аккаунт ниже"))
        self.sign_out_button.set_sensitive(bool(name))
        self.party_group.set_sensitive(bool(name))

    def set_account(self, npid, password):
        if npid != self.settings["supermedo_npid"]:
            self.settings["party_key"] = ""  # another account: its own key
            self.party = None
        self.settings["supermedo_npid"], self.settings["supermedo_pass"] = npid, password
        save_settings(self.settings)
        self.update_account()
        self.show_party(self.party)

    def on_check_online(self, _button):
        self.check_row.set_subtitle(tr("Проверка…"))
        run_in_background(online.server_check, lambda lines: self.check_row.set_subtitle(
            GLib.markup_escape_text(str(lines))))

    def open_uri(self, uri):
        try:
            Gtk.UriLauncher.new(uri).launch(self, None, None)
        except (AttributeError, GLib.Error):
            try:
                Gio.AppInfo.launch_default_for_uri(uri, None)
            except GLib.Error:
                pass

    def on_sign_in(self, _button):
        self.account_row.set_subtitle(tr("Запуск входа…"))

        def on_page(uri):
            def show():
                self.open_uri(uri)
                # No browser here (Steam Deck game mode): the address to open on another device.
                self.account_row.set_subtitle(GLib.markup_escape_text(
                    tr("Войдите через Discord на открывшейся странице и вернитесь сюда: {}").format(uri)))
            GLib.idle_add(lambda: show() and False)

        def done(result):
            if isinstance(result, Exception):
                self.update_account()
                self.account_row.set_subtitle(GLib.markup_escape_text(tr("Вход не удался: {}").format(result)))
                return
            self.set_account(*result)
        run_in_background(lambda: online.discord_sign_in(on_page), done)

    def on_sign_out(self, _button):
        self.set_account("", "")

    def on_password_sign_in(self, _button):
        npid = self.login_npid_row.get_text().strip()
        password = self.login_password_row.get_text()
        if not npid or not password:
            self.login_row.set_subtitle(tr("Введите Online ID и пароль"))
            return
        self.login_row.set_subtitle(tr("Проверка…"))

        def done(problem):
            if isinstance(problem, OSError):
                problem = tr("Нет связи с сервером: {}").format(problem)
            elif isinstance(problem, Exception):
                problem = str(problem)
            if problem:
                self.login_row.set_subtitle(GLib.markup_escape_text(tr("Вход не удался: {}").format(tr(problem))))
                return
            self.login_row.set_subtitle("")
            self.login_password_row.set_text("")
            self.set_account(npid, password)
        run_in_background(lambda: online.shadnet_check_login(online.SUPERMEDO_SERVER, npid, password), done)

    def on_create_account(self, _button):
        npid = self.new_npid_row.get_text().strip()
        password = self.new_password_row.get_text()
        mail = self.new_email_row.get_text().strip()
        if not npid or not password or "@" not in mail:
            self.create_row.set_subtitle(tr("Введите новый Online ID, пароль и адрес эл. почты"))
            return
        self.create_row.set_subtitle(tr("Создание аккаунта…"))

        def done(problem):
            if isinstance(problem, OSError):
                problem = tr("Сервер SuperMedo недоступен: {}").format(problem)
            elif isinstance(problem, Exception):
                problem = tr("Не удалось создать аккаунт: {}").format(problem)
            if problem:
                self.create_row.set_subtitle(GLib.markup_escape_text(problem))
                return
            self.create_row.set_subtitle(tr("Аккаунт {} создан, вход выполнен. Нажмите «Запустить»").format(npid))
            self.set_account(npid, password)
        run_in_background(lambda: online.shadnet_register(online.SUPERMEDO_SERVER, npid, password, mail), done)

    def on_copy_code(self, _button):
        self.get_clipboard().set(self.code_label.get_text())
        self.toasts.add_toast(Adw.Toast(title=tr("Код скопирован")))

    def party_act(self, call, body=None, quiet=False):
        """A party call (host, join, leave, kick, new_code, status) off the GTK thread; its answer is shown."""
        name, password = online.account(self.settings), self.settings["supermedo_pass"]
        if not name:
            if not quiet:
                self.party_actions.set_subtitle(tr("Сначала войдите в аккаунт SuperMedo (выше)"))
            return
        if not quiet:
            self.party_actions.set_subtitle(tr("Проверка…"))
        key = self.settings.get("party_key", "")

        def done(result):
            if name != online.account(self.settings):
                return  # signed out or into another account meanwhile
            if isinstance(result, Exception):
                if not quiet:
                    self.party_actions.set_subtitle(GLib.markup_escape_text(
                        tr("Ответ сервера SuperMedo: {}").format(result)))
                return
            answer, new_key = result
            if new_key != self.settings.get("party_key"):
                self.settings["party_key"] = new_key
                save_settings(self.settings)
            if not quiet:
                self.party_actions.set_subtitle("")
            self.show_party(answer.get("party"))
        run_in_background(lambda: online.party_act(call, body, name, password, key), done)

    def show_party(self, party):
        self.party = party
        self.member_list.clear()
        self.code_row.set_visible(bool(party and party.get("code")))  # the host's
        self.leave_button.set_sensitive(bool(party))
        if not party:
            self.party_row.set_subtitle(tr("Не в отряде"))
            self.members_group.set_visible(False)
            return
        hunters = party.get("members", [])
        self.party_row.set_subtitle(GLib.markup_escape_text(" · ".join(m["name"] for m in hunters)))
        self.code_label.set_text(party.get("code") or "")
        self.members_group.set_title(tr("В отряде {}/{}").format(len(hunters), party.get("size", 4)))
        self.members_group.set_visible(True)
        for m in hunters:
            state = [tr("хост")] if m.get("host") else []
            state.append(tr("в сети") if m.get("online") else tr("не в сети"))
            if m.get("level"):
                state.append(tr("уровень {}").format(m["level"]))
            row = Adw.ActionRow(title=GLib.markup_escape_text(m["name"] + ("  ★" if m.get("host") else "")),
                                subtitle=", ".join(state))
            icon = Gtk.Image(icon_name="avatar-default-symbolic")
            if not m.get("online"):
                icon.add_css_class("dim-label")
            row.add_prefix(icon)
            if party.get("code") and not m.get("host"):
                row.add_suffix(text_button(tr("Исключить"), lambda _b, n=m["name"]: self.party_act(
                    "kick", {"member": n})))
            self.member_list.add(m["name"], row)

    def poll_party(self):
        if self.stack.get_visible_child_name() == "online" and self.party and self.settings.get("party_key"):
            self.party_act("status", quiet=True)
        return True

    # --- mods ----------------------------------------------------------------------------

    def mods_dir(self):
        return Path(self.settings.get("mods_dir") or DATA_DIR / "mods").expanduser()

    def save_mod_profile(self):
        DATA_DIR.mkdir(parents=True, exist_ok=True)
        rows = self.mod_list.rows
        profile = {"order": [name for name, _ in rows],
                   "disabled": [name for name, row in rows if not row.get_active()]}
        (DATA_DIR / "mods.json").write_text(
            json.dumps(profile, indent=2, ensure_ascii=False) + "\n", encoding='utf-8')

    def refresh_mods(self):
        self.mod_list.clear()
        self.mods_folder_row.set_subtitle(str(self.mods_dir()))
        try:
            profile = json.loads((DATA_DIR / "mods.json").read_text(encoding='utf-8'))
        except (OSError, ValueError):
            profile = {}
        available = discover_mods(self.mods_dir())
        order = list(dict.fromkeys(n for n in [*profile.get("order", []), *available] if n in available))
        for name in order:
            row = Adw.SwitchRow(title=name, active=name not in profile.get("disabled", []))
            row.add_suffix(flat_button("go-up-symbolic", tr("Загрузить раньше"),
                                        lambda _b, n=name: self.move_mod(n, -1)))
            row.add_suffix(flat_button("go-down-symbolic", tr("Загрузить позже"),
                                        lambda _b, n=name: self.move_mod(n, 1)))
            self.mod_list.add(name, row)
        if not order:
            self.mods_folder_row.set_subtitle(f"{self.mods_dir()} — {tr('Модов нет')}")

    def move_mod(self, name, direction):
        rows = self.mod_list.rows
        index = next(i for i, (n, _) in enumerate(rows) if n == name)
        target = index + direction
        if 0 <= target < len(rows):
            rows[index], rows[target] = rows[target], rows[index]
            self.save_mod_profile()
            self.refresh_mods()

    def on_refresh_mods(self, _button):
        self.save_mod_profile()
        self.refresh_mods()

    def on_choose_mods(self, _button):
        self.save_mod_profile()

        def chosen(path):
            self.settings["mods_dir"] = path
            self.refresh_mods()
            self.store()
        self.choose_folder(tr("Папка модов"), self.mods_dir(), chosen)

    # --- third-party patches -------------------------------------------------------------

    def save_patch_profile(self):
        """patches.json: the switches that differ from each file's isEnabled."""
        enabled, disabled = [], []
        for key, row in self.patch_list.rows:
            if row.get_active() != row.default:
                (enabled if row.get_active() else disabled).append(key)
        path = DATA_DIR / "patches.json"
        try:
            profile = json.loads(path.read_text(encoding='utf-8'))
        except (OSError, ValueError):
            profile = {}
        # Patches of files not listed now (another folder) keep their choice.
        shown = {key for key, _ in self.patch_list.rows}
        profile = {"enabled": sorted({k for k in profile.get("enabled", []) if k not in shown} | set(enabled)),
                   "disabled": sorted({k for k in profile.get("disabled", []) if k not in shown} | set(disabled))}
        DATA_DIR.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(profile, indent=2, ensure_ascii=False) + "\n", encoding='utf-8')

    def refresh_patches(self):
        self.patch_list.clear()
        directory = patches_dir(self.settings)
        try:
            profile = json.loads((DATA_DIR / "patches.json").read_text(encoding='utf-8'))
        except (OSError, ValueError):
            profile = {}
        found = external_patches(directory)
        for key, _path, meta in found:
            default = meta.get("isEnabled", "false").lower() == "true"
            active = key in profile.get("enabled", []) or (default and key not in profile.get("disabled", []))
            subtitle = key.split("/", 1)[0]
            if meta.get("Author"):
                subtitle += " · " + tr("Автор: {}").format(meta.get("Author"))
            row = Adw.SwitchRow(title=GLib.markup_escape_text(meta.get("Name") or key),
                                subtitle=GLib.markup_escape_text(subtitle), active=active)
            if meta.get("Note"):
                row.set_tooltip_text(meta.get("Note").replace("\\n", "\n"))
            row.default = default
            self.patch_list.add(key, row)
        self.patches_folder_row.set_subtitle(
            str(directory) if found else f"{directory} — {tr('Патчей нет')}")

    def on_refresh_patches(self, _button):
        self.save_patch_profile()
        self.refresh_patches()

    def on_choose_patches(self, _button):
        self.save_patch_profile()

        def chosen(path):
            self.settings["patches_dir"] = path
            self.refresh_patches()
            self.store()
        self.choose_folder(tr("Папка патчей"), patches_dir(self.settings), chosen)

    # --- log page ------------------------------------------------------------------------

    def build_log_page(self):
        box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL)
        self.log_view = Gtk.TextView(editable=False, monospace=True, cursor_visible=False,
                                     wrap_mode=Gtk.WrapMode.WORD_CHAR)
        self.log_view.set_top_margin(8)
        self.log_view.set_left_margin(8)
        self.log_view.set_right_margin(8)
        scroller = Gtk.ScrolledWindow(vexpand=True, child=self.log_view)
        self.log_scroller = scroller
        box.append(scroller)
        return box

    def append_log(self, text):
        if self.run_log:
            try:
                self.run_log.write(text)
            except OSError:
                self.run_log = None  # full drive: the Log page still shows it
        buffer = self.log_view.get_buffer()
        buffer.insert(buffer.get_end_iter(), text)
        extra = buffer.get_line_count() - MAX_LOG_LINES
        if extra > 0:
            buffer.delete(buffer.get_start_iter(), buffer.get_iter_at_line(extra)[1])
        adj = self.log_scroller.get_vadjustment()
        GLib.idle_add(lambda: adj.set_value(adj.get_upper()) and False)

    # --- process -------------------------------------------------------------------------

    def on_launch(self, _button):
        if self.process:
            self.stop_game()
            return
        self.store()
        if self.settings["online"] and not online.account(self.settings):
            self.toasts.add_toast(Adw.Toast(title=tr(
                "Войдите в аккаунт SuperMedo на странице «Онлайн» или выключите онлайн")))
            self.stack.set_visible_child_name("online")
            return
        # Fair play: patches.py leaves these out of every online start. Said here, so the player can choose.
        left_out = self.offline_only_on() if self.settings["online"] else []
        if left_out:
            self.ask_fair_play(left_out)
            return
        self.start_game()

    def offline_only_on(self):
        """What is on of what patches.py leaves out of an online start: cheats, debug features, third-party patches."""
        names = [tr(title).split(" (")[0] for key, title, _default in EFFECTS
                 if key in online.OFFLINE_ONLY_EFFECTS and self.effect_rows[key].get_active()]
        return names + [key for key, row in self.patch_list.rows if row.get_active()]

    def ask_fair_play(self, names):
        dialog = Adw.MessageDialog(
            transient_for=self, heading="Bloodborne", body=tr(
                "Читы и функции отладки работают только офлайн — честно по отношению к другим игрокам. "
                "Сейчас включены: {}.").format(", ".join(names)))
        dialog.add_response("cancel", tr("Отмена"))
        dialog.add_response("offline", tr("Играть офлайн с ними"))
        dialog.add_response("online", tr("Играть онлайн без них"))
        dialog.set_default_response("online")
        dialog.set_close_response("cancel")

        def answered(_dialog, response):
            if response == "cancel":
                return
            self.start_game(offline_once=response == "offline")
        dialog.connect("response", answered)
        dialog.present()

    def start_game(self, offline_once=False):
        self.log_view.get_buffer().set_text("")
        self.open_run_log()
        try:
            # This start only: the Online page's switch stays on.
            environment = game_environment({**self.settings, "online": False}, log=print) if offline_once \
                else self.environment()
        except OSError as error:
            self.toasts.add_toast(Adw.Toast(title=tr("Не удалось запустить: {}").format(error)))
            return
        launcher = Gio.SubprocessLauncher.new(
            Gio.SubprocessFlags.STDOUT_PIPE | Gio.SubprocessFlags.STDERR_MERGE)
        launcher.set_environ([f"{k}={v}" for k, v in environment.items()])
        launcher.set_cwd(str(PORT_DIR))
        try:
            # setsid: the game and its helpers form one process group, stopped together.
            self.process = launcher.spawnv(["setsid", "bash", str(PORT_DIR / "run.sh")])
        except GLib.Error as error:
            self.toasts.add_toast(Adw.Toast(title=tr("Не удалось запустить: {}").format(error.message)))
            return
        self.stream = Gio.DataInputStream.new(self.process.get_stdout_pipe())
        self.read_line()
        self.process.wait_async(None, self.on_exit)
        self.update_launch_button()
        self.stack.set_visible_child_name("log")

    def open_run_log(self):
        """last_run.log in the saves folder (the previous run kept as last_run.1.log)."""
        self.close_run_log()
        log = user_folder(self.settings) / "last_run.log"
        try:
            log.parent.mkdir(parents=True, exist_ok=True)
            if log.exists():
                log.replace(log.with_name("last_run.1.log"))
            self.run_log = open(log, "w", encoding="utf-8", buffering=1)
        except OSError:
            self.run_log = None

    def close_run_log(self):
        if self.run_log:
            try:
                self.run_log.close()
            except OSError:
                pass
            self.run_log = None

    def read_line(self):
        self.stream.read_line_async(GLib.PRIORITY_DEFAULT, None, self.on_line)

    def on_line(self, stream, result):
        try:
            line, _length = stream.read_line_finish_utf8(result)
        except GLib.Error:
            return
        if line is None:
            return
        self.append_log(line + "\n")
        self.read_line()

    def stop_game(self):
        if not self.process:
            return
        pid = int(self.process.get_identifier())
        try:
            os.killpg(pid, signal.SIGTERM)
        except OSError:
            self.process.force_exit()
        GLib.timeout_add_seconds(3, self.kill_if_running, pid)

    def kill_if_running(self, pid):
        if self.process:
            try:
                os.killpg(pid, signal.SIGKILL)
            except OSError:
                pass
        return False

    def on_exit(self, process, result):
        try:
            process.wait_finish(result)
        except GLib.Error:
            pass
        status = process.get_exit_status() if process.get_if_exited() else -1
        self.process = None
        self.update_launch_button()
        self.update_game_status()
        self.append_log(tr("\n— игра завершилась (код {}) —\n").format(status))
        self.close_run_log()

    def on_close(self, _window):
        self.store()
        if self.process:
            self.stop_game()
        return False


class LauncherApp(Adw.Application):
    def __init__(self):
        super().__init__(application_id="io.github.bbport.Launcher",
                         flags=Gio.ApplicationFlags.DEFAULT_FLAGS)

    def do_activate(self):
        window = self.get_active_window() or LauncherWindow(self)
        window.present()


def play():
    """--play: the game with the saved settings, no window (Steam Deck game mode)."""
    settings = load_settings()
    if not (Path(settings["game_dir"]).expanduser() / "eboot.bin").is_file():
        print("bbport: choose the game folder in the launcher first", file=sys.stderr)
        return 1
    if settings["online"] and not online.account(settings):
        print("bbport: online is on, but no SuperMedo account: sign in on the launcher's Online page",
              file=sys.stderr)
        return 1
    os.chdir(PORT_DIR)
    os.execvpe("bash", ["bash", str(PORT_DIR / "run.sh")], game_environment(settings))


if __name__ == "__main__":
    if "--play" in sys.argv[1:]:
        sys.exit(play())
    sys.exit(LauncherApp().run(sys.argv))
