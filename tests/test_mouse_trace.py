"""Native diagnostic capture and report export, without game files."""
from paths import ROOT, executable
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
import zipfile
import mouse_trace


class MouseTraceDecoderTests(unittest.TestCase):
    def test_legacy_v1_report_remains_readable(self):
        data = bytearray(4096 + 336 * 4096)
        data[:8] = b'BBMOUSE1'
        struct.pack_into('<4IQ2I', data, 8, 1, 4096, 336, 4096, 1000, 64, 9)
        offsets = tuple(range(0, 64*4, 4)) + tuple(range(0x140, 0x164, 4))
        struct.pack_into('<73I', data, 64, *offsets)
        struct.pack_into('<3Q2ifI73f', data, 4096, 1, 1000, 0x1234, -256, 512, 100, 3, *([.25] * 73))
        columns, rows = mouse_trace.decode(data)
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0][4:6], [-1, 2])
        self.assertNotIn('pad_valid', columns)
        self.assertFalse(mouse_trace.movement_summary(columns, rows)['available'])


@unittest.skipUnless(sys.platform == 'win32' and executable('mouse-trace-test').exists(), 'Windows native diagnostic test')
class MouseTraceTests(unittest.TestCase):
    def run_native(self, directory, *args):
        result = subprocess.run([str(executable('mouse-trace-test')), *args], cwd=directory,
                                capture_output=True, text=True, timeout=45)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_disabled_and_write_failure_do_not_create_capture(self):
        for mode in ('disabled', 'blocked'):
            with self.subTest(mode=mode), tempfile.TemporaryDirectory() as temp:
                self.run_native(temp, mode)
                self.assertEqual(list(Path(temp).rglob('state.bin')), [])

    def test_bounded_capture_unicode_path_and_export(self):
        with tempfile.TemporaryDirectory(prefix='mouse-\u043a\u0430\u043c\u0435\u0440\u0430-') as temp:
            self.run_native(temp)
            directory = next((Path(temp) / 'logs').iterdir())
            data = (directory / 'state.bin').read_bytes()
            columns, rows = mouse_trace.decode(data)
            self.assertEqual(len(rows), 16384)
            self.assertEqual(rows[0][4:7], [-1.0, 2.0, 125.0])
            self.assertEqual(rows[0][7:11], [1, 1, 0, 1])
            self.assertEqual(rows[1][7:11], [0, 0, 0, 0])
            self.assertEqual(rows[0][columns.index('before_140')], .25)
            self.assertEqual(rows[0][columns.index('after_140')], -.5)
            self.assertEqual(rows[0][columns.index('before_030')], .25)
            self.assertEqual(rows[0][columns.index('native_flag_13c')], 1)
            self.assertEqual(rows[0][columns.index('native_flag_263')], 1)
            self.assertEqual(rows[0][columns.index('pad_valid')], 1)
            self.assertEqual(rows[0][columns.index('pad_buttons')], '0x2000')
            self.assertEqual(rows[0][columns.index('left_x'):columns.index('right_y')+1], [37, 37, 128, 128])
            summary = mouse_trace.movement_summary(columns, rows)
            self.assertEqual(summary['pad_samples'], 16384)
            self.assertEqual(summary['diagonal_capture_off'], 1)
            self.assertGreater(summary['diagonal_capture_on'], 1)
            code = (directory / 'code.txt').read_text()
            self.assertIn('call 0x0000000000000050', code)
            self.assertIn('image+0x50', code)
            self.assertIn('00000066:', code)  # RIP-relative data target
            report = mouse_trace.collect(directory)
            with zipfile.ZipFile(report) as archive:
                self.assertIsNone(archive.testzip())
                self.assertEqual(set(archive.namelist()), {'state.bin', 'state.csv', 'code.txt', 'metadata.json'})
                self.assertEqual(archive.read('state.bin'), data)
                self.assertEqual(len(archive.read('state.csv').splitlines()), 16385)
            with self.assertRaises(FileExistsError):
                mouse_trace.collect(directory)
            # Malformed/truncated reports are rejected without reading past a buffer.
            for invalid in (b'', data[:-1], b'unknown!' + data[8:]):
                with self.assertRaises(ValueError):
                    mouse_trace.decode(invalid)
            bad = bytearray(data)
            struct.pack_into('<Q', bad, 4096, 50000)
            with self.assertRaises(ValueError):
                mouse_trace.decode(bad)
            for at, value in ((40, 50), (356, 0x140), (560, 0xffff)):
                bad = bytearray(data)
                struct.pack_into('<I', bad, at, value)
                with self.assertRaises(ValueError):
                    mouse_trace.decode(bad)

    def test_capture_survives_forced_process_exit_without_explicit_flush(self):
        with tempfile.TemporaryDirectory() as temp:
            result = subprocess.run([str(executable('mouse-trace-test')), 'terminate'], cwd=temp,
                                    capture_output=True, timeout=45)
            self.assertEqual(result.returncode, 8, result.stdout + result.stderr)
            data = next(Path(temp).rglob('state.bin')).read_bytes()
            self.assertEqual(len(mouse_trace.decode(data)[1]), 16384)
            bad = bytearray(data)
            struct.pack_into('<I', bad, 8, 999)
            with self.assertRaises(ValueError):
                mouse_trace.decode(bad)


if __name__ == '__main__':
    unittest.main()
