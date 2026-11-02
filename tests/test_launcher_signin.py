# SPDX-License-Identifier: GPL-2.0-or-later
"""A new player's way online (2026-11-02): the SuperMedo account's three ways in where the setup guide and Play ask
for it (Discord alone before), the party on Seamless co-op too (its code brings the hunters together, no password
in the game), and Help's topic on playing together."""
from paths import ROOT
import sys
import tkinter
import types
import unittest
from tkinter import ttk
from unittest import mock

sys.path.insert(0, str(ROOT / 'launcher'))
sys.path.insert(0, str(ROOT / 'scripts'))
import bbport_launcher_win as L  # noqa: E402
import bbport_signin  # noqa: E402
from test_launcher_background import bare_launcher, button, hidden_root, run_ui_calls, texts, widgets  # noqa: E402


class SignInBoxTests(unittest.TestCase):
    """bbport_signin.build: the guide's card and Play's question."""

    def setUp(self):
        root = hidden_root(self)
        patcher = mock.patch.object(L, 'LANG', 'en')
        patcher.start()
        self.addCleanup(patcher.stop)
        variables = {}

        def var(key, _store):
            return variables.setdefault(key, tkinter.StringVar(root))
        self.app = bare_launcher(tk=tkinter, ttk=ttk, px=int, var=var, collect=mock.Mock())
        self.signed = mock.Mock()
        self.parent = tkinter.Frame(root)
        self.box = bbport_signin.build(self.app, self.parent, '#000000', self.signed)

    def entry(self, index):
        return [w for w in widgets(self.box) if w.winfo_class() == 'TEntry'][index]

    def account(self):
        return self.app.var('supermedo_npid', 'app').get(), self.app.var('supermedo_pass', 'app').get()

    def test_the_online_id_and_password(self):
        self.entry(0).insert(0, 'Hunter')
        self.entry(1).insert(0, 'secret')
        with mock.patch.object(L, 'shadnet_check_login', return_value=None) as login:
            button(self.box, 'Sign in').invoke()
            run_ui_calls(self.app)
        self.assertEqual(login.call_args.args[1:], ('Hunter', 'secret'))
        self.assertEqual(self.account(), ('Hunter', 'secret'))
        self.app.collect.assert_called_once()
        self.signed.assert_called_once()

    def test_a_wrong_password_is_said_and_nothing_kept(self):
        self.entry(0).insert(0, 'Hunter')
        self.entry(1).insert(0, 'wrong')
        with mock.patch.object(L, 'shadnet_check_login', return_value='The password is wrong.'):
            button(self.box, 'Sign in').invoke()
            run_ui_calls(self.app)
        self.assertIn('The password is wrong.', texts(self.box))
        self.assertEqual(self.account(), ('', ''))
        self.signed.assert_not_called()

    def test_a_server_out_of_reach_is_said(self):
        self.entry(0).insert(0, 'Hunter')
        self.entry(1).insert(0, 'secret')
        with mock.patch.object(L, 'shadnet_check_login', side_effect=ConnectionRefusedError(10061, 'refused')):
            button(self.box, 'Sign in').invoke()
            run_ui_calls(self.app)
        self.assertTrue(any(t.startswith('SuperMedo Server could not be reached') for t in texts(self.box)))
        self.signed.assert_not_called()

    def test_a_new_account(self):
        self.box.toggle_new()  # as "No account yet? Make one" does
        for index, value in ((2, 'NewHunter'), (3, 'secret'), (4, 'hunter@example.net')):
            self.entry(index).insert(0, value)
        with mock.patch.object(L, 'shadnet_register', return_value=None) as register:
            button(self.box, 'Create account').invoke()
            run_ui_calls(self.app)
        self.assertEqual(register.call_args.args[1:], ('NewHunter', 'secret', 'hunter@example.net'))
        self.assertEqual(self.account(), ('NewHunter', 'secret'))
        self.signed.assert_called_once()

    def test_discord_after_the_box_is_gone_still_keeps_the_account(self):
        with mock.patch.object(L, 'discord_sign_in', return_value=('Hunter', 'secret')), \
                mock.patch('webbrowser.open'):
            button(self.box, 'Sign in with Discord').invoke()
            self.parent.destroy()
            run_ui_calls(self.app)
        self.assertEqual(self.account(), ('Hunter', 'secret'))
        self.signed.assert_not_called()  # its window is gone


