// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/debug.h"
#include "common/elf_info.h"
#include "common/io_file.h"
#include "common/path_util.h"
#include "common/singleton.h"
#include "common/thread.h"
#include "core/debug_state.h"
#include "core/emulator_settings.h"
#include "sdl_window.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/renderdoc.h"
#include "video_core/renderer_vulkan/vk_platform.h"
#include "bbport_overlay.h"
#include "video_core/renderer_vulkan/vk_temporal_upscaler.h"
#include "video_core/renderer_vulkan/vk_presenter.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/texture_cache/image.h"
#include "bbport_settings.h"
#include "bbport_threads.h"
#include "video_core/renderer_vulkan/vk_fg_images.h"
#include "video_core/renderer_vulkan/vk_reflex.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/host_shaders/fg_hud_comp.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <csetjmp>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
#include <span>
#include <sstream>
#include <system_error>
#include <vector>
#include <vk_mem_alloc.h>
#ifdef MemoryBarrier
#undef MemoryBarrier // bbport: winnt.h macro (through fmt), clashes with vk::MemoryBarrier
#endif

namespace Vulkan {

struct FgFrameResources {
    FgImage color, full, depth, motion, ui;
    std::array<FgImage,3> outputs, presented;
    std::array<vk::ImageView,3> srgb_views{};
    vk::Device device{};
    vk::QueryPool timestamps{};
    u64 serial{};
    u32 count{};
    float frame_ms{16.6667f};
    bool timestamp_valid{};
    vk::Format sample_format{};
    ~FgFrameResources() {
        for (auto view : srgb_views) if (view) device.destroyImageView(view);
        if (timestamps) device.destroyQueryPool(timestamps);
    }
};

static FrameGeneration::Resource FgResource(const FgImage& image) {
    return {image.image,image.view,image.format,vk::ImageAspectFlagBits::eColor,image.width,image.height};
}
static std::int64_t FgNow() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool CanBlitToSwapchain(const vk::PhysicalDevice physical_device, vk::Format format) {
    const vk::FormatProperties props{physical_device.getFormatProperties(format)};
    return static_cast<bool>(props.optimalTilingFeatures & vk::FormatFeatureFlagBits::eBlitDst);
}

[[nodiscard]] vk::ImageSubresourceLayers MakeImageSubresourceLayers() {
    return vk::ImageSubresourceLayers{
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .mipLevel = 0,
        .baseArrayLayer = 0,
        .layerCount = 1,
    };
}

[[nodiscard]] vk::ImageBlit MakeImageBlit(s32 frame_width, s32 frame_height, s32 dst_width,
                                          s32 dst_height, s32 offset_x, s32 offset_y) {
    return vk::ImageBlit{
        .srcSubresource = MakeImageSubresourceLayers(),
        .srcOffsets =
            std::array{
                vk::Offset3D{
                    .x = 0,
                    .y = 0,
                    .z = 0,
                },
                vk::Offset3D{
                    .x = frame_width,
                    .y = frame_height,
                    .z = 1,
                },
            },
        .dstSubresource = MakeImageSubresourceLayers(),
        .dstOffsets =
            std::array{
                vk::Offset3D{
                    .x = offset_x,
                    .y = offset_y,
                    .z = 0,
                },
                vk::Offset3D{
                    .x = offset_x + dst_width,
                    .y = offset_y + dst_height,
                    .z = 1,
                },
            },
    };
}

[[nodiscard]] vk::ImageBlit MakeImageBlitStretch(s32 frame_width, s32 frame_height,
                                                 s32 swapchain_width, s32 swapchain_height) {
    return MakeImageBlit(frame_width, frame_height, swapchain_width, swapchain_height, 0, 0);
}

static vk::Rect2D FitImage(s32 frame_width, s32 frame_height, s32 swapchain_width,
                           s32 swapchain_height) {
    float frame_aspect = static_cast<float>(frame_width) / frame_height;
    float swapchain_aspect = static_cast<float>(swapchain_width) / swapchain_height;

    u32 dst_width = swapchain_width;
    u32 dst_height = swapchain_height;

    if (frame_aspect > swapchain_aspect) {
        dst_height = static_cast<s32>(swapchain_width / frame_aspect);
    } else {
        dst_width = static_cast<s32>(swapchain_height * frame_aspect);
    }

    const s32 offset_x = (swapchain_width - dst_width) / 2;
    const s32 offset_y = (swapchain_height - dst_height) / 2;

    return vk::Rect2D{{offset_x, offset_y}, {dst_width, dst_height}};
}

[[nodiscard]] vk::ImageBlit MakeImageBlitFit(s32 frame_width, s32 frame_height, s32 swapchain_width,
                                             s32 swapchain_height) {
    const auto& dst_rect = FitImage(frame_width, frame_height, swapchain_width, swapchain_height);

    return MakeImageBlit(frame_width, frame_height, dst_rect.extent.width, dst_rect.extent.height,
                         dst_rect.offset.x, dst_rect.offset.y);
}

// bbport: screenshot capture and ImGui overlays removed; the port presents directly.

Presenter::Presenter(Frontend::WindowSDL& window_, AmdGpu::Liverpool* liverpool_)
    : window{window_}, liverpool{liverpool_},
      instance{window, EmulatorSettings.GetGpuId(), EmulatorSettings.IsVkValidationEnabled(),
               EmulatorSettings.IsVkCrashDiagnosticEnabled()},
      draw_scheduler{instance, true}, present_scheduler{instance}, flip_scheduler{instance},
      swapchain{instance, window}, runtime{instance, draw_scheduler},
      rasterizer{std::make_unique<Rasterizer>(instance, draw_scheduler, runtime, liverpool)},
      texture_cache{rasterizer->GetTextureCache()} {
    const u32 num_images = swapchain.GetImageCount();
    const vk::Device device = instance.GetDevice();

    // Create presentation frames.
    present_frames.resize(num_images);
    for (u32 i = 0; i < num_images; i++) {
        Frame& frame = present_frames[i];
        frame.id = i;
        auto fence = Check<"create present done fence">(
            device.createFence({.flags = vk::FenceCreateFlagBits::eSignaled}));
        frame.present_done = fence;
        free_queue.push(&frame);
    }

    fsr_settings.enable = EmulatorSettings.IsFsrEnabled();
    fsr_settings.use_rcas = EmulatorSettings.IsRcasEnabled();
    fsr_settings.rcas_attenuation =
        static_cast<float>(EmulatorSettings.GetRcasAttenuation() / 1000.f);

    fsr_pass.Create(device, instance.GetAllocator(), num_images);
    pp_pass.Create(device, swapchain.GetSurfaceFormat().format);
    BbOverlay::Init(instance, swapchain.GetSurfaceFormat().format, num_images);
    present_scheduler.SetOutOfBand(true);
    present_scheduler.SetPresentQueue(true);
    flip_scheduler.SetOutOfBand(true);

    generation_factor = BbSettings::Get().startup_frame_generation;
    generation_backend = BbSettings::Get().startup_fg_backend;
    if (generation_factor >= 2) {
        generation = std::make_unique<FrameGeneration>(instance, draw_scheduler);
        std::printf("Frame generation: requested %s %dx; awaiting scene guides and provider capability\n",
            generation_backend == 1 ? "FSR" : "DLSS",generation_factor);
    }

}

Presenter::~Presenter() {

    draw_scheduler.Finish();
    present_scheduler.Finish();
    flip_scheduler.Finish();
    generation.reset();
    Check(draw_scheduler.CommandBuffer().reset());
    Check(present_scheduler.CommandBuffer().reset());
    Check(flip_scheduler.CommandBuffer().reset());

    const vk::Device device = instance.GetDevice();
    for (auto& frame : present_frames) {
        vmaDestroyImage(instance.GetAllocator(), frame.image, frame.allocation);
        device.destroyImageView(frame.image_view);
        device.destroyFence(frame.present_done);
    }
}

bool Presenter::IsVideoOutSurface(const AmdGpu::ColorBuffer& color_buffer) const {
    return std::ranges::find(vo_buffers_addr, color_buffer.Address()) != vo_buffers_addr.cend();
}

void Presenter::RecreateFrame(Frame* frame, u32 width, u32 height) {
    const vk::Device device = instance.GetDevice();
    if (frame->image_view) {
        device.destroyImageView(frame->image_view);
    }
    if (frame->image) {
        vmaDestroyImage(instance.GetAllocator(), frame->image, frame->allocation);
    }

    const vk::Format format = swapchain.GetSurfaceFormat().format;
    const vk::ImageCreateInfo image_info = {
        .flags = vk::ImageCreateFlagBits::eMutableFormat,
        .imageType = vk::ImageType::e2D,
        .format = format,
        .extent = {width, height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .usage = vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eTransferDst |
                 vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eSampled,
    };

    const VmaAllocationCreateInfo alloc_info = {
        .flags = VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
        .requiredFlags = 0,
        .preferredFlags = 0,
        .pool = VK_NULL_HANDLE,
        .pUserData = nullptr,
    };

    VkImage unsafe_image{};
    VkImageCreateInfo unsafe_image_info = static_cast<VkImageCreateInfo>(image_info);

    VkResult result = vmaCreateImage(instance.GetAllocator(), &unsafe_image_info, &alloc_info,
                                     &unsafe_image, &frame->allocation, nullptr);
    if (result != VK_SUCCESS) [[unlikely]] {
        LOG_CRITICAL(Render_Vulkan, "Failed allocating texture with error {}",
                     vk::to_string(vk::Result{result}));
        UNREACHABLE();
    }
    frame->image = vk::Image{unsafe_image};
    SetObjectName(device, frame->image, "Frame image #{}", frame->id);

    const vk::ImageViewCreateInfo view_info = {
        .image = frame->image,
        .viewType = vk::ImageViewType::e2D,
        .format = format,
        .subresourceRange{
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
    };
    auto view = Check<"create frame image view">(device.createImageView(view_info));
    frame->image_view = view;
    frame->width = width;
    frame->height = height;

    frame->is_hdr = swapchain.GetHDR();
}

Frame* Presenter::PrepareLastFrame() {
    if (last_submit_frame == nullptr) {
        return nullptr;
    }

    Frame* frame = last_submit_frame;

    while (true) {
        vk::Result result = instance.GetDevice().waitForFences(frame->present_done, false,
                                                               std::numeric_limits<u64>::max());
        if (result == vk::Result::eSuccess) {
            break;
        }
        if (result == vk::Result::eTimeout) {
            continue;
        }
        ASSERT_MSG(result != vk::Result::eErrorDeviceLost,
                   "Device lost during waiting for a frame");
    }

    auto& scheduler = flip_scheduler;
    scheduler.EndRendering();
    const auto cmdbuf = scheduler.CommandBuffer();

    const auto frame_subresources = vk::ImageSubresourceRange{
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .baseMipLevel = 0,
        .levelCount = 1,
        .baseArrayLayer = 0,
        .layerCount = VK_REMAINING_ARRAY_LAYERS,
    };

    const auto pre_barrier =
        vk::ImageMemoryBarrier2{.srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                                .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentRead,
                                .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                                .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
                                .oldLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
                                .newLayout = vk::ImageLayout::eGeneral,
                                .image = frame->image,
                                .subresourceRange{frame_subresources}};

    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &pre_barrier,
    });

    // Flush frame creation commands.
    frame->ready_semaphore = scheduler.GetWorkSemaphore()->Handle();
    frame->ready_tick = scheduler.CurrentTick();
    SubmitInfo info{};
    scheduler.Flush(info);
    return frame;
}

static vk::Format GetFrameViewFormat(const Libraries::VideoOut::PixelFormat format) {
    switch (format) {
    case Libraries::VideoOut::PixelFormat::A8B8G8R8Srgb:
        return vk::Format::eR8G8B8A8Srgb;
    case Libraries::VideoOut::PixelFormat::A8R8G8B8Srgb:
        return vk::Format::eB8G8R8A8Srgb;
    case Libraries::VideoOut::PixelFormat::A2R10G10B10:
    case Libraries::VideoOut::PixelFormat::A2R10G10B10Srgb:
    case Libraries::VideoOut::PixelFormat::A2R10G10B10Bt2020Pq:
        return vk::Format::eA2R10G10B10UnormPack32;
    default:
        break;
    }
    UNREACHABLE_MSG("Unknown format={}", static_cast<u32>(format));
    return {};
}

// bbport: raw copy of an image for BB_FRAME_DUMP_TRIGGER, written once the GPU is done.
static void DumpRaw(const Instance& instance, Scheduler& scheduler, vk::CommandBuffer cmdbuf,
                    vk::Image image, vk::ImageLayout layout, u32 w, u32 h, const char* name,
                    int index) {
    const VkBufferCreateInfo buffer_ci{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                       .size = VkDeviceSize(w) * h * 4,
                                       .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    const VmaAllocationCreateInfo alloc_ci{
        .flags = VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST};
    VkBuffer buffer{};
    VmaAllocation allocation{};
    VmaAllocationInfo info{};
    if (vmaCreateBuffer(instance.GetAllocator(), &buffer_ci, &alloc_ci, &buffer, &allocation,
                        &info) != VK_SUCCESS) {
        return;
    }
    cmdbuf.copyImageToBuffer(image, layout, buffer,
                             vk::BufferImageCopy{.imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
                                                 .imageExtent = {w, h, 1}});
    const char* dir = std::getenv("BB_DUMP_DIR");
    char path[512];
    std::snprintf(path, sizeof(path), "%s/%s_%03d_%ux%u.raw", dir && *dir ? dir : "out/dump", name,
                  index, w, h);
    scheduler.DeferPriorityOperation([allocator = instance.GetAllocator(), buffer, allocation, info,
                                      size = buffer_ci.size, file = std::string{path}] {
        vmaInvalidateAllocation(allocator, allocation, 0, VK_WHOLE_SIZE);
        if (FILE* f = std::fopen(file.c_str(), "wb")) {
            std::fwrite(info.pMappedData, 1, size, f);
            std::fclose(f);
        }
        vmaDestroyBuffer(allocator, buffer, allocation);
    });
}

// Opt-in, one-shot diagnostic capture. Own the readback allocations until the
// draw timeline completes; never read an image that a later frame can overwrite.
static void CaptureFg(const Instance& instance, Scheduler& scheduler, vk::CommandBuffer cmd,
                      const FgFrameResources& resources, const BbDlssCamera& camera,
                      u64 reflex_id, bool reset, const Frame& frame) {
    static const char* directory = std::getenv("BB_FG_CAPTURE_DIR");
    static const u64 first = [] {
        const char* value = std::getenv("BB_FG_CAPTURE_FRAME");
        return value ? std::max<u64>(1, std::strtoull(value, nullptr, 10)) : 120;
    }();
    static bool captured = false;
    if (!directory || !*directory || captured || resources.serial < first || !resources.count) return;
    struct Readback {
        VkBuffer buffer{};
        VmaAllocation allocation{};
        VmaAllocationInfo info{};
        VkDeviceSize bytes{};
        vk::Image image{};
        u32 width{}, height{};
        std::string name;
    };
    struct Capture {
        VmaAllocator allocator{};
        std::vector<Readback> buffers;
        ~Capture() {
            for (const auto& b : buffers)
                if (b.buffer) vmaDestroyBuffer(allocator, b.buffer, b.allocation);
        }
    };
    const auto capture = std::make_shared<Capture>();
    capture->allocator = instance.GetAllocator();
    const auto add_raw = [&](vk::Image image, u32 width, u32 height, const char* name) {
        Readback b{.bytes = VkDeviceSize(width) * height * 4,
                   .image = image, .width = width, .height = height, .name = name};
        const VkBufferCreateInfo ci{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                    .size = b.bytes, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
        const VmaAllocationCreateInfo allocation{
            .flags = VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT |
                     VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT,
            .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST};
        if (vmaCreateBuffer(capture->allocator, &ci, &allocation, &b.buffer, &b.allocation,
                            &b.info) != VK_SUCCESS) return false;
        capture->buffers.push_back(std::move(b));
        return true;
    };
    const auto add = [&](const FgImage& image, const char* name) {
        return add_raw(image.image,image.width,image.height,name);
    };
    if (!add(resources.depth, "depth.bin") || !add(resources.motion, "motion.bin") ||
        !add(resources.color, "color_rgba8.raw") || !add(resources.full, "full_rgba8.raw") ||
        !add(resources.ui, "ui_rgba8.raw")) return;
    for (u32 i = 0; i < resources.count; ++i) {
        const auto name = "generated_" + std::to_string(i + 1) + "_rgba8.raw";
        if (!add(resources.outputs[i], name.c_str())) return;
        const auto fitted = "presented_" + std::to_string(i + 1) + ".raw";
        if (!add(resources.presented[i], fitted.c_str())) return;
    }
    if (!add_raw(frame.image,frame.width,frame.height,"presented_real.raw")) return;
    const auto path = std::filesystem::path(directory) / ("frame_" + std::to_string(resources.serial));
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
    if (ec) return;
    for (const auto& b : capture->buffers) {
        const vk::ImageMemoryBarrier2 barrier{
            .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
            .dstAccessMask = vk::AccessFlagBits2::eTransferRead,
            .oldLayout = vk::ImageLayout::eGeneral, .newLayout = vk::ImageLayout::eGeneral,
            .image = b.image, .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}};
        cmd.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier});
        cmd.copyImageToBuffer(b.image, vk::ImageLayout::eGeneral, b.buffer,
            vk::BufferImageCopy{.imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
                                .imageExtent = {b.width, b.height, 1}});
    }
    std::ostringstream manifest;
    manifest << std::setprecision(9) << "{\n\"width\":" << resources.depth.width
        << ",\"height\":" << resources.depth.height
        << ",\"output_width\":" << resources.color.width
        << ",\"output_height\":" << resources.color.height
        << ",\"sample_format\":" << u32(resources.sample_format)
        << ",\"presentation_width\":" << frame.width
        << ",\"presentation_height\":" << frame.height
        << ",\"presentation_format\":" << u32(resources.presented[0].format)
        << ",\"depth_format\":100,\"motion_format\":83,\"render_frame_index\":" << resources.serial
        << ",\"frame_index\":" << resources.serial << ",\"reflex_frame_id\":" << reflex_id
        << ",\"frame_ms\":" << resources.frame_ms << ",\"reset\":" << (reset ? "true" : "false")
        << ",\"generated_count\":" << resources.count << ",\"jitter\":[" << camera.jitter[0]
        << ',' << camera.jitter[1] << "],\"camera\":{\"near_plane\":" << camera.near_plane
        << ",\"far_plane\":" << camera.far_plane << ",\"vertical_fov\":" << camera.vertical_fov << "}}\n";
    scheduler.DeferPriorityOperation([capture, path, text = manifest.str()] {
        bool complete = true;
        for (const auto& b : capture->buffers) {
            if (vmaInvalidateAllocation(capture->allocator, b.allocation, 0, VK_WHOLE_SIZE) != VK_SUCCESS) {
                complete = false;
                continue;
            }
            std::ofstream file(path / b.name, std::ios::binary);
            file.write(static_cast<const char*>(b.info.pMappedData), b.bytes);
            complete &= bool(file);
        }
        // Publish the manifest only after all raw files were successfully written.
        if (complete) std::ofstream(path / "guides_manifest.json") << text;
    });
    captured = true;
}

