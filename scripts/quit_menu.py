#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Quit to Desktop in the game's own menus (bbport.ini quit_to_desktop, game version 01.09).

The title menu keeps an entry of FromSoftware's PC builds that the console game hides, "Opinion
Box"; the patches show it as Quit to Desktop. The System menu gets a new entry in the world, where
the game builds its Language entry only to remove it again (outside the world Language stays, as
in the game):

- Title menu: the entry tells the port to close the game ("bb:q"). Nothing is lost there.
- System menu (in the world): the entry runs Exit Game, so the game saves and leaves the world as
  it always does. It tells the port first ("bb:p"); Exit Game's confirmation then asks the port
  ("bb:d") and shows the Quit to Desktop question. The port closes the game when the intro logos
  start loading, once the save is done (src/runtime_services.c), or at the latest when the
  title screen is built ("bb:t", the Play Online / Play Offline choice or the title menu).

The messages go through the game's sceSystemServiceLaunchWebBrowser import, which the port
answers itself (src/runtime_services.c). The new code lives past the end of the code segment, in
the rest of its last 4 KiB page (mapped with it, unused by the game; src/probe.c lets patches
write there). `text` writes the entries' texts as a mod layer (mods.py --layer): the game's menu
text archives with those strings added.
"""
import argparse
import os
import struct
import sys
import threading
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import hud_layout as swf  # noqa: E402  (SWF/GFX reading and writing)

EBOOT_BASE = 0x400000
# Image offsets (vaddr - EBOOT_BASE) in the 01.09 eboot.
BROWSER = 0x2BC09A8          # sceSystemServiceLaunchWebBrowser import stub
TEXT = 0x1AE8CC0             # text lookup (destination, FMG id, string id)
ADD_DELEGATE = 0x1B4C9A0     # list item with a {context, handler} callback (Exit Game's)
EXIT_HANDLER = 0x1C0C9F0     # Exit Game: confirmation, save, back to the title menu
OPINION_INVOKE = 0x1B46030   # the Opinion Box callback
OPINION_SLOT = 0x533B960     # its vtable slot
TITLE_CONDITION = 0x1B4ABF3  # call: is the Opinion Box shown
ONLINE_LABEL = 0x1B390A7     # call TEXT: "Play Online", the first screen after leaving the world
SYSTEM_ITEM = 0x1BB4412      # the Language item: its texts, then the item (Exit Game's is in r13)
SYSTEM_TEXT = 0x1BB441E      # SYSTEM_ITEM after its first two instructions
SYSTEM_KEEP = 0x1BB4561      # after the Language item's condition: the item stays
EXIT_DIALOG = 0x1C0CA1B      # Exit Game's confirmation text (mov esi, 0xcc; mov edx, 110000)
CODE_END = 0x50D96DC         # end of the code segment; up to its 4 KiB page end nothing is there
CAVE = 0x50D9700
CAVE_END = (CODE_END + 0xFFF) & ~0xFFF

# Text ids (menu.msgbnd.dcx): FMG 200 item names, 201 one-line help, 204 dialogs.
# SYSTEM_ID is free in 200 and 201 (110005 is Language's), DIALOG_ID in 204.
TITLE_ID, SYSTEM_ID, DIALOG_ID = 401307, 110006, 110005
ORIGINAL = {
    TITLE_CONDITION: 'e8c8eef9ff',
    ONLINE_LABEL: 'e814fcfaff',
    SYSTEM_ITEM: '4c8db5a0dfffff bec8000000 bab5ad0100',
    SYSTEM_KEEP: '488bbd90dfffff',
    EXIT_DIALOG: 'becc000000 bab0ad0100',
}


def rel32(at, end, target):
    return struct.pack('<i', target - (at + end))


class Code:
    """Machine code placed at an image offset, with rip-relative references."""

    def __init__(self, offset):
        self.offset, self.data, self.messages = offset, bytearray(), []

    @property
    def here(self):
        return self.offset + len(self.data)

    def raw(self, hexbytes):
        self.data += bytes.fromhex(hexbytes)

    def call(self, target):
        self.data += b'\xe8' + struct.pack('<i', target - (self.here + 5))

    def jmp(self, target):
        self.data += b'\xe9' + struct.pack('<i', target - (self.here + 5))

    def lea(self, prefix, target):  # prefix: '488d3d' (rdi), '488d05' (rax)
        self.data += bytes.fromhex(prefix) + struct.pack('<i', target - (self.here + 7))

    def browser(self, message):
        """Calls the browser import with message string number `message` (placed by link();
        8-byte aligned stack at the call)."""
        self.messages.append((len(self.data), message))
        self.raw('488d3d00000000')  # lea rdi, [rip + string]
        self.raw('31f6')  # xor esi, esi
        self.call(BROWSER)


def code_writes():
    """(offset, bytes) writes, the vtable slot holding a patch-base address (probe.c rebases it)."""
    cave = Code(CAVE)
    # Title menu condition: tell the port the title menu is being built; always show the entry.
    title_condition = cave.here
    cave.raw('4883ec08')       # sub rsp, 8
    cave.browser(0)
    cave.raw('4883c408b001c3')  # add rsp, 8; mov al, 1; ret
    # Title menu entry: close the game, then the entry's own job as before.
    title_invoke = cave.here
    cave.raw('57564883ec08')   # push rdi; push rsi; sub rsp, 8
    cave.browser(1)
    cave.raw('4883c4085e5f')   # add rsp, 8; pop rsi; pop rdi
    cave.jmp(OPINION_INVOKE)
    # System menu entry (handler(out, context) like Exit Game's): mark the quit, then Exit Game.
    system_handler = cave.here
    cave.raw('57564883ec08')
    cave.browser(2)
    cave.raw('4883c4085e5f')
    cave.jmp(EXIT_HANDLER)
    # Exit Game's confirmation text: Quit to Desktop's when the port says so.
    dialog = cave.here
    cave.raw('4883ec08')
    cave.browser(3)
    cave.raw('4883c408')
    cave.raw('becc000000bab0ad0100')  # mov esi, 0xcc; mov edx, 110000
    cave.raw('85c07405')               # test eax, eax; jz +5
    cave.raw('ba' + struct.pack('<I', DIALOG_ID).hex() + 'c3')  # mov edx, DIALOG_ID; ret
    # The Play Online / Play Offline label: the game is back at the title screen.
    online = cave.here
    cave.raw('575652')         # push rdi; push rsi; push rdx (the text lookup's arguments)
    cave.browser(0)
    cave.raw('5a5e5f')         # pop rdx; pop rsi; pop rdi
    cave.jmp(TEXT)
    # The System menu's Language item. Outside the world (r15b clear) the game's own, which
    # shows when the game allows it. In the world, where the game removes it, Quit to Desktop:
    # an item with a {context, handler} callback like Exit Game's, whose context is still at
    # [rbp-0x1fe0].
    system_item = cave.here
    cave.raw('4c8db5a0dfffff')  # lea r14, [rbp-0x2060] (the two instructions replaced)
    cave.raw('bec8000000')      # mov esi, 0xc8
    cave.raw('4584ff')          # test r15b, r15b
    cave.raw('7505')            # jnz quit
    cave.jmp(SYSTEM_TEXT)       # the game's Language item
    cave.raw('ba' + struct.pack('<I', SYSTEM_ID).hex())  # quit: mov edx, SYSTEM_ID
    cave.raw('4c89f7')          # mov rdi, r14
    cave.call(TEXT)
    cave.raw('488dbde0dfffff')  # lea rdi, [rbp-0x2020]
    cave.raw('bec9000000')      # mov esi, 0xc9
    cave.raw('ba' + struct.pack('<I', SYSTEM_ID).hex())
    cave.call(TEXT)
    cave.raw('488b8520e0ffff')  # mov rax, [rbp-0x1fe0]
    cave.raw('48898570dfffff')  # mov [rbp-0x2090], rax
    cave.lea('488d05', system_handler)
    cave.raw('48898578dfffff')  # mov [rbp-0x2088], rax
    cave.raw('488d9570dfffff')  # lea rdx, [rbp-0x2090]
    cave.raw('48899590dfffff')  # mov [rbp-0x2070], rdx
    cave.raw('31c94c89ef4c89f6')  # xor ecx, ecx; mov rdi, r13; mov rsi, r14
    cave.call(ADD_DELEGATE)
    cave.raw('4989c5')          # mov r13, rax
    cave.jmp(SYSTEM_KEEP)
    strings = cave.here
    for name in 'tqpd':
        cave.data += f'bb:{name}'.encode() + b'\0'
    for at, message in cave.messages:
        cave.data[at + 3:at + 7] = rel32(CAVE + at, 7, strings + 5 * message)
    if cave.here > CAVE_END:
        raise ValueError('quit menu code does not fit')

    item = Code(SYSTEM_ITEM)
    item.jmp(system_item)
    item.raw('90' * (SYSTEM_TEXT - item.here))
    title = Code(TITLE_CONDITION)
    title.call(title_condition)
    exit_dialog = Code(EXIT_DIALOG)
    exit_dialog.call(dialog)
    exit_dialog.raw('0f1f440000')
    online_label = Code(ONLINE_LABEL)
    online_label.call(online)
    return [(c.offset, bytes(c.data)) for c in (cave, item, title, exit_dialog, online_label)] + \
           [(OPINION_SLOT, struct.pack('<Q', EBOOT_BASE + title_invoke))]


def check_segments(segments):
    """segments: [(start, end)] of the eboot's PT_LOADs. Raises unless the code segment ends at
    CODE_END and nothing else lies in the rest of its page."""
    if not any(end == CODE_END for _, end in segments) or \
            any(start < CAVE_END and end > CODE_END for start, end in segments if end != CODE_END):
        raise ValueError('Quit to Desktop: unexpected code segment layout')


def check_original(image_bytes):
    """image_bytes(start, end): the eboot's memory image. Raises unless it is the 01.09 code."""
    for offset, expected in ORIGINAL.items():
        expected = bytes.fromhex(expected.replace(' ', ''))
        if image_bytes(offset, offset + len(expected)) != expected:
            raise ValueError(f'Quit to Desktop: unexpected code at {offset + EBOOT_BASE:#x}')


