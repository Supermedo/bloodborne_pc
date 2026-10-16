// SPDX-License-Identifier: GPL-2.0-or-later
#include "bbport_anti_lag.h"

#include <atomic>
#include <cstdio>

#include "bbport_settings.h"

namespace BbAntiLag {
namespace {
VkDevice device_handle = VK_NULL_HANDLE;
PFN_vkAntiLagUpdateAMD update = nullptr;
/// Real frames presented, and the count when the last input stage was marked.
std::atomic<unsigned long long> presented{0};
std::atomic<unsigned long long> input_at{~0ull};
std::atomic<unsigned long long> input_frames{0};

bool Enabled() {
    return update && BbSettings::Get().anti_lag.load(std::memory_order_relaxed);
}

void Mark(VkAntiLagStageAMD stage, unsigned long long frame) {
    const VkAntiLagPresentationInfoAMD info{
        .sType = VK_STRUCTURE_TYPE_ANTI_LAG_PRESENTATION_INFO_AMD,
        .pNext = nullptr,
        .stage = stage,
        .frameIndex = frame,
    };
    const VkAntiLagDataAMD data{
        .sType = VK_STRUCTURE_TYPE_ANTI_LAG_DATA_AMD,
        .pNext = nullptr,
        .mode = VK_ANTI_LAG_MODE_ON_AMD,
        .maxFPS = 0, // the port's own frame limit applies
        .pPresentationInfo = &info,
    };
    update(device_handle, &data);
}
} // namespace

void Init(VkDevice device, bool supported) {
    device_handle = device;
    update = supported ? reinterpret_cast<PFN_vkAntiLagUpdateAMD>(
                             VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceProcAddr(device, "vkAntiLagUpdateAMD"))
                       : nullptr;
    std::printf("Anti-Lag: %s\n", update ? (BbSettings::Get().anti_lag ? "on (VK_AMD_anti_lag)"
                                                                      : "available, off (anti_lag=0)")
                                         : "not offered by the driver");
}

bool Supported() {
    return update != nullptr;
}

void Input() {
    if (!Enabled()) {
        return;
    }
    // The game reads the pad many times per frame: only the first read after a present
    // starts a new frame's input stage.
    const unsigned long long now = presented.load(std::memory_order_acquire);
    unsigned long long last = input_at.load(std::memory_order_relaxed);
    if (now == last || !input_at.compare_exchange_strong(last, now, std::memory_order_acq_rel)) {
        return;
    }
    Mark(VK_ANTI_LAG_STAGE_INPUT_AMD, input_frames.fetch_add(1, std::memory_order_relaxed) + 1);
}

void Present() {
    if (!Enabled()) {
        return;
    }
    Mark(VK_ANTI_LAG_STAGE_PRESENT_AMD, presented.fetch_add(1, std::memory_order_acq_rel) + 1);
}

} // namespace BbAntiLag
