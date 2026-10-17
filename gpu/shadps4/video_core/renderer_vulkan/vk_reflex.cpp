// SPDX-License-Identifier: GPL-2.0-or-later
// NVIDIA Reflex (VK_NV_low_latency2 rev3) integration for bbport.

#include "video_core/renderer_vulkan/vk_reflex.h"
#include "video_core/renderer_vulkan/reflex_cadence_policy.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "common/logging/log.h"
#include "bbport_settings.h"

namespace Vulkan::Reflex {

namespace {

typedef VkResult (VKAPI_PTR *PFN_vkSetLatencySleepModeNV)(VkDevice device, VkSwapchainKHR swapchain, const VkLatencySleepModeInfoNV* pSleepModeInfo);
typedef VkResult (VKAPI_PTR *PFN_vkLatencySleepNV)(VkDevice device, VkSwapchainKHR swapchain, const VkLatencySleepInfoNV* pSleepInfo);
typedef void (VKAPI_PTR *PFN_vkSetLatencyMarkerNV)(VkDevice device, VkSwapchainKHR swapchain, const VkSetLatencyMarkerInfoNV* pLatencyMarkerInfo);
typedef void (VKAPI_PTR *PFN_vkGetLatencyTimingsNV)(VkDevice device, VkSwapchainKHR swapchain, VkGetLatencyMarkerInfoNV* pLatencyMarkerInfo);
typedef void (VKAPI_PTR *PFN_vkQueueNotifyOutOfBandNV)(VkQueue queue, const VkOutOfBandQueueTypeInfoNV* pQueueTypeInfo);

PFN_vkSetLatencySleepModeNV g_pfn_set_sleep_mode = nullptr;
PFN_vkLatencySleepNV g_pfn_latency_sleep = nullptr;
PFN_vkSetLatencyMarkerNV g_pfn_set_marker = nullptr;
PFN_vkGetLatencyTimingsNV g_pfn_get_timings = nullptr;
PFN_vkQueueNotifyOutOfBandNV g_pfn_queue_notify_oob = nullptr;

VkInstance g_instance = VK_NULL_HANDLE;
VkPhysicalDevice g_phys = VK_NULL_HANDLE;
VkDevice g_device = VK_NULL_HANDLE;
VkSwapchainKHR g_swapchain = VK_NULL_HANDLE;
VkQueue g_present_queue = VK_NULL_HANDLE;
bool g_marker_attribution = false;

VkSemaphore g_sleep_semaphore = VK_NULL_HANDLE;
uint64_t g_sleep_value = 0;
std::unordered_map<VkQueue, uint64_t> g_queue_ids;
uint64_t g_render_started = 0;
uint64_t g_report_logged = 0;
uint64_t g_input_sampled = 0;
struct CpuSample { uint64_t id{}; int64_t ns{}; };
std::array<CpuSample,512> g_cpu_samples{};

std::mutex g_mu;
std::mutex g_sleep_mu; // serializes sleeps and swapchain/semaphore destruction
std::mutex g_wsi_mu;   // WSI serialization: acquire/recreate/present
bool g_supported = false;
Mode g_mode = Mode::On;
CadencePolicy g_cadence_policy; // protected by g_mu
uint32_t g_min_interval_us = 0;

std::atomic<uint64_t> g_sim_frame_id{0};
std::atomic<bool> g_sim_cycle_active{false};
std::atomic<uint64_t> g_flip_ticket{0};
std::mutex g_cycle_mu; // gates simulation cycle transitions and pacing sleeps
thread_local uint64_t t_recording_frame_id = 0;

Mode parse_settings_or_env_mode() {
    // Environment variable overrides all
    if (const char* env = std::getenv("BB_REFLEX")) {
        if (std::strcmp(env, "0") == 0 || std::strcmp(env, "off") == 0) return Mode::Off;
        if (std::strcmp(env, "1") == 0 || std::strcmp(env, "on") == 0) return Mode::On;
        if (std::strcmp(env, "2") == 0 || std::strcmp(env, "boost") == 0) return Mode::Boost;
    }
    const int s_val = BbSettings::Get().reflex.load();
    if (s_val == BbSettings::ReflexOff) return Mode::Off;
    if (s_val == BbSettings::ReflexBoost) return Mode::Boost;
    return Mode::On;
}

void disable_locked(const char* operation, VkResult result) {
    LOG_ERROR(Render_Vulkan, "Reflex: {} failed ({}); driver pacing and markers disabled", operation, static_cast<int>(result));
    g_supported = false;
    FrameBacklog().Configure(false, Backlog::DefaultCap);
}

/// Applies the current mode and BB_REFLEX_MAX_QUEUED to the process backlog. Callers must not
/// hold the sleep, WSI or g_mu lock in a way that blocks the backlog's own mutex ordering.
void apply_backlog_mode(bool supported, Mode mode) {
    g_cadence_policy.Reset();
    if (!supported || mode == Mode::Off) {
        FrameBacklog().Configure(false, Backlog::DefaultCap);
        return;
    }
    uint32_t cap = Backlog::DefaultCap;
    if (const char* queued = std::getenv("BB_REFLEX_MAX_QUEUED")) {
        cap = static_cast<uint32_t>(std::clamp(std::atoi(queued), 0, 4));
    }
    FrameBacklog().Configure(cap != 0, cap);
}

// Internal helper called under g_sleep_mu + WsiMutex + g_mu
void apply_mode_under_wsi_locked() {
    if (!g_supported || !g_device || !g_swapchain || !g_pfn_set_sleep_mode) return;
    VkLatencySleepModeInfoNV info{};
    info.sType = VK_STRUCTURE_TYPE_LATENCY_SLEEP_MODE_INFO_NV;
    info.pNext = nullptr;
    info.lowLatencyMode = (g_mode != Mode::Off) ? VK_TRUE : VK_FALSE;
    info.lowLatencyBoost = (g_mode == Mode::Boost) ? VK_TRUE : VK_FALSE;
    info.minimumIntervalUs = g_min_interval_us;
    const VkResult r = g_pfn_set_sleep_mode(g_device, g_swapchain, &info);
    if (r != VK_SUCCESS) disable_locked("vkSetLatencySleepModeNV", r);
    LOG_INFO(Render_Vulkan, "Reflex: set latency sleep mode: mode={} boost={} min_interval={} us (result={})",
             static_cast<int>(info.lowLatencyMode), static_cast<int>(info.lowLatencyBoost),
             info.minimumIntervalUs, static_cast<int>(r));
}

// The specification permits markers concurrently with acquire/present. g_mu serializes
// our marker state with OnSwapchainDestroying, which clears g_swapchain before destruction.
// Do not make simulation/render markers wait behind WSI acquisition.
// https://docs.vulkan.org/refpages/latest/refpages/source/vkSetLatencyMarkerNV.html
void emit_marker_locked(uint64_t frame_id, VkLatencyMarkerNV marker) {
    if (!g_supported || !g_device || !g_swapchain || !g_pfn_set_marker) return;
    VkSetLatencyMarkerInfoNV info{};
    info.sType = VK_STRUCTURE_TYPE_SET_LATENCY_MARKER_INFO_NV;
    info.pNext = nullptr;
    info.presentID = frame_id;
    info.marker = marker;
    g_pfn_set_marker(g_device, g_swapchain, &info);
}

} // namespace

std::mutex& SleepMutex() {
    return g_sleep_mu;
}

std::mutex& WsiMutex() {
    return g_wsi_mu;
}

bool ConfigureDevice(vk::PhysicalDevice phys, std::vector<const char*>& exts,
                     vk::PhysicalDeviceVulkan12Features& features12,
                     vk::PhysicalDevicePresentIdFeaturesKHR& present_id) {
    auto [ext_res, available] = phys.enumerateDeviceExtensionProperties();
    if (ext_res != vk::Result::eSuccess) return false;

    bool latency = false, present = false;
    for (const auto& prop : available) {
        const char* name = prop.extensionName.data();
        latency |= (std::strcmp(name, VK_NV_LOW_LATENCY_2_EXTENSION_NAME) == 0 && prop.specVersion >= 3);
        present |= (std::strcmp(name, VK_KHR_PRESENT_ID_EXTENSION_NAME) == 0);
    }
    if (!latency || !present) return false;

    auto chain = phys.getFeatures2<vk::PhysicalDeviceFeatures2,
                                   vk::PhysicalDeviceVulkan12Features,
                                   vk::PhysicalDevicePresentIdFeaturesKHR>();
    const auto& q12 = chain.get<vk::PhysicalDeviceVulkan12Features>();
    const auto& qp = chain.get<vk::PhysicalDevicePresentIdFeaturesKHR>();

    if (!q12.timelineSemaphore || !qp.presentId) return false;

    features12.timelineSemaphore = true;
    present_id.presentId = true;

    for (auto name : {VK_NV_LOW_LATENCY_2_EXTENSION_NAME, VK_KHR_PRESENT_ID_EXTENSION_NAME}) {
        if (std::none_of(exts.begin(), exts.end(), [&](const char* x) { return std::strcmp(x, name) == 0; })) {
            exts.push_back(name);
        }
    }

    LOG_INFO(Render_Vulkan, "Reflex: low_latency2 revision 3, present IDs and timeline semaphores enabled");
    return true;
}

void Initialize(vk::Instance instance, vk::PhysicalDevice phys, vk::Device device, bool extensions_enabled) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_instance = static_cast<VkInstance>(instance);
    g_phys = static_cast<VkPhysicalDevice>(phys);
    g_device = static_cast<VkDevice>(device);

