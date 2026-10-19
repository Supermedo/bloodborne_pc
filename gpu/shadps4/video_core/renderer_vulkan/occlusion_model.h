// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: the PS4 depth-block counter model behind real occlusion results (vk_occlusion.h),
// without Vulkan, so the arithmetic is unit-tested (tests/test_occlusion.cpp).
#pragma once

#include <algorithm>
#include <cstdint>

namespace OcclusionModel {

constexpr std::uint64_t ValidBit = 1ull << 63;
/// What a draw without a usable result adds: plainly visible (the old fake's step), so any
/// begin/end pair around it reads visible, never occluded.
constexpr std::uint64_t Unmeasured = 0x2FFFFFFull;
constexpr std::uint32_t MaxPipes = 16;

/// DB `k`'s share of the running total: the shares sum to `total` and each grows with it, so the
/// game's sum of end - begin over the DBs is exactly the samples in between.
constexpr std::uint64_t Share(std::uint64_t total, std::uint32_t pipes, std::uint32_t k) {
    return total / pipes + (k < total % pipes ? 1 : 0);
}

enum class Block : std::uint32_t { Occluded = 0, Visible = 1, NotReady = 2 };

/// A Gnm block ({begin, end} per DB, 16 bytes apart) as the game reads it.
inline Block ReadBlock(const std::uint64_t* words, std::uint32_t pipes) {
    std::uint64_t sum = 0;
    for (std::uint32_t k = 0; k < pipes; ++k) {
        const std::uint64_t b = words[2 * k], e = words[2 * k + 1];
        if (!(b & ValidBit) || !(e & ValidBit)) {
            return Block::NotReady;
        }
        sum += (e & ~ValidBit) - (b & ~ValidBit);
    }
    return sum != 0 ? Block::Visible : Block::Occluded;
}

/// 16.16 factor taking samples counted at `width` x `height` (reduced scene targets) back to the
/// PS4's 1920x1080; never below 1.
constexpr std::uint32_t SampleScale(std::uint32_t width, std::uint32_t height) {
    const std::uint64_t pixels = std::uint64_t(width) * height;
    constexpr std::uint64_t native = 1920ull * 1080ull;
    if (pixels == 0 || pixels >= native) {
        return 1u << 16;
    }
    return static_cast<std::uint32_t>((native << 16) / pixels);
}

/// One query's contribution: unavailable reads visible; an imprecise device's nonzero result
/// (0/1 on NVIDIA) reads plainly visible; a precise count is scaled, rounding up.
constexpr std::uint64_t Samples(std::uint64_t value, bool available, bool precise,
                                std::uint32_t scale) {
    if (!available) {
        return Unmeasured;
    }
    if (!precise) {
        return value ? Unmeasured : 0;
    }
    return std::max<std::uint64_t>((value * scale + 0xFFFF) >> 16, value);
}

/// SET_PREDICATION's outcome for the draws up to its clear: unknown draws (a wrong "occluded"
/// makes geometry vanish), else draw when the visibility is the one asked for.
constexpr bool PredicatedDraws(bool visible, bool unknown, bool draw_if_visible) {
    return unknown || visible == draw_if_visible;
}

/// Hysteresis per Gnm block against flicker (lamps and their glare at the edge of occluders:
/// sample counts of small proxies going 0/N/0 with the jittered, reduced scene): a block
/// reports the largest of its last `Depth` measured counts. It reads occluded only after Depth
/// results in a row found no samples, visible as soon as one finds any. Results it has not seen
/// count as `UnknownPast`: visible, with the smallest count (a count-scaled effect stays dark
/// rather than flashing).
struct BlockHistory {
    static constexpr std::uint32_t Depth = 3;
    static constexpr std::uint64_t UnknownPast = 1;
    std::uint64_t previous[Depth - 1] = {UnknownPast, UnknownPast};

    /// The count to report for the block's new result `measured`, which is remembered.
    constexpr std::uint64_t Report(std::uint64_t measured) {
        std::uint64_t reported = measured;
        for (const std::uint64_t p : previous) {
            reported = std::max(reported, p);
        }
        for (std::uint32_t i = Depth - 2; i > 0; --i) {
            previous[i] = previous[i - 1];
        }
        previous[0] = measured;
        return reported;
    }
};

} // namespace OcclusionModel
