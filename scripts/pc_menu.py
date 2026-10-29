#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""PC Settings in the game's own System menu (bbport.ini pc_menu, game version 01.09).

The System menu gets three lines before Exit Game (and Quit to Desktop, quit_menu.py, after it):
PC Graphics, PC Effects and PC Options. Where the menu's builder is about to add Exit Game, with
the line list in r12, the patched code asks the port for the lines ("bb:o" through the browser import,
src/runtime_services.c), passing where r13 is kept. The port (gpu/shim/bbport_pc_menu.cpp) adds
them with the game's own calls. Each opens the PC page of the game's settings screen, built with
the game's choice lines of its kind, whose values the port applies and keeps in bbport.ini.

The settings screen (optionsetting.gfx) has one page per name, and the game's page opener finds
a page among six names. The three lines share one new page of the movie (`pc_pages`: a copy of the
Controls page with nine value rows and the three lines' titles, of which the port shows the one
opened), and the opener reads its names from a table of seven here, which the port fills when it
loads the game (bbgpu_pc_menu_prepare). One page, not one per line: the screen loads all its
pages whenever any settings page opens, and each page's rows took ~3.4 MiB of the menus' 34 MiB
Scaleform pool; with three, an open left ~1.8 MiB, and late in an online session the menus ran out
(write to 0 at 0xd67bb or 0x64466).

