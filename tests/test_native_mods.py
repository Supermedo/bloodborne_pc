from paths import ROOT
import hashlib
import importlib
import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(ROOT / 'scripts'))
native_mods = importlib.import_module('native_mods')
mods = importlib.import_module('mods')


class NativeModTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.mods = self.root / 'mods'
        self.out = self.root / 'out'
        self.folder = self.mods / "Hunter's Atlas"
        self.folder.mkdir(parents=True)
        self.module = self.folder / 'native' / 'hunters_atlas.dll'
        self.module.parent.mkdir()
        self.module.write_bytes(b'fixture dll')
        self.asset = self.folder / 'assets' / 'maps'
        self.asset.mkdir(parents=True)
        self.game_hash = hashlib.sha256(b'game').hexdigest()
        self.manifest = {
            'schema': 1, 'id': 'hunters-atlas', 'name': "Hunter's Atlas",
            'native_api': 1, 'module': 'native/hunters_atlas.dll',
            'game_executables': [self.game_hash],
            'environment': {'BB_ATLAS_ENABLED': '1',
                            'BB_MAP_DIR': {'path': 'assets/maps'}},
        }
        self.write_manifest()

    def write_manifest(self):
        (self.folder / mods.NATIVE_MANIFEST).write_text(
            json.dumps(self.manifest), encoding='utf-8')

    def test_native_only_package_is_a_mod(self):
        self.assertEqual(mods.discover(self.mods), ["Hunter's Atlas"])
        self.assertEqual(list(mods.mod_files(self.folder)), [])

    def test_prepare_writes_absolute_module_and_environment(self):
        environment = {}
        path = native_mods.prepare(self.mods, None, self.game_hash,
                                   self.out, environment)
        launch = json.loads(path.read_text(encoding='utf-8'))
        self.assertEqual(launch['mods'][0]['id'], 'hunters-atlas')
        self.assertTrue(Path(launch['mods'][0]['module']).is_absolute())
        self.assertEqual(environment['BB_ATLAS_ENABLED'], '1')
        self.assertEqual(Path(environment['BB_MAP_DIR']), self.asset.resolve())

    def test_disabled_mod_is_not_loaded_and_stale_environment_is_cleared(self):
        config = self.root / 'mods.json'
        config.write_text(json.dumps({'order': ["Hunter's Atlas"],
                                      'disabled': ["Hunter's Atlas"]}), encoding='utf-8')
        environment = {'BB_ATLAS_ENABLED': '1', 'BB_NATIVE_MODS_FILE': 'old'}
        self.assertIsNone(native_mods.prepare(self.mods, config, self.game_hash,
                                              self.out, environment))
        self.assertNotIn('BB_ATLAS_ENABLED', environment)
        self.assertNotIn('BB_NATIVE_MODS_FILE', environment)

    def test_wrong_game_is_rejected(self):
        environment = {}
        self.assertIsNone(native_mods.prepare(self.mods, None, '0' * 64,
                                              self.out, environment))
        self.assertNotIn('BB_ATLAS_ENABLED', environment)

    def test_path_escape_is_rejected_during_discovery(self):
        self.manifest['module'] = '../outside.dll'
        self.write_manifest()
        self.assertEqual(mods.discover(self.mods), [])
        self.assertIn('escapes', mods.unrecognized(self.mods)[0][1])

    def test_integrity_failure_is_rejected(self):
        self.manifest['integrity'] = {'native/hunters_atlas.dll': '0' * 64}
        self.write_manifest()
        self.assertIsNone(native_mods.prepare(self.mods, None, self.game_hash,
                                              self.out, {}))


if __name__ == '__main__':
    unittest.main()
