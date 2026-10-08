#define _GNU_SOURCE
#include <assert.h>
#include <unistd.h>
#include "../src/runtime_pad.c"

static int capture;
int bbgpu_overlay_captures_input(void) { return capture; }
uintptr_t runtime_lookup(const RuntimeExport *table, size_t count, const char *name) {
    (void)table; (void)count; (void)name;
    return 0;
}
/* T4's mouse/hotkey API lives in the GPU library (gpu/shim/bbgpu.cpp), not linked into this
 * C-only test; stubbed as "no mouse, nothing to reload" so runtime_pad.c's calls into it are
 * harmless here. T5 will need a controllable stub instead, to test the mouse-to-stick path. */
/* Controlled by the tests below via stub_mouse (T5): BbMouseInput as the window thread would
 * report it. Zero/not captured by default, matching "mouse_enable off" (MOU-001/MOU-013). */
static BbMouseInput stub_mouse;
void bbgpu_mouse_take(BbMouseInput *out) {
    if (!out) return;
    *out=stub_mouse;
    stub_mouse.dx=stub_mouse.dy=0.0f; /* MOU-008: motion is drained on every take, like the real one */
}
void bbgpu_input_configure(int mouse_mode_available, int32_t toggle_scancode, int32_t reload_scancode) {
    (void)mouse_mode_available; (void)toggle_scancode; (void)reload_scancode;
}
/* T6: controlled by the tests via stub_reload_requested, draining to 0 on read like the real
 * one (bbgpu_input_reload_requested's own doc comment: "1 once ... then clears back to 0"). */
static int stub_reload_requested;
int bbgpu_input_reload_requested(void) {
    int r=stub_reload_requested;
    stub_reload_requested=0;
    return r;
}

static void inject(const char *path, const char *tokens) {
    FILE *f=fopen(path,"w");
    assert(f);
    fputs(tokens,f);
    fclose(f);
    usleep(25000);
}

/* T2/T3: the binding-evaluation logic itself, with a HostState built directly (SDL's `dummy`
 * video driver cannot deliver real key presses, so this is the only deterministic way to
 * exercise a *held* key/button/axis -- test_custom_binding_config below only proves a custom
 * input.ini loads without crashing, not that a binding fires). */
static void test_binding_evaluation(void) {
    InputConfig cfg;
    input_config_parse(&cfg,
        "cross = j\n"
        "cross = a\n"           /* two bindings on the same output: either fires it (MIX-002) */
        "axis_left_x_minus = a\n"
        "axis_left_x_plus = d\n"
        "analog_deadzone = leftjoystick, 10, 100\n"
        "leftjoystick_halfmode = lctrl\n");
    assert(cfg.warnings==0);

    bool keys[SDL_SCANCODE_COUNT]={0};
    HostState host={keys,NULL,0,0};

    /* MIX-002: cross has two bindings (j, a); holding either fires it, holding neither doesn't. */
    assert(!button_output_held(&host,&cfg,OUT_CROSS));
    keys[SDL_SCANCODE_J]=true;
    assert(button_output_held(&host,&cfg,OUT_CROSS));
    keys[SDL_SCANCODE_J]=false; keys[SDL_SCANCODE_A]=true;
    assert(button_output_held(&host,&cfg,OUT_CROSS));
    keys[SDL_SCANCODE_A]=false;

    /* DZN-001: below inner (10), the stick is 0; a is also axis_left_x_minus, so it is a full
     * -127 contribution before the deadzone, which must then clamp it to -127 (well above 100,
     * the outer bound) -- i.e. a full key press always saturates past any sane deadzone. */
    int lx=apply_deadzone(axis_output_value(&host,&cfg,OUT_AXIS_LEFT_X_MINUS,-1)
                          +axis_output_value(&host,&cfg,OUT_AXIS_LEFT_X_PLUS,1),10,100);
    assert(lx==0); /* a released: no contribution at all */
    keys[SDL_SCANCODE_A]=true;
    lx=apply_deadzone(axis_output_value(&host,&cfg,OUT_AXIS_LEFT_X_MINUS,-1)
                      +axis_output_value(&host,&cfg,OUT_AXIS_LEFT_X_PLUS,1),10,100);
    assert(lx==-127);
    keys[SDL_SCANCODE_A]=false;

    /* DZN-001 ramp, exact value: deadzone(60,10,100) = 127*(60-10)/(100-10) = 70. */
    assert(apply_deadzone(60,10,100)==70);
    assert(apply_deadzone(-60,10,100)==-70);
    assert(apply_deadzone(10,10,100)==0);   /* at inner: still 0 */
    assert(apply_deadzone(100,10,100)==127); /* at outer: fully saturated */

    /* HLF-001: halfmode is just another button output; the halving itself happens in
     * sample_host, so here we only confirm the binding resolves. */
    keys[SDL_SCANCODE_LCTRL]=true;
    assert(button_output_held(&host,&cfg,OUT_LEFTJOYSTICK_HALFMODE));
    keys[SDL_SCANCODE_LCTRL]=false;
    assert(!button_output_held(&host,&cfg,OUT_LEFTJOYSTICK_HALFMODE));

    puts("PASS: binding evaluation (remap OR, deadzone ramp and saturation, halfmode binding)");
}