Frame* Presenter::PrepareFrame(const Libraries::VideoOut::BufferAttributeGroup& attribute,
                               VAddr cpu_address, u64 content_frame_id) {
    // bbport: BB_FRAME_DUMP_TRIGGER=<file>: when the file exists it is removed and this frame's
    // guest display buffer and presented image are written to BB_DUMP_DIR (default out/dump)
    // as raw 32-bit pixels (menus and movies included; the upscaler dumps scene frames only).
    static const char* frame_dump_trigger = std::getenv("BB_FRAME_DUMP_TRIGGER");
    static int frame_dump_index = 0;
    const bool frame_dump = frame_dump_trigger && std::remove(frame_dump_trigger) == 0;
    // bbport: scaled upscaler presets: the output-size display buffer drawn by the port.
    TemporalUpscaler::Display display{};
    const bool upscaled = rasterizer->GetUpscaler().DisplayOverride(cpu_address, display);
    VideoCore::ImageId image_id{};
    if (!upscaled) {
        auto desc = VideoCore::TextureCache::ImageDesc{attribute, cpu_address};
        image_id = texture_cache.FindImage(desc);
        texture_cache.UpdateImage(image_id);
    }

    Frame* frame = GetRenderFrame();
    frame->guest_address=cpu_address;
    // bbport: the accepted flip identity is authoritative (zero at startup is intentional); the
    // live/thread-local counters remain in use only where no flip has been accepted.
    frame->reflex_frame_id = content_frame_id;
    static const bool submit_trace = std::getenv("BB_REFLEX_SUBMIT_TRACE") != nullptr;
    if (submit_trace) {
        std::printf("reflex submit trace: phase=entry frame=%u accepted=%llu inherited=%llu "
                    "recording=%llu stageB=%d tid=%d\n",
                    frame->id, static_cast<unsigned long long>(content_frame_id),
                    static_cast<unsigned long long>(Reflex::RecordingFrameId()),
                    static_cast<unsigned long long>(draw_scheduler.GetFrameId()),
                    DrawPipe::OnStageB() ? 1 : 0, static_cast<int>(gettid()));
    }
    if (frame->generation) {
        frame->generation->count = 0;
        frame->generation->serial = 0;
        frame->generation->timestamp_valid = false;
    }

    const auto frame_subresources = vk::ImageSubresourceRange{
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .baseMipLevel = 0,
        .levelCount = 1,
        .baseArrayLayer = 0,
        .layerCount = VK_REMAINING_ARRAY_LAYERS,
    };

    const auto pre_barrier = vk::ImageMemoryBarrier2{
        .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentRead,
        .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
        .oldLayout = vk::ImageLayout::eUndefined,
        .newLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .image = frame->image,
        .subresourceRange{frame_subresources},
    };

    draw_scheduler.EndRendering();
    const auto cmdbuf = draw_scheduler.CommandBuffer();
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &pre_barrier,
    });

    VideoCore::ImageViewInfo view_info{};
    view_info.format = GetFrameViewFormat(attribute.attrib.pixel_format);
    // Exclude alpha from output frame to avoid blending with UI.
    view_info.mapping.a = AmdGpu::CompSwizzle::One;

    vk::ImageView image_view{};
    vk::Extent2D image_size{};
    if (upscaled) {
        const auto device = instance.GetDevice();
        image_view = Check(device.createImageView({
            .image = display.image,
            .viewType = vk::ImageViewType::e2D,
            .format = view_info.format,
            .components = {vk::ComponentSwizzle::eIdentity, vk::ComponentSwizzle::eIdentity,
                           vk::ComponentSwizzle::eIdentity, vk::ComponentSwizzle::eOne},
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        }));
        draw_scheduler.DeferOperation([device, image_view] { device.destroyImageView(image_view); });
        image_size = vk::Extent2D{display.width, display.height};
        const vk::ImageMemoryBarrier2 to_read{
            .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
            .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
            .oldLayout = vk::ImageLayout::eGeneral,
            .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
            .image = display.image,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        };
        cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &to_read});
    } else {
        auto& image = texture_cache.GetImage(image_id);
        if (frame_dump) {
            runtime.Transit(&image, vk::ImageLayout::eTransferSrcOptimal,
                            vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferRead);
            runtime.FlushBarriers();
            DumpRaw(instance, draw_scheduler, cmdbuf, image.GetImage(),
                    vk::ImageLayout::eTransferSrcOptimal, image.info.size.width,
                    image.info.size.height, "display", frame_dump_index);
        }
        image_view = *image.FindView(view_info).image_view;
        image_size = vk::Extent2D{image.info.size.width, image.info.size.height};
        runtime.Transit(&image, vk::ImageLayout::eShaderReadOnlyOptimal,
                        vk::PipelineStageFlagBits2::eFragmentShader,
                        vk::AccessFlagBits2::eShaderRead);
        runtime.FlushBarriers();
    }
    expected_ratio = static_cast<float>(image_size.width) / static_cast<float>(image_size.height);

    image_view = fsr_pass.Render(cmdbuf, image_view, image_size, {frame->width, frame->height},
                                 fsr_settings, frame->is_hdr);

    // Vulkan has no sRGB variant of the 10-bit format, so an A2R10G10B10Srgb buffer reaches
    // the post process pass still sRGB encoded and has to be decoded there instead.
    pp_settings.srgb_input =
        attribute.attrib.pixel_format == Libraries::VideoOut::PixelFormat::A2R10G10B10Srgb;
    pp_pass.Render(cmdbuf, image_view, image_size, *frame, pp_settings);
    if (generation && upscaled && !frame->is_hdr) {
        const vk::ImageMemoryBarrier2 read{
            .srcStageMask=vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask=vk::AccessFlagBits2::eMemoryRead|vk::AccessFlagBits2::eMemoryWrite,
            .dstStageMask=vk::PipelineStageFlagBits2::eTransfer,
            .dstAccessMask=vk::AccessFlagBits2::eTransferRead,
            .oldLayout=vk::ImageLayout::eShaderReadOnlyOptimal,
            .newLayout=vk::ImageLayout::eTransferSrcOptimal,
            .image=display.image,.subresourceRange={vk::ImageAspectFlagBits::eColor,0,1,0,1}};
        cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount=1,.pImageMemoryBarriers=&read});
        PrepareGeneration(frame,display.image,vk::ImageLayout::eTransferSrcOptimal,
            display.width,display.height,view_info.format,display.format);
        auto restore=read;
        restore.srcStageMask=vk::PipelineStageFlagBits2::eTransfer;
        restore.srcAccessMask=vk::AccessFlagBits2::eTransferRead;
        restore.dstStageMask=vk::PipelineStageFlagBits2::eAllCommands;
        restore.dstAccessMask=vk::AccessFlagBits2::eShaderRead;
        restore.oldLayout=vk::ImageLayout::eTransferSrcOptimal;
        restore.newLayout=vk::ImageLayout::eShaderReadOnlyOptimal;
        // Configure can preserve but never submit the current command buffer.
        draw_scheduler.CommandBuffer().pipelineBarrier2({.imageMemoryBarrierCount=1,.pImageMemoryBarriers=&restore});
    }
    if (frame_dump) {
        // The presented image as the swapchain blit reads it (General after the pass).
        const vk::MemoryBarrier2 done{.srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                                      .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
                                      .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
                                      .dstAccessMask = vk::AccessFlagBits2::eTransferRead};
        cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &done});
        DumpRaw(instance, draw_scheduler, cmdbuf, frame->image, vk::ImageLayout::eGeneral,
                frame->width, frame->height, "output", frame_dump_index);
        ++frame_dump_index;
    }

    if (submit_trace) {
        std::printf("reflex submit trace: phase=flush frame=%u accepted=%llu inherited=%llu "
                    "serial=%llu recording=%llu stageB=%d tid=%d\n",
                    frame->id, static_cast<unsigned long long>(content_frame_id),
                    static_cast<unsigned long long>(Reflex::RecordingFrameId()),
                    static_cast<unsigned long long>(frame->generation ? frame->generation->serial
                                                                     : 0),
                    static_cast<unsigned long long>(draw_scheduler.GetFrameId()),
                    DrawPipe::OnStageB() ? 1 : 0, static_cast<int>(gettid()));
    }

    // Flush frame creation commands.
    frame->ready_semaphore = draw_scheduler.GetWorkSemaphore()->Handle();
    frame->ready_tick = draw_scheduler.CurrentTick();
    SubmitInfo info{};
    draw_scheduler.Flush(info);

    // bbport: the GPU command thread runs at most BB_FRAMES_AHEAD (default 1) guest frames
    // ahead of the GPU: it waits here for the frame that many flips back. When the GPU is the
    // bottleneck it finishes frames at an even rate; without this bound the command thread ran
    // ahead and then blocked wherever a resource ran out, so flips (and the guest's frame
    // timing) came in bursts: 12.5/25 ms alternation at 80 FPS. 0 turns it off.
    static const u32 frames_ahead = [] {
        const char* env = std::getenv("BB_FRAMES_AHEAD");
        return env ? u32(std::max(0, std::atoi(env))) : 1u;
    }();
    if (frames_ahead) {
        recent_frame_ticks.push_back(frame->ready_tick);
        while (recent_frame_ticks.size() > frames_ahead) {
            const u64 tick = recent_frame_ticks.front();
            recent_frame_ticks.pop_front();
            if (recent_frame_ticks.size() == frames_ahead) {
                draw_scheduler.Wait(tick);
            }
        }
    }
    return frame;
}

