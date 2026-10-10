/* SPDX-License-Identifier: GPL-2.0-or-later
 * bbport: input.ini parser (keyboard/controller/mouse remap). Name tables for keys, mouse
 * inputs, controller buttons and axes are ported from shadPS4's src/input/input_handler.h
 * (commit 0fe263a, GPL-2.0-or-later), trimmed to what Bloodborne's pad exposes; see
 * specs/keyboard-and-mouse/spec-design-keyboard-mouse-input.md for the full contract.
 *
 * This file only parses text into an InputBindingTable; it does no I/O beyond input_config_load
 * reading/creating the file, no SDL video/window calls, and no locking (GUD-002: the caller
 * swaps the loaded InputConfig under its own lock). */
#define _GNU_SOURCE
#include "runtime_input_config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <SDL3/SDL.h>

/* GUD-004: bound every line and every name before comparing against the tables below.
 * NAME_MAX must exceed the longest real output/input name; "hotkey_toggle_mouse_to_joystick"
 * (31 chars) is the longest name in the tables below, so the margin here is intentional, not
 * tight to that exact value. */
#define LINE_MAX 255
#define NAME_MAX 47

/* ---- Name tables -------------------------------------------------------------------- */

typedef struct { const char *name; int32_t value; } NameValue;

/* string_to_keyboard_key_map (shadPS4 input_handler.h): logical SDLK_* keys, named by what the
 * key shows on the keyboard, not by physical position (NAM-001). Resolved to a scancode with
 * SDL_GetScancodeFromKey() at load time, so the mapping follows the active keyboard layout, as
 * in shadPS4. Mouse names live in a separate table (NAM-002). */
static const NameValue kKeyNames[] = {
    {"a",SDLK_A},{"b",SDLK_B},{"c",SDLK_C},{"d",SDLK_D},{"e",SDLK_E},{"f",SDLK_F},{"g",SDLK_G},
    {"h",SDLK_H},{"i",SDLK_I},{"j",SDLK_J},{"k",SDLK_K},{"l",SDLK_L},{"m",SDLK_M},{"n",SDLK_N},
    {"o",SDLK_O},{"p",SDLK_P},{"q",SDLK_Q},{"r",SDLK_R},{"s",SDLK_S},{"t",SDLK_T},{"u",SDLK_U},
    {"v",SDLK_V},{"w",SDLK_W},{"x",SDLK_X},{"y",SDLK_Y},{"z",SDLK_Z},
    {"0",SDLK_0},{"1",SDLK_1},{"2",SDLK_2},{"3",SDLK_3},{"4",SDLK_4},
    {"5",SDLK_5},{"6",SDLK_6},{"7",SDLK_7},{"8",SDLK_8},{"9",SDLK_9},
    {"f1",SDLK_F1},{"f2",SDLK_F2},{"f3",SDLK_F3},{"f4",SDLK_F4},{"f5",SDLK_F5},{"f6",SDLK_F6},
    {"f7",SDLK_F7},{"f8",SDLK_F8},{"f9",SDLK_F9},{"f10",SDLK_F10},{"f11",SDLK_F11},{"f12",SDLK_F12},
    {"escape",SDLK_ESCAPE},{"backspace",SDLK_BACKSPACE},{"delete",SDLK_DELETE},{"insert",SDLK_INSERT},
    {"home",SDLK_HOME},{"end",SDLK_END},{"pgup",SDLK_PAGEUP},{"pgdown",SDLK_PAGEDOWN},
    {"tab",SDLK_TAB},{"capslock",SDLK_CAPSLOCK},{"enter",SDLK_RETURN},
    {"lshift",SDLK_LSHIFT},{"rshift",SDLK_RSHIFT},{"lctrl",SDLK_LCTRL},{"rctrl",SDLK_RCTRL},
    {"lalt",SDLK_LALT},{"ralt",SDLK_RALT},{"lmeta",SDLK_LGUI},{"rmeta",SDLK_RGUI},
    {"lwin",SDLK_LGUI},{"rwin",SDLK_RGUI},
    {"space",SDLK_SPACE},{"up",SDLK_UP},{"down",SDLK_DOWN},{"left",SDLK_LEFT},{"right",SDLK_RIGHT},
    {"kp0",SDLK_KP_0},{"kp1",SDLK_KP_1},{"kp2",SDLK_KP_2},{"kp3",SDLK_KP_3},{"kp4",SDLK_KP_4},
    {"kp5",SDLK_KP_5},{"kp6",SDLK_KP_6},{"kp7",SDLK_KP_7},{"kp8",SDLK_KP_8},{"kp9",SDLK_KP_9},
    {"kpperiod",SDLK_KP_PERIOD},{"kpcomma",SDLK_KP_COMMA},{"kpslash",SDLK_KP_DIVIDE},
    {"kpasterisk",SDLK_KP_MULTIPLY},{"kpminus",SDLK_KP_MINUS},{"kpplus",SDLK_KP_PLUS},
    {"kpequals",SDLK_KP_EQUALS},{"kpenter",SDLK_KP_ENTER},
    {"grave",SDLK_GRAVE},{"minus",SDLK_MINUS},{"equals",SDLK_EQUALS},
    {"lbracket",SDLK_LEFTBRACKET},{"rbracket",SDLK_RIGHTBRACKET},{"backslash",SDLK_BACKSLASH},
    {"semicolon",SDLK_SEMICOLON},{"apostrophe",SDLK_APOSTROPHE},
    {"comma",SDLK_COMMA},{"period",SDLK_PERIOD},{"slash",SDLK_SLASH},
    {"printscreen",SDLK_PRINTSCREEN},{"scrolllock",SDLK_SCROLLLOCK},{"pausebreak",SDLK_PAUSE},
};

