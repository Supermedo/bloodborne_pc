/* SPDX-License-Identifier: GPL-2.0-or-later
 * bbport: input.ini (keyboard/controller/mouse remap), in the shadPS4 input config syntax
 * ("output = input", one input per line), so a Bloodborne config made for shadPS4 can be
 * copied over this file. See specs/keyboard-and-mouse/spec-design-keyboard-mouse-input.md. */
#ifndef BB_RUNTIME_INPUT_CONFIG_H
#define BB_RUNTIME_INPUT_CONFIG_H
#include <stdint.h>
#include <stddef.h>

/* Outputs the game pad exposes (spec section 4.1). Buttons/d-pad share one enum with the
 * stick/trigger and mode/hotkey outputs so a binding array can be indexed by output. */
typedef enum {
    OUT_CROSS, OUT_CIRCLE, OUT_SQUARE, OUT_TRIANGLE,
    OUT_L1, OUT_R1, OUT_L2, OUT_R2, OUT_L3, OUT_R3, OUT_OPTIONS,
    OUT_PAD_UP, OUT_PAD_DOWN, OUT_PAD_LEFT, OUT_PAD_RIGHT,
    OUT_TOUCHPAD_LEFT, OUT_TOUCHPAD_CENTER, OUT_TOUCHPAD_RIGHT,
    OUT_AXIS_LEFT_X_MINUS, OUT_AXIS_LEFT_X_PLUS, OUT_AXIS_LEFT_Y_MINUS, OUT_AXIS_LEFT_Y_PLUS,
    OUT_AXIS_RIGHT_X_MINUS, OUT_AXIS_RIGHT_X_PLUS, OUT_AXIS_RIGHT_Y_MINUS, OUT_AXIS_RIGHT_Y_PLUS,
    OUT_AXIS_LEFT_X, OUT_AXIS_LEFT_Y, OUT_AXIS_RIGHT_X, OUT_AXIS_RIGHT_Y,
    OUT_LEFTJOYSTICK_HALFMODE, OUT_RIGHTJOYSTICK_HALFMODE,
    OUT_HOTKEY_MOUSE_TOGGLE, OUT_HOTKEY_RELOAD,
    OUT_COUNT,
} InputOutput;

/* Input kinds (spec section 4: Nomes). */
typedef enum {
    IN_NONE = 0,     /* "unmapped": a binding that never fires */
    IN_KEY,           /* keyboard: value is an SDL_Scancode (resolved from an SDLK_* name) */
    IN_MOUSE_BUTTON,  /* value is an SDL_BUTTON_* *index* (SDL_BUTTON_LEFT=1, _MIDDLE=2, ...),
                       * NOT the SDL_BUTTON_MASK() bit BbMouseInput.buttons uses -- T5 must
                       * convert with SDL_BUTTON_MASK(value) before comparing against it. */
    IN_MOUSE_WHEEL,   /* value is 0=up,1=down,2=left,3=right */
    IN_CBUTTON,       /* value is an SDL_GAMEPAD_BUTTON_* */
    IN_AXIS,          /* value is an SDL_GAMEPAD_AXIS_*, full range (l2/r2/axis_*) */
    IN_AXIS_HALF,     /* value is an SDL_GAMEPAD_AXIS_*; sign carried in InputBinding.half_sign */
} InputKind;

/* Combos (shadPS4's "output = a, b, c"): up to 3 inputs that must all be held. Only digital
 * inputs (keys, mouse buttons and wheel, controller buttons) take part in one. */
#define INPUT_MAX_COMBO 3

/* One more input of a combo binding. */
typedef struct {
    InputKind kind;
    int32_t value;
} InputComboPart;

typedef struct {
    InputKind kind;
    int32_t value;
    int8_t half_sign; /* +1/-1 for IN_AXIS_HALF; unused otherwise */
    uint8_t extra_count; /* combo: inputs held together with this one (0 = a single input) */
    InputComboPart extra[INPUT_MAX_COMBO - 1];
} InputBinding;

/* OUT-004/OUT-005: thresholds used by runtime_pad.c (T2) to turn an analog input bound to a
 * button-like output into a press. Kept here, next to the bindings they apply to, instead of
 * being re-derived as magic numbers at the call site. */
#define INPUT_TRIGGER_BUTTON_THRESHOLD 30   /* of 0..255, matches the pre-existing l2/r2 threshold */
#define INPUT_HALF_AXIS_BUTTON_THRESHOLD 64 /* of 0..127 (SDL axis >> 8), shadPS4's 0x40 of 0x7f */

