// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#include "video_core/renderer_vulkan/vk_frame_generation.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_dlss.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <vector>

#include <ffx_vk_fsr3_3_1_5_bridge.h>

namespace Vulkan {

namespace {

uint32_t FindMemoryType(const vk::PhysicalDevice& phys, uint32_t type_bits, vk::MemoryPropertyFlags properties) {
    const auto mp = phys.getMemoryProperties();
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
        if ((type_bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }
    return UINT32_MAX;
}

} // namespace

class FrameGeneration::Impl {
public:
    explicit Impl(const Instance& instance_, Scheduler& scheduler_)
        : instance(instance_), scheduler(scheduler_) {
        device = instance.GetDevice();
        physical_device = instance.GetPhysicalDevice();
        CheckFsrSupport();
    }

    ~Impl() {
        Shutdown();
    }

    void CheckFsrSupport() {
        if (fsr_support_checked) return;
        fsr_support_checked = true;
        if (!instance.IsFsrGenerationSupported()) return;
        const auto [r, extensions] = physical_device.enumerateDeviceExtensionProperties();
        if (r != vk::Result::eSuccess) return;
        const bool has_derivatives = std::ranges::any_of(extensions, [](const auto& ext) {
            return std::strcmp(ext.extensionName.data(), VK_KHR_COMPUTE_SHADER_DERIVATIVES_EXTENSION_NAME) == 0;
        });
        if (has_derivatives) {
            vk::PhysicalDeviceComputeShaderDerivativesFeaturesKHR derivatives{};
            vk::PhysicalDeviceFeatures2 query{};
            query.pNext = &derivatives;
            physical_device.getFeatures2(&query);
            fsr_supported = derivatives.computeDerivativeGroupLinear;
        }
    }

    bool Available(int backend) const {
        if (backend == 0) {
            if (Dlss* dlss = Dlss::Get()) {
                return dlss->FgAvailable();
            }
            return false;
        }
        if (backend == 1) {
            return fsr_supported;
        }
        return false;
    }

    u32 MaxGenerated(int backend) const {
        if (backend == 0) {
            if (Dlss* dlss = Dlss::Get()) {
                return dlss->FgMaxGenerated();
            }
            return 0;
        }
        if (backend == 1) {
            return fsr_supported ? 1u : 0u;
        }
        return 0;
    }

    bool Configure(u32 width_, u32 height_, vk::Format color_format_, int factor_, int backend_) {
        if (configured && width == width_ && height == height_ &&
            color_format == color_format_ && factor == factor_ && backend == backend_) {
            return true;
        }

        // Must drain in-flight GPU work before destroying/recreating contexts.
        scheduler.WaitSubmitted();

        ShutdownContexts();

        width = width_;
        height = height_;
        color_format = color_format_;
        factor = factor_;
        backend = backend_;

        if (!Available(backend)) {
            configured = false;
            return false;
        }

        configured = true;
        rebuild_needed = true;
        context_needs_reset = true;
        return true;
    }

    void ShutdownContexts() {
        if (dlss_feature_created) {
            if (Dlss* dlss = Dlss::Get()) {
                dlss->ReleaseFgFeature();
            }
            dlss_feature_created = false;
        }
        if (fsr_fg_context) {
            ffxVkFsr3_3_1_6FrameGenerationContextDestroy(
                static_cast<FfxVkFsr3_3_1_6FrameGenerationContext*>(fsr_fg_context));
            fsr_fg_context = nullptr;
        }
    }

    void Shutdown() {
        scheduler.WaitSubmitted();
        ShutdownContexts();
        DestroyDisableBuffers();
        configured = false;
        rebuild_needed = false;
        context_needs_reset = false;
        width = height = 0;
    }

    struct DisableSlot {
        vk::Buffer buffer{};
        vk::DeviceMemory memory{};
        void* mapped = nullptr;
        u64 frame_id = 0;
        bool in_flight = false;
    };

    static constexpr size_t kMaxDisableSlots = 16;
    std::array<DisableSlot, kMaxDisableSlots> disable_slots{};
    size_t next_disable_slot = 0;

