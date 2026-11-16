// SPDX-FileCopyrightText: Copyright 2026 IFreemz, bbport contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string_view>

#include "video_core/renderer_vulkan/vk_dlss.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dxgiformat.h>
#include <stdexcept>
#include "../../../dlss_bridge/bbport_dlss_bridge.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#endif

namespace Vulkan {

int Dlss::QualityForScale(float scale) {
    // DLAA only for equal sizes: any real upscale needs at least the Quality mode.
    return scale >= 2.9f ? 4 : scale >= 1.95f ? 3 : scale >= 1.65f ? 2 : scale > 1.01f ? 1 : 0;
}

#ifdef _WIN32

namespace {
constexpr wchar_t BridgeName[] = L"bbport_dlss.dll";
constexpr wchar_t NgxName[] = L"nvngx_dlss.dll";

std::filesystem::path ExecutableDirectory() {
    std::wstring path(MAX_PATH, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), DWORD(path.size()));
    path.resize(length);
    return std::filesystem::path{path}.parent_path();
}

void BridgeLog(int warning, const char* message) {
    std::printf("DLSS: %s%s\n", warning ? "warning: " : "", message);
}

bool CollectExtensions(const VkExtensionProperties* required, u32 count,
                       const std::vector<vk::ExtensionProperties>& available,
                       std::vector<std::string>& storage, std::vector<const char*>& enabled) {
    storage.clear();
    for (u32 i = 0; i < count; ++i) {
        const auto& request = required[i];
        const auto match = std::ranges::find_if(available, [&](const auto& extension) {
            return std::strcmp(extension.extensionName.data(), request.extensionName) == 0 &&
                   extension.specVersion >= request.specVersion;
        });
        if (match == available.end()) {
            std::printf("DLSS: missing Vulkan extension %s\n", request.extensionName);
            return false;
        }
        storage.emplace_back(request.extensionName);
    }
    for (const auto& name : storage) {
        if (std::ranges::none_of(enabled, [&](const char* current) { return name == current; })) {
            enabled.push_back(name.c_str());
        }
    }
    return true;
}
} // namespace

struct Dlss::Impl {
    HMODULE module{};
    const BbDlssApi* api{};
    std::vector<std::string> instance_extensions, device_extensions;
    std::optional<FeatureDesc> feature;
    std::optional<FrameGenDesc> frame_gen;
    int frame_gen_max{};
    std::string problem;
    bool eligible{true}, instance_ready{}, device_ready{}, initialized{}, available{};

    // DLSS on Direct3D 12 (bridge ABI 3): the resources shared with the bridge's D3D12 device.
    struct D3D12Path {
        BbDlss12Config config{};
        bool ready{}, failed{}, first{true};
        vk::Device device{};
        vk::Image color{}, motion{}, output{};
        vk::DeviceMemory color_memory{}, motion_memory{}, output_memory{}, depth_memory{};
        vk::Buffer depth{};
        u32 depth_row_pitch{}, depth_texel{};
        vk::Semaphore input_ready{}, output_ready{};
        u64 input_value{};

        void Release() {
            if (!device)
                return;
            for (auto* image : {&color, &motion, &output}) {
                if (*image)
                    device.destroyImage(*image);
                *image = nullptr;
            }
            if (depth)
                device.destroyBuffer(depth);
            depth = nullptr;
            for (auto* memory : {&color_memory, &motion_memory, &output_memory, &depth_memory}) {
                if (*memory)
                    device.freeMemory(*memory);
                *memory = nullptr;
            }
            for (auto* semaphore : {&input_ready, &output_ready}) {
                if (*semaphore)
                    device.destroySemaphore(*semaphore);
                *semaphore = nullptr;
            }
            input_value = 0;
            ready = false;
            first = true;
        }
    } d3d12;

    void Disable(std::string_view reason) {
        eligible = available = false;
        problem = reason;
        std::printf("DLSS: unavailable (%.*s)\n", int(reason.size()), reason.data());
    }
};

Dlss* Dlss::Get() {
    static Dlss* const dlss = []() -> Dlss* {
        const char* setting = std::getenv("BB_DLSS");
        if (setting && setting[0] == '0') {
            return nullptr;
        }
        const auto directory = ExecutableDirectory();
        if (!std::filesystem::is_regular_file(directory / BridgeName) ||
            !std::filesystem::is_regular_file(directory / NgxName)) {
            return nullptr;
        }
        auto* created = new Dlss;
        // The process may end through quick_exit/_exit paths: release NGX first when it can.
        std::atexit([] {
            if (Dlss* live = Get()) {
                live->Shutdown();
            }
        });
        return created;
    }();
    return dlss;
}

