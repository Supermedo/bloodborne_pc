"""Send input to our own SDL capture windows and verify physical Windows-to-INI names."""
import ctypes
from ctypes import wintypes
import queue
from pathlib import Path
import subprocess
import sys
import threading
import uuid

user = ctypes.WinDLL('user32', use_last_error=True)
user.FindWindowW.argtypes = [wintypes.LPCWSTR, wintypes.LPCWSTR]
user.FindWindowW.restype = wintypes.HWND
user.PostMessageW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]
user.PostMessageW.restype = wintypes.BOOL
user.ClientToScreen.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.POINT)]
user.SetWindowPos.argtypes = [wintypes.HWND, wintypes.HWND, ctypes.c_int, ctypes.c_int,
                             ctypes.c_int, ctypes.c_int, wintypes.UINT]

def capture(message, wparam, lparam, expected):
    title = '\u041d\u0430\u0437\u043d\u0430\u0447\u0435\u043d\u0438\u0435 v2 - ' + uuid.uuid4().hex
    process = subprocess.Popen([str(Path(sys.argv[1]).resolve()), '--read-input', 'key', title],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, creationflags=subprocess.CREATE_NO_WINDOW)
    restore_cursor = None
    try:
        ready = queue.Queue()
        threading.Thread(target=lambda: ready.put(process.stderr.readline()), daemon=True).start()
        first_line = ready.get(timeout=20)
        assert first_line.strip() == b'Input capture ready', repr(first_line)
        hwnd = user.FindWindowW(None, title)
        assert hwnd, 'UTF-8 capture window title did not round-trip'
        # Target only our own window. Synthetic focus avoids stealing focus from the
        # user's app when this regression runs from a background build process.
        assert user.PostMessageW(hwnd, 0x0007, 0, 0)  # WM_SETFOCUS
        if message in (0x020a, 0x020b):
            # Mouse focus is based on the pointer; keep it inside our own window for
            # this test and restore it afterwards. No keyboard focus is stolen.
            point = wintypes.POINT(30, 30)
            assert user.ClientToScreen(hwnd, ctypes.byref(point))
            restore_cursor = wintypes.POINT()
            assert user.GetCursorPos(ctypes.byref(restore_cursor))
            assert user.SetWindowPos(hwnd, wintypes.HWND(-1), 0, 0, 0, 0, 0x13)
            assert user.SetCursorPos(point.x, point.y)
            if message == 0x020a:  # wheel coordinates are screen coordinates
                lparam = (point.x & 0xffff) | ((point.y & 0xffff) << 16)
            assert user.PostMessageW(hwnd, 0x0200, 0, 30 | (30 << 16))
        assert user.PostMessageW(hwnd, message, wparam, lparam)
        output, error = process.communicate(timeout=8)
        assert process.returncode == 0, error.decode('utf-8', errors='replace')
        assert output.decode('utf-8').strip() == expected, repr(output)
    finally:
        if process.poll() is None:
            process.kill(); process.communicate()
        if restore_cursor is not None:
            user.SetCursorPos(restore_cursor.x, restore_cursor.y)

capture(0x0100, 0xa3, 1 | (0x1d << 16) | (1 << 24), 'key Right Ctrl')
capture(0x0100, 0xbc, 1 | (0x33 << 16), 'key Comma')
capture(0x0100, 0x1b, 1 | (0x01 << 16), 'key Escape')
capture(0x020b, (2 << 16) | 0x40, 30 | (30 << 16), 'key Mouse X2')
capture(0x020a, 120 << 16, 30 | (30 << 16), 'key Mouse Wheel Up')
capture(0x0010, 0, 0, '')
print('PASS: real Win32/SDL capture, right modifier, comma, Escape, side mouse, wheel, Unicode title and cancel')
