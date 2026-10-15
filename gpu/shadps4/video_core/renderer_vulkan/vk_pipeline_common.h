// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <functional>

#include "shader_recompiler/profile.h"
#include "shader_recompiler/runtime_info.h"
#include "video_core/renderer_vulkan/vk_common.h"

#include <boost/container/small_vector.hpp>

namespace Shader {
struct Info;
struct PushData;
} // namespace Shader

namespace Vulkan {

static constexpr auto AllGraphicsStageBits =
    vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eTessellationControl |
    vk::ShaderStageFlagBits::eTessellationEvaluation | vk::ShaderStageFlagBits::eGeometry |
    vk::ShaderStageFlagBits::eFragment;

class Instance;
class Scheduler;
class DescriptorHeap;

/// bbport (BB_ASYNC_PIPELINES=1): graphics pipelines first created during play are created on
/// compile threads; everything a pipeline reads from the draw is taken where it is selected.
bool AsyncPipelinesEnabled();
/// Runs `job`, which creates `owner`, on a compile thread, first in, first out.
void SubmitPipelineJob(const void* owner, std::function<void()> job);
/// `owner`'s job when no compile thread has started it yet (the caller runs it), else empty.
std::function<void()> TakePipelineJob(const void* owner);
u32 PipelineCompileThreads();
/// Scene geometry of a pipeline still compiling is left out for at most this long
/// (BB_SKIP_UNCOMPILED=1, BB_SKIP_UNCOMPILED_MS default 1000); 0: never left out.
u64 SkipUncompiledBudgetNs();
/// BB_LATE_PIPELINE_BIND=1: other draws bind the pipeline on a recording thread, which waits.
bool LatePipelineBindEnabled();

class Pipeline {
public:
    Pipeline(const Instance& instance, Scheduler& scheduler, DescriptorHeap& desc_heap,
             const Shader::Profile& profile, vk::PipelineCache pipeline_cache,
             bool is_compute = false);
    virtual ~Pipeline();

    /// bbport: a graphics pipeline may still be created on a compile thread
    /// (BB_ASYNC_PIPELINES): the first caller that needs the handle waits for it. Null when the
    /// driver could not create it.
    vk::Pipeline Handle() const noexcept {
        if (!ready.load(std::memory_order_acquire)) [[unlikely]] {
            WaitReady();
        }
        return *pipeline;
    }

    /// bbport: false while the pipeline is still being created on a compile thread.
    [[nodiscard]] bool IsReady() const noexcept {
        return ready.load(std::memory_order_acquire);
    }

    /// bbport: the handle once the pipeline is created, else null without waiting.
    [[nodiscard]] vk::Pipeline HandleIfReady() const noexcept {
        return ready.load(std::memory_order_acquire) ? *pipeline : vk::Pipeline{};
    }

    /// bbport: when a draw of this pipeline was first left out while it compiled (0: never).
    /// Only the thread recording draws uses it (Rasterizer::SkipUncompiledDraw).
    mutable u64 first_skip_ns = 0;

    /// bbport: false when the driver could not create the pipeline. A pipeline still being
    /// created reads valid; a failed one then has a null Handle() and its draws are skipped.
    bool IsValid() const noexcept {
        return !ready.load(std::memory_order_acquire) || static_cast<bool>(pipeline);
    }

    vk::PipelineLayout GetLayout() const noexcept {
        return *pipeline_layout;
    }

    auto GetStages() const {
        static_assert(static_cast<u32>(Shader::SwStage::Compute) == Shader::MaxStageTypes - 1);
        if (is_compute) {
            return std::span{stages.cend() - 1, stages.cend()};
        } else {
            return std::span{stages.cbegin(), stages.cend() - 1};
        }
    }

    const Shader::Info& GetStage(Shader::SwStage stage) const noexcept {
        return *stages[u32(stage)];
    }

    bool IsCompute() const {
        return is_compute;
    }

    using DescriptorWrites = std::vector<vk::WriteDescriptorSet>;
    void BindResources(DescriptorWrites& set_writes, const Shader::PushData& push_data) const;

protected:
    [[nodiscard]] std::string GetDebugString() const;
    /// bbport: blocks until a compile thread has stored `pipeline`; runs the job itself when no
    /// compile thread has started it (TakePipelineJob).
    void WaitReady() const noexcept;
    /// bbport: the compile thread stores `pipeline`, then sets this (release) and wakes waiters.
    void MarkReady() noexcept;

    /// bbport: cleared by a graphics pipeline whose creation went to a compile thread.
    std::atomic<bool> ready{true};
    const Instance& instance;
    Scheduler& scheduler;
    DescriptorHeap& desc_heap;
    const Shader::Profile& profile;
    vk::UniquePipeline pipeline;
    vk::UniquePipelineLayout pipeline_layout;
    vk::UniqueDescriptorSetLayout desc_layout;
    std::array<const Shader::Info*, Shader::MaxStageTypes> stages{};
    bool uses_push_descriptors{};
    bool is_compute;
};

} // namespace Vulkan
