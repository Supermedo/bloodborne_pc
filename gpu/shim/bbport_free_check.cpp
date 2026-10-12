// SPDX-License-Identifier: GPL-2.0-or-later
#include "bbport_free_check.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <csignal>
#include <ucontext.h>
#include <unordered_set>
#include <sys/uio.h>
#include <unistd.h>

namespace BbFreeCheck {
namespace {
using u64 = std::uint64_t;
using u32 = std::uint32_t;

// The game's small-object pool (allocation at guest offset 0x263b8b0, release at 0x263b9b0).
constexpr u64 PageSize = 0x1000;
constexpr u64 HeaderOffset = 0xfc0;
constexpr u64 BlockSize = 0x30;
constexpr u64 MaxBlocks = HeaderOffset / BlockSize;

const char* const SourceNames[] = {"EOP fence",  "EOS fence",   "EOS GDS store", "WriteData",
                                   "compute WriteData", "ReleaseMem", "DumpConstRam",
                                   "DMA fill",   "DMA copy",    "buffer download",
                                   "image download"};

const char* Name(u32 source) {
    return source < std::size(SourceNames) ? SourceNames[source] : "?";
}

struct Header {
    u64 next, prev, head, count;
};

bool IsBlock(u64 page, u64 p) {
    return p >= page && p - page < HeaderOffset && (p - page) % BlockSize == 0;
}

bool Plausible(u64 page, const Header& h) {
    const auto pointer = [](u64 p) { return p != 0 && (p & 7) == 0 && p < (1ull << 47); };
    // A page with every block free has gone back to the allocator below (the release at
    // 0x263b9b0): its old header means nothing. The page list links point at other page
    // headers or at the pool's list heads; at least one of them is a header (pools of one
    // page are skipped: other data at a page end looked like headers).
    if (!pointer(h.next) || !pointer(h.prev) || h.count >= MaxBlocks) {
        return false;
    }
    if ((h.next & (PageSize - 1)) != HeaderOffset && (h.prev & (PageSize - 1)) != HeaderOffset) {
        return false;
    }
    return h.count == 0 ? h.head == 0 : IsBlock(page, h.head);
}

struct WalkResult {
    bool ok;
    u64 holder;   ///< where the bad pointer is stored (a free block, or the header's head)
    u64 value;    ///< the bad pointer
    u32 length;   ///< free blocks walked
    u64 free[2];  ///< bit per block index: free
};

bool IsFree(const u64 (&free)[2], u64 index) {
    return (free[index / 64] >> (index % 64)) & 1;
}

/// Walks the free list of `page`; `read(address)` returns the qword there.
template <typename Read>
WalkResult WalkFree(u64 page, const Header& h, Read&& read) {
    WalkResult r{true, page + HeaderOffset + 16, h.head, 0, {0, 0}};
    u64 node = h.head;
    while (node != 0) {
        if (!IsBlock(page, node) || r.length >= h.count) {
            r.ok = false;
            r.value = node;
            return r;
        }
        const u64 index = (node - page) / BlockSize;
        if (IsFree(r.free, index)) {
            r.ok = false; // a loop
            r.value = node;
            return r;
        }
        r.free[index / 64] |= 1ull << (index % 64);
        ++r.length;
        r.holder = node;
        node = read(node);
    }
    if (r.length != h.count) {
        r.ok = false;
        r.value = 0;
    }
    return r;
}

u64 NowNs() {
    static const auto start = std::chrono::steady_clock::now();
    return u64(std::chrono::duration_cast<std::chrono::nanoseconds>(
                   std::chrono::steady_clock::now() - start)
                   .count());
}

u32 Tid() {
    static thread_local const u32 tid = u32(gettid());
    return tid;
}

// Recent emulator writes into pool pages.
struct Entry {
    u64 address, size, value, ns, seq;
    u32 source, tid;
};
std::array<Entry, 1 << 15> ring;
std::atomic<u64> ring_head{0};

void Record(u64 address, u64 size, u64 value, Source source, u64 seq) {
    ring[ring_head.fetch_add(1, std::memory_order_relaxed) % ring.size()] =
        Entry{address, size, value, NowNs(), seq, u32(source), Tid()};
}

// Pool pages the watcher walks: an open-addressing set, insert-only.
std::array<std::atomic<u64>, 1 << 14> pages{};
std::atomic<u32> page_count{0};

void Watch(u64 page) {
    static thread_local u64 last = 0;
    if (page == last) {
        return;
    }
    last = page;
    u64 slot = (page >> 12) * 0x9E3779B97F4A7C15ull >> 50;
    for (u32 probe = 0; probe < 64; ++probe, slot = (slot + 1) % pages.size()) {
        u64 expected = pages[slot].load(std::memory_order_relaxed);
        if (expected == page) {
            return;
        }
        if (expected == 0) {
            if (pages[slot].compare_exchange_strong(expected, page) || expected == page) {
                if (expected != page) {
                    page_count.fetch_add(1, std::memory_order_relaxed);
                }
                return;
            }
        }
    }
}

std::atomic<u64> checked{0};
std::atomic<u64> free_hits{0};
std::atomic<u64> header_hits{0};
std::atomic<u64> corruptions{0};
std::atomic<u64> unordered{0};
std::atomic<u64> next_seq{0};
std::atomic<u64> max_written_seq{0};

// Fences as the GPU command thread decoded them, and the guest's graphics submissions.
struct Decoded {
    u64 label, value, packet, buffer, submit, ns;
};
std::array<Decoded, 1 << 15> decoded;
std::atomic<u64> decoded_head{0};
struct Submitted {
    u64 submit, buffer, size, ns;
};
std::array<Submitted, 1 << 12> submitted;
std::atomic<u64> submitted_head{0};

/// When the guest submitted `submit` (0 when no longer in the log).
u64 SubmitTime(u64 submit) {
    const u64 n = submitted_head.load(std::memory_order_relaxed);
    for (u64 i = n; i-- > (n > submitted.size() ? n - submitted.size() : 0);) {
        const Submitted s = submitted[i % submitted.size()];
        if (s.submit == submit) {
            return s.ns;
        }
    }
    return 0;
}

/// The decoded fences whose label lies in [begin, end), newest first.
void PrintDecoded(u64 begin, u64 end, u64 now, u32 limit) {
    const u64 n = decoded_head.load(std::memory_order_relaxed);
    u32 shown = 0;
    for (u64 i = n; i-- > (n > decoded.size() ? n - decoded.size() : 0) && shown < limit;) {
        const Decoded d = decoded[i % decoded.size()];
        if (d.label < begin || d.label >= end) {
            continue;
        }
        const u64 submit_ns = SubmitTime(d.submit);
        std::fprintf(stderr,
                     "Free check:   %8.3f ms before: decoded fence %#llx value %#llx, packet %#llx "
                     "in buffer %#llx, submission %llu (submitted %.3f ms before)\n",
                     double(now - d.ns) / 1e6, (unsigned long long)d.label,
                     (unsigned long long)d.value, (unsigned long long)d.packet,
                     (unsigned long long)d.buffer, (unsigned long long)d.submit,
                     submit_ns ? double(now - submit_ns) / 1e6 : -1.0);
        ++shown;
    }
    if (shown == 0) {
        std::fprintf(stderr, "Free check:   no decoded fence there in the log\n");
    }
}

/// Each kind of report has its own budget: a flood of one does not hide the others.
struct Budget {
    std::atomic<u32> used{0};
    u32 limit;
    bool Take() {
        return used.fetch_add(1, std::memory_order_relaxed) < limit;
    }
};
Budget write_reports{.limit = 64};
Budget corruption_reports{.limit = 64};
Budget order_reports{.limit = 32};
Budget latency_reports{.limit = 32};
Budget duplicate_reports{.limit = 32};

// Fence latency and duplicates: when each fence reached the draw recording thread (NextFenceSeq)
// and which sequence number each slot last wrote. A guest that waits for a label with a
// timeout frees its object when the label is late; a fence written twice lands late too.
constexpr u64 SeqSlots = 1 << 16;
std::array<std::atomic<u64>, SeqSlots> seq_start_ns{};
std::array<std::atomic<u64>, SeqSlots> seq_written{};
std::atomic<u64> duplicates{0}, late_20ms{0}, late_50ms{0}, max_latency_ns{0};

void PrintEntry(const Entry& e, u64 now) {
    std::fprintf(stderr,
                 "Free check:   %8.3f ms before: %s %#llx +%llu value %#llx seq %llu tid %u\n",
                 double(now - e.ns) / 1e6, Name(e.source), (unsigned long long)e.address,
                 (unsigned long long)e.size, (unsigned long long)e.value,
                 (unsigned long long)e.seq, e.tid);
}

/// The logged writes overlapping [begin, end), newest first.
void PrintRecent(u64 begin, u64 end, u64 now, u32 limit) {
    const u64 n = ring_head.load(std::memory_order_relaxed);
    u32 shown = 0;
    for (u64 i = n; i-- > (n > ring.size() ? n - ring.size() : 0) && shown < limit;) {
        const Entry e = ring[i % ring.size()];
        if (e.address + e.size <= begin || e.address >= end) {
            continue;
        }
        PrintEntry(e, now);
        ++shown;
    }
    if (shown == 0) {
        std::fprintf(stderr, "Free check:   no emulator writes there in the log\n");
    }
}

void PrintBlock(const unsigned char* bytes, u64 page, u64 block) {
    for (u64 at = block; at < block + BlockSize && at + 8 <= page + PageSize; at += 8) {
        u64 v;
        std::memcpy(&v, bytes + (at - page), 8);
        std::fprintf(stderr, "Free check:   [%#llx] = %#llx\n", (unsigned long long)at,
                     (unsigned long long)v);
    }
}

bool ReadPage(u64 page, unsigned char* out) {
    iovec local{out, PageSize};
    iovec remote{reinterpret_cast<void*>(page), PageSize};
    return process_vm_readv(getpid(), &local, 1, &remote, 1, 0) == ssize_t(PageSize);
}

/// What the watcher saw of a page's blocks: when each was last seen taken and released by the
/// game (to ~1 ms).
struct Track {
    bool valid = false;
    u64 free[2]{};
    std::array<u64, MaxBlocks> allocated_at{}, freed_at{};
};

void Watcher() {
    pthread_setname_np(pthread_self(), "bbFreeCheck");
    std::array<u64, 256> reported{};
    u32 reported_next = 0;
    std::unordered_map<u64, Track> tracks;
    alignas(64) unsigned char buf[PageSize];
    const auto walk_page = [&](u64 page, Header& h, WalkResult& r) {
        if (!ReadPage(page, buf)) {
            return false;
        }
        std::memcpy(&h, buf + HeaderOffset, sizeof(h));
        if (!Plausible(page, h)) {
            return false;
        }
        r = WalkFree(page, h, [&](u64 at) {
            u64 v;
            std::memcpy(&v, buf + (at - page), 8);
            return v;
        });
        return true;
    };
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        static u64 last_report = 0, last_l20 = 0, last_l50 = 0, last_dup = 0;
        if (const u64 now = NowNs(); now - last_report > 10000000000ull) {
            const u64 l20 = late_20ms.load(), l50 = late_50ms.load(), dup = duplicates.load();
            if (last_report) {
                std::fprintf(stderr,
                             "Free check: fences in 10 s: %llu over 20 ms, %llu over 50 ms, %llu "
                             "written twice (max latency so far %.1f ms)\n",
                             (unsigned long long)(l20 - last_l20),
                             (unsigned long long)(l50 - last_l50),
                             (unsigned long long)(dup - last_dup), max_latency_ns.load() / 1e6);
            }
            last_report = now;
            last_l20 = l20;
            last_l50 = l50;
            last_dup = dup;
        }
        for (auto& slot : pages) {
            const u64 page = slot.load(std::memory_order_relaxed);
            if (page == 0) {
                continue;
            }
            Header h;
            WalkResult r;
            if (!walk_page(page, h, r)) {
                tracks.erase(page);
                continue;
            }
            if (r.ok) {
                const u64 now = NowNs();
                Track& t = tracks[page];
                for (u64 i = 0; i < MaxBlocks; ++i) {
                    const bool was = IsFree(t.free, i), is = IsFree(r.free, i);
                    if (t.valid && was != is) {
                        (is ? t.freed_at : t.allocated_at)[i] = now;
                    }
                }
                t.free[0] = r.free[0];
                t.free[1] = r.free[1];
                t.valid = true;
                continue;
            }
            // The game may be inside an allocation or release: the same damage three times
            // over ~1 ms is not a race with it.
            bool persistent = true;
            for (int again = 0; again < 3 && persistent; ++again) {
                std::this_thread::sleep_for(std::chrono::microseconds(300));
                Header h2;
                WalkResult r2;
                persistent = walk_page(page, h2, r2) && !r2.ok && r2.holder == r.holder &&
                             r2.value == r.value;
            }
            if (!persistent) {
                continue;
            }
            const u64 key = r.holder ^ (r.value * 0x9E3779B97F4A7C15ull);
            if (std::find(reported.begin(), reported.end(), key) != reported.end()) {
                continue;
            }
            reported[reported_next++ % reported.size()] = key;
            corruptions.fetch_add(1, std::memory_order_relaxed);
            if (!corruption_reports.Take()) {
                continue;
            }
            const u64 now = NowNs();
            std::fprintf(stderr,
                         "Free check: CORRUPTED free list in pool page %#llx at %.3f s: "
                         "%#llx holds %#llx (%u of %llu free blocks walked; header next %#llx "
                         "prev %#llx head %#llx count %llu)\n",
                         (unsigned long long)page, double(now) / 1e9,
                         (unsigned long long)r.holder, (unsigned long long)r.value, r.length,
                         (unsigned long long)h.count, (unsigned long long)h.next,
                         (unsigned long long)h.prev, (unsigned long long)h.head,
                         (unsigned long long)h.count);
            if (r.holder < page + HeaderOffset) {
                PrintBlock(buf, page, r.holder);
                const u64 index = (r.holder - page) / BlockSize;
                if (const auto it = tracks.find(page); it != tracks.end()) {
                    const Track& t = it->second;
                    const auto ago = [&](u64 at) { return at ? double(now - at) / 1e6 : -1.0; };
                    std::fprintf(stderr,
                                 "Free check:   block %#llx: last taken %.3f ms ago, last "
                                 "released %.3f ms ago (-1: not seen), free in the last good "
                                 "walk: %s\n",
                                 (unsigned long long)r.holder, ago(t.allocated_at[index]),
                                 ago(t.freed_at[index]), IsFree(t.free, index) ? "yes" : "no");
                }
                std::fprintf(stderr, "Free check:   emulator writes into this block:\n");
                PrintRecent(r.holder, r.holder + BlockSize, now, 24);
                std::fprintf(stderr, "Free check:   fences decoded for this block:\n");
                PrintDecoded(r.holder, r.holder + BlockSize, now, 16);
            }
            std::fprintf(stderr, "Free check:   emulator writes into the page:\n");
            PrintRecent(page, page + PageSize, now, 32);
            std::fprintf(stderr, "Free check:   fences written out of stream order so far: "
                                 "%llu\n",
                         (unsigned long long)unordered.load());
        }
    }
}

void StartWatcher() {
    static std::once_flag once;
    std::call_once(once, [] {
        std::fprintf(stderr, "Free check: on (BB_FREE_CHECK=1): pool pages of %llu-byte "
                             "blocks, header at +%#llx\n",
                     (unsigned long long)BlockSize, (unsigned long long)HeaderOffset);
        std::thread(Watcher).detach();
    });
}

/// One page of a write: report a write into a free block or the header.
void CheckPage(u64 page, u64 begin, u64 end, u64 value, u64 size, Source source, u64 seq,
               bool small) {
    Header h;
    std::memcpy(&h, reinterpret_cast<const void*>(page + HeaderOffset), sizeof(h));
    if (!Plausible(page, h)) {
        return;
    }
    if (small) {
        Watch(page);
    }
    Record(begin, end - begin, value, source, seq);
    const auto report = [&](const char* what, u64 block) {
        if (!write_reports.Take()) {
            return;
        }
        const u64 now = NowNs();
        std::fprintf(stderr,
                     "Free check: %s write %#llx +%llu (of %llu) value %#llx seq %llu into %s "
                     "%#llx of pool page %#llx (free %llu), tid %u, at %.3f s\n",
                     Name(source), (unsigned long long)begin, (unsigned long long)(end - begin),
                     (unsigned long long)size, (unsigned long long)value,
                     (unsigned long long)seq, what, (unsigned long long)block,
                     (unsigned long long)page, (unsigned long long)h.count, Tid(),
                     double(now) / 1e9);
        if (block < page + HeaderOffset) {
            PrintBlock(reinterpret_cast<const unsigned char*>(page), page, block);
            PrintRecent(block, block + BlockSize, now, 16);
            PrintDecoded(block, block + BlockSize, now, 8);
        } else {
            PrintRecent(page, page + PageSize, now, 16);
        }
    };
    if (end > page + HeaderOffset) {
        header_hits.fetch_add(1, std::memory_order_relaxed);
        report("the HEADER", page + HeaderOffset);
        return;
    }
    // The game changes the list all the time (labels are taken and released in bursts): a
    // walk that met it halfway is repeated.
    WalkResult r{};
    for (int attempt = 0; attempt < 8; ++attempt) {
        std::memcpy(&h, reinterpret_cast<const void*>(page + HeaderOffset), sizeof(h));
        if (!Plausible(page, h)) {
            return;
        }
        r = WalkFree(page, h, [](u64 at) {
            u64 v;
            std::memcpy(&v, reinterpret_cast<const void*>(at), 8);
            return v;
        });
        if (r.ok) {
            break;
        }
    }
    if (!r.ok) {
        return; // still changing, or damaged: the watcher reports damage
    }
    for (u64 block = page + (begin - page) / BlockSize * BlockSize; block < end;
         block += BlockSize) {
        if (IsFree(r.free, (block - page) / BlockSize)) {
            free_hits.fetch_add(1, std::memory_order_relaxed);
            report("FREE block", block);
            return;
        }
    }
}
} // namespace

