from paths import ROOT
import os
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(ROOT / 'scripts'))
import esd  # noqa: E402
import patches  # noqa: E402
import quit_menu  # noqa: E402
import rebirth  # noqa: E402
from test_quit_menu import fmgs_of, menu_archive  # noqa: E402

GAME = Path(os.environ.get('BB_GAME_DIR', ROOT.parent / 'CUSA03173'))
TALK = GAME / 'dvdroot_ps4/script/talk' / rebirth.DOLL_SCRIPTS
ELF = ROOT / 'out/eboot.elf'


def talk_scripts():
    bnd = quit_menu.unpack_dcx(TALK.read_bytes())
    return {file_id: bnd[o:o + n] for _e, o, n, file_id in quit_menu.bnd4_files(bnd)}


class CodeTests(unittest.TestCase):
    def test_code_fits_and_links(self):
        writes = dict(rebirth.code_writes())
        cave = writes[rebirth.CAVE]
        self.assertGreaterEqual(rebirth.CAVE, quit_menu.CAVE + 0x200)  # past Quit to Desktop's code
        self.assertLessEqual(rebirth.CAVE + len(cave), quit_menu.CAVE_END)
        self.assertIn(b'bb:b\0', cave)
        self.assertIn(b'bb:f\0', cave)
        # Every hooked site calls (or jumps) into the new code, padded to what it replaced.
        for site, expected in rebirth.ORIGINAL.items():
            data = writes[site]
            self.assertEqual(len(data), len(bytes.fromhex(expected.replace(' ', ''))), hex(site))
            self.assertIn(data[0], (0xE8, 0xE9))
            target = site + 5 + struct.unpack_from('<i', data, 1)[0]
            self.assertTrue(rebirth.CAVE <= target < rebirth.CAVE + len(cave), hex(site))
            self.assertTrue(all(b == 0x90 for b in data[5:]))

    @unittest.skipUnless(ELF.is_file(), 'needs a run of the game (out/eboot.elf)')
    def test_hooked_code_is_the_games(self):
        elf = ELF.read_bytes()
        rebirth.check_original(lambda start, end: patches.eboot_bytes(elf, start, end))


class TextTests(unittest.TestCase):
    def test_texts_build_on_the_layer_as_one_language_after_another(self):
        with tempfile.TemporaryDirectory() as temp:
            game, layer, out = Path(temp) / 'game', Path(temp) / 'quit_text', Path(temp) / 'rebirth'

            def archive(base, name, label):
                path = base / 'dvdroot_ps4' / 'msg' / name / 'menu.msgbnd.dcx'
                path.parent.mkdir(parents=True)
                path.write_bytes(menu_archive({
                    rebirth.TALK_FMG: {rebirth.LEVEL_UP_TEXT + k: f'{label} {k}' for k in range(200)},
                    200: {401307: label},
                }))
                return path
            sources = {name: archive(game, name, f'{name} game') for name in ('deude', 'engus', 'frafr')}
            # Quit to Desktop's layer has its own archive for two of them: Rebirth builds on those.
            sources.update({name: archive(layer, name, f'{name} layer') for name in ('deude', 'frafr')})
            (game / 'dvdroot_ps4' / 'msg' / 'na').mkdir()  # no menu texts there
            with mock.patch.object(quit_menu, 'PACK_THREADS', 4):
                self.assertEqual(rebirth.write_texts(game, out, layer), 3)
            self.assertEqual(sorted(p.name for p in (out / 'dvdroot_ps4' / 'msg').iterdir()), sorted(sources))
            for name, source in sources.items():
                written = (out / 'dvdroot_ps4' / 'msg' / name / 'menu.msgbnd.dcx').read_bytes()
                self.assertEqual(written, rebirth.menu_texts(source.read_bytes(), rebirth.TEXTS['engus']), name)
                fmgs = fmgs_of(written)
                self.assertEqual(fmgs[200][401307], f'{name} {"layer" if name != "engus" else "game"}')
                self.assertEqual(fmgs[rebirth.TALK_FMG][rebirth.OPTION_TEXT], 'Rebirth')
                self.assertEqual(fmgs[rebirth.TALK_FMG][rebirth.LEVEL_UP_TEXT], fmgs_of(source.read_bytes())[
                    rebirth.TALK_FMG][rebirth.LEVEL_UP_TEXT])