    g_mode = parse_settings_or_env_mode();
    if (const char* interval = std::getenv("BB_REFLEX_INTERVAL_US")) {
        g_min_interval_us = static_cast<uint32_t>(std::clamp(std::atoi(interval), 0, 100000));
    }

    if (!extensions_enabled) {
        g_supported = false;
        apply_backlog_mode(false, g_mode);
        LOG_INFO(Render_Vulkan, "Reflex: extensions not enabled on device; low latency disabled");
        return;
    }

    auto get_proc = VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceProcAddr;
    if (!get_proc) {
        g_supported = false;
        apply_backlog_mode(false, g_mode);
        return;
    }

    g_pfn_set_sleep_mode = reinterpret_cast<PFN_vkSetLatencySleepModeNV>(get_proc(g_device, "vkSetLatencySleepModeNV"));
    g_pfn_latency_sleep = reinterpret_cast<PFN_vkLatencySleepNV>(get_proc(g_device, "vkLatencySleepNV"));
    g_pfn_set_marker = reinterpret_cast<PFN_vkSetLatencyMarkerNV>(get_proc(g_device, "vkSetLatencyMarkerNV"));
    g_pfn_get_timings = reinterpret_cast<PFN_vkGetLatencyTimingsNV>(get_proc(g_device, "vkGetLatencyTimingsNV"));
    g_pfn_queue_notify_oob = reinterpret_cast<PFN_vkQueueNotifyOutOfBandNV>(get_proc(g_device, "vkQueueNotifyOutOfBandNV"));

