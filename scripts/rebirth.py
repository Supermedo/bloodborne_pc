#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Rebirth at the Doll (bbport.ini rebirth, game version 01.09): a new choice in the Doll's menu
puts the character back to its origin's attributes and level for REBIRTH_INSIGHT Insight (never
below what the equipped weapons need: `weapons`), then opens the level-up screen, where the levels it had are bought again for free. No Blood Echoes
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
# level-up screen (reborn now, or levels still to regain), or nothing to do; CLOSED: the screen
# the rebirth opened was closed (the port undoes a rebirth no level was bought after): every
# choice in the Doll's list sets it on its way out (doll_script).
REQUEST, RESULT, NOTHING, CLOSED = 72100090, 72100091, 72100092, 72100093
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

# The Doll's Rebirth texts in each of the game's languages (msg/<folder>): the option, the Yes/No
# question, short of Insight, nothing to reset. {n} is REBIRTH_INSIGHT. The game's own words are
# used for Insight, Blood Echoes, Channel Blood Echoes and the origin (menu FMGs 2, 30 and 200).
TEXTS = {
    'engus': ('Rebirth',
              "Be reborn for {n} Insight? Your attributes and level return to your origin's, keeping what "
              'your equipped weapons need. Regain your levels without Blood Echoes, now or later with '
              'Channel Blood Echoes. Leaving without regaining a level cancels the rebirth.',
              'Rebirth requires {n} Insight.',
              'Your attributes are already those of your origin.'),
    'dandk': ('Genfødsel',
              'Bliv genfødt for {n} Indsigt? Dine attributter og dit niveau vender tilbage til din '
              'oprindelses, men det, dine udrustede våben kræver, bevares. Genvind dine niveauer uden '
              'blodekkoer, nu eller senere med Kanalisér blodekkoer. Går du uden at genvinde et niveau, '
              'annulleres genfødslen.',
              'Genfødsel kræver {n} Indsigt.',
              'Dine attributter er allerede din oprindelses.'),
    'deude': ('Wiedergeburt',
              'Für {n} Einsicht wiedergeboren werden? Deine Attribute und deine Stufe kehren zu denen '
              'deiner Herkunft zurück, was deine ausgerüsteten Waffen erfordern, bleibt erhalten. Gewinne '
              'deine Stufen ohne Blutechos zurück, jetzt oder später mit Blutechos kanalisieren. Gehst du, '
              'ohne eine Stufe zurückzugewinnen, wird die Wiedergeburt abgebrochen.',
              'Die Wiedergeburt erfordert {n} Einsicht.',
              'Deine Attribute entsprechen bereits deiner Herkunft.'),
    'finfi': ('Uudelleensyntyminen',
              'Synnytäänkö uudelleen {n} Oivalluksella? Ominaisuutesi ja tasosi palaavat taustasi '
              'mukaisiksi, mutta varustettujen aseidesi vaatimukset säilyvät. Saat tasosi takaisin ilman '
              'verihäiveitä nyt tai myöhemmin valinnalla Kanavoi verihäiveitä. Jos poistut saamatta tasoa '
              'takaisin, uudelleensyntyminen perutaan.',
              'Uudelleensyntyminen vaatii {n} Oivallusta.',
              'Ominaisuutesi ovat jo taustasi mukaiset.'),
    'frafr': ('Renaissance',
              'Renaître pour {n} points de Lucidité ? Vos attributs et votre niveau reviennent à ceux de '
              "votre origine, en gardant ce qu'exigent vos armes équipées. Regagnez vos niveaux sans "
              'échos de sang, maintenant ou plus tard avec Canaliser les échos du sang. Partir sans '
              'regagner de niveau annule la renaissance.',
              'La renaissance nécessite {n} points de Lucidité.',
              'Vos attributs sont déjà ceux de votre origine.'),
    'itait': ('Rinascita',
              'Rinascere per {n} Intuizione? Attributi e livello tornano a quelli della tua origine, '
              'mantenendo ciò che richiedono le armi equipaggiate. Recupera i livelli senza echi del '
              'sangue, ora o più tardi con Incanala echi del sangue. Uscire senza recuperare un livello '
              'annulla la rinascita.',
              'La rinascita richiede {n} Intuizione.',
              'I tuoi attributi sono già quelli della tua origine.'),
    'jpnjp': ('生まれ直す',
              '啓蒙を{n}消費して生まれ直しますか？能力とレベルが過去の値に戻ります（装備中の武器に必要な'
              '能力はそのまま残ります）。レベルは今または後で「血の遺志を力とする」から血の遺志なしで取り戻せ'
              'ます。レベルを取り戻さずに立ち去ると、生まれ直しは取り消されます。',
              '生まれ直すには啓蒙が{n}必要です。',
              '能力はすでに過去の値です。'),
    'korkr': ('다시 태어나기',
              '계몽을 {n} 소비해 다시 태어나겠습니까? 능력치와 레벨이 과거의 값으로 돌아갑니다(장비한 '
              '무기에 필요한 능력치는 유지됩니다). 레벨은 지금 또는 나중에 「피의 유지를 깃들인다」에서 피의 '
              '유지 없이 되찾을 수 있습니다. 레벨을 되찾지 않고 떠나면 다시 태어나기가 취소됩니다.',
              '다시 태어나려면 계몽이 {n} 필요합니다.',
              '능력치가 이미 과거의 값입니다.'),
    'nldnl': ('Wedergeboorte',
              'Herboren worden voor {n} Inzicht? Je attributen en niveau keren terug naar die van je '
              'herkomst, maar wat je uitgeruste wapens vereisen blijft. Win je niveaus terug zonder '
              'bloedsporen, nu of later met Bloedsporen kanaliseren. Weggaan zonder een niveau terug te '
              'winnen annuleert de wedergeboorte.',
              'Wedergeboorte vereist {n} Inzicht.',
              'Je attributen zijn al die van je herkomst.'),
    'norno': ('Gjenfødsel',
              'Bli gjenfødt for {n} Innsikt? Egenskapene og nivået ditt går tilbake til opphavets, men det '
              'de utrustede våpnene dine krever, beholdes. Vinn tilbake nivåene uten blodekko, nå eller '
              'senere med Kanaliser blodekko. Drar du uten å vinne tilbake et nivå, avbrytes gjenfødselen.',
              'Gjenfødsel krever {n} Innsikt.',
              'Egenskapene dine er allerede opphavets.'),
    'polpl': ('Odrodzenie',
              'Odrodzić się za {n} Wglądu? Atrybuty i poziom wrócą do wartości z pochodzenia, z '
              'zachowaniem wymagań wyposażonej broni. Odzyskaj poziomy bez tętnień krwi, teraz lub później '
              'przez Przepływ tętnień krwi. Wyjście bez odzyskania poziomu anuluje odrodzenie.',
              'Odrodzenie wymaga {n} Wglądu.',
              'Twoje atrybuty są już takie jak z pochodzenia.'),
    'porbr': ('Renascimento',
              'Renascer por {n} de discernimento? Seus atributos e nível voltam aos da sua origem, '
              'mantendo o que suas armas equipadas exigem. Recupere seus níveis sem ecos de sangue, agora '
              'ou depois com Canalizar ecos de sangue. Sair sem recuperar um nível cancela o renascimento.',
              'O renascimento requer {n} de discernimento.',
              'Seus atributos já são os da sua origem.'),
    'porpt': ('Renascimento',
              'Renascer por {n} de Iluminismo? Os teus atributos e nível voltam aos da tua origem, '
              'mantendo o que as tuas armas equipadas exigem. Recupera os teus níveis sem ecos de sangue, '
              'agora ou mais tarde com Canalizar ecos de sangue. Sair sem recuperar um nível cancela o '
              'renascimento.',
              'O renascimento requer {n} de Iluminismo.',
              'Os teus atributos já são os da tua origem.'),
    'rusru': ('Перерождение',
              'Переродиться за {n} озарения? Характеристики и уровень вернутся к значениям вашего '
              'происхождения, но требования экипированного оружия сохранятся. Уровни можно вернуть без '
              'отголосков крови сейчас или позже через Распределить отголоски крови. Если уйти, не вернув '
              'ни одного уровня, перерождение отменяется.',
              'Для перерождения нужно {n} озарения.',
              'Ваши характеристики уже соответствуют происхождению.'),
    'spaar': ('Renacimiento',
              '¿Renacer por {n} de Conocimiento? Tus atributos y nivel vuelven a los de tu origen, '
              'conservando lo que exigen tus armas equipadas. Recupera tus niveles sin ecos de sangre, '
              'ahora o más tarde con Canalizar ecos de sangre. Irte sin recuperar un nivel cancela el '
              'renacimiento.',
              'El renacimiento requiere {n} de Conocimiento.',
              'Tus atributos ya son los de tu origen.'),
    'spaes': ('Renacimiento',
              '¿Renacer por {n} de Lucidez? Tus atributos y nivel vuelven a los de tu origen, conservando '
              'lo que exigen tus armas equipadas. Recupera tus niveles sin ecos de sangre, ahora o más '
              'tarde con Canalizar ecos de sangre. Marcharte sin recuperar un nivel cancela el renacimiento.',
              'El renacimiento requiere {n} de Lucidez.',
              'Tus atributos ya son los de tu origen.'),
    'swese': ('Pånyttfödelse',
              'Bli pånyttfödd för {n} Insikt? Dina egenskaper och din nivå återgår till ditt ursprungs, '
              'men det dina utrustade vapen kräver behålls. Vinn tillbaka nivåerna utan blodekon, nu eller '
              'senare med Kanalisera blodekon. Lämnar du utan att vinna tillbaka en nivå avbryts '
              'pånyttfödelsen.',
              'Pånyttfödelse kräver {n} Insikt.',
              'Dina egenskaper är redan ditt ursprungs.'),
    'turtr': ('Yeniden Doğuş',
              '{n} Sezgi karşılığında yeniden doğmak ister misin? Özelliklerin ve seviyen kökenindeki '
              'değerlere döner, kuşandığın silahların gerektirdikleri korunur. Seviyelerini Kan Yankısı '
              "harcamadan şimdi ya da daha sonra Kan Yankıları'nı Aktar ile geri kazan. Hiç seviye geri "
              'kazanmadan ayrılırsan yeniden doğuş iptal edilir.',
              'Yeniden doğuş {n} Sezgi gerektirir.',
              'Özelliklerin zaten kökenindeki değerlerde.'),
    'zhocn': ('重生',
              '要消耗{n}点洞察力重生吗？属性与等级将恢复为背景的初始值，但保留已装备武器所需的属性。之后可'
              '通过“传导血之回响”，现在或以后无需血之回响即可恢复等级。若未恢复任何等级就离开，重生将被取消。',
              '重生需要{n}点洞察力。',
              '你的属性已是背景的初始值。'),
    'zhotw': ('重生',
              '要消耗{n}點靈視重生嗎？屬性與等級將恢復為背景的初始值，但保留已裝備武器所需的屬性。之後可'
              '透過「導引血之迴響」，現在或以後無需血之迴響即可恢復等級。若未恢復任何等級就離開，重生將被取消。',
              '重生需要{n}點靈視。',
              '你的屬性已是背景的初始值。'),
}
TEXTS['enggb'] = TEXTS['engus']


