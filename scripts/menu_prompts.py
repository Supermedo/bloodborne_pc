#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Button prompts and the gestures menu for this launch, made from the player's own game files.

    menu_prompts.py <game> --out <out> --prompts playstation|xbox|switch|keyboard [--gestures 0|1]

prints the folders (os.pathsep-separated) that mods.py lays under the mods (--ui-mods):
- Xbox and Switch: the icons of Dommo's "DS3 Style" prompts (Nexus Mods, Bloodborne mod 30) from
  assets/menu_prompts, put into the game's menu/common.tpf.dcx.
- keyboard: icons of the keyboard keys (tools/make_keyboard_prompts.py draws them).
- --gestures 1: Dommo's "Stripped down gesture menu" (no motion-control icons), as edits of the
  game's menu/gesturetop.gfx.
Only the prompt textures change; every other texture is the game's, untouched. Results are
kept in <out>/menu-prompts, keyed by their inputs (game file, these scripts and assets), so a
launch with the same choice is instant.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import sys

ROOT = Path(__file__).resolve().parent.parent
ASSETS = ROOT / 'assets' / 'menu_prompts'

# The prompt textures of menu/common.tpf.dcx and the PS4 input each shows (KG_OK is circle and
# KG_Cancel cross, Japanese style); clusters list (up, down, left, right).
ICONS = {
    'KG_OK': 'circle', 'KG_Cancel': 'cross', 'KG_R_U': 'triangle', 'KG_R_L': 'square',
    'KG_L1': 'l1', 'KG_R1': 'r1', 'KG_L2': 'l2', 'KG_R2': 'r2', 'KG_L3': 'l3', 'KG_R3': 'r3',
    'KG_Start': 'options', 'KG_TP_L': 'touchpad', 'KG_TP_R': 'touchpad_right',
    'KG_LS': ('move_up', 'move_down', 'move_left', 'move_right'),
    'KG_RS': ('look_up', 'look_down', 'look_left', 'look_right'),
    'KG_L_U': 'up', 'KG_L_D': 'down', 'KG_L_LR': (None, None, 'left', 'right'),
    'KG_L_UD': ('up', 'down', None, None), 'KG_L_UDLR': ('up', 'down', 'left', 'right'),
}
# The shared UI atlas also has big circle, cross and L3 prompts (26x26 at these spots); the
# block-aligned strip around them is all that is replaced in it.
ATLAS = 'MENU_Common_00091'
ATLAS_WIDTH = 1024
ATLAS_STRIP = (660, 120, 92, 32)
ATLAS_GLYPHS = {'KG_OK': (661, 123), 'KG_Cancel': (694, 123), 'KG_L3': (726, 123)}
PROMPTS = ('playstation', 'xbox', 'switch', 'keyboard')


