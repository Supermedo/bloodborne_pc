# SPDX-License-Identifier: GPL-2.0-or-later
"""The launcher opens where it was closed (R30): on the monitor it was on, maximized again when it was; never off screen
when that monitor is gone. The place is read once at close (window_place), nothing is added to a drag."""
from paths import ROOT
import ctypes
import sys
import tkinter
import types
import unittest
from unittest import mock

sys.path.insert(0, str(ROOT / 'launcher'))
import bbport_launcher_win as launcher  # noqa: E402


class PlacedTests(unittest.TestCase):
    def test_a_place_on_a_monitor_is_taken(self):
        self.assertEqual(launcher.placed('40,30'), (40, 30, False))
        self.assertEqual(launcher.placed('40,30,zoomed'), (40, 30, True))

    def test_a_place_on_no_monitor_is_dropped(self):
        self.assertIsNone(launcher.placed('-60000,-60000'))  # its monitor unplugged
        self.assertIsNone(launcher.placed('60000,30,zoomed'))

    def test_a_window_whose_top_left_is_in_a_gap_between_monitors_is_taken(self):
        """Monitors scaled differently: in Tk's coordinates the second one can start after a gap (here 3440-4128); a
        window at 3500 still shows its title bar there."""
        def on_monitor(point, _flags):
            return 1 if 0 <= point.x < 3440 or 4128 <= point.x < 5424 else 0
        with mock.patch.object(ctypes.windll.user32, 'MonitorFromPoint', on_monitor):
            self.assertEqual(launcher.placed('3500,300', 1180), (3500, 300, False))
            self.assertIsNone(launcher.placed('3500,300', 400))  # nothing of it on a monitor

    def test_no_or_a_broken_place_is_none(self):
        for text in ('', None, 'abc', '40', '40,x'):
            self.assertIsNone(launcher.placed(text), text)


class WindowPlaceTests(unittest.TestCase):
    def setUp(self):
        self.root = tkinter.Tk()
        self.root.geometry('300x200+40+30')
        self.root.update()
        self.app = types.SimpleNamespace(root=self.root)

    def tearDown(self):
        self.root.destroy()

    def test_the_top_left_of_a_normal_window(self):
        self.assertEqual(launcher.Launcher.window_place(self.app), '40,30')
        self.root.geometry('+-8+12')
        self.root.update()
        self.assertEqual(launcher.Launcher.window_place(self.app), '-8,12')

    def test_a_maximized_window_keeps_its_monitor_and_state(self):
        self.root.state('zoomed')
        self.root.update()
        x, y, state = launcher.Launcher.window_place(self.app).split(',')
        self.assertEqual(state, 'zoomed')
        self.assertEqual(launcher.placed(f'{x},{y},zoomed'), (int(x), int(y), True))

    def test_nothing_from_the_task_bar(self):
        self.root.iconify()
        self.root.update()
        self.assertEqual(launcher.Launcher.window_place(self.app), '')


if __name__ == '__main__':
    unittest.main()
