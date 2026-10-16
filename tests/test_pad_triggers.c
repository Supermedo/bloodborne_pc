/* SPDX-License-Identifier: GPL-2.0-or-later
 * Exercise the complete SDL -> scePadReadState trigger path without a GPU or game dump.
 * This test also runs natively on Windows; no /tmp files or POSIX-only test helpers. */
#include "../src/runtime_pad.c"

#define CHECK(expr) do { if (!(expr)) { fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#expr); exit(1); } } while (0)
int bbgpu_overlay_captures_input(void) { return 0; }
void bbgpu_mouse_take(BbMouseInput *out) { memset(out,0,sizeof(*out)); }
void bbgpu_input_configure(int mode,int32_t toggle,int32_t reload) { (void)mode;(void)toggle;(void)reload; }
void bbgpu_mouse_camera_configure(int direct,float sensitivity) { (void)direct;(void)sensitivity; }
int bbgpu_input_reload_requested(void) { return 0; }
uintptr_t runtime_lookup(const RuntimeExport *table,size_t count,const char *name) {
    (void)table;(void)count;(void)name;return 0;
}

static InputConfig cfg;
static SDL_Joystick *joystick;
static void configure(const char *text) {
    input_config_parse(&cfg,text);CHECK(cfg.warnings==0);
    loaded_config=&cfg;config_initialized=1;
}
static void set_trigger(SDL_GamepadAxis axis,int value) {
    /* Virtual joystick axes use the full signed range, unlike SDL's gamepad triggers. */
    CHECK(SDL_SetJoystickVirtualAxis(joystick,axis,(int16_t)(value*257-32768)));
}
static PadData read_triggers(void) {
    SDL_UpdateJoysticks();SDL_UpdateGamepads();
    PadData d;CHECK(pad_read_state(PAD_HANDLE,&d)==0);return d;
}
static void check_triggers(int l2,int r2) {
    PadData d=read_triggers();
    if(d.l2!=l2 || d.r2!=r2) fprintf(stderr,"expected L2/R2 %d/%d, got %u/%u\n",l2,r2,d.l2,d.r2);
    CHECK(d.l2==l2 && d.r2==r2);
    CHECK(!!(d.buttons&BTN_L2)==(l2>INPUT_TRIGGER_BUTTON_THRESHOLD));
    CHECK(!!(d.buttons&BTN_R2)==(r2>INPUT_TRIGGER_BUTTON_THRESHOLD));
}
int main(void) {
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER,"dummy");
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS,"1");
    SDL_SetHint(SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT,"0x1d50/0x6189");
    CHECK(SDL_Init(SDL_INIT_VIDEO|SDL_INIT_GAMEPAD));
    CHECK(pad_init()==0 && pad_open(1,0,0,NULL)==PAD_HANDLE);
    SDL_VirtualJoystickDesc desc;SDL_INIT_INTERFACE(&desc);
    desc.type=SDL_JOYSTICK_TYPE_GAMEPAD;desc.naxes=SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons=SDL_GAMEPAD_BUTTON_COUNT;desc.axis_mask=(1u<<SDL_GAMEPAD_AXIS_COUNT)-1;
    desc.button_mask=(1u<<SDL_GAMEPAD_BUTTON_COUNT)-1;
    desc.name="bbport trigger test";desc.vendor_id=0x1d50;desc.product_id=0x6189;
    SDL_JoystickID id=SDL_AttachVirtualJoystick(&desc);CHECK(id);
    joystick=SDL_OpenJoystick(id);CHECK(joystick);
    set_trigger(SDL_GAMEPAD_AXIS_LEFT_TRIGGER,0);set_trigger(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER,0);

    /* Existing default behavior remains exact, including the >30 button threshold. */
    configure("l2 = l2\nr2 = r2\n");
    check_triggers(0,0); /* Open the gamepad before applying the first non-neutral state. */
    for(int v=0;v<=255;++v) {
        set_trigger(SDL_GAMEPAD_AXIS_LEFT_TRIGGER,v);
        set_trigger(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER,255-v);check_triggers(v,255-v);
    }
    puts("PASS: unchanged default trigger range and button threshold");

    /* 1..127 config units map to 0..255 trigger units: 32 -> 64, 96 -> 192. */
    configure("l2 = l2\nr2 = r2\nanalog_deadzone = l2, 32, 96\nanalog_deadzone = r2, 16, 127\n");
    set_trigger(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER,0);
    const int inputs[]={0,30,49,64,65,128,191,192,255};
    const int outputs[]={0,0,0,0,1,127,253,255,255};
    for(size_t i=0;i<sizeof(inputs)/sizeof(*inputs);++i) {
        set_trigger(SDL_GAMEPAD_AXIS_LEFT_TRIGGER,inputs[i]);check_triggers(outputs[i],0);
    }
    set_trigger(SDL_GAMEPAD_AXIS_LEFT_TRIGGER,0);
    set_trigger(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER,32);check_triggers(0,0);
    set_trigger(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER,33);check_triggers(0,1);
    set_trigger(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER,255);check_triggers(0,255);
    set_trigger(SDL_GAMEPAD_AXIS_LEFT_TRIGGER,255);check_triggers(255,255);
    puts("PASS: independent deadzones, ramp boundaries, saturation and simultaneous triggers");

    /* Reproduce a drifting LT while the left stick moves. RT stays released. */
    configure("l2 = l2\nr2 = r2\naxis_left_x = axis_left_x\naxis_left_y = axis_left_y\nanalog_deadzone = l2, 28, 127\n");
    set_trigger(SDL_GAMEPAD_AXIS_LEFT_TRIGGER,49);set_trigger(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER,0);
    const int positions[]={-32768,-24576,-8192,0,8192,24576,32767};
    for(size_t x=0;x<7;++x) for(size_t y=0;y<7;++y) {
        CHECK(SDL_SetJoystickVirtualAxis(joystick,SDL_GAMEPAD_AXIS_LEFTX,(int16_t)positions[x]));
        CHECK(SDL_SetJoystickVirtualAxis(joystick,SDL_GAMEPAD_AXIS_LEFTY,(int16_t)positions[y]));
        check_triggers(0,0);
    }
    set_trigger(SDL_GAMEPAD_AXIS_LEFT_TRIGGER,255);check_triggers(255,0);
    set_trigger(SDL_GAMEPAD_AXIS_LEFT_TRIGGER,49);check_triggers(0,0);
    puts("PASS: drifting LT is released across 49 stick positions and still reaches full press");

    /* A held digital binding remains a full trigger, regardless of line order. */
    const char *mixed[]={
        "l2 = l1\nl2 = l2\nr2 = r1\nr2 = r2\nanalog_deadzone = l2, 28, 127\nanalog_deadzone = r2, 28, 127\n",
        "l2 = l2\nl2 = l1\nr2 = r2\nr2 = r1\nanalog_deadzone = l2, 28, 127\nanalog_deadzone = r2, 28, 127\n",
    };
    for(size_t i=0;i<2;++i) {
        configure(mixed[i]);
        CHECK(SDL_SetJoystickVirtualButton(joystick,SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,true));
        CHECK(SDL_SetJoystickVirtualButton(joystick,SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,true));
        check_triggers(255,255);
        CHECK(SDL_SetJoystickVirtualButton(joystick,SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,false));
        CHECK(SDL_SetJoystickVirtualButton(joystick,SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,false));
        check_triggers(0,0);
    }
    puts("PASS: digital trigger bindings survive idle/drifting analog bindings in either order");

    /* MIX-004: sum first, clamp, then apply the destination trigger's deadzone. */
    configure("l2 = l2\nl2 = r2\nanalog_deadzone = l2, 32, 96\n");
    set_trigger(SDL_GAMEPAD_AXIS_LEFT_TRIGGER,40);set_trigger(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER,40);
    check_triggers(31,0);
    set_trigger(SDL_GAMEPAD_AXIS_LEFT_TRIGGER,200);set_trigger(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER,200);
    check_triggers(255,0);
    configure("l2 = r2\nanalog_deadzone = l2, 32, 96\n");
    set_trigger(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER,49);check_triggers(0,0);
    set_trigger(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER,255);check_triggers(255,0);
    puts("PASS: analog mixing, clamping and remapped trigger destination");

    /* Equal bounds are a step at the outer limit, never a division by zero. */
    configure("l2 = l2\nanalog_deadzone = l2, 64, 64\n");
    set_trigger(SDL_GAMEPAD_AXIS_LEFT_TRIGGER,127);check_triggers(0,0);
    set_trigger(SDL_GAMEPAD_AXIS_LEFT_TRIGGER,128);check_triggers(255,0);
    configure("l2 = l2\nanalog_deadzone = l2, 127, 127\n");
    set_trigger(SDL_GAMEPAD_AXIS_LEFT_TRIGGER,254);check_triggers(0,0);
    set_trigger(SDL_GAMEPAD_AXIS_LEFT_TRIGGER,255);check_triggers(255,0);
    puts("PASS: equal inner/outer deadzone bounds");
    SDL_CloseGamepad(gamepad);gamepad=NULL;SDL_CloseJoystick(joystick);
    CHECK(SDL_DetachVirtualJoystick(id));SDL_Quit();return 0;
}
