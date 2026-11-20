#pragma once

#include <algorithm>
#include <cstdint>

namespace Vulkan::Rt {

constexpr uint32_t AdaptiveNoCap = 0xFFFFFFFFu;
constexpr uint32_t AdaptiveHoldFrames = 120;
constexpr double AdaptiveUpMarginMs = 2.5;
constexpr double AdaptiveEmaWeight = 0.05;
constexpr double AdaptiveBudgetMaxMs = 100.0;

struct AdaptiveBounces {
    uint32_t cap = AdaptiveNoCap;
    uint32_t hold = 0;
    double ema_ms = 0.0;
};

inline uint32_t AdaptiveStep(AdaptiveBounces& s, double frame_ms, double budget_ms,
                             uint32_t wanted) {
    if (budget_ms <= 0.0) {
        s = {};
        return wanted;
    }
    if (frame_ms > 0.0) {
        s.ema_ms = s.ema_ms <= 0.0 ? frame_ms : s.ema_ms + AdaptiveEmaWeight * (frame_ms - s.ema_ms);
    }
    const uint32_t used = std::min(wanted, s.cap);
    if (s.hold > 0) {
        --s.hold;
        return used;
    }
    if (s.ema_ms > budget_ms && used > 1) {
        s.cap = used - 1;
        s.hold = AdaptiveHoldFrames;
        return s.cap;
    }
    if (s.cap != AdaptiveNoCap && s.ema_ms < budget_ms - AdaptiveUpMarginMs) {
        s.cap = s.cap + 1 >= wanted ? AdaptiveNoCap : s.cap + 1;
        s.hold = AdaptiveHoldFrames;
    }
    return std::min(wanted, s.cap);
}

}
