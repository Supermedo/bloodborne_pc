// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <memory>
#include <vector>
#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {
class Instance;
class Scheduler;

// Optional model processing on the rendering queue. No device/model initialization
// or GPU work is performed while NR is disabled in the startup settings.
class NeuralRendering {
  public:
    struct Color {
        vk::Image image;
        vk::ImageView view;
        vk::Format format;
        u32 width, height;
        bool linear = false;
        bool hdr = false;
    };
    struct Guides {
        vk::ImageView motion{}, depth{};
        u32 width{}, height{};
        bool depth_inverted = true;
    };
    static void AppendInstanceExtensions(std::vector<const char *> &enabled);
    static bool AppendDeviceExtensions(vk::PhysicalDevice physical,
                                       std::vector<const char *> &enabled);
    static void DisableDevice();
    NeuralRendering(const Instance &instance, Scheduler &scheduler);
    ~NeuralRendering();
    void BeginFrame();
    bool Record(vk::CommandBuffer command, int stage, const Color &color, const Guides &guides,
                bool reset = false);
    void CaptureHudless(vk::CommandBuffer command, const Color &color);
    Color ProcessedHudless() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace Vulkan
