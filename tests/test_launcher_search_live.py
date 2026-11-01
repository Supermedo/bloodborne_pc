"""The search against the real launcher pages (Windows with a display; nothing is written): every
setting is in the index with its key, its own title finds it first, the players' words find it
among the first three, and a jump shows it on its page."""
from paths import ROOT
import sys
import tempfile
import time
import unittest
from pathlib import Path

sys.path.insert(0, str(ROOT / 'launcher'))
sys.path.insert(0, str(ROOT / 'scripts'))
from test_launcher_search_questions import MORE_QUESTIONS, QUESTIONS  # noqa: E402  (the players' questions)

# Settings without a row of their own: internal state, or a part of another setting's row.
INTERNAL = {'setup_done', 'auto_hw', 'bench_goal', 'test_compare', 'test_mode', 'test_scenario', 'thd_token',
            'supermedo_pass', 'party_key', 'online_custom_servers',
            'output_display', 'window_custom', 'eboot_check', 'seen_version', 'ui_mode', 'auto_undo', 'test_saves',
            'test_repeats', 'test_minutes', 'last_measure', 'window_last'}

# What players type (German and English), and the settings, pages or topics any of which must be
# among the first three results.
QUERIES = [
    ('vollbild', {'fullscreen'}), ('fullscreen', {'fullscreen'}), ('fenster', {'fullscreen', 'window_size'}),
    ('auflösung', {'output_res'}), ('resolution', {'output_res'}), ('4k', {'output_res'}),
    ('2400x1080', {'output_res'}), ('monitor', {'game_monitor'}), ('zweiter bildschirm', {'game_monitor'}),
    ('second monitor', {'game_monitor'}), ('fenstergröße', {'window_size'}), ('window size', {'window_size'}),
    ('fsr', {'upscaler'}), ('dlss', {'upscaler'}), ('fps', {'fps_mode', 'frame_cap', 'show_fps'}),
    ('fps begrenzen', {'frame_cap', 'fps_mode'}), ('fps anzeigen', {'show_fps'}), ('framegen', {'frame_gen'}),
    ('hdr', {'hdr'}),
    ('bewegungsunschärfe', {'effect_motion_blur'}), ('motion blur', {'effect_motion_blur'}),
    ('schärfe', {'sharpen', 'sharpness'}), ('schatten', {'effect_dynamic_shadows'}), ('lod', {'model_lod'}),
    ('hud', {'hud_position'}), ('preset', {'preset'}), ('textsprache', {'language'}),
    ('sprachausgabe', {'voice_language'}), ('japanisch', {'voice_language', 'language'}),
    ('launcher sprache', {'ui_language'}), ('spielername', {'player_name'}), ('intro', {'skip_intro'}),
    ('stumm', {'mute_unfocused'}), ('verknüpfung', {'button:shortcut'}), ('steam', {'button:steam'}),
    ('dlc', {'dlc_dir'}), ('spielordner', {'game_dir'}), ('game folder', {'game_dir'}),
    ('spielstandordner', {'user_dir'}), ('backup', {'auto_backup', 'button:backup'}),
    ('sicherung', {'auto_backup', 'button:backup'}), ('online', {'online'}), ('koop', {'online', 'online_pool'}),
    ('unsterblich', {'cheat_no_death'}), ('god mode', {'cheat_no_death'}),
    ('update', {'check_updates', 'button:updates'}), ('durchsichtig', {'translucent_window'}),
    ('titelleiste', {'windows_title_bar'}), ('title bar', {'windows_title_bar'}),
    ('grafikkarte', {'gpu_override', 'page:system'}), ('maus', {'page:controls', 'mouse:stick', 'mouse:layout',
    'mouse:camera'}), ('empfindlichkeit', {'mouse:camera_sensitivity', 'mouse:speed'}),
    ('tastenbelegung', {'page:controls', 'mouse:layout'}), ('protokoll', {'page:log'}), ('log', {'page:log'}),
    ('ziel', {'goal'}), ('leise', {'goal'}), ('ruckelt', {'topic:shader_cache', 'topic:vram'}),
    ('einrichtung', {'action:setup'}), ('bericht', {'button:report'}),
    ('shader', {'button:shader_cache', 'topic:black_screen'}), ('shader cache', {'button:shader_cache'}),
    ('schwarzer bildschirm', {'topic:black_screen', 'button:shader_cache'}), ('respec', {'button:respec'}),
    ('umverteilen', {'button:respec'}), ('downgrade', {'button:versions'}),
    ('andere version', {'button:versions'}),
    ('controller test', {'controller_test'}), ('dualsense', {'controller_test'}),
    ('controller geht nicht', {'topic:controller'}), ('spielstand kaputt', {'topic:save_lost'}),
    ('tastatur und maus', {'play_with'}), ('spiele mit', {'play_with'}), ('mauskamera', {'play_with'}),
    ('keyboard', {'play_with', 'mouse:layout'}),
    ('charakter weg', {'topic:save_lost'}),
] + QUESTIONS + MORE_QUESTIONS  # in English too, on the German pages: the players' words carry them


