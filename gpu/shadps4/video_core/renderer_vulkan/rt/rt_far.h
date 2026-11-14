#pragma once

#include <algorithm>
#include <array>

namespace Vulkan::Rt {

constexpr float FarFadeMaxM = 10000.0f;
constexpr float FarBandDefaultM = 10.0f;
constexpr float FarHardStepInvBand = 1.0e6f;

inline std::array<float, 4> FarFadeParams(float start_m, float band_m) {
    if (!(start_m > 0.0f)) {
        return {0.0f, 0.0f, 0.0f, 0.0f};
    }
    const float band = band_m > 0.0f ? band_m : 0.0f;
    const float inv_band = band > 0.0f ? 1.0f / band : FarHardStepInvBand;
    return {start_m, start_m + band, inv_band, 1.0f};
}

inline bool FarFadeOn(const std::array<float, 4>& f) {
    return f[3] > 0.0f;
}

inline float FarWeight(const std::array<float, 4>& f, float distance) {
    if (!FarFadeOn(f)) {
        return 1.0f;
    }
    return std::clamp((f[1] - distance) * f[2], 0.0f, 1.0f);
}

inline bool FarSkipped(const std::array<float, 4>& f, float distance) {
    return FarWeight(f, distance) <= 0.0f;
}

}