bool Enabled() {
    static const bool enabled = [] {
        const char* env = std::getenv("BB_FREE_CHECK");
        return env && env[0] == '1';
    }();
    return enabled;
}

namespace {
bool TrapEnabled();
void NoteTrapDecoded(u64 label);
void InstallStepTrap();
bool InstallFreeHook();
} // namespace

void NoteFenceDecoded(u64 label, u64 value, const void* packet, const void* buffer, u64 submit) {
    if (!Enabled()) {
        return;
    }
    decoded[decoded_head.fetch_add(1, std::memory_order_relaxed) % decoded.size()] =
        Decoded{label, value, reinterpret_cast<u64>(packet), reinterpret_cast<u64>(buffer), submit,
                NowNs()};
    if (TrapEnabled() && label != 0) {
        NoteTrapDecoded(label);
    }
}

void NoteSubmit(u64 submit, const void* buffer, u64 size) {
    if (!Enabled()) {
        return;
    }
    submitted[submitted_head.fetch_add(1, std::memory_order_relaxed) % submitted.size()] =
        Submitted{submit, reinterpret_cast<u64>(buffer), size, NowNs()};
}

u64 NextFenceSeq() {
    if (!Enabled()) {
        return 0;
    }
    const u64 seq = next_seq.fetch_add(1, std::memory_order_relaxed) + 1;
    seq_start_ns[seq % SeqSlots].store(NowNs(), std::memory_order_relaxed);
    return seq;
}

