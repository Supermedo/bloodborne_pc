"""The co-op pools (launcher/bbport_launcher_win.py): SuperMedo Server first and by default, Seamless co-op, and an
own server only under Seamless co-op's own start; a pool no longer offered plays on SuperMedo Server."""
from paths import ROOT
import os
import sys
import unittest
from unittest import mock

sys.path.insert(0, str(ROOT / 'launcher'))
sys.path.insert(0, str(ROOT / 'scripts'))
import bbport_launcher_win as L  # noqa: E402


class PoolTests(unittest.TestCase):
    """The co-op pools: SuperMedo Server first and by default, a shadNet server of the player's choice."""

    def test_supermedo_server_by_default(self):
        self.assertEqual(L.APP_DEFAULTS['online_pool'], 'supermedo')

    def test_a_pool_no_longer_offered_plays_on_supermedo_server(self):
        """A pool an older version saved: its players play on SuperMedo Server, and a start without its account
        asks for the sign-in (an older version's sign-in there does not count)."""
        old = {**L.APP_DEFAULTS, 'online': True, 'online_pool': 'gone'}
        self.assertEqual(L.settled_pool(old), 'supermedo')
        self.assertEqual(L.chosen_pool(old), 'supermedo')
        self.assertEqual(L.pool_account(old), '')
        self.assertTrue(L.needs_sign_in(old))
        self.assertEqual(L.signed_out_server(old), 'SuperMedo Server')
        self.assertEqual(L.pool_account({**old, 'supermedo_npid': 'n', 'supermedo_pass': 'p'}), 'n')

    def test_supermedo_server_first(self):
        source = (ROOT / 'launcher' / 'bbport_launcher_win.py').read_text(encoding='utf-8')
        page = source[source.index('    def build_online(self):'):source.index('    def build_mods(self):')]
        self.assertLess(page.index("tabs.append(('supermedo'"), page.index("tabs.append(('shadnet'"))

    def test_the_supermedo_pool_plays_on_supermedo_server(self):
        settings = {**L.APP_DEFAULTS, 'online': True, 'online_pool': 'supermedo'}
        env = L.game_environment(settings)
        self.assertEqual((env['BB_SHADNET_SERVER'], env['BB_COMMUNITY_SERVER']), (L.SUPERMEDO_SERVER, L.COMMUNITY_SERVER))
        self.assertTrue(env['BB_CLIENT_VERSION'].startswith('bbport/'))

    def test_the_game_server_is_supermedo_servers(self):
        self.assertIn('supermedo.com', L.COMMUNITY_SERVER)

    def test_a_start_moves_dead_ends_to_supermedo_server(self):
        """Beta 8 testers could not play online: an earlier launcher left them on shadNet without own servers,
        shadPS4's public server, which refuses the port. Unknown pools too."""
        for pool in ('shadnet', 'gone', None):
            self.assertEqual(L.settled_pool({**L.APP_DEFAULTS, 'online_pool': pool,
                                             'online_custom_servers': False}), 'supermedo', pool)

    def test_a_start_without_the_window_settles_the_same(self):
        """--play (Steam, a shortcut) never builds the window: settled_online gives it the same pool, also for
        settings saved before the "Use my own servers" box."""
        old_public = {'online': True, 'online_pool': 'shadnet', 'online_server': L.APP_DEFAULTS['online_server']}
        self.assertEqual(L.settled_online({**L.APP_DEFAULTS, **old_public}, old_public),
                         {'online_custom_servers': False, 'online_pool': 'supermedo'})
        seamless = {'online': True, 'online_pool': 'shadnet', 'online_server': 'play.seamlesshunt.com:31313'}
        self.assertEqual(L.settled_online({**L.APP_DEFAULTS, **seamless}, seamless),
                         {'online_custom_servers': True, 'online_pool': 'supermedo'})
        with mock.patch.dict(os.environ, {'BB_PROBE': r'C:\BBSC\bbsc-launch.exe'}):
            self.assertEqual(L.settled_online({**L.APP_DEFAULTS, **seamless}, seamless),
                             {'online_custom_servers': True, 'online_pool': 'shadnet'})
        boxed = {**seamless, 'online_custom_servers': False}  # the box saved off: the player's choice
        self.assertEqual(L.settled_online({**L.APP_DEFAULTS, **boxed}, boxed)['online_pool'], 'supermedo')

    def test_a_service_without_its_own_account_asks(self):
        """A start from Steam or a shortcut opens the launcher, whose Play asks: online on SuperMedo Server without
        its account stayed offline without a word (a new player). Another pool's sign-in does not count."""
        cases = [({'online_pool': 'gone'}, True),
                 ({'online_pool': 'supermedo'}, True), ({'online_pool': 'supermedo', 'online_npid': 'n'}, True),
                 ({'online_pool': 'supermedo', 'supermedo_npid': 'n', 'supermedo_pass': 'p'}, False),
                 ({'online_pool': 'shadnet'}, False)]
        for settings, asks in cases:
            self.assertEqual(L.needs_sign_in({'online': True, **settings}), asks, settings)
            self.assertFalse(L.needs_sign_in({'online': False, **settings}))
        self.assertTrue(L.needs_sign_in({'online': True, 'online_pool': 'seamless'}))
        self.assertFalse(L.needs_sign_in({'online': True, 'online_pool': 'seamless', 'supermedo_npid': 'n',
                                          'supermedo_pass': 'p'}))

    def test_each_service_keeps_its_own_account(self):
        """SuperMedo Server's account was in the shadNet boxes, which Seamless co-op writes too: one overwrote the
        other. Each pool signs in with its own now."""
        settings = {**L.APP_DEFAULTS, 'online': True, 'supermedo_npid': 'medo', 'supermedo_pass': 'mp',
                    'online_npid': 'seamless', 'online_password': 'sp',
                    'online_custom_servers': True, 'online_server': 'play.seamlesshunt.com:31313'}
        env = L.game_environment({**settings, 'online_pool': 'supermedo'})
        self.assertEqual((env['BB_SHADNET_SERVER'], env['BB_SHADNET_NPID'], env['BB_SHADNET_PASSWORD']),
                         (L.SUPERMEDO_SERVER, 'medo', 'mp'))
        env = L.game_environment({**settings, 'online_pool': 'shadnet'})
        self.assertEqual((env['BB_SHADNET_SERVER'], env['BB_SHADNET_NPID'], env['BB_SHADNET_PASSWORD']),
                         ('play.seamlesshunt.com:31313', 'seamless', 'sp'))
        for pool, name in (('supermedo', 'medo'), ('shadnet', 'seamless')):
            self.assertEqual(L.pool_account({**settings, 'online_pool': pool}), name)
        self.assertEqual(L.pool_account({**L.APP_DEFAULTS, 'online_npid': 'seamless'}), '')

    def test_seamless_co_op_plays_through_bbsc_with_the_supermedo_account(self):
        """SuperMedo Seamless co-op: the seamless server with the SuperMedo account, started through BBSC's
        bbsc-launch.exe (bbsc next to the launcher) with the port's own bb-probe.exe, and BBSC's game server."""
        settings = {**L.APP_DEFAULTS, 'online': True, 'online_pool': 'seamless', 'supermedo_npid': 'medo',
                    'supermedo_pass': 'mp'}
        env = L.game_environment(settings)
        self.assertEqual((env['BB_SHADNET_SERVER'], env['BB_SHADNET_WEBAPI'], env['BB_SHADNET_NPID'],
                          env['BB_SHADNET_PASSWORD'], env['BB_COMMUNITY_SERVER']),
                         ('seamless.supermedo.com:31313', 'http://seamless.supermedo.com:31315', 'medo', 'mp',
                          'http://seamless.supermedo.com:20443'))
        self.assertEqual(env['BB_PROBE'], str(L.PORT_DIR / 'bbsc' / 'bbsc-launch.exe'))
        self.assertEqual(env['BBSC_EMULATOR'], str(L.PORT_DIR / 'bin' / 'bb-probe.exe'))
        self.assertEqual(L.pool_account(settings), 'medo')
        with mock.patch.object(L, 'BBSC_LAUNCH', mock.Mock(exists=lambda: True)):
            self.assertEqual(L.settled_pool(settings), 'seamless')
        with mock.patch.object(L, 'BBSC_LAUNCH', mock.Mock(exists=lambda: False)):  # a build without BBSC
            self.assertEqual(L.settled_pool(settings), 'supermedo')

    def test_the_supermedo_account_leaves_the_shadnet_boxes_once(self):
        """Saved on SuperMedo Server up to beta 8: its account was in the shadNet boxes. Other pools' boxes are
        shadNet's (a public or a co-op server's account) and stay there."""
        beta8 = {'online_pool': 'supermedo', 'online_npid': 'medo', 'online_password': 'mp'}
        self.assertEqual(L.settled_online({**L.APP_DEFAULTS, **beta8}, beta8)['supermedo_npid'], 'medo')
        self.assertEqual(L.settled_online({**L.APP_DEFAULTS, **beta8}, beta8)['supermedo_pass'], 'mp')
        for saved in ({**beta8, 'supermedo_npid': 'other'}, {**beta8, 'online_pool': 'shadnet'},
                      {**beta8, 'online_pool': 'gone'}):
            self.assertNotIn('supermedo_npid', L.settled_online({**L.APP_DEFAULTS, **saved}, saved))

    def test_another_programs_writes(self):
        """Seamless co-op writes settings.json from outside: its shadNet settings are taken, the pool and the
        SuperMedo Server sign-in stay the launcher's (the Online page writes them back)."""
        written = {**{key: L.APP_DEFAULTS[key] for key in L.SHADNET_KEYS + L.OWN_ONLINE_KEYS},
                   'supermedo_npid': 'medo'}
        self.assertEqual(L.outside_online_changes(written, dict(written)), ({}, []))
        on_disk = {**written, 'online_server': 'bbsc.example:31313', 'online_npid': 'bbsc',
                   'online_pool': 'shadnet', 'supermedo_npid': ''}
        taken, kept = L.outside_online_changes(written, on_disk)
        self.assertEqual(taken, {'online_server': 'bbsc.example:31313', 'online_npid': 'bbsc'})
        self.assertEqual(sorted(kept), ['online_pool', 'supermedo_npid'])

    def test_only_seamless_coop_keeps_its_own_server(self):
        """Since 1.9.5 beta 10 SuperMedo Server only: an own server moves to SuperMedo Server, except under
        Seamless co-op's own start (bbsc-launch.exe the game program), whose server it is."""
        own = {**L.APP_DEFAULTS, 'online_pool': 'shadnet', 'online_server': 'play.seamlesshunt.com:31313'}
        own['online_custom_servers'] = L.has_own_servers(own)
        self.assertEqual(L.settled_pool(own, {}), 'supermedo')
        self.assertEqual(L.settled_pool(own, {'BB_PROBE': r'C:\BBSC\bbsc-launch.exe'}), 'shadnet')
        self.assertEqual(L.settled_pool({**own, 'online_custom_servers': False},
                                        {'BB_PROBE': r'C:\BBSC\bbsc-launch.exe'}), 'supermedo')
        self.assertEqual(L.settled_pool({**L.APP_DEFAULTS, 'online_pool': 'supermedo'}, {}), 'supermedo')

    def test_seamless_coop_gets_its_server_back_after_a_normal_start(self):
        """A normal start moved BBSC's pool to SuperMedo Server and saved it: BBSC's next start, naming the server
        its script saved in the shadNet boxes, plays there again; another server does not count."""
        bbsc = {'BB_PROBE': r'C:\\BBSC\\bbsc-launch.exe', 'BB_SHADNET_SERVER': 'play.seamlesshunt.com:31313'}
        moved = {**L.APP_DEFAULTS, 'online_pool': 'supermedo', 'online_server': 'play.seamlesshunt.com:31313'}
        self.assertEqual(L.settled_pool(moved, bbsc), 'shadnet')
        self.assertEqual(L.settled_pool(moved, {}), 'supermedo')
        self.assertEqual(L.settled_pool({**moved, 'online_server': 'friend.example:31313'}, bbsc), 'supermedo')
        with mock.patch.dict(os.environ, bbsc):
            self.assertEqual(L.settled_online(moved, {'online_custom_servers': False}),
                             {'online_custom_servers': True, 'online_pool': 'shadnet'})