class PlayWithoutAccountTests(unittest.TestCase):
    """Play, online without an account: the box's answer decides the start."""

    def play(self, answer, signs_in=False):
        ui = L.Launcher.__new__(L.Launcher)
        values = {'online': True, 'online_pool': 'supermedo', 'online_npid': ''}
        ui.var = lambda key, _store='app': types.SimpleNamespace(get=lambda: values.get(key, ''),
                                                                 set=lambda v: values.__setitem__(key, v))
        ui.app, ui.ini = {**L.APP_DEFAULTS, 'online': True, 'online_pool': 'supermedo'}, {}
        ui.process, ui.fsr411_downloading = None, False
        ui.patch_vars = {'extra.xml/On': types.SimpleNamespace(get=lambda: False)}  # no player's patch files read
        account = ['']
        ui.account_name = lambda: account[0]
        for method in ('update_installing', 'save_settings', 'show'):
            setattr(ui, method, mock.Mock(return_value=False))

        def ask(_ui):
            if signs_in:
                account[0] = 'Hunter'
            return answer
        with mock.patch.object(L, 'game_underway', return_value=False), \
                mock.patch.object(bbport_signin, 'ask', side_effect=ask) as asked:
            ui.play()  # save_settings answers False: the start stops right after the questions
        return asked, ui.save_settings

    def test_signed_in_there_goes_on(self):
        asked, saved = self.play('signed', signs_in=True)
        asked.assert_called_once()
        saved.assert_called_once()

    def test_offline_this_time_goes_on(self):
        _asked, saved = self.play('offline')
        saved.assert_called_once()

    def test_closed_starts_nothing(self):
        _asked, saved = self.play(None)
        saved.assert_not_called()


class SeamlessPartyTests(unittest.TestCase):
    def test_the_party_shows_with_seamless_co_op(self):
        """Medo's beta 5 showed it with SuperMedo Server only; its code brings Seamless co-op's hunters together
        too (Nick, 2026-11-02)."""
        root = hidden_root(self)
        for patcher in (mock.patch.object(L, 'LANG', 'en'), mock.patch.object(L, 'HUNTERS_DREAM_POOL', True),
                        mock.patch.object(L, 'BBSC_LAUNCH', mock.Mock(exists=lambda: True)),
                        mock.patch('webbrowser.open')):
            patcher.start()
            self.addCleanup(patcher.stop)
        app = self.app = bare_launcher(root=root, tk=tkinter, ttk=ttk, dpi=1, app=dict(L.APP_DEFAULTS), vars={},
                                       pages={}, content=ttk.Frame(root), refresh_status=mock.Mock(),
                                       ask_hunters=mock.Mock(), collect=mock.Mock())
        app.build_online()
        page = app.pages['online']
        for pool in ('supermedo', 'seamless'):
            app.var('online_pool', 'app').set(pool)
            label, = [w for w in widgets(page) if w.winfo_class() == 'TLabel' and w.cget('text') == 'Your party']
            self.assertTrue(label.grid_info(), pool)  # gridded, not removed
        shown = [' '.join(t.split()) for t in texts(page)]  # wrapped lines joined again
        self.assertTrue(any("rings the Hunt Bell" in t and "party's host" in t for t in shown))
        # A new character rings them first in the Hunter's Dream, where BBSC hands its kit out.
        self.assertTrue(any("in the Hunter's Dream" in t and 'at the Doll' in t for t in shown))


class PlayTogetherHelpTests(unittest.TestCase):
    def test_the_topic_names_the_bells_and_the_empty_password(self):
        import bbport_help
        title, why, steps = bbport_help.TOPICS['play_together']
        words = ' '.join(steps)
        for part in ('Hunt Bell', 'Kin Bell', 'Beckoning Bell', 'party code', 'empty', 'at the Doll'):
            self.assertIn(part, why + words)
        self.assertIn('play_together', bbport_help.TOPIC_ORDER)
        # The old way to Seamless co-op (an own server from BBSC's launcher) is no longer its only way.
        self.assertTrue(any(s.startswith('Seamless co-op: choose SuperMedo Seamless co-op')
                            for s in bbport_help.TOPICS['shadnet_rejected'][2]))


if __name__ == '__main__':
    unittest.main()
