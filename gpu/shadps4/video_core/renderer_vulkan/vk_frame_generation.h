// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include <span>
#include <string>
#include <vector>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "../../../dlss_bridge/bbport_dlss_bridge.h"

namespace Vulkan {

class Instance;
class Scheduler;

class FrameGeneration {
public:
    struct Resource {
        vk::Image image;
        vk::ImageView view;
        vk::Format format;
        vk::ImageAspectFlags aspect{vk::ImageAspectFlagBits::eColor};
        u32 width = 0;
        u32 height = 0;
    };

    struct Input {
        Resource color;  // Output resolution, RGBA8Unorm, HUDless, General layout
        Resource depth;  // Render resolution, R32Sfloat, General layout
        Resource motion; // Render resolution, RG16Sfloat, pixels current->previous, General layout
        BbDlssCamera camera{};
        float frame_ms = 16.6667f;
        bool reset = false;
        u64 render_frame_index = 0;
        u32 reflex_frame_id = 0;
    };

    struct Output {
        Resource target;
    };

    FrameGeneration(const Instance& instance, Scheduler& scheduler);
    ~FrameGeneration();

    /// Rebuilds / configures provider context if dimensions, color format, factor, or backend changed.
    /// Drains GPU before destroying old context.
    bool Configure(u32 width, u32 height, vk::Format color_format, int factor = 2, int backend = 0);

    /// Records generation into outputs. Returns count of generated frames recorded (0 on failure/fallback).
    u32 Record(vk::CommandBuffer cmdbuf, const Input& input, std::span<Output> outputs, u32 requested_factor);

    /// Caller signals GPU completion of a frame ID. Retires transient resources and collects disable flag.
    void Retire(u64 completed_frame_id);

    /// Checks if interpolation was disabled by the GPU for a completed frame.
    [[nodiscard]] bool IsInterpolationDisabled(u64 frame_id) const;

    /// Shuts down provider contexts and frees resources.
    void Shutdown();

    [[nodiscard]] bool Available(int backend) const;
    [[nodiscard]] u32 MaxGenerated(int backend) const;

private:
    class Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace Vulkan