Frame* Presenter::PrepareBlankFrame(bool present_thread, u64 content_frame_id) {
    // Request a free presentation frame.
    Frame* frame = GetRenderFrame();
    if (frame->generation) { frame->generation->count=0; frame->generation->serial=0; }
    // The present thread has no accepted flip and keeps the live identity; the flip path carries
    // the accepted identity explicitly, including zero at startup.
    frame->reflex_frame_id = present_thread ? Reflex::CurrentSimFrameId() : content_frame_id;

    auto& scheduler = present_thread ? present_scheduler : draw_scheduler;
    scheduler.EndRendering();

    const auto cmdbuf = scheduler.CommandBuffer();

    constexpr vk::ImageSubresourceRange simple_subresource = {
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .levelCount = 1,
        .layerCount = 1,
    };
    const auto pre_barrier = vk::ImageMemoryBarrier2{
        .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentRead,
        .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
        .oldLayout = vk::ImageLayout::eUndefined,
        .newLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .image = frame->image,
        .subresourceRange = simple_subresource,
    };

    const auto post_barrier = vk::ImageMemoryBarrier2{
        .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
        .oldLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .newLayout = vk::ImageLayout::eGeneral,
        .image = frame->image,
        .subresourceRange = simple_subresource,
    };

    const vk::RenderingAttachmentInfo attachment = {
        .imageView = frame->image_view,
        .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .loadOp = vk::AttachmentLoadOp::eClear,
        .storeOp = vk::AttachmentStoreOp::eStore,
    };
    const vk::RenderingInfo rendering_info = {
        .renderArea =
            {
                .extent = {frame->width, frame->height},
            },
        .layerCount = 1,
        .colorAttachmentCount = 1u,
        .pColorAttachments = &attachment,
    };

    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &pre_barrier,
    });

    cmdbuf.beginRendering(rendering_info);
    cmdbuf.endRendering();

    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &post_barrier,
    });

    // Flush frame creation commands.
    frame->ready_semaphore = scheduler.GetWorkSemaphore()->Handle();
    frame->ready_tick = scheduler.CurrentTick();
    SubmitInfo info{};
    scheduler.Flush(info);
    return frame;
}

