// SPDX-License-Identifier: GPL-2.0-or-later
// bbport BB_HEAP_SITES=1 (diagnostics): the game's malloc replacement (the pointers libc.prx calls
// through, libc+0xba190: malloc, free, calloc, realloc, memalign, posix_memalign) is wrapped, and
// the live allocations are counted per call site in the game (the first two return addresses into
// its code on the stack). Report() prints the sites whose live bytes grew since the last report:
// what leaks when the guest heap runs out.
#include "bbport_heap_sites.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace BbHeapSites {
namespace {
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using s64 = std::int64_t;

constexpr u64 ImageBase = 0x800000000ull;
constexpr u64 TextEnd = ImageBase + 0x50d96dcull;
constexpr u64 LibcBase = ImageBase + 0x56e0000ull;
constexpr u64 SlotMalloc = 0xba190, SlotFree = 0xba198, SlotCalloc = 0xba1a0,
              SlotRealloc = 0xba1a8, SlotMemalign = 0xba1b0, SlotPosixMemalign = 0xba1b8;

using MallocFn = void* (*)(std::size_t);
using FreeFn = void (*)(void*);
using CallocFn = void* (*)(std::size_t, std::size_t);
using ReallocFn = void* (*)(void*, std::size_t);
using MemalignFn = void* (*)(std::size_t, std::size_t);
using PosixMemalignFn = int (*)(void**, std::size_t, std::size_t);

MallocFn orig_malloc;
FreeFn orig_free;
CallocFn orig_calloc;
ReallocFn orig_realloc;
MemalignFn orig_memalign;
PosixMemalignFn orig_posix_memalign;

struct Site {
    u64 a = 0, b = 0; ///< return addresses into the game (image offsets); a == 0: empty slot
    s64 live = 0, bytes = 0;
    u64 allocs = 0;
    s64 reported_bytes = 0, reported_live = 0;
    u64 free_chain[5] = {}; ///< where an allocation of this site was freed last
};
constexpr u64 SiteCount = 1u << 14;
std::vector<Site> sites;

struct Alloc {
    u64 ptr; ///< 0 empty, 1 removed
    u32 site;
    u32 size;
};
constexpr u64 AllocBits = 22; // 4M live allocations
std::vector<Alloc> allocs;
u64 untracked_frees = 0, table_full = 0;

std::atomic_flag lock = ATOMIC_FLAG_INIT;
bool tracking = false; ///< BB_HEAP_SITES=1: per call site; else only the counters below
std::atomic<u64> count_allocs{0}, count_frees{0};
std::atomic<u64> release_ok{0}, release_busy{0}, release_missing{0}, release_other{0};
struct Guard {
    Guard() {
        while (lock.test_and_set(std::memory_order_acquire)) {
            __builtin_ia32_pause();
        }
    }
    ~Guard() {
        lock.clear(std::memory_order_release);
    }
};

u64 Hash(u64 v, u64 bits) {
    return (v * 0x9E3779B97F4A7C15ull) >> (64 - bits);
}

/// The first return addresses into the game's code above `frame` (up to `count`).
void CallChain(const u64* frame, u64* out, int count) {
    int n = 0;
    for (int i = 0; i < 160 && n < count; ++i) {
        const u64 v = frame[i];
        if (v > ImageBase && v < TextEnd) {
            out[n++] = v - ImageBase;
        }
    }
    for (; n < count; ++n) {
        out[n] = 0;
    }
}
void CallSites(const u64* frame, u64& a, u64& b) {
    u64 chain[2];
    CallChain(frame, chain, 2);
    a = chain[0];
    b = chain[1];
}

u32 SiteIndex(u64 a, u64 b) {
    const u64 key = a * 31 + b + 1;
    for (u64 i = Hash(key, 14), n = 0; n < SiteCount; ++n, i = (i + 1) & (SiteCount - 1)) {
        Site& site = sites[i];
        if (site.a == a + 1 && site.b == b) {
            return u32(i);
        }
        if (site.a == 0) {
            site.a = a + 1;
            site.b = b;
            return u32(i);
        }
    }
    return 0;
}

void Add(void* ptr, std::size_t size, const u64* frame) {
    u64 a, b;
    CallSites(frame, a, b);
    Guard guard;
    const u32 index = SiteIndex(a, b);
    Site& site = sites[index];
    ++site.live;
    site.bytes += s64(size);
    ++site.allocs;
    const u64 p = reinterpret_cast<u64>(ptr);
    const u64 mask = (1ull << AllocBits) - 1;
    for (u64 i = Hash(p, AllocBits), n = 0; n < 64; ++n, i = (i + 1) & mask) {
        if (allocs[i].ptr <= 1) {
            allocs[i] = {p, index, u32(std::min<std::size_t>(size, 0xffffffffu))};
            return;
        }
    }
    ++table_full;
}

void Remove(void* ptr, const u64* frame) {
    u64 chain[5];
    CallChain(frame, chain, 5);
    const u64 p = reinterpret_cast<u64>(ptr);
    const u64 mask = (1ull << AllocBits) - 1;
    Guard guard;
    for (u64 i = Hash(p, AllocBits), n = 0; n < 64; ++n, i = (i + 1) & mask) {
        if (allocs[i].ptr == p) {
            Site& site = sites[allocs[i].site];
            --site.live;
            std::copy_n(chain, 5, site.free_chain);
            site.bytes -= s64(allocs[i].size);
            allocs[i].ptr = 1;
            return;
        }
        if (allocs[i].ptr == 0) {
            break;
        }
    }
    ++untracked_frees;
}

void* WrapMalloc(std::size_t size) {
    void* p = orig_malloc(size);
    if (p && (count_allocs.fetch_add(1, std::memory_order_relaxed), tracking)) {
        Add(p, size, static_cast<const u64*>(__builtin_frame_address(0)));
    }
    return p;
}
void WrapFree(void* p) {
    if (p && (count_frees.fetch_add(1, std::memory_order_relaxed), tracking)) {
        Remove(p, static_cast<const u64*>(__builtin_frame_address(0)));
    }
    orig_free(p);
}
void* WrapCalloc(std::size_t count, std::size_t size) {
    void* p = orig_calloc(count, size);
    if (p && (count_allocs.fetch_add(1, std::memory_order_relaxed), tracking)) {
        Add(p, count * size, static_cast<const u64*>(__builtin_frame_address(0)));
    }
    return p;
}
void* WrapRealloc(void* old, std::size_t size) {
    void* p = orig_realloc(old, size);
    if (!old && p) {
        count_allocs.fetch_add(1, std::memory_order_relaxed);
    } else if (old && !p && size == 0) {
        count_frees.fetch_add(1, std::memory_order_relaxed);
    }
    if (tracking && (p || size == 0)) {
        if (old) {
            Remove(old, static_cast<const u64*>(__builtin_frame_address(0)));
        }
        if (p) {
            Add(p, size, static_cast<const u64*>(__builtin_frame_address(0)));
        }
    }
    return p;
}
void* WrapMemalign(std::size_t alignment, std::size_t size) {
    void* p = orig_memalign(alignment, size);
    if (p && (count_allocs.fetch_add(1, std::memory_order_relaxed), tracking)) {
        Add(p, size, static_cast<const u64*>(__builtin_frame_address(0)));
    }
    return p;
}
int WrapPosixMemalign(void** out, std::size_t alignment, std::size_t size) {
    const int result = orig_posix_memalign(out, alignment, size);
    if (result == 0 && out && *out &&
        (count_allocs.fetch_add(1, std::memory_order_relaxed), tracking)) {
        Add(*out, size, static_cast<const u64*>(__builtin_frame_address(0)));
    }
    return result;
}

bool InGame(u64 v) {
    return v >= ImageBase && v < TextEnd;
}
} // namespace