/* NAM-002: mouse buttons and wheel directions. Values are SDL_BUTTON_* (symbolic, not the
 * literals, even though they happen to equal 1..5 today) so a future SDL3 renumbering would
 * fail to compile here instead of silently mismatching the button bbgpu_mouse_take reports. */
static const NameValue kMouseButtonNames[] = {
    {"leftbutton",SDL_BUTTON_LEFT},{"middlebutton",SDL_BUTTON_MIDDLE},{"rightbutton",SDL_BUTTON_RIGHT},
    {"sidebuttonback",SDL_BUTTON_X1},{"sidebuttonforward",SDL_BUTTON_X2},
};
enum { WHEEL_UP=0, WHEEL_DOWN=1, WHEEL_LEFT=2, WHEEL_RIGHT=3 };
static const NameValue kMouseWheelNames[] = {
    {"mousewheelup",WHEEL_UP},{"mousewheeldown",WHEEL_DOWN},
    {"mousewheelleft",WHEEL_LEFT},{"mousewheelright",WHEEL_RIGHT},
};

/* NAM-003: controller buttons, by physical position (shadPS4 string_to_cbutton_map, trimmed to
 * what this port's pad resolves). "touchpad" itself is deliberately absent: the physical
 * touchpad button cannot be remapped (OUT-002), same as shadPS4. */
static const NameValue kCButtonNames[] = {
    {"cross",SDL_GAMEPAD_BUTTON_SOUTH},{"circle",SDL_GAMEPAD_BUTTON_EAST},
    {"square",SDL_GAMEPAD_BUTTON_WEST},{"triangle",SDL_GAMEPAD_BUTTON_NORTH},
    {"l1",SDL_GAMEPAD_BUTTON_LEFT_SHOULDER},{"r1",SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER},
    {"l3",SDL_GAMEPAD_BUTTON_LEFT_STICK},{"r3",SDL_GAMEPAD_BUTTON_RIGHT_STICK},
    {"options",SDL_GAMEPAD_BUTTON_START},
    {"back",SDL_GAMEPAD_BUTTON_BACK},{"share",SDL_GAMEPAD_BUTTON_BACK},
    {"pad_up",SDL_GAMEPAD_BUTTON_DPAD_UP},{"pad_down",SDL_GAMEPAD_BUTTON_DPAD_DOWN},
    {"pad_left",SDL_GAMEPAD_BUTTON_DPAD_LEFT},{"pad_right",SDL_GAMEPAD_BUTTON_DPAD_RIGHT},
    {"lpaddle_high",SDL_GAMEPAD_BUTTON_LEFT_PADDLE1},{"lpaddle_low",SDL_GAMEPAD_BUTTON_LEFT_PADDLE2},
    {"rpaddle_high",SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1},{"rpaddle_low",SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2},
    {"l4",SDL_GAMEPAD_BUTTON_LEFT_PADDLE1},{"l5",SDL_GAMEPAD_BUTTON_LEFT_PADDLE2},
    {"r4",SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1},{"r5",SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2},
    {"qam",SDL_GAMEPAD_BUTTON_MISC1},
};

