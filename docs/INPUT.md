# Keyboard, controller and mouse input

Bindings live in `input.ini`, next to `bbport.ini` (same folder rule: `BB_INPUT_CONFIG` if set,
otherwise the directory of `BB_CONFIG`/`bbport.ini`). If the file does not exist, bbport creates
it with the defaults below the first time the pad is opened.

The syntax is shadPS4's input config format (`output = input`, one input per line), so **a
Bloodborne config made for shadPS4** (`user/input_config/CUSA03173.ini` or `default.ini`) can be
copied over `input.ini` as is. Lines shadPS4 supports that bbport does not yet (key combos,
`key_toggle`, hotkeys other than the two below, `override_controller_color`, mouse-as-gyro,
mouse-as-touchpad) are skipped with one log warning per line; the rest of the file still loads.

Keyboard and controller bindings are always combined, not a fallback: the keyboard works
whether or not a controller is connected. When more than one source is bound to the same
output, buttons need only one source held; stick axes add their contributions together (and
are then clamped and passed through the deadzone), matching shadPS4.

## Outputs

| Output | What it does |
|---|---|
| `cross` `circle` `square` `triangle` | Face buttons |
| `l1` `r1` `l2` `r2` `l3` `r3` | Shoulders, triggers, stick clicks |
| `options` | Pause/menu |
| `pad_up` `pad_down` `pad_left` `pad_right` | D-pad |
| `touchpad_left` `touchpad_center` `touchpad_right` | A touch at that position on the pad |
| `axis_left_x_minus`/`_plus`, `axis_left_y_minus`/`_plus` | Left stick, one direction each |
| `axis_right_x_minus`/`_plus`, `axis_right_y_minus`/`_plus` | Right stick, one direction each |
| `axis_left_x` `axis_left_y` `axis_right_x` `axis_right_y` | A stick bound to a full physical axis (e.g. another stick) |
| `leftjoystick_halfmode` `rightjoystick_halfmode` | Holding this input halves that stick's deflection (walk) |
| `hotkey_toggle_mouse_to_joystick` | Key that toggles mouse look on/off (default `f7`) |
| `hotkey_reload_inputs` | Key that reloads this file without restarting (default `f8`) |

`l2`/`r2` also carry an analog value: a trigger bound to them passes its own reading; a key or
button snaps it to the maximum, same as the previous fixed layout.

## Inputs

- **Keyboard:** letters/digits (`a`..`z`, `0`..`9`), `f1`..`f12`, `space`, `enter`, `tab`,
  `backspace`, `escape`, `lshift`/`rshift`, `lctrl`/`rctrl`, `lalt`/`ralt`, arrow keys
  (`up`/`down`/`left`/`right`), the numpad (`kp0`..`kp9`, `kpslash`, ...) and the usual
  punctuation keys (`comma`, `period`, `semicolon`, ...). Names follow what the key shows on the
  active keyboard layout, like shadPS4 — not its physical position.
- **Mouse buttons:** `leftbutton`, `middlebutton`, `rightbutton`, `sidebuttonback`,
  `sidebuttonforward`.
- **Mouse wheel:** `mousewheelup`, `mousewheeldown`, `mousewheelleft`, `mousewheelright`.
- **Controller buttons:** `cross`, `circle`, `square`, `triangle` (by position, not the printed
  letter), `l1`, `r1`, `l3`, `r3`, `options`, `pad_up`/`pad_down`/`pad_left`/`pad_right`,
  `back`/`share`, paddles (`lpaddle_high`, `l4`, `l5`, ...).
- **Controller axes:** `l2`, `r2` (triggers, 0..255), `axis_left_x`/`axis_left_y`/`axis_right_x`/
  `axis_right_y` (a full stick axis) and their half-axis forms (`axis_left_x_minus`,
  `axis_left_x_plus`, ...).
- **`unmapped`:** an explicit "no binding", different from leaving the line out.

Up to 4 bindings per output; a 5th is accepted and logged as ignored.

## Mouse look

```ini
mouse_to_joystick = right            # left | right; absent = mouse look does not exist
mouse_movement_params = 0.5, 1, 0.125   # deadzone_offset (unused, see below), speed, speed_offset
```

Mouse motion is smoothed over a short, fixed time window (so a fast flick or a quick 180-degree
turn stays responsive, while small, slow movements don't produce a jumpy camera) and then
converted into the chosen stick with a formula based on shadPS4's `EmulateJoystick`, normalized
so the result does not depend on the game's frame rate. `speed` scales sensitivity; `speed_offset`
adds a constant on top of it, raising the response for slow movements without affecting fast ones
much. `deadzone_offset` is parsed for compatibility with existing files but no longer affects the
camera (an earlier version of this formula used it as a startup floor; the launcher's
"Smoothness" slider was removed along with that). Mouse buttons and the wheel only act while the
mouse is actually captured (window focused, settings menu and text entry closed, and look turned
on) — clicking the window to give it focus never fires an attack.

**Known limitation:** the stick is still a stick. Camera rotation saturates at the game's own
maximum turn speed and inherits its acceleration curve; this is not 1:1 mouse aim, by design —
it is the same tradeoff shadPS4 makes.

## Analog deadzone

```ini
analog_deadzone = leftjoystick, 1, 127   # device, inner, outer (1..127 each)
```

`device` is `leftjoystick`, `rightjoystick`, `l2` or `r2`. Below `inner` the output is 0; from
`inner` to `outer` it ramps up linearly; at or above `outer` it is fully deflected. The default
(`1, 127`) is effectively no deadzone, matching the behavior before this feature existed.

## Reserved keys

**Insert** (the settings menu) and **F9** (`BB_PAD_RECORD`) can never be bound to a game output,
and are rejected with a warning if a line tries. **Escape** can be bound (e.g. `options = escape`):
the settings menu only uses it to close itself, and the game gets no input while that menu or the
text entry box is open. Whatever key currently toggles
mouse look or reloads the file (`f7`/`f8` by default, or whatever `hotkey_toggle_mouse_to_joystick`/
`hotkey_reload_inputs` were last set to) is reserved the same way.

## Default file

```ini
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
```

Without an `input.ini`, this default reproduces the previous fixed layout, except: the keyboard
now works together with a controller instead of only without one, and **Q is lock-on (R3)**
instead of Triangle — **V is Triangle** now, and **C still works as R3** too. The mouse starts
off.

## Changing bindings while the game runs

Edit `input.ini` and press **F8** (or whatever `hotkey_reload_inputs` is set to). The whole file
is re-read and swapped in for the next pad sample — no restart needed. If the file cannot be
read (e.g. a typo left it without a closing line, or it was deleted mid-edit), the previous
bindings keep working and a warning is logged; the game is never left without any input config.

## See also

- [specs/keyboard-and-mouse/spec-design-keyboard-mouse-input.md](../specs/keyboard-and-mouse/spec-design-keyboard-mouse-input.md) —
  full design spec, requirement by requirement.
- [docs/MODS.md](MODS.md) — loose-file mods and third-party game patches.
