# SPDX-License-Identifier: GPL-2.0-or-later
"""The launcher's icon in the notification area (the system tray), with ctypes only: a hidden window
of its own receives the icon's clicks, a left click calls on_open, a right click shows Open / Quit.
The icon comes back when Explorer restarts (TaskbarCreated).

Tk's message loop dispatches the icon's messages, so the window procedure runs inside Tk's own event
handling. It never calls Tk there (as bbport_frame's): a Tk call from it leaves tkinter without its
thread state, and the next Tk callback ends the process ("Fatal Python error: PyEval_RestoreThread",
0xc0000409; R53, the launcher closed at every click on the icon). The procedure only notes the click;
a Tk after() loop, running while the icon is shown, calls on_open / on_quit."""
import ctypes
from ctypes import wintypes

WM_APP_TRAY = 0x8000 + 1  # WM_APP + 1: the icon's messages
WM_LBUTTONUP, WM_LBUTTONDBLCLK, WM_RBUTTONUP = 0x0202, 0x0203, 0x0205
NIM_ADD, NIM_DELETE, NIM_SETVERSION = 0, 2, 4
NIF_MESSAGE, NIF_ICON, NIF_TIP = 0x1, 0x2, 0x4
IMAGE_ICON, LR_LOADFROMFILE, LR_DEFAULTSIZE = 1, 0x10, 0x40
TPM_RETURNCMD, TPM_RIGHTBUTTON, MF_STRING, MF_SEPARATOR = 0x100, 0x2, 0x0, 0x800
MENU_OPEN, MENU_QUIT = 1, 2
POLL_MS = 100  # how often the after() loop looks for a click

LRESULT = ctypes.c_ssize_t
WNDPROC = ctypes.WINFUNCTYPE(LRESULT, wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM)


class NOTIFYICONDATAW(ctypes.Structure):
    _fields_ = [('cbSize', wintypes.DWORD), ('hWnd', wintypes.HWND), ('uID', wintypes.UINT),
                ('uFlags', wintypes.UINT), ('uCallbackMessage', wintypes.UINT), ('hIcon', wintypes.HICON),
                ('szTip', wintypes.WCHAR * 128), ('dwState', wintypes.DWORD), ('dwStateMask', wintypes.DWORD),
                ('szInfo', wintypes.WCHAR * 256), ('uVersion', wintypes.UINT), ('szInfoTitle', wintypes.WCHAR * 64),
                ('dwInfoFlags', wintypes.DWORD), ('guidItem', ctypes.c_byte * 16), ('hBalloonIcon', wintypes.HICON)]


class WNDCLASSW(ctypes.Structure):
    _fields_ = [('style', wintypes.UINT), ('lpfnWndProc', WNDPROC), ('cbClsExtra', ctypes.c_int),
                ('cbWndExtra', ctypes.c_int), ('hInstance', wintypes.HINSTANCE), ('hIcon', wintypes.HICON),
                ('hCursor', wintypes.HANDLE), ('hbrBackground', wintypes.HBRUSH), ('lpszMenuName', wintypes.LPCWSTR),
                ('lpszClassName', wintypes.LPCWSTR)]


user32, shell32, kernel32 = ctypes.windll.user32, ctypes.windll.shell32, ctypes.windll.kernel32
user32.DefWindowProcW.argtypes = (wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM)
user32.DefWindowProcW.restype = LRESULT
user32.CreateWindowExW.restype = wintypes.HWND
user32.CreateWindowExW.argtypes = (wintypes.DWORD, wintypes.LPCWSTR, wintypes.LPCWSTR, wintypes.DWORD, ctypes.c_int,
                                   ctypes.c_int, ctypes.c_int, ctypes.c_int, wintypes.HWND, wintypes.HMENU,
                                   wintypes.HINSTANCE, wintypes.LPVOID)
user32.LoadImageW.restype = wintypes.HANDLE
user32.LoadImageW.argtypes = (wintypes.HINSTANCE, wintypes.LPCWSTR, wintypes.UINT, ctypes.c_int, ctypes.c_int,
                              wintypes.UINT)
user32.CreatePopupMenu.restype = wintypes.HMENU
user32.AppendMenuW.argtypes = (wintypes.HMENU, wintypes.UINT, ctypes.c_size_t, wintypes.LPCWSTR)
user32.TrackPopupMenu.argtypes = (wintypes.HMENU, wintypes.UINT, ctypes.c_int, ctypes.c_int, ctypes.c_int,
                                  wintypes.HWND, wintypes.LPVOID)
user32.DestroyMenu.argtypes = (wintypes.HMENU,)
user32.PostMessageW.argtypes = (wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM)
shell32.Shell_NotifyIconW.argtypes = (wintypes.DWORD, ctypes.POINTER(NOTIFYICONDATAW))
kernel32.GetModuleHandleW.restype = wintypes.HMODULE


