#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""input.ini read/write for the launchers (shadPS4 input config syntax; see docs/INPUT.md and
src/runtime_input_config.c, the authoritative parser this mirrors). Scope: keyboard and mouse-
button bindings for the plain button/d-pad outputs (BUTTON_OUTPUTS) and the stick half-axes
(STICK_OUTPUTS, keyboard only -- a mouse button is a press, not an axis), which is what the
launcher's remap page edits, up to SLOTS per action, plus walk (leftjoystick_halfmode).
Controller bindings are preserved untouched (round-tripped as unknown lines by save_input_ini),
not edited here.
"""
import os
from pathlib import Path

# Keyboard-only outputs the remap page edits. (action key in the .ini, Tk display order).
BUTTON_OUTPUTS = ['cross', 'circle', 'square', 'triangle', 'l1', 'r1', 'l2', 'r2', 'l3', 'r3',
                   'options', 'pad_up', 'pad_down', 'pad_left', 'pad_right',
                   'touchpad_left', 'touchpad_right']
STICK_OUTPUTS = ['axis_left_x_minus', 'axis_left_x_plus', 'axis_left_y_minus', 'axis_left_y_plus',
                  'axis_right_x_minus', 'axis_right_x_plus', 'axis_right_y_minus', 'axis_right_y_plus']
# Held like a button, read as a modifier of the left stick (walk).
HOLD_OUTPUTS = ['leftjoystick_halfmode']
KEYBOARD_OUTPUTS = BUTTON_OUTPUTS + STICK_OUTPUTS + HOLD_OUTPUTS
# Bindings the Controls page shows per action. The game keeps up to four per output
# (INPUT_MAX_BINDINGS_PER_OUTPUT): three keyboard and mouse ones and the controller's.
SLOTS = 3

# (output, (english label, russian label)) for the remap page, in display order. Stick halves
# are grouped by direction so the UI can show one row per direction instead of exposing the
# _minus/_plus split (the UI combines a pair into "Move left"/"Move right" etc.).
OUTPUT_LABELS = {
    'cross': ('Cross', 'Крест'), 'circle': ('Circle', 'Круг'),
    'square': ('Square', 'Квадрат'), 'triangle': ('Triangle', 'Треугольник'),
    'l1': ('L1', 'L1'), 'r1': ('R1', 'R1'), 'l2': ('L2', 'L2'), 'r2': ('R2', 'R2'),
    'l3': ('L3', 'L3'), 'r3': ('R3 (lock-on)', 'R3 (захват цели)'),
    'options': ('Options', 'Options'),
    'pad_up': ('D-pad up', 'Крестовина вверх'), 'pad_down': ('D-pad down', 'Крестовина вниз'),
    'pad_left': ('D-pad left', 'Крестовина влево'), 'pad_right': ('D-pad right', 'Крестовина вправо'),
    'touchpad_left': ('Touchpad (left click)', 'Тачпад (левый клик)'),
    'touchpad_right': ('Touchpad (right click)', 'Тачпад (правый клик)'),
    'axis_left_x_minus': ('Move left', 'Движение влево'), 'axis_left_x_plus': ('Move right', 'Движение вправо'),
    'axis_left_y_minus': ('Move forward', 'Движение вперёд'), 'axis_left_y_plus': ('Move back', 'Движение назад'),
    'axis_right_x_minus': ('Camera left', 'Камера влево'), 'axis_right_x_plus': ('Camera right', 'Камера вправо'),
    'axis_right_y_minus': ('Camera up', 'Камера вверх'), 'axis_right_y_plus': ('Camera down', 'Камера вниз'),
    'leftjoystick_halfmode': ('Walk', 'Шаг'),
}

# The Controls page: (group title, [(output, (english, russian) action label)]), in display order.
ACTION_GROUPS = [
    (('Movement and camera', 'Движение и камера'), [
        ('axis_left_y_minus', ('Move forward', 'Движение вперёд')),
        ('axis_left_y_plus', ('Move back', 'Движение назад')),
        ('axis_left_x_minus', ('Move left', 'Движение влево')),
        ('axis_left_x_plus', ('Move right', 'Движение вправо')),
        ('leftjoystick_halfmode', ('Walk (hold)', 'Шаг (удерживать)')),
        ('circle', ('Dodge / dash (hold); back in menus (Circle)',
                    'Уклонение / бег (удерживать); назад в меню (Круг)')),
        ('l3', ('Jump while dashing (L3)', 'Прыжок во время бега (L3)')),
        ('r3', ('Lock on / reset camera (R3)', 'Захват цели / сброс камеры (R3)')),
        ('axis_right_y_minus', ('Camera up', 'Камера вверх')),
        ('axis_right_y_plus', ('Camera down', 'Камера вниз')),
        ('axis_right_x_minus', ('Camera left', 'Камера влево')),
        ('axis_right_x_plus', ('Camera right', 'Камера вправо')),
    ]),
    (('Combat', 'Бой'), [
        ('r1', ('Attack (R1)', 'Атака (R1)')),
        ('r2', ('Strong attack (R2)', 'Сильная атака (R2)')),
        ('l1', ('Transform weapon (L1)', 'Трансформация оружия (L1)')),
        ('l2', ('Firearm (L2)', 'Огнестрельное оружие (L2)')),
    ]),
    (('Items and menus', 'Предметы и меню'), [
        ('cross', ('Interact; confirm in menus (Cross)', 'Действие; подтвердить в меню (Крест)')),
        ('square', ('Use quick item (Square)', 'Использовать предмет (Квадрат)')),
        ('triangle', ('Blood vial (Triangle)', 'Флакон крови (Треугольник)')),
        ('pad_up', ('D-pad up', 'Крестовина вверх')),
        ('pad_down', ('Switch quick item (d-pad down)', 'Сменить предмет (крестовина вниз)')),
        ('pad_left', ('Switch left hand weapon (d-pad left)', 'Сменить оружие в левой руке (крестовина влево)')),
        ('pad_right', ('Switch right hand weapon (d-pad right)', 'Сменить оружие в правой руке (крестовина вправо)')),
        ('options', ('Game menu (Options)', 'Меню игры (Options)')),
        ('touchpad_left', ('Gestures (touchpad)', 'Жесты (тачпад)')),
        ('touchpad_right', ('Right half of the touchpad (debug menu)', 'Правая половина тачпада (меню отладки)')),
    ]),
]

# Names runtime_input_config.c's kKeyNames accepts (src/runtime_input_config.c); kept in the same
# order/groups for an easy side-by-side diff when that table changes.
KEY_NAMES = (
    [chr(c) for c in range(ord('a'), ord('z') + 1)] + [str(d) for d in range(10)] +
    [f'f{n}' for n in range(1, 13)] +
    ['escape', 'backspace', 'delete', 'insert', 'home', 'end', 'pgup', 'pgdown',
     'tab', 'capslock', 'enter', 'lshift', 'rshift', 'lctrl', 'rctrl', 'lalt', 'ralt',
     'lmeta', 'rmeta', 'lwin', 'rwin', 'space', 'up', 'down', 'left', 'right'] +
    [f'kp{d}' for d in range(10)] +
    ['kpperiod', 'kpcomma', 'kpslash', 'kpasterisk', 'kpminus', 'kpplus', 'kpequals', 'kpenter',
     'grave', 'minus', 'equals', 'lbracket', 'rbracket', 'backslash', 'semicolon', 'apostrophe',
     'comma', 'period', 'slash', 'printscreen', 'scrolllock', 'pausebreak']
)
KEY_NAMES_SET = set(KEY_NAMES)

# Names runtime_input_config.c's kMouseButtonNames and kMouseWheelNames accept: what a
# BUTTON_OUTPUTS slot can take from the mouse. A wheel step is a short press (33 ms in the game).
MOUSE_WHEEL_NAMES = ('mousewheelup', 'mousewheeldown', 'mousewheelleft', 'mousewheelright')
MOUSE_BUTTON_NAMES = ('leftbutton', 'middlebutton', 'rightbutton', 'sidebuttonback', 'sidebuttonforward',
                      *MOUSE_WHEEL_NAMES)
MOUSE_BUTTON_NAMES_SET = set(MOUSE_BUTTON_NAMES)

# What a BUTTON_OUTPUTS/STICK_OUTPUTS line's value may be for load_input_ini/save_input_ini to
# treat it as "this editor's own binding slot" rather than an untouched controller/other line.
# Sticks stay keyboard-only (KEY_NAMES_SET); buttons additionally accept a mouse button name.
BINDABLE_NAMES_SET = KEY_NAMES_SET | MOUSE_BUTTON_NAMES_SET

# Combos (src/runtime_input_config.c): "output = a, b, c", up to three inputs held together.
# Shift, Ctrl and Alt start one on the Controls page: pressed alone they wait for the next input.
MAX_COMBO = 3
MODIFIER_NAMES = ('lshift', 'rshift', 'lctrl', 'rctrl', 'lalt', 'ralt')


# How a slot shows an input: (english, russian), or one text for both. Keys not listed show as
# their name in capitals (E, F5).
DISPLAY_NAMES = {
    'lshift': 'Shift', 'rshift': ('Right Shift', 'Правый Shift'), 'lctrl': 'Ctrl',
    'rctrl': ('Right Ctrl', 'Правый Ctrl'), 'lalt': ('Left Alt', 'Левый Alt'),
    'ralt': ('Right Alt', 'Правый Alt'), 'lwin': 'Win', 'rwin': ('Right Win', 'Правый Win'),
    'lmeta': 'Meta', 'rmeta': ('Right Meta', 'Правый Meta'),
    'escape': 'Esc', 'backspace': 'Backspace', 'delete': 'Delete', 'insert': 'Insert', 'home': 'Home',
    'end': 'End', 'pgup': 'Page Up', 'pgdown': 'Page Down', 'tab': 'Tab', 'capslock': 'Caps Lock',
    'enter': 'Enter', 'space': ('Space', 'Пробел'), 'up': '↑', 'down': '↓', 'left': '←', 'right': '→',
    'kpperiod': 'Num .', 'kpcomma': 'Num ,', 'kpslash': 'Num /', 'kpasterisk': 'Num *',
    'kpminus': 'Num -', 'kpplus': 'Num +', 'kpequals': 'Num =', 'kpenter': 'Num Enter',
    'grave': '`', 'minus': '-', 'equals': '=', 'lbracket': '[', 'rbracket': ']', 'backslash': '\\',
    'semicolon': ';', 'apostrophe': "'", 'comma': ',', 'period': '.', 'slash': '/',
    'printscreen': 'Print Screen', 'scrolllock': 'Scroll Lock', 'pausebreak': 'Pause',
    'leftbutton': ('Left click', 'Левый клик'), 'rightbutton': ('Right click', 'Правый клик'),
    'middlebutton': ('Wheel click', 'Нажатие колеса'), 'sidebuttonback': ('Mouse back', 'Мышь назад'),
    'sidebuttonforward': ('Mouse forward', 'Мышь вперёд'),
    'mousewheelup': ('Wheel up', 'Колесо вверх'), 'mousewheeldown': ('Wheel down', 'Колесо вниз'),
    'mousewheelleft': ('Wheel left', 'Колесо влево'), 'mousewheelright': ('Wheel right', 'Колесо вправо'),
    **{f'kp{d}': f'Num {d}' for d in range(10)},
}


def display_name(part, russian=False):
    """'lshift' -> 'Shift', 'leftbutton' -> 'Left click', 'e' -> 'E'."""
    name = DISPLAY_NAMES.get(part, part.upper())
    if isinstance(name, tuple):
        return name[1] if russian else name[0]
    return name


def binding_parts(value):
    """The inputs of a binding value: ['lshift', 'leftbutton'] for 'lshift, leftbutton'."""
    return [p.strip() for p in value.split(',')]


def is_bindable(output, value):
    """Whether `value` is a binding this editor shows and may replace for `output`: one key (or,
    for a BUTTON_OUTPUTS entry, a mouse button or wheel step), or a combo of up to MAX_COMBO of
    them, each at most once. A controller line ("cross = cross") never is one."""
    parts = binding_parts(value)
    if not 1 <= len(parts) <= MAX_COMBO or len(set(parts)) != len(parts):
        return False
    allowed = KEY_NAMES_SET | (MOUSE_BUTTON_NAMES_SET if output not in STICK_OUTPUTS else set())
    return all(p in allowed for p in parts)


def format_binding(value):
    """The file's spelling of a binding value: 'lshift, leftbutton'."""
    return ', '.join(binding_parts(value))


