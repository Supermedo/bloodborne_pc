// SPDX-License-Identifier: GPL-2.0-or-later
// Standalone CPU regression checks for Vulkan NVIDIA Reflex integration.
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

#include "gpu/bbgpu.h"
#include "gpu/shim/bbport_settings.h"
#include "gpu/shadps4/video_core/renderer_vulkan/reflex_cadence_policy.h"
#include "gpu/shadps4/video_core/renderer_vulkan/vk_reflex.h"

#include "gpu/shim/common/logging/log.h"

// Local test stubs for Common::Log when running outside libbbgpu
namespace Common::Log {
bool ShouldLog(Level /*level*/) {
    return false;
}
void Write(std::string_view /*log_class*/, Level /*level*/, const char* /*file*/, int /*line*/,
           const char* /*function*/, std::string&& /*message*/) {}
} // namespace Common::Log



// Vulkan dynamic dispatcher storage definition for standalone test binary
#include <vulkan/vulkan.hpp>
VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE

// Stubs for bbgpu C exports when linking outside libbbgpu.a
extern "C" {
void bbgpu_reflex_pre_input_sample(void) {
    Vulkan::Reflex::PreInputSample();
}
void bbgpu_reflex_on_input_sampled(void) {
    Vulkan::Reflex::OnInputSampled();
}
void bbgpu_reflex_on_simulation_end(void) {
    Vulkan::Reflex::OnSimulationEnd();
}
uint64_t bbgpu_reflex_current_sim_frame_id(void) {
    return Vulkan::Reflex::CurrentSimFrameId();
}
}

// Adaptive source-queue depth helpers. These only shape observation streams; every
// expectation below is about the depth the policy selects, never about its internals.
namespace {

using Vulkan::Reflex::CadencePolicy;

// Period of a source running exactly at `cap` FPS, in nanoseconds, computed the same way the
// policy derives its own cap period from the selected cap.
std::int64_t CapPeriod(int cap) {
    return 1'000'000'000LL / cap;
}

// `slower` scales the cap period, e.g. 1.05 is a period 5 % slower than the cap.
std::int64_t PeriodAt(int cap, double slower) {
    return static_cast<std::int64_t>(static_cast<double>(CapPeriod(cap)) * slower);
}

// Feeds `count` identical observations and returns the depth selected afterwards.
std::uint32_t Feed(CadencePolicy& policy, int cap, std::int64_t period_ns, int count) {
    std::uint32_t depth = policy.CurrentCap();
    for (int i = 0; i < count; ++i) {
        depth = policy.Update(cap, period_ns);
    }
    return depth;
}

// Feeds cap-tight observations until the one-deep gate is selected, and returns how many
// observations that took (including the first, which only latches the cap).
int TightObservationsToEngage(CadencePolicy& policy, int cap, int limit) {
    int count = 0;
    while (count < limit) {
        ++count;
        if (policy.Update(cap, CapPeriod(cap)) == CadencePolicy::kDepthOne) {
            break;
        }
    }
    return count;
}

} // namespace

