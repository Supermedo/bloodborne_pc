# SPDX-License-Identifier: GPL-2.0-or-later
"""The port's own code in the code segment's last page: each script's cave apart from the others.

Two caves at the same addresses merge without a conflict in git, and one's code then runs the
other's bytes (Steady cloth at 0x50D9C00 under PC Settings' page table: a crash at the first cloth
step). Every script with a CAVE and code_writes() is checked, so a cave added later is too."""
import importlib
import re
import sys
import unittest
from pathlib import Path

SCRIPTS = Path(__file__).resolve().parents[1] / 'scripts'
sys.path.insert(0, str(SCRIPTS))
import quit_menu  # noqa: E402


def cave_writes():
    """(start, end, script) of every write into the last code page, per script."""
    found = []
    for path in sorted(SCRIPTS.glob('*.py')):
        text = path.read_text(encoding='utf-8')
        if not re.search(r'^CAVE\b', text, re.M) or '\ndef code_writes(' not in text:  # CAVE = or CAVE, CAVE_END =
            continue
        module = importlib.import_module(path.stem)
        for at, data in module.code_writes():
            if quit_menu.CAVE <= at < quit_menu.CAVE_END:
                found.append((at, at + len(data), path.stem))
    return found


class CodeCavesTest(unittest.TestCase):
    def test_every_cave_lies_in_the_last_code_page(self):
        for start, end, script in cave_writes():
            self.assertLessEqual(end, quit_menu.CAVE_END, script)

    def test_no_two_scripts_write_the_same_bytes(self):
        writes = cave_writes()
        self.assertGreaterEqual(len({script for _, _, script in writes}), 4)
        for start, end, script in writes:
            for other_start, other_end, other in writes:
                if other != script:
                    self.assertFalse(start < other_end and other_start < end,
                                     f'{script} {start:#x}-{end:#x} overlaps {other} {other_start:#x}-{other_end:#x}')


if __name__ == '__main__':
    unittest.main()
