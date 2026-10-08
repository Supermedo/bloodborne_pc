// bbport: the game window. Created by the VideoOut driver on first open; the
// event pump runs on the port's window thread (see window.cpp).
#pragma once
#include <atomic>
#include <mutex>
#include <string>
#include "common/types.h"

struct SDL_Window;

namespace Frontend {

enum class WindowSystemType : u8 { Headless, Windows, X11, Wayland, Metal };

struct WindowSystemInfo {
    void* display_connection = nullptr;
    void* render_surface = nullptr;
    float render_surface_scale = 1.0f;
    WindowSystemType type = WindowSystemType::Headless;
};

/// Mouse state accumulated by the window thread and consumed by the pad thread
/// (bbgpu_mouse_take / runtime_pad.c). Mirrors BbMouseInput in gpu/bbgpu.h; see
/// specs/keyboard-and-mouse/spec-design-keyboard-mouse-input.md section 4.5 (API-002): the
/// five fields are read and reset as one group under mouse_mutex, never field by field with
/// separate atomics, so a caller never observes e.g. buttons without the captured flag that
/// was true when they were set.
struct MouseInput {
    float dx = 0.0f, dy = 0.0f; // relative motion, SDL pixels, since the last take
    uint32_t buttons = 0;       // currently held mouse buttons, SDL_BUTTON_MASK bits
    uint32_t wheel = 0;         // bit 0 up, 1 down, 2 left, 3 right: active 33 ms wheel pulses
    bool captured = false;
};

class WindowSDL {
public:
    WindowSDL(s32 width, s32 height, const char* title);
    ~WindowSDL();
    s32 GetWidth() const { return width.load(std::memory_order_relaxed); }
    s32 GetHeight() const { return height.load(std::memory_order_relaxed); }
    SDL_Window* GetSDLWindow() const { return window; }
    WindowSystemInfo GetWindowInfo() const { return window_info; }
    bool IsOpen() const { return is_open.load(std::memory_order_relaxed); }
    /// Processes pending window events. Returns false once the user closed the window.
    bool PollEvents();
    /// Keyboard text entry for the system IME dialog; typed text shows in the title bar.
    void BeginTextInput(const std::string& initial, const std::string& prompt);
    /// 0 while typing, 1 confirmed (Enter), 2 cancelled (Escape); text is UTF-8.
    int PollTextInput(std::string& text);

    /// Any thread (API-002): copies the accumulated mouse state out and resets dx/dy/buttons/
    /// wheel to zero (captured is left as-is: it reflects the window's current state, not
    /// something to drain). Safe to call with no window yet (default-constructed MouseInput).
    void TakeMouseInput(MouseInput& out);
    /// Any thread, after loading input.ini (initial load or F8 reload): whether mouse-to-
    /// joystick mode exists at all (MOU-001) and the current toggle/reload hotkey scancodes
    /// (SDL_SCANCODE_UNKNOWN = none bound). Takes effect on the next PollEvents.
    void ConfigureInput(bool mouse_mode_available, int32_t toggle_scancode, int32_t reload_scancode);
    /// Any thread: 1 once after the reload hotkey was pressed, then clears back to 0. The pad
    /// thread polls this once per sample() to know when to re-read input.ini (CFG-008).
    int TakeInputReloadRequested();

private:
    std::atomic<s32> width, height;
    std::atomic<bool> is_open{true};
    std::mutex text_mutex;
    bool text_requested{}, text_active{};
    int text_state{};
    std::string text, text_prompt, base_title;
    void UpdateTextTitle();
    SDL_Window* window{};
    WindowSystemInfo window_info{};

    // Mouse capture and hotkeys (window thread only, except where noted): see MOU-002..008,
    // HOT-002, HOT-003 in the spec. `mouse_mutex` guards `mouse` (MouseInput) against the pad
    // thread calling TakeMouseInput concurrently; every other field below is touched only from
    // the window thread (PollEvents and the constructor), so it needs no lock.
    std::mutex mouse_mutex;
    MouseInput mouse;
    std::atomic<bool> mouse_mode_available{false};  // MOU-001: mouse_to_joystick present in input.ini
    std::atomic<bool> mouse_mode_on{false};         // toggled by the hotkey (MOU-002); not saved
    std::atomic<bool> input_configured_once{false}; // MOU-002: turn mouse_mode_on on only on the
                                                      // very first ConfigureInput (startup), never
                                                      // again on an F8 reload (which must not
                                                      // override a mode the player toggled with F7)
    // HOT-002: 0 (SDL_SCANCODE_UNKNOWN) until ConfigureInput runs once; no event ever carries
    // that scancode, so 0 doubles safely as "no key bound" without a separate sentinel.
    std::atomic<int32_t> toggle_scancode{0};
    std::atomic<int32_t> reload_scancode{0};
    std::atomic<bool> reload_requested{false};      // HOT-003, drained by TakeInputReloadRequested
    bool window_focused{true};
    /// Window thread, every PollEvents: MOU-003. True iff mouse mode is on, the window has
    /// focus, and neither the menu nor the text dialog is capturing input.
    bool WantsMouseCapture() const;
    /// Window thread: applies SDL_SetWindowRelativeMouseMode only when WantsMouseCapture()'s
    /// result changed since the last call, and resets the accumulator when capture ends
    /// (MOU-004, MOU-005) so no stale motion or held button survives into the next capture.
    void UpdateMouseCapture();
    bool mouse_captured_last{false};
};

} // namespace Frontend
