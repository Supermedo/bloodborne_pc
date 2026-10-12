// SPDX-License-Identifier: GPL-2.0-or-later
#include "bbport_gnm_hooks.h"

#include <algorithm>
#include <initializer_list>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>
#include <sys/mman.h>
#include <unistd.h>

namespace BbGnmHooks {
namespace {
using u8 = std::uint8_t;
using u64 = std::uint64_t;

struct Entry {
    u64 offset;
    u8 prologue_size;
    u8 prologue[15];
    unsigned sites;
    bool cb_method; // calls the command buffer's reserve callback: rdi is the command buffer
    const char* pm4;
};
constexpr Entry Table[] = {
#include "bbport_gnm_table.inc"
};
constexpr std::size_t Count = sizeof(Table) / sizeof(Table[0]);

/// Hook stub per function: `lock inc qword [rip+counter]`, the moved prologue, `jmp` back past it.
constexpr std::size_t SlotSize = 256; // observing stubs are ~190 bytes
constexpr std::size_t CounterStride = 64; // one cache line per counter

u8* stubs = nullptr;
u64* CounterAt(std::size_t i) {
    const std::size_t page = std::size_t(sysconf(_SC_PAGESIZE));
    const std::size_t code_bytes = ((Count + 1) * SlotSize + page - 1) / page * page;
    return reinterpret_cast<u64*>(stubs + code_bytes + i * CounterStride);
}

/// A mapping within +-2 GiB of the image text, for rel32 jumps both ways.
u8* MapNear(unsigned char* image, std::uint64_t image_size, std::size_t size) {
    const u64 base = reinterpret_cast<u64>(image);
    for (u64 k = 1; k <= 64; ++k) {
        for (const u64 hint : {base - k * (16ull << 20), base + image_size + k * (16ull << 20)}) {
            void* p = mmap(reinterpret_cast<void*>(hint), size, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
            if (p != MAP_FAILED) {
                return static_cast<u8*>(p);
            }
        }
    }
    return nullptr;
}

bool Rel32(const u8* from_next, const void* to, std::int32_t& rel) {
    const auto d = reinterpret_cast<std::intptr_t>(to) - reinterpret_cast<std::intptr_t>(from_next);
    if (d < INT32_MIN || d > INT32_MAX) {
        return false;
    }
    rel = static_cast<std::int32_t>(d);
    return true;
}

/// BB_GNM_OBSERVE=1: command buffer methods run through an observing stub. It saves the argument
/// registers, calls Enter (which notes the buffer's write pointer and puts ExitThunk in place of the
/// return address), runs the function; ExitThunk calls Leave, which marks every packet the call
/// wrote in a shadow table (writer, checksum). Submissions are checked against it: which packets
/// came from known API calls, which from elsewhere, which were changed after the call wrote them.
bool observe = false;
u8* exit_thunk = nullptr;

struct Frame {
    std::uint32_t index;
    u64 cb;
    u64 before;
    u64 ret;
    u64 args[6]; // rdi, rsi, rdx, rcx, r8, r9 (BB_GNM_TRACE)
};
/// BB_GNM_TRACE=offset[:count]: prints the arguments, the caller and the packets written by the
/// first `count` (default 40) calls of that function.
std::uint32_t trace_index = ~0u;
std::atomic<int> trace_left{0};
constexpr int MaxDepth = 64;
thread_local Frame frames[MaxDepth];
thread_local int depth = 0;

std::atomic<u64> anomalies{0};
std::atomic<u64> anomalies_by_fn[1024];
std::atomic<u64> anomaly_kind[3]; // 0 pointer went back or far, 1 not a packet, 2 packet past the end
std::atomic<u64> overflow{0};

/// Shadow of guest command memory: per dword at a packet start, (writer index + 1) << 22 | checksum.
constexpr int L1Bits = 14, L2Bits = 14;
std::atomic<std::atomic<std::uint32_t*>*> shadow_l1[1 << L1Bits];

std::uint32_t* ShadowPage(u64 va, bool create) {
    const u64 l1 = (va >> 26) & ((1 << L1Bits) - 1);
    const u64 l2 = (va >> 12) & ((1 << L2Bits) - 1);
    auto* table = shadow_l1[l1].load(std::memory_order_acquire);
    if (!table) {
        if (!create) {
            return nullptr;
        }
        auto* fresh = new std::atomic<std::uint32_t*>[1 << L2Bits]();
        if (!shadow_l1[l1].compare_exchange_strong(table, fresh)) {
            delete[] fresh;
        } else {
            table = fresh;
        }
    }
    auto* page = table[l2].load(std::memory_order_acquire);
    if (!page && create) {
        auto* fresh = new std::uint32_t[1024]();
        if (!table[l2].compare_exchange_strong(page, fresh)) {
            delete[] fresh;
        } else {
            page = fresh;
        }
    }
    return page;
}

std::uint32_t Checksum(const std::uint32_t* p, std::uint32_t n) {
    std::uint32_t h = 2166136261u;
    for (std::uint32_t k = 0; k < n; ++k) {
        h = (h ^ p[k]) * 16777619u;
    }
    return h & 0x3fffff;
}

/// Packet size in dwords (type 3: header + count + 1; type 2: filler), 0 for anything else.
std::uint32_t PacketDwords(std::uint32_t h) {
    switch (h >> 30) {
    case 3:
        return ((h >> 16) & 0x3fff) + 2;
    case 2:
        return 1;
    default:
        return 0;
    }
}

std::atomic<u64> chunk_switches{0};
std::atomic<u64> uncovered_after[1024]; // uncovered packets by the writer of the packet before
std::atomic<u64> uncovered_ctx_reg[0x400];
std::atomic<u64> uncovered_sh_reg[0x400];
std::atomic<u64> callback_seen[8];
std::atomic<u64> callback_hits[8];
u64 image_base = 0;

void NoteCallback(u64 cb) {
    const u64 fn = *reinterpret_cast<const u64*>(cb + 0x18) - image_base;
    for (int k = 0; k < 8; ++k) {
        u64 seen = callback_seen[k].load(std::memory_order_relaxed);
        if (seen == fn || (seen == 0 && callback_seen[k].compare_exchange_strong(seen, fn)) || seen == fn) {
            callback_hits[k].fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
}

void Mark(std::uint32_t index, u64 before, u64 after, u64 cb = 0) {
    if (after == before) {
        return;
    }
    if (cb && (after < before || after - before > (1u << 20))) {
        // The reserve callback started a new chunk (the engine's 0x26af230 keeps a chunk list):
        // the call wrote from the new chunk's start.
        const u64 begin = *reinterpret_cast<const u64*>(cb);
        if (begin <= after && after - begin <= (1u << 20)) {
            chunk_switches.fetch_add(1, std::memory_order_relaxed);
            before = begin;
        }
    }
    if (after < before || after - before > (1u << 20) || (before & 3)) {
        anomalies.fetch_add(1, std::memory_order_relaxed);
        anomalies_by_fn[index].fetch_add(1, std::memory_order_relaxed);
        anomaly_kind[0].fetch_add(1, std::memory_order_relaxed);
        if (cb) {
            NoteCallback(cb);
        }
        return;
    }
    for (u64 p = before; p < after;) {
        const auto* w = reinterpret_cast<const std::uint32_t*>(p);
        const std::uint32_t n = PacketDwords(*w);
        if (n == 0 || p + n * 4 > after) {
            anomalies.fetch_add(1, std::memory_order_relaxed);
            anomalies_by_fn[index].fetch_add(1, std::memory_order_relaxed);
            anomaly_kind[n == 0 ? 1 : 2].fetch_add(1, std::memory_order_relaxed);
            return;
        }
        std::uint32_t* page = ShadowPage(p, true);
        page[(p >> 2) & 1023] = ((index + 1) << 22) | Checksum(w, n);
        p += n * 4;
    }
}

extern "C" void BbGnmEnter(std::uint32_t index, u64 cb, u64* slot) {
    __atomic_fetch_add(CounterAt(index), 1, __ATOMIC_RELAXED);
    if (depth == MaxDepth) {
        overflow.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    Frame& f = frames[depth++];
    // A traced function that is not a command buffer method: no write pointer to note.
    f = {index, cb, Table[index].cb_method ? *reinterpret_cast<const u64*>(cb + 0x10) : 0, *slot, {}};
    if (index == trace_index) {
        // The stub saved rdi..r9 just below the return address slot.
        f.args[0] = slot[-1]; f.args[1] = slot[-2]; f.args[2] = slot[-3];
        f.args[3] = slot[-4]; f.args[4] = slot[-5]; f.args[5] = slot[-6];
    }
    *slot = reinterpret_cast<u64>(exit_thunk);
}

extern "C" u64 BbGnmLeave() {
    const Frame f = frames[--depth];
    const bool cb_method = Table[f.index].cb_method;
    if (f.index == trace_index && trace_left.fetch_sub(1, std::memory_order_relaxed) > 0) {
        const u64 after = cb_method ? *reinterpret_cast<const u64*>(f.cb + 0x10) : f.before;
        char line[4096];
        int n = std::snprintf(line, sizeof(line),
                              "Gnm trace 0x%07llx from 0x%llx: args %llx %llx %llx %llx %llx %llx; wrote",
                              (unsigned long long)Table[f.index].offset,
                              (unsigned long long)(f.ret - image_base), (unsigned long long)f.args[1],
                              (unsigned long long)f.args[2], (unsigned long long)f.args[3],
                              (unsigned long long)f.args[4], (unsigned long long)f.args[5], 0ull);
        if (!cb_method && f.args[0]) {
            n += std::snprintf(line + n, sizeof(line) - n, " (rdi %llx:", (unsigned long long)f.args[0]);
            for (int k = 0; k < 8; ++k) {
                n += std::snprintf(line + n, sizeof(line) - n, " %08x",
                                   reinterpret_cast<const std::uint32_t*>(f.args[0])[k]);
            }
            n += std::snprintf(line + n, sizeof(line) - n, ")");
        }
        if (after > f.before && after - f.before <= 256) {
            for (u64 p = f.before; p < after && n < int(sizeof(line)) - 12; p += 4) {
                n += std::snprintf(line + n, sizeof(line) - n, " %08x",
                                   *reinterpret_cast<const std::uint32_t*>(p));
            }
        }
        std::printf("%s\n", line);
    }
    if (!cb_method) {
        return f.ret;
    }
    Mark(f.index, f.before, *reinterpret_cast<const u64*>(f.cb + 0x10), f.cb);
    return f.ret;
}

// Submission check statistics (written by the submitting thread, read by the report thread).
std::atomic<u64> sub_packets{0}, sub_dwords{0}, cov_packets{0}, cov_dwords{0}, changed_packets{0};
std::atomic<u64> uncovered_by_op[256];
std::atomic<u64> uncovered_dwords_by_op[256];
std::atomic<u64> changed_by_fn[1024];
std::atomic<u64> nop_tags[16]; // uncovered NOPs by their first payload dword's top byte (markers)

struct Writer {
    u8* p;
    void Put(std::initializer_list<int> bytes) {
        for (int b : bytes) {
            *p++ = u8(b);
        }
    }
    void Imm32(std::uint32_t v) {
        std::memcpy(p, &v, 4);
        p += 4;
    }
    void Imm64(u64 v) {
        std::memcpy(p, &v, 8);
        p += 8;
    }
};

// movdqu [rsp+8k], xmmk / movdqu xmmk, [rsp+8k] for k = 0..7 (16 bytes apart).
void SaveXmm(Writer& w, int count, bool load) {
    for (int k = 0; k < count; ++k) {
        const int op = load ? 0x6f : 0x7f;
        if (k == 0) {
            w.Put({0xf3, 0x0f, op, 0x04, 0x24});
        } else {
            w.Put({0xf3, 0x0f, op, 0x44 | (k << 3), 0x24, k * 16});
        }
    }
}

void WriteExitThunk(u8* at) {
    Writer w{at};
    w.Put({0x48, 0x83, 0xec, 0x08}); // sub rsp, 8 (the return address slot)
    w.Put({0x50, 0x52});             // push rax; push rdx
    w.Put({0x48, 0x83, 0xec, 0x28}); // sub rsp, 0x28
    SaveXmm(w, 2, false);
    w.Put({0x48, 0xb8});
    w.Imm64(reinterpret_cast<u64>(&BbGnmLeave)); // mov rax, BbGnmLeave
    w.Put({0xff, 0xd0});                          // call rax
    w.Put({0x48, 0x89, 0x44, 0x24, 0x38});        // mov [rsp+0x38], rax
    SaveXmm(w, 2, true);
    w.Put({0x48, 0x83, 0xc4, 0x28}); // add rsp, 0x28
    w.Put({0x5a, 0x58, 0xc3});       // pop rdx; pop rax; ret
}

/// Observing entry stub; returns its end (where the moved prologue goes).
u8* WriteObserveStub(u8* at, std::uint32_t index) {
    Writer w{at};
    w.Put({0x57, 0x56, 0x52, 0x51, 0x41, 0x50, 0x41, 0x51, 0x50}); // push rdi..r9, rax
    w.Put({0x48, 0x81, 0xec});
    w.Imm32(0x80); // sub rsp, 0x80
    SaveXmm(w, 8, false);
    w.Put({0xbf});
    w.Imm32(index);                                            // mov edi, index
    w.Put({0x48, 0x8b, 0xb4, 0x24});
    w.Imm32(0xb0);                                             // mov rsi, [rsp+0xb0] (rdi)
    w.Put({0x48, 0x8d, 0x94, 0x24});
    w.Imm32(0xb8);                                             // lea rdx, [rsp+0xb8] (return slot)
    w.Put({0x48, 0xb8});
    w.Imm64(reinterpret_cast<u64>(&BbGnmEnter));               // mov rax, BbGnmEnter
    w.Put({0xff, 0xd0});                                       // call rax
    SaveXmm(w, 8, true);
    w.Put({0x48, 0x81, 0xc4});
    w.Imm32(0x80);                                             // add rsp, 0x80
    w.Put({0x58, 0x41, 0x59, 0x41, 0x58, 0x59, 0x5a, 0x5e, 0x5f}); // pop rax, r9, r8, rcx..rdi
    return w.p;
}

void ReportObserve() {
    const u64 sp = sub_packets.exchange(0), sd = sub_dwords.exchange(0);
    const u64 cp = cov_packets.exchange(0), cd = cov_dwords.exchange(0);
    const u64 ch = changed_packets.exchange(0);
    if (!sp) {
        return;
    }
    std::printf("Gnm observe: %llu packets submitted, %.1f%% from hooked API calls (%.1f%% of dwords), "
                "%llu changed after the call, %llu call anomalies, %llu depth overflows\n",
                (unsigned long long)sp, 100.0 * cp / sp, sd ? 100.0 * cd / sd : 0.0,
                (unsigned long long)ch, (unsigned long long)anomalies.exchange(0),
                (unsigned long long)overflow.exchange(0));
    std::vector<std::pair<u64, int>> ops;
    for (int k = 0; k < 256; ++k) {
        if (const u64 n = uncovered_by_op[k].exchange(0)) {
            ops.emplace_back(n, k);
        }
    }
    std::sort(ops.rbegin(), ops.rend());
    for (std::size_t k = 0; k < std::min<std::size_t>(ops.size(), 16); ++k) {
        const int op = ops[k].second;
        std::printf("  not from the API: opcode 0x%02x %llu packets, %llu dwords\n", op,
                    (unsigned long long)ops[k].first,
                    (unsigned long long)uncovered_dwords_by_op[op].exchange(0));
    }
    for (int k = 0; k < 16; ++k) {
        if (const u64 n = nop_tags[k].exchange(0)) {
            std::printf("  not from the API: NOP payload top nibble 0x%x: %llu\n", k,
                        (unsigned long long)n);
        }
    }
    for (std::size_t k = 0; k <= Count; ++k) {
        if (const u64 n = changed_by_fn[k + 1].exchange(0)) {
            std::printf("  changed after %s 0x%07llx wrote them: %llu packets\n",
                        k == Count ? "the driver" : "", k == Count ? 0ull : (unsigned long long)Table[k].offset,
                        (unsigned long long)n);
        }
        if (const u64 n = anomalies_by_fn[k].exchange(0)) {
            std::printf("  anomalies in %s0x%07llx: %llu\n", k == Count ? "the driver " : "",
                        k == Count ? 0ull : (unsigned long long)Table[k].offset, (unsigned long long)n);
        }
    }
    for (int k = 0; k < 8; ++k) {
        if (const u64 n = callback_hits[k].exchange(0)) {
            std::printf("  reserve callback at guest 0x%llx moved the pointer back: %llu\n",
                        (unsigned long long)callback_seen[k].load(), (unsigned long long)n);
        }
    }
    {
        std::vector<std::pair<u64, int>> top;
        for (int k = 0; k < 1024; ++k) {
            if (const u64 n = uncovered_after[k].exchange(0)) {
                top.emplace_back(n, k);
            }
        }
        std::sort(top.rbegin(), top.rend());
        for (std::size_t k = 0; k < std::min<std::size_t>(top.size(), 6); ++k) {
            const int id = top[k].second;
            std::printf("  not from the API, after a packet of %s0x%07llx: %llu\n",
                        id == 0 ? "(nothing) " : id == int(Count) + 1 ? "the driver " : "",
                        id >= 1 && id <= int(Count) ? (unsigned long long)Table[id - 1].offset : 0ull,
                        (unsigned long long)top[k].first);
        }
    }
    std::printf("  chunk switches: %llu\n", (unsigned long long)chunk_switches.exchange(0));
    for (int t = 0; t < 2; ++t) {
        auto* regs = t ? uncovered_sh_reg : uncovered_ctx_reg;
        std::vector<std::pair<u64, int>> top;
        for (int r = 0; r < 0x400; ++r) {
            if (const u64 n = regs[r].exchange(0)) {
                top.emplace_back(n, r);
            }
        }
        std::sort(top.rbegin(), top.rend());
        for (std::size_t k = 0; k < std::min<std::size_t>(top.size(), 8); ++k) {
            std::printf("  not from the API: %s reg 0x%x: %llu\n", t ? "SH" : "context", top[k].second,
                        (unsigned long long)top[k].first);
        }
    }
    std::printf("  anomaly kinds: pointer back/far %llu, not a packet %llu, past the end %llu\n",
                (unsigned long long)anomaly_kind[0].exchange(0),
                (unsigned long long)anomaly_kind[1].exchange(0),
                (unsigned long long)anomaly_kind[2].exchange(0));
}

void ReportLoop() {
    std::vector<u64> last(Count, 0);
    auto t0 = std::chrono::steady_clock::now();
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        const auto t1 = std::chrono::steady_clock::now();
        const double s = std::chrono::duration<double>(t1 - t0).count();
        t0 = t1;
        std::vector<std::pair<u64, std::size_t>> rates;
        u64 total = 0;
        std::size_t used = 0;
        for (std::size_t i = 0; i < Count; ++i) {
            const u64 now = __atomic_load_n(CounterAt(i), __ATOMIC_RELAXED);
            const u64 d = now - last[i];
            last[i] = now;
            total += d;
            used += now != 0;
            if (d) {
                rates.emplace_back(d, i);
            }
        }
        std::sort(rates.rbegin(), rates.rend());
        // BB_GNM_CENSUS_FILE: every function's total calls so far (offset, calls, cb method, PM4).
        if (const char* path = std::getenv("BB_GNM_CENSUS_FILE")) {
            if (FILE* f = std::fopen(path, "w")) {
                for (std::size_t i = 0; i < Count; ++i) {
                    std::fprintf(f, "0x%07llx %llu %d %s\n", (unsigned long long)Table[i].offset,
                                 (unsigned long long)last[i], int(Table[i].cb_method), Table[i].pm4);
                }
                std::fclose(f);
            }
        }
        std::printf("Gnm census: %.0f calls/s, %zu of %zu functions called so far\n", total / s,
                    used, Count);
        for (std::size_t k = 0; k < std::min<std::size_t>(rates.size(), 40); ++k) {
            const auto& e = Table[rates[k].second];
            std::printf("  0x%07llx %9.0f/s  %s\n", static_cast<unsigned long long>(e.offset),
                        rates[k].first / s, e.pm4);
        }
        if (observe) {
            ReportObserve();
        }
        std::fflush(stdout);
    }
}
} // namespace

void CheckSubmission(const std::uint32_t* commands, std::uint64_t dwords) {
    if (!observe || !commands) {
        return;
    }
    u64 packets = 0, covered = 0, covered_dw = 0, changed = 0;
    std::uint32_t previous = 0;
    for (u64 at = 0; at < dwords;) {
        const auto* w = commands + at;
        std::uint32_t n = PacketDwords(*w);
        if (n == 0 || at + n > dwords) {
            n = 1; // type 0/1 or cut off: counted as one uncovered dword
        }
        const u64 va = reinterpret_cast<u64>(w);
        std::uint32_t* page = ShadowPage(va, false);
        const std::uint32_t e = page ? page[(va >> 2) & 1023] : 0;
        ++packets;
        const int op = (*w >> 30) == 3 ? int((*w >> 8) & 0xff) : 0;
        if (e == 0) {
            uncovered_after[std::min<std::uint32_t>(previous, 1023)].fetch_add(1, std::memory_order_relaxed);
            uncovered_by_op[op].fetch_add(1, std::memory_order_relaxed);
            uncovered_dwords_by_op[op].fetch_add(n, std::memory_order_relaxed);
            if ((op == 0x69 || op == 0x76) && n > 1) {
                (op == 0x69 ? uncovered_ctx_reg : uncovered_sh_reg)[w[1] & 0x3ff].fetch_add(
                    1, std::memory_order_relaxed);
            }
            if (op == 0x10 && n > 1) {
                nop_tags[w[1] >> 28].fetch_add(1, std::memory_order_relaxed);
            }
        } else if ((e & 0x3fffff) != Checksum(w, n)) {
            ++changed;
            changed_by_fn[std::min<std::uint32_t>(e >> 22, 1023)].fetch_add(1, std::memory_order_relaxed);
        } else {
            ++covered;
            covered_dw += n;
        }
        if (e) {
            previous = e >> 22;
        }
        if (page) {
            page[(va >> 2) & 1023] = 0;
        }
        at += n;
    }
    sub_packets.fetch_add(packets, std::memory_order_relaxed);
    sub_dwords.fetch_add(dwords, std::memory_order_relaxed);
    cov_packets.fetch_add(covered, std::memory_order_relaxed);
    cov_dwords.fetch_add(covered_dw, std::memory_order_relaxed);
    changed_packets.fetch_add(changed, std::memory_order_relaxed);
}

void PatchImage(unsigned char* image, std::uint64_t size) {
    const char* env = std::getenv("BB_GNM_CENSUS");
    const char* observe_env = std::getenv("BB_GNM_OBSERVE");
    observe = observe_env && observe_env[0] == '1';
    if (!observe && (!env || env[0] != '1')) {
        return;
    }
    const std::size_t stub_bytes = (Count + 1) * SlotSize; // + the shared exit thunk
    const std::size_t page = std::size_t(sysconf(_SC_PAGESIZE));
    const std::size_t code_bytes = (stub_bytes + page - 1) / page * page;
    const std::size_t map_bytes = code_bytes + Count * CounterStride;
    image_base = reinterpret_cast<u64>(image);
    if (const char* t = std::getenv("BB_GNM_TRACE")) {
        unsigned long long offset = 0;
        int count = 40;
        std::sscanf(t, "%llx:%d", &offset, &count);
        for (std::size_t i = 0; i < Count; ++i) {
            if (Table[i].offset == offset) {
                trace_index = std::uint32_t(i);
            }
        }
        trace_left = count;
    }
    stubs = MapNear(image, size, map_bytes);
    if (!stubs) {
        std::printf("Gnm census: no memory near the image, off\n");
        return;
    }
    std::size_t patched = 0, mismatched = 0;
    exit_thunk = stubs + Count * SlotSize;
    WriteExitThunk(exit_thunk);
    // BB_GNM_CENSUS_RANGE=first:last (table indices, bisecting).
    std::size_t first = 0, last = Count;
    if (const char* r = std::getenv("BB_GNM_CENSUS_RANGE")) {
        std::sscanf(r, "%zu:%zu", &first, &last);
    }
    u8* counters = stubs + code_bytes;
    for (std::size_t i = first; i < std::min(last, Count); ++i) {
        const Entry& e = Table[i];
        u8* fn = image + e.offset;
        if (e.offset + e.prologue_size > size || std::memcmp(fn, e.prologue, e.prologue_size) != 0) {
            ++mismatched;
            continue;
        }
        u8* s = stubs + i * SlotSize;
        u8* counter = counters + i * CounterStride;
        std::int32_t rel;
        u8* body;
        if (observe && (e.cb_method || i == trace_index)) {
            body = WriteObserveStub(s, std::uint32_t(i));
        } else {
            // lock inc qword [rip+rel]
            s[0] = 0xf0; s[1] = 0x48; s[2] = 0xff; s[3] = 0x05;
            if (!Rel32(s + 8, counter, rel)) {
                ++mismatched;
                continue;
            }
            std::memcpy(s + 4, &rel, 4);
            body = s + 8;
        }
        std::memcpy(body, e.prologue, e.prologue_size);
        u8* j = body + e.prologue_size;
        j[0] = 0xe9;
        if (!Rel32(j + 5, fn + e.prologue_size, rel)) {
            ++mismatched;
            continue;
        }
        std::memcpy(j + 1, &rel, 4);
        // The entry: jmp stub, the rest of the moved prologue int3 (never reached).
        std::int32_t to_stub;
        if (!Rel32(fn + 5, s, to_stub)) {
            ++mismatched;
            continue;
        }
        fn[0] = 0xe9;
        std::memcpy(fn + 1, &to_stub, 4);
        std::memset(fn + 5, 0xcc, e.prologue_size - 5);
        ++patched;
    }
    mprotect(stubs, code_bytes, PROT_READ | PROT_EXEC);
    std::printf("Gnm census: %zu of %zu libGnm entry points hooked (%zu do not match the 1.09 eboot)%s\n",
                patched, Count, mismatched, observe ? "; command buffer methods observed" : "");
    std::thread(ReportLoop).detach();
}
} // namespace BbGnmHooks

namespace BbGnmHooks {
DriverWrite::~DriverWrite() {
    if (!observe || !commands) {
        return;
    }
    // The written packets end where the space does or at the first zero dword (unused space).
    const auto begin = reinterpret_cast<u64>(commands);
    u64 end = begin;
    const u64 limit = begin + dwords * 4;
    while (end < limit) {
        const std::uint32_t n = PacketDwords(*reinterpret_cast<const std::uint32_t*>(end));
        if (n == 0 || end + n * 4 > limit) {
            break;
        }
        end += n * 4;
    }
    Mark(std::uint32_t(Count), begin, end);
}
} // namespace BbGnmHooks