def _clash(pa, pb):
    same = pa == pb or set(pa) == set(pb)
    inside = (len(pa) == 1 and pa[0] in MODIFIER_NAMES and pa[0] in pb) or \
             (len(pb) == 1 and pb[0] in MODIFIER_NAMES and pb[0] in pa)
    return same or inside


def slot_conflicts(bindings):
    """{output: {slot index: sorted other outputs}} for every binding that clashes with another
    output's, from {output: [value or None, ...]}. Two equal bindings fire both actions. A
    modifier (Shift, Ctrl, Alt) that is a binding of its own and also starts a combo is pressed
    on its way into the combo (Shift bound to Circle, the dodge, and Shift + left click to R2),
    which the game sees. A plain key or button inside a combo is not a clash: the combo takes it
    while held. Bindings of the same output never clash with each other."""
    conflicts = {}
    items = [(o, i, binding_parts(v)) for o, values in bindings.items()
             for i, v in enumerate(values or []) if v]
    for n, (a, ia, pa) in enumerate(items):
        for b, ib, pb in items[n + 1:]:
            if a != b and _clash(pa, pb):
                conflicts.setdefault(a, {}).setdefault(ia, set()).add(b)
                conflicts.setdefault(b, {}).setdefault(ib, set()).add(a)
    return {o: {i: sorted(others) for i, others in slots.items()} for o, slots in conflicts.items()}


