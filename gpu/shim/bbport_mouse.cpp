// See bbport_mouse.h. Owns SDL relative-state sampling: the window pump never
// reads it, so motion is neither split nor replayed between the two threads.
#include "bbport_mouse.h"

#include <atomic>
#include <chrono>
#include <thread>

#include <SDL3/SDL.h>

#include "bloodborne_cam.h"
#include "common/logging/log.h"
#include "common/thread.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace BbMouse {
namespace {

std::thread g_thread;
std::atomic<bool> g_running{false};
std::atomic<bool> g_active{false};

#ifdef _WIN32
HANDLE g_stopEvent = nullptr;
HANDLE g_timer = nullptr;
#endif

void Sample() {
    if (g_active.load(std::memory_order_acquire)) {
        float xrel = 0.0f;
        float yrel = 0.0f;
        SDL_GetRelativeMouseState(&xrel, &yrel);
        if (xrel != 0.0f || yrel != 0.0f) {
            Core::BloodborneCam::Instance().OnMouseMotion(xrel, yrel);
        }
    } else {
        // Gated pumps must not bank motion for a later sample.
        SDL_GetRelativeMouseState(nullptr, nullptr);
    }
}

#ifdef _WIN32
void ThreadMain() {
    Common::SetCurrentThreadName("bb:mouse");
    if (g_timer != nullptr) {
        HANDLE handles[2] = {g_stopEvent, g_timer};
        for (;;) {
            const DWORD wait = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
            if (wait != WAIT_OBJECT_0 + 1) {
                break;
            }
            Sample();
        }
    } else {
        while (WaitForSingleObject(g_stopEvent, 1) == WAIT_TIMEOUT) {
            Sample();
        }
    }
}
#else
void ThreadMain() {
    Common::SetCurrentThreadName("bb:mouse");
    while (g_running.load(std::memory_order_acquire)) {
        Sample();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
#endif

} // namespace

void Start() {
    if (g_running.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
#ifdef _WIN32
    g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                     TIMER_ALL_ACCESS);
    if (g_stopEvent == nullptr) {
        g_running.store(false, std::memory_order_release);
        LOG_ERROR(Frontend, "BbMouse: stop event creation failed");
        return;
    }
    if (g_timer != nullptr) {
        LARGE_INTEGER due{};
        due.QuadPart = -10000; // 1 ms, relative
        if (!SetWaitableTimer(g_timer, &due, 1, nullptr, nullptr, FALSE)) {
            CloseHandle(g_timer);
            g_timer = nullptr;
        }
    }
#endif
    g_thread = std::thread(ThreadMain);
}

void Stop() {
    if (!g_running.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
#ifdef _WIN32
    if (g_stopEvent != nullptr) {
        SetEvent(g_stopEvent);
    }
#endif
    if (g_thread.joinable()) {
        g_thread.join();
    }
#ifdef _WIN32
    if (g_timer != nullptr) {
        CloseHandle(g_timer);
        g_timer = nullptr;
    }
    if (g_stopEvent != nullptr) {
        CloseHandle(g_stopEvent);
        g_stopEvent = nullptr;
    }
#endif
}

void SetActive(bool active) {
    g_active.store(active, std::memory_order_release);
}

} // namespace BbMouse
