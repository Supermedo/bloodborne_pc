#include "video_core/renderer_vulkan/vk_dlss_rr.h"

#include <cstdio>
#include <span>

#include <vk_mem_alloc.h>

#include "video_core/host_shaders/dlss_rr_guides_comp.h"
#include "video_core/host_shaders/dlss_rr_linearize_comp.h"
#include "video_core/host_shaders/dlss_rr_tonemap_comp.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_platform.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scene_resolution.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/texture_cache/texture_cache.h"

namespace Vulkan {

namespace {
constexpr u32 FlagAlbedo = 1;
constexpr u32 FlagNormal = 2;
constexpr u32 FlagG1 = 4;
constexpr u32 FlagG3 = 8;
constexpr std::array<vk::Format, 3> GuideFormats{vk::Format::eR8G8B8A8Unorm,
                                                 vk::Format::eR8G8B8A8Unorm,
                                                 vk::Format::eR16G16B16A16Sfloat};

bool IsSrgbFormat(vk::Format format) {
    return format == vk::Format::eR8G8B8A8Srgb || format == vk::Format::eB8G8R8A8Srgb ||
           format == vk::Format::eA8B8G8R8SrgbPack32;
}
}

DlssGuides::DlssGuides(const Instance& instance_, Scheduler& scheduler_, Runtime& runtime_,
                       VideoCore::TextureCache& texture_cache_, SceneTargets& scene_targets_)
    : instance{instance_}, scheduler{scheduler_}, runtime{runtime_},
      texture_cache{texture_cache_}, scene_targets{scene_targets_} {}

DlssGuides::~DlssGuides() = default;

bool DlssGuides::EnsurePipeline() {
    if (pipeline || pipeline_failed) {
        return bool(pipeline);
    }
    const auto device = instance.GetDevice();
    std::array<vk::DescriptorSetLayoutBinding, 8> bindings{};
    for (u32 i = 0; i < bindings.size(); ++i) {
        bindings[i] = {.binding = i,
                       .descriptorType = i < 5 ? vk::DescriptorType::eSampledImage
                                               : vk::DescriptorType::eStorageImage,
                       .descriptorCount = 1,
                       .stageFlags = vk::ShaderStageFlagBits::eCompute};
    }
    layout = Check(device.createDescriptorSetLayoutUnique({
        .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
        .bindingCount = u32(bindings.size()),
        .pBindings = bindings.data(),
    }));
    const vk::PushConstantRange push{.stageFlags = vk::ShaderStageFlagBits::eCompute,
                                     .offset = 0,
                                     .size = sizeof(Params)};
    pipeline_layout = Check(device.createPipelineLayoutUnique({
        .setLayoutCount = 1,
        .pSetLayouts = &*layout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &push,
    }));
    const auto module = CompileSPV(DLSS_RR_GUIDES_COMP, device);
    auto [result, created] = device.createComputePipelineUnique(
        {}, vk::ComputePipelineCreateInfo{
                .stage = {.stage = vk::ShaderStageFlagBits::eCompute,
                          .module = module,
                          .pName = "main"},
                .layout = *pipeline_layout,
            });
    device.destroyShaderModule(module);
    if (result != vk::Result::eSuccess) {
        pipeline_failed = true;
        std::printf("DLSS: Ray Reconstruction guide pipeline failed\n");
        return false;
    }
    pipeline = std::move(created);
    return true;
}

bool DlssGuides::EnsureTonemap() {
    if (tonemap_pipeline || tonemap_failed) {
        return bool(tonemap_pipeline);
    }
    const auto device = instance.GetDevice();
    const std::array<vk::DescriptorSetLayoutBinding, 2> bindings{{
        {.binding = 0,
         .descriptorType = vk::DescriptorType::eSampledImage,
         .descriptorCount = 1,
         .stageFlags = vk::ShaderStageFlagBits::eCompute},
        {.binding = 1,
         .descriptorType = vk::DescriptorType::eStorageImage,
         .descriptorCount = 1,
         .stageFlags = vk::ShaderStageFlagBits::eCompute},
    }};
    tonemap_layout = Check(device.createDescriptorSetLayoutUnique({
        .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
        .bindingCount = u32(bindings.size()),
        .pBindings = bindings.data(),
    }));
    const vk::PushConstantRange push{.stageFlags = vk::ShaderStageFlagBits::eCompute,
                                     .offset = 0,
                                     .size = sizeof(std::array<u32, 4>)};
    tonemap_pipeline_layout = Check(device.createPipelineLayoutUnique({
        .setLayoutCount = 1,
        .pSetLayouts = &*tonemap_layout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &push,
    }));
    const auto make = [&](std::span<const u32> code, vk::UniquePipeline& out) {
        const auto module = CompileSPV(code, device);
        auto [result, created] = device.createComputePipelineUnique(
            {}, vk::ComputePipelineCreateInfo{
                    .stage = {.stage = vk::ShaderStageFlagBits::eCompute,
                              .module = module,
                              .pName = "main"},
                    .layout = *tonemap_pipeline_layout,
                });
        device.destroyShaderModule(module);
        if (result != vk::Result::eSuccess) {
            return false;
        }
        out = std::move(created);
        return true;
    };
    if (!make(DLSS_RR_LINEARIZE_COMP, linearize_pipeline) ||
        !make(DLSS_RR_TONEMAP_COMP, tonemap_pipeline)) {
        tonemap_failed = true;
        linearize_pipeline.reset();
        tonemap_pipeline.reset();
        std::printf("DLSS: Ray Reconstruction tonemap pipeline failed\n");
        return false;
    }
    return true;
}

void DlssGuides::DispatchTonemap(vk::CommandBuffer cmdbuf, vk::Pipeline pass, vk::ImageView src,
                                 vk::ImageView dst, u32 w, u32 h) {
    const std::array<vk::DescriptorImageInfo, 2> infos{{
        {.imageView = src, .imageLayout = vk::ImageLayout::eGeneral},
        {.imageView = dst, .imageLayout = vk::ImageLayout::eGeneral},
    }};
    std::array<vk::WriteDescriptorSet, 2> writes{};
    for (u32 i = 0; i < writes.size(); ++i) {
        writes[i] = {.dstBinding = i,
                     .descriptorCount = 1,
                     .descriptorType = i == 0 ? vk::DescriptorType::eSampledImage
                                              : vk::DescriptorType::eStorageImage,
                     .pImageInfo = &infos[i]};
    }
    const std::array<u32, 4> size{w, h, 0, 0};
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, pass);
    if (src) {
        cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *tonemap_pipeline_layout, 0,
                                    writes);
    } else {
        cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *tonemap_pipeline_layout, 0,
                                    writes[1]);
    }
    cmdbuf.pushConstants(*tonemap_pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0,
                         sizeof(size), size.data());
    cmdbuf.dispatch((w + 15) / 16, (h + 15) / 16, 1);
}

