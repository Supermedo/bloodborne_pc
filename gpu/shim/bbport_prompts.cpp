// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: button prompts that follow the input device (bbport_prompts.h).

#include "bbport_prompts.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>
#include <tuple>
#include <vector>

#include <xxhash.h>

#include "bbport_prompt_icons.h"
#include "bbport_settings.h"
#include "core/memory.h"

extern "C" const unsigned char bb_font_ttf[]; // bbport_overlay.cpp

namespace BbPrompts {
namespace {

using Bytes = std::vector<uint8_t>;
using BbPromptIcons::HudCount;

/// A button picture found in guest memory: a KG_* icon (its 1 KiB at `base`) or a sprite area
/// of the HUD atlas at `base` (its blocks, gathered row by row). `original` is the game's,
/// `written` the hash of what is there now (ours or the game's): anything else means the game
/// reused the memory.
struct Entry {
    uintptr_t base;
    int icon;       // BbPromptIcons::Icon (a KG_* texture), or -1 for an atlas sprite
    int sprite;     // BbPromptIcons::HudSprite, or -1
    Bytes original;
    uint64_t written;
};

std::mutex mutex; // entries, the keyboard pictures, searched; never held while guest memory is written
std::vector<Entry> entries;
std::array<Bytes, BbPromptIcons::Count> keyboard_icons;
std::array<Bytes, HudCount> keyboard_sprites;
bool keyboard_ready = false;
// Xbox and Switch (BbPromptIcons::Pad order); empty for an icon that stays the game's own.
std::array<std::array<Bytes, BbPromptIcons::Count>, BbPromptIcons::PadCount> pad_icons;
std::array<std::array<Bytes, HudCount>, BbPromptIcons::PadCount> pad_sprites;
std::set<uintptr_t> searched;      // HUD atlases already looked at
std::atomic<int> device{-1};       // -1 nothing seen yet, else a Device
std::atomic<bool> dirty{false};    // something to write: a new picture, a device or binding change
int shown = PlayStation;           // Apply's last decision (window thread): what guest memory has

Device Want() {
    switch (BbSettings::Get().button_prompts.load(std::memory_order_relaxed)) {
    case BbSettings::PromptsPlayStation: return PlayStation;
    case BbSettings::PromptsKeyboard: return Keyboard;
    case BbSettings::PromptsXbox: return Xbox;
    case BbSettings::PromptsSwitch: return Switch;
    default: {
        const int used = device.load(std::memory_order_relaxed);
        return used < 0 ? Keyboard : Device(used); // nothing used yet: keyboard
    }
    }
}

/// The Xbox and Switch pictures: they do not depend on the bindings, drawn once.
void DrawPads() {
    for (int pad = 0; pad < BbPromptIcons::PadCount; ++pad) {
        for (int icon = 0; icon < BbPromptIcons::Count; ++icon) {
            BbPromptIcons::Pixels pixels;
            if (!BbPromptIcons::DrawPad(BbPromptIcons::Icon(icon), BbPromptIcons::Pad(pad), pixels)) continue;
            const auto encoded = BbPromptIcons::Encode(pixels);
            pad_icons[pad][icon].assign(encoded.begin(), encoded.end());
        }
        for (int s = 0; s < HudCount; ++s) {
            BbPromptIcons::Pixels pixels;
            if (BbPromptIcons::DrawPad(BbPromptIcons::HudIcon[s], BbPromptIcons::Pad(pad), pixels)) {
                pad_sprites[pad][s] = BbPromptIcons::RenderIconArea(pixels, BbPromptIcons::HudAreas[s]);
            }
        }
    }
}

/// Byte offsets from `base` of an entry's 16-byte pieces: the whole icon, or each block of the
/// sprite's area in the atlas's tiled layout.
std::vector<size_t> Pieces(int sprite) {
    std::vector<size_t> out;
    if (sprite < 0) {
        for (size_t at = 0; at < size_t(BbPromptIcons::Bytes); at += 16) out.push_back(at);
        return out;
    }
    const auto& area = BbPromptIcons::HudAreas[sprite];
    for (int by = area.y / 4; by < (area.y + area.h) / 4; ++by) {
        for (int bx = area.x / 4; bx < (area.x + area.w) / 4; ++bx) {
            out.push_back(BbPromptIcons::TiledBlockOffset(bx, by, BbPromptIcons::HudAtlasWidth));
        }
    }
    return out;
}

Bytes Gather(uintptr_t base, int sprite) {
    Bytes out;
    for (size_t at : Pieces(sprite)) {
        const auto* p = reinterpret_cast<const uint8_t*>(base + at);
        out.insert(out.end(), p, p + 16);
    }
    return out;
}

/// The guest range an entry touches (to check that it is mapped and to invalidate it).
std::pair<uintptr_t, size_t> Range(uintptr_t base, int sprite) {
    const auto pieces = Pieces(sprite);
    const auto [lo, hi] = std::minmax_element(pieces.begin(), pieces.end());
    return {base + *lo, *hi + 16 - *lo};
}

uint64_t Hash(const Bytes& data) {
    return XXH3_64bits(data.data(), data.size());
}

void Add(uintptr_t base, int icon, int sprite, Bytes original) {
    std::scoped_lock lock{mutex};
    for (const Entry& e : entries) {
        if (e.base == base && e.sprite == sprite) return;
    }
    const uint64_t hash = Hash(original);
    entries.push_back({base, icon, sprite, std::move(original), hash});
    dirty = true;
}

} // namespace

void Configure(const char* const* inputs, int count) {
    static const bool font = BbPromptIcons::SetFont(bb_font_ttf);
    if (!font || count != BbPromptIcons::Count) return;
    std::array<BbPromptIcons::Inputs, BbPromptIcons::Count> lists;
    std::array<Bytes, BbPromptIcons::Count> icons;
    for (int i = 0; i < count; ++i) {
        const std::string text = inputs[i] ? inputs[i] : "";
        for (size_t start = 0; start < text.size();) {
            const size_t end = std::min(text.find(' ', start), text.size());
            if (end > start) lists[i].push_back(text.substr(start, end - start));
            start = end + 1;
        }
        const auto icon = BbPromptIcons::Render(lists[i]);
        icons[i].assign(icon.begin(), icon.end());
    }
    std::array<Bytes, HudCount> sprites;
    for (int s = 0; s < HudCount; ++s) {
        sprites[s] = BbPromptIcons::RenderArea(lists[BbPromptIcons::HudIcon[s]], BbPromptIcons::HudAreas[s]);
    }
    std::scoped_lock lock{mutex};
    if (!keyboard_ready) DrawPads();
    keyboard_icons = icons;
    keyboard_sprites = sprites;
    keyboard_ready = true;
    dirty = true;
}

void OnInput(Device value) {
    if (device.exchange(value, std::memory_order_relaxed) != value) dirty = true;
}

void OnControllerConnected(Device value) {
    int unknown = -1;
    if (device.compare_exchange_strong(unknown, value, std::memory_order_relaxed)) dirty = true;
}

void OnUpload(uintptr_t address, size_t size, uint32_t width, uint32_t height, uint32_t levels,
              uint32_t bits) {
    static const bool trace = [] {
        const char* env = std::getenv("BB_PROMPT_TRACE");
        return env && env[0] == '1';
    }();
    if (trace && size <= 256 * 256 * 4) {
        static std::mutex trace_mutex;
        static std::set<uint64_t> seen;
        const uint64_t h = XXH3_64bits(reinterpret_cast<const void*>(address), size);
        std::scoped_lock lock{trace_mutex};
        if (seen.insert(h).second) {
            std::printf("Prompts trace: %ux%u levels %u bits %u, %zu bytes at %#llx, hash %016llx\n", width,
                        height, levels, bits, size, (unsigned long long)address, (unsigned long long)h);
            char name[64];
            std::snprintf(name, sizeof(name), "prompt-trace-%ux%u-%016llx.bin", width, height,
                          (unsigned long long)h);
            if (FILE* f = std::fopen(name, "wb")) {
                std::fwrite(reinterpret_cast<const void*>(address), 1, size, f);
                std::fclose(f);
            }
        }
    }
    if (levels != 1 || bits != 128) return;
    if (width == BbPromptIcons::Size && height == BbPromptIcons::Size && size == size_t(BbPromptIcons::Bytes)) {
        // A KG_* icon: the menus' key guides and the buttons in their messages.
        Bytes bytes = Gather(address, -1);
        const uint64_t hash = Hash(bytes);
        for (int i = 0; i < BbPromptIcons::Count; ++i) {
            if (BbPromptIcons::PlayStationHash[i] == hash) Add(address, i, -1, std::move(bytes));
        }
        return;
    }
    if (width == uint32_t(BbPromptIcons::HudAtlasWidth) && height == uint32_t(BbPromptIcons::HudAtlasHeight)) {
        // The HUD atlas: the prompts in the world draw its Circle, Cross and L3.
        {
            std::scoped_lock lock{mutex};
            if (!searched.insert(address).second) return;
        }
        for (int s = 0; s < HudCount; ++s) {
            Bytes bytes = Gather(address, s);
            if (Hash(bytes) == BbPromptIcons::HudPlayStationHash[s]) Add(address, -1, s, std::move(bytes));
        }
    }
}

void Apply() {
    const Device want = Want();
    if (!dirty.exchange(false) && want == shown) return;
    shown = want;

    std::vector<Entry> snapshot;
    std::array<Bytes, BbPromptIcons::Count> icons; // empty: the game's own
    std::array<Bytes, HudCount> sprites;
    {
        std::scoped_lock lock{mutex};
        if (want != PlayStation && !keyboard_ready) return;
        snapshot = entries;
        if (want == Keyboard) {
            icons = keyboard_icons;
            sprites = keyboard_sprites;
        } else if (want != PlayStation) {
            const int pad = want == Xbox ? BbPromptIcons::PadXbox : BbPromptIcons::PadSwitch;
            icons = pad_icons[pad];
            sprites = pad_sprites[pad];
        }
    }
    // Written without any lock: a write to a page the texture cache protects faults into it.
    auto* memory = Core::Memory::Instance();
    std::vector<std::tuple<uintptr_t, int, uint64_t>> written; // hash 0: the entry is gone
    for (const Entry& e : snapshot) {
        const auto [start, length] = Range(e.base, e.sprite);
        if (!memory->IsValidMapping(start, length) || Hash(Gather(e.base, e.sprite)) != e.written) {
            written.emplace_back(e.base, e.sprite, 0); // unmapped, or the game put something else there
            continue;
        }
        const Bytes& mine = e.sprite < 0 ? icons[e.icon] : sprites[e.sprite];
        const Bytes& target = mine.empty() ? e.original : mine;
        const uint64_t target_hash = Hash(target);
        if (target_hash == e.written) continue;
        const auto pieces = Pieces(e.sprite);
        for (size_t i = 0; i < pieces.size(); ++i) {
            std::memcpy(reinterpret_cast<void*>(e.base + pieces[i]), &target[i * 16], 16);
        }
        memory->InvalidateMemory(start, length);
        written.emplace_back(e.base, e.sprite, target_hash);
    }
    std::scoped_lock lock{mutex};
    for (const auto& [base, sprite, hash] : written) {
        for (size_t i = 0; i < entries.size(); ++i) {
            if (entries[i].base != base || entries[i].sprite != sprite) continue;
            if (hash) entries[i].written = hash;
            else entries.erase(entries.begin() + i);
            break;
        }
    }
}

} // namespace BbPrompts

extern "C" void bbgpu_prompts_configure(const char* const* inputs, int count) {
    BbPrompts::Configure(inputs, count);
}
