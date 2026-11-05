from paths import ROOT
from io import BytesIO
import importlib.util
import os
from pathlib import Path
import re
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('bbport_trophies', ROOT / 'launcher/bbport_trophies.py')
trophies = importlib.util.module_from_spec(spec)
spec.loader.exec_module(trophies)


class TrophyTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.user = Path(self.temp.name)
        self.tracker = trophies.Tracker(self.user)

    def test_list(self):
        ids = [t[0] for t in trophies.TROPHIES]
        self.assertEqual(ids, list(range(40)))
        self.assertEqual(trophies.BY_ID[0][3], trophies.PLATINUM)
        self.assertEqual(len(trophies.PLATINUM_NEEDS), 33)
        self.assertEqual(sum(t[4] == trophies.DLC for t in trophies.TROPHIES), 6)

    def test_read_log_keeps_earliest_and_skips_garbage(self):
        (self.user / 'trophies.log').write_text('14 200\n14 100\nbroken\n21 300 extra\n\n', encoding='utf-8')
        self.assertEqual(trophies.read_log(self.user / 'trophies.log'), {14: 100, 21: 300})
        self.assertEqual(trophies.read_log(self.user / 'missing.log'), {})

    def test_merge_adds_the_other_folders_unlocks_and_removes_none(self):
        other = self.user / 'other'
        other.mkdir()
        (other / 'trophies.log').write_text('14 100\n21 300\n15 400\n', encoding='utf-8')
        (self.user / 'trophies.log').write_text('21 200\n16 500\n', encoding='utf-8')
        self.assertEqual(trophies.merge_log(other / 'trophies.log', self.user / 'trophies.log'), 2)  # 14 and 15
        self.assertEqual(trophies.read_log(self.user / 'trophies.log'), {14: 100, 15: 400, 16: 500, 21: 200})
        self.assertEqual(trophies.merge_log(other / 'trophies.log', self.user / 'trophies.log'), 0)  # once is enough
        self.assertEqual(trophies.merge_log(self.user / 'missing.log', self.user / 'trophies.log'), 0)
        self.assertEqual(self.tracker.poll(), [14, 21, 15, 16])  # the Achievements page shows them

    def test_a_shadps4_trophy_list(self):
        """shadPS4 marks an unlock in TROP.XML: unlockstate and sceRtcGetCurrentTick's microseconds since the year 1."""
        when = 1790000000
        xml = self.user / 'TROP.XML'
        xml.write_text('<?xml version="1.0"?><trophyconf><title-name>Bloodborne</title-name>'
                       f'<trophy id="014" hidden="no" ttype="B" pid="000" unlockstate="true" '
                       f'timestamp="{(when + trophies.RTC_UNIX) * 1_000_000}"><name>Father Gascoigne</name></trophy>'
                       '<trophy id="015" ttype="B" unlockstate="false"></trophy>'
                       '<trophy id="021" ttype="B" unlockstate="true"></trophy>'
                       '<trophy id="022" ttype="B" unlockstate="true" timestamp="1790000123456"></trophy>'
                       '<trophy id="99" unlockstate="true" timestamp="0"></trophy></trophyconf>', encoding='utf-8')
        os.utime(xml, (1780000000, 1780000000))
        self.assertEqual(trophies.read_unlocks(xml), {14: when, 21: 1780000000, 22: 1790000123})  # no time: the file's
        other = self.user / 'TROP_other.XML'
        other.write_text('<title-name>Another game</title-name><trophy id="1" unlockstate="true"/>', encoding='utf-8')
        self.assertEqual(trophies.read_unlocks(other), {})
        self.assertEqual(trophies.read_unlocks(self.user / 'missing.xml'), {})
        # An installation's Xml folder: every language's list, the earliest time of each trophy.
        folder = self.user / 'Xml'
        folder.mkdir()
        xml.replace(folder / 'TROP.XML')
        (folder / 'TROP_08.XML').write_text('<title-name>Bloodborne</title-name><trophy id="014" unlockstate="true" '
                                            'timestamp="1789000000"/><trophy id="016" unlockstate="true" '
                                            'timestamp="1789000000"/>', encoding='utf-8')
        (folder / 'TROPCONF.XML').write_text('<title-name>Bloodborne</title-name><trophy id="017"/>', encoding='utf-8')
        self.assertEqual(trophies.read_unlocks(folder), {14: 1789000000, 16: 1789000000, 21: 1780000000,
                                                         22: 1790000123})

    def test_another_installations_unlocks_are_added_and_none_removed(self):
        """R86: a player with a new Windows merged two trophies.log by hand."""
        target = self.user / 'trophies.log'
        target.write_bytes(b'21 200\n16 500')  # a last line without its end
        found = {14: 100, 21: 150, 16: 600, 99: 5}  # 99 is no trophy of the game
        self.assertEqual(trophies.missing(found, target), {14: 100, 21: 150, 99: 5})
        added, copy = trophies.add_unlocks(found, target)
        self.assertEqual(added, 1)  # 14; 21 only gets its earlier time
        self.assertEqual(copy.read_bytes(), b'21 200\n16 500')
        self.assertEqual(copy.parent, self.user)
        self.assertEqual(trophies.read_log(target), {14: 100, 16: 500, 21: 150})
        self.assertTrue(target.read_text(encoding='utf-8').startswith('21 200\n16 500\n'))  # appended only
        self.assertEqual(trophies.add_unlocks(found, target), (0, None))  # once is enough: no second copy
        self.assertEqual(len(list(self.user.glob('trophies.log.*.bak'))), 1)
        fresh = self.user / 'new' / 'trophies.log'
        with self.assertRaises(OSError):  # a saves folder that is not there is never made here
            trophies.add_unlocks({14: 100}, fresh)
        self.assertFalse(fresh.parent.exists())
        fresh.parent.mkdir()
        self.assertEqual(trophies.add_unlocks({14: 100}, fresh), (1, None))  # nothing to copy yet
        self.assertEqual(trophies.read_log(fresh), {14: 100})

    def test_observe_records_once(self):
        self.assertEqual(self.tracker.observe('Runtime: trophy 14 unlocked\n'), [14])
        self.assertEqual(self.tracker.observe('Runtime: trophy 14 unlocked\n'), [])
        self.assertEqual(self.tracker.observe('Runtime: something else\n'), [])
        self.assertEqual(set(trophies.read_log(self.user / 'trophies.log')), {14})

    def test_runtime_entry_is_not_written_twice(self):
        (self.user / 'trophies.log').write_text('16 50\n', encoding='utf-8')
        self.assertEqual(self.tracker.observe('Runtime: trophy 16 unlocked\n'), [16])
        self.assertEqual((self.user / 'trophies.log').read_text(encoding='utf-8'), '16 50\n')
        self.assertEqual(self.tracker.unlocked, {16: 50})

    def test_poll_reads_the_game_log(self):
        self.assertEqual(self.tracker.poll(), [])
        (self.user / 'trophies.log').write_text('1 10\n2 20\n', encoding='utf-8')
        self.assertEqual(self.tracker.poll(), [1, 2])
        self.assertEqual(self.tracker.poll(), [])

    def test_poll_reads_last_run_log_incrementally(self):
        run = self.user / 'last_run.log'
        run.write_bytes(b'boot\nRuntime: trophy 21 unlocked\nRuntime: trophy 2')
        self.assertEqual(self.tracker.poll(), [21])
        with open(run, 'ab') as log:
            log.write(b'2 unlocked\n')
        os.utime(run, ns=(1, 2_000_000_000))
        self.assertEqual(self.tracker.poll(), [22])
        self.assertEqual(self.tracker.poll(run_log=False), [])
        # A new run rewrites the file: it is read from the start again.
        run.write_bytes(b'other start of a much longer log line\nRuntime: trophy 23 unlocked\n' * 3)
        self.assertEqual(self.tracker.poll(), [23])
        self.assertEqual(set(trophies.read_log(self.user / 'trophies.log')), {21, 22, 23})

    def test_platinum_after_every_base_trophy(self):
        base = sorted(trophies.PLATINUM_NEEDS)
        self.tracker.record(base[:-1], when=100)
        self.assertNotIn(0, self.tracker.unlocked)
        self.tracker.record([34, 35], when=150)
        self.assertNotIn(0, self.tracker.unlocked)
        self.assertEqual(self.tracker.record([base[-1]], when=200), [base[-1], 0])
        self.assertEqual(self.tracker.unlocked[0], 200)
        self.assertEqual(trophies.read_log(self.user / 'trophies.log')[0], 200)
        self.assertEqual(set(trophies.Tracker(self.user).poll()), set(base + [34, 35, 0]))

    def test_runtime_awards_the_platinum_for_the_same_trophies(self):
        """src/runtime_services.c awards the platinum itself (the in-game pop-up): same rule as here."""
        source = (ROOT / 'src/runtime_services.c').read_text(encoding='utf-8')
        value = lambda name: int(re.search(rf'#define {name} (\d+)', source).group(1))
        self.assertEqual(value('TROPHY_PLATINUM'), trophies.PLATINUM_ID)
        self.assertEqual(set(range(value('TROPHY_BASE_FIRST'), value('TROPHY_BASE_LAST') + 1)),
                         set(trophies.PLATINUM_NEEDS))


