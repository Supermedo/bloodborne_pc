// SPDX-License-Identifier: GPL-2.0-or-later
// Headless comparison of the same production wrappers used by the game.
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
#include <string_view>
#include <vector>
#include "video_core/host_shaders/test_fsr411_consume_comp.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_fsr4vk.h"
#include "video_core/renderer_vulkan/vk_fsr411_native.h"
#include "video_core/renderer_vulkan/vk_fsr412_driver.h"
#include "bbport_settings.h"
#include "video_core/texture_cache/image.h"
#include <vk_mem_alloc.h>

using namespace Vulkan;
extern "C" void runtime_restart() { std::abort(); }
static uint16_t Half(float f) { return std::bit_cast<uint16_t>(static_cast<_Float16>(f)); }
int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    try {
        const bool driver_vk=argc>1&&std::string_view(argv[1])=="driver-vk";
        const bool driver_dx=argc>1&&std::string_view(argv[1])=="driver-dx";
        BbSettings::Get().upscaler=driver_vk||driver_dx?BbSettings::UpscalerFsr412:BbSettings::UpscalerFsr411;
        const bool async = driver_dx || (argc > 1 && std::string_view(argv[1]) == "async");
        const bool native = async || (argc > 1 && std::string_view(argv[1]) == "dx12");
        // Use the actual game router for repeated overlay-style live method changes.
        const bool overlay_pending = std::getenv("BB_TEST_LIVE_METHOD") != nullptr;
        const bool reject_method = std::getenv("BB_TEST_METHOD_UNAVAILABLE") != nullptr;
        const bool fail_method = std::getenv("BB_TEST_METHOD_FAILURE") != nullptr;
        auto& settings = BbSettings::Get();
        settings.fsr411_backend = native ? BbSettings::Fsr411Dx12 : BbSettings::Fsr411Vulkan;
        settings.startup_fsr411_backend = settings.fsr411_backend;
        u32 rw = 1280, rh = 720, ow = 1920, oh = 1080;
        if (argc > 3) { std::sscanf(argv[2], "%ux%u", &rw, &rh); std::sscanf(argv[3], "%ux%u", &ow, &oh); }
        const bool hdr = argc < 5 || std::string_view(argv[4]) != "ldr";
        const int count = argc > 5 ? std::atoi(argv[5]) : 180;
        if (count <= 0 || !rw || !rh || !ow || !oh || rw > 3840 || ow > 3840 || rh > 2160 || oh > 2160)
            throw std::runtime_error("use positive frame counts and dimensions up to 3840x2160");
        const char* batch_setting = std::getenv("BB_FSR411_BATCH_SIZE");
        const int batch_size = batch_setting ? std::atoi(batch_setting) : 1;
        if (batch_size < 1 || batch_size > 64)
            throw std::runtime_error("BB_FSR411_BATCH_SIZE must be 1..64");
        const bool pipeline = std::getenv("BB_FSR411_PIPELINE_TEST") != nullptr || batch_size > 1;
        const bool vary = std::getenv("BB_FSR411_VARY_INPUT") != nullptr;
        const bool vary_motion = std::getenv("BB_FSR411_VARY_MOTION") != nullptr;
        const bool switch_preset = std::getenv("BB_FSR411_SWITCH_PRESET") != nullptr;
        Instance instance(0, false);
        if (std::getenv("BB_TEST_NO_FG_INTEROP") &&
            (instance.IsFg401InteropSupported() || !instance.IsFsr411NativeSupported() || !instance.IsFsr411AsyncSupported()))
            throw std::runtime_error("expected DX12 upscaling interop without ML-FG interop");
        VULKAN_HPP_DEFAULT_DISPATCHER.init(instance.GetInstance());
        VULKAN_HPP_DEFAULT_DISPATCHER.init(instance.GetDevice());
        const auto device = instance.GetDevice();
        Scheduler scheduler(instance);
        std::array<VideoCore::UniqueImage, 4> images;
        std::array<vk::UniqueImageView, 4> views;
        const char *depth_setting = std::getenv("BB_SHARED_TEST_DEPTH");
        const auto depth_format =
            depth_setting && std::string_view(depth_setting) == "d32" ? vk::Format::eD32Sfloat
            : depth_setting && std::string_view(depth_setting) == "d32s8"
                ? vk::Format::eD32SfloatS8Uint
            : depth_setting && std::string_view(depth_setting) == "d16" ? vk::Format::eD16Unorm
                                                                        : vk::Format::eR32Sfloat;
        const auto depth_aspect = depth_format == vk::Format::eR32Sfloat
                                      ? vk::ImageAspectFlagBits::eColor
                                      : vk::ImageAspectFlagBits::eDepth;
        const char *color_setting = std::getenv("BB_SHARED_TEST_COLOR");
        const std::string_view color_name = color_setting ? color_setting : "fp16";
        const auto color_format = color_name == "rgba8"        ? vk::Format::eR8G8B8A8Unorm
                                  : color_name == "rgba8-srgb" ? vk::Format::eR8G8B8A8Srgb
                                  : color_name == "bgra8"      ? vk::Format::eB8G8R8A8Unorm
                                  : color_name == "bgra8-srgb" ? vk::Format::eB8G8R8A8Srgb
                                  : color_name == "r11g11b10"  ? vk::Format::eB10G11R11UfloatPack32
                                                               : vk::Format::eR16G16B16A16Sfloat;
        const std::array formats{color_format, depth_format, vk::Format::eR16G16Sfloat,
                                 vk::Format::eR16G16B16A16Sfloat};
        std::printf("TEST_FORMATS color=%s depth=%s\n", color_setting ? color_setting : "fp16",
                    depth_setting ? depth_setting : "r32");
        for (u32 i = 0; i < 4; ++i) {
            images[i] = VideoCore::UniqueImage(device, instance.GetAllocator());
            images[i].Create(
                {.imageType = vk::ImageType::e2D,
                 .format = formats[i],
                 .extent = {i == 3 ? ow : rw, i == 3 ? oh : rh, 1},
                 .mipLevels = 1,
                 .arrayLayers = 1,
                 .samples = vk::SampleCountFlagBits::e1,
                 .tiling = vk::ImageTiling::eOptimal,
                 .usage = vk::ImageUsageFlagBits::eSampled |
                          (i == 3 ? vk::ImageUsageFlagBits::eStorage : vk::ImageUsageFlags{}) |
                          vk::ImageUsageFlagBits::eTransferSrc |
                          vk::ImageUsageFlagBits::eTransferDst});
            views[i] = Check(device.createImageViewUnique(
                {.image = vk::Image(images[i]),
                 .viewType = vk::ImageViewType::e2D,
                 .format = formats[i],
                 .subresourceRange = {i == 1 ? depth_aspect : vk::ImageAspectFlagBits::eColor, 0, 1,
                                      0, 1}}));
        }
        VideoCore::UniqueImage consumed(device, instance.GetAllocator());
        consumed.Create(
            {.imageType = vk::ImageType::e2D,
             .format = vk::Format::eR8G8B8A8Unorm,
             .extent = {ow, oh, 1},
             .mipLevels = 1,
             .arrayLayers = 1,
             .samples = vk::SampleCountFlagBits::e1,
             .tiling = vk::ImageTiling::eOptimal,
             .usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eTransferSrc});
        auto consumed_view = Check(device.createImageViewUnique(
            {.image = vk::Image(consumed),
             .viewType = vk::ImageViewType::e2D,
             .format = vk::Format::eR8G8B8A8Unorm,
             .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}}));
        std::array<vk::DescriptorSetLayoutBinding, 2> bindings{};
        for (u32 i = 0; i < 2; ++i)
            bindings[i] = {.binding = i,
                           .descriptorType = vk::DescriptorType::eStorageImage,
                           .descriptorCount = 1,
                           .stageFlags = vk::ShaderStageFlagBits::eCompute};
        auto consume_set = Check(device.createDescriptorSetLayoutUnique(
            {.flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
             .bindingCount = 2,
             .pBindings = bindings.data()}));
        const auto set = *consume_set;
        const vk::PushConstantRange params{
            .stageFlags = vk::ShaderStageFlagBits::eCompute, .offset = 0, .size = 8};
        auto consume_layout =
            Check(device.createPipelineLayoutUnique({.setLayoutCount = 1,
                                                     .pSetLayouts = &set,
                                                     .pushConstantRangeCount = 1,
                                                     .pPushConstantRanges = &params}));
        auto consume_shader = Check(device.createShaderModuleUnique(
            {.codeSize = sizeof(TEST_FSR411_CONSUME_COMP), .pCode = TEST_FSR411_CONSUME_COMP}));
        auto consume_pipeline = Check(device.createComputePipelineUnique(
            {}, {.stage = {.stage = vk::ShaderStageFlagBits::eCompute,
                           .module = *consume_shader,
                           .pName = "main"},
                 .layout = *consume_layout}));
        const bool poison_output = std::getenv("BB_SHARED_POISON_OUTPUT") != nullptr;
        const bool resize_input = std::getenv("BB_SHARED_RESIZE_INPUT") != nullptr;
        vk::Image final_output{};
        const size_t pixels = size_t(rw) * rh, upload_size = pixels * 16;
        VkBuffer staging{}; VmaAllocation allocation{}; VmaAllocationInfo allocation_info{};
        VkBufferCreateInfo buffer{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        buffer.size = std::max(upload_size, size_t(ow) * oh * 12);
        buffer.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        VmaAllocationCreateInfo host{};
        host.usage = VMA_MEMORY_USAGE_AUTO;
        host.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        if (vmaCreateBuffer(instance.GetAllocator(), &buffer, &host, &staging, &allocation, &allocation_info) != VK_SUCCESS)
            throw std::runtime_error("staging allocation failed");
        auto* data = static_cast<uint8_t*>(allocation_info.pMappedData);
        auto* color = reinterpret_cast<uint16_t*>(data);
        auto* depth = reinterpret_cast<float*>(data + pixels * 8);
        auto* motion = reinterpret_cast<uint16_t*>(data + pixels * 12);
        for (size_t y = 0; y < rh; ++y) for (size_t x = 0; x < rw; ++x) {
            const size_t i = y * rw + x;
            if (color_format == vk::Format::eR16G16B16A16Sfloat) {
                color[i * 4] = Half(float(x) / rw);
                color[i * 4 + 1] = Half(float(y) / rh);
                color[i * 4 + 2] = Half(((x / 16 + y / 16) & 1) ? .8f : .2f);
                color[i * 4 + 3] = Half(1);
            }
            if (depth_format == vk::Format::eD16Unorm)
                reinterpret_cast<uint16_t *>(depth)[i] =
                    uint16_t((.99f + .009f * float(y) / rh) * 65535.f);
            else
                depth[i] = .99f + .009f * float(y) / rh; // forward depth for both wrappers
            if (color_format == vk::Format::eB10G11R11UfloatPack32) {
                const auto r = uint32_t(Half(float(x) / rw) >> 4);
                const auto g = uint32_t(Half(float(y) / rh) >> 4);
                const auto b = uint32_t(Half(((x / 16 + y / 16) & 1) ? .8f : .2f) >> 5);
                reinterpret_cast<uint32_t *>(color)[i] = r | (g << 11) | (b << 22);
            } else if (color_format != vk::Format::eR16G16B16A16Sfloat) {
                auto *bytes = reinterpret_cast<uint8_t *>(color) + i * 4;
                const bool bgra = color_format == vk::Format::eB8G8R8A8Unorm ||
                                  color_format == vk::Format::eB8G8R8A8Srgb;
                bytes[bgra ? 2 : 0] = uint8_t(255 * x / rw);
                bytes[1] = uint8_t(255 * y / rh);
                bytes[bgra ? 0 : 2] = ((x / 16 + y / 16) & 1) ? 204 : 51;
                bytes[3] = 255;
            }
            motion[i * 2] = motion[i * 2 + 1] = 0;
        }
        vmaFlushAllocation(instance.GetAllocator(), allocation, 0, VK_WHOLE_SIZE);
        auto cmd = scheduler.CommandBuffer();
        std::array<vk::ImageMemoryBarrier2, 4> barriers{};
        for (u32 i = 0; i < 4; ++i)
            barriers[i] = {
                .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
                .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
                .oldLayout = vk::ImageLayout::eUndefined,
                .newLayout = vk::ImageLayout::eGeneral,
                .image = vk::Image(images[i]),
                .subresourceRange = {i == 1 ? depth_aspect : vk::ImageAspectFlagBits::eColor, 0, 1,
                                     0, 1}};
        cmd.pipelineBarrier2({.imageMemoryBarrierCount = 4, .pImageMemoryBarriers = barriers.data()});
        const std::array<size_t, 3> offsets{0, pixels * 8, pixels * 12};
        for (u32 i = 0; i < 3; ++i) {
            vk::BufferImageCopy copy{
                .bufferOffset = offsets[i],
                .imageSubresource = {i == 1 ? depth_aspect : vk::ImageAspectFlagBits::eColor, 0, 0,
                                     1},
                .imageExtent = {rw, rh, 1}};
            cmd.copyBufferToImage(staging, vk::Image(images[i]), vk::ImageLayout::eGeneral, copy);
        }
        for (auto& b : barriers) {
            b.srcStageMask = vk::PipelineStageFlagBits2::eTransfer; b.srcAccessMask = vk::AccessFlagBits2::eTransferWrite;
            b.dstStageMask = vk::PipelineStageFlagBits2::eAllCommands;
            b.dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite;
            b.oldLayout = vk::ImageLayout::eGeneral;
        }
        cmd.pipelineBarrier2({.imageMemoryBarrierCount = 4, .pImageMemoryBarriers = barriers.data()});
        scheduler.Finish();
        Fsr4Upscaler::Frame frame{};
        frame.color = {vk::Image(images[0]), *views[0], rw, rh, formats[0]};
        frame.depth = {vk::Image(images[1]), *views[1], rw, rh, formats[1]};
        frame.depth.aspect = depth_aspect;
        frame.motion = {vk::Image(images[2]), *views[2], rw, rh, formats[2]};
        frame.output = {vk::Image(images[3]), *views[3], ow, oh, formats[3]};
        frame.render_width = rw; frame.render_height = rh; frame.hdr = hdr;
        frame.preset = rw == ow ? 0 : 1; frame.frame_ms = 16.6667f;
        frame.near_plane = .05f; frame.far_plane = 3000; frame.vertical_fov = 1.22f;
        frame.auto_exposure = true;
        frame.sharpen = std::getenv("BB_FSR411_COMPARE_SHARPEN") != nullptr;
        frame.sharpness = .5f;
        Fsr4Vk fsr_vk(instance, scheduler);
        Fsr411Native fsr_dx(instance, scheduler);
        Fsr412Driver fsr_driver(instance,scheduler);
        Fsr4Upscaler production(instance, scheduler);
        if (overlay_pending && (reject_method || fail_method)) {
            // Simulate stale readiness after a driver-file failure, or a disabled method.
            settings.fsr41_backend_supported[driver_vk || driver_dx][native ? 0 : 1] = fail_method;
        }
        unsigned live_changes = 0;
        int last_method = -1;
        auto query = Check(device.createQueryPoolUnique({.queryType = vk::QueryType::eTimestamp, .queryCount = 2}));
        const float period = instance.GetPhysicalDevice().getProperties().limits.timestampPeriod;
        std::vector<double> gpu_times, wall_times;
        for (int i = 0; i < count + 16; ++i) {
            frame.cmdbuf = scheduler.CommandBuffer(); frame.reset = i == 0;
            frame.output = {vk::Image(images[3]), *views[3], ow, oh, formats[3]};
            if (resize_input) {
                frame.render_width = ((i / 24) % 2) ? rw / 2 : rw;
                frame.render_height = ((i / 24) % 2) ? rh / 2 : rh;
            }
            if (switch_preset) frame.preset = (i / 24) % 2 == 0 ? 1 : 2;
            if (vary) {
                // Alternate solid and patterned inputs on the GPU. No host-side staging
                // writes race with queued reads, and a stale cross-API input is visible.
                const vk::ImageMemoryBarrier2 writable{
                    .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                    .srcAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
                    .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
                    .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
                    .oldLayout = vk::ImageLayout::eGeneral, .newLayout = vk::ImageLayout::eGeneral,
                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .image = vk::Image(images[0]),
                    .subresourceRange = {vk::ImageAspectFlagBits::eColor,0,1,0,1}};
                frame.cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount=1,.pImageMemoryBarriers=&writable});
                if (i & 1) {
                    const vk::ClearColorValue value{std::array<float,4>{float(i%13)/13.f,.25f,.75f,1.f}};
                    frame.cmdbuf.clearColorImage(vk::Image(images[0]),vk::ImageLayout::eGeneral,value,writable.subresourceRange);
                } else {
                    const vk::BufferImageCopy copy{.imageSubresource={vk::ImageAspectFlagBits::eColor,0,0,1},.imageExtent={rw,rh,1}};
                    frame.cmdbuf.copyBufferToImage(staging,vk::Image(images[0]),vk::ImageLayout::eGeneral,copy);
                }
                frame.jitter[0]=float(i%8)/8.f-.5f;
                frame.jitter[1]=float(i%7)/7.f-.5f;
            }
            if (vary_motion) {
                const vk::ImageMemoryBarrier2 writable{
                    .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                    .srcAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
                    .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
                    .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
                    .oldLayout = vk::ImageLayout::eGeneral, .newLayout = vk::ImageLayout::eGeneral,
                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .image = vk::Image(images[2]),
                    .subresourceRange = {vk::ImageAspectFlagBits::eColor,0,1,0,1}};
                frame.cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount=1,.pImageMemoryBarriers=&writable});
                const vk::ClearColorValue value{std::array<float,4>{
                    float(i%5-2)*.75f, float(i%7-3)*.25f, 0.f, 0.f}};
                frame.cmdbuf.clearColorImage(vk::Image(images[2]),vk::ImageLayout::eGeneral,value,writable.subresourceRange);
            }
            const auto start = std::chrono::steady_clock::now();
            // Native dispatch submits this buffer, so only the all-Vulkan path uses these timestamps.
            if (!pipeline) {
                frame.cmdbuf.resetQueryPool(*query, 0, 2);
                frame.cmdbuf.writeTimestamp(vk::PipelineStageFlagBits::eTopOfPipe, *query, 0);
            }
            if (overlay_pending)
                settings.fsr411_backend = int(native) ^ ((i / 8) & 1);
            const bool ok = overlay_pending ? production.Record(frame) : driver_vk ? fsr_driver.Record(frame) : native ? fsr_dx.Record(frame, async) : fsr_vk.Record(frame);
            if (!ok) throw std::runtime_error(overlay_pending ? production.Problem() : driver_vk ? fsr_driver.Error() : native ? fsr_dx.Error() : fsr_vk.Problem());
            if (overlay_pending) {
                if (production.Fatal()) throw std::runtime_error("successful method retained a fatal fallback flag");
                const int expected = reject_method || fail_method ? int(native) : settings.fsr411_backend.load();
                if (settings.ActiveFsr411Backend() != expected)
                    throw std::runtime_error("live method report does not match the requested/retained provider");
                if ((frame.output.image == vk::Image(images[3])) != (expected == BbSettings::Fsr411Vulkan))
                    throw std::runtime_error("live method did not produce the expected Vulkan/DX12 output image");
                if (expected != last_method) { ++live_changes; last_method = expected; }
                if (settings.upscaler != (driver_vk || driver_dx ? BbSettings::UpscalerFsr412 : BbSettings::UpscalerFsr411))
                    throw std::runtime_error("live method changed the upscaler selection");
                if ((reject_method || fail_method) && ((i / 8) & 1) && !settings.fsr4_method_problem)
                    throw std::runtime_error("failed method did not report that the previous provider was retained");
            }
            final_output = frame.output.image;
            const auto access =
                vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite;
            std::array<vk::ImageMemoryBarrier2, 2> consume_barriers{};
            consume_barriers[0] = {
                .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                .srcAccessMask = access,
                .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
                .dstAccessMask = vk::AccessFlagBits2::eShaderStorageRead,
                .oldLayout = vk::ImageLayout::eGeneral,
                .newLayout = vk::ImageLayout::eGeneral,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = frame.output.image,
                .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}};
            consume_barriers[1] = {
                .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                .srcAccessMask = access,
                .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
                .dstAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
                .oldLayout = vk::ImageLayout::eUndefined,
                .newLayout = vk::ImageLayout::eGeneral,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = vk::Image(consumed),
                .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}};
            frame.cmdbuf.pipelineBarrier2(
                {.imageMemoryBarrierCount = 2, .pImageMemoryBarriers = consume_barriers.data()});
            const std::array<vk::DescriptorImageInfo, 2> infos{
                {{.imageView = frame.output.view, .imageLayout = vk::ImageLayout::eGeneral},
                 {.imageView = *consumed_view, .imageLayout = vk::ImageLayout::eGeneral}}};
            std::array<vk::WriteDescriptorSet, 2> writes{};
            for (u32 b = 0; b < 2; ++b)
                writes[b] = {.dstBinding = b,
                             .descriptorCount = 1,
                             .descriptorType = vk::DescriptorType::eStorageImage,
                             .pImageInfo = &infos[b]};
            frame.cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, *consume_pipeline);
            frame.cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *consume_layout, 0,
                                              writes);
            const std::array<u32, 2> size{ow, oh};
            frame.cmdbuf.pushConstants(*consume_layout, vk::ShaderStageFlagBits::eCompute, 0,
                                       sizeof(size), size.data());
            frame.cmdbuf.dispatch((ow + 7) / 8, (oh + 7) / 8, 1);
            if (poison_output && i != count + 15) {
                auto writable = consume_barriers[0];
                writable.dstStageMask = vk::PipelineStageFlagBits2::eTransfer;
                writable.dstAccessMask = vk::AccessFlagBits2::eTransferWrite;
                frame.cmdbuf.pipelineBarrier2(
                    {.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &writable});
                frame.cmdbuf.clearColorImage(
                    frame.output.image, vk::ImageLayout::eGeneral,
                    vk::ClearColorValue{std::array<float, 4>{.1f, .8f, .3f, 1.f}},
                    writable.subresourceRange);
            }
            if (i != count + 15) {
                if (overlay_pending) production.ReleaseOutput(frame.cmdbuf);
                else if (native) fsr_dx.ReleaseOutput(frame.cmdbuf);
            }
            if (!pipeline)
                frame.cmdbuf.writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe, *query, 1);
            if (pipeline) {
                if ((i + 1) % batch_size == 0) scheduler.Flush();
            }
            else scheduler.Finish();
            const double wall = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            if (i >= 16) wall_times.push_back(wall);
            if (!pipeline && i >= 16) {
                std::array<uint64_t, 2> results{};
                const auto result = device.getQueryPoolResults(*query, 0, 2, sizeof(results), results.data(), sizeof(uint64_t), vk::QueryResultFlagBits::e64 | vk::QueryResultFlagBits::eWait);
                if (result != vk::Result::eSuccess) throw std::runtime_error("timestamp readback failed");
                gpu_times.push_back((results[1] - results[0]) * double(period) / 1e6);
            }
        }
        cmd = scheduler.CommandBuffer();
        vk::MemoryBarrier2 ready{.srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite, .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
            .dstAccessMask = vk::AccessFlagBits2::eTransferRead};
        cmd.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &ready});
        vk::BufferImageCopy read{.imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1}, .imageExtent = {ow, oh, 1}};
        cmd.copyImageToBuffer(final_output, vk::ImageLayout::eGeneral, staging, read);
        auto post_read = read;
        post_read.bufferOffset = size_t(ow) * oh * 8;
        cmd.copyImageToBuffer(vk::Image(consumed), vk::ImageLayout::eGeneral, staging, post_read);
        if (overlay_pending) production.ReleaseOutput(cmd);
        else if (native) fsr_dx.ReleaseOutput(cmd);
        scheduler.Finish();
        vmaInvalidateAllocation(instance.GetAllocator(), allocation, 0, VK_WHOLE_SIZE);
        float lo = INFINITY, hi = -INFINITY; size_t invalid = 0;
        const auto* halves = reinterpret_cast<const _Float16*>(data);
        uint64_t hash = 14695981039346656037ull;
        for (size_t b = 0; b < size_t(ow) * oh * 8; ++b) {
            hash ^= data[b]; hash *= 1099511628211ull;
        }
        std::printf("OUTPUT hash=%016llx pipeline=%d\n", (unsigned long long)hash, pipeline);
        uint64_t post_hash = 14695981039346656037ull;
        for (size_t b = size_t(ow) * oh * 8; b < size_t(ow) * oh * 12; ++b) {
            post_hash ^= data[b];
            post_hash *= 1099511628211ull;
        }
        std::printf("POST_OUTPUT hash=%016llx\n", (unsigned long long)post_hash);
        if (overlay_pending) {
            if (!reject_method && !fail_method && live_changes < 4)
                throw std::runtime_error("expected repeated live provider changes");
            std::printf("PASS: live overlay methods; changes=%u retained=%d rollback=%d\n", live_changes, reject_method, fail_method);
        }
        for (size_t i = 0; i < size_t(ow) * oh; ++i) for (size_t c = 0; c < 3; ++c) {
            float value = float(halves[i * 4 + c]);
            invalid += !std::isfinite(value); lo = std::min(lo, value); hi = std::max(hi, value);
        }
        const auto mean = [](const auto& values) { return std::accumulate(values.begin(), values.end(), 0.) / values.size(); };
        std::printf("RESULT backend=%s render=%ux%u output=%ux%u color=%s frames=%d wall_ms=%.4f gpu_ms=%.4f min=%.6f max=%.6f nonfinite=%zu\n",
                    driver_vk ? "412-vulkan" : driver_dx ? "412-dx12" : native ? (async ? "411-dx12-async" : "411-dx12") : "411-fsr4vk", rw,rh,ow,oh,hdr ? "hdr" : "ldr",count,mean(wall_times),gpu_times.empty() ? 0 : mean(gpu_times),lo,hi,invalid);
        if (const char* dump = std::getenv("BB_FSR411_BENCH_DUMP")) {
            std::ofstream file(dump, std::ios::binary); file.write(reinterpret_cast<const char*>(data), size_t(ow) * oh * 8);
            std::ofstream post(std::string(dump) + ".post.rgba8", std::ios::binary);
            post.write(reinterpret_cast<const char *>(data) + size_t(ow) * oh * 8,
                       size_t(ow) * oh * 4);
        }
        vmaDestroyBuffer(instance.GetAllocator(), staging, allocation);
        if (invalid || hi - lo < .05f) throw std::runtime_error("output contains invalid or constant pixels");
        std::puts("PASS: production FSR 4.1.1 wrapper rendered finite nonconstant GPU output");
        return 0;
    } catch (const std::exception& e) { std::fprintf(stderr, "FAIL: %s\n", e.what()); return 1; }
}