def doll_texts(language):
    """The four texts for msg/<language>, English for a folder without its own."""
    return tuple(t.format(n=REBIRTH_INSIGHT) for t in TEXTS.get(language.casefold(), TEXTS['engus']))


def code_writes():
    cave = quit_menu.Code(CAVE)
    # SetEventFlag: REQUEST goes to the port and RESULT is set to its answer; CLOSED tells the
    # port and is then set as usual.
    hook = cave.here
    cave.raw('81fe' + struct.pack('<I', REQUEST).hex())  # cmp esi, REQUEST
    to_rebirth = len(cave.data)
    cave.raw('7400')                                      # je rebirth
    cave.raw('81fe' + struct.pack('<I', CLOSED).hex())   # cmp esi, CLOSED
    to_closed = len(cave.data)
    cave.raw('7400')                                      # je closed
    original = cave.here
    cave.raw('4189d0 8b4f1c 31d2'.replace(' ', ''))       # the replaced instructions
    cave.jmp(SET_FLAG_RESUME)
    if cave.here - original != 13:
        raise ValueError('flag hook layout')
    # The port answers in eax: bit 0 opens the level-up screen (RESULT), bit 1 says there is
    # nothing to reset (NOTHING). Both flags are set through the original function.
    cave.data[to_rebirth + 1] = cave.here - (cave.offset + to_rebirth + 2)
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
    # closed: the port is told ("bb:e"), then the flag is set with its arguments kept.
    cave.data[to_closed + 1] = cave.here - (cave.offset + to_closed + 2)
    cave.raw('57 56 52'.replace(' ', ''))                 # push rdi; push rsi; push rdx (aligned)
    cave.browser(2)
    cave.raw('5a 5e 5f'.replace(' ', ''))                 # pop rdx; pop rsi; pop rdi
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
    for name in 'bfe':
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
        # The level-up screen, as Channel Blood Echoes opens it (c1_31, then back to the list). The
        # script can't see the screen close: f25(23) stays 0 while it is open (neither 1 nor any
        # other value shows up), so the 1.9.5 betas' CLOSED when f25(23) == 0 undid every rebirth
        # the moment the screen opened, which then showed the reborn character at full price and
        # kept it on leaving. The list waits for the screen instead (none of its choices passes
        # while the screen is open, or the game's own Channel Blood Echoes would go on under it):
        # see the end of this loop.
        level_up.entry = [command(1, 11, RESULT, 0), esd.Command(1, 31, [])]
        level_up.conditions = [esd.Condition(menu, call_equals(25, [23], 0))]
        short.entry = [command(1, 17, 1, SHORT_TEXT, 1, 0, 1)]
        short.conditions = [esd.Condition(menu, call_equals(21, [], 0))]
        nothing.entry = [command(1, 11, NOTHING, 0), command(1, 17, 1, NOTHING_TEXT, 1, 0, 1)]
        nothing.conditions = [esd.Condition(menu, call_equals(21, [], 0))]
        group.extend([ask, reborn, level_up, short, nothing])
        # Every way out of the list (a choice, Leave, backing out) sets CLOSED: the level-up screen
        # a rebirth opened is closed by then, and a rebirth no level was bought after is undone
        # (the port does nothing without one).
        for condition in choice.conditions:
            condition.commands.insert(0, command(1, 11, CLOSED, 1))
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
    written = write_texts(game, out, source)
    print(f'Rebirth: the Doll\'s menu and texts for {written} languages')