The texts (`texts()`, the same ids as bbport_pc_menu.cpp) and the movie go into the Quit to
Desktop layer (quit_menu.py --pc), which also gives the System menu three more rows.
"""
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import quit_menu  # noqa: E402
from quit_menu import swf  # noqa: E402

EXIT_LINE = 0x1BB4313                # before Exit Game's line (r12: the lines; both ways here have them)
EXIT_NEXT = EXIT_LINE + 7            # after the instruction the jump replaces
PAGE_TABLE = 0x1B20987               # the page opener: lea rax, [rip + its six page names]
PAGE_COUNT = 0x1B20E1E               # ... cmp rdx, 6
HIDE_PAGES = 0x1BDACF2               # where the movie's setup has hidden the game's pages (r15:
                                     # the movie, [rbp-0x90]); next: lea rdx, [rip + "Top"]
# Every settings page ends with the opener's Defaults button (r12: the page), whose Yes/No
# dialog's Yes restores the page's defaults. On the PC page it is Restart Game: its name, help
# and question are the port's texts, and Yes restarts the game instead.
DEFAULTS_NAME = 0x1B20CB8            # mov edx, 40330 (esi 200 set before)
DEFAULTS_HELP = 0x1B20CD1            # mov edx, 40330 (after mov esi, 201)
DEFAULTS_QUESTION = 0x1B24887        # mov edx, 40300 (esi 204; r13: the page)
DEFAULTS_YES = 0x1B24C10             # the Yes callback (rdi: the callback, [rdi+8] the page)
CAVE = 0x50D9B00                     # past Rebirth's code (0x50D9900) in the last code page
ORIGINAL = {
    EXIT_LINE: '488d9d50e0ffff',     # lea rbx, [rbp-0x1fb0] (Exit Game's texts)
    PAGE_TABLE: '488d05228f8103',    # lea rax, [rip + 0x3818f22] (0x53398B0)
    PAGE_COUNT: '4883fa06',          # cmp rdx, 6
    HIDE_PAGES: '488d15bd93d502',    # lea rdx, [rip + 0x2d593bd] ("Top")
    DEFAULTS_NAME: 'ba8a9d0000',
    DEFAULTS_HELP: 'ba8a9d0000',
    DEFAULTS_QUESTION: 'ba6c9d0000',
    DEFAULTS_YES: '554889e55350488b5f08',  # push rbp; mov rbp, rsp; push rbx; push rax; mov rbx, [rdi+8]
}
# The game's page names in its table's order (image offsets of the strings).
GAME_PAGES = [0x49340B6, 0x4934055, 0x4934065, 0x4934075, 0x4934088, 0x493409A]
TABLE_MAGIC = b'bbPCpag\0'           # bbgpu_pc_menu_prepare checks it before the table

PAGE = 'PcSettings'                  # the PC page's name in optionsetting.gfx
TITLES = [119001, 119002, 119003]    # its titles, the System lines' names: instances PcTitle_<n>
TITLE_NAME = 'PcTitle_'
LINE_BASE = 119010

# The pages' lines (bbport_pc_menu.cpp's Line order): name, one-line help.
LINES = [
    ('Upscaler', 'Smooths edges and rebuilds detail at the output resolution.'),
    ('Upscaler Quality', 'Scene resolution: Native is sharpest, Performance is fastest.'),
    ('Sharpness', 'Sharpens the upscaled image.'),
    ('Frame Generation', 'Adds generated frames between the game\'s frames.'),
    ('Generated Frames', 'Frames shown per game frame (DLSS on RTX 50 up to 4x).'),
    ('Output Resolution', 'Final image size. Ultrawide sizes apply after restart.'),
    ('Frame Rate', 'The game\'s frame rate. Applies after restart.'),
    ('Frame Cap', 'Limit of the unlocked frame rate. Applies after restart.'),
    ('Model Detail', 'Detail of distant models. Applies after restart.'),
    ('Ambient Occlusion', 'Contact shadows in corners. Applies after restart.'),
    ('Dynamic Shadows', 'Shadows of moving lights. Applies after restart.'),
    ('Reflections', 'Screen-space reflections on wet ground. Applies after restart.'),
    ('Anti-Aliasing', 'The game\'s own edge smoothing. Applies after restart.'),
    ('Motion Blur', 'Blurs fast movement. Applies after restart.'),
    ('Depth of Field', 'Blurs what is out of focus. Applies after restart.'),
    ('Chromatic Aberration', 'Colour fringes at the screen edges. Applies after restart.'),
    ('Field of View', 'Camera field of view in degrees. Applies after restart.'),
    ('Camera Distance', 'Places the camera further back. Applies after restart.'),
    ('Auto Camera Rotation', 'Camera turns by itself while moving. Applies after restart.'),
    ('HUD Position', 'Ultrawide: moves the HUD toward the edges. Applies after restart.'),
    ('FPS Counter', 'Shows the frame rate in a corner of the screen.'),
    ('Button Prompts', 'Which buttons the game shows.'),
    ('Mute in Background', 'Silences the game while another window is active.'),
    ('Skip Intro', 'Skips the logos and opening movie. Applies after restart.'),
    ('Restart Game', 'Restarts the game now to apply the changes marked for restart.'),
]
PAGE_HELP = {
    119001: 'Upscaling, frame generation, resolution and frame rate.',
    119002: 'Model detail, shadows, reflections and post-processing effects.',
    119003: 'Camera, HUD, button prompts and other PC options.',
}

# Choice labels: (first id, labels), in bbport_pc_menu.cpp's order.
LABELS = [
    (119100, ['Off', 'FSR 3.1', 'FSR 4', 'FSR 4.1.1', 'TAA', 'DLSS']),
    (119110, ['Native', 'Quality', 'Balanced', 'Performance', 'Ultra Performance']),
    (119120, ['Off', '0.1', '0.2', '0.3', '0.4', '0.5', '0.6', '0.7', '0.8', '0.9', '1.0', '1.5', '2.0']),
    (119140, ['Off', 'FSR 3.1', 'DLSS', 'AMD ML']),
    (119150, ['2x', '3x', '4x']),
    (119160, ['1280 x 720', '1920 x 1080', '2560 x 1440', '3840 x 2160', '2560 x 1080 (21:9)',
              '3440 x 1440 (21:9)', '3840 x 1600 (21:9)', '5120 x 2160 (21:9)', '3840 x 1080 (32:9)',
              '5120 x 1440 (32:9)']),
    (119180, ['30 FPS', '60 FPS', '90 FPS', 'Unlocked']),
    (119190, ['Auto (display, max 120)', '60', '90', '120', '144', '165', '240', 'No limit']),
    (119200, ['30', '35', '40', '43 (default)', '45', '50', '55', '60', '65', '70', '75', '80', '85', '90']),
    (119220, ['Highest', 'High', 'Default', 'Low', 'Lowest']),
    (119230, ['0% (16:9)', '10%', '20%', '30%', '40%', '50%', '60%', '70%', '80%', '90%', '100% (edges)']),
    (119250, ['Auto', 'PlayStation', 'Keyboard & Mouse', 'Xbox', 'Switch']),
    (119260, ['-', 'Restart Now']),
]
PAGE_TITLES = {119001: 'PC Graphics', 119002: 'PC Effects', 119003: 'PC Options'}
RESTART = 119009                     # the Restart Game button: name (200), help (201), question (204)


def texts():
    """{FMG id: {text id: text}} for menu.msgbnd.dcx: 200 names, titles and labels, 201 help."""
    names, help_texts = dict(PAGE_TITLES), dict(PAGE_HELP)
    names[RESTART] = 'Restart Game'
    help_texts[RESTART] = 'Restarts the game to apply the changes marked for restart.'
    questions = {RESTART: 'Restart the game now\nto apply the changes?'}
    for i, (name, line_help) in enumerate(LINES):
        names[LINE_BASE + i] = name
        help_texts[LINE_BASE + i] = line_help
    for first, labels in LABELS:
        for i, label in enumerate(labels):
            names[first + i] = label
    return {200: names, 201: help_texts, 204: questions}


def check_original(read):
    """`read(start, end)`: bytes of the original eboot's image."""
    for offset, expected in ORIGINAL.items():
        data = bytes.fromhex(expected)
        if read(offset, offset + len(data)) != data:
            raise ValueError(f'PC Settings: unexpected code at {offset:#x}')