/* T5: the EmulateJoystick-derived formula (CNV-001..003, MOU-009), independent of SDL/pad
 * plumbing -- mouse_to_axis is deterministic given (dx, dy, dt_ms, params), so this is testable
 * without a real mouse or a running window thread. */
static void test_mouse_to_axis(void) {
    MouseParams mouse={.deadzone_offset=0.5f,.speed=1.0f,.speed_offset=0.125f,.stick=2};
    int x=99, y=99;

    /* CNV-002: no motion -> no contribution, regardless of dt. */
    mouse_to_axis(0.0f,0.0f,33.0,&mouse,&x,&y);
    assert(x==0 && y==0);

    /* dx=10 at dt=33ms (no normalization). speed = (10*1+16) * (1-exp(-10/9.6)) = ~16.83,
     * rounds to x=17. The exact value matters less than that it sits strictly between the
     * "no motion" (0) and "fully linear" (26 = 10*1+16) cases: proof the ramp is doing
     * something, not a specific tuning to defend. */
    mouse_to_axis(10.0f,0.0f,33.0,&mouse,&x,&y);
    assert(x==17 && y==0);

    /* CNV-003: the same physical speed (px/ms) at a different dt must give the same
     * contribution once normalized to the 33 ms window -- dx=10 over 33ms == dx=2.1212... over
     * 7ms, normalized back to 33ms's worth of motion. Tolerance +/-2 units for the rounding. */
    mouse_to_axis(10.0f*7.0f/33.0f,0.0f,7.0,&mouse,&x,&y);
    assert(x>=15 && x<=19 && y==0);

    /* The fix this formula exists for: a tiny movement must produce a small, nonzero,
     * proportionate contribution -- never a jump to some fixed floor, and never a jump to
     * a second fixed value further out either (the bug in the first version of this fix,
     * reported in the same session as "ainda sinto um pouco de degrau": the curve approached
     * the floor only asymptotically and so never actually met the line it switched to at the
     * crossing magnitude -- this version has no branch to jump between). dx=1 at dt=33ms:
     * speed = (1+16) * (1-exp(-1/9.6)) = ~1.68, rounds to x=2. */
    mouse_to_axis(1.0f,0.0f,33.0,&mouse,&x,&y);
    assert(x>=1 && x<=3 && y==0);

    /* A large movement must converge onto shadPS4's own linear response (the ramp factor is
     * within 1% of 1.0 well before this magnitude): dx=100 at dt=33ms: linear = 100+16 = 116,
     * ramp(100) = 1-exp(-100/9.6) ~= 0.99994, so speed ~= 115.997, rounds to x=116. */
    mouse_to_axis(100.0f,0.0f,33.0,&mouse,&x,&y);
    assert(x==116 && y==0);

    /* No discontinuity anywhere between a tiny and a large movement: sampling magnitude in
     * small steps must never show the output jump by much more than the step itself, at any
     * point along the curve -- this is the actual regression test for the "degrau" bug (a
     * spot check at one or two magnitudes would not have caught it; the bug was a jump between
     * two specific points, not a wrong value at any single one). */
    {
        int prev_x = 99, prev_set = 0;
        for (float m = 0.0f; m <= 200.0f; m += 0.5f) {
            mouse_to_axis(m,0.0f,33.0,&mouse,&x,&y);
            if (prev_set) {
                int step = x - prev_x;
                assert(step >= 0 && step <= 2); /* monotonic, no jump beyond what 0.5 magnitude units could cause */
            }
            prev_x = x; prev_set = 1;
        }
    }

    /* Diagonal motion: direction preserved via atan2, magnitude still clamped to 128 then the
     * +/-127 cast. A large deflection saturates both axes toward the 45-degree corner. */
    mouse_to_axis(1000.0f,1000.0f,33.0,&mouse,&x,&y);
    assert(x>=89 && x<=91 && y>=89 && y<=91); /* 127/sqrt(2) ~= 89.8 */

    puts("PASS: mouse_to_axis (EmulateJoystick formula: deadzone floor, dt normalization, saturation)");
}

