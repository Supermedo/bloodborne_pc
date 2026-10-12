# SPDX-License-Identifier: GPL-2.0-or-later
import importlib.util
from pathlib import Path
import struct
import tempfile
import unittest
import zipfile

spec = importlib.util.spec_from_file_location('install_remix', Path(__file__).parents[1] / 'scripts/install_remix.py')
installer = importlib.util.module_from_spec(spec)
spec.loader.exec_module(installer)


def pe(machine=0x8664):
    data = bytearray(128)
    data[:2] = b'MZ'
    struct.pack_into('<I', data, 60, 64)
    data[64:68] = b'PE\0\0'
    struct.pack_into('<H', data, 68, machine)
    return bytes(data)


class RuntimeInstallTest(unittest.TestCase):
    def test_extract_only_x64_runtime_and_licenses(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            with zipfile.ZipFile(root / 'runtime.zip', 'w') as archive:
                archive.writestr('.trex/d3d9.dll', pe())
                archive.writestr('.trex/usd/plugin/data.txt', 'plugin')
                archive.writestr('d3d9.dll', pe(0x14c))
                archive.writestr('NvRemixLauncher32.exe', b'bridge')
                archive.writestr('LICENSE.txt', 'license')
                archive.writestr('ThirdPartyLicenses-dxvk.txt', 'notices')
            target = root / 'output'
            installer.extract_runtime(root / 'runtime.zip', target)
            self.assertEqual((target / '.trex/d3d9.dll').read_bytes(), pe())
            self.assertTrue((target / 'LICENSE.txt').is_file())
            self.assertTrue((target / '.trex/usd/plugin/data.txt').is_file())
            self.assertFalse((target / 'd3d9.dll').exists())
            self.assertFalse((target / 'NvRemixLauncher32.exe').exists())

    def test_traversal_and_drive_paths_rejected(self):
        for name in ('../escaped', '.trex/../../escaped', '/absolute', 'C:/escaped', '.trex\\..\\..\\escaped'):
            with self.subTest(name=name), tempfile.TemporaryDirectory() as temp:
                root = Path(temp)
                with zipfile.ZipFile(root / 'runtime.zip', 'w') as archive:
                    archive.writestr('.trex/d3d9.dll', pe())
                    archive.writestr(name, b'data')
                with self.assertRaises(ValueError):
                    installer.extract_runtime(root / 'runtime.zip', root / 'output')
                self.assertFalse((root / 'escaped').exists())

    def test_x86_runtime_rejected(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            with zipfile.ZipFile(root / 'runtime.zip', 'w') as archive:
                archive.writestr('.trex/d3d9.dll', pe(0x14c))
            with self.assertRaisesRegex(ValueError, 'x64'):
                installer.extract_runtime(root / 'runtime.zip', root / 'output')

    def test_bad_hash_never_publishes_installation(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / 'bad.zip').write_bytes(b'wrong runtime')
            with self.assertRaisesRegex(ValueError, 'SHA-256'):
                installer.install(root / 'destination', root / 'bad.zip')
            self.assertFalse((root / 'destination').exists())

    def test_existing_directory_kept(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / 'keep.txt').write_text('keep')
            with self.assertRaisesRegex(ValueError, 'already exists'):
                installer.install(root, root / 'missing.zip')
            self.assertEqual((root / 'keep.txt').read_text(), 'keep')

    def test_symlink_rejected(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            with zipfile.ZipFile(root / 'runtime.zip', 'w') as archive:
                archive.writestr('.trex/d3d9.dll', pe())
                link = zipfile.ZipInfo('.trex/link')
                link.create_system = 3
                link.external_attr = 0o120777 << 16
                archive.writestr(link, '../../escape')
            with self.assertRaisesRegex(ValueError, 'link'):
                installer.extract_runtime(root / 'runtime.zip', root / 'output')


if __name__ == '__main__':
    unittest.main()
