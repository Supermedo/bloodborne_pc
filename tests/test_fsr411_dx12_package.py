# SPDX-License-Identifier: GPL-2.0-or-later
"""Pinned runtime preparation: reject untrusted bytes before replacing a DLL."""
import hashlib
import importlib.util
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import zipfile

spec = importlib.util.spec_from_file_location(
    'fetch_fsr411_dx12', Path(__file__).resolve().parents[1] / 'packaging/windows/fetch_fsr411_dx12.py')
fetch = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fetch)


class RuntimePackageTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.target = Path(temporary.name)
        self.payload = b'known runtime bytes'
        with zipfile.ZipFile(self.target / fetch.ARCHIVE_NAME, 'w') as archive:
            archive.writestr(fetch.DLL_MEMBER, self.payload)
            archive.writestr('../unexpected.txt', b'must not be extracted')
        data = (self.target / fetch.ARCHIVE_NAME).read_bytes()
        for name, value in {'ARCHIVE_SIZE': len(data),
                            'ARCHIVE_SHA256': hashlib.sha256(data).hexdigest(),
                            'DLL_SIZE': len(self.payload),
                            'DLL_SHA256': hashlib.sha256(self.payload).hexdigest()}.items():
            override = patch.object(fetch, name, value)
            override.start()
            self.addCleanup(override.stop)

    def test_extracts_only_verified_runtime_and_reuses_cache(self):
        with patch.object(fetch.urllib.request, 'urlopen', side_effect=AssertionError('unexpected download')):
            dll = fetch.prepare(self.target)
            self.assertEqual(dll.read_bytes(), self.payload)
            (self.target / fetch.ARCHIVE_NAME).unlink()
            self.assertEqual(fetch.prepare(self.target), dll)
        self.assertFalse((self.target.parent / 'unexpected.txt').exists())
        self.assertEqual({p.name for p in self.target.iterdir()}, {fetch.DLL_NAME})

    def test_rejects_wrong_member_hash_and_keeps_previous_dll(self):
        dll = self.target / fetch.DLL_NAME
        dll.write_bytes(b'previous runtime')
        with patch.object(fetch, 'DLL_SHA256', '0' * 64):
            with self.assertRaisesRegex(ValueError, 'DLL SHA-256'):
                fetch.prepare(self.target)
        self.assertEqual(dll.read_bytes(), b'previous runtime')

    def test_corrupt_download_does_not_replace_existing_files(self):
        archive = self.target / fetch.ARCHIVE_NAME
        archive.write_bytes(b'old corrupt archive')
        dll = self.target / fetch.DLL_NAME
        dll.write_bytes(b'previous runtime')
        import io
        with patch.object(fetch.urllib.request, 'urlopen', return_value=io.BytesIO(b'bad download')):
            with self.assertRaisesRegex(ValueError, 'archive size or SHA-256'):
                fetch.prepare(self.target)
        self.assertEqual(archive.read_bytes(), b'old corrupt archive')
        self.assertEqual(dll.read_bytes(), b'previous runtime')
        self.assertFalse(list(self.target.glob('*.part')))


if __name__ == '__main__':
    unittest.main()