@unittest.skipUnless(sys.platform == 'win32', 'the launcher window is Windows only')
class LiveSearchTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        try:
            import tkinter as tk
            from tkinter import filedialog, messagebox, ttk
            cls.root = tk.Tk()
        except Exception:  # no display
            raise unittest.SkipTest('no display') from None
        import bbport_launcher_win as L
        import bbport_search
        cls.L, cls.search = L, bbport_search
        cls.temp = tempfile.TemporaryDirectory()
        cls.saved = {name: getattr(L, name) for name in ('CONFIG_FILE', 'LANG')}
        L.CONFIG_FILE = Path(cls.temp.name) / 'settings.json'  # a fresh launcher; nothing of the player's
        L.LANG = 'de'
        patches = [('check_update', lambda self: None), ('collect', lambda self: None),
                   ('maybe_open_setup', lambda self: None)]
        cls.originals = {name: getattr(L.Launcher, name) for name, _f in patches}
        for name, function in patches:
            setattr(L.Launcher, name, function)
        cls.root.withdraw()
        cls.app = L.Launcher(cls.root, tk, ttk, filedialog, messagebox)
        cls.root.geometry('1280x860+0+0')
        cls.root.deiconify()
        for _i in range(40):
            cls.root.update()
        entries = [e for e in cls.app.search_entries if e['kind'] == 'setting']
        cls.settings = [e for e in entries if e.get('reveal') or
                        bbport_search.shown(e.get('widget'), cls.app.pages.get(e['page']))]

    @classmethod
    def tearDownClass(cls):
        cls.root.destroy()
        for name, function in cls.originals.items():
            setattr(cls.L.Launcher, name, function)
        for name, value in cls.saved.items():
            setattr(cls.L, name, value)
        cls.temp.cleanup()

    @staticmethod
    def key(entry):
        return f'topic:{entry["topic"]}' if entry.get('topic') else entry.get('key')

    def test_every_setting_is_in_the_search(self):
        indexed = {e.get('key') for e in self.app.search_entries}
        self.assertEqual(sorted(k for k in self.app.vars if k not in indexed and k not in INTERNAL), [])
        self.assertEqual([e['title'] for e in self.app.search_entries if e['kind'] == 'setting' and not e.get('key')],
                         [])

    def test_the_game_tests_are_gone_while_they_are_off(self):
        if self.L.GAME_TESTS:
            self.skipTest('the game tests are on')
        titles = [e['title'] for e in self.app.search_entries]
        self.assertNotIn(self.L._('Measure on this PC'), titles)
        self.assertNotIn(self.L._('Game tests'), titles)
        self.assertEqual(self.app.page_titles['system'], self.L._('Your PC'))
        self.assertFalse(getattr(self.app, 'bench_views', {}))
        self.assertTrue(self.app.search('benchmark'))  # Advanced > Run benchmark stays

    def test_its_own_title_finds_it_first(self):
        for entry in self.settings:
            with self.subTest(entry['title']):
                found = self.app.search(entry['title'])
                self.assertTrue(found)
                self.assertEqual(self.key(found[0]), entry['key'])

    def test_the_players_words(self):
        for query, keys in QUERIES:
            with self.subTest(query):
                found = [self.key(e) for e in self.app.search(query)[:3]]
                self.assertTrue(keys & set(found), f'{query!r} found {found}')

    def test_a_row_of_another_pool_is_found_and_leads_to_the_pool(self):
        """The Hunter's Dream's sign-in while SuperMedo Server is chosen: found with its note, and the jump
        shows "Play with" without switching the pool."""
        pool = self.app.var('online_pool', 'app')
        if pool.get() != 'supermedo' or not self.L.HUNTERS_DREAM_POOL:
            self.skipTest("SuperMedo Server is not chosen, or The Hunter's Dream is off")
        entry = next(e for e in self.app.search_entries if e.get('key') == 'button:signin_thd')
        self.assertTrue(self.app.hidden_for_now(entry))
        self.assertIn(entry, self.app.search('sign in hunters dream'))
        self.assertIn(self.L._("only with The Hunter's Dream"), self.app.search_where(entry))
        self.app.jump(entry)
        self.root.update()
        self.assertEqual(self.app.current_page, 'online')
        self.assertEqual(pool.get(), 'supermedo')
        choice = next(e for e in self.app.search_entries if e.get('key') == 'online_pool')
        self.assertTrue(choice['widget'].winfo_ismapped())

    def test_the_side_bar_buttons_are_marked_where_they_are(self):
        page, mode = self.app.current_page, self.app.app['ui_mode']
        for key in ('button:expert', 'button:site'):
            with self.subTest(key):
                entry = next(e for e in self.app.search_entries if e.get('key') == key)
                self.assertEqual((entry['kind'], self.app.search_where(entry)), ('side', self.L._('Side bar')))
                self.app.flash_parts = []
                self.app.jump(entry)
                end = time.time() + 1
                while time.time() < end and not self.app.flash_parts:
                    self.root.update()
                    time.sleep(0.02)
                self.assertTrue(self.app.flash_parts)
                self.assertIs(self.app.flash_parts[0].master, self.app.side)
        self.assertEqual((self.app.current_page, self.app.app['ui_mode']), (page, mode))  # nothing switched

    def test_a_jump_shows_the_setting(self):
        for entry in self.settings:
            with self.subTest(entry['title']):
                self.app.jump(entry)
                self.root.update()
                page = self.app.pages[entry['page']]
                self.assertEqual(self.app.current_page, entry['page'])
                self.assertTrue(entry['widget'].winfo_ismapped())
                y = entry['widget'].winfo_rooty()
                self.assertTrue(page.winfo_rooty() <= y <= page.winfo_rooty() + page.winfo_height() - 10)


if __name__ == '__main__':
    unittest.main()
