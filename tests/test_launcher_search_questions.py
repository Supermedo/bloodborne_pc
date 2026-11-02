"""Players' real questions from the Discord against the launcher's search, without a window: the index is
rebuilt from the pages' code (every index_entry, row, check and folder call with a written-out title, the
settings the pages list in a loop), and search_ready adds the pages, Help's topics and the setup as it does in
the launcher. "where is clear shader cache" and "how to update" found nothing (every word had to be found),
"sign in", "expert view" or "restore save" had no entry, "bug report" found the benchmark first; "absturz im
spiel" found Import first ("im"). test_launcher_search_live asks the same questions on the real pages."""
from paths import ROOT
import ast
import re
import sys
import unittest
from unittest import mock

sys.path.insert(0, str(ROOT / 'launcher'))
sys.path.insert(0, str(ROOT / 'scripts'))
import bbport_launcher_win as L  # noqa: E402  (the module only: no window)
import bbport_search as search  # noqa: E402

# A question as players wrote it, and the entries any of which must be among the first three.
QUESTIONS = [
    ('where is clear shader cache', {'button:shader_cache'}), ('how do i clear shader cache', {'button:shader_cache'}),
    ('how to update', {'button:updates'}), ('update launcher', {'button:updates'}),
    ('sign in', {'button:signin_supermedo', 'button:signin_thd'}),
    ('login', {'button:signin_supermedo', 'button:signin_thd'}), ('create account', {'button:create_account'}),
    ('test connection', {'button:test_connection'}), ('seamless', {'online', 'online_pool'}),
    ('co-op password', {'online', 'online_pool'}), ('fps limit 30', {'frame_cap', 'fps_mode'}),
    ('restore save', {'button:restore', 'button:backup'}), ('where are my saves', {'user_dir', 'page:saves'}),
    ('reset settings', {'action:setup'}), ('expert view', {'button:expert'}), ('older version', {'button:versions'}),
    ('bug report', {'button:report'}), ('mouse look', {'mouse:enabled'}),
]
# More of the same, and the German ones: with only some words found, a small word ("im" in "Import"), a plural in
# the wrong entry or a missing players' word put the wrong entries first.
MORE_QUESTIONS = [
    ('absturz im spiel', {'topic:page_file'}),
    # on the German pages "Spiel" is in many hints: there the stutter entries with both words (the video memory
    # topic, "Neue Shader beim Spielen") come before the shader cache topic
    ('ruckeln im spiel', {'topic:shader_cache', 'topic:vram', 'async_pipelines'}),
    ('wie melde ich mich an', {'button:signin_supermedo', 'button:signin_thd'}),
    ('wie melde ich mich ab', {'button:signout'}), ('sign-in', {'button:signin_supermedo', 'button:signin_thd'}),
    ('wo sind meine spielstände', {'user_dir', 'page:saves'}),
    ('wo sind die spielstände', {'user_dir', 'page:saves'}),
    ('why is my game stuttering', {'topic:shader_cache', 'frames_ahead'}),
    ('how do i play with friends', {'online', 'online_pool', 'page:online', 'topic:play_together'}),
    ('wie spiele ich mit freunden', {'topic:play_together', 'party_key', 'page:online'}),
    ('how do i add mods', {'mods_enabled', 'page:mods'}), ('install mods', {'page:mods'}),
    ('how to install', {'button:site'}),
]

SOURCES = ('bbport_launcher_win.py', 'bbport_setup.py', 'bbport_help.py', 'bbport_fsr41_ui.py')
# The functions that add rows to a page without opening it (scrolled_page): the page they are called for.
PAGE_OF = {'build': 'side', 'build_play': 'play', 'build_mouse_look': 'controls',
           'build_controller_bindings': 'controls', 'readouts_section': 'graphics', 'motion_cache_check': 'graphics',
           'settings_button': 'graphics', 'appearance_check': 'language'}
