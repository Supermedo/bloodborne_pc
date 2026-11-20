#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

// The sky image (rt_gi.h SkyImageSlot) learned from the screen, kept per sky model (rt_moon_cache.h
// SkyKey) on disk, so the clouds reach GI and mirrors in directions not yet on screen this session.
// A cell is one RGB9E5 word, 0 when the sky was never seen there; runs of unseen cells are stored
// as counts (about half the octahedral image is below the horizon and never captured).
namespace Vulkan::Rt {

// Shared-exponent RGB (9-bit mantissas, 5-bit exponent, bias 15), as VK_FORMAT_E5B9G9R9.
inline uint32_t PackRgb9e5(float r, float g, float b) {
    static constexpr float MaxValue = 511.0f / 512.0f * 65536.0f;
    const auto clampc = [](float c) {
        return std::isfinite(c) ? std::clamp(c, 0.0f, MaxValue) : 0.0f;
    };
    r = clampc(r);
    g = clampc(g);
    b = clampc(b);
    const float maxc = std::max({r, g, b});
    if (!(maxc > 0.0f)) {
        return 0;
    }
    int exp = std::max(-16, int(std::floor(std::log2(maxc)))) + 16;
    float denom = std::ldexp(1.0f, exp - 15 - 9);
    if (std::floor(maxc / denom + 0.5f) >= 512.0f) {
        denom *= 2.0f;
        ++exp;
    }
    const auto mant = [&](float c) { return std::min(uint32_t(std::floor(c / denom + 0.5f)), 511u); };
    return mant(r) | mant(g) << 9 | mant(b) << 18 | uint32_t(exp) << 27;
}

inline std::array<float, 3> UnpackRgb9e5(uint32_t v) {
    const float scale = std::ldexp(1.0f, int(v >> 27) - 15 - 9);
    return {float(v & 511u) * scale, float((v >> 9) & 511u) * scale,
            float((v >> 18) & 511u) * scale};
}

// Sky image cells (vec4: rgb, w > 0 where seen) to words; a seen black cell stays non-zero.
inline std::vector<uint32_t> PackSkyCells(const float* cells, size_t count, size_t* seen) {
    std::vector<uint32_t> words(count, 0u);
    size_t n = 0;
    for (size_t i = 0; i < count; ++i) {
        const float* c = cells + i * 4;
        if (!(c[3] > 0.0f)) {
            continue;
        }
        const uint32_t w = PackRgb9e5(c[0], c[1], c[2]);
        words[i] = w ? w : 1u;
        ++n;
    }
    if (seen) {
        *seen = n;
    }
    return words;
}

inline size_t UnpackSkyCells(const std::vector<uint32_t>& words, float* cells) {
    size_t n = 0;
    for (size_t i = 0; i < words.size(); ++i) {
        float* c = cells + i * 4;
        if (!words[i]) {
            c[0] = c[1] = c[2] = c[3] = 0.0f;
            continue;
        }
        const auto rgb = UnpackRgb9e5(words[i]);
        c[0] = rgb[0];
        c[1] = rgb[1];
        c[2] = rgb[2];
        c[3] = 1.0f;
        ++n;
    }
    return n;
}

constexpr uint32_t SkyCacheMagic = 0x4b534242u; // "BBSK"
constexpr uint32_t SkyCacheVersion = 1;

// Header (magic, version, cell count), then (unseen run, seen run, seen words...) to the end.
inline std::vector<uint32_t> EncodeSkyWords(const std::vector<uint32_t>& words) {
    std::vector<uint32_t> out{SkyCacheMagic, SkyCacheVersion, uint32_t(words.size())};
    size_t i = 0;
    while (i < words.size()) {
        const size_t zero_start = i;
        while (i < words.size() && words[i] == 0u) {
            ++i;
        }
        const size_t seen_start = i;
        while (i < words.size() && words[i] != 0u) {
            ++i;
        }
        out.push_back(uint32_t(seen_start - zero_start));
        out.push_back(uint32_t(i - seen_start));
        out.insert(out.end(), words.begin() + seen_start, words.begin() + i);
    }
    return out;
}

// False on a bad header, a different cell count, or runs that do not add up.
inline bool DecodeSkyWords(const std::vector<uint32_t>& in, size_t count,
                           std::vector<uint32_t>& words) {
    if (in.size() < 3 || in[0] != SkyCacheMagic || in[1] != SkyCacheVersion || in[2] != count) {
        return false;
    }
    words.assign(count, 0u);
    size_t at = 3, cell = 0;
    while (at < in.size()) {
        if (at + 2 > in.size()) {
            return false;
        }
        const size_t zeros = in[at], seen = in[at + 1];
        at += 2;
        if (cell + zeros + seen > count || at + seen > in.size()) {
            return false;
        }
        cell += zeros;
        std::copy(in.begin() + at, in.begin() + at + seen, words.begin() + cell);
        cell += seen;
        at += seen;
    }
    return cell == count;
}

inline bool SaveSkyFile(const char* path, const std::vector<uint32_t>& encoded) {
    std::FILE* f = std::fopen(path, "wb");
    if (!f) {
        return false;
    }
    const bool ok =
        std::fwrite(encoded.data(), 4, encoded.size(), f) == encoded.size();
    return std::fclose(f) == 0 && ok;
}

inline bool LoadSkyFile(const char* path, std::vector<uint32_t>& encoded) {
    std::FILE* f = std::fopen(path, "rb");
    if (!f) {
        return false;
    }
    encoded.clear();
    uint32_t buf[4096];
    size_t n = 0;
    while ((n = std::fread(buf, 4, 4096, f)) > 0) {
        encoded.insert(encoded.end(), buf, buf + n);
    }
    std::fclose(f);
    return true;
}

} // namespace Vulkan::Rt
