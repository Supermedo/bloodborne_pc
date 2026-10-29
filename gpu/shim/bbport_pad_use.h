// bbport: whether a controller is in use right now, from its SDL events (window thread). Mouse input
// that comes while one is comes from the controller through a mapping program (Steam's desktop
// layout, reWASD, JoyToKey...): the window keeps it from the cursor, the button prompts and the
// camera. Plain data, so tests/test_pad_use.cpp checks it without SDL.
#pragma once
#include <cstdint>
#include <cstdlib>

namespace BbPadUse {

// A stick or trigger past about an eighth of its way: below the dead zone of mapping programs
// (they move the mouse only beyond theirs) and above a worn stick's drift at rest. Enough for the
// cursor and the button prompts.
constexpr int OffRest = 4000;
// For the camera a stick must be pushed past about a third: a keyboard and mouse player with a
// controller lying beside them, its stick drifting, keeps the mouse camera. A mapping program's
// fast camera turns (the doubled ones players notice) come from sticks pushed far.
constexpr int Turning = 12000;

struct Pads {
    struct Pad {
        uint32_t id = 0;      // SDL joystick id; 0: a free slot
        int16_t axes[6] = {}; // SDL_GamepadAxis order: the sticks, then the triggers
        uint32_t buttons = 0; // held, a bit per SDL_GamepadButton
    };
    Pad pads[4]{}; // a fifth controller is not followed

    void Button(uint32_t id, int button, bool down) {
        if (Pad* pad = Slot(id)) {
            const uint32_t bit = 1u << (button & 31);
            pad->buttons = down ? pad->buttons | bit : pad->buttons & ~bit;
        }
    }
    void Axis(uint32_t id, int axis, int16_t value) {
        if (Pad* pad = Slot(id); pad && axis >= 0 && axis < 6) {
            pad->axes[axis] = value;
        }
    }
    void Removed(uint32_t id) {
        for (Pad& pad : pads) {
            if (pad.id == id) {
                pad = Pad{};
            }
        }
    }
    // A stick past `off_rest`: what moves the mouse through a mapping program.
    bool SticksPushed(int off_rest) const {
        for (const Pad& pad : pads) {
            for (int axis = 0; pad.id != 0 && axis < 4; ++axis) {
                if (std::abs(int(pad.axes[axis])) > off_rest) {
                    return true;
                }
            }
        }
        return false;
    }
    // A button held, or a stick or trigger off its rest.
    bool InUse() const {
        for (const Pad& pad : pads) {
            if (pad.id == 0) {
                continue;
            }
            if (pad.buttons) {
                return true;
            }
            for (const int16_t axis : pad.axes) {
                if (std::abs(int(axis)) > OffRest) {
                    return true;
                }
            }
        }
        return false;
    }

private:
    Pad* Slot(uint32_t id) {
        Pad* free = nullptr;
        for (Pad& pad : pads) {
            if (pad.id == id) {
                return &pad;
            }
            if (!free && pad.id == 0) {
                free = &pad;
            }
        }
        if (free) {
            free->id = id;
        }
        return free;
    }
};

} // namespace BbPadUse
