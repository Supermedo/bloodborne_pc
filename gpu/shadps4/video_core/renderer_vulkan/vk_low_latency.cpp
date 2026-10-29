// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <condition_variable>
#include <deque>
#include <thread>
#include <mutex>
#include <string_view>
#include "bbport_settings.h"
#include "common/logging/log.h"
#include "common/thread.h"
#include "core/emulator_settings.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_low_latency.h"

namespace Vulkan::LowLatency {

namespace {

enum class Mode { Off, Reflex, AntiLag, Pacing }; // Pacing: no driver support, the limit only

struct State {
    Mode mode = Mode::Off;
    bool boost = false;
    /// The frame limit, kept here before each frame starts (FrameSubmitted). A frame held for
    /// its slot after it was made waited there a frame or two: the latency doubled.
    std::chrono::nanoseconds period{};
    std::chrono::steady_clock::time_point next_start{};
    VkDevice device = VK_NULL_HANDLE;

    std::mutex mutex; ///< the swapchain against the markers and the sleep (other threads)
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkSemaphore sleep_semaphore = VK_NULL_HANDLE; ///< Reflex: timeline the driver signals
    u64 sleep_value = 0;
    VkSwapchainLatencyCreateInfoNV swapchain_info{
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_LATENCY_CREATE_INFO_NV,
        .latencyModeEnable = VK_TRUE,
    };

    /// The time from the game starting a frame to its present, always measured (also with low
    /// latency off, to compare): shown by the FPS counter (BbSettings::latency_ms), and with
    /// BB_FRAME_STATS=1 or BB_LATENCY_LOG=1 logged every 5 seconds.
    bool measure = true;
    bool log = false;
    double smoothed_ms = 0.0;
    std::mutex measure_mutex;
    /// One frame's way: the game starts it, submits it, the GPU thread reaches its flip, the
    /// presenter starts presenting it.
    struct Times {
        u64 id;
        std::chrono::steady_clock::time_point start, submit, flip, present;
    };
    std::array<Times, 64> times{};
    std::chrono::steady_clock::time_point window{};
    double latency_ms = 0.0, latency_max_ms = 0.0;
    std::array<double, 4> parts_ms{}; ///< game, GPU thread, waiting to present, present
    double gpu_done_ms = 0.0; ///< from the game starting a frame to the GPU finishing it
    u32 gpu_done_frames = 0;
    // The GPU watcher: waits for each game frame's GPU work (WatchGpu).
    struct GpuWatch {
        u64 id;
        VkSemaphore semaphore;
        u64 value;
    };
    std::mutex watch_mutex;
    std::condition_variable watch_cv;
    std::deque<GpuWatch> watches;
    std::thread watcher;
    u32 latency_frames = 0;

