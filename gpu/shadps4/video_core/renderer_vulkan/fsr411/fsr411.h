// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: FSR 4.1.1 (AMD's 4.1.1 upscaler DLL) on Vulkan, replaying what the DLL does on D3D12
// (docs/upscaler.md; tools/fsr4cap records it, tools/fsr4cap/extract.py builds the asset sets:
// SPIR-V of every pass and the model weights). Two variants of the model passes, as the DLL has
// them: INT8 dot products (any GPU with VK_VALVE_shader_mixed_float_dot_product) and FP8
// cooperative matrices (RDNA4: VK_EXT_shader_float8), each with its own weights.
//
// Plain Vulkan (no renderer types) so that the game (vk_fsr4.cpp) and the benchmark
// (tools/fsr4_bench.cpp) share it. Frames record into the caller's command buffer; the caller
// keeps the images in layout General and makes sure that the frame recorded kFramesInFlight
// frames earlier has completed before recording a new one (constant buffer ring).

#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include <vulkan/vulkan.h>

namespace Fsr411 {

constexpr uint32_t kFramesInFlight = 8;

struct Image {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE; ///< depth: a view of the depth aspect
    uint32_t width = 0, height = 0;
};

/// What the device was created with, for the variant of the model passes.
struct Features {
    /// VK_KHR_cooperative_matrix and VK_EXT_shader_float8 (shaderFloat8CooperativeMatrix), the
    /// Vulkan memory model, required subgroup size 32 with full subgroups: the FP8 variant.
    bool fp8_matrices = false;
    /// The same with FP16 instead of FP8 matrices: the FP8 variant emulated (fp8emu, testing only:
    /// BB_FSR411_VARIANT=fp8emu).
    bool fp16_matrices = false;
};

struct Frame {
    VkCommandBuffer cmdbuf = VK_NULL_HANDLE;
    Image color, depth, motion, output; ///< color RGBA16F, depth D32, motion RG16F (render pixels)
    uint32_t render_width = 0, render_height = 0;
    bool ultra_performance = false; ///< the second model (quality ratio 3)
    float jitter[2] = {};           ///< render pixels, as for FSR 3/4
    float motion_scale[2] = {1.0f, 1.0f};
    float pre_exposure = 1.0f;
    float sharpness = 0.0f; ///< 0..1
    bool sharpen = false;
    bool reset = false;
    bool auto_exposure = true;
};

class Upscaler {
public:
    /// `dir`: the asset sets (t1080_m0 ... t2160_m1, fp8/..., fp8emu/...). The FP8 variant when
    /// the device has FP8 matrices and its sets are there, else INT8; BB_FSR411_VARIANT=int8,
    /// fp8 or fp8emu picks one (when the device can run it).
    Upscaler(VkPhysicalDevice physical, VkDevice device, std::string dir, Features features = {});
    ~Upscaler();

    /// Records one upscale; false with Error() set when it cannot (assets, device, sizes).
    bool Record(const Frame& frame);
    [[nodiscard]] const std::string& Error() const noexcept;
    /// The variant, asset set and sizes in use, for logs ("FP8 t2160_m0 output 3840x2160").
    [[nodiscard]] std::string Describe() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace Fsr411