void Check(u64 address, u64 size, const void* data, Source source, u64 seq) {
    if (!Enabled() || size == 0 || address == 0) {
        return;
    }
    StartWatcher();
    checked.fetch_add(1, std::memory_order_relaxed);
    u64 value = 0;
    if (data) {
        std::memcpy(&value, data, size < 8 ? size : 8);
    }
    if (seq != 0) {
        const u64 now = NowNs();
        const u64 start = seq_start_ns[seq % SeqSlots].load(std::memory_order_relaxed);
        const u64 latency = start && now > start ? now - start : 0;
        u64 max_seen = max_latency_ns.load(std::memory_order_relaxed);
        while (latency > max_seen && !max_latency_ns.compare_exchange_weak(max_seen, latency)) {
        }
        if (latency > 20000000) {
            late_20ms.fetch_add(1, std::memory_order_relaxed);
        }
        // Honest labels (BB_GUEST_IN_PLACE, BB_HONEST_LABELS) are written once the GPU has done the
        // work: tens of ms after the recording thread are normal there; only stalls are reported.
        static const u64 report_ns = [] {
            const char* in_place = std::getenv("BB_GUEST_IN_PLACE");
            const char* honest = std::getenv("BB_HONEST_LABELS");
            return (in_place && in_place[0] == '1') || (honest && honest[0] == '1') ? 500000000ull
                                                                                      : 50000000ull;
        }();
        if (latency > 50000000) {
            late_50ms.fetch_add(1, std::memory_order_relaxed);
        }
        if (latency > report_ns) {
            if (latency_reports.Take()) {
                std::fprintf(stderr,
                             "Free check: LATE fence: %s seq %llu at %#llx written %.1f ms after "
                             "the draw recording thread reached it, tid %u, at %.3f s\n",
                             Name(source), (unsigned long long)seq, (unsigned long long)address,
                             latency / 1e6, Tid(), now / 1e9);
            }
        }
        if (seq_written[seq % SeqSlots].exchange(seq, std::memory_order_relaxed) == seq) {
            duplicates.fetch_add(1, std::memory_order_relaxed);
            if (duplicate_reports.Take()) {
                std::fprintf(stderr,
                             "Free check: DUPLICATE fence: %s seq %llu at %#llx (value %#llx) "
                             "written again, tid %u, at %.3f s\n",
                             Name(source), (unsigned long long)seq, (unsigned long long)address,
                             (unsigned long long)value, Tid(), now / 1e9);
            }
        }
        u64 seen = max_written_seq.load(std::memory_order_relaxed);
        while (seen < seq && !max_written_seq.compare_exchange_weak(seen, seq)) {
        }
        if (seen > seq) {
            unordered.fetch_add(1, std::memory_order_relaxed);
            if (order_reports.Take()) {
                std::fprintf(stderr,
                             "Free check: OUT OF ORDER: %s seq %llu at %#llx (value %#llx) "
                             "written after seq %llu, tid %u, at %.3f s\n",
                             Name(source), (unsigned long long)seq,
                             (unsigned long long)address, (unsigned long long)value,
                             (unsigned long long)seen, Tid(), double(NowNs()) / 1e9);
            }
        }
    }
    const bool small = size <= 64;
    const u64 end = address + size;
    for (u64 page = address & ~(PageSize - 1); page < end; page += PageSize) {
        CheckPage(page, std::max(address, page), std::min(end, page + PageSize), value, size,
                  source, seq, small);
    }
}

