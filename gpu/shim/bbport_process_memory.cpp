// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: the process's own memory, for the once-a-minute memory line in the log.
#include "bbport_process_memory.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#else
#include <cstdio>
#include <unistd.h>
#endif

namespace BbProcessMemory {

Usage Get() {
    Usage usage{};
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    if (K32GetProcessMemoryInfo(GetCurrentProcess(),
                                reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                                sizeof(counters))) {
        usage.private_bytes = counters.PrivateUsage;
        usage.working_set = counters.WorkingSetSize;
    }
#else
    if (std::FILE* f = std::fopen("/proc/self/statm", "r")) {
        unsigned long long size = 0, resident = 0;
        if (std::fscanf(f, "%llu %llu", &size, &resident) == 2) {
            const unsigned long long page = static_cast<unsigned long long>(sysconf(_SC_PAGESIZE));
            usage.private_bytes = size * page;
            usage.working_set = resident * page;
        }
        std::fclose(f);
    }
#endif
    return usage;
}

} // namespace BbProcessMemory
