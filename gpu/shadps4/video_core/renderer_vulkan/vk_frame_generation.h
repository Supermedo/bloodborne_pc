// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: frame generation (BbSettings::frame_gen, BB_FRAME_GEN), as in the Uncharted port.
// From two finished frames of the game (UI included) it makes the frames in between; the
// Presenter shows them before the game's frame, evenly paced. The depth and motion vectors are
// taken where the temporal upscaler runs, before the game's post-processing (BeginCapture):
// the scene depth and the upscaler's motion vectors (CameraMotion, camera and object motion).
//  - FSR 3.1 frame interpolation (FireBurn/FSR-Vulkan, native Vulkan): any GPU, 2x.
//  - DLSS Frame Generation (bbport_dlss.dll with nvngx_dlssg.dll): GeForce RTX 40 2x,
//    RTX 50 up to 4x.

#pragma once

#include <array>
#include <chrono>
#include <deque>
#include <span>
#include <vector>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/texture_cache/image.h"

struct FfxVkFsr3_3_1_6FrameGenerationContext;

namespace Vulkan {

class Instance;
class Scheduler;

/// The game's camera of a frame: near and far planes, vertical field of view (radians).
struct FrameGenCamera {
    float near_plane = 0.1f;
    float far_plane = 10000.0f;
    float vertical_fov = 1.0f;
    /// Row-vector D3D clip-space matrices (CameraMotion::ClipTransforms): the projection,
    /// current to previous clip, and back. Without them DLSS gets a camera that stood still.
    bool has_matrices = false;
    std::array<float, 16> view_to_clip{};
    std::array<float, 16> clip_to_prev_clip{};
    std::array<float, 16> prev_clip_to_clip{};
};

class FrameGenerator {
public:
    /// Frames generated per game frame at most (DLSS 4x).
    static constexpr u32 MaxGenerated = 3;
    /// The format of the frames read and written (the Presenter's frame images).
    static constexpr vk::Format ColorFormat = vk::Format::eR8G8B8A8Unorm;
    struct Output {
        vk::Image image;
        vk::ImageView view;
    };

    FrameGenerator(const Instance& instance, Scheduler& scheduler);
    ~FrameGenerator();

    /// Switched on in the settings (BbSettings::frame_gen), with a provider that can run.
    [[nodiscard]] bool Wanted() const;
    /// Generated frames per game frame the settings ask for (FSR 3.1: one; DLSS: the
    /// multiplier less one, within what the GPU can).
    [[nodiscard]] u32 WantedCount() const;
    /// A presented frame without generation (HDR output): its captured inputs are dropped and
    /// the next generation starts over.
    void Skip();
    /// Switched off: frees the inputs, the copy without UI and the providers' contexts.
    void Release();

    /// At the upscaler's trigger, render size `w` x `h`: records the copy of the scene depth
    /// (depth aspect view, sampled in General layout) and returns the motion vector image to
    /// write next (RG16F storage, General: pixels, previous minus current), or null when off.
    vk::ImageView BeginCapture(vk::CommandBuffer cmdbuf, vk::ImageView depth, u32 w, u32 h);
    using Camera = FrameGenCamera;
    /// After the motion vectors are written. `motion_valid` false: they are zero this frame.
    void EndCapture(vk::CommandBuffer cmdbuf, std::array<float, 2> jitter, bool motion_valid,
                    Camera camera = {});

    /// The game's picture before its UI (`source`, TransferSrcOptimal layout): copied.
    void CaptureHudless(vk::CommandBuffer cmdbuf, vk::Image source, vk::Format format, u32 w,
                        u32 h);
    /// This frame's copy without UI, sampled (ShaderReadOnlyOptimal) with alpha one, like the
    /// presenter reads the display buffer in `format`: the copy's channel order with the sRGB
    /// decoding of `format` (the game's display pass draws the UI target into the display
    /// buffer). Null when there is none.
    vk::ImageView Hudless(vk::Format format, vk::Extent2D& size);

