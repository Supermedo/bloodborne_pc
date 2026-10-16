# Keyboard, controller and mouse input

Bindings live in `input.ini`, next to `bbport.ini` (same folder rule: `BB_INPUT_CONFIG` if set,
otherwise the directory of `BB_CONFIG`/`bbport.ini`). If the file does not exist, bbport creates
it with the defaults below the first time the pad is opened.

The syntax is shadPS4's input config format (`output = input`, one input per line), so **a
Bloodborne config made for shadPS4** (`user/input_config/CUSA03173.ini` or `default.ini`) can be
copied over `input.ini` as is. Lines shadPS4 supports that bbport does not yet (`key_toggle`,
hotkeys other than the two below, `override_controller_color`, mouse-as-gyro, mouse-as-touchpad)
are skipped with one log warning per line; the rest of the file still loads.

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

## Combinations

```ini
r1 = leftbutton
r2 = lshift, leftbutton          # Shift + left click: strong attack
l2 = lctrl, lshift, rightbutton  # up to three inputs
```

Inputs separated by commas must all be held together, as in shadPS4. They can be keys, mouse
buttons, wheel steps and controller buttons, each at most once; analog sticks and triggers
cannot be part of a combination. A held combination takes priority over the bindings on its parts:
with the lines above, Shift + left click is R2 only, not R1 as well, and a binding on Shift alone
is released while the combination is held. Three inputs beat two the same way.

Bloodborne dodges when Circle is *released*, and running is Circle held. So a key that is an
action of its own and also part of a combination is pressed on the way into it: with
`circle = lshift` (the default), Shift + left click starts a sprint and then lets go of Circle,
which the game reads as a dodge. Give a modifier no action of its own (as the Dark Souls III
layout does with Shift), or use another modifier. The launcher's Controls page marks such
overlaps with ⚠, and records a combination when you hold Shift, Ctrl or Alt while pressing the
key or mouse button.

