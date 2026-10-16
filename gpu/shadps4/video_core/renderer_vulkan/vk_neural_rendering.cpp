// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_vulkan/vk_neural_rendering.h"
#include "bbport_neural_settings.h"
#include "bbport_settings.h"
#include "video_core/renderer_vulkan/vk_fg_images.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/host_shaders/neural_prepare_comp.h"
#include "video_core/host_shaders/neural_resolve_comp.h"
#include "video_core/host_shaders/neural_ui_comp.h"
#include "../../../nr_bridge/bbport_nr_bridge.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <optional>
#include <span>
#ifdef _WIN32
#include <windows.h>
#endif

namespace Vulkan {
namespace {
bool requested_device = false;
std::string device_reason = "Enable neural rendering in the launcher and restart the game";
void DeviceProblem(const char* text) {
    device_reason = text;
    BbNeural::SetStatus(text);
}
bool ProtectHud(const BbNeural::Config& config) {
    // FG needs a genuinely HUD-free scene even when NR's own correction is switched off.
    return config.ui_correction || BbSettings::Get().startup_frame_generation != 0;
}
constexpr std::array<const char *, 4> DeviceExtensions{
    "VK_NVX_binary_import", "VK_NVX_image_view_handle", "VK_KHR_push_descriptor",
    "VK_KHR_buffer_device_address"};

bool Same(const BbNeural::Config &a, const BbNeural::Config &b) {
    if (a.enabled != b.enabled || a.preset != b.preset || a.input_scale != b.input_scale ||
        a.pass_count != b.pass_count || a.ui_correction != b.ui_correction)
        return false;
    for (int i = 0; i < BbNeural::MaxPasses; ++i) {
        const auto &x = a.passes[i];
        const auto &y = b.passes[i];
        if (x.enabled != y.enabled || x.stage != y.stage || x.style != y.style ||
            x.intensity != y.intensity || x.structure != y.structure || x.tone != y.tone ||
            x.skin_structure != y.skin_structure || x.auto_skin != y.auto_skin)
            return false;
    }
    return true;
}

void Barrier(vk::CommandBuffer cmd, vk::Image image,
             vk::ImageLayout before = vk::ImageLayout::eGeneral,
             vk::ImageLayout after = vk::ImageLayout::eGeneral) {
    const vk::ImageMemoryBarrier2 b{
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
        .oldLayout = before,
        .newLayout = after,
        .image = image,
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}};
    cmd.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b});
}

void Copy(vk::CommandBuffer cmd, vk::Image src, vk::Image dst, u32 w, u32 h) {
    Barrier(cmd, src);
    Barrier(cmd, dst);
    const vk::ImageBlit r{
        .srcSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
        .srcOffsets = std::array{vk::Offset3D{0, 0, 0}, vk::Offset3D{s32(w), s32(h), 1}},
        .dstSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
        .dstOffsets = std::array{vk::Offset3D{0, 0, 0}, vk::Offset3D{s32(w), s32(h), 1}}};
    cmd.blitImage(src, vk::ImageLayout::eGeneral, dst, vk::ImageLayout::eGeneral, r,
                  vk::Filter::eNearest);
    Barrier(cmd, src);
    Barrier(cmd, dst);
}

NeuralRendering::Color ImageColor(const FgImage &i, bool linear, bool hdr) {
    return {i.image, i.view, i.format, i.width, i.height, linear, hdr};
}
BbNrImage BridgeImage(const FgImage &i) {
    return {i.image, i.view,  {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}, VkFormat(i.format),
            i.width, i.height};
}
void Log(int warning, const char *text) {
    std::printf("Neural rendering: %s%s\n", warning ? "warning: " : "", text ? text : "");
    std::fflush(stdout);
}
} // namespace

void NeuralRendering::AppendInstanceExtensions(std::vector<const char *> &enabled) {
    if (!BbNeural::Snapshot().enabled)
        return;
    constexpr const char *name = "VK_KHR_get_physical_device_properties2";
    if (std::none_of(enabled.begin(), enabled.end(),
                     [&](const char *x) { return std::strcmp(x, name) == 0; }))
        enabled.push_back(name);
}

