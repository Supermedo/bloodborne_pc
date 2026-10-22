"""The launcher's Help checks and log hints (launcher/bbport_help.py)."""
from paths import ROOT
import importlib.util
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(ROOT / 'launcher'))
sys.path.insert(0, str(ROOT / 'scripts'))
spec = importlib.util.spec_from_file_location('bbport_help', ROOT / 'launcher/bbport_help.py')
help_ = importlib.util.module_from_spec(spec)
spec.loader.exec_module(help_)


class LogHintTests(unittest.TestCase):
    """The game's and run.py's messages that name a fixable cause."""

    def test_messages(self):
        cases = {
            'Runtime: cannot create 6080 MiB of guest memory (error 1450); is the page file large enough?': 'page_file',
            "PermissionError: [WinError 32] The process cannot access the file 'boot-libc.bin'": 'antivirus',
            'prepare failed: out of bounds: 0x5000+0x1000 / 0x4000': 'eboot_truncated',
            'gxshader.shaderbnd.dcx -> not found': 'update_only',
            'Game files: eboot.bin is not the 01.09 build the community patches were made for': 'eboot_modified',
            "This eboot.bin has Lance McDonald's 60 fps patch applied to it": 'eboot_modified',
            'Texture cache: memory pressure, 7000 of 8000 MiB (critical 7600)': 'vram',
            'GPU: device lost (submit)': 'driver',
        }
        for line, topic in cases.items():
            self.assertEqual(help_.hints_in_log(line), [topic], line)

    def test_nothing_known(self):
        self.assertEqual(help_.hints_in_log('Frame stats: 98.0 FPS\nOverlay: menu ready'), [])
        self.assertEqual(help_.hints_in_log(''), [])

    def test_topic_buttons(self):
        for key, actions in help_.TOPIC_ACTIONS.items():
            self.assertIn(key, help_.TOPICS)
            for _text, method in actions:
                self.assertTrue(hasattr(help_.HelpMixin, method) or method == 'clear_cache', method)

    def test_every_hint_has_a_topic(self):
        for _pattern, topic in help_.LOG_HINTS:
            self.assertIn(topic, help_.TOPICS)
        self.assertEqual(sorted(help_.TOPIC_ORDER), sorted(help_.TOPICS))


class MemoryTests(unittest.TestCase):
    """The check before PLAY: the game reserves its memory in one block at the start."""

    def test_needed_by_resolution(self):
        self.assertEqual(help_.memory_needed('1920x1080'), 6)
        self.assertEqual(help_.memory_needed('1280x720'), 6)
        self.assertEqual(help_.memory_needed('2560x1440'), 10)
        self.assertEqual(help_.memory_needed('3840x2160'), 10)
        self.assertEqual(help_.memory_needed('bad'), 6)

    def test_problem(self):
        # (free now, commit limit, RAM) in GB
        self.assertIsNone(help_.memory_problem('2560x1440', (30.0, 48.0, 32.0)))
        free, need, off = help_.memory_problem('2560x1440', (8.0, 16.2, 16.0))
        self.assertEqual((need, off), (10, True))  # no page file: the limit is the RAM
        self.assertFalse(help_.memory_problem('1920x1080', (5.0, 24.0, 16.0))[2])
        self.assertIsNone(help_.memory_problem('1920x1080', (7.5, 24.0, 16.0)))


