"""The players online on The Hunter's Dream (launcher/bbport_launcher_win.py hunters_online): read
from the public statistics its start page shows, nothing when the server does not answer."""
from paths import ROOT
import contextlib
import io
import json
import os
import sys
import types
import unittest
import urllib.error
from unittest import mock

sys.path.insert(0, str(ROOT / 'launcher'))
sys.path.insert(0, str(ROOT / 'scripts'))
import bbport_launcher_win as L  # noqa: E402


def answer(data):
    response = io.BytesIO(json.dumps(data).encode())
    response.__enter__ = lambda *a: response
    response.__exit__ = lambda *a: None
    return mock.patch.object(L.bbport_tls, 'urlopen', return_value=response)


class HuntersOnlineTests(unittest.TestCase):
    def test_the_count_and_the_split(self):
        data = {'stats': {}, 'active_sessions': {'count': 798, 'server_online': True,
                                                 'by_client': {'bbhost': 787, 'shadps4': 11}}}
        with answer(data):
            self.assertEqual(L.hunters_online(), (798, 787, 11))

    def test_nothing_when_the_game_server_is_down_or_unreachable(self):
        with answer({'active_sessions': {'count': 0, 'server_online': False}}):
            self.assertIsNone(L.hunters_online())
        with mock.patch.object(L.bbport_tls, 'urlopen', side_effect=OSError('offline')):
            self.assertIsNone(L.hunters_online())
        with answer(['not', 'what', 'it', 'sends']):
            self.assertIsNone(L.hunters_online())

    def test_asked_rarely(self):
        self.assertGreaterEqual(L.HUNTERS_ONLINE_EVERY, 600)  # its answer is about 140 KB


class SignInTests(unittest.TestCase):
    """The Discord sign-in's calls go through the launcher's HTTPS (bbport_tls: expired copies in the
    Windows stores left out, Windows' trust when OpenSSL refuses), like every request of the launcher.
    Nothing is sent: the real device/start would make a sign-in code on the server."""

    def test_the_account_service_is_asked_through_it(self):
        with answer({'ResKind': 0, 'DeviceCode': 'd', 'UserCode': 'U'}) as urlopen:
            reply = L.thd_call(L.HUNTERS_DREAM_SERVER + '/auth/device/start', {'DeviceName': 'bbport on PC'})
        self.assertEqual(reply['UserCode'], 'U')
        request = urlopen.call_args.args[0]
        self.assertEqual((request.full_url, request.get_method()),
                         ('https://thehuntersdream.com/auth/device/start', 'POST'))

    def test_a_refused_certificate_says_why(self):
        reason = L.bbport_tls.CertificateRefused('The date or time of this PC looks wrong.', OSError())
        with mock.patch.object(L.bbport_tls, 'urlopen', side_effect=reason):
            with self.assertRaises(OSError) as caught:  # the sign-in shows "Sign-in failed: {}"
                L.thd_sign_in(lambda page, code: None)
        self.assertEqual(str(caught.exception), 'The date or time of this PC looks wrong.')



