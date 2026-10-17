// SPDX-License-Identifier: GPL-2.0-or-later
// Standalone CPU regression checks for frame-generation scheduling and history.
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include "gpu/shadps4/video_core/renderer_vulkan/fg_timing.h"

int main() {
    // Vulkan may complete a source frame before VideoOut's source cap permits
    // presenting it. That cap delay must not permanently expire all FG slots.
    for (unsigned factor : {2u, 3u, 4u}) {
        Vulkan::FgSchedule capped(true, true);
        for (unsigned frame = 1; frame < 200; ++frame) {
            const auto gpu = 1000000000ull + frame * 33333333ull;
            const auto host = static_cast<std::int64_t>(gpu) + 5000000000ll;
            // Establish a close calibration, then introduce sustained flip wait.
            const auto permitted = host + (frame < 3 ? 2000000ll : 45000000ll);
            capped.observe(gpu, gpu + 1500000, frame, permitted, 33.333f, 30);
            assert(capped.deadline(1, factor) >= permitted);
            assert(capped.deadline(1, factor) == permitted + 2000000ll);
            assert(capped.next_position(1, factor, permitted) == 1);
            assert(capped.deadline(factor, factor) <= permitted + 66666666ll);
        }
    }
    // 1. History tracking: separate rendered frame serials from accepted presentation flips.
    Vulkan::FgHistory history;
    assert(history.needs_reset(1));
    history.submitted(1);
    assert(!history.needs_reset(2));
    // Dropped render serial (e.g. 1 -> 3): motion vectors no longer match consecutive frames.
    assert(history.needs_reset(3));
    history.submitted(3);
    assert(!history.needs_reset(4)); // Recovered next consecutive pair
    assert(history.needs_reset(3));  // Repeated anchor/render serial must trigger reset
    assert(history.needs_reset(0));  // Invalid frame 0
    history.clear();
    assert(history.needs_reset(4));  // Reset after resolution change, failure, or clear

    // 2. Real deadline calculation (fg_real_deadline_ns).
    constexpr std::int64_t start = 1000000000ll;
    assert(Vulkan::fg_real_deadline_ns(start, 16.0f, 60) == start + 8333333);
    assert(Vulkan::fg_real_deadline_ns(start, 25.0f, 60) == start + 12500000);
    assert(Vulkan::fg_real_deadline_ns(start, 40.0f, 60) == start + 20000000);

    // Slow GPU evaluation must exhaust the spacing budget rather than adding a delay.
    const auto ready = start + 30000000ll;
    assert(Vulkan::fg_real_deadline_ns(start, 25.0f, 60) < ready);
    assert(Vulkan::fg_real_deadline_ns(start, std::numeric_limits<float>::quiet_NaN(), 60) == start + 8333333);
    assert(Vulkan::fg_real_deadline_ns(start, 500.0f, 60) == start + 50000000);
    assert(Vulkan::fg_real_deadline_ns(start, 20.0f, 0) == start + 10000000);
    assert(Vulkan::fg_real_deadline_ns(start, 20.0f, 60, 4, 1) == start + 5000000);
    assert(Vulkan::fg_real_deadline_ns(start, 20.0f, 60, 4, 2) == start + 10000000);
    assert(Vulkan::fg_real_deadline_ns(start, 20.0f, 60, 4) == start + 15000000);

    // 3. Cadence below cap and varying 30-60 FPS.
    // GPU source is 40 FPS (25 ms) while game cap is 60 FPS.
    Vulkan::FgSchedule schedule;
    schedule.observe(100000000, 104000000, 1, start, 16.7f, 60);
    schedule.observe(125000000, 129000000, 2, start + 25000000ll, 16.7f, 60);
    assert(schedule.period() == 25000000ll);

    // Source varying between 30 and 60 FPS.
    Vulkan::FgSchedule varying_fps(true, false);
    std::uint64_t gpu_src = 1000000000ull;
    std::int64_t host_t = start;
    for (unsigned i = 1; i <= 60; ++i) {
        // Alternate periods between 30 FPS (~33.3ms) and 60 FPS (~16.7ms)
        const std::uint64_t delta_ns = (i % 2 == 0) ? 33333333ull : 16666667ull;
        gpu_src += delta_ns;
        host_t += static_cast<std::int64_t>(delta_ns);
        varying_fps.observe(gpu_src, gpu_src + 3000000ull, i, host_t + 3000000ll, 25.0f, 60);
    }
    // Smoothed period should stay within [16.6ms, 33.4ms] bounds
    assert(varying_fps.period() >= 16666667ll && varying_fps.period() <= 33333334ll);

    // 4. Factor positions distinct, monotonic deadlines (factors 2, 3, 4).
    for (unsigned factor : {2u, 3u, 4u}) {
        Vulkan::FgSchedule sched(true, false);
        sched.observe(100000000, 103000000, 1, start, 20.0f, 60);
        sched.observe(120000000, 123000000, 2, start + 20000000ll, 20.0f, 60);

        std::int64_t prev_dl = -1;
        for (unsigned pos = 0; pos <= factor; ++pos) {
            const auto dl = sched.deadline(pos, factor);
            if (pos > 0) {
                assert(dl > prev_dl); // strictly monotonic
                // Check spacing matches period() / factor
                const auto step_spacing = sched.period() / factor;
                assert(std::abs((dl - prev_dl) - step_spacing) <= 1);
            }
            prev_dl = dl;
        }
    }

    // 5. Low latency mode deadlines and launch variation.
    Vulkan::FgSchedule early(true, true);
    early.observe(100000000, 104000000, 1, start, 25.0f, 60);
    assert(early.deadline(1, 2) == start + 2000000ll);
    assert(early.deadline(2, 2) == start + 14500000ll);
    assert(early.deadline(4, 4) == start + 20750000ll);

    Vulkan::FgSchedule variable(true, true);
    for (unsigned i = 1; i <= 100; ++i) {
        const auto stamp = 1000000000ull + i * 20000000ull + (i % 2 ? 7000000ull : 0ull);
        const auto completed = stamp + 2000000ull;
        variable.observe(stamp, completed, i, static_cast<std::int64_t>(completed + 100000000ull), 20.0f, 60);
        if (i > 50) {
            assert(variable.deadline(1, 2) >= static_cast<std::int64_t>(completed + 100000000ull));
            assert(variable.deadline(2, 2) - variable.deadline(1, 2) == variable.period() / 2);
        }
    }

    // 6. Expired positions skip and catch-up burst prevention.
    assert(early.next_position(1, 2, early.deadline(2, 2)) == 2);
    assert(schedule.deadline(4, 4) - schedule.deadline(3, 4) == 6250000);
    // Driver wait past subframe deadlines skips them instead of bursting.
    assert(schedule.next_position(1, 4, schedule.deadline(3, 4)) == 3);
    assert(schedule.next_position(1, 4, schedule.deadline(4, 4) + 10000000) == 4);
    assert(schedule.next_position(1, 4, schedule.deadline(1, 4) - 1) == 1);

    // Presentation delay must not become the next source frame's GPU period.
    schedule.observe(150000000, 154000000, 3, start + 80000000ll, 16.7f, 60);
    assert(schedule.period() == 25000000ll);
    // Cadence stays at 25 ms, but a delayed permitted flip restarts the
    // presentation phase instead of reusing deadlines already in the past.
    assert(schedule.deadline(4, 4) == start + 105000000ll);

    // Lost presentation frames retain per-render cadence rather than expanding it.
    schedule.observe(200000000, 204000000, 5, start + 100000000ll, 16.7f, 60);
    assert(schedule.period() == 25000000ll);

    // Driver cap simulation.
    Vulkan::FgSchedule capped;
    capped.observe(100000000, 102000000, 1, start, 16.0f, 60);
    constexpr std::int64_t driver_wait = 8620689;
    const auto evaluation_ready = capped.deadline(0, 4);
    assert(capped.next_position(1, 4, evaluation_ready + driver_wait) == 2);
    assert(capped.next_position(3, 4, evaluation_ready + 2 * driver_wait) == 4);

    // 7. Alternating load phase and late completion rejection.
    Vulkan::FgSchedule smooth, raw(false);
    std::int64_t previous_smooth = 0, previous_raw = 0;
    std::int64_t smooth_max_error = 0, raw_max_error = 0;
    constexpr std::int64_t source_period = 33333334ll;
    for (unsigned i = 1; i < 100; ++i) {
        const auto source = 1000000000ull + i * source_period + (i % 2 ? 2000000ull : 0ull);
        const auto ready_t = source + (i % 2 ? 6000000ull : 2000000ull);
        smooth.observe(source, ready_t, i, static_cast<std::int64_t>(ready_t + 100000000ull), 33.33f, 60);
        raw.observe(source, ready_t, i, static_cast<std::int64_t>(ready_t + 100000000ull), 33.33f, 60);
        const auto a = smooth.deadline(2, 2), b = raw.deadline(2, 2);
        if (i > 50) {
            smooth_max_error = std::max(smooth_max_error, std::abs(a - previous_smooth - source_period));
            raw_max_error = std::max(raw_max_error, std::abs(b - previous_raw - source_period));
            assert(a > previous_smooth); // strictly monotonic launch
        }
        previous_smooth = a; previous_raw = b;
    }
    assert(smooth_max_error < 1500000ll);
    assert(raw_max_error > 9000000ll);

    // 8. Render gaps and reset: loading gap must reset timeline rather than pacing stale deadlines.
    smooth.observe(6000000000ull, 6004000000ull, 200, 6104000000ll, 33.33f, 60);
    assert(smooth.deadline(1, 2) > 6104000000ll);

    // 9. Avoid unbounded synthetic grid: sustained GPU load converges to 30 FPS instead of keeping 60 Hz grid.
    Vulkan::FgSchedule load;
    std::uint64_t src_load = 1000000000ull;
    for (unsigned i = 1; i <= 100; ++i) {
        src_load += (i < 20 ? 16666667ull : static_cast<std::uint64_t>(source_period));
        load.observe(src_load, src_load + 4000000ull, i, static_cast<std::int64_t>(src_load + 104000000ull), 16.67f, 60);
    }
    assert(std::abs(load.period() - source_period) < 10000ll);

    // 10. Jitter straddling cap with backpressure.
    Vulkan::FgSchedule jitter;
    for (unsigned i = 1; i <= 2000; ++i) {
        const auto stamp = 1000000000ull + i * 16666667ull + (i % 2 ? 8000000ull : 0ull);
        const auto ready_j = stamp + 2000000ull;
        if (i > 500 && i % 4) continue; // presentation backpressure
        jitter.observe(stamp, ready_j, i, static_cast<std::int64_t>(ready_j + 100000000ull), 16.67f, 60);
        assert(jitter.deadline(0, 2) <= static_cast<std::int64_t>(ready_j + 120000000ull));
    }

    std::puts("Vulkan frame-generation timing & history tests: PASS");
    return 0;
}