void DumpAtFault(u64 rax, u64 r14) {
    if (!Enabled()) {
        return;
    }
    std::fprintf(stderr,
                 "Free check: %llu writes checked, %llu into free blocks, %llu into pool "
                 "headers, %llu corrupted lists seen, %llu fences out of order, %u pool pages "
                 "watched\n",
                 (unsigned long long)checked.load(), (unsigned long long)free_hits.load(),
                 (unsigned long long)header_hits.load(), (unsigned long long)corruptions.load(),
                 (unsigned long long)unordered.load(), page_count.load());
    std::fprintf(stderr,
                 "Free check: fences: max latency %.1f ms, %llu over 20 ms, %llu over 50 ms, %llu "
                 "written twice\n",
                 max_latency_ns.load() / 1e6, (unsigned long long)late_20ms.load(),
                 (unsigned long long)late_50ms.load(), (unsigned long long)duplicates.load());
    const u64 page = rax & ~(PageSize - 1);
    std::fprintf(stderr, "Free check: fault in pool page %#llx (r14 %#llx); writes into it:\n",
                 (unsigned long long)page, (unsigned long long)r14);
    PrintRecent(page, page + PageSize, NowNs(), 64);
}
namespace {
/// BB_LABEL_TRAP: fences decoded into pool pages (labels) until their write has landed.
struct Pending {
    u32 count = 0;
    u64 decoded_ns = 0;
    u64 writing_ns = 0; ///< when the newest write started (0: not started)
};
struct LabelTrap {
    std::mutex mutex;
    std::unordered_map<u64, Pending> pending; ///< label qword -> fences not yet landed
    /// Mode 1: pages with pending labels -> how many; the ones made read-only; every page ever
    /// made read-only (a fault that raced with its release, OnStaleTrapFault).
    std::unordered_map<u64, u32> page_pending;
    std::unordered_set<u64> read_only;
    std::unordered_set<u64> ever_trapped;
    std::atomic<u32> reports{0}, slow_reports{0};
    /// Reports whose fence write had started: the guest may have seen it land a moment before
    /// NoteFenceWritten (or the write is stuck): their own budget.
    std::atomic<u32> started_reports{0};
    std::atomic<u64> faults{0}, hits{0};
};
LabelTrap& Trap() {
    static LabelTrap trap;
    return trap;
}
constexpr u64 ImageBase = 0x800000000ull, ImageEnd = 0x810000000ull;
/// The guest allocator's release, and the branch of its GPU range collector (0x26aa860) taken
/// for a block whose label reads 4 (done).
constexpr u64 FreeHook = ImageBase + 0x263b9b0;
constexpr u64 GcHook = ImageBase + 0x26aa986;

/// BB_LABEL_TRAP: 1 pool pages with pending labels read-only (every guest write into them is
/// single-stepped: slow, it hid the bug), 2 breakpoints on the guest allocator's release and on
/// the collector's "done" branch: a label block released, or taken as done, before its fence
/// landed is reported with the guest's view and call chain.
int TrapMode() {
    static const int mode = [] {
        const char* env = std::getenv("BB_LABEL_TRAP");
        const int m = Enabled() && env && (env[0] == '1' || env[0] == '2') ? env[0] - '0' : 0;
        if (m != 0) {
            InstallStepTrap();
            if (m == 2 && !InstallFreeHook()) {
                return 0;
            }
            std::fprintf(stderr, "Free check: label trap on (BB_LABEL_TRAP=%d)\n", m);
        }
        return m;
    }();
    return mode;
}
bool TrapEnabled() {
    return TrapMode() != 0;
}
bool ReadQword(u64 address, u64& value) {
    iovec local{&value, 8}, remote{reinterpret_cast<void*>(address), 8};
    return process_vm_readv(getpid(), &local, 1, &remote, 1, 0) == 8;
}
u32 SignalTid() {
    return u32(syscall(SYS_gettid));
}
void NoteTrapDecoded(u64 label) {
    const u64 page = label & ~(PageSize - 1);
    if (label - page >= HeaderOffset) {
        return;
    }
    Header h;
    iovec local{&h, sizeof(h)}, remote{reinterpret_cast<void*>(page + HeaderOffset), sizeof(h)};
    if (process_vm_readv(getpid(), &local, 1, &remote, 1, 0) != ssize_t(sizeof(h)) ||
        !Plausible(page, h)) {
        return;
    }
    auto& t = Trap();
    std::scoped_lock lk{t.mutex};
    auto& p = t.pending[label & ~7ull];
    if (p.count++ == 0) {
        ++t.page_pending[page];
    }
    p.decoded_ns = NowNs();
    p.writing_ns = 0;
    t.ever_trapped.insert(page);
    if (TrapMode() == 1 && t.read_only.insert(page).second) {
        mprotect(reinterpret_cast<void*>(page), PageSize, PROT_READ);
    }
}
} // namespace