class PoolTests(unittest.TestCase):
    """The co-op pools: SuperMedo Server first and by default, The Hunter's Dream (the previous server) for the
    time being, a shadNet server of the player's choice."""

    def test_supermedo_server_by_default(self):
        self.assertEqual(L.APP_DEFAULTS['online_pool'], 'supermedo')
        self.assertTrue(L.HUNTERS_DREAM_POOL)

    def test_supermedo_server_first_and_the_hunters_dream_the_previous_server(self):
        source = (ROOT / 'launcher' / 'bbport_launcher_win.py').read_text(encoding='utf-8')
        page = source[source.index('    def build_online(self):'):source.index('    def build_mods(self):')]
        self.assertLess(page.index("value='supermedo'"), page.index("value='huntersdream'"))
        self.assertLess(page.index("value='huntersdream'"), page.index("value='shadnet'"))
        self.assertIn('_("The Hunter\'s Dream (previous server)")', page)
        self.assertIn("_('Switch to SuperMedo Server')", page)

    def test_the_supermedo_pool_plays_on_supermedo_server(self):
        settings = {**L.APP_DEFAULTS, 'online': True, 'online_pool': 'supermedo', 'thd_online_id': 'Hunter',
                    'thd_token': 't'}
        with mock.patch.object(L, 'HUNTERS_DREAM_POOL', True):
            env = L.game_environment(settings)
        self.assertNotEqual(env.get('BB_NET_BACKEND'), 'thd')
        self.assertEqual((env['BB_SHADNET_SERVER'], env['BB_COMMUNITY_SERVER']), (L.SUPERMEDO_SERVER, L.COMMUNITY_SERVER))

    def test_the_hunters_dream_has_its_own_address(self):
        self.assertIn('supermedo.com', L.COMMUNITY_SERVER)
        self.assertEqual(L.HUNTERS_DREAM_SERVER, 'https://thehuntersdream.com')

    def test_a_start_moves_dead_ends_to_supermedo_server(self):
        """Beta 8 testers could not play online: an earlier launcher left them on shadNet without own servers,
        shadPS4's public server, which refuses the port. The Hunter's Dream (off) and unknown pools too."""
        for pool in ('shadnet', 'gone', None):
            self.assertEqual(L.settled_pool({**L.APP_DEFAULTS, 'online_pool': pool,
                                             'online_custom_servers': False}), 'supermedo', pool)
        self.assertEqual(L.settled_pool({**L.APP_DEFAULTS, 'online_pool': 'huntersdream'}), 'huntersdream')
        with mock.patch.object(L, 'HUNTERS_DREAM_POOL', False):  # BB_HUNTERS_DREAM=0
            self.assertEqual(L.settled_pool({**L.APP_DEFAULTS, 'online_pool': 'huntersdream'}), 'supermedo')

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
        cases = [({'online_pool': 'huntersdream'}, True), ({'online_pool': 'huntersdream', 'thd_token': 't',
                                                            'thd_online_id': 'h'}, False),
                 ({'online_pool': 'supermedo'}, True), ({'online_pool': 'supermedo', 'thd_token': 't',
                                                         'thd_online_id': 'h', 'online_npid': 'n'}, True),
                 ({'online_pool': 'supermedo', 'supermedo_npid': 'n', 'supermedo_pass': 'p'}, False),
                 ({'online_pool': 'shadnet'}, False)]
        for settings, asks in cases:
            self.assertEqual(L.needs_sign_in({'online': True, **settings}), asks, settings)
            self.assertFalse(L.needs_sign_in({'online': False, **settings}))

    def test_each_service_keeps_its_own_account(self):
        """SuperMedo Server's account was in the shadNet boxes, which Seamless co-op writes too: one overwrote the
        other. Each pool signs in with its own now."""
        settings = {**L.APP_DEFAULTS, 'online': True, 'supermedo_npid': 'medo', 'supermedo_pass': 'mp',
                    'thd_online_id': 'hunter', 'thd_token': 't', 'online_npid': 'seamless', 'online_password': 'sp',
                    'online_custom_servers': True, 'online_server': 'play.seamlesshunt.com:31313'}
        env = L.game_environment({**settings, 'online_pool': 'supermedo'})
        self.assertEqual((env['BB_SHADNET_SERVER'], env['BB_SHADNET_NPID'], env['BB_SHADNET_PASSWORD']),
                         (L.SUPERMEDO_SERVER, 'medo', 'mp'))
        env = L.game_environment({**settings, 'online_pool': 'shadnet'})
        self.assertEqual((env['BB_SHADNET_SERVER'], env['BB_SHADNET_NPID'], env['BB_SHADNET_PASSWORD']),
                         ('play.seamlesshunt.com:31313', 'seamless', 'sp'))
        env = L.game_environment({**settings, 'online_pool': 'huntersdream'})
        self.assertEqual((env['BB_NET_BACKEND'], env['BB_THD_ONLINE_ID']), ('thd', 'hunter'))
        self.assertNotIn('BB_SHADNET_NPID', env)
        for pool, name in (('supermedo', 'medo'), ('huntersdream', 'hunter'), ('shadnet', 'seamless')):
            self.assertEqual(L.pool_account({**settings, 'online_pool': pool}), name)
        self.assertEqual(L.pool_account({**L.APP_DEFAULTS, 'online_npid': 'seamless'}), '')

    def test_the_supermedo_account_leaves_the_shadnet_boxes_once(self):
        """Saved on SuperMedo Server up to beta 8: its account was in the shadNet boxes. Other pools' boxes are
        shadNet's (a public or a co-op server's account) and stay there."""
        beta8 = {'online_pool': 'supermedo', 'online_npid': 'medo', 'online_password': 'mp'}
        self.assertEqual(L.settled_online({**L.APP_DEFAULTS, **beta8}, beta8)['supermedo_npid'], 'medo')
        self.assertEqual(L.settled_online({**L.APP_DEFAULTS, **beta8}, beta8)['supermedo_pass'], 'mp')
        for saved in ({**beta8, 'supermedo_npid': 'other'}, {**beta8, 'online_pool': 'shadnet'},
                      {**beta8, 'online_pool': 'huntersdream'}):
            self.assertNotIn('supermedo_npid', L.settled_online({**L.APP_DEFAULTS, **saved}, saved))

    def test_another_programs_writes(self):
        """Seamless co-op writes settings.json from outside: its shadNet settings are taken, the pool and the
        SuperMedo Server and The Hunter's Dream sign-ins stay the launcher's (the Online page writes them back)."""
        written = {**{key: L.APP_DEFAULTS[key] for key in L.SHADNET_KEYS + L.OWN_ONLINE_KEYS},
                   'supermedo_npid': 'medo', 'thd_token': 't'}
        self.assertEqual(L.outside_online_changes(written, dict(written)), ({}, []))
        on_disk = {**written, 'online_server': 'bbsc.example:31313', 'online_npid': 'bbsc',
                   'online_pool': 'shadnet', 'supermedo_npid': '', 'thd_token': ''}
        taken, kept = L.outside_online_changes(written, on_disk)
        self.assertEqual(taken, {'online_server': 'bbsc.example:31313', 'online_npid': 'bbsc'})
        self.assertEqual(sorted(kept), ['online_pool', 'supermedo_npid', 'thd_token'])

    def test_only_seamless_coop_keeps_its_own_server(self):
        """Since 1.9.5 beta 10 only SuperMedo Server and The Hunter's Dream: an own server moves to SuperMedo Server,
        except under Seamless co-op's own start (bbsc-launch.exe the game program), whose server it is."""
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
    A co-op tool wrote its own address into it, and The Hunter's Dream's sign-in went there: "has no
    account service"."""
    OTHER = 'http://play.seamlesshunt.com'

    def environment(self, **changes):
        settings = {**L.APP_DEFAULTS, 'online': True, 'thd_online_id': 'Hunter', 'thd_token': 't',
                    'online_community': self.OTHER, **changes}
        with mock.patch.object(L, 'HUNTERS_DREAM_POOL', True):
            return L.game_environment(settings)

    def test_the_hunters_dream_plays_on_its_own_server(self):
        for custom in (False, True):
            env = self.environment(online_pool='huntersdream', online_custom_servers=custom)
            self.assertEqual(env['BB_COMMUNITY_SERVER'], L.HUNTERS_DREAM_SERVER)

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

    def test_the_sign_in_asks_the_hunters_dream(self):
        asked = []

        def call(url, body):
            asked.append(url)
            return {'DeviceCode': 'd', 'UserCode': 'U', 'Interval': 1} if url.endswith('/start') else \
                {'Status': 'approved', 'OnlineId': 'Hunter', 'Token': 't'}
        with mock.patch.object(L, 'thd_call', call), mock.patch.object(L.time, 'sleep'):
            self.assertEqual(L.thd_sign_in(lambda page, code: None), ('Hunter', 't'))
        self.assertTrue(asked and all(url.startswith(L.HUNTERS_DREAM_SERVER + '/auth/') for url in asked))

    def server(self, *answers):
        """thd_open answering in turn: (status, body) or an exception; the calls are counted."""
        answers, calls = list(answers), []

        def open_(request):
            calls.append(request.full_url)
            answer = answers.pop(0)
            if isinstance(answer, BaseException):
                raise answer
            status, body = answer
            if status >= 400:
                raise urllib.error.HTTPError(request.full_url, status, 'error', {}, io.BytesIO(body))
            return contextlib.nullcontext(types.SimpleNamespace(status=status, read=lambda: body))
        return calls, mock.patch.object(L, 'thd_open', open_)

    def test_the_sign_in_asks_again_when_the_server_stumbles(self):
        """A tester's "Sign-in failed: …/auth/device/start has no account service" passed after a few clicks."""
        calls, opened = self.server((502, b'<html>Bad gateway</html>'), TimeoutError('timed out'),
                                    (200, b'{"ResKind": 0, "DeviceCode": "d"}'))
        with opened, mock.patch.object(L.time, 'sleep') as slept:
            self.assertEqual(L.thd_call(L.HUNTERS_DREAM_SERVER + '/auth/device/start', {})['DeviceCode'], 'd')
        self.assertEqual(len(calls), 3)
        self.assertEqual([c.args[0] for c in slept.call_args_list], [1, 2])

    def test_a_server_that_keeps_stumbling_says_so(self):
        calls, opened = self.server(*[(503, b'<html>down</html>')] * 3)
        with opened, mock.patch.object(L.time, 'sleep'), self.assertRaises(ValueError) as caught:
            L.thd_call(L.HUNTERS_DREAM_SERVER + '/auth/device/start', {})
        self.assertEqual(len(calls), 3)
        self.assertEqual(str(caught.exception), 'thehuntersdream.com did not answer properly (HTTP 503); '
                                                'try again in a minute')

    def test_the_services_own_refusal_is_not_asked_again(self):
        calls, opened = self.server((200, b'{"ResKind": 3, "Message": "unknown device"}'))
        with opened, mock.patch.object(L.time, 'sleep'), self.assertRaises(ValueError) as caught:
            L.thd_call(L.HUNTERS_DREAM_SERVER + '/auth/device/poll', {})
        self.assertEqual((len(calls), str(caught.exception)), (1, 'unknown device'))

    def test_a_connection_that_keeps_failing_shows_its_reason(self):
        """A certificate the PC cannot check keeps its own message (Help explains it)."""
        error = urllib.error.URLError('certificate verify failed: certificate has expired')
        calls, opened = self.server(error, error, error)
        with opened, mock.patch.object(L.time, 'sleep'), self.assertRaises(urllib.error.URLError) as caught:
            L.thd_call(L.HUNTERS_DREAM_SERVER + '/auth/device/start', {})
        self.assertEqual(len(calls), 3)
        self.assertIn('certificate has expired', str(caught.exception))

    def test_own_servers_on_for_settings_from_before_the_box(self):
        """Saved before "Use my own servers" existed, with a private server: the box starts on."""
        self.assertTrue(L.has_own_servers({**L.APP_DEFAULTS, 'online_server': 'friend.example:31313'}))
        self.assertTrue(L.has_own_servers({**L.APP_DEFAULTS, 'online_community': self.OTHER}))
        self.assertFalse(L.has_own_servers(L.APP_DEFAULTS))
        self.assertFalse(L.has_own_servers({**L.APP_DEFAULTS, 'online_server': ' '}))


if __name__ == '__main__':
    unittest.main()
