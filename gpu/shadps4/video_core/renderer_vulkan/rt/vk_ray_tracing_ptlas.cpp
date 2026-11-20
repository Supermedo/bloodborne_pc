#include "video_core/renderer_vulkan/rt/vk_ray_tracing_ptlas.h"

#include <algorithm>
#include <cstring>

#include "video_core/renderer_vulkan/vk_instance.h"

namespace Vulkan {

namespace {

static_assert(sizeof(VkPartitionedAccelerationStructureWriteInstanceDataNV) == Rt::PtlasWriteBytes);
static_assert(sizeof(VkBuildPartitionedAccelerationStructureIndirectCommandNV) ==
              Rt::PtlasCommandBytes);
static_assert(offsetof(VkPartitionedAccelerationStructureWriteInstanceDataNV, accelerationStructure) ==
              24 * 4);
static_assert(offsetof(VkBuildPartitionedAccelerationStructureIndirectCommandNV, argCount) == 4);
static_assert(Rt::PtlasShadowBytes >= Rt::PtlasWriteBytes + 4);
static_assert(Rt::PtlasHostWrite == VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT);
static_assert(Rt::PtlasHostRead == VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT);

void PtlasBarrier(vk::CommandBuffer cmdbuf, vk::PipelineStageFlags2 src_stage,
                  vk::AccessFlags2 src_access, vk::PipelineStageFlags2 dst_stage,
                  vk::AccessFlags2 dst_access) {
    const vk::MemoryBarrier2 barrier{
        .srcStageMask = src_stage,
        .srcAccessMask = src_access,
        .dstStageMask = dst_stage,
        .dstAccessMask = dst_access,
    };
    cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &barrier});
}

}

bool PartitionedTlas::Allocate(const Instance& instance, Buffer& out, u64 size,
                               VkBufferUsageFlags usage, u32 host_access, u64 alignment) {
    const VkBufferCreateInfo buffer_ci{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = size,
        .usage = usage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
    };
    VmaAllocationCreateInfo alloc_ci{
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
    };
    if (host_access) {
        alloc_ci.usage = VMA_MEMORY_USAGE_AUTO;
        alloc_ci.requiredFlags = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        alloc_ci.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT | host_access;
    }
    VkBuffer raw{};
    VmaAllocationInfo info{};
    if (vmaCreateBufferWithAlignment(instance.GetAllocator(), &buffer_ci, &alloc_ci,
                                     std::max<u64>(alignment, 16), &raw, &out.allocation,
                                     &info) != VK_SUCCESS) {
        return false;
    }
    out.buffer = raw;
    out.size = size;
    out.mapped = static_cast<u8*>(info.pMappedData);
    out.address = instance.GetDevice().getBufferAddress({.buffer = out.buffer});
    return out.address != 0 && (!host_access || out.mapped);
}

vk::PartitionedAccelerationStructureInstancesInputNV PartitionedTlas::Input() const {
    return {
        .flags = vk::BuildAccelerationStructureFlagBitsKHR::ePreferFastTrace,
        .instanceCount = planner.InstanceCount(),
        .maxInstancePerPartitionCount = planner.InstanceCount(),
        .partitionCount = Rt::PtlasPartitions,
        .maxInstanceInGlobalPartitionCount = planner.InstanceCount(),
    };
}

bool PartitionedTlas::Create(const Instance& instance, u32 entries, u32 positions, u32 slots) {
    planner.Reset(entries);
    positions_max = positions;
    entries_max = entries;
    layout = Rt::PtlasLayout(planner.InstanceCount(), positions);
    const auto input = Input();
    const auto sizes = instance.GetDevice().getPartitionedAccelerationStructuresBuildSizesNV(input);
    if (!sizes.accelerationStructureSize || !sizes.buildScratchSize) {
        return false;
    }
    if (!Allocate(instance, data, Rt::PtlasHeaderBytes + sizes.accelerationStructureSize,
                  VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
                      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                  0, 256) ||
        !Allocate(instance, scratch, sizes.buildScratchSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                  0, 256) ||
        !Allocate(instance, state, Rt::PtlasStateBytes(planner.InstanceCount()),
                  VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, Rt::PtlasHostAccess(true), 256) ||
        !Allocate(instance, centers, u64(entries) * 16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                  Rt::PtlasHostAccess(true), 16) ||
        !Allocate(instance, ring, layout.bytes * slots,
                  VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                  Rt::PtlasHostAccess(false), 256)) {
        Destroy(instance);
        return false;
    }
    std::memset(state.mapped, 0, state.size);
    std::memset(centers.mapped, 0, centers.size);
    inputs.reserve(positions);
    entry_ids.reserve(positions);
    dirty.reserve(positions);
    built.assign(entries, 0);
    built_list.clear();
    first = true;
    return true;
}

