// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
namespace FsrSdk24 {
struct VulkanFeatures {
    bool cooperative_matrix{};
    bool float8{};
    bool full_subgroups{};
    constexpr bool operator==(const VulkanFeatures&) const = default;
};
struct VulkanCoreFeatures {
    bool variable_pointers_storage_buffer{};
    bool variable_pointers{};
    bool memory_model{};
    bool memory_model_device_scope{};
    bool partially_bound{};
    constexpr bool operator==(const VulkanCoreFeatures&) const = default;
};
inline constexpr VulkanCoreFeatures ChooseVulkanCoreFeatures(bool requested,
                                                             VulkanCoreFeatures support) {
    return {requested && support.variable_pointers_storage_buffer,
        requested && support.variable_pointers_storage_buffer && support.variable_pointers,
        requested && support.memory_model,
        requested && support.memory_model && support.memory_model_device_scope,
        requested && support.partially_bound};
}
// Optional accelerators, never an FSR 4.1.2 hardware allow-list. Older AMD GPUs
// remain eligible for the SDK's INT8 implementation without Float8 support.
inline constexpr VulkanFeatures ChooseVulkanFeatures(bool requested,
    bool cooperative_extension, bool cooperative_supported,
    bool float8_extension, bool float8_supported, bool float8_cooperative_supported,
    bool full_subgroups_supported) {
    const bool cooperative = requested && cooperative_extension && cooperative_supported;
    return {cooperative,
        cooperative && float8_extension && float8_supported && float8_cooperative_supported,
        requested && full_subgroups_supported};
}
} // namespace FsrSdk24
