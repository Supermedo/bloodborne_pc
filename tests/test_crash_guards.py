# SPDX-License-Identifier: GPL-2.0-or-later
import struct
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
import crash_guards  # noqa: E402
import quit_menu  # noqa: E402


def target(code, at, length):
    """Where the rel32 jump ending at `at + length` in `code` (placed at its offset) goes."""
    return code[0] + at + length + struct.unpack_from('<i', code[1], at + length - 4)[0]


class CrashGuardsTest(unittest.TestCase):
    def test_the_guard_sits_between_quit_to_desktop_and_rebirth(self):
        (cave, code), _site = crash_guards.code_writes()
        quit_code = next(data for at, data in quit_menu.code_writes() if at == quit_menu.CAVE)
        self.assertGreaterEqual(cave, quit_menu.CAVE + len(quit_code))
        self.assertLessEqual(cave + len(code), crash_guards.CAVE_END)

    def test_no_data_goes_where_an_unknown_event_goes_and_data_carries_on(self):
        (cave, code), (site, jump) = crash_guards.code_writes()
        self.assertEqual(code[:3], bytes.fromhex('4d85e4'))                      # test r12, r12
        self.assertEqual(target((cave, code), 3, 6), crash_guards.EVENT_UNKNOWN)  # jz
        self.assertEqual(code[9:20], crash_guards.EVENT_CASE_BYTES)
        self.assertEqual(target((cave, code), 20, 5), crash_guards.EVENT_CASE + 11)
        self.assertEqual(len(jump), len(crash_guards.EVENT_CASE_BYTES))
        self.assertEqual(target((site, jump), 0, 5), cave)

    def test_other_code_at_the_site_is_refused(self):
        crash_guards.check_original(lambda start, end: crash_guards.EVENT_CASE_BYTES)
        with self.assertRaises(ValueError):
            crash_guards.check_original(lambda start, end: b'\x90' * (end - start))


if __name__ == '__main__':
    unittest.main()
