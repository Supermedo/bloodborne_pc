# SPDX-License-Identifier: GPL-2.0-or-later
"""External-launcher setup uses real filesystem transactions, isolated runner subprocesses and no GUI."""
from paths import ROOT
from contextlib import redirect_stdout, redirect_stderr
import importlib.util
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock

sys.path.insert(0, str(ROOT / 'launcher'))
import bbport_recomplauncher as integration


class IntegrationTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.game = self.root / 'game dump'
        self.game.mkdir()
        (self.game / 'eboot.bin').write_bytes(b'original source')
        self.data = self.root / 'installation'
        self.data.mkdir()
        self.saved = {'game_dir': 'another game', 'fullscreen': True, 'user_dir': 'custom saves',
                      'controller': 'automatic', 'custom_preference': {'keep': True}}
        self.launcher = SimpleNamespace(
            DATA_DIR=self.data, PORT_DIR=self.data, NO_WINDOW=0,
            APP_DEFAULTS={'game_dir': '', 'user_dir': ''}, INI_DEFAULTS={'upscaler': 'fsr3'},
            load_settings=mock.Mock(side_effect=lambda: dict(self.saved)),
            settled_online=mock.Mock(return_value={}),
            game_environment=mock.Mock(return_value={}),
            run_command=mock.Mock(return_value=['runner']),
            ini_path=mock.Mock(return_value=self.data / 'bbport.ini'),
            game_underway=mock.Mock(return_value=False),
            updater=SimpleNamespace(underway=mock.Mock(return_value=False)),
            start_problem=mock.Mock(return_value=None), play_without_window=mock.Mock(return_value=0))
        self.cache = integration.cache_folder(self.launcher)
        self.settings = dict(self.saved, game_dir=str(self.game))

    def process(self, command, **kwargs):
        self.assertEqual(command, ['runner', '--prepare-only'])
        self.assertEqual(kwargs['env']['BB_READ_ONLY_GAME'], '1')
        self.assertEqual(kwargs['env']['BB_GAME_DIR'], str(self.game))
        staged = Path(kwargs['env']['BB_PREPARED_OUT'])
        staged.mkdir(parents=True)
        for name in integration.REQUIRED:
            (staged / name).write_bytes(b'prepared')
        return SimpleNamespace(stdout=io.BytesIO(b'progress 0.500 Linking\n'), wait=lambda: 0)

    def prepare(self, process=None):
        with mock.patch.object(integration.subprocess, 'Popen', side_effect=process or self.process), \
                redirect_stdout(io.StringIO()), redirect_stderr(io.StringIO()):
            integration.prepare(self.launcher, self.settings, self.game, 'known hash')

    def main(self, mode='--prepare-content'):
        with mock.patch.object(integration, 'validate_game', return_value=(self.game, 'known hash')), \
                redirect_stdout(io.StringIO()), redirect_stderr(io.StringIO()):
            return integration.main([mode, '--game-dir', str(self.game)], self.launcher)

    def test_prepares_a_separate_cache_without_changing_dump_or_preferences(self):
        with mock.patch.object(integration.subprocess, 'Popen', side_effect=self.process):
            self.assertEqual(self.main(), 0)
        self.assertEqual((self.game / 'eboot.bin').read_bytes(), b'original source')
        self.assertEqual(self.saved['game_dir'], 'another game')
        self.assertTrue(integration.owned(self.cache))
        self.assertFalse(self.cache.with_name('recomplauncher.preparing').exists())
        self.launcher.play_without_window.assert_not_called()

    def test_replaces_only_a_completed_owned_preparation(self):
        self.prepare()
        (self.cache / 'old.bin').write_bytes(b'old')
        self.prepare()
        self.assertFalse((self.cache / 'old.bin').exists())

    def test_failed_subprocess_preserves_previous_cache(self):
        self.prepare()
        (self.cache / 'keep.bin').write_bytes(b'previous')
        def failed(*args, **kwargs):
            process = self.process(*args, **kwargs)
            process.wait = lambda: 7
            return process
        with self.assertRaisesRegex(ValueError, 'exit 7'):
            self.prepare(failed)
        self.assertEqual((self.cache / 'keep.bin').read_bytes(), b'previous')
        self.assertFalse(self.cache.with_name('recomplauncher.preparing').exists())

    def test_missing_output_is_not_published(self):
        def incomplete(*args, **kwargs):
            process = self.process(*args, **kwargs)
            (Path(kwargs['env']['BB_PREPARED_OUT']) / 'content.bin').unlink()
            return process
        with self.assertRaisesRegex(ValueError, 'content.bin'):
            self.prepare(incomplete)
        self.assertFalse(self.cache.exists())

    def test_unowned_and_interrupted_directories_are_preserved(self):
        self.cache.mkdir(parents=True)
        (self.cache / 'keep').write_text('mine')
        with self.assertRaisesRegex(ValueError, 'not managed'):
            self.prepare()
        self.assertEqual((self.cache / 'keep').read_text(), 'mine')
        self.cache.rename(self.cache.with_name('recomplauncher.preparing'))
        with self.assertRaisesRegex(ValueError, 'interrupted'):
            self.prepare()
        self.assertEqual((self.cache.with_name('recomplauncher.preparing') / 'keep').read_text(), 'mine')

    def test_publish_failure_rolls_back_previous_cache(self):
        self.prepare()
        (self.cache / 'keep').write_text('previous')
        rename = Path.rename
        def fail_new(path, target):
            if path.name == 'new':
                raise OSError('injected publish failure')
            return rename(path, target)
        with mock.patch.object(Path, 'rename', fail_new), self.assertRaises(OSError):
            self.prepare()
        self.assertEqual((self.cache / 'keep').read_text(), 'previous')

    def test_busy_game_and_update_return_errors_without_opening_gui(self):
        self.launcher.game_underway.return_value = True
        self.assertEqual(self.main('--recomplauncher-play'), 1)
        self.launcher.game_underway.return_value = False
        self.launcher.updater.underway.return_value = True
        self.assertEqual(self.main(), 1)
        self.launcher.play_without_window.assert_not_called()

    def test_direct_launch_changes_only_the_in_memory_game_path(self):
        self.prepare()
        old = dict(os.environ)
        def play(settings):
            self.assertEqual(settings, {**self.saved, 'game_dir': str(self.game)})
            self.assertEqual(os.environ['BB_READ_ONLY_GAME'], '1')
            self.assertEqual(os.environ['BB_PREPARED_OUT'], str(self.cache))
            return 17
        self.launcher.play_without_window.side_effect = play
        self.assertEqual(self.main('--recomplauncher-play'), 17)
        self.assertEqual(dict(os.environ), old)
        self.assertEqual(self.saved['game_dir'], 'another game')

    def test_unavailable_graphics_return_error_instead_of_gui_fallback(self):
        self.prepare()
        self.launcher.play_without_window.return_value = None
        self.assertEqual(self.main('--recomplauncher-play'), 1)

    def test_direct_launch_requires_a_safe_prepared_cache(self):
        self.assertEqual(self.main('--recomplauncher-play'), 1)
        self.cache.mkdir(parents=True)
        (self.cache / 'keep').write_text('unmanaged')
        self.assertEqual(self.main('--recomplauncher-play'), 1)
        self.assertEqual((self.cache / 'keep').read_text(), 'unmanaged')
        self.launcher.DATA_DIR = self.game
        self.assertEqual(self.main('--recomplauncher-play'), 1)
        self.launcher.play_without_window.assert_not_called()

    def test_invalid_cli_is_an_error(self):
        with redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as result:
            integration.main(['--prepare-content'], self.launcher)
        self.assertEqual(result.exception.code, 2)

    def test_overlap_and_case_aliases_are_rejected(self):
        self.launcher.DATA_DIR = self.game
        with self.assertRaisesRegex(ValueError, 'separate folders'):
            self.prepare()
        alias = self.game.with_name('GAME DUMP')
        if alias.exists():
            self.assertTrue(integration.overlaps(self.game, alias / 'child'))

    def test_non_ascii_paths_are_preserved(self):
        self.game.rename(self.root / 'Ελληνικά 日本語')
        self.game = self.root / 'Ελληνικά 日本語'
        self.settings['game_dir'] = str(self.game)
        self.prepare()
        marker = json.loads((self.cache / integration.OWNER).read_text())
        self.assertEqual(marker['game'], str(self.game))

    def test_known_title_version_and_loaded_image_are_checked(self):
        (self.game / 'sce_module').mkdir()
        (self.game / 'sce_module/libc.prx').write_bytes(b'libc')
        (self.game / 'sce_sys').mkdir()
        (self.game / 'sce_sys/param.sfo').write_bytes(b'sfo')
        (self.game / 'dvdroot_ps4').mkdir()
        from clean_eboot import CLEAN_IMAGE
        with mock.patch('prepare.sfo', return_value={'TITLE_ID': 'CUSA03173', 'APP_VER': '01.09'}), \
                mock.patch('clean_eboot.image_hash', return_value=CLEAN_IMAGE), \
                mock.patch('clean_eboot.restore') as restore:
            self.assertEqual(integration.validate_game(self.game), (self.game.resolve(), CLEAN_IMAGE))
            with mock.patch('clean_eboot.image_hash', return_value='modified'):
                with self.assertRaisesRegex(ValueError, 'clean 1.09'):
                    integration.validate_game(self.game)
            restore.assert_not_called()
        with mock.patch('prepare.sfo', return_value={'TITLE_ID': 'CUSA99999', 'APP_VER': '01.09'}):
            with self.assertRaisesRegex(ValueError, 'CUSA03173'):
                integration.validate_game(self.game)
        with mock.patch('prepare.sfo', return_value={'TITLE_ID': 'CUSA03173', 'APP_VER': '01.00'}):
            with self.assertRaisesRegex(ValueError, '1.09 update'):
                integration.validate_game(self.game)