# ---- Menu texts -------------------------------------------------------------------------------
# folder: (entry name, title menu help, System menu help, confirmation)
TEXTS = {
    'engus': ('Quit to Desktop', 'Close the game.', 'Save and close the game.',
              'Quit the game and return to the desktop?'),
    'dandk': ('Afslut til skrivebordet', 'Luk spillet.', 'Gem og luk spillet.',
              'Afslut spillet og vend tilbage til skrivebordet?'),
    'deude': ('Zum Desktop beenden', 'Das Spiel schließen.', 'Speichern und das Spiel schließen.',
              'Spiel beenden und zum Desktop zurückkehren?'),
    'finfi': ('Lopeta työpöydälle', 'Sulje peli.', 'Tallenna ja sulje peli.',
              'Lopetetaanko peli ja palataanko työpöydälle?'),
    'frafr': ('Quitter vers le bureau', 'Fermer le jeu.', 'Sauvegarder et fermer le jeu.',
              'Quitter le jeu et revenir au bureau ?'),
    'itait': ('Esci al desktop', 'Chiudi il gioco.', 'Salva e chiudi il gioco.',
              'Uscire dal gioco e tornare al desktop?'),
    'jpnjp': ('デスクトップに戻る', 'ゲームを終了します', 'セーブしてゲームを終了します',
              'ゲームを終了してデスクトップに戻りますか？'),
    'korkr': ('바탕 화면으로 종료', '게임을 종료합니다.', '저장하고 게임을 종료합니다.',
              '게임을 종료하고 바탕 화면으로 돌아가시겠습니까?'),
    'nldnl': ('Afsluiten naar bureaublad', 'Het spel afsluiten.', 'Opslaan en het spel afsluiten.',
              'Het spel afsluiten en terugkeren naar het bureaublad?'),
    'norno': ('Avslutt til skrivebordet', 'Lukk spillet.', 'Lagre og lukk spillet.',
              'Avslutte spillet og gå tilbake til skrivebordet?'),
    'polpl': ('Wyjdź do pulpitu', 'Zamknij grę.', 'Zapisz i zamknij grę.',
              'Zakończyć grę i wrócić do pulpitu?'),
    'porbr': ('Sair para a área de trabalho', 'Fechar o jogo.', 'Salvar e fechar o jogo.',
              'Sair do jogo e voltar para a área de trabalho?'),
    'porpt': ('Sair para o ambiente de trabalho', 'Fechar o jogo.', 'Guardar e fechar o jogo.',
              'Sair do jogo e voltar ao ambiente de trabalho?'),
    'rusru': ('Выйти на рабочий стол', 'Закрыть игру.', 'Сохранить и закрыть игру.',
              'Выйти из игры на рабочий стол?'),
    'spaes': ('Salir al escritorio', 'Cerrar el juego.', 'Guardar y cerrar el juego.',
              '¿Salir del juego y volver al escritorio?'),
    'swese': ('Avsluta till skrivbordet', 'Stäng spelet.', 'Spara och stäng spelet.',
              'Avsluta spelet och återgå till skrivbordet?'),
    'turtr': ('Masaüstüne çık', 'Oyunu kapat.', 'Kaydet ve oyunu kapat.',
              'Oyundan çıkıp masaüstüne dönülsün mü?'),
    'zhocn': ('退出到桌面', '关闭游戏。', '保存并关闭游戏。',
              '要退出游戏并返回桌面吗？'),
    'zhotw': ('結束並返回桌面', '關閉遊戲。', '存檔並關閉遊戲。',
              '要結束遊戲並返回桌面嗎？'),
}
TEXTS['enggb'] = TEXTS['engus']
TEXTS['spaar'] = TEXTS['spaes']


