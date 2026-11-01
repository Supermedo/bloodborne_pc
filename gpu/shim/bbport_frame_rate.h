// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// bbport: what the game ties to a 30 FPS frame follows each frame's real time at any frame rate
// (bbport_frame_rate.cpp). The pieces here change no memory protection and allocate nothing, so
// the tests can run them on their own buffers.

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace BbFrameRate {

constexpr float Frame30 = 1.0f / 30.0f;
/// The shortest frame the values are made for.
constexpr float ShortestFrame = 1.0f / 1000.0f;

/// What the game has for one 30 FPS frame, made for a frame of `dt` seconds: a step of one frame,
/// a damping, growth or approach per frame raised to the frame's length in 30 FPS frames, a ramp
/// scaled by it. Applied every frame, they give what the game gives at 30 FPS in the same time.
struct Values {
    float dt = Frame30;          ///< a step of one frame (1/30 s at 30 FPS)
    float approach30 = Frame30;  ///< an approach by 1/30 of the distance a frame
    float approach10 = 0.1f;     ///< an approach by 0.1 of the distance a frame
    float step = 0.1f;           ///< a ramp by 0.1 a frame
    float decay08 = 0.8f;        ///< x0.8 a frame
    float grow12 = 1.2f;         ///< x1.2 a frame
};

inline Values ValuesFor(float dt) {
    if (dt == Frame30) {
        return Values{}; // the game's own constants, to the bit
    }
    const float frames = dt * 30.0f;
    Values v;
    v.dt = dt;
    v.approach30 = 1.0f - std::pow(1.0f - Frame30, frames);
    v.approach10 = 1.0f - std::pow(0.9f, frames);
    v.step = 0.1f * frames;
    v.decay08 = std::pow(0.8f, frames);
    v.grow12 = std::pow(1.2f, frames);
    return v;
}

/// How the game's main loop takes a frame's step (image + 0x2018e36 on, after the patches). The
/// game's own code steps by a fixed time (1/30 s; 1/60 s with a "no deltatime" patch). The frame
/// rate patches put one of two forms there that step by the frame time the pacer measured, kept
/// between its target and a bound: the built-in presets' form keeps the fixed time while the
/// target is 1/30 s, the patches folder's form has its bound in a constant.
struct MainStep {
    enum Form { Unknown, Fixed, ClampUnlessTarget30, Clamp } form = Unknown;
    float fixed = Frame30; ///< the fixed step (also ClampUnlessTarget30's at a 1/30 s target)
    float bound = Frame30; ///< the clamp forms' upper bound
};

/// The image offsets of the main loop's step.
constexpr std::uint64_t MainStepAt = 0x2018e36;
constexpr std::uint64_t MainStepEnd = 0x2018e6b;

inline MainStep ReadMainStep(const unsigned char* image, std::uint64_t size) {
    // mov dword [rbp - 0x38], fixed
    static constexpr unsigned char head[] = {0xc7, 0x45, 0xc8};
    // lea rax, [game state]; cmp qword [rax], 0; jne
    static constexpr unsigned char game[] = {0x48, 0x8d, 0x05, 0xcc, 0x76, 0x52, 0x03,
                                             0x48, 0x83, 0x38, 0x00, 0x75, 0x21};
    // cmp dword [rbx + 0x18], 1/30; je; mov dword [rbp - 0x38], 1/30; movss xmm0, [rbx + 0x264];
    // maxss xmm0, [rbx + 0x18]; minss xmm0, [rbp - 0x38]; movss [rbp - 0x38], xmm0; lea rax, ..
    static constexpr unsigned char unless30[] = {
        0x81, 0x7b, 0x18, 0x89, 0x88, 0x08, 0x3d, 0x74, 0x1e, 0xc7, 0x45, 0xc8, 0x89, 0x88, 0x08, 0x3d,
        0xf3, 0x0f, 0x10, 0x83, 0x64, 0x02, 0x00, 0x00, 0xf3, 0x0f, 0x5f, 0x43, 0x18, 0xf3, 0x0f, 0x5d,
        0x45, 0xc8, 0xf3, 0x0f, 0x11, 0x45, 0xc8, 0x48, 0x8d, 0x05, 0xa5, 0x76, 0x52, 0x03};
    // movss xmm0, [rbx + 0x264]; maxss xmm0, [rbx + 0x18]; minss xmm0, [rip + bound];
    // movss [rbp - 0x38], xmm0; movss [rip + ..], xmm0; lea rax, ..; 5 nops
    static constexpr unsigned char clamp_head[] = {0xf3, 0x0f, 0x10, 0x83, 0x64, 0x02, 0x00, 0x00,
                                                   0xf3, 0x0f, 0x5f, 0x43, 0x18, 0xf3, 0x0f, 0x5d,
                                                   0x05};
    static constexpr unsigned char clamp_mid[] = {0xf3, 0x0f, 0x11, 0x45, 0xc8, 0xf3, 0x0f, 0x11, 0x05};
    static constexpr unsigned char clamp_tail[] = {0x48, 0x8d, 0x05, 0xaa, 0x76, 0x52, 0x03,
                                                   0x90, 0x90, 0x90, 0x90, 0x90};
    MainStep m;
    const unsigned char* at = image + MainStepAt;
    if (size < MainStepEnd || std::memcmp(at, head, sizeof(head)) != 0) {
        return m;
    }
    std::memcpy(&m.fixed, at + 3, sizeof(m.fixed));
    const unsigned char* code = at + 7;
    if (std::memcmp(code, game, sizeof(game)) == 0) {
        m.form = MainStep::Fixed;
    } else if (std::memcmp(code, unless30, sizeof(unless30)) == 0) {
        m.form = MainStep::ClampUnlessTarget30;
    } else if (std::memcmp(code, clamp_head, sizeof(clamp_head)) == 0 &&
               std::memcmp(code + 21, clamp_mid, sizeof(clamp_mid)) == 0 &&
               std::memcmp(code + 34, clamp_tail, sizeof(clamp_tail)) == 0) {
        std::int32_t disp = 0;
        std::memcpy(&disp, code + 17, sizeof(disp));
        const std::int64_t bound_at = std::int64_t(MainStepAt + 7 + 21) + disp;
        if (bound_at < 0 || std::uint64_t(bound_at) + sizeof(m.bound) > size) {
            return m;
        }
        std::memcpy(&m.bound, image + bound_at, sizeof(m.bound));
        m.form = MainStep::Clamp;
    }
    if (!(m.fixed > 0.0f && m.fixed <= 1.0f) || !(m.bound > 0.0f && m.bound <= 1.0f)) {
        m.form = MainStep::Unknown;
    }
    return m;
}

/// The step the main loop takes for a frame the pacer measured at `measured` s with its target at
/// `target` s, as its instructions compute it (maxss and minss take the second operand when one is
/// NaN), kept at ShortestFrame or more.
inline float StepFor(const MainStep& m, float measured, float target) {
    float step = Frame30;
    std::uint32_t target_bits = 0, frame30_bits = 0;
    std::memcpy(&target_bits, &target, 4);
    std::memcpy(&frame30_bits, &Frame30, 4);
    if (m.form == MainStep::Fixed ||
        (m.form == MainStep::ClampUnlessTarget30 && target_bits == frame30_bits)) {
        step = m.fixed;
    } else if (m.form == MainStep::ClampUnlessTarget30) {
        step = measured > target ? measured : target;
        step = step < Frame30 ? step : Frame30;
    } else if (m.form == MainStep::Clamp) {
        step = measured > target ? measured : target;
        step = step < m.bound ? step : m.bound;
    }
    return step >= ShortestFrame ? step : ShortestFrame; // NaN too
}

/// The VEX instruction at `image + site` reads `image + constant` RIP-relative, with nothing after
/// its 32-bit displacement: it reads `value` instead (within 2 GiB of the instruction). False,
/// and nothing written, for other bytes.
inline bool RedirectOperand(unsigned char* image, std::uint64_t site, std::uint64_t constant,
                            const void* value) {
    unsigned char* code = image + site;
    std::size_t disp_at = 0, length = 0;
    if (code[0] == 0xc5 && (code[3] & 0xc7) == 0x05) {
        disp_at = 4, length = 8;
    } else if (code[0] == 0xc4 && (code[4] & 0xc7) == 0x05) {
        disp_at = 5, length = 9;
    } else {
        return false;
    }
    std::int32_t disp = 0;
    std::memcpy(&disp, code + disp_at, 4);
    const auto next = reinterpret_cast<std::uintptr_t>(code + length);
    if (next + std::intptr_t(disp) != reinterpret_cast<std::uintptr_t>(image + constant)) {
        return false;
    }
    const std::int64_t to = std::int64_t(reinterpret_cast<std::uintptr_t>(value)) - std::int64_t(next);
    if (to != std::int32_t(to)) {
        return false;
    }
    const std::int32_t to32 = std::int32_t(to);
    std::memcpy(code + disp_at, &to32, 4);
    return true;
}

/// `mov dword [rbp - 0x30], imm32` at `image + site` (a subsystem's fixed step), followed by
/// `mov rdi, [r12 + 0xd0]` (so edi is free there): a jump to a stub at `stub` that stores the 32
/// bits at `value` instead (`mov edi, [value]; mov [rbp - 0x30], edi; jmp back`, 14 bytes; the
/// stub, `value` and the site within 2 GiB of each other). The flags and every other register
/// stay as they were. False, and nothing written, for other bytes.
inline bool DetourStepStore(unsigned char* image, std::uint64_t site, unsigned char* stub,
                            const void* value) {
    static constexpr unsigned char store[] = {0xc7, 0x45, 0xd0};
    static constexpr unsigned char next[] = {0x49, 0x8b, 0xbc, 0x24, 0xd0, 0x00, 0x00, 0x00};
    unsigned char* code = image + site;
    if (std::memcmp(code, store, sizeof(store)) != 0 || std::memcmp(code + 7, next, sizeof(next)) != 0) {
        return false;
    }
    const auto rel = [](const void* from_end, const void* to, std::int32_t& out) {
        const std::int64_t d = std::int64_t(reinterpret_cast<std::uintptr_t>(to)) -
                               std::int64_t(reinterpret_cast<std::uintptr_t>(from_end));
        out = std::int32_t(d);
        return d == out;
    };
    std::int32_t load = 0, back = 0, there = 0;
    if (!rel(stub + 6, value, load) || !rel(stub + 14, code + 7, back) || !rel(code + 5, stub, there)) {
        return false;
    }
    unsigned char s[14] = {0x8b, 0x3d, 0, 0, 0, 0, // mov edi, [rip + load]
                           0x89, 0x7d, 0xd0,       // mov [rbp - 0x30], edi
                           0xe9, 0, 0, 0, 0};      // jmp back
    std::memcpy(s + 2, &load, 4);
    std::memcpy(s + 10, &back, 4);
    std::memcpy(stub, s, sizeof(s));
    unsigned char j[7] = {0xe9, 0, 0, 0, 0, 0x66, 0x90}; // jmp stub; a 2-byte nop
    std::memcpy(j + 1, &there, 4);
    std::memcpy(code, j, sizeof(j));
    return true;
}

/// Points the game's per-frame constants at live values (at load, after the patches, while the
/// image is still writable).
void PatchImage(unsigned char* image, std::uint64_t size);
/// VideoOut showed a frame: the live values follow the game's frame time.
void OnFlip();

} // namespace BbFrameRate