def binding_conflicts(bindings):
    """{output: sorted other outputs} for every output whose binding clashes with another's,
    from {output: value or None} (one binding each; slot_conflicts takes several)."""
    conflicts = slot_conflicts({o: [v] for o, v in bindings.items()})
    return {o: sorted({b for others in slots.values() for b in others}) for o, slots in conflicts.items()}


# Reserved regardless of the file (HOT-001/HOT-002): the menu (Insert), BB_PAD_RECORD, and
# whichever key currently toggles the mouse or reloads input.ini. The last two are read from the
# file itself (hotkey_toggle_mouse_to_joystick / hotkey_reload_inputs) by reserved_keys() below.
# Escape is bindable: the overlay only uses it to close an open menu (src/runtime_input_config.c).
ALWAYS_RESERVED = {'insert', 'f9'}

# Tk's event.keysym (X11/Tk names) -> our key name. Only keys with a different spelling are
# listed; keysyms not in this table are tried lowercased as-is (covers a..z, 0..9, f1..f12).
TK_KEYSYM_TO_NAME = {
    'Escape': 'escape', 'BackSpace': 'backspace', 'Delete': 'delete', 'Insert': 'insert',
    'Home': 'home', 'End': 'end', 'Prior': 'pgup', 'Next': 'pgdown',
    'Tab': 'tab', 'Caps_Lock': 'capslock', 'Return': 'enter',
    'Shift_L': 'lshift', 'Shift_R': 'rshift', 'Control_L': 'lctrl', 'Control_R': 'rctrl',
    'Alt_L': 'lalt', 'Alt_R': 'ralt', 'Super_L': 'lwin', 'Super_R': 'rwin',
    'space': 'space', 'Up': 'up', 'Down': 'down', 'Left': 'left', 'Right': 'right',
    'KP_0': 'kp0', 'KP_1': 'kp1', 'KP_2': 'kp2', 'KP_3': 'kp3', 'KP_4': 'kp4',
    'KP_5': 'kp5', 'KP_6': 'kp6', 'KP_7': 'kp7', 'KP_8': 'kp8', 'KP_9': 'kp9',
    'KP_Decimal': 'kpperiod', 'KP_Separator': 'kpcomma', 'KP_Divide': 'kpslash',
    'KP_Multiply': 'kpasterisk', 'KP_Subtract': 'kpminus', 'KP_Add': 'kpplus',
    'KP_Equal': 'kpequals', 'KP_Enter': 'kpenter',
    'grave': 'grave', 'minus': 'minus', 'equal': 'equals',
    'bracketleft': 'lbracket', 'bracketright': 'rbracket', 'backslash': 'backslash',
    'semicolon': 'semicolon', 'apostrophe': 'apostrophe', 'quoteright': 'apostrophe',
    'comma': 'comma', 'period': 'period', 'slash': 'slash',
    'Print': 'printscreen', 'Scroll_Lock': 'scrolllock', 'Pause': 'pausebreak',
}


