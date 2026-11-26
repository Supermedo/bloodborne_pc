// SPDX-License-Identifier: GPL-2.0-or-later
// Set1: readonly parameters at binding0, two fixed read/write history views at binding1/2.
#pragma once
#include "common/assert.h"
#include "shader_recompiler/runtime_info.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include <array>

namespace Vulkan {
inline vk::UniqueDescriptorSetLayout CreateMotionParameterLayout(vk::Device device) {
    std::array<vk::DescriptorSetLayoutBinding, 3> bindings{};
    for (u32 i = 0; i < bindings.size(); ++i)
        bindings[i] = {
            .binding = i,
            .descriptorType = vk::DescriptorType::eStorageBuffer,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eVertex,
        };
    auto [result, layout] =
        device.createDescriptorSetLayoutUnique({.bindingCount = 3U, .pBindings = bindings.data()});
    ASSERT_MSG(result == vk::Result::eSuccess, "OMV parameter descriptor layout failed: {}",
               vk::to_string(result));
    return std::move(layout);
}
} // namespace Vulkan
