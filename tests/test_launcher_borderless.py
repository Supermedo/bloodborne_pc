"""Borderless fullscreen (launcher/bbport_display.py Placer, bbport_setup.py's switch): the game's window loses
its frame and covers its monitor, stays so when SDL puts the frame back, and the switch and Fullscreen turn each
other off."""
from paths import ROOT
import sys
import unittest
from unittest import mock

sys.path.insert(0, str(ROOT / 'launcher'))
sys.path.insert(0, str(ROOT / 'scripts'))
import bbport_display as display  # noqa: E402
import bbport_launcher_win as L  # noqa: E402
import bbport_setup  # noqa: E402

MONITOR = display.Monitor('\\\\.\\DISPLAY1', (0, 0, 2560, 1440), (0, 0, 2560, 1392), True)
FRAMED = 0x16CF0000  # WS_OVERLAPPEDWINDOW | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN


class FakeWindow:
    """One window as user32 sees it."""

    def __init__(self):
        self.style, self.extended, self.rect = FRAMED, 0x100, (312, 152, 2248, 1240)
        self.iconic = self.zoomed = False
        self.moves = []

    def long(self, _hwnd, index):
        return self.style if index == display.GWL_STYLE else self.extended

    def set_long(self, _hwnd, index, value):
        if index == display.GWL_STYLE:
            self.style = value
        else:
            self.extended = value

    def set_pos(self, _hwnd, _after, x, y, w, h, flags):
        self.rect = (x, y, x + w, y + h)
        self.moves.append((self.rect, flags))
        return True


class BorderlessTests(unittest.TestCase):
    def setUp(self):
        self.window = w = FakeWindow()
        user32 = mock.Mock()
        user32.SetWindowPos.side_effect = w.set_pos
        user32.IsIconic.side_effect = lambda _h: w.iconic
        user32.IsZoomed.side_effect = lambda _h: w.zoomed
        patches = [mock.patch.object(display, 'window_long', side_effect=w.long),
                   mock.patch.object(display, 'set_window_long', side_effect=w.set_long),
                   mock.patch.object(display, 'window_rect', side_effect=lambda _h: w.rect),
                   mock.patch.object(display, 'monitor', return_value=MONITOR),
                   mock.patch.object(display, 'monitor_of', return_value=MONITOR.rect),
                   mock.patch.object(display, 'square_corners'),
                   mock.patch.object(display.ctypes, 'windll', mock.Mock(user32=user32))]
        for patch in patches:
            patch.start()
            self.addCleanup(patch.stop)

    def placer(self, **settings):
        return display.Placer({'fullscreen': False, 'borderless_fullscreen': True, **settings}, (2560, 1440),
                              find=lambda: [7])

    def test_the_window_loses_its_frame_and_covers_the_monitor(self):
        placer = self.placer()
        placer.step()
        self.assertEqual(self.window.moves, [])  # SDL may still be making it
        placer.step()
        self.assertEqual(self.window.rect, MONITOR.rect)
        self.assertFalse(self.window.style & display.FRAME)
        self.assertFalse(self.window.extended & display.EDGES)
        self.assertTrue(self.window.style & 0x00020000)  # the minimize box stays: the taskbar minimizes it
        self.assertTrue(self.window.moves[0][1] & display.SWP_NOZORDER)  # never above other windows
        self.assertIsNone(placer.last)  # no window size remembered for 'As I left it'

    def test_a_frame_put_back_is_taken_off_again(self):
        placer = self.placer()
        placer.step(), placer.step()
        self.window.style |= display.WS_CAPTION
        self.window.rect = (100, 100, 1700, 1000)
        placer.step()
        self.assertEqual(self.window.rect, MONITOR.rect)
        self.assertFalse(self.window.style & display.FRAME)
        moves = len(self.window.moves)
        placer.step()
        self.assertEqual(len(self.window.moves), moves)  # nothing to do: nothing set

    def test_a_minimized_window_is_left_alone(self):
        placer = self.placer()
        placer.step(), placer.step()
        self.window.iconic, self.window.rect = True, (-32000, -32000, -31840, -31972)
        placer.step()
        self.assertEqual(self.window.rect, (-32000, -32000, -31840, -31972))

    def test_fullscreen_wins_and_a_window_stays_a_window(self):
        self.assertFalse(display.borderless({'fullscreen': True, 'borderless_fullscreen': True}))
        self.assertTrue(display.borderless({'fullscreen': False, 'borderless_fullscreen': True}))
        placer = self.placer(borderless_fullscreen=False, window_size='game')
        placer.step(), placer.step(), placer.step()
        self.assertEqual(self.window.style, FRAMED)

    def test_the_game_starts_in_a_window(self):
        settings = {**L.APP_DEFAULTS, 'fullscreen': False, 'borderless_fullscreen': True}
        env = L.game_environment(settings, dict(L.INI_DEFAULTS))
        self.assertEqual(env['BB_FULLSCREEN'], '0')
        self.assertNotIn('BB_WINDOW_MODE', env)


class Var:
    def __init__(self, value):
        self.value, self.traces = value, []

    def get(self):
        return self.value

    def set(self, value):
        self.value = value
        for trace in self.traces:
            trace()

    def trace_add(self, _mode, callback):
        self.traces.append(callback)


class SwitchTests(unittest.TestCase):
    def test_the_switch_and_fullscreen_turn_each_other_off(self):
        variables = {'fullscreen': Var(True), 'borderless_fullscreen': Var(True)}
        page = mock.Mock(var=lambda key, _store: variables[key])
        bbport_setup.SetupMixin.one_screen_mode(page)
        self.assertFalse(variables['borderless_fullscreen'].get())  # both on in an older file: Fullscreen stays
        variables['borderless_fullscreen'].set(True)
        self.assertFalse(variables['fullscreen'].get())
        variables['fullscreen'].set(True)
        self.assertFalse(variables['borderless_fullscreen'].get())

    def test_the_switch_comes_right_after_fullscreen(self):
        source = (ROOT / 'launcher' / 'bbport_setup.py').read_text(encoding='utf-8')
        at = source.index("self.check(f, 'fullscreen', 'app'")
        self.assertLess(source.index("self.check(f, 'borderless_fullscreen', 'app'", at),
                        source.index("self.check(f, 'exclusive_fullscreen', 'app'", at))


if __name__ == '__main__':
    unittest.main()
