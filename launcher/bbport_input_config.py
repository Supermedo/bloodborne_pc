#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""input.ini read/write for the launchers (shadPS4 input config syntax; see docs/INPUT.md and
src/runtime_input_config.c, the authoritative parser this mirrors). Scope: keyboard and mouse-
button bindings for the plain button/d-pad outputs (BUTTON_OUTPUTS) and the stick half-axes
(STICK_OUTPUTS, keyboard only -- a mouse button is a press, not an axis), which is what the
launcher's remap page edits. Controller bindings and the scroll wheel are preserved untouched
(round-tripped as unknown lines by save_input_ini), not edited here.
"""
import os
from pathlib import Path

# Keyboard-only outputs the remap page edits. (action key in the .ini, Tk display order).
BUTTON_OUTPUTS = ['cross', 'circle', 'square', 'triangle', 'l1', 'r1', 'l2', 'r2', 'l3', 'r3',
                   'options', 'pad_up', 'pad_down', 'pad_left', 'pad_right',
                   'touchpad_left', 'touchpad_right']
STICK_OUTPUTS = ['axis_left_x_minus', 'axis_left_x_plus', 'axis_left_y_minus', 'axis_left_y_plus',
                  'axis_right_x_minus', 'axis_right_x_plus', 'axis_right_y_minus', 'axis_right_y_plus']
KEYBOARD_OUTPUTS = BUTTON_OUTPUTS + STICK_OUTPUTS

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
}

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

# Names runtime_input_config.c's kMouseButtonNames accepts. Only buttons a BUTTON_OUTPUTS slot
# can take (the wheel is a repeating pulse, not a holdable press, and stays input.ini-only).
MOUSE_BUTTON_NAMES = ('leftbutton', 'middlebutton', 'rightbutton', 'sidebuttonback', 'sidebuttonforward')
MOUSE_BUTTON_NAMES_SET = set(MOUSE_BUTTON_NAMES)

# What a BUTTON_OUTPUTS/STICK_OUTPUTS line's value may be for load_input_ini/save_input_ini to
# treat it as "this editor's own binding slot" rather than an untouched controller/other line.
# Sticks stay keyboard-only (KEY_NAMES_SET); buttons additionally accept a mouse button name.
BINDABLE_NAMES_SET = KEY_NAMES_SET | MOUSE_BUTTON_NAMES_SET

# Reserved regardless of the file (HOT-001/HOT-002): the menu, BB_PAD_RECORD, and whichever key
# currently toggles the mouse or reloads input.ini. The last two are read from the file itself
# (hotkey_toggle_mouse_to_joystick / hotkey_reload_inputs) by reserved_keys() below.
ALWAYS_RESERVED = {'insert', 'escape', 'f9'}

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


def keysym_to_name(keysym):
    """Tk's event.keysym -> our key name, or None if this key has no equivalent (e.g. a dead
    key, a media key, or a layout-specific glyph outside KEY_NAMES)."""
    name = TK_KEYSYM_TO_NAME.get(keysym, keysym.lower())
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


def input_ini_path(bbport_ini_path):
    """Mirrors src/runtime_input_config.c's input_config_path(): BB_INPUT_CONFIG if set and
    non-empty, else input.ini next to bbport_ini_path (itself BB_CONFIG-derived)."""
    env = os.environ.get('BB_INPUT_CONFIG')
    if env:
        return Path(env)
    return Path(bbport_ini_path).parent / 'input.ini'


# shadPS4/bbport defaults when mouse_movement_params is absent (spec section 4.2 / 4.3).
MOUSE_SETTINGS_DEFAULTS = {'enabled': False, 'stick': 'right',
                            'deadzone_offset': 0.5, 'speed': 1.0, 'speed_offset': 0.125}


def _parse_mouse_line(output, value, settings):
    """Shared by the commented and uncommented cases below: mouse_to_joystick and
    mouse_movement_params, whichever `output`/`value` this particular line parsed to."""
    if output == 'mouse_to_joystick' and value in ('left', 'right'):
        settings['enabled'] = True
        settings['stick'] = value
        return True
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
    round-trip. An output can have more than one binding -- the shipped default actually does
    this for circle/square (a keyboard key as the first/listed slot, a mouse button as a later
    one); the UI below only edits the first slot and leaves any further ones alone, same spirit
    as the C parser's "extra bindings are kept, not silently dropped" rule, so rebinding Circle's
    key never touches its separate mouse-button slot (or vice versa -- a user who wants to
    retarget that second slot has to edit input.ini by hand, same as for a stick axis's second
    binding). A mouse button name is only accepted for a BUTTON_OUTPUTS entry -- a stick
    half-axis stays keyboard-only, since a button press has no notion of the sustained pull an
    axis slot expects.

    mouse_to_joystick is read whether or not its line is commented out (the default file ships
    it commented, "mouse off"), since the launcher needs to show/restore that line's state, not
    just whether the mouse is live right now -- uncommenting it is exactly what the "Enable
    mouse look" checkbox does in save_input_ini.
    """
    bindings, lines = {key: [] for key in KEYBOARD_OUTPUTS}, []
    try:
        lines = Path(path).read_text(encoding='utf-8').splitlines()
    except OSError:
        pass
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
        elif output in bindings and (value in KEY_NAMES_SET or
                                      (output in BUTTON_OUTPUTS and value in MOUSE_BUTTON_NAMES_SET)):
            bindings[output].append(value)
    return bindings, lines, toggle_key, reload_key, mouse


def reserved_keys(toggle_key, reload_key):
    return ALWAYS_RESERVED | {toggle_key, reload_key}


def save_input_ini(path, bindings, lines):
    """Rewrites the first keyboard- or mouse-button-binding line of each KEYBOARD_OUTPUTS key in
    place (same "preserve everything else" contract as the game's own BbSettings::Save for
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
            is_bindable = value in KEY_NAMES_SET or (output in BUTTON_OUTPUTS and value in MOUSE_BUTTON_NAMES_SET)
            if output in bindings and output not in written and is_bindable:
                written.add(output)
                new_key = bindings[output]
                if new_key is None:
                    continue  # drop this line: no first binding anymore
                out.append(f'{output} = {new_key}')
                continue
        out.append(raw)
    for output in KEYBOARD_OUTPUTS:
        if output not in written and bindings.get(output):
            out.append(f'{output} = {bindings[output]}')
    Path(path).write_text('\n'.join(out) + '\n', encoding='utf-8')


def save_mouse_settings(path, settings, lines):
    """Rewrites mouse_to_joystick (commenting it out when settings['enabled'] is False, same as
    the default file ships it) and mouse_movement_params in place; appends either line if the
    file never had it (an input.ini from before this feature, or one copied from shadPS4, which
    uses the same key names but never had mouse_to_joystick to begin with)."""
    written_toggle = written_params = False
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
    Path(path).write_text('\n'.join(out) + '\n', encoding='utf-8')