Dlss::Dlss() : impl{std::make_unique<Impl>()} {
    const auto directory = ExecutableDirectory();
    impl->module = LoadLibraryW((directory / BridgeName).c_str());
    const auto get_api =
        impl->module ? reinterpret_cast<BbDlssGetApiFn>(GetProcAddress(impl->module, "BbDlssGetApi"))
                     : nullptr;
    impl->api = get_api ? get_api() : nullptr;
    // ABI 1 bridges (DLSS only) still work; Frame Generation needs ABI 2.
    if (!impl->api || impl->api->abi < 1 || impl->api->abi > BBPORT_DLSS_BRIDGE_ABI) {
        impl->Disable("bbport_dlss.dll is missing or from another version");
        return;
    }
    // NGX writes its logs and model updates here: beside the saves and shader caches.
    std::filesystem::path data = directory / "dlss";
    if (const char* user = std::getenv("BB_GPU_USER_DIR"); user && user[0]) {
        data = std::filesystem::path{user} / "dlss";
    }
    std::error_code error;
    std::filesystem::create_directories(data, error);
    if (!impl->api->Configure(directory.c_str(), data.c_str(), BridgeLog)) {
        impl->Disable("bridge configuration failed");
    }
}

Dlss::~Dlss() {
    Shutdown();
}

void Dlss::AppendInstanceExtensions(std::vector<const char*>& enabled) {
    if (!impl->eligible) {
        return;
    }
    u32 count{};
    const VkExtensionProperties* required{};
    if (!impl->api->InstanceExtensions(&count, &required)) {
        return impl->Disable("instance extension query failed");
    }
    const auto [result, extensions] = vk::enumerateInstanceExtensionProperties();
    if (result != vk::Result::eSuccess ||
        !CollectExtensions(required, count, extensions, impl->instance_extensions, enabled)) {
        return impl->Disable("required Vulkan instance extensions are missing");
    }
    impl->instance_ready = true;
}

void Dlss::AppendDeviceExtensions(vk::Instance instance, vk::PhysicalDevice physical,
                                  std::vector<const char*>& enabled) {
    if (!impl->eligible || !impl->instance_ready) {
        return;
    }
    const auto properties = physical.getProperties();
    if (properties.vendorID != 0x10de) {
        return impl->Disable("not an NVIDIA GPU");
    }
    u32 count{};
    const VkExtensionProperties* required{};
    if (!impl->api->DeviceExtensions(instance, physical, &count, &required)) {
        return impl->Disable("this GPU or driver does not support DLSS (GeForce RTX needed)");
    }
    const auto [result, extensions] = physical.enumerateDeviceExtensionProperties();
    if (result != vk::Result::eSuccess ||
        !CollectExtensions(required, count, extensions, impl->device_extensions, enabled)) {
        return impl->Disable("required Vulkan device extensions are missing");
    }
    impl->device_ready = true;
}

void Dlss::Disable(const std::string& reason) {
    impl->Disable(reason);
}

void Dlss::Initialize(vk::Instance instance, vk::PhysicalDevice physical, vk::Device device) {
    if (!impl->eligible || !impl->device_ready || impl->initialized) {
        return;
    }
    impl->initialized = true;
    if (!impl->api->Initialize(instance, physical, device,
                               VULKAN_HPP_DEFAULT_DISPATCHER.vkGetInstanceProcAddr,
                               VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceProcAddr)) {
        return impl->Disable("NVIDIA NGX initialization failed (update the GPU driver)");
    }
    impl->available = true;
    if (impl->api->abi >= 2) {
        impl->frame_gen_max = std::max(0, impl->api->FrameGenMaxCount());
    } else {
        std::printf("DLSS: bbport_dlss.dll predates Frame Generation (rebuild it with "
                    "packaging/windows/build_dlss.sh)\n");
    }
    std::printf("DLSS: ready\n");
}

bool Dlss::Available() const {
    return impl->available;
}

std::string Dlss::Problem() const {
    return impl->problem;
}

