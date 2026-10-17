// SPDX-FileCopyrightText: Copyright 2026 bbhost and bbport contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Adapted from bbhost's frame-generation cadence and history tracking.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace Vulkan {

// Motion vectors describe consecutive rendered frames, not consecutive flips
// accepted by the presentation mailbox. Keep those two serials separate.
class FgHistory {
public:
    bool needs_reset(std::uint64_t render_frame) const {
        return !render_frame || !last_ || render_frame != last_ + 1;
    }
    void submitted(std::uint64_t render_frame) { last_ = render_frame; }
    void clear() { last_ = 0; }
private:
    std::uint64_t last_ = 0;
};

// GPU timestamps measure real source cadence even when the CPU records at the
// selected cap. Map the GPU clock to steady_clock with the earliest observed
// fence completion; later observations can include display/driver waiting.
class FgSchedule {
public:
    explicit FgSchedule(bool smooth = true, bool low_latency = false)
        : smooth_(smooth), low_latency_(low_latency) {}
    void observe(std::uint64_t source, std::uint64_t ready, std::uint64_t render,
                 std::int64_t host_ready, float fallback_ms, int game_cap) {
        minimum_ = game_cap > 0 ? 1000000000ll / game_cap : 8000000ll;
        std::int64_t period = std::isfinite(fallback_ms) ?
            static_cast<std::int64_t>(std::clamp<double>(fallback_ms, 8.0, 100.0) * 1e6) : minimum_;
        if (source && last_source_ && source > last_source_ && render > last_render_) {
            const auto measured = (source - last_source_) / (render - last_render_);
            if (measured >= 1000000 && measured <= 100000000) period = measured;
        }
        const auto frames = render > last_render_ ? render - last_render_ : 0;
        const bool continuous = clock_valid_ && frames && frames <= 8 &&
            source > last_source_ && source - last_source_ <= 500000000;
        if (smooth_ && continuous && cadence_valid_)
            period_ += (period - period_) / 8;
        else period_ = smooth_ ? period : std::max(minimum_, period);
        if (continuous) cadence_valid_ = true;
        if (source && ready >= source) {
            const auto offset = host_ready - static_cast<std::int64_t>(ready);
            if (!clock_valid_ || offset < offset_) offset_ = offset;
            clock_valid_ = true;
            const auto evaluation = static_cast<std::int64_t>(ready - source);
            // A slowly decaying evaluation budget absorbs ordinary NGX/FSR cost
            // variation. It does not make a fast/slow pair alternate its phase.
            evaluation_ = smooth_ && continuous ?
                std::max(evaluation, evaluation_ - evaluation_ / 200) : evaluation;
            const auto target = offset_ + static_cast<std::int64_t>(source) + evaluation_;
            if (smooth_ && continuous && cadence_valid_) {
                const auto predicted = source_host_ + period_ * static_cast<std::int64_t>(frames);
                // A first subframe scheduled before a late GPU completion
                // shortens its generated->real interval. Retain a bounded,
                // slowly decaying launch budget for those observed late anchors
                // instead of imposing the lowest possible delay under all loads.
                launch_variation_ = std::max(std::max<std::int64_t>(0, target - predicted),
                                            launch_variation_ - launch_variation_ / 200);
                // Follow sustained load changes while rejecting individual
                // source/evaluation spikes. Presentation remains bounded by
                // the owned frame sets; expired subframes are skipped.
                const auto limit = this->period() / 32;
                source_host_ = predicted + std::clamp((target - predicted) / 8, -limit, limit);
                // A long burst of irregular completions must not build up
                // presentation latency or feed output-set drops back into
                // an increasingly late phase. Keep smoothing within half a
                // source period of the current GPU anchor.
                source_host_ = std::clamp(source_host_, target - this->period() / 2,
                                                       target + this->period() / 2);
            } else { source_host_ = target; launch_variation_ = 0; }
        } else {
            source_host_ = host_ready;
            evaluation_ = 0;
            cadence_valid_ = false;
            launch_variation_ = 0;
        }
        // A source-frame limiter can hold an already completed frame before
        // this presenter receives it. GPU time still measures source cadence,
        // but that earlier completion cannot be used as a presentation phase:
        // every interpolated slot would already have expired at a 30 FPS cap.
        // Start the owned output sequence no earlier than the permitted flip.
        source_host_ = low_latency_ ? host_ready : std::max(source_host_, host_ready);
        last_source_ = source; last_render_ = render;
    }
    std::int64_t deadline(unsigned position, unsigned factor) const {
        factor = std::clamp(factor, 2u, 4u);
        const auto step = std::min(position, factor);
        // The midpoint is already available at the GPU-ready anchor. A small
        // compositor budget leaves room to prepare it without holding every
        // real endpoint for an additional generated-frame interval.
        if (low_latency_) {
            // Keep this preparation allowance fixed. Growing it in response to
            // a late readback/display wake feeds our own waits into measured
            // source cadence and can turn a short stall into seconds at 20 FPS.
            constexpr std::int64_t lead = 2000000;
            return source_host_ + lead + period() * (step ? step - 1 : 0) / factor;
        }
        return source_host_ + period() * step / factor;
    }
    // Apply the simulation floor to output spacing, not individual samples:
    // clamping samples first turns zero-mean completion jitter into a slower
    // source cadence and can drive the presentation phase into the future.
    std::int64_t period() const { return std::max(minimum_, period_); }
    // Select the newest temporal position due now. Never submit every expired
    // subframe in a catch-up burst when a cap or late evaluation blocks present.
    unsigned next_position(unsigned first, unsigned factor, std::int64_t now) const {
        while (first < factor && deadline(first + 1, factor) <= now) ++first;
        return first;
    }
private:
    std::uint64_t last_source_ = 0, last_render_ = 0;
    std::int64_t period_ = 16666667, minimum_ = 16666667;
    std::int64_t source_host_ = 0, evaluation_ = 0, offset_ = 0;
    std::int64_t launch_variation_ = 0;
    bool clock_valid_ = false, cadence_valid_ = false, smooth_ = true, low_latency_ = false;
};

// Budget the generated->real spacing from evaluation *start*. GPU waiting and
// generated presentation already consume this budget. Never add a whole half
// frame after a slow evaluation, or carry a synthetic 120 Hz grid into the next
// pair. The game simulation cap is unchanged.
inline std::int64_t fg_real_deadline_ns(std::int64_t evaluation_start_ns,
                                        float render_ms, int game_cap, unsigned factor = 2,
                                        unsigned generated_index = 0) {
    const double minimum_ms = game_cap > 0 ? 1000.0 / game_cap : 8.0;
    const double period_ms = std::isfinite(render_ms) ?
        std::clamp<double>(render_ms, minimum_ms, std::max(minimum_ms, 100.0)) : minimum_ms;
    factor = std::clamp(factor, 2u, 4u);
    const unsigned step = generated_index ? std::min(generated_index, factor - 1) : factor - 1;
    return evaluation_start_ns + static_cast<std::int64_t>(period_ms * 1000000.0 * step / factor);
}

} // namespace Vulkan
