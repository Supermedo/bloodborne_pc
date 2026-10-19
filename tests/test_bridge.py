from paths import ROOT
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import unittest


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, ROOT / path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


run = load('bb_run', 'run.py')
sys.path.insert(0, str(ROOT / 'launcher'))
launcher = load('bb_launcher', 'launcher/bbport_launcher_win.py')


class HuntersDreamTests(unittest.TestCase):
    """The Hunter's Dream pool: the engine's own backend (BB_NET_BACKEND=thd)."""

    def test_run_sets_the_backend_and_http_overrides(self):
        with tempfile.TemporaryDirectory() as temp:
            env = {'BB_THD_TOKEN': 't', 'BB_THD_ONLINE_ID': 'Hunter1', 'BB_SHADNET_SERVER': 'x:1'}
            run.hunters_dream(env, Path(temp))
            overrides = json.loads((Path(temp) / 'host_overrides.json').read_text(encoding='utf-8'))
        self.assertEqual(overrides['*.scej-network.jp'], 'https://thehuntersdream.com')
        self.assertEqual(overrides['bb-playlog-prod.s3.amazonaws.com'], 'https://thehuntersdream.com:18671')
        self.assertEqual(env['BB_THD_SERVER'], 'https://thehuntersdream.com:18671')
        self.assertNotIn('BB_SHADNET_SERVER', env)

    def test_shadnet_pool_sends_game_functions_over_http_only(self):
        with tempfile.TemporaryDirectory() as temp:
            env = {'BB_SHADNET_WEBAPI': 'http://srv.shadps4.net:31315'}
            run.online_overrides(env, Path(temp))
            overrides = json.loads((Path(temp) / 'host_overrides.json').read_text(encoding='utf-8'))
        self.assertFalse(any('31315' in v for v in overrides.values()))

    def test_launcher_environment(self):
        launcher.HUNTERS_DREAM_POOL = True
        settings = {**launcher.APP_DEFAULTS, 'game_dir': 'x', 'online': True, 'online_pool': 'huntersdream',
                    'thd_online_id': 'Hunter1', 'thd_token': 'tok'}
        env = launcher.game_environment(settings)
        self.assertEqual((env['BB_NET_BACKEND'], env['BB_THD_ONLINE_ID'], env['BB_THD_TOKEN']),
                         ('thd', 'Hunter1', 'tok'))
        self.assertTrue(env['BB_CLIENT_VERSION'].startswith('bbport/'))
        launcher.HUNTERS_DREAM_POOL = False
        env = launcher.game_environment(settings)
        self.assertNotIn('BB_NET_BACKEND', env)


if __name__ == '__main__':
    unittest.main()
