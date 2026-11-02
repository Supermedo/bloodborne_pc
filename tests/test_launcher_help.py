"""The launcher's Help checks and log hints (launcher/bbport_help.py)."""
from paths import ROOT
import importlib.util
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

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


class GraphicsCardTests(unittest.TestCase):
    """The check before PLAY: what the game's renderer needs, as the graphics driver answers it
    (scripts/vulkan_check.py); a GeForce 820M started and closed without a word."""

    def window(self, vulkan=None, answer=True):
        class Box:
            def __init__(self):
                self.asked = []

            def askyesno(self, title, text, **_options):
                self.asked.append(text)
                return answer

        class Window(help_.HelpMixin):
            def var(self, key, store):
                return type('Var', (), {'get': lambda _self: '1920x1080'})()
        window = Window()
        window.app, window.messagebox, window.vulkan = {}, Box(), vulkan
        return window

    @staticmethod
    def verdict(problem, name='GeForce 820M', missing=(), **more):
        return {'ok': problem is None, 'problem': problem, 'loader': '1.2.170',
                'device': {'name': name, 'missing': list(missing)} if problem in (None, 'device') else None, **more}

    def test_each_answer_says_what_to_do(self):
        self.assertIn('vulkan-1.dll', self.window(self.verdict('loader')).gpu_problem())
        self.assertIn('1.2.170', self.window(self.verdict('loader_version')).gpu_problem())
        self.assertIn('Vulkan 1.3', self.window(self.verdict('devices')).gpu_problem())
        text = self.window(self.verdict('device', missing=['Vulkan 1.3 (1.2.0)', 'VK_EXT_robustness2'])).gpu_problem()
        self.assertTrue(text.startswith('GeForce 820M cannot run the port'))
        self.assertIn('Vulkan 1.3 (1.2.0), VK_EXT_robustness2', text)
        self.assertIn('0xC0000005', self.window(self.verdict('crashed', code='0xC0000005')).gpu_problem())

    def test_helps_line(self):
        ok = {'ok': True, 'problem': None, 'device': {'name': 'AMD Radeon RX 9070 XT', 'api': '1.4.359'}}
        self.assertEqual(self.window(ok).gpu_check(), (
            'ok', None, 'Graphics card AMD Radeon RX 9070 XT: Vulkan 1.4.359 and everything else the port needs.'))
        state, topic, text = self.window(self.verdict('device', missing=['nullDescriptor'])).gpu_check()
        self.assertEqual((state, topic), ('bad', 'driver'))
        self.assertIn('nullDescriptor', text)
        self.assertIsNone(self.window(None).gpu_check())

    def test_the_play_page_shows_it_only_when_the_port_cannot_start(self):
        source = (ROOT / 'launcher' / 'bbport_launcher_win.py').read_text(encoding='utf-8')
        self.assertIn("self.check_line('gpu', _('✗ The graphics card cannot run the port: see Help') if "
                      "self.gpu_problem() else\n                        self.gpu_unused_line(),", source)

    # A player's RTX 5070 Ti: in Windows' list, not in Vulkan's; the game ran on the processor's graphics.
    UNSEEN = {'ok': True, 'problem': None, 'device': {'name': 'AMD Radeon(TM) Graphics', 'api': '1.4.315',
                                                      'missing': []}, 'unseen': ['NVIDIA GeForce RTX 5070 Ti']}

    def test_a_card_vulkan_does_not_see(self):
        window = self.window(self.UNSEEN)
        self.assertIsNone(window.gpu_problem())  # it starts
        state, topic, text = window.gpu_check()
        self.assertEqual((state, topic), ('warn', 'gpu_unseen'))
        self.assertTrue(text.startswith('Windows has NVIDIA GeForce RTX 5070 Ti, but its Vulkan driver does not '
                                        'answer, so the game runs on AMD Radeon(TM) Graphics'))
        self.assertIn('Install the driver for NVIDIA GeForce RTX 5070 Ti anew (clean installation)', text)
        self.assertIn('gpu_unseen', help_.TOPIC_ORDER)

    def test_no_card_but_one_vulkan_does_not_see(self):
        window = self.window({**self.verdict('devices'), 'unseen': ['NVIDIA GeForce RTX 5070 Ti']})
        self.assertTrue(window.gpu_problem().startswith('Windows has NVIDIA GeForce RTX 5070 Ti, but its Vulkan '
                                                        'driver does not answer. Install'))  # not "too old"
        self.assertEqual(window.gpu_check()[:2], ('bad', 'gpu_unseen'))

    def test_a_card_its_driver_holds_back(self):
        # R29: a GTX 1060 whose driver offers Vulkan 1.2, the game on the processor's Radeon Vega.
        verdict = {**self.UNSEEN, 'unseen': [], 'device': {'name': 'AMD Radeon(TM) Vega 8 Graphics', 'missing': []},
                   'held_back': [{'name': 'NVIDIA GeForce GTX 1060 6GB', 'missing': ['Vulkan 1.3 (1.2.175)']}]}
        window = self.window(verdict)
        self.assertEqual(window.unused_card(), 'NVIDIA GeForce GTX 1060 6GB')
        self.assertEqual(window.gpu_check()[:2], ('warn', 'gpu_unseen'))
        self.assertTrue(window.gpu_unseen().startswith('NVIDIA GeForce GTX 1060 6GB is not used: its driver lacks '
                                                       'Vulkan 1.3 (1.2.175), so the game runs on AMD Radeon(TM) '
                                                       'Vega 8 Graphics'))
        self.assertEqual(window.gpu_unused_line(),
                         '⚠ NVIDIA GeForce GTX 1060 6GB is not used: its driver is too old for the port (Help)')
        self.assertEqual(self.window(self.UNSEEN).gpu_unused_line(),
                         '⚠ NVIDIA GeForce RTX 5070 Ti is not used: its Vulkan driver does not answer (Help)')
        self.assertEqual(self.window(self.verdict(None, 'RX 9070 XT')).gpu_unused_line(), '')

    def test_asked_once_before_play(self):
        window = self.window(self.UNSEEN, answer=False)
        window.open_topic = lambda topic: window.opened.append(topic)
        window.opened = []
        with mock.patch.object(help_, 'memory_problem', lambda *_a: None):
            self.assertFalse(window.preflight())  # No: Help's topic instead of the game
            self.assertEqual(window.opened, ['gpu_unseen'])
            window.messagebox = type(window.messagebox)()
            window.messagebox.askyesno = lambda *_a, **_k: True
            self.assertTrue(window.preflight())
            self.assertEqual(window.app['gpu_warned'], 'NVIDIA GeForce RTX 5070 Ti')
            window.messagebox.askyesno = lambda *_a, **_k: self.fail('asked twice')
            self.assertTrue(window.preflight())

    def test_nothing_to_say(self):
        self.assertIsNone(self.window(self.verdict(None, 'RX 9070 XT')).gpu_problem())
        self.assertIsNone(self.window(None).gpu_problem())  # not asked yet, or the check could not run

    def test_asked_once_per_card(self):
        window = self.window(self.verdict('device', missing=['VK_EXT_robustness2']))
        with mock.patch.object(help_, 'memory_problem', lambda *_a: None):
            self.assertTrue(window.preflight())
            self.assertTrue(window.preflight())
            self.assertEqual(len(window.messagebox.asked), 1)
            window.vulkan = self.verdict('device', 'Other card', missing=['nullDescriptor'])
            self.assertTrue(window.preflight())
            self.assertEqual(len(window.messagebox.asked), 2)
            no = self.window(self.verdict('loader'), answer=False)
            self.assertFalse(no.preflight())
            self.assertFalse(no.preflight())  # asked again until the player says start