    void EnsureDisableSlot(size_t index) {
        auto& slot = disable_slots[index];
        if (slot.buffer) return;

        const vk::BufferCreateInfo bci{
            .size = 16,
            .usage = vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eTransferDst,
            .sharingMode = vk::SharingMode::eExclusive,
        };
        const auto [r_buf, buf] = device.createBuffer(bci);
        if (r_buf != vk::Result::eSuccess) return;
        slot.buffer = buf;

        const auto req = device.getBufferMemoryRequirements(slot.buffer);
        const uint32_t mem_type = FindMemoryType(physical_device, req.memoryTypeBits,
            vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
        const vk::MemoryAllocateInfo mai{
            .allocationSize = req.size,
            .memoryTypeIndex = mem_type,
        };
        const auto [r_mem, mem] = device.allocateMemory(mai);
        if (r_mem != vk::Result::eSuccess) return;
        slot.memory = mem;

        if (device.bindBufferMemory(slot.buffer, slot.memory, 0) != vk::Result::eSuccess) return;
        const auto [r_map, ptr] = device.mapMemory(slot.memory, 0, 16);
        if (r_map == vk::Result::eSuccess) {
            slot.mapped = ptr;
            std::memset(slot.mapped, 0, 16);
        }
    }

    void DestroyDisableBuffers() {
        for (auto& slot : disable_slots) {
            if (slot.mapped) device.unmapMemory(slot.memory);
            if (slot.buffer) device.destroyBuffer(slot.buffer);
            if (slot.memory) device.freeMemory(slot.memory);
            slot = {};
        }
    }

    struct RetiredFlag {
        u64 frame_id;
        bool disabled;
    };
    std::deque<RetiredFlag> retired_flags;

    void Retire(u64 completed_frame_id) {
        if (!completed_frame_id) return;

        if (backend == 1 && fsr_fg_context) {
            ffxVkFsr3_3_1_6FrameGenerationContextRetireFrame(
                static_cast<FfxVkFsr3_3_1_6FrameGenerationContext*>(fsr_fg_context),
                completed_frame_id);
        }

        // Cumulative retirement watermark: only retire slots whose work has completed on GPU
        for (auto& slot : disable_slots) {
            if (slot.in_flight && slot.frame_id <= completed_frame_id) {
                bool is_disabled = false;
                if (slot.mapped) {
                    is_disabled = (*static_cast<const uint32_t*>(slot.mapped) != 0);
                }
                retired_flags.push_back({slot.frame_id, is_disabled});
                if (retired_flags.size() > 64) {
                    retired_flags.pop_front();
                }
                slot.in_flight = false;
            }
        }
    }

    bool IsInterpolationDisabled(u64 frame_id) const {
        for (const auto& entry : retired_flags) {
            if (entry.frame_id == frame_id) {
                return entry.disabled;
            }
        }
        return false;
    }

    u32 Record(vk::CommandBuffer cmdbuf, const Input& input, std::span<Output> outputs, u32 requested_factor) {
        if (!configured || outputs.empty()) {
            return 0;
        }

        // Record into provider: on reset or warmup, feeds the inputs/reset to SDK history,
        // records commands, marks retirement serial alive, and returns 0 generated frames.
        if (backend == 0) {
            return RecordDlss(cmdbuf, input, outputs, requested_factor);
        } else if (backend == 1) {
            return RecordFsr(cmdbuf, input, outputs, requested_factor);
        }
        return 0;
    }

private:
    u32 RecordDlss(vk::CommandBuffer cmdbuf, const Input& input, std::span<Output> outputs, u32 requested_factor) {
        Dlss* dlss = Dlss::Get();
        if (!dlss || !dlss->FgAvailable()) return 0;

        if (rebuild_needed || !dlss_feature_created) {
            if (!dlss->CreateFgFeature(cmdbuf, width, height, color_format)) {
                return 0;
            }
            dlss_feature_created = true;
            rebuild_needed = false;
        }

        const u32 max_gen = dlss->FgMaxGenerated();
        const u32 desired_gen = std::clamp(requested_factor, 2u, 4u) - 1u;
        const u32 count = std::min({desired_gen, max_gen, static_cast<u32>(outputs.size())});
        if (count == 0) return 0;

        size_t slot_idx = next_disable_slot;
        EnsureDisableSlot(slot_idx);
        auto& slot = disable_slots[slot_idx];

        // Ensure slot is not currently in-flight on the GPU
        if (slot.in_flight) {
            bool found = false;
            for (size_t i = 0; i < kMaxDisableSlots; ++i) {
                if (!disable_slots[i].in_flight) {
                    slot_idx = i;
                    EnsureDisableSlot(slot_idx);
                    found = true;
                    break;
                }
            }
            if (!found) {
                scheduler.WaitSubmitted();
            }
        }

        next_disable_slot = (slot_idx + 1) % kMaxDisableSlots;
        auto& active_slot = disable_slots[slot_idx];
        active_slot.frame_id = input.render_frame_index;
        active_slot.in_flight = true;
        if (active_slot.mapped) {
            std::memset(active_slot.mapped, 0, 16);
        }

        const auto wrap = [](const Resource& r) {
            const vk::ImageSubresourceRange range{r.aspect, 0, 1, 0, 1};
            return BbDlssImage{
                static_cast<VkImage>(r.image),
                static_cast<VkImageView>(r.view),
                static_cast<VkImageSubresourceRange>(range),
                static_cast<VkFormat>(r.format),
                r.width,
                r.height,
            };
        };

        const bool effective_reset = input.reset || context_needs_reset;
        context_needs_reset = false;

        BbDlssFgEvaluate eval{};
        eval.color = wrap(input.color);
        eval.depth = wrap(input.depth);
        eval.motion = wrap(input.motion);
        eval.camera = input.camera;
        eval.disable_interpolation = static_cast<VkBuffer>(active_slot.buffer);
        eval.reset = effective_reset ? 1 : 0;

        const vk::MemoryBarrier pre_barrier{
            .srcAccessMask = vk::AccessFlagBits::eShaderWrite,
            .dstAccessMask = vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite,
        };

        u32 recorded = 0;
        for (u32 i = 0; i < count; ++i) {
            eval.output = wrap(outputs[i].target);
            if (!dlss->EvaluateFg(cmdbuf, eval, count, i + 1, input.render_frame_index)) {
                context_needs_reset = true;
                return 0;
            }
            recorded++;
            cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
                                   vk::PipelineStageFlagBits::eAllCommands,
                                   {}, 1, &pre_barrier, 0, nullptr, 0, nullptr);
        }

        // On reset or warmup frame (render_frame_index == 0), the SDK was seeded,
        // commands were recorded, but return 0 presentable generated frames.
        if (effective_reset || input.render_frame_index == 0) {
            return 0;
        }

        return recorded;
    }