/* NAM-004: full-range axes (l2/r2 trigger, or an integer stick axis bound to another stick). */
static const NameValue kAxisNames[] = {
    {"l2",SDL_GAMEPAD_AXIS_LEFT_TRIGGER},{"r2",SDL_GAMEPAD_AXIS_RIGHT_TRIGGER},
    {"axis_left_x",SDL_GAMEPAD_AXIS_LEFTX},{"axis_left_y",SDL_GAMEPAD_AXIS_LEFTY},
    {"axis_right_x",SDL_GAMEPAD_AXIS_RIGHTX},{"axis_right_y",SDL_GAMEPAD_AXIS_RIGHTY},
};
/* Half-axes: name -> (axis, sign). Table of (name, axis) pairs; sign comes from "_plus"/"_minus". */
typedef struct { const char *name; int32_t axis; int8_t sign; } HalfAxisName;
static const HalfAxisName kHalfAxisNames[] = {
    {"axis_left_x_minus",SDL_GAMEPAD_AXIS_LEFTX,-1},{"axis_left_x_plus",SDL_GAMEPAD_AXIS_LEFTX,1},
    {"axis_left_y_minus",SDL_GAMEPAD_AXIS_LEFTY,-1},{"axis_left_y_plus",SDL_GAMEPAD_AXIS_LEFTY,1},
    {"axis_right_x_minus",SDL_GAMEPAD_AXIS_RIGHTX,-1},{"axis_right_x_plus",SDL_GAMEPAD_AXIS_RIGHTX,1},
    {"axis_right_y_minus",SDL_GAMEPAD_AXIS_RIGHTY,-1},{"axis_right_y_plus",SDL_GAMEPAD_AXIS_RIGHTY,1},
};

/* Output names (left side of a binding line), matching InputOutput (spec section 4.1). */
static const NameValue kOutputNames[] = {
    {"cross",OUT_CROSS},{"circle",OUT_CIRCLE},{"square",OUT_SQUARE},{"triangle",OUT_TRIANGLE},
    {"l1",OUT_L1},{"r1",OUT_R1},{"l2",OUT_L2},{"r2",OUT_R2},{"l3",OUT_L3},{"r3",OUT_R3},
    {"options",OUT_OPTIONS},
    {"pad_up",OUT_PAD_UP},{"pad_down",OUT_PAD_DOWN},{"pad_left",OUT_PAD_LEFT},{"pad_right",OUT_PAD_RIGHT},
    {"touchpad_left",OUT_TOUCHPAD_LEFT},{"touchpad_center",OUT_TOUCHPAD_CENTER},{"touchpad_right",OUT_TOUCHPAD_RIGHT},
    {"axis_left_x_minus",OUT_AXIS_LEFT_X_MINUS},{"axis_left_x_plus",OUT_AXIS_LEFT_X_PLUS},
    {"axis_left_y_minus",OUT_AXIS_LEFT_Y_MINUS},{"axis_left_y_plus",OUT_AXIS_LEFT_Y_PLUS},
    {"axis_right_x_minus",OUT_AXIS_RIGHT_X_MINUS},{"axis_right_x_plus",OUT_AXIS_RIGHT_X_PLUS},
    {"axis_right_y_minus",OUT_AXIS_RIGHT_Y_MINUS},{"axis_right_y_plus",OUT_AXIS_RIGHT_Y_PLUS},
    {"axis_left_x",OUT_AXIS_LEFT_X},{"axis_left_y",OUT_AXIS_LEFT_Y},
    {"axis_right_x",OUT_AXIS_RIGHT_X},{"axis_right_y",OUT_AXIS_RIGHT_Y},
    {"leftjoystick_halfmode",OUT_LEFTJOYSTICK_HALFMODE},{"rightjoystick_halfmode",OUT_RIGHTJOYSTICK_HALFMODE},
    {"hotkey_toggle_mouse_to_joystick",OUT_HOTKEY_MOUSE_TOGGLE},{"hotkey_reload_inputs",OUT_HOTKEY_RELOAD},
};

