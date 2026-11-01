"""The packaging gate must reject broken bundles, including with Python -O."""
from paths import ROOT
import importlib.util
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

SCRIPT = ROOT / 'packaging/windows/check_launcher_runtime.py'
spec = importlib.util.spec_from_file_location('runtime_check', SCRIPT)
C = importlib.util.module_from_spec(spec)
spec.loader.exec_module(C)


class RuntimeCheckTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.own, self.other = [Path(temporary.name) / name for name in ('own', 'other')]
        self.files = ['_ssl.pyd', f'python{sys.version_info.major}{sys.version_info.minor}.dll',
                      'libssl-3.dll', 'libcrypto-3.dll', '_tkinter.pyd', 'PIL/_imaging.pyd',
                      'PIL/_imagingft.pyd', '_tcl_data/init.tcl', '_tk_data/tk.tcl']
        for folder in (self.own, self.other):
            for name in self.files:
                path = folder / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(name.encode())

    def test_matching_native_and_script_files(self):
        (self.other / 'extra.dll').write_bytes(b'launcher-only dependency')
        C.compare_bundles(self.own, self.other)
        self.assertEqual(set(C.bundle_files(self.own)), {Path(name) for name in self.files})

    def test_missing_and_modified_dependencies_are_rejected(self):
        for name in self.files:
            with self.subTest(file=name):
                target = self.other / name
                data = target.read_bytes()
                target.unlink()
                with self.assertRaisesRegex(RuntimeError, 'missing'):
                    C.compare_bundles(self.own, self.other)
                target.write_bytes(data + b'changed')
                with self.assertRaisesRegex(RuntimeError, 'differs'):
                    C.compare_bundles(self.own, self.other)
                target.write_bytes(data)

    def test_each_tls_component_is_required(self):
        for name in self.files[:4]:
            with self.subTest(file=name):
                target = self.own / name
                data = target.read_bytes()
                target.unlink()
                with self.assertRaisesRegex(RuntimeError, 'missing'):
                    C.bundle_files(self.own)
                target.write_bytes(data)

    def test_missing_bundle_is_rejected(self):
        with self.assertRaisesRegex(RuntimeError, 'missing'):
            C.compare_bundles(self.own, self.other / 'absent')

    def test_optimized_python_still_rejects_changes(self):
        (self.other / 'PIL/_imaging.pyd').write_bytes(b'changed')
        code = ('import runpy,sys; m=runpy.run_path(sys.argv[1]); '
                'm["compare_bundles"](sys.argv[2],sys.argv[3])')
        for option in ('-O', '-OO'):
            with self.subTest(option=option):
                result = subprocess.run([sys.executable, '-B', option, '-c', code, str(SCRIPT),
                                         str(self.own), str(self.other)], capture_output=True,
                                        text=True, timeout=15)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn('Launcher runtime file differs', result.stderr)

    def test_tcl_filesystem_and_embedded_libraries(self):
        tcl = mock.Mock()
        tcl.eval.side_effect = ['8.6', str(self.own / '_tcl_data')]
        C.check_tcl(tcl, self.own)
        tcl.eval.side_effect = ['9.0', '//zipfs:/lib/tcl/tcl_library']
        tcl.call.return_value = 'mount-list'
        tcl.splitlist.return_value = ('//zipfs:/lib/tcl', str(self.own / 'tcl90.dll'))
        C.check_tcl(tcl, self.own)
        for library in (str(self.other), '//zipfs:/lib/tcl/tcl_library'):
            tcl.eval.side_effect = ['9.0', library]
            tcl.splitlist.return_value = ('//zipfs:/lib/tcl', str(self.other / 'tcl90.dll'))
            with self.assertRaisesRegex(RuntimeError, 'outside'):
                C.check_tcl(tcl, self.own)
        tcl.eval.side_effect = ['9.0', '//zipfs:/lib/tcl/tcl_library']
        tcl.splitlist.return_value = ()
        with self.assertRaisesRegex(RuntimeError, 'Cannot locate'):
            C.check_tcl(tcl, self.own)

    def test_actual_launcher_failure_timeout_and_missing_marker(self):
        exe = self.other / 'Bloodborne.exe'
        exe.write_bytes(b'fixture')
        with mock.patch.object(C, 'check'), mock.patch.object(C.subprocess, 'run') as run:
            run.return_value = subprocess.CompletedProcess([], 1, '', 'import failed')
            with self.assertRaisesRegex(RuntimeError, 'import failed'):
                C.check_launcher(exe, SCRIPT)
            run.return_value = subprocess.CompletedProcess([], 0, '', '')
            with self.assertRaisesRegex(RuntimeError, 'did not complete'):
                C.check_launcher(exe, SCRIPT)
            run.side_effect = subprocess.TimeoutExpired([], 30)
            with self.assertRaisesRegex(RuntimeError, 'timed out after 30 seconds'):
                C.check_launcher(exe, SCRIPT)

    def test_launcher_timeout_keeps_partial_output_and_command(self):
        exe = self.other / 'Bloodborne.exe'
        exe.write_bytes(b'fixture')
        for stdout, stderr in ((b'partial stdout\n', b'partial stderr \xff'),
                               ('text stdout\n', 'text stderr'), (None, None)):
            with self.subTest(stdout=stdout), mock.patch.object(C, 'check'), \
                    mock.patch.object(C.subprocess, 'run') as run:
                run.side_effect = subprocess.TimeoutExpired(['launcher'], 2, output=stdout, stderr=stderr)
                with self.assertRaisesRegex(RuntimeError, 'timed out after 2 seconds') as error:
                    C.check_launcher(exe, SCRIPT, timeout=2)
                message = str(error.exception)
                self.assertIn('An error dialog may be blocking startup', message)
                self.assertIn('rerun the command manually', message)
                self.assertIsInstance(error.exception.__cause__, subprocess.TimeoutExpired)
                self.assertEqual(error.exception.__cause__.cmd, ['launcher'])
                if stdout:
                    self.assertIn('stdout', message)
                    self.assertIn('stderr', message)
                if isinstance(stderr, bytes):
                    self.assertIn('\ufffd', message)

    def test_windows_launcher_gets_only_windows_tools_on_path(self):
        exe = self.other / 'Bloodborne.exe'
        exe.write_bytes(b'fixture')
        windows = self.own / 'Windows'
        original_path = 'compiler-and-python-tools'
        with mock.patch.object(C, 'check'), mock.patch.object(C.subprocess, 'run') as run, \
                mock.patch.object(C.sys, 'platform', 'win32'), \
                mock.patch.dict(os.environ, {'SystemRoot': str(windows), 'PATH': original_path,
                                             'RUNTIME_CHECK_TEST': 'preserved'}):
            run.return_value = subprocess.CompletedProcess([], 0, C.PASS + '\n', '')
            C.check_launcher(exe, SCRIPT)
            child = run.call_args.kwargs['env']
            self.assertEqual(child['PATH'].split(os.pathsep), [str(windows / 'System32'), str(windows)])
            self.assertEqual(child['RUNTIME_CHECK_TEST'], 'preserved')
            self.assertEqual(os.environ['PATH'], original_path)

    def test_non_windows_launcher_preserves_path(self):
        exe = self.other / 'Bloodborne.exe'
        exe.write_bytes(b'fixture')
        with mock.patch.object(C, 'check'), mock.patch.object(C.subprocess, 'run') as run, \
                mock.patch.object(C.sys, 'platform', 'linux'), \
                mock.patch.dict(os.environ, {'PATH': 'system-tools:build-tools'}):
            run.return_value = subprocess.CompletedProcess([], 0, C.PASS + '\n', '')
            C.check_launcher(exe, SCRIPT)
            self.assertEqual(run.call_args.kwargs['env']['PATH'], os.environ['PATH'])

    def test_actual_launcher_success_and_missing_inputs(self):
        exe = self.other / 'Bloodborne.exe'
        with self.assertRaisesRegex(RuntimeError, 'executable missing'):
            C.check_launcher(exe, SCRIPT)
        exe.write_bytes(b'fixture')
        with self.assertRaisesRegex(RuntimeError, 'script missing'):
            C.check_launcher(exe, SCRIPT / 'absent')
        with mock.patch.object(C, 'check') as check, mock.patch.object(C.subprocess, 'run') as run:
            run.return_value = subprocess.CompletedProcess([], 0, C.PASS + '\n', '')
            C.check_launcher(exe, SCRIPT)
            check.assert_called_once_with(exe.resolve().parent / '_internal')
            self.assertEqual(run.call_args.kwargs['timeout'], 30)
            self.assertEqual(run.call_args.args[0], [str(exe.resolve()), '--script', str(SCRIPT.resolve())])

    def test_uninitialized_tcl_is_rejected(self):
        tcl = mock.Mock()
        tcl.eval.return_value = ''
        with self.assertRaisesRegex(RuntimeError, 'did not initialize'):
            C.check_tcl(tcl)

    def test_cli_errors_return_failure(self):
        with mock.patch.object(C.traceback, 'print_exc'):
            self.assertEqual(C.main(['--launcher']), 1)
            self.assertEqual(C.main(['one', 'two']), 1)
            with mock.patch.object(C, 'check', side_effect=ImportError('missing tkinter.font')):
                self.assertEqual(C.main([]), 1)
