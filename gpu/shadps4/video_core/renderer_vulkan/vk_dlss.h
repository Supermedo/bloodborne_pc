// SPDX-FileCopyrightText: Copyright 2026 IFreemz, bbport contributors
// SPDX-License-Identifier: GPL-2.0-or-later
//
// bbport: NVIDIA DLSS Super Resolution through the optional bridge library (gpu/dlss_bridge,
// MIT), loaded at run time from next to bb-probe together with NVIDIA's DLSS library:
// bbport_dlss.dll + nvngx_dlss.dll on Windows, libbbport_dlss.so + libnvidia-ngx-dlss.so.<version>
// on Linux. The port itself contains no NVIDIA code; without them, on other GPUs or with
// BB_DLSS=0 nothing changes. Adapted from IFreemz/shadPS4-Bloodborne-DLSS-FSR
// (vk_dlss_ngx).

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {

class Dlss {
public:
    /// The process-wide instance, or null when the libraries are absent or BB_DLSS=0.
    static Dlss* Get();
    ~Dlss();

    /// Instance and device creation: adds what NGX needs (or disables DLSS when it is missing).
    void AppendInstanceExtensions(std::vector<const char*>& enabled);
    void AppendDeviceExtensions(vk::Instance instance, vk::PhysicalDevice physical,
                                std::vector<const char*>& enabled);
    /// After the device exists.
    void Initialize(vk::Instance instance, vk::PhysicalDevice physical, vk::Device device);
    void Shutdown();

    [[nodiscard]] bool Available() const;
    /// Why DLSS cannot run on this system (empty when it can, or before device creation).
    [[nodiscard]] std::string Problem() const;

    struct Resource {
        vk::Image image;
        vk::ImageView view;
        vk::Format format;
        vk::ImageAspectFlags aspect;
        u32 width, height;
    };
    struct FeatureDesc {
        u32 input_width, input_height, output_width, output_height;
        int quality;
        bool hdr;
        u32 preset; ///< NVSDK_NGX_DLSS_Hint_Render_Preset, 0 NVIDIA's default (bbport.ini dlss_preset)
        bool operator==(const FeatureDesc&) const = default;
    };
    struct Frame {
        Resource color, depth, motion, output;
        float jitter_x, jitter_y;
        bool reset;
        float frame_ms;
        float sharpness;
    };

    /// DLSS quality mode for an output/render size ratio (0 DLAA .. 4 Ultra Performance).
    static int QualityForScale(float scale);
    [[nodiscard]] bool HasFeature(const FeatureDesc& desc) const;
    /// Records the feature creation. The GPU must be done with the previous one.
    bool CreateFeature(vk::CommandBuffer command, const FeatureDesc& desc);
    bool Evaluate(vk::CommandBuffer command, const Frame& frame);
    void ReleaseFeature();

private:
    Dlss();
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace Vulkan
