// SPDX-License-Identifier: GPL-2.0-or-later
// Execute the production visualization shader and read every pixel back.
#include "video_core/host_shaders/upscale_motion_debug_comp.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_object_motion.h"
#include "video_core/texture_cache/image.h"
#include "shader_recompiler/runtime_info.h"
#if __has_include("shader_recompiler/motion_storage.h")
#include "shader_recompiler/motion_storage.h"
#endif
#include "bbport_settings.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <type_traits>
#include <vk_mem_alloc.h>

#ifdef _WIN32
extern "C" void runtime_restart() { std::abort(); }
#endif

namespace {
static_assert(std::is_same_v<decltype(std::declval<const Vulkan::ObjectMotion&>().CurrentView(1, 1)),
                             vk::ImageView>);

void Need(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
struct Staging {
    VmaAllocator allocator;
    VkBuffer buffer{};
    VmaAllocation allocation{};
    void* mapped{};
    Staging(VmaAllocator a, size_t bytes) : allocator(a) {
        const VkBufferCreateInfo ci{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                    .size = bytes,
                                    .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                             VK_BUFFER_USAGE_TRANSFER_DST_BIT};
        const VmaAllocationCreateInfo ac{.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT |
                                                  VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT,
                                         .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST};
        VmaAllocationInfo info{};
        Need(vmaCreateBuffer(a, &ci, &ac, &buffer, &allocation, &info) == VK_SUCCESS, "staging");
        mapped = info.pMappedData;
    }
    ~Staging() { vmaDestroyBuffer(allocator, buffer, allocation); }
};
void RunCase(const Vulkan::Instance& instance, Vulkan::Scheduler& scheduler, u32 w, u32 h, u32 rw,
             u32 rh, unsigned object_mode, unsigned pattern = 0) {
    const auto device = instance.GetDevice();
    const auto allocator = instance.GetAllocator();
    const u32 object_w = object_mode == 2 ? rw + 1 : object_mode == 3 ? rw - 1 : rw;
    const u32 object_h = object_mode == 4 ? rh - 1 : rh;
    const size_t rgba_bytes = size_t(w) * h * 4, motion_bytes = size_t(rw) * rh * 4;
    const size_t object_bytes = size_t(object_w) * object_h * 16;
    const size_t motion_offset = (rgba_bytes + 15) & ~size_t(15);
    const size_t object_offset = (motion_offset + motion_bytes + 15) & ~size_t(15);
    const size_t read_offset = object_offset + object_bytes;
    Staging staging(allocator, read_offset + rgba_bytes);
    auto* bytes = static_cast<unsigned char*>(staging.mapped);
    for (size_t p = 0; p < size_t(w) * h; ++p) {
        const std::array<unsigned char, 4> color{51, 102, 153, 191};
        std::memcpy(bytes + p * 4, color.data(), 4);
    }
    constexpr std::array<std::array<uint16_t, 3>, 4> x_halves{{
        {0x3c00, 0x4000, 0x4400}, {0xbc00, 0xc000, 0xc400},
        {0xcc00, 0x4800, 0x5000}, {0, 0, 0}}};
    constexpr std::array<std::array<uint16_t, 3>, 4> y_halves{{
        {0xbc00, 0xc000, 0xc400}, {0x3c00, 0x4000, 0x4400},
        {0, 0xc800, 0xd000}, {0, 0, 0}}};
    for (u32 y = 0; y < rh; ++y)
        for (u32 x = 0; x < rw; ++x) {
            const std::array<uint16_t, 2> mv{x_halves[pattern][x % 3], y_halves[pattern][y % 3]};
            std::memcpy(bytes + motion_offset + (size_t(y) * rw + x) * 4, mv.data(), 4);
        }
    for (u32 y = 0; y < object_h; ++y)
        for (u32 x = 0; x < object_w; ++x) {
            const std::array<float, 4> object{0, 0, x % 5 == 0 ? 1.f : 0.f, 0};
            std::memcpy(bytes + object_offset + (size_t(y) * object_w + x) * 16, object.data(), 16);
        }
    Need(vmaFlushAllocation(allocator, staging.allocation, 0, VK_WHOLE_SIZE) == VK_SUCCESS,
         "flush");
    std::array<VideoCore::UniqueImage, 3> images;
    std::array<vk::UniqueImageView, 3> views;
    const std::array widths{w, rw, object_w}, heights{h, rh, object_h};
    const std::array formats{vk::Format::eR8G8B8A8Unorm, vk::Format::eR16G16Sfloat,
                             vk::Format::eR32G32B32A32Sfloat};
    const vk::ImageSubresourceRange range{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
    for (u32 i = 0; i < 3; ++i) {
        images[i] = VideoCore::UniqueImage(device, allocator);
        images[i].Create(
            {.imageType = vk::ImageType::e2D,
             .format = formats[i],
             .extent = {widths[i], heights[i], 1},
             .mipLevels = 1,
             .arrayLayers = 1,
             .samples = vk::SampleCountFlagBits::e1,
             .tiling = vk::ImageTiling::eOptimal,
             .usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled |
                      vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst});
        auto [result, view] = device.createImageViewUnique({.image = vk::Image(images[i]),
                                                            .viewType = vk::ImageViewType::e2D,
                                                            .format = formats[i],
                                                            .subresourceRange = range});
        Need(result == vk::Result::eSuccess, "image view");
        views[i] = std::move(view);
    }
    std::array<vk::DescriptorSetLayoutBinding, 3> bindings{};
    for (u32 i = 0; i < 3; ++i)
        bindings[i] = {.binding = i,
                       .descriptorType = i == 0 ? vk::DescriptorType::eStorageImage
                                                : vk::DescriptorType::eSampledImage,
                       .descriptorCount = 1,
                       .stageFlags = vk::ShaderStageFlagBits::eCompute};
    auto [dr, descriptors] = device.createDescriptorSetLayoutUnique(
        {.flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
         .bindingCount = 3,
         .pBindings = bindings.data()});
    Need(dr == vk::Result::eSuccess, "descriptors");
    auto [lr, layout] =
        device.createPipelineLayoutUnique({.setLayoutCount = 1, .pSetLayouts = &*descriptors});
    Need(lr == vk::Result::eSuccess, "layout");
    auto [mr, module] = device.createShaderModuleUnique(
        {.codeSize = sizeof(UPSCALE_MOTION_DEBUG_COMP), .pCode = UPSCALE_MOTION_DEBUG_COMP});
    Need(mr == vk::Result::eSuccess, "module");
    auto [pr, pipeline] = device.createComputePipelineUnique(
        {},
        {.stage = {.stage = vk::ShaderStageFlagBits::eCompute, .module = *module, .pName = "main"},
         .layout = *layout});
    Need(pr == vk::Result::eSuccess, "pipeline");
    const auto cmd = scheduler.CommandBuffer();
    std::array<vk::ImageMemoryBarrier2, 3> transitions{};
    for (u32 i = 0; i < 3; ++i)
        transitions[i] = {.dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
                          .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
                          .oldLayout = vk::ImageLayout::eUndefined,
                          .newLayout = vk::ImageLayout::eGeneral,
                          .image = vk::Image(images[i]),
                          .subresourceRange = range};
    cmd.pipelineBarrier2(
        {.imageMemoryBarrierCount = 3, .pImageMemoryBarriers = transitions.data()});
    const std::array<size_t, 3> offsets{0, motion_offset, object_offset};
    for (u32 i = 0; i < 3; ++i) {
        const vk::BufferImageCopy region{
            .bufferOffset = offsets[i],
            .imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
            .imageExtent = {widths[i], heights[i], 1}};
        cmd.copyBufferToImage(staging.buffer, vk::Image(images[i]), vk::ImageLayout::eGeneral,
                              region);
    }
    const vk::MemoryBarrier2 before{.srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
                                    .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
                                    .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
                                    .dstAccessMask = vk::AccessFlagBits2::eShaderRead |
                                                     vk::AccessFlagBits2::eShaderWrite};
    cmd.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &before});
    std::array<vk::DescriptorImageInfo, 3> infos{};
    std::array<vk::WriteDescriptorSet, 3> writes{};
    for (u32 i = 0; i < 3; ++i) {
        infos[i] = {.imageView = *views[object_mode == 1 && i == 2 ? 1 : i],
                    .imageLayout = vk::ImageLayout::eGeneral};
        writes[i] = {.dstBinding = i,
                     .descriptorCount = 1,
                     .descriptorType = bindings[i].descriptorType,
                     .pImageInfo = &infos[i]};
    }
    cmd.bindPipeline(vk::PipelineBindPoint::eCompute, *pipeline);
    cmd.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *layout, 0, writes);
    cmd.dispatch((w + 7) / 8, (h + 7) / 8, 1);
    const vk::MemoryBarrier2 after{.srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
                                   .srcAccessMask = vk::AccessFlagBits2::eShaderWrite,
                                   .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
                                   .dstAccessMask = vk::AccessFlagBits2::eTransferRead};
    cmd.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &after});
    const vk::BufferImageCopy read{.bufferOffset = read_offset,
                                   .imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
                                   .imageExtent = {w, h, 1}};
    cmd.copyImageToBuffer(vk::Image(images[0]), vk::ImageLayout::eGeneral, staging.buffer, read);
    const vk::MemoryBarrier2 host{.srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
                                  .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
                                  .dstStageMask = vk::PipelineStageFlagBits2::eHost,
                                  .dstAccessMask = vk::AccessFlagBits2::eHostRead};
    cmd.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &host});
    scheduler.Finish();
    Need(vmaInvalidateAllocation(allocator, staging.allocation, 0, VK_WHOLE_SIZE) == VK_SUCCESS,
         "invalidate");
    constexpr std::array<std::array<float, 3>, 4> x_motion{{
        {1, 2, 4}, {-1, -2, -4}, {-16, 8, 32}, {0, 0, 0}}};
    constexpr std::array<std::array<float, 3>, 4> y_motion{{
        {-1, -2, -4}, {1, 2, 4}, {0, -8, -32}, {0, 0, 0}}};
    for (u32 y = 0; y < h; ++y)
        for (u32 x = 0; x < w; ++x) {
            const u32 sx = x * rw / w, sy = y * rh / h;
            const std::array<float, 4> expected{
                0.108f + std::min(std::abs(x_motion[pattern][sx % 3]) / 8.f, 1.f),
                0.108f + std::min(std::abs(y_motion[pattern][sy % 3]) / 8.f, 1.f),
                0.108f + (object_mode == 0 && sx % 5 == 0 ? 0.6f : 0.f), 191.f / 255.f};
            for (u32 c = 0; c < 4; ++c)
                Need(std::abs(int(bytes[read_offset + (size_t(y) * w + x) * 4 + c]) -
                              int(std::lround(std::clamp(expected[c], 0.f, 1.f) * 255.f))) <= 1,
                     "motion mapping, object coverage or alpha");
        }
    std::printf("PASS scaled motion %ux%u <- %ux%u objects=%u pattern=%u; all pixels and alpha\n",
                w, h, rw, rh, object_mode, pattern);
}