def menu_texts(archive, texts):
    """menu.msgbnd.dcx with the Rebirth strings (texts: a TEXTS entry)."""
    option, question, short, nothing = texts
    bnd = quit_menu.unpack_dcx(archive)
    texts = {file_id: bnd[o:o + n] for _e, o, n, file_id in quit_menu.bnd4_files(bnd)}
    strings = quit_menu.read_fmg(texts[TALK_FMG])
    strings.update({OPTION_TEXT: option, QUESTION_TEXT: question, SHORT_TEXT: short, NOTHING_TEXT: nothing})
    replaced = quit_menu.rebuild_bnd4(bnd, {TALK_FMG: quit_menu.write_fmg(texts[TALK_FMG], strings)})
    return quit_menu.pack_dcx(archive, replaced)


def write_texts(game, out, source=None):
    """Every language's menu texts (those of the layer `source` where it has them) with Rebirth's
    strings; returns how many languages. They are packed side by side (quit_menu.in_parallel)."""
    languages = []
    for folder in sorted(p for p in (Path(game) / 'dvdroot_ps4' / 'msg').iterdir() if p.is_dir()):
        name = folder.name
        layered = Path(source) / 'dvdroot_ps4' / 'msg' / name / 'menu.msgbnd.dcx' if source else None
        path = layered if layered and layered.is_file() else folder / 'menu.msgbnd.dcx'
        if path.is_file():
            languages.append((name, path))

    def texts_for(language):
        name, path = language
        return menu_texts(path.read_bytes(), doll_texts(name))
    written = 0
    for archive, (name, _path) in zip(quit_menu.in_parallel(texts_for, languages), languages):
        target = Path(out) / 'dvdroot_ps4' / 'msg' / name / 'menu.msgbnd.dcx'
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(archive)
        written += 1
    return written