bool Dlss::HasFeature(const FeatureDesc& desc) const {
    return impl->feature && *impl->feature == desc;
}

bool Dlss::CreateFeature(vk::CommandBuffer command, const FeatureDesc& desc) {
    if (!impl->available) {
        return false;
    }
    impl->feature.reset();
    const BbDlssFeature feature{desc.input_width,  desc.input_height, desc.output_width,
                                desc.output_height, desc.quality,      0,
                                0,                  desc.hdr ? 1 : 0};
    if (!impl->api->CreateFeature(command, &feature)) {
        return false;
    }
    impl->feature = desc;
    return true;
}

bool Dlss::Evaluate(vk::CommandBuffer command, const Frame& frame) {
    if (!impl->available || !impl->feature) {
        return false;
    }
    const auto image = [](const Resource& r) {
        const vk::ImageSubresourceRange range{r.aspect, 0, 1, 0, 1};
        return BbDlssImage{r.image,
                           r.view,
                           static_cast<VkImageSubresourceRange>(range),
                           static_cast<VkFormat>(r.format),
                           r.width,
                           r.height};
    };
    const BbDlssEvaluate parameters{image(frame.color),  image(frame.depth), image(frame.motion),
                                    image(frame.output), frame.jitter_x,     frame.jitter_y,
                                    frame.reset ? 1 : 0, frame.frame_ms,     frame.sharpness};
    return impl->api->Evaluate(command, &parameters) != 0;
}

void Dlss::ReleaseFeature() {
    impl->feature.reset();
    if (impl->api) {
        impl->api->ReleaseFeature();
    }
}

int Dlss::FrameGenMaxCount() const {
    return impl->available ? impl->frame_gen_max : 0;
}

bool Dlss::HasFrameGen(const FrameGenDesc& desc) const {
    return impl->frame_gen && *impl->frame_gen == desc;
}

bool Dlss::CreateFrameGen(vk::CommandBuffer command, const FrameGenDesc& desc) {
    if (FrameGenMaxCount() < 1) {
        return false;
    }
    impl->frame_gen.reset();
    const BbDlssgFeature feature{desc.width, desc.height, static_cast<VkFormat>(desc.format),
                                 desc.render_width, desc.render_height};
    if (!impl->api->FrameGenCreate(command, &feature)) {
        return false;
    }
    impl->frame_gen = desc;
    return true;
}

bool Dlss::EvaluateFrameGen(vk::CommandBuffer command, const FrameGenFrame& frame) {
    if (!impl->frame_gen) {
        return false;
    }
    const auto image = [](const Resource& r) {
        const vk::ImageSubresourceRange range{r.aspect, 0, 1, 0, 1};
        return BbDlssImage{r.image,
                           r.view,
                           static_cast<VkImageSubresourceRange>(range),
                           static_cast<VkFormat>(r.format),
                           r.width,
                           r.height};
    };
    BbDlssgEvaluate parameters{
        image(frame.backbuffer), image(frame.depth),  image(frame.motion), image(frame.output),
        frame.hudless.image ? image(frame.hudless) : BbDlssImage{},
        frame.jitter_x,          frame.jitter_y,      frame.camera_near,   frame.camera_far,
        frame.camera_fov,        frame.depth_inverted ? 1 : 0,             frame.reset ? 1 : 0,
        frame.count,             frame.index};
    parameters.has_matrices = frame.has_matrices ? 1 : 0;
    std::memcpy(parameters.view_to_clip, frame.view_to_clip.data(), sizeof(parameters.view_to_clip));
    std::memcpy(parameters.clip_to_prev_clip, frame.clip_to_prev_clip.data(),
                sizeof(parameters.clip_to_prev_clip));
    std::memcpy(parameters.prev_clip_to_clip, frame.prev_clip_to_clip.data(),
                sizeof(parameters.prev_clip_to_clip));
    return impl->api->FrameGenEvaluate(command, &parameters) != 0;
}

void Dlss::ReleaseFrameGen() {
    impl->frame_gen.reset();
    if (impl->api && impl->api->abi >= 2) {
        impl->api->FrameGenRelease();
    }
}

// ---- DLSS on Direct3D 12 (bridge ABI 3) -----------------------------------------------------

