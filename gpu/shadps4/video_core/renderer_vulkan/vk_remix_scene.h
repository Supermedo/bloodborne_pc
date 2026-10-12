// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#ifdef _WIN32
#include <memory>
#include "video_core/renderer_vulkan/vk_common.h"
#include "common/types.h"
namespace AmdGpu { union Regs; }
namespace VideoCore { class TextureCache; }
namespace Vulkan {
class Instance; class Scheduler; class Runtime; class GraphicsPipeline;
class RemixScene {
public:
    RemixScene(const Instance&, Scheduler&, Runtime&, VideoCore::TextureCache&);
    ~RemixScene();
    bool Enabled() const;
    void OnFrameStart();
    void OnConstants(const float* data);
    struct CaptureRange { vk::Buffer buffer{}; vk::DeviceSize offset{}, size{}; };
    CaptureRange Capture(const GraphicsPipeline*, const AmdGpu::Regs&, u64 geometry,
                         u32 index_offset);
    // Called by native UI composition BEFORE HUD/menu draws, after guest tonemapping.
    bool Compose(vk::Image target, u32 width, u32 height);
private:
    struct Impl; std::unique_ptr<Impl> impl;
};
} // namespace Vulkan
#endif
