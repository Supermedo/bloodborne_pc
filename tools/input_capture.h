/* The launcher starts this short-lived SDL video process to capture physical input.
 * SDL produces the same scancodes and mouse names consumed by runtime_pad.c. */
#include "../src/host_input.h"

static inline void capture_open_pad(SDL_JoystickID id) {
    SDL_Gamepad *pad = SDL_GetGamepadFromID(id);
    if (!pad) pad = SDL_OpenGamepad(id);
    if (!pad) return;
    SDL_UpdateGamepads();
    const SDL_PropertiesID props = SDL_GetGamepadProperties(pad);
    Uint32 held = 0;
    for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; ++b)
        if (SDL_GetGamepadButton(pad, (SDL_GamepadButton)b)) held |= 1u << b;
    SDL_SetNumberProperty(props, "bbport.capture.held", held);
    SDL_SetBooleanProperty(props, "bbport.capture.left_armed",
        SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) < 8000);
    SDL_SetBooleanProperty(props, "bbport.capture.right_armed",
        SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) < 8000);
}

static const char *capture_event(const SDL_Event *e, int want_key, int want_pad) {
    if (want_key && e->type == SDL_EVENT_KEY_DOWN && !e->key.repeat)
        return bb_key_name(e->key.scancode);
    if (want_key && e->type == SDL_EVENT_MOUSE_BUTTON_DOWN)
        return bb_mouse_name(e->button.button);
    if (want_key && e->type == SDL_EVENT_MOUSE_WHEEL) {
        const float sign = e->wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -1.f : 1.f;
        const float x = e->wheel.x * sign, y = e->wheel.y * sign;
        if (SDL_fabsf(y) >= SDL_fabsf(x) && y) return y > 0 ? "Mouse Wheel Up" : "Mouse Wheel Down";
        if (x) return x > 0 ? "Mouse Wheel Right" : "Mouse Wheel Left";
    }
    if (want_pad && (e->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN || e->type == SDL_EVENT_GAMEPAD_BUTTON_UP ||
                     e->type == SDL_EVENT_GAMEPAD_AXIS_MOTION)) {
        SDL_JoystickID id = e->type == SDL_EVENT_GAMEPAD_AXIS_MOTION ? e->gaxis.which : e->gbutton.which;
        SDL_Gamepad *pad = SDL_GetGamepadFromID(id);
        const char *selected = getenv("BB_GAMEPAD");
        if (!pad) return "";
        if (selected && *selected) {
            char guid[33];
            SDL_GUIDToString(SDL_GetGamepadGUIDForID(id), guid, sizeof guid);
            if (SDL_strcasecmp(guid, selected)) return "";
        }
        const SDL_PropertiesID props = SDL_GetGamepadProperties(pad);
        if (e->type == SDL_EVENT_GAMEPAD_BUTTON_UP) {
            if (e->gbutton.button < SDL_GAMEPAD_BUTTON_COUNT)
                SDL_SetNumberProperty(props, "bbport.capture.held",
                    SDL_GetNumberProperty(props, "bbport.capture.held", 0) & ~(1u << e->gbutton.button));
            return "";
        }
        if (e->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
            if (e->gbutton.button >= SDL_GAMEPAD_BUTTON_COUNT ||
                (SDL_GetNumberProperty(props, "bbport.capture.held", 0) & (1u << e->gbutton.button))) return "";
            return SDL_GetGamepadStringForButton((SDL_GamepadButton)e->gbutton.button);
        }
        const char *armed = e->gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER ? "bbport.capture.left_armed"
                          : e->gaxis.axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER ? "bbport.capture.right_armed" : NULL;
        if (armed && e->gaxis.value < 8000) SDL_SetBooleanProperty(props, armed, true);
        if (armed && e->gaxis.value > 16000 && SDL_GetBooleanProperty(props, armed, true))
            return e->gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER ? "lefttrigger" : "righttrigger";
    }
    return "";
}

/* --read-input key|pad [title]. No keyboard key is reserved, including Escape.
 * Closing the window, losing focus or 30 seconds cancels without changing a binding. */
static inline int read_input(const char *kind, const char *title) {
    const int want_key = !strcmp(kind, "key"), want_pad = !strcmp(kind, "pad");
    if (!want_key && !want_pad) return 1;
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        fprintf(stderr, "Input capture: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Window *window = NULL;
    SDL_Renderer *renderer = NULL;
    if (!SDL_CreateWindowAndRenderer(title ? title : "Bloodborne - Assign input", 640, 112, 0,
                                    &window, &renderer)) {
        fprintf(stderr, "Input capture: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    int count = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&count);
    for (int i = 0; ids && i < count; ++i) capture_open_pad(ids[i]);
    SDL_free(ids);
    SDL_PumpEvents();
    // Discard startup device state and the click/key used to open this capture window.
    SDL_FlushEvents(SDL_EVENT_FIRST, SDL_EVENT_LAST);
    SDL_RaiseWindow(window);
    SDL_SetRenderDrawColor(renderer, 21, 18, 16, 255);
    SDL_RenderClear(renderer);
    SDL_SetRenderDrawColor(renderer, 233, 226, 214, 255);
    SDL_RenderDebugText(renderer, 18, 24, want_key ? "Press a key, mouse button or mouse wheel."
                                                : "Press a controller button or trigger.");
    SDL_RenderDebugText(renderer, 18, 52, "Close this window to cancel. Escape can be assigned.");
    SDL_RenderDebugText(renderer, 18, 76, "Waiting for up to 30 seconds...");
    SDL_RenderPresent(renderer);
    fprintf(stderr, "Input capture ready\n");
    fflush(stderr);
    const Uint64 end = SDL_GetTicks() + 30000;
    int focused = (SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS) != 0;
    int result = 0;
    while (SDL_GetTicks() < end) {
        SDL_Event e;
        if (!SDL_WaitEventTimeout(&e, 100)) continue;
        if (e.type == SDL_EVENT_QUIT || e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED ||
            (focused && e.type == SDL_EVENT_WINDOW_FOCUS_LOST)) break;
        if (e.type == SDL_EVENT_WINDOW_FOCUS_GAINED) focused = 1;
        if (e.type == SDL_EVENT_GAMEPAD_ADDED) capture_open_pad(e.gdevice.which);
        // Keyboard/mouse events are already addressed to this SDL window. A first
        // activating click can precede its focus notification; accept it too.
        if (!focused && want_pad) continue;
        const char *name = capture_event(&e, want_key, want_pad);
        if (name && *name) {
            printf("%s %s\n", want_key ? "key" : "pad", name);
            break;
        }
    }
    fflush(stdout);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return result;
}
