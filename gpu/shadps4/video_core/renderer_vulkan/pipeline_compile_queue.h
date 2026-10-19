// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: pipelines first seen during play (docs/pipeline_compiles.md). The compile threads'
// job queue, from which a thread that needs a pipeline right away takes its job, the rule for
// how long draws of a pipeline still compiling may be left out, and the statistics of both
// (BB_FRAME_STATS "Pipelines:" line). No Vulkan here: tests/test_pipeline_compile.cpp.

#pragma once

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <utility>

namespace Vulkan {

/// Compile jobs, first in, first out (the order the draws need them in), each with the object
/// it creates (`owner`). A thread about to wait for that object takes its job when no compile
/// thread has started it yet (Take) and runs it itself, instead of waiting behind the jobs
/// queued before it (20-90 pipelines at once entering an area).
class PipelineJobQueue {
public:
    using Job = std::function<void()>;

    void Push(const void* owner, Job job) {
        {
            std::scoped_lock lk{mutex};
            jobs.push_back({owner, std::move(job)});
        }
        cv.notify_one();
    }

    /// A compile thread: the oldest job (waits for one).
    Job Pop() {
        std::unique_lock lk{mutex};
        cv.wait(lk, [this] { return !jobs.empty(); });
        Job job = std::move(jobs.front().job);
        jobs.pop_front();
        return job;
    }

    /// `owner`'s job, taken out of the queue, when no thread has started it; else empty.
    Job Take(const void* owner) {
        std::scoped_lock lk{mutex};
        const auto it =
            std::find_if(jobs.begin(), jobs.end(), [owner](const Entry& e) { return e.owner == owner; });
        if (it == jobs.end()) {
            return {};
        }
        Job job = std::move(it->job);
        jobs.erase(it);
        return job;
    }

    [[nodiscard]] std::size_t Size() {
        std::scoped_lock lk{mutex};
        return jobs.size();
    }

private:
    struct Entry {
        const void* owner;
        Job job;
    };
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<Entry> jobs;
};

/// Whether a draw of a pipeline still compiling may be left out now. `first_skip_ns`: when one
/// of its draws was first left out (0: none yet; set here), `budget_ns`: for how long its draws
/// may be left out. Past that they wait for it: a compile that never ends (or a queue that does
/// not move) shows as a stall, not as geometry missing for good.
inline bool WithinSkipBudget(std::uint64_t& first_skip_ns, std::uint64_t now_ns,
                             std::uint64_t budget_ns) noexcept {
    if (first_skip_ns == 0) {
        first_skip_ns = now_ns != 0 ? now_ns : 1;
        return budget_ns != 0;
    }
    return now_ns - first_skip_ns < budget_ns;
}

/// Counted since the last statistics window (BB_FRAME_STATS), exchanged to 0 when printed.
struct PipelineCompileStats {
    std::atomic<std::uint64_t> background{0};        ///< pipelines handed to the compile threads
    std::atomic<std::uint64_t> taken{0};             ///< compiled by the thread that needed them
    std::atomic<std::uint64_t> skipped_draws{0};     ///< draws left out, their pipeline compiling
    std::atomic<std::uint64_t> skipped_pipelines{0}; ///< pipelines whose draws were left out
    std::atomic<std::uint64_t> late_binds{0};        ///< draws bound on the recording threads
    std::atomic<std::uint64_t> waits{0};             ///< threads that waited for a compile
    std::atomic<std::uint64_t> wait_ns{0};
    std::atomic<std::uint64_t> longest_wait_ns{0};

    void NoteWait(std::uint64_t ns) noexcept {
        waits.fetch_add(1, std::memory_order_relaxed);
        wait_ns.fetch_add(ns, std::memory_order_relaxed);
        std::uint64_t longest = longest_wait_ns.load(std::memory_order_relaxed);
        while (ns > longest &&
               !longest_wait_ns.compare_exchange_weak(longest, ns, std::memory_order_relaxed)) {
        }
    }
};

inline PipelineCompileStats pipeline_compile_stats;

} // namespace Vulkan
