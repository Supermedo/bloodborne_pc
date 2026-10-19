from paths import ROOT
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib
from pathlib import Path

import fov
import quit_menu

ROWS = [(0, 43.0), (2306, 48.0), (10, 43.0)]


def lock_cam_param(rows=ROWS):
    """A LOCK_CAM_PARAM_ST table as the game's: 64-bit row table at 0x40, 32-byte rows."""
    header = bytearray(0x40)
    struct.pack_into('<H', header, 0x0A, len(rows))
    header[0x0C:0x0C + 17] = b'LOCK_CAM_PARAM_ST'
    table = bytearray(len(rows) * 24)
    data = bytearray()
    start = 0x40 + len(table)
    for i, (row_id, fov_y) in enumerate(rows):
        struct.pack_into('<IIQQ', table, i * 24, row_id, 0, start + len(data), 0)
        data += struct.pack('<6f', 4.0, -40.0, 0.45, 1.42, 22.0, fov_y) + struct.pack('<2f', 1.0, 2.0)
    return bytes(header + table + data)


def archive(files):
    """A DFLT DCX around a BND4 with UTF-16 names, like gameparam.parambnd.dcx."""
    count = len(files)
    bnd = bytearray(0x40 + count * 0x24)
    bnd[0:4] = b'BND4'
    struct.pack_into('<I', bnd, 0x0C, count)
    struct.pack_into('<Q', bnd, 0x20, 0x24)
    bnd[0x30] = 1
    for i, (name, _body) in enumerate(files):
        struct.pack_into('<I', bnd, 0x40 + i * 0x24 + 0x20, len(bnd))
        bnd += ('N:\\param\\' + name).encode('utf-16-le') + b'\0\0'
    for i, (_name, body) in enumerate(files):
        bnd += b'\0' * (-len(bnd) % 16)
        entry = 0x40 + i * 0x24
        struct.pack_into('<QQII', bnd, entry + 0x08, len(body), len(body), len(bnd), i)
        bnd += body
    header = bytearray(0x4C)
    header[0:4] = b'DCX\0'
    header[0x28:0x2C] = b'DFLT'
    return quit_menu.pack_dcx(bytes(header), bytes(bnd))


def tables(dcx):
    bnd = quit_menu.unpack_dcx(dcx)
    return {fov.bnd4_name(bnd, e): bnd[o:o + n] for e, o, n, _i in quit_menu.bnd4_files(bnd)}


def fov_values(param):
    return [struct.unpack_from('<f', param, row + fov.CAM_FOV_Y)[0] for row in fov.param_rows(param)]


class FovTests(unittest.TestCase):
    def test_every_camera_is_scaled_and_nothing_else_changes(self):
        other = b'other table' * 7
        out, changed = fov.scaled_archive(archive([('Other.param', other), ('LockCamParam.param', lock_cam_param())]), 60)
        self.assertEqual(changed, 3)
        result = tables(out)
        self.assertEqual(result['Other.param'], other)
        values = fov_values(result['LockCamParam.param'])
        self.assertAlmostEqual(values[0], 60.0, places=4)
        self.assertAlmostEqual(values[1], 48.0 * 60 / 43, places=4)  # keeps its difference
        original, scaled = lock_cam_param(), result['LockCamParam.param']
        fov_bytes = {row + fov.CAM_FOV_Y + k for row in fov.param_rows(original) for k in range(4)}
        self.assertTrue(all(original[i] == scaled[i] for i in range(len(original)) if i not in fov_bytes))

    def test_not_a_field_of_view_is_refused(self):
        with self.assertRaises(ValueError):
            fov.scaled_table(lock_cam_param([(0, 0.0)]), 60)
        with self.assertRaises(ValueError):
            fov.scaled_archive(archive([('Other.param', b'x' * 32)]), 60)

    def test_layer_is_kept_until_the_setting_changes(self):
        with tempfile.TemporaryDirectory() as temp:
            game, out = Path(temp) / 'game', Path(temp) / 'layer'
            source = game / fov.ARCHIVE
            source.parent.mkdir(parents=True)
            source.write_bytes(archive([('LockCamParam.param', lock_cam_param())]))

            def run(value):
                result = subprocess.run([sys.executable, str(ROOT / 'scripts/fov.py'), str(game), '--out', str(out),
                                         '--fov', str(value)], capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stderr)
                return result.stdout

            self.assertIn('(3 cameras)', run(60))
            self.assertIn('(kept)', run(60))
            self.assertIn('(3 cameras)', run(70))
            self.assertAlmostEqual(fov_values(tables((out / fov.ARCHIVE).read_bytes())['LockCamParam.param'])[0], 70.0,
                                   places=4)
            self.assertIn('90 degrees', run(200))  # clamped


if __name__ == '__main__':
    unittest.main()
