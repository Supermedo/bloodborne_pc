// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_vulkan/vk_frame_generation.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/host_shaders/frame_generation_comp.h"
#include "video_core/host_shaders/frame_generation_depth_comp.h"
#include "bbport_settings.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vk_mem_alloc.h>
#include <ffx_vk_fsr3_3_1_5_bridge.h>

namespace Vulkan {
namespace {
constexpr auto all = vk::PipelineStageFlagBits2::eAllCommands;
constexpr auto rw = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite;
void Barrier(vk::CommandBuffer cmd) {
    const vk::MemoryBarrier2 b{
        .srcStageMask = all, .srcAccessMask = rw, .dstStageMask = all, .dstAccessMask = rw};
    cmd.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &b});
}
void InitializeImage(vk::CommandBuffer cmd, const Dlss::Resource &image) {
    const vk::ImageMemoryBarrier2 b{.srcStageMask = all,
                                    .srcAccessMask = rw,
                                    .dstStageMask = all,
                                    .dstAccessMask = rw,
                                    .oldLayout = vk::ImageLayout::eUndefined,
                                    .newLayout = vk::ImageLayout::eGeneral,
                                    .image = image.image,
                                    .subresourceRange = {image.aspect, 0, 1, 0, 1}};
    cmd.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b});
}
BbDlssImage BridgeImage(const Dlss::Resource &r) {
    return {r.image,
            r.view,
            VkImageSubresourceRange{VkImageAspectFlags(r.aspect), 0, 1, 0, 1},
            VkFormat(r.format),
            r.width,
            r.height};
}
FfxVkFsr3_3_1_6FrameGenerationImage FsrImage(const Dlss::Resource &r) {
    return {r.image,
            VkFormat(r.format),
            r.width,
            r.height,
            VK_IMAGE_LAYOUT_GENERAL,
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                (r.aspect == vk::ImageAspectFlagBits::eColor ? VK_IMAGE_USAGE_STORAGE_BIT : 0u)};
}
} // namespace

void GenerationImage::Create(const Instance &instance, u32 w, u32 h, vk::Format format) {
    view.reset();
    image.Destroy();
    image = VideoCore::UniqueImage(instance.GetDevice(), instance.GetAllocator());
    const bool depth = format == vk::Format::eD32Sfloat;
    image.Create({.flags = depth ? vk::ImageCreateFlags{}
                                 : vk::ImageCreateFlagBits::eMutableFormat |
                                       vk::ImageCreateFlagBits::eExtendedUsage,
                  .imageType = vk::ImageType::e2D,
                  .format = format,
                  .extent = {w, h, 1},
                  .mipLevels = 1,
                  .arrayLayers = 1,
                  .samples = vk::SampleCountFlagBits::e1,
                  .tiling = vk::ImageTiling::eOptimal,
                  .usage = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferSrc |
                           vk::ImageUsageFlagBits::eTransferDst |
                           (depth ? vk::ImageUsageFlags{}
                                  : vk::ImageUsageFlagBits::eStorage |
                                        vk::ImageUsageFlagBits::eColorAttachment)});
    resource = {vk::Image(image),
                {},
                format,
                depth ? vk::ImageAspectFlagBits::eDepth : vk::ImageAspectFlagBits::eColor,
                w,
                h};
    const vk::ImageViewUsageCreateInfo view_usage{
        .usage = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eColorAttachment};
    const bool bgra = format == vk::Format::eB8G8R8A8Unorm || format == vk::Format::eB8G8R8A8Srgb;
    view = Check(instance.GetDevice().createImageViewUnique(
        {.pNext = bgra ? &view_usage : nullptr,
         .image = resource.image,
         .viewType = vk::ImageViewType::e2D,
         .format = format,
         .subresourceRange = {resource.aspect, 0, 1, 0, 1}}));
    resource.view = *view;
}

FrameGenerationOutput::~FrameGenerationOutput() {
    if (disable_buffer)
        vmaDestroyBuffer(allocator, disable_buffer, disable_allocation);
}

