// SPDX-License-Identifier: GPL-2.0-or-later
#include <cassert>
#include <cstdio>
#include "gpu/shadps4/video_core/renderer_vulkan/ui_composition.h"

int main() {
    using namespace Vulkan::UiComposition;
    // Title/loading menus have no depth/camera. UI must still go to the native target.
    assert(Choose(true, true, false, true) == Background::Copy);
    assert(Choose(true, true, false, false) == Background::Copy);
    // In-game HUD: first reconstruct the scene, then draw the native UI over it.
    assert(Choose(true, true, true, true) == Background::Temporal);
    // Runtime FSR disable/failure must not change UI resolution.
    assert(Choose(true, true, true, false) == Background::Copy);
    assert(Choose(true, false, true, true) == Background::None);
    assert(Choose(false, true, true, true) == Background::None);
    assert(NativeViewport(1920, -1080));
    assert(!NativeViewport(960, -540));
    assert(MovieShader(0x34e8a281) && MovieShader(0x24042a9b));
    assert(!MovieShader(0x0b0acf50)); // fullscreen post/tonemap also has a native viewport
    assert(!MovieShader(0x6c62a79f)); // shadow geometry
    const auto display = Scale(960, 540, 1920, 1080, false);
    assert(display[0] == 2 && display[1] == 2);
    const auto ui = Scale(960, 540, 1920, 1080, true);
    assert(ui[0] == 1 && ui[1] == 1); // don't double an already native UI viewport
    const auto larger = Scale(960, 540, 3840, 2160, true);
    assert(larger[0] == 2 && larger[1] == 2);
    // Without Sharp UI (no BB_UI_RES) each draw's scale is the one the port always used.
    assert(!SharpUi() && StageScale() == 1.0f);
    const auto movie = DrawScale(1920, -1080, false, true, 960, 540, 3840, 2160);
    assert(movie[0] == 2 && movie[1] == 2);
    const auto window = DrawScale(0, 0, true, true, 960, 540, 3840, 2160);
    assert(window[0] == 2 && window[1] == 2);
    const auto icon = DrawScale(128, -128, false, true, 960, 540, 3840, 2160);
    assert(icon[0] == 4 && icon[1] == 4); // in the guest target's pixels, as before
    const auto shown = DrawScale(1920, -1080, false, false, 960, 540, 3840, 2160);
    assert(shown[0] == 4 && shown[1] == 4);
    std::puts("UI composition: PASS (menu without camera, HUD, FSR off, viewport scaling)");
}