bool NeuralRendering::AppendDeviceExtensions(vk::PhysicalDevice physical,
                                             std::vector<const char *> &enabled) {
    requested_device = false;
    if (!BbNeural::Snapshot().enabled)
        return false;
#ifdef _WIN32
    const auto properties = physical.getProperties();
    if (properties.vendorID != 0x10de) {
        DeviceProblem("Neural rendering requires a compatible NVIDIA GPU");
        return false;
    }
    const auto extensions = physical.enumerateDeviceExtensionProperties();
    if (extensions.result != vk::Result::eSuccess)
        return false;
    for (const char *name : DeviceExtensions) {
        if (std::none_of(extensions.value.begin(), extensions.value.end(), [&](const auto &e) {
                return std::strcmp(e.extensionName.data(), name) == 0;
            })) {
            DeviceProblem("The NVIDIA driver lacks neural-rendering Vulkan extensions");
            return false;
        }
    }
    const auto f =
        physical.getFeatures2<vk::PhysicalDeviceFeatures2, vk::PhysicalDeviceVulkan12Features>();
    if (!f.get<vk::PhysicalDeviceVulkan12Features>().bufferDeviceAddress) {
        DeviceProblem("The renderer cannot enable buffer device addresses");
        return false;
    }
    enabled.insert(enabled.end(), DeviceExtensions.begin(), DeviceExtensions.end());
    requested_device = true;
    return true;
#else
    DeviceProblem("This neural-rendering adapter requires Windows");
    return false;
#endif
}

void NeuralRendering::DisableDevice() {
    requested_device = false;
    DeviceProblem("Neural rendering unavailable after Vulkan device fallback");
}

struct NeuralRendering::Impl {
    const Instance &instance;
    Scheduler &scheduler;
    BbNeural::Config config = BbNeural::Snapshot();
    const BbNrApi *api{};
#ifdef _WIN32
    HMODULE module{};
#endif
    bool initialized{}, runtime_started{}, failed{}, rebuild{}, frame_reset{};
    const bool optimized_supported = BbNeural::RuntimeSupportsOptimized();
    u64 frame_serial{};
    struct Pass {
        FgImage input, motion, depth, output, resolved;
        u32 w{}, h{}, fw{}, fh{};
        bool built{};
        u64 last_frame{};
    };
    std::array<Pass, BbNeural::MaxPasses> passes;
    FgImage hudless, composed;
    bool hudless_valid{};
    Color hudless_color{}, processed{};
    Guides last_guides{};
    vk::UniqueSampler sampler, nearest;
    struct Pipeline {
        vk::UniqueDescriptorSetLayout descriptors;
        vk::UniquePipelineLayout layout;
        vk::UniquePipeline pipeline;
    } prepare, resolve, ui;

