from paths import ROOT
import ast
import importlib.util
from pathlib import Path
import re
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('bbport_input_config', ROOT / 'launcher/bbport_input_config.py')
config = importlib.util.module_from_spec(spec)
spec.loader.exec_module(config)


def game_default_text():
    """input_config_default_text() of src/runtime_input_config.c: its sections in order."""
    source = (ROOT / 'src/runtime_input_config.c').read_text(encoding='utf-8')
    sections = {}
    for name, body in re.findall(r'#define (DEFAULT_\w+) \\\n((?:".*"(?: \\)?\n)+)', source):
        sections[name] = ''.join(ast.literal_eval(line.rstrip(' \\')) for line in body.splitlines())
    order = re.search(r'input_config_default_text\(void\) \{\s*return ([^;]+);', source).group(1).split()
    return ''.join(sections[name] for name in order)


class InputDefaultsTests(unittest.TestCase):
    def test_launcher_default_matches_the_game(self):
        self.assertEqual(config.DEFAULT_INPUT_INI, game_default_text())

    def test_saving_before_the_game_made_the_file_keeps_the_controller(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / 'input.ini'
            bindings, lines, _toggle, _reload, mouse = config.load_input_ini(path)
            self.assertEqual(bindings['cross'], ['space'])
            first = {key: (values[0] if values else None) for key, values in bindings.items()}
            first['cross'] = 'g'
            config.save_input_ini(path, first, lines)
            mouse['enabled'] = True
            config.save_mouse_settings(path, mouse, config.load_input_ini(path)[1])
            text = path.read_text(encoding='utf-8')
        for line in ('cross = g', 'cross = cross', 'axis_left_x = axis_left_x', 'circle = lshift',
                     'mouse_to_joystick = right'):
            self.assertIn(line + '\n', text)
        self.assertNotIn('cross = space\n', text)

    def test_wheel_binds_to_a_button(self):
        self.assertEqual(config.wheel_to_name(120), 'mousewheelup')
        self.assertEqual(config.wheel_to_name(-240), 'mousewheeldown')
        self.assertEqual(config.wheel_to_name(120, horizontal=True), 'mousewheelright')
        self.assertEqual(config.wheel_to_name(-120, horizontal=True), 'mousewheelleft')
        self.assertIsNone(config.wheel_to_name(0))
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / 'input.ini'
            bindings, lines, *_rest = config.load_input_ini(path)
            first = {key: (values[0] if values else None) for key, values in bindings.items()}
            first['l1'] = 'mousewheelup'
            config.save_input_ini(path, first, lines)
            again = config.load_input_ini(path)[0]
        self.assertEqual(again['l1'][0], 'mousewheelup')
        # The game's parser knows the same names.
        source = (ROOT / 'src/runtime_input_config.c').read_text(encoding='utf-8')
        for name in config.MOUSE_WHEEL_NAMES:
            self.assertIn(f'"{name}"', source)


if __name__ == '__main__':
    unittest.main()
