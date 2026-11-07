"""The start steps Linux takes from run.py (scripts/linux_start.py) and run.sh's use of them. run.sh runs here with
stand-ins for its scripts and the game program, in a temporary folder with a fake game: never a real game folder."""
from paths import ROOT
import os
import shutil
import stat
import subprocess
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(ROOT / 'scripts'))
sys.path.insert(0, str(ROOT))
import linux_start as start
import run
from patches import OFFLINE_ONLY, EFFECTS


def clean_environ(**extra):
    env = {key: value for key, value in os.environ.items()
           if not key.startswith(('BB_', 'BBSC_', 'SHADPS4_'))}
    env.update(extra)
    return env


class StepCase(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        self.out = self.root / 'out'
        self.out.mkdir()
        self.config = self.root / 'bbport.ini'
        self.config.write_text('', encoding='utf-8')
        self.results = start.Results()
        environ = mock.patch.dict(os.environ, clean_environ(), clear=True)
        environ.start()
        self.addCleanup(environ.stop)


class LayerTests(StepCase):
    def layers(self, fail=(), groups=('fps', 'resolution', 'effects'), version='01.09'):
        self.calls = []

        def script(name, *args, **_kwargs):
            self.calls.append((name, [str(arg) for arg in args]))
            return name not in fail
        with mock.patch.object(run, 'run_script', script), mock.patch.object(start, 'game_app_version', lambda _g: version):
            return start.port_layers(self.root, self.out, str(self.config), set(groups), self.results)

    def test_the_same_order_as_run_py(self):
        self.config.write_text('output_res=2560x1080\nfov=60\nhud_position=50\n', encoding='utf-8')
        self.assertTrue(self.layers())
        self.assertEqual([name for name, _ in self.calls],
                         ['quit_menu.py', 'rebirth.py', 'bbsc_items.py', 'hud_layout.py', 'fov.py'])
        self.assertEqual(self.results.lines[:1], [f'layer {self.out / "quit_text"}'])
        self.assertEqual([line for line in self.results.lines if line.startswith('layer ')],
                         [f'layer {self.out / name}' for name in ('quit_text', 'rebirth', 'bbsc_items', 'hud_layout', 'fov')])
        self.assertIn('--pc', self.calls[0][1])
        self.assertEqual(self.calls[3][1][self.calls[3][1].index('--position') + 1], '0.5')
        self.assertEqual(self.calls[4][1][self.calls[4][1].index('--fov') + 1], '60')

    def test_16_9_and_the_games_field_of_view_need_no_layer(self):
        self.config.write_text('output_res=2560x1440\nfov=43\n', encoding='utf-8')
        self.layers()
        self.assertEqual([name for name, _ in self.calls], ['quit_menu.py', 'rebirth.py', 'bbsc_items.py'])

    def test_switched_off(self):
        self.config.write_text('quit_to_desktop=0\npc_menu=0\nrebirth=0\n', encoding='utf-8')
        self.layers()
        self.assertEqual([name for name, _ in self.calls], ['bbsc_items.py'])

    def test_texts_not_written_turn_the_game_code_off(self):
        self.layers(fail=('quit_menu.py', 'rebirth.py'))
        self.assertEqual(os.environ['BB_QUIT_TO_DESKTOP'], '0')
        self.assertEqual(os.environ['BB_PC_MENU'], '0')
        self.assertEqual(os.environ['BB_REBIRTH'], '0')
        self.assertIn('env BB_REBIRTH=0', self.results.lines)
        self.assertFalse(any('quit_text' in line for line in self.results.lines))

    def test_other_game_versions_get_no_menu_layers(self):
        self.layers(version='01.00', groups=())
        self.assertEqual([name for name, _ in self.calls], ['bbsc_items.py'])

    def test_wide_output_without_the_resolution_patches_keeps_the_16_9_menus(self):
        self.config.write_text('output_res=2560x1080\n', encoding='utf-8')
        self.layers(groups=('fps',))
        self.assertNotIn('hud_layout.py', [name for name, _ in self.calls])


class SaveTests(StepCase):
    def saves(self, name='user'):
        user = self.root / name
        folder = user / 'savedata' / '1' / 'CUSA03173'
        folder.mkdir(parents=True)
        (folder / 'userdata0000').write_bytes(b'save')
        (folder / 'SPRJ0005').mkdir()
        return user

    def test_backup_once_per_launch(self):
        user = self.saves()
        os.environ['BB_SAVE_BACKUPS'] = '1'
        with mock.patch.object(run, 'run_script', lambda *_a, **_k: True):
            start.saves_steps(user, self.results)
            first = list((user / 'save-backups').iterdir())
            self.assertEqual(len(first), 1)
            self.assertEqual(os.environ[start.BACKED_UP], '1')  # the in-game restart inherits it
            with mock.patch.object(run, 'backup_saves') as backup:
                start.saves_steps(user, self.results)
            backup.assert_not_called()

    def test_no_backup_unless_asked(self):
        user = self.saves()
        with mock.patch.object(run, 'run_script', lambda *_a, **_k: True):
            start.saves_steps(user, self.results)
        self.assertFalse((user / 'save-backups').exists())

    def test_bells_put_back_in_normal_saves(self):
        user = self.saves()
        script = mock.Mock(return_value=True)
        with mock.patch.object(run, 'run_script', script):
            start.saves_steps(user, self.results)
        self.assertEqual(script.call_args[0][:2], ('bbsc_items.py', '--repair'))

    def test_seamless_coop_saves_from_windows_are_left_alone(self):
        user = self.saves(run.COOP_SAVES)
        script = mock.Mock(return_value=True)
        with mock.patch.object(run, 'run_script', script):
            start.saves_steps(user, self.results)
        script.assert_not_called()


class TrophyTests(StepCase):
    def test_names_without_pillow_or_a_game_still_work(self):
        start.trophy_popup(self.root, self.out, str(self.config), self.results)
        self.assertEqual(Path(os.environ['BB_TROPHY_TEXT']), self.out / 'trophies.tsv')
        self.assertTrue((self.out / 'trophies.tsv').read_text(encoding='utf-8').startswith('lang\ten\n'))
        self.assertNotIn('BB_TROPHY_ICONS', os.environ)  # this fake game has no trophy file: the cup

    def test_chinese_unlocks_the_game_region(self):
        os.environ['BB_LANGUAGE'] = '11'
        start.trophy_popup(self.root, self.out, str(self.config), self.results)
        self.assertIn('patch Unlock Game Region', self.results.lines)


class ViewTests(StepCase):
    def view(self):
        game = self.root / 'game'
        game.mkdir(exist_ok=True)
        (game / 'eboot.bin').write_bytes(b'game')
        view = self.out / 'mod-game-old'
        view.mkdir()
        shutil.copy(game / 'eboot.bin', view / 'eboot.bin')
        return game, view

    def test_a_view_left_behind_is_removed(self):
        game, view = self.view()
        start.remove_stale_views(self.out, game, running=lambda: False)
        self.assertFalse(view.exists())
        self.assertTrue((game / 'eboot.bin').is_file())

    def test_not_while_a_game_runs(self):
        game, view = self.view()
        start.remove_stale_views(self.out, game, running=lambda: True)
        self.assertTrue(view.exists())

    def test_only_views(self):
        game, _view = self.view()
        other = self.out / 'quit_text'
        other.mkdir()
        start.remove_stale_views(self.out, game, running=lambda: False)
        self.assertTrue(other.is_dir())

    def test_game_running(self):
        proc = self.root / 'proc'
        (proc / '12').mkdir(parents=True)
        (proc / '12' / 'comm').write_text('bash\n', encoding='utf-8')
        self.assertFalse(start.game_running(proc))
        (proc / '13').mkdir()
        (proc / '13' / 'comm').write_text('bb-probe\n', encoding='utf-8')
        self.assertTrue(start.game_running(proc))
        self.assertTrue(start.game_running(self.root / 'none'))  # not known: the views stay


class ResolutionTests(StepCase):
    def ui(self, live, output, groups='effects,fps,resolution', ini=''):
        self.config.write_text(ini, encoding='utf-8')
        args = mock.Mock(live=live, output=output, groups=groups, config=str(self.config))
        start.ui_res(args, self.results)
        return self.results.lines

    def test_ultrawide_needs_the_startup_patch(self):
        lines = self.ui('1', '2560x1080', ini='output_res=2560x1080\n')
        self.assertIn('live 0', lines)
        self.assertTrue(any(line.startswith('env BB_UI_RES=') for line in lines))

    def test_16_9_keeps_live(self):
        lines = self.ui('1', '2560x1440', ini='output_res=2560x1440\n')
        self.assertEqual(lines, ['unset BB_UI_RES', 'live 1'])

    def test_no_resolution_patches_change_sizes_live(self):
        self.assertIn('live 1', self.ui('0', '2560x1440', groups='fps', ini='output_res=2560x1440\n'))

    def test_groups_not_known_change_nothing(self):
        self.assertIn('live 0', self.ui('0', '2560x1440', groups='unknown', ini='output_res=2560x1440\n'))

    def test_1080p(self):
        self.assertEqual(self.ui('0', ''), ['unset BB_UI_RES', 'live 0'])


class LowVramTests(StepCase):
    def low(self, vram, output='', groups='effects,fps,resolution', cores=8):
        args = mock.Mock(caps='caps', config=str(self.config), output=output, groups=groups)
        with mock.patch.object(run, 'low_vram_mb', lambda _env, _caps: vram), \
                mock.patch.object(run, 'run_script', lambda *_a, **_k: '960x540\n'), \
                mock.patch.object(run, 'physical_cores', lambda: cores):
            start.low_vram(args, self.results)
        return self.results.lines

    def test_an_8_gb_card_changes_nothing(self):
        self.assertEqual(self.low(0), [])

    def test_a_4_gb_card(self):
        lines = self.low(4096, cores=4)
        self.assertIn('env BB_GC_BUDGET_MB=3496', lines)
        self.assertIn('env BB_RENDER_RES=960x540', lines)
        self.assertIn('env BB_AUTO_RENDER_RES=1', lines)
        self.assertIn('env BB_PREP_WORKERS=0', lines)

    def test_a_chosen_size_stays(self):
        lines = self.low(4096, output='2560x1440')
        self.assertNotIn('env BB_RENDER_RES=960x540', lines)
        self.assertIn('env BB_GC_BUDGET_MB=3496', lines)


class FairPlayTests(unittest.TestCase):
    def test_the_launchers_list_matches_patches_py(self):
        sys.path.insert(0, str(ROOT / 'launcher'))
        import bbport_linux_online as online
        for key in online.OFFLINE_ONLY_EFFECTS:
            self.assertIn(EFFECTS[key][1], OFFLINE_ONLY)


class PackageTests(unittest.TestCase):
    def test_the_package_ships_run_py_and_pillow(self):
        nix = (ROOT / 'packaging' / 'default.nix').read_text(encoding='utf-8')
        self.assertIn('"run.py"', nix)
        self.assertIn('cp run.sh run.py $d/', nix)
        self.assertIn('ps.pillow', nix)
        self.assertNotIn('--set PYTHON ${pkgs.python3}', nix)

    def test_run_sh_uses_bash_builtins_for_the_results(self):
        text = (ROOT / 'run.sh').read_text(encoding='utf-8')
        for tool in ('sed ', 'grep ', 'awk '):
            self.assertNotIn(f'| {tool}', text)


STEP_STUB = textwrap.dedent('''\
    import os, sys
    from pathlib import Path
    args = sys.argv[1:]
    step = args[0]
    Path(os.environ['STUB_LOG']).open('a').write('step ' + step + ' ' + ' '.join(args[1:]) + '\\n')
    results = Path(args[args.index('--results') + 1])
    lines = {'before-view': os.environ.get('STUB_BEFORE', ''), 'ui-res': os.environ.get('STUB_UI', ''),
             'low-vram': os.environ.get('STUB_LOW', '')}[step]
    results.write_bytes(lines.replace('|', chr(10)).encode())
    sys.exit(int(os.environ.get('STUB_FAIL_' + step.replace('-', '_').upper(), '0')))
''')
MODS_STUB = textwrap.dedent('''\
    import os, shutil, sys
    from pathlib import Path

    def remove_overlay(overlay, game=None):
        Path(os.environ['STUB_LOG']).open('a').write('remove ' + str(overlay) + '\\n')
        shutil.rmtree(overlay)

    if __name__ == '__main__':
        args = sys.argv[1:]
        Path(os.environ['STUB_LOG']).open('a').write('mods ' + ' '.join(args) + '\\n')
        if '--layer' in args and os.environ.get('STUB_MODS_FAIL'):
            sys.exit(1)
        if '--layer' in args:
            view = Path(args[args.index('--out') + 1]) / 'mod-game-1'
            view.mkdir()
            sys.stdout.buffer.write(view.as_posix().encode())  # as on Linux, also when this test runs on Windows
        else:
            sys.stdout.buffer.write(args[0].encode())
''')
PATCHES_STUB = textwrap.dedent('''\
    import os, sys
    from pathlib import Path
    args = sys.argv[1:]
    if '--print-scaled' in args:
        sys.stdout.buffer.write(os.environ.get('STUB_SCALED', '').encode() + b'\\n')
    else:
        Path(os.environ['STUB_LOG']).open('a').write('patches ' + ' '.join(args) + '\\n')
''')
PLAIN_STUB = 'import sys\n'
LINK_STUB = textwrap.dedent('''\
    import json, sys
    from pathlib import Path
    args = sys.argv[1:]
    (Path(args[args.index('--out') + 1]) / 'link.json').write_text(json.dumps({}))
''')
PROBE_STUB = textwrap.dedent('''\
    #!/usr/bin/env bash
    {
      echo "probe $*"
      for name in BB_QUIT_TO_DESKTOP BB_PC_MENU BB_REBIRTH BB_TROPHY_TEXT BB_VBLANK_HZ BB_AUTO_VBLANK BB_UI_RES BB_GC_BUDGET_MB; do
        echo "game $name=${!name-}"
      done
    } >> "$STUB_LOG"
''')


@unittest.skipUnless(shutil.which('bash'), 'needs bash')
class RunShTests(unittest.TestCase):
    """run.sh itself: what it does with the steps' answers. The game folder is a fake one in the temporary folder."""

    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name).resolve()
        self.port = self.root / 'port'
        (self.port / 'scripts').mkdir(parents=True)
        (self.port / 'bin').mkdir()
        shutil.copy(ROOT / 'run.sh', self.port / 'run.sh')
        stubs = {'linux_start.py': STEP_STUB, 'mods.py': MODS_STUB, 'patches.py': PATCHES_STUB, 'prepare.py': PLAIN_STUB,
                 'link_libc.py': PLAIN_STUB, 'link_modules.py': LINK_STUB, 'content_profile.py': PLAIN_STUB}
        for name, text in stubs.items():
            (self.port / 'scripts' / name).write_text(text, encoding='utf-8')
        probe = self.port / 'bin' / 'bb-probe'
        probe.write_bytes(PROBE_STUB.encode())
        probe.chmod(probe.stat().st_mode | stat.S_IXUSR)
        self.game = self.root / 'game'
        self.game.mkdir()
        (self.game / 'eboot.bin').write_bytes(b'not a game')
        self.log = self.root / 'log.txt'

    def run_sh(self, **extra):
        env = clean_environ(PYTHON=sys.executable, BB_PREBUILT='1', BB_GAME_DIR=str(self.game), STUB_LOG=str(self.log),
                            BB_LOW_VRAM='0', **extra)
        result = subprocess.run(['bash', 'run.sh'], cwd=self.port, env=env, stdin=subprocess.DEVNULL,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=120)
        return result.returncode, result.stdout.decode('utf-8', 'replace')

    def lines(self, kind):
        return [line[len(kind) + 1:] for line in self.log.read_text(encoding='utf-8').splitlines()
                if line.startswith(kind + ' ')]

    def game_env(self):
        return dict(line.split('=', 1) for line in self.lines('game'))

    def test_layers_make_a_view_that_goes_away_and_the_game_stays(self):
        code, output = self.run_sh(STUB_BEFORE='groups effects,fps,resolution|layer out/quit_text|layer out/rebirth|'
                                               'env BB_TROPHY_TEXT=out/trophies.tsv|patch Unlock Game Region')
        self.assertEqual(code, 0, output)
        self.assertIn('--layer out/quit_text --layer out/rebirth', self.lines('mods')[0])
        self.assertIn('mod-game-1', self.lines('probe')[0])
        self.assertEqual(self.game_env()['BB_TROPHY_TEXT'], 'out/trophies.tsv')
        self.assertIn('--extra Unlock Game Region', self.lines('patches')[0])
        self.assertTrue(self.lines('remove'))
        self.assertFalse((self.port / 'out' / 'mod-game-1').exists())
        self.assertTrue((self.game / 'eboot.bin').is_file())

    def test_no_view_and_the_game_is_never_removed(self):
        code, output = self.run_sh(STUB_BEFORE='groups effects,fps,resolution')
        self.assertEqual(code, 0, output)
        self.assertEqual(self.lines('remove'), [])
        self.assertTrue((self.game / 'eboot.bin').is_file())
        self.assertIn(str(self.game), self.lines('probe')[0])

    def test_a_failed_step_starts_the_game_without_the_menu_entries(self):
        code, output = self.run_sh(STUB_FAIL_BEFORE_VIEW='1', STUB_BEFORE='layer out/quit_text')
        self.assertEqual(code, 0, output)
        self.assertNotIn('--layer', self.lines('mods')[0])
        env = self.game_env()
        self.assertEqual((env['BB_QUIT_TO_DESKTOP'], env['BB_PC_MENU'], env['BB_REBIRTH']), ('0', '0', '0'))
        self.assertEqual(self.lines('probe')[0].count('--app0'), 1)

    def test_a_view_that_fails_with_layers_is_made_without_them(self):
        code, output = self.run_sh(STUB_MODS_FAIL='1', STUB_BEFORE='layer out/quit_text')
        self.assertEqual(code, 0, output)
        self.assertEqual(len(self.lines('mods')), 2)
        self.assertNotIn('--layer', self.lines('mods')[1])
        self.assertEqual(self.game_env()['BB_REBIRTH'], '0')

    def test_other_steps_failing_do_not_stop_the_start(self):
        code, output = self.run_sh(STUB_FAIL_UI_RES='1', STUB_FAIL_LOW_VRAM='1')
        self.assertEqual(code, 0, output)
        self.assertTrue(self.lines('probe'))

    def test_a_modified_eboot_keeps_the_games_timing(self):
        code, output = self.run_sh(BB_FPS='uncap', STUB_BEFORE='groups effects')
        self.assertEqual(code, 0, output)
        self.assertIn('--fps 30', self.lines('patches')[0])
        self.assertEqual(self.game_env()['BB_VBLANK_HZ'], '60')

    def test_groups_not_known_keep_the_chosen_rate(self):
        code, output = self.run_sh(BB_FPS='uncap')
        self.assertEqual(code, 0, output)
        self.assertIn('--fps uncap', self.lines('patches')[0])
        self.assertEqual(self.game_env()['BB_VBLANK_HZ'], '0')

    def test_vblank_is_chosen_again_after_an_in_game_restart(self):
        code, output = self.run_sh(BB_FPS='60', BB_VBLANK_HZ='0', BB_AUTO_VBLANK='1')
        self.assertEqual(code, 0, output)
        self.assertEqual(self.game_env()['BB_VBLANK_HZ'], '60')

    def test_a_vblank_set_by_hand_is_used(self):
        code, output = self.run_sh(BB_FPS='60', BB_VBLANK_HZ='144')
        self.assertEqual(code, 0, output)
        self.assertEqual(self.game_env()['BB_VBLANK_HZ'], '144')

    def test_answers_of_the_later_steps_reach_the_game(self):
        code, output = self.run_sh(STUB_SCALED='1280x720 2560x1080', STUB_UI='env BB_UI_RES=2560x1080|live 0',
                                   STUB_LOW='env BB_GC_BUDGET_MB=3496')
        self.assertEqual(code, 0, output)
        env = self.game_env()
        self.assertEqual(env['BB_UI_RES'], '2560x1080')
        self.assertEqual(env['BB_GC_BUDGET_MB'], '3496')
        self.assertIn('--render-res 1280x720 --output-res 2560x1080', self.lines('patches')[0])

    def test_an_answer_can_turn_live_on(self):
        code, output = self.run_sh(STUB_SCALED='1280x720 2560x1440', STUB_UI='unset BB_UI_RES|live 1', BB_UI_RES='old')
        self.assertEqual(code, 0, output)
        self.assertEqual(self.game_env()['BB_UI_RES'], '')
        self.assertIn('--render-res  --output-res ', self.lines('patches')[0])
        self.assertIn('live resolution changes', output)


if __name__ == '__main__':
    unittest.main()