    Impl(const Instance &i, Scheduler &s) : instance(i), scheduler(s) {}
    ~Impl() {
        if (runtime_started || hudless.image || composed.image) {
            scheduler.Finish();
        }
        if (runtime_started) {
            api->Shutdown();
        }
#ifdef _WIN32
        if (module)
            FreeLibrary(module);
#endif
    }
    void Problem(const char *text) {
        failed = true;
        BbNeural::SetStatus(text);
        Log(1, text);
    }
    bool Initialize() {
        if (initialized)
            return true;
        if (failed)
            return false;
        if (!requested_device) {
            Problem(device_reason.c_str());
            return false;
        }
#ifdef _WIN32
        std::wstring executable(32768, L'\0');
        const auto count = GetModuleFileNameW(nullptr, executable.data(), DWORD(executable.size()));
        executable.resize(count);
        const auto directory = std::filesystem::path(executable).parent_path();
        const auto path = directory / L"bbport_nr_nvngx.dll";
        if (!module) module =
            LoadLibraryExW(path.c_str(), nullptr,
                           LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!module) {
            Problem("bbport_nr_nvngx.dll is not installed beside the renderer");
            return false;
        }
        const auto get =
            reinterpret_cast<const BbNrApi *(*)()>(GetProcAddress(module, "BbNrGetApi"));
        api = get ? get() : nullptr;
        if (!api || api->abi != BBPORT_NR_BRIDGE_ABI || api->size < sizeof(BbNrApi)) {
            api = nullptr;
            Problem("The neural-rendering adapter has an incompatible interface");
            return false;
        }
        if (!api->Configure || !api->Initialize || !api->CreatePass || !api->ReadyPass ||
            !api->EvaluatePass || !api->ReleasePass || !api->Shutdown) {
            api = nullptr;
            Problem("The neural-rendering adapter table is incomplete");
            return false;
        }
        auto runtime = directory;
        if (const wchar_t *env = _wgetenv(L"BB_NR_RUNTIME"); env && *env) {
            runtime = std::filesystem::path(env);
            if (_wcsicmp(runtime.extension().c_str(), L".dll") == 0)
                runtime = runtime.parent_path();
        }
        if (!std::filesystem::is_regular_file(runtime / L"nvngx_dlssnr.dll")) {
            Problem("nvngx_dlssnr.dll is not installed (BB_NR_RUNTIME can select its folder)");
            return false;
        }
        auto data = directory / L"nr-cache";
        if (const wchar_t *env = _wgetenv(L"BB_GPU_USER_DIR"))
            data = std::filesystem::path(env) / L"nr-cache";
        std::error_code error;
        std::filesystem::create_directories(data, error);
        if (!api->Configure(runtime.c_str(), data.c_str(), Log)) {
            Problem("The neural-rendering adapter could not configure its runtime");
            return false;
        }
        // Initialization may load the model and allocate parameters before a later step fails.
        // Those partial resources still need shutdown before the Vulkan device is destroyed.
        runtime_started = true;
        if (!api->Initialize(instance.GetInstance(), instance.GetPhysicalDevice(),
                             instance.GetDevice(),
                             VULKAN_HPP_DEFAULT_DISPATCHER.vkGetInstanceProcAddr,
                             VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceProcAddr)) {
            Problem("The neural-rendering model could not initialize; see the game log");
            return false;
        }
        initialized = true;
        return true;
#else
        Problem("Neural rendering requires Windows");
        return false;
#endif
    }
    void Clear() {
        if (runtime_started && !initialized) {
            api->Shutdown();
            runtime_started = false;
        }
        if (initialized)
            for (u32 i = 0; i < passes.size(); ++i)
                api->ReleasePass(i);
        for (auto &p : passes) {
            p.input.Reset();
            p.motion.Reset();
            p.depth.Reset();
            p.output.Reset();
            p.resolved.Reset();
            p.built = false;
            p.w = p.h = p.fw = p.fh = 0;
            p.last_frame = 0;
        }
        hudless.Reset();
        composed.Reset();
        hudless_valid = false;
        processed = {};
        failed = false;
        rebuild = false;
    }
    void MakePipeline(Pipeline &p, std::span<const u32> code, u32 sampled, u32 stored,
                      u32 constants) {
        if (p.pipeline)
            return;
        const auto device = instance.GetDevice();
        std::vector<vk::DescriptorSetLayoutBinding> bindings;
        for (u32 i = 0; i < sampled + stored; ++i)
            bindings.push_back({.binding = i,
                                .descriptorType = i < sampled
                                                      ? vk::DescriptorType::eCombinedImageSampler
                                                      : vk::DescriptorType::eStorageImage,
                                .descriptorCount = 1,
                                .stageFlags = vk::ShaderStageFlagBits::eCompute});
        p.descriptors = Check(device.createDescriptorSetLayoutUnique(
            {.flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
             .bindingCount = u32(bindings.size()),
             .pBindings = bindings.data()}));
        const vk::PushConstantRange range{.stageFlags = vk::ShaderStageFlagBits::eCompute,
                                          .size = constants};
        p.layout = Check(device.createPipelineLayoutUnique(
            {.setLayoutCount = 1,
             .pSetLayouts = &*p.descriptors,
             .pushConstantRangeCount = constants ? 1u : 0u,
             .pPushConstantRanges = constants ? &range : nullptr}));
        const auto shader = CompileSPV(code, device);
        p.pipeline = Check(device.createComputePipelineUnique(
            {}, vk::ComputePipelineCreateInfo{.stage = {.stage = vk::ShaderStageFlagBits::eCompute,
                                                        .module = shader,
                                                        .pName = "main"},
                                              .layout = *p.layout}));
        device.destroyShaderModule(shader);
        if (!sampler) {
            sampler = Check(
                device.createSamplerUnique({.magFilter = vk::Filter::eLinear,
                                            .minFilter = vk::Filter::eLinear,
                                            .mipmapMode = vk::SamplerMipmapMode::eNearest,
                                            .addressModeU = vk::SamplerAddressMode::eClampToEdge,
                                            .addressModeV = vk::SamplerAddressMode::eClampToEdge,
                                            .addressModeW = vk::SamplerAddressMode::eClampToEdge}));
            nearest = Check(
                device.createSamplerUnique({.magFilter = vk::Filter::eNearest,
                                            .minFilter = vk::Filter::eNearest,
                                            .mipmapMode = vk::SamplerMipmapMode::eNearest,
                                            .addressModeU = vk::SamplerAddressMode::eClampToEdge,
                                            .addressModeV = vk::SamplerAddressMode::eClampToEdge,
                                            .addressModeW = vk::SamplerAddressMode::eClampToEdge}));
        }
    }
    void Dispatch(vk::CommandBuffer cmd, Pipeline &p, std::span<const vk::ImageView> views,
                  u32 sampled, const void *constants, u32 bytes, u32 w, u32 h,
                  bool depth_last = false) {
        std::vector<vk::DescriptorImageInfo> infos;
        for (u32 i = 0; i < views.size(); ++i)
            infos.push_back({.sampler = i < sampled
                                            ? (depth_last && i == sampled - 1 ? *nearest : *sampler)
                                            : vk::Sampler{},
                             .imageView = views[i],
                             .imageLayout = vk::ImageLayout::eGeneral});
        std::vector<vk::WriteDescriptorSet> writes;
        for (u32 i = 0; i < views.size(); ++i)
            writes.push_back({.dstBinding = i,
                              .descriptorCount = 1,
                              .descriptorType = i < sampled
                                                    ? vk::DescriptorType::eCombinedImageSampler
                                                    : vk::DescriptorType::eStorageImage,
                              .pImageInfo = &infos[i]});
        cmd.bindPipeline(vk::PipelineBindPoint::eCompute, *p.pipeline);
        cmd.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *p.layout, 0, writes);
        if (bytes)
            cmd.pushConstants(*p.layout, vk::ShaderStageFlagBits::eCompute, 0, bytes, constants);
        cmd.dispatch((w + 7) / 8, (h + 7) / 8, 1);
    }
};

