"""The launcher's tray icon (launcher/bbport_tray.py): a click on it must not end the launcher (R53).

Windows calls the tray's window procedure from inside Tk's event handling. A Tk call there leaves
tkinter without its thread state and the next Tk callback ends the process (Fatal Python error,
0xc0000409). So the window procedures here and in bbport_frame.py never call Tk, and a click posted to
the icon's window under a running mainloop (in a process of its own: a fatal error ends it) is
carried out by the tray's after() loop."""
from paths import ROOT
import ast
import subprocess
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path

LAUNCHER = ROOT / 'launcher'
# What a window procedure (and what it calls) may not touch: Tk and the launcher's callbacks.
TK_NAMES = {'root', 'on_open', 'on_quit', 'after', 'after_idle', 'show', 'poll'}
WINDOW_SIDE = {'bbport_tray.py': ('window_proc', 'menu', 'add'),
               'bbport_frame.py': ('wndproc', 'window_menu', 'take_cover')}

CHILD = textwrap.dedent('''
    import ctypes, sys, threading, time
    sys.path.insert(0, sys.argv[1])
    import tkinter as tk
    import bbport_tray
    root = tk.Tk()
    root.withdraw()
    opened = []
    tray = bbport_tray.Tray(sys.argv[2], 'bbport test', lambda: opened.append(1), lambda: None, root=root)
    if not tray.show():
        print('no tray')
        sys.exit(0)

    def clicks():
        for message in ('click', 'click', 'taskbar', 'click'):
            time.sleep(0.4)
            if message == 'click':
                ctypes.windll.user32.PostMessageW(tray.hwnd, bbport_tray.WM_APP_TRAY, 0, bbport_tray.WM_LBUTTONUP)
            else:  # Explorer restarted (the icon gone): it comes back from the window procedure
                bbport_tray.shell32.Shell_NotifyIconW(bbport_tray.NIM_DELETE, ctypes.byref(tray.data))
                ctypes.windll.user32.PostMessageW(tray.hwnd, tray.taskbar_created, 0, 0)

    threading.Thread(target=clicks, daemon=True).start()
    root.after(3000, root.quit)
    root.mainloop()
    shown = tray.shown
    tray.close()
    print('opened', len(opened), 'shown', shown)
''')


def calls_and_attributes(func):
    names = set()
    for node in ast.walk(func):
        if isinstance(node, ast.Attribute):
            names.add(node.attr)
        elif isinstance(node, ast.Name):
            names.add(node.id)
    return names


class WindowProcedureTests(unittest.TestCase):
    def test_window_procedures_never_call_tk(self):
        for module, functions in WINDOW_SIDE.items():
            tree = ast.parse((LAUNCHER / module).read_text(encoding='utf-8'))
            found = {f.name: f for f in ast.walk(tree) if isinstance(f, ast.FunctionDef)}
            for name in functions:
                self.assertIn(name, found, f'{module}: {name}')
                used = calls_and_attributes(found[name]) & TK_NAMES
                self.assertFalse(used, f'{module}: {name} uses {sorted(used)}: Tk from a window procedure')

    def test_the_launcher_passes_its_window_not_after_lambdas(self):
        source = (LAUNCHER / 'bbport_launcher_win.py').read_text(encoding='utf-8')
        start = source.index('bbport_tray.Tray(')
        call = source[start:source.index('\n\n', start)]
        self.assertIn('root=self.root', call)
        self.assertNotIn('after(', call)


@unittest.skipUnless(sys.platform == 'win32', 'the notification area is Windows only')
class ClickTests(unittest.TestCase):
    def test_clicks_on_the_icon_under_a_running_mainloop(self):
        with tempfile.TemporaryDirectory() as folder:
            child = Path(folder) / 'tray_child.py'
            child.write_text(CHILD, encoding='utf-8')
            run = subprocess.run([sys.executable, str(child), str(LAUNCHER), str(LAUNCHER / 'bloodborne.ico')],
                                 capture_output=True, text=True, timeout=60)
        if run.stdout.strip() == 'no tray':
            self.skipTest('no notification area here')
        self.assertEqual(run.returncode, 0, run.stderr[-2000:])
        self.assertNotIn('Fatal Python error', run.stderr)
        self.assertIn('opened 3 shown True', run.stdout)


if __name__ == '__main__':
    unittest.main()
