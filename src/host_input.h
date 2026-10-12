/* Shared binding names for the launcher capture helper and the game runtime. */
#ifndef BB_HOST_INPUT_H
#define BB_HOST_INPUT_H
#include <SDL3/SDL.h>

enum {
    BB_WHEEL_UP = 1u << 5, BB_WHEEL_DOWN = 1u << 6,
    BB_WHEEL_LEFT = 1u << 7, BB_WHEEL_RIGHT = 1u << 8
};
static inline const char *bb_mouse_name(unsigned button) {
    static const char *const names[] = {"", "Mouse Left", "Mouse Middle", "Mouse Right", "Mouse X1", "Mouse X2"};
    return button < SDL_arraysize(names) ? names[button] : "";
}
static inline Uint32 bb_mouse_binding(const char *name) {
    for (unsigned b = 1; b <= 5; ++b)
        if (!SDL_strcasecmp(name, bb_mouse_name(b))) return SDL_BUTTON_MASK(b);
    if (!SDL_strcasecmp(name, "Mouse Wheel Up")) return BB_WHEEL_UP;
    if (!SDL_strcasecmp(name, "Mouse Wheel Down")) return BB_WHEEL_DOWN;
    if (!SDL_strcasecmp(name, "Mouse Wheel Left")) return BB_WHEEL_LEFT;
    if (!SDL_strcasecmp(name, "Mouse Wheel Right")) return BB_WHEEL_RIGHT;
    return 0;
}
static inline const char *bb_key_name(SDL_Scancode key) {
    /* A literal comma is the INI alternative separator, so give this key a safe name. */
    return key == SDL_SCANCODE_COMMA ? "Comma" : SDL_GetScancodeName(key);
}
static inline SDL_Scancode bb_key_binding(const char *name) {
    return !SDL_strcasecmp(name, "Comma") ? SDL_SCANCODE_COMMA : SDL_GetScancodeFromName(name);
}
#endif