    u32 RecordFsr(vk::CommandBuffer cmdbuf, const Input& input, std::span<Output> outputs, u32 requested_factor) {
        (void)requested_factor;
        if (!fsr_supported) return 0;

        if (rebuild_needed || !fsr_fg_context) {
            if (fsr_fg_context) {
                ffxVkFsr3_3_1_6FrameGenerationContextDestroy(
                    static_cast<FfxVkFsr3_3_1_6FrameGenerationContext*>(fsr_fg_context));
                fsr_fg_context = nullptr;
            }

            FfxVkFsr3_3_1_6FrameGenerationContext* ctx = nullptr;
            FfxVkFsr3_3_1_6FrameGenerationCreateInfo ci{};
            ci.physicalDevice = static_cast<VkPhysicalDevice>(physical_device);
            ci.device = static_cast<VkDevice>(device);
            // Live reconstruction presets can grow the render size without
            // changing output dimensions. Reserve the full native envelope.
            ci.maxRenderWidth = width;
            ci.maxRenderHeight = height;
            ci.displayWidth = width;
            ci.displayHeight = height;
            ci.colorFormat = static_cast<VkFormat>(color_format);

            if (ffxVkFsr3_3_1_6FrameGenerationContextCreate(&ci, &ctx) != FFX_VK_FSR3_3_1_6_FRAMEGEN_OK || !ctx) {
                return 0;
            }
            fsr_fg_context = ctx;
            rebuild_needed = false;
        }

        auto* ctx = static_cast<FfxVkFsr3_3_1_6FrameGenerationContext*>(fsr_fg_context);

        const auto wrap_fsr = [](const Resource& r) {
            FfxVkFsr3_3_1_6FrameGenerationImage img{};
            img.image = static_cast<VkImage>(r.image);
            img.format = static_cast<VkFormat>(r.format);
            img.width = r.width;
            img.height = r.height;
            img.layout = VK_IMAGE_LAYOUT_GENERAL;
            img.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
            return img;
        };

        const float near_p = input.camera.near_plane > 0.0f ? input.camera.near_plane : 0.1f;
        const float far_p = input.camera.far_plane > near_p ? input.camera.far_plane : 1000.0f;
        const float vfov = input.camera.vertical_fov > 0.0f ? input.camera.vertical_fov : 0.785f;
        const float dt_ms = (std::isfinite(input.frame_ms) && input.frame_ms > 0.0f) ? input.frame_ms : 16.6667f;
        const bool effective_reset = input.reset || context_needs_reset;
        context_needs_reset = false;

        FfxVkFsr3_3_1_6FrameGenerationPrepareInfo prep{};
        prep.commandBuffer = static_cast<VkCommandBuffer>(cmdbuf);
        prep.color = wrap_fsr(input.color);
        prep.depth = wrap_fsr(input.depth);
        prep.motionVectors = wrap_fsr(input.motion);
        prep.renderWidth = input.depth.width ? input.depth.width : width;
        prep.renderHeight = input.depth.height ? input.depth.height : height;
        prep.jitterOffsetX = input.camera.jitter[0];
        prep.jitterOffsetY = input.camera.jitter[1];
        prep.motionVectorScaleX = 1.0f;
        prep.motionVectorScaleY = 1.0f;
        prep.frameTimeMilliseconds = dt_ms;
        prep.minLuminance = 0.0f;
        prep.maxLuminance = 1.0f;
        prep.transferFunction = FFX_VK_FSR3_3_1_6_FRAMEGEN_TRANSFER_SRGB;
        prep.cameraNear = near_p;
        prep.cameraFar = far_p;
        prep.viewSpaceToMeters = 1.0f;
        prep.cameraVerticalFovRadians = vfov;
        std::memcpy(prep.cameraPosition, input.camera.position, sizeof(prep.cameraPosition));
        std::memcpy(prep.cameraUp, input.camera.up, sizeof(prep.cameraUp));
        std::memcpy(prep.cameraRight, input.camera.right, sizeof(prep.cameraRight));
        std::memcpy(prep.cameraForward, input.camera.forward, sizeof(prep.cameraForward));
        prep.frameId = input.render_frame_index;
        prep.reset = effective_reset ? VK_TRUE : VK_FALSE;

        if (ffxVkFsr3_3_1_6FrameGenerationContextRecordPrepare(ctx, &prep) != FFX_VK_FSR3_3_1_6_FRAMEGEN_OK) {
            context_needs_reset = true;
            return 0;
        }

        FfxVkFsr3_3_1_6FrameGenerationDispatchInfo disp{};
        disp.commandBuffer = static_cast<VkCommandBuffer>(cmdbuf);
        disp.color = wrap_fsr(input.color);
        disp.output = wrap_fsr(outputs[0].target);
        disp.distortionField = {};
        disp.displayWidth = width;
        disp.displayHeight = height;
        disp.interpolationX = 0;
        disp.interpolationY = 0;
        disp.interpolationWidth = width;
        disp.interpolationHeight = height;
        disp.frameTimeMilliseconds = dt_ms;
        disp.cameraNear = near_p;
        disp.cameraFar = far_p;
        disp.viewSpaceToMeters = 1.0f;
        disp.cameraVerticalFovRadians = vfov;
        disp.minLuminance = 0.0f;
        disp.maxLuminance = 1.0f;
        disp.transferFunction = FFX_VK_FSR3_3_1_6_FRAMEGEN_TRANSFER_SRGB;
        disp.frameId = input.render_frame_index;
        disp.reset = effective_reset ? VK_TRUE : VK_FALSE;

        if (ffxVkFsr3_3_1_6FrameGenerationContextRecordDispatch(ctx, &disp) != FFX_VK_FSR3_3_1_6_FRAMEGEN_OK) {
            context_needs_reset = true;
            return 0;
        }

        // On reset or warmup frame (render_frame_index == 0), the SDK was seeded,
        // commands were recorded, but return 0 presentable generated frames.
        if (effective_reset || input.render_frame_index == 0) {
            return 0;
        }

        return 1;
    }

