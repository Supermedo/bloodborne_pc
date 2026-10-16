// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <utility>
#include <vk_mem_alloc.h>
#include "video_core/renderer_vulkan/vk_instance.h"

namespace Vulkan {
// Owned images are reused only after their render and last presentation complete.
// Allocation failure disables generation rather than crashing ordinary presentation.
struct FgImage {
    vk::Device device{};
    VmaAllocator allocator{};
    vk::Image image{};
    vk::ImageView view{};
    VmaAllocation allocation{};
    u32 width{}, height{};
    vk::Format format{};
    bool initialized{};
    FgImage() = default;
    FgImage(const FgImage&) = delete;
    FgImage& operator=(const FgImage&) = delete;
    ~FgImage() { Reset(); }
    void Reset() {
        if (view) device.destroyImageView(view);
        if (image) vmaDestroyImage(allocator, image, allocation);
        image = nullptr; view = nullptr; allocation = nullptr;
        width = height = 0; initialized = false;
    }
    bool Ensure(const Instance& instance, u32 w, u32 h, vk::Format f, bool storage=true) {
        if (image && width == w && height == h && format == f) return true;
        Reset();
        if (!w || !h) return false;
        device = instance.GetDevice(); allocator = instance.GetAllocator();
        const VkImageCreateInfo ci{.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT,
            .imageType = VK_IMAGE_TYPE_2D, .format = VkFormat(f), .extent = {w,h,1},
            .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VkImageUsageFlags((storage ? VK_IMAGE_USAGE_STORAGE_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) | VK_IMAGE_USAGE_SAMPLED_BIT |
                     VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT),
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
        const VmaAllocationCreateInfo ai{.flags=VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT,
            .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        VkImage raw{};
        if (vmaCreateImage(allocator,&ci,&ai,&raw,&allocation,nullptr) != VK_SUCCESS) return false;
        image = raw;
        VkImageView raw_view{};
        const VkImageViewCreateInfo vi{.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = raw, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VkFormat(f),
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
        const auto result = VULKAN_HPP_DEFAULT_DISPATCHER.vkCreateImageView(device,&vi,nullptr,&raw_view);
        if (result != VK_SUCCESS) { Reset(); return false; }
        view = raw_view; width = w; height = h; format = f;
        return true;
    }
    void General(vk::CommandBuffer cmd, vk::AccessFlags2 access = vk::AccessFlagBits2::eShaderRead |
                 vk::AccessFlagBits2::eShaderWrite) {
        const vk::ImageMemoryBarrier2 b{.srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask = initialized ? vk::AccessFlags2(vk::AccessFlagBits2::eMemoryRead |
                vk::AccessFlagBits2::eMemoryWrite) : vk::AccessFlags2{},
            .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands, .dstAccessMask = access,
            .oldLayout = initialized ? vk::ImageLayout::eGeneral : vk::ImageLayout::eUndefined,
            .newLayout = vk::ImageLayout::eGeneral, .image = image,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor,0,1,0,1}};
        cmd.pipelineBarrier2({.imageMemoryBarrierCount = 1,.pImageMemoryBarriers = &b});
        initialized = true;
    }
};
inline void FgCopy(vk::CommandBuffer cmd, vk::Image source, vk::ImageLayout layout,
                   u32 sw, u32 sh, FgImage& target) {
    target.General(cmd, vk::AccessFlagBits2::eTransferWrite);
    const vk::ImageBlit r{.srcSubresource = {vk::ImageAspectFlagBits::eColor,0,0,1},
        .srcOffsets = std::array{vk::Offset3D{0,0,0},vk::Offset3D{s32(sw),s32(sh),1}},
        .dstSubresource = {vk::ImageAspectFlagBits::eColor,0,0,1},
        .dstOffsets = std::array{vk::Offset3D{0,0,0},vk::Offset3D{s32(target.width),s32(target.height),1}}};
    cmd.blitImage(source,layout,target.image,vk::ImageLayout::eGeneral,r,vk::Filter::eNearest);
    target.General(cmd);
}
}
