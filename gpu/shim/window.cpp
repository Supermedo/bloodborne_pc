// bbport: SDL3 window for the Vulkan swapchain (X11 or Wayland).
#include <cstdlib>
#include <cstring>
#include <array>
#include <SDL3/SDL.h>
#include "common/assert.h"
#include "common/logging/log.h"
#include "sdl_window.h"
#include "bbport_mouse.h"
#include "bbport_overlay.h"
#include "bloodborne_cam.h"

namespace Frontend {

namespace {
// MOU-007: each wheel step keeps its direction's bit set for 33 ms (shadPS4's pulse length,
// ported so a config's mouse_movement_params tuning carries over); consecutive steps extend
// it rather than stacking. One expiry timestamp per direction, window-thread only.
constexpr uint32_t kWheelPulseMs = 33;
std::array<uint64_t, 4> g_wheel_expiry_ms{}; // index: WHEEL_UP=0, DOWN=1, LEFT=2, RIGHT=3
} // namespace

WindowSDL::WindowSDL(s32 width_, s32 height_, const char* title) : width{width_}, height{height_} {
    // Gamepads are sampled by runtime_pad.c; their events are pumped here with the window's.
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        UNREACHABLE_MSG("Failed to initialize SDL video: {}", SDL_GetError());
    }
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetStringProperty(props, SDL_PROP_WINDOW_CREATE_TITLE_STRING, title);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_X_NUMBER, SDL_WINDOWPOS_CENTERED);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_Y_NUMBER, SDL_WINDOWPOS_CENTERED);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, width_);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, height_);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_RESIZABLE_BOOLEAN, true);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_VULKAN_BOOLEAN, true);
    const char* fullscreen = std::getenv("BB_FULLSCREEN");
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_FULLSCREEN_BOOLEAN, fullscreen && fullscreen[0] == '1');
    base_title = title;
    window = SDL_CreateWindowWithProperties(props);
    SDL_DestroyProperties(props);
    ASSERT_MSG(window, "Failed to create window: {}", SDL_GetError());

    const char* driver = SDL_GetCurrentVideoDriver();
    const SDL_PropertiesID wp = SDL_GetWindowProperties(window);
#ifdef _WIN32
    if (driver && !std::strcmp(driver, "windows")) {
        window_info.type = WindowSystemType::Windows;
        window_info.render_surface = SDL_GetPointerProperty(wp, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
    } else
#endif
    if (driver && !std::strcmp(driver, "x11")) {
        window_info.type = WindowSystemType::X11;
        window_info.display_connection = SDL_GetPointerProperty(wp, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr);
        window_info.render_surface = reinterpret_cast<void*>(SDL_GetNumberProperty(wp, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0));
    } else if (driver && !std::strcmp(driver, "wayland")) {
        window_info.type = WindowSystemType::Wayland;
        window_info.display_connection = SDL_GetPointerProperty(wp, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, nullptr);
        window_info.render_surface = SDL_GetPointerProperty(wp, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, nullptr);
    } else {
        UNREACHABLE_MSG("Unsupported SDL video driver {}", driver ? driver : "(none)");
    }
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(window, &w, &h);
    width = w;
    height = h;
    LOG_INFO(Frontend, "Window {}x{} on {}", w, h, driver);
    BbMouse::Start();
}

WindowSDL::~WindowSDL() {
    BbMouse::Stop();
    Core::BloodborneCam::Instance().Shutdown();
    SDL_DestroyWindow(window);
}

void WindowSDL::BeginTextInput(const std::string& initial, const std::string& prompt) {
    std::scoped_lock lock{text_mutex};
    text = initial;
    text_prompt = prompt;
    text_state = 0;
    text_requested = true;
}

int WindowSDL::PollTextInput(std::string& out) {
    std::scoped_lock lock{text_mutex};
    out = text;
    return text_state;
}

void WindowSDL::UpdateTextTitle() {
    const std::string title = text_active ? base_title + " \u2014 " + text_prompt + ": " + text + "_  (Enter = OK, Esc = cancel)"
                                          : base_title;
    SDL_SetWindowTitle(window, title.c_str());
    BbOverlay::SetTextEntry(text_active, text_prompt, text);
}

