#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Rebirth at the Doll (bbport.ini rebirth, game version 01.09): a new choice in the Doll's menu
puts the character back to its origin's attributes and level for REBIRTH_INSIGHT Insight, then
opens the level-up screen, where the levels it had are bought again for free. No Blood Echoes
come back: levels cost nothing only up to the level the character had (src/runtime_rebirth.c
keeps that target per character).

Three parts:
- The Doll's talk scripts (script/talk/m21_00_00_00.talkesdbnd.dcx: every list with Channel
  Blood Echoes, t210304's and others): "Rebirth" in the list, a Yes/No question, then the rebirth (event flag REQUEST, which the patch below turns into
  a call to the port), then the level-up screen on success or a message when Insight is short.
- The game's event flag setter: setting REQUEST calls the port ("bb:b") and sets RESULT to its
  answer instead. Every other flag is set as before.
- The level-up screen's pricing: its total, the check that the next level can be paid and the
  price it shows ask the port ("bb:f" with the level) whether that level is free, and count 0
  Blood Echoes if so.
`code_writes` gives the eboot patches; `main` writes the talk script and texts as a mod layer
(the menu texts on top of --source, Quit to Desktop's layer, which edits the same archive).
"""
import argparse
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import esd  # noqa: E402
import quit_menu  # noqa: E402

REBIRTH_INSIGHT = 5          # src/runtime_rebirth.c REBIRTH_INSIGHT
# Unused flags in the Doll's range (721000xx): the request, and the port's answer: open the
# level-up screen (reborn now, or levels still to regain), or nothing to do.
REQUEST, RESULT, NOTHING = 72100090, 72100091, 72100092
OPTION_TEXT, QUESTION_TEXT, SHORT_TEXT, NOTHING_TEXT = 15000170, 10010790, 10010791, 10010792  # free in FMG 30
TALK_FMG = 30
DOLL_SCRIPTS = 'm21_00_00_00.talkesdbnd.dcx'   # the Hunter's Dream: the Doll is t210304 and more
LEVEL_UP_TEXT = 15000100     # Channel Blood Echoes
OPTION = 4                   # the talk list's result for Rebirth (1-3 are the Doll's own)

# Image offsets in the 01.09 eboot.
BROWSER = quit_menu.BROWSER
SET_FLAG = 0x13CFCC0         # SetEventFlag(EventFlagMan*, flag, value)
SET_FLAG_RESUME = 0x13CFCC8  # after the instructions the jump replaces
RAISE = 0x169CA2D            # an older level-up screen: an attribute raised: price -> rax
LOWER = 0x169CA93            # an attribute lowered: price taken off
# The level-up screen the Doll opens (r12/r13: the screen; ebx = the priced level + 0x51):
TOTAL = 0x1B2E5C0            # the total cost, level by level: price -> rax, added to +0xEB0
AFFORD = 0x1B2F6DC           # can the next level be paid: price -> rcx, compared with eax
SHOWN = 0x1B2F9AA            # the next level's price shown -> r14
SHOWN_RED = 0x1B2FA51        # the same, to show it red when short -> rax
CAVE = 0x50D9900             # past Quit to Desktop's code in the code segment's last page
ORIGINAL = {
    SET_FLAG: '4189d0 8b4f1c 31d2',
    RAISE: 'c4e1fa2cc0 8b8bf8010000',
    LOWER: 'c4e1fa2cc0 2983f4010000',
    TOTAL: 'c4e1fa2cc0 41018424b00e0000',
    AFFORD: 'c4e1fa2cc8 39c1',
    SHOWN: 'c461fa2cf0',
    SHOWN_RED: 'c4e1fa2cc0',
}

TEXTS = {
    'engus': ('Rebirth',
              f'Be reborn for {REBIRTH_INSIGHT} Insight? Your attributes and level return to those of '
              f'your origin. Your levels can then be regained without Blood Echoes, now or later '
              f'with Channel Blood Echoes.',
              f'Rebirth requires {REBIRTH_INSIGHT} Insight.',
              'Your attributes are already those of your origin.'),
    'dandk': ('Genfødsel',
              f'Bliv genfødt for {REBIRTH_INSIGHT} indsigt? Dine egenskaber og dit niveau vender tilbage til '
              f'din oprindelses. Dine niveauer kan derefter genvindes uden blodekkoer, nu eller senere '
              f'via »Kanalisér blodekkoer«.',
              f'Genfødsel kræver {REBIRTH_INSIGHT} indsigt.',
              'Dine egenskaber svarer allerede til din oprindelses.'),
    'deude': ('Wiedergeburt',
              f'Für {REBIRTH_INSIGHT} Einsicht wiedergeboren werden? Deine Attribute und deine Stufe werden '
              f'auf die deiner Herkunft zurückgesetzt. Deine Stufen können danach ohne Blutechos wiedererlangt '
              f'werden, jetzt oder später über „Blutechos kanalisieren“.',
              f'Wiedergeburt erfordert {REBIRTH_INSIGHT} Einsicht.',
              'Deine Attribute entsprechen bereits denen deiner Herkunft.'),
    'finfi': ('Uudelleensyntymä',
              f'Synny uudelleen {REBIRTH_INSIGHT} oivalluksella? Ominaisuutesi ja tasosi palautuvat alkuperäsi '
              f'mukaisiksi. Tasosi voidaan tämän jälkeen palauttaa ilman verihäiveitä, nyt tai myöhemmin '
              f'kohdasta ”Kanavoi verihäiveitä”.',
              f'Uudelleensyntymä vaatii {REBIRTH_INSIGHT} oivallusta.',
              'Ominaisuutesi vastaavat jo alkuperääsi.'),
    'frafr': ('Renaissance',
              f'Renaître pour {REBIRTH_INSIGHT} point(s) de Lucidité ? Vos attributs et votre niveau '
              f'retourneront à ceux de votre origine. Vos niveaux pourront ensuite être regagnés sans échos '
              f'du sang, maintenant ou plus tard via « Canaliser les échos du sang ».',
              f'La renaissance requiert {REBIRTH_INSIGHT} point(s) de Lucidité.',
              'Vos attributs correspondent déjà à ceux de votre origine.'),
    'itait': ('Rinascita',
              f'Vuoi rinascere per {REBIRTH_INSIGHT} punti intuizione? I tuoi attributi e il tuo livello '
              f'torneranno a quelli della tua origine. I livelli potranno poi essere recuperati senza echi '
              f'del sangue, ora o in seguito tramite «Incanala echi del sangue».',
              f'La rinascita richiede {REBIRTH_INSIGHT} punti intuizione.',
              'I tuoi attributi corrispondono già a quelli della tua origine.'),
    'jpnjp': ('生まれ変わり',
              f'啓蒙を {REBIRTH_INSIGHT} 消費して生まれ変わりますか？能力値とレベルが過去の状態に戻ります。'
              f'消費したレベルは血の遺志を消費せずに再取得でき、今すぐ、または後から「血の遺志を力とする」で行えます。',
              f'生まれ変わりには啓蒙が {REBIRTH_INSIGHT} 必要です。',
              '能力値は既に過去の状態です。'),
    'korkr': ('다시 태어나기',
              f'계몽 {REBIRTH_INSIGHT}을(를) 소모하여 다시 태어나시겠습니까? 능력치와 레벨이 과거의 상태로 '
              f'되돌아갑니다. 원래의 레벨은 피의 유지 없이 무료로 다시 올릴 수 있으며, 지금 바로 또는 '
              f'나중에 「피의 유지를 깃들인다」를 통해 올릴 수 있습니다.',
              f'다시 태어나려면 계몽이 {REBIRTH_INSIGHT} 필요합니다.',
              '능력치가 이미 과거의 상태입니다.'),
    'nldnl': ('Wedergeboorte',
              f'Wedergeboren worden voor {REBIRTH_INSIGHT} inzicht? Je eigenschappen en niveau keren terug naar '
              f'die van je afkomst. Je niveaus kunnen danach zonder bloedsporen worden herwonnen, nu of later '
              f'via „Bloedsporen kanaliseren”.',
              f'Wedergeboorte vereist {REBIRTH_INSIGHT} inzicht.',
              'Je eigenschappen komen al overeen met die van je afkomst.'),
    'norno': ('Gjenfødelse',
              f'Bli gjenfødt for {REBIRTH_INSIGHT} innsikt? Dine egenskaper og nivå vender tilbake til din '
              f'opprinnelses. Nivåene kan deretter gjenvinnes uten blodekko, nå eller senere via '
              f'«Kanaliser blodekko».',
              f'Gjenfødelse krever {REBIRTH_INSIGHT} innsikt.',
              'Dine egenskaper tilsvarer allerede din opprinnelse.'),
    'polpl': ('Odrodzenie',
              f'Odrodzić się za {REBIRTH_INSIGHT} pkt. wglądu? Twoje atrybuty i poziom powrócą do stanu z Twojego '
              f'pochodzenia. Poziomy można odzyskać bez tętnień krwi, teraz lub później poprzez '
              f'«Przepływ tętnień krwi».',
              f'Odrodzenie wymaga {REBIRTH_INSIGHT} pkt. wglądu.',
              'Twoje atrybuty odpowiadają już Twojemu pochodzeniu.'),
    'porbr': ('Renascimento',
              f'Renascer por {REBIRTH_INSIGHT} de discernimento? Seus atributos e nível retornarão aos da sua '
              f'origem. Seus níveis poderão ser recuperados sem ecos de sangue, agora ou mais tarde através de '
              f'«Canalizar ecos de sangue».',
              f'O renascimento requer {REBIRTH_INSIGHT} de discernimento.',
              'Seus atributos já são os da sua origem.'),
    'porpt': ('Renascimento',
              f'Renascer por {REBIRTH_INSIGHT} de discernimento? Os teus atributos e nível regressarão aos da '
              f'tua origem. Os teus níveis poderão ser recuperados sem ecos de sangue, agora ou mais tarde através de '
              f'«Canalizar ecos de sangue».',
              f'O renascimento requer {REBIRTH_INSIGHT} de discernimento.',
              'Os teus atributos já são os da tua origem.'),
    'rusru': ('Перерождение',
              f'Переродиться за {REBIRTH_INSIGHT} озарений? Ваши характеристики и уровень вернутся к исходным '
              f'для вашего происхождения. Ваши уровни можно будет вернуть без отголосков крови, сейчас или '
              f'позже через «Распределить отголоски крови».',
              f'Для перерождения требуется {REBIRTH_INSIGHT} озарений.',
              'Ваши характеристики уже соответствуют вашему происхождению.'),
    'spaes': ('Renacer',
              f'¿Renacer por {REBIRTH_INSIGHT} de lucidez? Tus atributos y tu nivel volverán a los de tu origen. '
              f'Podrás recuperar tus niveles sin ecos de sangre, ahora o más adelante mediante '
              f'«Canalizar ecos de sangre».',
              f'Renacer requiere {REBIRTH_INSIGHT} de lucidez.',
              'Tus atributos ya corresponden a los de tu origen.'),
    'swese': ('Återfödelse',
              f'Återfödas för {REBIRTH_INSIGHT} insikt? Dina attribut och din nivå återgår till ditt ursprungs. '
              f'Dina nivåer kan sedan återfås utan blodekon, nu eller senare via ”Kanalisera blodekon”.',
              f'Återfödelse kräver {REBIRTH_INSIGHT} insikt.',
              'Dina attribut motsvarar redan ditt ursprungs.'),
    'turtr': ('Yeniden Doğuş',
              f'{REBIRTH_INSIGHT} Sezgi karşılığında yeniden doğulsun mu? Nitelikleriniz ve seviyeniz kökeninize '
              f'geri döner. Seviyeleriniz daha sonra Kan Yankıları harcamadan, şimdi veya daha sonra '
              f'«Kan Yankıları\'ni Aktar» ile geri kazanılabilir.',
              f'Yeniden doğuş için {REBIRTH_INSIGHT} Sezgi gerekir.',
              'Nitelikleriniz zaten kökeninizle aynı.'),
    'zhocn': ('重生',
              f'是否消耗 {REBIRTH_INSIGHT} 点灵视重生？能力值与等级将回到出身时的状态。此后可无需血之回响'
              f'重新提升等级，可立即提升，也可稍后通过“传导血之回响”提升。',
              f'重生需要 {REBIRTH_INSIGHT} 点灵视。',
              '能力值已处于出身时的状态。'),
    'zhotw': ('重生',
              f'是否消耗 {REBIRTH_INSIGHT} 點靈視重生？能力值與等級將回到出身時的狀態。此後可無需血之迴響'
              f'重新提升等級，可立即提升，亦可稍後透過「導引血之迴響」提升。',
              f'重生需要 {REBIRTH_INSIGHT} 點靈視。',
              '能力值已處於出身時的狀態。'),
}
TEXTS['enggb'] = TEXTS['engus']
TEXTS['spaar'] = TEXTS['spaes']
TEXTS['japanese'] = TEXTS['jpnjp']


def code_writes():
    cave = quit_menu.Code(CAVE)
    # SetEventFlag: REQUEST goes to the port; RESULT is set to its answer.
    hook = cave.here
    cave.raw('81fe' + struct.pack('<I', REQUEST).hex())  # cmp esi, REQUEST
    cave.raw('740d')                                      # je rebirth
    original = cave.here
    cave.raw('4189d0 8b4f1c 31d2'.replace(' ', ''))       # the replaced instructions
    cave.jmp(SET_FLAG_RESUME)
    if cave.here - original != 13:
        raise ValueError('flag hook layout')
    # The port answers in eax: bit 0 opens the level-up screen (RESULT), bit 1 says there is
    # nothing to reset (NOTHING). Both flags are set through the original function.
    cave.raw('57')                                        # rebirth: push rdi (aligned)
    cave.browser(0)
    cave.raw('5f')                                        # pop rdi
    cave.raw('57 50 4883ec08'.replace(' ', ''))           # push rdi; push rax; sub rsp, 8
    cave.raw('89c2 83e201'.replace(' ', ''))              # mov edx, eax; and edx, 1
    cave.raw('be' + struct.pack('<I', RESULT).hex())      # mov esi, RESULT
    cave.call(original)
    cave.raw('4883c408 58 5f'.replace(' ', ''))           # add rsp, 8; pop rax; pop rdi
    cave.raw('89c2 d1ea 83e201'.replace(' ', ''))         # mov edx, eax; shr edx, 1; and edx, 1
    cave.raw('be' + struct.pack('<I', NOTHING).hex())     # mov esi, NOTHING
    cave.jmp(original)
    # Pricing: rax = the price; 0 when the port says the level is free. r13d is the price's
    # level + 0x51 when raising (the screen's level + 0x51), and the screen's level + 0x50 when
    # lowering: either way the level being bought or given back is r13d - 0x51.
    def price(level_fix, tail):
        at = cave.here
        cave.raw('c4e1fa2cc0')  # vcvttss2si rax, xmm0
        cave.raw('50')          # push rax (aligned for the call)
        cave.raw('4489ee')      # mov esi, r13d
        cave.raw(level_fix)
        # The browser call with esi kept (quit_menu.Code.browser clears it).
        cave.messages.append((len(cave.data), 1))
        cave.raw('488d3d00000000')  # lea rdi, [rip + "bb:f"]
        cave.call(BROWSER)
        cave.raw('89c1')        # mov ecx, eax
        cave.raw('58')          # pop rax
        cave.raw('85c9 7402 31c0'.replace(' ', ''))  # test ecx, ecx; jz +2; xor eax, eax
        cave.raw(tail)
        cave.raw('c3')
        return at
    raise_price = price('83ee51', '8b8bf8010000')      # sub esi, 0x51 | mov ecx, [rbx+0x1f8]
    lower_price = price('83ee51', '2983f4010000')      # sub esi, 0x51 | sub [rbx+0x1f4], eax

    # is_free: esi = a level; eax = 1 when the port gives it free. Any stack alignment; every
    # register but rax and the flags (and the vector registers) is kept.
    is_free = cave.here
    cave.raw('55 4889e5 4883e4f0'.replace(' ', ''))          # push rbp; mov rbp, rsp; and rsp, -16
    cave.raw('51 52 56 57 4150 4151 4152 4153'.replace(' ', ''))  # push rcx rdx rsi rdi r8-r11
    cave.messages.append((len(cave.data), 1))
    cave.raw('488d3d00000000')                               # lea rdi, [rip + "bb:f"]
    cave.call(BROWSER)
    cave.raw('415b 415a 4159 4158 5f 5e 5a 59'.replace(' ', ''))  # pop r11-r8 rdi rsi rdx rcx
    cave.raw('4889ec 5d c3'.replace(' ', ''))                # mov rsp, rbp; pop rbp; ret

    def screen_price(convert, zero, tail=''):
        # The replaced conversion, then the price (in the register `zero` clears) is 0 when the
        # level ebx - 0x51 is free; `tail` is the instruction the call also replaced.
        at = cave.here
        cave.raw(convert)
        cave.raw('50 89de 83ee51'.replace(' ', ''))           # push rax; mov esi, ebx; sub esi, 0x51
        cave.call(is_free)
        cave.raw('85c0 58'.replace(' ', ''))                   # test eax, eax; pop rax
        cave.raw('74' + format(len(bytes.fromhex(zero)), '02x') + zero)  # jz +n; zero
        cave.raw(tail + 'c3')
        return at
    total_price = screen_price('c4e1fa2cc0', '31c0', '41018424b00e0000')  # add [r12+0xeb0], eax
    afford_price = screen_price('c4e1fa2cc8', '31c9', '39c1')             # cmp ecx, eax
    shown_price = screen_price('c461fa2cf0', '4531f6')                    # xor r14d, r14d
    shown_red_price = screen_price('c4e1fa2cc0', '31c0')
    strings = cave.here
    for name in 'bf':
        cave.data += f'bb:{name}'.encode() + b'\0'
    for at, message in cave.messages:
        cave.data[at + 3:at + 7] = quit_menu.rel32(CAVE + at, 7, strings + 5 * message)
    if cave.here > quit_menu.CAVE_END:
        raise ValueError('rebirth code does not fit')

    flag = quit_menu.Code(SET_FLAG)
    flag.jmp(hook)
    flag.raw('909090')
    writes = [(cave.offset, bytes(cave.data)), (flag.offset, bytes(flag.data))]
    for site, target, length in ((RAISE, raise_price, 11), (LOWER, lower_price, 11), (TOTAL, total_price, 13),
                                 (AFFORD, afford_price, 7), (SHOWN, shown_price, 5), (SHOWN_RED, shown_red_price, 5)):
        call = quit_menu.Code(site)
        call.call(target)
        call.raw('90' * (length - 5))
        writes.append((call.offset, bytes(call.data)))
    return writes


def check_original(image_bytes):
    for offset, expected in ORIGINAL.items():
        expected = bytes.fromhex(expected.replace(' ', ''))
        if image_bytes(offset, offset + len(expected)) != expected:
            raise ValueError(f'Rebirth: unexpected code at {offset + quit_menu.EBOOT_BASE:#x}')


# ---- The Doll's talk script -------------------------------------------------------------------

def call_equals(function, args, value):
    """Evaluator: f<function>(args) == value."""
    code = esd.push_int(function) + b''.join(esd.push_int(a) for a in args)
    return code + bytes([0x84 + len(args), 0xA6]) + esd.push_int(value) + b'\x95\xa1'


def command(bank, id, *args):
    return esd.Command(bank, id, [esd.int_arg(a) for a in args])


def doll_menus(script):
    """(group, menu state, choice state) of every talk list with Channel Blood Echoes: the state
    adding the list, and the one reading the player's choice into reg0 (f23) after it."""
    found = []
    for _gid, group in script.groups:
        for menu in group:
            if not any((c.bank, c.id) == (1, 19) and esd.disassemble(c.args[1]) == str(LEVEL_UP_TEXT)
                       for c in menu.entry):
                continue
            # The choice: a few unconditional steps on from the list.
            seen, frontier, choice = set(), [menu], None
            for _depth in range(4):
                following = []
                for st in frontier:
                    for c in st.conditions:
                        if c.target is not None and id(c.target) not in seen:
                            seen.add(id(c.target))
                            following.append(c.target)
                for st in following:
                    if any('= f23()' in esd.disassemble(c.evaluator) for c in st.conditions):
                        choice = st
                        break
                if choice or not following:
                    break
                frontier = following
            if choice:
                found.append((group, menu, choice))
    return found


def doll_script(data):
    """A Hunter's Dream talk script with Rebirth in each of the Doll's lists (data unchanged when
    it has none, or has it already)."""
    script = esd.read(data)
    menus = doll_menus(script)
    if not menus or any(esd.disassemble(c.args[1]) == str(OPTION_TEXT)
                        for _g, menu, _c in menus for c in menu.entry if (c.bank, c.id) == (1, 19)):
        return data
    for group, menu, choice in menus:
        used = {esd.disassemble(c.args[0]) for c in menu.entry if (c.bank, c.id) == (1, 19)}
        if str(OPTION) in used:
            raise ValueError(f'talk list entry {OPTION} already used')
        next_id = max(st.id for st in group) + 1
        ask, reborn, level_up, short, nothing = (esd.State(next_id + i) for i in range(5))
        # Rebirth right after Channel Blood Echoes.
        at = next(i for i, c in enumerate(menu.entry)
                  if (c.bank, c.id) == (1, 19) and esd.disassemble(c.args[1]) == str(LEVEL_UP_TEXT))
        menu.entry.insert(at + 1, command(1, 19, OPTION, OPTION_TEXT, -1))
        # reg0 == OPTION, right after the condition that reads the choice into reg0.
        read = next(i for i, c in enumerate(choice.conditions) if '= f23()' in esd.disassemble(c.evaluator))
        choice.conditions.insert(read + 1, esd.Condition(ask, b'\xaf' + esd.push_int(OPTION) + b'\x95\xa1'))
        # Yes/No (as m21_01's "Return to the Hunter's Dream?"): f22 is the button, f21 the dialog.
        ask.entry = [command(1, 17, 2, QUESTION_TEXT, 3, 4, 2)]
        ask.conditions = [esd.Condition(reborn, call_equals(22, [], 1)), esd.Condition(menu, call_equals(21, [], 0))]
        # The rebirth: the port answers through RESULT.
        reborn.entry = [command(1, 11, REQUEST, 1)]
        reborn.conditions = [esd.Condition(level_up, call_equals(15, [RESULT], 1)),
                             esd.Condition(nothing, call_equals(15, [NOTHING], 1)),
                             esd.Condition(short, b'\x41\xa1')]
        # The level-up screen, as Channel Blood Echoes opens it (c1_31, open while f25(23) != 0).
        level_up.entry = [command(1, 11, RESULT, 0), esd.Command(1, 31, [])]
        level_up.conditions = [esd.Condition(menu, call_equals(25, [23], 0))]
        short.entry = [command(1, 17, 1, SHORT_TEXT, 1, 0, 1)]
        short.conditions = [esd.Condition(menu, call_equals(21, [], 0))]
        nothing.entry = [command(1, 11, NOTHING, 0), command(1, 17, 1, NOTHING_TEXT, 1, 0, 1)]
        nothing.conditions = [esd.Condition(menu, call_equals(21, [], 0))]
        group.extend([ask, reborn, level_up, short, nothing])
    return esd.write(script)


def write_layer(game, out, source=None):
    dvd = Path(game) / 'dvdroot_ps4'
    talk = dvd / 'script' / 'talk' / DOLL_SCRIPTS
    archive = talk.read_bytes()
    bnd = quit_menu.unpack_dcx(archive)
    edited = {}
    for _e, o, n, file_id in quit_menu.bnd4_files(bnd):
        script = doll_script(bnd[o:o + n])
        if script != bnd[o:o + n]:
            edited[file_id] = script
    if not edited:
        raise ValueError("no Doll menu found in the Hunter's Dream scripts")
    target = Path(out) / 'dvdroot_ps4' / 'script' / 'talk' / DOLL_SCRIPTS
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(quit_menu.pack_dcx(archive, quit_menu.rebuild_bnd4(bnd, edited)))
    print(f'Rebirth: Doll menus in {len(edited)} talk scripts')
    written = 0
    for folder in sorted(p for p in (dvd / 'msg').iterdir() if p.is_dir()):
        name = folder.name
        layered = Path(source) / 'dvdroot_ps4' / 'msg' / name / 'menu.msgbnd.dcx' if source else None
        path = layered if layered and layered.is_file() else folder / 'menu.msgbnd.dcx'
        if not path.is_file():
            continue
        option, question, short, nothing = TEXTS.get(name.casefold(), TEXTS['engus'])
        archive = path.read_bytes()
        bnd = quit_menu.unpack_dcx(archive)
        texts = {file_id: bnd[o:o + n] for _e, o, n, file_id in quit_menu.bnd4_files(bnd)}
        strings = quit_menu.read_fmg(texts[TALK_FMG])
        strings.update({OPTION_TEXT: option, QUESTION_TEXT: question, SHORT_TEXT: short, NOTHING_TEXT: nothing})
        replaced = quit_menu.rebuild_bnd4(bnd, {TALK_FMG: quit_menu.write_fmg(texts[TALK_FMG], strings)})
        target = Path(out) / 'dvdroot_ps4' / 'msg' / name / 'menu.msgbnd.dcx'
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(quit_menu.pack_dcx(archive, replaced))
        written += 1
    print(f'Rebirth: the Doll\'s menu and texts for {written} languages')


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('game', type=Path, help='game folder (with dvdroot_ps4)')
    p.add_argument('--out', type=Path, required=True, help='mod layer folder to write')
    p.add_argument('--source', type=Path, help='a layer whose menu texts to build on')
    a = p.parse_args()
    write_layer(a.game, a.out, a.source)


if __name__ == '__main__':
    main()