The Controls page shows three slots per action (the controller's line is the fourth binding):
click a slot and press a key, click a mouse button or turn the wheel; right-click a slot to
clear it. Walk (`leftjoystick_halfmode`, the left stick at half) has a row too.

## Dark Souls III layout

The launcher's Controls page has **Use the Dark Souls III layout**: it replaces the keyboard and
mouse lines of `input.ini` (the controller's lines and the other settings stay) with this
layout, after Ryan Sousa's keyboard and mouse fork:

| Action | Keys |
|---|---|
| Move / walk (hold) | W A S D / Left Alt |
| Camera | Mouse (direct, 1:1), or I J K L |
| Dodge, run (hold), back in menus | Space |
| Attack / strong attack (R1 / R2) | Left click / Shift + left click |
| Transform / firearm (L1 / L2) | Right click / Shift + right click or Left Ctrl |
| Interact, confirm (Cross) | E / Enter |
| Quick item (Square) / blood vial (Triangle) | R / F |
| Lock on (R3) / jump (L3) | Q or wheel click / C |
| Items (d-pad up/down) | ↑ ↓ or wheel |
| Weapons (d-pad left/right) | ← → or Shift + wheel |
| Game menu / gestures / key items | Tab or Esc / G / Backspace |

Shift is only a modifier in it, so Shift + click never passes through a dodge (see
*Combinations*).

## Button prompts

```ini
button_prompts = auto   # bbport.ini: auto (default) | playstation | xbox | switch | keyboard
```

The game's button icons (its key guides in menus, the buttons in messages, and the prompts in the
world such as "Pick Up Item" or "Open Door") follow the device
you use: a key, a click, the wheel or moving the mouse shows keyboard and mouse icons, a controller
button or stick shows the controller's, mid-game: PlayStation for a DualShock or DualSense, Switch
for a Pro Controller or Joy-Cons, and Xbox for any other controller. The Xbox and Switch icons
stand for the buttons in the same place (Cross is Xbox A and Switch B, Circle is Xbox B and Switch
A); the d-pad and stick icons stay the game's own. Without any input yet they are the controller's
when one is connected. The keyboard icons are drawn from `input.ini` (a binding's first keyboard
or mouse input, the last input of a combination with its modifier above it, the mouse for the
stick it drives), so they follow your bindings and change with F8. Set in the launcher's
Controls page or the in-game menu.

How: the menus' icons are the game's `KG_*` textures in `menu/common.tpf.dcx` (32x32 BC7, in the
PS4's tiled layout); the prompts in the world draw their own Circle, Cross and L3 from the HUD
atlas `MENU_Common_00091` (1024x512, each a 26x26 sprite in a block-aligned area). The texture
cache recognises both by hash when the game uploads them (`gpu/shim/bbport_prompts.cpp`), and the
window thread writes the other set of pixels into guest memory when the device changes
(`gpu/shim/bbport_prompt_icons.cpp` draws and encodes them). `BB_PROMPT_TRACE=1` logs every
small texture the game uploads and saves its bytes, to find button pictures in other textures.

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
it is the same tradeoff shadPS4 makes. For 1:1 aim, use the direct camera below.

## Inverting and vertical sensitivity

```ini
mouse_invert_x = off        # on | off
mouse_invert_y = off        # on | off
mouse_sensitivity_y = 1.0   # 0.1..10: up/down as a multiple of left/right
```

These scale the mouse motion itself, so they apply to both camera modes (stick and direct) and
to the stick the mouse drives while locked on. The launcher's Controls page sets them under
*Mouse look*.

## Direct mouse camera (1:1, game 1.09)

```ini
mouse_camera = direct            # direct | stick (default: stick)
mouse_camera_sensitivity = 1.0   # 1.0 = 900 mouse counts per radian; higher turns faster
mouse_camera_sync = on           # on (default) | off: turn the drawn camera position too
mouse_to_joystick = right        # still required: mouse look on/off (F7), and the lock-on stick
```

With `mouse_camera = direct`, mouse motion writes the game's camera yaw/pitch itself instead of
emulating a stick, like a native PC game: no stick deadzone, no acceleration curve, no turn-speed
cap. It hooks the game's camera update (`gpu/shim/bloodborne_cam.cpp`: the Bloodborne mouse
camera by imedved on Nexus Mods, ported to this port by mcrib884 in #3) and samples the mouse at
1 kHz on its own thread. The launcher's Controls page sets the mode and this sensitivity under
*Mouse look* (the stick sensitivity there applies to stick emulation only).

The game draws the camera from a position that follows its ideal one around the character with a
spring, so turning the angles alone eases a turn in over about 100 ms. With `mouse_camera_sync =
on`, the drawn position is turned by the same angles, and the turn is drawn whole in the next
frame (the approach of Ryan Sousa's keyboard and mouse fork). Set it to `off` and press F8 to
compare.

- **Lock-on:** while a target is locked, motion goes to the stick instead (the game drives the
  camera then), so a flick still switches targets.
- **Monocular:** aiming through the Monocular follows the mouse too.
- **F7** turns mouse look off and removes the hook, so the right stick/arrow keys drive the
  camera again. While the direct camera is on, the stick does not turn the camera.
- **Fallback:** if the camera code is not found (another game version), the port logs
  `Mouse camera: ... using the stick mode` once and the stick emulation above is used instead.
- Latency is lowest with a locked frame rate (60 FPS works well).

## Keyboard movement: opposite keys and quick direction changes

```ini
stick_socd = last              # last (default) | neutral
stick_release_delay_ms = 60    # 0..200, left stick only; default 0 (off)
```

A neutral stick ends a sprint, and two things used to produce one when strafing A <-> D:

- **Both keys down during the switch.** With `stick_socd = last`, two opposite directions held
  together give the one pressed most recently (keyboard "snap tap"), instead of cancelling out.
  `neutral` restores the old sum-and-cancel behavior (shadPS4's). Applies to both sticks' half-axis
  bindings (keys, buttons, half sticks); full-axis bindings are unaffected.
- **A gap between releasing one key and pressing the next.** `stick_release_delay_ms` keeps the
  left stick's last direction for that long after its keys are released; any new direction
  replaces it immediately. 40-80 ms covers a fast key switch while still stopping on the spot
  when you let go.

```ini
stick_circular = on            # on (default) | off
stick_turn_rate = 900          # degrees per second, 0..5000; default 0 (instant)
```

Keys alone drive the left stick like no real thumbstick can:

- **Square diagonals.** W+A is (-127,-127), about 1.41x the reach of a real (round) stick.
  `stick_circular = on` clamps it to the circle: a diagonal is full deflection, like a controller.
- **Direction jumps.** Going from W to W+A turns the stick 45 degrees within one sample.
  `stick_turn_rate` rotates it toward the new direction at that many degrees per second instead,
  at full deflection the whole way (so a sprint is kept), like a thumb sliding along the gate.
  900 turns 45 degrees in 50 ms. A reversal of more than 150 degrees (A <-> D, W <-> S) still
  snaps, as a flick would, and after a release the next key starts straight at its direction.

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