    g_supported = g_pfn_set_sleep_mode && g_pfn_latency_sleep && g_pfn_set_marker && g_pfn_get_timings && g_pfn_queue_notify_oob;
    if (g_supported) {
        VkSemaphoreTypeCreateInfo type{};
        type.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
        type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        VkSemaphoreCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        ci.pNext = &type;
        auto create_sem = VULKAN_HPP_DEFAULT_DISPATCHER.vkCreateSemaphore;
        VkResult r = create_sem ? create_sem(g_device, &ci, nullptr, &g_sleep_semaphore) : VK_ERROR_INITIALIZATION_FAILED;
        if (r != VK_SUCCESS) {
            disable_locked("timeline semaphore creation", r);
        }
    }
    apply_backlog_mode(g_supported, g_mode);
    LOG_INFO(Render_Vulkan, "Reflex: device initialized: supported={} (mode={})",
             g_supported, g_mode == Mode::Off ? "off" : (g_mode == Mode::Boost ? "on+boost" : "on"));
}

void Shutdown() {
    std::lock_guard<std::mutex> sleep(g_sleep_mu);
    std::lock_guard<std::mutex> lk(g_mu);
    // Wake any guest blocked on the backlog; the presenter is going away.
    FrameBacklog().Configure(false, Backlog::DefaultCap);
    if (g_sleep_semaphore && g_device) {
        VULKAN_HPP_DEFAULT_DISPATCHER.vkDestroySemaphore(g_device, g_sleep_semaphore, nullptr);
        g_sleep_semaphore = VK_NULL_HANDLE;
    }
    g_swapchain = VK_NULL_HANDLE;
    g_device = VK_NULL_HANDLE;
    g_phys = VK_NULL_HANDLE;
    g_instance = VK_NULL_HANDLE;
    g_supported = false;
    g_marker_attribution = false;
    g_present_queue = VK_NULL_HANDLE;
    g_queue_ids.clear();
    g_render_started = 0;
    g_cadence_policy.Reset();
}

bool IsSupported() {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_supported;
}

bool IsActive() {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_supported && g_mode != Mode::Off;
}

Mode CurrentMode() {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_mode;
}

void SetMode(Mode mode, uint32_t minimum_interval_us) {
    // Systematic lock hierarchy: SleepMutex -> WsiMutex -> g_mu
    std::lock_guard<std::mutex> sleep_lk(g_sleep_mu);
    std::lock_guard<std::mutex> wsi_lk(g_wsi_mu);
    std::lock_guard<std::mutex> lk(g_mu);
    g_mode = mode;
    if (minimum_interval_us) g_min_interval_us = minimum_interval_us;
    apply_mode_under_wsi_locked();
    apply_backlog_mode(g_supported, g_mode);
}

void PrepareSwapchainCreateInfo(vk::SwapchainCreateInfoKHR& create_info,
                                VkSwapchainLatencyCreateInfoNV& latency_info) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_supported) return;
    latency_info = {};
    latency_info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_LATENCY_CREATE_INFO_NV;
    latency_info.pNext = create_info.pNext;
    latency_info.latencyModeEnable = (g_mode != Mode::Off) ? VK_TRUE : VK_FALSE;
    create_info.pNext = &latency_info;
}

