# SPDX-License-Identifier: GPL-2.0-or-later
import struct
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
import cloth  # noqa: E402
import quit_menu  # noqa: E402


class SteadyClothTest(unittest.TestCase):
    def test_the_code_fits_its_cave(self):
        (cave, code), (constants, values), (site, jump), *bends = cloth.code_writes()
        self.assertEqual(cave, cloth.CAVE)
        self.assertLessEqual(cave + len(code), cloth.CODE_END)  # foliage_time.py's code follows
        self.assertGreaterEqual(cave, 0x50D9C48)  # past PC Settings' page table (pc_menu.py)
        self.assertEqual(constants + len(values), quit_menu.CAVE_END)
        self.assertEqual(struct.unpack(f'<{len(cloth.CONSTANTS)}f', values), cloth.CONSTANTS)

    def test_the_site_jumps_to_the_cave_and_keeps_its_length(self):
        (cave, code), _constants, (site, jump), *bends = cloth.code_writes()
        self.assertEqual(site, cloth.SITE)
        self.assertEqual(len(jump), len(cloth.SITE_BYTES))
        self.assertEqual(jump[0], 0xE9)
        self.assertEqual(site + 5 + struct.unpack_from('<i', jump, 1)[0], cave)

    def test_the_cave_returns_after_the_site(self):
        code = cloth.code_writes()[0][1]
        end = code.index(cloth.BEND_BYTES)  # the bend detours follow the step's
        self.assertEqual(code[end - 5], 0xE9)
        self.assertEqual(cloth.CAVE + end + struct.unpack_from('<i', code, end - 4)[0], cloth.RETURN)

    def test_the_bend_sites_jump_to_their_detours(self):
        writes = cloth.code_writes()
        cave, code = writes[0]
        for (site, jump), original in zip(writes[3:], (cloth.BEND_BYTES, cloth.BATCH_BYTES)):
            self.assertEqual(len(jump), len(original))
            target = site + 5 + struct.unpack_from('<i', jump, 1)[0]
            self.assertEqual(code[target - cave:target - cave + len(original)], original)

    def test_other_code_at_the_site_is_refused(self):
        originals = {cloth.SITE: cloth.SITE_BYTES, cloth.BEND: cloth.BEND_BYTES, cloth.BATCH: cloth.BATCH_BYTES}
        cloth.check_original(lambda start, end: originals[start])
        with self.assertRaises(ValueError):
            cloth.check_original(lambda start, end: b'\x90' * (end - start))


if __name__ == '__main__':
    unittest.main()
