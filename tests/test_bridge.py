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


class BridgeTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.user = Path(self.temp.name)

    def test_config_points_at_the_account_file_in_the_saves(self):
        folder, config = run.bridge_files(self.user)
        self.assertEqual(folder, self.user / 'bbbridge')
        data = json.loads(config.read_text(encoding='utf-8'))
        self.assertEqual(data['state_file'], str(folder / 'bbbridge_state.json'))
        # The launcher writes the same file.
        self.assertEqual(launcher.bridge_config(str(self.user))[1], config)

    def test_accounts_from_the_state_file(self):
        self.assertEqual(run.bridge_accounts(self.user), [])
        folder = self.user / 'bbbridge'
        folder.mkdir()
        (folder / 'bbbridge_state.json').write_text(json.dumps(
            {'accounts': {'Hunter1': {'token': 't'}}}), encoding='utf-8')
        self.assertEqual(run.bridge_accounts(self.user), ['Hunter1'])
        self.assertEqual(launcher.bridge_accounts(str(self.user)), ['Hunter1'])

    def test_launcher_environment_for_the_hunters_dream(self):
        settings = {**launcher.APP_DEFAULTS, 'game_dir': 'x', 'online': True, 'online_npid': 'shadnet_name',
                    'online_password': 'secret', 'online_pool': 'huntersdream'}
        env = launcher.game_environment(settings)
        self.assertEqual((env['BB_ONLINE'], env['BB_BRIDGE'], env['BB_BRIDGE_LIVE_MAP']), ('1', '1', '1'))
        self.assertEqual((env['BB_SHADNET_NPID'], env['BB_SHADNET_PASSWORD']), ('', ''))
        env = launcher.game_environment({**settings, 'online_pool': 'shadnet'})
        self.assertNotIn('BB_BRIDGE', env)
        self.assertEqual(env['BB_SHADNET_NPID'], 'shadnet_name')

    def test_live_map_patch_exists_for_1_09(self):
        import xml.etree.ElementTree as ET
        names = {m.get('Name') for m in ET.parse(ROOT / 'patches/Bloodborne.xml').getroot().iter('Metadata')
                 if m.get('AppVer') == '01.09'}
        self.assertIn(run.LIVE_MAP_PATCH, names)


if __name__ == '__main__':
    unittest.main()
