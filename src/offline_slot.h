// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <cstdint>

namespace offline_slot {
using Record = std::array<std::uint32_t, 43>;
struct Flags {
  void* context;
  bool (*get)(void*, std::uint32_t, std::uint32_t, std::uint32_t*);
  bool (*set)(void*, std::uint32_t, std::uint32_t, std::uint32_t);
};
enum class Result {
  installed, already_present, invalid_source, io_error, no_empty_slot,
  concurrent_change, write_failed_restored, write_failed_unrestored
};
struct Outcome { Result result; int slot; };
// A dungeon record: nonzero channel with the high bit clear, a chalice dungeon map (area 29)
// in word 3, mirrored in word 0.
bool validate_record(const Record& source);
bool validate_83ss4pyq(const Record& source);
Outcome install(const Flags& flags, int target_slot, const Record& source);
Outcome install_83ss4pyq(const Flags& flags, int target_slot,
                          const Record& source);
} // namespace offline_slot
