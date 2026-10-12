from paths import ROOT, executable
from pathlib import Path
import subprocess
import unittest

EXE=executable('sema-test')

@unittest.skipUnless(EXE.exists(),'run bash build.sh --test first')
class SemaphoreTests(unittest.TestCase):
    def run_case(self,*args):
        return subprocess.run([str(EXE.resolve()),*args],capture_output=True,text=True,timeout=10)

    def test_counts_errors_timeout_and_stale_ids(self):
        r=self.run_case()
        self.assertEqual(r.returncode,0,r.stdout+r.stderr)

    def test_fifo_wakeup_and_token_reservation(self):
        r=self.run_case('--concurrency')
        self.assertEqual(r.returncode,0,r.stdout+r.stderr)

    def test_cancel_and_delete_wake_all_waiters(self):
        r=self.run_case('--cancel-delete')
        self.assertEqual(r.returncode,0,r.stdout+r.stderr)

    def test_priority_wait_blocks_until_signal(self):
        r=self.run_case('--priority')
        self.assertEqual(r.returncode,0,r.stdout+r.stderr)