void PartitionedTlas::Destroy(const Instance& instance) {
    for (Buffer* b : {&data, &scratch, &state, &centers, &ring}) {
        if (b->buffer) {
            vmaDestroyBuffer(instance.GetAllocator(), b->buffer, b->allocation);
        }
        *b = {};
    }
    inputs.clear();
    entry_ids.clear();
    dirty.clear();
    built.clear();
    built_list.clear();
    indices.clear();
    first = true;
}

void PartitionedTlas::Restart() {
    planner.Reset(entries_max);
    if (state.mapped) {
        std::memset(state.mapped, 0, state.size);
    }
    if (centers.mapped) {
        std::memset(centers.mapped, 0, centers.size);
    }
    inputs.clear();
    entry_ids.clear();
    dirty.clear();
    built.assign(entries_max, 0);
    built_list.clear();
    indices.clear();
    first = true;
}

void PartitionedTlas::Begin() {
    inputs.clear();
    entry_ids.clear();
    dirty.clear();
    for (const u32 entry : built_list) {
        built[entry] = 0;
    }
    built_list.clear();
}

void PartitionedTlas::MarkBuilt(u32 entry) {
    if (entry < built.size() && !built[entry]) {
        built[entry] = 1;
        built_list.push_back(entry);
    }
}

Rt::PtlasCenter PartitionedTlas::Center(u32 entry) const {
    Rt::PtlasCenter c{};
    if (entry < entries_max && centers.mapped) {
        std::memcpy(c.data(), centers.mapped + u64(entry) * 16, sizeof(c));
    }
    return c;
}

u32 PartitionedTlas::HiddenTotal() const {
    if (!state.mapped) {
        return 0;
    }
    return reinterpret_cast<const volatile u32*>(state.mapped)[0];
}

void PartitionedTlas::Add(u32 entry, const Rt::Affine& world,
                          const VkAccelerationStructureInstanceKHR& instance_data, u32 partition) {
    if (inputs.size() >= positions_max) {
        return;
    }
    dirty.push_back(entry < built.size() ? built[entry] : 0);
    inputs.push_back({
        .transform = world,
        .blas = instance_data.accelerationStructureReference,
        .custom_index = instance_data.instanceCustomIndex,
        .mask = instance_data.mask,
        .flags = Rt::PtlasFlags(instance_data.flags),
        .partition = partition,
    });
    entry_ids.push_back(entry);
}

