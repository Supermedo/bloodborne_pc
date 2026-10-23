// SPDX-License-Identifier: GPL-2.0-or-later
// Adapted from 0xCydral/bloodborne_pc, commit 66576efbbd272d0c97e04a9bdb5631455d530eae.
// Input normalization and safe scheduler handoff added for Supermedo/windows.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>

#include "video_core/host_shaders/fsr4vk_prepare_comp.h"
#include "video_core/renderer_vulkan/fsr4vk_post.h"
#include "video_core/renderer_vulkan/vk_fsr4vk.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/texture_cache/image.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "bbport_fsr4vk_package.h"

#include "bbport_toggles.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#endif

namespace Vulkan {

namespace {

std::filesystem::path ExecutableDirectory() {
#ifdef _WIN32
    std::wstring path(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), DWORD(path.size()));
    path.resize(length);
    return std::filesystem::path{path}.parent_path();
#else
    return {};
#endif
}

} // namespace

std::filesystem::path Fsr4Vk::Directory() {
#ifdef _WIN32
    // The wide environment: a folder with a non-ASCII name must read the same here and in
    // bb-gpu-capabilities, whatever the process's ANSI code page is.
    if (const wchar_t *dir = _wgetenv(L"BB_FSR4VK_DIR"); dir && dir[0]) {
        return dir;
    }
#else
    if (const char *dir = std::getenv("BB_FSR4VK_DIR"); dir && dir[0]) {
        return dir;
    }
#endif
    return ExecutableDirectory() / "fsr4vk";
}

std::string Fsr4Vk::LibraryPathUtf8() {
    const auto u8 = (Directory() / LibraryName).u8string();
    return {reinterpret_cast<const char *>(u8.data()), u8.size()};
}

bool Fsr4Vk::FilesPresent() {
#ifdef _WIN32
    const char *setting = std::getenv("BB_FSR4VK");
    if (setting && setting[0] == '0') {
        return false;
    }
    return Fsr4VkPackage::Verified(Directory());
#else
    return false;
#endif
}

#ifdef _WIN32