void Presenter::PrepareGeneration(Frame* frame, vk::Image full_color, vk::ImageLayout layout,
                                  u32 sw, u32 sh, vk::Format sample_format, vk::Format source_format) {
    const bool srgb=sample_format == vk::Format::eR8G8B8A8Srgb ||
                    sample_format == vk::Format::eB8G8R8A8Srgb;
    if (source_format != vk::Format::eR8G8B8A8Srgb &&
        source_format != vk::Format::eR8G8B8A8Unorm) {
        generation_history.clear();
        return;
    }
    const auto guides = rasterizer->GetUpscaler().FrameGenerationGuides(frame->guest_address);
    if (!guides.valid || !guides.serial || !guides.ui || guides.output_width != sw || guides.output_height != sh) {
        generation_history.clear();
        return;
    }
    std::scoped_lock provider_lock{generation_mutex};
    if (!generation->Configure(sw,sh,vk::Format::eR8G8B8A8Unorm,generation_factor,generation_backend)) {
        generation_history.clear();
        return;
    }
    if (!frame->generation) frame->generation=std::make_shared<FgFrameResources>();
    auto& resources=*frame->generation;
    resources.count=0;
    resources.serial=guides.serial;
    resources.frame_ms=guides.frame_ms;
    resources.device=instance.GetDevice();
    resources.timestamp_valid=false;
    if (!resources.color.Ensure(instance,sw,sh,vk::Format::eR8G8B8A8Unorm) ||
        !resources.full.Ensure(instance,sw,sh,vk::Format::eR8G8B8A8Unorm) ||
        !resources.ui.Ensure(instance,sw,sh,vk::Format::eR8G8B8A8Unorm) ||
        !resources.depth.Ensure(instance,guides.render_width,guides.render_height,vk::Format::eR32Sfloat) ||
        !resources.motion.Ensure(instance,guides.render_width,guides.render_height,vk::Format::eR16G16Sfloat)) return;
    const u32 count=std::min<u32>(generation_factor-1,generation->MaxGenerated(generation_backend));
    if (!count) return;
    for (u32 i=0;i<count;++i) {
        if (resources.outputs[i].width != sw || resources.outputs[i].height != sh ||
            resources.sample_format != sample_format) {
            if (resources.srgb_views[i]) resources.device.destroyImageView(resources.srgb_views[i]);
            resources.srgb_views[i]=nullptr;
        }
        if (!resources.outputs[i].Ensure(instance,sw,sh,vk::Format::eR8G8B8A8Unorm) ||
            !resources.presented[i].Ensure(instance,frame->width,frame->height,
                                            swapchain.GetSurfaceFormat().format,false)) return;
        if (srgb && !resources.srgb_views[i]) {
            const vk::ImageViewUsageCreateInfo sampled_usage{.usage=vk::ImageUsageFlagBits::eSampled};
            resources.srgb_views[i]=Check(resources.device.createImageView({
                .pNext=&sampled_usage,
                .image=resources.outputs[i].image,.viewType=vk::ImageViewType::e2D,
                // Match the ordinary display view, including its channel order.
                .format=sample_format,
                .subresourceRange={vk::ImageAspectFlagBits::eColor,0,1,0,1}}));
        }
    }
    resources.sample_format=sample_format;
    const auto cmd=draw_scheduler.CommandBuffer();
    const auto families=instance.GetPhysicalDevice().getQueueFamilyProperties();
    if (families[instance.GetGraphicsQueueFamilyIndex()].timestampValidBits) {
        if (!resources.timestamps) resources.timestamps=Check(resources.device.createQueryPool({
            .queryType=vk::QueryType::eTimestamp,.queryCount=2}));
        cmd.resetQueryPool(resources.timestamps,0,2);
        cmd.writeTimestamp2(vk::PipelineStageFlagBits2::eAllCommands,resources.timestamps,0);
        resources.timestamp_valid=true;
    }
    const auto readable=[&](vk::Image image) {
        const vk::ImageMemoryBarrier2 b{.srcStageMask=vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask=vk::AccessFlagBits2::eMemoryRead|vk::AccessFlagBits2::eMemoryWrite,
            .dstStageMask=vk::PipelineStageFlagBits2::eTransfer,.dstAccessMask=vk::AccessFlagBits2::eTransferRead,
            .oldLayout=vk::ImageLayout::eGeneral,.newLayout=vk::ImageLayout::eGeneral,
            .image=image,.subresourceRange={vk::ImageAspectFlagBits::eColor,0,1,0,1}};
        cmd.pipelineBarrier2({.imageMemoryBarrierCount=1,.pImageMemoryBarriers=&b});
    };
    readable(guides.color); readable(guides.depth); readable(guides.motion); readable(guides.ui);
    FgCopy(cmd,guides.color,vk::ImageLayout::eGeneral,sw,sh,resources.color);
    FgCopy(cmd,guides.depth,vk::ImageLayout::eGeneral,guides.render_width,guides.render_height,resources.depth);
    FgCopy(cmd,guides.motion,vk::ImageLayout::eGeneral,guides.render_width,guides.render_height,resources.motion);
    FgCopy(cmd,guides.ui,vk::ImageLayout::eGeneral,sw,sh,resources.ui);
    // Keep encoded RGBA bytes. An sRGB-to-UNORM blit decodes this display
    // buffer while the HUDless reconstruction remains encoded. That mismatch
    // masks the entire scene and replaces interpolated pixels with real ones.
    resources.full.General(cmd, vk::AccessFlagBits2::eTransferWrite);
    const vk::ImageCopy full_copy{
        .srcSubresource={vk::ImageAspectFlagBits::eColor,0,0,1},
        .dstSubresource={vk::ImageAspectFlagBits::eColor,0,0,1},
        .extent={sw,sh,1}};
    cmd.copyImage(full_color,layout,resources.full.image,vk::ImageLayout::eGeneral,full_copy);
    resources.full.General(cmd);
    std::array<FrameGeneration::Output,3> outputs{};
    for (u32 i=0;i<count;++i) {
        resources.outputs[i].General(cmd);
        outputs[i].target=FgResource(resources.outputs[i]);
    }
    FrameGeneration::Input input{.color=FgResource(resources.color),.depth=FgResource(resources.depth),
        .motion=FgResource(resources.motion),.camera=guides.camera,.frame_ms=guides.frame_ms,
        .reset=guides.reset||generation_history.needs_reset(guides.serial),
        .render_frame_index=guides.serial,.reflex_frame_id=u32(frame->reflex_frame_id)};
    resources.count=generation->Record(cmd,input,std::span(outputs.data(),count),count+1);
    generation_history.submitted(guides.serial);
    if (!hud_pipeline && resources.count) {
        const auto device=instance.GetDevice();
        const std::array bindings{
            vk::DescriptorSetLayoutBinding{.binding=0,.descriptorType=vk::DescriptorType::eSampledImage,
                .descriptorCount=1,.stageFlags=vk::ShaderStageFlagBits::eCompute},
            vk::DescriptorSetLayoutBinding{.binding=1,.descriptorType=vk::DescriptorType::eSampledImage,
                .descriptorCount=1,.stageFlags=vk::ShaderStageFlagBits::eCompute},
            vk::DescriptorSetLayoutBinding{.binding=2,.descriptorType=vk::DescriptorType::eStorageImage,
                .descriptorCount=1,.stageFlags=vk::ShaderStageFlagBits::eCompute},
            vk::DescriptorSetLayoutBinding{.binding=3,.descriptorType=vk::DescriptorType::eSampledImage,
                .descriptorCount=1,.stageFlags=vk::ShaderStageFlagBits::eCompute}};
        hud_desc=Check(device.createDescriptorSetLayoutUnique({
            .flags=vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
            .bindingCount=u32(bindings.size()),.pBindings=bindings.data()}));
        hud_layout=Check(device.createPipelineLayoutUnique({.setLayoutCount=1,.pSetLayouts=&*hud_desc}));
        const auto module=CompileSPV(FG_HUD_COMP,device);
        hud_pipeline=Check(device.createComputePipelineUnique({},vk::ComputePipelineCreateInfo{
            .stage={.stage=vk::ShaderStageFlagBits::eCompute,.module=module,.pName="main"},.layout=*hud_layout}));
        device.destroyShaderModule(module);
    }
    for (u32 i=0;i<resources.count;++i) {
        auto& output=resources.outputs[i];
        output.General(cmd);
        const std::array infos{
            vk::DescriptorImageInfo{.imageView=resources.full.view,.imageLayout=vk::ImageLayout::eGeneral},
            vk::DescriptorImageInfo{.imageView=resources.color.view,.imageLayout=vk::ImageLayout::eGeneral},
            vk::DescriptorImageInfo{.imageView=output.view,.imageLayout=vk::ImageLayout::eGeneral},
            vk::DescriptorImageInfo{.imageView=resources.ui.view,.imageLayout=vk::ImageLayout::eGeneral}};
        const std::array writes{
            vk::WriteDescriptorSet{.dstBinding=0,.descriptorCount=1,.descriptorType=vk::DescriptorType::eSampledImage,.pImageInfo=&infos[0]},
            vk::WriteDescriptorSet{.dstBinding=1,.descriptorCount=1,.descriptorType=vk::DescriptorType::eSampledImage,.pImageInfo=&infos[1]},
            vk::WriteDescriptorSet{.dstBinding=2,.descriptorCount=1,.descriptorType=vk::DescriptorType::eStorageImage,.pImageInfo=&infos[2]},
            vk::WriteDescriptorSet{.dstBinding=3,.descriptorCount=1,.descriptorType=vk::DescriptorType::eSampledImage,.pImageInfo=&infos[3]}};
        cmd.bindPipeline(vk::PipelineBindPoint::eCompute,*hud_pipeline);
        cmd.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute,*hud_layout,0,writes);
        cmd.dispatch((sw+7)/8,(sh+7)/8,1);
        // Match ordinary frame post processing and sRGB decoding at the fitted size.
        auto& fitted=resources.presented[i];
        fitted.General(cmd);
        const std::array barriers{
            vk::ImageMemoryBarrier2{.srcStageMask=vk::PipelineStageFlagBits2::eAllCommands,
                .srcAccessMask=vk::AccessFlagBits2::eMemoryRead|vk::AccessFlagBits2::eMemoryWrite,
                .dstStageMask=vk::PipelineStageFlagBits2::eFragmentShader,.dstAccessMask=vk::AccessFlagBits2::eShaderRead,
                .oldLayout=vk::ImageLayout::eGeneral,.newLayout=vk::ImageLayout::eShaderReadOnlyOptimal,
                .image=output.image,.subresourceRange={vk::ImageAspectFlagBits::eColor,0,1,0,1}},
            vk::ImageMemoryBarrier2{.srcStageMask=vk::PipelineStageFlagBits2::eAllCommands,
                .srcAccessMask=vk::AccessFlagBits2::eMemoryRead|vk::AccessFlagBits2::eMemoryWrite,
                .dstStageMask=vk::PipelineStageFlagBits2::eColorAttachmentOutput,.dstAccessMask=vk::AccessFlagBits2::eColorAttachmentWrite,
                .oldLayout=vk::ImageLayout::eGeneral,.newLayout=vk::ImageLayout::eColorAttachmentOptimal,
                .image=fitted.image,.subresourceRange={vk::ImageAspectFlagBits::eColor,0,1,0,1}}};
        cmd.pipelineBarrier2({.imageMemoryBarrierCount=u32(barriers.size()),.pImageMemoryBarriers=barriers.data()});
        Frame target{};
        target.width=frame->width; target.height=frame->height;
        target.image=fitted.image; target.image_view=fitted.view; target.is_hdr=false;
        pp_pass.Render(cmd,srgb ? resources.srgb_views[i] : output.view,{sw,sh},target,pp_settings);
        auto restore=barriers[0];
        restore.srcStageMask=vk::PipelineStageFlagBits2::eFragmentShader;
        restore.srcAccessMask=vk::AccessFlagBits2::eShaderRead;
        restore.dstStageMask=vk::PipelineStageFlagBits2::eAllCommands;
        restore.dstAccessMask=vk::AccessFlagBits2::eMemoryRead|vk::AccessFlagBits2::eMemoryWrite;
        restore.oldLayout=vk::ImageLayout::eShaderReadOnlyOptimal; restore.newLayout=vk::ImageLayout::eGeneral;
        cmd.pipelineBarrier2({.imageMemoryBarrierCount=1,.pImageMemoryBarriers=&restore});
    }
    if (resources.timestamp_valid)
        cmd.writeTimestamp2(vk::PipelineStageFlagBits2::eAllCommands,resources.timestamps,1);
    CaptureFg(instance, draw_scheduler, cmd, resources, guides.camera, frame->reflex_frame_id, input.reset,*frame);
}

