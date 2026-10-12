// SPDX-License-Identifier: GPL-2.0-or-later
// Real Win32/SDL raw-input registration and event-pump-to-camera integration.
#include <windows.h>
#include <SDL3/SDL.h>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>
#include "bbport_mouse.h"
#include "bbport_settings.h"
#include "sdl_window.h"
#include "mouse_camera_fixture.h"
#include "test_assert.h"

struct RestoreDesktop {
    HWND previous = GetForegroundWindow(), owned = nullptr;
    POINT cursor{};
    RestoreDesktop(HWND previous_, POINT cursor_) : previous(previous_), cursor(cursor_) {}
    ~RestoreDesktop() {
        // Do not take focus back if the user switched applications during the test.
        if (GetForegroundWindow() == owned) {
            if (IsWindow(previous)) SetForegroundWindow(previous);
            SetCursorPos(cursor.x, cursor.y);
        }
    }
};

// Keep physical user motion from contaminating the test's count totals. SDL
// labels Win32 SendInput raw packets with device 0 (their native hDevice is NULL).
struct IsolateMotion {
    static constexpr SDL_MouseID TestDevice = 0x12345678;
    bool native = false;
    IsolateMotion() { SDL_SetEventFilter(Filter, this); }
    ~IsolateMotion() { SDL_SetEventFilter(nullptr, nullptr); }
    static bool SDLCALL Filter(void* userdata, SDL_Event* event) {
        if (event->type != SDL_EVENT_MOUSE_MOTION) return true;
        const auto& state = *static_cast<IsolateMotion*>(userdata);
        const auto device = event->motion.which;
        return state.native ? device == 0 : device == TestDevice ||
            device == SDL_TOUCH_MOUSEID || device == SDL_PEN_MOUSEID;
    }
};

static void Put(void* camera, std::size_t offset, float value) {
    std::memcpy(static_cast<unsigned char*>(camera) + offset, &value, sizeof(value));
}
static float Get(void* camera, std::size_t offset) {
    float value;
    std::memcpy(&value, static_cast<unsigned char*>(camera) + offset, sizeof(value));
    return value;
}