class FileTests(unittest.TestCase):
    def test_eboot_states(self):
        with tempfile.TemporaryDirectory() as folder:
            self.assertEqual(help_.eboot_state(folder), ('missing', 0))
            (Path(folder) / 'eboot.bin').write_bytes(b'\x7fELF' + bytes(4096))
            self.assertEqual(help_.eboot_state(folder)[0], 'unreadable')
            (Path(folder) / 'eboot.bin').write_bytes(b'O\x15=\x1d' + bytes(60))
            self.assertIn(help_.eboot_state(folder)[0], ('truncated', 'unreadable'))

    def test_scan_and_mods(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            (root / 'dvdroot_ps4' / 'shader').mkdir(parents=True)
            (root / 'dvdroot_ps4' / 'shader' / 'gxshader.shaderbnd.dcx').write_bytes(b'x' * 10)
            (root / 'dvdroot_ps4' / 'empty.bin').write_bytes(b'')
            scan = help_.scan_game(root)
            self.assertEqual((scan['files'], scan['bytes'], scan['shaders']), (2, 10, True))
            self.assertEqual(scan['empty'], [str(Path('dvdroot_ps4/empty.bin'))])
            mods = root / 'mods'
            (mods / 'fps' / 'game').mkdir(parents=True)
            (mods / 'fps' / 'game' / 'eboot.bin').write_bytes(b'x')
            (mods / 'textures').mkdir()
            self.assertEqual(help_.mods_with_eboot(mods), ['fps'])

    def test_numbers_in_the_language(self):
        class Launcher:
            LANG = 'en'
        saved, help_.L = help_.L, Launcher
        try:
            self.assertEqual((help_.number(93825704), help_.number(29.31, 1)), ('93,825,704', '29.3'))
            Launcher.LANG = 'de'
            self.assertEqual((help_.number(93825704), help_.number(29.31, 1)), ('93.825.704', '29,3'))
            Launcher.LANG = 'fr'
            self.assertEqual(help_.number(1234.5, 1), '1\u202f234,5')
        finally:
            help_.L = saved

    def test_location(self):
        self.assertEqual(help_.location_problem(r'C:\Users\a\OneDrive\Desktop\BBPort'), 'location')
        self.assertEqual(help_.location_problem(r'C:\Program Files\BBPort'), 'location')
        self.assertIsNone(help_.location_problem(r'C:\Games\BBPort'))

    def test_report_hides_secrets(self):
        clean = help_.private_free({'online_password': 'x', 'thd_token': 'y', 'online_npid': 'name',
                                    'supermedo_user': 'n', 'fps_mode': 'uncap', 'online_password_empty': ''})
        self.assertEqual(clean['online_password'], '(hidden)')
        self.assertEqual(clean['thd_token'], '(hidden)')
        self.assertEqual(clean['online_npid'], '(hidden)')
        self.assertEqual(clean['supermedo_user'], '(hidden)')
        self.assertEqual(clean['fps_mode'], 'uncap')


class AfterExitTests(unittest.TestCase):
    """After the game ended with an error. Started from Steam or a shortcut (--play), the launcher opens with
    it: a log without a known cause gets a message too, where its own Play has the output on the screen."""

    def run_after(self, log, code, always):
        class Box:
            def __init__(self):
                self.shown = []

            def showinfo(self, title, text):
                self.shown.append(('info', text))

            def askyesno(self, title, text):
                self.shown.append(('ask', text))
                return True

        class Window(help_.HelpMixin):
            def __init__(self, user):
                self.app, self.messagebox, self.opened = {'user_dir': user}, Box(), []

            def open_topic(self, key):
                self.opened.append(key)
        with tempfile.TemporaryDirectory() as tmp:
            if log is not None:
                (Path(tmp) / 'last_run.log').write_text(log, encoding='utf-8')
            window = Window(tmp)
            window.after_exit(code, always=always)
        return window.messagebox.shown, window.opened

    def test_a_known_cause_offers_its_fix(self):
        shown, opened = self.run_after('GPU: device lost (submit)\n', 1, always=True)
        self.assertEqual([kind for kind, _t in shown], ['ask'])
        self.assertEqual(opened, ['driver'])

    def test_without_a_known_cause_a_message_only_when_asked_for(self):
        for log in ('Frame stats: 98.0 FPS\n', None):  # nothing known; no log at all
            shown, opened = self.run_after(log, 3221225477, always=True)
            self.assertEqual(len(shown), 1)
            self.assertEqual(shown[0][0], 'info')
            self.assertIn('code 0xC0000005', shown[0][1])  # as helpers know it, not 3221225477
            self.assertEqual(opened, [])
            self.assertEqual(self.run_after(log, 1, always=False), ([], []))

    def test_nothing_after_a_normal_end(self):
        self.assertEqual(self.run_after('GPU: device lost (submit)\n', 0, always=True), ([], []))

    def test_play_opens_help_after_an_error(self):
        source = (ROOT / 'launcher' / 'bbport_launcher_win.py').read_text(encoding='utf-8')
        main = source[source.index('\ndef main():'):]
        self.assertIn("('help', result)", main)
        self.assertIn('launcher.after_exit(failed, always=True)', main)


if __name__ == '__main__':
    unittest.main()
