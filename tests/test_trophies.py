from paths import ROOT
import importlib.util
import os
from pathlib import Path
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


if __name__ == '__main__':
    unittest.main()
