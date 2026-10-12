"""Windows launch defaults and restart preparation, without game files or a GPU."""
from paths import ROOT
import importlib.util
import io
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
    def test_remix_launch_bypasses_game_reconstruction_and_resolution_patches(self):
        with tempfile.TemporaryDirectory() as directory:
            data=Path(directory); game=data/'game'; game.mkdir()
            (game/'eboot.bin').touch(); probe=data/'bb-probe.exe'; probe.touch()
            config=data/'bbport.ini'; original='upscaler=dlss\npreset=3\nframe_generation=dlss\noutput_res=2560x1440\n'
            config.write_text(original)
            env=dict(BB_RTX_REMIX='1',BB_GAME_DIR=str(game),BB_DATA_DIR=str(data),
                     BB_CONFIG=str(config),BB_PROBE=str(probe),BB_HDR='1',BB_RENDER_RES='1280x720')
            calls=[]
            def script(name,*args,capture=False):
                calls.append((name,args)); return str(game) if name=='mods.py' else ''
            def launch(*args):
                self.assertEqual(os.environ['BB_HDR'],'0')
                self.assertEqual(os.environ['BB_UPSCALER'],'none')
                self.assertEqual(os.environ['BB_FRAME_GENERATION'],'off')
                self.assertNotIn('BB_RENDER_RES',os.environ)
                self.assertNotIn('BB_OUTPUT_RES',os.environ)
                self.assertEqual(config.read_text(),original)
                return 0
            previous=Path.cwd()
            try:
                with patch.dict(os.environ,env,clear=True),patch.object(runner,'PORT',data), \
                     patch.object(runner.sys,'argv',[str(ROOT/'run.py')]), \
                     patch.object(runner,'run_script',side_effect=script), \
                     patch('prepare_cache.prepare_game',return_value=True), \
                     patch.object(runner,'launch_probe',side_effect=launch):
                    self.assertEqual(runner.main(),0)
            finally: os.chdir(previous)
            self.assertFalse(any('--print-scaled' in args for name,args in calls if name=='patches.py'))

    def test_script_capture_decodes_utf8_paths(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'scripts').mkdir()
            (root / 'scripts/fixture.py').write_text(
                'import sys\nsys.stdout.buffer.write("путь к игре".encode("utf-8"))\n', encoding='utf-8')
            with patch.object(runner, 'PORT', root), patch.object(runner, 'no_console', return_value=0):
                self.assertEqual(runner.run_script('fixture.py', capture=True), 'путь к игре')

    def test_native_dword_status_survives_system_exit(self):
        for code in (0, 7, 23, 0xc0000005, 0xc0000409, 0xc0000374, 0xffffffff, -1073741819):
            status = code & 0xffffffff
            expected = status if status < 0x80000000 else status - 0x100000000
            output, log = io.StringIO(), io.StringIO()
            with patch.object(runner.sys, 'stdout', output):
                self.assertEqual(runner.report_native_exit(code, log), expected)
            self.assertIn(f'0x{status:08x}', log.getvalue())
            self.assertEqual(output.getvalue(), log.getvalue())

    def test_monitor_only_wraps_opted_in_launches(self):
        with patch.object(runner, 'no_console', return_value=0), \
             patch.object(runner.subprocess, 'run', return_value=subprocess.CompletedProcess([], 0xc0000409)) as launch, \
             patch.object(runner, 'find_executable', return_value=Path('bin/bb-crash-monitor.exe')) as find:
            for monitor in ('0', '1'):
                with patch.dict(os.environ, {'BB_CRASH_MONITOR': monitor}):
                    self.assertEqual(runner.launch_probe(['probe.exe', 'image.bin']), -1073740791)
                self.assertEqual(launch.call_args.args[0],
                    [str(Path('bin/bb-crash-monitor.exe')), 'probe.exe', 'image.bin'] if monitor == '1'
                    else ['probe.exe', 'image.bin'])
            find.assert_called_once_with('bb-crash-monitor.exe')

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
            self.assertEqual([name for name, _args in calls[:4]],
                             ['mods.py', 'prepare.py', 'link_modules.py', 'content_profile.py'])
            self.assertIn('--no-resource-inventory', calls[1][1])

    def test_cached_image_still_rebuilds_content_profile_and_settings_patches(self):
        with tempfile.TemporaryDirectory(prefix='bb cached launch ') as directory:
            data = Path(directory)
            game, probe = data / 'game', data / 'bb-probe.exe'
            game.mkdir()
            (game / 'eboot.bin').touch()
            probe.touch()
            calls = []

            def script(name, *args, capture=False):
                calls.append(name)
                return str(game) if name == 'mods.py' else None

            previous = Path.cwd()
            try:
                with patch.dict(os.environ, dict(BB_GAME_DIR=str(game), BB_DATA_DIR=str(data),
                        BB_PROBE=str(probe), BB_RENDER_RES='1280x720'), clear=True), \
                     patch.object(runner, 'PORT', data), \
                     patch.object(runner.sys, 'argv', [str(ROOT / 'run.py')]), \
                     patch.object(runner, 'run_script', side_effect=script), \
                     patch('prepare_cache.prepare_game', return_value=True) as prepare, \
                     patch.object(runner, 'launch_probe', return_value=0):
                    self.assertEqual(runner.main(), 0)
                    prepare.assert_called_once()
            finally:
                os.chdir(previous)
            self.assertEqual(calls, ['mods.py', 'content_profile.py', 'patches.py'])


if __name__ == '__main__':
    unittest.main()
