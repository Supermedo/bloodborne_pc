"""Exercise run.sh across a re-exec with lightweight preparation/probe stand-ins."""
from paths import ROOT
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest


class RestartResolutionTests(unittest.TestCase):
    def run_restarts(self, explicit=False, live=False, ini_extra='', caps=None, bare_path=False):
        with tempfile.TemporaryDirectory() as directory:
            data = Path(directory)
            # Preparation is unrelated to this test; allow the real patch compiler to
            # validate writes against a single ELF load segment spanning the game image.
            out = data / 'out'
            out.mkdir()
            elf = bytearray(120)
            struct.pack_into('<Q', elf, 0x20, 64)
            struct.pack_into('<HH', elf, 0x36, 56, 1)
            struct.pack_into('<IIQQQQQQ', elf, 64, 1, 0, 0, 0, 0, 0, 0x6000000, 0)
            (out / 'eboot.elf').write_bytes(elf)
            (data / 'eboot.bin').touch()
            config = data / 'bbport.ini'
            config.write_text('upscaler=fsr3\npreset=1\noutput_res=1280x720\n' + ini_extra)
            if caps is not None:  # the GPU check next to the probe (live_resolution=auto)
                tool = data / 'bb-gpu-capabilities'
                tool.write_text(f'#!/bin/sh\necho {caps}\n')
                tool.chmod(0o755)
            python = data / 'python'
            python.write_text(f'#!{sys.executable}\n' +
                'import subprocess, sys\n'
                'if sys.argv[1] in ("scripts/patches.py", "scripts/mods.py"):\n'
                '    sys.exit(subprocess.call([sys.executable, *sys.argv[1:]]))\n')
            python.chmod(0o755)
            probe = data / 'probe'
            probe.write_text(f'#!{sys.executable}\n' +
                'import json, os\n'
                'from pathlib import Path\n'
                'config=Path(os.environ["BB_CONFIG"])\n'
                'stage=int(os.environ.get("BB_TEST_STAGE", "0"))\n'
                'with (config.parent/"environments").open("a") as f:\n'
                '    f.write(json.dumps({key:os.environ.get(key) for key in '
                '("BB_RENDER_RES", "BB_OUTPUT_RES", "BB_AUTO_RENDER_RES")})+"\\n")\n'
                'if stage<2:\n'
                '    config.write_text("upscaler=fsr3\\npreset=4\\noutput_res="+'
                '("1280x720" if stage==0 else "1920x1080")+"\\n")\n'
                '    os.environ["BB_TEST_STAGE"]=str(stage+1)\n'
                '    os.execlp("bash", "bash", "run.sh")\n')
            probe.chmod(0o755)
            env = dict(os.environ, BB_PREBUILT='1', BB_PROBE=str(probe), PYTHON=str(python),
                       BB_DATA_DIR=str(data), BB_CONFIG=str(config), BB_GAME_DIR=str(data))
            for key in ('BB_RENDER_RES', 'BB_OUTPUT_RES', 'BB_AUTO_RENDER_RES', 'BB_TEST_STAGE'):
                env.pop(key, None)
            env.pop('BB_LIVE_RES', None)
            if explicit:
                env['BB_RENDER_RES'] = '800x450'
            if live:
                env['BB_LIVE_RES'] = '1'
            if bare_path:  # the AppImage's PATH: coreutils and bash, no sed/grep
                tools = data / 'bin'
                tools.mkdir()
                for name in ('bash', 'dirname', 'mkdir', 'realpath'):
                    (tools / name).symlink_to(shutil.which(name))
                env['PATH'] = str(tools)
            subprocess.run([shutil.which('bash'), 'run.sh'], cwd=ROOT, env=env,
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=True, timeout=30)
            return [json.loads(line) for line in (data / 'environments').read_text().splitlines()]

    def test_outputs_other_than_1080p_patch_the_render_size_and_restarts_recompute_it(self):
        rows = self.run_restarts()
        # 720p Quality, 720p Ultra Performance after a restart, then 1080p Ultra Performance
        # (a reducing preset patches the game's own render size at 1080p too).
        self.assertEqual([row['BB_RENDER_RES'] for row in rows], ['854x480', '426x240', '640x360'])
        self.assertEqual([row['BB_OUTPUT_RES'] for row in rows], ['1280x720', '1280x720', '1920x1080'])
        self.assertEqual([row['BB_AUTO_RENDER_RES'] for row in rows], ['1', '1', '1'])

    def test_1080p_with_a_reducing_preset_patches_the_scene_size(self):
        row = self.run_restarts(ini_extra='output_res=1920x1080\npreset=4\n')[0]
        self.assertEqual(row['BB_RENDER_RES'], '640x360')
        self.assertEqual(row['BB_OUTPUT_RES'], '1920x1080')

    def test_1080p_native_aa_and_explicit_live_stay_live(self):
        self.assertIsNone(self.run_restarts(
            ini_extra='output_res=1920x1080\npreset=0\n')[0]['BB_RENDER_RES'])
        self.assertIsNone(self.run_restarts(ini_extra='output_res=1920x1080\npreset=4\n',
                                            live=True)[0]['BB_RENDER_RES'])

    def test_auto_at_1080p_with_a_reducing_preset_takes_the_patch(self):
        # The live path keeps the game at 1080p internally; auto must not pick it for a preset
        # that reduces, whatever the GPU check says.
        for caps in (1, 0):
            with self.subTest(caps=caps):
                row = self.run_restarts(ini_extra='output_res=1920x1080\npreset=4\nlive_resolution=auto\n',
                                        caps=caps)[0]
                self.assertEqual(row['BB_RENDER_RES'], '640x360')

    def test_live_resolution_keeps_guest_sizes_native(self):
        rows = self.run_restarts(live=True)
        self.assertTrue(all(row['BB_RENDER_RES'] is None for row in rows))
        self.assertTrue(all(row['BB_OUTPUT_RES'] is None for row in rows))
        self.assertTrue(all(row['BB_AUTO_RENDER_RES'] is None for row in rows))

    def test_live_resolution_setting_and_gpu_check(self):
        # Only the first launch matters here: the probe rewrites bbport.ini for the restarts.
        self.assertIsNone(self.run_restarts(ini_extra='live_resolution=1\n')[0]['BB_RENDER_RES'])
        self.assertEqual(self.run_restarts(ini_extra='live_resolution=0\n', caps=1)[0]['BB_RENDER_RES'],
                         '854x480')
        # Unset: off (the startup patch), whatever the GPU; auto asks the GPU check.
        self.assertEqual(self.run_restarts(caps=1)[0]['BB_RENDER_RES'], '854x480')
        self.assertIsNone(self.run_restarts(ini_extra='live_resolution=auto\n', caps=1)[0]['BB_RENDER_RES'])
        self.assertEqual(self.run_restarts(ini_extra='live_resolution=auto\n', caps=0)[0]['BB_RENDER_RES'],
                         '854x480')

    def test_live_resolution_without_sed_or_grep(self):
        # A missing sed ended run.sh (exit 127) before the game in the AppImage on NixOS.
        self.assertEqual(self.run_restarts(ini_extra='live_resolution=0\n', caps=1,
                                           bare_path=True)[0]['BB_RENDER_RES'], '854x480')
        self.assertIsNone(self.run_restarts(ini_extra='live_resolution=auto\n', caps=1,
                                            bare_path=True)[0]['BB_RENDER_RES'])

    def test_explicit_render_override_survives_restart(self):
        rows = self.run_restarts(explicit=True)
        self.assertEqual([row['BB_RENDER_RES'] for row in rows], ['800x450'] * 3)
        self.assertTrue(all(row['BB_AUTO_RENDER_RES'] is None for row in rows))
