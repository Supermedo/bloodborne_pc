"""The independent FG pin preserves SR files and verifies cached SDK bytes."""
from pathlib import Path
import hashlib
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'packaging/windows'))
import fsr_sdk_files as sdk
import fetch_fg402_sdk24 as fg


class FgPackageTests(unittest.TestCase):
    def test_independent_official_pin(self):
        self.assertEqual(fg.PIN['tag'], 'v2.4.0')
        self.assertEqual(fg.PIN['commit'], '7539ae8b6568ad29ed78a753404351daf3b5cca7')
        self.assertEqual(len(fg.PIN['files']), 1)
        self.assertEqual(Path(fg.PIN['files'][0]['path']).name,
                         'amd_fidelityfx_framegeneration_dx12.dll')

    def test_cached_fg_and_upscaling_are_independent(self):
        with tempfile.TemporaryDirectory() as folder:
            target = Path(folder)
            sr = target / 'amd_fidelityfx_upscaler_vk.dll'
            sr.write_bytes(b'unchanged SR')
            name = 'amd_fidelityfx_framegeneration_dx12.dll'
            payload = b'FG fixture'
            pin = dict(commit=fg.PIN['commit'], files=[dict(
                path=name, bytes=len(payload), sha256=hashlib.sha256(payload).hexdigest())])
            (target / name).write_bytes(payload)
            with patch.object(fg, 'PIN', pin), patch.object(
                    sdk.urllib.request, 'urlopen', side_effect=AssertionError('no download')):
                self.assertEqual(fg.prepare(target, True), target)
                self.assertEqual(fg.prepare(target), target)
                self.assertEqual(sr.read_bytes(), b'unchanged SR')
                (target / name).write_bytes(b'wrong FG')
                with self.assertRaises(ValueError):
                    fg.prepare(target, True)
            self.assertEqual(sr.read_bytes(), b'unchanged SR')

    def test_missing_check_never_downloads(self):
        with tempfile.TemporaryDirectory() as folder, patch.object(
                sdk.urllib.request, 'urlopen', side_effect=AssertionError('no download')):
            with self.assertRaises(ValueError):
                fg.prepare(folder, True)


if __name__ == '__main__':
    unittest.main()
