// SPDX-License-Identifier: GPL-2.0-or-later
// Real GPU interpolation, HUD composition, copied guides and history reset.
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <SDL3/SDL.h>
#include "sdl_window.h"
#include "video_core/renderer_vulkan/vk_swapchain.h"
#include "video_core/renderer_vulkan/vk_frame_generation.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "bbport_settings.h"
#include <vk_mem_alloc.h>
#include "test_assert.h"

int main(int argc, char **argv) {
    const bool with_window = argc > 1 && std::strcmp(argv[1], "window") == 0;
    const bool dlss = argc > 1 && std::strcmp(argv[1], "dlss") == 0;
    std::unique_ptr<Frontend::WindowSDL> window;
    if (with_window) {
        SDL_setenv_unsafe("BB_HIDDEN_WINDOW", "1", 1);
        SDL_setenv_unsafe("BB_FULLSCREEN", "0", 1);
        window = std::make_unique<Frontend::WindowSDL>(640, 360, "Frame generation test");
    }
    const bool validation = std::getenv("BB_TEST_VALIDATION") != nullptr;
    auto instance_owner = window ? std::make_unique<Vulkan::Instance>(*window, 0, validation)
                                 : std::make_unique<Vulkan::Instance>(0, validation);
    auto &instance = *instance_owner;
    static vk::detail::DynamicLoader loader;
    vk::detail::DispatchLoaderDynamic d;
    d.init(loader.getProcAddress<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr"));
    d.init(instance.GetInstance());
    d.init(instance.GetDevice());
    const auto device = instance.GetDevice();
    Vulkan::Scheduler scheduler(instance, true);
    std::unique_ptr<Vulkan::Swapchain> swapchain;
    if (window)
        swapchain = std::make_unique<Vulkan::Swapchain>(instance, *window);
    Vulkan::FrameGeneration generation(instance, scheduler);
    auto &settings = BbSettings::Get();
    if (dlss && !settings.dlss_fg_supported) {
        std::puts("DLSS FG hardware unavailable: SKIP");
        return 77;
    }
    settings.frame_generation =
        dlss ? BbSettings::FrameGenerationDlss : BbSettings::FrameGenerationFsr;
    constexpr u32 W = 640, H = 360, RW = W / 2, RH = H / 2, N = W * H, RN = RW * RH;
    const auto color_format = with_window ? vk::Format::eB8G8R8A8Unorm : vk::Format::eR8G8B8A8Unorm;
    Vulkan::GenerationImage color, hudless, depth, motion;
    color.Create(instance, W, H, color_format);
    hudless.Create(instance, W, H, color_format);
    depth.Create(instance, RW, RH, vk::Format::eD32Sfloat);
    motion.Create(instance, RW, RH, vk::Format::eR16G16Sfloat);
    std::shared_ptr<Vulkan::FrameGenerationOutput> output;
    generation.EnsureOutput(output, W, H, color_format);
    std::vector<std::shared_ptr<Vulkan::FrameGenerationInput>> pool;
    VkBuffer staging{};
    VmaAllocation allocation{};
    VmaAllocationInfo ai{};
    const VkBufferCreateInfo bi{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                .size = N * 16,
                                .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                         VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    const VmaAllocationCreateInfo ac{.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT |
                                              VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT,
                                     .usage = VMA_MEMORY_USAGE_AUTO,
                                     .requiredFlags = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT};
    assert(vmaCreateBuffer(instance.GetAllocator(), &bi, &ac, &staging, &allocation, &ai) ==
           VK_SUCCESS);
    const auto barrier = [&](vk::CommandBuffer cmd) {
        const vk::MemoryBarrier2 b{
            .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite};
        cmd.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &b}, d);
    };
    unsigned presents = 0;
    const auto present = [&](const Vulkan::Dlss::Resource &image) {
        if (!swapchain)
            return;
        window->PollEvents();
        assert(swapchain->AcquireNextImage());
        auto cmd = scheduler.CommandBuffer();
        const std::array transitions{
            vk::ImageMemoryBarrier2{
                .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
                .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
                .dstAccessMask = vk::AccessFlagBits2::eTransferRead,
                .oldLayout = vk::ImageLayout::eGeneral,
                .newLayout = vk::ImageLayout::eTransferSrcOptimal,
                .image = image.image,
                .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}},
            vk::ImageMemoryBarrier2{
                .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
                .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
                .oldLayout = vk::ImageLayout::eUndefined,
                .newLayout = vk::ImageLayout::eTransferDstOptimal,
                .image = swapchain->Image(),
                .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}}};
        cmd.pipelineBarrier2(
            {.imageMemoryBarrierCount = 2, .pImageMemoryBarriers = transitions.data()}, d);
        const auto extent = swapchain->GetExtent();
        const vk::ImageBlit blit{
            .srcSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
            .srcOffsets =
                std::array{vk::Offset3D{}, vk::Offset3D{int(image.width), int(image.height), 1}},
            .dstSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
            .dstOffsets =
                std::array{vk::Offset3D{}, vk::Offset3D{int(extent.width), int(extent.height), 1}}};
        cmd.blitImage(image.image, vk::ImageLayout::eTransferSrcOptimal, swapchain->Image(),
                      vk::ImageLayout::eTransferDstOptimal, blit, vk::Filter::eLinear, d);
        auto done = transitions;
        done[0].srcStageMask = vk::PipelineStageFlagBits2::eTransfer;
        done[0].srcAccessMask = vk::AccessFlagBits2::eTransferRead;
        done[0].dstStageMask = vk::PipelineStageFlagBits2::eAllCommands;
        done[0].dstAccessMask =
            vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite;
        done[0].oldLayout = vk::ImageLayout::eTransferSrcOptimal;
        done[0].newLayout = vk::ImageLayout::eGeneral;
        done[1].srcStageMask = vk::PipelineStageFlagBits2::eTransfer;
        done[1].srcAccessMask = vk::AccessFlagBits2::eTransferWrite;
        done[1].dstStageMask = vk::PipelineStageFlagBits2::eBottomOfPipe;
        done[1].dstAccessMask = {};
        done[1].oldLayout = vk::ImageLayout::eTransferDstOptimal;
        done[1].newLayout = vk::ImageLayout::ePresentSrcKHR;
        cmd.pipelineBarrier2({.imageMemoryBarrierCount = 2, .pImageMemoryBarriers = done.data()},
                             d);
        Vulkan::SubmitInfo submit{};
        submit.AddWait(swapchain->GetImageAcquiredSemaphore());
        submit.AddSignal(swapchain->GetPresentReadySemaphore());
        const u64 tick = scheduler.CurrentTick();
        scheduler.Flush(submit);
        scheduler.WaitSubmitted(tick);
        {
            std::scoped_lock lock{Vulkan::Scheduler::submit_mutex};
            swapchain->Present();
        }
        scheduler.Finish();
        ++presents;
    };
    BbFrameCamera camera{};
    camera.near_plane = .1f;
    camera.far_plane = 3000;
    camera.vertical_fov = 1.2f;
    camera.up[1] = camera.right[0] = camera.forward[2] = 1;
    for (int i = 0; i < 4; ++i)
        camera.clip_to_previous[i * 4 + i] = camera.previous_to_clip[i * 4 + i] = 1;
    camera.view_to_clip[0] = camera.view_to_clip[5] = 1;
    camera.view_to_clip[10] = 1.0000333f;
    camera.view_to_clip[11] = 1;
    camera.view_to_clip[14] = -.10000333f;
    camera.clip_to_view[0] = camera.clip_to_view[5] = 1;
    camera.clip_to_view[11] = -9.999667f;
    camera.clip_to_view[14] = 1;
    camera.clip_to_view[15] = 10;
    unsigned interpolated = 0;
    for (u32 f = 0; f < 20; ++f) {
        auto *bytes = static_cast<u8 *>(ai.pMappedData);
        const int left = 120 + int(f) * 6;
        for (u32 y = 0; y < H; ++y)
            for (u32 x = 0; x < W; ++x) {
                const bool bar = int(x) >= left && int(x) < left + 60 && y >= 80 && y < 240;
                const u8 value = bar ? 230 : 35;
                const u32 p = (y * W + x) * 4;
                for (u32 c = 0; c < 3; ++c)
                    bytes[p + c] = bytes[N * 4 + p + c] = value;
                bytes[p + 3] = bytes[N * 4 + p + 3] = 255;
                if (x < 70 && y < 30) {
                    bytes[p] = with_window ? 0 : 255;
                    bytes[p + 1] = 255;
                    bytes[p + 2] = with_window ? 255 : 0;
                }
            }
        auto *mvec = reinterpret_cast<u16 *>(bytes + N * 8);
        for (u32 y = 0; y < RH; ++y)
            for (u32 x = 0; x < RW; ++x) {
                mvec[(y * RW + x) * 2] =
                    (int(x * 2) >= left && int(x * 2) < left + 60 && y * 2 >= 80 && y * 2 < 240)
                        ? 0xc200
                        : 0;
                mvec[(y * RW + x) * 2 + 1] = 0; // -3 render pixels = -6 display pixels.
            }
        auto cmd = scheduler.CommandBuffer();
        const std::array resources{color.resource, hudless.resource, depth.resource,
                                   motion.resource, output->hudless.resource};
        for (const auto &r : resources) {
            const vk::ImageMemoryBarrier2 b{
                .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                .srcAccessMask =
                    vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
                .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                .dstAccessMask =
                    vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
                .oldLayout = vk::ImageLayout::eUndefined,
                .newLayout = vk::ImageLayout::eGeneral,
                .image = r.image,
                .subresourceRange = {r.aspect, 0, 1, 0, 1}};
            cmd.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b}, d);
        }
        for (u32 i = 0; i < 3; ++i) {
            const auto &target = i == 0   ? color.resource
                                 : i == 1 ? hudless.resource
                                          : motion.resource;
            const vk::BufferImageCopy region{
                .bufferOffset = i == 0   ? 0
                                : i == 1 ? N * 4
                                         : N * 8,
                .imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
                .imageExtent = {target.width, target.height, 1}};
            cmd.copyBufferToImage(staging, target.image, vk::ImageLayout::eGeneral, region, d);
        }
        cmd.clearDepthStencilImage(
            depth.resource.image, vk::ImageLayout::eGeneral, {.depth = .5f, .stencil = 0},
            vk::ImageSubresourceRange{vk::ImageAspectFlagBits::eDepth, 0, 1, 0, 1}, d);
        barrier(cmd);
        const bool reset = f == 0 || f == 12;
        auto input =
            Vulkan::FrameGeneration::Capture(instance, scheduler, hudless.resource, depth.resource,
                                             motion.resource, camera, 16.6667f, reset, pool);
        cmd = scheduler.CommandBuffer();
        const vk::ImageCopy copy{.srcSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
                                 .dstSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
                                 .extent = {W, H, 1}};
        cmd.copyImage(input->color.resource.image, vk::ImageLayout::eGeneral,
                      output->hudless.resource.image, vk::ImageLayout::eGeneral, copy, d);
        barrier(cmd);
        const bool generated = generation.Record(cmd, color.resource, input, output);
        assert(generated != reset);
        barrier(cmd);
        const vk::BufferImageCopy readback{
            .bufferOffset = N * 12,
            .imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
            .imageExtent = {W, H, 1}};
        cmd.copyImageToBuffer(output->generated.resource.image, vk::ImageLayout::eGeneral, staging,
                              readback, d);
        scheduler.Finish();
        const auto *result = bytes + N * 12;
        if (generated) {
            assert(result[(10 * W + 10) * 4] == 255 && result[(10 * W + 10) * 4 + 1] == 255 &&
                   result[(10 * W + 10) * 4 + 2] == 0);
            double center = 0;
            unsigned count = 0;
            for (u32 x = 80; x < W - 80; ++x)
                if (result[(150 * W + x) * 4] > 160) {
                    center += x;
                    ++count;
                }
            assert(count > 40 && count < 80);
            center /= count;
            const double real = left + 29.5, mid = real - 3;
            std::printf("%s FG frame %u: center %.3f (real %.1f, midpoint %.1f)\n",
                        dlss ? "DLSS" : "FSR", f, center, real, mid);
            if (std::abs(center - mid) < 1.5 && std::abs(center - real) > 1.5)
                ++interpolated;
            present(output->generated.resource);
        }
        present(color.resource);
        output->input.reset();
    }
    assert(interpolated >= 10); // A repeated real frame cannot satisfy this check.
    // Switching off and back on must discard history. Recreate both the output
    // and provider at a different display size, then switch back without restart.
    settings.frame_generation = BbSettings::FrameGenerationOff;
    generation.Invalidate();
    assert(!generation.Record(scheduler.CommandBuffer(), color.resource, pool.front(), output));
    settings.frame_generation =
        dlss ? BbSettings::FrameGenerationDlss : BbSettings::FrameGenerationFsr;
    for (u32 width : {800u, W}) {
        if (window) {
            assert(SDL_SetWindowSize(window->GetSDLWindow(), int(width), int(H)));
            SDL_SyncWindow(window->GetSDLWindow());
            window->PollEvents();
            swapchain->Recreate(window->GetWidth(), window->GetHeight());
        }
        Vulkan::GenerationImage resized;
        resized.Create(instance, width, H, color_format);
        generation.EnsureOutput(output, width, H, color_format);
        auto cmd = scheduler.CommandBuffer();
        const std::array resources{resized.resource, output->hudless.resource};
        for (const auto &r : resources) {
            const vk::ImageMemoryBarrier2 b{.dstStageMask =
                                                vk::PipelineStageFlagBits2::eAllCommands,
                                            .dstAccessMask = vk::AccessFlagBits2::eMemoryWrite,
                                            .oldLayout = vk::ImageLayout::eUndefined,
                                            .newLayout = vk::ImageLayout::eGeneral,
                                            .image = r.image,
                                            .subresourceRange = {r.aspect, 0, 1, 0, 1}};
            cmd.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b}, d);
            cmd.clearColorImage(r.image, vk::ImageLayout::eGeneral,
                                vk::ClearColorValue{std::array<float, 4>{.2f, .2f, .2f, 1}},
                                vk::ImageSubresourceRange{r.aspect, 0, 1, 0, 1}, d);
        }
        barrier(cmd);
        auto input = pool.front();
        input->reset = false;
        assert(!generation.Record(cmd, resized.resource, input, output));
        scheduler.Finish();
        assert(generation.Record(scheduler.CommandBuffer(), resized.resource, input, output));
        scheduler.Finish();
        present(output->generated.resource);
        present(resized.resource);
    }
    std::printf("PASS: %u intermediate frames, exact HUD pixels, explicit history reset\n",
                interpolated);
    if (window) {
        assert(presents == 42);
        std::printf("PASS: %u Win32 swapchain presents and resize\n", presents);
    }
    vmaDestroyBuffer(instance.GetAllocator(), staging, allocation);
}
