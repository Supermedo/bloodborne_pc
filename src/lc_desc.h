// SPDX-License-Identifier: GPL-2.0-or-later
// Glyph descriptions composed from game text (catalog v4 recipes; see make_catalog_v4.py).
// The same rules as short_desc.py: Farm / False depth, then L1..L4 (runes, gems, weapons, tags,
// that layer's bosses), items without a layer, the chalice, bosses without a layer; the least
// important items (bosses, last first; then the chalice) are dropped until the line fits.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace lc_desc {

enum Kind : uint8_t { kFarm = 1, kFalse, kRune, kGem, kWeapon, kGameTag, kOwnTag, kBoss, kChalice };
struct Item { uint8_t kind, layer, priority, order; uint16_t fmg, extra; uint32_t id, value; };
static_assert(sizeof(Item) == 16, "catalog item layout");

// Per language: the probe hash that recognises it, the list comma, our own four words and the
// mod's own sentences (messages.json order).
struct Language {
    uint32_t probe;
    std::string code;
    std::u16string comma, farm, rare, glitch, false_depth;
    std::vector<std::u16string> messages;
};

// The game's text for (fmg, id), or null / empty when it has none.
using Lookup = const char16_t* (*)(void* context, uint32_t fmg, uint32_t id);

// FNV-1a 32 over the UTF-16 code units of the probe texts, concatenated.
uint32_t probe_hash(const std::vector<std::pair<uint32_t, uint32_t>>& probes, Lookup lookup, void* context);
// The language whose probe hash matches, else the first (English).
const Language& detect(const std::vector<Language>& languages, uint32_t hash);

// Display width: East Asian wide / fullwidth characters count 2.
size_t width(const std::u16string& text);
// The description, at most `limit` wide ("—" when nothing is left).
std::u16string compose(const Item* items, size_t count, const Language& language, Lookup lookup, void* context,
                       size_t limit = 150);

std::u16string utf16(const char* text, size_t size);

}  // namespace lc_desc