# With Shift held (a Shift + key combination), Tk reports the shifted character's keysym: the
# key that types it on a US layout.
TK_SHIFTED_KEYSYM_TO_NAME = {
    'exclam': '1', 'at': '2', 'numbersign': '3', 'dollar': '4', 'percent': '5',
    'asciicircum': '6', 'ampersand': '7', 'asterisk': '8', 'parenleft': '9', 'parenright': '0',
    'underscore': 'minus', 'plus': 'equals', 'braceleft': 'lbracket', 'braceright': 'rbracket',
    'bar': 'backslash', 'colon': 'semicolon', 'quotedbl': 'apostrophe', 'less': 'comma',
    'greater': 'period', 'question': 'slash', 'asciitilde': 'grave',
}


def keysym_to_name(keysym):
    """Tk's event.keysym -> our key name, or None if this key has no equivalent (e.g. a dead
    key, a media key, or a layout-specific glyph outside KEY_NAMES)."""
    name = TK_KEYSYM_TO_NAME.get(keysym) or TK_SHIFTED_KEYSYM_TO_NAME.get(keysym) or keysym.lower()
    return name if name in KEY_NAMES_SET else None


# Tk's event.num for a <ButtonPress> -> our mouse button name. 1/2/3 are the standard left/
# middle/right buttons on every platform Tk supports; 4/5 are the side (back/forward) buttons on
# Windows specifically (Tk's core-8-6-branch gained WM_XBUTTONDOWN/UP support in 2019, reusing
# the Aqua button numbering rather than X11's 8/9, since X11 uses 4/5 for the scroll wheel and
# Windows delivers wheel scroll separately via <MouseWheel>/event.delta, so there is no clash on
# this platform). Requires a reasonably current Tcl/Tk (8.6.12+, which is what Python 3.10+'s
# official Windows installers bundle); an older frozen interpreter would simply never fire a
# <ButtonPress> for the side buttons, same as if the button didn't exist.
TK_BUTTON_NUM_TO_NAME = {1: 'leftbutton', 2: 'middlebutton', 3: 'rightbutton',
                          4: 'sidebuttonback', 5: 'sidebuttonforward'}