void Presenter::CountPresentation(bool real) {
    ++presentation_count;
    if (real) ++source_count; else ++generated_count;
    const auto now=std::chrono::steady_clock::now();
    const float seconds=std::chrono::duration<float>(now-counter_start).count();
    if (seconds>=1.0f) {
        BbSettings::Get().source_fps=float(source_count)/seconds;
        BbSettings::Get().display_fps=float(presentation_count)/seconds;
        if (generation_factor>=2 && std::getenv("BB_FG_TIMING"))
            std::printf("FG pacing: source %.2f presented %.2f generated %llu skipped %llu\n",
                float(source_count)/seconds,float(presentation_count)/seconds,
                (unsigned long long)generated_count,(unsigned long long)skipped_count);
        presentation_count=source_count=generated_count=skipped_count=0;
        counter_start=now;
    }
}

void Presenter::Present(Frame* frame, bool is_reusing_frame, bool is_game_frame) {
    u32 count=0;
    bool grouped_present = false;
    std::int64_t real_deadline = 0;
    const char* gpu_pacing = std::getenv("BB_FSR_GPU_PACING");
    const char* dlss_gpu_pacing = std::getenv("BB_DLSS_GPU_PACING");
    const bool pace_after_copy =
        (generation_backend == 1 && (!gpu_pacing || gpu_pacing[0] != '0')) ||
        (generation_backend == 0 && (!dlss_gpu_pacing || dlss_gpu_pacing[0] != '0'));
    if (generation && frame->generation && frame->generation->serial && !is_reusing_frame) {
        auto& resources=*frame->generation;
        // This completes evaluation even for a failed/reset recording. No host
        // flag reads or provider resource retirement occur before this wait.
        draw_scheduler.Wait(frame->ready_tick);
        // The input gate bounds unfinished source work, not the CPU sleeps used to spread
        // generated images across the display interval. Keep this frame and its SDK lease
        // alive through every presentation, but let the guest prepare its next source once
        // this source's render/evaluation work has completed. Holding the ticket until the
        // real endpoint starves the GPU with a two-source cap under heavier render load.
        // The ordinary endpoint below is an idempotent fallback for non-FG/loading frames.
        if (is_game_frame) {
            Reflex::OnSourceReady(frame->flip_ticket);
        }
        const auto now=FgNow();
        {
            std::scoped_lock lock{generation_mutex};
            generation->Retire(resources.serial);
            if (!generation->IsInterpolationDisabled(resources.serial)) count=resources.count;
        }
        if (count && window.GetWidth() && window.GetHeight()) {
            std::array<u64,2> ticks{};
            u64 source_ns=0,ready_ns=0;
            if (resources.timestamp_valid && instance.GetDevice().getQueryPoolResults(
                resources.timestamps,0,2,sizeof(ticks),ticks.data(),sizeof(u64),
                vk::QueryResultFlagBits::e64) == vk::Result::eSuccess) {
                const double period=instance.GetPhysicalDevice().getProperties().limits.timestampPeriod;
                source_ns=u64(double(ticks[0])*period);
                ready_ns=u64(double(ticks[1])*period);
            }
            float frame_ms=resources.frame_ms;
            if (!source_ns && last_ready_ns && resources.serial>last_ready_serial)
                frame_ms=float(now-last_ready_ns)/float(resources.serial-last_ready_serial)/1e6f;
            int cap = EmulatorSettings.GetFrameLimit();
            if (!cap) {
                if (const char* setting=std::getenv("BB_FPS")) cap=std::atoi(setting);
            }
            generation_schedule.observe(source_ns,ready_ns,resources.serial,now,frame_ms,cap);
            Reflex::OnSourceCadence(cap, generation_schedule.period(),
                last_ready_serial && resources.serial == last_ready_serial + 1 &&
                last_ready_ns && now > last_ready_ns && now - last_ready_ns < 500000000);
            if (std::getenv("BB_FG_TRACE")) {
                std::printf("FG ready: serial=%llu host_ns=%lld source_ns=%llu ready_ns=%llu period_ns=%lld first_ns=%lld real_ns=%lld count=%u\n",
                    (unsigned long long)resources.serial,(long long)now,
                    (unsigned long long)source_ns,(unsigned long long)ready_ns,
                    (long long)generation_schedule.period(),
                    (long long)generation_schedule.deadline(1,count+1),
                    (long long)generation_schedule.deadline(count+1,count+1),count);
            }
            if (std::getenv("BB_FG_TIMING") && resources.serial % 30 == 0) {
                std::printf("FG phase: serial=%llu period_ms=%.3f evaluation_ms=%.3f first_lag_ms=%.3f real_lag_ms=%.3f\n",
                    (unsigned long long)resources.serial,generation_schedule.period()/1e6,
                    ready_ns>=source_ns ? (ready_ns-source_ns)/1e6 : 0.0,
                    (now-generation_schedule.deadline(1,count+1))/1e6,
                    (now-generation_schedule.deadline(count+1,count+1))/1e6);
            }
            last_ready_ns=now; last_ready_serial=resources.serial;
            // One application-rendered frame owns this whole interpolation group. Keep its
            // ordinary present markers around the logical operation; every physical present
            // inside the group (including the real endpoint) uses out-of-band markers. Using
            // an ordinary marker only at the endpoint would reuse an ID already mapped by
            // the generated presents. See VK_NV_low_latency2 Frame Attribution.
            Reflex::OnPresentBegin(frame->reflex_frame_id, true);
            grouped_present = true;
            unsigned position=1;
            while (position<=count) {
                const auto fresh=generation_schedule.next_position(position,count+1,FgNow());
                skipped_count+=fresh-position;
                position=fresh;
                if (position>count) break;
                const auto deadline=generation_schedule.deadline(position,count+1);
                const auto delay=deadline-FgNow();
                if (!pace_after_copy && delay>0) std::this_thread::sleep_for(std::chrono::nanoseconds(delay));
                // One fence owns every use of this leased frame. Before resetting
                // it for another subframe, the previous submission must finish.
                Check(instance.GetDevice().waitForFences(frame->present_done,true,
                    std::numeric_limits<u64>::max()));
                if (!PresentOne(frame,resources.presented[position-1].image,false,
                    pace_after_copy ? deadline : 0, grouped_present)) break;
                ++position;
            }
            real_deadline = generation_schedule.deadline(count+1,count+1);
            const auto delay=real_deadline-FgNow();
            if (!pace_after_copy && delay>0) std::this_thread::sleep_for(std::chrono::nanoseconds(delay));
        }
    }
    // Ordinary frames, loading screens, unsupported/failed providers and expired
    // subframes all preserve this real endpoint and the original game flip.
    if (!count && !is_reusing_frame) Reflex::OnSourceCadence(0, 0, false);
    Check(instance.GetDevice().waitForFences(frame->present_done,true,
        std::numeric_limits<u64>::max()));
    PresentOne(frame,frame->image,!is_reusing_frame && is_game_frame,
        pace_after_copy ? real_deadline : 0, grouped_present);
    if (grouped_present) Reflex::OnPresentEnd(frame->reflex_frame_id, true);
    // bbport: every real source frame is retired once this call returns, including a minimized
    // window or a skipped present, so the bounded backlog always frees its slot. Retire by the
    // flip's accepted ticket (not content_frame_id, which the Reflex/latency path owns).
    if (!is_reusing_frame && is_game_frame) {
        Reflex::OnPresentRetired(frame->flip_ticket);
    }
    if (!is_reusing_frame) {
        last_submit_frame=frame;
        std::scoped_lock lock{free_mutex};
        free_queue.push(frame);
        free_cv.notify_one();
    }
    if (!is_reusing_frame && is_game_frame) DebugState.IncFlipFrameNum();
}

