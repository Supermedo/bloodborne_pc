// SPDX-License-Identifier: GPL-2.0-or-later
// Internal integration test: production VS exporter -> bounded GPU capture ->
// production worker -> official SDK -> shared Vulkan image. No game data needed.
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/ir/ir_emitter.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "remix_client.h"
#include "remix_vulkan.h"
#include <vk_mem_alloc.h>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <numeric>

static void Require(bool ok,const char* message) { if(!ok) throw std::runtime_error(message); }
int wmain(int argc,wchar_t** argv) try {
    using namespace Shader;
    const auto dir=std::filesystem::absolute(argc>1 ? argv[1] : L"remix-test");
    std::filesystem::create_directories(dir);
    Info info{}; info.hw_stage=HwStage::Vertex; info.sw_stage=SwStage::Vertex;
    RuntimeInfo runtime{}; runtime.Initialize(info.hw_stage,info.sw_stage);
    runtime.hw.vs.remix_capture=true; runtime.hw.vs.remix_uv=0;
    Common::ObjectPool<IR::Inst> pool; IR::Block block(pool); IR::IREmitter ir(block);
    info.loads.Set(IR::Attribute::VertexId,0);
    const auto id=ir.GetAttributeU32(IR::Attribute::VertexId);
    const IR::F32 x{ir.Select(ir.IEqual(id,ir.Imm32(1u)),ir.Imm32(1.f),ir.Imm32(-1.f))};
    const IR::F32 y{ir.Select(ir.IEqual(id,ir.Imm32(2u)),ir.Imm32(1.f),ir.Imm32(-1.f))};
    const IR::F32 values[]{x,y,ir.Imm32(2.9f),ir.Imm32(3.f)};
    for(u32 i=0;i<4;++i) {
        info.stores.Set(IR::Attribute::Position0,i); ir.SetAttribute(IR::Attribute::Position0,values[i],i);
        if(i<2) { info.stores.Set(IR::Attribute::Param0,i); ir.SetAttribute(IR::Attribute::Param0,values[i],i); }
    }
    ir.Epilogue(); IR::Program program(info); program.blocks.push_back(&block);
    program.syntax_list.push_back({.data={.block=&block},.type=IR::AbstractSyntaxNode::Type::Block});
    program.syntax_list.push_back({.type=IR::AbstractSyntaxNode::Type::Return});
    Profile profile{}; profile.supported_spirv=0x00010600;
    Backend::Bindings bindings{};
    const auto code=Backend::SPIRV::EmitSPIRV(profile,runtime,program,bindings);
    { std::ofstream file(dir/"remix-capture.spv",std::ios::binary);
      file.write(reinterpret_cast<const char*>(code.data()),code.size()*sizeof(u32)); Require(bool(file),"SPIR-V output"); }
    if(argc>2 && std::wstring_view(argv[2])==L"--shader-only") return 0;
    Vulkan::Instance instance(0,false);
    Require(instance.IsRemixCaptureSupported(),"Adapter lacks bounded transform feedback / external sharing");
    static vk::detail::DynamicLoader loader; vk::detail::DispatchLoaderDynamic d;
    d.init(loader.getProcAddress<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr"));
    d.init(instance.GetInstance()); d.init(instance.GetDevice());
    const auto device=instance.GetDevice(); Vulkan::Scheduler scheduler(instance);
    auto module=device.createShaderModuleUnique({.codeSize=code.size()*4,.pCode=code.data()},nullptr,d).value;
    const vk::PushConstantRange push{vk::ShaderStageFlagBits::eVertex,0,128};
    auto layout=device.createPipelineLayoutUnique({.pushConstantRangeCount=1,.pPushConstantRanges=&push},nullptr,d).value;
    const vk::PipelineShaderStageCreateInfo stage{.stage=vk::ShaderStageFlagBits::eVertex,.module=*module,.pName="main"};
    const vk::PipelineVertexInputStateCreateInfo vertex{};
    const vk::PipelineInputAssemblyStateCreateInfo assembly{.topology=vk::PrimitiveTopology::eTriangleList};
    const vk::Viewport viewport{0,0,8,8,0,1}; const vk::Rect2D scissor{{0,0},{8,8}};
    const vk::PipelineViewportStateCreateInfo vp{.viewportCount=1,.pViewports=&viewport,.scissorCount=1,.pScissors=&scissor};
    const vk::PipelineRasterizationStateCreateInfo raster{.rasterizerDiscardEnable=true,.lineWidth=1};
    const vk::PipelineMultisampleStateCreateInfo ms{.rasterizationSamples=vk::SampleCountFlagBits::e1};
    const vk::PipelineRenderingCreateInfo rendering{};
    auto pipeline=device.createGraphicsPipelineUnique({}, {.pNext=&rendering,.stageCount=1,.pStages=&stage,
        .pVertexInputState=&vertex,.pInputAssemblyState=&assembly,.pViewportState=&vp,
        .pRasterizationState=&raster,.pMultisampleState=&ms,.layout=*layout},nullptr,d).value;
    VkBuffer buffer{}; VmaAllocation allocation{}; VmaAllocationInfo ai{};
    const VkBufferCreateInfo bi{.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=512,
        .usage=VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_BUFFER_BIT_EXT|VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    const VmaAllocationCreateInfo ac{.flags=VMA_ALLOCATION_CREATE_MAPPED_BIT|VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT,
        .usage=VMA_MEMORY_USAGE_AUTO,.requiredFlags=VK_MEMORY_PROPERTY_HOST_COHERENT_BIT};
    Require(vmaCreateBuffer(instance.GetAllocator(),&bi,&ac,&buffer,&allocation,&ai)==VK_SUCCESS,"TF allocation");
    auto* bytes=static_cast<uint8_t*>(ai.pMappedData);
    auto capture=[&](vk::DeviceSize size,u32 count) {
        std::memset(bytes,0xab,512);
        const auto cmd=scheduler.CommandBuffer();
        const vk::MemoryBarrier2 before{.srcStageMask=vk::PipelineStageFlagBits2::eHost,.srcAccessMask=vk::AccessFlagBits2::eHostWrite,
            .dstStageMask=vk::PipelineStageFlagBits2::eTransformFeedbackEXT,.dstAccessMask=vk::AccessFlagBits2::eTransformFeedbackWriteEXT};
        cmd.pipelineBarrier2({.memoryBarrierCount=1,.pMemoryBarriers=&before},d);
        cmd.beginRendering({.renderArea={{0,0},{8,8}},.layerCount=1},d);
        cmd.bindPipeline(vk::PipelineBindPoint::eGraphics,*pipeline,d);
        const vk::Buffer bound(buffer); const vk::DeviceSize offset=64;
        cmd.bindTransformFeedbackBuffersEXT(0,1,&bound,&offset,&size,d);
        cmd.beginTransformFeedbackEXT(0,0,nullptr,nullptr,d); cmd.draw(count,1,0,0,d);
        cmd.endTransformFeedbackEXT(0,0,nullptr,nullptr,d); cmd.endRendering(d);
        const vk::MemoryBarrier2 after{.srcStageMask=vk::PipelineStageFlagBits2::eTransformFeedbackEXT,.srcAccessMask=vk::AccessFlagBits2::eTransformFeedbackWriteEXT,
            .dstStageMask=vk::PipelineStageFlagBits2::eHost,.dstAccessMask=vk::AccessFlagBits2::eHostRead};
        cmd.pipelineBarrier2({.memoryBarrierCount=1,.pMemoryBarriers=&after},d); scheduler.Finish();
        for(size_t i=0;i<64;++i) Require(bytes[i]==0xab,"Capture escaped prefix");
        for(size_t i=64+size;i<512;++i) Require(bytes[i]==0xab,"Capture escaped suffix");
    };
    capture(32,3);
    for(size_t i=0;i<512;++i) Require(bytes[i]==0xab,"Partial primitive must not be captured");
    capture(96,300); // Multiple complete primitives cannot escape the draw's range.
    BbRemix::SceneCamera camera{{1,0,0,0,0,1,0,0,0,0,1,0},{1,1,1,-0.1f}};
    const u32 indices[]{0,1,2};
    auto mesh=BbRemix::BuildMesh({reinterpret_cast<BbRemix::CapturedVertex*>(bytes+64),3},indices,BbRemix::Topology::Triangles,camera);
    Require(mesh.vertices.size()==3 && mesh.indices.size()==3,"Production vertex export did not reconstruct a triangle");
    Require(mesh.vertices[0].position[0]==-1 && mesh.vertices[0].position[2]==3 && mesh.vertices[1].uv[0]==1,"Exported position / UV");
    vmaDestroyBuffer(instance.GetAllocator(),buffer,allocation);
    std::puts("PASS: production SPIR-V, complete primitive capture, undersized range and prefix/suffix guards");
    if(argc<4) return 0;
    BbRemix::Client client;
    const auto props=instance.GetPhysicalDevice().getProperties2<vk::PhysicalDeviceProperties2,vk::PhysicalDeviceIDProperties>(d);
    const auto& ids=props.get<vk::PhysicalDeviceIDProperties>();
    Require(ids.deviceLUIDValid,"No adapter LUID");
    BbRemix::Wire::Initialize init{320,180}; std::copy_n(ids.deviceLUID.data(),8,init.luid.data());
    Require(client.Start(std::filesystem::absolute(argv[2]),std::filesystem::absolute(argv[3]),dir/"worker.log",init),client.Error().c_str());
    const auto send=[&](u32 w,u32 h,bool update,bool bad=false) {
        static uint64_t number=0;
        BbRemix::Wire::Writer wire; wire.Put(BbRemix::Wire::Frame{++number,w,h,update?1u:0u,1,number==1,camera});
        if(update) {
            wire.Put(BbRemix::Wire::Mesh{1,number,1,u32(mesh.vertices.size()),u32(mesh.indices.size())});
            wire.Bytes(mesh.vertices.data(),mesh.vertices.size()*sizeof(BbRemix::Vertex));
            auto copy=mesh.indices; if(bad) copy[0]=999;
            wire.Bytes(copy.data(),copy.size()*4);
        }
        wire.Put(uint64_t(1)); return client.Render(wire);
    };
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(180);
    bool first=true;
    while(!client.Ready() && std::chrono::steady_clock::now()<deadline) {
        Require(send(320,180,first),client.Error().c_str()); first=false;
    }
    Require(client.Ready(),"SDK warmup never produced an image");
    BbRemix::VulkanOutput output(instance.GetPhysicalDevice(),device);
    const auto inspect=[&](u32 w,u32 h) {
        Require(output.Import(client.Output()),output.Error().c_str());
        VkBuffer pixels{}; VmaAllocation memory{}; VmaAllocationInfo mapped{};
        auto pb=bi; pb.size=uint64_t(w)*h*8; pb.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        Require(vmaCreateBuffer(instance.GetAllocator(),&pb,&ac,&pixels,&memory,&mapped)==VK_SUCCESS,"Shared readback allocation");
        const auto cmd=scheduler.CommandBuffer(); const auto family=instance.GetGraphicsQueueFamilyIndex();
        output.Acquire(cmd,family);
        cmd.copyImageToBuffer(output.Image(),vk::ImageLayout::eGeneral,pixels,
            vk::BufferImageCopy{.imageSubresource={vk::ImageAspectFlagBits::eColor,0,0,1},.imageExtent={w,h,1}},d);
        output.Release(cmd,family);
        const vk::MemoryBarrier2 host{.srcStageMask=vk::PipelineStageFlagBits2::eTransfer,.srcAccessMask=vk::AccessFlagBits2::eTransferWrite,
            .dstStageMask=vk::PipelineStageFlagBits2::eHost,.dstAccessMask=vk::AccessFlagBits2::eHostRead};
        cmd.pipelineBarrier2({.memoryBarrierCount=1,.pMemoryBarriers=&host},d); scheduler.Finish();
        const auto* p=static_cast<uint16_t*>(mapped.pMappedData); size_t visible=0;
        uint16_t lowest=0x7bff,highest=0;
        for(size_t i=0;i<size_t(w)*h*4;i+=4) {
            for(unsigned j=0;j<3;++j) Require((p[i+j]&0x7c00)!=0x7c00,"Non-finite SDK output");
            visible+=(p[i]&0x7fff)>0x0400;
            if(!(p[i]&0x8000)) { lowest=std::min(lowest,p[i]); highest=std::max(highest,p[i]); }
        }
        vmaDestroyBuffer(instance.GetAllocator(),pixels,memory);
        Require(visible>100,"SDK shared image is black");
        Require(highest>lowest+64,"SDK image lacks the captured triangle / spatial contrast");
        std::printf("PASS: official SDK shared output %ux%u, %zu visible pixels, contrast %04x..%04x\n",w,h,visible,lowest,highest);
    };
    inspect(320,180); output.Reset();
    Require(send(400,180,false),client.Error().c_str()); inspect(400,180); output.Reset();
    mesh.vertices[0].position[0]-=0.1f;
    Require(send(400,180,true),client.Error().c_str()); inspect(400,180); output.Reset();
    Require(!send(400,180,true,true),"Invalid scene must fail safely in isolated worker");
    client.Stop(); std::puts("PASS: cached frame, shared resize, mesh revision and invalid-index worker isolation");
    return 0;
} catch(const std::exception& e) { std::fprintf(stderr,"FAIL: %s\n",e.what()); return 1; }