void OnSwapchainCreated(vk::SwapchainKHR swapchain) {
    // Caller (Swapchain::Create) holds SleepMutex and WsiMutex
    std::lock_guard<std::mutex> lk(g_mu);
    g_swapchain = static_cast<VkSwapchainKHR>(swapchain);
    apply_mode_under_wsi_locked();
}

void OnSwapchainDestroying(vk::SwapchainKHR old_swapchain) {
    // Caller (Swapchain::Destroy) holds SleepMutex and WsiMutex
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_swapchain == static_cast<VkSwapchainKHR>(old_swapchain)) {
        g_swapchain = VK_NULL_HANDLE;
    }
}

void OnPresentQueueCreated(vk::Queue present_queue) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_present_queue = static_cast<VkQueue>(present_queue);
    if (g_supported && g_present_queue && g_pfn_queue_notify_oob) {
        VkOutOfBandQueueTypeInfoNV info{};
        info.sType = VK_STRUCTURE_TYPE_OUT_OF_BAND_QUEUE_TYPE_INFO_NV;
        info.pNext = nullptr;
        info.queueType = VK_OUT_OF_BAND_QUEUE_TYPE_PRESENT_NV;
        g_pfn_queue_notify_oob(g_present_queue, &info);
        LOG_INFO(Render_Vulkan, "Reflex: present queue marked OUT_OF_BAND (present)");
    }
}

void OnGraphicsQueueCreated(vk::Queue graphics_queue, bool independent_presentation) {
    std::lock_guard<std::mutex> lk(g_mu);
    // Shared presentation copies may belong to an older source than the latest
    // render submission. Keep their captured identity through render markers;
    // explicit IDs cannot regress, and clearing them is not exclusion.
    // Independent presentation leaves graphics submissions in monotonic source order, so
    // explicit IDs give the driver accurate GPU work boundaries. Shared queues need markers.
    const char* option = std::getenv("BB_REFLEX_MARKER_ATTRIBUTION");
    g_queue_ids.clear();
    g_render_started = 0;
    g_marker_attribution = (option ? std::strcmp(option, "1") == 0 : !independent_presentation) && g_supported &&
                           graphics_queue && g_pfn_queue_notify_oob;
    if (g_marker_attribution) {
        VkOutOfBandQueueTypeInfoNV info{};
        info.sType = VK_STRUCTURE_TYPE_OUT_OF_BAND_QUEUE_TYPE_INFO_NV;
        info.queueType = VK_OUT_OF_BAND_QUEUE_TYPE_RENDER_NV;
        g_pfn_queue_notify_oob(static_cast<VkQueue>(graphics_queue), &info);
    }
    std::printf("Reflex submission attribution: %s\n",
                g_marker_attribution ? "render markers" : "explicit IDs");
}

bool UsesMarkerAttribution() {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_marker_attribution;
}

uint64_t PrepareSubmit(vk::Queue queue, uint64_t frame_id, vk::SubmitInfo& submit,
                       VkLatencySubmissionPresentIdNV& attribution) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_supported) return 0;
    // A shared queue can copy an older presentation after submitting newer source
    // work. Explicit IDs cannot regress, and zero would inherit another frame.
    // Render-marker attribution names the captured source without that fallback.
    if (g_marker_attribution) return frame_id;
    auto raw_queue = static_cast<VkQueue>(queue);
    // OUT_OF_BAND_PRESENT queues are excluded by the extension. Do not attach zero IDs:
    // zero changes explicit-attribution state rather than excluding an individual submit.
    if (raw_queue == g_present_queue) return frame_id;
    auto& last = g_queue_ids[raw_queue];
    if (!frame_id && last && std::getenv("BB_REFLEX_SUBMIT_TRACE")) {
        std::printf("reflex submit trace: phase=zero-submit previous=%llu\n",
                    static_cast<unsigned long long>(last));
    }
    if (frame_id && frame_id < last) {
        static unsigned logs = 0;
        if (logs++ < 8) {
            LOG_WARNING(Render_Vulkan, "Reflex: stale submission ID {} after {}; retaining monotonic queue attribution",
                        frame_id, last);
        }
        frame_id = last;
    }
    if (frame_id) last = frame_id;
    attribution = {};
    attribution.sType = VK_STRUCTURE_TYPE_LATENCY_SUBMISSION_PRESENT_ID_NV;
    attribution.presentID = frame_id;
    attribution.pNext = submit.pNext;
    submit.pNext = &attribution;
    return frame_id;
}

uint64_t CurrentSimFrameId() {
    return g_sim_frame_id.load(std::memory_order_relaxed);
}

uint64_t AllocateFlipTicket() {
    // Keep tickets unique across mode changes and device recreation.
    return g_flip_ticket.fetch_add(1, std::memory_order_relaxed) + 1;
}

uint64_t ResolveRecordingFrameId(uint64_t current_frame_id, uint64_t submission_frame_id,
                                 bool submission_valid) {
    return submission_valid ? submission_frame_id : current_frame_id;
}

