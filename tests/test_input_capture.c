#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "test_platform.h"
#include "../tools/input_capture.h"

int main(void) {
    SDL_Event e={.type=SDL_EVENT_KEY_DOWN};
    e.key.scancode=SDL_SCANCODE_COMMA;
    assert(!strcmp(capture_event(&e,1,0),"Comma"));
    assert(bb_key_binding(capture_event(&e,1,0))==SDL_SCANCODE_COMMA);
    e.key.scancode=SDL_SCANCODE_RCTRL;
    assert(bb_key_binding(capture_event(&e,1,0))==SDL_SCANCODE_RCTRL);
    e.key.repeat=true;
    assert(!*capture_event(&e,1,0));
    e.key.repeat=false;
    assert(!*capture_event(&e,0,1));
    for (unsigned b=1;b<=5;++b) {
        e=(SDL_Event){.type=SDL_EVENT_MOUSE_BUTTON_DOWN}; e.button.button=(Uint8)b;
        assert(bb_mouse_binding(capture_event(&e,1,0))==SDL_BUTTON_MASK(b));
    }
    e=(SDL_Event){.type=SDL_EVENT_MOUSE_WHEEL}; e.wheel.y=1;
    assert(bb_mouse_binding(capture_event(&e,1,0))==BB_WHEEL_UP);
    e.wheel.direction=SDL_MOUSEWHEEL_FLIPPED;
    assert(bb_mouse_binding(capture_event(&e,1,0))==BB_WHEEL_DOWN);
    e.wheel.y=0; e.wheel.x=1;
    assert(bb_mouse_binding(capture_event(&e,1,0))==BB_WHEEL_LEFT);
    assert(SDL_Init(SDL_INIT_GAMEPAD));
    SDL_VirtualJoystickDesc desc; SDL_INIT_INTERFACE(&desc);
    desc.type=SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.naxes=SDL_GAMEPAD_AXIS_COUNT; desc.nbuttons=SDL_GAMEPAD_BUTTON_COUNT;
    desc.button_mask=(1u<<SDL_GAMEPAD_BUTTON_COUNT)-1;
    desc.axis_mask=(1u<<SDL_GAMEPAD_AXIS_COUNT)-1;
    SDL_JoystickID id=SDL_AttachVirtualJoystick(&desc);
    assert(id);
    SDL_Gamepad *pad=SDL_OpenGamepad(id); assert(pad);
    e=(SDL_Event){.type=SDL_EVENT_GAMEPAD_BUTTON_DOWN};
    e.gbutton.which=id; e.gbutton.button=SDL_GAMEPAD_BUTTON_NORTH;
    assert(!strcmp(capture_event(&e,0,1),"y"));
    setenv("BB_GAMEPAD","nonmatching-guid",1);
    assert(!*capture_event(&e,0,1));
    char guid[33]; SDL_GUIDToString(SDL_GetGamepadGUIDForID(id),guid,sizeof guid);
    setenv("BB_GAMEPAD",guid,1);
    assert(!strcmp(capture_event(&e,0,1),"y"));
    e=(SDL_Event){.type=SDL_EVENT_GAMEPAD_AXIS_MOTION};
    e.gaxis.which=id; e.gaxis.axis=SDL_GAMEPAD_AXIS_RIGHT_TRIGGER; e.gaxis.value=32767;
    assert(!strcmp(capture_event(&e,0,1),"righttrigger"));
    e.gaxis.value=100;
    assert(!*capture_event(&e,0,1));
    assert(SDL_SetJoystickVirtualAxis(SDL_GetGamepadJoystick(pad),SDL_GAMEPAD_AXIS_RIGHT_TRIGGER,32767));
    SDL_UpdateJoysticks(); SDL_UpdateGamepads();
    capture_open_pad(id);
    e.gaxis.value=32767;
    assert(!*capture_event(&e,0,1)); // a trigger held before opening must be released first
    e.gaxis.value=0; assert(!*capture_event(&e,0,1));
    e.gaxis.value=32767; assert(!strcmp(capture_event(&e,0,1),"righttrigger"));
    SDL_CloseGamepad(pad); SDL_DetachVirtualJoystick(id); SDL_Quit();
    puts("PASS: physical binding names, side buttons, wheel, repeat filtering and selected controller");
}