namespace {

// The ABI of the FFX API (AMD FidelityFX SDK api/include/ffx_api.h, ffx_api_types.h and
// upscalers/include/ffx_upscale.h, MIT) as fsr4vk's provider implements it, plus fsr4vk's own
// descriptors (provider/ffx_vk_*.h in its repository). The port's headers are newer than the SDK
// this provider was built with, so the layouts are repeated here and checked below.
using StructType = u64;
struct Header {
    StructType type;
    Header *next;
};
struct Dimensions {
    u32 width, height;
};
struct Float2 {
    float x, y;
};
struct ResourceDescription {
    u32 type, format, width, height, depth, mip_count, flags, usage;
};
struct Resource {
    void *resource;
    ResourceDescription description;
    u32 state;
};
using Message = void (*)(u32 type, const wchar_t *text);
struct CreateUpscale {
    Header header;
    u32 flags;
    Dimensions max_render_size, max_upscale_size;
    Message message;
};
struct DispatchUpscale {
    Header header;
    void *command_list;
    Resource color, depth, motion_vectors, exposure, reactive, transparency_and_composition, output;
    Float2 jitter_offset, motion_vector_scale;
    Dimensions render_size, upscale_size;
    bool enable_sharpening;
    float sharpness, frame_time_delta, pre_exposure;
    bool reset;
    float camera_near, camera_far, camera_fov_vertical, view_space_to_meters;
    u32 flags;
};
struct OverrideVersion {
    Header header;
    u64 version_id;
};
struct BackendVk {
    Header header;
    VkDevice device;
    VkPhysicalDevice physical_device;
    PFN_vkGetDeviceProcAddr get_device_proc_addr;
};
struct ApiVersion {
    Header header;
    u32 api_version;
};
struct QueryDeviceFeatures {
    Header header;
    u32 version;
};
struct CreateDeviceFeatures {
    Header header;
    u32 enabled_flags, api_version, queue_family_index;
};
struct QueryProviderVersion {
    Header header;
    u64 version_id;
    const char *version_name;
};
struct QueryPresetCapabilities {
    Header header;
    u32 forced_preset_mask;
};
struct ConfigurePreset {
    Header header;
    u32 preset;
};
static_assert(sizeof(Resource) == 48 && sizeof(CreateUpscale) == 48 &&
              sizeof(DispatchUpscale) == 432 && sizeof(OverrideVersion) == 24);
static_assert(offsetof(CreateUpscale, max_upscale_size) == 28 &&
              offsetof(CreateUpscale, message) == 40);
static_assert(offsetof(DispatchUpscale, output) == 312 &&
              offsetof(DispatchUpscale, jitter_offset) == 360 &&
              offsetof(DispatchUpscale, enable_sharpening) == 392 &&
              offsetof(DispatchUpscale, reset) == 408 && offsetof(DispatchUpscale, flags) == 428);

constexpr StructType CreateUpscaleType = 0x00010000u;
constexpr StructType DispatchUpscaleType = 0x00010001u;
constexpr StructType OverrideVersionType = 5u;
constexpr StructType QueryProviderVersionType = 6u;
constexpr StructType BackendVkType = 3u;
constexpr StructType ApiVersionType = 0x46535234564b4150ull;
constexpr StructType QueryDeviceFeaturesType = 0x4653523444455651ull;
constexpr StructType CreateDeviceFeaturesType = 0x4653523444455643ull;
constexpr StructType QueryPresetCapabilitiesType = 0x4653523450434150ull;
constexpr StructType ConfigurePresetType = 0x4653523450534554ull;
// (4 << 22) | (1 << 12) | 1 under the provider's 0xF5A5CA1E tag: FSR 4.1.1 INT8.
constexpr u64 Version411 = (0xF5A5CA1Eull << 32) | ((4ull << 22) | (1ull << 12) | 1ull);

constexpr u32 FlagHdr = 1u << 0;
constexpr u32 FlagDepthInverted = 1u << 3;
constexpr u32 FlagAutoExposure = 1u << 5;
// FFX formats after normalization: RGBA16F color, R32F reverse depth, RG16F motion.
constexpr u32 FormatRgba16Float = 4, FormatRg16Float = 18, FormatR32Float = 28;
constexpr u32 StateUav = 2, StateComputeRead = 4;
constexpr u32 UsageUav = 1u << 1;
constexpr u32 ResourceTexture2d = 2;
constexpr u32 DeviceRobustBufferAccess = 1;
constexpr u32 ReturnOk = 0;
// The provider keeps eight descriptor, constant and view slots; the ninth outstanding frame
// would overwrite the first one's.
constexpr size_t FramesInFlight = 8;
// 0 native, 1 quality, 2 balanced, 3 performance, 4 drs, 5 ultra performance; the port's
// presets 0..4 skip DRS.
constexpr std::array<u32, 5> ProviderPreset{0, 1, 2, 3, 5};
constexpr std::array<const char *, 5> PresetName{"native", "quality", "balanced", "performance",
                                                 "ultra performance"};

using CreateContext = u32 (*)(void **context, Header *desc, const void *callbacks);
using DestroyContext = u32 (*)(void **context, const void *callbacks);
using Configure = u32 (*)(void **context, const Header *desc);
using Query = u32 (*)(void **context, Header *desc);
using Dispatch = u32 (*)(void **context, const Header *desc);

void ProviderMessage(u32 type, const wchar_t *text) {
    std::printf("Upscaler: fsr4vk %s: %ls\n", type == 0 ? "error" : "warning", text ? text : L"");
}

const char *ReturnName(u32 code) {
    static constexpr std::array<const char *, 8> names{
        "ok",          "error",         "unknown descriptor type", "runtime error",
        "no provider", "out of memory", "invalid parameter",       "descriptor type too new"};
    return code < names.size() ? names[code] : "unknown";
}

// v0.4.3 imports core Vulkan functions directly instead of using the supplied procedure
// callback. Temporarily redirect only its shader-module import during first dispatch.
// Neither the on-disk DLL nor the application's Vulkan dispatch table is modified.
thread_local bool optimize_post_shader{};

struct PostShaderScope {
    bool previous;
    explicit PostShaderScope(bool enabled) : previous{std::exchange(optimize_post_shader, enabled)} {}
    ~PostShaderScope() { optimize_post_shader = previous; }
};

VKAPI_ATTR VkResult VKAPI_CALL CreateProviderShaderModule(
    VkDevice device, const VkShaderModuleCreateInfo* info, const VkAllocationCallbacks* allocator,
    VkShaderModule* module) noexcept {
    // Forward through the same loader export as the provider's unmodified import,
    // including calls on other threads/devices outside the optimization scope.
    static const auto create_module = reinterpret_cast<PFN_vkCreateShaderModule>(
        GetProcAddress(GetModuleHandleW(L"vulkan-1.dll"), "vkCreateShaderModule"));
    if (optimize_post_shader && info && info->pCode && info->codeSize % sizeof(u32) == 0) {
        try {
            const auto code = Fsr4VkPost::Rewrite({info->pCode, info->codeSize / sizeof(u32)});
            if (!code.empty()) {
                auto optimized = *info;
                optimized.pCode = code.data();
                optimized.codeSize = code.size() * sizeof(u32);
                if (create_module(device, &optimized, allocator, module) == VK_SUCCESS) {
                    std::printf("Upscaler: fsr4vk coalesced output stores (4K Quality SDR)\n");
                    return VK_SUCCESS;
                }
                std::printf("Upscaler: fsr4vk optimized module rejected; using original shader\n");
            }
        } catch (...) {
            // Allocation/patch failure must not break an otherwise usable provider.
            std::printf("Upscaler: fsr4vk post rewrite unavailable; using original shader\n");
        }
    }
    return create_module(device, info, allocator, module);
}

class ProviderShaderPatch {
    inline static std::mutex mutex;
    std::unique_lock<std::mutex> lock{mutex};
    void** slot{};
    void* original{};