std::shared_ptr<FrameGenerationInput>
FrameGeneration::Capture(const Instance &instance, Scheduler &scheduler,
                         const Dlss::Resource &color, const Dlss::Resource &depth,
                         const Dlss::Resource &motion, const BbFrameCamera &camera, float frame_ms,
                         bool reset, std::vector<std::shared_ptr<FrameGenerationInput>> &pool) {
    std::shared_ptr<FrameGenerationInput> input;
    for (auto &candidate : pool)
        if (candidate.use_count() == 1 && scheduler.IsFree(candidate->capture_tick)) {
            input = candidate;
            break;
        }
    if (!input) {
        input = std::make_shared<FrameGenerationInput>();
        pool.push_back(input);
    }
    if (input->color.resource.width != color.width ||
        input->color.resource.height != color.height ||
        input->color.resource.format != color.format ||
        input->motion.resource.width != motion.width ||
        input->motion.resource.height != motion.height) {
        input->color.Create(instance, color.width, color.height, color.format);
        input->depth.Create(instance, motion.width, motion.height, vk::Format::eR32Sfloat);
        input->motion.Create(instance, motion.width, motion.height, vk::Format::eR16G16Sfloat);
    }
    input->camera = camera;
    input->frame_ms = frame_ms;
    input->reset = reset;
    input->capture_tick = scheduler.CurrentTick();
    const auto device = instance.GetDevice();
    if (!input->depth_pipeline) {
        const std::array bindings{
            vk::DescriptorSetLayoutBinding{0, vk::DescriptorType::eSampledImage, 1,
                                           vk::ShaderStageFlagBits::eCompute},
            vk::DescriptorSetLayoutBinding{1, vk::DescriptorType::eStorageImage, 1,
                                           vk::ShaderStageFlagBits::eCompute}};
        input->depth_descriptors = Check(device.createDescriptorSetLayoutUnique(
            {.flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
             .bindingCount = 2,
             .pBindings = bindings.data()}));
        input->depth_layout = Check(device.createPipelineLayoutUnique(
            {.setLayoutCount = 1, .pSetLayouts = &*input->depth_descriptors}));
        auto module =
            Check(device.createShaderModuleUnique({.codeSize = sizeof(FRAME_GENERATION_DEPTH_COMP),
                                                   .pCode = FRAME_GENERATION_DEPTH_COMP}));
        input->depth_pipeline = Check(device.createComputePipelineUnique(
            {}, {.stage = {.stage = vk::ShaderStageFlagBits::eCompute,
                           .module = *module,
                           .pName = "main"},
                 .layout = *input->depth_layout}));
    }
    // FSR's public API consumes R32_FLOAT. Sample depth instead of copying
    // between incompatible depth/stencil and color formats (also handles D32/S8).
    const vk::ImageViewUsageCreateInfo usage{.usage = vk::ImageUsageFlagBits::eSampled};
    const auto depth_view =
        Check(device.createImageView({.pNext = &usage,
                                      .image = depth.image,
                                      .viewType = vk::ImageViewType::e2D,
                                      .format = depth.format,
                                      .subresourceRange = {depth.aspect, 0, 1, 0, 1}}));
    scheduler.Record([input, color, depth_view, motion](vk::CommandBuffer cmd) {
        const std::array sources{color, motion};
        const std::array destinations{input->color.resource, input->motion.resource};
        Barrier(cmd);
        for (u32 i = 0; i < 2; ++i) {
            InitializeImage(cmd, destinations[i]);
            const vk::ImageCopy copy{.srcSubresource = {sources[i].aspect, 0, 0, 1},
                                     .dstSubresource = {destinations[i].aspect, 0, 0, 1},
                                     .extent = {destinations[i].width, destinations[i].height, 1}};
            cmd.copyImage(sources[i].image, vk::ImageLayout::eGeneral, destinations[i].image,
                          vk::ImageLayout::eGeneral, copy);
        }
        InitializeImage(cmd, input->depth.resource);
        const std::array infos{vk::DescriptorImageInfo{.imageView = depth_view,
                                                       .imageLayout = vk::ImageLayout::eGeneral},
                               vk::DescriptorImageInfo{.imageView = input->depth.resource.view,
                                                       .imageLayout = vk::ImageLayout::eGeneral}};
        const std::array writes{
            vk::WriteDescriptorSet{.dstBinding = 0,
                                   .descriptorCount = 1,
                                   .descriptorType = vk::DescriptorType::eSampledImage,
                                   .pImageInfo = &infos[0]},
            vk::WriteDescriptorSet{.dstBinding = 1,
                                   .descriptorCount = 1,
                                   .descriptorType = vk::DescriptorType::eStorageImage,
                                   .pImageInfo = &infos[1]}};
        cmd.bindPipeline(vk::PipelineBindPoint::eCompute, *input->depth_pipeline);
        cmd.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *input->depth_layout, 0, writes);
        cmd.dispatch((input->depth.resource.width + 7) / 8, (input->depth.resource.height + 7) / 8,
                     1);
        Barrier(cmd);
    });
    scheduler.DeferOperation([device, depth_view] { device.destroyImageView(depth_view); });
    return input;
}