namespace {

DXGI_FORMAT DxgiColor(vk::Format format) {
    switch (format) {
    case vk::Format::eR16G16B16A16Sfloat: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case vk::Format::eB10G11R11UfloatPack32: return DXGI_FORMAT_R11G11B10_FLOAT;
    case vk::Format::eA2B10G10R10UnormPack32: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case vk::Format::eR8G8B8A8Unorm: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case vk::Format::eR8G8B8A8Srgb: return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    case vk::Format::eB8G8R8A8Unorm: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case vk::Format::eB8G8R8A8Srgb: return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    case vk::Format::eR32G32B32A32Sfloat: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    default:
        throw std::runtime_error("unsupported colour format " + vk::to_string(format));
    }
}

DXGI_FORMAT DxgiDepth(vk::Format format) {
    switch (format) {
    case vk::Format::eD32Sfloat:
    case vk::Format::eD32SfloatS8Uint:
    case vk::Format::eR32Sfloat: return DXGI_FORMAT_R32_FLOAT;
    case vk::Format::eD16Unorm:
    case vk::Format::eD16UnormS8Uint:
    case vk::Format::eR16Unorm: return DXGI_FORMAT_R16_UNORM;
    default:
        throw std::runtime_error("unsupported depth format " + vk::to_string(format));
    }
}

void Require(vk::Result result, const char* operation) {
    if (result != vk::Result::eSuccess)
        throw std::runtime_error(std::string(operation) + ": " + vk::to_string(result));
}

vk::DeviceMemory ImportMemory(const Instance& instance, HANDLE handle,
                              const vk::MemoryRequirements& requirements, vk::Image image,
                              vk::Buffer buffer) {
    const auto device = instance.GetDevice();
    const auto handles = device.getMemoryWin32HandlePropertiesKHR(
        vk::ExternalMemoryHandleTypeFlagBits::eD3D12Resource, handle);
    Require(handles.result, "D3D12 memory properties");
    const auto types = instance.GetPhysicalDevice().getMemoryProperties();
    u32 type = UINT32_MAX;
    for (u32 i = 0; i < types.memoryTypeCount; ++i) {
        if ((requirements.memoryTypeBits & handles.value.memoryTypeBits & (1u << i)) &&
            (types.memoryTypes[i].propertyFlags & vk::MemoryPropertyFlagBits::eDeviceLocal)) {
            type = i;
            break;
        }
    }
    if (type == UINT32_MAX)
        throw std::runtime_error("no device-local memory type for the D3D12 resource");
    const vk::MemoryDedicatedAllocateInfo dedicated{.image = image, .buffer = buffer};
    const vk::ImportMemoryWin32HandleInfoKHR imported{
        .pNext = &dedicated,
        .handleType = vk::ExternalMemoryHandleTypeFlagBits::eD3D12Resource,
        .handle = handle};
    const auto memory = device.allocateMemory(
        {.pNext = &imported, .allocationSize = requirements.size, .memoryTypeIndex = type});
    Require(memory.result, "import D3D12 memory");
    return memory.value;
}

void ImportImage(const Instance& instance, HANDLE handle, vk::Format format, u32 width, u32 height,
                 vk::Image& image, vk::DeviceMemory& memory) {
    const auto device = instance.GetDevice();
    const vk::ExternalMemoryImageCreateInfo external{
        .handleTypes = vk::ExternalMemoryHandleTypeFlagBits::eD3D12Resource};
    const auto created = device.createImage(
        {.pNext = &external,
         .imageType = vk::ImageType::e2D,
         .format = format,
         .extent = {width, height, 1},
         .mipLevels = 1,
         .arrayLayers = 1,
         .samples = vk::SampleCountFlagBits::e1,
         .tiling = vk::ImageTiling::eOptimal,
         .usage = vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst,
         .sharingMode = vk::SharingMode::eExclusive,
         .initialLayout = vk::ImageLayout::eUndefined});
    Require(created.result, "shared image");
    image = created.value;
    memory = ImportMemory(instance, handle, device.getImageMemoryRequirements(image), image, {});
    Require(device.bindImageMemory(image, memory, 0), "bind shared image");
}

// Queue-family ownership of a shared resource, to (acquire) or from the D3D12 queue.
void ImageOwnership(const Instance& instance, vk::CommandBuffer cmdbuf, vk::Image image,
                    bool acquire, bool write, bool initial) {
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
        .image = image,
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}};
    cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier});
}