/* T5: mouse buttons and wheel through the full binding path -- bbgpu_mouse_take, captured vs.
 * not, the index-vs-mask distinction (NAM-002/API note) and the wheel's bit layout. */
static void test_mouse_buttons_and_wheel(void) {
    InputConfig cfg;
    input_config_parse(&cfg,
        "r1 = leftbutton\n"
        "r2 = rightbutton\n"
        "pad_up = mousewheelup\n");
    assert(cfg.warnings==0);

    HostState host={NULL,NULL,0,0};
    assert(!button_output_held(&host,&cfg,OUT_R1));

    /* IN_MOUSE_BUTTON stores the SDL_BUTTON_* index (1=left); host.mouse_buttons must be
     * compared as the SDL_BUTTON_MASK() bit, not the raw index -- this is exactly the
     * conversion T4's header warns T5 to apply. */
    host.mouse_buttons=SDL_BUTTON_MASK(SDL_BUTTON_LEFT);
    assert(button_output_held(&host,&cfg,OUT_R1));
    assert(!button_output_held(&host,&cfg,OUT_R2)); /* right not held */
    host.mouse_buttons=SDL_BUTTON_MASK(SDL_BUTTON_RIGHT);
    assert(!button_output_held(&host,&cfg,OUT_R1));
    assert(button_output_held(&host,&cfg,OUT_R2));

    host.mouse_buttons=0;
    host.mouse_wheel=1u<<0; /* WHEEL_UP */
    assert(button_output_held(&host,&cfg,OUT_PAD_UP));
    host.mouse_wheel=1u<<1; /* WHEEL_DOWN: must not fire pad_up */
    assert(!button_output_held(&host,&cfg,OUT_PAD_UP));

    puts("PASS: mouse buttons and wheel (index->mask conversion, wheel bit layout)");
}

/* T5 end to end: a captured mouse moving the right stick through sample_host/pad_read_state,
 * and MOU-006 (mouse bindings are inert while bbgpu_mouse_take reports captured=0). */
static void test_mouse_end_to_end(void) {
    char config_path[]="/tmp/bbport-pad-test-mouse-XXXXXX";
    int fd=mkstemp(config_path);
    assert(fd>=0);
    FILE *f=fdopen(fd,"w");
    assert(f);
    fputs("mouse_to_joystick = right\n"
          "mouse_movement_params = 0.5, 1, 0.125\n"
          "r1 = leftbutton\n",f);
    fclose(f);
    setenv("BB_INPUT_CONFIG",config_path,1);
    free(loaded_config); config_initialized=0; loaded_config=NULL;

    PadData data;
    stub_mouse=(BbMouseInput){.dx=0.0f,.dy=0.0f,.buttons=0,.wheel=0,.captured=0};
    assert(pad_read_state(1,&data)==0);
    assert(data.right_x==128 && data.right_y==128); /* not captured: no contribution */
    assert(!(data.buttons & BTN_R1)); /* MOU-006: mouse button inert while not captured */

    stub_mouse=(BbMouseInput){.dx=50.0f,.dy=0.0f,.buttons=SDL_BUTTON_MASK(SDL_BUTTON_LEFT),.wheel=0,.captured=1};
    assert(pad_read_state(1,&data)==0);
    assert(data.right_x>128); /* captured, moving right: stick deflects positive X */
    assert(data.buttons & BTN_R1); /* captured: mouse button now fires */

    stub_mouse=(BbMouseInput){0};
    unsetenv("BB_INPUT_CONFIG");
    unlink(config_path);
    free(loaded_config); config_initialized=0; loaded_config=NULL;
    puts("PASS: mouse end to end through pad_read_state (captured gate, right-stick deflection)");
}