void Backlog::Configure(bool enabled_, uint32_t cap_) {
    std::lock_guard<std::mutex> lk(mutex);
    enabled = enabled_;
    cap = cap_ ? std::min(cap_, MaxCap) : DefaultCap;
    count = 0;
    cv.notify_all();
}

void Backlog::Clear() {
    std::lock_guard<std::mutex> lk(mutex);
    count = 0;
    cv.notify_all();
}

void Backlog::SetCap(uint32_t cap_) {
    std::lock_guard<std::mutex> lk(mutex);
    cap = std::clamp(cap_, 1u, MaxCap);
    cv.notify_all();
}

void Backlog::Register(uint64_t flip_ticket) {
    std::lock_guard<std::mutex> lk(mutex);
    if (!enabled) {
        return; // A disabled gate does not retain guest frames.
    }
    // A direct flip's presentation can complete before SubmitFlip returns, so this exact ticket
    // may already be retired. Compare against the exact retirement cache rather than a
    // high-water mark: tickets are allocated before enqueue, so a newer retirement must not
    // suppress an older flip that is still queued.
    if (retired_tickets[flip_ticket % RetireCacheSize] == flip_ticket) {
        return; // this flip's own presentation already returned
    }
    for (uint32_t i = 0; i < count; ++i) {
        if (ids[i] == flip_ticket) {
            return; // duplicate flip for a frame already in flight
        }
    }
    if (count == Capacity) {
        return; // fixed storage: never overflow
    }
    ids[count++] = flip_ticket;
}

void Backlog::Cancel(uint64_t flip_ticket) {
    std::lock_guard<std::mutex> lk(mutex);
    if (!enabled) {
        return;
    }
    for (uint32_t i = 0; i < count; ++i) {
        if (ids[i] == flip_ticket) {
            for (uint32_t j = i; j + 1 < count; ++j) {
                ids[j] = ids[j + 1];
            }
            --count;
            cv.notify_all();
            return;
        }
    }
}

void Backlog::Retire(uint64_t flip_ticket) {
    std::lock_guard<std::mutex> lk(mutex);
    // Presentation can finish before the guest registers the accepted flip. Record the exact
    // ticket so a later Register of this flip is suppressed, without hiding any other ticket.
    retired_tickets[flip_ticket % RetireCacheSize] = flip_ticket;
    if (!enabled) {
        return;
    }
    for (uint32_t i = 0; i < count; ++i) {
        if (ids[i] == flip_ticket) {
            for (uint32_t j = i; j + 1 < count; ++j) {
                ids[j] = ids[j + 1];
            }
            --count;
            cv.notify_all();
            return;
        }
    }
}

