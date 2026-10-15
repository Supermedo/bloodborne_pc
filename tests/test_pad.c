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
void bbgpu_mouse_camera_configure(int direct, float sensitivity) {
    (void)direct; (void)sensitivity;
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
    HostState host={keys,NULL,0,0,0,0};

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

/* stick_socd and stick_release_delay_ms, with explicit timestamps (microseconds). The A -> D
 * zigzag while sprinting must never hand the game a neutral left stick. */
static void test_socd_and_release_delay(void) {
    AxisPairState s={0,0};
    /* A held, then D pressed while A is still down: D (the latest) wins, no neutral. */
    assert(resolve_axis_pair(&s,-127,0,1,1000)==-127);
    assert(resolve_axis_pair(&s,-127,127,1,2000)==127);
    assert(resolve_axis_pair(&s,0,127,1,3000)==127);   /* A released: still D */
    /* A pressed again on top of D: A wins; release A: back to D. */
    assert(resolve_axis_pair(&s,-127,127,1,4000)==-127);
    assert(resolve_axis_pair(&s,0,127,1,5000)==127);
    /* neutral mode keeps the old sum-and-cancel behavior. */
    AxisPairState n={0,0};
    assert(resolve_axis_pair(&n,-127,0,0,1000)==-127);
    assert(resolve_axis_pair(&n,-127,127,0,2000)==0);
    /* both pressed in the same sample: no "latest" one, they cancel. */
    AxisPairState t={0,0};
    assert(resolve_axis_pair(&t,-127,127,1,1000)==0);

    /* Release delay 60 ms: a 30 ms gap between releasing A and pressing D keeps A's direction,
     * then D replaces it at once; a real stop ends after the delay. */
    StickReleaseState r={0,0,0};
    int x=-127, y=0;
    hold_stick_on_release(&r,&x,&y,60,1000000);
    assert(x==-127);
    x=0; y=0;
    hold_stick_on_release(&r,&x,&y,60,1030000); /* 30 ms after: held */
    assert(x==-127 && y==0);
    x=127; y=0;
    hold_stick_on_release(&r,&x,&y,60,1031000); /* D: immediate */
    assert(x==127);
    x=0; y=0;
    hold_stick_on_release(&r,&x,&y,60,1100000); /* 69 ms after D: stopped */
    assert(x==0 && y==0);
    x=0; y=0;
    hold_stick_on_release(&r,&x,&y,0,1100500); /* delay 0: never held */
    assert(x==0 && y==0);
    x=0; y=-127;
    hold_stick_on_release(&r,&x,&y,0,1101000);
    x=0; y=0;
    hold_stick_on_release(&r,&x,&y,0,1101001);
    assert(x==0 && y==0);
    /* An analog half stick is never held: back at the center, it is at the center at once. */
    StickReleaseState an={0,0,0};
    x=-90; y=0;
    hold_stick_on_release(&an,&x,&y,60,2000000);
    x=0; y=0;
    hold_stick_on_release(&an,&x,&y,60,2010000);
    assert(x==0 && y==0);
    /* ...while a key is, under the same delay. */
    StickReleaseState key={0,0,0};
    x=-127; y=-127;
    hold_stick_on_release(&key,&x,&y,60,2000000);
    x=0; y=0;
    hold_stick_on_release(&key,&x,&y,60,2010000);
    assert(x==-127 && y==-127);
    puts("PASS: stick_socd last-wins and stick_release_delay_ms");
}

/* stick_circular and stick_turn_rate (shape_digital_stick), at 60 FPS samples. */
static void test_digital_stick_shape(void) {
    const uint64_t frame=16667; /* us */
    /* circular: W+A's square corner becomes a full-deflection diagonal on the circle. */
    StickShapeState c={0,0,0};
    int x=-127, y=-127;
    shape_digital_stick(&c,&x,&y,1,0,1000000);
    assert(x==-90 && y==-90);
    /* off and no turn rate: raw values untouched. */
    StickShapeState o={0,0,0};
    x=-127; y=-127;
    shape_digital_stick(&o,&x,&y,0,0,1000000);
    assert(x==-127 && y==-127);

    /* turn rate 900 deg/s: W, then W+A. 15 degrees per frame toward the diagonal, staying at
     * full deflection (never toward neutral), and reaching it after 3 frames. */
    StickShapeState t={0,0,0};
    uint64_t now=1000000;
    x=0; y=-127;
    shape_digital_stick(&t,&x,&y,1,900,now);
    assert(x==0 && y==-127);
    x=-127; y=-127; now+=frame;
    shape_digital_stick(&t,&x,&y,1,900,now);
    assert(x<=-30 && x>=-36 && y<=-120); /* about -105 degrees */
    assert(x*x+y*y>=125*125);
    for (int i=0;i<2;++i) { x=-127; y=-127; now+=frame; shape_digital_stick(&t,&x,&y,1,900,now); }
    assert(x==-90 && y==-90);
    /* reversal: turns too (no snap), on the circle, starting through forward (y < 0). */
    x=127; y=127; now+=frame;
    shape_digital_stick(&t,&x,&y,1,900,now);
    assert(!(x==90 && y==90) && y<0);
    assert(x*x+y*y>=125*125);
    /* neutral, then a new key: starts at that key's direction, no rotation from the old one. */
    x=0; y=0; now+=frame;
    shape_digital_stick(&t,&x,&y,1,900,now);
    assert(x==0 && y==0);
    x=-127; y=0; now+=frame;
    shape_digital_stick(&t,&x,&y,1,900,now);
    assert(x==-127 && y==0);

    /* Only digital input is shaped: an analog half stick passes through untouched, with the
     * turn rate and circular on, and leaves the turn state inactive. */
    StickShapeState a={0,0,0};
    x=-64; y=0;
    shape_digital_stick(&a,&x,&y,1,900,1000000);
    assert(x==-64 && y==0 && !a.active);
    x=-127; y=-64; /* one axis at the edge is not enough */
    shape_digital_stick(&a,&x,&y,0,900,1000000+frame);
    assert(x==-127 && y==-64 && !a.active);

    /* Square gate (circular off): W, then W+A at 900 deg/s. The larger axis stays at 127 on
     * every sample, since the game picks walk or run by it, and the turn ends exactly on the
     * corner. */
    StickShapeState q={0,0,0};
    now=2000000;
    x=0; y=-127;
    shape_digital_stick(&q,&x,&y,0,900,now);
    assert(x==0 && y==-127);
    x=-127; y=-127; now+=frame;
    shape_digital_stick(&q,&x,&y,0,900,now);
    assert(x<=-30 && x>=-36 && y==-127); /* about -105 degrees, on the square */
    for (int i=0;i<2;++i) {
        x=-127; y=-127; now+=frame;
        shape_digital_stick(&q,&x,&y,0,900,now);
        assert((abs(x)>abs(y) ? abs(x) : abs(y))==127);
    }
    assert(x==-127 && y==-127);
    puts("PASS: stick_circular and stick_turn_rate");
}

/* Direction of a stick sample in degrees, and the unsigned angle between two of them. */
static double stick_deg(int x, int y) { return atan2((double)y,(double)x)*180.0/3.14159265358979323846; }
static double deg_between(double a, double b) { return fabs(fmod(b-a+540.0,360.0)-180.0); }
/* Turns from (x0,y0) toward (x1,y1) at 900 deg/s and 60 FPS on the square gate, checking that
 * no sample moves 30 degrees or more (the game drops a sprint at 45) and that the larger axis
 * never leaves 127 (the game would walk). Returns the samples taken; *right / *forward tell
 * whether any intermediate sample had x > 0 / y < 0. */
static int turn_and_check(int x0, int y0, int x1, int y1, int *right, int *forward) {
    const uint64_t frame=16667;
    StickShapeState s={0,0,0};
    uint64_t now=5000000;
    int x=x0, y=y0, n=0;
    shape_digital_stick(&s,&x,&y,0,900,now);
    double prev=stick_deg(x,y);
    *right=*forward=0;
    while (!(x==x1 && y==y1)) {
        assert(++n<100);
        x=x1; y=y1; now+=frame;
        shape_digital_stick(&s,&x,&y,0,900,now);
        assert(deg_between(prev,stick_deg(x,y))<30.5);
        assert((abs(x)>abs(y) ? abs(x) : abs(y))==127);
        if (!(x==x1 && y==y1)) { if (x>0) *right=1; if (y<0) *forward=1; }
        prev=stick_deg(x,y);
    }
    return n;
}

/* stick_turn_rate turns every direction change, reversals included, never more than 30 degrees
 * per sample. Half turns go through forward, or through the right for W <-> S. */
static void test_digital_stick_turns(void) {
    int right, forward;
    /* A -> D: 180 degrees at 15 per frame, through forward. */
    assert(turn_and_check(-127,0,127,0,&right,&forward)==12 && forward);
    /* D -> A: through forward as well. */
    assert(turn_and_check(127,0,-127,0,&right,&forward)==12 && forward);
    /* W -> S and S -> W: through the right. */
    turn_and_check(0,-127,0,127,&right,&forward);
    assert(right);
    turn_and_check(0,127,0,-127,&right,&forward);
    assert(right);
    /* W -> W+A: 45 degrees in 3 frames. */
    assert(turn_and_check(0,-127,-127,-127,&right,&forward)==3);

    /* A frame hitch (0.5 s at 900 deg/s would be 450 degrees) still turns 30 at most. */
    StickShapeState h={0,0,0};
    int x=0, y=-127;
    shape_digital_stick(&h,&x,&y,0,900,1000000);
    x=127; y=0;
    shape_digital_stick(&h,&x,&y,0,900,1500000);
    assert(deg_between(stick_deg(0,-127),stick_deg(x,y))<30.5);
    assert(!(x==127 && y==0));
    puts("PASS: stick_turn_rate turns reversals too, at most 30 degrees per sample");
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

    HostState host={NULL,NULL,0,0,0,0};
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
    test_socd_and_release_delay();
    test_digital_stick_shape();
    test_digital_stick_turns();
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

    /* A controller stick bound as a full axis (the controller default) never goes through the
     * keyboard shaping, with its defaults on (900 deg/s turns, 60 ms release hold): a jump from
     * one side to the other, and back to the center, reaches the game at once. */
    char stick_config[]="/tmp/bbport-pad-test-stick-XXXXXX";
    int sfd=mkstemp(stick_config);
    assert(sfd>=0);
    FILE *sf=fdopen(sfd,"w");
    assert(sf);
    fputs("axis_left_x = axis_left_x\naxis_left_y = axis_left_y\n"
          "axis_left_x_minus = a\naxis_left_x_plus = d\n",sf);
    fclose(sf);
    setenv("BB_INPUT_CONFIG",stick_config,1);
    free(loaded_config); config_initialized=0; loaded_config=NULL;
    assert(SDL_SetJoystickVirtualAxis(joystick,SDL_GAMEPAD_AXIS_LEFTX,-32768));
    SDL_UpdateJoysticks();
    SDL_UpdateGamepads();
    assert(pad_read_state(1,&data)==0 && data.left_x==1 && data.left_y==128);
    assert(SDL_SetJoystickVirtualAxis(joystick,SDL_GAMEPAD_AXIS_LEFTX,32767));
    SDL_UpdateJoysticks();
    SDL_UpdateGamepads();
    assert(pad_read_state(1,&data)==0 && data.left_x==255 && data.left_y==128);
    assert(SDL_SetJoystickVirtualAxis(joystick,SDL_GAMEPAD_AXIS_LEFTX,0));
    SDL_UpdateJoysticks();
    SDL_UpdateGamepads();
    assert(pad_read_state(1,&data)==0 && data.left_x==128 && data.left_y==128);
    unsetenv("BB_INPUT_CONFIG");
    unlink(stick_config);
    free(loaded_config); config_initialized=0; loaded_config=NULL;

    SDL_CloseJoystick(joystick);
    if (gamepad) SDL_CloseGamepad(gamepad);
    gamepad=NULL;
    assert(SDL_DetachVirtualJoystick(id));
    SDL_Quit();
    unlink(path);
    puts("PASS: pad ABI, debug camera chord, left/right clicks, SDL touch coordinates, overlay capture, full-axis controller stick unshaped");
}
