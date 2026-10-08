// SPDX-License-Identifier: GPL-2.0-or-later
// Exercise the real NGX runtime with a stationary, subpixel-jittered scene.
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_dlss.h"
#include "video_core/texture_cache/image.h"
#include <vk_mem_alloc.h>
#include "test_assert.h"

static u16 Half(float f) {
    const u32 v = std::bit_cast<u32>(f);
    const int e = int((v >> 23) & 255) - 127 + 15;
    return u16((v >> 16) & 0x8000) | (e <= 0 ? 0 : u16(e << 10) | u16((v >> 13) & 1023));
}
static float Float(u16 v) {
    const int e = (v >> 10) & 31;
    return (v & 0x8000 ? -1.f : 1.f) * (e == 0
                                            ? std::ldexp(float(v & 1023), -24)
                                            : std::ldexp(1.f + float(v & 1023) / 1024.f, e - 15));
}
static float Halton(u32 n, u32 base) {
    float result = 0, f = 1;
    while (n) {
        f /= base;
        result += f * (n % base);
        n /= base;
    }
    return result;
}
int main() {
    Vulkan::Instance instance(0, false);
    auto *dlss = Vulkan::Dlss::Get();
    if (!dlss || !dlss->Available()) {
        std::puts("DLSS unavailable: SKIP");
        return 77;
    }
    static vk::detail::DynamicLoader loader;
    vk::detail::DispatchLoaderDynamic d;
    d.init(loader.getProcAddress<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr"));
    d.init(instance.GetInstance());
    d.init(instance.GetDevice());
    const auto device = instance.GetDevice();
    Vulkan::Scheduler scheduler(instance);
    constexpr u32 W = 384, H = 216, OW = W * 2, OH = H * 2, N = W * H;
    std::array<VideoCore::UniqueImage, 4> images;
    std::array<vk::UniqueImageView, 4> views;
    std::array<Vulkan::Dlss::Resource, 4> resources{};
    for (u32 i = 0; i < 4; ++i) {
        const auto format = i == 1   ? vk::Format::eD32Sfloat
                            : i == 2 ? vk::Format::eR16G16Sfloat
                                     : vk::Format::eR16G16B16A16Sfloat;
        const u32 w = i == 3 ? OW : W, h = i == 3 ? OH : H;
        const auto aspect =
            i == 1 ? vk::ImageAspectFlagBits::eDepth : vk::ImageAspectFlagBits::eColor;
        images[i] = VideoCore::UniqueImage(device, instance.GetAllocator());
        images[i].Create(
            {.imageType = vk::ImageType::e2D,
             .format = format,
             .extent = {w, h, 1},
             .mipLevels = 1,
             .arrayLayers = 1,
             .samples = vk::SampleCountFlagBits::e1,
             .tiling = vk::ImageTiling::eOptimal,
             .usage = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferSrc |
                      vk::ImageUsageFlagBits::eTransferDst |
                      (i == 1 ? vk::ImageUsageFlags{} : vk::ImageUsageFlagBits::eStorage)});
        views[i] = device
                       .createImageViewUnique({.image = vk::Image(images[i]),
                                               .viewType = vk::ImageViewType::e2D,
                                               .format = format,
                                               .subresourceRange = {aspect, 0, 1, 0, 1}},
                                              nullptr, d)
                       .value;
        resources[i] = {vk::Image(images[i]), *views[i], format, aspect, w, h};
    }
    VkBuffer staging{};
    VmaAllocation allocation{};
    VmaAllocationInfo ai{};
    const VkBufferCreateInfo bi{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                .size = OW * OH * 8,
                                .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                         VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    const VmaAllocationCreateInfo ac{.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT |
                                              VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT,
                                     .usage = VMA_MEMORY_USAGE_AUTO,
                                     .requiredFlags = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT};
    assert(vmaCreateBuffer(instance.GetAllocator(), &bi, &ac, &staging, &allocation, &ai) ==
           VK_SUCCESS);
    const auto pattern = [](float x, float y) {
        return .5f + .2f * std::sin(x * 1.8f) + .2f * std::cos(y * 1.6f);
    };
    const auto run = [&](float sign) {
        dlss->ReleaseFeature();
        auto cmd = scheduler.CommandBuffer();
        assert(dlss->CreateFeature(cmd, {W, H, OW, OH, 3, false}));
        for (u32 f = 0; f < 40; ++f) {
            const float jx = Halton(f % 32 + 1, 2) - .5f, jy = Halton(f % 32 + 1, 3) - .5f;
            auto *pixels = static_cast<u16 *>(ai.pMappedData);
            for (u32 y = 0; y < H; ++y)
                for (u32 x = 0; x < W; ++x) {
                    const u16 v = Half(pattern(x + .5f - jx, y + .5f - jy));
                    const u32 p = (y * W + x) * 4;
                    pixels[p] = pixels[p + 1] = pixels[p + 2] = v;
                    pixels[p + 3] = Half(1);
                }
            cmd = scheduler.CommandBuffer();
            std::array<vk::ImageMemoryBarrier2, 4> transitions{};
            for (u32 i = 0; i < 4; ++i)
                transitions[i] = {.srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                                  .srcAccessMask = vk::AccessFlagBits2::eMemoryRead |
                                                   vk::AccessFlagBits2::eMemoryWrite,
                                  .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                                  .dstAccessMask = vk::AccessFlagBits2::eMemoryRead |
                                                   vk::AccessFlagBits2::eMemoryWrite,
                                  .oldLayout = vk::ImageLayout::eUndefined,
                                  .newLayout = vk::ImageLayout::eGeneral,
                                  .image = vk::Image(images[i]),
                                  .subresourceRange = {resources[i].aspect, 0, 1, 0, 1}};
            cmd.pipelineBarrier2(
                {.imageMemoryBarrierCount = 4, .pImageMemoryBarriers = transitions.data()}, d);
            const vk::BufferImageCopy upload{
                .imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
                .imageExtent = {W, H, 1}};
            cmd.copyBufferToImage(staging, vk::Image(images[0]), vk::ImageLayout::eGeneral, upload,
                                  d);
            cmd.clearDepthStencilImage(
                vk::Image(images[1]), vk::ImageLayout::eGeneral, {.depth = .5f, .stencil = 0},
                vk::ImageSubresourceRange{vk::ImageAspectFlagBits::eDepth, 0, 1, 0, 1}, d);
            cmd.clearColorImage(
                vk::Image(images[2]), vk::ImageLayout::eGeneral,
                vk::ClearColorValue{std::array<float, 4>{0, 0, 0, 0}},
                vk::ImageSubresourceRange{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}, d);
            const vk::MemoryBarrier2 ready{.srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                                           .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
                                           .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                                           .dstAccessMask = vk::AccessFlagBits2::eMemoryRead |
                                                            vk::AccessFlagBits2::eMemoryWrite};
            cmd.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &ready}, d);
            assert(dlss->Evaluate(cmd, {resources[0], resources[1], resources[2], resources[3],
                                        sign * jx, sign * jy, f == 0, 16.6667f, 0}));
            cmd.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &ready}, d);
            const vk::BufferImageCopy readback{
                .imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
                .imageExtent = {OW, OH, 1}};
            cmd.copyImageToBuffer(vk::Image(images[3]), vk::ImageLayout::eGeneral, staging,
                                  readback, d);
            scheduler.Finish();
        }
        const auto *pixels = static_cast<u16 *>(ai.pMappedData);
        double error = 0;
        u32 samples = 0;
        for (u32 y = 16; y < OH - 16; ++y)
            for (u32 x = 16; x < OW - 16; ++x) {
                const float v = Float(pixels[(y * OW + x) * 4]);
                assert(std::isfinite(v));
                const float delta = v - pattern((x + .5f) / 2, (y + .5f) / 2);
                error += delta * delta;
                ++samples;
            }
        error /= samples;
        std::printf("DLSS viewport jitter sign %+.0f: MSE %.8f\n", sign, error);
        return error;
    };
    const auto positive = run(1), negative = run(-1);
    std::printf("DLSS jitter ratio (+/-): %.3f\n", positive / negative);
    // A positive viewport shift is the production convention. Incorrect jitter
    // must measurably lose detail rather than silently regress to blurry output.
    assert(positive < .002 && positive < negative * .25);
    dlss->ReleaseFeature();
    vmaDestroyBuffer(instance.GetAllocator(), staging, allocation);
}
