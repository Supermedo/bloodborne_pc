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


def expected_image(overrides=None):
    """Build a reader answering every range validated by check_original() and check_ssr_original()."""
    chunks = {
        crash_guards.EVENT_CASE: crash_guards.EVENT_CASE_BYTES,
        crash_guards.TEX_QUERY_SITE: crash_guards.TEX_QUERY_ORIGINAL,
        crash_guards.VISCERAL_SITE: crash_guards.VISCERAL_SITE_BYTES,
        crash_guards.GNM_COPY_SITE: crash_guards.GNM_COPY_BYTES + bytes.fromhex('415641554154'),
        0x21688A2: bytes.fromhex('410fb65e3b'),
        crash_guards.GNM_COPY_CAVE: bytes(crash_guards.GNM_COPY_CAVE_END - crash_guards.GNM_COPY_CAVE),
        0x21C2128: bytes.fromhex('4d85ff7445'),
        0x21C23E0: bytes.fromhex('4c89bde8feffff'),
        0x21C2430: bytes.fromhex('742f'),
        0x21C2458: bytes.fromhex('7407'),
        crash_guards.SSR_SITE: crash_guards.SSR_ORIGINAL,
        0x21C24D5: bytes.fromhex('4c8bbde8feffff498b4748'),
        0x21C26E3: bytes.fromhex('0f84e7010000'),
        crash_guards.SSR_LOOP_EXIT: bytes.fromhex('4d85f67421'),
        0x21C295D: bytes.fromhex('b3014d85ff7418'),
    }
    if overrides:
        chunks.update(overrides)

    def read(start, end):
        if start in chunks and len(chunks[start]) == end - start:
            return chunks[start]
        raise KeyError(f'Unexpected image query [{start:#x}..{end:#x})')

    return read


