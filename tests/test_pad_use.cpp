// SPDX-License-Identifier: GPL-2.0-or-later
// Which mouse input is the controller's (gpu/shim/bbport_pad_use.h): while a controller is in use, a
// mapping program (Steam's desktop layout, reWASD...) turns its stick into mouse movement and its
// buttons into clicks; the window keeps that from the cursor, the button prompts and the camera.
// ninja -C out/gpu pad-use-test, then run out/gpu/pad-use-test.
#include <cassert>
#include <cstdio>
#include "bbport_pad_use.h"

using BbPadUse::OffRest;
using BbPadUse::Pads;
using BbPadUse::Turning;

int main() {
    // SDL_GamepadAxis: 0 left x, 1 left y, 2 right x, 3 right y, 4 left trigger, 5 right trigger.
    constexpr uint32_t Pad1 = 7, Pad2 = 12;
    Pads pads;
    assert(!pads.InUse() && !pads.SticksPushed(OffRest));  // nothing connected

    // A stick at rest, and one drifting a little: a real mouse still counts.
    pads.Axis(Pad1, 2, 900);
    pads.Axis(Pad1, 3, -3500);
    assert(!pads.InUse());
    // A drift past an eighth hides the cursor's comeback, but the camera keeps the mouse.
    pads.Axis(Pad1, 3, -9000);
    assert(pads.InUse() && !pads.SticksPushed(Turning));

    // The right stick pushed to turn the camera: the mapping program's mouse movement is the stick's.
    pads.Axis(Pad1, 2, 30000);
    assert(pads.InUse() && pads.SticksPushed(Turning));
    pads.Axis(Pad1, 2, 0);
    pads.Axis(Pad1, 3, 0);
    assert(!pads.InUse() && !pads.SticksPushed(OffRest));

    // A trigger counts for clicks (a program maps RT to the left button), not for the camera.
    pads.Axis(Pad1, 5, 32767);
    assert(pads.InUse() && !pads.SticksPushed(OffRest));
    pads.Axis(Pad1, 5, 0);

    // A held button, released.
    pads.Button(Pad1, 10, true);
    assert(pads.InUse());
    pads.Button(Pad1, 10, false);
    assert(!pads.InUse());

    // Two controllers: either one in use counts; a removed one no longer does.
    pads.Axis(Pad2, 0, -20000);
    assert(pads.InUse() && pads.SticksPushed(Turning));
    pads.Removed(Pad2);
    assert(!pads.InUse());

    // A fifth controller is not followed, and nothing breaks.
    for (uint32_t id = 20; id < 25; ++id) {
        pads.Axis(id, 1, 100);
    }
    pads.Axis(99, 0, 32000);
    assert(!pads.SticksPushed(Turning));

    // Out-of-range axes and buttons from a future SDL are ignored.
    pads.Axis(Pad1, 6, 32000);
    pads.Axis(Pad1, -1, 32000);
    assert(!pads.SticksPushed(OffRest));

    std::puts("pad use: ok");
    return 0;
}
