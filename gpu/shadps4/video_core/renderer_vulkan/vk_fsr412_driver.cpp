// SPDX-License-Identifier: GPL-2.0-or-later
// Local prototype using AMD's original Windows Vulkan driver provider.
#include "video_core/renderer_vulkan/vk_fsr412_driver.h"
#ifdef _WIN32
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/host_shaders/fsr4vk_prepare_comp.h"
#include "video_core/texture_cache/image.h"
#include "driver_provider.h"
#include "ffx_upscale.h"
#include <deque>
#include <cstdlib>
#include <stdexcept>
namespace Vulkan {
namespace {
struct Backend {ffxCreateContextDescHeader header;VkDevice device;VkPhysicalDevice physical;PFN_vkGetDeviceProcAddr gdpa;};
struct Version {ffxCreateContextDescHeader header;uint32_t version;};
void Message(uint32_t type,const wchar_t*message){std::printf("AMD Vulkan 4.1.2 message %u: %ls\n",type,message);}
}
struct Fsr412Driver::Impl {
    const Instance& instance; Scheduler& scheduler;
    DriverFsr::External provider{}; ffxContext context{};
    u32 out_width{},out_height{},context_rw{},context_rh{},input_width{},input_height{};
    bool context_hdr{};
    VideoCore::UniqueImage prepared_color,prepared_depth;
    vk::UniqueImageView prepared_color_view,prepared_depth_view;
    vk::UniqueDescriptorSetLayout prepare_descriptors;
    vk::UniquePipelineLayout prepare_layout;
    vk::UniquePipeline prepare_pipeline;
    vk::UniqueSampler prepare_sampler;
    std::deque<u64> ticks;
    std::string error;
    Impl(const Instance&i,Scheduler&s):instance(i),scheduler(s){}
    ~Impl(){if(context){scheduler.Finish();provider.provider.destroyContext(&context,nullptr);}}
    void Create(const Fsr4Upscaler::Frame& f){
        if(!provider.provider.createContext){
            provider=DriverFsr::Get(DriverFsr::Runtime(false),FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE);
            if((uint32_t(provider.provider.versionId)&0x7fffffffu)!=((4u<<22)|(1u<<12)|2u))
                throw std::runtime_error("Driver did not provide FSR 4.1.2");
        }
        Backend backend{};backend.header.type=0x01000003u;
        backend.device=VkDevice(instance.GetDevice());backend.physical=VkPhysicalDevice(instance.GetPhysicalDevice());
        auto vk=GetModuleHandleW(L"vulkan-1.dll");
        backend.gdpa=reinterpret_cast<PFN_vkGetDeviceProcAddr>(GetProcAddress(vk,"vkGetDeviceProcAddr"));
        if(!backend.gdpa || !DriverFsr::Supported(provider,&backend.device))
            throw std::runtime_error("AMD Vulkan 4.1.2 is unsupported on the selected device");
        Version version{};version.header.type=FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION;
        version.version=(4u<<22)|(1u<<12)|2u;backend.header.pNext=&version.header;
        ffxCreateContextDescUpscale create{};create.header.type=FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
        create.header.pNext=&backend.header;
        create.flags=FFX_UPSCALE_ENABLE_AUTO_EXPOSURE|FFX_UPSCALE_ENABLE_DEPTH_INVERTED;
        create.flags|=f.hdr?FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE:FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE;
        create.maxRenderSize={f.render_width,f.render_height};create.maxUpscaleSize={f.output.width,f.output.height};
        create.fpMessage=&Message;
        auto rc=provider.provider.createContext(&context,&create.header,nullptr);
        if(rc||!context)throw std::runtime_error("AMD Vulkan 4.1.2 context returned "+std::to_string(rc));
        out_width=f.output.width;out_height=f.output.height;context_rw=f.render_width;context_rh=f.render_height;context_hdr=f.hdr;
        std::printf("FSR 4.1.2 Vulkan driver: ACTIVE %s, %ux%u -> %ux%u, %s; original AMD shaders\n",
            provider.provider.versionName,f.render_width,f.render_height,out_width,out_height,f.hdr?"HDR":"sRGB");
    }
    static FfxApiResource Resource(const Fsr4Upscaler::Image&image,u32 format,bool output=false){
        FfxApiResource r{};r.resource=reinterpret_cast<void*>(VkImage(image.image));
        r.description.type=FFX_API_RESOURCE_TYPE_TEXTURE2D;r.description.format=format;
        r.description.width=image.width;r.description.height=image.height;r.description.depth=1;r.description.mipCount=1;
        r.description.usage=output?FFX_API_RESOURCE_USAGE_UAV:FFX_API_RESOURCE_USAGE_READ_ONLY;
        r.state=output?FFX_API_RESOURCE_STATE_UNORDERED_ACCESS:FFX_API_RESOURCE_STATE_COMPUTE_READ;
        return r;
    }
    void Transition(vk::CommandBuffer cmdbuf, const Fsr4Upscaler::Frame &f, bool to_read_only) {
        const std::array<std::pair<const Fsr4Upscaler::Image *, vk::ImageAspectFlags>, 3> inputs{{
            {&f.color, vk::ImageAspectFlagBits::eColor},
            {&f.depth, vk::ImageAspectFlagBits::eColor},
            {&f.motion, vk::ImageAspectFlagBits::eColor},
        }};
        std::array<vk::ImageMemoryBarrier2, 3> barriers;
        for (size_t i = 0; i < inputs.size(); ++i) {
            barriers[i] = vk::ImageMemoryBarrier2{
                .srcStageMask = to_read_only ? vk::PipelineStageFlagBits2::eAllCommands
                                             : vk::PipelineStageFlagBits2::eComputeShader,
                .srcAccessMask = to_read_only ? vk::AccessFlagBits2::eMemoryWrite
                                              : vk::AccessFlagBits2::eShaderRead,
                .dstStageMask = to_read_only ? vk::PipelineStageFlagBits2::eComputeShader
                                             : vk::PipelineStageFlagBits2::eAllCommands,
                .dstAccessMask = to_read_only ? vk::AccessFlagBits2::eShaderRead
                                              : vk::AccessFlagBits2::eMemoryRead |
                                                    vk::AccessFlagBits2::eMemoryWrite,
                .oldLayout = to_read_only ? vk::ImageLayout::eGeneral
                                          : vk::ImageLayout::eShaderReadOnlyOptimal,
                .newLayout = to_read_only ? vk::ImageLayout::eShaderReadOnlyOptimal
                                          : vk::ImageLayout::eGeneral,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = inputs[i].first->image,
                .subresourceRange = {inputs[i].second, 0, 1, 0, 1},
            };
        }
        // The output: earlier users are done before the provider writes it, and its writes are
        // visible to everything after.
        const vk::MemoryBarrier2 memory{
            .srcStageMask = to_read_only ? vk::PipelineStageFlagBits2::eAllCommands
                                         : vk::PipelineStageFlagBits2::eComputeShader,
            .srcAccessMask =
                to_read_only ? vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite
                             : vk::AccessFlagBits2::eShaderStorageWrite,
            .dstStageMask = to_read_only ? vk::PipelineStageFlagBits2::eComputeShader
                                         : vk::PipelineStageFlagBits2::eAllCommands,
            .dstAccessMask =
                to_read_only ? vk::AccessFlagBits2::eShaderStorageRead |
                                   vk::AccessFlagBits2::eShaderStorageWrite
                             : vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
        };
        cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1,
                                 .pMemoryBarriers = &memory,
                                 .imageMemoryBarrierCount = u32(barriers.size()),
                                 .pImageMemoryBarriers = barriers.data()});
    }

    Fsr4Upscaler::Frame Prepare(const Fsr4Upscaler::Frame &f) {
        const auto device = instance.GetDevice();
        if (!prepare_pipeline) {
            std::array<vk::DescriptorSetLayoutBinding, 4> bindings{};
            for (u32 i = 0; i < 4; ++i)
                bindings[i] = {.binding = i,
                               .descriptorType = i < 2 ? vk::DescriptorType::eCombinedImageSampler
                                                       : vk::DescriptorType::eStorageImage,
                               .descriptorCount = 1,
                               .stageFlags = vk::ShaderStageFlagBits::eCompute};
            prepare_descriptors = Check(device.createDescriptorSetLayoutUnique(
                {.flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
                 .bindingCount = 4,
                 .pBindings = bindings.data()}));
            vk::PushConstantRange push{vk::ShaderStageFlagBits::eCompute, 0, 16};
            prepare_layout =
                Check(device.createPipelineLayoutUnique({.setLayoutCount = 1,
                                                         .pSetLayouts = &*prepare_descriptors,
                                                         .pushConstantRangeCount = 1,
                                                         .pPushConstantRanges = &push}));
            const auto module = CompileSPV(FSR4VK_PREPARE_COMP, device);
            prepare_pipeline = Check(device.createComputePipelineUnique(
                {}, {.stage = {.stage = vk::ShaderStageFlagBits::eCompute,
                               .module = module,
                               .pName = "main"},
                     .layout = *prepare_layout}));
            device.destroyShaderModule(module);
            prepare_sampler = Check(
                device.createSamplerUnique({.magFilter = vk::Filter::eNearest,
                                            .minFilter = vk::Filter::eNearest,
                                            .mipmapMode = vk::SamplerMipmapMode::eNearest,
                                            .addressModeU = vk::SamplerAddressMode::eClampToEdge,
                                            .addressModeV = vk::SamplerAddressMode::eClampToEdge,
                                            .addressModeW = vk::SamplerAddressMode::eClampToEdge}));
        }
        bool resized = input_width != f.render_width || input_height != f.render_height;
        if (resized) {
            scheduler.WaitSubmitted();
            const auto make = [&](auto &image, auto &view, vk::Format format) {
                view.reset();
                image = VideoCore::UniqueImage(device, instance.GetAllocator());
                image.Create(
                    {.imageType = vk::ImageType::e2D,
                     .format = format,
                     .extent = {f.render_width, f.render_height, 1},
                     .mipLevels = 1,
                     .arrayLayers = 1,
                     .samples = vk::SampleCountFlagBits::e1,
                     .tiling = vk::ImageTiling::eOptimal,
                     .usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled,
                     .initialLayout = vk::ImageLayout::eUndefined});
                view = Check(device.createImageViewUnique(
                    {.image = vk::Image(image),
                     .viewType = vk::ImageViewType::e2D,
                     .format = format,
                     .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}}));
            };
            make(prepared_color, prepared_color_view, vk::Format::eR16G16B16A16Sfloat);
            make(prepared_depth, prepared_depth_view, vk::Format::eR32Sfloat);
            input_width = f.render_width;
            input_height = f.render_height;
        }
        std::array<vk::ImageMemoryBarrier2, 2> barriers{};
        for (u32 i = 0; i < 2; ++i)
            barriers[i] = {.srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                           .srcAccessMask =
                               vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
                           .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
                           .dstAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
                           .oldLayout =
                               resized ? vk::ImageLayout::eUndefined : vk::ImageLayout::eGeneral,
                           .newLayout = vk::ImageLayout::eGeneral,
                           .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                           .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                           .image = i ? vk::Image(prepared_depth) : vk::Image(prepared_color),
                           .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}};
        f.cmdbuf.pipelineBarrier2(
            {.imageMemoryBarrierCount = 2, .pImageMemoryBarriers = barriers.data()});
        const std::array<vk::ImageView, 4> views{f.color.view, f.depth.view, *prepared_color_view,
                                                 *prepared_depth_view};
        std::array<vk::DescriptorImageInfo, 4> infos{};
        std::array<vk::WriteDescriptorSet, 4> writes{};
        for (u32 i = 0; i < 4; ++i) {
            infos[i] = {.sampler = i < 2 ? *prepare_sampler : vk::Sampler{},
                        .imageView = views[i],
                        .imageLayout = vk::ImageLayout::eGeneral};
            writes[i] = {.dstBinding = i,
                         .descriptorCount = 1,
                         .descriptorType = i < 2 ? vk::DescriptorType::eCombinedImageSampler
                                                 : vk::DescriptorType::eStorageImage,
                         .pImageInfo = &infos[i]};
        }
        const bool srgb = f.color.format == vk::Format::eR8G8B8A8Srgb ||
                          f.color.format == vk::Format::eB8G8R8A8Srgb;
        std::array<u32, 4> constants{f.render_width, f.render_height, !f.hdr && srgb ? 1u : 0u, 0};
        f.cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, *prepare_pipeline);
        f.cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *prepare_layout, 0, writes);
        f.cmdbuf.pushConstants(*prepare_layout, vk::ShaderStageFlagBits::eCompute, 0, 16,
                               constants.data());
        f.cmdbuf.dispatch((f.render_width + 7) / 8, (f.render_height + 7) / 8, 1);
        auto prepared = f;
        prepared.color = {vk::Image(prepared_color), *prepared_color_view, f.render_width,
                          f.render_height};
        prepared.depth = {vk::Image(prepared_depth), *prepared_depth_view, f.render_width,
                          f.render_height};
        return prepared;
    }


    bool Record(Fsr4Upscaler::Frame& input){
        try{
            const bool changed=context&&(out_width!=input.output.width||out_height!=input.output.height||
                context_rw!=input.render_width||context_rh!=input.render_height||context_hdr!=input.hdr);
            if(changed){scheduler.Finish();input.cmdbuf=scheduler.CommandBuffer();
                provider.provider.destroyContext(&context,nullptr);context=nullptr;ticks.clear();}
            if(!context)Create(input);
            while(ticks.size()>=3){scheduler.Wait(ticks.front());ticks.pop_front();}
            auto f=Prepare(input);
            if(f.motion.width!=f.render_width||f.motion.height!=f.render_height)
                throw std::runtime_error("AMD Vulkan prototype requires render-sized motion vectors");
            ffxDispatchDescUpscale d{};d.header.type=FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
            d.commandList=reinterpret_cast<void*>(VkCommandBuffer(f.cmdbuf));
            d.color=Resource(f.color,FFX_API_SURFACE_FORMAT_R16G16B16A16_FLOAT);
            d.depth=Resource(f.depth,FFX_API_SURFACE_FORMAT_R32_FLOAT);
            d.motionVectors=Resource(f.motion,FFX_API_SURFACE_FORMAT_R16G16_FLOAT);
            d.output=Resource(f.output,FFX_API_SURFACE_FORMAT_R16G16B16A16_FLOAT,true);
            d.jitterOffset={f.jitter[0],f.jitter[1]};d.motionVectorScale={1.f,1.f};
            d.renderSize={f.render_width,f.render_height};d.upscaleSize={f.output.width,f.output.height};
            d.frameTimeDelta=f.frame_ms;d.preExposure=1.f;d.reset=f.reset;d.cameraNear=f.near_plane;
            d.cameraFar=f.far_plane;d.cameraFovAngleVertical=f.vertical_fov;d.viewSpaceToMetersFactor=1.f;
            d.flags=f.hdr?0u:FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_SRGB;
            Transition(f.cmdbuf,f,true);
            auto rc=provider.provider.dispatch(&context,&d.header);
            Transition(f.cmdbuf,f,false);
            if(rc)throw std::runtime_error("AMD Vulkan 4.1.2 dispatch returned "+std::to_string(rc));
            ticks.push_back(scheduler.CurrentTick());error.clear();return true;
        }catch(const std::exception&e){error=e.what();std::printf("FSR 4.1.2 Vulkan driver: %s\n",error.c_str());return false;}
    }
};
Fsr412Driver::Fsr412Driver(const Instance&i,Scheduler&s):impl(std::make_unique<Impl>(i,s)){}
Fsr412Driver::~Fsr412Driver()=default;
bool Fsr412Driver::Record(Fsr4Upscaler::Frame&f){return impl->Record(f);}
const std::string&Fsr412Driver::Error()const{return impl->error;}
bool Fsr412Driver::Available(const Instance& instance, int* status) {
    const auto result = [status](int code) { if (status) *status=code; return code==0; };
    if (instance.GetVendorID() != 0x1002) return result(1);
    try {
        const auto provider = DriverFsr::Get(DriverFsr::Runtime(false), FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE);
        // The driver's support query consumes the Vulkan backend triplet starting
        // at device, including physical device and gdpa (as in Create()).
        Backend backend{};
        backend.device = VkDevice(instance.GetDevice());
        backend.physical = VkPhysicalDevice(instance.GetPhysicalDevice());
        const auto vulkan = GetModuleHandleW(L"vulkan-1.dll");
        backend.gdpa = reinterpret_cast<PFN_vkGetDeviceProcAddr>(GetProcAddress(vulkan, "vkGetDeviceProcAddr"));
        if ((uint32_t(provider.provider.versionId) & 0x7fffffffu) != ((4u << 22) | (1u << 12) | 2u)) return result(3);
        if (!backend.gdpa) return result(5);
        if (!provider.isSupported) return result(5);
        return result(DriverFsr::Supported(provider, &backend.device) ? 0 : 1);
    } catch (const std::exception&) {
        return result(3);
    }
}
}
#endif