def build_common(base, prompts):
    """The game's menu/common.tpf.dcx (bytes `base`) with `prompts` (xbox, switch, keyboard)."""
    import menu_textures as mt
    tpf, header = mt.read_dcx(base)
    tpf = bytearray(tpf)
    textures = mt.textures(tpf)
    folder = ASSETS / prompts
    for slot in ICONS:
        texture = textures[slot]
        new = mt.dds_blocks((folder / f'{slot}.dds').read_bytes())
        if len(new) != (texture['width'] // 4) * (texture['height'] // 4):
            raise ValueError(f'{slot}: unexpected size')
        mt.put_blocks(tpf, texture, dict(enumerate(new)))
    atlas = textures[ATLAS]
    if atlas['width'] != ATLAS_WIDTH:
        raise ValueError(f'{ATLAS} is {atlas["width"]} wide, expected {ATLAS_WIDTH}')
    strip = mt.dds_blocks((folder / 'atlas.dds').read_bytes())
    which, rect = mt.rect_blocks(atlas, *ATLAS_STRIP)
    if rect != ATLAS_STRIP or len(which) != len(strip):
        raise ValueError('unexpected atlas strip')
    mt.put_blocks(tpf, atlas, dict(zip(which, strip)))
    return mt.write_dcx(bytes(tpf), header)


def apply_gestures(original):
    """Dommo's gestures menu: the edits of assets/menu_prompts/gesturetop.json."""
    patch = json.loads((ASSETS / 'gesturetop.json').read_text())
    if hashlib.sha256(original).hexdigest() != patch['original_sha256']:
        raise ValueError('menu/gesturetop.gfx is not the one the gestures patch was made for')
    out, at = bytearray(), 0
    for offset, length, data in patch['edits']:
        out += original[at:offset] + bytes.fromhex(data)
        at = offset + length
    out += original[at:]
    if hashlib.sha256(out).hexdigest() != patch['result_sha256']:
        raise ValueError('gestures patch gave an unexpected result')
    return bytes(out)


def game_file(game, relative):
    """A file under the game's dvdroot_ps4/menu, whatever the case of its name."""
    folder = Path(game) / 'dvdroot_ps4'
    for part in ('menu', relative):
        match = [p for p in folder.iterdir() if p.name.casefold() == part] if folder.is_dir() else []
        if not match:
            raise FileNotFoundError(f'{relative} not found in the game')
        folder = match[0]
    return folder


def sources(*extra):
    """A hash of what the results are made from: these scripts and the files used."""
    digest = hashlib.sha256()
    for path in (Path(__file__), Path(__file__).with_name('menu_textures.py'), *extra):
        digest.update(path.read_bytes())
    return digest.hexdigest()


def cached(out, key, relative, make):
    """<out>/menu-prompts/<key>/dvdroot_ps4/menu/<relative>, made once by make()."""
    folder = Path(out) / 'menu-prompts' / key
    target = folder / 'dvdroot_ps4' / 'menu' / relative
    if not target.is_file():
        target.parent.mkdir(parents=True, exist_ok=True)
        partial = target.with_name(target.name + '.part')
        partial.write_bytes(make())
        os.replace(partial, target)
    return folder


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('game', type=Path)
    parser.add_argument('--out', required=True, type=Path)
    parser.add_argument('--prompts', choices=PROMPTS, default='playstation')
    parser.add_argument('--gestures', choices=('0', '1'), default='0')
    args = parser.parse_args(argv)
    folders, used = [], set()
    if args.prompts != 'playstation':
        base = game_file(args.game, 'common.tpf.dcx')
        stat = base.stat()
        key = hashlib.sha256(json.dumps([sources(*sorted((ASSETS / args.prompts).glob('*.dds'))),
                                         args.prompts, str(base), stat.st_size, stat.st_mtime_ns])
                             .encode()).hexdigest()[:16]
        folders.append(cached(args.out, f'prompts-{key}', 'common.tpf.dcx',
                              lambda: build_common(base.read_bytes(), args.prompts)))
        used.add(f'prompts-{key}')
    if args.gestures == '1':
        base = game_file(args.game, 'gesturetop.gfx')
        stat = base.stat()
        key = hashlib.sha256(json.dumps([sources(ASSETS / 'gesturetop.json'), str(base), stat.st_size,
                                         stat.st_mtime_ns]).encode()).hexdigest()[:16]
        folders.append(cached(args.out, f'gestures-{key}', 'gesturetop.gfx',
                              lambda: apply_gestures(base.read_bytes())))
        used.add(f'gestures-{key}')
    # Older results (other keys, other game folders) are dropped.
    cache = Path(args.out) / 'menu-prompts'
    if cache.is_dir():
        for entry in cache.iterdir():
            if entry.name not in used:
                shutil.rmtree(entry, ignore_errors=True)
    print(os.pathsep.join(str(f) for f in folders))


if __name__ == '__main__':
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    try:
        main()
    except (OSError, ValueError) as error:
        print(f'Button prompts: {error}; the game\'s own are used', file=sys.stderr)
        print('')
