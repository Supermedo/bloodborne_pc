"""Party codes on the Online page (launcher/bbport_launcher_win.py party_call, party_line): the calls to
SuperMedo Server's /party/v1 and how a party reads."""
from paths import ROOT
import io
import json
import sys
import unittest
import urllib.error
from unittest import mock

sys.path.insert(0, str(ROOT / 'launcher'))
sys.path.insert(0, str(ROOT / 'scripts'))
import bbport_launcher_win as L  # noqa: E402


class Reply(io.BytesIO):
    status = 200

    def __enter__(self):
        return self

    def __exit__(self, *_args):
        return False


class PartyTest(unittest.TestCase):
    def test_a_call_posts_json_with_the_key(self):
        seen = {}

        def urlopen(request, timeout):
            seen.update(url=request.full_url, body=json.loads(request.data), auth=request.get_header('Authorization'),
                        method=request.get_method())
            return Reply(json.dumps({'name': 'Medo', 'party': None}).encode())
        with mock.patch.object(L.bbport_tls, 'urlopen', urlopen):
            status, answer = L.party_call('join', {'code': 'SM-ABCD-EFGH'}, 'k3y')
        self.assertEqual((status, answer), (200, {'name': 'Medo', 'party': None}))
        self.assertEqual(seen, {'url': L.COMMUNITY_SERVER + '/party/v1/join', 'body': {'code': 'SM-ABCD-EFGH'},
                                'auth': 'Bearer k3y', 'method': 'POST'})

    def test_a_refusal_gives_the_servers_reason(self):
        def urlopen(request, timeout):
            raise urllib.error.HTTPError(request.full_url, 409, 'Conflict', {},
                                         io.BytesIO(b'{"error": "that party is full"}'))
        with mock.patch.object(L.bbport_tls, 'urlopen', urlopen):
            self.assertEqual(L.party_call('join', {'code': 'x'}, 'k'), (409, {'error': 'that party is full'}))

    def test_a_refusal_without_json(self):
        def urlopen(request, timeout):
            raise urllib.error.HTTPError(request.full_url, 502, 'Bad Gateway', {}, io.BytesIO(b'<html>'))
        with mock.patch.object(L.bbport_tls, 'urlopen', urlopen):
            status, answer = L.party_call('status', None, 'k')
        self.assertEqual(status, 502)
        self.assertIn('error', answer)

    def test_the_party_line(self):
        party = {'members': [{'name': 'Alice', 'host': True, 'online': True}, {'name': 'Bob', 'host': False, 'online': False}]}
        self.assertEqual(L.party_line(party), 'Alice (host, online) · Bob (offline)')

    def test_the_key_is_the_launchers_own_online_setting(self):
        """Another program writing the shadNet settings doesn't take it (OWN_ONLINE_KEYS), and it starts empty."""
        self.assertIn('party_key', L.OWN_ONLINE_KEYS)
        self.assertEqual(L.APP_DEFAULTS['party_key'], '')


if __name__ == '__main__':
    unittest.main()