/* T2: a custom input.ini end to end through pad_read_state -- proves the file is actually
 * found, parsed and wired into sample_host via BB_INPUT_CONFIG, not just that
 * test_binding_evaluation's lower-level calls work in isolation. Keys cannot be held under the
 * dummy driver, so this only checks "loads and produces a neutral, centered read", which is
 * still a real regression guard: a bug in ensure_config_loaded/active_config that crashed, hung,
 * or left stale bindings from the earlier test would show up here. */
static void test_custom_binding_config(void) {
    char config_path[]="/tmp/bbport-pad-test-config-XXXXXX";
    int fd=mkstemp(config_path);
    assert(fd>=0);
    FILE *f=fdopen(fd,"w");
    assert(f);
    fputs("cross = j\n"
          "axis_left_x_minus = a\n"
          "axis_left_x_plus = d\n"
          "analog_deadzone = leftjoystick, 10, 100\n"
          "leftjoystick_halfmode = lctrl\n",f);
    fclose(f);
    setenv("BB_INPUT_CONFIG",config_path,1);
    free(loaded_config); config_initialized=0; loaded_config=NULL; /* force a reload from this path */

    PadData data;
    assert(pad_read_state(1,&data)==0);
    assert(!(data.buttons & BTN_CROSS));
    assert(data.left_x==128 && data.left_y==128);

    unsetenv("BB_INPUT_CONFIG");
    unlink(config_path);
    free(loaded_config); config_initialized=0; loaded_config=NULL; /* restore defaults below */
    puts("PASS: custom input.ini loads end to end through pad_read_state");
}

/* T6/AC-018: editing input.ini and signaling F8 (bbgpu_input_reload_requested) changes the
 * live binding without restarting the process -- the actual behavior "F8 reloads", not just
 * that the two pieces (input_config_load, reload_config_if_requested) exist in isolation. */
static void test_reload_on_f8(void) {
    char config_path[]="/tmp/bbport-pad-test-reload-XXXXXX";
    int fd=mkstemp(config_path);
    assert(fd>=0);
    FILE *f=fdopen(fd,"w");
    assert(f);
    fputs("cross = j\n",f); /* cross NOT bound to space */
    fclose(f);
    setenv("BB_INPUT_CONFIG",config_path,1);
    free(loaded_config); config_initialized=0; loaded_config=NULL;

    PadData data;
    assert(pad_read_state(1,&data)==0); /* loads the file above via ensure_config_loaded */
    bool before_has_space=false;
    for (int i=0;i<loaded_config->table.binding_count[OUT_CROSS];++i) {
        const InputBinding *b=&loaded_config->table.bindings[OUT_CROSS][i];
        if (b->kind==IN_KEY && b->value==SDL_SCANCODE_SPACE) before_has_space=true;
    }
    assert(!before_has_space);

    f=fopen(config_path,"w"); /* edit the file in place, as a player would with F8 */
    assert(f);
    fputs("cross = space\n",f);
    fclose(f);
    stub_reload_requested=1; /* simulate F8: the window thread would set this */
    assert(pad_read_state(1,&data)==0); /* reload_config_if_requested runs at the top of sample() */
    assert(stub_reload_requested==0); /* drained, like the real one (CFG-008) */
    bool after_has_space=false;
    for (int i=0;i<loaded_config->table.binding_count[OUT_CROSS];++i) {
        const InputBinding *b=&loaded_config->table.bindings[OUT_CROSS][i];
        if (b->kind==IN_KEY && b->value==SDL_SCANCODE_SPACE) after_has_space=true;
    }
    assert(after_has_space);

    unsetenv("BB_INPUT_CONFIG");
    unlink(config_path);
    free(loaded_config); config_initialized=0; loaded_config=NULL;
    puts("PASS: F8 reloads input.ini live (AC-018)");
}