bool Backlog::Gate(uint32_t timeout_ms) {
    std::unique_lock<std::mutex> lk(mutex);
    if (!enabled) {
        return true;
    }
    if (count < cap) {
        return true;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    if (cv.wait_until(lk, deadline, [this] { return !enabled || count < cap; })) {
        return true;
    }
    // The presenter did not retire a frame in time. Drop the stale entries so the guest
    // cannot deadlock; later accepted flips repopulate the backlog.
    count = 0;
    ++timeouts;
    cv.notify_all();
    return false;
}

bool Backlog::Enabled() const {
    std::lock_guard<std::mutex> lk(mutex);
    return enabled;
}

uint32_t Backlog::Outstanding() const {
    std::lock_guard<std::mutex> lk(mutex);
    return count;
}

uint32_t Backlog::Cap() const {
    std::lock_guard<std::mutex> lk(mutex);
    return cap;
}

uint64_t Backlog::Timeouts() const {
    std::lock_guard<std::mutex> lk(mutex);
    return timeouts;
}

Backlog& FrameBacklog() {
    static Backlog backlog;
    return backlog;
}

void OnFlipAccepted(uint64_t flip_ticket) {
    if (!flip_ticket) return;
    FrameBacklog().Register(flip_ticket);
}

void OnFlipCancelled(uint64_t flip_ticket) {
    if (!flip_ticket) return;
    FrameBacklog().Cancel(flip_ticket);
}

void OnPresentRetired(uint64_t flip_ticket) {
    if (!flip_ticket) return;
    FrameBacklog().Retire(flip_ticket);
}

void OnSourceReady(uint64_t flip_ticket) {
    if (!flip_ticket) return;
    FrameBacklog().Retire(flip_ticket);
}

void OnSourceCadence(int target_cap, int64_t period_ns, bool continuous) {
    std::lock_guard<std::mutex> lk(g_mu);
    // Explicit diagnostic overrides retain their exact requested limit (including off).
    if (!g_supported || g_mode == Mode::Off || std::getenv("BB_REFLEX_MAX_QUEUED")) return;
    if (!continuous) g_cadence_policy.Reset();
    const auto cap = continuous ? g_cadence_policy.Update(target_cap, period_ns)
                                : Backlog::DefaultCap;
    if (FrameBacklog().Cap() != cap) {
        FrameBacklog().SetCap(cap);
        if (std::getenv("BB_REFLEX_TIMING")) {
            std::printf("reflex cadence: cap=%u target=%d period_ns=%lld\n",
                        cap, target_cap, static_cast<long long>(period_ns));
        }
    }
}

void SetRecordingFrameId(uint64_t frame_id) {
    t_recording_frame_id = frame_id;
}

uint64_t RecordingFrameId() {
    return t_recording_frame_id;
}

uint64_t OnFrameBegin() {
    std::lock_guard<std::mutex> cycle_lk(g_cycle_mu);
    if (!g_sim_cycle_active.load(std::memory_order_relaxed)) {
        g_sim_cycle_active.store(true, std::memory_order_release);
        const uint64_t fid = g_sim_frame_id.fetch_add(1, std::memory_order_relaxed) + 1;
        std::lock_guard<std::mutex> lk(g_mu);
        emit_marker_locked(fid, VK_LATENCY_MARKER_SIMULATION_START_NV);
        return fid;
    }
    return g_sim_frame_id.load(std::memory_order_relaxed);
}

void PreInputSample() {
    // Only open and pace a simulation cycle if one is not already open.
    // Multiple pad reads across different threads/calls within the same cycle reuse the open ID.
    if (g_sim_cycle_active.load(std::memory_order_acquire)) {
        return;
    }

    std::unique_lock<std::mutex> cycle_lk(g_cycle_mu);
    if (g_sim_cycle_active.load(std::memory_order_relaxed)) {
        return; // another thread already opened this cycle: do not wait on the backlog
    }

    // bbport: bounded guest backlog, inside the cycle gate so two threads cannot both pass
    // the capacity check before either opens a cycle. It is released by Presenter::Present.
    // The backlog has its own mutex, never the sleep/WSI/scheduler locks. A timeout drops
    // stale entries so the guest can never deadlock if the presenter stops.
    static const uint32_t backlog_timeout_ms = [] {
        if (const char* env = std::getenv("BB_REFLEX_BACKLOG_TIMEOUT_MS")) {
            return static_cast<uint32_t>(std::clamp(std::atoi(env), 1, 5000));
        }
        return 150u;
    }();
    static const bool backlog_report = std::getenv("BB_REFLEX_TIMING") != nullptr;
    const auto backlog_started = backlog_report ? std::chrono::steady_clock::now()
                                               : std::chrono::steady_clock::time_point{};
    if (!FrameBacklog().Gate(backlog_timeout_ms) && backlog_report) {
        std::printf("reflex backlog: wait timed out (drops=%llu)\n",
                    static_cast<unsigned long long>(FrameBacklog().Timeouts()));
    }
    if (backlog_report) {
        static auto last_gate_report = std::chrono::steady_clock::time_point{};
        const auto now = std::chrono::steady_clock::now();
        if (now - last_gate_report >= std::chrono::seconds(1)) {
            last_gate_report = now;
            std::printf("reflex gate: took_us=%lld enabled=%u pending=%u cap=%u timeouts=%llu\n",
                        static_cast<long long>(std::chrono::duration_cast<std::chrono::microseconds>(
                            now - backlog_started).count()), unsigned(FrameBacklog().Enabled()),
                        FrameBacklog().Outstanding(), FrameBacklog().Cap(),
                        static_cast<unsigned long long>(FrameBacklog().Timeouts()));
        }
    }

    // Driver pacing sleep before starting simulation / sampling input.
    // Ensure g_sleep_mu protects Vulkan device/swapchain destruction.
    std::unique_lock<std::mutex> sleep_lk(g_sleep_mu);
    std::unique_lock<std::mutex> lk(g_mu);

    static const bool sleep_timing = std::getenv("BB_REFLEX_TIMING") != nullptr;
    static auto last_sleep_report = std::chrono::steady_clock::time_point{};
    if (g_supported && g_mode != Mode::Off && g_device && g_swapchain && g_pfn_latency_sleep && g_sleep_semaphore) {
        VkLatencySleepInfoNV sleep_info{};
        sleep_info.sType = VK_STRUCTURE_TYPE_LATENCY_SLEEP_INFO_NV;
        sleep_info.pNext = nullptr;
        sleep_info.signalSemaphore = g_sleep_semaphore;
        sleep_info.value = ++g_sleep_value;
        const auto device = g_device;
        const auto swapchain = g_swapchain;
        const auto started = std::chrono::steady_clock::now();

        lk.unlock();

        VkResult r;
        {
            // Driver latency sleep requires WSI serialization
            std::lock_guard<std::mutex> wsi(g_wsi_mu);
            r = g_pfn_latency_sleep(device, swapchain, &sleep_info);
        }
        if (r == VK_SUCCESS) {
            // CPU timeline wait: never hold wsi lock or g_mu or input pad lock here
            VkSemaphoreWaitInfo wait{};
            wait.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
            wait.semaphoreCount = 1;
            wait.pSemaphores = &g_sleep_semaphore;
            wait.pValues = &sleep_info.value;
            auto wait_fn = VULKAN_HPP_DEFAULT_DISPATCHER.vkWaitSemaphores;
            if (wait_fn) {
                r = wait_fn(device, &wait, 100000000ull); // 100 ms timeout
            } else {
                r = VK_ERROR_INITIALIZATION_FAILED;
            }
        }

        lk.lock();
        const auto sleep_us = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - started).count();
        const auto finished = std::chrono::steady_clock::now();
        // BB_REFLEX_TIMING reports the driver's release delay so the effectiveness of
        // vkLatencySleepNV can be measured independently of the driver's own reports. A plain
        // printf is used because LOG_INFO is invisible at the default log level, and the
        // zero/immediate-release case is exactly what has to be seen.
        if (finished - last_sleep_report >= std::chrono::seconds(1) &&
            (sleep_timing || sleep_us > 50000)) {
            last_sleep_report = finished;
            if (sleep_timing) {
                std::printf("reflex sleep: took_us=%lld result=%d value=%llu mode=%d\n",
                            static_cast<long long>(sleep_us), static_cast<int>(r),
                            (unsigned long long)sleep_info.value, static_cast<int>(g_mode));
            } else {
                LOG_INFO(Render_Vulkan, "Reflex: latency sleep took {} us",
                         static_cast<long long>(sleep_us));
            }
        }
        if (r != VK_SUCCESS) {
            disable_locked("latency sleep/wait", r);
        }
    }

    // Now open the new simulation cycle with a newly incremented frame ID
    const uint64_t fid = g_sim_frame_id.fetch_add(1, std::memory_order_relaxed) + 1;
    g_sim_cycle_active.store(true, std::memory_order_release);
    emit_marker_locked(fid, VK_LATENCY_MARKER_SIMULATION_START_NV);
}