class RunnerTests(unittest.TestCase):
    def test_prepare_only_never_repairs_the_dump_queries_gpu_or_starts_game(self):
        spec = importlib.util.spec_from_file_location('bbport_external_run', ROOT / 'run.py')
        runner = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(runner)
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            game, out = root / 'game', root / 'staged'
            (game / 'sce_module').mkdir(parents=True)
            (game / 'sce_sys').mkdir()
            (game / 'sce_sys/param.sfo').write_bytes(b'fixture')
            (game / 'eboot.bin').write_bytes(b'original')
            probe = root / 'bb-probe.exe'
            probe.write_bytes(b'fixture')
            def script(name, *args, **kwargs):
                if name == 'mods.py':
                    return str(game)
                return '' if kwargs.get('capture') else True
            from contextlib import ExitStack
            with ExitStack() as stack:
                for patcher in (
                    mock.patch.object(sys, 'argv', ['run.py', '--prepare-only']),
                    mock.patch.dict(os.environ, {'BB_GAME_DIR': str(game), 'BB_DATA_DIR': str(root),
                                                  'BB_PREPARED_OUT': str(out), 'BB_PROBE': str(probe)}, clear=True),
                    mock.patch.object(runner, 'PORT', root), mock.patch.object(runner.os, 'chdir'),
                    mock.patch.object(runner, 'hold_game_mutex', return_value=True),
                    mock.patch.object(runner, 'files_note', return_value=None),
                    mock.patch.object(runner, 'prepare_driver_environment'),
                    mock.patch.object(runner, 'image_sha256', return_value='modified'),
                    mock.patch.object(runner, 'remove_stale_views'),
                    mock.patch.object(runner, 'run_script', side_effect=script),
                    mock.patch('patches.game_app_version', return_value='01.09'),
                    mock.patch('patches.image_groups', return_value=set()),
                    redirect_stdout(io.StringIO())):
                    stack.enter_context(patcher)
                repair = stack.enter_context(mock.patch('clean_eboot.restore'))
                gpu = stack.enter_context(mock.patch.object(runner, 'low_vram_mb'))
                launch = stack.enter_context(mock.patch.object(runner.subprocess, 'run'))
                users = stack.enter_context(mock.patch.object(runner, 'user_folders'))
                self.assertEqual(runner.main(), 0)
                repair.assert_not_called()
                gpu.assert_not_called()
                launch.assert_not_called()
                users.assert_not_called()
            self.assertEqual((game / 'eboot.bin').read_bytes(), b'original')


if __name__ == '__main__':
    unittest.main()
