// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
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

inline bool ViewportIs(float width, float height, float target_width, float target_height) {
    return std::abs(std::abs(width) - target_width) < 0.5f &&
           std::abs(std::abs(height) - target_height) < 0.5f;
}

// Sharp UI (bbport.ini sharp_ui, patches.py ui_viewport; from Awaken's Lantern Engine): the UI
// viewport has the output's size, larger than the 1920x1080 stage both ways, and Scaleform draws
// the stage scaled up to it - text and shapes for the output's pixels instead of 1080 lines
// stretched afterwards.
inline float StageScale() {
    return std::max(1.0f, std::min(float(Canvas()[0]) / 1920.0f, float(Canvas()[1]) / 1080.0f));
}
inline bool SharpUi() {
    return StageScale() > 1.0f;
}

// A UI movie's viewport: the UI viewport, and with Sharp UI also the stage's own 1920x1080.
inline bool NativeViewport(float width, float height) {
    return ViewportIs(width, height, float(Canvas()[0]), float(Canvas()[1])) ||
           (SharpUi() && ViewportIs(width, height, 1920.0f, 1080.0f));
}

// Scaleform draws, including the first stencil/movie pass. A native-size viewport
// alone also matches every fullscreen post pass when guest targets stay at 1080p.
constexpr bool MovieShader(uint64_t hash) {
    return hash == 0x34e8a281 || hash == 0x81d336ce || hash == 0x09957251 ||
           hash == 0x24042a9b || hash == 0xa400228b ||
           // The title menu while its highlight animates: without these its frames alternated
           // between 1080p and output size above 1080p (flicker). From parsifal295 (PR #56).
           hash == 0xbb4d5f8c || hash == 0xe94b5b06 || hash == 0x9951d199 ||
           hash == 0x44072322 || hash == 0xa334a845;
}

inline std::array<float, 2> Scale(uint32_t guest_width, uint32_t guest_height,
                                uint32_t output_width, uint32_t output_height,
                                bool native_coordinates) {
    return {float(output_width) / float(native_coordinates ? Canvas()[0] : guest_width),
            float(output_height) / float(native_coordinates ? Canvas()[1] : guest_height)};
}

// The scale of one draw into a UI pass's output-size image (Rasterizer::UpdateUiTransform), from its
// viewport (width x height, as the registers give it), whether its vertices are in window space,
// whether the pass draws the native UI, the guest target's size and the output's.
//
// Without Sharp UI: the UI viewport (or window space) is in UI coordinates, anything else in the
// guest target's pixels - as the port always did. With Sharp UI a pass mixes viewports (the game's
// UI pass sets one per picture): the stage's own 1920x1080 viewport is the stage scaled up; a
// viewport of its own inside the UI viewport (an item icon's rectangle) and window-space vertices
// are in the UI viewport's pixels; only the viewport over the whole guest target is in its pixels.
// (Scaled by the output over the render size, the icons came out up to three times their size,
// spread from the top left: Awaken's finding.)
inline std::array<float, 2> DrawScale(float width, float height, bool window_space, bool native_ui,
                                      uint32_t guest_width, uint32_t guest_height,
                                      uint32_t output_width, uint32_t output_height) {
    if (!SharpUi()) {
        return Scale(guest_width, guest_height, output_width, output_height,
                     native_ui && (NativeViewport(width, height) || window_space));
    }
    const float w = std::abs(width), h = std::abs(height);
    const bool target = std::abs(w - float(guest_width)) < 8.0f && std::abs(h - float(guest_height)) < 8.0f;
    const bool stage = !window_space && ViewportIs(w, h, 1920.0f, 1080.0f);
    const bool inner = !window_space && !target && w >= 1.0f && h >= 1.0f &&
                       w <= float(Canvas()[0]) + 0.5f && h <= float(Canvas()[1]) + 0.5f;
    if (!native_ui || !(stage || inner || window_space ||
                        ViewportIs(w, h, float(Canvas()[0]), float(Canvas()[1])))) {
        return Scale(guest_width, guest_height, output_width, output_height, false);
    }
    const float ui_w = stage ? 1920.0f : float(Canvas()[0]), ui_h = stage ? 1080.0f : float(Canvas()[1]);
    return {float(output_width) / ui_w, float(output_height) / ui_h};
}

} // namespace Vulkan::UiComposition