void OnInputSampled() {
    const uint64_t fid = CurrentSimFrameId();
    if (!fid) return;
    // Timestamp the sample before any marker-state lock contention.
    const bool timing = std::getenv("BB_REFLEX_TIMING") != nullptr;
    const auto sampled_ns = timing
        ? std::chrono::duration_cast<std::chrono::nanoseconds>(
              std::chrono::steady_clock::now().time_since_epoch()).count()
        : 0;
    std::lock_guard<std::mutex> lk(g_mu);
    if (fid <= g_input_sampled) return;
    g_input_sampled = fid;
    if (timing) {
        g_cpu_samples[fid % g_cpu_samples.size()] = {fid, sampled_ns};
    }
    emit_marker_locked(fid, VK_LATENCY_MARKER_INPUT_SAMPLE_NV);
}

void TraceCpuPresent(uint64_t fid) {
    if (!fid || !std::getenv("BB_REFLEX_TIMING")) return;
    const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    CpuSample sample;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        sample = g_cpu_samples[fid % g_cpu_samples.size()];
    }
    if (sample.id == fid && now >= sample.ns) {
        std::printf("reflex cpu: id=%llu sampled_ns=%lld present_ns=%lld latency_ms=%.3f\n",
            (unsigned long long)fid,(long long)sample.ns,(long long)now,(now-sample.ns)/1e6);
    }
}

void OnSimulationEnd(uint64_t captured_frame_id) {
    if (!captured_frame_id) return;
    // Close the simulation cycle on CPU flip consumption so the next PreInputSample can
    // advance. Validate the captured id against the cycle that is actually open *under the
    // cycle gate*, before emitting the marker or clearing the flag: a stale or duplicate flip
    // (one whose cycle already ended, or that belongs to an older cycle) must not emit
    // SIMULATION_END for, or close, a newer cycle the guest has opened since. The lock order
    // matches PreInputSample/OnFrameBegin: g_cycle_mu -> g_mu.
    std::unique_lock<std::mutex> cycle_lk(g_cycle_mu);
    if (!g_sim_cycle_active.load(std::memory_order_relaxed)) {
        return; // no open cycle: stale or duplicate close
    }
    if (g_sim_frame_id.load(std::memory_order_relaxed) != captured_frame_id) {
        return; // the open cycle is a newer one; this flip does not own it
    }
    g_sim_cycle_active.store(false, std::memory_order_release);
    std::lock_guard<std::mutex> lk(g_mu);
    emit_marker_locked(captured_frame_id, VK_LATENCY_MARKER_SIMULATION_END_NV);
}

