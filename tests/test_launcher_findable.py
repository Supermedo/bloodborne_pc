"""Important launcher functions sit on a page and the search finds them; every search entry for a button has its
button. The shader cache button had been left in Help's black screen topic alone after the redesign, and its
search words pointed at nothing."""
from paths import ROOT
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
}


def sources():
    return [p for p in sorted((ROOT / 'launcher').glob('bbport_*.py'))
            if not p.name.startswith('bbport_lang') and p.name != 'bbport_launcher.py']


def scan():
    """(the actions page buttons run, the search keys index_entry registers)."""
    commands, keys = set(), set()
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
            if name == 'index_entry':
                key = keywords.get('key') or (node.args[3] if len(node.args) > 3 else None)
                if isinstance(key, ast.Constant):
                    keys.add(key.value)
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


if __name__ == '__main__':
    unittest.main()