void PartitionedTlas::Plan(u32 slot) {
    const Rt::PtlasPlan& plan = planner.Plan(inputs, entry_ids, dirty);
    indices = plan.index;
    writes = u32(plan.writes.size());
    refits = plan.refits;
    u8* base = ring.mapped + u64(slot) * layout.bytes;
    const u64 base_address = ring.address + u64(slot) * layout.bytes;
    std::array<u32, Rt::PtlasHeaderWords> header{};
    header[0] = 1;
    header[1] = u32(layout.aux / 4);
    header[2] = u32(layout.writes / 4);
    header[3] = u32(Rt::PtlasStateHeaderBytes / 4);
    header[4] = u32(layout.commands / 4 + 1);
    header[5] = u32(state.address);
    header[6] = u32(state.address >> 32);
    header[7] = u32(inputs.size());
    header[8] = u32(centers.address);
    header[9] = u32(centers.address >> 32);
    std::memcpy(base, header.data(), sizeof(header));
    const VkBuildPartitionedAccelerationStructureIndirectCommandNV command{
        .opType = VK_PARTITIONED_ACCELERATION_STRUCTURE_OP_TYPE_WRITE_INSTANCE_NV,
        .argCount = writes,
        .argData = {.startAddress = base_address + layout.writes,
                    .strideInBytes = Rt::PtlasWriteBytes},
    };
    std::memcpy(base + layout.commands, &command, sizeof(command));
    auto* records =
        reinterpret_cast<VkPartitionedAccelerationStructureWriteInstanceDataNV*>(base + layout.writes);
    for (u32 w = 0; w < writes; ++w) {
        const Rt::PtlasWrite& in = plan.writes[w];
        VkPartitionedAccelerationStructureWriteInstanceDataNV record{};
        std::memcpy(&record.transform.matrix[0][0], in.instance.transform.data(), sizeof(float) * 12);
        record.instanceID = in.instance.custom_index;
        record.instanceMask = in.instance.mask & 0xFFu;
        record.instanceContributionToHitGroupIndex = 0;
        record.instanceFlags = in.instance.flags;
        record.instanceIndex = in.index;
        record.partitionIndex = in.instance.partition;
        record.accelerationStructure = in.instance.blas;
        records[w] = record;
    }
    auto* aux = reinterpret_cast<u32*>(base + layout.aux);
    for (size_t i = 0; i < indices.size(); ++i) {
        const u32 index = indices[i];
        const u64 blas = index == Rt::PtlasNoIndex ? 0 : planner.State(index).blas;
        aux[i * 4 + 0] = u32(blas);
        aux[i * 4 + 1] = u32(blas >> 32);
        aux[i * 4 + 2] = index;
        aux[i * 4 + 3] = plan.write_of[i];
    }
}

vk::DescriptorBufferInfo PartitionedTlas::ProbeInfo(u32 slot) const {
    return {ring.buffer, u64(slot) * layout.bytes, layout.bytes};
}

void PartitionedTlas::WriteSpace(vk::CommandBuffer cmdbuf, vk::Buffer buffer,
                                 const Rt::Affine& space, vk::PipelineStageFlags2 readers) {
    using Stage = vk::PipelineStageFlagBits2;
    using Access = vk::AccessFlagBits2;
    PtlasBarrier(cmdbuf, Stage::eAllCommands, Access::eShaderRead, Stage::eTransfer,
                 Access::eTransferWrite);
    cmdbuf.updateBuffer(buffer, 0, sizeof(float) * 12, space.data());
    PtlasBarrier(cmdbuf, Stage::eTransfer, Access::eTransferWrite, readers, Access::eShaderRead);
}

void PartitionedTlas::Record(vk::CommandBuffer cmdbuf, u32 slot, const Rt::Affine& space,
                            vk::PipelineStageFlags2 readers) {
    using Stage = vk::PipelineStageFlagBits2;
    using Access = vk::AccessFlagBits2;
    WriteSpace(cmdbuf, data.buffer, space, readers);
    PtlasBarrier(cmdbuf, readers | Stage::eAccelerationStructureBuildKHR,
                 Access::eShaderWrite | Access::eShaderRead | Access::eAccelerationStructureReadKHR,
                 Stage::eAccelerationStructureBuildKHR,
                 Access::eIndirectCommandRead | Access::eShaderRead |
                     Access::eAccelerationStructureReadKHR | Access::eAccelerationStructureWriteKHR);
    const u64 slot_address = ring.address + u64(slot) * layout.bytes;
    const vk::BuildPartitionedAccelerationStructureInfoNV info{
        .input = Input(),
        .srcAccelerationStructureData = first ? 0 : Address(),
        .dstAccelerationStructureData = Address(),
        .scratchData = scratch.address,
        .srcInfos = slot_address + layout.commands,
        .srcInfosCount = slot_address,
    };
    cmdbuf.buildPartitionedAccelerationStructuresNV(info);
    first = false;
}

}
