// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <span>
#include <vector>

#include <boost/container/small_vector.hpp>
#include <boost/container/static_vector.hpp>

#include "shader_recompiler/resource.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_pipeline_common.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

namespace Vulkan {

Pipeline::Pipeline(const Instance& instance_, Scheduler& scheduler_, DescriptorHeap& desc_heap_,
                   const Shader::Profile& profile_, vk::PipelineCache pipeline_cache,
                   bool is_compute_ /*= false*/)
    : instance{instance_}, scheduler{scheduler_}, desc_heap{desc_heap_}, profile{profile_},
      is_compute{is_compute_} {}

Pipeline::~Pipeline() = default;


void Pipeline::BindResources(DescriptorWrites& set_writes, const Shader::PushData& push_data,
                             std::span<const vk::DescriptorImageInfo> image_infos,
                             std::span<const vk::DescriptorBufferInfo> buffer_infos) const {
    const auto bind_point =
        IsCompute() ? vk::PipelineBindPoint::eCompute : vk::PipelineBindPoint::eGraphics;
    const auto stage_flags = IsCompute() ? vk::ShaderStageFlagBits::eCompute : AllGraphicsStageBits;
    const vk::PipelineLayout layout = *pipeline_layout;
    scheduler.Record([layout, stage_flags, push_data](vk::CommandBuffer cmdbuf) {
        cmdbuf.pushConstants(layout, stage_flags, 0u, sizeof(push_data), &push_data);
    });

    if (set_writes.empty()) {
        return;
    }

    // dstSet encodes the descriptor set index as (index + 1) so writes from several graphics
    // stages can share one array. Compute still uses set 0 with a null handle.
    auto set_of = [](const vk::WriteDescriptorSet& write) -> u32 {
        const auto handle = static_cast<VkDescriptorSet>(write.dstSet);
        if (!handle) {
            return 0;
        }
        return static_cast<u32>(reinterpret_cast<uintptr_t>(handle)) - 1;
    };

    const auto push_set = [&](u32 set, std::span<const vk::WriteDescriptorSet> writes) {
        if (writes.empty()) {
            return;
        }
        if (!scheduler.IsRecordingDeferred()) {
            scheduler.Record([bind_point, layout, set, writes = std::vector(writes.begin(),
                                                                            writes.end())](
                                 vk::CommandBuffer cmdbuf) mutable {
                for (auto& write : writes) {
                    write.dstSet = VK_NULL_HANDLE;
                }
                cmdbuf.pushDescriptorSetKHR(bind_point, layout, set, writes);
            });
            return;
        }
        // Writes and the infos they point to are laid out in the recording chunk, with the
        // pointers already aimed at those copies: the command only carries a span.
        // bbport: when every write points into the caller's info arrays (draws and
        // dispatches), each array is copied once and the pointers are rebased: a copy per
        // descriptor (two per write) cost the draw recording thread ~3% of its time.
        const auto within = [](const auto* p, u32 count, const auto& array) {
            return p >= array.data() && p + count <= array.data() + array.size();
        };
        bool bulk = !image_infos.empty() || !buffer_infos.empty();
        for (const auto& write : writes) {
            if (!bulk) {
                break;
            }
            bulk = !write.pTexelBufferView &&
                   (!write.pImageInfo || within(write.pImageInfo, write.descriptorCount,
                                                image_infos)) &&
                   (!write.pBufferInfo || within(write.pBufferInfo, write.descriptorCount,
                                                 buffer_infos));
        }
        if (bulk) {
            scheduler.ReserveRecordData(writes.size() * sizeof(vk::WriteDescriptorSet) +
                                        image_infos.size_bytes() + buffer_infos.size_bytes() +
                                        256);
            const auto recorded =
                scheduler.RecordData(std::span<const vk::WriteDescriptorSet>{writes});
            const auto images = scheduler.RecordData(image_infos);
            const auto buffers = scheduler.RecordData(buffer_infos);
            auto* patched = const_cast<vk::WriteDescriptorSet*>(recorded.data());
            for (size_t i = 0; i < recorded.size(); ++i) {
                auto& write = patched[i];
                write.dstSet = VK_NULL_HANDLE;
                if (write.pImageInfo) {
                    write.pImageInfo = images.data() + (write.pImageInfo - image_infos.data());
                }
                if (write.pBufferInfo) {
                    write.pBufferInfo = buffers.data() + (write.pBufferInfo - buffer_infos.data());
                }
            }
            scheduler.Record([bind_point, layout, set, recorded](vk::CommandBuffer cmdbuf) {
                cmdbuf.pushDescriptorSetKHR(bind_point, layout, set, recorded);
            });
            return;
        }
        size_t bytes = writes.size() * sizeof(vk::WriteDescriptorSet) + 64;
        for (const auto& write : writes) {
            bytes += write.descriptorCount *
                         (sizeof(vk::DescriptorBufferInfo) + sizeof(vk::DescriptorImageInfo) +
                          sizeof(vk::BufferView)) +
                     32;
        }
        scheduler.ReserveRecordData(bytes);
        const auto recorded =
            scheduler.RecordData(std::span<const vk::WriteDescriptorSet>{writes});
        auto* patched = const_cast<vk::WriteDescriptorSet*>(recorded.data());
        for (size_t i = 0; i < recorded.size(); ++i) {
            auto& write = patched[i];
            write.dstSet = VK_NULL_HANDLE;
            if (write.pBufferInfo) {
                write.pBufferInfo =
                    scheduler.RecordData(std::span{write.pBufferInfo, write.descriptorCount}).data();
            }
            if (write.pImageInfo) {
                write.pImageInfo =
                    scheduler.RecordData(std::span{write.pImageInfo, write.descriptorCount}).data();
            }
            if (write.pTexelBufferView) {
                write.pTexelBufferView =
                    scheduler
                        .RecordData(std::span{write.pTexelBufferView, write.descriptorCount})
                        .data();
            }
        }
        scheduler.Record([bind_point, layout, set, recorded](vk::CommandBuffer cmdbuf) {
            cmdbuf.pushDescriptorSetKHR(bind_point, layout, set, recorded);
        });
    };

    if (desc_set_count <= 1 && uses_push_descriptors) {
        push_set(0, set_writes);
        return;
    }

    std::array<boost::container::small_vector<vk::WriteDescriptorSet, 32>, MaxDescSets> per_set;
    for (const auto& write : set_writes) {
        const u32 set = std::min(set_of(write), desc_set_count - 1);
        per_set[set].push_back(write);
    }

    for (u32 set = 0; set < desc_set_count; ++set) {
        auto& writes = per_set[set];
        if (writes.empty()) {
            continue;
        }
        if (set_is_push[set] || (desc_set_count == 1 && uses_push_descriptors)) {
            push_set(set, writes);
            continue;
        }
        const vk::DescriptorSetLayout set_layout =
            desc_layout_handles[set] ? desc_layout_handles[set] : *desc_layout;
        const auto desc_set = desc_heap.Commit(set_layout);
        for (auto& write : writes) {
            write.dstSet = desc_set;
        }
        instance.GetDevice().updateDescriptorSets(writes, {});
        scheduler.Record([bind_point, layout, set, desc_set](vk::CommandBuffer cmdbuf) {
            cmdbuf.bindDescriptorSets(bind_point, layout, set, desc_set, {});
        });
    }
}

std::string Pipeline::GetDebugString() const {
    std::string stage_desc;
    for (const auto& stage : stages) {
        if (stage) {
            const auto shader_name = PipelineCache::GetShaderName(stage->hw_stage, stage->pgm_hash);
            if (stage_desc.empty()) {
                stage_desc = shader_name;
            } else {
                stage_desc = fmt::format("{},{}", stage_desc, shader_name);
            }
        }
    }
    return stage_desc;
}

} // namespace Vulkan
