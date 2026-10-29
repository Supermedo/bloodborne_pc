// SPDX-License-Identifier: GPL-2.0-or-later
// Sharp UI (patches.py ui_viewport, UiComposition::DrawScale): at a 2560x1440 output the game's UI
// viewport is 2560x1440 and each draw of a UI pass is scaled by what its own viewport holds.
// ninja -C out/gpu ui-composition-sharp-test, then run it. (test_ui_composition.cpp: without it.)
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include "gpu/shadps4/video_core/renderer_vulkan/ui_composition.h"

static bool Near(std::array<float, 2> scale, float x, float y) {
    return std::abs(scale[0] - x) < 1e-3f && std::abs(scale[1] - y) < 1e-3f;
}

int main() {
    // run.py passes the UI viewport as BB_UI_RES before the game starts (Canvas() reads it once).
#ifdef _WIN32
    _putenv_s("BB_UI_RES", "2560x1440");
#else
    setenv("BB_UI_RES", "2560x1440", 1);
#endif
    using namespace Vulkan::UiComposition;
    assert(Canvas()[0] == 2560 && Canvas()[1] == 1440);
    assert(SharpUi() && std::abs(StageScale() - 2560.0f / 1920.0f) < 1e-4f);
    // Both the UI viewport and the stage's own 1920x1080 are a UI movie's viewport.
    assert(NativeViewport(2560, -1440) && NativeViewport(1920, -1080));
    assert(!NativeViewport(1706, -960) && !NativeViewport(128, -128));

    // A UI pass into the 2560x1440 output, its guest target at the render size (Quality: 1706x960).
    constexpr uint32_t gw = 1706, gh = 960, ow = 2560, oh = 1440;
    // The movie viewport is in the output's pixels already.
    assert(Near(DrawScale(2560, -1440, false, true, gw, gh, ow, oh), 1, 1));
    // The stage's 1920x1080 viewport: the stage scaled up to the output.
    assert(Near(DrawScale(1920, -1080, false, true, gw, gh, ow, oh), 2560.0f / 1920, 1440.0f / 1080));
    // An item icon's rectangle is in the UI viewport's pixels (not stretched by output / render size).
    assert(Near(DrawScale(128, -128, false, true, gw, gh, ow, oh), 1, 1));
    // Window-space vertices are in the UI viewport's pixels too.
    assert(Near(DrawScale(0, 0, true, true, gw, gh, ow, oh), 1, 1));
    // The viewport over the whole guest target (a few pixels larger than the scene) is in its pixels.
    assert(Near(DrawScale(1708, -960, false, true, gw, gh, ow, oh), 2560.0f / gw, 1440.0f / gh));
    // The display pass (no native UI): the guest target's pixels, whatever the viewport.
    assert(Near(DrawScale(2560, -1440, false, false, gw, gh, ow, oh), 2560.0f / gw, 1440.0f / gh));
    assert(Near(DrawScale(128, -128, false, false, gw, gh, ow, oh), 2560.0f / gw, 1440.0f / gh));

    // A 16:10 output (2560x1600): the UI viewport stays 2560x1440 (ui_viewport), stretched to the
    // output's height as the 1080 canvas was before.
    assert(Near(DrawScale(2560, -1440, false, true, gw, gh, 2560, 1600), 1, 1600.0f / 1440));
    std::puts("UI composition with Sharp UI: PASS (movie, stage, icon, window space, target, display pass)");
}