void OnRenderSubmitBegin(uint64_t frame_id) {
    if (!frame_id) return;
    std::lock_guard<std::mutex> lk(g_mu);
    if (frame_id > g_render_started) {
        g_render_started = frame_id;
        emit_marker_locked(frame_id, VK_LATENCY_MARKER_RENDERSUBMIT_START_NV);
    }
    if (g_marker_attribution) {
        emit_marker_locked(frame_id, VK_LATENCY_MARKER_OUT_OF_BAND_RENDERSUBMIT_START_NV);
    }
}

void OnRenderSubmitEnd(uint64_t frame_id) {
    if (!frame_id) return;
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_marker_attribution) {
        emit_marker_locked(frame_id, VK_LATENCY_MARKER_OUT_OF_BAND_RENDERSUBMIT_END_NV);
    }
    emit_marker_locked(frame_id, VK_LATENCY_MARKER_RENDERSUBMIT_END_NV);
}

void OnForeignSubmitBegin(uint64_t frame_id) {
    if (!frame_id) return;
    std::lock_guard<std::mutex> lk(g_mu);
    emit_marker_locked(frame_id, VK_LATENCY_MARKER_OUT_OF_BAND_RENDERSUBMIT_START_NV);
}

void OnForeignSubmitEnd(uint64_t frame_id) {
    if (!frame_id) return;
    std::lock_guard<std::mutex> lk(g_mu);
    emit_marker_locked(frame_id, VK_LATENCY_MARKER_OUT_OF_BAND_RENDERSUBMIT_END_NV);
}

void OnPresentBegin(uint64_t frame_id, bool is_real) {
    if (!frame_id) return;
    std::lock_guard<std::mutex> lk(g_mu);
    emit_marker_locked(frame_id, is_real ? VK_LATENCY_MARKER_PRESENT_START_NV
                                         : VK_LATENCY_MARKER_OUT_OF_BAND_PRESENT_START_NV);
}

void OnPresentEnd(uint64_t frame_id, bool is_real) {
    if (!frame_id) return;
    std::lock_guard<std::mutex> lk(g_mu);
    emit_marker_locked(frame_id, is_real ? VK_LATENCY_MARKER_PRESENT_END_NV
                                         : VK_LATENCY_MARKER_OUT_OF_BAND_PRESENT_END_NV);
}

bool QueryLatencyTimings(LatencyReport* out_report) {
    if (!out_report) return false;
    // Capture state under SleepMutex -> WsiMutex -> g_mu hierarchy
    std::lock_guard<std::mutex> sleep_lk(g_sleep_mu);
    std::lock_guard<std::mutex> wsi_lk(g_wsi_mu);
    VkDevice dev = VK_NULL_HANDLE;
    VkSwapchainKHR sc = VK_NULL_HANDLE;
    PFN_vkGetLatencyTimingsNV pfn_timings = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (!g_supported || !g_device || !g_swapchain || !g_pfn_get_timings) return false;
        dev = g_device;
        sc = g_swapchain;
        pfn_timings = g_pfn_get_timings;
    }

    VkGetLatencyMarkerInfoNV info{};
    info.sType = VK_STRUCTURE_TYPE_GET_LATENCY_MARKER_INFO_NV;
    pfn_timings(dev, sc, &info);
    if (!info.timingCount || info.timingCount > 4096) return false;

    std::vector<VkLatencyTimingsFrameReportNV> reports(info.timingCount);
    for (auto& report : reports) report.sType = VK_STRUCTURE_TYPE_LATENCY_TIMINGS_FRAME_REPORT_NV;
    info.pTimings = reports.data();
    pfn_timings(dev, sc, &info);

    const VkLatencyTimingsFrameReportNV* latest = nullptr;
    for (uint32_t i = 0; i < std::min(info.timingCount, uint32_t(reports.size())); ++i) {
        const auto& r = reports[i];
        if (r.presentID && r.simStartTimeUs && r.simEndTimeUs && r.renderSubmitStartTimeUs &&
            r.renderSubmitEndTimeUs && r.presentStartTimeUs && r.presentEndTimeUs &&
            r.gpuRenderStartTimeUs && r.gpuRenderEndTimeUs && (!latest || r.presentID > latest->presentID)) {
            latest = &r;
        }
    }
    if (!latest) return false;
    const auto& report = *latest;
    out_report->present_id = report.presentID;
    out_report->input_sample_us = report.inputSampleTimeUs;
    out_report->sim_start_us = report.simStartTimeUs;
    out_report->sim_end_us = report.simEndTimeUs;
    out_report->render_submit_start_us = report.renderSubmitStartTimeUs;
    out_report->render_submit_end_us = report.renderSubmitEndTimeUs;
    out_report->present_start_us = report.presentStartTimeUs;
    out_report->present_end_us = report.presentEndTimeUs;
    out_report->gpu_render_start_us = report.gpuRenderStartTimeUs;
    out_report->gpu_render_end_us = report.gpuRenderEndTimeUs;
    return true;
}

} // namespace Vulkan::Reflex