int main() {
    SDL_SetHintWithPriority(SDL_HINT_MOUSE_RELATIVE_SYSTEM_SCALE, "1", SDL_HINT_OVERRIDE);
    SDL_SetHintWithPriority(SDL_HINT_MOUSE_RELATIVE_SPEED_SCALE, "7", SDL_HINT_OVERRIDE);
    SDL_SetHintWithPriority(SDL_HINT_MOUSE_RELATIVE_WARP_MOTION, "1", SDL_HINT_OVERRIDE);
    SDL_SetHintWithPriority(SDL_HINT_MOUSE_EMULATE_WARP_WITH_RELATIVE, "1", SDL_HINT_OVERRIDE);
    auto* image = static_cast<unsigned char*>(VirtualAlloc(nullptr, CameraFixture::ImageSize,
        MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    assert(image);
    CameraFixture::Populate(image);
    assert(BbMouse::Install(image, CameraFixture::ImageSize));
    auto& settings = BbSettings::Get();
    settings.mouse_camera = true;
    settings.mouse_sensitivity = 100.f;
    settings.mouse_invert_y = false;
    _putenv_s("BB_HIDDEN_WINDOW", "0");
    _putenv_s("BB_FULLSCREEN", "0");
    const HWND previous = GetForegroundWindow();
    POINT cursor{};
    GetCursorPos(&cursor);
    {
        Frontend::WindowSDL window(640, 160, "Raw mouse input regression");
        RestoreDesktop restore(previous, cursor);
        restore.owned = static_cast<HWND>(window.GetWindowInfo().render_surface);
        SDL_RaiseWindow(window.GetSDLWindow());
        const auto foreground_thread = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
        const bool attached = foreground_thread != GetCurrentThreadId() &&
            AttachThreadInput(GetCurrentThreadId(), foreground_thread, TRUE);
        SetForegroundWindow(restore.owned);
        SetFocus(restore.owned);
        if (attached) AttachThreadInput(GetCurrentThreadId(), foreground_thread, FALSE);
        assert(window.PollEvents());
        if (GetForegroundWindow() == restore.owned) {
            SendMessageW(restore.owned, WM_SETFOCUS, 0, 0);
            assert(window.PollEvents());
        }
        assert(!std::strcmp(SDL_GetHint(SDL_HINT_MOUSE_RELATIVE_SYSTEM_SCALE), "0"));
        assert(!std::strcmp(SDL_GetHint(SDL_HINT_MOUSE_RELATIVE_SPEED_SCALE), "1"));
        assert(!std::strcmp(SDL_GetHint(SDL_HINT_MOUSE_RELATIVE_WARP_MOTION), "0"));
        assert(!std::strcmp(SDL_GetHint(SDL_HINT_MOUSE_EMULATE_WARP_WITH_RELATIVE), "0"));
        if (GetForegroundWindow() != restore.owned ||
            SDL_GetKeyboardFocus() != window.GetSDLWindow()) {
            std::printf("Owned hwnd %p, foreground %p, SDL focus %p, window %p\n",
                restore.owned, GetForegroundWindow(), SDL_GetKeyboardFocus(), window.GetSDLWindow());
            std::puts("SKIP: desktop focus unavailable for native raw mouse integration");
            return 77;
        }
        assert(SDL_GetWindowRelativeMouseMode(window.GetSDLWindow()));
        UINT count = 0;
        assert(GetRegisteredRawInputDevices(nullptr, &count, sizeof(RAWINPUTDEVICE)) != UINT(-1));
        std::vector<RAWINPUTDEVICE> devices(count);
        assert(GetRegisteredRawInputDevices(devices.data(), &count, sizeof(RAWINPUTDEVICE)) != UINT(-1));
        assert(std::any_of(devices.begin(), devices.end(), [](const RAWINPUTDEVICE& device) {
            return device.usUsagePage == 1 && device.usUsage == 2 && !(device.dwFlags & RIDEV_REMOVE);
        }));

        std::array<unsigned char,0x300> camera{};
        IsolateMotion isolation;
        Put(camera.data(), 0x1f0, -1.94f); Put(camera.data(), 0x1ec, 1.71f);
        const auto reset = [&] {
            BbMouse::SetActive(false); BbMouse::SetActive(true);
            Put(camera.data(), 0x140, 0.f); Put(camera.data(), 0x144, 0.f);
        };
        const auto motion = [&](SDL_MouseID device, SDL_WindowID id, float dx, float dy) {
            SDL_Event event{};
            event.type = SDL_EVENT_MOUSE_MOTION;
            event.motion.windowID = id; event.motion.which = device;
            event.motion.x = 9000; event.motion.y = 6000; // cursor position/DPI must not affect counts
            event.motion.xrel = dx; event.motion.yrel = dy;
            assert(SDL_PushEvent(&event)); assert(window.PollEvents());
            BbMouse::Apply(camera.data());
        };
        const auto id = SDL_GetWindowID(window.GetSDLWindow());
        reset();
        motion(IsolateMotion::TestDevice, id, 90, 90);
        assert(Get(camera.data(), 0x140) == Get(camera.data(), 0x144));
        assert(Get(camera.data(), 0x148) == Get(camera.data(), 0x140));
        assert(Get(camera.data(), 0x14c) == Get(camera.data(), 0x144));
        for (const int packet : {1, 30, 900}) {
            reset();
            for (int n = 0; n < 900; n += packet)
                motion(IsolateMotion::TestDevice, id, float(packet), float(packet) / 2.f);
            if (GetForegroundWindow() != restore.owned) return 77;
            std::printf("SDL packets of %d: pitch %.6f, yaw %.6f\n", packet,
                        Get(camera.data(), 0x140), Get(camera.data(), 0x144));
            assert(std::abs(Get(camera.data(), 0x144) - 1.f) < .00001f);
            assert(std::abs(Get(camera.data(), 0x140) - .5f) < .00001f);
        }
        reset();
        motion(SDL_TOUCH_MOUSEID, id, 900, 450);
        motion(SDL_PEN_MOUSEID, id, 900, 450);
        motion(IsolateMotion::TestDevice, id + 1, 900, 450);
        assert(Get(camera.data(), 0x140) == 0.f && Get(camera.data(), 0x144) == 0.f);

        // SendInput produces real native mouse packets, unlike SDL_PushEvent.
        // Prime the relative backend, then verify the kernel-to-SDL-to-camera path.
        INPUT input{};
        isolation.native = true;
        input.type = INPUT_MOUSE;
        input.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_MOVE_NOCOALESCE;
        input.mi.dx = 1;
        assert(SendInput(1, &input, sizeof(input)) == 1);
        for (unsigned n = 0; n < 10; ++n) { SDL_Delay(5); assert(window.PollEvents()); }
        for (const int packet : {6, 30, 90}) {
            reset();
            input.mi.dx = packet; input.mi.dy = packet / 2;
            for (int count = 0; count < 90; count += packet) {
                assert(SendInput(1, &input, sizeof(input)) == 1);
                if (packet == 6) SDL_Delay(2); // slow stream versus a single flick
                assert(window.PollEvents());
            }
            for (unsigned n = 0; n < 10; ++n) { SDL_Delay(5); assert(window.PollEvents()); }
            BbMouse::Apply(camera.data());
            if (GetForegroundWindow() != restore.owned) return 77;
            std::printf("Native raw packets of %d: pitch %.6f, yaw %.6f\n", packet,
                        Get(camera.data(), 0x140), Get(camera.data(), 0x144));
            assert(std::abs(Get(camera.data(), 0x140) - .05f) < .00001f);
            assert(std::abs(Get(camera.data(), 0x144) - .1f) < .00001f);
            SDL_Delay(10); assert(window.PollEvents()); BbMouse::Apply(camera.data());
            assert(std::abs(Get(camera.data(), 0x144) - .1f) < .00001f);
        }
        settings.mouse_camera = false;
        assert(window.PollEvents());
        assert(!SDL_GetWindowRelativeMouseMode(window.GetSDLWindow()));
        isolation.native = false;
        motion(IsolateMotion::TestDevice, id, 900, 450);
        assert(std::abs(Get(camera.data(), 0x144) - .1f) < .00001f);
    }
    VirtualFree(image, 0, MEM_RELEASE);
    std::puts("PASS: native Raw Input registration, unscaled Win32 packets, linear event-pump camera input, synthetic-device filtering and capture release");
}
