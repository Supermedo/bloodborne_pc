// GPU regression: three CPU frame slots, GPU input/FG/output work in flight.
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/image.h"
#include "vk_fg401_native.h"
#include "bbport_settings.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <vector>
#include <vk_mem_alloc.h>
using namespace Vulkan;
extern "C" void runtime_restart() { std::abort(); }
static uint16_t half(float v) { return std::bit_cast<uint16_t>(static_cast<_Float16>(v)); }
int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    try {
        if (argc > 2 || (argc == 2 && std::strcmp(argv[1], "4.0.2")))
            throw std::runtime_error("Optional provider must be 4.0.2");
        if (argc == 2) BbSettings::SelectFrameGen(BbSettings::FrameGenFsr402);
        constexpr u32 rw = 64, rh = 32, ow = 128, oh = 64, N = 256, Slots = 3;
        constexpr size_t cp = size_t(ow) * oh, ip = size_t(rw) * rh;
        const std::array<size_t, 4> offsets{0, cp * 4, cp * 4 + ip * 4, cp * 8 + ip * 8};
        const size_t bytes = offsets[3] + cp * 4;
        const bool overlap = std::getenv("BB_FG401_CHECK_OVERLAP") != nullptr;
        Instance instance(0, false);
        VULKAN_HPP_DEFAULT_DISPATCHER.init(instance.GetInstance());
        VULKAN_HPP_DEFAULT_DISPATCHER.init(instance.GetDevice());
        const auto device = instance.GetDevice();
        Scheduler draw(instance), completion(instance, false, instance.GetPresentQueue());
        std::array<VideoCore::UniqueImage, 4> images;
        std::array<vk::UniqueImageView, 4> views;
        const std::array formats{vk::Format::eR8G8B8A8Unorm, vk::Format::eR32Sfloat,
                                 vk::Format::eR16G16Sfloat, vk::Format::eR8G8B8A8Unorm};
        const std::array<u32, 4> widths{ow, rw, rw, ow}, heights{oh, rh, rh, oh};
        for (u32 i = 0; i < 4; ++i) {
            images[i] = VideoCore::UniqueImage(device, instance.GetAllocator());
            images[i].Create({.imageType = vk::ImageType::e2D,
                              .format = formats[i],
                              .extent = {widths[i], heights[i], 1},
                              .mipLevels = 1,
                              .arrayLayers = 1,
                              .samples = vk::SampleCountFlagBits::e1,
                              .tiling = vk::ImageTiling::eOptimal,
                              .usage = vk::ImageUsageFlagBits::eSampled |
                                       vk::ImageUsageFlagBits::eStorage |
                                       vk::ImageUsageFlagBits::eTransferSrc |
                                       vk::ImageUsageFlagBits::eTransferDst});
            views[i] = Check(device.createImageViewUnique(
                {.image = vk::Image(images[i]),
                 .viewType = vk::ImageViewType::e2D,
                 .format = formats[i],
                 .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}}));
        }
        VkBuffer host{};
        VmaAllocation allocation{};
        VmaAllocationInfo mapping{};
        VkBufferCreateInfo bc{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bc.size = bytes * Slots;
        bc.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        VmaAllocationCreateInfo ac{};
        ac.usage = VMA_MEMORY_USAGE_AUTO;
        ac.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        if (vmaCreateBuffer(instance.GetAllocator(), &bc, &ac, &host, &allocation, &mapping) !=
            VK_SUCCESS)
            throw std::runtime_error("Staging allocation failed");
        auto *base = static_cast<uint8_t *>(mapping.pMappedData);
        Fsr4Upscaler::Frame f{};
        f.render_width = rw;
        f.render_height = rh;
        f.frame_ms = 10;
        f.hdr = false;
        f.near_plane = .1f;
        f.far_plane = 3000;
        f.vertical_fov = 1;
        f.color = {vk::Image(images[0]), *views[0], ow, oh, formats[0]};
        f.depth = {vk::Image(images[1]), *views[1], rw, rh, formats[1]};
        f.motion = {vk::Image(images[2]), *views[2], rw, rh, formats[2]};
        Fg401Native bridge(instance, draw, overlap);
        std::array<int, Slots> pending;
        pending.fill(-1);
        std::array<u64, Slots> ticks{};
        std::vector<uint64_t> hashes(N);
        auto &consumer = overlap ? completion : draw;
        const auto collect = [&](u32 slot) {
            const int frame = pending[slot];
            if (frame < 0)
                return;
            consumer.Wait(ticks[slot]);
            vmaInvalidateAllocation(instance.GetAllocator(), allocation, slot * bytes, bytes);
            auto *data = base + slot * bytes + offsets[3];
            uint64_t hash = 14695981039346656037ull;
            uint8_t lo = 255, hi = 0;
            for (size_t i = 0; i < cp * 4; ++i) {
                hash ^= data[i];
                hash *= 1099511628211ull;
                if (i % 4 != 3) {
                    lo = std::min(lo, data[i]);
                    hi = std::max(hi, data[i]);
                }
            }
            if (hi - lo < 30)
                throw std::runtime_error("Constant output during in-flight test");
            hashes[frame] = hash;
            pending[slot] = -1;
        };
        for (u32 frame = 0; frame < N; ++frame) {
            const u32 slot = frame % Slots;
            collect(slot);
            const size_t offset = slot * bytes;
            auto *data = base + offset;
            for (u32 y = 0; y < oh; ++y)
                for (u32 x = 0; x < ow; ++x) {
                    const size_t p = (size_t(y) * ow + x) * 4;
                    const u32 shifted = (x + ow - (frame % 16) * 2) % ow;
                    data[p] = uint8_t(20 + 200 * shifted / ow);
                    data[p + 1] = uint8_t(20 + 200 * y / oh);
                    data[p + 2] = ((shifted / 8 + y / 8) & 1) ? 220 : 30;
                    data[p + 3] = 255;
                }
            auto *depth = reinterpret_cast<float *>(data + offsets[1]);
            auto *mv = reinterpret_cast<uint16_t *>(data + offsets[2]);
            for (size_t i = 0; i < ip; ++i) {
                depth[i] = .99f;
                mv[i * 2] = half(-2.f * float(rw) / ow);
                mv[i * 2 + 1] = half(0);
            }
            vmaFlushAllocation(instance.GetAllocator(), allocation, offset, bytes);
            auto cmd = draw.CommandBuffer();
            for (u32 i = 0; i < 3; ++i) {
                const vk::ImageMemoryBarrier2 b{
                    .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                    .srcAccessMask =
                        vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
                    .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
                    .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
                    .oldLayout = frame ? vk::ImageLayout::eGeneral : vk::ImageLayout::eUndefined,
                    .newLayout = vk::ImageLayout::eGeneral,
                    .image = vk::Image(images[i]),
                    .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}};
                cmd.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b});
                cmd.copyBufferToImage(
                    host, vk::Image(images[i]), vk::ImageLayout::eGeneral,
                    vk::BufferImageCopy{
                        .bufferOffset = offset + offsets[i],
                        .imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
                        .imageExtent = {widths[i], heights[i], 1}});
            }
            f.cmdbuf = cmd;
            f.reset = frame == 0 || frame == 37 || frame == 129;
            f.output = {vk::Image(images[3]), *views[3], ow, oh, formats[3]};
            if (!(overlap ? bridge.RecordOverlap(f, completion) : bridge.Record(f, true)))
                throw std::runtime_error(bridge.Error());
            cmd = f.cmdbuf;
            const vk::ImageMemoryBarrier2 visible{
                .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
                .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
                .dstAccessMask = vk::AccessFlagBits2::eTransferRead,
                .oldLayout = vk::ImageLayout::eGeneral,
                .newLayout = vk::ImageLayout::eGeneral,
                .image = f.output.image,
                .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}};
            cmd.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &visible});
            cmd.copyImageToBuffer(
                f.output.image, vk::ImageLayout::eGeneral, host,
                vk::BufferImageCopy{.bufferOffset = offset + offsets[3],
                                    .imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
                                    .imageExtent = {ow, oh, 1}});
            ticks[slot] = consumer.CurrentTick();
            pending[slot] = frame;
            if (overlap) {
                bridge.FlushOverlap(completion);
            } else {
                bridge.ReleaseOutput(cmd);
                draw.Flush();
            }
        }
        for (u32 slot = 0; slot < Slots; ++slot)
            collect(slot);
        draw.Finish();
        completion.Finish();
        for (u32 frame = 0; frame < N; ++frame)
            std::printf("FRAME %u hash=%016llx\n", frame, (unsigned long long)hashes[frame]);
        vmaDestroyBuffer(instance.GetAllocator(), host, allocation);
        std::printf("PASS in-flight %s frames=%u slots=%u resets=0,37,129\n",
                    overlap ? "overlap" : "reference", N, Slots);
        return 0;
    } catch (const std::exception &e) {
        std::fprintf(stderr, "FAIL %s\n", e.what());
        return 1;
    }
}