const char* CoverageSkipReason(bool vertex_stores, bool guarded_gpu, bool discrete, u64 vram,
                               bool forced, bool storage_limits = true) {
    // Match ObjectMotion's pre-allocation guards; setup/allocation failures must fail.
    if (!vertex_stores)
        return "vertex shader stores/atomics unavailable";
    if (!storage_limits)
        return "required SSBO limits unavailable";
    if (!forced && guarded_gpu)
        return "existing GPU/driver guard";
    if (!forced && discrete && vram < (6ull << 30) - (256ull << 20))
        return "existing low-VRAM policy";
    return nullptr;
}

void TestCoveragePolicy() {
    constexpr u64 enough = 8ull << 30, low = 4ull << 30;
    Need(CoverageSkipReason(false, false, true, enough, true) != nullptr, "missing stores must skip");
    Need(CoverageSkipReason(true, true, true, enough, false) != nullptr, "guarded GPU must skip");
    Need(!CoverageSkipReason(true, true, true, enough, true), "explicit GPU override ignored");
    Need(CoverageSkipReason(true, false, true, low, false) != nullptr, "low VRAM must skip");
    Need(!CoverageSkipReason(true, false, true, low, true), "explicit VRAM override ignored");
    Need(!CoverageSkipReason(true, false, false, low, false), "integrated GPU incorrectly guarded");
    Need(!CoverageSkipReason(true, false, true, enough, false), "supported GPU must run coverage");
    constexpr u64 threshold = (6ull << 30) - (256ull << 20);
    Need(!CoverageSkipReason(true, false, true, threshold, false), "VRAM boundary must run coverage");
    Need(CoverageSkipReason(true, false, true, threshold - 1, false) != nullptr, "VRAM below boundary must skip");
    Need(CoverageSkipReason(true, false, true, enough, true, false) != nullptr,
         "explicit override must not bypass required SSBO limits");
    Need(CoverageSkipReason(true, false, true, enough, false, false) != nullptr,
         "missing SSBO limits must skip");
    // A constructor/resource failure on supported hardware is not an expected skip.
    Need(!CoverageSkipReason(true, false, true, enough, false, true),
         "supported policy incorrectly hides a resource failure");
    std::puts("PASS current-frame coverage policy: features, SSBO limits, guards, VRAM and overrides");
}

