# SPDX-License-Identifier: GPL-2.0-or-later
"""The monitors of this PC and the game's window on them.

The port opens its window at 1920 x 1080 in the middle of the main monitor (gpu/shim/window.cpp)
and keeps nothing of where the player moved it or how big they made it. The launcher places the
window instead, once it exists: on the chosen monitor, at the chosen size (the output resolution,
the size the player left it at last time, or their own), and in fullscreen over the whole chosen
monitor. The game's in-game restart opens a new window; it is placed the same way. Borderless
fullscreen ('borderless_fullscreen', with Fullscreen off): the game opens in a window, and the
launcher takes its frame off and keeps it over the whole monitor it is on, so Alt+Tab is instant and
other windows can go on top of it.

Every size here is in physical pixels: the code that measures monitors and moves the window runs
per-monitor DPI aware, whatever the rest of the launcher is."""
import ctypes
import re
import struct
import threading
import time
from ctypes import wintypes

MIN_SIZE, MAX_SIZE = (640, 360), (7680, 4320)
SWP_NOZORDER, SWP_NOACTIVATE, SWP_FRAMECHANGED, SWP_NOOWNERZORDER = 0x0004, 0x0010, 0x0020, 0x0200
GWL_STYLE, GWL_EXSTYLE = -16, -20
# Borderless: no caption, sizing border or maximize box (minimize and the system menu stay: the taskbar
# and Win+D still minimize it), and no edges.
WS_CAPTION, WS_THICKFRAME, WS_MAXIMIZEBOX = 0x00C00000, 0x00040000, 0x00010000
FRAME = WS_CAPTION | WS_THICKFRAME | WS_MAXIMIZEBOX
EDGES = 0x00000001 | 0x00000100 | 0x00000200 | 0x00020000  # WS_EX_DLGMODALFRAME, WINDOWEDGE, CLIENTEDGE, STATICEDGE
DWMWA_WINDOW_CORNER_PREFERENCE, DWMWCP_DONOTROUND = 33, 1
PER_MONITOR_AWARE_V2 = ctypes.c_void_p(-4)


def parse_size(text):
    """(width, height) from '2400x1080', '2400 × 1080' or '2400*1080', rounded down to even numbers
    (the upscalers want them); None when it is not a size or outside 640 x 360 to 7680 x 4320."""
    match = re.fullmatch(r'\s*(\d{3,5})\s*[x×*X]\s*(\d{3,5})\s*', str(text or ''))
    if not match:
        return None
    w, h = int(match.group(1)) // 2 * 2, int(match.group(2)) // 2 * 2
    if not (MIN_SIZE[0] <= w <= MAX_SIZE[0] and MIN_SIZE[1] <= h <= MAX_SIZE[1]):
        return None
    return w, h


def size_text(size):
    return f'{size[0]}x{size[1]}'


def narrower_than_16_9(size):
    """True for 16:10, 4:3 and the like: the port draws those 16:9 and stretches them."""
    return size[0] * 9 < size[1] * 16


class physical:
    """with physical(): this thread measures and moves windows in physical pixels."""

    def __enter__(self):
        try:
            self.old = ctypes.windll.user32.SetThreadDpiAwarenessContext(PER_MONITOR_AWARE_V2)
        except (AttributeError, OSError):  # before Windows 10 1607: the process's own awareness
            self.old = None
        return self

    def __exit__(self, *_exc):
        if self.old:
            ctypes.windll.user32.SetThreadDpiAwarenessContext(ctypes.c_void_p(self.old))


class MONITORINFOEXW(ctypes.Structure):
    _fields_ = [('cbSize', wintypes.DWORD), ('rcMonitor', wintypes.RECT), ('rcWork', wintypes.RECT),
                ('dwFlags', wintypes.DWORD), ('szDevice', wintypes.WCHAR * 32)]


class Monitor:
    def __init__(self, device, rect, work, primary, name=''):
        self.device, self.rect, self.work, self.primary, self.name = device, rect, work, primary, name

    @property
    def size(self):
        return self.rect[2] - self.rect[0], self.rect[3] - self.rect[1]

    @property
    def number(self):
        digits = re.findall(r'\d+', self.device)
        return int(digits[-1]) if digits else 0

    def __repr__(self):
        return f'Monitor({self.device!r}, {self.rect}, primary={self.primary}, name={self.name!r})'


class SourceName(ctypes.Structure):  # DISPLAYCONFIG_SOURCE_DEVICE_NAME
    _fields_ = [('type', ctypes.c_uint32), ('size', ctypes.c_uint32), ('adapter_low', ctypes.c_uint32),
                ('adapter_high', ctypes.c_int32), ('id', ctypes.c_uint32), ('gdi', wintypes.WCHAR * 32)]