NeuralRendering::NeuralRendering(const Instance &i, Scheduler &s)
    : impl(std::make_unique<Impl>(i, s)) {}
NeuralRendering::~NeuralRendering() = default;
void NeuralRendering::BeginFrame() {
    const auto next = BbNeural::Snapshot();
    const bool encoding_changed = next.resolve != impl->config.resolve ||
                                  next.white_point != impl->config.white_point;
    if (!Same(next, impl->config) || impl->rebuild) {
        if (impl->runtime_started || impl->hudless.image || impl->composed.image) {
            impl->scheduler.Finish();
        }
        // Scratch images can exist before the model finishes initialization.
        // Releasing them must not depend on successful model initialization.
        impl->Clear();
    }
    impl->config = next;
    impl->hudless_valid = false;
    impl->processed = {};
    impl->last_guides = {};
    impl->frame_reset = encoding_changed;
    ++impl->frame_serial;
    if (!next.enabled)
        BbNeural::SetStatus("Disabled");
}

void NeuralRendering::CaptureHudless(vk::CommandBuffer cmd, const Color &color) {
    auto &s = *impl;
    if (!s.config.enabled || !requested_device || s.failed || !ProtectHud(s.config) || !color.image)
        return;
    bool present = false;
    for (int i = 0; i < s.config.pass_count; ++i)
        present |= s.config.passes[i].enabled && s.config.passes[i].stage == BbNeural::Present;
    if (!present)
        return;
    if (s.hudless.image && (s.hudless.width != color.width || s.hudless.height != color.height)) {
        s.rebuild = true;
        return;
    }
    if (!s.hudless.Ensure(s.instance, color.width, color.height, vk::Format::eR16G16B16A16Sfloat))
        return;
    s.hudless.General(cmd);
    Copy(cmd, color.image, s.hudless.image, color.width, color.height);
    s.hudless_color = ImageColor(s.hudless, color.linear, color.hdr);
    s.hudless_valid = true;
}

