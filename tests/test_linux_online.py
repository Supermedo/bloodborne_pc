"""The Linux launcher's online play (launcher/bbport_linux_online.py) and its use in bbport_launcher.py, without
GTK or a network: SuperMedo Server's variables and host_overrides.json, a Windows user folder's BBSC files and
Seamless co-op saves, the party calls, the Discord sign-in and the shadNet account request against local fakes."""
from paths import ROOT
import ast
import importlib
import io
import json
import os
import socket
import struct
import sys
import tempfile
import threading
import types
import unittest
import urllib.error
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(ROOT / 'launcher'))
import bbport_linux_online as online


class Reply(io.BytesIO):
    def __init__(self, body, status=200):
        super().__init__(json.dumps(body).encode())
        self.status = status

    def __enter__(self):
        return self

    def __exit__(self, *_exc):
        return False


class FakeServer:
    """An opener for urlopen(request, timeout): answers from a list of (path, status, body) in order."""

    def __init__(self, answers):
        self.answers, self.requests = list(answers), []

    def __call__(self, request, _timeout):
        path, status, body = self.answers.pop(0)
        self.requests.append(request)
        assert request.full_url.endswith(path), (request.full_url, path)
        if status != 200:
            raise urllib.error.HTTPError(request.full_url, status, 'no', {}, io.BytesIO(json.dumps(body).encode()))
        return Reply(body)


class EnvironmentTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.user = Path(self.temp.name) / 'user'
        self.lines = []

    def settings(self, **values):
        return {**online.DEFAULTS, 'supermedo_npid': 'hunter', 'supermedo_pass': 'secret', **values}

    def test_offline_adds_nothing(self):
        self.assertEqual(online.environment(self.settings(), self.user, self.lines.append), {})
        self.assertFalse(self.user.exists())

    def test_supermedo_server(self):
        env = online.environment(self.settings(online=True), self.user, self.lines.append)
        self.assertEqual(env['BB_ONLINE'], '1')
        self.assertEqual(env['BB_SHADNET_SERVER'], 'np.supermedo.com:31313')
        self.assertEqual(env['BB_SHADNET_WEBAPI'], 'http://np.supermedo.com:31315')
        self.assertEqual((env['BB_SHADNET_NPID'], env['BB_SHADNET_PASSWORD']), ('hunter', 'secret'))
        self.assertEqual(env['BB_UPNP'], '1')
        self.assertEqual(env['BB_COMMUNITY_SERVER'], 'https://game.supermedo.com')
        self.assertEqual(env['BB_CLIENT_VERSION'], f'bbport/{online.VERSION}')
        self.assertNotIn('BB_PROBE', env)  # never BBSC's launcher: Seamless co-op is Windows only
        path = Path(env['SHADPS4_HTTP_HOST_OVERRIDES_JSON'])
        self.assertEqual(path, self.user / 'host_overrides.json')
        overrides = json.loads(path.read_text(encoding='utf-8'))
        self.assertEqual(overrides['https://ss4.scej-network.jp:20443'], 'https://game.supermedo.com')
        self.assertEqual(overrides['https://bb-playlog-prod.s3.amazonaws.com:443'], 'https://game.supermedo.com:18671')
        self.assertEqual(self.lines, ['Co-op: game server https://game.supermedo.com'])

    def test_upnp_off(self):
        env = online.environment(self.settings(online=True, online_upnp=False), self.user, self.lines.append)
        self.assertEqual(env['BB_UPNP'], '0')

    def test_own_custom_overrides_are_used(self):
        self.user.mkdir()
        custom = self.user / 'host_overrides.custom.json'
        custom.write_text('{"https://ss4.scej-network.jp:20443": "https://example.org"}', encoding='utf-8')
        env = online.environment(self.settings(online=True), self.user, self.lines.append)
        self.assertEqual(env['SHADPS4_HTTP_HOST_OVERRIDES_JSON'], str(custom))
        self.assertFalse((self.user / 'host_overrides.json').exists())

    def test_bbsc_custom_overrides_from_windows_are_not(self):
        self.user.mkdir()
        (self.user / 'host_overrides.custom.json').write_text(
            '{"https://summon_messenger.example": "http://127.0.0.1:31315/"}', encoding='utf-8')
        env = online.environment(self.settings(online=True), self.user, self.lines.append)
        self.assertEqual(env['SHADPS4_HTTP_HOST_OVERRIDES_JSON'], str(self.user / 'host_overrides.json'))
        self.assertIn("BBSC's", self.lines[0])

    def test_account(self):
        self.assertEqual(online.account(self.settings()), 'hunter')
        self.assertEqual(online.account(self.settings(supermedo_npid=' hunter ')), 'hunter')
        self.assertEqual(online.account(self.settings(supermedo_pass='')), '')

    def test_coop_saves(self):
        self.assertTrue(online.coop_saves('/home/me/bbport-windows/user/seamless-coop'))
        self.assertFalse(online.coop_saves('/home/me/.local/share/bbport/user'))