# What the pages hide at their defaults (SuperMedo Server, FSR 3.1): The Hunter's Dream's rows and fsr4vk with a
# note, an own server's rows (only when Seamless co-op starts the launcher) not at all.
ONLY = {'thd_online_id': 'online_pool', 'bridge_live_map': 'online_pool', 'button:signin_thd': 'online_pool',
        'button:fsr4vk': 'upscaler'}
HIDDEN = {'online_npid', 'online_password', 'online_community', 'online_server', 'online_webapi'}


def text(node):
    """A written-out text: 'Title' or _('Title'[, 'Russian']); None for one made at run time."""
    if isinstance(node, ast.Constant) and isinstance(node.value, str):
        return node.value
    if isinstance(node, ast.Call) and getattr(node.func, 'id', '') in ('_', 'translate') and node.args \
            and not isinstance(node.args[0], ast.Starred):
        return text(node.args[0])
    return None


def constant(node):
    return node.value if isinstance(node, ast.Constant) and isinstance(node.value, str) else None


def calls():
    """(page, English title, hint, key) of every call that registers a search entry with a written-out title."""
    found = []
    for name in SOURCES:
        for func in ast.walk(ast.parse((ROOT / 'launcher' / name).read_text(encoding='utf-8'))):
            if not isinstance(func, ast.FunctionDef):
                continue
            nodes = [n for n in ast.walk(func) if isinstance(n, ast.Call)]
            opened = [constant(n.args[0]) for n in nodes if getattr(n.func, 'attr', '') == 'scrolled_page' and n.args]
            page = opened[0] if opened else PAGE_OF.get(func.name)
            if not page:
                continue
            # frame.setting_key = 'key': the setting a row of that frame edits; server = self.var('key', …): its box
            owned = {ast.unparse(n.targets[0].value): n.value.value for n in ast.walk(func)
                     if isinstance(n, ast.Assign) and isinstance(n.targets[0], ast.Attribute)
                     and n.targets[0].attr == 'setting_key' and constant(n.value)}
            owned.update({f'textvariable={n.targets[0].id}': constant(n.value.args[0]) for n in ast.walk(func)
                          if isinstance(n, ast.Assign) and isinstance(n.targets[0], ast.Name)
                          and isinstance(n.value, ast.Call) and getattr(n.value.func, 'attr', '') == 'var'
                          and n.value.args and constant(n.value.args[0])})
            for node in nodes:
                kind, args = getattr(node.func, 'attr', ''), node.args
                kw = {k.arg: k.value for k in node.keywords if k.arg}
                if kind == 'index_entry' and len(args) >= 2:
                    key = constant(kw.get('key') or (args[3] if len(args) > 3 else None))
                    entry = (text(args[1]), args[2] if len(args) > 2 else kw.get('hint'), key)
                elif kind == 'row' and len(args) >= 3:
                    source = ast.unparse(args[2])
                    match = re.search(r"self\.(?:choice\(\w+, |var\()'(\w+)'", source)
                    box = re.search(r'textvariable=\w+', source)
                    key = constant(kw.get('key')) or (match.group(1) if match else
                                                      owned.get(box.group(0) if box else source))
                    entry = (text(args[1]), args[3] if len(args) > 3 else kw.get('hint'), key)
                elif kind == 'check' and len(args) >= 4 and getattr(node.func.value, 'id', '') == 'self':
                    entry = (text(args[3]), args[4] if len(args) > 4 else kw.get('hint'), constant(args[1]))
                elif kind == 'folder' and len(args) >= 3:
                    entry = (text(args[2]), args[4] if len(args) > 4 else kw.get('hint'), constant(args[1]))
                else:
                    continue
                title, hint, key = entry
                if title:
                    found.append((page, title, text(hint) or '', key))
    # the settings a page lists in a loop
    for key, title, _on in L.EFFECTS:
        found.append(('picture' if key in L.TASTE_EFFECTS else 'graphics', title[0], '', key))
    for table, page in ((L.TWEAKS, 'gameplay'), (L.CHEATS, 'cheats'), (L.EXTRAS, 'cheats')):
        found += [('gameplay' if key == 'skip_intro' else page, title[0], '', key) for key, title, _on in table]
    found += [('graphics', title[0], '', key) for key, title in L.READOUTS]
    return found