/* ---- Small string helpers ------------------------------------------------------------ */

static int EqualsCaseInsensitive(const char *a, const char *b) {
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
        ++a; ++b;
    }
    return *a == *b;
}

static int LookupName(const NameValue *table, size_t count, const char *name, int32_t *out) {
    for (size_t i = 0; i < count; ++i) {
        if (EqualsCaseInsensitive(table[i].name, name)) { *out = table[i].value; return 1; }
    }
    return 0;
}
#define LOOKUP(table, name, out) LookupName((table), sizeof(table)/sizeof(*(table)), (name), (out))

/* Strips a trailing ":<digits>" gamepad-index suffix (CFG-003); the digits are not validated,
 * matching shadPS4 treating a malformed suffix as a warning, not a hard parse error -- we simply
 * discard the suffix either way (NOT per-controller configs, out of scope). */
static void StripColonSuffix(char *s) {
    char *colon = strrchr(s, ':');
    if (colon) *colon = '\0';
}

int input_output_from_name(const char *name, InputOutput *out) {
    int32_t v;
    if (!LOOKUP(kOutputNames, name, &v)) return 0;
    *out = (InputOutput)v;
    return 1;
}

const char *input_output_name(InputOutput out) {
    for (size_t i = 0; i < sizeof(kOutputNames)/sizeof(*kOutputNames); ++i)
        if ((InputOutput)kOutputNames[i].value == out) return kOutputNames[i].name;
    return "?";
}

/* ---- Binding resolution for one input token ------------------------------------------ */

static int ResolveInput(const char *token, InputBinding *out) {
    int32_t v;
    if (EqualsCaseInsensitive(token, "unmapped")) { out->kind = IN_NONE; return 1; }
    if (LOOKUP(kKeyNames, token, &v)) {
        SDL_Scancode sc = SDL_GetScancodeFromKey((SDL_Keycode)v, NULL);
        if (sc == SDL_SCANCODE_UNKNOWN) return 0;
        out->kind = IN_KEY; out->value = (int32_t)sc; return 1;
    }
    if (LOOKUP(kMouseButtonNames, token, &v)) { out->kind = IN_MOUSE_BUTTON; out->value = v; return 1; }
    if (LOOKUP(kMouseWheelNames, token, &v)) { out->kind = IN_MOUSE_WHEEL; out->value = v; return 1; }
    if (LOOKUP(kCButtonNames, token, &v)) { out->kind = IN_CBUTTON; out->value = v; return 1; }
    if (LOOKUP(kAxisNames, token, &v)) { out->kind = IN_AXIS; out->value = v; return 1; }
    for (size_t i = 0; i < sizeof(kHalfAxisNames)/sizeof(*kHalfAxisNames); ++i) {
        if (EqualsCaseInsensitive(kHalfAxisNames[i].name, token)) {
            out->kind = IN_AXIS_HALF; out->value = kHalfAxisNames[i].axis;
            out->half_sign = kHalfAxisNames[i].sign;
            return 1;
        }
    }
    return 0;
}

/* ---- Hotkey reservation (HOT-001, HOT-002) -------------------------------------------- */

/* Reserved no matter what the file says: the menu (Insert) and BB_PAD_RECORD (F9).
 * Escape is bindable: the overlay only uses it to close an open menu or cancel text entry,
 * and the pad is neutral while either is open (bbgpu_overlay_captures_input).
 * Used both for game-output bindings and for the hotkey lines themselves (HOT-001): F9 cannot
 * become hotkey_reload_inputs's key either, not just a cross/circle/... binding. */
static int IsUnconditionallyReserved(int32_t scancode) {
    return scancode == SDL_SCANCODE_INSERT || scancode == SDL_SCANCODE_F9;
}
/* Reserved for game-output bindings specifically: the above, plus whichever key currently
 * toggles the mouse or reloads input.ini (HOT-002). */
static int IsReservedScancode(int32_t scancode, const InputConfig *cfg) {
    if (IsUnconditionallyReserved(scancode)) return 1;
    if (cfg->toggle_scancode != SDL_SCANCODE_UNKNOWN && scancode == cfg->toggle_scancode) return 1;
    if (cfg->reload_scancode != SDL_SCANCODE_UNKNOWN && scancode == cfg->reload_scancode) return 1;
    return 0;
}

