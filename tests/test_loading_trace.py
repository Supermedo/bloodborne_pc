"""Loading diagnostics: isolate the process tree, preserve settings and exclude private files."""
from paths import ROOT
import ctypes
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
import zipfile
import loading_trace


class LoadingTraceTests(unittest.TestCase):
    def test_descendants_do_not_capture_another_game(self):
        entries = [(10, 1, 'Bloodborne.exe'), (12, 11, 'bb-probe.exe'),
                   (11, 10, 'Bloodborne.exe'), (20, 1, 'bb-probe.exe'), (30, 30, 'cycle')]
        self.assertEqual(loading_trace.descendants(10, entries), {10, 11, 12})

    def test_report_uses_an_allowlist(self):
        with tempfile.TemporaryDirectory() as temp:
            directory = Path(temp) / 'loading-test'
            directory.mkdir()
            for name in loading_trace.REPORT_FILES:
                (directory / name).write_text('test', encoding='utf-8')
            for name in ('savedata.bin', 'settings.json', 'eboot.bin', 'private.dmp'):
                (directory / name).write_text('private', encoding='utf-8')
            report = loading_trace.collect(directory)
            with zipfile.ZipFile(report) as archive:
                self.assertEqual(set(archive.namelist()), set(loading_trace.REPORT_FILES))
                self.assertIsNone(archive.testzip())
            with self.assertRaises(FileExistsError):
                loading_trace.collect(directory)

    @unittest.skipUnless(sys.platform == 'win32', 'Windows read-only APIs')
    def test_native_counter_structures_and_handle_cleanup(self):
        self.assertEqual(ctypes.sizeof(loading_trace.ProcessEntry), 568)
        self.assertEqual(ctypes.sizeof(loading_trace.ThreadEntry), 28)
        sampler = loading_trace.WindowsSampler()
        try:
            processes, threads = sampler.sample(os.getpid(), (Path(sys.executable).name.lower(),))
            self.assertIn(os.getpid(), {row[0] for row in processes})
            self.assertTrue(threads)
            self.assertTrue(all(row[1] > 0 for row in processes))  # process creation identity
        finally:
            sampler.close()
        self.assertEqual(sampler.processes, {})
        self.assertEqual(sampler.threads, {})

    @unittest.skipUnless(sys.platform == 'win32', 'Windows input observation')
    def test_markers_require_game_focus_and_new_press(self):
        class Input:
            down, pid = False, 123

            def GetAsyncKeyState(self, _key):
                return 0x8000 if self.down else 0

            def GetForegroundWindow(self):
                return 1

            def GetWindowThreadProcessId(self, _window, target):
                target._obj.value = self.pid

        sampler = loading_trace.WindowsSampler()
        sampler.u = Input()
        try:
            self.assertFalse(sampler.marker({123}))
            sampler.u.down = True
            self.assertTrue(sampler.marker({123}))
            self.assertFalse(sampler.marker({123}))  # holding the key gives one marker
            sampler.u.down = False
            self.assertFalse(sampler.marker({123}))
            sampler.u.pid, sampler.u.down = 456, True
            self.assertFalse(sampler.marker({123}))  # another window
            sampler.u.pid = 123
            self.assertFalse(sampler.marker({123}))  # focus changed while key was held
            sampler.u.down = False
            sampler.marker({123})
            sampler.u.down = True
            self.assertTrue(sampler.marker({123}))
        finally:
            sampler.close()

    @unittest.skipUnless(sys.platform == 'win32', 'Windows counter sampling')
    def test_inventory_and_empty_names_are_not_queried_on_every_sample(self):
        sampler = loading_trace.WindowsSampler()
        try:
            with patch.object(sampler, 'inventory', return_value=(
                    [(os.getpid(), 0, Path(sys.executable).name)], [(1, os.getpid())])) as inventory, \
                 patch.object(sampler, 'times', return_value=(1, 0, 0)), \
                 patch.object(sampler, 'thread_name', return_value='') as name, \
                 patch.object(sampler.k, 'OpenProcess', return_value=111), \
                 patch.object(sampler.k, 'OpenThread', return_value=222), \
                 patch.object(sampler.k, 'GetProcessIdOfThread', return_value=os.getpid()), \
                 patch.object(sampler.k, 'GetProcessIoCounters', return_value=False), \
                 patch.object(sampler.k, 'CloseHandle'), \
                 patch.object(loading_trace.time, 'perf_counter') as clock:
                for tick in range(61):
                    clock.return_value = tick / 10
                    sampler.sample(os.getpid(), (Path(sys.executable).name.lower(),))
                self.assertEqual(inventory.call_count, 13)
                self.assertEqual(name.call_count, 4)  # initial query, three bounded retries
                sampler.close()
        finally:
            sampler.close()

    @unittest.skipUnless(sys.platform == 'win32', 'Windows thread identity')
    def test_reused_thread_id_from_another_process_is_rejected(self):
        sampler = loading_trace.WindowsSampler()
        try:
            with patch.object(sampler, 'inventory', return_value=(
                    [(os.getpid(), 0, Path(sys.executable).name)], [(1, os.getpid())])), \
                 patch.object(sampler.k, 'OpenProcess', return_value=None), \
                 patch.object(sampler.k, 'OpenThread', return_value=222), \
                 patch.object(sampler.k, 'GetProcessIdOfThread', return_value=os.getpid() + 1), \
                 patch.object(sampler.k, 'CloseHandle') as close, \
                 patch.object(sampler, 'thread_name') as name:
                _processes, threads = sampler.sample(os.getpid(), (Path(sys.executable).name.lower(),))
                self.assertEqual(threads, [])
                close.assert_called_once_with(222)
                name.assert_not_called()
        finally:
            sampler.close()

    @unittest.skipUnless(sys.platform == 'win32', 'Windows process sampling')
    def test_real_child_capture(self):
        env_before = dict(os.environ)
        result = subprocess.run([sys.executable, str(ROOT / 'tests/loading_trace_smoke.py')],
                                cwd=ROOT, capture_output=True, timeout=45)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn(b'Loading trace smoke passed', result.stdout)
        self.assertEqual(dict(os.environ), env_before)


if __name__ == '__main__':
    unittest.main()
