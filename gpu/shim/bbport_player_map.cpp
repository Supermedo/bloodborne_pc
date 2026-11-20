// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: the map block the player stands in, for the path tracer's per-map knobs
// (docs/PATH_TRACING.md "Per-map look"). Read from the render thread with guarded copies: the
// player character can be freed mid-read (title, loads).
#include "../bbgpu.h"
#include "bbport_guest_memory.h"

#include <atomic>
#include <cstdint>

namespace {
std::atomic<unsigned char*> g_image{nullptr};

// Bloodborne 1.09, offsets from the image start (PS4 VA - 0x400000): WorldChrMan, its player
// at +0x60 (checked against the PlayerIns vtable), the player's map block at +0x3F8
// (0xAABBCCDD = mAA_BB_CC_DD).
constexpr uintptr_t kWcmGlobal = 0x553E878, kVtPlayerIns = 0x538F310;
constexpr uintptr_t kWcmPlayer = 0x60, kChrBlock = 0x3F8;

bool Ptr(uintptr_t p) {
    return p >= 0x10000 && !(p & 7) && p < (1ull << 47);
}
} // namespace

extern "C" void bbgpu_player_map_set_image(unsigned char* image) {
    g_image.store(image, std::memory_order_release);
}

// 0xFFFFFFFF while there is no player (title, loads).
extern "C" uint32_t bbgpu_player_block(void) {
    unsigned char* image = g_image.load(std::memory_order_acquire);
    if (!image) {
        return 0xFFFFFFFFu;
    }
    const uintptr_t base = reinterpret_cast<uintptr_t>(image);
    uintptr_t wcm = 0, me = 0, vt = 0;
    uint32_t block = 0xFFFFFFFFu;
    if (!BbGuest::Read(&wcm, base + kWcmGlobal, sizeof(wcm)) || !Ptr(wcm) ||
        !BbGuest::Read(&me, wcm + kWcmPlayer, sizeof(me)) || !Ptr(me) ||
        !BbGuest::Read(&vt, me, sizeof(vt)) || vt != base + kVtPlayerIns ||
        !BbGuest::Read(&block, me + kChrBlock, sizeof(block))) {
        return 0xFFFFFFFFu;
    }
    return block;
}