def button_num_to_name(num):
    """Tk's event.num from a <ButtonPress> -> our mouse button name, or None for a button number
    Tk reports that bbport has no name for."""
    return TK_BUTTON_NUM_TO_NAME.get(num)


def wheel_to_name(delta, horizontal=False):
    """Tk's <MouseWheel> event.delta -> our wheel name. Windows reports a tilt of the wheel
    (horizontal scrolling) as <Shift-MouseWheel>, positive to the right."""
    if not delta:
        return None
    if horizontal:
        return 'mousewheelright' if delta > 0 else 'mousewheelleft'
    return 'mousewheelup' if delta > 0 else 'mousewheeldown'


def input_ini_path(bbport_ini_path):
    """Mirrors src/runtime_input_config.c's input_config_path(): BB_INPUT_CONFIG if set and
    non-empty, else input.ini next to bbport_ini_path (itself BB_CONFIG-derived)."""
    env = os.environ.get('BB_INPUT_CONFIG')
    if env:
        return Path(env)
    return Path(bbport_ini_path).parent / 'input.ini'


# The file the game writes when there is none (src/runtime_input_config.c,
# input_config_default_text; tests/test_input_defaults.py checks that they match). The
# launcher starts from it too, or saving the Controls page before the first game start would
# write only the edited lines, and the controller would then do nothing.
DEFAULT_INPUT_INI = '''\
# bbport input (shadPS4 syntax: output = input, one input per line).
# A shadPS4 input config (user/input_config/CUSA03173.ini) can be copied over this file.
# F7 toggles the mouse, F8 reloads this file.

# Keyboard
cross = space
circle = lshift
square = e
triangle = v
l1 = 1
r1 = 3
l2 = r
r2 = f
l3 = z
r3 = q
r3 = c
options = enter
pad_up = i
pad_down = k
pad_left = j
pad_right = l
touchpad_left = tab
touchpad_right = backspace

axis_left_x_minus = a
axis_left_x_plus = d
axis_left_y_minus = w
axis_left_y_plus = s
axis_right_x_minus = left
axis_right_x_plus = right
axis_right_y_minus = up
axis_right_y_plus = down

# Hold to halve the left stick (walk): uncomment and pick a key
# leftjoystick_halfmode = lalt

# Mouse (uncomment mouse_to_joystick to turn it on; buttons work only while captured)
# mouse_to_joystick = right
mouse_movement_params = 0.5, 1, 0.125
r1 = leftbutton
r2 = rightbutton
circle = sidebuttonback
square = sidebuttonforward

# Controller
cross = cross
circle = circle
square = square
triangle = triangle
l1 = l1
r1 = r1
l2 = l2
r2 = r2
l3 = l3
r3 = r3
options = options
touchpad_left = back
pad_up = pad_up
pad_down = pad_down
pad_left = pad_left
pad_right = pad_right
axis_left_x = axis_left_x
axis_left_y = axis_left_y
axis_right_x = axis_right_x
axis_right_y = axis_right_y

# Hotkeys
hotkey_toggle_mouse_to_joystick = f7
hotkey_reload_inputs = f8
'''

