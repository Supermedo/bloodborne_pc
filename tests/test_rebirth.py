from paths import ROOT
import os
from pathlib import Path
import struct
import sys
import unittest

sys.path.insert(0, str(ROOT / 'scripts'))
import esd  # noqa: E402
import patches  # noqa: E402
import quit_menu  # noqa: E402
import rebirth  # noqa: E402

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




class TextTests(unittest.TestCase):
    def test_texts_defined_for_all_supported_languages(self):
        expected_languages = [
            'engus', 'enggb', 'zhocn', 'zhotw', 'jpnjp', 'japanese', 'korkr',
            'rusru', 'deude', 'frafr', 'itait', 'spaes', 'spaar', 'porbr', 'porpt',
            'polpl', 'dandk', 'finfi', 'nldnl', 'norno', 'swese', 'turtr'
        ]
        for lang in expected_languages:
            with self.subTest(language=lang):
                self.assertIn(lang, rebirth.TEXTS)
                entries = rebirth.TEXTS[lang]
                self.assertEqual(len(entries), 4)
                option, question, short, nothing = entries
                self.assertTrue(all(isinstance(s, str) and len(s.strip()) > 0 for s in entries))
                self.assertIn(str(rebirth.REBIRTH_INSIGHT), question)
                self.assertIn(str(rebirth.REBIRTH_INSIGHT), short)


if __name__ == '__main__':
    unittest.main()