    static void** FindImport(HMODULE module) {
        auto* base = reinterpret_cast<unsigned char*>(module);
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
        const auto* pe = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        if (pe->Signature != IMAGE_NT_SIGNATURE) return nullptr;
        const auto directory = pe->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        if (!directory.VirtualAddress || !directory.Size) return nullptr;
        const auto* imports = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + directory.VirtualAddress);
        // The module has already been validated/mapped by the Windows loader.
        for (auto* dll = imports; dll->Name; ++dll) {
            const char* name = reinterpret_cast<const char*>(base + dll->Name);
            if (_stricmp(name, "vulkan-1.dll") != 0 || !dll->OriginalFirstThunk) continue;
            auto* names = reinterpret_cast<const IMAGE_THUNK_DATA*>(base + dll->OriginalFirstThunk);
            auto* addresses = reinterpret_cast<IMAGE_THUNK_DATA*>(base + dll->FirstThunk);
            for (; names->u1.AddressOfData; ++names, ++addresses) {
                if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
                const auto* symbol = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
                if (std::strcmp(symbol->Name, "vkCreateShaderModule") == 0) {
                    auto** target = reinterpret_cast<void**>(&addresses->u1.Function);
                    // Do not replace an existing third-party import hook.
                    const auto expected = GetProcAddress(GetModuleHandleA(name), "vkCreateShaderModule");
                    return *target == reinterpret_cast<void*>(expected) ? target : nullptr;
                }
            }
        }
        return nullptr;
    }

public:
    explicit ProviderShaderPatch(HMODULE module) {
        auto** target = FindImport(module);
        DWORD protection{};
        if (!target || !VirtualProtect(target, sizeof(void*), PAGE_READWRITE, &protection)) {
            std::printf("Upscaler: fsr4vk post import unavailable; using original shader\n");
            return;
        }
        original = InterlockedExchangePointer(target, reinterpret_cast<void*>(CreateProviderShaderModule));
        slot = target;
        DWORD ignored{};
        VirtualProtect(target, sizeof(void*), protection, &ignored);
    }

