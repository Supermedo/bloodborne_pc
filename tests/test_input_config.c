/* SPDX-License-Identifier: GPL-2.0-or-later
 * Unit tests for src/runtime_input_config.c: parsing only, no window/GPU. Covers the parser
 * rules (CFG-003..006), name tables (NAM-001..005), hotkey reservation (HOT-001, HOT-002) and
 * the file path/load fallback (CFG-001, CFG-002), per
 * specs/keyboard-and-mouse/spec-design-keyboard-mouse-input.md section 6. */
#define _GNU_SOURCE
#include "../src/runtime_input_config.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <SDL3/SDL.h>

static int BindingCountFor(const InputConfig *cfg, InputOutput out) {
    return cfg->table.binding_count[out];
}
static int HasKeyBinding(const InputConfig *cfg, InputOutput out, SDL_Scancode sc) {
    for (int i = 0; i < cfg->table.binding_count[out]; ++i) {
        const InputBinding *b = &cfg->table.bindings[out][i];
        if (b->kind == IN_KEY && b->value == (int32_t)sc) return 1;
    }
    return 0;
}
static int HasCButtonBinding(const InputConfig *cfg, InputOutput out, int button) {
    for (int i = 0; i < cfg->table.binding_count[out]; ++i) {
        const InputBinding *b = &cfg->table.bindings[out][i];
        if (b->kind == IN_CBUTTON && b->value == button) return 1;
    }
    return 0;
}

/* AC-001-equivalent at the parser level: one binding, exactly the key asked for. */
static void TestBasicBinding(void) {
    InputConfig cfg;
    input_config_parse(&cfg, "cross = j\n");
    assert(BindingCountFor(&cfg, OUT_CROSS) == 1);
    assert(HasKeyBinding(&cfg, OUT_CROSS, SDL_SCANCODE_J));
    assert(!HasKeyBinding(&cfg, OUT_CROSS, SDL_SCANCODE_SPACE));
}

/* AC-002: an invalid input value is ignored with a warning; other lines still load. */
static void TestInvalidValueWarnsAndContinues(void) {
    InputConfig cfg;
    input_config_parse(&cfg, "cross = banana\nsquare = e\n");
    assert(cfg.warnings == 1);
    assert(BindingCountFor(&cfg, OUT_CROSS) == 0);
    assert(HasKeyBinding(&cfg, OUT_SQUARE, SDL_SCANCODE_E));
}

/* AC-003: a binding onto a reserved key (Insert: the menu) is rejected; Insert stays free. */
static void TestReservedKeyRejected(void) {
    InputConfig cfg;
    input_config_parse(&cfg, "r1 = insert\n");
    assert(cfg.warnings == 1);
    assert(BindingCountFor(&cfg, OUT_R1) == 0);
}

/* HOT-002: F9 (BB_PAD_RECORD) cannot be reassigned as the mouse-toggle hotkey. */
static void TestReservedF9CannotBecomeHotkey(void) {
    InputConfig cfg;
    input_config_parse(&cfg, "hotkey_reload_inputs = f9\n");
    assert(cfg.warnings == 1);
    assert(cfg.reload_scancode == SDL_SCANCODE_F8); /* default kept */
}

/* HOT-002: remapping the toggle hotkey frees its old key for ordinary bindings. */
static void TestHotkeyRemapFreesDefaultKey(void) {
    InputConfig cfg;
    input_config_parse(&cfg, "hotkey_toggle_mouse_to_joystick = f6\ncross = f7\n");
    assert(cfg.toggle_scancode == SDL_SCANCODE_F6);
    assert(cfg.warnings == 0);
    assert(HasKeyBinding(&cfg, OUT_CROSS, SDL_SCANCODE_F7));
}

/* HOT-002: the *current* toggle hotkey (even default F7) is reserved for game outputs. */
static void TestDefaultHotkeyKeyReserved(void) {
    InputConfig cfg;
    input_config_parse(&cfg, "cross = f7\n");
    assert(cfg.warnings == 1);
    assert(BindingCountFor(&cfg, OUT_CROSS) == 0);
}

/* CFG-003: whitespace is stripped, '#' starts a comment, case does not matter. */
static void TestWhitespaceCommentsAndCase(void) {
    InputConfig cfg;
    input_config_parse(&cfg, "  Cross = SPACE   # inline comment\n# full line comment\n\n");
    assert(cfg.warnings == 0);
    assert(HasKeyBinding(&cfg, OUT_CROSS, SDL_SCANCODE_SPACE));
}

/* CFG-003: a ':N' gamepad-index suffix on either side is accepted and discarded. */
static void TestColonSuffixDiscarded(void) {
    InputConfig cfg;
    input_config_parse(&cfg, "r1:1 = r1:2\n");
    assert(cfg.warnings == 0);
    assert(HasCButtonBinding(&cfg, OUT_R1, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER));
}