@unittest.skipUnless(TALK.is_file(), 'needs the game (BB_GAME_DIR)')
class TalkScriptTests(unittest.TestCase):
    def test_hunters_dream_scripts_round_trip(self):
        for file_id, data in talk_scripts().items():
            self.assertEqual(esd.write(esd.read(data)), data, file_id)

    def test_rebirth_in_every_doll_menu_once(self):
        edited = {}
        for file_id, data in talk_scripts().items():
            script = rebirth.doll_script(data)
            if script != data:
                edited[file_id] = script
                self.assertEqual(rebirth.doll_script(script), script)  # applied once
        self.assertIn(18, edited)  # t210304, the Doll
        for script in edited.values():
            parsed = esd.read(script)
            for _group, menu, choice in rebirth.doll_menus(parsed):
                texts = [esd.disassemble(c.args[1]) for c in menu.entry if (c.bank, c.id) == (1, 19)]
                self.assertEqual(texts[texts.index(str(rebirth.LEVEL_UP_TEXT)) + 1], str(rebirth.OPTION_TEXT))
                evaluators = [esd.disassemble(c.evaluator) for c in choice.conditions]
                read = next(i for i, e in enumerate(evaluators) if '= f23()' in e)
                self.assertEqual(evaluators[read + 1], f'(reg0 == {rebirth.OPTION})')

    def test_the_level_up_screen_never_cancels_the_rebirth(self):
        # f25(23) stays 0 while the screen is open: a "closed" flag set on it undid every
        # rebirth at once (1.9.5 betas: the reborn character then paid full price and kept it).
        for data in talk_scripts().values():
            parsed = esd.read(rebirth.doll_script(data))
            for _gid, group in parsed.groups:
                for state in group:
                    for c in state.entry:
                        if (c.bank, c.id) == (1, 11):
                            self.assertNotEqual(esd.disassemble(c.args[0]), str(rebirth.CLOSED))

    def test_every_way_out_of_the_list_says_the_screen_is_closed(self):
        # The list's choices only pass once the level-up screen is gone: CLOSED goes there.
        for data in talk_scripts().values():
            for _group, _menu, choice in rebirth.doll_menus(esd.read(rebirth.doll_script(data))):
                self.assertTrue(choice.conditions)
                for condition in choice.conditions:
                    first = condition.commands[0]
                    self.assertEqual((first.bank, first.id), (1, 11))
                    self.assertEqual([esd.disassemble(a) for a in first.args], [str(rebirth.CLOSED), '1'])



@unittest.skipUnless((GAME / 'dvdroot_ps4/msg/engus').is_dir(), 'needs the game (BB_GAME_DIR)')
class TextTests(unittest.TestCase):
    def test_every_game_language_has_its_own_texts(self):
        folders = {p.name for p in (GAME / 'dvdroot_ps4/msg').iterdir() if (p / 'menu.msgbnd.dcx').is_file()}
        folders.discard('japanese')  # a second Japanese folder some dumps carry: English there
        self.assertEqual(folders - set(rebirth.TEXTS), set())
        for language in folders:
            option, question, short, nothing = rebirth.doll_texts(language)
            self.assertIn(str(rebirth.REBIRTH_INSIGHT), question + short, language)
            self.assertNotIn('{', question + short, language)


@unittest.skipUnless((GAME / rebirth.GAME_PARAMS).is_file(), 'needs the game (BB_GAME_DIR)')
class WeaponTests(unittest.TestCase):
    def test_requirements_are_the_games(self):
        weapons = rebirth.weapons(GAME)
        self.assertEqual(weapons[7000000], (8, 7, 0, 0))      # Saw Cleaver: Strength, Skill
        self.assertEqual(weapons[26000000], (16, 12, 0, 14))  # Holy Moonlight Sword: Arcane last
        self.assertNotIn(1000000, weapons)                    # Unarmed needs nothing
        self.assertTrue(all(len(n) == 4 and max(n) <= 99 for n in weapons.values()))

if __name__ == '__main__':
    unittest.main()
