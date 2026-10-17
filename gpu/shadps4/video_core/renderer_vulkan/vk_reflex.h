// SPDX-License-Identifier: GPL-2.0-or-later
// NVIDIA Reflex (VK_NV_low_latency2 rev3) integration for bbport.

#pragma once

#include <cstdint>
#include <condition_variable>
#include <mutex>
#include <vector>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan::Reflex {

enum class Mode : int {
    Off = 0,
    On = 1,
    Boost = 2
};

struct LatencyReport {
    uint64_t present_id = 0;
    uint64_t input_sample_us = 0;
    uint64_t sim_start_us = 0;
    uint64_t sim_end_us = 0;
    uint64_t render_submit_start_us = 0;
    uint64_t render_submit_end_us = 0;
    uint64_t present_start_us = 0;
    uint64_t present_end_us = 0;
    uint64_t gpu_render_start_us = 0;
    uint64_t gpu_render_end_us = 0;
};

/// Outer lifecycle mutex protecting sleep semaphore and swapchain handle lifetime.
/// Must be acquired BEFORE WsiMutex whenever both are taken.
std::mutex& SleepMutex();

/// Mutex to serialize WSI operations (acquire, present, swapchain recreation) with driver sleep.
/// Never held across CPU sleeps or marker operations.
std::mutex& WsiMutex();

/// Device extension and feature configuration before vkCreateDevice.
bool ConfigureDevice(vk::PhysicalDevice phys, std::vector<const char*>& exts,
                     vk::PhysicalDeviceVulkan12Features& features12,
                     vk::PhysicalDevicePresentIdFeaturesKHR& present_id);

/// Lifecycle on Vulkan device.
void Initialize(vk::Instance instance, vk::PhysicalDevice phys, vk::Device device, bool extensions_enabled);
void Shutdown();

bool IsSupported();
bool IsActive();
Mode CurrentMode();
void SetMode(Mode mode, uint32_t minimum_interval_us = 0);

/// Swapchain creation pNext chain setup (attaches VkSwapchainLatencyCreateInfoNV when active/supported)
void PrepareSwapchainCreateInfo(vk::SwapchainCreateInfoKHR& create_info,
                                VkSwapchainLatencyCreateInfoNV& latency_info);

/// Swapchain and present queue lifecycle
void OnSwapchainCreated(vk::SwapchainKHR swapchain);
void OnSwapchainDestroying(vk::SwapchainKHR old_swapchain);
void OnPresentQueueCreated(vk::Queue present_queue);
void OnGraphicsQueueCreated(vk::Queue graphics_queue, bool independent_presentation);
bool UsesMarkerAttribution();

/// Attaches Reflex present ID attribution to a submission.
/// Safe to call on any queue; returns the effective frame ID (or 0 if stripped/unsupported).
uint64_t PrepareSubmit(vk::Queue queue, uint64_t frame_id, vk::SubmitInfo& submit,
                       VkLatencySubmissionPresentIdNV& attribution);

/// Thread Attribution & Context Tracking
uint64_t CurrentSimFrameId();
void SetRecordingFrameId(uint64_t frame_id);
uint64_t RecordingFrameId();

/// Simulation & Input Tracing
uint64_t OnFrameBegin();
void PreInputSample();
void OnInputSampled();

/// Allocates the next monotonic accepted-flip ticket.
///
/// The bounded backlog registers and retires by ticket, not by the guest simulation content
/// id. A content id can repeat or be reused across accepted flips, which makes `Register`
/// dedupe genuinely distinct accepted flips and undercount the outstanding depth; the ticket
/// counter is global and never reset, so retirement always names its own flip even when
/// allocation and enqueue orders differ.
uint64_t AllocateFlipTicket();