    u64 frame = 1; ///< the frame the game works on (game render thread)
    std::mutex queue_mutex;
    std::deque<u64> submitted; ///< submitted, not flipped yet (game thread to GPU thread)
};

State state;

std::string_view Setting() {
    const char* env = std::getenv("BB_LOW_LATENCY");
    return env ? std::string_view{env} : std::string_view{};
}

void Marker(VkLatencyMarkerNV marker, u64 id) {
    std::scoped_lock lock{state.mutex};
    if (!state.swapchain) {
        return;
    }
    const VkSetLatencyMarkerInfoNV info{
        .sType = VK_STRUCTURE_TYPE_SET_LATENCY_MARKER_INFO_NV,
        .presentID = id,
        .marker = marker,
    };
    VULKAN_HPP_DEFAULT_DISPATCHER.vkSetLatencyMarkerNV(state.device, state.swapchain, &info);
}

void AntiLag(VkAntiLagStageAMD stage, u64 id) {
    const VkAntiLagPresentationInfoAMD presentation{
        .sType = VK_STRUCTURE_TYPE_ANTI_LAG_PRESENTATION_INFO_AMD,
        .stage = stage,
        .frameIndex = id,
    };
    const VkAntiLagDataAMD data{
        .sType = VK_STRUCTURE_TYPE_ANTI_LAG_DATA_AMD,
        .mode = VK_ANTI_LAG_MODE_ON_AMD,
        .maxFPS = 0, // the limit is kept in FrameSubmitted
        .pPresentationInfo = &presentation,
    };
    VULKAN_HPP_DEFAULT_DISPATCHER.vkAntiLagUpdateAMD(state.device, &data);
}

// Reflex: the driver signals the semaphore when the next frame should begin.
void ReflexSleep() {
    u64 value = 0;
    {
        std::scoped_lock lock{state.mutex};
        if (!state.swapchain) {
            return;
        }
        value = ++state.sleep_value;
        const VkLatencySleepInfoNV info{
            .sType = VK_STRUCTURE_TYPE_LATENCY_SLEEP_INFO_NV,
            .signalSemaphore = state.sleep_semaphore,
            .value = value,
        };
        if (VULKAN_HPP_DEFAULT_DISPATCHER.vkLatencySleepNV(state.device, state.swapchain, &info) !=
            VK_SUCCESS) {
            return;
        }
    }
    // Bounded: nothing is presented while the window is minimized.
    const VkSemaphoreWaitInfo wait{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
        .semaphoreCount = 1,
        .pSemaphores = &state.sleep_semaphore,
        .pValues = &value,
    };
    VULKAN_HPP_DEFAULT_DISPATCHER.vkWaitSemaphores(state.device, &wait, 100'000'000);
}

bool Active() {
    return state.mode != Mode::Off || state.measure;
}

const char* ModeName() {
    switch (state.mode) {
    case Mode::Reflex:
        return state.boost ? "NVIDIA Reflex + Boost" : "NVIDIA Reflex";
    case Mode::AntiLag:
        return "AMD Anti-Lag 2";
    case Mode::Pacing:
        return "low latency pacing";
    default:
        return "low latency off";
    }
}

bool Flag(const char* name) {
    const char* env = std::getenv(name);
    return env && env[0] == '1';
}

} // namespace

void WatchLoop() {
    while (true) {
        State::GpuWatch watch;
        {
            std::unique_lock lock{state.watch_mutex};
            state.watch_cv.wait(lock, [] { return !state.watches.empty(); });
            watch = state.watches.front();
            state.watches.pop_front();
        }
        const VkSemaphoreWaitInfo wait{
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
            .semaphoreCount = 1,
            .pSemaphores = &watch.semaphore,
            .pValues = &watch.value,
        };
        if (VULKAN_HPP_DEFAULT_DISPATCHER.vkWaitSemaphores(state.device, &wait, 1'000'000'000) !=
            VK_SUCCESS) {
            continue;
        }
        const auto now = std::chrono::steady_clock::now();
        std::scoped_lock lock{state.measure_mutex};
        const auto& times = state.times[watch.id % state.times.size()];
        if (times.id == watch.id) {
            state.gpu_done_ms +=
                std::chrono::duration<double, std::milli>(now - times.start).count();
            ++state.gpu_done_frames;
        }
    }
}

bool Wanted() {
    const auto setting = Setting();
    return setting == "on" || setting == "boost";
}

void Init(const Instance& instance) {
    state.log = Flag("BB_FRAME_STATS") || Flag("BB_LATENCY_LOG");
    state.device = instance.GetDevice();
    if (state.log) {
        state.watcher = std::thread{WatchLoop};
        state.watcher.detach();
    }
    if (const u32 limit = EmulatorSettings.GetFrameLimit()) {
        state.period = std::chrono::nanoseconds(1'000'000'000 / limit);
    }
    if (!Wanted()) {
        // VRR with V-Sync: the frame is paced before it starts, or V-Sync and the limiter
        // would each hold it (a frame or two of latency).
        if (EmulatorSettings.IsVrr() && state.period.count()) {
            state.mode = Mode::Pacing;
            BbSettings::Get().latency_mode.store(4);
        }
        return;
    }
    state.device = instance.GetDevice();
    state.boost = Setting() == "boost";
    if (instance.IsNvLowLatencyEnabled()) {
        const VkSemaphoreTypeCreateInfo type{
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
            .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
        };
        const VkSemaphoreCreateInfo info{
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
            .pNext = &type,
        };
        if (VULKAN_HPP_DEFAULT_DISPATCHER.vkCreateSemaphore(state.device, &info, nullptr,
                                                            &state.sleep_semaphore) ==
            VK_SUCCESS) {
            state.mode = Mode::Reflex;
        }
    } else if (instance.IsAmdAntiLagEnabled()) {
        state.mode = Mode::AntiLag;
    } else {
        state.mode = Mode::Pacing;
    }
    BbSettings::Get().latency_mode.store(state.mode == Mode::Reflex    ? (state.boost ? 2 : 1)
                                         : state.mode == Mode::AntiLag ? 3
                                         : state.mode == Mode::Pacing  ? 4
                                                                       : 0);
    switch (state.mode) {
    case Mode::Reflex:
        std::printf("Low latency: NVIDIA Reflex%s\n", state.boost ? " + Boost" : "");
        break;
    case Mode::AntiLag:
        std::printf("Low latency: AMD Anti-Lag 2\n");
        break;
    case Mode::Pacing:
        std::printf("Low latency: frame pacing only (this GPU or driver has neither NVIDIA Reflex, "
                    "VK_NV_low_latency2, nor AMD Anti-Lag 2, VK_AMD_anti_lag)\n");
        break;
    case Mode::Off:
        break;
    }
}

const void* SwapchainCreateNext() {
    return state.mode == Mode::Reflex ? &state.swapchain_info : nullptr;
}

void OnSwapchain(vk::SwapchainKHR swapchain) {
    if (state.mode != Mode::Reflex) {
        return;
    }
    std::scoped_lock lock{state.mutex};
    state.swapchain = swapchain;
    if (!swapchain) {
        // A game thread waiting for the old swapchain's signal goes on.
        const VkSemaphoreSignalInfo signal{
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO,
            .semaphore = state.sleep_semaphore,
            .value = ++state.sleep_value,
        };
        VULKAN_HPP_DEFAULT_DISPATCHER.vkSignalSemaphore(state.device, &signal);
        return;
    }
    const VkLatencySleepModeInfoNV mode{
        .sType = VK_STRUCTURE_TYPE_LATENCY_SLEEP_MODE_INFO_NV,
        .lowLatencyMode = VK_TRUE,
        .lowLatencyBoost = state.boost ? VK_TRUE : VK_FALSE,
        .minimumIntervalUs = 0, // the limit is kept in FrameSubmitted
    };
    const auto result =
        VULKAN_HPP_DEFAULT_DISPATCHER.vkSetLatencySleepModeNV(state.device, swapchain, &mode);
    if (result != VK_SUCCESS) {
        LOG_WARNING(Render_Vulkan, "Reflex: the driver did not take the sleep mode ({})",
                    static_cast<int>(result));
    }
}

void FrameSubmitted() {
    if (!Active()) {
        return;
    }
    const u64 frame = state.frame;
    if (state.measure) {
        std::scoped_lock lock{state.measure_mutex};
        auto& times = state.times[frame % state.times.size()];
        if (times.id == frame) {
            times.submit = std::chrono::steady_clock::now();
        }
    }
    if (state.mode == Mode::Reflex) {
        Marker(VK_LATENCY_MARKER_SIMULATION_END_NV, frame);
        Marker(VK_LATENCY_MARKER_RENDERSUBMIT_START_NV, frame);
    }
    {
        std::scoped_lock lock{state.queue_mutex};
        // Flips that never came (a frame the game dropped) must not hold the ids back.
        while (state.submitted.size() >= 8) {
            state.submitted.pop_front();
        }
        state.submitted.push_back(frame);
    }

    const u64 next = state.frame = frame + 1;
    if (state.mode == Mode::Reflex) {
        ReflexSleep();
        Marker(VK_LATENCY_MARKER_SIMULATION_START_NV, next);
        Marker(VK_LATENCY_MARKER_INPUT_SAMPLE_NV, next);
    } else if (state.mode == Mode::AntiLag) {
        AntiLag(VK_ANTI_LAG_STAGE_INPUT_AMD, next); // waits in the driver
    }
    if (PacesFrames()) {
        // As VideoOut's limiter did: the next start a period after the slot, but no earlier
        // than a period minus a little after this one (a late frame does not make the next
        // one early).
        const auto now = std::chrono::steady_clock::now();
        if (state.next_start > now) {
            Common::PreciseSleepUntil(state.next_start);
        }
        const auto started = std::chrono::steady_clock::now();
        state.next_start = std::max(state.next_start + state.period,
                                    started + state.period - state.period / 16);
    }
    if (state.measure) {
        std::scoped_lock lock{state.measure_mutex};
        const auto now = std::chrono::steady_clock::now();
        state.times[next % state.times.size()] = {next, now, now, now, now};
    }
}

bool PacesFrames() {
    return state.mode != Mode::Off && state.period.count() != 0;
}

u64 RenderSubmitEnd() {
    if (!Active()) {
        return 0;
    }
    u64 id = 0;
    {
        std::scoped_lock lock{state.queue_mutex};
        if (!state.submitted.empty()) {
            id = state.submitted.front();
            state.submitted.pop_front();
        }
    }
    if (id && state.mode == Mode::Reflex) {
        Marker(VK_LATENCY_MARKER_RENDERSUBMIT_END_NV, id);
    }
    if (id && state.measure) {
        std::scoped_lock lock{state.measure_mutex};
        auto& times = state.times[id % state.times.size()];
        if (times.id == id) {
            times.flip = std::chrono::steady_clock::now();
        }
    }
    return id;
}

void PresentStart(u64 id) {
    if (!id) {
        return;
    }
    if (state.measure) {
        std::scoped_lock lock{state.measure_mutex};
        auto& times = state.times[id % state.times.size()];
        if (times.id == id) {
            times.present = std::chrono::steady_clock::now();
        }
    }
    if (state.mode == Mode::Reflex) {
        Marker(VK_LATENCY_MARKER_PRESENT_START_NV, id);
    } else if (state.mode == Mode::AntiLag) {
        AntiLag(VK_ANTI_LAG_STAGE_PRESENT_AMD, id);
    }
}

void PresentEnd(u64 id) {
    if (id && state.mode == Mode::Reflex) {
        Marker(VK_LATENCY_MARKER_PRESENT_END_NV, id);
    }
    if (!id || !state.measure) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    std::scoped_lock lock{state.measure_mutex};
    const auto& times = state.times[id % state.times.size()];
    if (times.id == id) {
        const auto span = [](auto from, auto to) {
            return std::chrono::duration<double, std::milli>(to - from).count();
        };
        const double ms = span(times.start, now);
        state.parts_ms[0] += span(times.start, times.submit);
        state.parts_ms[1] += span(times.submit, times.flip);
        state.parts_ms[2] += span(times.flip, times.present);
        state.parts_ms[3] += span(times.present, now);
        state.latency_ms += ms;
        state.latency_max_ms = std::max(state.latency_max_ms, ms);
        ++state.latency_frames;
        // Smoothed over about a second for the counter; loading hitches do not stick.
        const double sample = std::min(ms, 250.0);
        state.smoothed_ms = state.smoothed_ms == 0.0
                                ? sample
                                : state.smoothed_ms + (sample - state.smoothed_ms) * 0.02;
        BbSettings::Get().latency_ms.store(float(state.smoothed_ms), std::memory_order_relaxed);
    }
    if (state.window == std::chrono::steady_clock::time_point{}) {
        state.window = now;
    }
    if (now - state.window >= std::chrono::seconds(5)) {
        if (state.latency_frames && state.log) {
            const double n = state.latency_frames;
            std::printf("Latency: %.1f ms from the game starting a frame to its present (worst "
                        "%.1f ms, %u frames, %s): game %.1f, GPU thread %.1f, waiting to present "
                        "%.1f, present %.1f ms; GPU finished it at %.1f ms\n",
                        state.latency_ms / n, state.latency_max_ms, state.latency_frames,
                        ModeName(), state.parts_ms[0] / n, state.parts_ms[1] / n,
                        state.parts_ms[2] / n, state.parts_ms[3] / n,
                        state.gpu_done_frames ? state.gpu_done_ms / state.gpu_done_frames : 0.0);
        }
        state.window = now;
        state.latency_ms = state.latency_max_ms = 0.0;
        state.parts_ms = {};
        state.gpu_done_ms = 0.0;
        state.gpu_done_frames = 0;
        state.latency_frames = 0;
    }
}

void WatchGpu(u64 id, vk::Semaphore semaphore, u64 value) {
    if (!id || !state.log || !semaphore) {
        return;
    }
    {
        std::scoped_lock lock{state.watch_mutex};
        if (state.watches.size() < 16) {
            state.watches.push_back({id, semaphore, value});
        }
    }
    state.watch_cv.notify_one();
}

} // namespace Vulkan::LowLatency