/* Up to 4 bindings per output (spec KBD-001/PAD-001). A 5th binding on the same output is
 * accepted and ignored with a warning (not silently dropped without a trace). */
#define INPUT_MAX_BINDINGS_PER_OUTPUT 4

typedef struct {
    InputBinding bindings[OUT_COUNT][INPUT_MAX_BINDINGS_PER_OUTPUT];
    uint8_t binding_count[OUT_COUNT];
} InputBindingTable;

typedef struct {
    float deadzone_offset, speed, speed_offset; /* mouse_movement_params; shadPS4 defaults 0.5,1,0.125 */
    int stick; /* 0=none, 1=left, 2=right; mouse_to_joystick */
    int camera_direct;        /* mouse_camera = direct (1) | stick (0, default) */
    float camera_sensitivity; /* mouse_camera_sensitivity; 1.0 default */
    /* Both camera modes, applied to the mouse motion itself: */
    int invert_x, invert_y;   /* mouse_invert_x / mouse_invert_y = on | off (default) */
    float sensitivity_y;      /* mouse_sensitivity_y: vertical as a multiple of horizontal, 1.0 */
    int camera_sync;          /* mouse_camera_sync = on (default) | off: the direct camera turns
                               * its drawn position too, so a turn is not eased in by a spring */
} MouseParams;

/* The mouse motion's multipliers from MouseParams: -1 for an inverted axis, the vertical one
 * times sensitivity_y. */
static inline float input_mouse_scale_x(const MouseParams *m) { return m->invert_x ? -1.0f : 1.0f; }
static inline float input_mouse_scale_y(const MouseParams *m) {
    return (m->invert_y ? -1.0f : 1.0f) * m->sensitivity_y;
}

typedef struct {
    int inner, outer; /* analog_deadzone; 1..127 each, inner <= outer */
} AxisDeadzone;

typedef enum { DEADZONE_LEFT_STICK, DEADZONE_RIGHT_STICK, DEADZONE_L2, DEADZONE_R2, DEADZONE_COUNT } DeadzoneTarget;

/* Parsed, immutable configuration (GUD-002: load/parse happen off the pad lock; only the
 * pointer swap on reload is done under it). */
typedef struct InputConfig {
    InputBindingTable table;
    MouseParams mouse;
    AxisDeadzone deadzone[DEADZONE_COUNT];
    int stick_socd_last;        /* stick_socd = last (1, default) | neutral (0) */
    int stick_release_delay_ms; /* stick_release_delay_ms = 0..200 (default 0), left stick */
    int stick_circular;         /* stick_circular = on (1, default) | off (0), left stick keys */
    int stick_turn_rate;        /* stick_turn_rate = 0..5000 degrees/s (0 = instant, default) */
    int32_t toggle_scancode; /* hotkey_toggle_mouse_to_joystick; SDL_SCANCODE_UNKNOWN = none bound */
    int32_t reload_scancode; /* hotkey_reload_inputs */
    unsigned warnings;       /* lines ignored during parse, for the startup log line */
} InputConfig;

/* Parses config text already in memory (used directly by tests; also by input_config_load). */
void input_config_parse(InputConfig *out, const char *text);
/* Fills *out with the compiled-in defaults (spec section 4.3), as if the default file had
 * been parsed. Equivalent to input_config_parse(out, input_config_default_text()). */
void input_config_defaults(InputConfig *out);
/* Loads from `path`: creates it with the default text first if missing (CFG-002). On any
 * I/O failure, falls back to defaults in memory and returns 0 (never fails the caller). */
int input_config_load(InputConfig *out, const char *path);
/* BB_INPUT_CONFIG if set and non-empty; otherwise the directory of the bbport.ini path
 * (BB_CONFIG, or "bbport.ini" in the working directory) with "input.ini" appended. Writes
 * into `out` (size `out_size`); returns 0 on truncation. */
int input_config_path(char *out, size_t out_size);
/* The default input.ini text (spec section 4.3), null-terminated, static storage. */
const char *input_config_default_text(void);

/* Name lookup, exposed for reuse by BB_PAD_FILE injection (GUD-003) and for tests. Returns 1
 * and fills *out on a match (case-insensitive), 0 otherwise. */
int input_output_from_name(const char *name, InputOutput *out);
const char *input_output_name(InputOutput out);
/* A keyboard or mouse binding as input.ini names it, a combination joined with '+'
 * ("lshift+leftbutton"); 0 for a controller binding or a name that does not fit `size`. */
int input_binding_keyboard_text(const InputBinding *b, char *out, size_t size);

#endif