/// Guest flip thread: a flip was accepted for presentation under `flip_ticket`. Registers it in
/// the bounded backlog before the simulation cycle closes.
void OnFlipAccepted(uint64_t flip_ticket);
/// Guest flip thread: a queued flip under `flip_ticket` failed and will never be presented.
void OnFlipCancelled(uint64_t flip_ticket);
/// Presenter thread: the matching source render/evaluation timeline has completed. Releases
/// only the input gate ticket; frame resources remain leased until presentation completes.
void OnSourceReady(uint64_t flip_ticket);
/// Presenter-owned cadence observation; discontinuities restore the two-source limit.
void OnSourceCadence(int target_cap, int64_t period_ns, bool continuous);
/// Presenter thread: a real source frame's presentation call returned. Retires the flip that
/// allocated `flip_ticket` from the bounded backlog, including for minimized, skipped or
/// loading frames.
void OnPresentRetired(uint64_t flip_ticket);

/// Frame identity a command recording batch should be attributed to.
///
/// The guest's simulation identity is only correct at the moment a top-level
/// graphics submission is accepted. Reading it later on the GPU thread would
/// attribute the batch to whichever simulation cycle happens to be open when the
/// batch is finally executed. Callers therefore capture CurrentSimFrameId() at
/// acceptance and hand it back here; `submission_valid` distinguishes an
/// accepted submission (which may legitimately be frame 0) from one that never
/// captured an identity, in which case the live counter is used as a fallback.
uint64_t ResolveRecordingFrameId(uint64_t current_frame_id, uint64_t submission_frame_id,
                                 bool submission_valid);

/// Recording identity state machine shared by the scheduler and its CPU regression tests.
///
/// The rules it encodes are the ones that make marker attribution correct:
///  - accepting a submission applies its captured identity immediately, so the very next
///    allocation records under it (no off-by-one waiting for a later allocation);
///  - the accepted identity persists across every command buffer allocated for that
///    submission, so a later flush in the same submission cannot fall back to the live
///    counter the guest has already advanced;
///  - only a new accepted submission or an explicit SetFrameId replaces it.
class RecordingIdentity {
public:
    /// A top-level submission captured `frame_id` at acceptance.
    void BeginSubmission(uint64_t frame_id) {
        submission_frame_id = frame_id;
        submission_valid = true;
        recording_frame_id = frame_id;
    }

    /// The presenter or another owner sets the identity explicitly.
    void SetFrameId(uint64_t frame_id) {
        recording_frame_id = frame_id;
        submission_valid = false;
    }

    /// A command buffer is allocated and records under the resolved identity.
    uint64_t Allocate(uint64_t live_frame_id) {
        recording_frame_id = ResolveRecordingFrameId(live_frame_id, submission_frame_id,
                                                     submission_valid);
        return recording_frame_id;
    }

    uint64_t RecordingId() const {
        return recording_frame_id;
    }

    /// True while a top-level submission owns the recording identity.
    bool HasSubmission() const {
        return submission_valid;
    }

    /// The identity of the owning submission (only meaningful when HasSubmission()).
    uint64_t SubmissionId() const {
        return submission_frame_id;
    }

private:
    uint64_t recording_frame_id = 0;
    uint64_t submission_frame_id = 0;
    bool submission_valid = false;
};

/// Bounded backlog of accepted source work before input sampling.
///
/// Counts accepted sources until their FG render/evaluation completes, with presentation-time
/// retirement for ordinary/loading frames. Frame leases independently bound presentation
/// run-ahead. It complements vendor latency sleep: the guest registers each accepted flip,
/// and the presenter releases that exact ticket without releasing the frame's GPU resources;
/// `Gate` blocks the next input sample while the backlog is full.
///
/// Threading: `Register`/`Cancel` run on the guest flip thread, `Retire` on the presenter
/// thread and `Gate` on the guest simulation thread. All state is under `mutex`; the class
/// never takes the scheduler, WSI or sleep locks, so callers must keep those out of the way.
class Backlog {
public:
    /// Fixed storage: no allocation on any of these paths.
    static constexpr uint32_t Capacity = 64;
    static constexpr uint32_t DefaultCap = 2;
    static constexpr uint32_t MaxCap = 4;

    /// Enables or disables the gate. Disabling clears the backlog and wakes waiters.
    void Configure(bool enabled, uint32_t cap);
    /// Changes capacity without dropping accepted tickets or retirement history.
    void SetCap(uint32_t cap);
    /// Clears the backlog and wakes waiters (shutdown, unsupported driver, mode off).
    void Clear();

