---
title: Keyboard/controller remap and mouse control (camera and buttons)
version: 2.1
date_created: 2026-10-07
last_updated: 2026-10-10
owner: bbport (Windows fork)
tags: [design, input, pad, mouse, keyboard, config, shadps4-compat]
---

# Introduction

This specification defines bbport's configurable keyboard/controller remap and mouse control
(camera via an emulated stick, mouse buttons as PS4 buttons). Configuration lives in a dedicated
input file (`input.ini`), using shadPS4's input config syntax and semantics, so a Bloodborne
config made for shadPS4 works here too. There is no in-game remap screen yet (see the launcher's
Controls page instead, which edits this same file); the file can always be edited by hand too.

**Reference:** shadPS4, commit `0fe263a`: `src/input/input_handler.{h,cpp}`,
`src/input/input_mouse.cpp`, `src/sdl_window.cpp`. Both projects are GPL-2.0-or-later, so tables
and snippets can be copied with an SPDX header and a `bbport:` marker on changes, the same way
`gpu/shadps4/`'s vendored code already works.

## 1. Purpose and scope

**Purpose:** play with keyboard and mouse the way shadPS4 does, reusing configs made for it,
without breaking the existing controller-only experience.

**In scope:**

- An `input.ini` file using shadPS4's syntax, generated with bbport's defaults if missing.
- Remapping keyboard keys, controller buttons/axes, and mouse buttons/wheel to PS4 outputs.
- Keyboard, controller and mouse all active at the same time.
- Mouse-as-stick (`mouse_to_joystick`), with a formula inspired by shadPS4's own.
- Half-speed stick mode (`leftjoystick_halfmode`, `rightjoystick_halfmode`).
- Configurable stick/trigger deadzone (`analog_deadzone`).
- F7 (toggle mouse) and F8 (reload `input.ini`) hotkeys, remappable.
- Automated tests and documentation.

**Out of scope:**

- An in-game or launcher remap screen beyond what already ships (see §11/Phase 2 notes).
- Multi-key combos (`lalt, f12`), `key_toggle`, shadPS4's other hotkeys, mouse-as-gyro (F6) and
  mouse-as-touchpad (Delete).
- Per-controller configuration (`:N` suffix), a `global.ini` file, per-game profiles.
- 1:1 mouse aim (CON-004).
- Changes to `BbSettings::Save()`: `input.ini` is a separate file the game's menu never rewrites.

**Audience:** developers and LLM agents extending this feature.

**Assumptions:** SDL3 is the only input dependency. The code builds on Linux (GCC, `build.sh`)
and Windows (MSYS2 CLANG64).

## 2. Definitions

| Term | Definition |
|---|---|
| Pad / `PadData` | The `OrbisPadData` struct returned to the game by `scePadReadState` ([src/runtime_pad.c](../../src/runtime_pad.c)). Sticks are `uint8_t` centered on 128; `l2`/`r2` triggers range 0 to 255. |
| Output | What the game sees: a PS4 button, an axis direction, a full analog axis, a mode (halfmode), or a hotkey. The left side of an `input.ini` line. |
| Input | A key, a controller button/axis, or a mouse button/wheel step. The right side of a line. |
| Binding | One `output = input` line. An output can have several bindings (several lines). |
| Mouse mode | The on/off state of mouse-to-stick emulation, toggled by F7. |
| Capture | SDL's relative mouse mode active (`SDL_SetWindowRelativeMouseMode`): cursor hidden and confined to the window. |
| Window thread | The thread running `WindowSDL::PollEvents` ([gpu/shim/window.cpp](../../gpu/shim/window.cpp)), the only one allowed to call SDL video/window functions. |
| Pad thread | The game thread that calls `scePadReadState`; it runs `sample()` in `runtime_pad.c`. |
| Menu | bbport's ImGui settings menu (Insert or L3+R3). |
| Text box | The on-screen character-name entry drawn over the game (`SetTextEntry`). |
| Axis unit | A signed value from -127 to 127 used when summing axis contributions; 0 is center. On output it becomes `128 + v`, clamped to 0..255. |

## 3. Requirements, constraints and guidelines

### File and syntax