# A keyboard and mouse layout like Dark Souls III's (after Ryan Sousa's keyboard and mouse
# fork): Shift is only a modifier, so Shift + click never passes through a dodge.
DS3_LAYOUT = '''\
# Keyboard and mouse: Dark Souls III layout (launcher, Controls page)
axis_left_x_minus = a
axis_left_x_plus = d
axis_left_y_minus = w
axis_left_y_plus = s
leftjoystick_halfmode = lalt
axis_right_x_minus = j
axis_right_x_plus = l
axis_right_y_minus = i
axis_right_y_plus = k
mouse_to_joystick = right
mouse_camera = direct
circle = space
l3 = c
r3 = q
r3 = middlebutton
r1 = leftbutton
r2 = lshift, leftbutton
l1 = rightbutton
l2 = lshift, rightbutton
l2 = lctrl
cross = e
cross = enter
square = r
triangle = f
pad_up = up
pad_up = mousewheelup
pad_down = down
pad_down = mousewheeldown
pad_left = left
pad_left = lshift, mousewheelup
pad_right = right
pad_right = lshift, mousewheeldown
options = tab
options = escape
touchpad_left = g
touchpad_right = backspace
'''
# Lines a layout replaces besides the keyboard and mouse bindings (commented-out ones too).
LAYOUT_SETTINGS = ('leftjoystick_halfmode', 'rightjoystick_halfmode', 'mouse_to_joystick', 'mouse_camera')


def apply_layout(lines, layout=DS3_LAYOUT):
    """`lines` with every keyboard and mouse binding, and the LAYOUT_SETTINGS lines, replaced by
    `layout`: controller lines, the other settings and comments stay."""
    out = []
    header = layout.splitlines()[0]
    for raw in lines:
        if raw.strip() == header:
            continue  # applied before: its header comes again with the layout
        line = ''.join(raw.split())
        commented = line.startswith('#')
        body = line.lstrip('#').split('#', 1)[0]
        if '=' in body:
            output, value = (p.split(':')[0] for p in body.split('=', 1))
            if output in LAYOUT_SETTINGS:
                continue
            if not commented and output in KEYBOARD_OUTPUTS and is_bindable(output, value):
                continue
        out.append(raw)
    while out and not out[-1].strip():
        out.pop()
    return out + [''] + layout.splitlines()


# shadPS4/bbport defaults when mouse_movement_params is absent (spec section 4.2 / 4.3).
MOUSE_SETTINGS_DEFAULTS = {'enabled': False, 'stick': 'right',
                            'deadzone_offset': 0.5, 'speed': 1.0, 'speed_offset': 0.125,
                            'invert_x': False, 'invert_y': False, 'sensitivity_y': 1.0,
                            'camera': 'stick', 'camera_sensitivity': 1.0}
# The one-value mouse lines (src/runtime_input_config.c), written only when not the default.
MOUSE_SIMPLE_LINES = {'invert_x': 'mouse_invert_x', 'invert_y': 'mouse_invert_y',
                      'sensitivity_y': 'mouse_sensitivity_y', 'camera': 'mouse_camera',
                      'camera_sensitivity': 'mouse_camera_sensitivity'}