# EquipParamWeapon (param/gameparam/gameparam.parambnd.dcx): each weapon's requirements, one byte
# each from +0xED: Strength, Skill, Bloodtinge, Arcane (the Saw Cleaver 8 7 0 0, the Holy
# Moonlight Sword 16 12 0 14). A weapon used below them breaks the game's attack animations (a
# T-pose on a hit): the game never lets that happen, but a rebirth lowers the attributes.
GAME_PARAMS = Path('dvdroot_ps4') / 'param' / 'gameparam' / 'gameparam.parambnd.dcx'
WEAPON_PARAMS = 'EquipParamWeapon.param'
REQUIREMENTS = 0xED


def weapons(game):
    """{weapon id: (Strength, Skill, Bloodtinge, Arcane)} of the weapons that need any."""
    from fov import bnd4_name
    bnd = quit_menu.unpack_dcx((Path(game) / GAME_PARAMS).read_bytes())
    for entry, offset, length, _file_id in quit_menu.bnd4_files(bnd):
        if bnd4_name(bnd, entry) != WEAPON_PARAMS:
            continue
        param = bnd[offset:offset + length]
        if param[0x0C:0x0C + 21] != b'EQUIP_PARAM_WEAPON_ST':
            raise ValueError('EquipParamWeapon is not an EQUIP_PARAM_WEAPON_ST table')
        count, = struct.unpack_from('<H', param, 0x0A)
        found = {}
        for i in range(count):
            row, data, _name = struct.unpack_from('<QQQ', param, 0x40 + i * 24)
            needs = tuple(param[data + REQUIREMENTS:data + REQUIREMENTS + 4])
            if any(needs):
                found[row & 0xFFFFFFFF] = needs
        return found
    raise ValueError(f'{WEAPON_PARAMS} not found')


def write_weapons(game, path):
    """The table src/runtime_rebirth.c reads (BB_REBIRTH_WEAPONS): "id str skill blood arcane"."""
    table = weapons(game)
    Path(path).write_text(''.join(f'{row} {" ".join(map(str, needs))}\n' for row, needs in sorted(table.items())),
                          encoding='ascii')
    print(f'Rebirth: requirements of {len(table)} weapons')


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('game', type=Path, help='game folder (with dvdroot_ps4)')
    p.add_argument('--out', type=Path, required=True, help='mod layer folder to write')
    p.add_argument('--source', type=Path, help='a layer whose menu texts to build on')
    p.add_argument('--weapons', type=Path, help="file for the weapons' requirements (src/runtime_rebirth.c)")
    a = p.parse_args()
    write_layer(a.game, a.out, a.source)
    if a.weapons:
        try:
            write_weapons(a.game, a.weapons)
        except (OSError, ValueError) as error:  # the rebirth then keeps no attribute for a weapon
            print(f"Rebirth: the weapons' requirements could not be read ({error})")


if __name__ == '__main__':
    main()
