// SPDX-License-Identifier: GPL-2.0-or-later
// bbport BB_TIMELINE=<file>: a timeline of the frame's way to the GPU (guest submissions and
// waits, decoding, Vulkan submissions, GPU completion), to see where the GPU waits for work.
// Recording starts BB_TIMELINE_START seconds (default 30) after the first event and stops when
// the buffer is full (~15 s of play); the file is then written once: "ns event arg arg2" per line.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>

namespace BbTimeline {

enum Event : std::uint32_t {
    GuestSubmit,       ///< arg: queue (0 graphics), arg2: submission number
    GuestSubmitDone,   ///< arg: frame
    GuestWaitBegin,    ///< the guest waits for the previous frame
    GuestWaitEnd,
    DecodeBegin,       ///< arg: queue, arg2: decoded submissions so far
    DecodeEnd,
    PipeTask,          ///< arg: 0 signals flush, 1 frame retired, 2 idle signal, 3 end of frame
    VulkanSubmit,      ///< arg: tick, arg2: 1 on the recording thread
    GpuDone,           ///< arg: tick the GPU has reached
    FrameRetired,      ///< arg: frame
    DecoderIdleBegin,  ///< the GPU command thread has nothing to decode
    DecoderIdleEnd,
};

struct Entry {
    std::uint64_t ns;
    std::uint32_t event;
    std::uint32_t arg;
    std::uint64_t arg2;
};

inline constexpr std::uint64_t Capacity = 400000;

struct State {
    const char* path = std::getenv("BB_TIMELINE");
    std::int64_t start_ns = [] {
        const char* env = std::getenv("BB_TIMELINE_START");
        return std::int64_t(env ? std::atof(env) * 1e9 : 30e9);
    }();
    std::atomic<std::int64_t> first_ns{0};
    std::atomic<std::uint64_t> next{0};
    std::atomic<bool> written{false};
    std::unique_ptr<Entry[]> entries = path ? std::make_unique<Entry[]>(Capacity) : nullptr;
};

inline State& Get() {
    static State state;
    return state;
}

inline std::int64_t Now() {
    return std::chrono::steady_clock::now().time_since_epoch().count();
}

inline void Write(State& s) {
    if (s.written.exchange(true)) {
        return;
    }
    if (FILE* f = std::fopen(s.path, "w")) {
        for (std::uint64_t i = 0; i < Capacity; ++i) {
            const auto& e = s.entries[i];
            std::fprintf(f, "%llu %u %u %llu\n", (unsigned long long)e.ns, e.event, e.arg,
                         (unsigned long long)e.arg2);
        }
        std::fclose(f);
        std::printf("Timeline: %llu events written to %s\n", (unsigned long long)Capacity, s.path);
    }
}

inline void Note(Event event, std::uint64_t arg = 0, std::uint64_t arg2 = 0) {
    auto& s = Get();
    if (!s.path) {
        return;
    }
    const std::int64_t now = Now();
    std::int64_t first = s.first_ns.load(std::memory_order_relaxed);
    if (first == 0) {
        s.first_ns.compare_exchange_strong(first, now);
        first = s.first_ns.load(std::memory_order_relaxed);
    }
    if (now - first < s.start_ns) {
        return;
    }
    const std::uint64_t index = s.next.fetch_add(1, std::memory_order_relaxed);
    if (index < Capacity) {
        s.entries[index] = {std::uint64_t(now), std::uint32_t(event), std::uint32_t(arg), arg2};
        if (index == Capacity - 1) {
            Write(s);
        }
    }
}

} // namespace BbTimeline