class GameServerTests(unittest.TestCase):
    """The game server field counts only where the Online page shows it (shadNet with "Use my own servers").
    A co-op tool wrote its own address into it."""
    OTHER = 'http://play.seamlesshunt.com'

    def environment(self, **changes):
        settings = {**L.APP_DEFAULTS, 'online': True, 'online_community': self.OTHER, **changes}
        return L.game_environment(settings)

    def test_supermedo_server_plays_on_its_own_game_server(self):
        for custom in (False, True):
            env = self.environment(online_pool='supermedo', online_custom_servers=custom)
            self.assertEqual(env['BB_COMMUNITY_SERVER'], L.COMMUNITY_SERVER)

    def test_shadnet_uses_the_field_only_with_own_servers(self):
        self.assertEqual(self.environment(online_pool='shadnet')['BB_COMMUNITY_SERVER'], L.COMMUNITY_SERVER)
        self.assertEqual(self.environment(online_pool='shadnet', online_custom_servers=True)['BB_COMMUNITY_SERVER'],
                         self.OTHER)

    def test_shadnet_uses_its_fields_only_with_own_servers(self):
        """A beta tester's hidden WebAPI field said https://srv.shadps4.net:31315; shadNet speaks http there."""
        fields = {'online_pool': 'shadnet', 'online_server': 'old.example:31313',
                  'online_webapi': 'https://srv.shadps4.net:31315'}
        env = self.environment(**fields)
        self.assertEqual(env['BB_SHADNET_SERVER'], L.APP_DEFAULTS['online_server'])
        self.assertEqual(env['BB_SHADNET_WEBAPI'], 'http://srv.shadps4.net:31315')
        env = self.environment(online_custom_servers=True, **fields)
        self.assertEqual(env['BB_SHADNET_SERVER'], 'old.example:31313')
        self.assertEqual(env['BB_SHADNET_WEBAPI'], 'https://srv.shadps4.net:31315')

    def test_own_servers_on_for_settings_from_before_the_box(self):
        """Saved before "Use my own servers" existed, with a private server: the box starts on."""
        self.assertTrue(L.has_own_servers({**L.APP_DEFAULTS, 'online_server': 'friend.example:31313'}))
        self.assertTrue(L.has_own_servers({**L.APP_DEFAULTS, 'online_community': self.OTHER}))
        self.assertFalse(L.has_own_servers(L.APP_DEFAULTS))
        self.assertFalse(L.has_own_servers({**L.APP_DEFAULTS, 'online_server': ' '}))


if __name__ == '__main__':
    unittest.main()
