# SPDX-License-Identifier: GPL-2.0-or-later
"""Keep valid installs on failures and never accept an unverified download."""
import hashlib
import io
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'packaging/windows'))
import fsr_sdk_files as sdk


class SdkFilesTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.target = Path(self.temporary.name) / 'SDK Umlaut ä'
        self.data = b'checked SDK effect file'
        self.row = dict(path='Kits/FidelityFX/signedbin/effect.dll', bytes=len(self.data),
                        sha256=hashlib.sha256(self.data).hexdigest())
        self.pin = dict(commit='7539ae8b6568ad29ed78a753404351daf3b5cca7', files=[self.row])

    def test_valid_cache_never_uses_the_network(self):
        self.target.mkdir()
        (self.target / 'effect.dll').write_bytes(self.data)
        with patch.object(sdk.urllib.request, 'urlopen') as network:
            self.assertEqual(sdk.prepare(self.target, self.pin), self.target)
        network.assert_not_called()

    def test_check_only_never_creates_directories_or_downloads(self):
        with patch.object(sdk.urllib.request, 'urlopen') as network:
            with self.assertRaisesRegex(ValueError, 'missing or changed'):
                sdk.prepare(self.target, self.pin, check=True)
        network.assert_not_called()
        self.assertFalse(self.target.exists())

    def test_valid_download_uses_pinned_commit_and_replaces_atomically(self):
        self.target.mkdir()
        (self.target / 'effect.dll').write_bytes(b'old')
        with patch.object(sdk.urllib.request, 'urlopen', return_value=io.BytesIO(self.data)) as network:
            sdk.prepare(self.target, self.pin)
        self.assertIn('/' + self.pin['commit'] + '/', network.call_args.args[0].full_url)
        self.assertEqual((self.target / 'effect.dll').read_bytes(), self.data)
        self.assertFalse(list(self.target.glob('*.part')))

    def test_same_size_wrong_hash_keeps_existing_file_and_cleans_partial(self):
        self.target.mkdir()
        (self.target / 'effect.dll').write_bytes(b'old')
        with patch.object(sdk.urllib.request, 'urlopen', return_value=io.BytesIO(b'x' * len(self.data))):
            with self.assertRaisesRegex(ValueError, 'SHA256 mismatch'):
                sdk.prepare(self.target, self.pin)
        self.assertEqual((self.target / 'effect.dll').read_bytes(), b'old')
        self.assertFalse(list(self.target.glob('*.part')))

    def test_network_failure_keeps_existing_file_and_cleans_partial(self):
        self.target.mkdir()
        (self.target / 'effect.dll').write_bytes(b'old')
        with patch.object(sdk.urllib.request, 'urlopen', side_effect=OSError('offline')):
            with self.assertRaises(OSError):
                sdk.prepare(self.target, self.pin)
        self.assertEqual((self.target / 'effect.dll').read_bytes(), b'old')
        self.assertFalse(list(self.target.glob('*.part')))

    def test_unchecked_component_is_not_requested(self):
        with patch.object(sdk.urllib.request, 'urlopen', return_value=io.BytesIO(self.data)) as network:
            sdk.prepare(self.target, self.pin)
        self.assertEqual(network.call_count, 1)
        self.assertEqual([p.name for p in self.target.iterdir()], ['effect.dll'])


if __name__ == '__main__':
    unittest.main()
