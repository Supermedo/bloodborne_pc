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

    def test_combos_round_trip(self):
        self.assertTrue(config.is_bindable('r2', 'lshift,leftbutton'))
        self.assertTrue(config.is_bindable('axis_left_y_minus', 'lalt, w'))
        self.assertFalse(config.is_bindable('axis_left_y_minus', 'lshift, leftbutton'))  # sticks: keys only
        self.assertFalse(config.is_bindable('cross', 'a, b, c, d'))
        self.assertFalse(config.is_bindable('cross', 'e, e'))
        self.assertFalse(config.is_bindable('cross', 'cross'))  # a controller line
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / 'input.ini'
            bindings, lines, *_rest = config.load_input_ini(path)
            first = {key: (values[0] if values else None) for key, values in bindings.items()}
            first['r2'] = 'lshift, leftbutton'
            config.save_input_ini(path, first, lines)
            text = path.read_text(encoding='utf-8')
            again, lines, *_rest = config.load_input_ini(path)
            # Saving again with the combo as the first slot replaces it in place.
            first = {key: (values[0] if values else None) for key, values in again.items()}
            first['r2'] = 'lctrl, lshift, leftbutton'
            config.save_input_ini(path, first, lines)
            third = path.read_text(encoding='utf-8')
        self.assertIn('r2 = lshift, leftbutton\n', text)
        self.assertNotIn('r2 = f\n', text)
        self.assertEqual(again['r2'][0], 'lshift, leftbutton')
        self.assertIn('r2 = lctrl, lshift, leftbutton\n', third)
        self.assertNotIn('r2 = lshift, leftbutton\n', third)
        self.assertIn('r2 = r2\n', third)  # the controller line is untouched

    def test_conflicts(self):
        conflicts = config.binding_conflicts({
            'r1': 'leftbutton', 'r2': 'lshift, leftbutton', 'circle': 'lshift',
            'cross': 'space', 'square': 'space', 'l1': 'lctrl, e', 'triangle': 'e, lctrl', 'l2': None})
        self.assertEqual(conflicts['circle'], ['r2'])       # Shift alone and inside Shift + click
        self.assertEqual(conflicts['r2'], ['circle'])
        self.assertNotIn('r1', conflicts)                   # a click inside a combo is fine
        self.assertEqual(conflicts['cross'], ['square'])    # the same key twice
        self.assertEqual(conflicts['l1'], ['triangle'])     # the same combo in another order
        self.assertNotIn('l2', conflicts)

    def test_mouse_invert_and_vertical_sensitivity(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / 'input.ini'
            _b, lines, _t, _r, mouse = config.load_input_ini(path)
            config.save_mouse_settings(path, mouse, lines)
            untouched = path.read_text(encoding='utf-8')
            mouse.update(invert_y=True, sensitivity_y=0.6)
            config.save_mouse_settings(path, mouse, config.load_input_ini(path)[1])
            changed = config.load_input_ini(path)[4]
            changed['invert_y'] = False
            config.save_mouse_settings(path, changed, config.load_input_ini(path)[1])
            text = path.read_text(encoding='utf-8')
        self.assertNotIn('mouse_invert', untouched)  # defaults add no lines
        self.assertTrue(changed is not None)
        self.assertEqual(text.count('mouse_invert_y'), 1)  # rewritten in place, not added again
        self.assertIn('mouse_invert_y = off\n', text)
        self.assertIn('mouse_sensitivity_y = 0.6\n', text)
        self.assertNotIn('mouse_invert_x', text)

    def test_camera_mode_and_direct_sensitivity(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / 'input.ini'
            path.write_text('\n'.join(config.apply_layout(config.DEFAULT_INPUT_INI.splitlines())) + '\n',
                            encoding='utf-8')
            _b, lines, _t, _r, mouse = config.load_input_ini(path)
            self.assertEqual(mouse['camera'], 'direct')  # the DS3 layout's
            mouse['camera_sensitivity'] = 1.75
            config.save_mouse_settings(path, mouse, lines)
            again = config.load_input_ini(path)[4]
            again['camera'] = 'stick'
            config.save_mouse_settings(path, again, config.load_input_ini(path)[1])
            text = path.read_text(encoding='utf-8')
        self.assertEqual(again['camera_sensitivity'], 1.75)
        self.assertIn('mouse_camera_sensitivity = 1.75\n', text)
        self.assertEqual(text.count('\nmouse_camera = '), 1)  # switched in place
        self.assertIn('mouse_camera = stick\n', text)

    def test_ds3_layout(self):
        text = '\n'.join(config.apply_layout(config.DEFAULT_INPUT_INI.splitlines())) + '\n'
        for kept in ('cross = cross', 'axis_left_x = axis_left_x', 'hotkey_reload_inputs = f8',
                     'mouse_movement_params = 0.5, 1, 0.125'):
            self.assertIn(kept + '\n', text)
        for gone in ('cross = space', 'circle = lshift', 'r1 = 3', 'circle = sidebuttonback',
                     '# mouse_to_joystick = right', '# leftjoystick_halfmode = lalt'):
            self.assertNotIn(gone + '\n', text)
        self.assertIn('r2 = lshift, leftbutton\n', text)
        # Esc opens the game menu like Tab; it is not Circle (a dodge outside the menus).
        self.assertIn('options = escape\n', text)
        self.assertNotIn('circle = escape\n', text)
        self.assertEqual(text.count('\nmouse_to_joystick = '), 1)
        self.assertNotIn('# mouse_to_joystick =', text)
        # Applying it twice gives the same file, and the layout has no clashes of its own.
        self.assertEqual(config.apply_layout(text.splitlines()), text.splitlines())
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / 'input.ini'
            path.write_text(text, encoding='utf-8')
            bindings = config.load_input_ini(path)[0]
        self.assertEqual(config.binding_conflicts({o: v[0] if v else None for o, v in bindings.items()}), {})
        self.assertEqual(config.slot_conflicts(bindings), {})
        self.assertEqual(bindings['l2'], ['lshift, rightbutton', 'lctrl'])  # two slots
        self.assertEqual(bindings['leftjoystick_halfmode'], ['lalt'])

    def test_several_bindings_per_action(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / 'input.ini'
            bindings, lines, *_rest = config.load_input_ini(path)
            self.assertEqual(bindings['circle'], ['lshift', 'sidebuttonback'])
            # All of an action's keyboard and mouse bindings, as the Controls page edits them.
            config.save_input_ini(path, {'circle': ['space', 'lshift, leftbutton', 'mousewheeldown'],
                                         'r3': [], 'leftjoystick_halfmode': ['lalt', 'middlebutton']}, lines)
            text = path.read_text(encoding='utf-8')
            again = config.load_input_ini(path)[0]
        self.assertEqual(again['circle'], ['space', 'lshift, leftbutton', 'mousewheeldown'])
        self.assertEqual(again['r3'], [])
        self.assertEqual(again['leftjoystick_halfmode'], ['lalt', 'middlebutton'])
        self.assertEqual(again['cross'], ['space'])  # not edited: kept
        for kept in ('circle = circle', 'r3 = r3', 'cross = cross', '# leftjoystick_halfmode = lalt'):
            self.assertIn(kept + '\n', text)
        for gone in ('circle = lshift', 'circle = sidebuttonback', 'r3 = q', 'r3 = c'):
            self.assertNotIn(gone + '\n', text)
        # Written where the first one was: the keyboard section, before the controller's line.
        self.assertLess(text.index('circle = mousewheeldown'), text.index('circle = circle'))
        self.assertLess(text.index('circle = space'), text.index('square = e'))

    def test_slot_conflicts(self):
        conflicts = config.slot_conflicts({
            'circle': ['space', 'lshift'], 'r2': [None, 'lshift, leftbutton'], 'cross': ['e', 'space'],
            'square': ['g', 'g']})
        self.assertEqual(conflicts['circle'], {0: ['cross'], 1: ['r2']})
        self.assertEqual(conflicts['r2'], {1: ['circle']})
        self.assertEqual(conflicts['cross'], {1: ['circle']})
        self.assertNotIn('square', conflicts)  # twice on one action is no clash

    def test_display_names(self):
        self.assertEqual(config.display_name('lshift'), 'Shift')
        self.assertEqual(config.display_name('leftbutton'), 'Left click')
        self.assertEqual(config.display_name('leftbutton', russian=True), 'Левый клик')
        self.assertEqual(config.display_name('e'), 'E')
        self.assertEqual(config.display_name('f5'), 'F5')
        self.assertEqual(config.display_name('backslash'), '\\')
        for name in config.KEY_NAMES + list(config.MOUSE_BUTTON_NAMES):
            self.assertTrue(config.display_name(name))

    def test_every_output_has_one_row(self):
        rows = [o for _group, actions in config.ACTION_GROUPS for o, _label in actions]
        self.assertEqual(sorted(rows), sorted(config.KEYBOARD_OUTPUTS))
        # Three slots and the controller's line fit the game's four bindings per output.
        header = (ROOT / 'src/runtime_input_config.h').read_text(encoding='utf-8')
        self.assertIn(f'#define INPUT_MAX_BINDINGS_PER_OUTPUT {config.SLOTS + 1}', header)

    def test_shifted_keysyms(self):
        self.assertEqual(config.keysym_to_name('exclam'), '1')
        self.assertEqual(config.keysym_to_name('E'), 'e')
        self.assertEqual(config.keysym_to_name('Shift_L'), 'lshift')


if __name__ == '__main__':
    unittest.main()
