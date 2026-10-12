// SPDX-License-Identifier: GPL-2.0-or-later
// Oversized deferred payloads must retain their contents until Vulkan records them.
#include <algorithm>
#include <atomic>
#include <cstring>
#include <span>
#include <vector>
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include <vk_mem_alloc.h>
#include "test_assert.h"

int main() {
    Vulkan::Instance instance(0, false);
    static vk::detail::DynamicLoader loader;
    vk::detail::DispatchLoaderDynamic d;
    d.init(loader.getProcAddress<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr"));
    d.init(instance.GetInstance());
    d.init(instance.GetDevice());
    Vulkan::Scheduler scheduler(instance, true);
    // More regions than can fit in the former fixed 128 KiB recording chunk.
    constexpr size_t Count=Vulkan::RecordChunk::Capacity/sizeof(vk::BufferCopy)+257;
    VkBuffer buffers[2]{};
    VmaAllocation allocations[2]{};
    VmaAllocationInfo maps[2]{};
    for (unsigned i=0;i<2;++i) {
        const VkBufferCreateInfo bi{.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size=Count*sizeof(u32), .usage=i ? VK_BUFFER_USAGE_TRANSFER_DST_BIT : VK_BUFFER_USAGE_TRANSFER_SRC_BIT};
        const VmaAllocationCreateInfo ai{.flags=VMA_ALLOCATION_CREATE_MAPPED_BIT |
            VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT, .usage=VMA_MEMORY_USAGE_AUTO};
        assert(vmaCreateBuffer(instance.GetAllocator(),&bi,&ai,&buffers[i],&allocations[i],&maps[i])==VK_SUCCESS);
    }
    auto* source=static_cast<u32*>(maps[0].pMappedData);
    auto* output=static_cast<u32*>(maps[1].pMappedData);
    std::atomic<unsigned> executed{0};
    struct alignas(64) Marker { u32 value; };
    for (unsigned pass=0;pass<12;++pass) {
        for (size_t i=0;i<Count;++i) source[i]=u32(i)^((pass+1)*0x1234567u);
        assert(vmaFlushAllocation(instance.GetAllocator(),allocations[0],0,VK_WHOLE_SIZE)==VK_SUCCESS);
        std::vector<vk::BufferCopy> regions(Count);
        for (size_t i=0;i<Count;++i)
            regions[i]={.srcOffset=i*sizeof(u32),.dstOffset=i*sizeof(u32),.size=sizeof(u32)};
        // Start near a chunk boundary so a reservation must also preserve earlier commands.
        for (unsigned i=0;i<4000;++i) scheduler.Record([](vk::CommandBuffer) {});
        if (pass%2==0) {
            const auto saved=scheduler.RecordData(std::span<const vk::BufferCopy>{regions});
            scheduler.Record([saved,src=vk::Buffer{buffers[0]},dst=vk::Buffer{buffers[1]},&d,&executed](vk::CommandBuffer cmd) {
                cmd.copyBuffer(src,dst,saved.size(),saved.data(),d);
                executed.fetch_add(1);
            });
        } else {
            // Individually fitting arrays whose combined reservation exceeds the chunk.
            const auto half=Count/2;
            scheduler.ReserveRecordData(regions.size()*sizeof(regions[0])+sizeof(Marker)+256);
            const auto first=scheduler.RecordData(std::span<const vk::BufferCopy>{regions.data(),half});
            const Marker marker{.value=pass};
            const auto aligned=scheduler.RecordData(std::span<const Marker>{&marker,1});
            const auto second=scheduler.RecordData(std::span<const vk::BufferCopy>{regions.data()+half,Count-half});
            assert(reinterpret_cast<uintptr_t>(aligned.data())%alignof(Marker)==0);
            scheduler.Record([first,second,aligned,pass,src=vk::Buffer{buffers[0]},dst=vk::Buffer{buffers[1]},&d,&executed](vk::CommandBuffer cmd) {
                assert(aligned[0].value==pass);
                cmd.copyBuffer(src,dst,first.size(),first.data(),d);
                cmd.copyBuffer(src,dst,second.size(),second.data(),d);
                executed.fetch_add(1);
            });
        }
        // Source descriptors are no longer valid; the recorder must own its copies.
        std::memset(regions.data(),0,regions.size()*sizeof(regions[0]));
        regions.clear(); regions.shrink_to_fit();
        for (unsigned i=0;i<9000;++i) scheduler.Record([](vk::CommandBuffer) {});
        scheduler.Record([&d](vk::CommandBuffer cmd) {
            const vk::MemoryBarrier2 barrier{
                .srcStageMask=vk::PipelineStageFlagBits2::eTransfer,
                .srcAccessMask=vk::AccessFlagBits2::eTransferWrite,
                .dstStageMask=vk::PipelineStageFlagBits2::eHost,
                .dstAccessMask=vk::AccessFlagBits2::eHostRead};
            cmd.pipelineBarrier2({.memoryBarrierCount=1,.pMemoryBarriers=&barrier},d);
        });
        scheduler.Finish();
        assert(vmaInvalidateAllocation(instance.GetAllocator(),allocations[1],0,VK_WHOLE_SIZE)==VK_SUCCESS);
        assert(std::memcmp(source,output,Count*sizeof(u32))==0);
        assert(executed.load()==pass+1);
    }
    for (unsigned i=0;i<2;++i) vmaDestroyBuffer(instance.GetAllocator(),buffers[i],allocations[i]);
    std::puts("PASS: oversized and aggregate record data, alignment, source lifetime, chunk recycling and Vulkan readback");
}
