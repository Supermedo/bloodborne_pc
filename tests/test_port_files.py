"""The game files the port changes (scripts/port_files.py) and mods: the port's layers are built on a mod's
version of a file and go above the mods, so both keep what they change; where a mod's file cannot take the
port's change, that feature is off (no menu row without its text, R55) and out/mod-report.json says why."""
from paths import ROOT
import importlib.util
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

import fov
import port_files
import quit_menu
import rebirth
from test_fov import archive, fov_values, lock_cam_param, tables
from test_quit_menu import fmgs_of, menu_archive

spec = importlib.util.spec_from_file_location('bbport_mods_for_port_files', ROOT / 'scripts/mods.py')
mods = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mods)
spec = importlib.util.spec_from_file_location('bbport_run_for_port_files', ROOT / 'run.py')
run = importlib.util.module_from_spec(spec)
spec.loader.exec_module(run)

TEXTS = 'dvdroot_ps4/msg/{}/menu.msgbnd.dcx'


def texts(label):
    """A menu text archive with the files Quit to Desktop, PC Settings and Rebirth add to."""
    return menu_archive({
        200: {401300: f'{label} item', 10200: f'{label} Level'},
        201: {401300: f'{label} help', 110005: 'Language'},
        204: {110000: 'Return to title screen?'},
        rebirth.TALK_FMG: {15000100: f'{label} Channel Blood Echoes'},
    })


def write(path, data):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)
    return path


class TableTests(unittest.TestCase):
    def test_the_generators_names_are_the_tables(self):
        for name in quit_menu.TITLE_MOVIES:
            self.assertTrue(port_files.in_layer('quit_text', f'dvdroot_ps4/menu/{name}'), name)
        self.assertTrue(port_files.in_layer('quit_text', f'dvdroot_ps4/menu/{quit_menu.SYSTEM_MOVIE}'))
        self.assertTrue(port_files.in_layer('rebirth', rebirth.DOLL_SCRIPTS_PATH))
        self.assertTrue(port_files.in_layer('fov', fov.ARCHIVE.as_posix()))
        self.assertTrue(port_files.in_layer('hud_layout', 'dvdroot_ps4/menu/fe.gfx'))
        self.assertIn(('rebirth', 'reads'), [(f.key, how) for f, how in port_files.uses(rebirth.GAME_PARAMS)])
        # run.py builds exactly the layers of the table, in its order
        source = (ROOT / 'run.py').read_text(encoding='utf-8')
        self.assertIn('layers = (text_layer, rebirth_layer, hud_layer, fov_layer)', source)
        self.assertEqual(port_files.LAYERS, ('quit_text', 'rebirth', 'hud_layout', 'fov'))
        for layer in port_files.LAYERS:
            self.assertIn(f"out / '{layer}'", source)

    def test_paths_match_whatever_the_case(self):
        self.assertTrue(port_files.watched('dvdroot_ps4/MSG/EngUS/Menu.msgbnd.dcx'))
        self.assertTrue(port_files.watched(Path('dvdroot_ps4') / 'menu' / 'OptionSetting.gfx'))
        self.assertFalse(port_files.watched('dvdroot_ps4/chr/c0000.chrbnd.dcx'))
        self.assertEqual({f.key for f, _how in port_files.uses('dvdroot_ps4/msg/deude/menu.msgbnd.dcx')},
                         {'quit_to_desktop', 'pc_menu', 'rebirth', 'discord'})

    def test_the_first_layer_with_the_file_wins_then_the_game(self):
        with tempfile.TemporaryDirectory() as temp:
            game, port, mod = (Path(temp) / name for name in ('game', 'port', 'mod'))
            write(game / TEXTS.format('engus'), b'game')
            write(mod / 'dvdroot_ps4/MSG/engus/MENU.msgbnd.dcx', b'mod')
            self.assertEqual(port_files.source(game, TEXTS.format('engus'), port, mod).read_bytes(), b'mod')
            write(port / TEXTS.format('engus'), b'port')
            self.assertEqual(port_files.source(game, TEXTS.format('engus'), port, mod).read_bytes(), b'port')
            self.assertEqual(port_files.source(game, TEXTS.format('engus')).read_bytes(), b'game')
            self.assertFalse(port_files.from_layer(port_files.source(game, TEXTS.format('engus')), game))
            self.assertTrue(port_files.from_layer(port_files.source(game, TEXTS.format('engus'), mod), game))


