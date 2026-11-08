// SPDX-License-Identifier: GPL-2.0-or-later
#include "offline_slot.h"

#include <cstddef>

namespace offline_slot {
namespace {
constexpr std::uint32_t kBase = 94000000;
constexpr std::uint32_t kStride = 100000;
constexpr std::uint32_t kWidth = 32;
constexpr std::size_t kWords = 43;

// v0.5 LocalChalice/catalog.json, key 83ss4pyq: FormData v4 words 2..25.
// They map to slot-record words 3..26. Every other word comes from the caller.
constexpr std::array<std::uint32_t, 24> kKnownLayout = {
    0x1d0a0000, 0x000017dd, 0xffffffff, 0xffffffff,
    0xffffffff, 0xffffff23, 0xffffffff, 0xffffff32,
    0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff,
    0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff,
    0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff,
    0x00000000, 0x00000000, 0x00000000, 0x00000000};

std::uint32_t id(int slot, std::size_t word) {
  return kBase + static_cast<std::uint32_t>(slot) * kStride +
         static_cast<std::uint32_t>(word) * kWidth;
}
bool read_record(const Flags& f, int slot, Record* out) {
  for (std::size_t i = 0; i < kWords; ++i)
    if (!f.get(f.context, id(slot, i), kWidth, &(*out)[i])) return false;
  return true;
}
bool same_record(const Flags& f, int slot, const Record& want) {
  Record actual{};
  return read_record(f, slot, &actual) && actual == want;
}
// Commit the map discriminator last, after readback of all other words.
bool put_record(const Flags& f, int slot, const Record& record) {
  for (std::size_t i = 0; i < kWords; ++i) {
    if (i == 3) continue;
    if (!f.set(f.context, id(slot, i), kWidth, record[i])) return false;
    std::uint32_t check = 0;
    if (!f.get(f.context, id(slot, i), kWidth, &check) || check != record[i])
      return false;
  }
  if (!f.set(f.context, id(slot, 3), kWidth, record[3])) return false;
  return same_record(f, slot, record);
}
} // namespace

bool validate_83ss4pyq(const Record& source) {
  // Top bit marks the game's absent / never-uploaded channel.
  const std::uint64_t channel = (std::uint64_t{source[2]} << 32) | source[1];
  if (channel == 0 || (channel >> 63) != 0) return false;
  for (std::size_t i = 0; i < 20; ++i)
    if (source[i + 3] != kKnownLayout[i]) return false;
  // Words 23..26 hold the creator name. The catalog leaves it empty; a dungeon
  // joined through LocalChalice v0.4/v0.5 is saved as "LocalHunter".
  constexpr std::array<std::uint32_t, 4> kLocalHunter = {
      0x61636f4c, 0x6e75486c, 0x00726574, 0x00000000};
  bool empty_name = true, local_name = true;
  for (std::size_t i = 0; i < 4; ++i) {
    empty_name = empty_name && source[23 + i] == kKnownLayout[20 + i];
    local_name = local_name && source[23 + i] == kLocalHunter[i];
  }
  return empty_name || local_name;
}

bool validate_record(const Record& source) {
  const std::uint64_t channel = (std::uint64_t{source[2]} << 32) | source[1];
  return channel != 0 && (channel >> 63) == 0 && (source[3] >> 24) == 0x1d &&
         source[0] == source[3];
}

Outcome install_83ss4pyq(const Flags& f, int target_slot,
                          const Record& source) {
  if (!validate_83ss4pyq(source)) return {Result::invalid_source, -1};
  return install(f, target_slot, source);
}

Outcome install(const Flags& f, int target_slot, const Record& source) {
  if (!f.get || !f.set || target_slot < 1 || target_slot > 6 ||
      !validate_record(source))
    return {Result::invalid_source, -1};
  // The same dungeon may sit on several altars, as in the game: each slot has its own record
  // and its own progress flags. Only the target slot must be empty.
  Record original{};
  if (!read_record(f, target_slot, &original)) return {Result::io_error, -1};
  if (original[1] != 0 || original[2] != 0x80000000 ||
      original[3] != 0xffffffff || original[4] != 0xffffffff)
    return {Result::no_empty_slot, target_slot};
  if (!same_record(f, target_slot, original))
    return {Result::concurrent_change, target_slot};
  if (put_record(f, target_slot, source))
    return {Result::installed, target_slot};
  const bool restored = put_record(f, target_slot, original);
  return {restored ? Result::write_failed_restored
                   : Result::write_failed_unrestored, target_slot};
}
} // namespace offline_slot