def read_fmg(data):
    """{id: text or None} of an FMG (version 2: 64-bit string offsets)."""
    groups, = struct.unpack_from('<I', data, 0x0C)
    offsets, = struct.unpack_from('<Q', data, 0x18)
    strings = {}
    for g in range(groups):
        index, first, last, _ = struct.unpack_from('<iiii', data, 0x28 + 16 * g)
        for k, string_id in enumerate(range(first, last + 1)):
            at, = struct.unpack_from('<q', data, offsets + 8 * (index + k))
            if at <= 0:
                strings[string_id] = None
                continue
            end = at
            while data[end:end + 2] != b'\0\0':
                end += 2
            strings[string_id] = data[at:end].decode('utf-16-le')
    return strings


def write_fmg(template, strings):
    """An FMG like `template` (its header flags) holding `strings`."""
    ids = sorted(strings)
    groups = []
    for string_id in ids:
        if groups and groups[-1][2] == string_id - 1:
            groups[-1][2] = string_id
        else:
            groups.append([sum(g[2] - g[1] + 1 for g in groups), string_id, string_id])
    offsets_at = 0x28 + 16 * len(groups)
    text_at = offsets_at + 8 * len(ids)
    texts = bytearray()
    slots = []
    for string_id in ids:
        if strings[string_id] is None:
            slots.append(0)
            continue
        slots.append(text_at + len(texts))
        texts += strings[string_id].encode('utf-16-le') + b'\0\0'
    out = bytearray(template[:0x28])
    struct.pack_into('<I', out, 0x0C, len(groups))
    struct.pack_into('<I', out, 0x10, len(ids))
    struct.pack_into('<Q', out, 0x18, offsets_at)
    for index, first, last in groups:
        out += struct.pack('<iiii', index, first, last, 0)
    for slot in slots:
        out += struct.pack('<q', slot)
    out += texts
    out += b'\0' * (4 + (-(len(out) + 4) % 4))  # the game's files end in zeros like this
    struct.pack_into('<I', out, 0x04, len(out))
    return bytes(out)


