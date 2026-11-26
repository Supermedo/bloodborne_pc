// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <span>
#include <utility>
#include <vector>

// A submission owns its top-level command bytes until its decoder coroutine
// retires. SubmitDone/new frames cannot reuse a previous submission's storage.
// bbport: BB_DCB_CHECK=1 with BB_COPY_GPU_BUFFERS=0 makes the same copy as a snapshot only (the GPU
// thread reads the game's memory) and compares the two when the GPU thread starts and ends it.
class BbCommandBufferCopy {
public:
    /// Copies both buffers.
    BbCommandBufferCopy(std::span<const std::uint32_t> dcb,
                        std::span<const std::uint32_t> ccb)
        : draw_copy(dcb.begin(), dcb.end()), constant(ccb.begin(), ccb.end()), draw(draw_copy) {}

    /// bbport: the draw commands are a copy already made at this submission (the draw
    /// preparation's), kept alive by `owner_`; only the constant buffer is copied.
    BbCommandBufferCopy(std::shared_ptr<const void> owner_, std::span<const std::uint32_t> dcb_copy,
                        std::span<const std::uint32_t> ccb)
        : owner(std::move(owner_)), constant(ccb.begin(), ccb.end()), draw(dcb_copy) {}

    BbCommandBufferCopy(const BbCommandBufferCopy&) = delete;
    BbCommandBufferCopy& operator=(const BbCommandBufferCopy&) = delete;

    std::span<const std::uint32_t> Draw() const { return draw; }
    std::span<const std::uint32_t> Constant() const { return constant; }

    const std::chrono::steady_clock::time_point submitted = std::chrono::steady_clock::now();
    std::uint32_t queued_ahead = 0; ///< submissions not yet finished when this one was queued

private:
    const std::shared_ptr<const void> owner;
    const std::vector<std::uint32_t> draw_copy, constant;
    const std::span<const std::uint32_t> draw;
};

// bbport: BB_DCB_CHECK=1 diagnostics: did the game change a command buffer in its memory between
// submitting it and the GPU thread reading it? `when`: "start" or "end" of its processing.
inline void BbDcbCompare(const BbCommandBufferCopy& snapshot, std::span<const std::uint32_t> guest,
                         const char* when) {
    static std::atomic<std::uint64_t> checked{}, changed_start{}, changed_end{};
    static std::atomic<int> reported{};
    static auto last_summary = std::chrono::steady_clock::now();
    const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                              snapshot.submitted)
                        .count();
    const bool start = when[0] == 's';
    const auto copy = snapshot.Draw();
    std::size_t first = copy.size(), differing = 0;
    for (std::size_t i = 0; i < copy.size() && i < guest.size(); ++i) {
        if (copy[i] != guest[i]) {
            differing += 1;
            first = first == copy.size() ? i : first;
        }
    }
    // The packet (as submitted) holding the first difference, and the nested buffers it calls.
    static std::atomic<std::uint64_t> nested{};
    std::size_t at = 0, holder_at = 0;
    std::uint32_t holder = 0;
    bool holder_found = false;
    while (at < copy.size()) {
        const std::uint32_t h = copy[at];
        const std::size_t words = (h >> 30) == 3 ? ((h >> 16) & 0x3fff) + 2 : (h >> 30) == 2 ? 1 : 0;
        if (words == 0) {
            break;
        }
        if (start && (h >> 30) == 3 && ((h >> 8) & 0xff) == 0x3f) { // IndirectBuffer
            nested.fetch_add(1, std::memory_order_relaxed);
        }
        if (!holder_found && first < at + words) {
            holder = h, holder_at = at, holder_found = true;
        }
        at += words;
    }
    if (start) {
        checked.fetch_add(1, std::memory_order_relaxed);
    }
    if (differing != 0) {
        // The GPU's own writes into a NOP body (data the game keeps in its command buffer) are
        // counted apart: the parser never reads them as packets.
        const bool in_nop = holder_found && (holder >> 30) == 3 && ((holder >> 8) & 0xff) == 0x10;
        static std::atomic<std::uint64_t> in_nop_end{};
        if (!start && in_nop) {
            in_nop_end.fetch_add(1, std::memory_order_relaxed);
        } else {
            (start ? changed_start : changed_end).fetch_add(1, std::memory_order_relaxed);
        }
        static std::atomic<int> nop_reported{};
        if ((!start && in_nop ? nop_reported : reported).fetch_add(1, std::memory_order_relaxed) <
            (!start && in_nop ? 3 : 40)) {
            std::printf("DCB check: buffer %p (%zu dw) changed by %s, %.1f ms after submit "
                        "(%u queued ahead): %zu dw differ, first at +%#zx (%#010x submitted, "
                        "%#010x now) in packet %#010x at +%#zx\n",
                        static_cast<const void*>(guest.data()), copy.size(), when, ms,
                        snapshot.queued_ahead, differing, first * 4, copy[first], guest[first],
                        holder, holder_at * 4);
            std::fflush(stdout);
        }
    }
    if (start && std::chrono::steady_clock::now() - last_summary > std::chrono::seconds(5)) {
        last_summary = std::chrono::steady_clock::now();
        std::printf("DCB check: %llu buffers (%llu nested calls), %llu changed before the start, "
                    "%llu during (outside NOP bodies)\n",
                    static_cast<unsigned long long>(checked.load()),
                    static_cast<unsigned long long>(nested.load()),
                    static_cast<unsigned long long>(changed_start.load()),
                    static_cast<unsigned long long>(changed_end.load()));
        std::fflush(stdout);
    }
}
