// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <span>

// bbport: what ProcessGraphics keeps of a command buffer after a garbage packet header. A
// submit-and-flip buffer ends with 64 dwords that PatchFlipRequest (gnmdriver.cpp) turned into the
// surface lock (WriteData), the flip marker (Nop, PatchedFlip) and its label or EOP event. Dropped
// with the rest, the flip already queued in VideoOut would never get its interrupt, so those 64
// dwords are kept when the garbage comes before them. Anything else after the garbage is dropped.
inline std::span<const std::uint32_t> BbKeepAfterGarbage(std::span<const std::uint32_t> whole,
                                                         const std::uint32_t* at) {
    constexpr std::size_t FlipTail = 64;
    constexpr std::uint32_t WriteData = 0x37, Nop = 0x10, PatchedFlip = 0x68750776u;
    const auto is_packet = [](std::uint32_t header, std::uint32_t opcode) {
        return (header >> 30) == 3 && ((header >> 8) & 0xff) == opcode;
    };
    if (whole.size() < FlipTail) {
        return {};
    }
    const auto tail = whole.subspan(whole.size() - FlipTail);
    if (at < whole.data() || at >= tail.data() || !is_packet(tail[0], WriteData) ||
        !is_packet(tail[5], Nop) || tail[6] != PatchedFlip) {
        return {};
    }
    return tail;
}
