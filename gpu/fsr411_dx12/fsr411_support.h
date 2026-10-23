// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <utility>
#include <vulkan/vulkan.h>

struct Fsr411Support {
    bool supported;
    const char* reason;
    std::string detail{};
};
// Shared ordering with bbport_fsr41.py and its generated overlay text catalog.
inline int FsrSupportCode(const Fsr411Support& status) {
    if (status.supported) return 0;
    const std::string_view reason = status.reason;
    if (reason == "hardware") {
        return status.detail.find("AMD GPU required") != std::string::npos ||
                       status.detail.find("unsupported by selected GPU") != std::string::npos ? 1 : 2;
    }
    if (reason == "provider") return 2;
    if (reason == "driver") return 3;
    if (reason == "files" || reason == "download") return 4;
    if (reason == "disabled") return 6;
    if (reason == "interop" || reason == "adapter") return 7;
    if (reason == "checking") return 8;
    return 5;
}
struct FsrSupportCheck {
    const char* id;
    const char* state;
    std::string detail;
};
using FsrSupportTrace = std::vector<FsrSupportCheck>;
inline void FsrCheck(FsrSupportTrace* trace, const char* id, const char* state,
                     std::string detail = {}) {
    if (trace) trace->push_back({id, state, std::move(detail)});
}
// Query the exact adapter/provider without creating an upscaler context or rendering.
Fsr411Support ProbeFsrDx12Interop(VkPhysicalDevice device, FsrSupportTrace* trace = nullptr);
Fsr411Support ProbeFsr411Dx12(const uint8_t luid[8], FsrSupportTrace* trace = nullptr);
Fsr411Support ProbeFsr412Dx12(const uint8_t luid[8], FsrSupportTrace* trace = nullptr);