def unpack_dcx(data):
    if data[:4] != b'DCX\0' or data[0x28:0x2C] != b'DFLT':
        raise ValueError('not a DFLT DCX archive')
    return zlib.decompress(data[0x4C:])


def pack_dcx(template, body):
    packed = zlib.compress(body, 9)
    header = bytearray(template[:0x4C])
    struct.pack_into('>II', header, 0x1C, len(body), len(packed))
    return bytes(header) + packed


# Level 9 is slow: the 22 language menu archives, packed one after another, took 5.5 s of every
# game start here and as much again in rebirth.py. zlib lets the other threads run while it
# compresses, so they are packed side by side (the same bytes on any thread). At most 8 at once:
# each archive in work holds about 12 MB.
PACK_THREADS = min(8, os.cpu_count() or 1)


def in_parallel(function, items):
    """function(item) for every item, in their order, on up to PACK_THREADS threads (for work that
    spends its time in zlib). A result comes once it and all before it are done, so a failure
    stops the caller at the same item as a plain loop; items not started by then are left out."""
    items = list(items)
    count = min(PACK_THREADS, len(items))
    if count < 2:
        yield from map(function, items)
        return
    # Plain threads, not concurrent.futures: Bloodborne.exe (PyInstaller) runs these scripts with
    # the standard modules it bundles, and concurrent is not among them.
    results, done = [None] * len(items), [threading.Event() for _ in items]
    order, lock, stop = iter(range(len(items))), threading.Lock(), threading.Event()

    def work():
        while not stop.is_set():
            with lock:
                i = next(order, None)
            if i is None:
                return
            try:
                results[i] = (True, function(items[i]))
            except BaseException as error:  # raised for the caller at its item
                results[i] = (False, error)
            done[i].set()
    threads = [threading.Thread(target=work, daemon=True) for _ in range(count)]
    for thread in threads:
        thread.start()
    try:
        for i in range(len(items)):
            done[i].wait()
            ok, value = results[i]
            results[i] = None  # the caller has it now
            if not ok:
                raise value
            yield value
    finally:
        stop.set()
        for thread in threads:
            thread.join()