/* ---- Parser state ---------------------------------------------------------------------
 * A first pass resolves the two hotkey lines (so IsReservedScancode knows the final toggle/
 * reload keys before the main pass rejects game-output bindings on those keys), then a second
 * pass applies every other line. Both passes share the same tokenizer. */

typedef struct {
    InputConfig *cfg;
    int hotkeys_pass; /* 1: only hotkey_, mouse_ and analog_deadzone lines; 0: everything else */
} ParseState;

/* A duplicate line (same output, same input) has no extra effect and is not a warning
 * (spec section 9): "cross = space" twice must leave exactly one binding, not two identical
 * ones that would otherwise just double-count toward INPUT_MAX_BINDINGS_PER_OUTPUT. */
static int SameBinding(const InputBinding *a, const InputBinding *b) {
    return a->kind == b->kind && a->value == b->value &&
           (a->kind != IN_AXIS_HALF || a->half_sign == b->half_sign);
}
static void AddBinding(InputConfig *cfg, InputOutput out, InputBinding binding) {
    uint8_t *count = &cfg->table.binding_count[out];
    for (uint8_t i = 0; i < *count; ++i) {
        if (SameBinding(&cfg->table.bindings[out][i], &binding)) return;
    }
    if (*count >= INPUT_MAX_BINDINGS_PER_OUTPUT) { ++cfg->warnings; return; }
    cfg->table.bindings[out][*count] = binding;
    ++*count;
}

static float ParseFloatStrict(const char *s, int *ok) {
    char *end;
    double v = strtod(s, &end);
    *ok = (end != s && *end == '\0');
    return (float)v;
}

/* mouse_movement_params = deadzone_offset, speed, speed_offset (section 4.2). */
static void ParseMouseMovementParams(InputConfig *cfg, char *value) {
    char *a = value, *b = strchr(a, ','), *c = b ? strchr(b + 1, ',') : NULL;
    if (!b || !c) { ++cfg->warnings; return; }
    *b = '\0'; *c = '\0';
    char *sa = a, *sb = b + 1, *sc = c + 1;
    int ok1, ok2, ok3;
    float deadzone = ParseFloatStrict(sa, &ok1), speed = ParseFloatStrict(sb, &ok2), offset = ParseFloatStrict(sc, &ok3);
    if (!ok1 || !ok2 || !ok3 || deadzone < 0.0f || deadzone > 1.0f || speed < 0.01f || speed > 20.0f ||
        offset < 0.0f || offset > 1.0f) {
        ++cfg->warnings;
        return;
    }
    cfg->mouse.deadzone_offset = deadzone;
    cfg->mouse.speed = speed;
    cfg->mouse.speed_offset = offset;
}

/* mouse_to_joystick = left|right (section 4.2, MOU-001). */
static void ParseMouseToJoystick(InputConfig *cfg, const char *value) {
    if (EqualsCaseInsensitive(value, "left")) cfg->mouse.stick = 1;
    else if (EqualsCaseInsensitive(value, "right")) cfg->mouse.stick = 2;
    else ++cfg->warnings;
}

/* mouse_camera = direct|stick: direct writes the game's camera angles from mouse motion
 * (gpu/shim/bloodborne_cam.cpp, game 1.09), stick keeps the mouse-to-joystick emulation.
 * Direct still needs mouse_to_joystick (it is the stick used while locked on). */
static void ParseMouseCamera(InputConfig *cfg, const char *value) {
    if (EqualsCaseInsensitive(value, "direct")) cfg->mouse.camera_direct = 1;
    else if (EqualsCaseInsensitive(value, "stick")) cfg->mouse.camera_direct = 0;
    else ++cfg->warnings;
}

/* mouse_camera_sensitivity = multiplier for the direct camera (1.0 = 900 counts per radian). */
static void ParseMouseCameraSensitivity(InputConfig *cfg, const char *value) {
    int ok;
    float v = ParseFloatStrict(value, &ok);
    if (!ok || v < 0.05f || v > 20.0f) { ++cfg->warnings; return; }
    cfg->mouse.camera_sensitivity = v;
}

