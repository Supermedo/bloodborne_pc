# Summary: keyboard/controller remap and mouse control

Spec: [spec-design-keyboard-mouse-input.md](spec-design-keyboard-mouse-input.md) (v2.1, shadPS4
syntax). This file summarizes what was built and the key design decisions; the spec's
requirement IDs (`CFG-*`, `MOU-*`, etc.) are referenced below for traceability.

## What was built

**`src/runtime_input_config.c`/`.h`** — a standalone parser for `input.ini` (shadPS4's syntax),
testable without SDL video or a window [CFG-001..008, NAM-001..005, HOT-001, HOT-002, GUD-001,
GUD-004]. Name tables for keys, mouse buttons/wheel, controller buttons/axes are ported from
shadPS4 (SPDX-attributed). Unknown or out-of-scope lines (combos, `key_toggle`, hotkeys other
than F7/F8, `override_controller_color`) are skipped with one warning each, so a real shadPS4
config file loads without erroring out.

**`src/runtime_pad.c`** — `sample_host()` now evaluates `InputConfig` bindings instead of the
previous fixed keyboard/controller tables [OUT-001..006, MIX-001..006]: buttons OR across sources,
axes sum in axis units then go through deadzone and halfmode, triggers carry both a button bit
and an analog value. `reload_config_if_requested()` hot-swaps the whole config under the pad lock
on F8 [CFG-008, GUD-002] — load/parse I/O happens off the lock, only the pointer swap is
synchronized.

**`gpu/bbgpu.h` / `gpu/shim/bbgpu.cpp` / `gpu/shim/window.cpp` / `gpu/shim/sdl_window.h`** — mouse
capture and the F7/F8 hotkeys live on the window thread (the only thread allowed to call SDL
window/video functions) [MOU-002..008, HOT-003]. `bbgpu_mouse_take()` hands the accumulated mouse
state to the pad thread under a dedicated mutex (not per-field atomics, since the fields must be
read as one consistent group — e.g. a stale `buttons` without its matching `captured=0` could
read as a phantom click after capture ends) [API-001..002].

**Mouse-to-stick conversion** (`mouse_to_axis()` in `runtime_pad.c`) — converts accumulated mouse
motion into a stick contribution, normalized to a fixed 33 ms window so sensitivity doesn't
depend on frame rate [MOU-009, CNV-001..003]. The final design applies shadPS4's linear response
formula unfiltered to each raw delta, then lerps only the *output* stick value toward that target
with an exponential time constant. See spec §4.4 for the exact formula and its history: four
earlier iterations (a hard floor, a magnitude-based ramp, a velocity low-pass filter) each fixed
one in-game feel problem (micro-jumps, a value discontinuity, a stall during fast reversals) while
introducing another, before landing on smoothing the output instead of the input.

**Launcher (`launcher/bbport_input_config.py`, `launcher/bbport_launcher_win.py`)** — a "Controls"
page lets players rebind keyboard keys and mouse buttons (left/middle/right/side) without editing
`input.ini` by hand. Mouse-button capture listens for `<ButtonPress>` alongside `<KeyPress>`;
Tk's `event.num` 4/5 report the side buttons on Windows (Tcl/Tk 8.6.12+, which ships with modern
Python, so no extra dependency was needed). Editing only replaces an output's first binding slot,
matching how a stick half-axis's second binding is already left alone — the shipped default
actually binds some outputs (e.g. Circle) to both a keyboard key and a mouse button at once, and
rebinding one doesn't disturb the other.

**Tests** — `tests/test_input_config.c` covers the parser in isolation, including shadPS4's own
default config as a fixture. `tests/test_pad.c` covers binding evaluation, deadzone, halfmode,
the mouse formula (steady state, frame-rate independence, 180-degree reversal, saturation), mouse
buttons/wheel through the full `BbMouseInput` → bit-mask path, and F8 reload — via controllable
stubs of `bbgpu_mouse_take`/`bbgpu_input_reload_requested`.

**Docs** — `docs/INPUT.md` (the user-facing reference) and the README's keyboard section.

## Key design decisions

- **shadPS4's file syntax, not a bbport-specific format**: anyone who already plays Bloodborne on
  shadPS4 can copy their config in as-is.
- **Keyboard, controller and mouse always combined, never a fallback**: the pre-existing behavior
  (keyboard only without a controller) was intentionally changed, since always-combined matches
  shadPS4 and is what most players expect.
- **One key per line, no combos**: covers the common case without the added complexity of
  shadPS4's own combo/key-consumption logic. Deferred, not rejected — see "Not done" below.
- **Mouse clicks only fire while the mouse is captured**: clicking the window to give it focus
  must never register as an attack. This is a deliberate divergence from shadPS4.
- **`input.ini` is a separate file from `bbport.ini`**: the game's settings menu never rewrites
  it, so hand edits and shadPS4 imports survive normal play.
- **Config immutability (GUD-002)**: a reload builds an entirely new config and swaps a pointer
  under the existing pad lock, rather than mutating shared state in place, avoiding any
  intermediate state visible to the game.

## Not done (left for later work)

- **Multi-key combos** (e.g. Ctrl+E for one action). Two pieces, neither started: the C parser
  would need to accept a comma-separated input and require both held at once (CFG-005 currently
  rejects any comma as out of scope); the launcher's capture flow would need to accumulate held
  modifier keys before committing a binding, instead of committing on the first keypress.
- **Linux launcher remap page** (GTK) — the Windows Tkinter launcher's Controls page has no
  equivalent there yet.
- **In-game remap UI** (ImGui, both platforms), with immediate application via the same mechanism
  F8 already uses for a full-file reload.
- **`key_toggle`, mouse-as-gyro, mouse-as-touchpad** — shadPS4 features not ported.
- **`BbSettings::Save()` preserving unknown `bbport.ini` keys** — not required by this feature,
  but still a useful improvement on its own.