bool Presenter::PresentOne(Frame* frame, vk::Image source_image, bool real, std::int64_t deadline,
                           bool grouped_present) {
    const bool trace = std::getenv("BB_FG_TRACE") != nullptr;
    const auto trace_begin = trace ? FgNow() : 0;
    // bbport: a minimized window has no pixels on Windows (0x0); keep the swapchain and skip
    // presenting until the window is restored.
    if (window.GetWidth() == 0 || window.GetHeight() == 0) {
        return false;
    }

    // Recreate the swapchain if the window was resized.
    if (window.GetWidth() != swapchain.GetWidth() || window.GetHeight() != swapchain.GetHeight()) {
        swapchain.Recreate(window.GetWidth(), window.GetHeight());
    }

    if (!swapchain.AcquireNextImage()) {
        swapchain.Recreate(window.GetWidth(), window.GetHeight());
        if (!swapchain.AcquireNextImage()) {
            // User resizes the window too fast and GPU can't keep up. Skip this frame.
            LOG_WARNING(Render_Vulkan, "Skipping frame!");
            return false;
        }
    }

    // Reset fence for queue submission. Do it here instead of GetRenderFrame() because we may
    // skip frame because of slow swapchain recreation. If a frame skip occurs, we skip signal
    // the frame's present fence and future GetRenderFrame() call will hang waiting for this frame.
    const auto reset_result = instance.GetDevice().resetFences(frame->present_done);
    ASSERT_MSG(reset_result == vk::Result::eSuccess,
               "Unexpected error resetting present done fence: {}", vk::to_string(reset_result));

    // bbport: the game frame is blitted (letterboxed) straight into the swapchain image.
    const vk::Image swapchain_image = swapchain.Image();
    auto& scheduler = present_scheduler;
    Reflex::SetRecordingFrameId(frame->reflex_frame_id);
    scheduler.SetFrameId(frame->reflex_frame_id);
    const auto cmdbuf = scheduler.CommandBuffer();

    if (EmulatorSettings.IsVkHostMarkersEnabled()) {
        cmdbuf.beginDebugUtilsLabelEXT(vk::DebugUtilsLabelEXT{
            .pLabelName = "Present",
        });
    }

    {
        const vk::Extent2D extent = swapchain.GetExtent();
        SetExpectedGameSize(s32(extent.width), s32(extent.height));
        const vk::ImageSubresourceRange color_range{
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = VK_REMAINING_ARRAY_LAYERS,
        };
        const std::array pre_barriers{
            vk::ImageMemoryBarrier{
                .srcAccessMask = vk::AccessFlagBits::eNone,
                .dstAccessMask = vk::AccessFlagBits::eTransferWrite,
                .oldLayout = vk::ImageLayout::eUndefined,
                .newLayout = vk::ImageLayout::eTransferDstOptimal,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = swapchain_image,
                .subresourceRange = color_range,
            },
            vk::ImageMemoryBarrier{
                .srcAccessMask = vk::AccessFlagBits::eColorAttachmentWrite,
                .dstAccessMask = vk::AccessFlagBits::eTransferRead,
                .oldLayout = vk::ImageLayout::eGeneral,
                .newLayout = vk::ImageLayout::eTransferSrcOptimal,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = source_image,
                .subresourceRange = color_range,
            },
        };
        cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
                               vk::PipelineStageFlagBits::eTransfer, vk::DependencyFlagBits::eByRegion,
                               {}, {}, pre_barriers);
        const vk::ClearColorValue black{std::array<float, 4>{0.0f, 0.0f, 0.0f, 1.0f}};
        cmdbuf.clearColorImage(swapchain_image, vk::ImageLayout::eTransferDstOptimal, black, color_range);
        const vk::MemoryBarrier clear_done{
            .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
            .dstAccessMask = vk::AccessFlagBits::eTransferWrite,
        };
        cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eTransfer,
                               vk::DependencyFlagBits::eByRegion, clear_done, {}, {});
        cmdbuf.blitImage(source_image, vk::ImageLayout::eTransferSrcOptimal, swapchain_image,
                         vk::ImageLayout::eTransferDstOptimal,
                         MakeImageBlitFit(frame->width, frame->height, extent.width, extent.height),
                         vk::Filter::eLinear);
        // bbport: the settings menu / FPS counter over the frame, at display resolution.
        const bool overlay = BbOverlay::Visible();
        const std::array post_barriers{
            vk::ImageMemoryBarrier{
                .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
                .dstAccessMask = overlay ? vk::AccessFlagBits::eColorAttachmentRead |
                                               vk::AccessFlagBits::eColorAttachmentWrite
                                         : vk::AccessFlagBits::eNone,
                .oldLayout = vk::ImageLayout::eTransferDstOptimal,
                .newLayout = overlay ? vk::ImageLayout::eColorAttachmentOptimal
                                     : vk::ImageLayout::ePresentSrcKHR,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = swapchain_image,
                .subresourceRange = color_range,
            },
            vk::ImageMemoryBarrier{
                .srcAccessMask = vk::AccessFlagBits::eTransferRead,
                .dstAccessMask = vk::AccessFlagBits::eColorAttachmentWrite,
                .oldLayout = vk::ImageLayout::eTransferSrcOptimal,
                .newLayout = vk::ImageLayout::eGeneral,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = source_image,
                .subresourceRange = color_range,
            },
        };
        cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                               vk::PipelineStageFlagBits::eAllCommands,
                               vk::DependencyFlagBits::eByRegion, {}, {}, post_barriers);
        if (overlay) {
            BbOverlay::Render(cmdbuf, swapchain.ImageView(), extent);
            const vk::ImageMemoryBarrier to_present{
                .srcAccessMask = vk::AccessFlagBits::eColorAttachmentWrite,
                .dstAccessMask = vk::AccessFlagBits::eNone,
                .oldLayout = vk::ImageLayout::eColorAttachmentOptimal,
                .newLayout = vk::ImageLayout::ePresentSrcKHR,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = swapchain_image,
                .subresourceRange = color_range,
            };
            cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eColorAttachmentOutput,
                                   vk::PipelineStageFlagBits::eBottomOfPipe,
                                   vk::DependencyFlagBits::eByRegion, {}, {}, to_present);
        }
    }
    if (EmulatorSettings.IsVkHostMarkersEnabled()) {
        cmdbuf.endDebugUtilsLabelEXT();
    }

    // Flush vulkan commands.

    SubmitInfo info{};
    info.AddWait(swapchain.GetImageAcquiredSemaphore());
    info.AddWait(frame->ready_semaphore, frame->ready_tick);
    info.AddSignal(swapchain.GetPresentReadySemaphore());
    info.AddSignal(frame->present_done);
    scheduler.Flush(info);

    // The composition copy must be complete before its paced present. A CPU
    // deadline alone cannot prevent a copy queued behind the next source frame
    // from reaching the display late and creating a short/long pair.
    if (deadline) {
        Check(instance.GetDevice().waitForFences(frame->present_done,true,
            std::numeric_limits<u64>::max()));
        Common::PreciseSleepUntil(std::chrono::steady_clock::time_point(
            std::chrono::nanoseconds(deadline)));
    }

    // Present to swapchain.
    if (real) Reflex::TraceCpuPresent(frame->content_frame_id);
    Reflex::OnPresentBegin(frame->reflex_frame_id,real && !grouped_present);
    {
        std::scoped_lock submit_lock{Scheduler::submit_mutex};
        if (!swapchain.Present() && window.GetWidth() != 0 && window.GetHeight() != 0) {
            swapchain.Recreate(window.GetWidth(), window.GetHeight());
        }
    }

    Reflex::OnPresentEnd(frame->reflex_frame_id, real && !grouped_present);
    if (trace) {
        std::printf("FG present: serial=%llu id=%llu real=%u begin_ns=%lld end_ns=%lld\n",
            (unsigned long long)(frame->generation ? frame->generation->serial : 0),
            (unsigned long long)frame->reflex_frame_id,unsigned(real),
            (long long)trace_begin,(long long)FgNow());
    }
    if (real && std::getenv("BB_REFLEX_TIMING")) {
        // Driver report queries can disturb presentation under GPU load. CPU
        // input/present records remain per frame; sample driver reports at 1 Hz
        // unless an explicitly detailed diagnostic run requests every frame.
        static const bool full_driver_timing = [] {
            const char* value = std::getenv("BB_REFLEX_DRIVER_TIMING_FULL");
            return value && value[0] == '1';
        }();
        const auto now = FgNow();
        if (full_driver_timing || now - last_reflex_query_ns >= 1000000000ll) {
            last_reflex_query_ns = now;
            Reflex::LatencyReport r{};
            if (Reflex::QueryLatencyTimings(&r) && r.present_id>last_reflex_report) {
                last_reflex_report=r.present_id;
                std::printf("reflex timing: id=%llu input=%llu sim=%llu..%llu render=%llu..%llu present=%llu..%llu gpu=%llu..%llu\n",
                    (unsigned long long)r.present_id,(unsigned long long)r.input_sample_us,
                    (unsigned long long)r.sim_start_us,(unsigned long long)r.sim_end_us,
                    (unsigned long long)r.render_submit_start_us,(unsigned long long)r.render_submit_end_us,
                    (unsigned long long)r.present_start_us,(unsigned long long)r.present_end_us,
                    (unsigned long long)r.gpu_render_start_us,(unsigned long long)r.gpu_render_end_us);
            }
        }
    }
    CountPresentation(real);
    return true;
}