def bnd4_files(data):
    """[(entry offset, data offset, size)] of a BND4 with 0x24-byte entries."""
    if data[:4] != b'BND4':
        raise ValueError('not a BND4 archive')
    count, = struct.unpack_from('<I', data, 0x0C)
    size, = struct.unpack_from('<Q', data, 0x20)
    if size != 0x24:
        raise ValueError(f'unsupported BND4 entry size {size:#x}')
    out = []
    for i in range(count):
        entry = 0x40 + i * size
        length, = struct.unpack_from('<Q', data, entry + 0x08)
        offset, = struct.unpack_from('<I', data, entry + 0x18)
        file_id, = struct.unpack_from('<I', data, entry + 0x1C)
        out.append((entry, offset, length, file_id))
    return out


def rebuild_bnd4(data, replace):
    """The BND4 with files {id: bytes} replaced; data re-laid out with the original alignment."""
    files = bnd4_files(data)
    start = min(offset for _, offset, _, _ in files)
    out = bytearray(data[:start])
    for entry, offset, length, file_id in sorted(files, key=lambda f: f[1]):
        body = replace.get(file_id, data[offset:offset + length])
        out += b'\0' * (-len(out) % 16)
        struct.pack_into('<QQI', out, entry + 0x08, len(body), len(body), len(out))
        out += body
    return bytes(out)


