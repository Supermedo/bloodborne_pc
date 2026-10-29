"""The launcher on mods and the port's own changes (launcher/bbport_mods.py): the notes under each mod on the
Mods page, the Play page's line when a mod turned a feature off at the last start (out/mod-report.json,
run.py), and Start once without mods (only that start loads none)."""
from paths import ROOT
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(ROOT / 'launcher'))
import bbport_launcher_win as L  # noqa: E402  (binds bbport_mods)
import bbport_mods  # noqa: E402


class English(unittest.TestCase):
    """The texts in English, whatever language an earlier test left the launcher in."""
    def setUp(self):
        patcher = mock.patch.object(L, 'LANG', 'en')
        patcher.start()
        self.addCleanup(patcher.stop)


def write(path):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(b'x')


class ModNotesTests(English):
    def setUp(self):
        super().setUp()
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name) / 'mods'
        for relative in ('translation/dvdroot_ps4/msg/engus/menu.msgbnd.dcx',
                         'ui/dvdroot_ps4/menu/optionsetting.gfx',
                         'balance/dvdroot_ps4/param/gameparam/gameparam.parambnd.dcx',
                         'doll/dvdroot_ps4/script/talk/m21_00_00_00.talkesdbnd.dcx',
                         'hud/dvdroot_ps4/menu/fe.gfx',
                         'textures/dvdroot_ps4/chr/c0000.chrbnd.dcx', 'textures/dvdroot_ps4/chr/c0001.chrbnd.dcx',
                         'textures2/dvdroot_ps4/chr/c0000.chrbnd.dcx'):
            write(self.root / relative)
        self.order = ['translation', 'ui', 'balance', 'doll', 'hud', 'textures', 'textures2']

    def test_each_mod_says_what_it_changes_of_the_ports(self):
        notes = bbport_mods.notes(self.root, self.order, self.order)
        self.assertEqual(notes['translation'], ['Changes menu texts: the port adds its own (Quit to Desktop, PC '
                                                'Settings, Rebirth) to them.'])
        self.assertIn('Quit to Desktop and PC Settings are fitted into it', notes['ui'][0])
        self.assertEqual(notes['balance'], ['Changes game parameters: the field of view and Rebirth use its values.'])
        self.assertIn('Rebirth is fitted into it', notes['doll'][0])
        self.assertIn('ultrawide HUD layout', notes['hud'][0])
        self.assertEqual(notes['textures'], ['textures2 (lower in the list) replaces 1 of its files.'])
        self.assertNotIn('textures2', notes)  # nothing of the port's, nothing taken from it

    def test_a_switched_off_mod_takes_no_files(self):
        notes = bbport_mods.notes(self.root, self.order, [n for n in self.order if n != 'textures2'])
        self.assertNotIn('textures', notes)

    def test_the_last_start_names_what_a_mod_turned_off(self):
        last = {'pc_menu': {'state': 'off', 'mod': 'ui', 'file': 'dvdroot_ps4/menu/optionsetting.gfx', 'why': 'x'},
                'quit_to_desktop': {'state': 'off', 'mod': 'ui', 'file': 'dvdroot_ps4/menu/optionsetting.gfx',
                                    'why': 'x'},
                'rebirth': {'state': 'adapted', 'mod': 'translation', 'file': 'x', 'why': 'adds'}}
        notes = bbport_mods.notes(self.root, self.order, self.order, last)
        self.assertEqual(notes['ui'][-1], "⚠ Off at the last start because of this mod's optionsetting.gfx: PC "
                                          'Settings in the game menu, Quit to Desktop.')  # one line per file
        self.assertEqual(bbport_mods.warning(last, ['ui']),
                         "⚠ Mod 'ui': PC Settings in the game menu was off at the last start (click for why)")
        self.assertEqual(bbport_mods.warning(last, ['translation']), '')  # adapted: nothing to warn about
        self.assertEqual(bbport_mods.warning(last, []), '')  # turned off since

    def test_the_report_and_the_mods_a_start_loads(self):
        data = self.root.parent
        (data / 'out').mkdir()
        self.assertEqual(bbport_mods.report(data), {})
        (data / 'out' / 'mod-report.json').write_text(json.dumps({'features': {'fov': {'state': 'off'}}}))
        self.assertEqual(bbport_mods.report(data), {'fov': {'state': 'off'}})
        (data / 'mods.json').write_text(json.dumps({'disabled': ['hud'], 'order': ['ui', 'translation']}))
        app = {'mods_enabled': True, 'mods_dir': str(self.root), 'game_dir': str(data / 'game')}
        loaded = bbport_mods.enabled_mods(app, data)
        self.assertEqual(loaded[:2], ['ui', 'translation'])
        self.assertNotIn('hud', loaded)
        self.assertEqual(bbport_mods.enabled_mods({**app, 'mods_enabled': False}, data), [])


class WithoutModsTests(English):
    def test_only_that_start_loads_none(self):
        source = (ROOT / 'launcher' / 'bbport_launcher_win.py').read_text(encoding='utf-8')
        play = source[source.index('    def play(self):'):]
        play = play[:play.index('\n    def ', 10)]
        self.assertIn("if getattr(self, 'without_mods_once', False):", play)
        self.assertIn('self.without_mods_once = False', play)  # cleared at that start
        self.assertIn("environment['BB_MODS_ENABLED'] = '0'", play)
        self.assertIn('env=environment', play)

    def test_offered_after_an_error_with_mods_loaded(self):
        class App:
            app = {}
            messagebox = mock.Mock()
            started = 0

            def play_without_mods(self):
                App.started += 1
        import bbport_help
        app = App()
        app.messagebox.askyesno.return_value = True
        with mock.patch.object(bbport_help, 'hints_in_log', return_value=[]), \
                mock.patch.object(bbport_mods, 'enabled_mods', return_value=['a', 'b']):
            bbport_help.HelpMixin.after_exit(app, 1)
        self.assertEqual(App.started, 1)
        self.assertIn('2 mods were loaded', app.messagebox.askyesno.call_args.args[1])
        App.started = 0
        with mock.patch.object(bbport_help, 'hints_in_log', return_value=[]), \
                mock.patch.object(bbport_mods, 'enabled_mods', return_value=[]):
            bbport_help.HelpMixin.after_exit(app, 1)  # no mods: as before
        self.assertEqual(App.started, 0)


if __name__ == '__main__':
    unittest.main()
