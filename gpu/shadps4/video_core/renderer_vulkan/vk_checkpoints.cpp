// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include "video_core/renderer_vulkan/vk_checkpoints.h"
#include "video_core/renderer_vulkan/vk_instance.h"

#include <vk_mem_alloc.h>

namespace Vulkan::Checkpoints {

namespace {

enum class Mode { Off, Nvidia, Amd };

/// The descriptions of the last markers, by id (an id's slot is id % RingSize).
constexpr u32 RingSize = 8192;
struct Entry {
    std::atomic<u32> id{0};
    char text[120]{};
};
Entry ring[RingSize];

/// NVIDIA: a checkpoint after the work carries its id with this bit (one before it without).
constexpr u32 AfterBit = 0x80000000u;

Mode mode = Mode::Off;
/// BB_GPU_CHECKPOINTS=2: the markers also every 600 frames, without a device loss (a check that
/// they are written and read).
bool every_600_frames = false;
vk::Queue queue_for_checks;
std::atomic<u32> next_id{0};
std::atomic<u32> frame{0};
VkBuffer marker_buffer{};
VmaAllocation marker_allocation{};
/// AMD: [0] the last marker every command before it started past (top of pipe), [1] the last
/// work that finished (bottom of pipe). Host visible and coherent: readable after the loss.
volatile u32* markers = nullptr;

} // namespace

bool Wanted() {
    const char* value = std::getenv("BB_GPU_CHECKPOINTS");
    return value && (value[0] == '1' || value[0] == '2');
}

void Init(const Instance& instance) {
    if (!Wanted()) {
        return;
    }
    every_600_frames = std::getenv("BB_GPU_CHECKPOINTS")[0] == '2';
    queue_for_checks = instance.GetGraphicsQueue();
    if (instance.HasNvCheckpoints()) {
        mode = Mode::Nvidia;
        std::printf("GPU checkpoints: on (NVIDIA diagnostic checkpoints)\n");
        return;
    }
    if (instance.HasAmdBufferMarker()) {
        const VkBufferCreateInfo buffer_ci{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                           .size = 64,
                                           .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
        const VmaAllocationCreateInfo alloc_ci{
            .flags = VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT,
            .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
            .requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT};
        VmaAllocationInfo info{};
        if (vmaCreateBuffer(instance.GetAllocator(), &buffer_ci, &alloc_ci, &marker_buffer,
                            &marker_allocation, &info) == VK_SUCCESS &&
            info.pMappedData) {
            markers = static_cast<volatile u32*>(info.pMappedData);
            markers[0] = markers[1] = 0;
            mode = Mode::Amd;
            std::printf("GPU checkpoints: on (AMD buffer markers)\n");
            return;
        }
    }
    std::printf("GPU checkpoints: unavailable (the driver has neither "
                "VK_NV_device_diagnostic_checkpoints nor VK_AMD_buffer_marker)\n");
}

u32 Note(const char* what, u64 a, u64 b, u64 c) {
    if (mode == Mode::Off) {
        return 0;
    }
    const u32 id = (next_id.fetch_add(1, std::memory_order_relaxed) + 1) & ~AfterBit;
    Entry& entry = ring[id % RingSize];
    std::snprintf(entry.text, sizeof(entry.text), "frame %u %s %016llx %016llx %llu",
                  frame.load(std::memory_order_relaxed), what, static_cast<unsigned long long>(a),
                  static_cast<unsigned long long>(b), static_cast<unsigned long long>(c));
    entry.id.store(id, std::memory_order_release);
    return id;
}

void Start(vk::CommandBuffer cmdbuf, u32 id) {
    if (id == 0) {
        return;
    }
    if (mode == Mode::Nvidia) {
        cmdbuf.setCheckpointNV(reinterpret_cast<const void*>(static_cast<uintptr_t>(id)));
    } else if (mode == Mode::Amd) {
        VULKAN_HPP_DEFAULT_DISPATCHER.vkCmdWriteBufferMarkerAMD(
            cmdbuf, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, marker_buffer, 0, id);
    }
}

void Finish(vk::CommandBuffer cmdbuf, u32 id) {
    if (id == 0) {
        return;
    }
    if (mode == Mode::Nvidia) {
        cmdbuf.setCheckpointNV(reinterpret_cast<const void*>(static_cast<uintptr_t>(id | AfterBit)));
    } else if (mode == Mode::Amd) {
        VULKAN_HPP_DEFAULT_DISPATCHER.vkCmdWriteBufferMarkerAMD(
            cmdbuf, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, marker_buffer, 4, id);
    }
}

void NextFrame() {
    const u32 now = frame.fetch_add(1, std::memory_order_relaxed) + 1;
    if (every_600_frames && mode != Mode::Off && now % 600 == 0) {
        Dump(queue_for_checks);
    }
}

void Dump(vk::Queue queue) {
    if (mode == Mode::Off) {
        return;
    }
    // started: the last marker the GPU reached the start of; finished: the last work it completed.
    u32 started = 0, finished = 0;
    if (mode == Mode::Nvidia) {
        u32 count = 0;
        VULKAN_HPP_DEFAULT_DISPATCHER.vkGetQueueCheckpointDataNV(queue, &count, nullptr);
        std::vector<VkCheckpointDataNV> data(count, VkCheckpointDataNV{
                                                        .sType = VK_STRUCTURE_TYPE_CHECKPOINT_DATA_NV});
        VULKAN_HPP_DEFAULT_DISPATCHER.vkGetQueueCheckpointDataNV(queue, &count, data.data());
        for (u32 i = 0; i < count; ++i) {
            const auto marker = static_cast<u32>(reinterpret_cast<uintptr_t>(data[i].pCheckpointMarker));
            const u32 id = marker & ~AfterBit;
            const bool after = (marker & AfterBit) != 0;
            std::printf("GPU checkpoints: stage %s reached %s #%u\n",
                        vk::to_string(vk::PipelineStageFlagBits(data[i].stage)).c_str(),
                        after ? "the end of" : "the start of", id);
            if (data[i].stage & VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT) {
                started = std::max(started, id);
            }
            if (data[i].stage & VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT) {
                finished = std::max(finished, after ? id : id - 1);
            }
        }
    } else {
        started = markers[0];
        finished = markers[1];
    }
    const u32 last = next_id.load(std::memory_order_relaxed);
    std::printf("GPU checkpoints: started #%u, finished #%u, %u marked in all (frame %u)\n", started,
                finished, last, frame.load(std::memory_order_relaxed));
    const u32 from = finished > 4 ? finished - 4 : 1;
    const u32 to = std::min(last, std::max(started, finished) + 4);
    for (u32 id = from; id <= to && id - from < 24; ++id) {
        const Entry& entry = ring[id % RingSize];
        if (entry.id.load(std::memory_order_acquire) != id) {
            continue;
        }
        const char* state = id <= finished ? "done   " : id <= started + 1 ? "RUNNING" : "queued ";
        std::printf("GPU checkpoints: %s #%u %s\n", state, id, entry.text);
    }
    std::fflush(stdout);
}

} // namespace Vulkan::Checkpoints
