# SPDX-License-Identifier: GPL-2.0-or-later
import struct
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
import cloth  # noqa: E402
import cloth_time  # noqa: E402
import foliage_time  # noqa: E402


def rel32(at, code, offset):
    """The target of a rel32 operand at `offset` in `code` placed at `at`."""
    return at + offset + 4 + struct.unpack_from('<i', code, offset)[0]


class PlantSwayTimeTest(unittest.TestCase):
    def test_the_code_lies_between_the_cloths_code_and_constants(self):
        (cave, code), *_sites = foliage_time.code_writes()
        cloth_code, cloth_constants = cloth.code_writes()[:2]
        self.assertEqual(cave, foliage_time.CAVE)
        self.assertGreaterEqual(cave, cloth_code[0] + len(cloth_code[1]))
        self.assertEqual(cave + len(code), cloth_constants[0])
        self.assertEqual(struct.unpack('<f', code[-4:])[0], struct.unpack('<f', struct.pack('<f', cloth_time.LONGEST))[0])

    def test_the_sites_jump_to_their_detours_and_keep_their_length(self):
        (cave, code), *sites = foliage_time.code_writes()
        for (site, jump), at, original in zip(sites, (foliage_time.STORE, foliage_time.KEPT),
                                              (foliage_time.STORE_BYTES, foliage_time.KEPT_BYTES)):
            self.assertEqual(site, at)
            self.assertEqual(len(jump), len(original))
            self.assertEqual(jump[0], 0xE9)
            self.assertTrue(jump[5:] == b'\x90' * (len(original) - 5))
            self.assertTrue(cave <= rel32(site, jump, 1) < cave + len(code))

    def test_the_detours_return_after_their_sites(self):
        (cave, code), *sites = foliage_time.code_writes()
        for (site, jump), original in zip(sites, (foliage_time.STORE_BYTES, foliage_time.KEPT_BYTES)):
            detour = rel32(site, jump, 1) - cave
            back = code.index(b'\xe9', detour + 5)  # after the call to the routine
            self.assertEqual(rel32(cave, code, back + 1), site + len(original))

    def test_the_routine_reads_the_timer_and_the_steady_time(self):
        (cave, code), *_sites = foliage_time.code_writes()
        self.assertEqual(code[:3], bytes.fromhex('505152'))          # push rax, rcx, rdx
        self.assertEqual(rel32(cave, code, 6), cloth_time.FRAME_TIMER)
        steady = code.index(bytes.fromhex('488b15'))
        self.assertEqual(rel32(cave, code, steady + 3), cloth_time.STEADY)
        self.assertIn(bytes.fromhex('5a5958c3'), code)                 # pop rdx, rcx, rax; ret

    def test_the_stored_step_is_stored_as_before(self):
        (cave, code), *_sites = foliage_time.code_writes()
        self.assertIn(foliage_time.STORE_BYTES, code)


if __name__ == '__main__':
    unittest.main()
