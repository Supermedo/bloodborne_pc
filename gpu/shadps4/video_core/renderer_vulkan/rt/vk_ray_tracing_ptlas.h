#pragma once

#include <span>
#include <string>
#include <vector>

#include "common/types.h"
#include "video_core/renderer_vulkan/rt/rt_ptlas.h"
#include "video_core/renderer_vulkan/vk_common.h"

#include <vk_mem_alloc.h>

namespace Vulkan {

class Instance;

class PartitionedTlas {
public:
    bool Create(const Instance& instance, u32 entries, u32 positions, u32 slots);
    void Destroy(const Instance& instance);

    u64 Address() const {
        return data.address + Rt::PtlasHeaderBytes;
    }
    u32 InstanceCount() const {
        return planner.InstanceCount();
    }
    u64 Bytes() const {
        return data.size + scratch.size + state.size + centers.size + ring.size;
    }

    void Restart();
    void Begin();
    void MarkBuilt(u32 entry);
    Rt::PtlasCenter Center(u32 entry) const;
    void Add(u32 entry, const Rt::Affine& world, const VkAccelerationStructureInstanceKHR& data,
             u32 partition);
    void Plan(u32 slot);
    vk::DescriptorBufferInfo ProbeInfo(u32 slot) const;
    void Record(vk::CommandBuffer cmdbuf, u32 slot, const Rt::Affine& space,
                vk::PipelineStageFlags2 readers);
    std::span<const u32> Indices() const {
        return indices;
    }
    u32 Writes() const {
        return writes;
    }
    u32 Refits() const {
        return refits;
    }
    u32 HiddenTotal() const;

    static void WriteSpace(vk::CommandBuffer cmdbuf, vk::Buffer buffer, const Rt::Affine& space,
                           vk::PipelineStageFlags2 readers);

private:
    struct Buffer {
        vk::Buffer buffer{};
        VmaAllocation allocation{};
        u8* mapped{};
        u64 address{};
        u64 size{};
    };
    bool Allocate(const Instance& instance, Buffer& out, u64 size, VkBufferUsageFlags usage,
                  u32 host_access, u64 alignment);
    vk::PartitionedAccelerationStructureInstancesInputNV Input() const;

    Rt::PtlasPlanner planner;
    Rt::PtlasSlotLayout layout{};
    Buffer data, scratch, state, centers, ring;
    u32 positions_max = 0;
    u32 entries_max = 0;
    std::vector<Rt::PtlasInstance> inputs;
    std::vector<u32> entry_ids;
    std::vector<u8> dirty;
    std::vector<u8> built;
    std::vector<u32> built_list;
    std::vector<u32> indices;
    u32 writes = 0;
    u32 refits = 0;
    bool first = true;
};

}