class CrashGuardsTest(unittest.TestCase):
    def test_caves_respect_quit_rebirth_and_pc_menu_boundaries(self):
        writes = dict(crash_guards.code_writes())
        ssr = dict(crash_guards.ssr_code_writes())
        quit_code = next(data for at, data in quit_menu.code_writes() if at == quit_menu.CAVE)

        cave1 = (crash_guards.CAVE, writes[crash_guards.CAVE])
        self.assertGreaterEqual(cave1[0], quit_menu.CAVE + len(quit_code))

        cave2_off = cave1[0] + len(cave1[1])
        self.assertIn(cave2_off, writes)
        self.assertLessEqual(cave2_off + len(writes[cave2_off]), crash_guards.SSR_CAVE)

        ssr_cave = (crash_guards.SSR_CAVE, ssr[crash_guards.SSR_CAVE])
        self.assertLessEqual(ssr_cave[0] + len(ssr_cave[1]), crash_guards.CAVE_END)

        gnm_cave = (crash_guards.GNM_COPY_CAVE, writes[crash_guards.GNM_COPY_CAVE])
        self.assertGreaterEqual(gnm_cave[0], quit_menu.CODE_END)
        self.assertLessEqual(gnm_cave[0] + len(gnm_cave[1]), crash_guards.GNM_COPY_CAVE_END)

        abort = (crash_guards.SSR_ABORT_CAVE, ssr[crash_guards.SSR_ABORT_CAVE])
        self.assertGreaterEqual(abort[0], 0x50D9C48)
        self.assertLessEqual(abort[0] + len(abort[1]), crash_guards.SSR_ABORT_CAVE_END)

    def test_online_event_and_visceral_guards_and_no_xor_ecx_bug(self):
        writes = dict(crash_guards.code_writes())
        cave1 = (crash_guards.CAVE, writes[crash_guards.CAVE])
        site1 = (crash_guards.EVENT_CASE, writes[crash_guards.EVENT_CASE])
        self.assertEqual(cave1[1][:3], bytes.fromhex('4d85e4'))
        self.assertEqual(target(cave1, 3, 6), crash_guards.EVENT_UNKNOWN)
        self.assertEqual(cave1[1][9:20], crash_guards.EVENT_CASE_BYTES)
        self.assertEqual(target(cave1, 20, 5), crash_guards.EVENT_CASE + len(crash_guards.EVENT_CASE_BYTES))
        self.assertEqual(len(site1[1]), len(crash_guards.EVENT_CASE_BYTES))
        self.assertEqual(target(site1, 0, 5), cave1[0])

        cave2_off = cave1[0] + len(cave1[1])
        cave2 = (cave2_off, writes[cave2_off])
        site2 = (crash_guards.VISCERAL_SITE, writes[crash_guards.VISCERAL_SITE])
        self.assertNotIn(b'\x31\xc9', cave2[1])
        self.assertEqual(cave2[1][:11], bytes.fromhex('488b094885c974038b4930'))
        self.assertEqual(target(cave2, 11, 5), crash_guards.VISCERAL_RESUME)
        self.assertEqual(len(site2[1]), len(crash_guards.VISCERAL_SITE_BYTES))
        self.assertEqual(target(site2, 0, 5), cave2[0])

    def test_texture_getter_guards_keep_adjacent_methods_untouched(self):
        writes = dict(crash_guards.code_writes())
        self.assertEqual(writes[0x219F860], bytes.fromhex('488b47404885c074050fb6404ac3c390'))
        self.assertEqual(writes[0x219F870], bytes.fromhex('488b47404885c07402eb1bc30f1f4000'))
        self.assertEqual(writes[0x219F880], bytes.fromhex('488b47404885c07402eb19c30f1f4000'))
        self.assertEqual(writes[0x219F896], bytes.fromhex('0fb740444088f1d3e8c3'))
        self.assertEqual(writes[0x219F8A4], bytes.fromhex('0fb740464088f1d3e8c36690'))
        for at, data in crash_guards.TEX_QUERY_WRITES:
            end = at + len(data)
            self.assertFalse(at < 0x219F896 and 0x219F890 < end)
            self.assertFalse(at < 0x219F8A4 and 0x219F8A0 < end)

    def test_gnm_copy_and_ssr_loop_abort_guards(self):
        writes = dict(crash_guards.code_writes())
        ssr = dict(crash_guards.ssr_code_writes())

        cave3 = (crash_guards.GNM_COPY_CAVE, writes[crash_guards.GNM_COPY_CAVE])
        site3 = (crash_guards.GNM_COPY_SITE, writes[crash_guards.GNM_COPY_SITE])
        self.assertEqual(cave3[1][:19], bytes.fromhex('4885ff740d4885d2740848837c2410007501c3'))
        self.assertEqual(cave3[1][19:25], crash_guards.GNM_COPY_BYTES)
        self.assertEqual(target(cave3, 25, 5), crash_guards.GNM_COPY_RESUME)
        self.assertEqual(target(site3, 0, 5), cave3[0])

        ssr_cave = (crash_guards.SSR_CAVE, ssr[crash_guards.SSR_CAVE])
        ssr_abort = (crash_guards.SSR_ABORT_CAVE, ssr[crash_guards.SSR_ABORT_CAVE])
        ssr_site = (crash_guards.SSR_SITE, ssr[crash_guards.SSR_SITE])
        self.assertEqual(ssr_cave[1][:3], bytes.fromhex('4d85f6'))
        self.assertEqual(target(ssr_cave, 3, 6), ssr_abort[0])
        self.assertEqual(ssr_cave[1][9:14], crash_guards.SSR_SITE_BYTES)
        self.assertEqual(target(ssr_cave, 14, 5), crash_guards.SSR_RESUME)
        self.assertEqual(ssr_abort[1][:7], bytes.fromhex('4c8bbde8feffff'))
        self.assertEqual(target(ssr_abort, 7, 5), crash_guards.SSR_LOOP_EXIT)
        self.assertEqual(len(ssr_site[1]), len(crash_guards.SSR_SITE_BYTES))
        self.assertEqual(target(ssr_site, 0, 5), ssr_cave[0])

    def test_original_byte_checks_accept_0109_and_refuse_mismatches(self):
        reader = expected_image()
        crash_guards.check_original(reader)
        crash_guards.check_ssr_original(reader)

        for address in (
            crash_guards.EVENT_CASE,
            crash_guards.TEX_QUERY_SITE,
            crash_guards.VISCERAL_SITE,
            crash_guards.GNM_COPY_SITE,
            0x21688A2,
            crash_guards.GNM_COPY_CAVE,
        ):
            size = {
                crash_guards.EVENT_CASE: len(crash_guards.EVENT_CASE_BYTES),
                crash_guards.TEX_QUERY_SITE: len(crash_guards.TEX_QUERY_ORIGINAL),
                crash_guards.VISCERAL_SITE: len(crash_guards.VISCERAL_SITE_BYTES),
                crash_guards.GNM_COPY_SITE: 12,
                0x21688A2: 5,
                crash_guards.GNM_COPY_CAVE: crash_guards.GNM_COPY_CAVE_END - crash_guards.GNM_COPY_CAVE,
            }[address]
            with self.assertRaises(ValueError):
                crash_guards.check_original(expected_image({address: b'\x90' * size}))

        with self.assertRaises(ValueError):
            crash_guards.check_ssr_original(
                expected_image({crash_guards.SSR_SITE: b'\x90' * len(crash_guards.SSR_ORIGINAL)})
            )


if __name__ == '__main__':
    unittest.main()