def code_writes():
    """(offset, bytes) writes: the cave code and table, the jump to it, the opener's table."""
    cave = quit_menu.Code(CAVE)
    line = cave.here
    cave.raw('4154')            # push r12 (the line list; the port may replace it)
    cave.raw('4889e6')          # mov rsi, rsp
    cave.raw('4883ec08')        # sub rsp, 8 (16-byte aligned call)
    message_at = len(cave.data)
    cave.raw('488d3d00000000')  # lea rdi, [rip + "bb:o"]
    cave.call(quit_menu.BROWSER)
    cave.raw('4883c408')        # add rsp, 8
    cave.raw('415c')            # pop r12
    cave.raw(ORIGINAL[EXIT_LINE])  # the replaced instruction
    cave.jmp(EXIT_NEXT)
    # The movie's setup: the PC page hidden as the game's own ("bb:h" with the movie).
    hide = cave.here
    hide_message_at = len(cave.data)
    cave.raw('488d3d00000000')  # lea rdi, [rip + "bb:h"]
    cave.raw('488db570ffffff')  # lea rsi, [rbp-0x90]
    cave.call(quit_menu.BROWSER)
    cave.lea('488d15', GAME_PAGES[0])  # lea rdx, [rip + "Top"] (the replaced instruction)
    cave.jmp(HIDE_PAGES + 7)
    message = cave.here
    cave.data += b'bb:o\0'
    cave.data[message_at + 3:message_at + 7] = quit_menu.rel32(CAVE + message_at, 7, message)
    hide_message = cave.here
    cave.data += b'bb:h\0'
    cave.data[hide_message_at + 3:hide_message_at + 7] = quit_menu.rel32(CAVE + hide_message_at, 7, hide_message)
    # The Defaults button: the port says which texts (its own on a PC page) and what Yes does.
    messages = []

    def ask(message, arg):  # lea rdi, [rip + message]; mov rsi, <arg>; call the browser
        messages.append((len(cave.data), message))
        cave.raw('488d3d00000000')
        cave.raw(arg)
        cave.call(quit_menu.BROWSER)
    defaults_name = cave.here
    ask('bb:l', '4c89e6')       # mov rsi, r12 (the page)
    cave.raw('89c2')            # mov edx, eax
    cave.raw('bec8000000')      # mov esi, 200
    cave.jmp(DEFAULTS_NAME + 5)
    help_line = cave.here
    ask('bb:l', '4c89e6')
    cave.raw('89c2bec9000000')  # mov edx, eax; mov esi, 201
    cave.jmp(DEFAULTS_HELP + 5)
    question = cave.here
    ask('bb:r', '4c89ee')       # mov rsi, r13 (the page)
    cave.raw('89c2becc000000')  # mov edx, eax; mov esi, 204
    cave.jmp(DEFAULTS_QUESTION + 5)
    yes = cave.here
    cave.raw('57')              # push rdi (16-byte aligned call)
    cave.raw('488b7708')        # mov rsi, [rdi+8] (the page)
    ask('bb:y', '')
    cave.raw('5f')              # pop rdi
    cave.raw('85c07401c3')      # test eax, eax; jz +1; ret (the port restarts the game)
    cave.raw(ORIGINAL[DEFAULTS_YES])
    cave.jmp(DEFAULTS_YES + 10)
    for message in ('bb:l', 'bb:r', 'bb:y'):
        at = cave.here
        cave.data += message.encode() + b'\0'
        for offset, name_ in messages:
            if name_ == message:
                cave.data[offset + 3:offset + 7] = quit_menu.rel32(CAVE + offset, 7, at)
    # The page names: image offsets here, made addresses by the port when it loads the game.
    name = cave.here
    cave.data += PAGE.encode() + b'\0'
    cave.data += b'\0' * (-len(cave.data) % 8)
    cave.data += TABLE_MAGIC
    table = cave.here
    offsets = GAME_PAGES + [name]
    for offset in offsets:
        cave.data += struct.pack('<Q', offset)
    if cave.here > quit_menu.CAVE_END:
        raise ValueError('PC Settings code does not fit')
    keep = quit_menu.Code(EXIT_LINE)
    keep.jmp(line)
    keep.raw('9090')
    opener = quit_menu.Code(PAGE_TABLE)
    opener.lea('488d05', table)
    count = quit_menu.Code(PAGE_COUNT)
    count.raw('4883fa' + f'{len(offsets):02x}')
    hidden = quit_menu.Code(HIDE_PAGES)
    hidden.jmp(hide)
    hidden.raw('9090')
    hooks = []
    for site, target in ((DEFAULTS_NAME, defaults_name), (DEFAULTS_HELP, help_line),
                         (DEFAULTS_QUESTION, question), (DEFAULTS_YES, yes)):
        hook = quit_menu.Code(site)
        hook.jmp(target)
        hook.raw('90' * (len(bytes.fromhex(ORIGINAL[site])) - 5))
        hooks.append(hook)
    return [(c.offset, bytes(c.data)) for c in (cave, keep, opener, count, hidden, *hooks)]