def self_file(segments, program_type, offset):
    """A PS4 SELF with one program header, and a segment table entry pointing at `offset` if `segments`."""
    import struct
    table = struct.pack('<QQQQ', 0x800, offset, 16, 16) if segments else b''
    head = b'O\x15=\x1d' + bytes(20) + struct.pack('<H', len(table) // 32) + bytes(6)
    elf = struct.pack('<16sHHIQQQIHHHHHH', b'\x7fELF\x02\x01\x01' + bytes(9), 2, 62, 1, 0, 64, 0, 0, 64, 56,
                      1, 0, 0, 0)
    return head + table + elf + struct.pack('<IIQQQQQQ', program_type, 0, 0x200, 0, 0, 16, 16, 0)


class FileTests(unittest.TestCase):
    def test_eboot_states(self):
        with tempfile.TemporaryDirectory() as folder:
            self.assertEqual(help_.eboot_state(folder), ('missing', 0))
            (Path(folder) / 'eboot.bin').write_bytes(b'\x7fELF' + bytes(4096))
            self.assertEqual(help_.eboot_state(folder)[0], 'unreadable')
            (Path(folder) / 'eboot.bin').write_bytes(b'O\x15=\x1d' + bytes(60))
            self.assertIn(help_.eboot_state(folder)[0], ('truncated', 'unreadable'))

    def test_only_a_short_eboot_is_cut_off(self):
        """R78: an eboot.bin of the clean one's size whose program does not build was "cut off (93,825,704 bytes,
        the clean one has 93,825,704)". It is unreadable; only a shorter one is cut off."""
        with tempfile.TemporaryDirectory() as folder, mock.patch.object(help_, 'EBOOT_SIZE', 4096):
            path = Path(folder) / 'eboot.bin'
            # It parses, but has no segment to load: building the image fails (the second step).
            nothing_to_load = self_file(False, 0, 0)
            # Its segment lies beyond the end of the file: out of bounds while parsing (the first step).
            beyond = self_file(True, 1, 0x10000)
            for program in (nothing_to_load, beyond):
                for size, state in ((4096, 'unreadable'), (4100, 'unreadable'), (4095, 'truncated')):
                    with self.subTest(segments=program is beyond, size=size):
                        path.write_bytes(program.ljust(size, b'\0'))
                        self.assertEqual(help_.eboot_state(folder), (state, size))
            path.write_bytes(beyond[:40])  # cut off inside its headers
            self.assertEqual(help_.eboot_state(folder), ('truncated', 40))

    def test_an_older_launchers_cut_off_is_checked_again(self):
        """R78 after the update: settings.json still has 1.9.6's "truncated" for the full-size file. That answer
        ('2|') is not reused, the file is read again; this launcher's own answer is, while the file is unchanged."""
        scan = {'files': 28800, 'bytes': 30 * 1024 ** 3, 'empty': [], 'shaders': True}
        with tempfile.TemporaryDirectory() as folder, mock.patch.object(help_, 'EBOOT_SIZE', 4096), \
                mock.patch.object(help_, 'scan_game', return_value=scan), \
                mock.patch.object(help_, 'restore_eboot', return_value=None):
            path = Path(folder) / 'eboot.bin'
            path.write_bytes(self_file(False, 0, 0).ljust(4096, b'\0'))
            stat = path.stat()
            old = {'key': f'2|{path.resolve()}|{stat.st_size}|{int(stat.st_mtime)}', 'state': 'truncated'}
            mods = str(Path(folder) / 'mods')
            checks, eboot = help_.run_checks(folder, mods, '', '1920x1080', old)
            self.assertEqual(eboot['state'], 'unreadable')
            self.assertIn(('bad', 'eboot_unreadable', 'eboot.bin cannot be read (4,096 bytes).'), checks)
            self.assertNotIn('eboot_truncated', [topic for _state, topic, _text in checks])
            with mock.patch.object(help_, 'eboot_state', side_effect=AssertionError('read again')):
                self.assertEqual(help_.run_checks(folder, mods, '', '1920x1080', eboot)[1], eboot)

    def test_out_of_bounds_in_a_full_size_eboot(self):
        """The log's "out of bounds" comes from any eboot.bin that does not build: after a failed start, a
        full-size one gets the topic of a damaged file (with the game folder's button), not "cut off" (R78)."""
        log = 'prepare failed: out of bounds: 0x5000+0x1000 / 0x4000\n'
        with tempfile.TemporaryDirectory() as folder, mock.patch.object(help_, 'EBOOT_SIZE', 4096):
            path = Path(folder) / 'eboot.bin'
            self.assertEqual(help_.hints_for_game(log, folder), ['eboot_truncated'])  # no file: the log decides
            self.assertEqual(help_.hints_for_game(log, ''), ['eboot_truncated'])
            path.write_bytes(bytes(4095))
            self.assertEqual(help_.hints_for_game(log, folder), ['eboot_truncated'])
            path.write_bytes(bytes(4096))
            self.assertEqual(help_.hints_for_game(log, folder), ['eboot_unreadable'])
            self.assertEqual(help_.hints_for_game(log + 'expected PS4 SELF or ELF\n', folder), ['eboot_unreadable'])
        self.assertEqual(help_.TOPIC_ACTIONS['eboot_unreadable'], [('Open the game folder', 'open_game_folder')])

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

    def test_report_hides_every_account_secret_of_the_launcher(self):
        """supermedo_pass and party_key went into the report zip in clear text (2026-11-02): every account key the
        launcher keeps (OWN_ONLINE_KEYS, the shadNet boxes) is hidden, and secret variables in extra_env too."""
        import bbport_launcher_win as L
        settings = {key: 'value-123' for key in L.OWN_ONLINE_KEYS + ('online_npid', 'online_password')}
        settings['extra_env'] = 'BB_SHADNET_PASSWORD=hunter2 BB_FRAME_STATS=1 BB_THD_TOKEN="abc def"'
        clean = help_.private_free(settings)
        for key in L.OWN_ONLINE_KEYS + ('online_npid', 'online_password'):
            if key == 'online_pool':
                continue  # which server, no secret
            with self.subTest(key=key):
                self.assertEqual(clean[key], '(hidden)')
        self.assertNotIn('hunter2', clean['extra_env'])
        self.assertNotIn('abc def', clean['extra_env'])
        self.assertIn('BB_FRAME_STATS=1', clean['extra_env'])


class AfterExitTests(unittest.TestCase):
    """After the game ended with an error. Started from Steam or a shortcut (--play), the launcher opens with
    it: a log without a known cause gets a message too, where its own Play has the output on the screen."""

    def run_after(self, log, code, always, game=''):
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
                self.app, self.messagebox, self.opened = {'user_dir': user, 'game_dir': game}, Box(), []

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

    def test_a_full_size_eboot_is_not_called_cut_off(self):
        with tempfile.TemporaryDirectory() as game, mock.patch.object(help_, 'EBOOT_SIZE', 4096):
            (Path(game) / 'eboot.bin').write_bytes(bytes(4096))
            shown, opened = self.run_after('prepare failed: out of bounds: 0x5000+0x1000 / 0x4000\n', 1, True, game)
        self.assertEqual(opened, ['eboot_unreadable'])
        self.assertIn('eboot.bin cannot be read', shown[0][1])
        self.assertNotIn('cut off', shown[0][1])

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