/* analog_deadzone = device, inner, outer (section 4.2, DZN-001). */
static void ParseAnalogDeadzone(InputConfig *cfg, char *value) {
    char *a = value, *b = strchr(a, ','), *c = b ? strchr(b + 1, ',') : NULL;
    if (!b || !c) { ++cfg->warnings; return; }
    *b = '\0'; *c = '\0';
    char *device = a, *si = b + 1, *so = c + 1;
    DeadzoneTarget target;
    if (EqualsCaseInsensitive(device, "leftjoystick")) target = DEADZONE_LEFT_STICK;
    else if (EqualsCaseInsensitive(device, "rightjoystick")) target = DEADZONE_RIGHT_STICK;
    else if (EqualsCaseInsensitive(device, "l2")) target = DEADZONE_L2;
    else if (EqualsCaseInsensitive(device, "r2")) target = DEADZONE_R2;
    else { ++cfg->warnings; return; }
    char *end1, *end2;
    long inner = strtol(si, &end1, 10), outer = strtol(so, &end2, 10);
    if (*end1 || *end2 || inner < 1 || inner > 127 || outer < 1 || outer > 127 || inner > outer) {
        ++cfg->warnings;
        return;
    }
    cfg->deadzone[target].inner = (int)inner;
    cfg->deadzone[target].outer = (int)outer;
}

/* One line, already stripped of whitespace and comments by ParseText. Mutates `line` (the
 * ':' suffix strip and the '=' split write into it), which is a private per-line copy.
 *
 * The file is walked twice (ParseState.hotkeys_pass): once to resolve the two hotkey lines
 * (so IsReservedScancode knows the final toggle/reload keys before any game-output binding is
 * checked against them), once for every other line. A warning must be counted exactly once
 * per malformed or rejected line, not once per pass -- so every early-exit below that counts a
 * warning is reached in exactly one of the two passes; this function never increments
 * cfg->warnings before deciding which pass currently owns the line. */
static void ParseLine(ParseState *state, char *line) {
    InputConfig *cfg = state->cfg;
    char *eq = strchr(line, '=');
    if (!eq) { if (!state->hotkeys_pass) ++cfg->warnings; return; }
    *eq = '\0';
    char *output_raw = line, *input_raw = eq + 1;
    StripColonSuffix(output_raw);
    StripColonSuffix(input_raw);
    if (!*output_raw || !*input_raw || strlen(output_raw) > NAME_MAX || strlen(input_raw) > NAME_MAX) {
        if (!state->hotkeys_pass) ++cfg->warnings;
        return;
    }

    /* Special multi-parameter lines (section 4.2): recognized in both passes so they are
     * consumed before the generic fallback below, but only acted on in the hotkeys pass
     * (arbitrarily chosen; they don't depend on hotkey resolution, so either pass would do). */
    if (EqualsCaseInsensitive(output_raw, "mouse_to_joystick")) {
        if (!state->hotkeys_pass) return;
        ParseMouseToJoystick(cfg, input_raw);
        return;
    }
    if (EqualsCaseInsensitive(output_raw, "mouse_movement_params")) {
        if (!state->hotkeys_pass) return;
        ParseMouseMovementParams(cfg, input_raw);
        return;
    }
    if (EqualsCaseInsensitive(output_raw, "analog_deadzone")) {
        if (!state->hotkeys_pass) return;
        ParseAnalogDeadzone(cfg, input_raw);
        return;
    }
    if (EqualsCaseInsensitive(output_raw, "mouse_camera")) {
        if (!state->hotkeys_pass) return;
        ParseMouseCamera(cfg, input_raw);
        return;
    }
    if (EqualsCaseInsensitive(output_raw, "mouse_camera_sensitivity")) {
        if (!state->hotkeys_pass) return;
        ParseMouseCameraSensitivity(cfg, input_raw);
        return;
    }

    /* Hotkey lines (HOT-002): resolved in the hotkeys pass so IsReservedScancode is accurate
     * for the main pass that follows. A combo here (comma) is rejected the same as any other
     * output, by the CFG-005 check below -- so this must run before that check consumes the
     * line in the hotkeys pass. */
    if (EqualsCaseInsensitive(output_raw, "hotkey_toggle_mouse_to_joystick") ||
        EqualsCaseInsensitive(output_raw, "hotkey_reload_inputs")) {
        if (!state->hotkeys_pass) return;
        if (strchr(input_raw, ',')) { ++cfg->warnings; return; }
        int32_t key;
        if (!LOOKUP(kKeyNames, input_raw, &key)) { ++cfg->warnings; return; }
        SDL_Scancode sc = SDL_GetScancodeFromKey((SDL_Keycode)key, NULL);
        if (sc == SDL_SCANCODE_UNKNOWN) { ++cfg->warnings; return; }
        if (IsUnconditionallyReserved((int32_t)sc)) { ++cfg->warnings; return; }
        if (EqualsCaseInsensitive(output_raw, "hotkey_toggle_mouse_to_joystick")) cfg->toggle_scancode = (int32_t)sc;
        else cfg->reload_scancode = (int32_t)sc;
        return;
    }
    if (state->hotkeys_pass) return; /* everything else is handled only in the main pass */

    /* CFG-005: one input per line; a comma here means a combo, which is out of scope. Checked
     * only in the main pass (reached once per line, unlike the hotkey branch above). */
    if (strchr(input_raw, ',')) { ++cfg->warnings; return; }

    InputOutput out;
    if (!input_output_from_name(output_raw, &out)) {
        /* CFG-006: either an output this port never supports (key_toggle, the shadPS4 hotkeys
         * other than the two handled above, override_controller_color, ...) or a genuinely
         * unknown name -- both are a single warning, and the rest of the file keeps loading. */
        ++cfg->warnings;
        return;
    }
    InputBinding binding;
    if (!ResolveInput(input_raw, &binding)) { ++cfg->warnings; return; }
    if (binding.kind == IN_NONE) return; /* "unmapped": explicitly no binding, not a warning */

    /* HOT-001: a game-output binding cannot use a reserved key. */
    if (binding.kind == IN_KEY && IsReservedScancode(binding.value, cfg)) { ++cfg->warnings; return; }

    AddBinding(cfg, out, binding);
}