# ---- optionsetting.gfx: the PC page -------------------------------------------------------------
# The page is a copy of the Controls page (sprite CONTROLS) with value rows only: its button row
# (Item_6_0) removed, rows added below Item_5_0 up to ROWS, the frame (a copy of FRAME) and the
# help bar where the button row was, and a title per System line (copies of TITLE whose text
# field is StaticText_<title id>, placed as the Controls page's title and named PcTitle_<n>). The
# root places the page under its name, as the game's own pages.
CONTROLS, FRAME, TITLE = 138, 135, 137
CONTROLS_NAME, TITLE_FIELD = b'ControllSetting\0', b'StaticText_113020\0'
ROWS = 9
ROW_STEP = 1143                      # twips between the Controls page's rows
BUTTON_ROW_DEPTH, LAST_ROW_DEPTH, STATUS_DEPTH, TITLE_DEPTH = 110, 101, 53, 117
FRAME_ROWS_END = (14, 25)            # in the frame: the side ornament and separator under the rows
FRAME_BOTTOM = (33, 49)              # ... the lower separator and bottom ornament
FRAME_BACKGROUND = 41
DROPDOWN_BACK = 80                   # the dropdown list's frame: its background at depth 1
# Between the two separators the Controls page has its button row, not used here: the part of
# that band left out (one row's height stays, above the help bar's own line).
BUTTON_BAND = 10104 - 6836 - ROW_STEP
# Tags that define a character (its id first): shapes, sprites, texts, fonts, bitmaps, buttons.
DEFINING = {2, 22, 32, 83, 39, 37, 11, 33, 10, 48, 75, 91, 6, 21, 35, 36, 20, 90, 46, 84, 7, 34, 14, 60}


