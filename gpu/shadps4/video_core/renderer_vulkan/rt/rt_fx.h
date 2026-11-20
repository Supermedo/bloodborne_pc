#pragma once

#include <array>
#include <cstdint>

namespace Vulkan::Rt {

constexpr uint32_t FxSmokeMaxIndices = 12288;
constexpr uint32_t FxSmokeMaxParticles = 256;

constexpr uint32_t FxBlendSrcAlpha = 4;
constexpr uint32_t FxBlendOneMinusSrcAlpha = 5;
constexpr uint32_t FxBlendAdd = 0;
constexpr uint32_t FxFormat16_16_16_16 = 12;
constexpr uint32_t FxPrimTriangleList = 4;
constexpr uint32_t FxPrimTriangleStrip = 6;
constexpr uint32_t FxPrimPatch = 9;
constexpr uint32_t FxStagesVs = 0;
constexpr uint32_t FxStagesLsHs = 0x45;

struct FxDrawState {
    bool blend_enable{};
    bool blend_bypass{};
    uint32_t src_factor{};
    uint32_t dst_factor{};
    uint32_t blend_func{};
    bool depth_test{};
    bool depth_write{};
    uint32_t color_format{};
    uint32_t primitive{};
    uint32_t indices{};
    uint32_t stages{};
};

inline bool FxSmokeTopology(uint32_t primitive) {
    return primitive == FxPrimTriangleList || primitive == FxPrimTriangleStrip;
}

inline bool FxSmokeTargets(uint32_t bound_mask, uint32_t export_mask) {
    return (bound_mask & export_mask) == 1u;
}

inline bool FxSmokeBlend(const FxDrawState& s) {
    return s.blend_enable && !s.blend_bypass && s.blend_func == FxBlendAdd &&
           s.src_factor == FxBlendSrcAlpha && s.dst_factor == FxBlendOneMinusSrcAlpha &&
           s.depth_test && !s.depth_write && s.color_format == FxFormat16_16_16_16;
}

inline bool FxSmokeRule(const FxDrawState& s) {
    return FxSmokeBlend(s) && s.stages == FxStagesVs && FxSmokeTopology(s.primitive) &&
           s.indices >= 3 && s.indices <= FxSmokeMaxIndices;
}

inline bool FxSmokePatchRule(const FxDrawState& s) {
    return FxSmokeBlend(s) && s.stages == FxStagesLsHs && s.primitive == FxPrimPatch &&
           s.indices >= 1;
}

struct FxParticle {
    std::array<float, 3> centre{};
    float size{};
    float alpha{1.0f};
    u32 rgb{0xFFFFFFu};
};

constexpr uint32_t FxSmokeMaxDraws = 4;
constexpr uint32_t FxSmokeQuadVertices = 6;
constexpr uint32_t FxSmokeVertices = 1 + FxSmokeQuadVertices * FxSmokeMaxParticles;
constexpr uint32_t FxSmokeMaxTriangles = 2 * FxSmokeMaxParticles;
constexpr uint32_t FxSmokeReserveTriangles = FxSmokeMaxTriangles + 64;
constexpr uint32_t FxSmokePositionsOffset = 256;
constexpr uint32_t FxSmokeUvsOffset =
    FxSmokePositionsOffset + ((FxSmokeVertices * 16 + 255) & ~255u);
constexpr uint32_t FxSmokeSlotBytes = FxSmokeUvsOffset + ((FxSmokeVertices * 4 + 255) & ~255u);
constexpr uint64_t FxSmokeGeometryKey = 0x534D4F4B45ull;

constexpr uint32_t FxSmokeTriangleBase(uint32_t capacity_triangles) {
    return capacity_triangles > FxSmokeReserveTriangles
               ? (capacity_triangles - FxSmokeMaxTriangles) & ~63u
               : 0u;
}

constexpr std::array<float, 4> FxClipFromView(const std::array<float, 3>& view, float xs,
                                              float ys) {
    return {view[0] * xs, view[1] * ys, 0.0f, view[2]};
}

constexpr std::array<float, 2> FxQuadUv(uint32_t vertex) {
    constexpr std::array<uint32_t, 6> corner{0u, 1u, 2u, 0u, 2u, 3u};
    const uint32_t k = corner[vertex % 6u];
    return {(k == 1 || k == 2) ? 1.0f : 0.0f, (k >= 2) ? 0.0f : 1.0f};
}

constexpr bool FxFinite(float v) {
    return v == v && v > -1e30f && v < 1e30f;
}

inline float FxParticleSize(const float* attr, uint32_t components, uint32_t component,
                            float scale, float fixed, float fallback) {
    if (fixed > 0.0f) {
        return fixed;
    }
    if (attr && component < components) {
        const float v = attr[component] * scale;
        if (FxFinite(v) && v > 0.001f && v < 100.0f) {
            return v;
        }
    }
    return fallback;
}

template <typename Out>
uint32_t FxBuildQuads(const FxParticle* particles, uint32_t count,
                      const std::array<float, 3>& right, const std::array<float, 3>& up,
                      Out& out) {
    const uint32_t used = count < FxSmokeMaxParticles ? count : FxSmokeMaxParticles;
    for (uint32_t i = 0; i < used; ++i) {
        const auto& p = particles[i];
        const float h = p.size * 0.5f;
        std::array<std::array<float, 3>, 4> c{};
        for (uint32_t k = 0; k < 4; ++k) {
            const float sx = (k == 1 || k == 2) ? h : -h;
            const float sy = (k >= 2) ? h : -h;
            for (uint32_t a = 0; a < 3; ++a) {
                c[k][a] = p.centre[a] + right[a] * sx + up[a] * sy;
            }
        }
        for (const uint32_t k : {0u, 1u, 2u, 0u, 2u, 3u}) {
            out.push_back(c[k]);
        }
    }
    return used;
}

template <typename At, typename Out>
void ExpandStrip(At at, uint32_t count, bool restart, uint32_t marker, Out& out) {
    uint32_t run = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t v = at(i);
        if (restart && v == marker) {
            run = 0;
            continue;
        }
        if (++run >= 3) {
            const uint32_t a = at(i - 2), b = at(i - 1);
            const bool odd = (run & 1u) == 0u;
            out.push_back(odd ? b : a);
            out.push_back(odd ? a : b);
            out.push_back(v);
        }
    }
}

}