void BufferOwnership(const Instance& instance, vk::CommandBuffer cmdbuf, vk::Buffer buffer,
                     bool acquire) {
    const vk::BufferMemoryBarrier2 barrier{
        .srcStageMask = acquire ? vk::PipelineStageFlagBits2::eNone
                                : vk::PipelineStageFlagBits2::eAllTransfer,
        .srcAccessMask = acquire ? vk::AccessFlags2{} : vk::AccessFlagBits2::eTransferWrite,
        .dstStageMask = acquire ? vk::PipelineStageFlagBits2::eAllTransfer
                                : vk::PipelineStageFlagBits2::eNone,
        .dstAccessMask = acquire ? vk::AccessFlagBits2::eTransferWrite : vk::AccessFlags2{},
        .srcQueueFamilyIndex =
            acquire ? VK_QUEUE_FAMILY_EXTERNAL : instance.GetGraphicsQueueFamilyIndex(),
        .dstQueueFamilyIndex =
            acquire ? instance.GetGraphicsQueueFamilyIndex() : VK_QUEUE_FAMILY_EXTERNAL,
        .buffer = buffer,
        .offset = 0,
        .size = VK_WHOLE_SIZE};
    cmdbuf.pipelineBarrier2({.bufferMemoryBarrierCount = 1, .pBufferMemoryBarriers = &barrier});
}

// A General-layout image of the renderer, before and after a transfer.
void GameImageAccess(vk::CommandBuffer cmdbuf, vk::Image image, vk::ImageAspectFlags aspect,
                     bool write, bool after) {
    const auto transfer =
        write ? vk::AccessFlagBits2::eTransferWrite : vk::AccessFlagBits2::eTransferRead;
    const auto all = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite;
    const vk::ImageMemoryBarrier2 barrier{
        .srcStageMask = after ? vk::PipelineStageFlagBits2::eAllTransfer
                              : vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = after ? vk::AccessFlags2{transfer} : vk::AccessFlags2{all},
        .dstStageMask = after ? vk::PipelineStageFlagBits2::eAllCommands
                              : vk::PipelineStageFlagBits2::eAllTransfer,
        .dstAccessMask = after ? vk::AccessFlags2{all} : vk::AccessFlags2{transfer},
        .oldLayout = vk::ImageLayout::eGeneral,
        .newLayout = vk::ImageLayout::eGeneral,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange = {aspect, 0, 1, 0, 1}};
    cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier});
}

} // namespace

bool Dlss::D3D12Usable(const Instance& instance) const {
    if (!impl->available || !impl->api || impl->api->abi < 3 || impl->d3d12.failed)
        return false;
    // Opt-in: only DirectX 12 add-ons hooked on NGX need it, and it costs a frame split.
    const char* setting = std::getenv("BB_DLSS_D3D12");
    if (!setting || setting[0] != '1')
        return false;
    // The same interop FSR 4.1.1 on D3D12 uses: external memory, D3D12 fences as timelines.
    return instance.IsFsr411AsyncSupported();
}

void Dlss::ReleaseD3D12(Scheduler& scheduler) {
    auto& d = impl->d3d12;
    if (!d.ready)
        return;
    scheduler.Finish();
    d.Release();
    if (impl->api && impl->api->abi >= 3)
        impl->api->D3D12Release();
}