def _parse_mouse_line(output, value, settings):
    """Shared by the commented and uncommented cases below: mouse_to_joystick and
    mouse_movement_params, whichever `output`/`value` this particular line parsed to."""
    if output == 'mouse_to_joystick' and value in ('left', 'right'):
        settings['enabled'] = True
        settings['stick'] = value
        return True
    if output in ('mouse_invert_x', 'mouse_invert_y') and value in ('on', 'off'):
        settings['invert_x' if output == 'mouse_invert_x' else 'invert_y'] = value == 'on'
        return True
    if output == 'mouse_camera' and value in ('direct', 'stick'):
        settings['camera'] = value
        return True
    for key, name, lo, hi in (('sensitivity_y', 'mouse_sensitivity_y', 0.1, 10.0),
                              ('camera_sensitivity', 'mouse_camera_sensitivity', 0.05, 20.0)):
        if output == name:
            try:
                sensitivity = float(value)
            except ValueError:
                return False
            if lo <= sensitivity <= hi:
                settings[key] = sensitivity
                return True
            return False
    if output == 'mouse_movement_params':
        parts = value.split(',')
        if len(parts) == 3:
            try:
                dz, sp, so = (float(p) for p in parts)
            except ValueError:
                return False
            if 0.0 <= dz <= 1.0 and 0.01 <= sp <= 20.0 and 0.0 <= so <= 1.0:
                settings['deadzone_offset'], settings['speed'], settings['speed_offset'] = dz, sp, so
                return True
    return False


def load_input_ini(path):
    """Keyboard and mouse-button bindings for KEYBOARD_OUTPUTS: {output: [name, ...]} (in file
    order, so the first binding shown matches what a fresh install's default.ini documents), the
    mouse settings dict (MOUSE_SETTINGS_DEFAULTS' keys), plus the raw lines for save_input_ini to
    round-trip. An output can have more than one binding -- the shipped default does this for
    circle/square (a keyboard key, then a mouse button); the Controls page shows them in SLOTS
    slots. A mouse button name is not accepted for a STICK_OUTPUTS entry -- a stick half-axis
    stays keyboard-only, since a button press has no notion of the sustained pull an axis slot
    expects.

    mouse_to_joystick is read whether or not its line is commented out (the default file ships
    it commented, "mouse off"), since the launcher needs to show/restore that line's state, not
    just whether the mouse is live right now -- uncommenting it is exactly what the "Enable
    mouse look" checkbox does in save_input_ini.
    """
    bindings = {key: [] for key in KEYBOARD_OUTPUTS}
    try:
        lines = Path(path).read_text(encoding='utf-8').splitlines()
    except FileNotFoundError:
        lines = DEFAULT_INPUT_INI.splitlines()
    except OSError:
        lines = []
    toggle_key, reload_key = 'f7', 'f8'
    mouse = dict(MOUSE_SETTINGS_DEFAULTS)
    for raw in lines:
        line = ''.join(raw.split())  # CFG-003: all whitespace stripped before parsing
        hash_pos = line.find('#')
        body = line[:hash_pos] if hash_pos >= 0 else line
        commented_body = line[hash_pos + 1:] if hash_pos >= 0 else ''
        if '=' in commented_body:
            output, value = (p.strip() for p in commented_body.split('=', 1))
            if output.split(':')[0] == 'mouse_to_joystick' and value.split(':')[0] in ('left', 'right'):
                # A *commented-out* mouse_to_joystick line still tells us which stick to
                # restore to if re-enabled, just not that it's on right now.
                mouse['stick'] = value.split(':')[0]
        if '=' not in body:
            continue
        output, value = (p.strip() for p in body.split('=', 1))
        output = output.split(':')[0]
        value = value.split(':')[0]
        if output == 'hotkey_toggle_mouse_to_joystick' and value in KEY_NAMES_SET:
            toggle_key = value
        elif output == 'hotkey_reload_inputs' and value in KEY_NAMES_SET:
            reload_key = value
        elif _parse_mouse_line(output, value, mouse):
            pass
        elif output in bindings and is_bindable(output, value):
            bindings[output].append(format_binding(value))
    return bindings, lines, toggle_key, reload_key, mouse


def reserved_keys(toggle_key, reload_key):
    return ALWAYS_RESERVED | {toggle_key, reload_key}


