# SPDX-License-Identifier: GPL-2.0-or-later
import struct
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
import cloth  # noqa: E402
import cloth_time  # noqa: E402
import effect_time  # noqa: E402


def rel32(at, code, offset):
    """The target of a rel32 operand at `offset` in `code` placed at `at`."""
    return at + offset + 4 + struct.unpack_from('<i', code, offset)[0]


def rel8(at, code, offset):
    return at + offset + 1 + struct.unpack_from('<b', code, offset)[0]


class EffectTimeTest(unittest.TestCase):
    def setUp(self):
        (self.site, self.site_code), (self.cave, self.cave_code), (self.branch, self.branch_code) = \
            effect_time.code_writes()

    def test_it_writes_uncap_fps_sites_only_and_the_cloth_caves_rest(self):
        # The same bytes Uncap FPS++ writes there (the frame rate patches' fingerprint covers them).
        self.assertEqual((self.site, len(self.site_code)), (0x1F77CEA, 0x26))
        self.assertEqual((self.branch, len(self.branch_code)), (0x1F77D48, 10))  # up to the call
        cloth_cave = cloth_time.code_writes()[0]
        self.assertEqual(self.cave, cloth_cave[0] + len(cloth_cave[1]))
        self.assertEqual(self.cave + len(self.cave_code), 0x50D98C0)
        self.assertEqual(cloth_time.CAVE_END, self.cave)

    def test_the_accumulator_follows_the_other_state_in_the_data_tail(self):
        self.assertEqual(effect_time.ACCUMULATED, cloth_time.STEADY + 8)
        self.assertGreaterEqual(cloth_time.STEADY, cloth.STATE + 0x18)

    def test_the_zero_frame_flag_still_gives_zero(self):
        self.assertEqual(self.branch_code[0], 0xEB)                     # jmp to the site's jne
        target = rel8(self.branch, self.branch_code, 1)
        self.assertEqual(target, self.site + 2)
        self.assertEqual(self.site_code[2], 0x75)                       # jne: xmm0 is still 0 there
        self.assertEqual(rel8(self.site, self.site_code, 3), effect_time.CALL)

    def test_the_site_reads_the_frame_time_and_adds_the_accumulated(self):
        code = self.site_code
        self.assertEqual(code[:2], bytes.fromhex('eb24'))                 # past the assert block, as Uncap
        self.assertEqual(rel32(self.site, code, 7), effect_time.FRAME_TIMER)
        self.assertEqual(code[11:19], bytes.fromhex('c5fa108964020000'))  # vmovss xmm1, [rcx+0x264]
        self.assertEqual(rel32(self.site, code, 23), effect_time.ACCUMULATED)
        self.assertEqual(code[27], 0xE9)
        self.assertEqual(rel32(self.site, code, 28), self.cave)
        self.assertEqual(code[32:], b'\x90' * 6)

    def test_the_cave_passes_whole_steps_and_keeps_the_rest(self):
        code = self.cave_code
        step = self.cave + len(code) - 4
        self.assertEqual(struct.unpack('<f', code[-4:])[0], struct.unpack('<f', struct.pack('<f', 1 / 60))[0])
        self.assertEqual(rel32(self.cave, code, 4), step)                # vdivss xmm0, xmm1, [step]
        self.assertEqual(code[8:14], bytes.fromhex('c4e3790ac009'))      # vroundss ..., 9: down
        self.assertEqual(rel32(self.cave, code, 18), step)               # vmulss xmm0, xmm0, [step]
        self.assertEqual(rel32(self.cave, code, 30), effect_time.ACCUMULATED)
        self.assertEqual(code[34], 0xE9)
        self.assertEqual(rel32(self.cave, code, 35), self.branch + 2)    # back before the call

    def test_without_a_whole_step_the_update_is_not_called(self):
        # The update adds to its particles on every call, whatever the step: called more often than
        # 60 times a second they piled up even with whole steps (0 in between).
        code = self.branch_code
        self.assertEqual(code[2:8], bytes.fromhex('c5f97ec185c9'))      # vmovd ecx, xmm0; test ecx, ecx
        self.assertEqual(code[8], 0x74)                                 # jz past the call
        self.assertEqual(rel8(self.branch, code, 9), effect_time.AFTER)
        self.assertEqual(self.branch + len(code), effect_time.CALL)

    def test_with_90_fps_the_presets_fixed_step_is_accumulated(self):
        (site, code), (cave, cave_code), branch = effect_time.code_writes(measured=False)
        self.assertEqual(code[4:8], bytes.fromhex('c5fa100d'))           # vmovss xmm1, [fixed step]
        self.assertEqual(rel32(site, code, 8), effect_time.FIXED_STEP)
        self.assertEqual(rel32(site, code, 16), effect_time.ACCUMULATED)
        self.assertEqual(code[20], 0xE9)
        self.assertEqual(rel32(site, code, 21), cave)
        self.assertEqual((cave, cave_code), (self.cave, self.cave_code))  # the same steps, the same skip
        self.assertEqual(branch, (self.branch, self.branch_code))

    def test_the_arithmetic(self):
        # What the cave computes, frame by frame: at 144 FPS the steps come every 2-3 frames, and
        # over a second they add up to the real time within one step.
        step = struct.unpack('<f', struct.pack('<f', 1 / 60))[0]
        for fps in (30, 59.3, 60, 75, 120, 144, 240):
            acc, given = 0.0, 0.0
            for _ in range(int(fps * 10)):
                acc += 1 / fps
                passed = int(acc / step) * step
                self.assertGreaterEqual(passed, 0)
                acc -= passed
                given += passed
                self.assertTrue(passed == 0 or passed >= step * 0.999)
            self.assertAlmostEqual(given, 10.0, delta=step * 1.01)


if __name__ == '__main__':
    unittest.main()
