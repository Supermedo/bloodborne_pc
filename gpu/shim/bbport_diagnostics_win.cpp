// SPDX-License-Identifier: GPL-2.0-or-later
// Linux's dma-buf mappings and ELF/SysV diagnostic trampolines cannot be installed in
// the Windows loader. Keep their interfaces available to the shared renderer while
// using the Windows pagefile-backed memory and exception tracking implementation.
#include "bbport_free_check.h"
#include "bbport_gnm_hooks.h"
#include "bbport_guest_hooks.h"
#include "bbport_guest_memory.h"
#include "bbport_heap_sites.h"

#include <cstdio>
#include <cstdlib>

namespace BbGuestMemory {
bool Usable(const Vulkan::Instance&) { return false; }
bool PcModelGpu(const Vulkan::Instance&) { return false; }
void Install(const Vulkan::Instance&) {
    const auto requested = [](const char* key) {
        const char* value = std::getenv(key);
        return value && value[0] == '1';
    };
    if (requested("BB_PC_MODEL") || requested("BB_GUEST_IN_PLACE") || requested("BB_GUEST_GPU_MEMORY")) {
        std::puts("Guest memory: Windows uses pagefile-backed memory; the Linux dma-buf model is unavailable");
    }
}
const Chunk* Find(std::uint64_t) { return nullptr; }
}

namespace BbGuestHooks {
void Install(RangeCallback) {}
}

namespace BbGnmHooks {
void PatchImage(unsigned char*, std::uint64_t) {}
void CheckSubmission(const std::uint32_t*, std::uint64_t) {}
DriverWrite::~DriverWrite() = default;
}

namespace BbHeapSites {
void Install() {}
void Report() {}
void NoteReleaseCheck(unsigned) {}
void InstallCounters() {}
bool Counting() { return false; }
long long LiveAllocations() { return 0; }
}

namespace BbFreeCheck {
bool Enabled() { return false; }
void Check(std::uint64_t, std::uint64_t, const void*, Source, std::uint64_t) {}
std::uint64_t NextFenceSeq() { return 0; }
void NoteFenceDecoded(std::uint64_t, std::uint64_t, const void*, const void*, std::uint64_t) {}
void NoteFenceWriting(std::uint64_t) {}
void NoteFenceWritten(std::uint64_t, std::uint64_t) {}
bool OnTrapFault(void*, std::uint64_t) { return false; }
bool OnStaleTrapFault(std::uint64_t) { return false; }
void NoteSubmit(std::uint64_t, const void*, std::uint64_t) {}
void DumpAtFault(std::uint64_t, std::uint64_t) {}
}