    ~ProviderShaderPatch() {
        if (!slot) return;
        DWORD protection{};
        if (VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &protection)) {
            InterlockedExchangePointer(slot, original);
            DWORD ignored{};
            VirtualProtect(slot, sizeof(void*), protection, &ignored);
        } else {
            // The remaining wrapper forwards unchanged outside its thread-local scope.
            std::printf("Upscaler: fsr4vk post import restoration failed\n");
        }
    }

    explicit operator bool() const { return slot != nullptr; }
};

} // namespace

struct Fsr4Vk::Impl {
    const Instance &instance;
    Scheduler &scheduler;
    HMODULE module{};
    CreateContext create{};
    DestroyContext destroy{};
    Configure configure{};
    Query query{};
    Dispatch dispatch{};
    void *context{};
    u32 out_width{}, out_height{};
    u32 input_width{}, input_height{};
    bool context_hdr{true};
    bool context_post_enabled{};
    VideoCore::UniqueImage prepared_color, prepared_depth;
    vk::UniqueImageView prepared_color_view, prepared_depth_view;
    vk::UniqueDescriptorSetLayout prepare_descriptors;
    vk::UniquePipelineLayout prepare_layout;
    vk::UniquePipeline prepare_pipeline;
    vk::UniqueSampler prepare_sampler;
    u32 preset_mask{};
    int configured_preset{-1};
    std::deque<u64> ticks; ///< scheduler ticks of the frames in flight (the provider's slot ring)
    std::string problem, described, provider_name;
    bool fatal{};
    bool post_attempted{};
    u64 recorded_frames{}, full_finishes{}, slot_waits{};
    std::chrono::nanoseconds full_wait_time{}, slot_wait_time{};

    Impl(const Instance &instance_, Scheduler &scheduler_)
        : instance{instance_}, scheduler{scheduler_} {}

    ~Impl() {
        if (context || input_width)
            scheduler.Finish();
        DestroyContextNow(true);
        if (module)
            FreeLibrary(module);
    }

    /// Fsr4Upscaler logs the reason and the fallback to FSR 3.1.
    void Fail(std::string reason, bool permanent) {
        problem = std::move(reason);
        fatal |= permanent;
    }

    /// `recording`: called from Record() on the command buffer being recorded, so only the earlier
    /// submissions are waited for.
    void DestroyContextNow(bool recording) {
        if (!context) {
            return;
        }
        if (recording) {
            scheduler.WaitSubmitted();
        } else {
            scheduler.Finish();
        }
        destroy(&context, nullptr);
        context = nullptr;
        ticks.clear();
        configured_preset = -1;
        post_attempted = false;
        described.clear();
    }

    bool Load() {
        if (module) {
            return true;
        }
        const auto path = Fsr4Vk::Directory() / LibraryName;
        module =
            LoadLibraryExW(path.c_str(), nullptr,
                           LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!module) {
            Fail("cannot load " + Fsr4Vk::LibraryPathUtf8() + " (error " +
                     std::to_string(GetLastError()) + ")",
                 true);
            return false;
        }
        bool complete = true;
        const auto load = [&](auto &function, const char *name) {
            function = reinterpret_cast<std::remove_reference_t<decltype(function)>>(
                GetProcAddress(module, name));
            complete = complete && function;
        };
        load(create, "ffxCreateContext");
        load(destroy, "ffxDestroyContext");
        load(configure, "ffxConfigure");
        load(query, "ffxQuery");
        load(dispatch, "ffxDispatch");
        if (!complete) {
            Fail(Fsr4Vk::LibraryPathUtf8() + " is not an FFX API provider", true);
            return false;
        }
        return true;
    }