def menu_texts(archive, texts, extra=None):
    """menu.msgbnd.dcx with the Quit to Desktop strings, and `extra` ({FMG id: {text id: text}},
    PC Settings') when given."""
    name, title_help, system_help, question = texts
    bnd = unpack_dcx(archive)
    files = {file_id: bnd[offset:offset + length] for _, offset, length, file_id in bnd4_files(bnd)}
    if not {200, 201, 204} <= files.keys():
        raise ValueError('menu texts without the SP menu, help or dialog files')
    replace = {}
    for file_id, changes in ((200, {TITLE_ID: name, SYSTEM_ID: name}),
                             (201, {TITLE_ID: title_help, SYSTEM_ID: system_help}),
                             (204, {DIALOG_ID: question})):
        strings = read_fmg(files[file_id])
        strings.update(changes)
        strings.update((extra or {}).get(file_id, {}))
        replace[file_id] = write_fmg(files[file_id], strings)
    return pack_dcx(archive, rebuild_bnd4(bnd, replace))


# ---- Menu rows --------------------------------------------------------------------------------
# The title menu and the System menu show five rows and scroll; with Quit to Desktop they hold
# six. Their movies get a sixth row (and the System panel grows to fit it).
TITLE_MOVIES = ('title.gfx', 'title_dlc.gfx', 'title_dlc_eu.gfx')
# The System menu: optionsetting.gfx's Top panel. Depths in it: the screen dimming, the frame;
# in the frame: its background, and what sits below the rows (bottom edge, lower separator).
SYSTEM_MOVIE = 'optionsetting.gfx'
SYSTEM_PANEL = {'list': 'Top', 'dim': 1, 'frame': 3, 'background': 1, 'below': (17, 27)}


def placements_in(data, start, end):
    """{name or depth: (code, body, length, character, matrix)} of a timeline's first frame."""
    found = {}
    for code, _, body, length in swf.tags(data, start, end):
        if code == 1:
            break
        if code not in (26, 70):
            continue
        character, span, name = swf.place_fields(data, body, code)
        if not span:
            continue
        depth, = struct.unpack_from('<H', data, body + 1 + (code == 70))
        entry = (code, body, length, character, swf.read_matrix(data, span[0])[0])
        found[depth] = entry
        if name:
            found[name] = entry
    return found


def moved(matrix, dy):
    scale, rotate, (tx, ty) = matrix
    return (scale, rotate, (tx, ty + round(dy)))


def depth_of(data, body, code):
    return struct.unpack_from('<H', data, body + 1 + (code == 70))[0]


def shift_depth(data, timeline, depth, dy, edits):
    """Moves what sits at `depth` by `dy` twips in every frame of `timeline` (start, end)."""
    for code, _, body, length in swf.tags(data, *timeline):
        if code in (26, 70) and depth_of(data, body, code) == depth:
            _, span, _ = swf.place_fields(data, body, code)
            if span:
                edits[body] = moved(swf.read_matrix(data, span[0])[0], dy)


def row_count(rows):
    return next(n for n in range(64) if f'Item_{n}_0' not in rows)


def add_row(data, timeline, rows, inserts, after):
    """A next row (Item_5_0 after Item_4_0, ...) in `timeline` (start, end): its first placement
    and its animation (each later keyframe of the last row, one row lower) at a free depth below
    it. Returns the row step in twips."""
    last = row_count(rows) - 1
    name, new = f'Item_{last}_0\0'.encode(), f'Item_{last + 1}_0\0'.encode()
    code, body, length, _, matrix = rows[f'Item_{last}_0']
    step = matrix[2][1] - rows[f'Item_{last - 1}_0'][4][2][1]
    depth = depth_of(data, body, code)
    used = {depth_of(data, b, c) for c, _, b, _ in swf.tags(data, *timeline) if c in (26, 70)}
    free = next(d for d in range(depth + 1, depth + 64) if d not in used)

    def copy(code, body, length):
        record = bytearray(data[body:body + length])
        struct.pack_into('<H', record, 1 + (code == 70), free)
        _, span, _ = swf.place_fields(data, body, code)
        if span:
            matrix = swf.read_matrix(data, span[0])[0]
            record = record[:span[0] - body] + swf.write_matrix(moved(matrix, step)) + record[span[1] - body:]
        return swf.tag(code, bytes(record).replace(name, new))
    first = copy(code, body, length)
    if new not in first:
        raise ValueError('unexpected list row')
    inserts[timeline[0]] = inserts.get(timeline[0], b'') + first
    for c, _, b, n in swf.tags(data, *timeline):
        if b <= body:
            continue
        if c in (26, 70) and depth_of(data, b, c) == depth:
            after[b] = after.get(b, b'') + copy(c, b, n)
        elif c == 28 and struct.unpack_from('<H', data, b)[0] == depth:  # RemoveObject2
            after[b] = after.get(b, b'') + swf.tag(28, struct.pack('<H', free))
    return step


