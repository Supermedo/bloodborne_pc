"""The players online on The Hunter's Dream (launcher/bbport_launcher_win.py hunters_online): read
from the public statistics its start page shows, nothing when the server does not answer."""
from paths import ROOT
import contextlib
import io
import json
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
    return mock.patch.object(L.urllib.request, 'urlopen', return_value=response)


class HuntersOnlineTests(unittest.TestCase):
    def test_the_count_and_the_split(self):
        data = {'stats': {}, 'active_sessions': {'count': 798, 'server_online': True,
                                                 'by_client': {'bbhost': 787, 'shadps4': 11}}}
        with answer(data):
            self.assertEqual(L.hunters_online(), (798, 787, 11))

    def test_nothing_when_the_game_server_is_down_or_unreachable(self):
        with answer({'active_sessions': {'count': 0, 'server_online': False}}):
            self.assertIsNone(L.hunters_online())
        with mock.patch.object(L.urllib.request, 'urlopen', side_effect=OSError('offline')):
            self.assertIsNone(L.hunters_online())
        with answer(['not', 'what', 'it', 'sends']):
            self.assertIsNone(L.hunters_online())

    def test_asked_rarely(self):
        self.assertGreaterEqual(L.HUNTERS_ONLINE_EVERY, 600)  # its answer is about 140 KB



class PoolTests(unittest.TestCase):
    """The co-op pools: The Hunter's Dream by default, a shadNet server of the player's choice; SuperMedo
    Server is no longer in the launcher."""

    def test_the_hunters_dream_by_default(self):
        self.assertEqual(L.APP_DEFAULTS['online_pool'], 'huntersdream')
        self.assertFalse([key for key in L.APP_DEFAULTS if 'supermedo' in key])

    def test_a_saved_supermedo_pool_plays_on_the_hunters_dream(self):
        settings = {**L.APP_DEFAULTS, 'online': True, 'online_pool': 'supermedo', 'thd_online_id': 'Hunter',
                    'thd_token': 't'}
        with mock.patch.object(L, 'HUNTERS_DREAM_POOL', True):
            env = L.game_environment(settings)
        self.assertEqual(env.get('BB_NET_BACKEND'), 'thd')
        self.assertNotIn('supermedo', env.get('BB_SHADNET_SERVER', '').lower())

    def test_no_supermedo_server_left(self):
        source = (ROOT / 'launcher' / 'bbport_launcher_win.py').read_text(encoding='utf-8').lower()
        self.assertNotIn('supermedo.com', source)
        self.assertNotIn("value='supermedo'", source)


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

    def test_the_sign_in_asks_the_hunters_dream(self):
        asked = []

        def call(url, body):
            asked.append(url)
            return {'DeviceCode': 'd', 'UserCode': 'U', 'Interval': 1} if url.endswith('/start') else \
                {'Status': 'approved', 'OnlineId': 'Hunter', 'Token': 't'}
        with mock.patch.object(L, 'thd_call', call), mock.patch.object(L.time, 'sleep'):
            self.assertEqual(L.thd_sign_in(lambda page, code: None), ('Hunter', 't'))
        self.assertTrue(asked and all(url.startswith(L.COMMUNITY_SERVER + '/auth/') for url in asked))

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
            self.assertEqual(L.thd_call(L.COMMUNITY_SERVER + '/auth/device/start', {})['DeviceCode'], 'd')
        self.assertEqual(len(calls), 3)
        self.assertEqual([c.args[0] for c in slept.call_args_list], [1, 2])

    def test_a_server_that_keeps_stumbling_says_so(self):
        calls, opened = self.server(*[(503, b'<html>down</html>')] * 3)
        with opened, mock.patch.object(L.time, 'sleep'), self.assertRaises(ValueError) as caught:
            L.thd_call(L.COMMUNITY_SERVER + '/auth/device/start', {})
        self.assertEqual(len(calls), 3)
        self.assertEqual(str(caught.exception), 'thehuntersdream.com did not answer properly (HTTP 503); '
                                                'try again in a minute')

    def test_the_services_own_refusal_is_not_asked_again(self):
        calls, opened = self.server((200, b'{"ResKind": 3, "Message": "unknown device"}'))
        with opened, mock.patch.object(L.time, 'sleep'), self.assertRaises(ValueError) as caught:
            L.thd_call(L.COMMUNITY_SERVER + '/auth/device/poll', {})
        self.assertEqual((len(calls), str(caught.exception)), (1, 'unknown device'))

    def test_a_connection_that_keeps_failing_shows_its_reason(self):
        """A certificate the PC cannot check keeps its own message (Help explains it)."""
        error = urllib.error.URLError('certificate verify failed: certificate has expired')
        calls, opened = self.server(error, error, error)
        with opened, mock.patch.object(L.time, 'sleep'), self.assertRaises(urllib.error.URLError) as caught:
            L.thd_call(L.COMMUNITY_SERVER + '/auth/device/start', {})
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
