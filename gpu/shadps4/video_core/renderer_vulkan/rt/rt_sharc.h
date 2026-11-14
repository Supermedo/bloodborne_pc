#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace Vulkan::Rt {

constexpr uint32_t SharcSlotsLog2 = 20;
constexpr uint32_t SharcSlots = 1u << SharcSlotsLog2;
constexpr uint32_t SharcEntryWords = 8;
constexpr uint64_t SharcBytes = uint64_t(SharcSlots) * SharcEntryWords * 4;
constexpr uint32_t SharcProbes = 8;
constexpr float SharcBaseCell = 0.25f;
constexpr float SharcCellPerMeter = 0.02f;
constexpr uint32_t SharcMaxLevel = 15;
constexpr uint32_t SharcScale = 4096;
constexpr float SharcMaxRadiance = 16.0f;
constexpr uint32_t SharcFrameCap = 256;
constexpr float SharcMaxWeight = 32.0f;
constexpr float SharcMinWeight = 4.0f;
constexpr uint32_t SharcStaleFrames = 64;
constexpr uint32_t SharcUpdateMask = 7;
constexpr float SharcMinRayCells = 1.5f;

constexpr uint32_t SharcHash(uint32_t x) {
    x ^= x >> 16u;
    x *= 0x7feb352du;
    x ^= x >> 15u;
    x *= 0x846ca68bu;
    x ^= x >> 16u;
    return x;
}

inline uint32_t SharcLevel(float distance) {
    const float want = distance * SharcCellPerMeter / SharcBaseCell;
    if (!(want > 1.0f)) {
        return 0;
    }
    const float level = std::ceil(std::log2(want));
    return level >= float(SharcMaxLevel) ? SharcMaxLevel : uint32_t(level);
}

inline float SharcCellSize(uint32_t level) {
    return SharcBaseCell * std::exp2(float(level));
}

struct SharcKey {
    uint32_t lo = 0;
    uint32_t hi = 0;
};

inline SharcKey SharcMakeKey(const std::array<float, 3>& world, uint32_t level,
                             const std::array<float, 3>& normal) {
    const float size = SharcCellSize(level);
    std::array<uint32_t, 3> c{};
    for (int i = 0; i < 3; ++i) {
        c[i] = uint32_t(int32_t(std::floor(world[i] / size))) & 0x1FFFFu;
    }
    const uint32_t octant = (normal[0] < 0.0f ? 1u : 0u) | (normal[1] < 0.0f ? 2u : 0u) |
                            (normal[2] < 0.0f ? 4u : 0u);
    return {c[0] | (c[1] << 17u),
            (c[1] >> 15u) | (c[2] << 2u) | (level << 19u) | (octant << 23u)};
}

constexpr uint32_t SharcHashSlot(SharcKey k) {
    return SharcHash(k.lo ^ SharcHash(k.hi)) & (SharcSlots - 1u);
}

constexpr uint32_t SharcChecksum(SharcKey k) {
    const uint32_t c = SharcHash(k.hi ^ SharcHash(k.lo + 0x9E3779B9u));
    return c == 0u ? 1u : c;
}

inline uint32_t SharcEncode(float v) {
    if (std::isnan(v)) {
        return 0;
    }
    return uint32_t(std::clamp(v, 0.0f, SharcMaxRadiance) * float(SharcScale) + 0.5f);
}

constexpr bool SharcSumSafe() {
    return uint64_t(SharcFrameCap) * uint64_t(SharcMaxRadiance * float(SharcScale) + 1.0f) <
           (uint64_t(1) << 32);
}

struct SharcResolved {
    std::array<float, 3> rgb{};
    float weight = 0.0f;
};

inline SharcResolved SharcResolve(SharcResolved old, const std::array<uint32_t, 3>& sum,
                                  uint32_t count) {
    const uint32_t n = std::min(count, SharcFrameCap);
    if (n == 0) {
        return old;
    }
    const float total = old.weight + float(n);
    const float alpha = std::min(float(n) / std::min(total, SharcMaxWeight), 1.0f);
    SharcResolved out;
    for (int i = 0; i < 3; ++i) {
        const float mean = float(sum[i]) / (float(n) * float(SharcScale));
        out.rgb[i] = old.rgb[i] + (mean - old.rgb[i]) * alpha;
    }
    out.weight = std::min(total, SharcMaxWeight);
    return out;
}

constexpr bool SharcReady(float weight) {
    return weight >= SharcMinWeight;
}

constexpr bool SharcStale(uint32_t stamp, uint32_t frame) {
    return frame - stamp > SharcStaleFrames;
}

constexpr bool SharcUpdatePath(uint32_t x, uint32_t y, uint32_t frame) {
    return (SharcHash(x * 1973u ^ y * 9277u ^ (frame * 26699u + 0x5bd1e995u)) & SharcUpdateMask) ==
           0u;
}

constexpr uint32_t SharcGroups() {
    return SharcSlots / 64u;
}

}