def taller_title(data):
    """title*.gfx: a sixth row under the title menu's five."""
    _, sprites, _ = swf.definitions(data)
    root = placements_in(data, swf.stage_start(data), len(data))
    top = placements_in(data, *sprites[root['Top'][3]])
    timeline = sprites[top['CommandList'][3]]
    rows = placements_in(data, *timeline)
    if 'Item_5_0' in rows:
        return None
    inserts, after = {}, {}
    add_row(data, timeline, rows, inserts, after)
    return rebuild(data, {}, inserts, after)


def taller_list(data, panel=SYSTEM_PANEL, rows_wanted=6):
    """The System menu panel: one more row (None once it has `rows_wanted`), what sits below the
    rows (frame edge, separator, help and button guide) one row lower, the panel half a row
    higher to stay centred. The panel animates as it opens: the moves apply to every frame."""
    _, sprites, _ = swf.definitions(data)
    start = swf.stage_start(data)
    root = placements_in(data, start, len(data))
    code, body, _, character, _ = root[panel['list']]
    timeline = sprites[character]
    rows = placements_in(data, *timeline)
    if row_count(rows) >= rows_wanted:
        return None
    dim, frame_place = rows.get(panel['dim']), rows.get(panel['frame'])
    if not dim or not frame_place or 'StatusBar' not in rows or frame_place[3] not in sprites:
        raise ValueError('unexpected list panel')
    frame = placements_in(data, *sprites[frame_place[3]])
    background = frame.get(panel['background'])
    below = [frame.get(depth) for depth in panel['below']]
    if not background or None in below:
        raise ValueError('unexpected list frame')
    inserts, after, edits = {}, {}, {}
    step = add_row(data, timeline, rows, inserts, after)
    shift_depth(data, (start, len(data)), depth_of(data, body, code), -step / 2, edits)  # centred
    shift_depth(data, timeline, panel['dim'], step / 2, edits)  # the screen dimming stays put
    shift_depth(data, timeline, depth_of(data, rows['StatusBar'][1], rows['StatusBar'][0]), step, edits)
    for entry in below:
        shift_depth(data, sprites[frame_place[3]], depth_of(data, entry[1], entry[0]), step, edits)
    # The background grows downward: its top edge (shape top, in twips) stays.
    shape_top, shape_bottom = swf_shape_y(data, background[3])
    scale, rotate, (tx, ty) = background[4]
    sx, sy = scale or (65536, 65536)
    grow = (shape_bottom - shape_top + step * 65536 / sy) / (shape_bottom - shape_top)
    edits[background[1]] = ((sx, round(sy * grow)), rotate,
                            (tx, round(ty + shape_top * sy / 65536 * (1 - grow))))
    return rebuild(data, edits, inserts, after)


