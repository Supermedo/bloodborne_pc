"""One launcher window at a time (bbport_launcher_win.one_launcher). A player had two launchers open, both on the
shared settings in %APPDATA%; a second start now brings the open window to the front."""
from paths import ROOT
import os
import subprocess
import sys
import time
import unittest
import uuid

sys.path.insert(0, str(ROOT / 'launcher'))
sys.path.insert(0, str(ROOT / 'scripts'))
import bbport_launcher_win as L  # noqa: E402


@unittest.skipUnless(os.name == 'nt', 'Windows named objects')
class OneWindowTests(unittest.TestCase):
    def test_a_second_start_brings_the_first_to_the_front(self):
        name, front = f'Local\\bbport-test-{uuid.uuid4().hex}', f'Local\\bbport-test-front-{uuid.uuid4().hex}'
        self.assertTrue(L.one_launcher(name, front))
        requests = []
        self.assertTrue(L.bring_to_front_on_request(lambda: requests.append('front'), front))
        self.assertFalse(L.one_launcher(name, front))
        for _ in range(100):
            if requests:
                break
            time.sleep(0.02)
        self.assertEqual(requests, ['front'])

    def test_another_process_is_turned_away(self):
        name = f'Local\\bbport-test-{uuid.uuid4().hex}'
        self.assertTrue(L.one_launcher(name, name + '-front'))
        code = ("import sys; sys.path[:0] = [r'%s', r'%s']; import bbport_launcher_win as L; "
                "sys.exit(0 if L.one_launcher(r'%s', r'%s-front') else 7)") % (
            ROOT / 'launcher', ROOT / 'scripts', name, name)
        self.assertEqual(subprocess.run([sys.executable, '-c', code], timeout=60).returncode, 7)

    def test_a_restart_waits_for_the_launcher_it_replaces(self):
        start = time.time()
        L.wait_for_exit(2 ** 31 - 1)  # no such process: no wait
        self.assertLess(time.time() - start, 2)
        setup = (ROOT / 'launcher' / 'bbport_setup.py').read_text(encoding='utf-8')
        self.assertIn("command + ['--after', str(os.getpid())]", setup)
        main = (ROOT / 'launcher' / 'bbport_launcher_win.py').read_text(encoding='utf-8')
        main = main[main.index('\ndef main():'):]
        self.assertLess(main.index("args[:1] == ['--after']"), main.index('one_launcher()'))


if __name__ == '__main__':
    unittest.main()