    /// Guest flip thread: the flip that allocated `flip_ticket` was accepted for presentation.
    /// No-op when it was already retired, which can happen because a direct flip's presentation
    /// may complete before `SubmitFlip` returns.
    void Register(uint64_t flip_ticket);
    /// Guest flip thread: the queued flip under `flip_ticket` failed and will never be presented.
    void Cancel(uint64_t flip_ticket);
    /// Presenter thread: source work completed or fallback presentation returned. Idempotent;
    /// this queue slot is independent of ownership of the frame and its SDK resources.
    void Retire(uint64_t flip_ticket);

    /// Guest simulation thread: blocks while `cap` source frames are outstanding. Returns
    /// false when the wait timed out (stale entries were then dropped so the guest cannot
    /// deadlock) and true when there is room or the gate is disabled.
    bool Gate(uint32_t timeout_ms);

    /// Diagnostics.
    bool Enabled() const;
    uint32_t Outstanding() const;
    uint32_t Cap() const;
    uint64_t Timeouts() const;

private:
    mutable std::mutex mutex;
    std::condition_variable cv;
    /// Accepted-flip tickets, oldest first, newest last; removing an entry preserves the order.
    uint64_t ids[Capacity]{};
    uint32_t count = 0; ///< entries in [0, count)
    /// Exact retirement cache, keyed by `ticket % RetireCacheSize`; a slot stores the exact
    /// ticket last retired there. `Register` therefore suppresses only the flip whose own
    /// ticket was already presented. A newer retirement must not hide an older, still-queued
    /// ticket, because tickets are allocated before enqueue and the two orders can differ.
    /// Retained across `Configure`/`Clear` to match nearby late registrations.
    static constexpr uint32_t RetireCacheSize = Capacity;
    uint64_t retired_tickets[RetireCacheSize]{};
    uint64_t timeouts = 0;
    bool enabled = false;
    uint32_t cap = DefaultCap;
};

/// Process-wide backlog used by the guest flip and presenter hooks.
Backlog& FrameBacklog();

/// Diagnostic CPU pad-sample to present-call interval for an explicitly owned
/// guest flip. Excludes driver queuing, scanout and physical input devices.
void TraceCpuPresent(uint64_t content_frame_id);

/// Guest flip thread: close the simulation cycle the flip belongs to.
///
/// `captured_frame_id` is the input-cycle identity captured by the flip, separate from its
/// queue ticket. It is validated against the currently open cycle under the cycle
/// gate before any marker is emitted or the open flag is cleared, so a stale or duplicate
/// close (a cycle that already ended, or a flip from an older cycle) cannot emit
/// SIMULATION_END for, or close, a newer cycle the guest has opened since. A close that does
/// not match the open cycle is a no-op.
void OnSimulationEnd(uint64_t captured_frame_id);

/// Legacy no-argument close kept for out-of-scope callers of the C export
/// (`bbgpu_reflex_on_simulation_end`). It samples the live cycle id, so it preserves the
/// historical behaviour for that path only; the guest flip path must pass the captured id.
inline void OnSimulationEnd() {
    OnSimulationEnd(CurrentSimFrameId());
}

/// Render Queue Attribution
void OnRenderSubmitBegin(uint64_t frame_id);
void OnRenderSubmitEnd(uint64_t frame_id);
void OnForeignSubmitBegin(uint64_t frame_id);
void OnForeignSubmitEnd(uint64_t frame_id);

/// Present Tracking
/// \param is_real true for the application's own presentation; false for a
///        frame-generation presentation that shares the same application-rendered
///        frame. Frame generation presents several images per source frame, so the
///        generated presentations carry the out-of-band markers the driver uses to
///        attribute every vkQueuePresentKHR call to that one frame.
void OnPresentBegin(uint64_t frame_id, bool is_real);
void OnPresentEnd(uint64_t frame_id, bool is_real);

/// Latency diagnostic query
bool QueryLatencyTimings(LatencyReport* out_report);

} // namespace Vulkan::Reflex
