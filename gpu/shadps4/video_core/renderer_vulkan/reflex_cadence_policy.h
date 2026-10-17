// SPDX-FileCopyrightText: Copyright 2026 bbhost and bbport contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Adaptive accepted-source queue-depth policy for the NVIDIA Reflex path.
//
// Deliberately free of Vulkan, clocks, environment lookups and heap allocation: it is a pure
// state machine, so the decision it makes can be regression-tested on the CPU without a GPU or
// a driver. The caller feeds it the selected source frame-rate cap and the smoothed
// GPU-timestamp source period (see FgSchedule) and the policy answers with the queue depth the
// bounded backlog gate should use for the next accepted source.
//
// Why the depth varies at all: a one-deep gate removes roughly a frame of guest/CPU queueing
// latency and CPU cost while the source reliably hits its cap, but it loses source throughput
// once the GPU is the limiter (heavy upscaling or post work), where a two-deep gate keeps the
// GPU fed. The policy therefore engages one-deep only on sustained cap-tight cadence and falls
// back to two-deep as soon as the source is measurably slow.
//
// The rules live here, in one place, so the runtime hook and its tests cannot drift apart:
//   * two-deep is the default and the answer to every ambiguous observation: a changed cap,
//     an unlimited (<= 0) cap, an invalid period, or an explicit Reset;
//   * one-deep is engaged only after kEngageSustainedNs of consecutive periods within
//     kCapTolerancePermille of the cap, i.e. the source is genuinely hitting the selected cap;
//   * two-deep is restored after kSlowStreakLimit consecutive periods more than
//     kSlowTolerancePermille slower than the cap, i.e. the GPU has become the limiter;
//   * any period outside the tight band clears the sustained history, so a single jittery
//     sample cannot keep one-deep alive and the two decisions cannot oscillate.
#pragma once

#include <cstdint>

namespace Vulkan::Reflex {

class CadencePolicy {
public:
    /// Accepted-source queue depths this policy can select.
    static constexpr std::uint32_t kDepthTwo = 2;
    static constexpr std::uint32_t kDepthOne = 1;

    /// Sustained cap-tight cadence needed before the one-deep gate is engaged (~2 s).
    static constexpr std::int64_t kEngageSustainedNs = 2'000'000'000;
    /// Tight band around the cap: 2 % of the cap period still counts as "hitting the cap".
    static constexpr std::int64_t kCapTolerancePermille = 20;
    /// Slower than the cap by this much (3 %) is a GPU-limited sample.
    static constexpr std::int64_t kSlowTolerancePermille = 30;
    /// Consecutive GPU-limited samples that restore the two-deep gate.
    static constexpr std::uint32_t kSlowStreakLimit = 8;
    /// Saturating bound on the slow streak so a long run can never wrap the counter.
    static constexpr std::uint32_t kSlowStreakMax = 64;
    /// Upper bound on an acceptable source period (1 s, i.e. 1 FPS). Anything slower is
    /// treated as an invalid measurement: it can neither classify the source nor overflow the
    /// sustained accumulator.
    static constexpr std::int64_t kMaxPeriodNs = 1'000'000'000;

    /// Starts (and returns to) the safe two-deep default.
    void Reset() {
        observed_cap = 0;
        sustained_ns = 0;
        slow_streak = 0;
        depth = kDepthTwo;
    }

    /// The queue depth the gate should use right now.
    std::uint32_t CurrentCap() const {
        return depth;
    }

    /// Feeds one source observation and returns the depth for the next accepted source.
    ///
    /// \param cap        selected source frame-rate cap in frames per second; <= 0 means
    ///                   unlimited, which is never a basis for adapting.
    /// \param period_ns  smoothed GPU-timestamp period of the last source frame; <= 0 (or
    ///                   beyond kMaxPeriodNs) is an invalid/missing measurement.
    std::uint32_t Update(int cap, std::int64_t period_ns) {
        if (cap <= 0 || period_ns <= 0 || period_ns > kMaxPeriodNs) {
            // Unlimited or unmeasurable: there is nothing to adapt to, so stay safe.
            Reset();
            return depth;
        }
        if (cap != observed_cap) {
            // A new cap invalidates every measurement collected under the previous one, and
            // the transition sample itself is ambiguous.
            Reset();
            observed_cap = cap;
            return depth;
        }

        const std::int64_t cap_period_ns = 1'000'000'000LL / cap;
        const std::int64_t tight_band = cap_period_ns * kCapTolerancePermille / 1000;
        const std::int64_t slow_band = cap_period_ns * kSlowTolerancePermille / 1000;
        const std::int64_t delta = period_ns - cap_period_ns;
        const std::int64_t magnitude = delta < 0 ? -delta : delta;

        if (magnitude <= tight_band) {
            // Hitting the cap. Add at most what is still missing so the accumulator cannot
            // overflow no matter how long the run is.
            if (sustained_ns < kEngageSustainedNs) {
                const std::int64_t remaining = kEngageSustainedNs - sustained_ns;
                sustained_ns += period_ns < remaining ? period_ns : remaining;
            }
            slow_streak = 0;
            if (sustained_ns >= kEngageSustainedNs) {
                depth = kDepthOne;
            }
            return depth;
        }

        // A miss. Drop the sustained history first, so the one-deep decision has to be earned
        // again and a single jittery period can never leave the gate in a state the
        // measurements do not support.
        sustained_ns = 0;
        if (delta > slow_band) {
            if (slow_streak < kSlowStreakMax) {
                ++slow_streak;
            }
        } else {
            slow_streak = 0;
        }
        if (slow_streak >= kSlowStreakLimit) {
            depth = kDepthTwo;
        }
        return depth;
    }

private:
    std::int64_t observed_cap = 0;
    std::int64_t sustained_ns = 0;
    std::uint32_t slow_streak = 0;
    std::uint32_t depth = kDepthTwo;
};

} // namespace Vulkan::Reflex