    const Instance& instance;
    Scheduler& scheduler;
    vk::Device device{};
    vk::PhysicalDevice physical_device{};

    bool fsr_support_checked = false;
    bool fsr_supported = false;

    bool configured = false;
    bool rebuild_needed = false;
    bool context_needs_reset = false;
    u32 width = 0;
    u32 height = 0;
    vk::Format color_format = vk::Format::eUndefined;
    int factor = 2;
    int backend = 0;

    bool dlss_feature_created = false;
    void* fsr_fg_context = nullptr;
};

FrameGeneration::FrameGeneration(const Instance& instance, Scheduler& scheduler)
    : impl(std::make_unique<Impl>(instance, scheduler)) {}

FrameGeneration::~FrameGeneration() = default;

bool FrameGeneration::Configure(u32 width, u32 height, vk::Format color_format, int factor, int backend) {
    return impl->Configure(width, height, color_format, factor, backend);
}

u32 FrameGeneration::Record(vk::CommandBuffer cmdbuf, const Input& input, std::span<Output> outputs, u32 requested_factor) {
    return impl->Record(cmdbuf, input, outputs, requested_factor);
}

void FrameGeneration::Retire(u64 completed_frame_id) {
    impl->Retire(completed_frame_id);
}

bool FrameGeneration::IsInterpolationDisabled(u64 frame_id) const {
    return impl->IsInterpolationDisabled(frame_id);
}

void FrameGeneration::Shutdown() {
    impl->Shutdown();
}

bool FrameGeneration::Available(int backend) const {
    return impl->Available(backend);
}

u32 FrameGeneration::MaxGenerated(int backend) const {
    return impl->MaxGenerated(backend);
}

} // namespace Vulkan