class PartyTests(unittest.TestCase):
    def test_stored_key(self):
        server = FakeServer([('/party/v1/status', 200, {'party': None})])
        answer, key = online.party_act('status', None, 'hunter', 'secret', 'k1', server)
        self.assertEqual((answer, key), ({'party': None}, 'k1'))
        self.assertEqual(server.requests[0].get_header('Authorization'), 'Bearer k1')
        self.assertIn('(Linux)', server.requests[0].get_header('User-agent'))

    def test_signin_without_a_key_and_after_an_old_one(self):
        for stored, answers in (('', []), ('old', [('/party/v1/host', 401, {'error': 'unknown key'})])):
            with self.subTest(stored=stored):
                server = FakeServer(answers + [('/party/v1/signin', 200, {'key': 'new'}),
                                               ('/party/v1/host', 200, {'party': {'code': 'SM-AAAA-BBBB'}})])
                answer, key = online.party_act('host', None, 'hunter', 'secret', stored, server)
                self.assertEqual(key, 'new')
                self.assertEqual(answer['party']['code'], 'SM-AAAA-BBBB')
                signin = server.requests[-2]
                self.assertEqual(json.loads(signin.data), {'name': 'hunter', 'password': 'secret'})
                self.assertEqual(server.requests[-1].get_header('Authorization'), 'Bearer new')

    def test_refusals_say_why(self):
        server = FakeServer([('/party/v1/signin', 403, {'error': 'wrong password'})])
        with self.assertRaisesRegex(ValueError, 'wrong password'):
            online.party_act('status', None, 'hunter', 'bad', '', server)
        server = FakeServer([('/party/v1/join', 404, {'error': 'no such party'})])
        with self.assertRaisesRegex(ValueError, 'no such party'):
            online.party_act('join', {'code': 'SM-0000-0000'}, 'hunter', 'secret', 'k1', server)


class SignInTests(unittest.TestCase):
    def test_discord(self):
        server = FakeServer([('/auth/start', 200, {'url': 'https://discord.example/x', 'session': 's1'}),
                             ('/auth/poll?session=s1', 200, {'status': 'pending'}),
                             ('/auth/poll?session=s1', 200, {'status': 'done', 'npid': 'hunter', 'password': 'pw'})])
        pages = []
        self.assertEqual(online.discord_sign_in(pages.append, opener=server, sleep=lambda _s: None), ('hunter', 'pw'))
        self.assertEqual(pages, ['https://discord.example/x'])
        self.assertEqual(server.requests[0].get_method(), 'POST')

    def test_discord_failed(self):
        server = FakeServer([('/auth/start', 200, {'url': 'u', 'session': 's'}),
                             ('/auth/poll?session=s', 200, {'status': 'failed', 'message': 'cancelled'})])
        with self.assertRaisesRegex(ValueError, 'cancelled'):
            online.discord_sign_in(lambda _u: None, opener=server, sleep=lambda _s: None)

    def shadnet(self, error):
        """A one-request shadNet server: ServerInfo, then the Create reply with `error`; returns (address, got)."""
        listener = socket.create_server(('127.0.0.1', 0))
        got = {}

        def serve():
            with listener, listener.accept()[0] as sock:
                info = b'\x00' + struct.pack('<H', 0) + struct.pack('<I', 19) + bytes(8) + struct.pack('<I', 1)
                sock.sendall(info)
                header = sock.recv(15)
                size = struct.unpack_from('<I', header, 3)[0]
                body = b''
                while len(body) < size - 15:
                    body += sock.recv(size - 15 - len(body))
                got['command'], got['body'] = struct.unpack_from('<H', header, 1)[0], body
                sock.sendall(bytes(15) + bytes([error]))
        thread = threading.Thread(target=serve, daemon=True)
        thread.start()
        return f'127.0.0.1:{listener.getsockname()[1]}', got, thread

    def test_create_account(self):
        address, got, thread = self.shadnet(0)
        self.assertIsNone(online.shadnet_register(address, 'hunter', 'pw', 'a@b.c', timeout=5))
        thread.join(5)
        self.assertEqual(got['command'], 2)
        self.assertIn(b'hunter', got['body'])
        self.assertIn(b'a@b.c', got['body'])

    def test_create_account_refused(self):
        address, _got, thread = self.shadnet(11)
        self.assertEqual(online.shadnet_register(address, 'hunter', 'pw', 'a@b.c', timeout=5), 'This name is taken.')
        thread.join(5)


class VersionTests(unittest.TestCase):
    def test_same_version_as_the_windows_launcher(self):
        tree = ast.parse((ROOT / 'launcher' / 'bbport_launcher_win.py').read_text(encoding='utf-8'))
        version = next(node.value.value for node in tree.body if isinstance(node, ast.Assign)
                       and any(getattr(t, 'id', None) == 'VERSION' for t in node.targets))
        self.assertEqual(online.VERSION, version)