static void ParseTextPass(ParseState *state, const char *text) {
    const char *p = text;
    char raw[LINE_MAX + 2];
    while (*p) {
        size_t n = 0;
        while (*p && *p != '\n' && n < sizeof(raw) - 1) raw[n++] = *p++;
        while (*p && *p != '\n') ++p; /* GUD-004: discard anything past the line cap */
        if (*p == '\n') ++p;
        raw[n] = '\0';

        /* CFG-003: strip all whitespace, then truncate at '#'. */
        char line[LINE_MAX + 2];
        size_t w = 0;
        for (size_t i = 0; i < n; ++i) if (!isspace((unsigned char)raw[i])) line[w++] = raw[i];
        line[w] = '\0';
        char *hash = strchr(line, '#');
        if (hash) *hash = '\0';
        if (!*line) continue;

        ParseLine(state, line);
    }
}

void input_config_parse(InputConfig *out, const char *text) {
    memset(out, 0, sizeof(*out));
    out->mouse.deadzone_offset = 0.5f;
    out->mouse.speed = 1.0f;
    out->mouse.speed_offset = 0.125f;
    out->mouse.stick = 0;
    out->mouse.camera_direct = 0;
    out->mouse.camera_sensitivity = 1.0f;
    for (int i = 0; i < DEADZONE_COUNT; ++i) { out->deadzone[i].inner = 1; out->deadzone[i].outer = 127; }
    out->toggle_scancode = SDL_SCANCODE_F7;
    out->reload_scancode = SDL_SCANCODE_F8;

    ParseState state = {out, 1};
    ParseTextPass(&state, text); /* pass 1: hotkeys + special lines, so reservation is known */
    state.hotkeys_pass = 0;
    ParseTextPass(&state, text); /* pass 2: button/axis/mode bindings */
}

void input_config_defaults(InputConfig *out) {
    input_config_parse(out, input_config_default_text());
}

/* ---- Path and file I/O (CFG-001, CFG-002) --------------------------------------------- */

