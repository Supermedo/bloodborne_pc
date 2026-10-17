// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <deque>

#include <condition_variable>

#include "core/libraries/videoout/buffer.h"
#include "video_core/renderer_vulkan/host_passes/fsr_pass.h"
#include "video_core/renderer_vulkan/host_passes/pp_pass.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_swapchain.h"
#include "video_core/texture_cache/texture_cache.h"
#include "video_core/renderer_vulkan/vk_frame_generation.h"
#include "video_core/renderer_vulkan/fg_timing.h"

namespace Frontend {
class WindowSDL;
}

namespace AmdGpu {
struct Liverpool;
}

namespace Vulkan {

struct FgFrameResources;

struct Frame {
    u32 width;
    u32 height;
    VmaAllocation allocation;
    vk::Image image;
    vk::ImageView image_view;
    vk::Fence present_done;
    vk::Semaphore ready_semaphore;
    u64 ready_tick;
    bool is_hdr{false};
    u8 id{};
    std::shared_ptr<FgFrameResources> generation;
    u64 reflex_frame_id{};
    u64 content_frame_id{}; ///< Guest flip identity for driver-independent latency diagnostics.
    /// Monotonic accepted-flip ticket for the bounded backlog. Set by the driver for every
    /// flip-prepared frame (including 0), so the presenter retires exactly the flip that owns
    /// this frame; kept separate from content_frame_id, which the Reflex/latency path uses.
    u64 flip_ticket{};
    VAddr guest_address{};

};

enum SchedulerType {
    Draw,
    Present,
    CpuFlip,
};

class Rasterizer;

class Presenter {
public:
    Presenter(Frontend::WindowSDL& window, AmdGpu::Liverpool* liverpool);
    ~Presenter();

    HostPasses::PostProcessingPass::Settings& GetPPSettingsRef() {
        return pp_settings;
    }

    HostPasses::FsrPass::Settings& GetFsrSettingsRef() {
        return fsr_settings;
    }

    Frontend::WindowSDL& GetWindow() const {
        return window;
    }

    Rasterizer& GetRasterizer() const {
        return *rasterizer.get();
    }

    bool IsHDRSupported() const {
        return swapchain.HasHDR();
    }

    void SetHDR(bool enable) {
        if (!IsHDRSupported()) {
            return;
        }
        swapchain.SetHDR(enable);
        pp_settings.hdr = enable ? 1 : 0;
    }

    VideoCore::Image& RegisterVideoOutSurface(
        const Libraries::VideoOut::BufferAttributeGroup& attribute, VAddr cpu_address) {
        vo_buffers_addr.emplace_back(cpu_address);
        auto desc = VideoCore::TextureCache::ImageDesc{attribute, cpu_address};
        const auto image_id = texture_cache.FindImage(desc);
        auto& image = texture_cache.GetImage(image_id);
        image.usage.vo_surface = 1u;
        return image;
    }

    bool IsVideoOutSurface(const AmdGpu::ColorBuffer& color_buffer) const;

    /// bbport: `content_frame_id` is the identity captured when the guest's flip was accepted.
    /// It becomes the frame's Reflex identity instead of the live/thread-local counter.
    Frame* PrepareFrame(const Libraries::VideoOut::BufferAttributeGroup& attribute,
                        VAddr cpu_address, u64 content_frame_id);

    /// The present thread has no accepted flip and keeps the live identity; the flip path passes
    /// the accepted identity explicitly, including zero at startup.
    Frame* PrepareBlankFrame(bool present_thread, u64 content_frame_id = 0);

    void Present(Frame* frame, bool is_reusing_frame = false, bool is_game_frame = true);
    Frame* PrepareLastFrame();

private:
    Frame* GetRenderFrame();

    void RecreateFrame(Frame* frame, u32 width, u32 height);

    void SetExpectedGameSize(s32 width, s32 height);
    void PrepareGeneration(Frame* frame, vk::Image full_color, vk::ImageLayout layout,
                           u32 width, u32 height, vk::Format sample_format, vk::Format source_format);
    bool PresentOne(Frame* frame, vk::Image image, bool real, std::int64_t deadline = 0,
                    bool grouped_present = false);
    void CountPresentation(bool real);

private:
    float expected_ratio{1920.0 / 1080.0f};
    u32 expected_frame_width{1920};
    u32 expected_frame_height{1080};

    Frontend::WindowSDL& window;
    Instance instance;
    HostPasses::FsrPass fsr_pass;
    HostPasses::FsrPass::Settings fsr_settings{};
    HostPasses::PostProcessingPass::Settings pp_settings{};
    HostPasses::PostProcessingPass pp_pass;
    AmdGpu::Liverpool* liverpool;
    Scheduler draw_scheduler;
    std::deque<u64> recent_frame_ticks; ///< bbport: BB_FRAMES_AHEAD bound (PrepareFrame)
    Scheduler present_scheduler;
    Scheduler flip_scheduler;
    Swapchain swapchain;
    Runtime runtime;
    std::unique_ptr<Rasterizer> rasterizer;
    VideoCore::TextureCache& texture_cache;
    vk::UniqueCommandPool command_pool;
    std::vector<Frame> present_frames;
    std::queue<Frame*> free_queue;
    Frame* last_submit_frame;
    std::mutex free_mutex;
    std::condition_variable free_cv;
    std::condition_variable_any frame_cv;
    std::vector<VAddr> vo_buffers_addr;
    std::unique_ptr<FrameGeneration> generation;
    std::mutex generation_mutex;
    FgHistory generation_history;
    FgSchedule generation_schedule{true,true};
    vk::UniqueDescriptorSetLayout hud_desc;
    vk::UniquePipelineLayout hud_layout;
    vk::UniquePipeline hud_pipeline;
    int generation_factor{}, generation_backend{};
    u64 presentation_count{}, source_count{}, generated_count{}, skipped_count{};
    std::int64_t last_ready_ns{};
    u64 last_ready_serial{};
    u64 last_reflex_report{};
    std::int64_t last_reflex_query_ns{};
    std::chrono::steady_clock::time_point counter_start{std::chrono::steady_clock::now()};
};

} // namespace Vulkan
