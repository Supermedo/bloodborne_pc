#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Rebuilds assets/menu_prompts from Dommo's Nexus mods (Bloodborne mod 30), keeping only what
the mods drew, never the game's own textures:

    python3 tools/extract_prompt_assets.py <game>/dvdroot_ps4/menu \\
        "DS3 Style - Modern Xbox prompts-*.zip" "DS3 Style - Nintendo Switch prompts-*.zip" \\
        "Stripped down gesture menu-*.zip"

- <variant>/KG_*.dds: the 20 button icons, BC7 blocks exactly as the mod has them (row-major).
- <variant>/atlas.dds: the prompt strip of MENU_Common_00091, BC7; the mod's atlas is twice the
  game's size, so the strip is scaled down to the game's and encoded again.
- gesturetop.json: the edits that turn the game's menu/gesturetop.gfx into the mod's.
The game folder's files are only compared against, to cut out the mods' changes."""
import difflib
import hashlib
import json
from pathlib import Path
import sys
import zipfile

from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / 'scripts'))
import menu_prompts  # noqa: E402
import menu_textures as mt  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent / 'assets' / 'menu_prompts'
VARIANTS = {'one_ds3/menu/common.tpf.dcx': 'xbox', 'switch_ds3/menu/common.tpf.dcx': 'switch'}


def main(menu, *archives):
    menu = Path(menu)
    for archive in archives:
        with zipfile.ZipFile(archive) as z:
            names = {n.casefold(): n for n in z.namelist()}
            for member, variant in VARIANTS.items():
                if member not in names:
                    continue
                tpf, _ = mt.read_dcx(z.read(names[member]))
                textures = mt.textures(tpf)
                out = ROOT / variant
                out.mkdir(parents=True, exist_ok=True)
                for slot in menu_prompts.ICONS:
                    texture = textures[slot]
                    (out / f'{slot}.dds').write_bytes(
                        mt.dds(texture['width'], texture['height'], mt.blocks(tpf, texture)))
                atlas = textures[menu_prompts.ATLAS]
                scale = atlas['width'] // menu_prompts.ATLAS_WIDTH
                x, y, w, h = menu_prompts.ATLAS_STRIP
                image = mt.decode(atlas['width'], atlas['height'], mt.blocks(tpf, atlas))
                strip = image.crop((x * scale, y * scale, (x + w) * scale, (y + h) * scale))
                strip = strip.resize((w, h), Image.LANCZOS)
                (out / 'atlas.dds').write_bytes(mt.dds(w, h, mt.encode(strip)))
                print(f'{variant}: {len(menu_prompts.ICONS)} icons and the atlas strip')
            if 'gesturetop/menu/gesturetop.gfx' in names:
                original = (menu / 'gesturetop.gfx').read_bytes()
                modded = z.read(names['gesturetop/menu/gesturetop.gfx'])
                matcher = difflib.SequenceMatcher(None, original, modded, autojunk=False)
                edits = [[i1, i2 - i1, modded[j1:j2].hex()]
                         for op, i1, i2, j1, j2 in matcher.get_opcodes() if op != 'equal']
                patch = dict(original_sha256=hashlib.sha256(original).hexdigest(),
                             result_sha256=hashlib.sha256(modded).hexdigest(), edits=edits)
                ROOT.mkdir(parents=True, exist_ok=True)
                (ROOT / 'gesturetop.json').write_text(json.dumps(patch, indent=1) + '\n')
                print(f'gesturetop: {len(edits)} edits')


if __name__ == '__main__':
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    main(*sys.argv[1:])