run_spec = importlib.util.spec_from_file_location('bbport_run', ROOT / 'run.py')
run_py = importlib.util.module_from_spec(run_spec)
run_spec.loader.exec_module(run_py)
lang_spec = importlib.util.spec_from_file_location('bbport_lang_trophies', ROOT / 'launcher/bbport_lang_trophies.py')
lang_trophies = importlib.util.module_from_spec(lang_spec)
lang_spec.loader.exec_module(lang_trophies)


class TrophyPopupTextTests(unittest.TestCase):
    """run.py's texts for the in-game pop-up (BB_TROPHY_TEXT, read by gpu/shim/bbport_overlay.cpp)."""

    def written(self, language):
        with tempfile.TemporaryDirectory() as folder:
            path = run_py.write_trophy_text(Path(folder) / 'trophies.tsv', language)
            return path.read_text(encoding='utf-8')

    def parse(self, text):
        header, rows = {}, {}
        for line in text.splitlines():
            fields = line.split('\t')
            if fields[0] in ('lang', 'heading'):
                self.assertEqual(len(fields), 2, line)
                header[fields[0]] = fields[1]
            else:
                self.assertEqual(len(fields), 4, line)  # no tab or line break inside a text
                self.assertIn(fields[1], 'pgsb')
                self.assertTrue(fields[2] and fields[3], line)
                rows[int(fields[0])] = fields[1:]
        return header, rows

    def test_english_by_default(self):
        for language in ('1', None, '', '99'):
            header, rows = self.parse(self.written(language))
            self.assertEqual(header, {'lang': 'en', 'heading': 'Trophy earned'})
            self.assertEqual(sorted(rows), list(range(40)))
            self.assertEqual(rows[0], ['p', 'Bloodborne', 'All trophies acquired. Hats off!'])
            self.assertEqual(rows[1][0], 'g')
            self.assertEqual(rows[14][:2], ['b', 'Father Gascoigne'])

    def test_every_game_language_has_its_heading_and_names(self):
        for language, code in run_py.TROPHY_LANGUAGES.items():
            header, rows = self.parse(self.written(language))
            self.assertEqual(header['lang'], code)
            self.assertEqual(header['heading'], run_py.TROPHY_HEADINGS[language])
            self.assertEqual(sorted(rows), list(range(40)))
            if code != 'en':
                joiner = '' if code in ('zh', 'ja') else ' '
                for trophy in (1, 2):
                    for column, english in ((1, trophies.BY_ID[trophy][1]), (2, trophies.BY_ID[trophy][2])):
                        words = lang_trophies.TRANSLATIONS[code][english].splitlines()
                        self.assertEqual(rows[trophy][column], joiner.join(w.strip() for w in words if w.strip()))

    def test_translated_line_breaks_are_joined(self):
        _, rows = self.parse(self.written('4'))  # German: a description with a line break
        self.assertEqual(rows[2][2], 'Ergriffen von der Präsenz des Mondes schwörst du, über den Traum des Jägers zu wachen.')