bool DlssGuides::Linearize(vk::CommandBuffer cmdbuf, const Dlss::Resource& color,
                           Dlss::Resource& out) {
    if (!EnsureTonemap()) {
        return false;
    }
    if (linear_width != color.width || linear_height != color.height || !vk::Image(linear_image)) {
        scheduler.WaitSubmitted();
        const auto device = instance.GetDevice();
        linear_view.reset();
        linear_image = VideoCore::UniqueImage(device, instance.GetAllocator());
        linear_image.Create(vk::ImageCreateInfo{
            .imageType = vk::ImageType::e2D,
            .format = vk::Format::eR16G16B16A16Sfloat,
            .extent = {color.width, color.height, 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = vk::SampleCountFlagBits::e1,
            .tiling = vk::ImageTiling::eOptimal,
            .usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled,
            .initialLayout = vk::ImageLayout::eUndefined,
        });
        linear_view = Check(device.createImageViewUnique({
            .image = vk::Image(linear_image),
            .viewType = vk::ImageViewType::e2D,
            .format = vk::Format::eR16G16B16A16Sfloat,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        }));
        linear_width = color.width;
        linear_height = color.height;
        linear_fresh = true;
    }
    const auto all = vk::PipelineStageFlagBits2::eAllCommands;
    const std::array<vk::ImageMemoryBarrier2, 2> before{{
        {.srcStageMask = all,
         .srcAccessMask = vk::AccessFlagBits2::eMemoryRead,
         .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
         .dstAccessMask = vk::AccessFlagBits2::eShaderWrite,
         .oldLayout = linear_fresh ? vk::ImageLayout::eUndefined : vk::ImageLayout::eGeneral,
         .newLayout = vk::ImageLayout::eGeneral,
         .image = vk::Image(linear_image),
         .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}},
        {.srcStageMask = all,
         .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
         .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
         .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
         .oldLayout = vk::ImageLayout::eGeneral,
         .newLayout = vk::ImageLayout::eGeneral,
         .image = color.image,
         .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}},
    }};
    linear_fresh = false;
    cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = u32(before.size()),
                             .pImageMemoryBarriers = before.data()});
    DispatchTonemap(cmdbuf, *linearize_pipeline, color.view, *linear_view, color.width,
                    color.height);
    const vk::ImageMemoryBarrier2 after{
        .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .srcAccessMask = vk::AccessFlagBits2::eShaderWrite,
        .dstStageMask = all,
        .dstAccessMask = vk::AccessFlagBits2::eMemoryRead,
        .oldLayout = vk::ImageLayout::eGeneral,
        .newLayout = vk::ImageLayout::eGeneral,
        .image = vk::Image(linear_image),
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
    };
    cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &after});
    out = {vk::Image(linear_image), *linear_view, vk::Format::eR16G16B16A16Sfloat,
           vk::ImageAspectFlagBits::eColor, color.width, color.height};
    return true;
}