/* CFG-005: a comma in the input (a combo) is out of scope and warned, not silently accepted
 * as a single bad token. */
static void TestComboInputWarns(void) {
    InputConfig cfg;
    input_config_parse(&cfg, "l2 = lshift,mousewheelup\n");
    assert(cfg.warnings == 1);
    assert(BindingCountFor(&cfg, OUT_L2) == 0);
}

/* CFG-006: an output this port does not support (key_toggle) is skipped with one warning, and
 * does not stop the rest of the file from loading -- this is what lets a shadPS4 file import. */
static void TestUnsupportedOutputWarnsAndContinues(void) {
    InputConfig cfg;
    input_config_parse(&cfg, "key_toggle = f1,w\ncross = n\n");
    assert(cfg.warnings == 1);
    assert(HasKeyBinding(&cfg, OUT_CROSS, SDL_SCANCODE_N));
}

/* NAM-005: "unmapped" creates no binding and is not a warning. */
static void TestUnmappedIsNotAWarning(void) {
    InputConfig cfg;
    input_config_parse(&cfg, "cross = unmapped\n");
    assert(cfg.warnings == 0);
    assert(BindingCountFor(&cfg, OUT_CROSS) == 0);
}

/* A duplicate line has no extra effect and is not a warning. */
static void TestDuplicateLineHarmless(void) {
    InputConfig cfg;
    input_config_parse(&cfg, "cross = space\ncross = space\n");
    assert(cfg.warnings == 0);
    assert(BindingCountFor(&cfg, OUT_CROSS) == 1);
}

/* Section 4.2: mouse_to_joystick, with an invalid value leaving the mode absent. */
static void TestMouseToJoystick(void) {
    InputConfig cfg;
    input_config_parse(&cfg, "mouse_to_joystick = right\n");
    assert(cfg.mouse.stick == 2);
    assert(cfg.warnings == 0);

    InputConfig bad;
    input_config_parse(&bad, "mouse_to_joystick = both\n");
    assert(bad.mouse.stick == 0);
    assert(bad.warnings == 1);
}

/* Section 4.2: mouse_movement_params, including the range check (AC-016 depends on this). */
static void TestMouseMovementParams(void) {
    InputConfig cfg;
    input_config_parse(&cfg, "mouse_movement_params = 0.4, 1.5, 0.1\n");
    assert(cfg.warnings == 0);
    assert(cfg.mouse.deadzone_offset > 0.39f && cfg.mouse.deadzone_offset < 0.41f);
    assert(cfg.mouse.speed > 1.49f && cfg.mouse.speed < 1.51f);
    assert(cfg.mouse.speed_offset > 0.09f && cfg.mouse.speed_offset < 0.11f);

    InputConfig incomplete;
    input_config_parse(&incomplete, "mouse_movement_params = 0.5, 1\n");
    assert(incomplete.warnings == 1);
    assert(incomplete.mouse.speed > 0.99f && incomplete.mouse.speed < 1.01f); /* default kept */
}

/* Section 4.2: analog_deadzone, including inner > outer rejection. */
static void TestAnalogDeadzone(void) {
    InputConfig cfg;
    input_config_parse(&cfg, "analog_deadzone = leftjoystick, 5, 120\n");
    assert(cfg.warnings == 0);
    assert(cfg.deadzone[DEADZONE_LEFT_STICK].inner == 5);
    assert(cfg.deadzone[DEADZONE_LEFT_STICK].outer == 120);

    InputConfig bad;
    input_config_parse(&bad, "analog_deadzone = leftjoystick, 120, 5\n");
    assert(bad.warnings == 1);
    assert(bad.deadzone[DEADZONE_LEFT_STICK].inner == 1); /* default kept */
}

/* GUD-004: a name beyond NAME_MAX is rejected rather than silently truncated into a false match. */
static void TestOverlongNameRejected(void) {
    InputConfig cfg;
    input_config_parse(&cfg, "cross = aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\n");
    assert(cfg.warnings == 1);
    assert(BindingCountFor(&cfg, OUT_CROSS) == 0);
}

/* GUD-004: a line far beyond LINE_MAX does not overflow the parser's stack buffers; the whole
 * line is discarded past the cap and treated as malformed (no crash, one warning). */
static void TestOverlongLineDoesNotCrash(void) {
    char text[2048];
    size_t n = 0;
    memcpy(text + n, "cross", 5); n += 5;
    text[n++] = '=';
    for (int i = 0; i < 2000; ++i) text[n++] = 'x';
    text[n++] = '\n';
    text[n] = '\0';
    InputConfig cfg;
    input_config_parse(&cfg, text); /* must not crash or overrun */
    assert(cfg.warnings >= 1);
}