int input_config_path(char *out, size_t out_size) {
    const char *env = getenv("BB_INPUT_CONFIG");
    if (env && *env) {
        if (strlen(env) >= out_size) return 0;
        strcpy(out, env);
        return 1;
    }
    const char *config = getenv("BB_CONFIG");
    if (!config || !*config) config = "bbport.ini";
    const char *slash = strrchr(config, '/');
#ifdef _WIN32
    const char *bslash = strrchr(config, '\\');
    if (!slash || (bslash && bslash > slash)) slash = bslash;
#endif
    size_t dir_len = slash ? (size_t)(slash - config) : 0;
    const char *suffix = "input.ini";
    size_t needed = dir_len + (dir_len ? 1 : 0) + strlen(suffix) + 1;
    if (needed > out_size) return 0;
    if (dir_len) { memcpy(out, config, dir_len); out[dir_len] = '/'; strcpy(out + dir_len + 1, suffix); }
    else strcpy(out, suffix);
    return 1;
}

int input_config_load(InputConfig *out, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        FILE *w = fopen(path, "wb");
        if (w) {
            fputs(input_config_default_text(), w);
            fclose(w);
            f = fopen(path, "rb");
        }
        if (!f) {
            fprintf(stderr, "Input config: cannot create or open %s; using built-in defaults\n", path);
            input_config_defaults(out);
            return 0;
        }
    }
    char *text = NULL;
    size_t cap = 0, len = 0;
    char chunk[4096];
    size_t n;
    while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0) {
        if (len + n + 1 > cap) {
            cap = (len + n + 1) * 2;
            char *grown = realloc(text, cap);
            if (!grown) { free(text); fclose(f); input_config_defaults(out); return 0; }
            text = grown;
        }
        memcpy(text + len, chunk, n);
        len += n;
    }
    fclose(f);
    if (!text) { text = malloc(1); if (!text) { input_config_defaults(out); return 0; } len = 0; }
    text[len] = '\0';
    input_config_parse(out, text);
    free(text);
    return 1;
}

/* ---- Default file text (section 4.3) -------------------------------------------------- */

const char *input_config_default_text(void) {
    return
"# bbport input (shadPS4 syntax: output = input, one input per line).\n"
"# A shadPS4 input config (user/input_config/CUSA03173.ini) can be copied over this file.\n"
"# F7 toggles the mouse, F8 reloads this file.\n"
"\n"
"# Keyboard\n"
"cross = space\n"
"circle = lshift\n"
"square = e\n"
"triangle = v\n"
"l1 = 1\n"
"r1 = 3\n"
"l2 = r\n"
"r2 = f\n"
"l3 = z\n"
"r3 = q\n"
"r3 = c\n"
"options = enter\n"
"pad_up = i\n"
"pad_down = k\n"
"pad_left = j\n"
"pad_right = l\n"
"touchpad_left = tab\n"
"touchpad_right = backspace\n"
"\n"
"axis_left_x_minus = a\n"
"axis_left_x_plus = d\n"
"axis_left_y_minus = w\n"
"axis_left_y_plus = s\n"
"axis_right_x_minus = left\n"
"axis_right_x_plus = right\n"
"axis_right_y_minus = up\n"
"axis_right_y_plus = down\n"
"\n"
"# Hold to halve the left stick (walk): uncomment and pick a key\n"
"# leftjoystick_halfmode = lalt\n"
"\n"
"# Mouse (uncomment mouse_to_joystick to turn it on; buttons work only while captured)\n"
"# mouse_to_joystick = right\n"
"mouse_movement_params = 0.5, 1, 0.125\n"
"r1 = leftbutton\n"
"r2 = rightbutton\n"
"circle = sidebuttonback\n"
"square = sidebuttonforward\n"
"\n"
"# Controller\n"
"cross = cross\n"
"circle = circle\n"
"square = square\n"
"triangle = triangle\n"
"l1 = l1\n"
"r1 = r1\n"
"l2 = l2\n"
"r2 = r2\n"
"l3 = l3\n"
"r3 = r3\n"
"options = options\n"
"touchpad_left = back\n"
"pad_up = pad_up\n"
"pad_down = pad_down\n"
"pad_left = pad_left\n"
"pad_right = pad_right\n"
"axis_left_x = axis_left_x\n"
"axis_left_y = axis_left_y\n"
"axis_right_x = axis_right_x\n"
"axis_right_y = axis_right_y\n"
"\n"
"# Hotkeys\n"
"hotkey_toggle_mouse_to_joystick = f7\n"
"hotkey_reload_inputs = f8\n";
}
