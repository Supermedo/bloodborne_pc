"""Important launcher functions sit on a page and the search finds them; every search entry for a button has its
button, and only one (a button on two pages is found on the one of both views). The shader cache button had been
left in Help's black screen topic alone after the redesign, and its search words pointed at nothing; signing in,
importing, restoring a backup and the expert view were not in the search at all."""
from paths import ROOT
from collections import Counter
import ast
import sys
import unittest

sys.path.insert(0, str(ROOT / 'launcher'))
import bbport_search as search  # noqa: E402

# The action (a launcher method) -> its search entry. An action players look for: on a page, findable.
IMPORTANT = {
    'clear_cache': 'button:shader_cache',
    'save_report': 'button:report',
    'open_respec': 'button:respec',
    'choose_version': 'button:versions',
    'download_fsr4': 'button:fsr4',
    'download_fsr411': 'button:fsr4vk',
    'shortcut': 'button:shortcut',
    'add_to_steam': 'button:steam',
    'backup_now': 'button:backup',
    'open_bug_benchmark': 'button:benchmark',
    'check_game_files': 'button:game_files',
    'check_update_now': 'button:updates',
    'sign_in_discord': 'button:signin_supermedo',  # the Online page's own functions, not methods
    'create_account': 'button:create_account',
    'import_save': 'button:import',
    'restore_backup': 'button:restore',
    'toggle_mode': 'button:expert',
    'open_forum': 'button:forum',
    'import_trophies': 'button:trophies',
}


def sources():
    return [p for p in sorted((ROOT / 'launcher').glob('bbport_*.py'))
            if not p.name.startswith('bbport_lang') and p.name != 'bbport_launcher.py']


def scan():
    """(the actions page buttons run: methods, and a page's own functions given as command=, the search keys
    index_entry registers with how often)."""
    commands, keys = set(), Counter()
    for path in sources():
        for node in ast.walk(ast.parse(path.read_text(encoding='utf-8'))):
            if not isinstance(node, ast.Call):
                continue
            name = node.func.attr if isinstance(node.func, ast.Attribute) else getattr(node.func, 'id', '')
            keywords = {k.arg: k.value for k in node.keywords}
            if name in ('Button', 'link_row') or 'command' in keywords:
                for part in [keywords.get('command'), *node.args]:
                    for inner in ast.walk(part) if part is not None else ():
                        if isinstance(inner, ast.Attribute) and isinstance(inner.value, ast.Name) \
                                and inner.value.id in ('self', 'app'):
                            commands.add(inner.attr)
                        elif isinstance(inner, ast.Name) and part is keywords.get('command'):
                            commands.add(inner.id)
            if name == 'index_entry':
                key = keywords.get('key') or (node.args[3] if len(node.args) > 3 else None)
                if isinstance(key, ast.Constant):
                    keys[key.value] += 1
    return commands, keys


class FindableTests(unittest.TestCase):
    def test_important_actions_are_on_a_page_and_found(self):
        commands, keys = scan()
        for action, key in IMPORTANT.items():
            with self.subTest(action=action):
                self.assertIn(action, commands, f'{action} has no button on a page (a Help topic is not enough)')
                self.assertIn(key, keys, f'{action}: nothing registers {key} for the search')
                self.assertIn(key, search.WORDS, f'{action}: {key} has no players\' words')

    def test_every_button_entry_has_its_button(self):
        _commands, keys = scan()
        dead = sorted(key for key in search.WORDS if key.startswith('button:') and key not in keys)
        self.assertEqual(dead, [], 'search words for buttons that no longer exist')

    def test_one_entry_per_button(self):
        _commands, keys = scan()
        self.assertEqual(sorted(key for key, count in keys.items() if key.startswith('button:') and count > 1), [])

    def test_the_upkeep_is_on_help(self):
        """Clear the shader cache, Check for updates and Other versions were on Advanced alone (the expert view):
        Help has them too, and the search leads there, a page of both views."""
        help_page = (ROOT / 'launcher' / 'bbport_help.py').read_text(encoding='utf-8')
        build = help_page[help_page.index('    def build_help('):help_page.index('    def topic_card(')]
        for key in ('button:shader_cache', 'button:updates', 'button:versions'):
            with self.subTest(key=key):
                self.assertIn(f"key='{key}'", build)
        self.assertEqual(build.count("_('Repeat the setup')"), 1)  # the link; the card had it a second time


if __name__ == '__main__':
    unittest.main()
