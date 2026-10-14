// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>

namespace BbProcessMemory {

struct Usage {
    std::uint64_t private_bytes; ///< committed memory of the process (Linux: its virtual size)
    std::uint64_t working_set;   ///< in RAM now
};

Usage Get();

} // namespace BbProcessMemory
