"""Folders of another installation (bbport_help.other_installations): a tester's 1.9.5 beta beside 1.9.4 took the old
installation's saves and mods folder from the shared launcher settings, and its shader cache with them."""
from paths import ROOT
import importlib.util
import sys
import tempfile
import types
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(ROOT / 'launcher'))
sys.path.insert(0, str(ROOT / 'scripts'))
spec = importlib.util.spec_from_file_location('bbport_help_folders', ROOT / 'launcher/bbport_help.py')
help_ = importlib.util.module_from_spec(spec)
spec.loader.exec_module(help_)


class OwnFolderTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        root = Path(self.tmp.name)
        self.old, self.beta = root / 'bbport', root / 'bbport-beta'
        for port in (self.old, self.beta):
            (port / 'bin').mkdir(parents=True)
            (port / 'run.py').write_text('run')
            (port / 'Bloodborne.exe').write_bytes(b'MZ')
        (self.old / 'user').mkdir()
        (self.old / 'Bloodborne' / 'Mods').mkdir(parents=True)
        self.settings = {'user_dir': str(self.old / 'user'), 'mods_dir': str(self.old / 'Bloodborne' / 'Mods'),
                         'cache_dir': ''}

    def test_the_folders_of_the_old_installation(self):
        found = help_.other_installations(self.settings, self.beta, self.beta)
        self.assertEqual([(key, folder) for key, _name, folder in found],
                         [('user_dir', self.old.resolve()), ('mods_dir', self.old.resolve())])
        self.assertEqual(found[0][1], ('Saves folder', 'Папка сохранений'))

    def test_own_or_unrelated_folders_are_fine(self):
        own = {'user_dir': str(self.beta / 'user'), 'mods_dir': str(Path(self.tmp.name) / 'my mods'), 'cache_dir': ''}
        self.assertEqual(help_.other_installations(own, self.beta, self.beta), [])
        # From the old installation, its own folders are its own.
        self.assertEqual(help_.other_installations(self.settings, self.old, self.old), [])

    def test_the_button_sets_this_installations_own(self):
        values = dict(self.settings, cache_dir=str(Path(self.tmp.name) / 'cache'))
        calls = []

        class Window(help_.HelpMixin):
            def var(self, key, store):
                return types.SimpleNamespace(get=lambda: values[key], set=lambda value: values.__setitem__(key, value))

            def collect(self):
                calls.append('collect')

            def refresh_status(self):
                calls.append('status')

            def help_check(self):
                calls.append('check')
        launcher = types.SimpleNamespace(PORT_DIR=self.beta, DATA_DIR=self.beta)
        with mock.patch.object(help_, 'L', launcher):
            Window().use_own_folders()
        # The saves folder stays: the settings are shared, so setting it back took the characters and achievements
        # from the beta and from the old version too.
        self.assertEqual(values['user_dir'], str(self.old / 'user'))
        self.assertEqual(values['mods_dir'], '')
        self.assertTrue(values['cache_dir'])  # not in another installation: kept
        self.assertEqual(calls, ['collect', 'status', 'check'])

    def test_the_check_names_the_mods_and_cache_folders_only(self):
        self.assertEqual(help_.SEPARATE_FOLDERS, ('mods_dir', 'cache_dir'))
        settings = dict(self.settings, cache_dir=str(self.old / 'user'))
        checked = {key: settings[key] for key in help_.SEPARATE_FOLDERS}
        self.assertEqual([key for key, _name, _folder in help_.other_installations(checked, self.beta, self.beta)],
                         ['mods_dir', 'cache_dir'])
        steps = ' '.join(help_.TOPICS['other_install'][2])
        self.assertNotIn('Bring your characters over', steps)
        self.assertIn('saves folder stays', steps)

    def test_the_topic_has_its_button(self):
        self.assertIn('other_install', help_.TOPIC_ORDER)
        self.assertEqual(help_.TOPIC_ACTIONS['other_install'][0][1], 'use_own_folders')


if __name__ == '__main__':
    unittest.main()
