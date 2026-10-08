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

/* T5: the shadPS4-based mouse formula (CNV-001..003, MOU-009). mouse_to_axis is stateful again
 * (reads/writes smoothed_mouse_stick_x/y), but this time the state is the *output* stick value
 * being lerped toward an unfiltered, instantaneous target -- not a filtered velocity. The raw
 * delta->target formula runs fresh every call (so a flick or the start of a 180 reversal is
 * immediately reflected in the target, with no ramp-up and no reversal-stall risk); only the
 * final value sent to the game is smoothed, which is what damps jitter from noisy per-sample
 * deltas without making the response itself laggy. See the doc comment above mouse_to_axis for
 * the fourth-iteration history (micro-jumps -> degrau -> stall-on-reversal -> "crispy"). Each
 * case below resets that state first (mirroring MOU-005/CNV-002's reset on a capture gap), so
 * the tests are independent of each other and of call order. */
static void reset_mouse_filter(void) { smoothed_mouse_stick_x=smoothed_mouse_stick_y=0.0; }

static void test_mouse_to_axis(void) {
    MouseParams mouse={.deadzone_offset=0.5f,.speed=1.0f,.speed_offset=0.125f,.stick=2};
    int x=99, y=99;

    /* CNV-002: no motion -> no contribution. */
    reset_mouse_filter();
    mouse_to_axis(0.0f,0.0f,33.0,&mouse,&x,&y);
    assert(x==0 && y==0);

    /* Steady state: a constant dx=10 at dt=6.67ms (150 FPS), held long enough for the lerp to
     * settle, must converge to the un-filtered linear response for that velocity: magnitude =
     * 10 * (33/6.67) ~= 49.5, speed = 49.5*1+16 ~= 65.5. The lerp changes the *path* to get
     * there, not the destination. */
    reset_mouse_filter();
    for (int i=0;i<60;++i) mouse_to_axis(10.0f,0.0f,6.67,&mouse,&x,&y);
    assert(x>=63 && x<=67 && y==0);

    /* Responsive from a standstill: unlike both earlier designs (hard floor, magnitude ramp),
     * the target here is the plain unfiltered formula, so a single large flick must already
     * equal its own instantaneous target (minus a single lerp step, which still leaves most of
     * it) -- this is what actually fixes "dificuldade ... fazer um 180" / a sluggish start. */
    reset_mouse_filter();
    mouse_to_axis(30.0f,0.0f,6.67,&mouse,&x,&y);
    assert(x>=25 && y==0);

    /* A flick ramps up toward its target while held, then decays (not instantly) once the
     * physical motion stops -- the lerp's memory working as intended, not stuck or jumping. */
    reset_mouse_filter();
    {
        int seq[]={20,20,20,0,0,0,0,0}, out[8];
        for (int i=0;i<8;++i) { mouse_to_axis((float)seq[i],0.0f,6.67,&mouse,&x,&y); out[i]=x; }
        assert(out[2]>out[0]); /* ramps up while flicking */
        assert(out[7]<out[2]); /* decays after the flick stops */
    }

    /* The actual regression test for "ainda sinto dificuldade ... fazer um 180": a fast
     * reversal (the mouse decelerates, passes near zero speed, then accelerates the other way)
     * must cross to the opposite sign and keep moving that way, not stall at the near-zero
     * sample as the earlier, stateless magnitude-ramp formula did, and not amplify noise around
     * that crossing as the later, velocity-filtered formula did. */
    reset_mouse_filter();
    {
        int seq[]={20,20,5,-5,-20,-20}, out[6];
        for (int i=0;i<6;++i) { mouse_to_axis((float)seq[i],0.0f,6.67,&mouse,&x,&y); out[i]=x; }
        assert(out[5]<0);           /* crosses to negative by the end of the reversal */
        assert(out[4]<out[3]);      /* still moving negative, not stalled, right after crossing */
    }

    /* Diagonal motion, steady state: direction preserved via atan2, magnitude clamped to 128
     * then the +/-127 cast. A large deflection saturates both axes toward the 45-degree corner. */
    reset_mouse_filter();
    for (int i=0;i<60;++i) mouse_to_axis(1000.0f,1000.0f,33.0,&mouse,&x,&y);
    assert(x>=89 && x<=91 && y>=89 && y<=91); /* 127/sqrt(2) ~= 89.8 */

    /* Smooth acceleration from rest (a realistic trajectory -- dx ramping 0,1,2,...,10 over ten
     * samples, then holding -- rather than an instantaneous jump to a fixed magnitude): once
     * motion is under way, no sample-to-sample jump should be much larger than the acceleration
     * itself justifies. The very first nonzero sample is excluded -- that one jump (roughly
     * speed_offset's own contribution, faded in near zero magnitude but already near full
     * strength by dx=1) is speed_offset doing its job of giving slow movements an immediate
     * minimum response, not a defect in the smoothing curve. */
    reset_mouse_filter();
    {
        int prev_x=0, prev_set=0, max_jump=0;
        for (int i=0;i<20;++i) {
            float dx=(float)(i<10 ? i : 10);
            mouse_to_axis(dx,0.0f,6.67,&mouse,&x,&y);
            if (prev_set && i>1) {
                int step=x-prev_x;
                if (step>max_jump) max_jump=step;
            }
            prev_x=x; prev_set=1;
        }
        assert(max_jump<=10);
    }

    puts("PASS: mouse_to_axis (output-value lerp: steady state, responsiveness, 180 reversal, saturation)");
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
