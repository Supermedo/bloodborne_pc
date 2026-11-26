// SPDX-License-Identifier: GPL-2.0-or-later
// Exercise the real native menu applier without invoking guest code or its thread.
#include "../gpu/shim/bbport_pc_menu.cpp"
#include <stdexcept>

extern "C" void runtime_restart() { throw std::runtime_error("unexpected restart"); }

void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

int main() {
    try {
        using namespace BbSettings;
        auto& s = Get();
        unsigned cases = 0;
        for (unsigned mask = 0; mask != 8; ++mask) {
            s.fg401_supported = (mask & 1) != 0;
            s.fg402_supported = (mask & 2) != 0;
            s.dlssg_max_multiplier = (mask & 4) ? 4 : 1;
            for (int selected = FrameGenOff; selected != FrameGenCount; ++selected) {
                for (int pick = FrameGenOff; pick != FrameGenCount; ++pick) {
                    SelectFrameGen(selected);
                    const bool supported = pick == FrameGenOff || pick == FrameGenFsr3 ||
                        (pick == FrameGenFsr401 && (mask & 1)) ||
                        (pick == FrameGenFsr402 && (mask & 2)) ||
                        (pick == FrameGenDlss && (mask & 4));
                    Require(FrameGenSupported(pick) == supported, "provider support mismatch");
                    const bool accepted = supported || pick == selected;
                    Require(Apply(Line::FrameGen, pick) == accepted, "stale unsupported pick saved");
                    Require(s.frame_gen == (accepted ? pick : selected), "saved choice lost");
                    if (!accepted)
                        Require(Value(Line::FrameGen) == selected && applied[Line::FrameGen] == selected,
                                "rejected menu choice not reset");
                    ++cases;
                }
            }
        }
        std::printf("PASS: %u native frame-generation support/selection cases\n", cases);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