bool NeuralRendering::Record(vk::CommandBuffer cmd, int stage, const Color &color,
                             const Guides &guides, bool reset) {
    auto &s = *impl;
    const auto &config = s.config;
    s.frame_reset |= reset;
    if (guides.motion && guides.depth)
        s.last_guides = guides;
    if (!config.enabled || s.failed || s.rebuild || !color.image)
        return false;
    std::vector<int> active;
    for (int i = 0; i < config.pass_count; ++i)
        if (config.passes[i].enabled && config.passes[i].stage == stage)
            active.push_back(i);
    if (active.empty() || !s.Initialize())
        return false;
    const u32 w =
        std::max(16u, u32(std::lround(float(color.width) * config.input_scale / 16)) * 16);
    const u32 h =
        std::max(16u, u32(std::lround(float(color.height) * config.input_scale / 16)) * 16);
    const u32 nw = config.input_scale >= 1 ? color.width : w,
              nh = config.input_scale >= 1 ? color.height : h;
    bool ready = true;
    for (int index : active) {
        auto &p = s.passes[index];
        if (p.built && (p.w != nw || p.h != nh || p.fw != color.width || p.fh != color.height)) {
            s.rebuild = true;
            BbNeural::SetStatus("Rebuilding NR at the next frame boundary");
            return false;
        }
        if (!p.built) {
            if (!p.input.Ensure(s.instance, nw, nh, vk::Format::eR16G16B16A16Sfloat) ||
                !p.output.Ensure(s.instance, nw, nh, vk::Format::eR16G16B16A16Sfloat) ||
                !p.motion.Ensure(s.instance, nw, nh, vk::Format::eR16G16Sfloat) ||
                !p.depth.Ensure(s.instance, nw, nh, vk::Format::eR32Sfloat) ||
                !p.resolved.Ensure(s.instance, color.width, color.height,
                                   vk::Format::eR16G16B16A16Sfloat)) {
                s.Problem("NR allocation failed; reduce input scale or pass count");
                return false;
            }
            const auto &tuning = config.passes[index];
            const BbNrTuning desc{nw,
                                  nh,
                                  u32(BbNeural::EffectivePreset(config.preset, s.optimized_supported)),
                                  u32(tuning.style),
                                  tuning.intensity,
                                  tuning.structure,
                                  tuning.tone,
                                  tuning.skin_structure,
                                  u32(tuning.auto_skin),
                                  0};
            if (!s.api->CreatePass(cmd, u32(index), &desc)) {
                s.Problem("NR model creation failed; the original frame is retained");
                return false;
            }
            p.w = nw;
            p.h = nh;
            p.fw = color.width;
            p.fh = color.height;
            p.built = true;
        }
        ready = ready && s.api->ReadyPass(u32(index));
    }
    if (!ready) {
        BbNeural::SetStatus("Loading neural-rendering weights");
        return false;
    }
    const bool protect = stage == BbNeural::Present && ProtectHud(config) && s.hudless_valid &&
                         s.hudless.width == color.width && s.hudless.height == color.height;
    Color working = protect ? s.hudless_color : color;
    const auto guide = guides.motion && guides.depth ? guides : s.last_guides;
    struct Constants {
        u32 full[2], work[2], guide[2];
        s32 encoding, method, protect_ui;
        float transfer, colour, max_ratio, white_point;
    };
    static_assert(sizeof(Constants) == 52);
    s.MakePipeline(s.prepare, NEURAL_PREPARE_COMP, 3, 3, sizeof(Constants));
    s.MakePipeline(s.resolve, NEURAL_RESOLVE_COMP, 4, 1, sizeof(Constants));
    for (int index : active) {
        auto &p = s.passes[index];
        Barrier(cmd, working.image);
        p.input.General(cmd);
        p.motion.General(cmd);
        p.depth.General(cmd);
        p.output.General(cmd);
        p.resolved.General(cmd);
        const s32 encoding = working.hdr ? 2 : working.linear ? 1 : 0;
        const bool has_guides = guide.motion && guide.depth && guide.width && guide.height;
        const s32 method = config.resolve == BbNeural::ResolveAuto
                               ? (config.input_scale >= 1 ? 0 : 1)
                               : config.resolve;
        const Constants pc{{color.width, color.height},
                           {nw, nh},
                           {has_guides ? guide.width : 0, has_guides ? guide.height : 0},
                           encoding,
                           method,
                           0,
                           config.transfer_strength,
                           config.colour_strength,
                           config.max_ratio,
                           config.white_point};
        const std::array prep_views{working.view,
                                    has_guides ? guide.motion : working.view,
                                    has_guides ? guide.depth : working.view,
                                    p.input.view,
                                    p.motion.view,
                                    p.depth.view};
        s.Dispatch(cmd, s.prepare, prep_views, 3, &pc, sizeof(pc), nw, nh, true);
        p.input.General(cmd);
        p.output.General(cmd);
        Barrier(cmd, p.motion.image, vk::ImageLayout::eGeneral,
                vk::ImageLayout::eShaderReadOnlyOptimal);
        Barrier(cmd, p.depth.image, vk::ImageLayout::eGeneral,
                vk::ImageLayout::eShaderReadOnlyOptimal);
        const BbNrEvaluate eval{
            BridgeImage(p.input),
            BridgeImage(p.motion),
            BridgeImage(p.depth),
            BridgeImage(p.output),
            u32(guide.depth_inverted),
            u32(s.frame_reset || !p.last_frame || p.last_frame + 1 != s.frame_serial),
            1,
            1};
        const bool ok = s.api->EvaluatePass(cmd, u32(index), &eval) != 0;
        Barrier(cmd, p.motion.image, vk::ImageLayout::eShaderReadOnlyOptimal,
                vk::ImageLayout::eGeneral);
        Barrier(cmd, p.depth.image, vk::ImageLayout::eShaderReadOnlyOptimal,
                vk::ImageLayout::eGeneral);
        if (!ok) {
            s.Problem("NR evaluation failed; the original frame is retained");
            return false;
        }
        if (!p.last_frame) {
            std::printf("Neural rendering: PASS %d active at %ux%u, stage %s, style %d\n",
                        index + 1, nw, nh, BbNeural::StageName(stage), config.passes[index].style);
            std::fflush(stdout);
        }
        p.last_frame = s.frame_serial;
        p.output.General(cmd);
        const std::array resolve_views{working.view, p.input.view, p.output.view, color.view,
                                       p.resolved.view};
        s.Dispatch(cmd, s.resolve, resolve_views, 4, &pc, sizeof(pc), color.width, color.height);
        p.resolved.General(cmd);
        working = ImageColor(p.resolved, working.linear, working.hdr);
    }
    if (stage == BbNeural::Present && protect)
        s.processed = working;
    if (protect) {
        if (s.composed.image &&
            (s.composed.width != color.width || s.composed.height != color.height)) {
            s.rebuild = true;
            return false;
        }
        if (!s.composed.Ensure(s.instance, color.width, color.height,
                               vk::Format::eR16G16B16A16Sfloat))
            return false;
        s.composed.General(cmd);
        s.MakePipeline(s.ui, NEURAL_UI_COMP, 3, 1, 0);
        const std::array ui_views{working.view, s.hudless.view, color.view, s.composed.view};
        s.Dispatch(cmd, s.ui, ui_views, 3, nullptr, 0, color.width, color.height);
        s.composed.General(cmd);
        working = ImageColor(s.composed, working.linear, working.hdr);
    }
    Copy(cmd, working.image, color.image, color.width, color.height);
    BbNeural::SetStatus("Active");
    return true;
}
NeuralRendering::Color NeuralRendering::ProcessedHudless() const { return impl->processed; }
} // namespace Vulkan