/* Half-axis names resolve to the right axis and sign (used by runtime_pad.c, T2). */
static void TestHalfAxisNames(void) {
    InputConfig cfg;
    input_config_parse(&cfg, "axis_left_x_minus = axis_left_x_minus\naxis_left_x_plus = axis_left_x_plus\n");
    assert(cfg.warnings == 0);
    assert(cfg.table.binding_count[OUT_AXIS_LEFT_X_MINUS] == 1);
    assert(cfg.table.bindings[OUT_AXIS_LEFT_X_MINUS][0].kind == IN_AXIS_HALF);
    assert(cfg.table.bindings[OUT_AXIS_LEFT_X_MINUS][0].half_sign == -1);
    assert(cfg.table.bindings[OUT_AXIS_LEFT_X_PLUS][0].half_sign == 1);
}

/* The compiled-in default file must itself parse with only the one documented warning
 * (override_controller_color is not emitted by our own default, so the true baseline is 0;
 * this guards against the default text silently drifting out of sync with the parser). */
static void TestDefaultTextParsesCleanly(void) {
    InputConfig cfg;
    input_config_parse(&cfg, input_config_default_text());
    assert(cfg.warnings == 0);
    assert(HasKeyBinding(&cfg, OUT_CROSS, SDL_SCANCODE_SPACE));
    assert(HasCButtonBinding(&cfg, OUT_CROSS, SDL_GAMEPAD_BUTTON_SOUTH));
    assert(cfg.mouse.stick == 0); /* mouse_to_joystick left commented out */
    assert(cfg.toggle_scancode == SDL_SCANCODE_F7);
    assert(cfg.reload_scancode == SDL_SCANCODE_F8);
}

/* AC-008: a real shadPS4 default input config loads without error; only
 * override_controller_color (and nothing else) should warn. */
static void TestShadps4DefaultFileImports(void) {
    FILE *f = fopen("tests/data/shadps4_default_input.ini", "rb");
    assert(f);
    char text[8192];
    size_t n = fread(text, 1, sizeof(text) - 1, f);
    fclose(f);
    text[n] = '\0';
    InputConfig cfg;
    input_config_parse(&cfg, text);
    assert(cfg.warnings == 1); /* override_controller_color only */
    assert(HasKeyBinding(&cfg, OUT_TRIANGLE, SDL_SCANCODE_KP_8));
    assert(HasCButtonBinding(&cfg, OUT_CROSS, SDL_GAMEPAD_BUTTON_SOUTH));
}

/* CFG-002: a missing file is created with the default text and still loads correctly. */
static void TestLoadCreatesMissingFile(void) {
    char path[] = "/tmp/bbport-input-test-XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    close(fd);
    unlink(path); /* exists as a name, not as a file, like a fresh install */
    InputConfig cfg;
    int created = input_config_load(&cfg, path);
    assert(created);
    assert(HasKeyBinding(&cfg, OUT_CROSS, SDL_SCANCODE_SPACE));
    FILE *f = fopen(path, "rb");
    assert(f);
    fclose(f);
    unlink(path);
}

/* CFG-001: BB_INPUT_CONFIG wins over a bbport.ini-derived path. */
static void TestPathPrefersEnvOverride(void) {
    char out[512];
    setenv("BB_INPUT_CONFIG", "/tmp/explicit-input.ini", 1);
    assert(input_config_path(out, sizeof(out)));
    assert(!strcmp(out, "/tmp/explicit-input.ini"));
    unsetenv("BB_INPUT_CONFIG");

    setenv("BB_CONFIG", "/tmp/data/bbport.ini", 1);
    assert(input_config_path(out, sizeof(out)));
    assert(!strcmp(out, "/tmp/data/input.ini"));
    unsetenv("BB_CONFIG");

    assert(input_config_path(out, sizeof(out)));
    assert(!strcmp(out, "input.ini")); /* no env at all: alongside a bare "bbport.ini" */
}

int main(void) {
    TestBasicBinding();
    TestInvalidValueWarnsAndContinues();
    TestReservedKeyRejected();
    TestReservedF9CannotBecomeHotkey();
    TestHotkeyRemapFreesDefaultKey();
    TestDefaultHotkeyKeyReserved();
    TestWhitespaceCommentsAndCase();
    TestColonSuffixDiscarded();
    TestComboInputWarns();
    TestUnsupportedOutputWarnsAndContinues();
    TestUnmappedIsNotAWarning();
    TestDuplicateLineHarmless();
    TestMouseToJoystick();
    TestMouseMovementParams();
    TestAnalogDeadzone();
    TestOverlongNameRejected();
    TestOverlongLineDoesNotCrash();
    TestHalfAxisNames();
    TestDefaultTextParsesCleanly();
    TestShadps4DefaultFileImports();
    TestLoadCreatesMissingFile();
    TestPathPrefersEnvOverride();
    puts("test_input_config: all tests passed");
    return 0;
}
