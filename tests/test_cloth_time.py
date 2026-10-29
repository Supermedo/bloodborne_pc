# SPDX-License-Identifier: GPL-2.0-or-later
import struct
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
import cloth  # noqa: E402
import cloth_time  # noqa: E402
import crash_guards  # noqa: E402
import pc_menu  # noqa: E402
import quit_menu  # noqa: E402
import rebirth  # noqa: E402


def end_of(writes, start):
    at, data = next(w for w in writes if w[0] == start)
    return at + len(data)


def steady(times, slower=cloth_time.SLOWER, faster=cloth_time.FASTER, longest=cloth_time.LONGEST):
    """The cave's rule, frame by frame."""
    out, before = [], 0.0
    for time in times:
        if before <= 0:
            before = time
        before = min(max(time, before * slower), before * faster, longest)
        out.append(before)
    return out


class SteadyClothTimeTest(unittest.TestCase):
    def test_the_code_fits_the_gaps_after_quit_to_desktop_and_rebirth(self):
        (cave, code), (more, more_code), _site = cloth_time.code_writes()
        self.assertEqual((cave, cave + len(code)), (cloth_time.CAVE, cloth_time.CAVE_END))
        self.assertEqual((more, more + len(more_code)), (cloth_time.MORE, cloth_time.MORE_END))
        self.assertLessEqual(end_of(quit_menu.code_writes(), quit_menu.CAVE), cave)
        self.assertLessEqual(cloth_time.CAVE_END, crash_guards.CAVE)
        self.assertLessEqual(end_of(rebirth.code_writes(), rebirth.CAVE), more)
        self.assertLessEqual(cloth_time.MORE_END, pc_menu.CAVE)
        constants = more_code[-4 * len(cloth_time.CONSTANTS):]
        self.assertEqual(struct.unpack(f'<{len(cloth_time.CONSTANTS)}f', constants),
                         tuple(struct.unpack('<f', struct.pack('<f', c))[0] for c in cloth_time.CONSTANTS))

    def test_the_site_jumps_to_the_cave_and_the_code_returns_after_it(self):
        (cave, code), (more, more_code), (site, jump) = cloth_time.code_writes()
        self.assertEqual(site, cloth_time.SITE)
        self.assertEqual(len(jump), len(cloth_time.SITE_BYTES))
        self.assertEqual(site + 5 + struct.unpack_from('<i', jump, 1)[0], cave)
        self.assertEqual(code[:5], cloth_time.SITE_BYTES)  # the time as given is saved first
        end = more_code.index(bytes.fromhex('5a59')) + 2  # pop rdx; pop rcx, then the jump back
        self.assertEqual(more_code[end], 0xE9)
        self.assertEqual(more + end + 5 + struct.unpack_from('<i', more_code, end + 1)[0], cloth_time.RETURN)

    def test_its_state_is_past_cloths(self):
        self.assertGreaterEqual(cloth_time.STEADY, cloth.SOFTEN + 4)
        self.assertEqual(cloth_time.STEADY % 8, 0)  # lock cmpxchg on 8 aligned bytes
        self.assertEqual(cloth_time.STEADY >> 12, cloth.STATE >> 12)  # the same page of the data tail

    def test_a_stall_is_no_kick(self):
        # Measured standing still at 120 FPS: 8.3 ms frames, a 94 ms stall, short frames after it.
        times = [1 / 120] * 10 + [0.094, 0.0042, 0.0042, 0.006] + [1 / 120] * 10
        out = steady(times)
        for before, after in zip(out, out[1:]):
            self.assertLessEqual(after / before, cloth_time.FASTER + 1e-9)
            self.assertGreaterEqual(after / before, cloth_time.SLOWER - 1e-9)
        self.assertAlmostEqual(out[-1], 1 / 120)

    def test_a_lasting_frame_rate_is_followed(self):
        for fps in (30, 60, 90, 120, 144, 240):
            self.assertAlmostEqual(steady([1 / 120] * 5 + [1 / fps] * 60)[-1], 1 / fps)
        self.assertAlmostEqual(steady([1 / 20] * 20)[-1], 1 / 30)  # never more than the PS4's frame

    def test_other_code_at_the_site_is_refused(self):
        cloth_time.check_original(lambda start, end: cloth_time.SITE_BYTES)
        with self.assertRaises(ValueError):
            cloth_time.check_original(lambda start, end: b'\x90' * (end - start))


if __name__ == '__main__':
    unittest.main()
