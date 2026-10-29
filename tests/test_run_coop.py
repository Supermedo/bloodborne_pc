"""Seamless co-op (BBSC) with this port: its own saves, and BBSC's left-over game server redirect."""
from paths import ROOT
import contextlib
import importlib.util
import io
import json
import sys
import tempfile
import unittest
import unittest.mock
from pathlib import Path

spec = importlib.util.spec_from_file_location('bbrun', ROOT / 'run.py')
run = importlib.util.module_from_spec(spec)
spec.loader.exec_module(run)
sys.path.insert(0, str(ROOT / 'launcher'))
import bbport_launcher_win as launcher  # noqa: E402

# As BBSC 0.0.2's tools/play-bbport.ps1 writes it.
BBSC_CUSTOM = {'https://ss4.scej-network.jp:20443': 'http://199.195.193.131:20443',
               'http://thehuntersdream.com:18671/summon_messenger': 'http://199.195.193.131:31315',
               'https://thehuntersdream.com:18671/summon_messenger': 'http://199.195.193.131:31315'}
BBSC = r'C:\BBSC\bbsc\bbsc-launch.exe'


class CoopTests(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.user = Path(temp.name) / 'user'
        self.user.mkdir()
        self.game = Path(temp.name) / 'game'
        self.game.mkdir()

    def overrides(self, env):
        with contextlib.redirect_stdout(io.StringIO()):
            run.online_overrides(env, self.user)
        return json.loads(Path(env['SHADPS4_HTTP_HOST_OVERRIDES_JSON']).read_text(encoding='utf-8'))

    def test_bbsc_left_over_redirect_is_not_used_without_bbsc(self):
        (self.user / 'host_overrides.custom.json').write_text(json.dumps(BBSC_CUSTOM), encoding='utf-8')
        normal = self.overrides({'BB_COMMUNITY_SERVER': 'https://example.org'})
        self.assertEqual(normal[run.GAME_SERVER], 'https://example.org')
        self.assertEqual(self.overrides({'BB_PROBE': BBSC}), BBSC_CUSTOM)

    def test_a_players_own_custom_redirect_is_still_used(self):
        mine = {run.GAME_SERVER: 'http://192.168.1.5:20443'}
        (self.user / 'host_overrides.custom.json').write_text(json.dumps(mine), encoding='utf-8')
        self.assertEqual(self.overrides({}), mine)

    def test_bbsc_plays_its_own_saves(self):
        (self.user / 'addcont' / 'CUSA03173' / 'SPEXPANSIONDLC03').mkdir(parents=True)
        env = {'BB_PROBE': BBSC}
        self.assertTrue(run.seamless_coop(env))
        self.assertFalse(run.seamless_coop({'BB_PROBE': r'C:\port\bin\bb-probe.exe'}))
        with contextlib.redirect_stdout(io.StringIO()):
            coop = run.coop_user_dir(env, self.user, self.game)
        self.assertEqual(coop, self.user / 'seamless-coop')
        self.assertEqual(env['BBSC_SAVE_DIR'], str(coop / 'savedata' / '1' / 'CUSA00207' / 'SPRJ0005'))
        self.assertEqual(env['BB_DLC_DIR'], str(self.user / 'addcont' / 'CUSA03173'))


class ItsScriptTests(unittest.TestCase):
    """A launcher opened by BBSC's script keeps BBSC for a start on BBSC's server only."""
    def start(self, **settings):
        script = {'BB_PROBE': BBSC, 'BBSC_EMULATOR': 'x', 'BB_ONLINE': '1',
                  'BB_SHADNET_SERVER': 'play.seamlesshunt.com:31313', 'BB_USER_NAME': 'hunter'}
        with unittest.mock.patch.dict('os.environ', script):
            s = {**launcher.APP_DEFAULTS, 'game_dir': 'C:/game', **settings}
            return launcher.game_environment(s, graphics={})

    def test_kept_for_its_server_only(self):
        on_it = {'online': True, 'online_pool': 'shadnet', 'online_server': 'play.seamlesshunt.com:31313'}
        self.assertEqual(self.start(**on_it)['BB_PROBE'], BBSC)
        for other in ({**on_it, 'online': False}, {**on_it, 'online_pool': 'huntersdream'},
                      {**on_it, 'online_server': 'rpcn.example.org:31313'}):
            env = self.start(**other)
            self.assertNotIn('BB_PROBE', env)
            self.assertNotIn('BBSC_EMULATOR', env)


if __name__ == '__main__':
    unittest.main()
