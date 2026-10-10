from paths import ROOT
import hashlib
import json
from pathlib import Path
import struct
import tempfile
import unittest
from unittest import mock

import menu_prompts
import menu_textures as mt


def fake_common(textures):
    """A PS4 TPF in a DCX holding `textures` (name -> (width, height)), BC7, data all zero but
    for one marker byte per texture."""
    names = list(textures)
    count = len(names)
    table = 16 + 0x24 * count
    name_data, name_at = b'', []
    for name in names:
        name_at.append(table + len(name_data))
        name_data += name.encode('utf-16-le') + b'\0\0'
    data_at = table + len(name_data)
    entries, blobs = b'', b''
    for i, name in enumerate(names):
        w, h = textures[name]
        size = max(w // 4, 8) * max(h // 4, 8) * mt.BLOCK  # whole 8x8-block tiles
        blob = bytearray(size)
        blob[0] = i + 1
        entries += struct.pack('<IIBBBBHHiiiii', data_at + len(blobs), size, 0x66, 0, 1, 0, w, h,
                               1, 0xD, name_at[i], 0, mt.BC7)
        blobs += bytes(blob)
    tpf = b'TPF\0' + struct.pack('<II', 0, count) + bytes([4, 0, 1, 0]) + entries + name_data + blobs
    header = bytearray(0x4c)
    header[0:4], header[0x28:0x2c] = b'DCX\0', b'DFLT'
    return mt.write_dcx(tpf, header)


class TextureTests(unittest.TestCase):
    def test_tile_order_is_a_permutation_with_morton_tiles(self):
        order = mt.tile_order(16, 8)
        self.assertEqual(sorted(order), list(range(128)))
        self.assertEqual(order[:4], [0, 1, 16, 17])  # x, then y, inside the first tile
        self.assertEqual(order[64], 8)  # the second tile starts 8 blocks to the right

class PromptTests(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        sizes = {slot: (32, 32) for slot in menu_prompts.ICONS}
        sizes[menu_prompts.ATLAS] = (1024, 512)
        sizes['MENU_Icon_00001'] = (64, 64)
        self.base = fake_common(sizes)

    def test_xbox_prompts_are_dommos_blocks_and_nothing_else_changes(self):
        tpf, _ = mt.read_dcx(menu_prompts.build_common(self.base, 'xbox'))
        old, _ = mt.read_dcx(self.base)
        textures = mt.textures(tpf)
        self.assertEqual(len(tpf), len(old))
        expected = mt.dds_blocks((menu_prompts.ASSETS / 'xbox' / 'KG_OK.dds').read_bytes())
        self.assertEqual(mt.blocks(tpf, textures['KG_OK']), expected)
        other = textures['MENU_Icon_00001']
        self.assertEqual(tpf[other['offset']:other['offset'] + other['size']],
                         old[other['offset']:other['offset'] + other['size']])
        # Only the strip's blocks of the atlas change.
        atlas = textures[menu_prompts.ATLAS]
        which, _ = mt.rect_blocks(atlas, *menu_prompts.ATLAS_STRIP)
        changed = [i for i, (a, b) in enumerate(zip(mt.blocks(old, atlas), mt.blocks(tpf, atlas))) if a != b]
        self.assertTrue(set(changed) <= set(which))

    def test_every_variant_has_all_its_textures(self):
        for variant in ('xbox', 'switch', 'keyboard'):
            folder = menu_prompts.ASSETS / variant
            for slot in menu_prompts.ICONS:
                self.assertEqual(len(mt.dds_blocks((folder / f'{slot}.dds').read_bytes())), 64, (variant, slot))
            self.assertEqual(len(mt.dds_blocks((folder / 'atlas.dds').read_bytes())), 23 * 8, variant)

    def test_keyboard_and_switch_prompts_differ_from_xbox(self):
        outputs = {v: menu_prompts.build_common(self.base, v) for v in ('xbox', 'switch', 'keyboard')}
        self.assertEqual(len(set(outputs.values())), 3)

    def test_gestures_patch_needs_the_games_file(self):
        original = b'0123456789'
        patch = dict(original_sha256=hashlib.sha256(original).hexdigest(),
                     result_sha256=hashlib.sha256(b'01ab456789!').hexdigest(),
                     edits=[[2, 2, '6162'], [10, 0, '21']])
        assets = self.root / 'assets'
        assets.mkdir()
        (assets / 'gesturetop.json').write_text(json.dumps(patch))
        with mock.patch.object(menu_prompts, 'ASSETS', assets):
            self.assertEqual(menu_prompts.apply_gestures(original), b'01ab456789!')
            with self.assertRaises(ValueError):
                menu_prompts.apply_gestures(b'another file')

    def test_command_caches_and_cleans(self):
        game = self.root / 'game' / 'dvdroot_ps4' / 'menu'
        game.mkdir(parents=True)
        (game / 'common.tpf.dcx').write_bytes(self.base)
        out = self.root / 'out'
        import contextlib
        import io
        for prompts in ('switch', 'switch', 'xbox'):
            printed = io.StringIO()
            with contextlib.redirect_stdout(printed):
                menu_prompts.main([str(self.root / 'game'), '--out', str(out), '--prompts', prompts])
            folder = Path(printed.getvalue().strip())
            self.assertTrue((folder / 'dvdroot_ps4/menu/common.tpf.dcx').is_file())
        self.assertEqual([p.name for p in (out / 'menu-prompts').iterdir()], [folder.name])
        printed = io.StringIO()
        with contextlib.redirect_stdout(printed):
            menu_prompts.main([str(self.root / 'game'), '--out', str(out)])
        self.assertEqual(printed.getvalue().strip(), '')


if __name__ == '__main__':
    unittest.main()
