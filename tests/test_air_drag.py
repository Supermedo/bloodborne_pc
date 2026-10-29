# SPDX-License-Identifier: GPL-2.0-or-later
import struct
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
import air_drag  # noqa: E402
import cloth  # noqa: E402
import pc_menu  # noqa: E402


class AirDragTest(unittest.TestCase):
    def test_the_code_fits_between_pc_settings_and_cloth(self):
        (cave, code), _site = air_drag.code_writes()
        self.assertEqual(cave, air_drag.CAVE)
        self.assertEqual(cave + len(code), air_drag.CAVE_END)
        self.assertEqual(air_drag.CAVE_END, cloth.CAVE)
        menu_cave, menu_code = next(w for w in pc_menu.code_writes() if w[0] == pc_menu.CAVE)
        self.assertLessEqual(menu_cave + len(menu_code), cave)
        constants = code[-4 * len(air_drag.CONSTANTS):]
        self.assertEqual(struct.unpack(f'<{len(air_drag.CONSTANTS)}f', constants),
                         tuple(struct.unpack('<f', struct.pack('<f', c))[0] for c in air_drag.CONSTANTS))

    def test_the_site_jumps_to_the_cave_and_the_cave_returns_after_it(self):
        (cave, code), (site, jump) = air_drag.code_writes()
        self.assertEqual(site, air_drag.SITE)
        self.assertEqual(len(jump), len(air_drag.SITE_BYTES))
        self.assertEqual(jump[0], 0xE9)
        self.assertEqual(site + 5 + struct.unpack_from('<i', jump, 1)[0], cave)
        self.assertTrue(all(b == 0x90 for b in jump[5:]))
        end = code.index(bytes.fromhex('4883c410')) + 4  # add rsp, 16, then the jump back
        self.assertEqual(code[end], 0xE9)
        self.assertEqual(cave + end + 5 + struct.unpack_from('<i', code, end + 1)[0], site + len(air_drag.SITE_BYTES))

    def test_the_drag_per_second_is_the_ps4s_at_any_frame_rate(self):
        drag, thirty, _zero, most = air_drag.CONSTANTS
        for fps in (30, 60, 90, 120, 144, 240):
            step = min(max(thirty / fps, 0.0), most)
            self.assertAlmostEqual((drag ** step) ** fps, drag ** 30, places=6)


if __name__ == '__main__':
    unittest.main()
