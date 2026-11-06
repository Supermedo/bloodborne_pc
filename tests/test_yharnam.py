"""SuperMedo Seamless co-op with BBSC: Yharnam Together 1.0.0 (launcher/bbport_yharnam.py: its game started by this
launcher on the seamless server; scripts/fetch_yharnam.py: its game, without its launcher, for the package)."""
from paths import ROOT
import hashlib
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock
import zipfile

sys.path.insert(0, str(ROOT / 'launcher'))
sys.path.insert(0, str(ROOT / 'scripts'))
import bbport_yharnam as yharnam  # noqa: E402
import fetch_yharnam  # noqa: E402

KEY = 'ab' * 32


class Folders(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        self.port = self.root / 'port'
        self.base = self.port / 'seamless'
        for path in ('bin/bb-probe.exe', 'run.py', 'tools/party_boss_events.py'):  # BBSC's release
            (self.base / path).parent.mkdir(parents=True, exist_ok=True)
            (self.base / path).write_bytes(b'tool')
        (self.port / 'bin').mkdir(parents=True)
        (self.port / 'bin' / 'bb-probe-seamless.exe').write_bytes(b'engine')  # this port's seamless engine
        self.game = self.root / 'games' / 'CUSA03173'
        (self.game / 'dvdroot_ps4' / 'event').mkdir(parents=True)
        (self.game / 'dvdroot_ps4' / 'event' / 'common.emevd.dcx').write_bytes(b'events')
        (self.game / 'sce_sys').mkdir()
        (self.game / 'eboot.bin').write_bytes(b'eboot')


class StartTests(Folders):
    def test_available_with_this_ports_seamless_engine(self):
        self.assertTrue(yharnam.available(self.port))
        (self.port / 'bin' / 'bb-probe-seamless.exe').unlink()
        self.assertFalse(yharnam.available(self.port))  # BBSC's own engine is never used

    def test_the_environment_is_this_launchers_game_on_the_seamless_server(self):
        game_env = {'PATH': 'p', 'BB_RENDER_RES': '2560x1440', 'BB_UPSCALER': 'dlss', 'BB_MODS_ENABLED': '1',
                    'BB_MODS_DIR': 'my-mods', 'BB_PATCHES_DIR': 'my-patches', 'BB_GAME_DIR': 'my-game',
                    'BB_PROBE': 'normal', 'BB_FPS': 'uncap', 'BB_FPS_LIMIT': '30', 'BB_USER_DIR': 'normal-saves',
                    'BB_ONLINE': '1', 'BB_SHADNET_SERVER': 'normal:31313', 'BB_NET_BACKEND': 'thd',
                    'BB_THD_TOKEN': 't', 'BBSC_EMULATOR': 'x', 'SHADPS4_BLOODBORNE_PARTY': '1'}
        settings = {'supermedo_npid': ' hunter ', 'supermedo_pass': 'secret', 'yt_enemies': 'on', 'yt_rot': True,
                    'yt_travel': False, 'yt_vote': True, 'yt_tempo': 'relentless'}
        env = yharnam.environment(self.port, self.root / 'view', settings, '1.2.3.4', KEY, game_env)
        for name in ('PATH', 'BB_RENDER_RES', 'BB_UPSCALER', 'BB_PATCHES_DIR'):  # the player's
            self.assertEqual(env[name], game_env[name])
        for name in ('BB_NET_BACKEND', 'BB_THD_TOKEN', 'BBSC_EMULATOR', 'SHADPS4_BLOODBORNE_PARTY'):
            self.assertNotIn(name, env)
        self.assertEqual(env['BB_PROBE'], str(self.port / 'bin' / 'bb-probe-seamless.exe'))
        self.assertEqual(env['BB_GAME_DIR'], str(self.root / 'view' / 'CUSA03173'))
        self.assertEqual(env['BB_USER_DIR'], str(self.base / 'user'))  # never the normal saves
        self.assertEqual((env['BB_SHADNET_SERVER'], env['BB_SHADNET_WEBAPI']), ('1.2.3.4:31314', 'http://1.2.3.4:31315'))
        self.assertEqual((env['BB_SHADNET_NPID'], env['BB_SHADNET_PASSWORD']), ('hunter', 'secret'))
        self.assertEqual(env['BB_SHADNET_SERVER_KEY'], KEY)
        self.assertEqual(env['BB_MODS_ENABLED'], '1')  # the -mods folder brings the co-op events
        self.assertEqual(env['BB_MODS_GAME_LAST'], '1')  # the co-op events over a mod's (MOAL's)
        self.assertEqual(env['BB_MODS_DIR'], str(self.base / 'mods-view'))  # the player's mods as links
        self.assertEqual(env['SHADPS4_BLOODBORNE_SEAMLESS_COOP'], '1')
        self.assertEqual(env['SHADPS4_BLOODBORNE_HOST_RULES'], 'rot,relentless')  # no vote without travel
        self.assertEqual(env['SHADPS4_BLOODBORNE_HARDER_ENEMIES'], '3,1.5,3')
        self.assertEqual((env['BB_FPS'], env['BB_FPS_LIMIT']), ('uncap', '30'))  # the player's frame rate
        self.assertEqual(env['BB_GPU_USER_DIR'], str(self.base / 'user'))  # co-op files never in the normal cache
        self.assertEqual(env['BBSC_DISCORD_STATUS'], '0')  # the port's own Discord status

    def test_host_rules(self):
        self.assertEqual(yharnam.host_rules(dict(yharnam.DEFAULTS)), 'off')
        self.assertEqual(yharnam.host_rules({'yt_travel': True, 'yt_vote': True, 'yt_enemies': 'auto',
                                             'yt_tempo': 'aggressive'}), 'travel,vote,scale,aggressive')
        self.assertEqual(yharnam.host_rules({'yt_tempo': 'wild'}), 'off')
        self.assertEqual(yharnam.host_rules({'yt_revive': False}), 'norevive')

    def test_custom_enemies(self):
        env = yharnam.environment(self.port, self.root, {'yt_enemies': 'custom', 'yt_custom_hp': '2.5',
                                                         'yt_custom_damage': '0', 'yt_custom_poise': 'x'},
                                  '1.2.3.4', '', {})
        self.assertEqual(env['SHADPS4_BLOODBORNE_HARDER_ENEMIES'], '2.5,1,2')
        self.assertEqual(yharnam.custom_enemies({'yt_custom_hp': '40', 'yt_custom_damage': '1,5'}), '10,1.5,2')

    def test_normal_enemies_set_nothing(self):
        env = yharnam.environment(self.port, self.root, {'yt_enemies': 'off'}, '1.2.3.4', '', {})
        self.assertNotIn('SHADPS4_BLOODBORNE_HARDER_ENEMIES', env)
        self.assertNotIn('BB_SHADNET_SERVER_KEY', env)

    def test_the_backend_override_points_at_the_server(self):
        path = yharnam.write_host_overrides(self.port, '1.2.3.4')
        self.assertEqual(json.loads(path.read_text(encoding='utf-8')),
                         {'https://ss4.scej-network.jp:20443': 'http://1.2.3.4:20443'})

    def test_the_first_use_key_is_kept_once_from_the_games_own_line(self):
        line = f"[Net] <Info> net_link.cpp:120 Hello: {yharnam.LEARNED_KEY_LINE}{KEY}"
        self.assertEqual(yharnam.learned_key(line), KEY)
        self.assertIsNone(yharnam.learned_key(f'chat: {yharnam.LEARNED_KEY_LINE}{KEY}'))
        log = self.base / 'user' / 'log' / 'console.txt'  # the seamless game's output
        log.parent.mkdir(parents=True)
        log.write_text('other\n' + line + '\n', encoding='utf-8')
        self.assertTrue(yharnam.learn_from_log(self.port))
        self.assertEqual(yharnam.link_key(self.port), KEY)
        self.assertFalse(yharnam.keep_link_key(self.port, 'cd' * 32))  # never replaced from a log
        self.assertEqual(yharnam.link_key(self.port), KEY)

    def test_frozen_scripts_run_through_this_executable(self):
        with mock.patch.object(sys, 'frozen', True, create=True), mock.patch.object(sys, 'executable', 'B.exe'):
            self.assertEqual(yharnam.script_command('t.py', 'a'), ['B.exe', '--script', 't.py', 'a'])


class PrepareTests(Folders):
    def tool(self, command, **kwargs):
        """party_boss_events.py's part: the event files and their manifest."""
        out, events = Path(command[-2]), Path(command[-1])
        self.assertFalse(any(name.startswith('BB_') for name in kwargs['env']))
        out.mkdir(parents=True)
        (out / 'common.emevd.dcx').write_bytes(b'patched')
        (out / 'manifest.json').write_text(json.dumps({
            'tool_sha256': hashlib.sha256(b'tool').hexdigest(),
            'files': {'common.emevd.dcx': {'input_sha256': hashlib.sha256((events / 'common.emevd.dcx').read_bytes())
                                           .hexdigest(), 'output_sha256': 'mine'}}}), encoding='utf-8')
        return 0

    def test_prepare_links_the_game_and_puts_the_events_beside_it(self):
        with mock.patch.object(yharnam, 'view_root', return_value=self.root / 'view'):
            root, note = yharnam.prepare(self.port, self.game, lambda folder: '01.09', say=lambda _t: None,
                                         call=self.tool)
            self.assertFalse(yharnam.needs_prepare(self.port, self.game))
        self.assertEqual(note, '')
        view = root / 'CUSA03173'
        self.assertTrue(yharnam.is_junction(view / 'dvdroot_ps4'))
        self.assertEqual((view / 'eboot.bin').read_bytes(), b'eboot')
        self.assertEqual((root / 'CUSA03173-mods' / 'dvdroot_ps4' / 'event' / 'common.emevd.dcx').read_bytes(),
                         b'patched')
        # made again: the old view's links go, never the game's own files
        with mock.patch.object(yharnam, 'view_root', return_value=self.root / 'view'):
            yharnam.prepare(self.port, self.game, lambda folder: '01.09', say=lambda _t: None, call=self.tool)
        self.assertEqual((self.game / 'eboot.bin').read_bytes(), b'eboot')
        self.assertTrue((self.game / 'dvdroot_ps4' / 'event' / 'common.emevd.dcx').is_file())

    def test_the_same_game_files_make_no_note_whatever_bytes_bbscs_python_made(self):
        same = hashlib.sha256(b'events').hexdigest()
        (self.base / 'event-reference.json').write_text(json.dumps(
            {'files': {'common.emevd.dcx': {'input_sha256': same, 'output_sha256': 'theirs'}}}), encoding='utf-8')
        with mock.patch.object(yharnam, 'view_root', return_value=self.root / 'view'):
            _root, note = yharnam.prepare(self.port, self.game, lambda folder: '01.09', say=lambda _t: None,
                                          call=self.tool)
        self.assertEqual(note, '')
        (self.base / 'event-reference.json').write_text(json.dumps(
            {'files': {'common.emevd.dcx': {'input_sha256': 'other', 'output_sha256': 'theirs'}}}), encoding='utf-8')
        with mock.patch.object(yharnam, 'view_root', return_value=self.root / 'view'):
            _root, note = yharnam.prepare(self.port, self.game, lambda folder: '01.09', say=lambda _t: None,
                                          call=self.tool)
        self.assertIn('common.emevd.dcx', note)

    def test_another_version_without_its_update_is_refused(self):
        with self.assertRaises(RuntimeError):
            yharnam.prepare(self.port, self.game, lambda folder: '01.00', say=lambda _t: None, call=self.tool)

    def test_a_view_folder_with_files_of_its_own_is_not_emptied(self):
        view = self.root / 'view' / 'CUSA03173'
        view.mkdir(parents=True)
        (view / 'mine.txt').write_text('keep', encoding='utf-8')
        with mock.patch.object(yharnam, 'view_root', return_value=self.root / 'view'), \
                self.assertRaises(RuntimeError):
            yharnam.prepare(self.port, self.game, lambda folder: '01.09', say=lambda _t: None, call=self.tool)
        self.assertEqual((view / 'mine.txt').read_text(encoding='utf-8'), 'keep')


def crlf(text):
    return text.replace('\n', '\r\n')


class FetchTests(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        self.zip = self.root / 'release.zip'
        with zipfile.ZipFile(self.zip, 'w') as archive:
            archive.writestr('Yharnam Together/', b'')
            archive.writestr('Yharnam Together/BLauncher.exe', b'MZ')
            archive.writestr('Yharnam Together/_internal/python.dll', b'MZ')
            archive.writestr('Yharnam Together/launcher/bloodborne.png', b'png')
            archive.writestr('Yharnam Together/run.py', b'run')
            archive.writestr('Yharnam Together/bin/bb-probe.exe', b'MZ2')
            # BBSC's event tool, with the text FIXES changes (and its CRLF line ends)
            archive.writestr('Yharnam Together/tools/party_boss_events.py',
                             crlf('def parse():\n' + fetch_yharnam.FIXES[0][1] + '    pass\n'))
            archive.writestr('Yharnam Together/../escape.txt', b'no')
            archive.writestr('Other/stray.txt', b'no')
        self.sha = hashlib.sha256(self.zip.read_bytes()).hexdigest()

    def test_the_game_unpacks_without_its_launcher(self):
        dest = self.root / 'out' / 'seamless'
        (dest / 'old').mkdir(parents=True)
        self.assertEqual(fetch_yharnam.unpack(self.zip, dest, expected=self.sha), 3)
        self.assertEqual((dest / 'bin' / 'bb-probe.exe').read_bytes(), b'MZ2')
        # An event without instructions may give any offset (a mod's event file gave -1); line ends are kept.
        tool = (dest / 'tools' / 'party_boss_events.py').read_bytes().decode()
        self.assertIn(crlf(fetch_yharnam.FIXES[0][2]), tool)
        self.assertNotIn(crlf(fetch_yharnam.FIXES[0][1]), tool)
        self.assertIn('Changed by the SuperMedo port: tools/party_boss_events.py',
                      (dest / fetch_yharnam.MARK).read_text(encoding='utf-8'))
        for gone in ('BLauncher.exe', '_internal', 'launcher', 'old'):
            self.assertFalse((dest / gone).exists(), gone)
        self.assertFalse((self.root / 'out' / 'escape.txt').exists())
        self.assertIn(self.sha, (dest / fetch_yharnam.MARK).read_text(encoding='utf-8'))

    def test_a_fix_whose_text_is_gone_is_refused(self):
        tools = self.root / 'other' / 'tools'
        tools.mkdir(parents=True)
        (tools / 'party_boss_events.py').write_text('def parse():\n    pass\n', encoding='utf-8')
        with self.assertRaises(ValueError):
            fetch_yharnam.apply_fixes(self.root / 'other')

    def test_another_zip_is_refused(self):
        with self.assertRaises(ValueError):
            fetch_yharnam.unpack(self.zip, self.root / 'dest')
        self.assertFalse((self.root / 'dest').exists())

    def test_the_icon_modules_come_from_the_source_zip(self):
        source = self.root / 'source.zip'
        with zipfile.ZipFile(source, 'w') as archive:
            for name in fetch_yharnam.ICON_MODULES:
                archive.writestr(fetch_yharnam.SOURCE_TOP + name, name)
        (self.root / 'dest' / 'scripts').mkdir(parents=True)
        fetch_yharnam.add_icon_modules(source, self.root / 'dest', hashlib.sha256(source.read_bytes()).hexdigest())
        for name in fetch_yharnam.ICON_MODULES:
            self.assertEqual((self.root / 'dest' / 'scripts' / name).read_text(encoding='utf-8'), name)
        with self.assertRaises(ValueError):
            fetch_yharnam.add_icon_modules(source, self.root / 'dest')


class KeepTests(Folders):
    """A package refresh and an update replace BBSC's program, never the player's files or the game behind a view."""
    def setUp(self):
        super().setUp()
        (self.base / 'user' / 'save').mkdir(parents=True)
        (self.base / 'user' / 'save' / 'character').write_bytes(b'hunter')
        (self.base / 'bbport.ini').write_text('[graphics]\n', encoding='utf-8')
        view = self.base / 'coop-game' / 'CUSA03173'
        view.mkdir(parents=True)
        (view / yharnam.VIEW_MARK).write_text('view\n', encoding='utf-8')
        (view / 'eboot.bin').write_bytes(b'eboot')
        yharnam.make_junction(view / 'dvdroot_ps4', self.game / 'dvdroot_ps4')
        yharnam.make_junction(self.base / 'linked', self.game / 'sce_sys')

    def test_clear_program_keeps_the_player_files_and_the_game(self):
        yharnam.clear_program(self.base)
        self.assertEqual(sorted(p.name for p in self.base.iterdir()), ['bbport.ini', 'coop-game', 'user'])  # PLAYER_FILES
        self.assertEqual((self.base / 'user' / 'save' / 'character').read_bytes(), b'hunter')
        self.assertEqual((self.game / 'dvdroot_ps4' / 'event' / 'common.emevd.dcx').read_bytes(), b'events')
        self.assertTrue((self.game / 'sce_sys').is_dir())

    def test_an_update_leaves_the_view_behind(self):
        import bbport_update
        new = self.root / 'new' / 'seamless'
        new.mkdir(parents=True)
        bbport_update.carry_over(self.base, new, [])
        self.assertEqual((new / 'user' / 'save' / 'character').read_bytes(), b'hunter')
        self.assertFalse((new / 'coop-game' / 'CUSA03173').exists())
        self.assertEqual(bbport_update.VIEW_MARK, yharnam.VIEW_MARK)


class ModsViewTests(Folders):
    """Seamless's own mods folder: the player's chosen mods as links in their order, the party HUD last."""
    def setUp(self):
        super().setUp()
        (self.game / 'dvdroot_ps4' / 'menu').mkdir(parents=True)
        (self.game / 'dvdroot_ps4' / 'menu' / 'fe.gfx').write_bytes(b'game hud')
        self.mods = self.root / 'my-mods'
        for name, files in (('Compact HUD', {'menu/fe.gfx': b'compact hud'}), ('Blood', {'chr/c0000.chrbnd': b'x'}),
                            ('Off', {'chr/c1000.chrbnd': b'y'})):
            for relative, data in files.items():
                path = self.mods / name / 'dvdroot_ps4' / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(data)
        self.config = self.root / 'mods.json'
        self.config.write_text(json.dumps({'order': ['Compact HUD', 'Blood'], 'disabled': ['Off']}), encoding='utf-8')
        scripts = self.base / 'scripts'
        scripts.mkdir()
        (scripts / 'hud_patch.py').write_text(
            'from pathlib import Path\nPATCH_VERSION = b"t"\nHUD_ICONS = Path(__file__).parent / "icons.bin"\n'
            'def patch_fe(data, strip=None):\n    return b"party:" + data\n', encoding='utf-8')

    def test_the_players_mods_then_the_party_hud(self):
        view, config, note = yharnam.mods_view(self.port, self.game, self.mods, self.config, True)
        self.assertEqual(note, '')
        order = json.loads(config.read_text(encoding='utf-8'))['order']
        self.assertEqual(order, ['Compact HUD', 'Blood', yharnam.PARTY_HUD])
        self.assertTrue(yharnam.is_link(view / 'Blood'))
        self.assertFalse((view / 'Off').exists())
        hud = view / yharnam.PARTY_HUD / 'dvdroot_ps4' / 'menu' / 'fe.gfx'
        self.assertEqual(hud.read_bytes(), b'party:compact hud')  # made from the HUD the mods show
        yharnam.mods_view(self.port, self.game, self.mods, self.config, True)  # again: links remade, mods kept
        self.assertEqual((self.mods / 'Blood' / 'dvdroot_ps4' / 'chr' / 'c0000.chrbnd').read_bytes(), b'x')

    def test_mods_off_bring_only_the_party_hud(self):
        view, config, _note = yharnam.mods_view(self.port, self.game, self.mods, self.config, False)
        self.assertEqual(json.loads(config.read_text(encoding='utf-8'))['order'], [yharnam.PARTY_HUD])
        hud = view / yharnam.PARTY_HUD / 'dvdroot_ps4' / 'menu' / 'fe.gfx'
        self.assertEqual(hud.read_bytes(), b'party:game hud')
        env = yharnam.environment(self.port, self.root, {}, '1.2.3.4', '', {'BB_MODS_ENABLED': '0'}, (view, config))
        self.assertEqual((env['BB_MODS_ENABLED'], env['BB_MODS_DIR']), ('1', str(view)))

    def test_without_the_patcher_the_game_still_starts(self):
        (self.base / 'scripts' / 'hud_patch.py').unlink()
        _view, config, note = yharnam.mods_view(self.port, self.game, self.mods, self.config, True)
        self.assertIn('party HUD is off', note)
        self.assertEqual(json.loads(config.read_text(encoding='utf-8'))['order'], ['Compact HUD', 'Blood'])


class PackageTests(unittest.TestCase):
    def script(self):
        return (ROOT / 'packaging' / 'windows' / 'package.sh').read_text(encoding='utf-8')

    def test_the_package_ships_one_seamless(self):
        script = self.script()
        new, old = script.index('out/bb-probe-seamless.exe'), script.index('out/bbsc/bbsc-launch.exe')
        self.assertLess(new, old)
        self.assertIn('llvm-strip --strip-debug -o "$dest/bin/bb-probe-seamless.exe" "$seamless_exe"', script)
        self.assertIn('ldd "$dest/bin/bb-probe-seamless.exe"', script)  # its own DLLs (abseil's logging)
        self.assertIn('elif [[ -f out/bbsc/bbsc-launch.exe ]]', script)

    def test_the_package_takes_named_files_from_bbscs_release_only(self):
        script = self.script()
        self.assertNotIn('cp -r out/seamless ', script)
        self.assertNotIn('find out/seamless', script)
        self.assertNotIn('out/seamless/bin', script)  # BBSC's own engine is not shipped
        self.assertIn('bbport_yharnam.clear_program(sys.argv[1])', script)
        self.assertIn('! -name seamless', script)


if __name__ == '__main__':
    unittest.main()