if __name__ == '__main__':
    unittest.main()


class TrophyPopupPictureTests(unittest.TestCase):
    """run.py's pictures for the in-game pop-up (BB_TROPHY_ICONS: <id>.rgba from the game's trophy00.trp)."""

    def setUp(self):
        try:
            from PIL import Image
        except ImportError:
            self.skipTest('Pillow is not installed')
        from test_trophy_icons import archive
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.game = Path(self.temp.name) / 'game'
        self.folder = Path(self.temp.name) / 'out' / 'trophy_icons'
        self.trp = self.game / 'sce_sys' / 'trophy' / 'trophy00.trp'

        def png(colour, size):
            data = BytesIO()
            Image.new('RGBA', (size, size), colour).save(data, 'PNG')
            return data.getvalue()
        self.png, self.archive = png, archive

    def test_each_picture_as_raw_rgba_and_none_for_the_missing(self):
        self.archive(self.trp, [(b'TROP000.PNG', self.png((255, 0, 0, 255), 240), 0),
                                (b'TROP014.PNG', self.png((0, 0, 255, 128), 16), 0)])
        run_py.write_trophy_icons(self.folder, self.game)
        side = run_py.TROPHY_ICON_SIZE
        for trophy, pixel in ((0, b'\xff\x00\x00\xff'), (14, b'\x00\x00\xff\x80')):
            data = (self.folder / f'{trophy}.rgba').read_bytes()
            self.assertEqual(len(data), side * side * 4)
            self.assertEqual(data[:4], pixel)
        self.assertEqual(sorted(p.name for p in self.folder.glob('*.rgba')), ['0.rgba', '14.rgba'])

    def test_done_again_only_when_the_trophy_file_changed(self):
        self.archive(self.trp, [(b'TROP000.PNG', self.png((255, 0, 0, 255), 32), 0)])
        run_py.write_trophy_icons(self.folder, self.game)
        (self.folder / '0.rgba').write_bytes(b'kept')
        run_py.write_trophy_icons(self.folder, self.game)  # the same trophy file: nothing written
        self.assertEqual((self.folder / '0.rgba').read_bytes(), b'kept')
        self.archive(self.trp, [(b'TROP000.PNG', self.png((0, 255, 0, 255), 64), 0)])
        os.utime(self.trp, ns=(self.trp.stat().st_atime_ns, self.trp.stat().st_mtime_ns + 1_000_000_000))
        run_py.write_trophy_icons(self.folder, self.game)
        self.assertEqual((self.folder / '0.rgba').read_bytes()[:4], b'\x00\xff\x00\xff')

    def test_no_trophy_file_raises_for_the_start_to_note(self):
        with self.assertRaises(OSError):
            run_py.write_trophy_icons(self.folder, self.game)