class Tray:
    """One icon. show() puts it in the notification area, hide() takes it out; close() for good.
    root: the Tk window whose after() calls on_open / on_quit (on Tk's thread, outside the window
    procedure)."""

    def __init__(self, icon_path, tip, on_open, on_quit, words=('Open', 'Quit'), root=None):
        self.on_open, self.on_quit, self.words, self.root = on_open, on_quit, words, root
        self.shown = False   # the icon is in the notification area
        self.wanted = False  # between show() and hide(): the icon should be there (again)
        self.clicked = None  # 'open' or 'quit', noted by the window procedure
        self.polling = None  # the after() id while wanted
        self.proc = WNDPROC(self.window_proc)  # kept: Windows calls it as long as the window lives
        instance = kernel32.GetModuleHandleW(None)
        name = f'bbport-tray-{id(self)}'
        cls = WNDCLASSW(lpfnWndProc=self.proc, hInstance=instance, lpszClassName=name)
        if not user32.RegisterClassW(ctypes.byref(cls)):
            raise OSError('the tray window class could not be registered')
        self.hwnd = user32.CreateWindowExW(0, name, name, 0, 0, 0, 0, 0, None, None, instance, None)
        if not self.hwnd:
            raise OSError('the tray window could not be created')
        self.taskbar_created = user32.RegisterWindowMessageW('TaskbarCreated')
        self.icon = user32.LoadImageW(None, str(icon_path), IMAGE_ICON, 0, 0, LR_LOADFROMFILE | LR_DEFAULTSIZE)
        self.data = NOTIFYICONDATAW(cbSize=ctypes.sizeof(NOTIFYICONDATAW), hWnd=self.hwnd, uID=1,
                                    uFlags=NIF_MESSAGE | NIF_ICON | NIF_TIP, uCallbackMessage=WM_APP_TRAY,
                                    hIcon=self.icon, szTip=tip[:127])

    def show(self):
        """From Tk (never from the window procedure): the icon, and the loop that answers its clicks."""
        self.add()
        self.wanted = self.shown
        if self.wanted and self.polling is None and self.root is not None:
            self.polling = self.root.after(POLL_MS, self.poll)
        return self.shown

    def add(self):
        """Windows calls only: also from the window procedure (TaskbarCreated)."""
        if not self.shown:
            self.shown = bool(shell32.Shell_NotifyIconW(NIM_ADD, ctypes.byref(self.data)))

    def hide(self):
        self.wanted = False
        if self.polling is not None:
            try:
                self.root.after_cancel(self.polling)
            except Exception:  # the window is gone already
                pass
            self.polling = None
        # Also when not shown: a TaskbarCreated without a lost icon leaves it there, not shown.
        shell32.Shell_NotifyIconW(NIM_DELETE, ctypes.byref(self.data))
        self.shown = False

    def poll(self):
        """Tk's after(): a click the window procedure noted, carried out on Tk's side. An icon
        Explorer lost and could not take back yet (TaskbarCreated) is added again here."""
        self.polling = None
        clicked, self.clicked = self.clicked, None
        if self.wanted and self.root is not None:
            self.add()
            self.polling = self.root.after(POLL_MS, self.poll)
        if clicked == 'open':
            self.on_open()
        elif clicked == 'quit':
            self.on_quit()

    def close(self):
        self.hide()
        if self.hwnd:
            user32.DestroyWindow(self.hwnd)
            self.hwnd = None

    def menu(self):
        menu = user32.CreatePopupMenu()
        user32.AppendMenuW(menu, MF_STRING, MENU_OPEN, self.words[0])
        user32.AppendMenuW(menu, MF_SEPARATOR, 0, None)
        user32.AppendMenuW(menu, MF_STRING, MENU_QUIT, self.words[1])
        point = wintypes.POINT()
        user32.GetCursorPos(ctypes.byref(point))
        user32.SetForegroundWindow(self.hwnd)  # else the menu stays open when clicked elsewhere
        chosen = user32.TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, point.x, point.y, 0, self.hwnd, None)
        user32.PostMessageW(self.hwnd, 0, 0, 0)  # WM_NULL: the menu closes properly (KB135788)
        user32.DestroyMenu(menu)
        if chosen == MENU_OPEN:
            self.clicked = 'open'
        elif chosen == MENU_QUIT:
            self.clicked = 'quit'

    def window_proc(self, hwnd, message, wparam, lparam):
        try:
            # Windows calls only in here, never Tk (see the top): clicks are noted for poll().
            if message == WM_APP_TRAY:
                if lparam in (WM_LBUTTONUP, WM_LBUTTONDBLCLK):
                    self.clicked = 'open'
                elif lparam == WM_RBUTTONUP:
                    self.menu()
                return 0
            if message == self.taskbar_created and self.wanted:  # Explorer restarted: the icon again
                self.shown = False
                self.add()
        except Exception:  # never an exception into Windows' call of this procedure
            pass
        return user32.DefWindowProcW(hwnd, message, wparam, lparam)