void NoteFenceWriting(u64 label) {
    if (!TrapEnabled() || label == 0) {
        return;
    }
    auto& t = Trap();
    std::scoped_lock lk{t.mutex};
    if (const auto it = t.pending.find(label & ~7ull); it != t.pending.end()) {
        it->second.writing_ns = NowNs();
    }
}

void NoteFenceWritten(u64 label, u64 write_ns) {
    if (!TrapEnabled() || label == 0) {
        return;
    }
    auto& t = Trap();
    if (write_ns > 200000 && t.slow_reports.fetch_add(1, std::memory_order_relaxed) < 32) {
        std::fprintf(stderr, "Free check: SLOW fence write %#llx: %.3f ms, tid %u, at %.3f s\n",
                     (unsigned long long)label, write_ns / 1e6, Tid(), NowNs() / 1e9);
    }
    std::scoped_lock lk{t.mutex};
    const auto it = t.pending.find(label & ~7ull);
    if (it == t.pending.end() || --it->second.count != 0) {
        return;
    }
    t.pending.erase(it);
    const u64 page = label & ~(PageSize - 1);
    if (const auto pp = t.page_pending.find(page); pp != t.page_pending.end() && --pp->second == 0) {
        t.page_pending.erase(pp);
        if (t.read_only.erase(page)) {
            mprotect(reinterpret_cast<void*>(page), PageSize, PROT_READ | PROT_WRITE);
        }
    }
}

