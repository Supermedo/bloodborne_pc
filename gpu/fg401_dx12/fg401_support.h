// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <string>
#include <string_view>
struct Fg401Support {
    bool supported{};
    std::string reason;
    std::string detail{};
};
inline bool IsRx9000(uint32_t vendor, std::string_view name) {
    if (vendor != 0x1002)
        return false;
    const auto pos = name.find("RX 9");
    if (pos == std::string_view::npos || pos + 7 > name.size())
        return false;
    for (size_t i = pos + 3; i < pos + 7; ++i)
        if (name[i] < '0' || name[i] > '9')
            return false;
    return pos + 7 == name.size() || name[pos + 7] == ' ';
}
bool Fg401WindowsSupported();
inline constexpr uint32_t Fg401Version = (4u << 22) | 1u;
inline constexpr uint32_t Fg402Version = (4u << 22) | 2u;
inline const char* Fg401VersionName(uint32_t version) {
    return version == Fg402Version ? "4.0.2" : "4.0.1";
}
inline bool Fg401ProviderMatches(uint64_t id, uint32_t version) {
    // Bit 31 marks a driver provider, rather than a different algorithm version.
    return uint32_t(id >> 32) == 0xf6000000u && (uint32_t(id) & 0x7fffffffu) == version;
}
Fg401Support ProbeFg401(const uint8_t luid[8], uint32_t version = Fg401Version);
inline std::string Fg401SupportMessage(const Fg401Support& s) {
    if (s.reason == "windows11") return "ML-FG requires Windows 11.";
    if (s.reason == "rx9000") return "ML-FG requires a Radeon RX 9000.";
    if (s.reason == "files") return "ML-FG file missing: " + s.detail;
    if (s.reason == "runtime") return "ML-FG runtime load failed: " + s.detail;
    if (s.reason == "vulkan") return "ML-FG requires Vulkan 1.3.";
    if (s.reason == "adapter") return "ML-FG GPU matching failed.";
    if (s.reason == "interop") return "ML-FG sharing/queue support missing.";
    if (s.reason == "dx12") return "ML-FG driver support missing: " + s.detail;
    if (s.reason == "provider") return "ML-FG " + (s.detail.empty() ? std::string("4.0.1") : s.detail) + " provider unavailable.";
    return "ML-FG support check failed.";
}