Frame* Presenter::GetRenderFrame() {
    // Wait for free presentation frames
    Frame* frame;
    {
        std::unique_lock lock{free_mutex};
        free_cv.wait(lock, [this] { return !free_queue.empty(); });
        LOG_DEBUG(Render_Vulkan, "Got render frame, remaining {}", free_queue.size() - 1);

        // Take the frame from the queue
        frame = free_queue.front();
        free_queue.pop();
    }

    const vk::Device device = instance.GetDevice();
    vk::Result result{};

    const auto wait = [&]() {
        result = device.waitForFences(frame->present_done, false, std::numeric_limits<u64>::max());
        return result;
    };

    // Wait for the presentation to be finished so all frame resources are free
    while (wait() != vk::Result::eSuccess) {
        ASSERT_MSG(result != vk::Result::eErrorDeviceLost,
                   "Device lost during waiting for a frame");
        // Retry if the waiting times out
        if (result == vk::Result::eTimeout) {
            continue;
        }
    }

    if (frame->width != expected_frame_width || frame->height != expected_frame_height ||
        frame->is_hdr != swapchain.GetHDR()) {
        RecreateFrame(frame, expected_frame_width, expected_frame_height);
    }

    return frame;
}

void Presenter::SetExpectedGameSize(s32 width, s32 height) {
    if (width <= 0 || height <= 0) {
        return; // no surface (minimized window): keep the last frame size
    }
    const float ratio = (float)width / (float)height;

    expected_frame_height = height;
    expected_frame_width = width;
    if (ratio > expected_ratio) {
        expected_frame_width = static_cast<s32>(height * expected_ratio);
    } else {
        expected_frame_height = static_cast<s32>(width / expected_ratio);
    }
}

} // namespace Vulkan