bool Dlss::EvaluateD3D12(const Instance& instance, Scheduler& scheduler, vk::CommandBuffer& cmdbuf,
                         const FeatureDesc& desc, const Frame& frame) {
    auto& d = impl->d3d12;
    const auto device = instance.GetDevice();
    try {
        BbDlss12Config next{};
        next.render_width = desc.input_width;
        next.render_height = desc.input_height;
        next.output_width = desc.output_width;
        next.output_height = desc.output_height;
        next.color_format = DxgiColor(frame.color.format);
        next.depth_format = DxgiDepth(frame.depth.format);
        next.quality = desc.quality;
        next.preset = 0;
        next.hdr = desc.hdr ? 1 : 0;
        const auto id = instance.GetPhysicalDevice()
                            .getProperties2<vk::PhysicalDeviceProperties2,
                                            vk::PhysicalDeviceIDProperties>()
                            .get<vk::PhysicalDeviceIDProperties>();
        if (!id.deviceLUIDValid)
            throw std::runtime_error("the Vulkan device has no LUID");
        std::memcpy(next.adapter_luid, id.deviceLUID.data(), sizeof(next.adapter_luid));

        bool reset = frame.reset;
        if (!d.ready || std::memcmp(&next, &d.config, sizeof(next)) != 0) {
            if (d.ready) {
                scheduler.Finish(); // the old resources may still be in flight
                cmdbuf = scheduler.CommandBuffer();
            }
            d.Release();
            d.device = device;
            BbDlss12Shared shared{};
            if (!impl->api->D3D12Create(&next, &shared))
                throw std::runtime_error("the bridge could not create DLSS on D3D12");
            ImportImage(instance, shared.color, frame.color.format, next.render_width,
                        next.render_height, d.color, d.color_memory);
            ImportImage(instance, shared.motion, vk::Format::eR16G16Sfloat, next.render_width,
                        next.render_height, d.motion, d.motion_memory);
            ImportImage(instance, shared.output, vk::Format::eR16G16B16A16Sfloat,
                        next.output_width, next.output_height, d.output, d.output_memory);
            const vk::ExternalMemoryBufferCreateInfo external{
                .handleTypes = vk::ExternalMemoryHandleTypeFlagBits::eD3D12Resource};
            const auto buffer = device.createBuffer(
                {.pNext = &external,
                 .size = shared.depth_buffer_size,
                 .usage = vk::BufferUsageFlagBits::eTransferDst,
                 .sharingMode = vk::SharingMode::eExclusive});
            Require(buffer.result, "shared depth buffer");
            d.depth = buffer.value;
            d.depth_memory = ImportMemory(instance, shared.depth_buffer,
                                          device.getBufferMemoryRequirements(d.depth), {}, d.depth);
            Require(device.bindBufferMemory(d.depth, d.depth_memory, 0), "bind depth buffer");
            d.depth_row_pitch = shared.depth_row_pitch;
            d.depth_texel = shared.depth_texel_size;
            const vk::SemaphoreTypeCreateInfo timeline{.semaphoreType = vk::SemaphoreType::eTimeline};
            for (auto [target, handle] : {std::pair{&d.input_ready, shared.input_fence},
                                         std::pair{&d.output_ready, shared.output_fence}}) {
                const auto semaphore = device.createSemaphore({.pNext = &timeline});
                Require(semaphore.result, "shared timeline");
                *target = semaphore.value;
                Require(device.importSemaphoreWin32HandleKHR(
                            {.semaphore = *target,
                             .handleType = vk::ExternalSemaphoreHandleTypeFlagBits::eD3D12Fence,
                             .handle = handle}),
                        "import D3D12 fence");
            }
            d.config = next;
            d.ready = true;
            d.first = true;
            reset = true;
            std::printf("DLSS: Direct3D 12 path %ux%u -> %ux%u\n", next.render_width,
                        next.render_height, next.output_width, next.output_height);
        }

        // Inputs -> shared resources.
        const u32 w = next.render_width, h = next.render_height;
        GameImageAccess(cmdbuf, frame.color.image, frame.color.aspect, false, false);
        GameImageAccess(cmdbuf, frame.motion.image, frame.motion.aspect, false, false);
        GameImageAccess(cmdbuf, frame.depth.image, frame.depth.aspect, false, false);
        ImageOwnership(instance, cmdbuf, d.color, true, true, d.first);
        ImageOwnership(instance, cmdbuf, d.motion, true, true, d.first);
        BufferOwnership(instance, cmdbuf, d.depth, true);
        const vk::ImageCopy copy{.srcSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
                                 .dstSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
                                 .extent = {w, h, 1}};
        cmdbuf.copyImage(frame.color.image, vk::ImageLayout::eGeneral, d.color,
                         vk::ImageLayout::eGeneral, copy);
        cmdbuf.copyImage(frame.motion.image, vk::ImageLayout::eGeneral, d.motion,
                         vk::ImageLayout::eGeneral, copy);
        const vk::BufferImageCopy depth_copy{
            .bufferOffset = 0,
            .bufferRowLength = d.depth_row_pitch / d.depth_texel,
            .bufferImageHeight = h,
            .imageSubresource = {(frame.depth.aspect & vk::ImageAspectFlagBits::eDepth)
                                     ? vk::ImageAspectFlagBits::eDepth
                                     : vk::ImageAspectFlagBits::eColor,
                                 0, 0, 1},
            .imageExtent = {w, h, 1}};
        cmdbuf.copyImageToBuffer(frame.depth.image, vk::ImageLayout::eGeneral, d.depth, depth_copy);
        ImageOwnership(instance, cmdbuf, d.color, false, true, false);
        ImageOwnership(instance, cmdbuf, d.motion, false, true, false);
        BufferOwnership(instance, cmdbuf, d.depth, false);

        // Submit up to here; D3D12 starts when the copies are done.
        SubmitInfo signal{};
        signal.AddSignal(d.input_ready, ++d.input_value);
        scheduler.Flush(signal);
        cmdbuf = scheduler.CommandBuffer();
        const BbDlss12Frame parameters{frame.jitter_x, frame.jitter_y, reset ? 1 : 0,
                                       frame.frame_ms, frame.sharpness};
        u64 output_value{};
        const bool evaluated =
            impl->api->D3D12Evaluate(&parameters, d.input_value, &output_value) != 0;
        if (!output_value)
            throw std::runtime_error("the D3D12 queue could not be submitted");
        scheduler.WaitExternal(d.output_ready, output_value);

        // Shared output -> the renderer's output (General layout).
        ImageOwnership(instance, cmdbuf, d.output, true, false, d.first);
        GameImageAccess(cmdbuf, frame.output.image, vk::ImageAspectFlagBits::eColor, true, false);
        const vk::ImageCopy output_copy{
            .srcSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
            .dstSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
            .extent = {next.output_width, next.output_height, 1}};
        cmdbuf.copyImage(d.output, vk::ImageLayout::eGeneral, frame.output.image,
                         vk::ImageLayout::eGeneral, output_copy);
        GameImageAccess(cmdbuf, frame.output.image, vk::ImageAspectFlagBits::eColor, true, true);
        ImageOwnership(instance, cmdbuf, d.output, false, false, false);
        d.first = false;
        if (!evaluated)
            throw std::runtime_error("DLSS evaluation on D3D12 failed");
        return true;
    } catch (const std::exception& e) {
        std::printf("DLSS: Direct3D 12 path off (%s); back to Vulkan DLSS\n", e.what());
        d.failed = true;
        return false;
    }
}

