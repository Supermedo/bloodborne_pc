// bbport: when the "Video memory is running low" banner comes up (texture_cache.cpp). Plain data,
// so tests/test_vram_warning.cpp checks it without a GPU.
#pragma once
#include <algorithm>
#include <cstdint>

namespace BbVramWarning {

constexpr uint64_t MiB = uint64_t(1) << 20;
// Again each 256 MiB closer to the limit.
constexpr uint64_t Step = 256 * MiB;

// How far below the limit the warning starts: `wanted` (BB_VRAM_WARN_MB, 1.5 GB by default; 0 turns
// it off), but not before the collector's critical mark. Under that mark the collector has not done
// its utmost yet: with the low-VRAM profile's 3496 MiB budget on a 4 GB card, 1.5 GB left is 2 GB
// in use, below the 70% where collecting starts, and the banner came up all session (Sirpixel, RX
// 570, beta 8). At least one step, for a budget whose critical mark is at its very end. From 8 GB
// the critical mark is ~1.6 GB below the limit and the 1.5 GB stand.
inline uint64_t Margin(uint64_t limit, uint64_t critical, uint64_t wanted) {
    const uint64_t to_critical = limit > critical ? limit - critical : 0;
    return std::min(wanted, std::max(to_critical, Step));
}

struct State {
    uint64_t warned_free = ~uint64_t(0); // armed; the free bytes at the last warning otherwise

    // Whether to warn now, with `free` bytes left under the limit. Again each step closer; armed
    // again only with a step more than the margin free, so usage going back and forth around the
    // mark does not bring the banner up again each time.
    bool Check(uint64_t free, uint64_t margin) {
        if (margin == 0) {
            return false;
        }
        if (free >= margin + Step) {
            warned_free = ~uint64_t(0);
            return false;
        }
        if (free >= margin) {
            return false;
        }
        if (warned_free == ~uint64_t(0) || free + Step <= warned_free) {
            warned_free = free;
            return true;
        }
        return false;
    }
};

} // namespace BbVramWarning