- **CFG-001**: The path is `BB_INPUT_CONFIG` if set and non-empty, else `input.ini` next to
  `bbport.ini`, whose own path follows `BbSettings::Path()`'s rule (`BB_CONFIG`, else `bbport.ini`
  in the current directory).
- **CFG-002**: If the file doesn't exist, bbport creates it with the text in §4.3 and uses it. If
  it can't be created, the same defaults are used in memory and a warning is logged.
- **CFG-003**: Syntax follows shadPS4:
  - all whitespace is stripped before parsing;
  - `#` starts a comment to end of line;
  - each line is `output = input`;
  - a `:N` suffix on either side is accepted and discarded;
  - blank lines are ignored.
- **CFG-004**: Names are compared case-insensitively — a superset of shadPS4, which only accepts
  lowercase.
- **CFG-005**: Each line has a single input. An input with a comma (a combo) is ignored with a
  warning. The exception is the special lines in §4.2, whose parameters are comma-separated.
- **CFG-006**: Lines with an unknown output or input, `key_toggle`, hotkeys other than F7/F8, and
  `override_controller_color` are ignored with one warning per line at load time. The rest of the
  file still loads, so a shadPS4 file imports without erroring out.
- **CFG-007**: With no controller line at all, the controller drives nothing, matching shadPS4.
  The default file ships controller lines.
- **CFG-008**: The file is read on `scePadOpen` and re-read when the user presses
  `hotkey_reload_inputs` (default F8). A reload swaps the whole config at once, between two pad
  samples, with no intermediate state visible to the game.

### Names

- **NAM-001**: Key names match shadPS4's `string_to_keyboard_key_map` table (`a`-`z`, `0`-`9`,
  `f1`-`f12`, `space`, `enter`, `tab`, `backspace`, `lshift`, `lctrl`, `lalt`, `up`, `left`,
  `kp0`-`kp9`, `comma`, `period`, and the rest). Each name maps to a logical SDL key (`SDLK_*`),
  converted to a scancode via `SDL_GetScancodeFromKey` at load time — so names follow the active
  keyboard layout, matching shadPS4.
- **NAM-002**: Mouse names: `leftbutton`, `rightbutton`, `middlebutton`, `sidebuttonback`,
  `sidebuttonforward`, `mousewheelup`, `mousewheeldown`, `mousewheelleft`, `mousewheelright`.
- **NAM-003**: Controller button names (`string_to_cbutton_map` table), by physical position:
  `cross` = south, `circle` = east, `square` = west, `triangle` = north, plus `l1`, `r1`, `l3`,
  `r3`, `options`, `pad_up`, `pad_down`, `pad_left`, `pad_right`, `back` and `share` (both SDL's
  Back button), `lpaddle_high`, `lpaddle_low`, `rpaddle_high`, `rpaddle_low`, `l4`, `l5`, `r4`,
  `r5`, `qam`.
- **NAM-004**: Controller axis names (`string_to_axis_map` table): `l2`, `r2` (triggers),
  `axis_left_x`, `axis_left_y`, `axis_right_x`, `axis_right_y` (full axis) and
  `axis_left_x_plus`/`_minus` and equivalents (half axis).
- **NAM-005**: `unmapped` as an input creates a binding that never fires. Lets a config explicitly
  state an output has no key, instead of just omitting the line.

### Outputs

- **OUT-001**: Valid outputs are listed in §4.1. Buttons, axis directions and full axes follow
  shadPS4's names. The three touchpad zones (`touchpad_left`, `touchpad_center`, `touchpad_right`)
  produce a touch at x = 480, 960 or 1440, y = 471.
