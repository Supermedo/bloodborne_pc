// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <deque>
#include <memory>
#include "video_core/renderer_vulkan/vk_dlss.h"
#include "video_core/texture_cache/image.h"
struct FfxVkFsr3_3_1_6FrameGenerationContext;

namespace Vulkan {
class Instance;
class Scheduler;

struct GenerationImage {
    VideoCore::UniqueImage image;
    vk::UniqueImageView view;
    Dlss::Resource resource{};
    void Create(const Instance &instance, u32 w, u32 h, vk::Format format);
};

// Independent copies: the guest is allowed to overwrite its guides after Capture.
struct FrameGenerationInput {
    GenerationImage color, depth, motion;
    BbFrameCamera camera{};
    float frame_ms = 16.6667f;
    bool reset = false;
    u64 capture_tick = 0;
    vk::UniqueDescriptorSetLayout depth_descriptors;
    vk::UniquePipelineLayout depth_layout;
    vk::UniquePipeline depth_pipeline;
};

struct FrameGenerationOutput {
    GenerationImage hudless, ui, generated, converted_color, converted_scene;
    std::shared_ptr<FrameGenerationInput> input;
    vk::UniqueFence present_done;
    VkBuffer disable_buffer{};
    VmaAllocation disable_allocation{};
    VmaAllocator allocator{};
    void *disable_data{};
    bool dispatched = false;
    ~FrameGenerationOutput();
};

class FrameGeneration {
  public:
    FrameGeneration(const Instance &instance, Scheduler &scheduler);
    ~FrameGeneration();
    static std::shared_ptr<FrameGenerationInput>
    Capture(const Instance &instance, Scheduler &scheduler, const Dlss::Resource &color,
            const Dlss::Resource &depth, const Dlss::Resource &motion, const BbFrameCamera &camera,
            float frame_ms, bool reset, std::vector<std::shared_ptr<FrameGenerationInput>> &pool);
    void EnsureOutput(std::shared_ptr<FrameGenerationOutput> &output, u32 w, u32 h,
                      vk::Format format);
    bool Record(vk::CommandBuffer cmd, const Dlss::Resource &color,
                std::shared_ptr<FrameGenerationInput> input,
                std::shared_ptr<FrameGenerationOutput> &output);
    void Invalidate();

  private:
    void Release();
    const Instance &instance;
    Scheduler &scheduler;
    FfxVkFsr3_3_1_6FrameGenerationContext *fsr = nullptr;
    int mode = 0;
    u32 width = 0, height = 0, render_width = 0, render_height = 0;
    vk::Format format{};
    bool history_valid = false;
    bool failed = false;
    u64 frame_id = 0;
    struct Pending {
        u64 tick, id;
    };
    std::deque<Pending> pending;
    vk::UniqueDescriptorSetLayout descriptors;
    vk::UniquePipelineLayout layout;
    vk::UniquePipeline pipeline;
};
} // namespace Vulkan
