#pragma once

#include <algorithm>
#include <cstdint>

namespace Vulkan::Rt {

constexpr uint32_t OmmMaxLevel = 5;
constexpr uint32_t OmmDefaultLevel = 3;
constexpr uint32_t OmmTriangleRecordBytes = 8;
constexpr uint32_t OmmIndexBytes = 4;
constexpr uint32_t OmmSpecialTransparent = 0xFFFFFFFFu;
constexpr uint32_t OmmSpecialOpaque = 0xFFFFFFFEu;
constexpr uint32_t OmmSpecialUnknownOpaque = 0xFFFFFFFCu;

enum class OmmState : uint32_t { Transparent = 0, Opaque = 1, UnknownTransparent = 2, UnknownOpaque = 3 };

constexpr uint32_t OmmMicroTriangles(uint32_t level) {
    return 1u << (2 * level);
}

constexpr uint32_t OmmDataWords(uint32_t level) {
    return std::max<uint32_t>(1, OmmMicroTriangles(level) * 2 / 32);
}

constexpr uint32_t OmmDataBytes(uint32_t level) {
    return OmmDataWords(level) * 4;
}

constexpr uint32_t OmmSamplesPerEdge(uint32_t level) {
    return 1u << (level + 2);
}

constexpr uint32_t OmmInputBytes(uint32_t triangles, uint32_t level) {
    return triangles * (OmmDataBytes(level) + OmmTriangleRecordBytes + OmmIndexBytes);
}

inline uint32_t OmmSpaceFillingIndex(float u, float v, uint32_t level) {
    u = std::clamp(u, 0.0f, 1.0f);
    v = std::clamp(v, 0.0f, 1.0f);
    const float fu = u * float(1u << level);
    const float fv = v * float(1u << level);
    uint32_t iu = uint32_t(fu);
    uint32_t iv = uint32_t(fv);
    const float uf = fu - float(iu);
    const float vf = fv - float(iv);
    if (iu >= (1u << level)) {
        iu = (1u << level) - 1u;
    }
    if (iv >= (1u << level)) {
        iv = (1u << level) - 1u;
    }
    const uint32_t iuv = iu + iv;
    if (iuv >= (1u << level)) {
        iu -= iuv - (1u << level) + 1u;
    }
    uint32_t iw = ~(iu + iv);
    if (uf + vf >= 1.0f && iuv < (1u << level) - 1u) {
        --iw;
    }
    uint32_t b0 = ~(iu ^ iw);
    b0 &= ((1u << level) - 1u);
    const uint32_t t = (iu ^ iv) & b0;
    uint32_t f = t;
    f ^= f >> 1u;
    f ^= f >> 2u;
    f ^= f >> 4u;
    f ^= f >> 8u;
    uint32_t b1 = ((f ^ iu) & ~b0) | t;
    b0 = (b0 | (b0 << 8u)) & 0x00ff00ffu;
    b0 = (b0 | (b0 << 4u)) & 0x0f0f0f0fu;
    b0 = (b0 | (b0 << 2u)) & 0x33333333u;
    b0 = (b0 | (b0 << 1u)) & 0x55555555u;
    b1 = (b1 | (b1 << 8u)) & 0x00ff00ffu;
    b1 = (b1 | (b1 << 4u)) & 0x0f0f0f0fu;
    b1 = (b1 | (b1 << 2u)) & 0x33333333u;
    b1 = (b1 | (b1 << 1u)) & 0x55555555u;
    return b0 | (b1 << 1u);
}

constexpr OmmState OmmClassify(bool any_opaque, bool any_transparent) {
    if (any_opaque && !any_transparent) {
        return OmmState::Opaque;
    }
    if (any_transparent && !any_opaque) {
        return OmmState::Transparent;
    }
    return OmmState::UnknownOpaque;
}

constexpr uint32_t OmmTriangleIndex(uint32_t triangle, bool all_opaque, bool all_transparent) {
    if (all_opaque) {
        return OmmSpecialOpaque;
    }
    if (all_transparent) {
        return OmmSpecialTransparent;
    }
    return triangle;
}

template <typename Alpha>
inline void OmmBakeTriangle(uint32_t level, float alpha_ref, Alpha&& alpha, uint32_t* words,
                            uint32_t& index, uint32_t triangle) {
    const uint32_t micro = OmmMicroTriangles(level);
    const uint32_t n = OmmSamplesPerEdge(level);
    uint64_t opaque_mask[OmmMicroTriangles(OmmMaxLevel) / 64 + 1]{};
    uint64_t transparent_mask[OmmMicroTriangles(OmmMaxLevel) / 64 + 1]{};
    for (uint32_t j = 0; j < n; ++j) {
        for (uint32_t i = 0; i + j < n; ++i) {
            const float u = (float(i) + 0.5f) / float(n);
            const float v = (float(j) + 0.5f) / float(n);
            const uint32_t k = OmmSpaceFillingIndex(u, v, level);
            if (alpha(u, v) >= alpha_ref) {
                opaque_mask[k / 64] |= 1ull << (k % 64);
            } else {
                transparent_mask[k / 64] |= 1ull << (k % 64);
            }
        }
    }
    bool all_opaque = true, all_transparent = true;
    for (uint32_t w = 0; w < OmmDataWords(level); ++w) {
        words[w] = 0;
    }
    for (uint32_t k = 0; k < micro; ++k) {
        const bool o = (opaque_mask[k / 64] >> (k % 64)) & 1;
        const bool t = (transparent_mask[k / 64] >> (k % 64)) & 1;
        const OmmState state = OmmClassify(o, t);
        all_opaque = all_opaque && state == OmmState::Opaque;
        all_transparent = all_transparent && state == OmmState::Transparent;
        words[k / 16] |= uint32_t(state) << ((k % 16) * 2);
    }
    index = OmmTriangleIndex(triangle, all_opaque, all_transparent);
}

}
