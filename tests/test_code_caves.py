# SPDX-License-Identifier: GPL-2.0-or-later
"""The port's own code in the code segment's last page: each script's cave apart from the others.

Two caves at the same addresses merge without a conflict in git, and one's code then runs the
other's bytes (Steady cloth at 0x50D9C00 under PC Settings' page table: a crash at the first cloth
step). Every script with a CAVE and code_writes() is checked, plus pc_menu's runtime table slot
and ssr_code_writes(), so any overlap with the 32-byte gap before quit_menu.CAVE or the runtime
PC Settings page table fails tests.
"""
import importlib
import re
import sys
import unittest
from pathlib import Path

SCRIPTS = Path(__file__).resolve().parents[1] / 'scripts'
sys.path.insert(0, str(SCRIPTS))
import quit_menu  # noqa: E402


def cave_writes():
    """(start, end, script) of every write in the last code page (including pre-CAVE gap and runtime slots)."""
    found = []
    for path in sorted(SCRIPTS.glob('*.py')):
        text = path.read_text(encoding='utf-8')
        if not re.search(r'^CAVE\b', text, re.M) or '\ndef code_writes(' not in text:  # CAVE = or CAVE, CAVE_END =
            continue
        module = importlib.import_module(path.stem)
        all_writes = list(module.code_writes())
        if hasattr(module, 'ssr_code_writes'):
            all_writes.extend(module.ssr_code_writes())
        for at, data in all_writes:
            if quit_menu.CODE_END <= at < quit_menu.CAVE_END:
                found.append((at, at + len(data), path.stem))
                if path.stem == 'pc_menu' and at == module.CAVE:
                    # bbgpu_pc_menu_prepare fills the 7-entry page table right after pc_menu.CAVE.
                    end = at + len(data)
                    found.append((end, end + 0x20, 'pc_menu_runtime'))
    return found


class CodeCavesTest(unittest.TestCase):
    def test_every_cave_lies_in_the_last_code_page(self):
        for start, end, script in cave_writes():
            self.assertGreaterEqual(start, quit_menu.CODE_END, script)
            self.assertLessEqual(end, quit_menu.CAVE_END, script)

    def test_no_two_scripts_write_the_same_bytes(self):
        writes = cave_writes()
        self.assertGreaterEqual(len({script for _, _, script in writes}), 4)
        for i, (start, end, script) in enumerate(writes):
            for j, (other_start, other_end, other) in enumerate(writes):
                if i != j:
                    self.assertFalse(start < other_end and other_start < end,
                                     f'{script} {start:#x}-{end:#x} overlaps {other} {other_start:#x}-{other_end:#x}')


if __name__ == '__main__':
    unittest.main()