    bool CreateContextNow(u32 ow, u32 oh, bool hdr) {
        if (ow < 64 || oh < 64 || ow > 3840 || oh > 2160) {
            Fail("output size " + std::to_string(ow) + "x" + std::to_string(oh) +
                     " is outside 64x64 to 3840x2160",
                 true);
            return false;
        }
        CreateUpscale desc{};
        desc.header.type = CreateUpscaleType;
        desc.flags = FlagDepthInverted | FlagAutoExposure | (hdr ? FlagHdr : (1u << 8));
        context_hdr = hdr;
        desc.max_render_size = {ow, oh};
        desc.max_upscale_size = {ow, oh};
        desc.message = ProviderMessage;
        OverrideVersion version{{OverrideVersionType, nullptr}, Version411};
        BackendVk backend{{BackendVkType, nullptr},
                          VkDevice(instance.GetDevice()),
                          VkPhysicalDevice(instance.GetPhysicalDevice()),
                          VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceProcAddr};
        ApiVersion api{{ApiVersionType, nullptr}, VK_API_VERSION_1_3};
        QueryDeviceFeatures features_query{{QueryDeviceFeaturesType, nullptr}, 0};
        CreateDeviceFeatures features{
            {CreateDeviceFeaturesType, nullptr},
            instance.IsRobustBufferAccessEnabled() ? DeviceRobustBufferAccess : 0u,
            VK_API_VERSION_1_3,
            instance.GetGraphicsQueueFamilyIndex()};
        desc.header.next = &version.header;
        version.header.next = &backend.header;
        backend.header.next = &api.header;
        api.header.next = nullptr;
        // The device snapshot is optional: a provider that does not know it gets none.
        if (query(nullptr, &features_query.header) == ReturnOk && features_query.version >= 1) {
            api.header.next = &features.header;
        }
        if (const u32 result = create(&context, &desc.header, nullptr); result != ReturnOk) {
            context = nullptr;
            Fail(std::string{"context creation failed: "} + ReturnName(result) + " (" +
                     std::to_string(result) +
                     "); the provider may lack FSR 4.1.1 or a device feature",
                 true);
            return false;
        }
        out_width = ow;
        out_height = oh;
        QueryPresetCapabilities caps{{QueryPresetCapabilitiesType, nullptr}, 0};
        preset_mask = query(&context, &caps.header) == ReturnOk ? caps.forced_preset_mask : 0;
        QueryProviderVersion name{{QueryProviderVersionType, nullptr}, 0, nullptr};
        // The provider's names start with a space ("FSR" is prefixed by the host): " 4.1.1 VK
        // INT8".
        std::string shown =
            query(&context, &name.header) == ReturnOk && name.version_name ? name.version_name : "";
        shown.erase(0, shown.find_first_not_of(' '));
        provider_name = shown.empty() ? "fsr4vk" : "FSR " + shown;
        if ((ow | oh) & 1) {
            std::printf("Upscaler: fsr4vk leaves the last row or column of an odd output size "
                        "unwritten (a known 4.1.1 limitation)\n");
        }
        return true;
    }

    bool SelectPreset(int preset) {
        if (preset == configured_preset) {
            return true;
        }
        const u32 id = ProviderPreset[size_t(preset)];
        std::string model = "automatic (by scale)";
        if (preset_mask & (1u << id)) {
            ConfigurePreset configuration{{ConfigurePresetType, nullptr}, id};
            if (const u32 result = configure(&context, &configuration.header); result != ReturnOk) {
                Fail(std::string{"cannot select the "} + PresetName[size_t(preset)] +
                         " model: " + ReturnName(result),
                     true);
                return false;
            }
            model = std::string{PresetName[size_t(preset)]};
        } else {
            std::printf("Upscaler: fsr4vk cannot force the %s model; it picks the model by scale\n",
                        PresetName[size_t(preset)]);
        }
        configured_preset = preset;
        post_attempted = false;
        described = provider_name + ", " + model + " model, output " + std::to_string(out_width) +
                    "x" + std::to_string(out_height);
        return true;
    }