int main() {
    using namespace Vulkan::Reflex;

    std::puts("Running test_reflex_cpu...");

    // 1. Initial / unsupported state verification
    assert(!IsSupported());
    assert(!IsActive());
    assert(CurrentSimFrameId() == 0);

    // 2. Pre-input hook pacing and once-per-frame gating verification
    // Calling PreInputSample repeatedly within the SAME simulation cycle must NOT increment frame ID
    PreInputSample();
    const uint64_t fid1 = CurrentSimFrameId();
    assert(fid1 == 1);

    // Explicitly call PreInputSample multiple times within the same frame: must stay on frame 1
    PreInputSample();
    assert(CurrentSimFrameId() == 1);

    PreInputSample();
    assert(CurrentSimFrameId() == 1);

    // Input sampled within the cycle
    OnInputSampled();
    assert(CurrentSimFrameId() == 1);

    // Ending the simulation cycle (flip consumed) allows the next frame to open. The close
    // carries the identity captured at flip acceptance.
    OnSimulationEnd(fid1);
    assert(CurrentSimFrameId() == 1);

    // Now next pre-input sample advances to frame 2 (exactly 1 increment)
    PreInputSample();
    const uint64_t fid2 = CurrentSimFrameId();
    assert(fid2 == 2);

    // Multiple calls within frame 2 stay on frame 2
    PreInputSample();
    assert(CurrentSimFrameId() == 2);

    OnInputSampled();
    OnSimulationEnd(fid2);

    // 2b. Owned-cycle closure. A close is validated against the cycle that is actually open,
    // under the cycle gate, before it emits SIMULATION_END or clears the open flag. A stale or
    // duplicate close must therefore never close a newer cycle the guest has opened since.
    {
        // Cycle A opens, is sampled, and is closed by its own captured id.
        PreInputSample();
        const uint64_t cycle_a = CurrentSimFrameId();
        assert(cycle_a == fid2 + 1);
        OnInputSampled();
        OnSimulationEnd(cycle_a);
        assert(CurrentSimFrameId() == cycle_a);

        // A duplicate close of the cycle that just ended is a no-op and must not corrupt the
        // state: the next pad read still opens exactly one new cycle.
        OnSimulationEnd(cycle_a);

        // Cycle B opens. A stale close for cycle A must not close or mark cycle B.
        PreInputSample();
        const uint64_t cycle_b = CurrentSimFrameId();
        assert(cycle_b == cycle_a + 1);
        OnSimulationEnd(cycle_a);
        // Still open: a pad read inside cycle B must not advance the id.
        PreInputSample();
        assert(CurrentSimFrameId() == cycle_b);

        // A close that matches neither the open id nor any closed id is also a no-op.
        OnSimulationEnd(cycle_b + 7);
        PreInputSample();
        assert(CurrentSimFrameId() == cycle_b);

        // A zero id can never close an open cycle.
        OnSimulationEnd(0);
        PreInputSample();
        assert(CurrentSimFrameId() == cycle_b);

        // The owning close ends cycle B; only then does the next pad read advance.
        OnSimulationEnd(cycle_b);
        PreInputSample();
        assert(CurrentSimFrameId() == cycle_b + 1);
        OnSimulationEnd(CurrentSimFrameId());
    }

    // 2c. A thread spamming stale/foreign closes must not close the active cycle, must not
    // corrupt its markers, and must not deadlock against the owning close.
    {
        const uint64_t stale_start = CurrentSimFrameId();
        std::atomic<bool> stop{false};
        std::thread stale_closer([&] {
            // Ids that never become the open cycle: a fixed sentinel plus an old id.
            while (!stop.load(std::memory_order_relaxed)) {
                OnSimulationEnd(0x5EED);
                OnSimulationEnd(stale_start);
            }
        });
        constexpr uint64_t kWantedCycles = 300;
        for (uint64_t i = 0; i < kWantedCycles; ++i) {
            PreInputSample();
            const uint64_t fid = CurrentSimFrameId();
            OnInputSampled();
            OnSimulationEnd(fid);
        }
        stop.store(true, std::memory_order_relaxed);
        stale_closer.join();
        assert(CurrentSimFrameId() == stale_start + kWantedCycles);
    }

    // 3. Thread attribution & context tracking
    SetRecordingFrameId(fid2);
    assert(RecordingFrameId() == fid2);
    SetRecordingFrameId(0);
    assert(RecordingFrameId() == 0);

    // 4. Systematic Lock Hierarchy & Deadlock-Free Concurrency Test
    // Hierarchy: SleepMutex -> WsiMutex
    // Test: Thread A holds SleepMutex and operates, Thread B acquires SleepMutex then WsiMutex (like Swapchain::Recreate)
    // while Thread C performs PreInputSample. Neither thread should deadlock.
    {
        std::atomic<bool> thread_started{false};
        std::atomic<bool> test_done{false};

        std::thread recreate_thread([&] {
            thread_started = true;
            for (int i = 0; i < 50; ++i) {
                std::lock_guard<std::mutex> sleep_lk{SleepMutex()};
                std::lock_guard<std::mutex> wsi_lk{WsiMutex()};
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
            test_done = true;
        });

        while (!thread_started) {
            std::this_thread::yield();
        }

        while (!test_done) {
            PreInputSample();
            OnInputSampled();
            OnSimulationEnd();
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }

        recreate_thread.join();
    }

    // 5. C API hooks (bbgpu_reflex_*) verification
    bbgpu_reflex_pre_input_sample();
    const uint64_t c_fid_curr = bbgpu_reflex_current_sim_frame_id();
    assert(c_fid_curr > 2);

    // Re-entrant calls stay at current frame
    bbgpu_reflex_pre_input_sample();
    assert(bbgpu_reflex_current_sim_frame_id() == c_fid_curr);

    bbgpu_reflex_on_input_sampled();
    bbgpu_reflex_on_simulation_end();

    // 6. Submission attribution sanitization without GPU/driver
    vk::SubmitInfo submit{};
    VkLatencySubmissionPresentIdNV attribution{};
    const uint64_t prep_id = PrepareSubmit(nullptr, c_fid_curr, submit, attribution);
    // Unsupported driver path returns 0 and does not corrupt submission
    assert(prep_id == 0);

    // 6b. Recording identity: a submission captured at frame 41 must be applied
    // immediately and must survive every later allocation of that submission, even while the
    // live counter the guest has advanced to (42, 43) changes underneath it. A new submission
    // captured at 44 replaces it. A batch that never captured an identity keeps the live value.
    {
        RecordingIdentity id;

        // Nothing captured yet: allocation follows the live counter.
        assert(id.Allocate(40) == 40);
        assert(id.RecordingId() == 40);

        // Submission accepted at 41. It must apply immediately, before any allocation.
        id.BeginSubmission(41);
        assert(id.RecordingId() == 41);
        assert(id.HasSubmission() && id.SubmissionId() == 41);

        // Several command buffers are allocated for the same submission while the live
        // counter advances. Every one of them must stay on 41.
        assert(id.Allocate(42) == 41);
        assert(id.Allocate(43) == 41);
        assert(id.RecordingId() == 41);

        // Next source boundary: a submission captured at 44 applies immediately and persists.
        id.BeginSubmission(44);
        assert(id.RecordingId() == 44);
        assert(id.Allocate(45) == 44);
        assert(id.Allocate(46) == 44);

        // An explicit owner (presenter) overrides and drops the submission capture.
        id.SetFrameId(90);
        assert(id.RecordingId() == 90);
        assert(!id.HasSubmission());
        assert(id.Allocate(91) == 91);

        // A captured frame 0 is legitimate and must not fall back to a later live value.
        id.BeginSubmission(0);
        assert(id.Allocate(92) == 0);
        assert(id.Allocate(93) == 0);
    }

    // The pure resolver keeps its documented meaning.
    assert(ResolveRecordingFrameId(42, 7, true) == 7);
    assert(ResolveRecordingFrameId(42, 0, true) == 0);
    assert(ResolveRecordingFrameId(42, 7, false) == 42);

    // 6c. Bounded guest backlog. The guest may have at most `cap` accepted source frames
    // outstanding; the presenter retires each one when its present call returns.
    {
        Backlog backlog;
        backlog.Configure(true, 2);
        assert(backlog.Enabled() && backlog.Cap() == 2);

        // Cap clamp and disable.
        backlog.Configure(true, 99);
        assert(backlog.Cap() == Backlog::MaxCap);
        backlog.Configure(false, 2);
        assert(!backlog.Enabled());
        assert(backlog.Gate(1)); // disabled gate never blocks
        backlog.Configure(true, 2);

        // A direct flip may present before SubmitFlip returns: retire first, then register.
        // The registration must be a no-op so the stale id cannot occupy a slot.
        backlog.Retire(50);
        backlog.Register(50);
        assert(backlog.Outstanding() == 0);

        // Two in flight fill the cap; the gate blocks until one is retired.
        backlog.Register(51);
        backlog.Register(52);
        assert(backlog.Outstanding() == 2);
        assert(!backlog.Gate(5)); // times out, drops the stale entries
        assert(backlog.Outstanding() == 0);
        assert(backlog.Timeouts() == 1);

        backlog.Register(53);
        backlog.Register(54);
        backlog.Retire(53);
        assert(backlog.Outstanding() == 1);
        assert(backlog.Gate(5)); // room again

        // Duplicate registration of a frame already in flight must not consume another slot.
        // FIFO order: list is [54]; register 55 twice -> [54, 55].
        backlog.Register(55);
        backlog.Register(55);
        assert(backlog.Outstanding() == 2);
        backlog.Retire(55);
        assert(backlog.Outstanding() == 1);

        // FIFO retirement: retiring the older 54 leaves the newer 55 in place; a new 56 can
        // then register and be retired, and the list must be empty.
        backlog.Retire(54);
        assert(backlog.Outstanding() == 0);
        backlog.Register(56);
        backlog.Retire(56);
        assert(backlog.Outstanding() == 0);
        backlog.Retire(56); // already retired: harmless
        assert(backlog.Outstanding() == 0);

        // Sparse ids: only registered ones count.
        backlog.Register(61);
        backlog.Register(65);
        assert(backlog.Outstanding() == 2);
        backlog.Retire(65);
        assert(backlog.Outstanding() == 1);
        backlog.Retire(61);
        assert(backlog.Outstanding() == 0);

        // A queued EOP flip that fails is cancelled by its exact id.
        backlog.Register(66);
        backlog.Register(67);
        backlog.Cancel(66);
        assert(backlog.Outstanding() == 1);
        backlog.Cancel(66); // idempotent
        assert(backlog.Outstanding() == 1);
        backlog.Retire(67);
        assert(backlog.Outstanding() == 0);

        // Disabling clears the backlog and wakes a blocked gate.
        backlog.Register(68);
        backlog.Register(69);
        assert(backlog.Outstanding() == 2);
        std::thread unblock([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            backlog.Configure(false, 2);
        });
        assert(backlog.Gate(500)); // returns true once disabled
        unblock.join();
        assert(!backlog.Enabled() && backlog.Outstanding() == 0);
        // A mode transition must not forget a presentation which preceded registration.
        backlog.Retire(70);
        backlog.Configure(true, 2);
        backlog.Register(70);
        assert(backlog.Outstanding() == 0);
    }

    // 6d. A retire that arrives while the gate is blocked wakes it (two in flight, retire one).
    {
        Backlog backlog;
        backlog.Configure(true, 2);
        backlog.Register(90);
        backlog.Register(91);
        std::thread retire([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            backlog.Retire(90);
        });
        assert(backlog.Gate(500));
        retire.join();
        assert(backlog.Outstanding() == 1);
        backlog.Retire(91);
        assert(backlog.Outstanding() == 0);
    }

    // 6e. Accepted-flip tickets. The bounded backlog registers/retires by a monotonic ticket,
    // kept separate from the guest simulation content id (which can repeat across accepted
    // flips). Tickets are allocated before enqueue and the allocate/enqueue orders can differ
    // across guest threads, so retirement is matched exactly rather than by a high-water mark.
    {
        // Strictly increasing and therefore unique; never reset by a mode change.
        uint64_t last_ticket = AllocateFlipTicket();
        assert(last_ticket > 0);
        for (int i = 0; i < 300; ++i) {
            const uint64_t next_ticket = AllocateFlipTicket();
            assert(next_ticket > last_ticket);
            last_ticket = next_ticket;
        }

        // Two accepted flips that share one simulation cycle still get distinct tickets and
        // must both occupy backlog slots (no content-id-style dedupe).
        const uint64_t cycle_ticket_a = AllocateFlipTicket();
        const uint64_t cycle_ticket_b = AllocateFlipTicket();
        assert(cycle_ticket_b > cycle_ticket_a);

        const uint64_t older_ticket = AllocateFlipTicket();
        const uint64_t newer_ticket = AllocateFlipTicket();
        assert(newer_ticket > older_ticket);

        {
            Backlog backlog;
            backlog.Configure(true, 4);

            backlog.Register(cycle_ticket_a);
            backlog.Register(cycle_ticket_b);
            assert(backlog.Outstanding() == 2);

            // A newer ticket retiring first must not suppress an older ticket whose Register has
            // not arrived yet, and the already-retired newer ticket is suppressed exactly.
            backlog.Retire(newer_ticket);
            backlog.Register(older_ticket);
            assert(backlog.Outstanding() == 3);
            backlog.Register(newer_ticket);
            assert(backlog.Outstanding() == 3);

            // Exact retirement is idempotent.
            backlog.Retire(newer_ticket);
            assert(backlog.Outstanding() == 3);

            backlog.Retire(cycle_ticket_a);
            backlog.Retire(cycle_ticket_b);
            backlog.Retire(older_ticket);
            assert(backlog.Outstanding() == 0);
        }
        {
            // Retained across Configure: a retirement recorded before the mode change is not
            // forgotten, so a late Register of the same exact ticket is still suppressed.
            Backlog backlog;
            backlog.Configure(true, 2);
            backlog.Retire(newer_ticket);
            backlog.Configure(true, 2);
            backlog.Register(newer_ticket);
            assert(backlog.Outstanding() == 0);
        }
    }

    // Render completion can unblock the next sample before paced presentation. The later
    // endpoint must neither consume another frame's ticket nor resurrect a completed ticket
    // when the guest registers it late.
    {
        auto& backlog = FrameBacklog();
        backlog.Configure(true, 2);
        const auto rendered = AllocateFlipTicket();
        const auto waiting = AllocateFlipTicket();
        OnFlipAccepted(rendered);
        OnFlipAccepted(waiting);
        assert(backlog.Outstanding() == 2);
        std::atomic<bool> released = false;
        std::thread input([&] { released = backlog.Gate(1000); });
        OnSourceReady(rendered);
        input.join();
        assert(released && backlog.Outstanding() == 1);
        OnPresentRetired(rendered);
        OnFlipAccepted(rendered);
        assert(backlog.Outstanding() == 1);
        OnPresentRetired(waiting); // Loading/non-FG fallback still frees the slot.
        assert(backlog.Outstanding() == 0);
        const auto late = AllocateFlipTicket();
        OnSourceReady(late);
        OnFlipAccepted(late);
        OnPresentRetired(late);
        assert(backlog.Outstanding() == 0);
        backlog.Configure(false, 2);
    }

    // 6f. Adaptive accepted-source queue depth. The policy chooses between a two-deep backlog
    // gate (slack while the GPU is the limiter) and a one-deep gate (lower guest/CPU queueing
    // cost while the source reliably hits its selected cap). Every check below is about the
    // depth that comes out for a given stream of observations.
    {
        // Default, and the ~2 s sustained window that earns the one-deep gate. The very first
        // observation only latches the cap; the transition sample itself is ambiguous.
        CadencePolicy policy;
        assert(policy.CurrentCap() == CadencePolicy::kDepthTwo);
        assert(policy.Update(60, CapPeriod(60)) == CadencePolicy::kDepthTwo);

        const int observations = TightObservationsToEngage(policy, 60, 1000);
        const std::int64_t elapsed = static_cast<std::int64_t>(observations) * CapPeriod(60);
        assert(policy.CurrentCap() == CadencePolicy::kDepthOne);
        assert(elapsed >= CadencePolicy::kEngageSustainedNs);
        assert(elapsed < CadencePolicy::kEngageSustainedNs + 3 * CapPeriod(60));
        assert(observations > 100); // dozens of frames are not "sustained"

        // Engagement is measured in time, not samples: 30 and 120 Hz need their own window and
        // must each be judged against their own period. A tenth of the window is never enough.
        for (const int cap : {30, 60, 120, 144}) {
            CadencePolicy per_cap;
            const int needed = TightObservationsToEngage(per_cap, cap, 2000);
            const std::int64_t held = static_cast<std::int64_t>(needed) * CapPeriod(cap);
            assert(per_cap.CurrentCap() == CadencePolicy::kDepthOne);
            assert(held >= CadencePolicy::kEngageSustainedNs);
            assert(held < CadencePolicy::kEngageSustainedNs + 3 * CapPeriod(cap));

            CadencePolicy early;
            Feed(early, cap, CapPeriod(cap), 1);
            Feed(early, cap, CapPeriod(cap), needed / 10);
            assert(early.CurrentCap() == CadencePolicy::kDepthTwo);
        }

        // Eight consecutive GPU-limited periods restore the two-deep gate, and the streak is
        // genuinely consecutive: one cap-tight period in the middle clears it.
        const std::int64_t slow60 = PeriodAt(60, 1.05); // 5 % slower than the cap
        assert(Feed(policy, 60, slow60, 7) == CadencePolicy::kDepthOne);
        assert(policy.Update(60, CapPeriod(60)) == CadencePolicy::kDepthOne);
        assert(Feed(policy, 60, slow60, 7) == CadencePolicy::kDepthOne);
        assert(policy.Update(60, slow60) == CadencePolicy::kDepthTwo);
        assert(policy.CurrentCap() == CadencePolicy::kDepthTwo);

        // After a demotion the one-deep gate has to be earned again in full.
        Feed(policy, 60, CapPeriod(60), 100);
        assert(policy.CurrentCap() == CadencePolicy::kDepthTwo);
        TightObservationsToEngage(policy, 60, 1000);
        assert(policy.CurrentCap() == CadencePolicy::kDepthOne);
    }

    // 6g. Isolated jitter must not thrash either decision.
    {
        // A period 2.5 % slower than the cap is a miss (outside the 2 % band) but not a
        // GPU-limited sample (inside the 3 % band), so it must never demote the one-deep gate
        // no matter how often it repeats.
        CadencePolicy jitter;
        Feed(jitter, 60, CapPeriod(60), 200);
        assert(jitter.CurrentCap() == CadencePolicy::kDepthOne);
        const std::int64_t slight = PeriodAt(60, 1.025);
        for (int i = 0; i < 500; ++i) {
            assert(jitter.Update(60, slight) == CadencePolicy::kDepthOne);
            assert(jitter.Update(60, CapPeriod(60)) == CadencePolicy::kDepthOne);
        }

        // Conversely, a stream of interleaved cap-tight and jittery periods can never talk the
        // policy into one-deep: every miss clears the sustained history first.
        CadencePolicy noisy;
        for (int i = 0; i < 2000; ++i) {
            assert(noisy.Update(60, CapPeriod(60)) == CadencePolicy::kDepthTwo);
            assert(noisy.Update(60, slight) == CadencePolicy::kDepthTwo);
        }

        // The bands are tight but not razor-thin: a period a hair inside 2 % keeps
        // accumulating, and one a hair outside clears the history instead of demoting.
        const std::int64_t cap_period = CapPeriod(60);
        const std::int64_t inside = cap_period + cap_period * CadencePolicy::kCapTolerancePermille / 1000;
        const std::int64_t outside = inside + 1;

        CadencePolicy edge;
        Feed(edge, 60, cap_period, 1);
        Feed(edge, 60, inside, 121);
        assert(edge.CurrentCap() == CadencePolicy::kDepthOne);

        CadencePolicy boundary;
        Feed(boundary, 60, cap_period, 1);
        Feed(boundary, 60, inside, 100);
        assert(boundary.CurrentCap() == CadencePolicy::kDepthTwo);
        assert(boundary.Update(60, outside) == CadencePolicy::kDepthTwo);
        // The just-outside sample cleared the sustained history: another 119 tight periods
        // still fall short of the window, which would not be true had they accumulated.
        Feed(boundary, 60, cap_period, 119);
        assert(boundary.CurrentCap() == CadencePolicy::kDepthTwo);
    }

    // 6h. Cap changes, unlimited caps, invalid periods and Reset all fall back to two-deep
    // immediately and require a fresh window.
    {
        CadencePolicy changes;
        Feed(changes, 60, CapPeriod(60), 200);
        assert(changes.CurrentCap() == CadencePolicy::kDepthOne);

        // A different cap is an immediate demotion even when the new cap's own period is also
        // cap-tight: the collected history described the old cap.
        assert(changes.Update(120, CapPeriod(120)) == CadencePolicy::kDepthTwo);
        Feed(changes, 120, CapPeriod(120), 400);
        assert(changes.CurrentCap() == CadencePolicy::kDepthOne);
        assert(changes.Update(60, CapPeriod(60)) == CadencePolicy::kDepthTwo);

        // Unlimited (0 or negative) is never a basis for adapting.
        Feed(changes, 60, CapPeriod(60), 200);
        assert(changes.CurrentCap() == CadencePolicy::kDepthOne);
        assert(changes.Update(0, CapPeriod(60)) == CadencePolicy::kDepthTwo);
        assert(changes.Update(-1, CapPeriod(60)) == CadencePolicy::kDepthTwo);
        assert(changes.CurrentCap() == CadencePolicy::kDepthTwo);

        // Invalid periods (missing measurement, or absurdly long) demote immediately and clear
        // the sustained history.
        Feed(changes, 60, CapPeriod(60), 200);
        assert(changes.CurrentCap() == CadencePolicy::kDepthOne);
        assert(changes.Update(60, 0) == CadencePolicy::kDepthTwo);
        assert(changes.Update(60, -1) == CadencePolicy::kDepthTwo);
        assert(changes.Update(60, CadencePolicy::kMaxPeriodNs + 1) == CadencePolicy::kDepthTwo);
        Feed(changes, 60, CapPeriod(60), 119);
        assert(changes.CurrentCap() == CadencePolicy::kDepthTwo);
        Feed(changes, 60, CapPeriod(60), 3);
        assert(changes.CurrentCap() == CadencePolicy::kDepthOne);

        // Explicit Reset returns to the default and forgets the latched cap.
        changes.Reset();
        assert(changes.CurrentCap() == CadencePolicy::kDepthTwo);
        assert(changes.Update(60, CapPeriod(60)) == CadencePolicy::kDepthTwo);
    }

    // 6i. Bounded long run: the depth is always one of the two legal values, a saturated slow
    // streak cannot wrap its counter, and a long cap-tight run still engages.
    {
        CadencePolicy soak;
        uint32_t seed = 12345;
        auto next = [&seed] {
            seed = (seed * 1103515245u + 12345u) & 0x7fffffff;
            return seed;
        };

        for (int cap = 30; cap <= 60; ++cap) {
            soak.Reset();
            for (int i = 0; i < 4000; ++i) {
                std::int64_t period = CapPeriod(cap);
                switch (next() % 4) {
                case 0: break;                                // cap-tight
                case 1: period = PeriodAt(cap, 1.002); break;  // inside the band
                case 2: period = PeriodAt(cap, 1.025); break;   // miss, not GPU-limited
                default: period = PeriodAt(cap, 1.05); break;  // GPU-limited
                }
                const std::uint32_t depth = soak.Update(cap, period);
                assert(depth == CadencePolicy::kDepthOne || depth == CadencePolicy::kDepthTwo);
            }
            // Whatever the load history, a sustained cap-tight run at this cap must engage.
            assert(Feed(soak, cap, CapPeriod(cap), 400) == CadencePolicy::kDepthOne);
        }

        // A very long GPU-limited run saturates the streak without wrapping: the gate stays
        // two-deep throughout and one tight sample cannot bring one-deep straight back.
        soak.Reset();
        Feed(soak, 60, CapPeriod(60), 1);
        for (int i = 0; i < 100000; ++i) {
            assert(soak.Update(60, PeriodAt(60, 1.05)) == CadencePolicy::kDepthTwo);
        }
        assert(soak.Update(60, CapPeriod(60)) == CadencePolicy::kDepthTwo);
    }

    // 6j. Adaptive backlog depth. Raising the gate cap must wake a blocked input sample exactly
    // once for the newly available slot; lowering it must preserve the accepted flips already in
    // flight (no silent drops) and make the gate wait until the surplus above the new cap has
    // been retired.
    {
        auto& backlog = FrameBacklog();
        backlog.Configure(true, 1);
        assert(backlog.Cap() == 1);

        // One accepted flip fills the one-deep gate.
        const uint64_t first = AllocateFlipTicket();
        backlog.Register(first);
        assert(backlog.Outstanding() == 1);

        // A gate blocked on the full one-deep backlog is woken by raising the cap to two.
        std::atomic<bool> admitted{false};
        std::thread waiter([&] { admitted = backlog.Gate(2000); });
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        backlog.SetCap(2);
        waiter.join();
        assert(admitted);
        assert(backlog.Cap() == 2);
        assert(backlog.Outstanding() == 1);

        // A second accepted flip is admitted under the raised cap.
        const uint64_t second = AllocateFlipTicket();
        backlog.Register(second);
        assert(backlog.Outstanding() == 2);

        // Lowering back to one keeps both flips in flight and blocks the gate until the surplus
        // above the new cap is retired.
        backlog.SetCap(1);
        assert(backlog.Cap() == 1);
        assert(backlog.Outstanding() == 2);

        std::atomic<bool> finished{false};
        std::atomic<bool> released{false};
        std::thread lowered([&] {
            released = backlog.Gate(2000);
            finished = true;
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        assert(!finished.load()); // two in flight against a one-deep gate
        backlog.Retire(first);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        assert(!finished.load()); // one in flight is still not below the cap
        backlog.Retire(second);
        lowered.join();
        assert(finished.load());
        assert(released);
        assert(backlog.Outstanding() == 0);

        // A cap change must not disturb the pending set when nothing is blocked.
        const uint64_t third = AllocateFlipTicket();
        backlog.Register(third);
        backlog.SetCap(2);
        assert(backlog.Outstanding() == 1);
        backlog.Retire(third);
        assert(backlog.Outstanding() == 0);
        backlog.Configure(false, 2);
    }

    // 7. Stubs for present and render markers do not crash when unsupported
    OnRenderSubmitBegin(c_fid_curr);
    OnRenderSubmitEnd(c_fid_curr);
    OnForeignSubmitBegin(c_fid_curr);
    OnForeignSubmitEnd(c_fid_curr);
    OnPresentBegin(c_fid_curr, true);
    OnPresentEnd(c_fid_curr, true);
    OnPresentBegin(c_fid_curr, false);
    OnPresentEnd(c_fid_curr, false);

    // 8. Timings query stub
    LatencyReport report{};
    assert(!QueryLatencyTimings(&report));

    // 9. Swapchain latency create info setup stub
    vk::SwapchainCreateInfoKHR sc_ci{};
    VkSwapchainLatencyCreateInfoNV lat_ci{};
    PrepareSwapchainCreateInfo(sc_ci, lat_ci);

    // 10. Multi-threaded pad sample simulation
    // Verify that multiple threads polling pad simultaneously never leak duplicate frame IDs
    const uint64_t sim_before = CurrentSimFrameId();
    PreInputSample();
    const uint64_t sim_active = CurrentSimFrameId();
    assert(sim_active == sim_before + 1);

    for (int t = 0; t < 4; ++t) {
        std::thread([&] {
            bbgpu_reflex_pre_input_sample();
            bbgpu_reflex_on_input_sampled();
        }).join();
    }
    // Still sim_active because OnSimulationEnd has not been called!
    assert(CurrentSimFrameId() == sim_active);
    OnSimulationEnd();

    // Next PreInputSample moves to sim_active + 1
    PreInputSample();
    assert(CurrentSimFrameId() == sim_active + 1);
    OnSimulationEnd();

    // Accepted flip boundaries advance attribution even when the next frame does not
    // read the pad. A duplicate close must leave the prepared cycle untouched.
    PreInputSample();
    uint64_t boundary_id = CurrentSimFrameId();
    for (int i = 0; i < 100; ++i) {
        OnSimulationEnd(boundary_id);
        PreInputSample();
        const uint64_t next_id = CurrentSimFrameId();
        assert(next_id == boundary_id + 1);
        OnSimulationEnd(boundary_id);
        PreInputSample();
        assert(CurrentSimFrameId() == next_id);
        boundary_id = next_id;
    }
    OnSimulationEnd(boundary_id);

    // 11. Shutdown cleans up gracefully
    Shutdown();
    assert(!IsSupported());
    assert(!IsActive());

    std::puts("Vulkan NVIDIA Reflex CPU integration tests: PASS");
    return 0;
}
