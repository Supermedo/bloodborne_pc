"""The Russia route (Online page, SuperMedo Server): the game and the launcher reach the SuperMedo hosts
through the relay's address, the hosts' names kept for TLS and HTTP (launcher/bbport_launcher_win.py
relay_routes, launcher/bbport_tls.py set_routes)."""
from paths import ROOT
import socket
import sys
import unittest
from unittest import mock

sys.path.insert(0, str(ROOT / 'launcher'))
sys.path.insert(0, str(ROOT / 'scripts'))
import bbport_launcher_win as L  # noqa: E402
import bbport_tls  # noqa: E402
import bbport_yharnam as Y  # noqa: E402
import json  # noqa: E402
import tempfile  # noqa: E402
from pathlib import Path  # noqa: E402


class RussiaRouteEnvironmentTests(unittest.TestCase):
    def environment(self, **changes):
        return L.game_environment({**L.APP_DEFAULTS, 'online': True, 'supermedo_npid': 'hunter',
                                   'supermedo_pass': 'p', **changes})

    def test_off_by_default_the_server_as_before(self):
        env = self.environment()
        self.assertEqual((env['BB_SHADNET_SERVER'], env['BB_SHADNET_WEBAPI']),
                         (L.SUPERMEDO_SERVER, L.SUPERMEDO_WEBAPI))
        self.assertNotIn('BB_HOST_ROUTES', env)
        self.assertEqual(env['BB_COMMUNITY_SERVER'], L.COMMUNITY_SERVER)

    def test_on_the_relay_for_both_hosts(self):
        env = self.environment(online_relay=True)
        self.assertEqual(env['BB_SHADNET_SERVER'], f'{L.RUSSIA_RELAY}:31313')
        self.assertEqual(env['BB_SHADNET_WEBAPI'], f'http://{L.RUSSIA_RELAY}:31315')
        self.assertEqual(env['BB_HOST_ROUTES'],
                         f'np.supermedo.com={L.RUSSIA_RELAY},game.supermedo.com={L.RUSSIA_RELAY}')
        self.assertEqual(env['BB_COMMUNITY_SERVER'], L.COMMUNITY_SERVER)  # the name, for its certificate

    def test_only_with_supermedo_server(self):
        for pool in ('seamless', 'shadnet'):
            env = self.environment(online_relay=True, online_pool=pool)
            self.assertNotIn('BB_HOST_ROUTES', env, pool)
            self.assertNotEqual(env.get('BB_SHADNET_SERVER'), f'{L.RUSSIA_RELAY}:31313', pool)

    def test_offline_nothing(self):
        self.assertNotIn('BB_HOST_ROUTES', self.environment(online=False, online_relay=True))


class LauncherRoutesTests(unittest.TestCase):
    def tearDown(self):
        bbport_tls.set_routes({})

    def test_a_routed_host_connects_to_the_relay(self):
        bbport_tls.set_routes(L.relay_routes({'online_relay': True, 'online_pool': 'supermedo'}))
        self.assertEqual(bbport_tls.routed(('game.supermedo.com', 443)), (L.RUSSIA_RELAY, 443))
        self.assertEqual(bbport_tls.routed(('NP.supermedo.com', 31313)), (L.RUSSIA_RELAY, 31313))
        self.assertEqual(bbport_tls.routed(('github.com', 443)), ('github.com', 443))

    def test_the_socket_module_goes_through_the_routes(self):
        bbport_tls.set_routes({'game.supermedo.com': '192.0.2.1'})
        with mock.patch.object(bbport_tls, '_create_connection', return_value='sock') as connect:
            self.assertEqual(socket.create_connection(('game.supermedo.com', 20443), timeout=3), 'sock')
        connect.assert_called_once_with(('192.0.2.1', 20443), timeout=3)

    def test_no_routes_no_change(self):
        bbport_tls.set_routes(L.relay_routes({'online_relay': False, 'online_pool': 'supermedo'}))
        self.assertEqual(bbport_tls.routed(('game.supermedo.com', 443)), ('game.supermedo.com', 443))


class SeamlessRouteTests(unittest.TestCase):
    """Seamless co-op through the relay: its own ports there, sent on to the seamless server's."""
    def test_the_same_relay_as_normal_online(self):
        self.assertEqual(Y.RELAY_IP, L.RUSSIA_RELAY)

    def test_off_the_seamless_server_as_before(self):
        self.assertEqual(Y.server_ip({}, lookup=lambda host: '203.0.113.7'), '203.0.113.7')
        self.assertEqual(Y.ports({}), (31314, 31315, 20443))

    def test_on_the_relay_and_its_ports(self):
        s = {'online_relay': True}
        self.assertEqual(Y.server_ip(s, lookup=lambda host: self.fail('no lookup')), L.RUSSIA_RELAY)
        with tempfile.TemporaryDirectory() as port_dir:
            env = Y.environment(port_dir, port_dir, s, Y.server_ip(s), game_env={})
            self.assertEqual(env['BB_SHADNET_SERVER'], f'{L.RUSSIA_RELAY}:41314')
            self.assertEqual(env['BB_SHADNET_WEBAPI'], f'http://{L.RUSSIA_RELAY}:41315')
            path = Y.write_host_overrides(port_dir, Y.server_ip(s), Y.ports(s)[2])
            self.assertEqual(json.loads(Path(path).read_text(encoding='utf-8')),
                             {Y.GAME_SERVER: f'http://{L.RUSSIA_RELAY}:40443'})

    def test_the_launcher_routes_with_the_seamless_pool_too(self):
        self.assertTrue(L.relay_routes({'online_relay': True, 'online_pool': 'seamless'}))
        self.assertFalse(L.relay_routes({'online_relay': True, 'online_pool': 'shadnet'}))


if __name__ == '__main__':
    unittest.main()