def parse(body):
    """[[code, bytearray body]] of a tag sequence."""
    return [[code, bytearray(body[b:b + n])] for code, _, b, n in swf.tags(body, 0, len(body))]


def serialize(items):
    return b''.join(swf.tag(code, bytes(body)) for code, body in items)


def is_place(code):
    return code in (26, 70)


def place_depth(code, body):
    return struct.unpack_from('<H', body, 1 + (code == 70))[0]


def set_place(code, body, depth=None, character=None, dy=0, matrix=None):
    """A PlaceObject2/3 body with another depth or character, or its matrix moved by `dy`
    (or given by `matrix(old)`)."""
    out = bytearray(body)
    if depth is not None:
        struct.pack_into('<H', out, 1 + (code == 70), depth)
    char, span, _ = swf.place_fields(bytes(out), 0, code)
    if character is not None and char is not None:
        i = 1 + (code == 70) + 2
        if code == 70 and out[1] & 8:
            i = out.index(0, i) + 1
        struct.pack_into('<H', out, i, character)
    if span and (dy or matrix):
        old = swf.read_matrix(bytes(out), span[0])[0]
        new = matrix(old) if matrix else quit_menu.moved(old, dy)
        out = out[:span[0]] + swf.write_matrix(new) + out[span[1]:]
    return out


def named(code, body, name):
    """A PlaceObject2/3 body without an instance name, with `name` as its name."""
    data = bytes(body)
    if data[0] & 0x20:
        raise ValueError('the placement has a name already')
    i = 1 + (code == 70) + 2
    if code == 70 and data[1] & 8:
        i = data.index(0, i) + 1   # class name
    if data[0] & 2:
        i += 2                     # character
    if data[0] & 4:
        _, i = swf.read_matrix(data, i)
    if data[0] & 8:                # CXFORMWITHALPHA
        r = swf.Reader(data, i)
        add, mult = r.unsigned(1), r.unsigned(1)
        r.bit += r.unsigned(4) * 4 * (add + mult)
        i = r.end()
    if data[0] & 16:
        i += 2                     # ratio
    return bytearray(bytes([data[0] | 0x20]) + data[1:i] + name.encode() + b'\0' + data[i:])


def shift(items, depth, dy):
    for code, body in items:
        if is_place(code) and place_depth(code, body) == depth:
            body[:] = set_place(code, body, dy=dy)


def sprite_items(data, sprites, character):
    start, end = sprites[character]
    return parse(data[start:end])


def define_sprite(character, frames, items):
    return [39, bytearray(struct.pack('<HH', character, frames) + serialize(items))]


