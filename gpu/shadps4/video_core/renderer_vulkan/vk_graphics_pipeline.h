// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <memory>
#include <boost/container/static_vector.hpp>
#include <xxhash.h>

#include "shader_recompiler/frontend/fetch_shader.h"
#include "video_core/amdgpu/regs_color.h"
#include "video_core/amdgpu/regs_depth.h"
#include "video_core/amdgpu/regs_primitive.h"
#include "video_core/renderer_vulkan/vk_pipeline_common.h"

namespace VideoCore {
class BufferCache;
class TextureCache;
} // namespace VideoCore

namespace Vulkan {

static constexpr u32 MaxShaderStages = static_cast<u32>(Shader::SwStage::NumLogicalStages);
static constexpr u32 MaxVertexBufferCount = 32;

class Instance;
class Scheduler;
class DescriptorHeap;

template <typename T>
using VertexInputs = boost::container::static_vector<T, MaxVertexBufferCount>;

struct GraphicsPipelineKey {
    std::array<size_t, MaxShaderStages> stage_hashes;
    std::array<vk::Format, MaxVertexBufferCount> vertex_buffer_formats;
    u32 patch_control_points;
    u32 num_color_attachments;
    std::array<Shader::PsColorBuffer, AmdGpu::NUM_COLOR_BUFFERS> color_buffers;
    std::array<AmdGpu::BlendControl, AmdGpu::NUM_COLOR_BUFFERS> blend_controls;
    std::array<vk::ColorComponentFlags, AmdGpu::NUM_COLOR_BUFFERS> write_masks;
    AmdGpu::ColorBufferMask cb_shader_mask;
    AmdGpu::ColorControl::LogicOp logic_op;
    u8 num_samples;
    u8 depth_samples;
    std::array<u8, AmdGpu::NUM_COLOR_BUFFERS> color_samples;
    // bbport: every byte of the key is a member. The key is hashed, compared and saved as raw
    // bytes, but a C++ copy need not keep padding: the pipeline cache's own copies of a key
    // (background build queue, preload jobs, map entries) lost the 4 bytes of tail padding and
    // took stack bytes there. Such a copy missed its entry, was saved with those bytes, and
    // the next start could leave the real key with an empty entry: that draw skipped for the
    // whole session (Bloodborne's motion blur scene copy: a smeared, frozen picture).
    std::array<u8, 2> unused0;
    u32 mrt_mask;
    u32 motion_vectors = 0;
    union { // the u32 makes all 4 bytes part of every copy, unused bits included
        struct {
            AmdGpu::DepthBuffer::ZFormat z_format : 2;
            AmdGpu::DepthBuffer::StencilFormat stencil_format : 1;
            u32 depth_clamp_enable : 1;
        };
        u32 depth_bits;
    };
    union {
        struct {
            AmdGpu::PrimitiveType prim_type : 5;
            AmdGpu::PolygonMode polygon_mode : 2;
            AmdGpu::ClipSpace clip_space : 1;
            AmdGpu::ProvokingVtxLast provoking_vtx_last : 1;
            u32 depth_clip_enable : 1;
        };
        u32 raster_bits;
    };
    u32 unused1;

    GraphicsPipelineKey() {
        std::memset(this, 0, sizeof(*this));
    }

    bool operator==(const GraphicsPipelineKey& key) const noexcept {
        return std::memcmp(this, &key, sizeof(key)) == 0;
    }