namespace {
void InstallWrappers(bool counters) {
    static const bool wanted = [] {
        const char* env = std::getenv("BB_HEAP_SITES");
        return env && env[0] == '1';
    }();
    static bool done = false;
    if ((!wanted && !counters) || done) {
        return;
    }
    auto* slots = reinterpret_cast<u64*>(LibcBase);
    if (slots[SlotMalloc / 8] == 0) {
        return; // libc.prx has not set up the game's malloc yet: tried again later
    }
    done = true;
    const u64 targets[6] = {slots[SlotMalloc / 8], slots[SlotFree / 8], slots[SlotCalloc / 8],
                            slots[SlotRealloc / 8], slots[SlotMemalign / 8],
                            slots[SlotPosixMemalign / 8]};
    for (const u64 target : targets) {
        if (!InGame(target)) {
            std::printf("Heap sites: libc's malloc table not as expected (%#llx); not installed\n",
                        (unsigned long long)target);
            return;
        }
    }
    if (wanted) {
        sites.resize(SiteCount);
        allocs.resize(1ull << AllocBits);
        tracking = true;
    }
    orig_malloc = reinterpret_cast<MallocFn>(targets[0]);
    orig_free = reinterpret_cast<FreeFn>(targets[1]);
    orig_calloc = reinterpret_cast<CallocFn>(targets[2]);
    orig_realloc = reinterpret_cast<ReallocFn>(targets[3]);
    orig_memalign = reinterpret_cast<MemalignFn>(targets[4]);
    orig_posix_memalign = reinterpret_cast<PosixMemalignFn>(targets[5]);
    __atomic_store_n(&slots[SlotMalloc / 8], reinterpret_cast<u64>(&WrapMalloc), __ATOMIC_SEQ_CST);
    __atomic_store_n(&slots[SlotFree / 8], reinterpret_cast<u64>(&WrapFree), __ATOMIC_SEQ_CST);
    __atomic_store_n(&slots[SlotCalloc / 8], reinterpret_cast<u64>(&WrapCalloc), __ATOMIC_SEQ_CST);
    __atomic_store_n(&slots[SlotRealloc / 8], reinterpret_cast<u64>(&WrapRealloc), __ATOMIC_SEQ_CST);
    __atomic_store_n(&slots[SlotMemalign / 8], reinterpret_cast<u64>(&WrapMemalign),
                     __ATOMIC_SEQ_CST);
    __atomic_store_n(&slots[SlotPosixMemalign / 8], reinterpret_cast<u64>(&WrapPosixMemalign),
                     __ATOMIC_SEQ_CST);
    std::printf("Heap sites: the game's malloc (+%#llx) and free (+%#llx) are counted%s\n",
                (unsigned long long)(targets[0] - ImageBase),
                (unsigned long long)(targets[1] - ImageBase), tracking ? " per call site" : "");
}
} // namespace

