// SPDX-License-Identifier: GPL-2.0-or-later
// Full readback/validity regression across alternating sizes and cache eviction.
#include <algorithm>
#include <array>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>
#include "bbport_settings.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_object_motion.h"
#include "shader_recompiler/runtime_info.h"
#include <vk_mem_alloc.h>

using namespace Vulkan;
#ifdef _WIN32
extern "C" void runtime_restart() { std::abort(); }
#endif

int main(int argc, char **argv) {
    try {
        const std::string kind = argc > 1 ? argv[1] : "tiny";
        const u32 steps = kind == "game" ? 12 : 64;
        std::vector<std::pair<u32, u32>> sizes;
        if (kind == "game")
            sizes = {{2560, 1440}, {1280, 720}, {2560, 1440}, {2560, 1440}};
        else if (kind == "churn")
            sizes = {{128, 72}, {96, 54}, {64, 36}, {48, 27}, {32, 18}, {16, 9}, {1, 1}};
        else
            sizes = {{128, 72}, {64, 36}, {128, 72}, {128, 72}, {64, 36}, {64, 36}};
        const u64 stride = u64(sizes[0].first) * sizes[0].second * 16;
        BbSettings::Get().object_motion = true;
        BbSettings::Get().upscaler = BbSettings::UpscalerFsr3;
        Instance instance(0, false);
        VULKAN_HPP_DEFAULT_DISPATCHER.init(instance.GetInstance());
        VULKAN_HPP_DEFAULT_DISPATCHER.init(instance.GetDevice());
        const auto device = instance.GetDevice();
        Scheduler scheduler(instance);
        VkBuffer staging{};
        VmaAllocation allocation{};
        VmaAllocationInfo allocation_info{};
        VkBufferCreateInfo buffer{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        buffer.size = stride * steps;
        buffer.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        VmaAllocationCreateInfo host{};
        host.usage = VMA_MEMORY_USAGE_AUTO;
        host.flags =
            VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        if (vmaCreateBuffer(instance.GetAllocator(), &buffer, &host, &staging, &allocation,
                            &allocation_info) != VK_SUCCESS)
            throw std::runtime_error("readback allocation failed");
        std::vector<std::array<u32, 4>> expected;
        u64 fnv = 1469598103934665603ull;
        {
            ObjectMotion motion(instance, scheduler);
            if (!motion.Enabled())
                throw std::runtime_error("object motion unavailable");
            const char* cache_setting = std::getenv("BB_OBJECT_MOTION_CACHE");
            const bool cache = cache_setting && std::strcmp(cache_setting, "1") == 0;
            VmaTotalStatistics initial_stats{};
            vmaCalculateStatistics(instance.GetAllocator(), &initial_stats);
            std::set<std::pair<u32, u32>> observed_sizes;
            std::array<u32, 4> contents{};
            for (u32 i = 0; i < steps; ++i) {
                if (i % 4 == 0)
                    motion.OnFrameStart();
                const auto [w, h] = sizes[i % sizes.size()];
                RenderState state{};
                state.width = w;
                state.height = h;
                state.num_layers = 1;
                motion.Attach(state, w, h);
                observed_sizes.emplace(w, h);
                VmaTotalStatistics stats{};
                vmaCalculateStatistics(instance.GetAllocator(), &stats);
                const u32 targets = stats.total.statistics.allocationCount -
                                    initial_stats.total.statistics.allocationCount;
                const u32 expected_targets = cache ? std::min<size_t>(observed_sizes.size(), 4) : 1;
                if (targets != expected_targets)
                    throw std::runtime_error("target allocation count differs from selected cache mode");
                auto &attachment = state.color_attachments[Shader::MotionVectors::Output];
                if (attachment.is_clear) {
                    contents = {std::bit_cast<u32>(float(i + 1) / 128),
                                std::bit_cast<u32>(-float(i + 1) / 256), std::bit_cast<u32>(0.5f),
                                std::bit_cast<u32>(1.0f)};
                }
                attachment.clear_value = contents;
                scheduler.BeginRendering(state);
                scheduler.EndRendering();
                bool valid = false;
                auto cmd = scheduler.CommandBuffer();
                const u32 requested = i % 5 == 0 ? w + 1 : w;
                motion.PrepareRead(cmd, requested, h, valid);
                if (valid != (requested == w))
                    throw std::runtime_error("validity differs from legacy resize semantics");
                if (bool(motion.Image(requested, h)) != valid)
                    throw std::runtime_error("Image validity mismatch");
                const vk::BufferImageCopy copy{
                    .bufferOffset = i * stride,
                    .imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
                    .imageExtent = {w, h, 1}};
                cmd.copyImageToBuffer(motion.Image(w, h), vk::ImageLayout::eGeneral, staging, copy);
                expected.push_back(contents);
            }
            auto cmd = scheduler.CommandBuffer();
            const vk::MemoryBarrier2 host_barrier{
                .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
                .dstStageMask = vk::PipelineStageFlagBits2::eHost,
                .dstAccessMask = vk::AccessFlagBits2::eHostRead};
            cmd.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &host_barrier});
            scheduler.Finish();
            vmaInvalidateAllocation(instance.GetAllocator(), allocation, 0, VK_WHOLE_SIZE);
            for (u32 i = 0; i < steps; ++i) {
                const auto [w, h] = sizes[i % sizes.size()];
                const auto *values = reinterpret_cast<const u32 *>(
                    static_cast<const u8 *>(allocation_info.pMappedData) + i * stride);
                for (u64 p = 0; p < u64(w) * h * 4; ++p) {
                    if (values[p] != expected[i][p % 4]) {
                        std::printf("MISMATCH step=%u word=%llu actual=%08x expected=%08x\n", i,
                                    (unsigned long long)p, values[p], expected[i][p % 4]);
                        throw std::runtime_error("motion pixels changed");
                    }
                    fnv = (fnv ^ values[p]) * 1099511628211ull;
                }
            }
        }
        vmaDestroyBuffer(instance.GetAllocator(), staging, allocation);
        std::printf("PASS kind=%s steps=%u all_pixels_identical_to_expected hash=%016llx\n",
                    kind.c_str(), steps, (unsigned long long)fnv);
        return 0;
    } catch (const std::exception &e) {
        std::printf("FAIL: %s\n", e.what());
        return 1;
    }
}