namespace {
/// Threads single-stepping the write that faulted on a trapped page (TF set): the page is
/// writable for that one instruction, then read-only again (OnStepTrap). A table instead of
/// thread_local: this library is loaded with dlopen, its TLS is not signal-safe.
struct StepSlot {
    std::atomic<u32> tid{0};
    std::atomic<u64> page{0};
};
std::array<StepSlot, 256> step_slots;
struct sigaction previous_trap_action {};
constexpr u64 TrapFlag = 0x100;

void OnFreeHook(void* ucontext);
void OnGcHook(void* ucontext);

void OnStepTrap(int sig, siginfo_t* info, void* ucontext) {
    // BB_LABEL_TRAP=2: the breakpoints (RIP is past the int3).
    const u64 rip = u64(static_cast<ucontext_t*>(ucontext)->uc_mcontext.gregs[REG_RIP]);
    if (rip == FreeHook + 1) {
        OnFreeHook(ucontext);
        return;
    }
    if (rip == GcHook + 1) {
        OnGcHook(ucontext);
        return;
    }
    const u32 tid = SignalTid();
    for (auto& slot : step_slots) {
        if (slot.tid.load(std::memory_order_acquire) != tid) {
            continue;
        }
        const u64 page = slot.page.exchange(0, std::memory_order_acq_rel);
        slot.tid.store(0, std::memory_order_release);
        static_cast<ucontext_t*>(ucontext)->uc_mcontext.gregs[REG_EFL] &= ~TrapFlag;
        auto& t = Trap();
        std::scoped_lock lk{t.mutex};
        if (page && t.read_only.count(page)) {
            mprotect(reinterpret_cast<void*>(page), PageSize, PROT_READ);
        }
        return;
    }
    // Not a step of ours.
    if (previous_trap_action.sa_flags & SA_SIGINFO) {
        if (previous_trap_action.sa_sigaction) {
            previous_trap_action.sa_sigaction(sig, info, ucontext);
            return;
        }
    } else if (previous_trap_action.sa_handler != SIG_DFL &&
               previous_trap_action.sa_handler != SIG_IGN && previous_trap_action.sa_handler) {
        previous_trap_action.sa_handler(sig);
        return;
    }
    signal(SIGTRAP, SIG_DFL);
    raise(SIGTRAP);
}

/// int3 over the first byte of an instruction the hook then performs itself.
bool PatchInt3(u64 address, std::initializer_list<unsigned char> expected) {
    unsigned char bytes[8]{};
    iovec local{bytes, expected.size()}, remote{reinterpret_cast<void*>(address), expected.size()};
    if (process_vm_readv(getpid(), &local, 1, &remote, 1, 0) != ssize_t(expected.size()) ||
        std::memcmp(bytes, expected.begin(), expected.size()) != 0) {
        std::fprintf(stderr, "Free check: hook skipped (unexpected code at +%#llx)\n",
                     (unsigned long long)(address - ImageBase));
        return false;
    }
    void* page = reinterpret_cast<void*>(address & ~(PageSize - 1));
    if (mprotect(page, PageSize, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        return false;
    }
    __atomic_store_n(reinterpret_cast<unsigned char*>(address), static_cast<unsigned char>(0xcc),
                     __ATOMIC_SEQ_CST);
    mprotect(page, PageSize, PROT_READ | PROT_EXEC);
    return true;
}

/// The release starts with `mov rax, rsi` (48 89 f0), the collector's branch with
/// `mov rcx, [rsp+0x28]` (48 8b 4c 24 28).
bool InstallFreeHook() {
    if (!PatchInt3(FreeHook, {0x48, 0x89, 0xf0, 0x48, 0x85, 0xc0})) {
        return false;
    }
    PatchInt3(GcHook, {0x48, 0x8b, 0x4c, 0x24, 0x28});
    return true;
}

void InstallStepTrap() {
    struct sigaction action {};
    action.sa_sigaction = OnStepTrap;
    action.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigemptyset(&action.sa_mask);
    sigaction(SIGTRAP, &action, &previous_trap_action);
}

bool ArmStep(void* ucontext, u64 page) {
    const u32 tid = SignalTid();
    for (auto& slot : step_slots) {
        u32 expected = 0;
        if (slot.tid.load(std::memory_order_acquire) == tid ||
            slot.tid.compare_exchange_strong(expected, tid, std::memory_order_acq_rel)) {
            slot.page.store(page, std::memory_order_release);
            static_cast<ucontext_t*>(ucontext)->uc_mcontext.gregs[REG_EFL] |= TrapFlag;
            return true;
        }
    }
    return false; // table full: the page stays writable until the next decode
}

bool IsGuest(u64 v) {
    return v >= ImageBase && v < ImageEnd;
}

/// The newest decodes and writes of fences into the label qword at `label`, for a report.
int AppendHistory(char* line, int n, int size, u64 label, const char* name) {
    const u64 now = NowNs(), want = label & ~7ull;
    n += std::snprintf(line + n, size - n, "\n  %s %#llx decoded:", name, (unsigned long long)want);
    int shown = 0;
    const u64 nd = decoded_head.load(std::memory_order_relaxed);
    for (u64 i = nd; i-- > (nd > decoded.size() ? nd - decoded.size() : 0) && shown < 4 &&
                     n < size - 80;) {
        const Decoded& d = decoded[i % decoded.size()];
        if ((d.label & ~7ull) == want) {
            n += std::snprintf(line + n, size - n, " [%.3f ms ago, submission %llu, value %#llx]",
                               (now - d.ns) / 1e6, (unsigned long long)d.submit,
                               (unsigned long long)d.value);
            ++shown;
        }
    }
    n += std::snprintf(line + n, size - n, "; written:");
    shown = 0;
    const u64 nr = ring_head.load(std::memory_order_relaxed);
    for (u64 i = nr; i-- > (nr > ring.size() ? nr - ring.size() : 0) && shown < 4 && n < size - 80;) {
        const Entry& e = ring[i % ring.size()];
        if ((e.address & ~7ull) == want && e.source == u32(Eop)) {
            n += std::snprintf(line + n, size - n, " [%.3f ms ago, seq %llu, value %#llx]",
                               (now - e.ns) / 1e6, (unsigned long long)e.seq,
                               (unsigned long long)e.value);
            ++shown;
        }
    }
    return n;
}

/// The guest call chain at a breakpoint or fault: [rsp] (a leaf's return address) and the
/// rbp frames, as guest offsets.
int AppendStack(char* line, int n, int size, const greg_t* g, bool leaf) {
    if (leaf) {
        u64 ret = 0;
        if (ReadQword(u64(g[REG_RSP]), ret)) {
            n += std::snprintf(line + n, size - n, "\n  caller: %s%#llx", IsGuest(ret) ? "+" : "",
                               (unsigned long long)(IsGuest(ret) ? ret - ImageBase : ret));
        }
    }
    n += std::snprintf(line + n, size - n, "\n  frames:");
    u64 rbp = u64(g[REG_RBP]);
    for (int i = 0; i < 16 && rbp && n < size - 24; ++i) {
        u64 saved = 0, ret = 0;
        if (!ReadQword(rbp, saved) || !ReadQword(rbp + 8, ret)) {
            break;
        }
        n += std::snprintf(line + n, size - n, " %s%#llx", IsGuest(ret) ? "+" : "",
                           (unsigned long long)(IsGuest(ret) ? ret - ImageBase : ret));
        if (saved <= rbp) {
            break;
        }
        rbp = saved;
    }
    return n;
}

/// "not written" / "write started X ms ago": where the pending fence is.
int AppendState(char* line, int n, int size, const Pending& p) {
    const u64 now = NowNs();
    n += std::snprintf(line + n, size - n, "%u fence(s) pending, decoded %.3f ms ago, ", p.count,
                       (now - p.decoded_ns) / 1e6);
    if (p.writing_ns) {
        n += std::snprintf(line + n, size - n, "write started %.3f ms ago",
                           (now - p.writing_ns) / 1e6);
    } else {
        n += std::snprintf(line + n, size - n, "write not started");
    }
    return n;
}

bool FindPending(u64 label, Pending& out) {
    auto& t = Trap();
    std::scoped_lock lk{t.mutex};
    const auto it = t.pending.find(label & ~7ull);
    if (it == t.pending.end()) {
        return false;
    }
    out = it->second;
    return true;
}

bool TakeReport(const Pending& p) {
    auto& t = Trap();
    t.hits.fetch_add(1, std::memory_order_relaxed);
    return p.writing_ns ? t.started_reports.fetch_add(1, std::memory_order_relaxed) < 16
                        : t.reports.fetch_add(1, std::memory_order_relaxed) < 32;
}

void Emit(char* line, int n, int size) {
    n += std::snprintf(line + n, size - n, "\n");
    const ssize_t written = write(2, line, std::min<std::size_t>(n, size - 1));
    (void)written;
}

/// The guest allocator's release (int3): performs the replaced `mov rax, rsi`; a label block
/// whose fence has not landed is reported with the caller.
void OnFreeHook(void* ucontext) {
    auto* g = static_cast<ucontext_t*>(ucontext)->uc_mcontext.gregs;
    g[REG_RAX] = g[REG_RSI];
    g[REG_RIP] = greg_t(FreeHook + 3);
    const u64 block = u64(g[REG_RSI]);
    Pending p;
    if (!FindPending(block, p)) {
        return;
    }
    if (!TakeReport(p)) {
        return;
    }
    char line[3072];
    int n = std::snprintf(line, sizeof(line),
                          "Free check: RELEASED label block %#llx, tid %u, at %.3f s: ",
                          (unsigned long long)block, SignalTid(), NowNs() / 1e9);
    n = AppendState(line, n, sizeof(line), p);
    n = AppendStack(line, n, sizeof(line), g, true);
    u64 value = 0;
    ReadQword(block, value);
    n += std::snprintf(line + n, sizeof(line) - n, "\n  value %#llx", (unsigned long long)value);
    n = AppendHistory(line, n, sizeof(line), block, "this label");
    Emit(line, n, sizeof(line));
}

/// The collector's "done" branch (int3): performs the replaced `mov rcx, [rsp+0x28]`. r14 is the
/// block taken as done, its first qword the label (or bit 0: a pointer to the batch's label),
/// eax what the collector read there (4). Reported when that label's fence has not landed.
void OnGcHook(void* ucontext) {
    auto* g = static_cast<ucontext_t*>(ucontext)->uc_mcontext.gregs;
    g[REG_RCX] = greg_t(*reinterpret_cast<const u64*>(u64(g[REG_RSP]) + 0x28));
    g[REG_RIP] = greg_t(GcHook + 5);
    const u64 node = u64(g[REG_R14]);
    u64 raw = 0;
    ReadQword(node, raw);
    const u64 label = (raw & 1) ? raw & ~1ull : node;
    Pending p, own;
    const bool label_pending = FindPending(label, p);
    const bool node_pending = label != node && FindPending(node, own);
    if (!label_pending && !node_pending) {
        return;
    }
    if (!TakeReport(label_pending ? p : own)) {
        return;
    }
    char line[3072];
    int n = std::snprintf(line, sizeof(line),
                          "Free check: TAKEN AS DONE block %#llx, tid %u, at %.3f s: first qword "
                          "%#llx, read %#x",
                          (unsigned long long)node, SignalTid(), NowNs() / 1e9,
                          (unsigned long long)raw, unsigned(g[REG_RAX]));
    if (label_pending) {
        n += std::snprintf(line + n, sizeof(line) - n, "\n  label %#llx: ",
                           (unsigned long long)label);
        n = AppendState(line, n, sizeof(line), p);
    }
    if (node_pending) {
        n += std::snprintf(line + n, sizeof(line) - n, "\n  block's own label: ");
        n = AppendState(line, n, sizeof(line), own);
    }
    n = AppendStack(line, n, sizeof(line), g, false);
    n = AppendHistory(line, n, sizeof(line), label, "label");
    if (label != node) {
        n = AppendHistory(line, n, sizeof(line), node, "block");
    }
    Emit(line, n, sizeof(line));
}
} // namespace

bool OnTrapFault(void* ucontext, u64 address) {
    if (TrapMode() != 1) {
        return false;
    }
    const u64 page = address & ~(PageSize - 1);
    auto& t = Trap();
    std::unique_lock lk{t.mutex};
    if (!t.read_only.count(page)) {
        return false;
    }
    t.faults.fetch_add(1, std::memory_order_relaxed);
    const auto it = t.pending.find(address & ~7ull);
    const bool hit = it != t.pending.end();
    const Pending p = hit ? it->second : Pending{};
    // Writable for the faulting instruction only (single step), read-only again after it.
    mprotect(reinterpret_cast<void*>(page), PageSize, PROT_READ | PROT_WRITE);
    if (!ArmStep(ucontext, page)) {
        t.read_only.erase(page);
    }
    lk.unlock();
    const auto* g = static_cast<const ucontext_t*>(ucontext)->uc_mcontext.gregs;
    if (!hit || !IsGuest(u64(g[REG_RIP]))) {
        return true;
    }
    t.hits.fetch_add(1, std::memory_order_relaxed);
    if (t.reports.fetch_add(1, std::memory_order_relaxed) >= 24) {
        return true;
    }
    char line[3072];
    int n = std::snprintf(line, sizeof(line),
                          "Free check: TRAP guest write %#llx into a label block, tid %u, at %.3f "
                          "s: rip +%#llx rdi %#llx rsi %#llx rax %#llx rcx %#llx\n  ",
                          (unsigned long long)address, SignalTid(), NowNs() / 1e9,
                          (unsigned long long)(u64(g[REG_RIP]) - ImageBase),
                          (unsigned long long)g[REG_RDI], (unsigned long long)g[REG_RSI],
                          (unsigned long long)g[REG_RAX], (unsigned long long)g[REG_RCX]);
    n = AppendState(line, n, sizeof(line), p);
    n = AppendStack(line, n, sizeof(line), g, true);
    u64 old = 0;
    ReadQword(address & ~7ull, old);
    n += std::snprintf(line + n, sizeof(line) - n, "\n  old value %#llx", (unsigned long long)old);
    n = AppendHistory(line, n, sizeof(line), address, "this label");
    Emit(line, n, sizeof(line));
    return true;
}

bool OnStaleTrapFault(u64 address) {
    if (TrapMode() != 1) {
        return false;
    }
    const u64 page = address & ~(PageSize - 1);
    auto& t = Trap();
    std::scoped_lock lk{t.mutex};
    if (!t.ever_trapped.count(page)) {
        return false;
    }
    // The write faulted while the page was read-only and its fences were written before the
    // handler got the lock: the page is writable again (or made so) and the write is retried.
    // Trapped again meanwhile: the retry faults into OnTrapFault.
    if (!t.read_only.count(page)) {
        mprotect(reinterpret_cast<void*>(page), PageSize, PROT_READ | PROT_WRITE);
    }
    return true;
}
} // namespace BbFreeCheck