bool WindowSDL::PollEvents() {
    {
        std::scoped_lock lock{text_mutex};
        if (text_requested) { // SDL text input must be toggled from the window thread
            text_requested = false;
            text_active = true;
            SDL_StartTextInput(window);
            UpdateTextTitle();
        }
    }
    if (!text_active) {
        BbOverlay::UpdateTextInput(window);
    }
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        // HOT-003: the toggle/reload hotkeys work regardless of the menu or text dialog (F8
        // must reload input.ini even with the menu open), so they are handled before anything
        // else gets a chance to consume the key event. Held-down repeats are ignored the same
        // way the menu's own Insert/Escape toggle already is (!event.key.repeat).
        if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat) {
            const int32_t scancode = static_cast<int32_t>(event.key.scancode);
            if (scancode == toggle_scancode.load(std::memory_order_relaxed) &&
                mouse_mode_available.load(std::memory_order_relaxed)) {
                mouse_mode_on.store(!mouse_mode_on.load(std::memory_order_relaxed), std::memory_order_relaxed);
                continue;
            }
            if (scancode == reload_scancode.load(std::memory_order_relaxed)) {
                reload_requested.store(true, std::memory_order_relaxed);
                continue;
            }
        }
        if (event.type == SDL_EVENT_WINDOW_FOCUS_GAINED) {
            window_focused = true;
        } else if (event.type == SDL_EVENT_WINDOW_FOCUS_LOST) {
            window_focused = false;
        }
        // Mouse motion/buttons/wheel feed the pad only while captured (MOU-002..006); the
        // overlay (menu) gets its own mouse handling below via BbOverlay::HandleEvent and is
        // mutually exclusive with capture (WantsMouseCapture() is false whenever the menu is
        // open), so there is no double consumption of the same event by both paths. Note:
        // mouse_captured_last only updates once per PollEvents call (below the event loop), so
        // a menu-opening event (L3+R3, Insert) followed within the same batch by a mouse event
        // can still see the old captured state for one iteration; accepted as a one-frame
        // edge case rather than re-evaluating capture per event for a cosmetic gain.
        if (mouse_captured_last) {
            // Direct camera: the BbMouse thread turns motion into camera angles, so it must not
            // also reach the stick -- except while locked on, where a flick switches targets.
            const Core::BloodborneCam& cam = Core::BloodborneCam::Instance();
            const bool motion_to_stick = !cam.IsEnabled() || cam.IsLockedOn();
            if (event.type == SDL_EVENT_MOUSE_MOTION && motion_to_stick) {
                std::scoped_lock lock{mouse_mutex};
                mouse.dx += event.motion.xrel;
                mouse.dy += event.motion.yrel;
            } else if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN || event.type == SDL_EVENT_MOUSE_BUTTON_UP) {
                std::scoped_lock lock{mouse_mutex};
                const uint32_t mask = SDL_BUTTON_MASK(event.button.button);
                if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN) mouse.buttons |= mask;
                else mouse.buttons &= ~mask;
            } else if (event.type == SDL_EVENT_MOUSE_WHEEL) {
                const uint64_t expiry = SDL_GetTicks() + kWheelPulseMs;
                if (event.wheel.y > 0) g_wheel_expiry_ms[0] = expiry; // WHEEL_UP
                else if (event.wheel.y < 0) g_wheel_expiry_ms[1] = expiry; // WHEEL_DOWN
                if (event.wheel.x > 0) g_wheel_expiry_ms[3] = expiry; // WHEEL_RIGHT
                else if (event.wheel.x < 0) g_wheel_expiry_ms[2] = expiry; // WHEEL_LEFT
            }
        }
        if (text_active && (event.type == SDL_EVENT_TEXT_INPUT || event.type == SDL_EVENT_KEY_DOWN)) {
            std::scoped_lock lock{text_mutex};
            if (event.type == SDL_EVENT_TEXT_INPUT) {
                text += event.text.text;
            } else if (event.key.key == SDLK_BACKSPACE && !text.empty()) {
                size_t cut = text.size() - 1; // drop one UTF-8 code point
                while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) --cut;
                text.erase(cut);
            } else if (event.key.key == SDLK_RETURN || event.key.key == SDLK_KP_ENTER || event.key.key == SDLK_ESCAPE) {
                text_state = event.key.key == SDLK_ESCAPE ? 2 : 1;
                text_active = false;
                SDL_StopTextInput(window);
            }
            UpdateTextTitle();
            continue;
        }
        // The controller finishes the text dialog too: Cross (A) accepts, Circle (B) cancels.
        if (text_active && event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN &&
            (event.gbutton.button == SDL_GAMEPAD_BUTTON_SOUTH ||
             event.gbutton.button == SDL_GAMEPAD_BUTTON_EAST)) {
            std::scoped_lock lock{text_mutex};
            text_state = event.gbutton.button == SDL_GAMEPAD_BUTTON_SOUTH ? 1 : 2;
            text_active = false;
            SDL_StopTextInput(window);
            UpdateTextTitle();
            continue;
        }
        if (BbOverlay::HandleEvent(event)) {
            continue;
        }
        switch (event.type) {
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        case SDL_EVENT_WINDOW_RESIZED: {
            int w = 0, h = 0;
            SDL_GetWindowSizeInPixels(window, &w, &h);
            width = w;
            height = h;
            break;
        }
        case SDL_EVENT_QUIT:
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            is_open = false;
            break;
        default:
            break;
        }
    }
    // MOU-004: re-evaluated every PollEvents (not only on the events that could change it),
    // because the menu can also close from the render thread (bbport_overlay.cpp's "Close"
    // button sets menu_open there, not through an SDL event here).
    UpdateMouseCapture();
    UpdateMouseCamera();
    return is_open;
}