    /// The frame was composed into `color` (ColorFormat, General layout, `width` x `height`):
    /// records the generated frames into `outputs` (storage and transfer source, oldest first,
    /// left in General layout) and returns how many. Zero when off, and for frames without a
    /// Capture (menus, movies). `hudless`: the same frame composed from Hudless() (General),
    /// or null: FSR 3.1 then interpolates it and draws this frame's UI over the result; DLSS
    /// gets it as its HUD-less input.
    u32 Generate(vk::CommandBuffer cmdbuf, vk::Image color, vk::ImageView color_view,
                 vk::ImageUsageFlags color_usage, vk::Image hudless, vk::ImageView hudless_view,
                 u32 width, u32 height, std::span<const Output> outputs);

    /// Diagnosis: with the presenter's frame dump pending (BB_FRAME_DUMP_TRIGGER), writes
    /// `image` (General layout, 4 bytes per pixel) to BB_DUMP_DIR as fg_<name>_*.raw.
    void DumpIfRequested(vk::CommandBuffer cmdbuf, vk::Image image, u32 w, u32 h,
                         const char* name);

private:
    void EnsureInputs(u32 w, u32 h);
    void CreatePipeline();
    u32 GenerateFsr3(vk::CommandBuffer cmdbuf, vk::Image color, vk::ImageUsageFlags color_usage,
                     u32 width, u32 height, const Output& output, float frame_ms);
    u32 GenerateDlss(vk::CommandBuffer cmdbuf, vk::Image color, vk::ImageView color_view,
                     vk::Image hudless, vk::ImageView hudless_view, u32 width, u32 height,
                     std::span<const Output> outputs, u32 count, float frame_ms);
    /// Where `color` and `hudless` differ (the UI), `color` replaces the generated pixels.
    void ComposeUi(vk::CommandBuffer cmdbuf, vk::ImageView color, vk::ImageView hudless,
                   const Output& output, u32 width, u32 height);
    void DestroyFsr3();
    /// A provider failed for good: DLSS hands over to FSR 3.1, FSR 3.1 switches it off.
    void Fail(int provider, const char* reason);

    const Instance& instance;
    Scheduler& scheduler;

    // Inputs at render size, rewritten every captured frame.
    VideoCore::UniqueImage depth_image;  ///< R32F hardware depth
    VideoCore::UniqueImage motion_image; ///< RG16F pixels, previous - current
    vk::UniqueImageView depth_view, motion_view;
    u32 input_width = 0, input_height = 0;
    vk::UniqueDescriptorSetLayout desc_layout;
    vk::UniquePipelineLayout pipeline_layout;
    vk::UniquePipeline pipeline;
    vk::UniqueDescriptorSetLayout ui_desc_layout;
    vk::UniquePipelineLayout ui_pipeline_layout;
    vk::UniquePipeline ui_pipeline;

    // The picture before the UI, as the game drew it (display buffer format).
    VideoCore::UniqueImage hudless_image;
    vk::Format hudless_format{};
    u32 hudless_width = 0, hudless_height = 0;
    struct HudlessView {
        vk::Format format;
        vk::UniqueImageView view;
    };
    std::vector<HudlessView> hudless_views;
    bool hudless_captured = false; ///< this frame's

    bool captured = false; ///< BeginCapture ran since the last Generate
    std::array<float, 2> capture_jitter{};
    Camera capture_camera{};
    bool reset = true;
    int last_provider = 0;
    bool last_hudless = false;
    u32 hudless_changes = 0; ///< reported the first few times
    bool depth_inverted = false;
    std::chrono::steady_clock::time_point last_generate{};

    // FSR 3.1: the context and the frames it still holds views for until the GPU is done.
    FfxVkFsr3_3_1_6FrameGenerationContext* fsr3 = nullptr;
    u32 fsr3_width = 0, fsr3_height = 0, fsr3_render_width = 0, fsr3_render_height = 0;
    u64 fsr3_frame_id = 0;
    struct Pending {
        u64 frame_id;
        u64 tick;
    };
    std::deque<Pending> fsr3_pending;
};

} // namespace Vulkan