void Dlss::Shutdown() {
    impl->available = false;
    impl->feature.reset();
    impl->frame_gen.reset();
    if (impl->api && impl->initialized) {
        impl->api->Shutdown();
        impl->initialized = false;
    }
}

#else

struct Dlss::Impl {};
Dlss::Dlss() = default;
Dlss::~Dlss() = default;
Dlss* Dlss::Get() {
    return nullptr;
}
void Dlss::AppendInstanceExtensions(std::vector<const char*>&) {}
void Dlss::AppendDeviceExtensions(vk::Instance, vk::PhysicalDevice, std::vector<const char*>&) {}
void Dlss::Initialize(vk::Instance, vk::PhysicalDevice, vk::Device) {}
void Dlss::Disable(const std::string&) {}
void Dlss::Shutdown() {}
bool Dlss::Available() const {
    return false;
}
std::string Dlss::Problem() const {
    return {};
}
bool Dlss::HasFeature(const FeatureDesc&) const {
    return false;
}
bool Dlss::CreateFeature(vk::CommandBuffer, const FeatureDesc&) {
    return false;
}
bool Dlss::Evaluate(vk::CommandBuffer, const Frame&) {
    return false;
}
void Dlss::ReleaseFeature() {}
int Dlss::FrameGenMaxCount() const {
    return 0;
}
bool Dlss::HasFrameGen(const FrameGenDesc&) const {
    return false;
}
bool Dlss::CreateFrameGen(vk::CommandBuffer, const FrameGenDesc&) {
    return false;
}
bool Dlss::EvaluateFrameGen(vk::CommandBuffer, const FrameGenFrame&) {
    return false;
}
void Dlss::ReleaseFrameGen() {}
bool Dlss::D3D12Usable(const Instance&) const {
    return false;
}
bool Dlss::EvaluateD3D12(const Instance&, Scheduler&, vk::CommandBuffer&, const FeatureDesc&,
                         const Frame&) {
    return false;
}
void Dlss::ReleaseD3D12(Scheduler&) {}

#endif

} // namespace Vulkan
