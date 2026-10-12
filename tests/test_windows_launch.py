"""Windows launch defaults and restart preparation, without game files or a GPU."""
from paths import ROOT
import importlib.util
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('run_win', ROOT / 'run.py')
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)


class WindowsLaunchTests(unittest.TestCase):
    def test_upstream_defaults_and_windows_memory_policy(self):
        env = {'BB_PC_MODEL': '1', 'BB_GUEST_IN_PLACE': '1', 'BB_UFFD': '1',
               'BB_PREUPLOAD': '0', 'BB_AS_0_3': '1'}
        runner.configure_runtime(env, 'uncap', ROOT)
        self.assertEqual(env['BB_GUEST_IN_PLACE'], '0')
        self.assertEqual(env['BB_PC_MODEL'], '0')
        self.assertEqual(env['BB_GUEST_GPU_MEMORY'], '0')
        self.assertEqual(env['BB_UFFD'], '0')
        self.assertEqual(env['BB_PREUPLOAD'], '0')
        self.assertEqual(env['BB_COPY_GPU_BUFFERS'], '1')
        self.assertEqual(env['BB_GPU_WRITE_TWINS_MAX'], '65536')
        self.assertEqual(env['BB_VBLANK_HZ'], '480')
        self.assertEqual(env['BB_HOST_COPY_WAITS'], 'all')
        self.assertEqual(env['BB_PRODUCER_CHECK'], '1')

    def test_frame_pacing_and_explicit_override(self):
        for fps, expected in [('90', '90'), ('60', '60'), ('30', '60')]:
            env = {}
            runner.configure_runtime(env, fps, ROOT)
            self.assertEqual(env['BB_VBLANK_HZ'], expected)
        env = {'BB_VBLANK_HZ': '120'}
        runner.configure_runtime(env, 'uncap', ROOT)
        self.assertEqual(env['BB_VBLANK_HZ'], '120')

    def test_saved_log_contains_child_output_and_propagates_exit(self):
        with tempfile.TemporaryDirectory() as directory:
            env = {'BB_SAVE_LOG': '1'}
            logfile = runner.configure_runtime(env, 'uncap', directory)
            self.assertEqual(env['BB_FRAME_STATS'], '1')
            self.assertTrue(Path(env['BB_FRAME_LOG']).parent.is_dir())
            command = [sys.executable, '-c', 'import sys; print("frame"); print("driver", file=sys.stderr); sys.exit(7)']
            with patch.object(runner, 'no_console', return_value=0):
                self.assertEqual(runner.launch_probe(command, logfile), 7)
            self.assertIn('frame', logfile.read_text(encoding='utf-8'))
            self.assertIn('driver', logfile.read_text(encoding='utf-8'))

    def test_main_prepares_scaled_output_and_restart_without_linux_shell(self):
        with tempfile.TemporaryDirectory(prefix='bbport windows ') as directory:
            data = Path(directory)
            game = data / 'game'
            game.mkdir()
            (game / 'eboot.bin').touch()
            probe = data / 'bb-probe.exe'
            probe.touch()
            config = data / 'bbport.ini'
            config.write_text('live_resolution=0\n')
            env = {'BB_GAME_DIR': str(game), 'BB_DATA_DIR': str(data), 'BB_CONFIG': str(config),
                   'BB_PROBE': str(probe)}
            calls = []

            def script(name, *args, capture=False):
                calls.append((name, args))
                if name == 'mods.py':
                    return str(game)
                if name == 'patches.py' and '--print-scaled' in args:
                    return '854x480 1280x720'
                return '' if capture else None

            def launch(command, log_path):
                self.assertEqual(os.environ['BB_RENDER_RES'], '854x480')
                self.assertEqual(os.environ['BB_OUTPUT_RES'], '1280x720')
                self.assertEqual(os.environ['BB_DMEM_MB'], '9152')
                self.assertEqual(os.environ['BB_COPY_GPU_BUFFERS'], '1')
                restart = os.environ['BB_RESTART_COMMAND']
                self.assertIn('--after', restart)
                self.assertIn('bbport windows ', restart)
                self.assertIn('--strict-imports', command)
                return 7

            previous = Path.cwd()
            try:
                with patch.dict(os.environ, env, clear=True), patch.object(runner, 'PORT', data), \
                     patch.object(runner, '__file__', str(data / 'run.py')), \
                     patch.object(runner.sys, 'argv', [str(ROOT / 'run.py'), '--strict-imports']), \
                     patch.object(runner, 'run_script', side_effect=script), \
                     patch.object(runner, 'launch_probe', side_effect=launch):
                    self.assertEqual(runner.main(), 7)
            finally:
                os.chdir(previous)
            self.assertEqual([name for name, _args in calls[:5]],
                             ['mods.py', 'prepare.py', 'link_libc.py', 'link_modules.py', 'content_profile.py'])


if __name__ == '__main__':
    unittest.main()
