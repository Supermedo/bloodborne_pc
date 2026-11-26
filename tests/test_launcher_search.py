"""The launcher's search (launcher/bbport_search.py): the players' words find settings, pages
and Help topics."""
from paths import ROOT
import importlib.util
import sys
import unittest

sys.path.insert(0, str(ROOT / 'launcher'))


def load(name):
    spec = importlib.util.spec_from_file_location(name, ROOT / f'launcher/{name}.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


search = load('bbport_search')
help_ = load('bbport_help')


class Launcher:
    EXPERT_PAGES = {'graphics', 'cheats', 'system', 'advanced', 'log'}

    @staticmethod
    def _(*texts):
        return texts[0]


class Fake(search.SearchMixin):
    def __init__(self, mode='simple'):
        self.app = {'ui_mode': mode}
        entries = [
            {'kind': 'setting', 'page': 'picture', 'title': 'Fullscreen', 'hint': '', 'key': 'fullscreen'},
            {'kind': 'setting', 'page': 'graphics', 'title': 'Frame rate', 'hint': 'Community patches', 'key': 'fps_mode'},
            {'kind': 'setting', 'page': 'game', 'title': 'Text language', 'hint': '', 'key': 'language'},
            {'kind': 'page', 'page': 'controls', 'title': 'Controls', 'hint': '', 'key': 'page:controls'},
            {'kind': 'page', 'page': 'saves', 'title': 'Saves', 'hint': '', 'key': 'page:saves'},
        ]
        for key in help_.TOPIC_ORDER:
            title, why, _steps = help_.TOPICS[key]
            entries.append({'kind': 'topic', 'page': 'help', 'topic': key, 'title': title, 'hint': why,
                            'key': f'topic:{key}'})
        for entry in entries:
            entry['haystack'] = (search.fold(entry['title']), search.fold(entry['hint']),
                                 search.fold(search.WORDS.get(entry['key'], '')))
        self.search_entries = entries


class Widget:
    """A widget as the search sees it: its master, and whether it is laid out (winfo_manager)."""
    def __init__(self, manager, master=None):
        self.manager, self.master = manager, master

    def winfo_manager(self):
        return self.manager


class Root:
    def __init__(self):
        self.later = []

    def after(self, _ms, call):
        self.later.append(call)

    def update_idletasks(self):
        pass


class Online(Fake):
    """The Online page with SuperMedo Server chosen: Seamless co-op's part is hidden; the jump records
    where it goes."""
    def __init__(self):
        super().__init__()
        self.page = Widget('pack')
        self.pages, self.page_titles = {'online': self.page}, {'online': 'Online play'}
        self.vars = {'online_pool': 'supermedo'}
        self.seamless = Widget('', self.page)  # Seamless co-op's rows, hidden
        self.pool = Widget('grid', self.page)
        self.signin, self.test = Widget('grid', self.seamless), Widget('grid', self.page)
        self.search_settings = [
            {'kind': 'setting', 'widget': self.pool, 'mark': self.pool, 'title': 'Play with', 'hint': '',
             'key': 'online_pool'},
            {'kind': 'setting', 'widget': self.signin, 'mark': self.signin, 'title': 'Enemy health per guest',
             'hint': '', 'key': 'bbsc_difficulty'},
            {'kind': 'setting', 'widget': self.test, 'mark': self.test, 'title': 'Test connection', 'hint': '',
             'key': 'button:test_connection'}]
        for entry in self.search_settings:
            entry.update(page='online', haystack=(search.fold(entry['title']), '',
                                                  search.fold(search.WORDS.get(entry['key'], ''))))
        self.search_entries += self.search_settings
        self.root, self.shown_pages, self.marked, self.scrolled = Root(), [], [], []

    def show(self, page):
        self.shown_pages.append(page)

    def scroll_to(self, widget, above=None):
        self.scrolled.append(widget)

    def flash(self, mark, widget=None):
        self.marked.append(mark)

    @staticmethod
    def px(value):
        return value


class SearchTests(unittest.TestCase):
    def setUp(self):
        search.bind(Launcher)

    def first(self, query, mode='simple'):
        found = Fake(mode).search(query)
        return (found[0].get('topic') or found[0]['key']) if found else None

    def test_players_words(self):
        self.assertEqual(self.first('vollbild'), 'fullscreen')
        self.assertEqual(self.first('Fullscreen'), 'fullscreen')
        self.assertEqual(self.first('schwarzer bildschirm'), 'black_screen')
        self.assertEqual(self.first('black screen'), 'black_screen')
        self.assertEqual(self.first('controller'), 'page:controls')
        self.assertEqual(self.first('spielstand shadps4'), 'page:saves')
        self.assertEqual(self.first('code 3'), 'page_file')
        self.assertEqual(self.first('eboot lance'), 'eboot_modified')
        self.assertEqual(self.first('deutsch'), 'language')

    def test_accents_and_case_do_not_matter(self):
        self.assertEqual(search.fold('Stürzt AB'), 'sturzt ab')
        self.assertEqual(search.fold('Größe'), 'grosse')
        self.assertEqual(self.first('STÜRZT'), self.first('sturzt'))

    def test_entries_with_every_word_first(self):
        found = [e['key'] for e in Fake().search('frame rate')]
        self.assertEqual(found, ['fps_mode'])  # not the entries with only one of the words
        self.assertEqual(Fake().search(''), [])

    def test_without_an_entry_with_every_word_the_most_of_them(self):
        """A question with a word no entry has found nothing ("Nothing found")."""
        self.assertEqual(self.first('vollbild xyzzy'), 'fullscreen')
        self.assertIsNone(self.first('xyzzy'))
        fake = Fake()
        self.assertEqual(fake.search('controls vollbild fenster xyzzy')[0]['key'], 'fullscreen')  # two of the words
        self.assertEqual({e['key'] for e in fake.search('controls saves xyzzy')[:2]}, {'page:controls', 'page:saves'})

    def test_the_words_around_a_question_are_left_out(self):
        self.assertEqual(search.query_words('Where is clear shader cache?'), ['clear', 'shader', 'cache'])
        self.assertEqual(search.query_words('how do I update'), ['update'])
        self.assertEqual(search.query_words('Wo ist mein Spielstand?'), ['spielstand'])
        self.assertEqual(search.query_words('the'), ['the'])  # nothing else: it stays
        self.assertEqual(search.query_words('log in'), ['login'])  # not "log" alone (the Log page)
        self.assertEqual(search.query_words('2560x1440'), ['resolution'])
        self.assertEqual(search.query_words('?'), [])
        self.assertEqual(self.first('where is fullscreen'), 'fullscreen')
        self.assertEqual(self.first('wie kann ich vollbild'), 'fullscreen')

    def test_the_german_small_words_and_the_sign_in_spellings(self):
        """"im" found "Import", "mich" a hint; "sign-in" (the launcher's own spelling) found nothing."""
        self.assertEqual(search.query_words('Absturz im Spiel'), ['absturz', 'spiel'])
        self.assertEqual(search.query_words('wie melde ich mich an'), ['melde'])
        self.assertEqual(search.query_words('spielen mit freunden'), ['spielen', 'mit', 'freunden'])  # "Play with"
        for query, word in (('sign-in', 'signin'), ('Log-in?', 'login'), ('sign-out', 'signout'),
                            ('log-out', 'logout'), ('logged in', 'login')):
            self.assertEqual(search.query_words(query), [word], query)
        self.assertEqual(self.first('absturz im spiel'), 'page_file')

    def test_a_setting_for_another_choice_is_found_with_a_note(self):
        """A row of the online pool not chosen: found, after what the page shows, and the result says
        which pool it needs. Without such a note a hidden row stays out."""
        fake = Fake()
        page = Widget('pack')
        fake.pages, fake.page_titles = {'online': page}, {'online': 'Online play'}
        for title, key, widget, only in (
                ('Sign in with Discord', 'button:signin_supermedo', Widget('grid', page), None),
                ('Enemy health per guest', 'bbsc_difficulty', Widget('', page),
                 ('only with SuperMedo Seamless co-op', 'online_pool')),
                ('Account', 'online_npid', Widget('', page), None)):
            fake.search_entries.append({'kind': 'setting', 'page': 'online', 'title': title, 'hint': '', 'key': key,
                                        'widget': widget, 'only': only,
                                        'haystack': (search.fold(title), '', search.fold(search.WORDS.get(key, '')))})
        found = [e['key'] for e in fake.search('enemy health')]
        self.assertEqual(found[0], 'bbsc_difficulty')
        self.assertEqual(fake.search_where(fake.search('enemy health')[0]),
                         'Online play · only with SuperMedo Seamless co-op')
        self.assertEqual(fake.search('sign in')[0]['key'], 'button:signin_supermedo')  # the shown one first
        self.assertNotIn('online_npid', [e['key'] for e in fake.search('account')])  # hidden, no note: out
        side = {'kind': 'side', 'page': 'side', 'title': 'Expert view', 'hint': '', 'key': 'button:expert'}
        self.assertEqual(fake.search_where(side), 'Side bar')

    def test_index_only_marks_what_is_below_the_widgets(self):
        online = Online()
        online.index_only([online.seamless], 'only with SuperMedo Seamless co-op', 'online_pool')
        only = {e['key']: e.get('only') for e in online.search_settings}
        self.assertEqual(only['bbsc_difficulty'], ('only with SuperMedo Seamless co-op', 'online_pool'))
        self.assertIsNone(only['online_pool'])  # beside the hidden part, not in it
        self.assertIsNone(only['button:test_connection'])

    def test_the_jump_to_another_choice_marks_where_it_is_made(self):
        """A row of the pool not chosen: the jump marks "Play with" and leaves the pool as the player chose it;
        once that pool is chosen, the row itself."""
        online = Online()
        online.index_only([online.seamless], 'only with SuperMedo Seamless co-op', 'online_pool')
        entry = next(e for e in online.search_entries if e.get('key') == 'bbsc_difficulty')
        online.jump(entry)
        for call in online.root.later:
            call()
        self.assertEqual(online.shown_pages, ['online'])
        self.assertEqual(online.marked, [online.pool])
        self.assertEqual(online.scrolled, [online.pool])
        self.assertEqual(online.vars, {'online_pool': 'supermedo'})
        online.seamless.manager = 'grid'  # Seamless co-op chosen: its rows show
        online.root.later, online.marked = [], []
        online.jump(entry)
        for call in online.root.later:
            call()
        self.assertEqual(online.marked, [online.signin])

    def test_a_side_bar_button_is_only_marked(self):
        online = Online()
        button = Widget('pack')
        entry = {'kind': 'side', 'page': 'side', 'widget': button, 'mark': button, 'title': 'Expert view',
                 'key': 'button:expert'}
        online.jump(entry)
        for call in online.root.later:
            call()
        self.assertEqual((online.shown_pages, online.marked), ([], [button]))  # the expert view stays as it is

    def test_symptoms_find_the_fix_first(self):
        self.assertIn(self.first('ruckelt'), ('shader_cache', 'vram'))  # Help topics before the settings

    def test_words_point_at_things_that_exist(self):
        topics = set(help_.TOPICS)
        for key in search.WORDS:
            if key.startswith('topic:'):
                self.assertIn(key[6:], topics, key)

    def test_plural_and_inflected_words(self):
        self.assertEqual(search.near('details', 'model detail lod'), 'detail')
        self.assertEqual(search.near('einstellungen', 'deine einstellung'), 'einstellung')
        self.assertIsNone(search.near('ruckelt', 'control the enemy zuruck'))  # not inside another word
        self.assertIsNone(search.near('fps', 'fp'))  # short words stay as they are
        self.assertEqual(self.first('Bildraten'), 'fps_mode')

    def test_the_exact_word_beats_a_near_form(self):
        fake = Fake()
        fake.search_entries.append({'kind': 'setting', 'page': 'advanced', 'title': 'Check for updates', 'hint': '',
                                    'key': 'button:updates',
                                    'haystack': ('check for updates', '', search.fold(search.WORDS['button:updates']))})
        self.assertEqual(fake.search('updates')[0]['key'], 'button:updates')

    def test_hidden_widgets_are_left_out(self):
        page = Widget('pack')
        self.assertTrue(search.shown(Widget('grid', Widget('pack', page)), page))
        self.assertFalse(search.shown(Widget('grid', Widget('', page)), page))  # its frame is hidden
        self.assertTrue(search.shown(None, page))

    def test_players_find_buttons_and_the_setup(self):
        for word in ('verknupfung', 'steam', 'bericht', 'updates'):
            self.assertTrue(any(word in search.fold(words) for key, words in search.WORDS.items()
                                if key.startswith('button:')), word)
        self.assertIn('einrichtung', search.WORDS['action:setup'])
        self.assertIn('tastenbelegung', search.WORDS['page:controls'])
        self.assertIn('unsterblich', search.WORDS['cheat_no_death'])


if __name__ == '__main__':
    unittest.main()