class TargetName(ctypes.Structure):  # DISPLAYCONFIG_TARGET_DEVICE_NAME
    _fields_ = [('type', ctypes.c_uint32), ('size', ctypes.c_uint32), ('adapter_low', ctypes.c_uint32),
                ('adapter_high', ctypes.c_int32), ('id', ctypes.c_uint32), ('flags', ctypes.c_uint32),
                ('technology', ctypes.c_uint32), ('maker', ctypes.c_uint16), ('product', ctypes.c_uint16),
                ('connector', ctypes.c_uint32), ('name', wintypes.WCHAR * 64), ('path', wintypes.WCHAR * 128)]


PATH_INFO = 72  # sizeof(DISPLAYCONFIG_PATH_INFO): source adapter and id at 0 and 8, target's at 20 and 28


def monitor_names():
    """{GDI device name: the monitor's own name} from Windows' display configuration, e.g.
    {'\\\\.\\DISPLAY1': 'DELL S2721DGF'}; empty when Windows does not say."""
    names = {}
    try:
        user32 = ctypes.windll.user32
        paths, modes = ctypes.c_uint32(), ctypes.c_uint32()
        QDC_ONLY_ACTIVE_PATHS = 2
        if user32.GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, ctypes.byref(paths), ctypes.byref(modes)):
            return names
        path_data = (ctypes.c_byte * (PATH_INFO * paths.value))()
        mode_data = (ctypes.c_byte * (64 * modes.value))()  # DISPLAYCONFIG_MODE_INFO
        if user32.QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, ctypes.byref(paths), path_data, ctypes.byref(modes),
                                     mode_data, None):
            return names
        raw = bytes(path_data)
        for at in range(0, paths.value * PATH_INFO, PATH_INFO):
            low, high, source_id = struct.unpack_from('<Iii', raw, at)
            target_low, target_high, target_id = struct.unpack_from('<Iii', raw, at + 20)
            source = SourceName(1, ctypes.sizeof(SourceName), low, high, source_id)
            target = TargetName(2, ctypes.sizeof(TargetName), target_low, target_high, target_id)
            if user32.DisplayConfigGetDeviceInfo(ctypes.byref(source)) or \
                    user32.DisplayConfigGetDeviceInfo(ctypes.byref(target)):
                continue
            if target.name.strip():
                names[source.gdi] = target.name.strip()
    except (AttributeError, OSError, ValueError):
        pass
    return names


def monitors():
    """The monitors, main monitor first, then by Windows' number; [] where Windows cannot say."""
    found = []
    try:
        user32 = ctypes.windll.user32
        with physical():
            def each(handle, _dc, _rect, _data):
                info = MONITORINFOEXW()
                info.cbSize = ctypes.sizeof(info)
                if user32.GetMonitorInfoW(handle, ctypes.byref(info)):
                    r, w = info.rcMonitor, info.rcWork
                    found.append(Monitor(info.szDevice, (r.left, r.top, r.right, r.bottom),
                                         (w.left, w.top, w.right, w.bottom), bool(info.dwFlags & 1)))
                return True
            callback = ctypes.WINFUNCTYPE(ctypes.c_int, wintypes.HMONITOR, wintypes.HDC,
                                          ctypes.POINTER(wintypes.RECT), wintypes.LPARAM)(each)
            user32.EnumDisplayMonitors(None, None, callback, 0)
    except (AttributeError, OSError):
        return []
    names = monitor_names()
    for m in found:
        m.name = names.get(m.device, '')
    return sorted(found, key=lambda m: (not m.primary, m.number))


def monitor(device, available=None):
    """The chosen monitor ('' or one that is gone: the main one); None without any."""
    available = monitors() if available is None else available
    return next((m for m in available if device and m.device == device), None) or \
        next((m for m in available if m.primary), None) or (available[0] if available else None)