    /// The inputs and the output are General; the provider reads its inputs as shader-read-only
    /// images and writes the output as a storage image.
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

    static Resource ResourceOf(const Fsr4Upscaler::Image &image, u32 width, u32 height, u32 format,
                               u32 usage, u32 state) {
        Resource r{};
        r.resource = reinterpret_cast<void *>(VkImage(image.image));
        r.description = {ResourceTexture2d, format, width, height, 1, 1, 0, usage};
        r.state = state;
        return r;
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

    bool Record(Fsr4Upscaler::Frame &input) {
        if (fatal || !Load())
            return false;
        for (const auto *image : {&input.color, &input.depth, &input.motion}) {
            if (image->width < input.render_width || image->height < input.render_height) {
                Fail("input image is smaller than the render extent", true);
                return false;
            }
        }
        // v0.4.3 reuses its eight descriptor/constant/view slots in FIFO order.
        // A completed submission containing the oldest dispatch also guarantees
        // that slot's completion event is set. Preserve overlap with later work;
        // wait only for that slot, not for the command buffer being recorded.
        // BB_FSR4VK_WAIT_ALL=1 selects the previous full-drain path
        // for A/B tests. This does not change the model, inputs or GPU barriers.
        static const bool force_full_wait = [] {
            const char* value = std::getenv("BB_FSR4VK_WAIT_ALL");
            return value && value[0] == '1';
        }();
        const bool full_wait = force_full_wait;
        if (ticks.size() >= FramesInFlight) {
            if (full_wait) {
                const auto started = std::chrono::steady_clock::now();
                scheduler.Finish();
                full_wait_time += std::chrono::steady_clock::now() - started;
                ++full_finishes;
                input.cmdbuf = scheduler.CommandBuffer();
                ticks.clear();
            } else {
                const u64 oldest = ticks.front();
                if (!scheduler.IsFree(oldest)) {
                    const auto started = std::chrono::steady_clock::now();
                    scheduler.Wait(oldest);
                    slot_wait_time += std::chrono::steady_clock::now() - started;
                    ++slot_waits;
                    // Wait may flush if multiple dispatches share the current
                    // submission. Never keep recording into a submitted buffer.
                    input.cmdbuf = scheduler.CommandBuffer();
                }
                ticks.pop_front();
            }
        }
        static const bool enable_post = [] {
            const char* setting = std::getenv("BB_FSR4VK_POST");
            return !setting || setting[0] != '0';
        }();
        const bool eligible_post = enable_post &&
            Fsr4VkPost::Eligible(instance.GetVendorID(), instance.GetDeviceID(),
                                input.output.width, input.output.height,
                                input.render_width, input.render_height, input.preset, input.hdr);
        const bool replace_context = context &&
            (input.output.width != out_width || input.output.height != out_height ||
             input.hdr != context_hdr || eligible_post != context_post_enabled);
        const bool replace_inputs = input_width &&
            (input_width != input.render_width || input_height != input.render_height);
        // Complete even unsubmitted dispatches before replacing their resources. This
        // happens only on a mode/size change, never during steady-state rendering.
        if (replace_context || replace_inputs) {
            scheduler.Finish();
            input.cmdbuf = scheduler.CommandBuffer();
        }
        const auto f = Prepare(input);
        // The provider reads the render-size part from the origin of each input.
        const auto exact = [&](const Fsr4Upscaler::Image &image) {
            return image.width == f.render_width && image.height == f.render_height;
        };
        if (!exact(f.color) || !exact(f.depth) || !exact(f.motion)) {
            Fail("the scene targets are larger than the render size (fixed render resolution mode)",
                 true);
            return false;
        }
        // Cached pipelines must not carry the rewrite into an unvalidated configuration.
        if (!context || replace_context) {
            DestroyContextNow(true);
            if (!CreateContextNow(f.output.width, f.output.height, f.hdr)) {
                return false;
            }
            context_post_enabled = eligible_post;
        }
        if (!SelectPreset(std::clamp(f.preset, 0, 4))) {
            return false;
        }
        DispatchUpscale d{};
        d.header.type = DispatchUpscaleType;
        d.command_list = reinterpret_cast<void *>(VkCommandBuffer(f.cmdbuf));
        // Prepare converted forward depth to a sampled R32 reverse-depth image.
        d.color = ResourceOf(f.color, f.render_width, f.render_height, FormatRgba16Float, 0,
                             StateComputeRead);
        d.depth = ResourceOf(f.depth, f.render_width, f.render_height, FormatR32Float, 0,
                             StateComputeRead);
        d.motion_vectors = ResourceOf(f.motion, f.render_width, f.render_height, FormatRg16Float, 0,
                                      StateComputeRead);
        d.output = ResourceOf(f.output, f.output.width, f.output.height, FormatRgba16Float,
                              UsageUav, StateUav);
        d.jitter_offset = {f.jitter[0], f.jitter[1]};
        // Match the original port: previous-current motion, measured in render pixels.
        d.motion_vector_scale = {1.0f, 1.0f};
        d.render_size = {f.render_width, f.render_height};
        d.upscale_size = {f.output.width, f.output.height};
        d.flags = f.hdr ? 0u : (1u << 1);
        d.frame_time_delta = f.frame_ms;
        d.pre_exposure = 1.0f;
        d.reset = f.reset;
        d.camera_near = f.near_plane;
        d.camera_far = f.far_plane;
        d.camera_fov_vertical = f.vertical_fov;
        d.view_space_to_meters = 1.0f;
        Transition(f.cmdbuf, f, true);
        std::optional<ProviderShaderPatch> post_patch;
        if (eligible_post && !post_attempted) {
            post_patch.emplace(module);
            post_attempted = true;
        }
        const PostShaderScope post_scope{post_patch && bool(*post_patch)};
        const u32 result = dispatch(&context, &d.header);
        Transition(f.cmdbuf, f, false);
        if (result != ReturnOk) {
            Fail(std::string{"dispatch failed: "} + ReturnName(result) + " (" +
                     std::to_string(result) + ")",
                 true);
            return false;
        }
        ticks.push_back(scheduler.CurrentTick());
        if (BbStats::enabled && ++recorded_frames % 300 == 0) {
            std::printf("fsr4vk retirement: frames=%llu mode=%s full_finishes=%llu "
                        "slot_waits=%llu full_wait_ms=%.3f slot_wait_ms=%.3f\n",
                        static_cast<unsigned long long>(recorded_frames),
                        full_wait ? "full" : "ring",
                        static_cast<unsigned long long>(full_finishes),
                        static_cast<unsigned long long>(slot_waits),
                        std::chrono::duration<double, std::milli>(full_wait_time).count(),
                        std::chrono::duration<double, std::milli>(slot_wait_time).count());
        }
        problem.clear();
        return true;
    }
};

Fsr4Vk::Fsr4Vk(const Instance &instance, Scheduler &scheduler)
    : impl{std::make_unique<Impl>(instance, scheduler)} {}

Fsr4Vk::~Fsr4Vk() = default;

bool Fsr4Vk::Record(Fsr4Upscaler::Frame &frame) { return impl->Record(frame); }

const char *Fsr4Vk::Problem() const noexcept {
    return impl->problem.empty() ? nullptr : impl->problem.c_str();
}

bool Fsr4Vk::Fatal() const noexcept { return impl->fatal; }

std::string Fsr4Vk::Describe() const { return impl->described; }

#else

struct Fsr4Vk::Impl {};
Fsr4Vk::Fsr4Vk(const Instance &, Scheduler &) {}
Fsr4Vk::~Fsr4Vk() = default;
bool Fsr4Vk::Record(Fsr4Upscaler::Frame &) { return false; }
const char *Fsr4Vk::Problem() const noexcept { return "fsr4vk is built for Windows only"; }
bool Fsr4Vk::Fatal() const noexcept { return true; }
std::string Fsr4Vk::Describe() const { return {}; }

#endif

} // namespace Vulkan