def swf_shape_y(data, character):
    """A shape's (top, bottom) in twips; for a sprite, that of the shapes it places."""
    _, sprites, _ = swf.definitions(data)
    if character in sprites:
        spans = []
        for code, _, body, length in swf.tags(data, *sprites[character]):
            if code == 1:
                break
            if code in (26, 70):
                child, span, _ = swf.place_fields(data, body, code)
                if child is not None and child not in sprites:
                    dy = swf.read_matrix(data, span[0])[0][2][1] if span else 0
                    top, bottom = swf_shape_y(data, child)
                    spans.append((top + dy, bottom + dy))
        if not spans:
            raise ValueError(f'sprite {character} has no shapes')
        return min(t for t, _ in spans), max(b for _, b in spans)
    shape = character

    def scan(start, end):
        for code, _, body, length in swf.tags(data, start, end):
            if code in swf.SHAPE_TAGS and struct.unpack_from('<H', data, body)[0] == shape:
                r = swf.Reader(data, body + 2)
                n = r.unsigned(5)
                r.signed(n), r.signed(n)
                return r.signed(n), r.signed(n)
            if code == 39:
                found = scan(body + 4, body + length)
                if found:
                    return found
        return None
    found = scan(swf.stage_start(data), len(data))
    if not found:
        raise ValueError(f'shape {shape} not found')
    return found


def rebuild(data, edits, inserts, after=None):
    start = swf.stage_start(data)
    out = data[:start] + swf.rewrite_timeline(data, start, len(data), edits, inserts, after)
    return out[:4] + struct.pack('<I', len(out)) + out[8:]


def write_row_layer(game, out, quit=True, pc=False):
    """The title menu's sixth row (Quit to Desktop) and the System menu's rows: five, one more
    for Quit to Desktop and three for PC Settings' lines, whose page the settings screen gets too."""
    menu = game / 'dvdroot_ps4' / 'menu'
    target = out / 'dvdroot_ps4' / 'menu'
    target.mkdir(parents=True, exist_ok=True)
    system_rows = 5 + quit + 3 * pc
    movies = ([(n, taller_title) for n in TITLE_MOVIES] if quit else []) + \
        [(SYSTEM_MOVIE, lambda data: taller_list(data, rows_wanted=system_rows))]
    for name, grow in movies:
        source = menu / name
        if not source.is_file():
            continue
        data, grown = source.read_bytes(), False
        try:
            while (taller := grow(data)) is not None:
                data, grown = taller, True
        except (ValueError, KeyError, IndexError, StopIteration, struct.error) as error:
            print(f'Quit to Desktop: {name}: {error}; the list keeps five rows', file=sys.stderr)
            continue
        if pc and name == SYSTEM_MOVIE:
            import pc_menu
            try:
                data, grown = pc_menu.pc_pages(data) or data, True
            except (ValueError, KeyError, IndexError, StopIteration, struct.error) as error:
                print(f'PC Settings: {name}: {error}', file=sys.stderr)
        if grown:
            (target / name).write_bytes(data)
    print(f'Quit to Desktop: title and System menus with six rows' if quit and not pc else
          f'Menus: System menu with {system_rows} rows')


def write_text_layer(game, out, extra=None):
    msg = game / 'dvdroot_ps4' / 'msg'
    sources = [folder / 'menu.msgbnd.dcx' for folder in sorted(p for p in msg.iterdir() if p.is_dir())]
    sources = [source for source in sources if source.is_file()]

    def texts_for(source):
        return menu_texts(source.read_bytes(), TEXTS.get(source.parent.name.casefold(), TEXTS['engus']), extra)
    written = 0
    for archive, source in zip(in_parallel(texts_for, sources), sources):
        target = out / 'dvdroot_ps4' / 'msg' / source.parent.name / source.name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(archive)
        written += 1
    print(f'Quit to Desktop: menu texts for {written} languages')


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('game', type=Path, help='game folder (with dvdroot_ps4)')
    p.add_argument('--out', type=Path, required=True, help='mod layer folder to write')
    p.add_argument('--no-quit', action='store_true', help='without the Quit to Desktop rows')
    p.add_argument('--pc', action='store_true', help='with the PC Settings texts and System row')
    a = p.parse_args()
    extra = None
    if a.pc:
        import pc_menu
        extra = pc_menu.texts()
    write_text_layer(a.game, a.out, extra)
    write_row_layer(a.game, a.out, quit=not a.no_quit, pc=a.pc)


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, KeyError, struct.error, zlib.error) as error:
        # The game still starts; the entries then keep their old names.
        print(f'Quit to Desktop: menu texts: {error}', file=sys.stderr)