bool CurrentFrameCoverage(const Vulkan::Instance& instance, Vulkan::Scheduler& scheduler) {
    BbSettings::Get().object_motion = true;
    BbSettings::Get().upscaler = BbSettings::UpscalerFsr3;
    const auto physical = instance.GetPhysicalDevice();
    const auto props = physical.getProperties();
    bool guarded_gpu = props.vendorID == 0x1002 && props.deviceID < 0x7440;
    bool storage_limits = true;
#if __has_include("shader_recompiler/motion_storage.h")
    // The independent RX6000 fix lifts this guard for its automatic SSBO path.
    const bool ssbo = Shader::MotionStorage::UsesSsbo(props.vendorID, props.deviceID);
    guarded_gpu &= !ssbo;
    const auto& limits = props.limits;
    storage_limits = !ssbo || (limits.maxVertexOutputComponents >= 128 &&
        limits.maxFragmentInputComponents >= 128 &&
        limits.maxPerStageDescriptorStorageBuffers >= 3 && limits.maxBoundDescriptorSets >= 2 &&
        limits.maxStorageBufferRange >= vk::DeviceSize(4u << 20) * 16 + 16);
#endif
    const auto& memory = instance.GetMemoryProperties();
    u64 vram = 0;
    for (u32 heap = 0; heap < memory.memoryHeapCount; ++heap)
        if (memory.memoryHeaps[heap].flags & vk::MemoryHeapFlagBits::eDeviceLocal)
            vram = std::max<u64>(vram, memory.memoryHeaps[heap].size);
    const char* forced = std::getenv("BB_OBJECT_MOTION");
    if (const char* reason = CoverageSkipReason(physical.getFeatures().vertexPipelineStoresAndAtomics,
            guarded_gpu, props.deviceType == vk::PhysicalDeviceType::eDiscreteGpu, vram,
            forced && forced[0] == '1', storage_limits)) {
        std::printf("SKIP current-frame coverage: %s\n", reason);
        return false;
    }
    Vulkan::ObjectMotion motion(instance, scheduler);
    Need(motion.Enabled(), "object motion unexpectedly unavailable after policy checks (buffer/setup failure)");
    Need(!motion.CurrentView(7, 5), "new target has current coverage");
    Staging staging(instance.GetAllocator(), 16);
    const std::array<std::array<u32, 2>, 4> sizes{{{7, 5}, {7, 5}, {3, 2}, {7, 5}}};
    for (const auto [w, h] : sizes) {
        motion.OnFrameStart();
        Need(!motion.CurrentView(w, h), "unwritten frame exposes coverage");
        Vulkan::RenderState state{};
        state.width = w;
        state.height = h;
        state.num_layers = 1;
        motion.Attach(state, w, h);
        // A real retained blue pixel, independent of whether it is used next frame.
        state.color_attachments[Shader::MotionVectors::Output].clear_value = {0, 0, 0x3f800000, 0};
        scheduler.BeginRendering(state);
        scheduler.EndRendering();
        auto cmd = scheduler.CommandBuffer();
        bool valid = false;
        motion.PrepareRead(cmd, w, h, valid);
        Need(valid && motion.CurrentView(w, h) == motion.View(), "current target rejected");
        Need(!motion.CurrentView(w + 1, h) && !motion.CurrentView(w, h + 1),
             "wrong render size exposes coverage");
        const auto previous_image = motion.Image(w, h);
        const auto previous_view = motion.View();
        motion.OnFrameStart();
        Need(motion.View() == previous_view && !motion.CurrentView(w, h),
             "retained previous-frame view accepted as current");
        cmd = scheduler.CommandBuffer();
        const vk::MemoryBarrier2 before{
            .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
            .dstAccessMask = vk::AccessFlagBits2::eTransferRead};
        cmd.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &before});
        const vk::BufferImageCopy copy{
            .imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
            .imageExtent = {1, 1, 1}};
        cmd.copyImageToBuffer(previous_image, vk::ImageLayout::eGeneral, staging.buffer, copy);
        const vk::MemoryBarrier2 host{
            .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
            .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eHost,
            .dstAccessMask = vk::AccessFlagBits2::eHostRead};
        cmd.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &host});
        scheduler.Finish();
        Need(vmaInvalidateAllocation(instance.GetAllocator(), staging.allocation, 0, VK_WHOLE_SIZE)
                 == VK_SUCCESS, "coverage invalidate");
        Need(static_cast<const u32*>(staging.mapped)[2] == 0x3f800000,
             "stale-target test did not retain a blue pixel");
        std::printf("PASS current-frame coverage %ux%u: current, wrong-size, retained stale pixel\n", w, h);
    }
    return true;
}
} // namespace
int main(int argc, char** argv) {
    try {
        TestCoveragePolicy();
        if (argc == 2 && std::strcmp(argv[1], "--policy-only") == 0)
            return 0;
        Vulkan::Instance instance(0, std::getenv("BB_TEST_VULKAN_VALIDATION") != nullptr);
        VULKAN_HPP_DEFAULT_DISPATCHER.init(instance.GetInstance());
        VULKAN_HPP_DEFAULT_DISPATCHER.init(instance.GetDevice());
        Vulkan::Scheduler scheduler(instance);
        for (unsigned mode = 0; mode < 5; ++mode)
            RunCase(instance, scheduler, 13, 9, 7, 5, mode);
        RunCase(instance, scheduler, 2560, 1440, 1707, 960, 0);
        RunCase(instance, scheduler, 3840, 2160, 2560, 1440, 0);
        RunCase(instance, scheduler, 1920, 1080, 960, 540, 0);
        RunCase(instance, scheduler, 3840, 2160, 3840, 2160, 0);
        RunCase(instance, scheduler, 13, 9, 13, 9, 0);
        for (unsigned pattern = 1; pattern < 4; ++pattern)
            RunCase(instance, scheduler, 13, 9, 7, 5, 0, pattern);
        const bool coverage = CurrentFrameCoverage(instance, scheduler);
        std::puts(coverage ? "PASS scaled motion: shader and current-frame coverage complete"
                           : "PASS scaled motion: shader cases complete; current-frame coverage SKIPPED");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL scaled motion: %s\n", e.what());
        return 1;
    }
}