FrameGeneration::FrameGeneration(const Instance &instance_, Scheduler &scheduler_)
    : instance(instance_), scheduler(scheduler_) {
    auto &settings = BbSettings::Get();
    const auto features = instance.GetPhysicalDevice().getFeatures();
    settings.fsr_fg_supported = features.shaderStorageImageWriteWithoutFormat;
    const auto *dlss = Dlss::Get();
    settings.dlss_fg_supported =
        settings.fsr_fg_supported && dlss && dlss->FrameGenerationAvailable();
    if (!settings.fsr_fg_supported)
        return;
    const auto device = instance.GetDevice();
    std::array<vk::DescriptorSetLayoutBinding, 5> bindings{};
    for (u32 i = 0; i < 5; ++i)
        bindings[i] = {.binding = i,
                       .descriptorType = i == 2 || i == 3 ? vk::DescriptorType::eStorageImage
                                                          : vk::DescriptorType::eSampledImage,
                       .descriptorCount = 1,
                       .stageFlags = vk::ShaderStageFlagBits::eCompute};
    descriptors = Check(device.createDescriptorSetLayoutUnique(
        {.flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
         .bindingCount = u32(bindings.size()),
         .pBindings = bindings.data()}));
    const vk::PushConstantRange push{vk::ShaderStageFlagBits::eCompute, 0, 4};
    layout = Check(device.createPipelineLayoutUnique({.setLayoutCount = 1,
                                                      .pSetLayouts = &*descriptors,
                                                      .pushConstantRangeCount = 1,
                                                      .pPushConstantRanges = &push}));
    auto module = Check(device.createShaderModuleUnique(
        {.codeSize = sizeof(FRAME_GENERATION_COMP), .pCode = FRAME_GENERATION_COMP}));
    pipeline = Check(device.createComputePipelineUnique(
        {},
        {.stage = {.stage = vk::ShaderStageFlagBits::eCompute, .module = *module, .pName = "main"},
         .layout = *layout}));
}
FrameGeneration::~FrameGeneration() { Release(); }
void FrameGeneration::Invalidate() {
    history_valid = false;
    if (BbSettings::Get().frame_generation == BbSettings::FrameGenerationOff) {
        failed = false;
        mode = 0;
        BbSettings::Get().frame_generation_problem = nullptr;
    }
}
void FrameGeneration::Release() {
    scheduler.WaitSubmitted();
    if (fsr)
        ffxVkFsr3_3_1_6FrameGenerationContextDestroy(fsr);
    fsr = nullptr;
    if (auto *dlss = Dlss::Get())
        dlss->ReleaseFrameGeneration();
    pending.clear();
    history_valid = false;
    failed = false;
    mode = 0;
}