def fit(size, room):
    """`size` made smaller (same shape) until it fits in `room`."""
    w, h = size
    scale = min(1.0, room[0] / w, room[1] / h)
    return max(2, int(w * scale) // 2 * 2), max(2, int(h * scale) // 2 * 2)


# ---- the game's window -----------------------------------------------------------------------
def game_windows():
    """The visible top-level windows of the running game (bb-probe.exe)."""
    import bbport_benchmark as bench
    return [hwnd for hwnd in (bench.game_window(pid) for pid in bench.game_pids()) if hwnd]


def window_rect(hwnd):
    rect = wintypes.RECT()
    ctypes.windll.user32.GetWindowRect(hwnd, ctypes.byref(rect))
    return rect.left, rect.top, rect.right, rect.bottom


def client_size(hwnd):
    rect = wintypes.RECT()
    ctypes.windll.user32.GetClientRect(hwnd, ctypes.byref(rect))
    return rect.right, rect.bottom


def frame_for(hwnd, client, dpi):
    """The window's outer size for a client size (its own frame, at the target monitor's DPI)."""
    user32 = ctypes.windll.user32
    rect = wintypes.RECT(0, 0, client[0], client[1])
    style, extended = user32.GetWindowLongW(hwnd, GWL_STYLE), user32.GetWindowLongW(hwnd, GWL_EXSTYLE)
    try:
        user32.AdjustWindowRectExForDpi(ctypes.byref(rect), style & 0xFFFFFFFF, False, extended & 0xFFFFFFFF, dpi)
    except AttributeError:
        user32.AdjustWindowRectEx(ctypes.byref(rect), style & 0xFFFFFFFF, False, extended & 0xFFFFFFFF)
    return rect.right - rect.left, rect.bottom - rect.top, -rect.left, -rect.top


def monitor_dpi(m):
    try:
        handle = ctypes.windll.user32.MonitorFromPoint(wintypes.POINT(m.rect[0] + 1, m.rect[1] + 1), 2)
        x, y = ctypes.c_uint(), ctypes.c_uint()
        ctypes.windll.shcore.GetDpiForMonitor(handle, 0, ctypes.byref(x), ctypes.byref(y))
        return x.value or 96
    except (AttributeError, OSError):
        return 96


def frameless(style, extended):
    """The window styles without frame and edges (borderless fullscreen)."""
    return style & ~FRAME, extended & ~EDGES


def borderless(settings):
    """Borderless fullscreen: on, and Fullscreen off (Fullscreen wins when an older file has both)."""
    return bool(settings.get('borderless_fullscreen')) and not settings.get('fullscreen')


def window_long(hwnd, index):
    user32 = ctypes.windll.user32
    get = getattr(user32, 'GetWindowLongPtrW', user32.GetWindowLongW)
    get.restype, get.argtypes = ctypes.c_ssize_t, (wintypes.HWND, ctypes.c_int)
    return get(hwnd, index)


def set_window_long(hwnd, index, value):
    user32 = ctypes.windll.user32
    put = getattr(user32, 'SetWindowLongPtrW', user32.SetWindowLongW)
    put.restype, put.argtypes = ctypes.c_ssize_t, (wintypes.HWND, ctypes.c_int, ctypes.c_ssize_t)
    put(hwnd, index, value)


def monitor_of(hwnd):
    """The rect of the monitor the window is on (most of it), or None."""
    user32 = ctypes.windll.user32
    user32.MonitorFromWindow.restype = wintypes.HMONITOR
    user32.MonitorFromWindow.argtypes = (wintypes.HWND, wintypes.DWORD)
    info = MONITORINFOEXW()
    info.cbSize = ctypes.sizeof(info)
    handle = user32.MonitorFromWindow(hwnd, 2)  # MONITOR_DEFAULTTONEAREST
    if not handle or not user32.GetMonitorInfoW(handle, ctypes.byref(info)):
        return None
    r = info.rcMonitor
    return r.left, r.top, r.right, r.bottom


def make_borderless(hwnd, rect):
    """The window without its frame, over `rect` (a monitor, physical pixels); True when it changed. Only
    what differs is set: SDL may put its frame back (a DPI change, a restore), and this runs every 0.25 s."""
    user32 = ctypes.windll.user32
    style, extended = window_long(hwnd, GWL_STYLE), window_long(hwnd, GWL_EXSTYLE)
    new_style, new_extended = frameless(style, extended)
    framed = (new_style, new_extended) != (style, extended)
    if framed:
        set_window_long(hwnd, GWL_STYLE, new_style)
        set_window_long(hwnd, GWL_EXSTYLE, new_extended)
    if not framed and window_rect(hwnd) == tuple(rect):
        return False
    # The normal Z order: after Alt+Tab other windows stay above the game.
    user32.SetWindowPos(hwnd, None, rect[0], rect[1], rect[2] - rect[0], rect[3] - rect[1],
                        SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER | (SWP_FRAMECHANGED if framed else 0))
    return True


def square_corners(hwnd):
    """Windows 11 rounds the corners of framed windows; a window over the monitor keeps square ones."""
    try:
        corner = ctypes.c_int(DWMWCP_DONOTROUND)
        ctypes.windll.dwmapi.DwmSetWindowAttribute(wintypes.HWND(hwnd), DWMWA_WINDOW_CORNER_PREFERENCE,
                                                   ctypes.byref(corner), ctypes.sizeof(corner))
    except (AttributeError, OSError):  # before Windows 11
        pass


def window_target(settings, output):
    """The game window's client size: the output resolution, the last size, the player's own, or
    None ('game': as the port opens it, the launcher leaves its size alone)."""
    mode = settings.get('window_size') or 'output'
    if mode == 'game':
        return None
    size = parse_size(settings.get('window_custom')) if mode == 'custom' else \
        parse_size(settings.get('window_last')) if mode == 'last' else None
    return size or output


class Placer:
    """Beside a running game: places each new game window, and keeps the last size the player
    gave it in a window (`last`, for 'window_size': 'last')."""

    def __init__(self, settings, output, find=game_windows):
        self.fullscreen = bool(settings.get('fullscreen'))
        self.borderless = borderless(settings)
        self.device = settings.get('game_monitor') or ''
        self.size = window_target(settings, output)
        self.find = find
        self.placed, self.seen = set(), {}
        self.last = None
        self.stopped = threading.Event()

    def start(self):
        threading.Thread(target=self.run, daemon=True).start()
        return self

    def stop(self):
        self.stopped.set()

    def run(self):
        with physical():
            while not self.stopped.wait(0.25):
                try:
                    self.step()
                except Exception:  # one failed look (a ctypes.ArgumentError, a window gone) must not end the placing
                    pass

    def step(self):
        for hwnd in self.find():
            rect = window_rect(hwnd)
            if hwnd not in self.placed:
                # once the window stands still: SDL has finished making it (and its fullscreen)
                if self.seen.get(hwnd) == rect:
                    self.place(hwnd)
                    self.placed.add(hwnd)
                self.seen[hwnd] = rect
            elif self.borderless:
                self.keep_borderless(hwnd)
            elif not self.fullscreen:
                self.remember(hwnd)

    def keep_borderless(self, hwnd):
        """A placed borderless window stays frameless over the monitor it is on (Win+Shift+Arrow still moves
        it); a minimized one is left alone until it comes back."""
        user32 = ctypes.windll.user32
        if user32.IsIconic(hwnd):
            return
        if user32.IsZoomed(hwnd):
            user32.ShowWindow(hwnd, 9)
        rect = monitor_of(hwnd)
        if rect:
            make_borderless(hwnd, rect)

    def remember(self, hwnd):
        user32 = ctypes.windll.user32
        if user32.IsZoomed(hwnd) or user32.IsIconic(hwnd):
            return
        size = client_size(hwnd)
        if size[0] >= MIN_SIZE[0] and size[1] >= MIN_SIZE[1]:
            self.last = (size[0] // 2 * 2, size[1] // 2 * 2)

    def place(self, hwnd):
        m = monitor(self.device)
        if not m:
            return
        user32 = ctypes.windll.user32
        move = lambda x, y, w, h: user32.SetWindowPos(hwnd, None, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE)
        if self.borderless:
            if user32.IsZoomed(hwnd):
                user32.ShowWindow(hwnd, 9)  # SW_RESTORE: a maximized window would only cover the work area
            make_borderless(hwnd, m.rect)
            square_corners(hwnd)
            return
        if self.fullscreen:
            if window_rect(hwnd) != m.rect:
                move(m.rect[0], m.rect[1], *m.size)
            return
        if user32.IsZoomed(hwnd):
            return
        if self.size is None and (not self.device or m.primary):
            return  # as the game opens it, where it opens it: untouched
        dpi = monitor_dpi(m)
        work = (m.work[2] - m.work[0], m.work[3] - m.work[1])
        frame = frame_for(hwnd, (100, 100), dpi)
        extra = (frame[0] - 100, frame[1] - 100)
        client = fit(self.size or client_size(hwnd), (work[0] - extra[0], work[1] - extra[1]))
        for _attempt in range(2):  # a move between monitors of different scaling resizes once more
            outer = frame_for(hwnd, client, dpi)
            x = m.work[0] + max(0, (work[0] - outer[0]) // 2)
            y = m.work[1] + max(0, (work[1] - outer[1]) // 2)
            move(x, y, outer[0], outer[1])
            time.sleep(0.2)
            if client_size(hwnd) == client:
                break
        self.last = client_size(hwnd)
