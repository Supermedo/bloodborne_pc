# SPDX-License-Identifier: GPL-2.0-or-later
"""Cheats, debug features and third-party patches run offline only (patches.py leaves them out of every online
start; players went online with them, 2026-11-02). Play asks first: online without them, offline with them, or not
at all. Offline with them is this one start: "Play online" stays on.

The launcher's settings live in a Tcl interpreter without a window, its files in a temporary folder."""
from paths import ROOT
import functools
import os
from pathlib import Path
import sys
import tempfile
import tkinter
import types
import unittest
from unittest.mock import Mock, patch

sys.path.insert(0, str(ROOT / 'launcher'))
import bbport_launcher_win as launcher  # noqa: E402
import bbport_textfile as textfile  # noqa: E402


class FairPlayTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.folder = Path(temporary.name)
        self.ini = self.folder / 'bbport.ini'
        self.ini.write_text('cheat_no_death=1\ndebug_camera=1\n', encoding='utf-8')
        for patcher in (patch.dict(os.environ, {'BB_CONFIG': str(self.ini), 'BB_INPUT_CONFIG': ''}),
                        patch.multiple(launcher, CONFIG_DIR=self.folder, CONFIG_FILE=self.folder / 'settings.json',
                                       DATA_DIR=self.folder, LANG='en'),
                        patch.object(textfile, 'REPAIRS', [])):
            patcher.start()
            self.addCleanup(patcher.stop)
        tcl = tkinter.Tcl()
        ui = launcher.Launcher.__new__(launcher.Launcher)
        ui.tk = types.SimpleNamespace(TclError=tkinter.TclError, **{
            kind: functools.partial(getattr(tkinter, kind), tcl) for kind in ('BooleanVar', 'StringVar', 'DoubleVar')})
        ui.app = {**launcher.APP_DEFAULTS, 'online': True, 'online_pool': 'supermedo'}
        ui.ini, ui.ini_lines = launcher.load_ini(ui.app)
        ui.app = launcher.mirrored(ui.app, ui.ini)
        ui.vars, ui.mod_order = {}, []
        ui.process, ui.fsr411_downloading = None, False
        # The Mods page's boxes of a player's patch file.
        ui.patch_vars = {'extra.xml/On': ui.tk.BooleanVar(value=True), 'extra.xml/Off': ui.tk.BooleanVar(value=False)}
        for method in ('update_installing', 'account_name', 'save_settings', 'preflight', 'set_log', 'append', 'pad_stop',
                       'auto_backup', 'remember_game_start', 'show'):
            setattr(ui, method, Mock())
        ui.update_installing.return_value = False
        ui.account_name.return_value = 'hunter'
        ui.save_settings.return_value = ui.preflight.return_value = True
        ui.messagebox = types.SimpleNamespace(askyesnocancel=Mock(), askyesno=Mock(), showwarning=Mock())
        self.ui = ui

    def play(self, answer):
        """Play with this answer to the question; the environment the game would start with (None: not started)."""
        self.ui.messagebox.askyesnocancel.reset_mock()
        self.ui.messagebox.askyesnocancel.return_value = answer
        started = []

        def popen(*_args, env, **_kwargs):
            started.append(env)
            raise OSError('test')
        with patch.object(launcher, 'game_underway', return_value=False), \
             patch.object(launcher, 'game_info', return_value=('01.09',)), \
             patch.object(launcher, 'start_problem', return_value=None), \
             patch.object(launcher, 'misplaced_saves', return_value=('', '')), \
             patch.object(launcher.subprocess, 'Popen', side_effect=popen):
            self.ui.play()
        return started[0] if started else None

    def test_what_stays_off_online_is_named(self):
        self.assertEqual(self.ui.offline_only_on(), ['Never die', 'Free camera', 'extra.xml/On'])

    def test_play_asks_online_without_them_or_offline_with_them(self):
        env = self.play(True)
        self.assertIn('Never die, Free camera, extra.xml/On', self.ui.messagebox.askyesnocancel.call_args.args[1])
        self.assertEqual(env['BB_ONLINE'], '1')  # patches.py leaves them out
        env = self.play(False)
        self.assertNotEqual(env.get('BB_ONLINE'), '1')
        self.assertTrue(self.ui.var('online', 'app').get())  # this start only
        self.assertIsNone(self.play(None))

    def test_no_question_offline_or_without_cheats(self):
        self.ui.var('online', 'app').set(False)
        self.ui.app['online'] = False  # as save_settings takes it
        self.ui.account_name.return_value = ''  # else Play asks whether to play online with it
        self.assertNotEqual(self.play(None).get('BB_ONLINE'), '1')
        self.ui.messagebox.askyesnocancel.assert_not_called()
        self.ui.var('online', 'app').set(True)
        self.ui.app['online'] = True
        self.ui.account_name.return_value = 'hunter'
        for key in ('cheat_no_death', 'debug_camera'):
            self.ui.var(key, 'ini').set(False)
        self.ui.patch_vars['extra.xml/On'].set(False)
        self.assertEqual(self.play(None)['BB_ONLINE'], '1')
        self.ui.messagebox.askyesnocancel.assert_not_called()


if __name__ == '__main__':
    unittest.main()