void DlssGuides::Tonemap(vk::CommandBuffer cmdbuf, const Dlss::Resource& output) {
    const auto all = vk::PipelineStageFlagBits2::eAllCommands;
    vk::ImageMemoryBarrier2 barrier{
        .srcStageMask = all,
        .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite,
        .oldLayout = vk::ImageLayout::eGeneral,
        .newLayout = vk::ImageLayout::eGeneral,
        .image = output.image,
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
    };
    cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier});
    DispatchTonemap(cmdbuf, *tonemap_pipeline, {}, output.view, output.width, output.height);
    barrier.srcStageMask = vk::PipelineStageFlagBits2::eComputeShader;
    barrier.srcAccessMask = vk::AccessFlagBits2::eShaderWrite;
    barrier.dstStageMask = all;
    barrier.dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite;
    cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier});
}

bool DlssGuides::EnsureResources(u32 new_width, u32 new_height) {
    if (width == new_width && height == new_height && vk::Image(images[0])) {
        return true;
    }
    scheduler.WaitSubmitted();
    const auto device = instance.GetDevice();
    const auto allocator = instance.GetAllocator();
    for (u32 i = 0; i < images.size(); ++i) {
        views[i].reset();
        images[i] = VideoCore::UniqueImage(device, allocator);
        images[i].Create(vk::ImageCreateInfo{
            .imageType = vk::ImageType::e2D,
            .format = GuideFormats[i],
            .extent = {new_width, new_height, 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = vk::SampleCountFlagBits::e1,
            .tiling = vk::ImageTiling::eOptimal,
            .usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled,
            .initialLayout = vk::ImageLayout::eUndefined,
        });
        views[i] = Check(device.createImageViewUnique({
            .image = vk::Image(images[i]),
            .viewType = vk::ImageViewType::e2D,
            .format = GuideFormats[i],
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        }));
    }
    width = new_width;
    height = new_height;
    images_fresh = true;
    return true;
}

bool DlssGuides::Bind(VideoCore::ImageId id, bool reduced, u32 want_width, u32 want_height,
                      vk::Image& image, vk::ImageView& view) {
    if (!id) {
        return false;
    }
    auto& source = texture_cache.GetImage(id);
    if (reduced) {
        if (!scene_targets.EligibleScene(source)) {
            return false;
        }
        VideoCore::ImageViewInfo info;
        info.format = source.info.pixel_format;
        const auto target = scene_targets.Read(id, info, vk::PipelineStageFlagBits2::eComputeShader,
                                               vk::AccessFlagBits2::eShaderRead);
        const auto size = scene_targets.Size();
        if (!target.image || !target.view || size.width != want_width ||
            size.height != want_height) {
            return false;
        }
        image = target.image;
        view = target.view;
        return true;
    }
    if (source.info.size.width != want_width || source.info.size.height != want_height) {
        return false;
    }
    runtime.Transit(&source, vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eComputeShader,
                    vk::AccessFlagBits2::eShaderRead);
    const auto device = instance.GetDevice();
    const auto created = Check(device.createImageView({
        .image = source.GetImage(),
        .viewType = vk::ImageViewType::e2D,
        .format = source.info.pixel_format,
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
    }));
    scheduler.DeferOperation([device, created] { device.destroyImageView(created); });
    image = source.GetImage();
    view = created;
    return true;
}

bool DlssGuides::Record(vk::CommandBuffer cmdbuf, const RayTracing::RrSource& source,
                        const Dlss::Resource& depth, bool reduced, const Camera& camera,
                        Dlss::RrGuides& out) {
    if (source.guides_ready) {
        const auto resource = [&](const RayTracing::Bound& b, vk::Format format) {
            return Dlss::Resource{b.image, b.view, format, vk::ImageAspectFlagBits::eColor,
                                  b.width, b.height};
        };
        out = {resource(source.diffuse_albedo, GuideFormats[0]),
               resource(source.specular_albedo, GuideFormats[1]),
               resource(source.normal_roughness, GuideFormats[2]),
               resource(source.specular_hit, vk::Format::eR16Sfloat)};
        return out.diffuse_albedo.image && out.specular_albedo.image &&
               out.normal_roughness.image;
    }
    if (!camera.valid || !EnsurePipeline() || !EnsureResources(depth.width, depth.height)) {
        return false;
    }
    int albedo_index = source.albedo_target;
    if (albedo_index < 0) {
        for (u32 i = 0; i < source.gbuffer.size(); ++i) {
            if (source.gbuffer[i] &&
                IsSrgbFormat(texture_cache.GetImage(source.gbuffer[i]).info.pixel_format)) {
                albedo_index = int(i);
                break;
            }
        }
    }
    const auto bind_target = [&](int index, vk::Format format, vk::Image& image,
                                 vk::ImageView& view) {
        if (index < 0 || u32(index) >= source.gbuffer.size() || !source.gbuffer[index]) {
            return false;
        }
        if (format != vk::Format::eUndefined &&
            texture_cache.GetImage(source.gbuffer[index]).info.pixel_format != format) {
            return false;
        }
        return Bind(source.gbuffer[index], reduced, depth.width, depth.height, image, view);
    };
    vk::Image albedo_image{}, normal_image{}, g1_image{}, g3_image{};
    vk::ImageView albedo_view{}, normal_view{}, g1_view{}, g3_view{};
    u32 flags = 0;
    if (bind_target(albedo_index, vk::Format::eUndefined, albedo_image, albedo_view)) {
        flags |= FlagAlbedo;
    }
    if (source.normal.target != 0xFF &&
        bind_target(int(source.normal.target), vk::Format::eUndefined, normal_image, normal_view)) {
        flags |= FlagNormal;
    }
    if (bind_target(source.roughness_target, vk::Format::eR8G8B8A8Unorm, g1_image, g1_view)) {
        flags |= FlagG1;
    }
    if (bind_target(source.specular_target, vk::Format::eR8G8B8A8Unorm, g3_image, g3_view)) {
        flags |= FlagG3;
    }
    runtime.FlushBarriers();
    if (!printed) {
        printed = true;
        std::printf("DLSS: Ray Reconstruction guides %ux%u, albedo %s, normal %s, roughness %s, "
                    "specular %s\n",
                    depth.width, depth.height, flags & FlagAlbedo ? "G-buffer" : "grey",
                    flags & FlagNormal ? "G-buffer" : "depth",
                    flags & FlagG1 ? "G-buffer 1" : "0.5", flags & FlagG3 ? "G-buffer 3" : "0.04");
    }

    const auto all = vk::PipelineStageFlagBits2::eAllCommands;
    std::array<vk::ImageMemoryBarrier2, 3> before{};
    for (u32 i = 0; i < before.size(); ++i) {
        before[i] = {
            .srcStageMask = all,
            .srcAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .dstAccessMask = vk::AccessFlagBits2::eShaderWrite,
            .oldLayout = images_fresh ? vk::ImageLayout::eUndefined : vk::ImageLayout::eGeneral,
            .newLayout = vk::ImageLayout::eGeneral,
            .image = vk::Image(images[i]),
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        };
    }
    images_fresh = false;
    cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = u32(before.size()),
                             .pImageMemoryBarriers = before.data()});

    const auto fallback = depth.view;
    const std::array<vk::ImageView, 8> bound{depth.view,
                                             albedo_view ? albedo_view : fallback,
                                             normal_view ? normal_view : fallback,
                                             g1_view ? g1_view : fallback,
                                             g3_view ? g3_view : fallback,
                                             *views[0],
                                             *views[1],
                                             *views[2]};
    std::array<vk::DescriptorImageInfo, 8> infos{};
    std::array<vk::WriteDescriptorSet, 8> writes{};
    for (u32 i = 0; i < infos.size(); ++i) {
        infos[i] = {.imageView = bound[i], .imageLayout = vk::ImageLayout::eGeneral};
        writes[i] = {.dstBinding = i,
                     .descriptorCount = 1,
                     .descriptorType = i < 5 ? vk::DescriptorType::eSampledImage
                                             : vk::DescriptorType::eStorageImage,
                     .pImageInfo = &infos[i]};
    }
    const Params params{
        .proj = camera.projection,
        .world_rows = Rt::Rows(camera.inv_view),
        .size_jitter = {float(depth.width), float(depth.height), camera.jitter[0],
                        camera.jitter[1]},
        .cfg = {source.normal.mode | source.normal.swizzle << 8 | source.normal.components << 16,
                flags, 0, 0},
    };
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, *pipeline);
    cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *pipeline_layout, 0, writes);
    cmdbuf.pushConstants(*pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(params),
                         &params);
    cmdbuf.dispatch((depth.width + 15) / 16, (depth.height + 15) / 16, 1);

    std::array<vk::ImageMemoryBarrier2, 3> after{};
    for (u32 i = 0; i < after.size(); ++i) {
        after[i] = {
            .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .srcAccessMask = vk::AccessFlagBits2::eShaderWrite,
            .dstStageMask = all,
            .dstAccessMask = vk::AccessFlagBits2::eMemoryRead,
            .oldLayout = vk::ImageLayout::eGeneral,
            .newLayout = vk::ImageLayout::eGeneral,
            .image = vk::Image(images[i]),
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        };
    }
    cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = u32(after.size()),
                             .pImageMemoryBarriers = after.data()});

    const auto own = [&](u32 i) {
        return Dlss::Resource{vk::Image(images[i]), *views[i], GuideFormats[i],
                              vk::ImageAspectFlagBits::eColor, width, height};
    };
    const auto& hit = source.specular_hit;
    const bool hit_ok = hit.image && hit.width == width && hit.height == height;
    const u32 hit_state = hit_ok ? 1u : 2u;
    if (!(hit_logged & hit_state)) {
        hit_logged |= hit_state;
        std::printf("DLSS: Ray Reconstruction specular hit distance %s\n",
                    hit_ok ? "from the ray-traced reflections" : "not given");
    }
    out = {own(0), own(1), own(2),
           hit_ok ? Dlss::Resource{hit.image, hit.view, vk::Format::eR16Sfloat,
                                   vk::ImageAspectFlagBits::eColor, hit.width, hit.height}
                  : Dlss::Resource{}};
    return true;
}

}
