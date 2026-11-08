// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: what the game ties to a 30 FPS frame follows each frame's real time.
//
// The frame rate patches (the launcher's FPS presets, 60/90/120/Uncap FPS++ in the patches folder)
// step the game's main loop by the frame time its pacer measures, but much of what the game ties to
// a 30 FPS frame they set to one value or leave as it is: a step of 1/30 s, approaches and ramps by
// a fixed amount a frame. A value made for 30 FPS then runs 4 times as fast at 120 FPS, one set to
// 1/240 s at half speed. The timers of the body parts the physics moves (0x1515ec0) ran 4 times as
// fast.
//
// The sites are bbport 0.6's list (deadinside28/bloodborne_pc, gpu/shim/bbport_frame_rate.cpp,
// GPL-2.0-or-later: found by the frame time each function is called with), less what is read once
// when something is made (cloth constraints' stiffness bound, particle buffer sizes: a value taken
// in a slow frame would stay for the session, so the patches' own values stay there), plus the wall
// check's recovery (0x1514c77, the counterpart of its damping). The port itself times the air drag
// (scripts/air_drag.py), the plants' clocks (scripts/foliage_time.py, the 0x2313d8d..0x2315d71
// steps), the effects (scripts/effect_time.py: whole 1/60 s steps) and the cloth since 1.9.5, so
// those sites are not here: a measured step below 1/60 s at the effects' update (0x1f77d4a) would
// pile their particles up again. Their instructions read the value from a page next to the image
// instead of the constant (the operand's displacement changes, nothing else). The values follow the
// step the main loop takes, worked out from its instructions (ReadMainStep): written once for a
// fixed step, by OnFlip for each frame otherwise; the game's own fixed 1/30 s step is left alone.
// One subsystem's fixed step (0x1572a7d, 1/240 s in Uncap FPS++) stores the frame's time through a
// 14-byte stub. Sites whose bytes are not the expected ones (a patch changed them) are left as they
// are, and everything is when the main loop is in a form not known here. Where the 30, 60 and
// 90 FPS++ presets set their own values (a timer at 0x12a4416, the follow camera's approaches, the
// fixed step: 60 FPS++'s camera approach of 1/15 a frame at 60 FPS, the game's 1/30 at 30 FPS),
// the game's values, timed, take their place. BB_FRAME_VALUES=0 turns it off.

#include "bbport_frame_rate.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <new>

#include "../bbgpu.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif

namespace BbFrameRate {
namespace {
using u64 = std::uint64_t;

/// What the redirected instructions read: written for each frame (once for a fixed step).
struct alignas(64) Live {
    float dt = Frame30;
    float approach30 = Frame30;
    float approach10 = 0.1f;
    float step_down = -0.1f;
    float step_up = 0.1f;
    float decay08 = 0.8f;
    float grow12 = 1.2f;
};

Live* live = nullptr;
MainStep main_step;
std::atomic<unsigned char*> image_base{nullptr};

/// The game's frame pacer: a pointer at this image offset (set when the main loop starts), its
/// frame time target at +0x18 and the frame time it measured at +0x264.
constexpr u64 PacerPointer = 0x55404f8;

struct Redirect {
    u64 site;
    u64 constant;
    std::size_t value; ///< offsetof(Live, ..)
};
#define LIVE(field) offsetof(Live, field)
constexpr Redirect Redirects[] = {
    // A step of one 30 FPS frame, or "one frame":
    {0xab9f0d, 0x47efdcc, LIVE(dt)},   // a time step, one frame when unset
    {0xdca3ed, 0x4888688, LIVE(dt)},   // a per-frame budget
    {0x1044943, 0x48cc3f4, LIVE(dt)},  // an update by one frame
    {0x12a4416, 0x492577c, LIVE(dt)},  // a timer advanced by one frame each frame
    {0x15dc1f7, 0x4926c9c, LIVE(dt)},
    {0x173456e, 0x49270e4, LIVE(dt)},  // an update by one frame
    {0x17f9d26, 0x4927554, LIVE(dt)},
    {0x180f647, 0x49275f4, LIVE(dt)},  // a timer advanced by one frame each frame
    {0x18bc4f7, 0x4927c4c, LIVE(dt)},  // an update by one frame (twice)
    {0x18bc50d, 0x4927c4c, LIVE(dt)},
    {0x1dbc308, 0x4928e98, LIVE(dt)},  // frames counted at one frame's time
    {0x227784b, 0x4929b80, LIVE(dt)},  // timers advanced by one frame
    {0x2277892, 0x4929b80, LIVE(dt)},
    // Body parts the physics moves (0x1515ec0, each part every frame): their timers.
    {0x1515fab, 0x4926364, LIVE(dt)},
    {0x1516030, 0x4926364, LIVE(dt)},
    // Ramps and approaches per frame:
    {0x19084f5, 0x4927fd4, LIVE(step_down)}, // a value ramped by 0.1 a frame
    {0x1908522, 0x4927fd4, LIVE(step_down)},
    {0x1908518, 0x4927fe0, LIVE(step_up)},
    {0x190854c, 0x4927fe0, LIVE(step_up)},
    {0x143e2b2, 0x4925e94, LIVE(approach30)}, // the follow camera's approaches
    {0x143af8c, 0x4925e68, LIVE(approach10)},
    // Damping and growth per frame:
    {0x1514c4d, 0x4926334, LIVE(decay08)}, // the run's wall check: x0.8 a frame while blocked,
    {0x1514c77, 0x4926338, LIVE(grow12)},  // x1.2 a frame back up to 1 after
};
#undef LIVE
/// A subsystem stepped by a fixed time (`mov dword [rbp - 0x30], imm32`; 1/30 s, 1/240 s in
/// Uncap FPS++).
constexpr u64 FixedStepSite = 0x1572a7d;
constexpr u64 HighestSite = 0x2277892 + 9;

constexpr std::size_t PageSize = 4096;
constexpr std::size_t BlockSize = 64 * 1024; // the live page, then the stub page

/// A block within 512 MiB below the image (RIP-relative operands and rel32 jumps reach it).
unsigned char* MapNear(unsigned char* image) {
    const auto base = reinterpret_cast<std::uintptr_t>(image);
    for (std::uintptr_t k = 1; k <= 64; ++k) {
        const std::uintptr_t hint = base - k * (8ull << 20);
        if (hint >= base) {
            break;
        }
#ifdef _WIN32
        if (void* p = VirtualAlloc(reinterpret_cast<void*>(hint), BlockSize, MEM_RESERVE | MEM_COMMIT,
                                   PAGE_READWRITE)) {
            return static_cast<unsigned char*>(p);
        }
#else
        void* p = mmap(reinterpret_cast<void*>(hint), BlockSize, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
        if (p != MAP_FAILED) {
            return static_cast<unsigned char*>(p);
        }
#endif
    }
    return nullptr;
}

bool ProtectStubs(unsigned char* stubs) {
#ifdef _WIN32
    DWORD old = 0;
    if (!VirtualProtect(stubs, PageSize, PAGE_EXECUTE_READ, &old)) {
        return false;
    }
    FlushInstructionCache(GetCurrentProcess(), stubs, PageSize);
    return true;
#else
    return mprotect(stubs, PageSize, PROT_READ | PROT_EXEC) == 0;
#endif
}

void Store(const Values& v) {
    live->dt = v.dt;
    live->approach30 = v.approach30;
    live->approach10 = v.approach10;
    live->step_down = -v.step;
    live->step_up = v.step;
    live->decay08 = v.decay08;
    live->grow12 = v.grow12;
}

void DescribeMainStep(const MainStep& m, char* out, std::size_t size) {
    if (m.form == MainStep::Fixed) {
        std::snprintf(out, size, "a fixed %.6f s", double(m.fixed));
    } else if (m.form == MainStep::ClampUnlessTarget30) {
        std::snprintf(out, size, "the measured time between the target and 1/30 s, %.6f s at a "
                      "1/30 s target", double(m.fixed));
    } else {
        std::snprintf(out, size, "the measured time between the target and %.6f s", double(m.bound));
    }
}
} // namespace

void PatchImage(unsigned char* image, std::uint64_t size) {
    if (const char* off = std::getenv("BB_FRAME_VALUES"); off && off[0] == '0') {
        std::printf("Frame rate values: off (BB_FRAME_VALUES=0)\n");
        return;
    }
    if (!image || size < HighestSite) {
        return;
    }
    const MainStep main = ReadMainStep(image, size);
    if (main.form == MainStep::Unknown) {
        std::printf("Frame rate values: the main loop's step at +%#llx is in a form not known here: "
                    "as the patches have them\n",
                    static_cast<unsigned long long>(MainStepAt));
        return;
    }
    if (main.form == MainStep::Fixed && main.fixed == Frame30) {
        std::printf("Frame rate values: the game's own fixed 1/30 s step: left as it is\n");
        return;
    }
    unsigned char* block = MapNear(image);
    if (!block) {
        std::printf("Frame rate values: no memory next to the game's code: as the patches have them\n");
        return;
    }
    live = new (block) Live{};
    unsigned char* stubs = block + PageSize;
    int redirected = 0;
    for (const Redirect& r : Redirects) {
        if (RedirectOperand(image, r.site, r.constant, block + r.value)) {
            ++redirected;
        } else {
            std::printf("Frame rate values: +%#llx is not the expected code, left as it is\n",
                        static_cast<unsigned long long>(r.site));
        }
    }
    unsigned char saved[7];
    std::memcpy(saved, image + FixedStepSite, sizeof(saved));
    bool step = DetourStepStore(image, FixedStepSite, stubs, &live->dt);
    const char* step_note = step ? "too" : "is not the expected code, left as it is";
    if (step && !ProtectStubs(stubs)) {
        std::memcpy(image + FixedStepSite, saved, sizeof(saved)); // not run yet: undone
        step = false;
        step_note = "is left as it is (its stub page could not be made executable)";
    }
    main_step = main;
    if (main.form == MainStep::Fixed) {
        Store(ValuesFor(StepFor(main, 0.0f, 0.0f)));
    }
    image_base.store(image, std::memory_order_release);
    char how[128];
    DescribeMainStep(main, how, sizeof(how));
    std::printf("Frame rate values: %d of %zu per-frame values follow the main loop's step (%s); the "
                "fixed step at +%#llx %s\n",
                redirected, std::size(Redirects), how, static_cast<unsigned long long>(FixedStepSite),
                step_note);
}

void OnFlip() {
    unsigned char* image = image_base.load(std::memory_order_acquire);
    if (!image || main_step.form == MainStep::Fixed) {
        return;
    }
    std::uint64_t pacer = 0;
    std::memcpy(&pacer, image + PacerPointer, sizeof(pacer));
    if (!pacer) {
        return; // the main loop has not started
    }
    float measured = 0.0f, target = 0.0f;
    std::memcpy(&measured, reinterpret_cast<const void*>(pacer + 0x264), sizeof(measured));
    std::memcpy(&target, reinterpret_cast<const void*>(pacer + 0x18), sizeof(target));
    Store(ValuesFor(StepFor(main_step, measured, target)));
}

} // namespace BbFrameRate

extern "C" void bbgpu_frame_rate_patch(unsigned char* image, uint64_t size) {
    BbFrameRate::PatchImage(image, size);
}
