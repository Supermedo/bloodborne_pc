#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Draws assets/menu_prompts/keyboard: button prompts showing the keyboard keys of
src/runtime_pad.c (KEYS below), as scripts/menu_prompts.py uses them. Needs Pillow; run again
when the keys change:

    python tools/make_keyboard_prompts.py

Which textures to draw (the KG_* icons and three spots of the UI atlas) follows Abken's KBM Icons
Generator (Nexus Mods, Bloodborne mod 537); nothing of it is used."""
from pathlib import Path
import sys

from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'scripts'))
import menu_prompts  # noqa: E402
import menu_textures as mt  # noqa: E402

# src/runtime_pad.c: the keyboard's buttons and sticks.
KEYS = {
    'cross': 'Space', 'circle': 'Shift', 'square': 'E', 'triangle': 'Q', 'l1': '1', 'r1': '3',
    'l2': 'R', 'r2': 'F', 'l3': 'Z', 'r3': 'C', 'options': 'Enter', 'touchpad': 'Tab',
    'touchpad_right': '⌫', 'up': 'I', 'down': 'K', 'left': 'J', 'right': 'L',
    'move_up': 'W', 'move_down': 'S', 'move_left': 'A', 'move_right': 'D',
    'look_up': '↑', 'look_down': '↓', 'look_left': '←', 'look_right': '→',
}
FONTS = ('C:/Windows/Fonts/segoeuib.ttf', '/usr/share/fonts/TTF/DejaVuSans-Bold.ttf',
         '/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf')
SCALE = 4  # drawn at 4x, then scaled down


def font(size):
    for path in FONTS:
        if Path(path).is_file():
            return ImageFont.truetype(path, size)
    return ImageFont.load_default(size)


def cap(draw, box, label):
    """One key cap with its label, fitted into box (x0, y0, x1, y1)."""
    x0, y0, x1, y1 = box
    draw.rounded_rectangle(box, radius=max(2, (y1 - y0) // 5), fill=(58, 58, 58, 238),
                           outline=(170, 170, 170, 220), width=SCALE)
    size = int((y1 - y0) * 0.7)
    while size > 4:
        face = font(size)
        left, top, right, bottom = draw.textbbox((0, 0), label, font=face)
        if right - left <= (x1 - x0) - 4 * SCALE:
            break
        size -= 1
    draw.text(((x0 + x1 - (right - left)) / 2 - left, (y0 + y1 - (bottom - top)) / 2 - top), label,
              font=face, fill=(216, 214, 210, 255))


def icon(inputs):
    """A 32x32 prompt: one key cap, or a cluster (up, down, left, right; None left out)."""
    size = 32 * SCALE
    image = Image.new('RGBA', (size, size), (0, 0, 0, 0))
    draw = ImageDraw.Draw(image)
    if isinstance(inputs, str):
        cap(draw, (0, 3 * SCALE, size, size - 3 * SCALE), KEYS[inputs])
    else:
        up, down, left, right = inputs
        if up and down and left and right:  # a T: up above, left/down/right below
            w, h = (size - 2 * SCALE) // 3, 15 * SCALE
            spots = {'up': ((size - w) // 2, SCALE), 'left': (0, 16 * SCALE),
                     'down': ((size - w) // 2, 16 * SCALE), 'right': (size - w, 16 * SCALE)}
        elif left and right:  # a pair side by side
            w, h = 16 * SCALE, 24 * SCALE
            spots = {'left': (0, 4 * SCALE), 'right': (16 * SCALE, 4 * SCALE)}
        else:  # a pair one above the other
            w, h = 22 * SCALE, 16 * SCALE
            spots = {'up': (5 * SCALE, 0), 'down': (5 * SCALE, 16 * SCALE)}
        for name, which in zip(('up', 'down', 'left', 'right'), (up, down, left, right)):
            if which:
                x, y = spots[name]
                cap(draw, (x, y, x + w - SCALE // 2, y + h - SCALE // 2), KEYS[which])
    return image.resize((32, 32), Image.LANCZOS)


def main():
    out = menu_prompts.ASSETS / 'keyboard'
    out.mkdir(parents=True, exist_ok=True)
    icons = {slot: icon(inputs) for slot, inputs in menu_prompts.ICONS.items()}
    for slot, image in icons.items():
        (out / f'{slot}.dds').write_bytes(mt.dds(32, 32, mt.encode(image)))
    x0, y0, w, h = menu_prompts.ATLAS_STRIP
    strip = Image.new('RGBA', (w, h), (0, 0, 0, 0))
    for slot, (x, y) in menu_prompts.ATLAS_GLYPHS.items():
        strip.alpha_composite(icons[slot].resize((26, 26), Image.LANCZOS), (x - x0, y - y0))
    (out / 'atlas.dds').write_bytes(mt.dds(w, h, mt.encode(strip)))
    print(f'{out}: {len(icons)} icons and the atlas strip')


if __name__ == '__main__':
    main()
