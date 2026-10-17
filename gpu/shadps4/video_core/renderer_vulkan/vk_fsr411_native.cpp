// SPDX-License-Identifier: GPL-2.0-or-later
#ifdef _WIN32
#include "video_core/renderer_vulkan/vk_fsr411_native.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "native_fsr411.h"
#include <dxgiformat.h>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <stdexcept>

namespace Vulkan {
namespace {
DXGI_FORMAT NativeFormat(vk::Format format) {
    switch (format) {
    case vk::Format::eR16G16B16A16Sfloat: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case vk::Format::eB10G11R11UfloatPack32: return DXGI_FORMAT_R11G11B10_FLOAT;
    case vk::Format::eR8G8B8A8Unorm: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case vk::Format::eR8G8B8A8Srgb: return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    case vk::Format::eB8G8R8A8Unorm: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case vk::Format::eB8G8R8A8Srgb: return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    case vk::Format::eD32Sfloat: case vk::Format::eD32SfloatS8Uint: case vk::Format::eR32Sfloat:
        return DXGI_FORMAT_R32_FLOAT;
    case vk::Format::eD16Unorm: return DXGI_FORMAT_R16_UNORM;
    default: throw std::runtime_error("FSR 4.1.1 native: unsupported Vulkan image format " + vk::to_string(format));
    }
}
bool CanImportTexture(const Instance &instance, vk::Format format, vk::ImageUsageFlags usage,
                      uint32_t width, uint32_t height) {
    const VkPhysicalDeviceExternalImageFormatInfo external{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO, nullptr,
        VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT};
    VkPhysicalDeviceImageFormatInfo2 info{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2};
    info.pNext = &external;
    info.format = VkFormat(format);
    info.type = VK_IMAGE_TYPE_2D;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VkImageUsageFlags(usage);
    VkExternalImageFormatProperties support{VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
    VkImageFormatProperties2 properties{VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2};
    properties.pNext = &support;
    const auto result = VULKAN_HPP_DEFAULT_DISPATCHER.vkGetPhysicalDeviceImageFormatProperties2(
        instance.GetPhysicalDevice(), &info, &properties);
    return result == VK_SUCCESS &&
           (support.externalMemoryProperties.externalMemoryFeatures &
            VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) &&
           (support.externalMemoryProperties.compatibleHandleTypes &
            VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT) &&
           width <= properties.imageFormatProperties.maxExtent.width &&
           height <= properties.imageFormatProperties.maxExtent.height;
}
void CheckNative(vk::Result result, const char* operation) {
    if (result != vk::Result::eSuccess)
        throw std::runtime_error(std::string(operation) + ": " + vk::to_string(result));
}
}
struct Fsr411Native::Impl {
    const Instance& instance;
    Scheduler& scheduler;
    std::unique_ptr<NativeFsr411> native;
    vk::Buffer shared{};
    vk::DeviceMemory memory{};
    std::array<vk::Image, 4> textures{};
    std::array<vk::DeviceMemory, 4> texture_memory{};
    bool first_inputs{true};
    vk::ImageView output_view{};
    bool output_leased{};
    vk::Semaphore input_ready{}, output_ready{};
    uint64_t input_value{};
    NativeFsr411Config config{};
    std::string error;
    Impl(const Instance& i, Scheduler& s) : instance(i), scheduler(s) {}
    void Release() {
        const auto device = instance.GetDevice();
        if (input_ready) device.destroySemaphore(input_ready);
        if (output_ready) device.destroySemaphore(output_ready);
        input_ready = nullptr; output_ready = nullptr; input_value = 0;
        if (output_view)
            device.destroyImageView(output_view);
        output_view = nullptr;
        output_leased = false;
        for (auto &image : textures) {
            if (image)
                device.destroyImage(image);
            image = nullptr;
        }
        for (auto &allocation : texture_memory) {
            if (allocation)
                device.freeMemory(allocation);
            allocation = nullptr;
        }
        first_inputs = true;
        if (shared) device.destroyBuffer(shared);
        if (memory) device.freeMemory(memory);
        shared = nullptr; memory = nullptr; native.reset();
    }
    ~Impl() {
        ReleaseOutput(scheduler.CommandBuffer());
        if (shared) scheduler.Finish();
        Release();
    }
    void Ensure(Fsr4Upscaler::Frame& f) {
        NativeFsr411Config next{f.render_width, f.render_height, f.output.width, f.output.height,
            uint32_t(NativeFormat(f.color.format)), uint32_t(NativeFormat(f.depth.format)), f.hdr, {}};
        // Color, motion and the result cross the API boundary as shared textures.
        // Depth keeps the buffer route to support the game's depth/stencil format.
        next.shared_texture_mask = (1u << 0) | (1u << 2) | (1u << 3);
        // Nonlinear mode consumes the original encoded texels. A hardware sRGB SRV
        // would decode them before FSR applies its selected input color transform.
        if (!f.hdr && next.color_format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)
            next.color_format = DXGI_FORMAT_R8G8B8A8_UNORM;
        if (!f.hdr && next.color_format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)
            next.color_format = DXGI_FORMAT_B8G8R8A8_UNORM;
        if (native && next.render_width == config.render_width &&
            next.render_height == config.render_height &&
            next.output_width == config.output_width &&
            next.output_height == config.output_height &&
            next.color_format == config.color_format && next.depth_format == config.depth_format &&
            next.hdr == config.hdr)
            return;
        const std::array texture_formats{
            next.color_format == DXGI_FORMAT_R8G8B8A8_UNORM   ? vk::Format::eR8G8B8A8Unorm
            : next.color_format == DXGI_FORMAT_B8G8R8A8_UNORM ? vk::Format::eB8G8R8A8Unorm
                                                              : f.color.format,
            vk::Format::eR32Sfloat, vk::Format::eR16G16Sfloat, vk::Format::eR16G16B16A16Sfloat};
        const auto texture_usage = [](unsigned i) {
            return vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst |
                   (i == 3 ? vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled
                           : vk::ImageUsageFlagBits::eSampled);
        };
        // Preserve the original bridge on drivers without shared-image import support.
        // A process-start override keeps the buffer route available for regression tests.
        const char *shared_setting = std::getenv("BB_FSR411_SHARED_TEXTURES");
        if (shared_setting && shared_setting[0] == '0')
            next.shared_texture_mask = 0;
        for (unsigned i : {0u, 2u, 3u}) {
            if (!CanImportTexture(instance, texture_formats[i], texture_usage(i),
                                  i == 3 ? next.output_width : next.render_width,
                                  i == 3 ? next.output_height : next.render_height)) {
                next.shared_texture_mask = 0;
            }
        }
        const auto properties = instance.GetPhysicalDevice().getProperties2<vk::PhysicalDeviceProperties2,
                                                                  vk::PhysicalDeviceIDProperties>();
        const auto& id = properties.get<vk::PhysicalDeviceIDProperties>();
        if (!id.deviceLUIDValid) throw std::runtime_error("Vulkan adapter has no DXGI LUID");
        std::memcpy(next.adapter_luid, id.deviceLUID.data(), 8);
        if (native || shared) {
            scheduler.Finish();
            f.cmdbuf = scheduler.CommandBuffer();
            Release();
        }
        config = next;
        f.reset = true;
        native = std::make_unique<NativeFsr411>(config);
        if (!native->Ready()) throw std::runtime_error(native->Error());
        const auto device = instance.GetDevice();
        const vk::ExternalMemoryBufferCreateInfo external{
            .handleTypes = vk::ExternalMemoryHandleTypeFlagBits::eD3D12Resource};
        auto created = device.createBuffer({.pNext = &external, .size = native->SharedSize(),
            .usage = vk::BufferUsageFlagBits::eTransferSrc | vk::BufferUsageFlagBits::eTransferDst,
            .sharingMode = vk::SharingMode::eExclusive});
        CheckNative(created.result, "Create Vulkan interop buffer"); shared = created.value;
        const auto requirements = device.getBufferMemoryRequirements(shared);
        const auto handles = device.getMemoryWin32HandlePropertiesKHR(
            vk::ExternalMemoryHandleTypeFlagBits::eD3D12Resource, native->SharedHandle());
        CheckNative(handles.result, "Query shared D3D12 buffer memory");
        const auto types = instance.GetPhysicalDevice().getMemoryProperties();
        uint32_t type = UINT32_MAX;
        for (uint32_t i = 0; i < types.memoryTypeCount; ++i) {
            if ((requirements.memoryTypeBits & handles.value.memoryTypeBits & (1u << i)) &&
                (types.memoryTypes[i].propertyFlags & vk::MemoryPropertyFlagBits::eDeviceLocal)) { type = i; break; }
        }
        if (type == UINT32_MAX) throw std::runtime_error("No compatible D3D12/Vulkan device-local memory type");
        const vk::MemoryDedicatedAllocateInfo dedicated{.buffer = shared};
        const vk::ImportMemoryWin32HandleInfoKHR imported{.pNext = &dedicated,
            .handleType = vk::ExternalMemoryHandleTypeFlagBits::eD3D12Resource, .handle = native->SharedHandle()};
        const auto allocated = device.allocateMemory({.pNext = &imported,
            .allocationSize = requirements.size, .memoryTypeIndex = type});
        CheckNative(allocated.result, "Import D3D12 memory into Vulkan"); memory = allocated.value;
        CheckNative(device.bindBufferMemory(shared, memory, 0), "Bind shared D3D12/Vulkan buffer");

        for (unsigned i = 0; i < 4; ++i) {
            const auto handle = native->SharedTextureHandle(i);
            if (!handle)
                continue;
            const auto usage = texture_usage(i);
            const VkPhysicalDeviceExternalImageFormatInfo external_info{
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO, nullptr,
                VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT};
            VkPhysicalDeviceImageFormatInfo2 format_info{
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2};
            format_info.pNext = &external_info;
            format_info.format = VkFormat(texture_formats[i]);
            format_info.type = VK_IMAGE_TYPE_2D;
            format_info.tiling = VK_IMAGE_TILING_OPTIMAL;
            format_info.usage = VkImageUsageFlags(usage);
            VkExternalImageFormatProperties support{
                VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
            VkImageFormatProperties2 properties{VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2};
            properties.pNext = &support;
            CheckNative(
                vk::Result(VULKAN_HPP_DEFAULT_DISPATCHER.vkGetPhysicalDeviceImageFormatProperties2(
                    instance.GetPhysicalDevice(), &format_info, &properties)),
                "Query shared image format");
            if (!(support.externalMemoryProperties.externalMemoryFeatures &
                  VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT))
                throw std::runtime_error("Shared image format is not importable");
            const vk::ExternalMemoryImageCreateInfo external{
                .handleTypes = vk::ExternalMemoryHandleTypeFlagBits::eD3D12Resource};
            const auto created_image =
                device.createImage({.pNext = &external,
                                    .imageType = vk::ImageType::e2D,
                                    .format = texture_formats[i],
                                    .extent = {i == 3 ? next.output_width : next.render_width,
                                               i == 3 ? next.output_height : next.render_height, 1},
                                    .mipLevels = 1,
                                    .arrayLayers = 1,
                                    .samples = vk::SampleCountFlagBits::e1,
                                    .tiling = vk::ImageTiling::eOptimal,
                                    .usage = usage,
                                    .sharingMode = vk::SharingMode::eExclusive,
                                    .initialLayout = vk::ImageLayout::eUndefined});
            CheckNative(created_image.result, "Create imported Vulkan texture");
            textures[i] = created_image.value;
            const auto requirements = device.getImageMemoryRequirements(textures[i]);
            const auto handles = device.getMemoryWin32HandlePropertiesKHR(
                vk::ExternalMemoryHandleTypeFlagBits::eD3D12Resource, handle);
            CheckNative(handles.result, "Query D3D12 texture memory");
            uint32_t type = UINT32_MAX;
            for (uint32_t t = 0; t < types.memoryTypeCount; ++t)
                if ((requirements.memoryTypeBits & handles.value.memoryTypeBits & (1u << t)) &&
                    (types.memoryTypes[t].propertyFlags &
                     vk::MemoryPropertyFlagBits::eDeviceLocal)) {
                    type = t;
                    break;
                }
            if (type == UINT32_MAX)
                throw std::runtime_error("No shared-texture memory type");
            const vk::MemoryDedicatedAllocateInfo dedicated{.image = textures[i]};
            const vk::ImportMemoryWin32HandleInfoKHR imported{
                .pNext = &dedicated,
                .handleType = vk::ExternalMemoryHandleTypeFlagBits::eD3D12Resource,
                .handle = handle};
            const auto allocated = device.allocateMemory(
                {.pNext = &imported, .allocationSize = requirements.size, .memoryTypeIndex = type});
            CheckNative(allocated.result, "Import shared texture memory");
            texture_memory[i] = allocated.value;
            CheckNative(device.bindImageMemory(textures[i], texture_memory[i], 0),
                        "Bind shared texture");
            if (i == 3) {
                const auto view = device.createImageView(
                    {.image = textures[i],
                     .viewType = vk::ImageViewType::e2D,
                     .format = texture_formats[i],
                     .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}});
                CheckNative(view.result, "Create shared output view");
                output_view = view.value;
            }
        }
        std::puts(textures[3] ? "FSR 4.1.1 native: shared color/motion textures, direct shared "
                                "output, buffer for depth"
                              : "FSR 4.1.1 native: shared-buffer compatibility path");
        if (instance.IsFsr411AsyncSupported()) {
            const vk::SemaphoreTypeCreateInfo timeline{.semaphoreType = vk::SemaphoreType::eTimeline};
            for (auto [target, handle] : {std::pair{&input_ready, native->InputFenceHandle()},
                                         std::pair{&output_ready, native->OutputFenceHandle()}}) {
                const auto result = device.createSemaphore({.pNext = &timeline});
                CheckNative(result.result, "Create shared timeline semaphore");
                *target = result.value;
                CheckNative(device.importSemaphoreWin32HandleKHR({.semaphore = *target,
                    .handleType = vk::ExternalSemaphoreHandleTypeFlagBits::eD3D12Fence,
                    .handle = handle}), "Import D3D12 timeline fence");
            }
        }
        std::printf("FSR 4.1.1 native: shared GPU buffer %.1f MiB, no Linux assets required\n", native->SharedSize() / 1048576.0);
    }
    void Ownership(vk::CommandBuffer command, bool acquire, bool write) {
        const auto access = write ? vk::AccessFlagBits2::eTransferWrite : vk::AccessFlagBits2::eTransferRead;
        const vk::BufferMemoryBarrier2 barrier{
            .srcStageMask = acquire ? vk::PipelineStageFlagBits2::eNone : vk::PipelineStageFlagBits2::eAllTransfer,
            .srcAccessMask = acquire ? vk::AccessFlags2{} : access,
            .dstStageMask = acquire ? vk::PipelineStageFlagBits2::eAllTransfer : vk::PipelineStageFlagBits2::eNone,
            .dstAccessMask = acquire ? access : vk::AccessFlags2{},
            .srcQueueFamilyIndex = acquire ? VK_QUEUE_FAMILY_EXTERNAL : instance.GetGraphicsQueueFamilyIndex(),
            .dstQueueFamilyIndex = acquire ? instance.GetGraphicsQueueFamilyIndex() : VK_QUEUE_FAMILY_EXTERNAL,
            .buffer = shared, .offset = 0, .size = VK_WHOLE_SIZE};
        command.pipelineBarrier2({.bufferMemoryBarrierCount = 1, .pBufferMemoryBarriers = &barrier});
    }
    void TextureOwnership(vk::CommandBuffer command, unsigned i, bool acquire, bool write,
                          bool initial = false) {
        const auto access =
            write ? vk::AccessFlagBits2::eTransferWrite : vk::AccessFlagBits2::eTransferRead;
        const vk::ImageMemoryBarrier2 barrier{
            .srcStageMask = acquire ? vk::PipelineStageFlagBits2::eNone
                                    : vk::PipelineStageFlagBits2::eAllTransfer,
            .srcAccessMask = acquire ? vk::AccessFlags2{} : access,
            .dstStageMask = acquire ? vk::PipelineStageFlagBits2::eAllTransfer
                                    : vk::PipelineStageFlagBits2::eNone,
            .dstAccessMask = acquire ? access : vk::AccessFlags2{},
            .oldLayout = initial ? vk::ImageLayout::eUndefined : vk::ImageLayout::eGeneral,
            .newLayout = vk::ImageLayout::eGeneral,
            .srcQueueFamilyIndex =
                acquire ? VK_QUEUE_FAMILY_EXTERNAL : instance.GetGraphicsQueueFamilyIndex(),
            .dstQueueFamilyIndex =
                acquire ? instance.GetGraphicsQueueFamilyIndex() : VK_QUEUE_FAMILY_EXTERNAL,
            .image = textures[i],
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}};
        command.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier});
    }
    void OutputOwnership(vk::CommandBuffer command, bool acquire) {
        const auto rw = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite;
        const vk::ImageMemoryBarrier2 barrier{
            .srcStageMask = acquire ? vk::PipelineStageFlagBits2::eNone
                                    : vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask = acquire ? vk::AccessFlags2{} : rw,
            .dstStageMask = acquire ? vk::PipelineStageFlagBits2::eAllCommands
                                    : vk::PipelineStageFlagBits2::eNone,
            .dstAccessMask = acquire ? rw : vk::AccessFlags2{},
            .oldLayout = vk::ImageLayout::eGeneral,
            .newLayout = vk::ImageLayout::eGeneral,
            .srcQueueFamilyIndex =
                acquire ? VK_QUEUE_FAMILY_EXTERNAL : instance.GetGraphicsQueueFamilyIndex(),
            .dstQueueFamilyIndex =
                acquire ? instance.GetGraphicsQueueFamilyIndex() : VK_QUEUE_FAMILY_EXTERNAL,
            .image = textures[3],
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}};
        command.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier});
    }
    void ReleaseOutput(vk::CommandBuffer command) {
        if (!output_leased)
            return;
        OutputOwnership(command, false);
        output_leased = false;
    }
    void ImageAccess(vk::CommandBuffer command, const Fsr4Upscaler::Image& image, bool write) {
        const vk::ImageMemoryBarrier2 barrier{
            .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eAllTransfer,
            .dstAccessMask = write ? vk::AccessFlagBits2::eTransferWrite : vk::AccessFlagBits2::eTransferRead,
            .oldLayout = vk::ImageLayout::eGeneral, .newLayout = vk::ImageLayout::eGeneral,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = image.image, .subresourceRange = {image.aspect, 0, 1, 0, 1}};
        command.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier});
    }
    bool Record(Fsr4Upscaler::Frame& f, bool asynchronous) {
        try {
            if (output_leased)
                throw std::runtime_error("Shared FSR output was not released by its consumers");
            Ensure(f);
            Ownership(f.cmdbuf, true, true);
            const std::array inputs{f.color, f.depth, f.motion};
            for (uint32_t i = 0; i < inputs.size(); ++i) {
                const auto& p = native->Plane(i);
                const auto& input = inputs[i];
                if (input.width < p.width || input.height < p.height)
                    throw std::runtime_error("FSR 4.1.1 input is smaller than the render dimensions");
                ImageAccess(f.cmdbuf, input, false);
                if (textures[i]) {
                    TextureOwnership(f.cmdbuf, i, true, true, first_inputs);
                    const vk::ImageCopy copy{
                        .srcSubresource = {input.aspect, 0, 0, 1},
                        .dstSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
                        .extent = {p.width, p.height, 1}};
                    f.cmdbuf.copyImage(input.image, vk::ImageLayout::eGeneral, textures[i],
                                       vk::ImageLayout::eGeneral, copy);
                    TextureOwnership(f.cmdbuf, i, false, true);
                    continue;
                }
                const vk::BufferImageCopy region{.bufferOffset = p.offset,
                    .bufferRowLength = p.row_pitch / p.bytes_per_pixel, .bufferImageHeight = p.height,
                    .imageSubresource = {input.aspect, 0, 0, 1}, .imageExtent = {p.width, p.height, 1}};
                f.cmdbuf.copyImageToBuffer(input.image, vk::ImageLayout::eGeneral, shared, region);
            }
            first_inputs = false;
            Ownership(f.cmdbuf, false, true);
            // Split at the API boundary. Every caller receives the replacement command buffer;
            // continuing to record into the submitted one would corrupt the AMD driver.
            const NativeFsr411Frame frame{f.jitter[0], f.jitter[1], f.frame_ms, f.near_plane,
                f.far_plane, f.vertical_fov, f.sharpness, f.sharpen, f.reset};
            if (asynchronous) {
                if (!input_ready || !output_ready)
                    throw std::runtime_error("D3D12 fence import is unavailable");
                SubmitInfo signal{};
                signal.AddSignal(input_ready, ++input_value);
                scheduler.Flush(signal);
                f.cmdbuf = scheduler.CommandBuffer();
                uint64_t output_value{};
                if (!native->DispatchAsync(frame, input_value, output_value))
                    throw std::runtime_error(native->Error());
                scheduler.WaitExternal(output_ready, output_value);
            } else {
                scheduler.Finish();
                f.cmdbuf = scheduler.CommandBuffer();
                if (!native->Dispatch(frame)) throw std::runtime_error(native->Error());
            }
            if (textures[3]) {
                OutputOwnership(f.cmdbuf, true);
                output_leased = true;
                f.output = {textures[3], output_view, config.output_width, config.output_height,
                            vk::Format::eR16G16B16A16Sfloat};
                error.clear();
                return true;
            }
            Ownership(f.cmdbuf, true, false);
            ImageAccess(f.cmdbuf, f.output, true);
            const auto& p = native->Plane(3);
            const vk::BufferImageCopy output{.bufferOffset = p.offset,
                .bufferRowLength = p.row_pitch / p.bytes_per_pixel, .bufferImageHeight = p.height,
                .imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1}, .imageExtent = {p.width, p.height, 1}};
            f.cmdbuf.copyBufferToImage(shared, f.output.image, vk::ImageLayout::eGeneral, output);
            Ownership(f.cmdbuf, false, false);
            const vk::ImageMemoryBarrier2 visible{
                .srcStageMask = vk::PipelineStageFlagBits2::eAllTransfer,
                .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
                .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                .dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
                .oldLayout = vk::ImageLayout::eGeneral, .newLayout = vk::ImageLayout::eGeneral,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = f.output.image, .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}};
            f.cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &visible});
            error.clear(); return true;
        } catch (const std::exception& e) { error = e.what(); return false; }
    }
};
Fsr411Native::Fsr411Native(const Instance& i, Scheduler& s) : impl(std::make_unique<Impl>(i, s)) {}
Fsr411Native::~Fsr411Native() = default;
bool Fsr411Native::Record(Fsr4Upscaler::Frame& f, bool asynchronous) { return impl->Record(f, asynchronous); }
void Fsr411Native::ReleaseOutput(vk::CommandBuffer command) { impl->ReleaseOutput(command); }
const std::string& Fsr411Native::Error() const { return impl->error; }
}
#endif