def fake_gi():
    """Just enough of PyGObject for bbport_launcher.py's module level (its window is not built)."""
    repository = types.SimpleNamespace(
        Adw=types.SimpleNamespace(ApplicationWindow=object, Application=object),
        Gdk=types.SimpleNamespace(), Gio=types.SimpleNamespace(), GLib=types.SimpleNamespace(),
        Gtk=types.SimpleNamespace())
    gi = types.ModuleType('gi')
    gi.require_version = lambda *_a: None
    gi.repository = repository
    module = types.ModuleType('gi.repository')
    module.__dict__.update(vars(repository))
    return {'gi': gi, 'gi.repository': module}


class LinuxLauncherTests(unittest.TestCase):
    """bbport_launcher.py's settings and the environment it gives run.sh."""

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.data = Path(self.temp.name) / 'data'
        environ = {'BB_DATA_DIR': str(self.data), 'XDG_CONFIG_HOME': str(Path(self.temp.name) / 'config'),
                   'BB_PREBUILT': '1'}
        patches = [mock.patch.dict(os.environ, environ), mock.patch.dict(sys.modules, fake_gi())]
        for patch in patches:
            patch.start()
            self.addCleanup(patch.stop)
        sys.modules.pop('bbport_launcher', None)
        self.addCleanup(sys.modules.pop, 'bbport_launcher', None)
        self.launcher = importlib.import_module('bbport_launcher')

    def test_defaults_have_online_off(self):
        settings = self.launcher.load_settings()
        self.assertFalse(settings['online'])
        self.assertTrue(settings['online_upnp'])
        self.assertEqual(settings['game_dir'], '')  # packaged: chosen in the launcher

    def test_settings_are_the_linux_launchers_own(self):
        self.assertEqual(self.launcher.CONFIG_FILE,
                         Path(self.temp.name) / 'config' / 'bbport-launcher' / 'settings.json')

    def test_settings_saved_private(self):
        settings = self.launcher.load_settings()
        settings.update(supermedo_npid='hunter', supermedo_pass='secret')
        self.launcher.save_settings(settings)
        self.assertEqual(self.launcher.load_settings()['supermedo_pass'], 'secret')
        if os.name == 'posix':
            self.assertEqual(self.launcher.CONFIG_FILE.stat().st_mode & 0o777, 0o600)

    def test_game_environment(self):
        settings = self.launcher.load_settings()
        settings.update(game_dir='/games/CUSA03173', online=True, supermedo_npid='hunter', supermedo_pass='secret')
        lines = []
        env = self.launcher.game_environment(settings, log=lines.append)
        self.assertEqual(env['BB_GAME_DIR'], str(Path('/games/CUSA03173')))
        self.assertEqual(env['BB_SHADNET_NPID'], 'hunter')
        self.assertEqual(Path(env['SHADPS4_HTTP_HOST_OVERRIDES_JSON']), self.data / 'user' / 'host_overrides.json')
        self.assertNotIn('BBSC_EMULATOR', env)
        settings['online'] = False
        self.assertNotIn('BB_ONLINE', self.launcher.game_environment(settings, log=lines.append))

    def test_chosen_user_folder(self):
        settings = self.launcher.load_settings()
        settings.update(user_dir=str(Path(self.temp.name) / 'mine'), online=True, supermedo_npid='h',
                        supermedo_pass='p')
        env = self.launcher.game_environment(settings, log=lambda _l: None)
        self.assertEqual(env['BB_USER_DIR'], settings['user_dir'])
        self.assertEqual(Path(env['SHADPS4_HTTP_HOST_OVERRIDES_JSON']).parent, Path(settings['user_dir']))

    def test_play_needs_an_account_when_online(self):
        settings = self.launcher.load_settings()
        game = Path(self.temp.name) / 'game'
        game.mkdir()
        (game / 'eboot.bin').write_bytes(b'')
        settings.update(game_dir=str(game), online=True)
        self.launcher.save_settings(settings)
        with mock.patch('os.execvpe') as execvpe, mock.patch('sys.stderr', io.StringIO()) as err:
            self.assertEqual(self.launcher.play(), 1)
        execvpe.assert_not_called()
        self.assertIn('SuperMedo account', err.getvalue())

    def test_run_log(self):
        """The game's output goes to last_run.log too; the run before is kept as last_run.1.log."""
        window = types.SimpleNamespace(settings=self.launcher.load_settings(), run_log=None)
        for name in ('open_run_log', 'close_run_log'):
            setattr(window, name, types.MethodType(getattr(self.launcher.LauncherWindow, name), window))
        user = self.data / 'user'
        for run in ('first run\n', 'second run\n'):
            window.open_run_log()
            window.run_log.write(run)
            window.close_run_log()
        self.assertEqual((user / 'last_run.log').read_text(encoding='utf-8'), 'second run\n')
        self.assertEqual((user / 'last_run.1.log').read_text(encoding='utf-8'), 'first run\n')


if __name__ == '__main__':
    unittest.main()