int main(void) {
    test_binding_evaluation();
    test_mouse_to_axis();
    test_mouse_buttons_and_wheel();

    char path[]="/tmp/bbport-pad-test-XXXXXX";
    int fd=mkstemp(path);
    assert(fd>=0);
    close(fd);
    setenv("BB_PAD_FILE",path,1);
    setenv("SDL_VIDEODRIVER","dummy",1);
    /* Only the virtual test controller is a gamepad, whatever is plugged in. */
    SDL_SetHint(SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT,"0x1d50/0x6189");
    assert(SDL_Init(SDL_INIT_VIDEO|SDL_INIT_GAMEPAD));
    assert(pad_init()==0 && pad_open(1,0,0,NULL)==1);
    test_custom_binding_config();
    test_reload_on_f8();
    test_mouse_end_to_end();
    PadData data;
    inject(path,"cross l3 touchpad_left");
    assert(pad_read_state(1,&data)==0);
    assert((data.buttons & (BTN_CROSS|BTN_L3|BTN_TOUCHPAD))==(BTN_CROSS|BTN_L3|BTN_TOUCHPAD));
    assert(data.touch_count==1 && data.touches[0].x==480 && data.touches[0].y==471);
    inject(path,"touchpad_right");
    assert(pad_read_state(1,&data)==0 && data.touch_count==1 && data.touches[0].x==1440);
    inject(path,"");
    assert(pad_read_state(1,&data)==0 && data.buttons==0 && data.touch_count==0);

    SDL_VirtualJoystickTouchpadDesc touch={.nfingers=2};
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type=SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.naxes=SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons=SDL_GAMEPAD_BUTTON_COUNT;
    desc.button_mask=(1u<<SDL_GAMEPAD_BUTTON_COUNT)-1;
    desc.axis_mask=(1u<<SDL_GAMEPAD_AXIS_COUNT)-1;
    desc.name="bbport test controller";
    desc.vendor_id=0x1d50;
    desc.product_id=0x6189;
    desc.ntouchpads=1;
    desc.touchpads=&touch;
    SDL_JoystickID id=SDL_AttachVirtualJoystick(&desc);
    assert(id!=0);
    SDL_Joystick *joystick=SDL_OpenJoystick(id);
    assert(joystick);
    assert(SDL_SetJoystickVirtualTouchpad(joystick,0,0,true,0.75f,0.5f,1.0f));
    assert(SDL_SetJoystickVirtualTouchpad(joystick,0,1,true,0.25f,1.0f,1.0f));
    assert(SDL_SetJoystickVirtualButton(joystick,SDL_GAMEPAD_BUTTON_TOUCHPAD,true));
    SDL_UpdateJoysticks();
    SDL_UpdateGamepads();
    assert(pad_read_state(1,&data)==0);
    assert(gamepad && data.touch_count==2 && (data.buttons & BTN_TOUCHPAD));
    assert(data.touches[0].x==1439 && data.touches[0].y==471 && data.touches[0].id==0);
    assert(data.touches[1].x==480 && data.touches[1].y==942 && data.touches[1].id==1);
    capture=1;
    assert(pad_read_state(1,&data)==0 && data.touch_count==0 && data.buttons==0);
    capture=0;
    assert(SDL_SetJoystickVirtualTouchpad(joystick,0,0,false,0,0,0));
    assert(SDL_SetJoystickVirtualTouchpad(joystick,0,1,false,0,0,0));
    SDL_UpdateJoysticks();
    SDL_UpdateGamepads();
    assert(pad_read_state(1,&data)==0 && data.touch_count==1 && data.touches[0].x==480);
    SDL_CloseJoystick(joystick);
    if (gamepad) SDL_CloseGamepad(gamepad);
    gamepad=NULL;
    assert(SDL_DetachVirtualJoystick(id));
    SDL_Quit();
    unlink(path);
    puts("PASS: pad ABI, debug camera chord, left/right clicks, SDL touch coordinates, overlay capture");
}