void WindowSDL::UpdateMouseCamera() {
    Core::BloodborneCam& cam = Core::BloodborneCam::Instance();
    const bool wanted = cam.IsConfigured() && mouse_mode_available.load(std::memory_order_relaxed) &&
                        mouse_mode_on.load(std::memory_order_relaxed);
    if (!wanted) {
        cam_hook_failed = false; // F7/F8 retry after a failed attempt
        if (cam.IsEnabled()) {
            cam.SetEnabled(false);
        }
    } else if (!cam.IsEnabled() && !cam_hook_failed && mouse_captured_last) {
        // Hooked once the game is actually being played (captured), F7 off unhooks so the
        // stick/arrow keys drive the camera again.
        cam_hook_failed = !cam.SetEnabled(true);
    }
    BbMouse::SetActive(mouse_captured_last && cam.IsEnabled());
}

bool WindowSDL::WantsMouseCapture() const {
    // MOU-003: all of mouse mode on, window focused, menu closed, text dialog inactive.
    // BbOverlay::CapturesInput() already covers both the menu and the text entry box.
    return mouse_mode_on.load(std::memory_order_relaxed) && window_focused && !BbOverlay::CapturesInput();
}

void WindowSDL::UpdateMouseCapture() {
    const bool wants = WantsMouseCapture();
    if (wants == mouse_captured_last) {
        return;
    }
    mouse_captured_last = wants;
    SDL_SetWindowRelativeMouseMode(window, wants);
    // Enabling relative mode hides the cursor by itself; disabling it does not document giving
    // it back, so that side is made explicit here (SDL_ShowCursor is a no-op if already shown).
    if (!wants) {
        SDL_ShowCursor();
    }
    {
        // MOU-005/MOU-007: leaving capture drops whatever motion/buttons/wheel accumulated so
        // far -- a button still held when capture ends must never reach the pad as a press.
        std::scoped_lock lock{mouse_mutex};
        mouse.dx = mouse.dy = 0.0f;
        mouse.buttons = 0;
        mouse.wheel = 0;
        mouse.captured = wants;
    }
    if (!wants) {
        g_wheel_expiry_ms.fill(0);
    }
}

void WindowSDL::TakeMouseInput(MouseInput& out) {
    std::scoped_lock lock{mouse_mutex};
    const uint64_t now = SDL_GetTicks();
    mouse.wheel = 0;
    for (size_t i = 0; i < g_wheel_expiry_ms.size(); ++i) {
        if (g_wheel_expiry_ms[i] > now) mouse.wheel |= (1u << i);
    }
    out = mouse;
    // Motion is drained (MOU-008: a delta is reported once, to the next sample only); held
    // buttons are not -- a button still down stays down across calls until its SDL_EVENT_MOUSE_
    // BUTTON_UP, same as SDL_GetMouseState would report. UpdateMouseCapture() is what clears
    // buttons/wheel on the capture->uncaptured transition (MOU-005/MOU-007), not this function.
    mouse.dx = mouse.dy = 0.0f;
}

void WindowSDL::ConfigureInput(bool mouse_mode_available_, int32_t toggle_scancode_, int32_t reload_scancode_) {
    mouse_mode_available.store(mouse_mode_available_, std::memory_order_relaxed);
    toggle_scancode.store(toggle_scancode_, std::memory_order_relaxed);
    reload_scancode.store(reload_scancode_, std::memory_order_relaxed);
    if (!mouse_mode_available_) {
        // MOU-001: without mouse_to_joystick in input.ini, the mode does not exist; turning it
        // off here also covers an F8 reload that removed the line while it was on.
        mouse_mode_on.store(false, std::memory_order_relaxed);
    } else if (!input_configured_once.exchange(true, std::memory_order_relaxed)) {
        // MOU-002: "mode starts on" applies only to the very first load (game startup). A
        // later F8 reload that keeps mouse_to_joystick present must not re-enable a mode the
        // player turned off with the hotkey in between.
        mouse_mode_on.store(true, std::memory_order_relaxed);
    }
}

int WindowSDL::TakeInputReloadRequested() {
    return reload_requested.exchange(false, std::memory_order_relaxed) ? 1 : 0;
}

} // namespace Frontend