def save_input_ini(path, bindings, lines):
    """`bindings[output]` is a list: the output's keyboard and mouse bindings, all of them. They
    are written where its first one was (or at the end of the file), and its other keyboard and
    mouse lines are dropped; controller lines are never touched. An empty list unbinds it.

    A single value (or None) instead rewrites only the first keyboard- or mouse-button-binding
    line of the output in place (same "preserve everything else" contract as the game's own BbSettings::Save for
    bbport.ini): comments, controller lines and other outputs are kept byte for byte. A second+
    existing binding on the same output (e.g. the default's "r3 = q" / "r3 = c" pair) is left
    untouched; only the first occurrence is replaced, since that is the only slot this editor
    shows. An output set to None removes its first binding line entirely (NAME-005 equivalent:
    the user can still type "unmapped" by hand for a line this editor doesn't touch).
    """
    written, out = set(), []
    for raw in lines:
        line = ''.join(raw.split())
        hash_pos = line.find('#')
        body = line[:hash_pos] if hash_pos >= 0 else line
        if '=' in body:
            output, value = (p.split(':')[0] for p in body.split('=', 1))
            # Only a line whose value is a name this editor itself can produce (a keyboard key,
            # or -- for a BUTTON_OUTPUTS entry -- a mouse button) is "the first binding" for this
            # output -- a controller line with the same output (e.g. "cross = cross", the button
            # name, not a key/mouse name) must never be mistaken for one and overwritten.
            if output in bindings and isinstance(bindings[output], list) and is_bindable(output, value):
                if output not in written:
                    written.add(output)
                    out.extend(f'{output} = {format_binding(v)}' for v in bindings[output])
                continue  # a later binding: written with the first one above, or gone
            if output in bindings and output not in written and is_bindable(output, value):
                written.add(output)
                new_key = bindings[output]
                if new_key is None:
                    continue  # drop this line: no first binding anymore
                out.append(f'{output} = {format_binding(new_key)}')
                continue
        out.append(raw)
    for output in KEYBOARD_OUTPUTS:
        if output not in written and bindings.get(output):
            values = bindings[output]
            out.extend(f'{output} = {format_binding(v)}' for v in (values if isinstance(values, list) else [values]))
    Path(path).write_text('\n'.join(out) + '\n', encoding='utf-8')


def save_mouse_settings(path, settings, lines):
    """Rewrites mouse_to_joystick (commenting it out when settings['enabled'] is False, same as
    the default file ships it) and mouse_movement_params in place; appends either line if the
    file never had it (an input.ini from before this feature, or one copied from shadPS4, which
    uses the same key names but never had mouse_to_joystick to begin with)."""
    written_toggle = written_params = False
    written_simple = set()
    out = []
    for raw in lines:
        line = ''.join(raw.split())
        hash_pos = line.find('#')
        body = line[:hash_pos] if hash_pos >= 0 else line
        commented_body = line[hash_pos + 1:] if hash_pos >= 0 else ''
        is_toggle_line = ('mouse_to_joystick' in body.split('=', 1)[0] if '=' in body else False) or \
            ('mouse_to_joystick' in commented_body.split('=', 1)[0] if '=' in commented_body else False)
        is_params_line = '=' in body and body.split('=', 1)[0].split(':')[0] == 'mouse_movement_params'
        if is_toggle_line and not written_toggle:
            written_toggle = True
            new_line = f"mouse_to_joystick = {settings['stick']}"
            out.append(new_line if settings['enabled'] else f'# {new_line}')
            continue
        simple = next((key for key, name in MOUSE_SIMPLE_LINES.items()
                       if '=' in body and body.split('=', 1)[0].split(':')[0] == name), None)
        if simple:
            if simple not in written_simple:
                written_simple.add(simple)
                out.append(_mouse_simple_line(simple, settings[simple]))
            continue
        if is_params_line and not written_params:
            written_params = True
            out.append('mouse_movement_params = {:.3g}, {:.3g}, {:.3g}'.format(
                settings['deadzone_offset'], settings['speed'], settings['speed_offset']))
            continue
        out.append(raw)
    if not written_toggle:
        new_line = f"mouse_to_joystick = {settings['stick']}"
        out.append(new_line if settings['enabled'] else f'# {new_line}')
    if not written_params:
        out.append('mouse_movement_params = {:.3g}, {:.3g}, {:.3g}'.format(
            settings['deadzone_offset'], settings['speed'], settings['speed_offset']))
    for key in MOUSE_SIMPLE_LINES:
        if key not in written_simple and settings[key] != MOUSE_SETTINGS_DEFAULTS[key]:
            out.append(_mouse_simple_line(key, settings[key]))
    Path(path).write_text('\n'.join(out) + '\n', encoding='utf-8')


def _mouse_simple_line(key, value):
    if isinstance(value, bool):
        return f"{MOUSE_SIMPLE_LINES[key]} = {'on' if value else 'off'}"
    if isinstance(value, str):
        return f'{MOUSE_SIMPLE_LINES[key]} = {value}'
    return f'{MOUSE_SIMPLE_LINES[key]} = {value:.3g}'
