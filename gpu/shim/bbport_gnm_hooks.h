// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: hooks on the entry points of the eboot's libGnm (sce::Gnm command buffer methods, linked
// statically into the game: it writes its own PM4). They are the game's graphics API, the level a
// translation layer like DXVK sits at. BB_GNM_CENSUS=1 counts the calls per function and prints
// the busiest every 5 s; later steps replace the functions themselves.
#pragma once

#include <cstdint>

namespace BbGnmHooks {
/// Patches the image (the loader calls it once, before any game code runs, while the image is
/// still writable). Each entry is checked byte for byte against the 1.09 eboot.
void PatchImage(unsigned char* image, std::uint64_t size);

/// BB_GNM_OBSERVE=1: checks a submitted command buffer against the packets the hooked API calls
/// wrote (the submitting thread; statistics in the census report).
void CheckSubmission(const std::uint32_t* commands, std::uint64_t dwords);

/// BB_GNM_OBSERVE: packets our GnmDriver writes into space the game reserved ([commands,
/// commands + dwords)), marked when the guard goes out of scope.
struct DriverWrite {
    std::uint32_t* commands;
    std::uint64_t dwords;
    ~DriverWrite();
};
} // namespace BbGnmHooks
