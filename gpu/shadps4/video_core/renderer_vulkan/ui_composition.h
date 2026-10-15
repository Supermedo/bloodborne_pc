// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace Vulkan::UiComposition {

enum class Background { None, Copy, Temporal };

// UI composition must not require scene depth/camera or an active FSR context.
constexpr Background Choose(bool scaled, bool ui_draw, bool scene_ready, bool fsr_active) {
    if (!scaled || !ui_draw) {
        return Background::None;
    }
    return scene_ready && fsr_active ? Background::Temporal : Background::Copy;
}

// The game's UI coordinate space: 1920x1080, or wider for ultrawide outputs (BB_UI_RES from
// run.py, patches.py hud_canvas: the HUD canvas stays 1080 high).
inline std::array<uint32_t, 2> Canvas() {
    static const std::array<uint32_t, 2> canvas = [] {
        uint32_t w = 0, h = 0;
        const char* res = std::getenv("BB_UI_RES");
        if (res && std::sscanf(res, "%ux%u", &w, &h) == 2 && w && h) {
            return std::array<uint32_t, 2>{w, h};
        }
        return std::array<uint32_t, 2>{1920, 1080};
    }();
    return canvas;
}

inline bool NativeViewport(float width, float height) {
    return std::abs(std::abs(width) - float(Canvas()[0])) < 0.5f &&
           std::abs(std::abs(height) - float(Canvas()[1])) < 0.5f;
}

// Scaleform draws, including the first stencil/movie pass. A native-size viewport
// alone also matches every fullscreen post pass when guest targets stay at 1080p.
constexpr bool MovieShader(uint64_t hash) {
    return hash == 0x34e8a281 || hash == 0x81d336ce || hash == 0x09957251 ||
           hash == 0x24042a9b || hash == 0xa400228b ||
           // The title menu while it animates (its highlight): without these its frames were
           // shown at the game's 1080p between output-size ones, a flicker.
           hash == 0xbb4d5f8c || hash == 0xe94b5b06 || hash == 0x9951d199 ||
           hash == 0x44072322 || hash == 0xa334a845;
}

inline std::array<float, 2> Scale(uint32_t guest_width, uint32_t guest_height,
                                uint32_t output_width, uint32_t output_height,
                                bool native_coordinates) {
    return {float(output_width) / float(native_coordinates ? Canvas()[0] : guest_width),
            float(output_height) / float(native_coordinates ? Canvas()[1] : guest_height)};
}

} // namespace Vulkan::UiComposition