void FrameGeneration::EnsureOutput(std::shared_ptr<FrameGenerationOutput> &output, u32 w, u32 h,
                                   vk::Format format) {
    if (output && output->generated.resource.width == w && output->generated.resource.height == h &&
        output->hudless.resource.format == format)
        return;
    output = std::make_shared<FrameGenerationOutput>();
    output->hudless.Create(instance, w, h, format);
    output->ui.Create(instance, w, h, vk::Format::eR16G16B16A16Sfloat);
    // The public FSR provider accepts RGBA8/16F, whereas Windows swapchains
    // commonly use BGRA8. Convert channels with a blit instead of relabelling
    // bytes or passing an unsupported native format to the provider.
    output->generated.Create(instance, w, h, vk::Format::eR8G8B8A8Unorm);
    if (format != vk::Format::eR8G8B8A8Unorm) {
        output->converted_color.Create(instance, w, h, vk::Format::eR8G8B8A8Unorm);
        output->converted_scene.Create(instance, w, h, vk::Format::eR8G8B8A8Unorm);
    }
    output->present_done = Check(
        instance.GetDevice().createFenceUnique({.flags = vk::FenceCreateFlagBits::eSignaled}));
    output->allocator = instance.GetAllocator();
    const VkBufferCreateInfo bi{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                .size = 4,
                                .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                         VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    const VmaAllocationCreateInfo ac{.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT |
                                              VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT,
                                     .usage = VMA_MEMORY_USAGE_AUTO,
                                     .requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT};
    VmaAllocationInfo ai{};
    Check(vk::Result(vmaCreateBuffer(output->allocator, &bi, &ac, &output->disable_buffer,
                                     &output->disable_allocation, &ai)));
    output->disable_data = ai.pMappedData;
}

bool FrameGeneration::Record(vk::CommandBuffer cmd, const Dlss::Resource &color,
                             std::shared_ptr<FrameGenerationInput> input,
                             std::shared_ptr<FrameGenerationOutput> &output) {
    auto &settings = BbSettings::Get();
    if (output)
        output->dispatched = false;
    const int selected = settings.frame_generation;
    if (!input || selected == 0 || !output) {
        history_valid = false;
        return false;
    }
    if ((selected == BbSettings::FrameGenerationDlss && !settings.dlss_fg_supported) ||
        (selected == BbSettings::FrameGenerationFsr && !settings.fsr_fg_supported)) {
        settings.frame_generation_problem = "Frame generation unavailable on this GPU/driver";
        history_valid = false;
        return false;
    }
    auto backbuffer = color, scene = output->hudless.resource;
    const u32 w = color.width, h = color.height, rw_ = input->motion.resource.width,
              rh = input->motion.resource.height;
    Barrier(cmd);
    if (color.format != output->generated.resource.format) {
        const std::array sources{color, scene};
        const std::array destinations{output->converted_color.resource,
                                      output->converted_scene.resource};
        for (u32 i = 0; i < 2; ++i) {
            InitializeImage(cmd, destinations[i]);
            const vk::ImageBlit blit{
                .srcSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
                .srcOffsets = std::array{vk::Offset3D{}, vk::Offset3D{int(w), int(h), 1}},
                .dstSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
                .dstOffsets = std::array{vk::Offset3D{}, vk::Offset3D{int(w), int(h), 1}}};
            cmd.blitImage(sources[i].image, vk::ImageLayout::eGeneral, destinations[i].image,
                          vk::ImageLayout::eGeneral, blit, vk::Filter::eNearest);
        }
        backbuffer = destinations[0];
        scene = destinations[1];
        Barrier(cmd);
    }
    if (mode != selected || width != w || height != h || render_width != rw_ ||
        render_height != rh || format != backbuffer.format) {
        Release();
        width = w;
        height = h;
        render_width = rw_;
        render_height = rh;
        format = backbuffer.format;
        bool ok = false;
        if (selected == BbSettings::FrameGenerationDlss)
            ok = Dlss::Get()->CreateFrameGeneration(cmd, w, h, format);
        else {
            const FfxVkFsr3_3_1_6FrameGenerationCreateInfo ci{instance.GetPhysicalDevice(),
                                                              instance.GetDevice(),
                                                              rw_,
                                                              rh,
                                                              w,
                                                              h,
                                                              VkFormat(format)};
            ok = ffxVkFsr3_3_1_6FrameGenerationContextCreate(&ci, &fsr) ==
                 FFX_VK_FSR3_3_1_6_FRAMEGEN_OK;
        }
        mode = selected;
        if (!ok) {
            failed = true;
            settings.frame_generation_problem = "Frame generation initialization failed; see log";
            return false;
        }
        std::printf("Frame generation: %s 2x, guides %ux%u, output %ux%u\n",
                    mode == BbSettings::FrameGenerationDlss ? "DLSS" : "FSR 3.1.6", rw_, rh, w, h);
    }
    if (failed)
        return false;
    while (!pending.empty() && scheduler.IsFree(pending.front().tick)) {
        if (fsr)
            ffxVkFsr3_3_1_6FrameGenerationContextRetireFrame(fsr, pending.front().id);
        pending.pop_front();
    }
    const bool reset =
        input->reset || !history_valid || input->frame_ms > 100 || input->frame_ms <= 0;
    output->input = std::move(input);
    output->dispatched = false;
    const auto &guides = *output->input;
    InitializeImage(cmd, output->ui.resource);
    InitializeImage(cmd, output->generated.resource);
    Barrier(cmd);
    cmd.fillBuffer(output->disable_buffer, 0, 4, reset ? 1 : 0);
    const std::array images{backbuffer, scene, output->ui.resource, output->generated.resource,
                            output->generated.resource};
    std::array<vk::DescriptorImageInfo, 5> infos{};
    std::array<vk::WriteDescriptorSet, 5> writes{};
    for (u32 i = 0; i < 5; ++i) {
        infos[i] = {.imageView = images[i].view, .imageLayout = vk::ImageLayout::eGeneral};
        writes[i] = {.dstBinding = i,
                     .descriptorCount = 1,
                     .descriptorType = i == 2 || i == 3 ? vk::DescriptorType::eStorageImage
                                                        : vk::DescriptorType::eSampledImage,
                     .pImageInfo = &infos[i]};
    }
    const auto compose = [&](u32 pass) {
        cmd.bindPipeline(vk::PipelineBindPoint::eCompute, *pipeline);
        cmd.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *layout, 0, writes);
        cmd.pushConstants(*layout, vk::ShaderStageFlagBits::eCompute, 0, 4, &pass);
        cmd.dispatch((w + 7) / 8, (h + 7) / 8, 1);
        Barrier(cmd);
    };
    compose(0);
    bool ok = false;
    if (mode == BbSettings::FrameGenerationDlss) {
        const BbDlssGenerate frame{BridgeImage(backbuffer),
                                   BridgeImage(scene),
                                   BridgeImage(output->ui.resource),
                                   BridgeImage(guides.depth.resource),
                                   BridgeImage(guides.motion.resource),
                                   BridgeImage(output->generated.resource),
                                   guides.camera,
                                   output->disable_buffer,
                                   reset ? 1 : 0};
        ok = Dlss::Get()->GenerateFrame(cmd, frame);
    } else {
        FfxVkFsr3_3_1_6FrameGenerationPrepareInfo p{};
        p.commandBuffer = cmd;
        p.color = FsrImage(scene);
        p.depth = FsrImage(guides.depth.resource);
        p.motionVectors = FsrImage(guides.motion.resource);
        p.renderWidth = rw_;
        p.renderHeight = rh;
        p.jitterOffsetX = guides.camera.jitter[0];
        p.jitterOffsetY = guides.camera.jitter[1];
        p.motionVectorScaleX = p.motionVectorScaleY = 1;
        p.frameTimeMilliseconds = std::clamp(guides.frame_ms, 1.f, 100.f);
        p.minLuminance = 0;
        p.maxLuminance = 1000;
        p.transferFunction = FFX_VK_FSR3_3_1_6_FRAMEGEN_TRANSFER_SRGB;
        p.cameraNear = guides.camera.near_plane;
        p.cameraFar = guides.camera.far_plane;
        p.viewSpaceToMeters = 1;
        p.cameraVerticalFovRadians = guides.camera.vertical_fov;
        std::copy_n(guides.camera.position, 3, p.cameraPosition);
        std::copy_n(guides.camera.up, 3, p.cameraUp);
        std::copy_n(guides.camera.right, 3, p.cameraRight);
        std::copy_n(guides.camera.forward, 3, p.cameraForward);
        p.frameId = frame_id;
        p.reset = reset;
        auto result = ffxVkFsr3_3_1_6FrameGenerationContextRecordPrepare(fsr, &p);
        if (result == FFX_VK_FSR3_3_1_6_FRAMEGEN_OK) {
            FfxVkFsr3_3_1_6FrameGenerationDispatchInfo f{};
            f.commandBuffer = cmd;
            f.color = p.color;
            f.output = FsrImage(output->generated.resource);
            f.displayWidth = w;
            f.displayHeight = h;
            f.frameTimeMilliseconds = p.frameTimeMilliseconds;
            f.cameraNear = p.cameraNear;
            f.cameraFar = p.cameraFar;
            f.viewSpaceToMeters = 1;
            f.cameraVerticalFovRadians = p.cameraVerticalFovRadians;
            f.minLuminance = p.minLuminance;
            f.maxLuminance = p.maxLuminance;
            f.transferFunction = p.transferFunction;
            f.frameId = frame_id;
            f.reset = reset;
            result = ffxVkFsr3_3_1_6FrameGenerationContextRecordDispatch(fsr, &f);
        }
        ok = result == FFX_VK_FSR3_3_1_6_FRAMEGEN_OK;
        if (!ok)
            std::printf("Frame generation: FSR dispatch error %d\n", int(result));
    }
    pending.push_back({scheduler.CurrentTick(), frame_id++});
    Barrier(cmd);
    if (ok)
        compose(1);
    const vk::MemoryBarrier2 host{.srcStageMask = all,
                                  .srcAccessMask = rw,
                                  .dstStageMask = vk::PipelineStageFlagBits2::eHost,
                                  .dstAccessMask = vk::AccessFlagBits2::eHostRead};
    cmd.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &host});
    history_valid = ok;
    failed = !ok; // Retry after a mode/size change; keep normal presentation working meanwhile.
    output->dispatched = ok && !reset;
    settings.frame_generation_problem =
        ok ? nullptr : "Frame generation failed; displaying real frames";
    return output->dispatched;
}
} // namespace Vulkan
