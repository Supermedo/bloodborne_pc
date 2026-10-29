from paths import ROOT
import re
import struct
import sys
import unittest

sys.path.insert(0, str(ROOT / 'scripts'))
import pc_menu  # noqa: E402
import quit_menu  # noqa: E402
import rebirth  # noqa: E402


class PcMenuTests(unittest.TestCase):
    def test_code_fits_its_page_and_leaves_the_other_menus_alone(self):
        writes = dict(pc_menu.code_writes())
        cave = writes[pc_menu.CAVE]
        self.assertLessEqual(pc_menu.CAVE + len(cave), quit_menu.CAVE_END)
        others = quit_menu.code_writes() + rebirth.code_writes()
        for offset, data in writes.items():
            for other, other_data in others:
                self.assertFalse(offset < other + len(other_data) and other < offset + len(data),
                                 f'{offset:#x} overlaps {other:#x}')
        for offset, original in pc_menu.ORIGINAL.items():
            self.assertEqual(len(writes[offset]), len(bytes.fromhex(original)))

    def test_page_table_holds_every_page_and_the_opener_counts_them(self):
        writes = dict(pc_menu.code_writes())
        cave = writes[pc_menu.CAVE]
        at = cave.index(pc_menu.TABLE_MAGIC) + 8
        count = len(pc_menu.GAME_PAGES) + 1
        offsets = struct.unpack_from(f'<{count}Q', cave, at)
        self.assertEqual(list(offsets[:6]), pc_menu.GAME_PAGES)
        start = offsets[6] - pc_menu.CAVE
        self.assertEqual(cave[start:start + len(pc_menu.PAGE) + 1], pc_menu.PAGE.encode() + b'\0')
        self.assertEqual(writes[pc_menu.PAGE_COUNT], bytes.fromhex('4883fa') + bytes([count]))

    def test_one_page_with_a_title_per_line(self):
        """The three System lines share one page (each page's rows cost menu memory at every
        settings open), with the three titles as named instances the port switches."""
        source = (ROOT / 'gpu/shim/bbport_pc_menu.cpp').read_text(encoding='utf-8')
        self.assertIn(f'PageName = "{pc_menu.PAGE}"', source)
        titles = re.search(r'TitleNames\[PageCount\] = \{(.*?)\}', source).group(1)
        self.assertEqual(re.findall(r'"(\w+)"', titles),
                         [f'{pc_menu.TITLE_NAME}{n}' for n in range(len(pc_menu.TITLES))])
        self.assertEqual(pc_menu.TITLES, [119001, 119002, 119003])  # the System lines' names

    def test_a_name_goes_where_the_format_puts_it(self):
        # PlaceObject3 with a character and a matrix (no scale or rotation, translation 0/0)
        body = bytearray([0x06, 0x00, 117, 0, 137, 0, 0x00])
        out = pc_menu.named(70, body, 'PcTitle_0')
        self.assertEqual(out[0], 0x26)
        self.assertEqual(quit_menu.swf.place_fields(bytes(out), 0, 70), (137, (6, 7), 'PcTitle_0'))
        with self.assertRaises(ValueError):
            pc_menu.named(70, out, 'again')

    def test_texts_match_the_port(self):
        source = (ROOT / 'gpu/shim/bbport_pc_menu.cpp').read_text(encoding='utf-8')
        names = pc_menu.texts()[200]
        lines = re.search(r'enum Line : int \{(.*?)LineCount', source, re.S).group(1)
        self.assertEqual(len([n for n in lines.replace('\n', ' ').split(',') if n.strip()]),
                         len(pc_menu.LINES))
        for constant, value in re.findall(r'(\w+_TEXT) = (\d+)', source):
            self.assertIn(int(value), names, constant)


if __name__ == '__main__':
    unittest.main()
