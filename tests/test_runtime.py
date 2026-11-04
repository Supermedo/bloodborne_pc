from paths import ROOT
import os
from pathlib import Path
import subprocess
import unittest

# build.sh --test (Linux), or on Windows: ninja -C out/gpu runtime-test, copied to out/.
EXE = next((p for p in (ROOT / 'out/runtime-test', ROOT / 'out/runtime-test.exe') if p.exists()),
           ROOT / 'out/runtime-test')


@unittest.skipUnless(EXE.exists(), 'run bash build.sh --test first')
class RuntimeTests(unittest.TestCase):
    def run_case(self, *args):
        env = dict(os.environ)
        if os.name == 'nt':  # the CLANG64 DLLs it links (SDL3, ...), first on PATH as msys2-env.sh puts them
            root = os.environ.get('MSYS2_ROOT', '')
            root = root if root[1:2] == ':' else 'C:/msys64'
            env['PATH'] = os.path.join(root, 'clang64', 'bin') + os.pathsep + env.get('PATH', '')
        return subprocess.run([str(EXE.resolve()), *args], capture_output=True, text=True, timeout=10, env=env)

    def test_runtime_contracts(self):
        r=self.run_case()
        self.assertEqual(r.returncode,0,r.stdout+r.stderr)
        self.assertIn('PASS: callback lifecycle',r.stdout)

    def test_recursive_guard_fails_explicitly(self):
        r=self.run_case('--guard-recursion')
        self.assertEqual(r.returncode,21)
        self.assertIn('recursive/concurrent static initialization',r.stderr)

    def test_stack_corruption_is_not_ignored(self):
        r=self.run_case('--stack-failure')
        self.assertEqual(r.returncode,22)
        self.assertIn('stack protector detected corruption',r.stderr)

    def test_tls_bounds_are_checked(self):
        r=self.run_case('--bad-tls')
        self.assertEqual(r.returncode,21)
        self.assertIn('unsupported TLS module/offset',r.stderr)

    def test_rwlock_lifecycle_and_ownership(self):
        r=self.run_case('--rwlock-lifecycle')
        self.assertEqual(r.returncode,0,r.stdout+r.stderr)

    def test_rwlock_multiple_readers_exclude_writer(self):
        r=self.run_case('--rwlock-concurrency')
        self.assertEqual(r.returncode,0,r.stdout+r.stderr)

    def test_rwlock_timeouts_use_orbis_errors(self):
        r=self.run_case('--rwlock-timeouts')
        self.assertEqual(r.returncode,0,r.stdout+r.stderr)

    def test_rwlock_nested_read_passes_waiting_writer(self):
        r=self.run_case('--rwlock-nested-read')
        self.assertEqual(r.returncode,0,r.stdout+r.stderr)
