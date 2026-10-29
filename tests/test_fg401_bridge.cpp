// GPU regression: real Vulkan/D3D12 shared textures and fences, no game data.
#include "bbport_settings.h"
#include "video_core/renderer_vulkan/vk_frame_generation.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/image.h"
#include "vk_fg401_native.h"
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <numeric>
#include <stdexcept>
#include <string>
#include <vector>
#include <vk_mem_alloc.h>
using namespace Vulkan;
extern "C" void runtime_restart() { std::abort(); }
static uint16_t half(float v) { return std::bit_cast<uint16_t>(static_cast<_Float16>(v)); }
int main(int argc, char **argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    try {
        u32 rw = 64, rh = 32, ow = 128, oh = 64;
        int count = 64;
        if (argc >= 3) {
            std::sscanf(argv[1], "%ux%u", &rw, &rh);
            std::sscanf(argv[2], "%ux%u", &ow, &oh);
        }
        if (argc >= 4)
            count = std::stoi(argv[3]);
        const int selected = argc >= 5 && std::string(argv[4]) == "4.0.2"
                                 ? BbSettings::FrameGenFsr402 : BbSettings::FrameGenFsr401;
        const bool switch_versions = argc >= 6 && std::string(argv[5]) == "switch";
        if (!rw || !rh || !ow || !oh || ow > 3840 || oh > 2160 || rw > ow || rh > oh || count < 1 ||
            count > 2000)
            throw std::runtime_error("Invalid test dimensions/count");
        const bool moving = ow <= 256;
        Instance instance(0, false);
        VULKAN_HPP_DEFAULT_DISPATCHER.init(instance.GetInstance());
        VULKAN_HPP_DEFAULT_DISPATCHER.init(instance.GetDevice());
        auto device = instance.GetDevice();
        Scheduler scheduler(instance);
        for (unsigned i = 0; i < 8 && instance.GetPresentQueue() != instance.GetGraphicsQueue(); ++i) {
            const auto tick = scheduler.CurrentTick();
            scheduler.Flush();
            scheduler.SetExecutionQueue(i % 2 ? instance.GetGraphicsQueue()
                                              : instance.GetPresentQueue());
            if (!scheduler.IsFree(tick)) {
                throw std::runtime_error("Provider queue switch left an earlier timeline value pending");
            }
        }
        auto &settings = BbSettings::Get();
        BbSettings::SelectFrameGen(selected);
        FrameGenerator provider_check(instance, scheduler);
        if (std::getenv("BB_CHECK_UNSUPPORTED")) {
            if (settings.MlFgSupported(selected) || settings.frame_gen != selected ||
                settings.ActiveFrameGen() != BbSettings::FrameGenFsr3)
                throw std::runtime_error("Unavailable provider did not fall back to FSR 3.1");
            std::puts("PASS unavailable ML provider falls back to FSR 3.1 before generation");
            return 0;
        }
        if (!settings.MlFgSupported(selected) || provider_check.WantedCount() != 1)
            throw std::runtime_error("Supported ML provider selection failed");
        settings.frame_gen = BbSettings::FrameGenFsr3;
        if (provider_check.WantedCount() != 1)
            throw std::runtime_error("FSR 3.1 was removed");
        settings.frame_gen = BbSettings::FrameGenOff;
        if (provider_check.WantedCount() != 0)
            throw std::runtime_error("FG off does not disable generation");
        BbSettings::SelectFrameGen(selected);
        BbSettings::ConfigureFg401Support(false, "Test unavailable", selected);
        auto empty_command = scheduler.CommandBuffer();
        provider_check.Generate(empty_command, {}, {}, {}, {}, {}, 0, 0, {});
        if (settings.frame_gen != selected || settings.ActiveFrameGen() != BbSettings::FrameGenFsr3)
            throw std::runtime_error("Manual unsupported ML selection bypasses renderer guard");
        BbSettings::ConfigureFg401Support(true, nullptr, selected);
        BbSettings::SelectFrameGen(selected);
        std::puts("PASS renderer ML/FSR3/off choices and unsupported runtime fallback");

        const bool overlap = std::getenv("BB_FG401_CHECK_OVERLAP") != nullptr;
        Scheduler completion(instance, false, instance.GetPresentQueue());
        std::printf("CHECK mode=%s separate_queue=%d\n", overlap ? "overlap" : "reference",
                    instance.GetPresentQueue() != instance.GetGraphicsQueue());
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
        const size_t cp = size_t(ow) * oh, ip = size_t(rw) * rh;
        const std::array<size_t, 4> offsets{0, cp * 4, cp * 4 + ip * 4, cp * 8 + ip * 8};
        const size_t bytes = offsets[3] + cp * 4;
        VkBuffer staging{};
        VmaAllocation allocation{};
        VmaAllocationInfo mapped{};
        VkBufferCreateInfo bc{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bc.size = bytes;
        bc.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        VmaAllocationCreateInfo ac{};
        ac.usage = VMA_MEMORY_USAGE_AUTO;
        ac.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        if (vmaCreateBuffer(instance.GetAllocator(), &bc, &ac, &staging, &allocation, &mapped) !=
            VK_SUCCESS)
            throw std::runtime_error("Host staging allocation failed");
        auto *data = static_cast<uint8_t *>(mapped.pMappedData);
        auto *depth = reinterpret_cast<float *>(data + offsets[1]);
        auto *mv = reinterpret_cast<uint16_t *>(data + offsets[2]);
        for (size_t i = 0; i < ip; ++i) {
            depth[i] = .99f;
            mv[i * 2] = mv[i * 2 + 1] = half(0);
        }
        Fsr4Upscaler::Frame f{};
        f.render_width = rw;
        f.render_height = rh;
        f.frame_ms = 10;
        f.near_plane = .1f;
        f.far_plane = 3000;
        f.vertical_fov = 1;
        f.hdr = false;
        f.color = {vk::Image(images[0]), *views[0], ow, oh, formats[0]};
        f.depth = {vk::Image(images[1]), *views[1], rw, rh, formats[1]};
        f.motion = {vk::Image(images[2]), *views[2], rw, rh, formats[2]};
        Fg401Native bridge(instance, scheduler, overlap);
        std::vector<double> wall;
        uint64_t first_hash{}, last_hash{};
        size_t different = 0;
        std::vector<uint8_t> previous(cp * 4);
        for (int frame = 0; frame < count + 16; ++frame) {
            if (switch_versions && (frame == 16 || frame == 32 || frame == 48)) {
                BbSettings::SelectFrameGen(frame == 32 ? BbSettings::FrameGenFsr401
                                                       : BbSettings::FrameGenFsr402);
                std::printf("CHECK provider switch: %s\n", BbSettings::FrameGenName(settings.frame_gen));
            }
            auto cmd = scheduler.CommandBuffer();
            if (frame == 0 || moving) {
                for (u32 y = 0; y < oh; ++y)
                    for (u32 x = 0; x < ow; ++x) {
                        size_t p = (size_t(y) * ow + x) * 4;
                        const u32 shifted = (x + ow - (moving ? (frame % 16) * 2 : 0)) % ow;
                        data[p] = uint8_t(20 + 200 * shifted / ow);
                        data[p + 1] = uint8_t(20 + 200 * y / oh);
                        data[p + 2] = ((shifted / 8 + y / 8) & 1) ? 220 : 30;
                        data[p + 3] = 255;
                    }
                // Exact UV/pixel convention used by the existing FG capture: previous-current.
                for (size_t i = 0; i < ip; ++i) {
                    mv[i * 2] = half(moving ? -2.f * float(rw) / ow : 0.f);
                    mv[i * 2 + 1] = half(0);
                }
                vmaFlushAllocation(instance.GetAllocator(), allocation, 0, VK_WHOLE_SIZE);
                for (u32 i = 0; i < 3; ++i) {
                    const vk::ImageMemoryBarrier2 b{
                        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                        .srcAccessMask =
                            vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
                        .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
                        .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
                        .oldLayout =
                            frame == 0 ? vk::ImageLayout::eUndefined : vk::ImageLayout::eGeneral,
                        .newLayout = vk::ImageLayout::eGeneral,
                        .image = vk::Image(images[i]),
                        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}};
                    cmd.pipelineBarrier2(
                        {.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b});
                    const vk::BufferImageCopy region{
                        .bufferOffset = offsets[i],
                        .imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
                        .imageExtent = {widths[i], heights[i], 1}};
                    cmd.copyBufferToImage(staging, vk::Image(images[i]), vk::ImageLayout::eGeneral,
                                          region);
                }
            }
            f.cmdbuf = cmd;
            f.reset = frame == 0 || frame == 37;
            f.output = {vk::Image(images[3]), *views[3], ow, oh, formats[3]};
            auto start = std::chrono::steady_clock::now();
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
            const vk::BufferImageCopy read{
                .bufferOffset = offsets[3],
                .imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
                .imageExtent = {ow, oh, 1}};
            cmd.copyImageToBuffer(f.output.image, vk::ImageLayout::eGeneral, staging, read);
            if (overlap) {
                bridge.FlushOverlap(completion);
                completion.Finish();
                scheduler.Finish();
            } else {
                bridge.ReleaseOutput(cmd);
                scheduler.Finish();
            }
            if (frame >= 16)
                wall.push_back(std::chrono::duration<double, std::milli>(
                                   std::chrono::steady_clock::now() - start)
                                   .count());
            if (moving || frame == count + 15) {
                vmaInvalidateAllocation(instance.GetAllocator(), allocation, 0, VK_WHOLE_SIZE);
                auto *output = data + offsets[3];
                uint64_t hash = 14695981039346656037ull;
                uint8_t lo = 255, hi = 0;
                for (size_t i = 0; i < cp * 4; ++i) {
                    hash ^= output[i];
                    hash *= 1099511628211ull;
                    if (i % 4 != 3) {
                        lo = std::min(lo, output[i]);
                        hi = std::max(hi, output[i]);
                    }
                }
                if (hi - lo < 30)
                    throw std::runtime_error("FG output is constant/black");
                if (frame == 16)
                    first_hash = hash;
                if (frame > 16 && hash != last_hash)
                    ++different;
                last_hash = hash;
                if (moving)
                    std::printf("FRAME %d reset=%d hash=%016llx range=%u:%u\n", frame, f.reset,
                                (unsigned long long)hash, lo, hi);
                if (frame == count + 15) {
                    std::ofstream file("output.rgba8", std::ios::binary);
                    file.write(reinterpret_cast<char *>(output), cp * 4);
                    std::ofstream input("input.rgba8", std::ios::binary);
                    input.write(reinterpret_cast<char *>(data), cp * 4);
                }
            }
        }
        if (moving && different < 8)
            throw std::runtime_error("FG output does not respond to moving inputs");
        std::printf("RESULT render=%ux%u display=%ux%u frames=%d wall_readback_ms=%.6f "
                    "changed_frames=%zu final_hash=%016llx\n",
                    rw, rh, ow, oh, count,
                    std::accumulate(wall.begin(), wall.end(), 0.0) / wall.size(), different,
                    (unsigned long long)last_hash);
        vmaDestroyBuffer(instance.GetAllocator(), staging, allocation);
        std::puts("PASS Vulkan -> DX12 AMD FG -> Vulkan output, changing images and reset");
        return 0;
    } catch (const std::exception &e) {
        std::fprintf(stderr, "FAIL %s\n", e.what());
        return 1;
    }
}