- **OUT-002**: The controller's physical touchpad still reports real finger position and click,
  as today ([runtime_pad.c:118-130](../../src/runtime_pad.c#L118-L130)), independent of any
  binding. Like shadPS4, the physical `touchpad` button cannot be remapped.
- **OUT-003**: A button or key bound to `l2`/`r2` sets the button bit and drives the analog value
  to 255.
- **OUT-004**: A physical trigger bound to a button output fires the button once it passes 30 out
  of 0..255 (bbport's existing threshold). Bound to `l2`/`r2`, it passes the analog value through
  and fires the bit above 30.
- **OUT-005**: A physical half axis (`axis_left_x_plus` etc.) bound to a button output fires past
  half travel, matching shadPS4 (`0x40` of `0x7f`).
- **OUT-006**: The L3+R3 combo that opens the menu uses the physical buttons, independent of any
  remap.

### Mixing

- **MIX-001**: Keyboard, controller and mouse are all read every sample, with or without a
  controller connected.
- **MIX-002**: Buttons: any one active binding is enough to activate the output (OR).
- **MIX-003**: Axes, as in shadPS4: every active binding's contribution (keys and half axes at
  ±127, physical axes at their read value, mouse at the value from §4.4) is summed in axis units.
  Deadzone (`analog_deadzone`) and the halfmode factor apply after that, and the result is clamped
  to -127..127. Opposite directions cancel out.
- **MIX-004**: Analog triggers: contributions sum, clamped to 0..255.
- **MIX-005**: `sample()`'s order is unchanged: host sample (keyboard, controller and mouse
  combined), then recording (`BB_PAD_RECORD`), injection (`BB_PAD_FILE`) and replay
  (`BB_PAD_REPLAY`). Recording captures the already-combined state, mouse included.
- **MIX-006**: With the menu or text box open, the game receives neutral input, and the
  post-close release delay (`hold_after_capture`) still applies.

### Halfmode and deadzone

- **HLF-001**: While a `leftjoystick_halfmode` (or `rightjoystick_halfmode`) binding is active,
  that stick's output is multiplied by 0.5 after deadzone, matching shadPS4. The default file
  binds no key to this.
- **DZN-001**: `analog_deadzone = <leftjoystick|rightjoystick|l2|r2>, <inner>, <outer>`, values
  1 to 127. Below `inner` the output is 0; from `inner` to `outer` it ramps linearly to 127; at or
  above `outer` it is 127. This is shadPS4's `ApplyDeadzone` formula. The default (`1, 127`) is
  effectively no deadzone, matching the pre-existing behavior.

### Mouse

- **MOU-001**: `mouse_to_joystick = left|right` picks the mouse's stick. Without this line, mouse
  mode doesn't exist, F7 does nothing, and the cursor is never captured. The default file ships
  this line commented out, so the mouse starts off.
- **MOU-002**: With `mouse_to_joystick` set, mouse mode starts on when the game opens.
  `hotkey_toggle_mouse_to_joystick` (default F7) toggles it on/off, and the state isn't saved
  between sessions.
- **MOU-003**: Capture is active if and only if all of these hold: mouse mode is on, the window
  has focus, the menu is closed, and the text box is inactive.
  **MOU-004**: The window thread evaluates MOU-003 on every `PollEvents` and calls
  `SDL_SetWindowRelativeMouseMode` only when the result changes. The menu can also close from the
  render thread ([bbport_overlay.cpp:396](../../gpu/shim/bbport_overlay.cpp#L396)), so the decision
  can't depend only on open/close events.
- **MOU-005**: Only while captured does the window thread accumulate `xrel`/`yrel`, button state
  and wheel steps. When capture ends, all of it is cleared and no button stays stuck.
- **MOU-006**: Mouse buttons and wheel only activate outputs while captured. Clicking the window
  to give it focus never fires an action. This diverges from shadPS4.
- **MOU-007**: Each wheel step keeps its `mousewheel*` input active for 33 ms, matching shadPS4.
  Consecutive steps merge.
- **MOU-008**: The pad thread drains the accumulated state every sample (read and clear) via
  `bbgpu_mouse_take` (§4.5), and never calls SDL mouse functions itself.
- **MOU-009**: Conversion to a stick value uses the formula in §4.4 and `mouse_movement_params`.

### Reserved keys and hotkeys

- **HOT-001**: `Insert`, `Escape` (menu) and `F9` (`BB_PAD_RECORD`) are reserved. A game-output
  binding to any of them is ignored with a warning.
- **HOT-002**: `hotkey_toggle_mouse_to_joystick` (default `f7`) and `hotkey_reload_inputs`
  (default `f8`) can be remapped in `input.ini`. Whatever key is assigned becomes reserved too.
  Missing either line falls back to its default.
- **HOT-003**: Hotkeys are handled on the window thread, via key events, not through the pad
  read path. They work with the menu open (F8) and with capture off (F7).

### Compatibility and constraints

- **CON-001**: No new dependencies. Only SDL3 and the standard library.
- **CON-002**: Code follows existing style: C11 in `src/`, C++ in `gpu/shim/`, comments in the
  same tone and density. Tables copied from shadPS4 keep their SPDX header and a source comment.
- **CON-003**: `BB_PAD_FILE`, `BB_PAD_RECORD` and `BB_PAD_REPLAY` keep working with the same file
  format.
- **CON-004**: The mouse is emulated as a stick. Rotation saturates at the game's own maximum
  turn speed and inherits its acceleration curve. This is a known, documented limitation, not a
  bug.
- **CON-005**: Compared to the pre-existing behavior, generating `input.ini` from defaults changes
  only what's expected: the keyboard works alongside a controller (MIX-001), Q becomes R3 and
  Triangle moves to V, and the mouse starts off.
- **CON-006**: Builds on Linux and Windows with no new platform `#ifdef`s in the input logic.

### Guidelines

- **GUD-001**: Parsing lives in its own C module (`src/runtime_input_config.c`), testable without
  GPU or a window. shadPS4's name tables are ported to C arrays.
- **GUD-002**: The loaded config is immutable. A reload (F8) builds a new struct and only then
  swaps the pointer under the pad lock, which already serializes `sample()`
  ([src/runtime_pad.c:58](../../src/runtime_pad.c#L58)). Loading itself (disk I/O, parsing) runs
  outside the lock: the pad mutex is also taken by `pad_read_state` every frame
  ([src/runtime_pad.c:313-319](../../src/runtime_pad.c#L313-L319)), and I/O under that lock would
  stall the game's own pad read while the file loads.
- **GUD-003**: `BB_PAD_FILE`'s name table ([runtime_pad.c:181-186](../../src/runtime_pad.c#L181-L186))
  now uses the module's own output values, without changing the names injection accepts.

## 4. Interfaces and data contracts

### 4.1 Outputs

| Output | Type | Effect on `PadData` |
|---|---|---|
| `cross` `circle` `square` `triangle` `l1` `r1` `l3` `r3` `options` | button | bit in `buttons` |
| `pad_up` `pad_down` `pad_left` `pad_right` | button | D-pad bit in `buttons` |
| `l2` `r2` | button + trigger | bit in `buttons` and `l2`/`r2` (OUT-003, OUT-004) |
| `touchpad_left` `touchpad_center` `touchpad_right` | touchpad | `BTN_TOUCHPAD` and a touch at x=480/960/1440, y=471 |
| `axis_left_x_minus` `axis_left_x_plus` `axis_left_y_minus` `axis_left_y_plus` | half axis | -127 or +127 contribution to `left_x`/`left_y` |
| `axis_right_x_minus` … `axis_right_y_plus` | half axis | same, to `right_x`/`right_y` |
| `axis_left_x` `axis_left_y` `axis_right_x` `axis_right_y` | full axis | contribution of the bound physical axis |
| `leftjoystick_halfmode` `rightjoystick_halfmode` | mode | HLF-001 |
| `hotkey_toggle_mouse_to_joystick` `hotkey_reload_inputs` | hotkey | HOT-002 |

### 4.2 Special lines

| Line | Parameters | Default without the line |
|---|---|---|
| `mouse_to_joystick = left\|right` | the mouse's stick | mouse mode doesn't exist |
| `mouse_movement_params = <deadzone_offset>, <speed>, <speed_offset>` | three floats | `0.5, 1, 0.125` |
| `analog_deadzone = <device>, <inner>, <outer>` | `leftjoystick`/`rightjoystick`/`l2`/`r2`, 1..127, 1..127 | `1, 127` |

Valid ranges for `mouse_movement_params`: `speed_offset` 0.0 to 1.0; `speed` 0.01 to 20.0.
`deadzone_offset` is parsed (0.0 to 1.0) for file compatibility but has no effect on the current
formula (§4.4) — an earlier iteration used it as a response floor; see §4.4's history note. A
value outside its range invalidates the whole line, which falls back to the default with a
warning.

### 4.3 Default file (generated `input.ini`)

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

### 4.4 Mouse-to-stick conversion

Every pad sample, with mouse mode on and capture active, `mouse_to_axis()`
([src/runtime_pad.c](../../src/runtime_pad.c)) converts the raw mouse delta accumulated since the
last sample into a stick contribution:

```text
(dx, dy) = motion accumulated since the last sample (SDL pixels)
dt       = time since the last sample, clamped to [1 ms, 100 ms]
(sdx, sdy) = (dx, dy) * (33 ms / dt)             # rescale to shadPS4's fixed 33 ms window,
                                                  # so sensitivity doesn't depend on frame rate
magnitude = sqrt(sdx² + sdy²)
if magnitude < 0.01: target = (0, 0)
else:
    offset_fade = min(magnitude / 1.0, 1.0)      # fades speed_offset in over the first 1 px,
                                                  # instead of switching it on at a fixed threshold
    speed       = min(magnitude * speed + speed_offset * 128 * offset_fade, 128)
    angle       = atan2(sdy, sdx)
    target      = (cos(angle) * speed, sin(angle) * speed)   # axis units

alpha = 1 - exp(-dt / 25ms)
stick += (target - stick) * alpha                # exponential lerp toward target; stick is
                                                  # per-axis state that persists across samples
```

- **CNV-001**: `stick`'s contribution enters MIX-003's sum on `mouse_to_joystick`'s stick.
- **CNV-002**: With no motion in a sample, `target` is zero and `stick` lerps toward center
  rather than snapping to it — see the history note below for why.
- **CNV-003**: The 33 ms rescale keeps sensitivity independent of this port's actual frame rate
  (30-150+ FPS, vs. shadPS4's fixed ~30 Hz poll), so `mouse_movement_params` values tuned for
  shadPS4 carry over directly.

**History note:** this formula went through four iterations before settling here, all driven by
how the camera actually felt to play, not by a failing test — the earlier ones all passed their
own unit tests while still feeling wrong in-game:

1. shadPS4's own `EmulateJoystick` clamps speed up to a hard floor (`deadzone_offset * 128`)
   the instant any motion is seen — a step function. At this port's higher sample rate, a slow
   mouse move produces per-sample deltas small enough that the camera alternated between 0 and
   the floor every sample ("micro-jumps").
2. Replacing the floor with a curve that ramped up smoothly removed the step at zero, but the
   curve only asymptotically approached the floor, so switching from "use the curve" to "use the
   line" at the crossing point was still an abrupt jump in value.
3. Replacing that crossing with a single seamless exponential ramp (scaling the whole response by
   each sample's *instantaneous* magnitude) removed the remaining jump, but was still stateless:
   during a fast 180-degree reversal, magnitude dips near zero at the inflection point, resetting
   the ramp exactly when responsiveness mattered most, and a single fast flick from a standstill
   always started the ramp at 0.
4. Filtering mouse *velocity* (dx/dt) with a per-component low-pass filter fixed the reversal
   stall and slow start, but amplified the mouse's own sampling noise — velocity is a derivative,
   and dividing small, irregular per-sample deltas by a small `dt` makes the filtered input
   noisier, not smoother, which read as a jittery/"crispy" camera.

The formula above is the fifth iteration: it moves smoothing from input space (velocity) to
output space (the stick value the player perceives as camera rotation). The raw-delta formula
runs unfiltered every call — so it reacts immediately, with no reversal-stall or slow-start risk
— and only the final stick value is lerped toward that instantaneous target. A lerp has no
near-zero singularity to stall at, so it damps jitter without reintroducing the earlier problems.

### 4.5 GPU library/runtime API (`gpu/bbgpu.h`)

```c
/* Mouse input accumulated by the window thread since the last call; the call resets it.
 * All zeros while capture is inactive. buttons uses SDL_BUTTON_MASK bits; wheel holds the
 * mousewheel* inputs currently active (33 ms pulses), one bit per direction. */
typedef struct {
    float dx, dy;          /* relative motion, SDL pixels */
    uint32_t buttons;      /* currently held mouse buttons */
    uint32_t wheel;        /* bit 0 up, 1 down, 2 left, 3 right */
    int32_t captured;      /* 1 while capture is active */
} BbMouseInput;
void bbgpu_mouse_take(BbMouseInput *out);
/* Applies the loaded input config to the window thread: whether mouse mode exists, and the
 * scancodes of the toggle and reload hotkeys (SDL_SCANCODE_UNKNOWN = none). */
void bbgpu_input_configure(int mouse_mode_available, int toggle_scancode, int reload_scancode);
/* 1 once after the reload hotkey was pressed; the pad thread then re-reads input.ini. */
int bbgpu_input_reload_requested(void);
```

- **API-001**: The runtime calls `bbgpu_input_configure` after every load (open and F8).
- **API-002**: `bbgpu_mouse_take` and `bbgpu_input_reload_requested` are safe from any thread and
  never call SDL. `BbMouseInput`'s 5 fields need to be read and cleared as one consistent group
  (e.g. `buttons` without its matching `captured` could become a phantom click after capture
  ends), so the internal state uses one small dedicated mutex, not a `std::atomic` per field —
  unlike `width`/`height`/`is_open` in
  [sdl_window.h:39-40](../../gpu/shim/sdl_window.h#L39-L40), which are independent of each other.
  `bbgpu_mouse_take` copies the whole struct under that mutex and clears the accumulator before
  releasing it.
- **API-003**: `tests/test_pad.c` `#include`s `runtime_pad.c`, so it gets test-controlled stubs
  of these three functions for free.

### 4.6 Config module (`src/runtime_input_config.c`)

```c
typedef struct InputConfig InputConfig;   /* bindings, special lines, hotkeys */
InputConfig *input_config_load(const char *path);          /* creates the default file if missing */
InputConfig *input_config_parse(const char *text);          /* tests: parse from memory */
void input_config_free(InputConfig *config);
const char *input_config_default_text(void);                /* section 4.3 */
```

The internal layout is an implementation detail, as long as the separation holds: parsing needs
no SDL video or GPU, and is testable from in-memory text.

- **GUD-004**: Every line is truncated to 255 bytes before parsing (`fgets` with a 256-byte
  buffer, like `BbSettings::Load()` in [bbport_settings.cpp](../../gpu/shim/bbport_settings.cpp)).
  An output or input name over 47 bytes (margin above the longest real table name,
  `hotkey_toggle_mouse_to_joystick`, 31 bytes) is rejected rather than compared truncated against
  the §4 tables; never a width-less `scanf`/`sscanf` `%s`, for the same reason
  [src/runtime_pad.c:191](../../src/runtime_pad.c#L191) (`fscanf(f,"%63s",token)`) is already
  bounded. A line or name past the limit is treated as invalid (CFG-004: warning, rest of the
  file still loads).

## 5. Acceptance criteria

- **AC-001**: Given `cross = j` in `input.ini`, when the game reads the pad with J held, then
  `BTN_CROSS` is active. With no other `cross` keyboard line, Space does not trigger `cross`.
- **AC-002**: Given the line `cross = banana`, then it's ignored, a load-time warning is logged,
  and the rest of the file still applies.
- **AC-003**: Given `r1 = insert`, then the line is ignored as reserved and Insert still opens
  the menu.
- **AC-004**: Given the default file and a connected controller, when W is pressed, then
  `left_y` moves accordingly — the keyboard works alongside the controller.
- **AC-005**: Given the controller's left stick at +72 on X and A held (-127), then `left_x` =
  128 + (72 − 127) = 73.
- **AC-006**: Given `cross = circle`, when the controller's east button is pressed, then
  `BTN_CROSS` is active.
- **AC-007**: Given `r1 = r2`, when the right trigger passes 30, then `BTN_R1` is active.
- **AC-008**: Given shadPS4's own default input config (`GetDefaultInputConfig`, commit
  `0fe263a`) copied in as `input.ini`, then the game loads it without error, keyboard and
  controller bindings work, and only `override_controller_color` warns.
- **AC-009**: Given no `input.ini`, when the game opens the pad, then the file is created with
  §4.3's text and behaves per CON-005.
- **AC-010**: Given `mouse_to_joystick = right`, with the window focused and the menu closed,
  then the cursor is captured. Opening the menu (Insert, L3+R3) releases it; closing it via the
  "Close" button releases it too.
- **AC-011**: Given `mouse_to_joystick = right`, when the window loses focus, then capture ends
  and no mouse button stays stuck. Regaining focus re-captures.
- **AC-012**: Given `mouse_to_joystick = right`, when F7 is pressed, then mouse mode turns off
  and the cursor is released; another F7 turns it back on and re-captures.
- **AC-013**: Without `mouse_to_joystick`, the cursor is never captured, F7 does nothing, and
  clicks produce no input.
- **AC-014**: With the mouse captured and the cursor released via F7, a left click does not
  trigger `r1`.
- **AC-015**: Given the same constant-velocity motion, the mouse's steady-state contribution is
  the same (within a small tolerance) sampled every 33 ms or every 7 ms — CNV-003's frame-rate
  independence.
- **AC-016**: Given `mouse_movement_params = 0.5, 1, 0.125` and a steady 10 px/33 ms motion in X,
  held long enough for the lerp to settle, then the contribution converges to the unfiltered
  formula's own value for that magnitude (§4.4) — not necessarily its first-sample value.
- **AC-017**: Given `leftjoystick_halfmode = lalt` with W and left Alt held, then `left_y` =
  128 − 63 = 65 (−127 × 0.5, truncated).
- **AC-018**: Given the game running, when `cross = j` is added to `input.ini` and F8 is pressed,
  then J starts triggering `cross` without a restart.
- **AC-019**: Given `hotkey_toggle_mouse_to_joystick = f6`, then F6 toggles the mouse and F7 is
  free for bindings.
- **AC-020**: Given `mousewheelup` bound to `pad_up`, each wheel step produces `BTN_UP` for 33 ms.
- **AC-021**: `BB_PAD_FILE`, `BB_PAD_RECORD` and `BB_PAD_REPLAY` pass `test_pad.c`'s existing
  tests with no format change.
- **AC-022**: `bash build.sh --test` and `python3 -m unittest discover -s tests` pass on Linux,
  and the MSYS2 CLANG64 build compiles with no new warnings.

## 6. Test strategy

- **Levels:** unit (parsing, mouse formula, deadzone, halfmode), integration (`sample()` with an
  SDL virtual controller and the mouse stub), and manual (in-game).
- **Tools:** C tests using `assert`, compiled by `build.sh --test`, matching `tests/test_pad.c`'s
  existing pattern (`dummy` SDL video driver, `SDL_AttachVirtualJoystick`).
- **Data:** in-memory config text (`input_config_parse`) and shadPS4's own default file embedded
  as a fixture (`tests/data/shadps4_default_input.ini`) for AC-008.
- **Unit:** every rule in CFG-003 to CFG-006; names in NAM-001 to NAM-005; reserved keys
  (HOT-001, HOT-002); the mouse formula's steady-state and frame-rate-independence behavior
  (AC-015, AC-016).
- **Integration:** mixing (AC-004, AC-005), virtual-controller remap (AC-006, AC-007), buttons
  and wheel via the stub (AC-014, AC-020), neutral input with the menu open, reload (AC-018 via
  the `bbgpu_input_reload_requested` stub).
- **Manual:** AC-010 to AC-013 and AC-019 on Windows and Linux (X11 and Wayland).
- **CI:** no pipeline; tests run locally before each merge.

## 7. Rationale

- **shadPS4's format in a dedicated file:** anyone already playing Bloodborne on shadPS4 reuses
  their config as-is. The format is community-documented, and a separate file is never rewritten
  by the game's own menu (`BbSettings::Save()` only rewrites `bbport.ini`'s known keys).
- **One key per line:** covers typical shadPS4 files without bringing in combo/key-consumption
  logic, the most complex part of `input_handler.cpp`.
- **Summed axes:** parity with shadPS4; the mouse adds into the same sum instead of overriding
  the axis the way some other emulators do.
- **Mouse enabled by the file's own line:** keeps the mouse off by default and avoids needing F7
  every session.
- **Clicks only while captured:** clicking the window to give it focus must never fire an attack.
- **33 ms rescale:** the game reads the pad once per frame (30-150+ FPS). Rescaling keeps
  sensitivity independent of frame rate and lets shadPS4's own `mouse_movement_params` values
  carry over.
- **Capture decided by the window thread every frame:** the menu can also close from the render
  thread, and SDL window functions must run on the window thread.
- **shadPS4's key names as logical keys:** matches shadPS4's own semantics (`SDLK_*`), so an
  imported file behaves the same in both. Converting to a scancode at load time keeps runtime
  reads going through `SDL_GetKeyboardState`.
- **Q as lock-on, V as Triangle, no middle-mouse default, no halfmode key by default:** product
  decisions, not technical constraints.
- **Output-space lerp for the mouse formula (§4.4):** see that section's history note — four
  earlier iterations each fixed one in-game feel problem while introducing another, until
  smoothing moved from the input (velocity) to the output (stick value).

## 8. Dependencies and external integrations

### Platform dependencies

- **PLT-001**: SDL3: keyboard state, controllers, mouse events, relative mode, and
  `SDL_GetScancodeFromKey`. Already a project dependency.
- **PLT-002**: Linux (X11 and Wayland) and Windows. On Wayland, relative mode depends on the
  compositor supporting pointer constraints; checked in the manual test pass.

### Internal dependencies

- **INT-001**: `BbSettings::Path()` (`gpu/shim/bbport_settings.cpp`): defines `input.ini`'s
  folder (CFG-001). The C runtime needs the same rule; replicating it in C (a `getenv` and a
  `dirname`) is the simplest approach.
- **INT-002**: `BbOverlay::CapturesInput()`: menu and text box state for MOU-003.
- **INT-003**: Launchers: the Windows launcher (`launcher/bbport_launcher_win.py`) has a Controls
  page that edits `input.ini` directly (keyboard and mouse-button bindings); a Linux launcher
  equivalent, and in-game remap, remain future work.

### External reference

- **EXT-001**: shadPS4 `src/input/input_handler.h` (name tables), `input_handler.cpp` (parsing,
  `ApplyDeadzone`, defaults), `input_mouse.cpp` (`EmulateJoystick`, the formula this feature's own
  mouse conversion was originally inspired by — see §4.4's history note for how it diverged),
  `sdl_window.cpp` (the 33 ms wheel pulse).

## 9. Examples and edge cases

```ini
# A shadPS4 file with some out-of-scope lines
cross = n
hotkey_screenshot_with_overlays = lalt, f12   # hotkey other than F7/F8: ignored with a warning
key_toggle = f1, w                            # key_toggle: ignored with a warning
l2 = lshift, mousewheelup                     # combo: ignored with a warning
r1:1 = r1:2                                   # controller-index suffixes: accepted and discarded
mouse_to_joystick = right
mouse_movement_params = 0.4, 1.5, 0.1
```

Edge cases:

- `cross =` (empty input): line ignored with a warning.
- `Cross = SPACE`: valid (CFG-004).
- Two identical lines (`cross = space` twice): no extra effect, no warning.
- `mouse_to_joystick = both`: invalid; the line is ignored and mouse mode doesn't exist.
- `hotkey_reload_inputs = f9`: F9 is already reserved (HOT-001); the line is ignored and F8 keeps
  working.
- `mouse_movement_params = 0.5, 1`: missing a parameter; the line falls back to the default with
  a warning.
- Mouse and the controller's right stick at the same time: contributions sum (MIX-003).
- F8 with a broken file (e.g. deleted mid-edit): CFG-002 applies (the file is recreated with
  defaults) and the log warns. The game is never left without a config.
- F8 with a key held down: the new config applies on the next sample; the held key now triggers
  whatever the new file says.

## 10. Validation criteria

- Every criterion in §5 verified: automatable ones in `build.sh --test`, manual ones recorded in
  the PR.
- No regression in `tests/test_pad.c` or `python3 -m unittest discover -s tests`.
- README (keyboard section) and `docs/INPUT.md` cover the format, the default layout, how to
  import from shadPS4, what's ignored (CFG-005, CFG-006), reserved keys, and the CON-004
  limitation.
- Linux and Windows builds both compile.

## 11. Related specs and further reading

- [tasks.md](tasks.md): summary of what was built and why.
- [docs/INPUT.md](../../docs/INPUT.md): the user-facing reference for `input.ini`.
- shadPS4: https://github.com/shadps4-emu/shadPS4 (`src/input/` folder).
- SDL3: `SDL_GetScancodeFromKey`, `SDL_SetWindowRelativeMouseMode` (wiki.libsdl.org).
