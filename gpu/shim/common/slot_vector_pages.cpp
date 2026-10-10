// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: address-space reservation for Common::SlotVector (gpu/shadps4/common/slot_vector.h),
// after upstream shadPS4's src/common/slot_vector.cpp. Reserved pages cost no memory until
// committed, so a SlotVector can keep every entry at a fixed address.

#include <cstddef>

#include "common/assert.h"
#include "common/slot_vector.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif

namespace Common {

void* SlotVectorReservePages(std::size_t size) noexcept {
#ifdef _WIN32
    void* const base = VirtualAlloc(nullptr, size, MEM_RESERVE, PAGE_NOACCESS);
    ASSERT_MSG(base, "SlotVector: reserving address space failed");
#else
    void* const base = mmap(nullptr, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    ASSERT_MSG(base != MAP_FAILED, "SlotVector: reserving address space failed");
#endif
    return base;
}

void SlotVectorCommitPages(void* address, std::size_t size) noexcept {
#ifdef _WIN32
    void* const result = VirtualAlloc(address, size, MEM_COMMIT, PAGE_READWRITE);
#else
    void* const result =
        mmap(address, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
#endif
    ASSERT_MSG(result == address, "SlotVector: committing memory failed");
}

void SlotVectorReleasePages(void* base, [[maybe_unused]] std::size_t size) noexcept {
    if (!base) {
        return;
    }
#ifdef _WIN32
    VirtualFree(base, 0, MEM_RELEASE);
#else
    munmap(base, size);
#endif
}

} // namespace Common
