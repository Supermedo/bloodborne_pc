#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Ultrawide HUD layout: copies of the game's menu movies (dvdroot_ps4/menu/*.gfx) for a HUD canvas
wider than 16:9 (patches.py hud_canvas), which the game fills with its 1920x1080 stage centred.

Full-screen backdrops (dimming, fades, the shared menu textures) are widened to the canvas;
pictures (the title art, loading screens) keep their shape. In the HUD movie (fe.gfx) the
health/stamina/item block (PlayerHUD) at the left and Blood Echoes and Insight (inside PlayerHUD)
and the autosave icon at the right move by `position` (0 centred, 1 at the edges) times the
canvas margin. Lock-on and ally tags are placed by the game at run time and stay as they are.
The copies go into a mod layer (mods.py --layer).
"""
import argparse
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import port_files  # noqa: E402

# (sprite id or None for the root timeline, instance name): -1 moves left, +1 right. The right
# side of PlayerHUD moves with it first, hence 2.
MOVES = {
    (None, 'PlayerHUD'): -1,
    ('PlayerHUD', 'Soul'): 2,
    ('PlayerHUD', 'HeroPoint'): 2,
    (None, 'AutoSaveIcon'): 1,
}
STAGE_WIDTH = 1920
SHAPE_TAGS = (2, 22, 32, 83)
# Shared menu textures that full-screen backdrops use (dark overlays, banners); other bitmaps are
# pictures that keep their 16:9 shape.
BACKDROP_TEXTURES = ('MENU_Common_',)


class Reader:
    def __init__(self, data, offset):
        self.data, self.bit = data, offset * 8

    def unsigned(self, n):
        value = 0
        for _ in range(n):
            value = value << 1 | (self.data[self.bit >> 3] >> (7 - (self.bit & 7))) & 1
            self.bit += 1
        return value

    def signed(self, n):
        value = self.unsigned(n)
        return value - (1 << n) if n and value >> (n - 1) else value

    def end(self):
        return (self.bit + 7) >> 3


class Writer:
    def __init__(self):
        self.bits = []

    def unsigned(self, value, n):
        self.bits += [(value >> (n - 1 - i)) & 1 for i in range(n)]

    def bytes(self):
        bits = self.bits + [0] * (-len(self.bits) % 8)
        return bytes(sum(b << (7 - i) for i, b in enumerate(bits[k:k + 8]))
                     for k in range(0, len(bits), 8))


def signed_bits(*values):
    return max([1] + [v.bit_length() + 1 if v >= 0 else (~v).bit_length() + 1 for v in values])


def read_matrix(data, offset):
    """MATRIX at offset: ((scale or None), (rotate or None), (tx, ty) in twips), end offset."""
    r = Reader(data, offset)
    scale = rotate = None
    if r.unsigned(1):
        n = r.unsigned(5)
        scale = (r.signed(n), r.signed(n))
    if r.unsigned(1):
        n = r.unsigned(5)
        rotate = (r.signed(n), r.signed(n))
    n = r.unsigned(5)
    translate = (r.signed(n), r.signed(n))
    return (scale, rotate, translate), r.end()


def write_matrix(matrix):
    scale, rotate, translate = matrix
    w = Writer()
    for pair in (scale, rotate):
        w.unsigned(pair is not None, 1)
        if pair is not None:
            n = signed_bits(*pair)
            w.unsigned(n, 5)
            for v in pair:
                w.unsigned(v & ((1 << n) - 1), n)
    n = signed_bits(*translate) if any(translate) else 0
    w.unsigned(n, 5)
    for v in translate:
        w.unsigned(v & ((1 << n) - 1), n)
    return w.bytes()


def tags(data, offset, end):
    """(code, header start, body start, body length) of each tag in [offset, end)."""
    while offset < end:
        header, = struct.unpack_from('<H', data, offset)
        code, length, body = header >> 6, header & 63, offset + 2
        if length == 63:
            length, = struct.unpack_from('<I', data, body)
            body += 4
        yield code, offset, body, length
        offset = body + length


def tag(code, body):
    if len(body) < 63 and code not in (39,):
        return struct.pack('<H', code << 6 | len(body)) + body
    return struct.pack('<HI', code << 6 | 63, len(body)) + body


def place_fields(data, body, code):
    """PlaceObject2/3: (character id, matrix span (start, end) or None, instance name)."""
    flags = data[body]
    i = body + 1
    flags2 = 0
    if code == 70:
        flags2 = data[i]
        i += 1
    i += 2  # depth
    if code == 70 and flags2 & 8:
        i = data.index(0, i) + 1  # class name
    character = None
    if flags & 2:
        character, = struct.unpack_from('<H', data, i)
        i += 2
    span = None
    if flags & 4:
        _, end = read_matrix(data, i)
        span = (i, end)
        i = end
    if flags & 8:  # CXFORMWITHALPHA: skip
        r = Reader(data, i)
        add, mult = r.unsigned(1), r.unsigned(1)
        n = r.unsigned(4)
        r.bit += n * 4 * (add + mult)
        i = r.end()
    if flags & 16:
        i += 2  # ratio
    name = None
    if flags & 32:
        end = data.index(0, i)
        name = data[i:end].decode('latin-1')
    return character, span, name


def placements(data, start, end):
    """Named placements in a timeline: {name: character id}."""
    found = {}
    for code, _, body, length in tags(data, start, end):
        if code in (26, 70):
            character, _, name = place_fields(data, body, code)
            if name and character is not None:
                found[name] = character
    return found


def rewrite_timeline(data, start, end, edits, inserts=None, after=None):
    """The tags of [start, end) with the placements at body offsets in `edits` given new
    matrices, the tags in `inserts` {timeline start: bytes} added before that timeline's first
    frame ends and those in `after` {tag body offset: bytes} right after that tag; sprites
    holding changes are rebuilt with their new length."""
    inserts, after = inserts or {}, after or {}
    changed = list(edits) + list(inserts) + list(after)
    out = bytearray()
    pending = inserts.get(start)
    for code, header, body, length in tags(data, start, end):
        if code == 1 and pending:
            out += pending
            pending = None
        if body in edits:
            _, span, _ = place_fields(data, body, code)
            out += tag(code, data[body:span[0]] + write_matrix(edits[body]) + data[span[1]:body + length])
        elif code == 39 and any(body < offset < body + length for offset in changed):
            inner = rewrite_timeline(data, body + 4, body + length, edits, inserts, after)
            out += tag(code, data[body:body + 4] + inner)
        else:
            out += data[header:body + length]
        out += after.get(body, b'')
    return bytes(out)


def stage_start(data):
    bits = data[8] >> 3
    return 8 + (5 + 4 * bits + 7) // 8 + 4  # RECT, frame rate, frame count


def definitions(data):
    """({shape id: (fill styles, x min, x max)}, {sprite id: (start, end)}, {bitmap id: name})."""
    shapes, sprites, bitmaps = {}, {}, {}

    def scan(start, end):
        for code, _, body, length in tags(data, start, end):
            if code in SHAPE_TAGS:
                shape, = struct.unpack_from('<H', data, body)
                r = Reader(data, body + 2)
                n = r.unsigned(5)
                x0, x1 = r.signed(n) / 20, r.signed(n) / 20
                shapes[shape] = (fill_styles(data, body, code), x0, x1)
            elif code == 39:
                sprite, = struct.unpack_from('<H', data, body)
                sprites[sprite] = (body + 4, body + length)
                scan(body + 4, body + length)
            elif code == 1009:  # GFX external image: id, format, size, then the texture name
                bitmap, = struct.unpack_from('<H', data, body)
                text = bytes(c if 32 <= c < 127 else 0 for c in data[body + 2:body + length])
                names = [n for n in text.split(b'\0') if len(n) > 2]
                bitmaps[bitmap] = names[0].decode('latin-1') if names else ''
    scan(stage_start(data), len(data))
    return shapes, sprites, bitmaps


def fill_styles(data, body, code):
    """Fill style types of a shape, bitmap fills as (type, bitmap id); None past an unknown one."""
    r = Reader(data, body + 2)
    bits = r.unsigned(5)
    r.bit += 4 * bits
    i = r.end()
    if code in (83, 84):  # DefineShape4: edge bounds, flags
        r = Reader(data, i)
        bits = r.unsigned(5)
        r.bit += 4 * bits
        i = r.end() + 1
    count = data[i]
    i += 1
    if count == 0xFF:
        count, = struct.unpack_from('<H', data, i)
        i += 2
    alpha = code in (32, 83, 84)
    styles = []
    for _ in range(count):
        kind = data[i]
        i += 1
        if kind == 0x00:
            styles.append(kind)
            i += 4 if alpha else 3
        elif kind in (0x10, 0x12, 0x13):
            styles.append(kind)
            _, i = read_matrix(data, i)
            i += 1 + (data[i] & 15) * (5 if alpha else 4) + (2 if kind == 0x13 else 0)
        elif 0x40 <= kind <= 0x43:
            bitmap, = struct.unpack_from('<H', data, i)
            styles.append((kind, bitmap))
            _, i = read_matrix(data, i + 2)
        else:
            return None
    return styles


def widens(styles, bitmaps):
    """A full-stage shape that is a backdrop (solid, gradient, shared menu textures), not a
    picture (the title art, loading screens) that would stretch."""
    if not styles:
        return False
    for style in styles:
        if isinstance(style, tuple) and not bitmaps.get(style[1], '').startswith(BACKDROP_TEXTURES):
            return False
    return True


def backdrop_edits(data, factor, shapes, sprites, bitmaps):
    """Matrices that widen full-stage backdrops by `factor` around the stage centre: {placement
    body offset: matrix}. A placement reached with different parent transforms is left alone."""
    centre = STAGE_WIDTH / 2
    edits, seen = {}, {}

    def walk(start, end, px, ps, level):
        if level > 16:
            return
        characters = {}
        for code, _, body, length in tags(data, start, end):
            if code not in (26, 70):
                continue
            flags = data[body]
            depth, = struct.unpack_from('<H', data, body + 1 + (code == 70))
            character, span, _ = place_fields(data, body, code)
            if character is None:
                character = characters.get(depth)
            else:
                characters[depth] = character
            if character is None or not span:
                continue
            (scale, rotate, (tx, ty)), _ = read_matrix(data, span[0])
            sx = (scale[0] if scale else 65536) / 65536
            x = px + ps * tx / 20
            if character in sprites and flags & 2:
                walk(*sprites[character], x, ps * sx, level + 1)
            if character not in shapes or rotate or not ps:
                continue
            styles, x0, x1 = shapes[character]
            left, right = sorted((x + ps * sx * x0, x + ps * sx * x1))
            if left > 8 or right < STAGE_WIDTH - 8 or not widens(styles, bitmaps):
                continue
            scale = scale or (65536, 65536)
            new_tx = round(factor * tx + (factor - 1) * (px - centre) / ps * 20)
            matrix = ((round(scale[0] * factor), scale[1]), None, (new_tx, ty))
            other = seen.setdefault(body, matrix)
            if other and (other[0] != matrix[0] or abs(other[2][0] - new_tx) > 8 * 20):
                seen[body] = None  # shared by parents placed apart: left alone
            if seen[body] is None:
                edits.pop(body, None)
            else:
                edits[body] = matrix
    walk(stage_start(data), len(data), 0.0, 1.0, 0)
    return edits


def hud_edits(data, margin, sprites):
    """fe.gfx: the corner groups `margin` stage pixels toward the screen edges."""
    twips = round(margin * 20)
    edits = {}
    holders = []

    def move(start, end, parent):
        for code, _, body, length in tags(data, start, end):
            if code not in (26, 70):
                continue
            character, span, name = place_fields(data, body, code)
            sign = MOVES.get((parent, name))
            if sign and span:
                (scale, rotate, (tx, ty)), _ = read_matrix(data, span[0])
                edits[body] = (scale, rotate, (tx + sign * twips, ty))
            if parent is None and name == 'PlayerHUD' and character in sprites:
                holders.append(character)
    move(stage_start(data), len(data), None)
    for character in holders:
        move(*sprites[character], 'PlayerHUD')
    if not edits:
        raise ValueError('PlayerHUD is not in the HUD movie')
    return edits


def layout(data, canvas_width, margin=None):
    """A menu movie for a `canvas_width` HUD canvas: backdrops widened, and for the HUD movie
    (margin given) the corner groups moved. None when nothing changes."""
    if data[:3] not in (b'GFX', b'FWS'):
        raise ValueError('not an uncompressed GFX/SWF movie')
    shapes, sprites, bitmaps = definitions(data)
    # 1% beyond the canvas: no seam at the screen edges.
    edits = backdrop_edits(data, canvas_width / STAGE_WIDTH * 1.01, shapes, sprites, bitmaps)
    if margin is not None:
        edits.update(hud_edits(data, margin, sprites))
    if not edits:
        return None
    start = stage_start(data)
    out = data[:start] + rewrite_timeline(data, start, len(data), edits)
    return out[:4] + struct.pack('<I', len(out)) + out[8:]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('game', type=Path, help='game folder (with dvdroot_ps4)')
    p.add_argument('--out', type=Path, required=True, help='mod layer folder to write')
    p.add_argument('--canvas-width', type=int, required=True, help='HUD canvas width (BB_UI_RES)')
    p.add_argument('--position', type=float, default=1.0, help='0 centred 16:9 .. 1 screen edges')
    p.add_argument('--source', type=Path, action='append', default=[],
                   help="a layer whose menu movies replace the game's (quit_menu.py's), then the mods' "
                        'files (mods.py --sources): the first that has a movie wins')
    a = p.parse_args()
    margin = max(0.0, min(1.0, a.position)) * (a.canvas_width - STAGE_WIDTH) / 2
    menu = a.game / 'dvdroot_ps4' / 'menu'
    target = a.out / 'dvdroot_ps4' / 'menu'
    target.mkdir(parents=True, exist_ok=True)
    written, failed = 0, {}
    for original in sorted(menu.glob('*.gfx')):
        relative = f'dvdroot_ps4/menu/{original.name}'
        source = port_files.source(a.game, relative, *a.source)
        try:
            data = layout(source.read_bytes(), a.canvas_width,
                          margin if original.name.casefold() == 'fe.gfx' else None)
        except (ValueError, IndexError, struct.error) as error:
            print(f'HUD layout: {original.name}: {error}; left as it is', file=sys.stderr)
            if port_files.from_layer(source, a.game):
                failed[relative] = str(error)
            continue
        if data:
            (target / original.name).write_bytes(data)
            written += 1
    import quit_menu
    quit_menu.write_status(a.out, failed)
    print(f'HUD layout: corner groups {margin:.0f} px toward the edges, backdrops widened in '
          f'{written} menus (canvas {a.canvas_width})')


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, IndexError, struct.error) as error:
        # The game still starts, with the HUD centred in 16:9.
        print(f'HUD layout: {error}; the HUD stays centred', file=sys.stderr)