    void Serialize(Serialization::Archive& ar) const;
    bool Deserialize(Serialization::Archive& ar);
};
// No padding anywhere: the members end where the next begins, and the last ends at the end.
static_assert(offsetof(GraphicsPipelineKey, unused0) ==
                  offsetof(GraphicsPipelineKey, color_samples) + AmdGpu::NUM_COLOR_BUFFERS &&
              offsetof(GraphicsPipelineKey, mrt_mask) == offsetof(GraphicsPipelineKey, unused0) + 2 &&
              offsetof(GraphicsPipelineKey, unused1) + sizeof(u32) == sizeof(GraphicsPipelineKey) &&
              sizeof(GraphicsPipelineKey) == 352,
              "GraphicsPipelineKey has padding: hash, compare and save would read it");

class GraphicsPipeline : public Pipeline {
public:
    struct SerializationSupport {
        VertexInputs<vk::VertexInputAttributeDescription> vertex_attributes{};
        VertexInputs<vk::VertexInputBindingDescription> vertex_bindings{};
        VertexInputs<vk::VertexInputBindingDivisorDescriptionEXT> divisors{};
        vk::PipelineMultisampleStateCreateInfo multisampling{};
        std::vector<u32> tcs{};
        std::vector<u32> tes{};
        std::vector<u32> fragment{};

        void Serialize(Serialization::Archive& ar) const;
        bool Deserialize(Serialization::Archive& ar);
    };

    GraphicsPipeline(const Instance& instance, Scheduler& scheduler, DescriptorHeap& desc_heap,
                     const Shader::Profile& profile, const GraphicsPipelineKey& key,
                     vk::PipelineCache pipeline_cache,
                     std::span<const Shader::Info*, MaxShaderStages> stages,
                     std::span<const Shader::RuntimeInfo, MaxShaderStages> runtime_infos,
                     std::optional<const Shader::Gcn::FetchShaderData> fetch_shader,
                     std::span<const vk::ShaderModule> modules, SerializationSupport& sdata,
                     bool preloading, bool deferred = false);
    ~GraphicsPipeline();

    /// bbport: a deferred pipeline is set up on the GPU thread (everything that reads guest
    /// state) and built by the driver later, on any thread (BuildDeferred). Until then it is
    /// not built and not used.
    void BuildDeferred();
    [[nodiscard]] bool IsBuilt() const noexcept {
        return built.load(std::memory_order_acquire);
    }

    const std::optional<const Shader::Gcn::FetchShaderData>& GetFetchShader() const noexcept {
        return fetch_shader;
    }

    const GraphicsPipelineKey& GetGraphicsKey() const {
        return key;
    }

    /// Gets the attributes and bindings for vertex inputs.
    template <typename Attribute, typename Binding>
    void GetVertexInputs(VertexInputs<Attribute>& attributes, VertexInputs<Binding>& bindings,
                         VertexInputs<vk::VertexInputBindingDivisorDescriptionEXT>& divisors,
                         VertexInputs<AmdGpu::Buffer>& guest_buffers, u32 step_rate_0,
                         u32 step_rate_1) const;

private:
    void BuildDescSetLayout(bool preloading);
    /// The driver's part: reads only the key, the stage modules and `sdata`.
    void Build(vk::PipelineCache pipeline_cache, const SerializationSupport& sdata,
               bool clip_discard);

private:
    GraphicsPipelineKey key;
    vk::UniqueDescriptorSetLayout motion_param_layout;
    std::optional<const Shader::Gcn::FetchShaderData> fetch_shader{};
    std::array<vk::ShaderModule, MaxShaderStages> stage_modules{};
    struct DeferredBuild {
        vk::PipelineCache pipeline_cache;
        SerializationSupport sdata;
        bool clip_discard;
    };
    std::unique_ptr<DeferredBuild> deferred_build;
    std::atomic<bool> built{true};
};

struct ClipDistanceShaderKey {
    std::array<std::tuple<u8, u8>, 8> clip_locations;

    bool operator==(const ClipDistanceShaderKey& key) const noexcept {
        return std::memcmp(this, &key, sizeof(key)) == 0;
    }
};

} // namespace Vulkan

template <>
struct std::hash<Vulkan::GraphicsPipelineKey> {
    std::size_t operator()(const Vulkan::GraphicsPipelineKey& key) const noexcept {
        return XXH3_64bits(&key, sizeof(key));
    }
};

template <>
struct std::hash<Vulkan::ClipDistanceShaderKey> {
    std::size_t operator()(const Vulkan::ClipDistanceShaderKey& key) const noexcept {
        return XXH3_64bits(&key, sizeof(key));
    }
};
