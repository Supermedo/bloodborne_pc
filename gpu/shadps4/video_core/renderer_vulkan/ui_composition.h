// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <utility>

namespace Vulkan::UiComposition {

enum class Background { None, Copy, Temporal };

// UI composition must not require scene depth/camera or an active FSR context.
constexpr Background Choose(bool scaled, bool ui_draw, bool scene_ready, bool fsr_active) {
    if (!scaled || !ui_draw) {
        return Background::None;
    }
    return scene_ready && fsr_active ? Background::Temporal : Background::Copy;
}

// The movie fits its 1920x1080 logical stage into this physical viewport.
inline std::pair<uint32_t, uint32_t> MovieViewport() {
    static const auto size = [] {
        uint32_t w = 1920, h = 1080, x = 0, y = 0;
        if (const char* res = std::getenv("BB_UI_RES")) {
            if (std::sscanf(res, "%ux%u", &x, &y) == 2 && x && y) { w = x; h = y; }
        }
        return std::pair{w, h};
    }();
    return size;
}
inline bool NativeViewport(float width, float height) {
    const auto [w, h] = MovieViewport();
    return std::abs(std::abs(width) - float(w)) < 0.5f &&
           std::abs(std::abs(height) - float(h)) < 0.5f;
}

// Scaleform draws, including the first stencil/movie pass. A native-size viewport
// alone also matches every fullscreen post pass when guest targets stay at 1080p.
constexpr bool MovieShader(uint64_t hash) {
    return hash == 0x34e8a281 || hash == 0x81d336ce || hash == 0x09957251 ||
           hash == 0x24042a9b || hash == 0xa400228b;
}

inline std::array<float, 2> Scale(uint32_t guest_width, uint32_t guest_height,
                                uint32_t output_width, uint32_t output_height,
                                bool native_coordinates) {
    const auto [ui_width, ui_height] = MovieViewport();
    return {float(output_width) / float(native_coordinates ? ui_width : guest_width),
            float(output_height) / float(native_coordinates ? ui_height : guest_height)};
}

} // namespace Vulkan::UiComposition
