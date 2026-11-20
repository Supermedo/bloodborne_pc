#pragma once

namespace Vulkan::Rt {

struct HitchSample {
    double cpu_ms = 0.0;
    double wait_ms = 0.0;
    double gpu_ms = 0.0;
};

inline bool IsHitch(const HitchSample& s, double threshold_ms) {
    return s.cpu_ms > threshold_ms || s.wait_ms > 2.0;
}

}