def page_sprites(data, sprites, title_chars, frame_char, page_char, title_ids):
    """[DefineSprite] of the PC page: its titles, frame and page."""
    extra = (ROWS - 6) * ROW_STEP
    grown = extra - BUTTON_BAND        # what the panel grows by: more rows, no button row
    titles = []
    for title_id in title_ids:
        title = sprite_items(data, sprites, TITLE)
        for code, body in title:
            if is_place(code) and TITLE_FIELD in body:
                body[:] = body.replace(TITLE_FIELD, f'StaticText_{title_id}\0'.encode())
        titles.append(title)
    frame = sprite_items(data, sprites, FRAME)
    for depth in FRAME_ROWS_END:
        shift(frame, depth, extra)
    for depth in FRAME_BOTTOM:
        shift(frame, depth, grown)
    for code, body in frame:
        if is_place(code) and place_depth(code, body) == FRAME_BACKGROUND:
            char, _, _ = swf.place_fields(bytes(body), 0, code)
            top, bottom = quit_menu.swf_shape_y(data, char)

            def grow(matrix, top=top, bottom=bottom):  # downward: the top edge stays
                scale, rotate, (tx, ty) = matrix
                sx, sy = scale or (65536, 65536)
                height = (bottom - top) * sy / 65536
                factor = (height + grown) / height
                return (sx, round(sy * factor)), rotate, (tx, round(ty + top * sy / 65536 * (1 - factor)))
            body[:] = set_place(code, body, matrix=grow)
    start, _ = sprites[CONTROLS]
    frames, = struct.unpack_from('<H', data, start - 2)
    page = sprite_items(data, sprites, CONTROLS)
    button = BUTTON_ROW_DEPTH
    page = [[code, body] for code, body in page
            if not ((is_place(code) and place_depth(code, body) == button) or
                    (code == 28 and struct.unpack_from('<H', body, 0)[0] == button))]
    for code, body in page:
        if is_place(code):
            char, _, _ = swf.place_fields(bytes(body), 0, code)
            if char == FRAME:
                body[:] = set_place(code, body, character=frame_char)
    shift(page, STATUS_DEPTH, grown)
    used = {place_depth(code, body) for code, body in page if is_place(code)}
    free = iter(d for d in range(LAST_ROW_DEPTH + 1, 1000) if d not in used)
    for row in range(6, ROWS):
        depth = next(free)
        old, new = b'Item_5_0\0', f'Item_{row}_0\0'.encode()
        out = []
        for code, body in page:
            out.append([code, body])
            if is_place(code) and place_depth(code, body) == LAST_ROW_DEPTH:
                copy = set_place(code, body, depth=depth, dy=(row - 5) * ROW_STEP)
                out.append([code, bytearray(bytes(copy).replace(old, new))])
            elif code == 28 and struct.unpack_from('<H', body, 0)[0] == LAST_ROW_DEPTH:
                out.append([28, bytearray(struct.pack('<H', depth))])
        page = out
    # The titles: each where the Controls page has its title, frame by frame (its fade), the first
    # at the title's own depth, the others just above it; the port shows one (bbport_pc_menu.cpp).
    used = {place_depth(code, body) for code, body in page if is_place(code)}
    depths = [TITLE_DEPTH] + [max(used) + n for n in range(1, len(title_chars))]
    out = []
    for code, body in page:
        if is_place(code) and place_depth(code, body) == TITLE_DEPTH:
            first = swf.place_fields(bytes(body), 0, code)[0] is not None
            for n, (char, depth) in enumerate(zip(title_chars, depths)):
                copy = set_place(code, body, depth=depth, character=char)
                out.append([code, named(code, copy, f'{TITLE_NAME}{n}') if first else copy])
        elif code == 28 and struct.unpack_from('<H', body, 0)[0] == TITLE_DEPTH:
            out += [[28, bytearray(struct.pack('<H', depth))] for depth in depths]
        else:
            out.append([code, body])
    page = out
    # A dropdown opens downward from its row: the rows above draw over the rows below, else the
    # rows under an open list showed through it (the game's pages have no value rows under one).
    rows = {}
    for code, body in page:
        if is_place(code):
            _, _, name = swf.place_fields(bytes(body), 0, code)
            if name and name.startswith('Item_') and name.endswith('_0'):
                rows[place_depth(code, body)] = int(name[5:-2])
    used = {place_depth(code, body) for code, body in page if is_place(code)}
    top = max(used) + ROWS + 1
    remap = {depth: top - row for depth, row in rows.items()}
    for code, body in page:
        if is_place(code) and place_depth(code, body) in remap:
            body[:] = set_place(code, body, depth=remap[place_depth(code, body)])
        elif code == 28 and struct.unpack_from('<H', body, 0)[0] in remap:
            struct.pack_into('<H', body, 0, remap[struct.unpack_from('<H', body, 0)[0]])
    return [*(define_sprite(char, 1, title) for char, title in zip(title_chars, titles)),
            define_sprite(frame_char, 1, frame), define_sprite(page_char, frames, page)]