class ModSourceTests(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        self.game = self.root / 'CUSA03173'
        write(self.game / 'eboot.bin', b'eboot')
        write(self.game / TEXTS.format('engus'), texts('game'))
        write(self.game / 'dvdroot_ps4/chr/c0000.chrbnd.dcx', b'hunter')
        self.mods = self.root / 'mods'

    def mod(self, name, files):
        for relative, data in files.items():
            write(self.mods / name / relative, data)

    def test_sources_hold_only_the_files_the_port_changes_in_the_games_spelling(self):
        self.mod('A', {'dvdroot_ps4/msg/ENGUS/menu.msgbnd.dcx': texts('A'), 'dvdroot_ps4/chr/c0000.chrbnd.dcx': b'A'})
        self.mod('B', {'dvdroot_ps4/msg/engus/menu.msgbnd.dcx': texts('B')})
        folder = self.root / 'out' / 'mod_sources'
        with mock.patch('sys.stderr'):
            mods.write_sources(self.game, mods.mod_layers(self.game, self.mods), folder)
        report = json.loads((folder / 'sources.json').read_text(encoding='utf-8'))
        self.assertEqual(report['files'], {TEXTS.format('engus'): 'B'})
        self.assertEqual(report['mods'], ['A', 'B'])
        self.assertEqual(report['taken'], [['dvdroot_ps4/msg/engus/menu.msgbnd.dcx', 'A', 'B']])
        self.assertEqual((folder / TEXTS.format('engus')).read_bytes(), texts('B'))
        self.assertFalse((folder / 'dvdroot_ps4/chr').exists())

    def test_the_ports_layers_win_over_the_mods_from_the_top(self):
        self.mod('A', {'dvdroot_ps4/msg/engus/menu.msgbnd.dcx': b'mod'})
        layer = self.root / 'out' / 'quit_text'
        write(layer / TEXTS.format('engus'), b'port on the mod')

        def view(place):
            result = subprocess.run([sys.executable, str(ROOT / 'scripts/mods.py'), str(self.game), '--out',
                                     str(self.root / 'out'), '--mods-dir', str(self.mods), place, str(layer)],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            made = Path(result.stdout.strip())
            content = (made / TEXTS.format('engus')).read_bytes()
            mods.remove_overlay(made, self.game)
            return content
        self.assertEqual(view('--top-layer'), b'port on the mod')
        self.assertEqual(view('--layer'), b'mod')  # below the mods, as before

    def test_the_text_layer_keeps_the_mods_texts_and_adds_the_ports(self):
        self.mod('translation', {'dvdroot_ps4/msg/engus/menu.msgbnd.dcx': texts('translated')})
        folder, out = self.root / 'out' / 'mod_sources', self.root / 'out' / 'quit_text'
        with mock.patch('sys.stderr'), mock.patch('sys.stdout'):
            mods.write_sources(self.game, mods.mod_layers(self.game, self.mods), folder)
            failed = quit_menu.write_text_layer(self.game, out, None, [folder])
        self.assertEqual(failed, {})
        fmgs = fmgs_of((out / TEXTS.format('engus')).read_bytes())
        self.assertEqual(fmgs[200][401300], 'translated item')  # the mod's
        self.assertEqual(fmgs[200][quit_menu.TITLE_ID], quit_menu.TEXTS['engus'][0])  # and the port's
        for path in (out / 'dvdroot_ps4').rglob('*'):
            if path.is_file():
                self.assertTrue(port_files.in_layer('quit_text', path.relative_to(out)), path)

    def test_a_mods_archive_that_cannot_take_the_texts_is_left_to_the_mod(self):
        write(self.game / TEXTS.format('deude'), texts('game de'))
        self.mod('broken', {'dvdroot_ps4/msg/engus/menu.msgbnd.dcx': b'not an archive'})
        folder, out = self.root / 'out' / 'mod_sources', self.root / 'out' / 'quit_text'
        with mock.patch('sys.stderr'), mock.patch('sys.stdout'):
            mods.write_sources(self.game, mods.mod_layers(self.game, self.mods), folder)
            failed = quit_menu.write_text_layer(self.game, out, None, [folder])
        self.assertEqual(list(failed), [TEXTS.format('engus')])
        self.assertFalse((out / TEXTS.format('engus')).exists())  # the mod's stays
        self.assertTrue((out / TEXTS.format('deude')).exists())
        # The game's own archive broken: the step fails, as before (run.py turns the features off).
        write(self.game / TEXTS.format('deude'), b'broken game file')
        with mock.patch('sys.stdout'), self.assertRaises(Exception):
            quit_menu.write_text_layer(self.game, out, None, [folder])

    def test_rebirths_texts_build_on_quit_to_desktops_then_the_mods(self):
        write(self.game / TEXTS.format('deude'), texts('game de'))
        sources = self.root / 'mod_sources'
        write(sources / TEXTS.format('deude'), texts('mod de'))
        layer, out = self.root / 'quit_text', self.root / 'rebirth'
        write(layer / TEXTS.format('engus'), texts('port en'))
        with mock.patch.object(rebirth, 'doll_texts', return_value=rebirth.TEXTS['engus']):
            self.assertEqual(rebirth.write_texts(self.game, out, [layer, sources]), 2)
        self.assertEqual(fmgs_of((out / TEXTS.format('engus')).read_bytes())[200][401300], 'port en item')
        self.assertEqual(fmgs_of((out / TEXTS.format('deude')).read_bytes())[200][401300], 'mod de item')

    def test_rebirth_reads_a_mods_weapon_requirements(self):
        def weapons_param(needs):
            header = bytearray(0x40)
            struct.pack_into('<H', header, 0x0A, 1)
            header[0x0C:0x0C + 21] = b'EQUIP_PARAM_WEAPON_ST'
            row = bytearray(0x100)
            row[rebirth.REQUIREMENTS:rebirth.REQUIREMENTS + 4] = bytes(needs)
            return bytes(header) + struct.pack('<QQQ', 7000000, 0x58, 0) + bytes(row)
        write(self.game / rebirth.GAME_PARAMS, archive([(rebirth.WEAPON_PARAMS, weapons_param((8, 7, 0, 0)))]))
        sources = self.root / 'mod_sources'
        write(sources / rebirth.GAME_PARAMS, archive([(rebirth.WEAPON_PARAMS, weapons_param((20, 7, 0, 0)))]))
        self.assertEqual(rebirth.weapons(self.game), {7000000: (8, 7, 0, 0)})
        self.assertEqual(rebirth.weapons(self.game, [sources]), {7000000: (20, 7, 0, 0)})

    def test_the_field_of_view_scales_a_mods_cameras(self):
        write(self.game / fov.ARCHIVE, archive([('LockCamParam.param', lock_cam_param())]))
        sources, out = self.root / 'mod_sources', self.root / 'fov'
        write(sources / fov.ARCHIVE, archive([('LockCamParam.param', lock_cam_param(((0, 50.0), (1, 48.0))))]))
        result = subprocess.run([sys.executable, str(ROOT / 'scripts/fov.py'), str(self.game), '--out', str(out),
                                 '--fov', '86', '--source', str(sources)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        values = fov_values(tables((out / fov.ARCHIVE).read_bytes())['LockCamParam.param'])
        self.assertAlmostEqual(values[0], 100.0, places=3)  # the mod's 50, doubled
        write(sources / fov.ARCHIVE, b'not a parameter archive')
        result = subprocess.run([sys.executable, str(ROOT / 'scripts/fov.py'), str(self.game), '--out', str(out),
                                 '--fov', '86', '--source', str(sources)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 1)  # run.py: the field of view is off for that mod


class ReportTests(unittest.TestCase):
    FILES = {'files': {TEXTS.format('engus'): 'translation', 'dvdroot_ps4/menu/optionsetting.gfx': 'menus',
                       'dvdroot_ps4/param/gameparam/gameparam.parambnd.dcx': 'balance'}}

    def test_what_each_feature_does_with_the_mods_files(self):
        failed = {'dvdroot_ps4/menu/optionsetting.gfx': 'no list in the movie'}
        report = run.mod_report(self.FILES, failed, {})
        self.assertEqual(report['pc_menu']['state'], 'off')
        self.assertEqual(report['pc_menu']['mod'], 'menus')
        self.assertEqual(report['quit_to_desktop']['state'], 'off')
        self.assertEqual(report['rebirth'], {'state': 'adapted', 'mod': 'translation', 'file': TEXTS.format('engus'),
                                             'why': 'adds'})
        self.assertEqual(report['fov']['state'], 'adapted')
        env = {}
        run.switch_off(report, env)
        self.assertEqual(env, {'BB_QUIT_TO_DESKTOP': '0', 'BB_PC_MENU': '0'})
        lines = run.mod_report_lines(report)
        self.assertIn("Mods: menus's dvdroot_ps4/menu/optionsetting.gfx cannot take PC Settings in the System menu: "
                      'it is off for this start (turn the mod off to have it; no list in the movie)', lines)
        self.assertIn("Mods: Rebirth at the Doll is added to translation's dvdroot_ps4/msg/engus/menu.msgbnd.dcx",
                      lines)

    def test_a_feature_turned_off_is_taken_out_of_its_layer(self):
        """The System menu came out garbled in a game test: Quit to Desktop's layer kept the rows grown for
        PC Settings while the game's code no longer filled them."""
        source = (ROOT / 'run.py').read_text(encoding='utf-8')
        after = source[source.index('    switch_off(report, env)\n    # A feature a mod turned off'):]
        after = after[:after.index("    place = '--top-layer'")]
        self.assertIn("shutil.rmtree(rebirth_layer, ignore_errors=True)", after)
        self.assertIn('make_text_layer(game, text_layer, quit_menu, pc_menu, sources, again=True)', after)
        self.assertIn('shutil.rmtree(text_layer, ignore_errors=True)', after)
        self.assertIn('make_hud_layer(', after)
        self.assertNotIn('discord', run.mod_report({'files': {TEXTS.format('engus'): 'translation'}}, {}, {}))

    def test_only_the_features_this_start_uses(self):
        report = run.mod_report(self.FILES, {}, {}, active={'rebirth', 'fov'})
        self.assertEqual(sorted(report), ['fov', 'rebirth'])  # no HUD layout at 16:9, no Quit to Desktop off
        source = (ROOT / 'run.py').read_text(encoding='utf-8')
        self.assertIn('mod_report(from_mods, layer_failures(layers), failed_steps, active)', source)

    def test_a_failed_step_on_a_mods_file_is_that_mods(self):
        report = run.mod_report(self.FILES, {}, {'fov': 'its camera table could not be written'})
        self.assertEqual((report['fov']['state'], report['fov']['mod']), ('off', 'balance'))
        self.assertEqual(run.mod_report({'files': {}}, {}, {'fov': 'x'}), {})  # not a mod's doing
        self.assertEqual(run.mod_report(None, {}, {}), {})

    def test_without_the_layers_every_feature_with_rows_is_off(self):
        """The game view made without the port's layers (mods.py past its time limit): PC Settings' rows
        were left without their texts in 1.9.5 (R55)."""
        env = {}
        run.switch_off(run.without_layers({}), env)
        self.assertEqual(env, {'BB_QUIT_TO_DESKTOP': '0', 'BB_PC_MENU': '0', 'BB_REBIRTH': '0'})

    def test_the_report_is_written_for_the_launcher(self):
        with tempfile.TemporaryDirectory() as temp:
            run.write_mod_report(temp, {'pc_menu': {'state': 'off', 'mod': 'menus', 'file': 'x', 'why': 'y'}})
            self.assertEqual(json.loads((Path(temp) / 'mod-report.json').read_text(encoding='utf-8'))['features']
                             ['pc_menu']['mod'], 'menus')


if __name__ == '__main__':
    unittest.main()
