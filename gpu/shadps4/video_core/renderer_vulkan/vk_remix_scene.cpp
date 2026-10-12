// SPDX-License-Identifier: GPL-2.0-or-later
#ifdef _WIN32
#include "video_core/renderer_vulkan/vk_remix_scene.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_graphics_pipeline.h"
#include "video_core/amdgpu/regs.h"
#include "video_core/renderer_vulkan/motion_history.h"
#include "video_core/texture_cache/texture_cache.h"
#include "shader_recompiler/runtime_info.h"
#include "remix_client.h"
#include "remix_vulkan.h"
#include <vk_mem_alloc.h>
#include <xxhash.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <unordered_map>

namespace Vulkan {
namespace {
bool Requested() { const char* v=std::getenv("BB_RTX_REMIX"); return v && std::string_view(v)=="1"; }
std::filesystem::path ExecutableDir() {
    std::wstring name(32768,L'\0'); const DWORD n=GetModuleFileNameW(nullptr,name.data(),DWORD(name.size()));
    if (!n || n>=name.size()) throw std::runtime_error("Cannot locate Remix game host");
    name.resize(n); return std::filesystem::path(name).parent_path();
}
std::filesystem::path DataDir() {
    wchar_t data[32768]; const DWORD n=GetEnvironmentVariableW(L"BB_DATA_DIR",data,DWORD(std::size(data)));
    return n && n<std::size(data) ? std::filesystem::path(data) : ExecutableDir().parent_path();
}
bool SameCamera(const BbRemix::SceneCamera& a,const BbRemix::SceneCamera& b) {
    for (unsigned i=0;i<12;++i) if (std::abs(a.inverse_view[i]-b.inverse_view[i])>0.01f) return false;
    for (unsigned i=0;i<4;++i) if (std::abs(a.projection[i]-b.projection[i])>0.001f) return false;
    return true;
}
float Distance(const std::array<float,3>& a,const std::array<float,3>& b) {
    float v=0; for(unsigned i=0;i<3;++i) v+=(a[i]-b[i])*(a[i]-b[i]); return v;
}
struct Buffer {
    vk::Buffer handle{}; VmaAllocation allocation{}; void* mapped{};
};
bool MakeBuffer(const Instance& instance, uint64_t bytes, VkBufferUsageFlags usage, bool host, Buffer& out) {
    VkBufferCreateInfo ci{.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=bytes,.usage=usage};
    VmaAllocationCreateInfo ai{.usage=VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (host) {
        ai.usage=VMA_MEMORY_USAGE_AUTO;
        ai.flags=VMA_ALLOCATION_CREATE_MAPPED_BIT|VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT;
    }
    VmaAllocationInfo info{}; VkBuffer raw{};
    if (vmaCreateBuffer(instance.GetAllocator(),&ci,&ai,&raw,&out.allocation,&info)!=VK_SUCCESS) return false;
    out.handle=raw; out.mapped=info.pMappedData;
    if (host && !out.mapped) { vmaDestroyBuffer(instance.GetAllocator(),raw,out.allocation); out={}; return false; }
    return true;
}
void DestroyBuffer(const Instance& instance,Buffer& buffer) {
    if(buffer.handle) vmaDestroyBuffer(instance.GetAllocator(),buffer.handle,buffer.allocation);
    buffer={};
}
}
struct RemixScene::Impl {
    const Instance& instance; Scheduler& scheduler; Runtime& runtime; VideoCore::TextureCache& textures;
    bool enabled=false, failed=false, started=false, composed=false, reset=true;
    uint64_t frame=0, next_id=1; uint32_t used=0, capacity=0, empty_frames=0, skipped=0;
    Buffer capture, readback;
    BbRemix::SceneCamera camera{}, previous_camera{};
    BbRemix::Client client;
    std::unique_ptr<BbRemix::VulkanOutput> output;
    uint32_t output_width{},output_height{};
    std::filesystem::path data;
    struct Texture { VideoCore::ImageId id; uint64_t uid{}; std::wstring path; };
    std::unordered_map<uint64_t,Texture> texture_exports;
    struct Draw {
        uint64_t key{},material{}; uint32_t offset{},count{};
        bool dynamic{},cutout{}; uint64_t albedo{},normal{};
        BbRemix::SceneCamera camera;
    };
    std::vector<Draw> draws;
    struct Entry {
        uint64_t id{},key{},revision{},material{},last{};
        std::array<float,3> center{}; bool dynamic{}; uint32_t vertices{};
    };
    std::unordered_map<uint64_t,std::vector<Entry>> cache;
    Impl(const Instance& i,Scheduler& s,Runtime& r,VideoCore::TextureCache& t)
        :instance(i),scheduler(s),runtime(r),textures(t) {}
    void Fail(const char* message) {
        std::printf("RTX Remix game: %s; Vulkan scene retained\n",message);
        failed=true;
        Shader::RemixCapture::enabled=false;
        // Scheduler finishes every imported read before a host can release its texture.
        scheduler.Finish(); output.reset(); client.Stop(); started=false;
    }
    void ExportTextures() {
        uint64_t budget=16u<<20;
        for(auto& [uid,texture]:texture_exports) {
            if(!texture.path.empty()) continue;
            auto* live=textures.TryGetImage(texture.id,uid);
            if(!live) continue;
            auto& image=*live;
            if(image.info.size.depth>1 || image.info.resources.layers>1 ||
               image.usage.render_target || image.usage.depth_target || image.usage.vo_surface) continue;
            const uint32_t w=image.info.size.width,h=image.info.size.height;
            const uint64_t bytes=uint64_t(w)*h*4;
            if(!w || !h || w>4096 || h>4096 || bytes>budget) continue;
            const auto flags=image.format_features;
            if(!(flags & vk::FormatFeatureFlagBits2::eBlitSrc)) continue;
            const bool srgb=image.info.pixel_format==vk::Format::eR8G8B8A8Srgb ||
                image.info.pixel_format==vk::Format::eB8G8R8A8Srgb ||
                image.info.pixel_format==vk::Format::eBc1RgbaSrgbBlock ||
                image.info.pixel_format==vk::Format::eBc2SrgbBlock ||
                image.info.pixel_format==vk::Format::eBc3SrgbBlock ||
                image.info.pixel_format==vk::Format::eBc7SrgbBlock;
            const vk::Format format=srgb ? vk::Format::eR8G8B8A8Srgb : vk::Format::eR8G8B8A8Unorm;
            VideoCore::UniqueImage converted(instance.GetDevice(),instance.GetAllocator());
            converted.Create(vk::ImageCreateInfo{.imageType=vk::ImageType::e2D,.format=format,
                .extent={w,h,1},.mipLevels=1,.arrayLayers=1,.samples=vk::SampleCountFlagBits::e1,
                .tiling=vk::ImageTiling::eOptimal,.usage=vk::ImageUsageFlagBits::eTransferDst|vk::ImageUsageFlagBits::eTransferSrc});
            Buffer pixels;
            if(!MakeBuffer(instance,bytes,VK_BUFFER_USAGE_TRANSFER_DST_BIT,true,pixels)) continue;
            runtime.Transit(&image,vk::ImageLayout::eTransferSrcOptimal,vk::PipelineStageFlagBits2::eTransfer,vk::AccessFlagBits2::eTransferRead);
            runtime.FlushBarriers(); scheduler.EndRendering();
            scheduler.Record([src=image.GetImage(),dst=vk::Image(converted),buffer=pixels.handle,w,h](vk::CommandBuffer cmd) {
                vk::ImageMemoryBarrier2 barrier{.srcStageMask=vk::PipelineStageFlagBits2::eTopOfPipe,
                    .dstStageMask=vk::PipelineStageFlagBits2::eTransfer,.dstAccessMask=vk::AccessFlagBits2::eTransferWrite,
                    .oldLayout=vk::ImageLayout::eUndefined,.newLayout=vk::ImageLayout::eTransferDstOptimal,
                    .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
                    .image=dst,.subresourceRange={vk::ImageAspectFlagBits::eColor,0,1,0,1}};
                cmd.pipelineBarrier2({.imageMemoryBarrierCount=1,.pImageMemoryBarriers=&barrier});
                const vk::ImageBlit region{.srcSubresource={vk::ImageAspectFlagBits::eColor,0,0,1},
                    .srcOffsets=std::array{vk::Offset3D{0,0,0},vk::Offset3D{s32(w),s32(h),1}},
                    .dstSubresource={vk::ImageAspectFlagBits::eColor,0,0,1},
                    .dstOffsets=std::array{vk::Offset3D{0,0,0},vk::Offset3D{s32(w),s32(h),1}}};
                cmd.blitImage(src,vk::ImageLayout::eTransferSrcOptimal,dst,vk::ImageLayout::eTransferDstOptimal,region,vk::Filter::eNearest);
                barrier.srcStageMask=vk::PipelineStageFlagBits2::eTransfer; barrier.srcAccessMask=vk::AccessFlagBits2::eTransferWrite;
                barrier.dstAccessMask=vk::AccessFlagBits2::eTransferRead; barrier.oldLayout=vk::ImageLayout::eTransferDstOptimal;
                barrier.newLayout=vk::ImageLayout::eTransferSrcOptimal;
                cmd.pipelineBarrier2({.imageMemoryBarrierCount=1,.pImageMemoryBarriers=&barrier});
                const vk::BufferImageCopy copy{.imageSubresource={vk::ImageAspectFlagBits::eColor,0,0,1},.imageExtent={w,h,1}};
                cmd.copyImageToBuffer(dst,vk::ImageLayout::eTransferSrcOptimal,buffer,copy);
                const vk::MemoryBarrier2 host{.srcStageMask=vk::PipelineStageFlagBits2::eTransfer,.srcAccessMask=vk::AccessFlagBits2::eTransferWrite,
                    .dstStageMask=vk::PipelineStageFlagBits2::eHost,.dstAccessMask=vk::AccessFlagBits2::eHostRead};
                cmd.pipelineBarrier2({.memoryBarrierCount=1,.pMemoryBarriers=&host});
            });
            scheduler.Finish(); vmaInvalidateAllocation(instance.GetAllocator(),pixels.allocation,0,bytes);
            if(image.info.pixel_format==vk::Format::eBc5UnormBlock) {
                auto* rgba=static_cast<uint8_t*>(pixels.mapped);
                for(uint64_t i=0;i<bytes;i+=4) {
                    const float x=float(rgba[i])/127.5f-1.f,y=float(rgba[i+1])/127.5f-1.f;
                    rgba[i+2]=uint8_t(std::clamp((std::sqrt(std::max(0.f,1.f-x*x-y*y))+1.f)*127.5f,0.f,255.f));
                    rgba[i+3]=255;
                }
            }
            const uint32_t metadata[]{w,h,uint32_t(format)};
            const uint64_t hash=XXH3_64bits_withSeed(pixels.mapped,bytes,XXH3_64bits(metadata,sizeof(metadata)));
            const auto path=data/"remix-assets"/(std::to_string(hash)+".dds");
            std::filesystem::create_directories(path.parent_path());
            if(!std::filesystem::exists(path)) {
                // DDS DX10 header: RGBA8, preserving the source texture's sRGB interpretation.
                uint32_t header[37]{}; header[0]=0x20534444; header[1]=124; header[2]=0x100f;
                header[3]=h; header[4]=w; header[5]=w*4; header[7]=1;
                header[19]=32; header[20]=4; header[21]=0x30315844; header[27]=0x1000;
                header[32]=srgb ? 29 : 28; header[33]=3; header[35]=1;
                auto temporary=path; temporary+=L".tmp";
                std::ofstream file(temporary,std::ios::binary); file.write(reinterpret_cast<const char*>(header),sizeof(header));
                file.write(static_cast<const char*>(pixels.mapped),std::streamsize(bytes));
                file.close();
                if(!file) { std::error_code error; std::filesystem::remove(temporary,error); DestroyBuffer(instance,pixels); continue; }
                std::error_code error; std::filesystem::rename(temporary,path,error);
                if(error) { std::filesystem::remove(temporary,error); DestroyBuffer(instance,pixels); continue; }
            }
            texture.path=path.wstring(); DestroyBuffer(instance,pixels); budget-=bytes;
            if(budget<4096) break;
        }
    }
};
RemixScene::RemixScene(const Instance& instance,Scheduler& scheduler,Runtime& runtime,VideoCore::TextureCache& textures)
    :impl(std::make_unique<Impl>(instance,scheduler,runtime,textures)) {
    auto& p=*impl;
    if(!Requested()) return;
    if(!instance.IsRemixCaptureSupported()) { p.Fail("transform feedback / shared output unsupported on the selected adapter"); return; }
    try {
        p.data=std::filesystem::absolute(DataDir());
        uint64_t size=128u<<20;
        if(const char* mb=std::getenv("BB_REMIX_CAPTURE_MB")) size=std::clamp<uint64_t>(std::strtoull(mb,nullptr,10),32,256)<<20;
        size=std::min(size,instance.MaxTransformFeedbackBufferSize());
        p.capacity=uint32_t(size/sizeof(BbRemix::CapturedVertex));
        if(!p.capacity || !MakeBuffer(instance,size,VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_BUFFER_BIT_EXT|VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT,false,p.capture) ||
           !MakeBuffer(instance,size,VK_BUFFER_USAGE_TRANSFER_DST_BIT,true,p.readback)) { p.Fail("scene capture buffer allocation failed"); return; }
        p.enabled=true; Shader::RemixCapture::enabled=true;
        OnFrameStart(); std::printf("RTX Remix game: enabled, driver-bounded transform feedback (%u vertices)\n",p.capacity);
    } catch(const std::exception& e) { p.Fail(e.what()); }
}
RemixScene::~RemixScene() {
    auto& p=*impl; p.scheduler.Finish(); p.output.reset(); p.client.Stop();
    Shader::RemixCapture::enabled=false; DestroyBuffer(p.instance,p.capture); DestroyBuffer(p.instance,p.readback);
}
bool RemixScene::Enabled() const { return impl->enabled && !impl->failed; }
void RemixScene::OnFrameStart() {
    auto& p=*impl; if(!Enabled()) return;
    p.scheduler.Finish(); // Includes last SDK image read before its producer may reuse it.
    p.draws.clear(); p.used=p.skipped=0; p.camera={}; p.composed=false; ++p.frame;
    // A stale tail from a short strip/restart is never mistaken for captured vertices.
    p.scheduler.EndRendering();
    p.scheduler.Record([buffer=p.capture.handle](vk::CommandBuffer cmd) {
        const vk::MemoryBarrier2 before{.srcStageMask=vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask=vk::AccessFlagBits2::eMemoryRead|vk::AccessFlagBits2::eMemoryWrite,
            .dstStageMask=vk::PipelineStageFlagBits2::eTransfer,.dstAccessMask=vk::AccessFlagBits2::eTransferWrite};
        cmd.pipelineBarrier2({.memoryBarrierCount=1,.pMemoryBarriers=&before}); cmd.fillBuffer(buffer,0,VK_WHOLE_SIZE,0);
        const vk::MemoryBarrier2 after{.srcStageMask=vk::PipelineStageFlagBits2::eTransfer,.srcAccessMask=vk::AccessFlagBits2::eTransferWrite,
            .dstStageMask=vk::PipelineStageFlagBits2::eTransformFeedbackEXT,.dstAccessMask=vk::AccessFlagBits2::eTransformFeedbackWriteEXT};
        cmd.pipelineBarrier2({.memoryBarrierCount=1,.pMemoryBarriers=&after});
    });
}
void RemixScene::OnConstants(const float* data) {
    auto& p=*impl; if(!Enabled() || data[0]!=3000.f || data[4]<64 || data[5]<64 || std::abs(data[1]*data[0]-1)>1e-3f) return;
    std::copy_n(data+180,12,p.camera.inverse_view);
    p.camera.projection[0]=data[52]; p.camera.projection[1]=data[57];
    p.camera.projection[2]=data[62]; p.camera.projection[3]=data[63];
}
RemixScene::CaptureRange RemixScene::Capture(const GraphicsPipeline* pipeline,const AmdGpu::Regs& regs,u64 geometry,u32 index_offset) {
    auto& p=*impl; if(!Enabled() || p.composed || !p.camera.Valid() || !pipeline->GetGraphicsKey().remix_capture) return {};
    const auto topology=regs.primitive_type;
    uint64_t vertices=regs.num_indices;
    if(topology==AmdGpu::PrimitiveType::TriangleStrip || topology==AmdGpu::PrimitiveType::TriangleFan)
        vertices=vertices>=3 ? (vertices-2)*3 : 0;
    else if(topology!=AmdGpu::PrimitiveType::TriangleList) return {};
    vertices*=regs.num_instances.NumInstances();
    if(!vertices || vertices>p.capacity-p.used || p.draws.size()>=8192) { ++p.skipped; return {}; }
    const auto& vs=pipeline->GetStage(Shader::SwStage::Vertex);
    const auto* ps=pipeline->GetStages()[u32(Shader::SwStage::Fragment)];
    Impl::Draw draw{};
    uint64_t key[6]{vs.pgm_hash,geometry,regs.index_base_address.Address<VAddr>(),regs.num_indices,index_offset,regs.num_instances.NumInstances()};
    draw.key=XXH3_64bits(key,sizeof(key)); draw.camera=p.camera;
    draw.camera.viewport_sign[0]=regs.viewports[0].xscale<0 ? -1.f : 1.f;
    draw.camera.viewport_sign[1]=regs.viewports[0].yscale<0 ? -1.f : 1.f;
    p.camera=draw.camera;
    draw.offset=p.used; draw.count=uint32_t(vertices);
    for(const auto& resource:vs.buffers) {
        if(resource.IsSpecial()) continue;
        const auto sharp=resource.GetSharp(vs);
        if(sharp.GetStride()==16 && Motion::ClassifyBuffer(sharp.GetSize())!=Motion::BufferRole::Other) draw.dynamic=true;
    }
    draw.cutout=ps && ps->has_discard;
    const u32 uv=pipeline->GetGraphicsKey().remix_uv;
    if(ps && uv!=0xffff) for(const auto& resource:ps->images) {
        if(resource.is_depth || resource.is_written || resource.remix_uv==0xffff) continue;
        const u32 input=resource.remix_uv/4;
        if(input>=regs.num_interp || regs.ps_inputs[input].use_default ||
            regs.ps_inputs[input].input_offset*4+resource.remix_uv%4!=uv) continue;
        const auto sharp=resource.GetSharp(*ps);
        if(sharp.GetType()!=AmdGpu::ImageType::Color2D || !sharp.Address()) continue;
        auto desc=VideoCore::TextureCache::ImageDesc(sharp,resource);
        const auto id=p.textures.FindImage(desc);
        auto& image=p.textures.GetImage(id);
        if(image.usage.render_target || image.usage.depth_target || image.info.resources.layers>1) continue;
        const bool albedo=sharp.GetNumberFmt()==AmdGpu::NumberFormat::Srgb;
        const bool normal=sharp.GetDataFmt()==AmdGpu::DataFormat::FormatBc5 &&
            image.info.pixel_format==vk::Format::eBc5UnormBlock;
        if((albedo && !draw.albedo) || (normal && !draw.normal)) {
            const uint64_t uid=image.image_uid;
            p.texture_exports.try_emplace(uid,Impl::Texture{id,uid,{}});
            if(albedo) draw.albedo=uid; else draw.normal=uid;
        }
    }
    p.draws.push_back(draw); p.used+=draw.count;
    return {p.capture.handle,vk::DeviceSize(draw.offset)*32,vk::DeviceSize(draw.count)*32};
}
bool RemixScene::Compose(vk::Image target,u32 width,u32 height) {
    auto& p=*impl; if(!Enabled() || p.composed) return false; p.composed=true;
    if(p.draws.empty() || !p.camera.Valid()) {
        if(++p.empty_frames>60) { p.cache.clear(); p.reset=true; } return false;
    }
    p.empty_frames=0;
    try {
        p.scheduler.EndRendering();
        p.scheduler.Record([src=p.capture.handle,dst=p.readback.handle,size=vk::DeviceSize(p.used)*32](vk::CommandBuffer cmd) {
            const vk::MemoryBarrier2 before{.srcStageMask=vk::PipelineStageFlagBits2::eTransformFeedbackEXT,
                .srcAccessMask=vk::AccessFlagBits2::eTransformFeedbackWriteEXT,.dstStageMask=vk::PipelineStageFlagBits2::eTransfer,
                .dstAccessMask=vk::AccessFlagBits2::eTransferRead};
            cmd.pipelineBarrier2({.memoryBarrierCount=1,.pMemoryBarriers=&before}); cmd.copyBuffer(src,dst,vk::BufferCopy{0,0,size});
            const vk::MemoryBarrier2 after{.srcStageMask=vk::PipelineStageFlagBits2::eTransfer,.srcAccessMask=vk::AccessFlagBits2::eTransferWrite,
                .dstStageMask=vk::PipelineStageFlagBits2::eHost,.dstAccessMask=vk::AccessFlagBits2::eHostRead};
            cmd.pipelineBarrier2({.memoryBarrierCount=1,.pMemoryBarriers=&after});
        });
        p.scheduler.Finish(); vmaInvalidateAllocation(p.instance.GetAllocator(),p.readback.allocation,0,vk::DeviceSize(p.used)*32);
        p.ExportTextures();
        std::array<float,3> position{p.camera.inverse_view[3],p.camera.inverse_view[7],p.camera.inverse_view[11]};
        std::array<float,3> previous{p.previous_camera.inverse_view[3],p.previous_camera.inverse_view[7],p.previous_camera.inverse_view[11]};
        if(p.previous_camera.Valid() && Distance(position,previous)>2500.f) { p.cache.clear(); p.reset=true; }
        p.previous_camera=p.camera;
        BbRemix::Wire::Writer updates;
        uint32_t update_count=0; uint64_t update_vertices=0;
        for(const auto& draw:p.draws) {
            if(!SameCamera(draw.camera,p.camera)) continue;
            const auto* captured=static_cast<const BbRemix::CapturedVertex*>(p.readback.mapped)+draw.offset;
            std::vector<uint32_t> indices(draw.count); for(uint32_t i=0;i<draw.count;++i) indices[i]=i;
            auto mesh=BbRemix::BuildMesh({captured,draw.count},indices,BbRemix::Topology::Triangles,draw.camera);
            if(mesh.vertices.empty()) continue;
            auto& entries=p.cache[draw.key]; Impl::Entry* entry=nullptr;
            float closest=draw.dynamic ? 16.f : 0.0004f;
            for(auto& old:entries) {
                const float distance=Distance(old.center,mesh.center);
                if(old.last!=p.frame && (!draw.dynamic || old.last+2>=p.frame) && distance<closest) { closest=distance; entry=&old; }
            }
            if(!entry) { entries.push_back({.id=p.next_id++,.key=draw.key,.dynamic=draw.dynamic}); entry=&entries.back(); }
            const auto path=[&](uint64_t uid)->std::wstring {
                const auto it=p.texture_exports.find(uid); return it==p.texture_exports.end() ? std::wstring{} : it->second.path;
            };
            const std::wstring albedo=path(draw.albedo),normal=path(draw.normal);
            uint64_t material=XXH3_64bits(albedo.data(),albedo.size()*sizeof(wchar_t));
            material=XXH3_64bits_withSeed(normal.data(),normal.size()*sizeof(wchar_t),material)^uint64_t(draw.cutout); if(!material) material=1;
            const uint64_t revision=mesh.revision^material;
            if(entry->revision!=revision || p.reset) {
                if(update_vertices+mesh.vertices.size()>(2u<<20)) { ++p.skipped; continue; }
                BbRemix::Wire::Mesh info{entry->id,revision,material,uint32_t(mesh.vertices.size()),uint32_t(mesh.indices.size()),draw.cutout,
                    uint32_t(albedo.size()),uint32_t(normal.size())};
                updates.Put(info); updates.Bytes(albedo.data(),albedo.size()*sizeof(wchar_t)); updates.Bytes(normal.data(),normal.size()*sizeof(wchar_t));
                updates.Bytes(mesh.vertices.data(),mesh.vertices.size()*sizeof(BbRemix::Vertex)); updates.Bytes(mesh.indices.data(),mesh.indices.size()*sizeof(uint32_t));
                ++update_count; update_vertices+=mesh.vertices.size();
                entry->revision=revision; entry->material=material; entry->vertices=uint32_t(mesh.vertices.size());
            }
            entry->center=mesh.center; entry->last=p.frame;
        }
        std::vector<uint64_t> instances;
        uint64_t cached_vertices=0;
        for(auto it=p.cache.begin();it!=p.cache.end();) {
            auto& entries=it->second;
            std::erase_if(entries,[&](const auto& entry){return !entry.revision || entry.last+(entry.dynamic ? 1 : 600)<p.frame;});
            for(const auto& entry:entries) { instances.push_back(entry.id); cached_vertices+=entry.vertices; }
            if(entries.empty()) it=p.cache.erase(it); else ++it;
        }
        if(instances.empty()) return false;
        if(instances.size()>BbRemix::Wire::MaxMeshes || cached_vertices>(8u<<20)) {
            p.cache.clear(); p.reset=true; std::puts("RTX Remix: scene retention limit reached; rebuilding visible scene"); return false;
        }
        if(!p.started) {
            const auto bin=ExecutableDir(); auto root=bin.parent_path();
            wchar_t custom[32768]; DWORD n=GetEnvironmentVariableW(L"BB_REMIX_RUNTIME",custom,DWORD(std::size(custom)));
            const auto dll=n && n<std::size(custom) ? std::filesystem::absolute(custom) : root/"remix-runtime-1.5.2/.trex/d3d9.dll";
            const auto props=p.instance.GetPhysicalDevice().getProperties2<vk::PhysicalDeviceProperties2,vk::PhysicalDeviceIDProperties>();
            const auto& id=props.get<vk::PhysicalDeviceIDProperties>();
            BbRemix::Wire::Initialize info{width,height}; std::copy_n(id.deviceLUID.data(),8,info.luid.data());
            if(!id.deviceLUIDValid || !p.client.Start(bin/"bb-remix-host.exe",dll,p.data/"logs/rtx-remix-game.log",info)) { p.Fail(p.client.Error().c_str()); return false; }
            p.started=true; p.output=std::make_unique<BbRemix::VulkanOutput>(p.instance.GetPhysicalDevice(),p.instance.GetDevice());
        }
        // Release all Vulkan references before the producer resizes the shared texture.
        if(p.output_width!=width || p.output_height!=height) p.output->Reset();
        BbRemix::Wire::Frame frame{p.frame,width,height,update_count,uint32_t(instances.size()),p.reset,p.camera};
        BbRemix::Wire::Writer request; request.Put(frame); request.Bytes(updates.data.data(),updates.data.size()); request.Bytes(instances.data(),instances.size()*sizeof(uint64_t));
        if(!p.client.Render(request)) { p.Fail(p.client.Error().c_str()); return false; }
        p.reset=false; p.output_width=width; p.output_height=height;
        if(!p.client.Ready()) return false; // Shader compilation keeps native gameplay visible.
        if(!p.output->Import(p.client.Output())) { p.Fail(p.output->Error().c_str()); return false; }
        const vk::Image source(p.output->Image()); const uint32_t family=p.instance.GetGraphicsQueueFamilyIndex();
        auto* output=p.output.get();
        p.scheduler.Record([output,source,target,width,height,family](vk::CommandBuffer cmd) {
            output->Acquire(cmd,family);
            vk::ImageMemoryBarrier2 barrier{.srcStageMask=vk::PipelineStageFlagBits2::eAllCommands,
                .srcAccessMask=vk::AccessFlagBits2::eMemoryRead|vk::AccessFlagBits2::eMemoryWrite,
                .dstStageMask=vk::PipelineStageFlagBits2::eTransfer,.dstAccessMask=vk::AccessFlagBits2::eTransferWrite,
                .oldLayout=vk::ImageLayout::eGeneral,.newLayout=vk::ImageLayout::eGeneral,
                .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
                .image=target,.subresourceRange={vk::ImageAspectFlagBits::eColor,0,1,0,1}};
            cmd.pipelineBarrier2({.imageMemoryBarrierCount=1,.pImageMemoryBarriers=&barrier});
            const vk::ImageBlit region{.srcSubresource={vk::ImageAspectFlagBits::eColor,0,0,1},
                .srcOffsets=std::array{vk::Offset3D{0,0,0},vk::Offset3D{s32(width),s32(height),1}},
                .dstSubresource={vk::ImageAspectFlagBits::eColor,0,0,1},
                .dstOffsets=std::array{vk::Offset3D{0,0,0},vk::Offset3D{s32(width),s32(height),1}}};
            cmd.blitImage(source,vk::ImageLayout::eGeneral,target,vk::ImageLayout::eGeneral,region,vk::Filter::eNearest);
            output->Release(cmd,family);
            barrier.srcStageMask=vk::PipelineStageFlagBits2::eTransfer; barrier.srcAccessMask=vk::AccessFlagBits2::eTransferWrite;
            barrier.dstStageMask=vk::PipelineStageFlagBits2::eColorAttachmentOutput;
            barrier.dstAccessMask=vk::AccessFlagBits2::eColorAttachmentRead|vk::AccessFlagBits2::eColorAttachmentWrite;
            cmd.pipelineBarrier2({.imageMemoryBarrierCount=1,.pImageMemoryBarriers=&barrier});
        });
        if(p.frame%300==0) std::printf("RTX Remix game: %zu meshes (%llu vertices), %u updates, %u skipped; official SDK image composed before HUD\n",
            instances.size(),static_cast<unsigned long long>(cached_vertices),update_count,p.skipped);
        return true;
    } catch(const std::exception& e) { p.Fail(e.what()); return false; }
}
} // namespace Vulkan
#endif