def pc_pages(data, page=PAGE, titles=TITLES):
    """optionsetting.gfx with the PC page (instance name `page`, one title per text id in
    `titles`), or None when it has it."""
    _, sprites, _ = swf.definitions(data)
    start = swf.stage_start(data)
    root = parse(data[start:])
    if any(is_place(code) and page.encode() + b'\0' in body for code, body in root):
        return None
    next_id = 1 + max(struct.unpack_from('<H', body, 0)[0] for code, body in root
                      if code in DEFINING and len(body) >= 2)
    controls = next(i for i, (code, body) in enumerate(root) if is_place(code) and CONTROLS_NAME in body)
    depth = 1 + max(place_depth(code, body) for code, body in root if is_place(code))
    grown = (ROWS - 6) * ROW_STEP - BUTTON_BAND
    title_chars = list(range(next_id, next_id + len(titles)))
    frame_char, page_char = next_id + len(titles), next_id + len(titles) + 1
    definitions = page_sprites(data, sprites, title_chars, frame_char, page_char, titles)
    code, body = root[controls]
    placed = set_place(code, body, depth=depth, character=page_char, dy=-grown / 2)
    placements = [[code, bytearray(bytes(placed).replace(CONTROLS_NAME, page.encode() + b'\0'))]]
    # The page plays as the Controls page: its script class (frame scripts that stop the fade in
    # and out), else its timeline looped and the page flashed.
    for code, body in root:
        if code == 76:  # SymbolClass
            count, = struct.unpack_from('<H', body, 0)
            links, at = {}, 2
            for _ in range(count):
                character, = struct.unpack_from('<H', body, at)
                end = body.index(0, at + 2)
                links[character] = bytes(body[at + 2:end + 1])
                at = end + 1
            body[:] = struct.pack('<H', count + 1) + body[2:] + struct.pack('<H', page_char) + links[CONTROLS]
    # The dropdown list's background (95% opaque): opaque, over the rows under the list.
    for code, body in root:
        if code == 39 and struct.unpack_from('<H', body, 0)[0] == DROPDOWN_BACK:
            inner = parse(bytes(body[4:]))
            for inner_code, inner_body in inner:
                if inner_code == 26 and place_depth(26, inner_body) == 1 and inner_body[0] & 8:
                    _, span, _ = swf.place_fields(bytes(inner_body), 0, 26)
                    # CXFORMWITHALPHA: multiply terms only, 10 bits each, all 256 (1.0).
                    bits = '01' + '1010' + format(256, '010b') * 4
                    bits += '0' * (-len(bits) % 8)
                    cxform = int(bits, 2).to_bytes(len(bits) // 8, 'big')
                    end = span[1] + 6  # the game's: the same 46 bits
                    inner_body[:] = inner_body[:span[1]] + cxform + inner_body[end:]
            body[:] = body[:4] + serialize(inner)
    root = root[:controls] + definitions + root[controls:controls + 1] + placements + root[controls + 1:]
    out = data[:start] + serialize(root)
    return out[:4] + struct.pack('<I', len(out)) + out[8:]


if __name__ == '__main__':
    for offset, data in code_writes():
        print(f'{offset:#x}: {data.hex()}')
    print(sum(len(t) for t in texts().values()), 'texts')
