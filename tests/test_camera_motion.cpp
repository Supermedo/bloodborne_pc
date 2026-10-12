// SPDX-License-Identifier: GPL-2.0-or-later
// Execute production camera motion SPIR-V: distant geometry, sky and stale object vectors.
#include <array>
#include <cassert>
#include <cmath>
#include <cstdio>
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/image.h"
#include "video_core/host_shaders/camera_motion_comp.h"
#include <vk_mem_alloc.h>
#include "test_assert.h"

int main() {
    Vulkan::Instance instance(0, false);
    static vk::detail::DynamicLoader loader;
    vk::detail::DispatchLoaderDynamic d;
    d.init(loader.getProcAddress<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr"));
    d.init(instance.GetInstance()); d.init(instance.GetDevice());
    const auto device = instance.GetDevice();
    Vulkan::Scheduler scheduler(instance);
    constexpr u32 W=9,H=7,N=W*H;
    std::array<VideoCore::UniqueImage,3> images;
    std::array<vk::UniqueImageView,3> views;
    std::array<vk::DescriptorSetLayoutBinding,3> bindings{};
    for (u32 i=0;i<3;++i) {
        const auto format=i==1 ? vk::Format::eR16G16Sfloat : vk::Format::eR32G32B32A32Sfloat;
        images[i]=VideoCore::UniqueImage(device,instance.GetAllocator());
        images[i].Create({.imageType=vk::ImageType::e2D,.format=format,.extent={W,H,1},
            .mipLevels=1,.arrayLayers=1,.samples=vk::SampleCountFlagBits::e1,
            .tiling=vk::ImageTiling::eOptimal,.usage=vk::ImageUsageFlagBits::eSampled |
            vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eTransferSrc |
            vk::ImageUsageFlagBits::eTransferDst});
        views[i]=device.createImageViewUnique({.image=vk::Image(images[i]),
            .viewType=vk::ImageViewType::e2D,.format=format,
            .subresourceRange={vk::ImageAspectFlagBits::eColor,0,1,0,1}},nullptr,d).value;
        bindings[i]={.binding=i,.descriptorType=i==1 ? vk::DescriptorType::eStorageImage :
            vk::DescriptorType::eSampledImage,.descriptorCount=1,
            .stageFlags=vk::ShaderStageFlagBits::eCompute};
    }
    auto descriptors=device.createDescriptorSetLayoutUnique({
        .flags=vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
        .bindingCount=3,.pBindings=bindings.data()},nullptr,d).value;
    struct Params {
        std::array<float,12> reproject;
        std::array<float,4> proj,prev_proj;
        std::array<float,2> size,jitter,previous_jitter;
        u32 mode;
    };
    const vk::PushConstantRange push{vk::ShaderStageFlagBits::eCompute,0,sizeof(Params)};
    auto layout=device.createPipelineLayoutUnique({.setLayoutCount=1,.pSetLayouts=&*descriptors,
        .pushConstantRangeCount=1,.pPushConstantRanges=&push},nullptr,d).value;
    auto module=device.createShaderModuleUnique({.codeSize=sizeof(CAMERA_MOTION_COMP),
        .pCode=CAMERA_MOTION_COMP},nullptr,d).value;
    auto pipeline=device.createComputePipelineUnique({}, {
        .stage={.stage=vk::ShaderStageFlagBits::eCompute,.module=*module,.pName="main"},
        .layout=*layout},nullptr,d).value;
    VkBuffer staging{}; VmaAllocation allocation{}; VmaAllocationInfo ai{};
    const VkBufferCreateInfo bi{.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size=N*16*2,.usage=VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    const VmaAllocationCreateInfo ac{.flags=VMA_ALLOCATION_CREATE_MAPPED_BIT |
        VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT,.usage=VMA_MEMORY_USAGE_AUTO,
        .requiredFlags=VK_MEMORY_PROPERTY_HOST_COHERENT_BIT};
    assert(vmaCreateBuffer(instance.GetAllocator(),&bi,&ac,&staging,&allocation,&ai)==VK_SUCCESS);
    const auto barrier=[&](vk::CommandBuffer cmd,vk::PipelineStageFlags2 src,
                          vk::AccessFlags2 src_access,vk::PipelineStageFlags2 dst,
                          vk::AccessFlags2 dst_access) {
        const vk::MemoryBarrier2 b{.srcStageMask=src,.srcAccessMask=src_access,
                                  .dstStageMask=dst,.dstAccessMask=dst_access};
        cmd.pipelineBarrier2({.memoryBarrierCount=1,.pMemoryBarriers=&b},d);
    };
    const auto run=[&](const char* label,float depth,float translation,float rotation,
                       float object_depth,u32 mode,float expected,bool jitter=false) {
        auto* pixels=static_cast<float*>(ai.pMappedData);
        for (u32 p=0;p<N;++p) {
            pixels[p*4]=depth;
            pixels[N*4+p*4]=2.f; pixels[N*4+p*4+1]=0.f;
            pixels[N*4+p*4+2]=1.f; pixels[N*4+p*4+3]=object_depth;
        }
        const auto cmd=scheduler.CommandBuffer();
        std::array<vk::ImageMemoryBarrier2,3> transitions{};
        for (u32 i=0;i<3;++i) transitions[i]={
            .srcStageMask=vk::PipelineStageFlagBits2::eAllCommands,
            .dstStageMask=vk::PipelineStageFlagBits2::eAllCommands,
            .dstAccessMask=vk::AccessFlagBits2::eMemoryRead|vk::AccessFlagBits2::eMemoryWrite,
            .oldLayout=vk::ImageLayout::eUndefined,.newLayout=vk::ImageLayout::eGeneral,
            .image=vk::Image(images[i]),.subresourceRange={vk::ImageAspectFlagBits::eColor,0,1,0,1}};
        cmd.pipelineBarrier2({.imageMemoryBarrierCount=3,.pImageMemoryBarriers=transitions.data()},d);
        for (u32 i : {0u,2u}) {
            const vk::BufferImageCopy region{.bufferOffset=i/2*N*16,
                .imageSubresource={vk::ImageAspectFlagBits::eColor,0,0,1},.imageExtent={W,H,1}};
            cmd.copyBufferToImage(staging,vk::Image(images[i]),vk::ImageLayout::eGeneral,region,d);
        }
        barrier(cmd,vk::PipelineStageFlagBits2::eTransfer,vk::AccessFlagBits2::eTransferWrite,
            vk::PipelineStageFlagBits2::eComputeShader,vk::AccessFlagBits2::eShaderRead);
        std::array<vk::DescriptorImageInfo,3> infos{};
        std::array<vk::WriteDescriptorSet,3> writes{};
        for (u32 i=0;i<3;++i) {
            infos[i]={.imageView=*views[i],.imageLayout=vk::ImageLayout::eGeneral};
            writes[i]={.dstBinding=i,.descriptorCount=1,.descriptorType=bindings[i].descriptorType,
                .pImageInfo=&infos[i]};
        }
        Params params{{1,0,rotation,translation,0,1,0,0,0,0,1,0},
            {1,1,1,-0.05f},{1,1,1,-0.05f},{W,H},
            {jitter ? 0.37f : 0.f,jitter ? -0.29f : 0.f},{0,0},mode};
        cmd.bindPipeline(vk::PipelineBindPoint::eCompute,*pipeline,d);
        cmd.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute,*layout,0,writes,d);
        cmd.pushConstants(*layout,vk::ShaderStageFlagBits::eCompute,0,sizeof(params),&params,d);
        cmd.dispatch((W+7)/8,(H+7)/8,1,d);
        barrier(cmd,vk::PipelineStageFlagBits2::eComputeShader,vk::AccessFlagBits2::eShaderWrite,
            vk::PipelineStageFlagBits2::eTransfer,vk::AccessFlagBits2::eTransferRead);
        const vk::BufferImageCopy region{.imageSubresource={vk::ImageAspectFlagBits::eColor,0,0,1},
            .imageOffset={4,3,0},.imageExtent={1,1,1}};
        cmd.copyImageToBuffer(vk::Image(images[1]),vk::ImageLayout::eGeneral,staging,region,d);
        barrier(cmd,vk::PipelineStageFlagBits2::eTransfer,vk::AccessFlagBits2::eTransferWrite,
            vk::PipelineStageFlagBits2::eHost,vk::AccessFlagBits2::eHostRead);
        scheduler.Finish();
        const auto* result=static_cast<u16*>(ai.pMappedData);
        const auto half=[](u16 v) {
            const int e=(v>>10)&31;
            return (v&0x8000 ? -1.f : 1.f)*(e==0 ? std::ldexp(float(v&1023),-24) :
                std::ldexp(1.f+float(v&1023)/1024.f,e-15));
        };
        assert(std::abs(half(result[0])-expected)<0.002f);
        assert(std::abs(half(result[1]))<0.001f);
        std::printf("Camera motion: %s PASS (%.5f)\n",label,half(result[0]));
    };
    constexpr float distant=0.999995f;
    run("distant geometry moves",distant,1000,0,0,0,4.5f*1000*(1-distant)/0.05f);
    run("sky rotates",1,0,0.1f,0,0,0.45f);
    run("sky ignores translation",1,1000,0,0,0,0);
    run("static camera cancels jitter",distant,0,0,0,0,0,true);
    run("matching object vector",distant,0,0,distant,1,2);
    run("stale distant object vector",distant,0,0,0.9999f,1,0);
    vmaDestroyBuffer(instance.GetAllocator(),staging,allocation);
}