void Install() {
    InstallWrappers(false);
}

void InstallCounters() {
    InstallWrappers(true);
}

bool Counting() {
    return orig_malloc != nullptr;
}

long long LiveAllocations() {
    return s64(count_allocs.load(std::memory_order_relaxed)) -
           s64(count_frees.load(std::memory_order_relaxed));
}

void NoteReleaseCheck(unsigned result) {
    if (s64(std::int32_t(result)) >= 0) {
        ++release_ok;
    } else if (result == 0x80004005u) {
        ++release_busy;
    } else if (result == 0x80000003u) {
        ++release_missing;
    } else {
        ++release_other;
    }
}

void Report() {
    Install();
    if (sites.empty()) {
        return;
    }
    std::printf("Heap sites: release checks %llu ok, %llu manager busy, %llu not registered, %llu "
                "other\n",
                (unsigned long long)release_ok.exchange(0), (unsigned long long)release_busy.exchange(0),
                (unsigned long long)release_missing.exchange(0),
                (unsigned long long)release_other.exchange(0));
    struct Row {
        u64 a, b;
        s64 grew, live_grew, live, bytes;
        u64 allocs;
        u64 free_chain[5];
    };
    std::vector<Row> rows;
    s64 total_bytes = 0, total_live = 0;
    u64 frees_unknown, full;
    {
        Guard guard;
        for (auto& site : sites) {
            if (!site.a) {
                continue;
            }
            total_bytes += site.bytes;
            total_live += site.live;
            if (site.bytes != site.reported_bytes || site.live != site.reported_live) {
                rows.push_back({site.a - 1, site.b, site.bytes - site.reported_bytes,
                                site.live - site.reported_live, site.live, site.bytes, site.allocs,
                                {site.free_chain[0], site.free_chain[1], site.free_chain[2],
                                 site.free_chain[3], site.free_chain[4]}});
            }
            site.reported_bytes = site.bytes;
            site.reported_live = site.live;
        }
        frees_unknown = untracked_frees;
        full = table_full;
    }
    std::sort(rows.begin(), rows.end(), [](const Row& x, const Row& y) { return x.grew > y.grew; });
    std::printf("Heap sites: %.1f MB live in %lld allocations (%llu frees of untracked, %llu not "
                "stored); grew most:",
                total_bytes / 1e6, (long long)total_live, (unsigned long long)frees_unknown,
                (unsigned long long)full);
    for (std::size_t i = 0; i < std::min<std::size_t>(rows.size(), 8); ++i) {
        const auto& r = rows[i];
        if (r.grew <= 0) {
            break;
        }
        std::printf("%s +%#llx<+%#llx %+.1f KB %+lld (live %.1f KB in %lld, %llu allocs; freed at "
                    "+%#llx<+%#llx<+%#llx<+%#llx<+%#llx)",
                    i ? ";" : "", (unsigned long long)r.a, (unsigned long long)r.b, r.grew / 1e3,
                    (long long)r.live_grew, r.bytes / 1e3, (long long)r.live,
                    (unsigned long long)r.allocs, (unsigned long long)r.free_chain[0],
                    (unsigned long long)r.free_chain[1], (unsigned long long)r.free_chain[2],
                    (unsigned long long)r.free_chain[3], (unsigned long long)r.free_chain[4]);
    }
    std::printf("\n");
}
} // namespace BbHeapSites