class Widget:
    def __init__(self, master=None, manager='grid'):
        self.master, self.manager = master, manager

    def winfo_manager(self):
        return self.manager


class Index(search.SearchMixin):
    """The launcher's search on the rebuilt index, in the simple view."""
    def __init__(self):
        self.app = {'ui_mode': 'simple'}
        self.pages = {name: Widget(manager='pack') for name, _title in L.NAV_SIMPLE + L.NAV_EXPERT + [('log', '')]}
        self.side = Widget(manager='pack')
        for page, title, hint, key in calls():
            hidden = key in ONLY or key in HIDDEN
            self.index_entry(Widget(self.side if page == 'side' else self.pages[page], '' if hidden else 'grid'),
                             title, hint, key)
            if key in ONLY:
                self.search_settings[-1]['only'] = ('only for another choice', ONLY[key])
        # the side bar's website button: its title is the site's address, not written out
        self.index_entry(Widget(self.side), L.COMMUNITY_SITE.split('//')[1].strip('/'),
                         'Install guide and FAQ on bbport.de', 'button:site')
        self.search_ready()


class QuestionTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        with mock.patch.object(L, 'LANG', 'en'):
            cls.index = Index()

    @staticmethod
    def key(entry):
        return f'topic:{entry["topic"]}' if entry.get('topic') else entry.get('key')

    def found(self, query, count=3):
        with mock.patch.object(L, 'LANG', 'en'):
            return [self.key(e) for e in self.index.search(query)[:count]]

    def test_the_index_has_the_pages_settings(self):
        keys = {e.get('key') for e in self.index.search_entries}
        for key in ('fullscreen', 'online_pool', 'user_dir', 'button:shader_cache', 'button:expert', 'mouse:enabled',
                    'button:signin_supermedo', 'button:import', 'effect_motion_blur', 'action:setup', 'button:site'):
            self.assertIn(key, keys)
        self.assertEqual(next(e for e in self.index.search_entries if e.get('key') == 'button:expert')['kind'], 'side')

    def test_the_players_questions(self):
        for query, keys in QUESTIONS + MORE_QUESTIONS:
            with self.subTest(query):
                found = self.found(query)
                self.assertTrue(keys & set(found), f'{query!r} found {found}')

    def test_what_is_meant_comes_first(self):
        """Not only among three: the small words and the generic ones do not win over what the player means."""
        for query, key in (('absturz im spiel', 'topic:page_file'), ('rollback', 'button:versions'),
                           ('wie melde ich mich an', 'button:signin_supermedo'), ('fehler melden', 'button:report'),
                           ('wo sind meine spielstände', 'user_dir'), ('log-in', 'button:signin_supermedo'),
                           ('why is my game stuttering', 'topic:shader_cache'), ('sign-out', 'button:signout')):
            with self.subTest(query):
                self.assertEqual(self.found(query, 1), [key])
        self.assertNotIn('button:add_backup', self.found('add mods', 7))  # every word in the mods' entries

    def test_what_the_page_shows_comes_before_another_choice(self):
        self.assertEqual(self.found('sign in', 1), ['button:signin_supermedo'])
        self.assertIn('button:signin_thd', self.found('sign in hunters dream', 1))
        self.assertNotIn('online_password', self.found('password', 7))  # an own server's: not offered

    def test_the_words_moved_to_their_pages(self):
        """Launcher customization lost the game's sound and voices to Gameplay, Advanced the shortcuts."""
        self.assertEqual(self.found('stimmen', 2)[-1:], ['page:gameplay'])
        self.assertNotIn('page:advanced', self.found('steam', 7))
        self.assertIn('page:language', self.found('steam', 3))


if __name__ == '__main__':
    unittest.main()
