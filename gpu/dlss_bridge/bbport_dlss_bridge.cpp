// SPDX-FileCopyrightText: Copyright 2026 IFreemz (shadps4_dlss bridge), bbport contributors, bbhost contributors
// SPDX-License-Identifier: MIT AND GPL-3.0-or-later
//
// bbport_dlss.dll: the only part of the project that uses the NVIDIA DLSS (NGX) SDK. Built with
// MSVC (the SDK's libraries are MSVC only); the port loads it at run time.
// Adapted from IFreemz/shadPS4-Bloodborne-DLSS-FSR (dlss_bridge) and bbhost ngx_bridge.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <limits>
#include <string>
#include <vector>

#include "bbport_dlss_bridge.h" // Vulkan first: the NGX headers expect its types

#include <nvsdk_ngx_helpers.h>
#include <nvsdk_ngx_vk.h>
#include <nvsdk_ngx_helpers_vk.h>
#include <nvsdk_ngx_helpers_dlssg_vk.h>

namespace {

constexpr char ProjectId[] = "42359704-c9f3-4806-9fb5-d469000fdb8a";
constexpr char EngineVersion[] = "bbport-windows";

struct NgxTransferSync {
    PFN_vkGetInstanceProcAddr gipa = nullptr;
    PFN_vkGetDeviceProcAddr gdpa = nullptr;
    PFN_vkCmdPipelineBarrier barrier = nullptr;
    PFN_vkCmdFillBuffer fill = nullptr;
    VkDevice device = VK_NULL_HANDLE;
    bool enabled = true;
} g_transfer_sync;

thread_local VkCommandBuffer g_transfer_cmd = VK_NULL_HANDLE;

struct TransferScope {
    VkCommandBuffer previous = g_transfer_cmd;
    explicit TransferScope(VkCommandBuffer cmd) {
        g_transfer_cmd = g_transfer_sync.enabled ? cmd : VK_NULL_HANDLE;
    }
    ~TransferScope() { g_transfer_cmd = previous; }
};

void VKAPI_CALL transfer_barrier(VkCommandBuffer cmd, VkPipelineStageFlags src,
                                 VkPipelineStageFlags dst, VkDependencyFlags flags,
                                 uint32_t memory_count, const VkMemoryBarrier* memory,
                                 uint32_t buffer_count, const VkBufferMemoryBarrier* buffers,
                                 uint32_t image_count, const VkImageMemoryBarrier* images) {
    if (cmd == g_transfer_cmd && image_count) {
        std::vector<VkImageMemoryBarrier> complete(images, images + image_count);
        for (auto& image : complete) {
            image.srcAccessMask |= VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
            image.dstAccessMask |= VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
        }
        g_transfer_sync.barrier(cmd, src | VK_PIPELINE_STAGE_TRANSFER_BIT,
                                dst | VK_PIPELINE_STAGE_TRANSFER_BIT, flags, memory_count, memory,
                                buffer_count, buffers, image_count, complete.data());
    } else {
        g_transfer_sync.barrier(cmd, src, dst, flags, memory_count, memory,
                                buffer_count, buffers, image_count, images);
    }
}

void VKAPI_CALL transfer_fill(VkCommandBuffer cmd, VkBuffer buffer,
                              VkDeviceSize offset, VkDeviceSize size, uint32_t data) {
    if (cmd == g_transfer_cmd) {
        VkBufferMemoryBarrier b{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        b.srcAccessMask = b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.buffer = buffer;
        b.offset = offset;
        b.size = size;
        g_transfer_sync.barrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 1, &b, 0, nullptr);
    }
    g_transfer_sync.fill(cmd, buffer, offset, size, data);
}

PFN_vkVoidFunction wrap_transfer_proc(const char* name, PFN_vkVoidFunction original) {
    if (!original || !name || !g_transfer_sync.enabled) return original;
    if (!std::strcmp(name, "vkCmdPipelineBarrier"))
        return reinterpret_cast<PFN_vkVoidFunction>(transfer_barrier);
    if (!std::strcmp(name, "vkCmdFillBuffer"))
        return reinterpret_cast<PFN_vkVoidFunction>(transfer_fill);
    return original;
}

PFN_vkVoidFunction VKAPI_CALL transfer_gdpa(VkDevice device, const char* name) {
    auto original = g_transfer_sync.gdpa(device, name);
    return device == g_transfer_sync.device ? wrap_transfer_proc(name, original) : original;
}

PFN_vkVoidFunction VKAPI_CALL transfer_gipa(VkInstance instance, const char* name) {
    auto original = g_transfer_sync.gipa(instance, name);
    if (original && name && !std::strcmp(name, "vkGetDeviceProcAddr"))
        return reinterpret_cast<PFN_vkVoidFunction>(transfer_gdpa);
    return wrap_transfer_proc(name, original);
}

struct State {
    BbDlssLogFn log{};
    std::wstring dll_directory;
    std::wstring data_directory;
    const wchar_t* dll_path{};
    NVSDK_NGX_FeatureCommonInfo common{};
    NVSDK_NGX_FeatureDiscoveryInfo discovery{};
    NVSDK_NGX_Parameter* capabilities{};
    NVSDK_NGX_Parameter* parameters{};
    NVSDK_NGX_Handle* feature{};
    BbDlssFeature feature_desc{};
    VkDevice device{};
    bool configured{};
    bool initialized{};
    bool fg_available{};
    NVSDK_NGX_Parameter* fg_parameters{};
    NVSDK_NGX_Handle* fg_feature{};
} state;

template <typename... Args>
void Log(int warning, const char* format, Args... args) {
    if (!state.log)
        return;
    std::array<char, 1024> text{};
    std::snprintf(text.data(), text.size(), format, args...);
    state.log(warning, text.data());
}

bool Check(const char* operation, NVSDK_NGX_Result result) {
    if (NVSDK_NGX_FAILED(result)) {
        Log(1, "%s failed: 0x%08x", operation, static_cast<unsigned>(result));
        return false;
    }
    return true;
}

void NVSDK_CONV NgxLog(const char* message, NVSDK_NGX_Logging_Level, NVSDK_NGX_Feature) {
    Log(0, "NGX: %s", message ? message : "");
}

int32_t Configure(const wchar_t* dll_directory, const wchar_t* data_directory, BbDlssLogFn log) {
    state.log = log;
    state.dll_directory = dll_directory ? dll_directory : L".";
    state.data_directory = data_directory ? data_directory : L".";
    state.dll_path = state.dll_directory.c_str();
    state.common.PathListInfo = {&state.dll_path, 1};
    state.common.LoggingInfo = {NgxLog, NVSDK_NGX_LOGGING_LEVEL_OFF, false};
    state.discovery.SDKVersion = NVSDK_NGX_Version_API;
    state.discovery.FeatureID = NVSDK_NGX_Feature_SuperSampling;
    state.discovery.Identifier.IdentifierType = NVSDK_NGX_Application_Identifier_Type_Project_Id;
    state.discovery.Identifier.v.ProjectDesc = {ProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM,
                                                EngineVersion};
    state.discovery.ApplicationDataPath = state.data_directory.c_str();
    state.discovery.FeatureInfo = &state.common;
    state.configured = true;
    return 1;
}

int32_t InstanceExtensions(uint32_t* count, const VkExtensionProperties** extensions) {
    VkExtensionProperties* required{};
    if (!state.configured ||
        !Check("Instance extension query",
               NVSDK_NGX_VULKAN_GetFeatureInstanceExtensionRequirements(&state.discovery, count,
                                                                       &required)))
        return 0;
    *extensions = required;
    return 1;
}

int32_t DeviceExtensions(VkInstance instance, VkPhysicalDevice physical, uint32_t* count,
                         const VkExtensionProperties** extensions) {
    if (!state.configured)
        return 0;
    NVSDK_NGX_FeatureRequirement support{};
    if (!Check("Feature requirement query",
               NVSDK_NGX_VULKAN_GetFeatureRequirements(instance, physical, &state.discovery,
                                                       &support)))
        return 0;
    if (support.FeatureSupported != NVSDK_NGX_FeatureSupportResult_Supported) {
        Log(1, "DLSS is not supported on this GPU or driver (code 0x%x)",
            static_cast<unsigned>(support.FeatureSupported));
        return 0;
    }
    VkExtensionProperties* required{};
    if (!Check("Device extension query",
               NVSDK_NGX_VULKAN_GetFeatureDeviceExtensionRequirements(
                   instance, physical, &state.discovery, count, &required)))
        return 0;
    *extensions = required;
    return 1;
}

int32_t Initialize(VkInstance instance, VkPhysicalDevice physical, VkDevice device,
                   PFN_vkGetInstanceProcAddr get_instance_proc,
                   PFN_vkGetDeviceProcAddr get_device_proc) {
    if (!state.configured || state.initialized)
        return 0;

    g_transfer_sync.gipa = get_instance_proc;
    g_transfer_sync.gdpa = get_device_proc;
    g_transfer_sync.device = device;
    g_transfer_sync.barrier = reinterpret_cast<PFN_vkCmdPipelineBarrier>(get_device_proc(device, "vkCmdPipelineBarrier"));
    g_transfer_sync.fill = reinterpret_cast<PFN_vkCmdFillBuffer>(get_device_proc(device, "vkCmdFillBuffer"));
    const char* sync_override = std::getenv("BBPORT_NGX_TRANSFER_SYNC");
    g_transfer_sync.enabled = g_transfer_sync.barrier && g_transfer_sync.fill &&
        !(sync_override && !std::strcmp(sync_override, "0"));

    if (!Check("NGX initialization",
               NVSDK_NGX_VULKAN_Init_with_ProjectID(
                   ProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM, EngineVersion,
                   state.data_directory.c_str(), instance, physical, device,
                   transfer_gipa, transfer_gdpa, &state.common)))
        return 0;
    state.initialized = true;
    state.device = device;
    if (!Check("Capability query", NVSDK_NGX_VULKAN_GetCapabilityParameters(&state.capabilities)))
        return 0;
    int available{}, needs_driver{};
    NVSDK_NGX_Parameter_GetI(state.capabilities, NVSDK_NGX_Parameter_SuperSampling_Available,
                             &available);
    NVSDK_NGX_Parameter_GetI(state.capabilities,
                             NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &needs_driver);
    if (needs_driver) {
        Log(1, "DLSS needs a newer NVIDIA driver");
        return 0;
    }
    if (!available) {
        Log(1, "DLSS Super Resolution is not available on this system");
        return 0;
    }

    int fg_avail = 0;
    NVSDK_NGX_Parameter_GetI(state.capabilities, NVSDK_NGX_Parameter_FrameGeneration_Available, &fg_avail);
    state.fg_available = (fg_avail != 0);
    Log(0, "DLSS ready (Frame Generation %s)", state.fg_available ? "available" : "unavailable");

    return 1;
}

void ReleaseFeature() {
    if (state.feature) {
        NVSDK_NGX_VULKAN_ReleaseFeature(state.feature);
        state.feature = nullptr;
    }
    if (state.parameters) {
        NVSDK_NGX_VULKAN_DestroyParameters(state.parameters);
        state.parameters = nullptr;
    }
}

int32_t CreateFeature(VkCommandBuffer command, const BbDlssFeature* desc) {
    if (!state.initialized || !state.capabilities || !desc)
        return 0;
    ReleaseFeature();
    if (!Check("Parameter allocation", NVSDK_NGX_VULKAN_AllocateParameters(&state.parameters)))
        return 0;
    for (const char* key : {NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA,
                            NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality,
                            NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced,
                            NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance,
                            NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance,
                            NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraQuality})
        NVSDK_NGX_Parameter_SetUI(state.parameters, key, desc->preset);
    static constexpr std::array qualities{
        NVSDK_NGX_PerfQuality_Value_DLAA, NVSDK_NGX_PerfQuality_Value_MaxQuality,
        NVSDK_NGX_PerfQuality_Value_Balanced, NVSDK_NGX_PerfQuality_Value_MaxPerf,
        NVSDK_NGX_PerfQuality_Value_UltraPerformance};
    NVSDK_NGX_DLSS_Create_Params settings{};
    settings.Feature.InWidth = desc->input_width;
    settings.Feature.InHeight = desc->input_height;
    settings.Feature.InTargetWidth = desc->output_width;
    settings.Feature.InTargetHeight = desc->output_height;
    settings.Feature.InPerfQualityValue = qualities[std::clamp(desc->quality, 0, 4)];
    settings.InFeatureCreateFlags =
        NVSDK_NGX_DLSS_Feature_Flags_MVLowRes |
        (desc->depth_inverted ? NVSDK_NGX_DLSS_Feature_Flags_DepthInverted : 0) |
        (desc->hdr ? NVSDK_NGX_DLSS_Feature_Flags_IsHDR | NVSDK_NGX_DLSS_Feature_Flags_AutoExposure
                   : 0);
    const auto result = NGX_VULKAN_CREATE_DLSS_EXT1(state.device, command, 1, 1, &state.feature,
                                                   state.parameters, &settings);
    if (!Check("DLSS feature creation", result) || !state.feature) {
        state.feature = nullptr;
        return 0;
    }
    state.feature_desc = *desc;
    Log(0, "DLSS %ux%u -> %ux%u, quality %d, preset %u%s", desc->input_width, desc->input_height,
        desc->output_width, desc->output_height, desc->quality, desc->preset,
        desc->hdr ? ", HDR input" : "");
    return 1;
}

NVSDK_NGX_Resource_VK Wrap(const BbDlssImage& image, bool writable) {
    VkImageSubresourceRange range = image.range;
    if (!range.aspectMask) {
        range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    }
    if (!range.levelCount) range.levelCount = 1;
    if (!range.layerCount) range.layerCount = 1;
    return NVSDK_NGX_Create_ImageView_Resource_VK(image.view, image.image, range,
                                                  image.format, image.width, image.height,
                                                  writable);
}

int32_t Evaluate(VkCommandBuffer command, const BbDlssEvaluate* evaluate) {
    if (!state.feature || !state.parameters || !evaluate)
        return 0;
    auto color = Wrap(evaluate->color, false);
    auto depth = Wrap(evaluate->depth, false);
    auto motion = Wrap(evaluate->motion, false);
    auto output = Wrap(evaluate->output, true);
    NVSDK_NGX_VK_DLSS_Eval_Params parameters{};
    parameters.Feature.pInColor = &color;
    parameters.Feature.pInOutput = &output;
    parameters.Feature.InSharpness = evaluate->sharpness;
    parameters.pInDepth = &depth;
    parameters.pInMotionVectors = &motion;
    parameters.InRenderSubrectDimensions = {state.feature_desc.input_width,
                                            state.feature_desc.input_height};
    parameters.InJitterOffsetX = evaluate->jitter_x;
    parameters.InJitterOffsetY = evaluate->jitter_y;
    parameters.InReset = evaluate->reset;
    parameters.InMVScaleX = parameters.InMVScaleY = 1.0f;
    parameters.InPreExposure = parameters.InExposureScale = 1.0f;
    parameters.InFrameTimeDeltaInMsec = evaluate->frame_ms;
    return Check("DLSS evaluation", NGX_VULKAN_EVALUATE_DLSS_EXT(command, state.feature,
                                                                state.parameters, &parameters))
               ? 1
               : 0;
}

// -----------------------------------------------------------------------------
// Frame Generation (ABI 2)
// -----------------------------------------------------------------------------

int32_t FgAvailable() {
    return (state.initialized && state.fg_available) ? 1 : 0;
}

uint32_t FgMaxGenerated() {
    if (!state.initialized || !state.fg_available || !state.capabilities)
        return 0;
    unsigned int maximum = 1;
    NVSDK_NGX_Parameter_GetUI(state.capabilities, NVSDK_NGX_DLSSG_Parameter_MultiFrameCountMax, &maximum);
    return std::max(1u, maximum);
}

int32_t FgExtensions(VkInstance instance, VkPhysicalDevice physical, uint32_t* count,
                     const VkExtensionProperties** extensions) {
    if (!state.configured)
        return 0;
    NVSDK_NGX_FeatureDiscoveryInfo discovery = state.discovery;
    discovery.FeatureID = NVSDK_NGX_Feature_FrameGeneration;
    VkExtensionProperties* required = nullptr;
    const auto r = physical
        ? NVSDK_NGX_VULKAN_GetFeatureDeviceExtensionRequirements(instance, physical, &discovery, count, &required)
        : NVSDK_NGX_VULKAN_GetFeatureInstanceExtensionRequirements(&discovery, count, &required);
    if (NVSDK_NGX_FAILED(r)) return 0;
    *extensions = required;
    return 1;
}

void ReleaseFgFeature() {
    if (state.fg_feature) {
        NVSDK_NGX_VULKAN_ReleaseFeature(state.fg_feature);
        state.fg_feature = nullptr;
    }
    if (state.fg_parameters) {
        NVSDK_NGX_VULKAN_DestroyParameters(state.fg_parameters);
        state.fg_parameters = nullptr;
    }
}

int32_t CreateFgFeature(VkCommandBuffer command, uint32_t width, uint32_t height, VkFormat format) {
    if (!state.initialized || !state.fg_available)
        return 0;
    ReleaseFgFeature();
    if (!Check("FG parameter allocation", NVSDK_NGX_VULKAN_AllocateParameters(&state.fg_parameters)))
        return 0;
    NVSDK_NGX_DLSSG_Create_Params params{};
    params.Width = width;
    params.Height = height;
    params.NativeBackbufferFormat = format;
    const auto r = NGX_VK_CREATE_DLSSG(command, 1, 1, &state.fg_feature, state.fg_parameters, &params);
    if (!Check("FG feature creation", r) || !state.fg_feature) {
        state.fg_feature = nullptr;
        return 0;
    }
    Log(0, "DLSS FG feature created: %ux%u", width, height);
    return 1;
}

int32_t EvaluateFg(VkCommandBuffer command, const BbDlssFgEvaluate* evaluate,
                   uint32_t multi_frame_count, uint32_t multi_frame_index, uint64_t render_frame) {
    if (!state.fg_feature || !state.fg_parameters || !evaluate)
        return 0;
    if (!multi_frame_count || multi_frame_count > FgMaxGenerated() ||
        !multi_frame_index || multi_frame_index > multi_frame_count)
        return 0;

    auto color = Wrap(evaluate->color, false);
    auto depth = Wrap(evaluate->depth, false);
    auto motion = Wrap(evaluate->motion, false);
    auto output = Wrap(evaluate->output, true);
    auto real = evaluate->real.image ? Wrap(evaluate->real, true) : NVSDK_NGX_Resource_VK{};
    auto hudless = evaluate->hudless.image ? Wrap(evaluate->hudless, false) : NVSDK_NGX_Resource_VK{};
    auto ui = evaluate->ui.image ? Wrap(evaluate->ui, false) : NVSDK_NGX_Resource_VK{};

    NVSDK_NGX_Resource_VK disable{};
    if (evaluate->disable_interpolation) {
        disable = NVSDK_NGX_Create_Buffer_Resource_VK(evaluate->disable_interpolation, 4, true);
    }

    NVSDK_NGX_VK_DLSSG_Eval_Params images{};
    images.pBackbuffer = &color;
    images.pDepth = &depth;
    images.pMVecs = &motion;
    images.pOutputInterpFrame = &output;
    images.pOutputRealFrame = evaluate->real.image ? &real : nullptr;
    images.pOutputDisableInterpolation = evaluate->disable_interpolation ? &disable : nullptr;
    images.pHudless = evaluate->hudless.image ? &hudless : nullptr;
    images.pUI = evaluate->ui.image ? &ui : nullptr;

    NVSDK_NGX_DLSSG_Opt_Eval_Params p{};
    std::memcpy(p.cameraViewToClip, evaluate->camera.view_to_clip, 64);
    std::memcpy(p.clipToCameraView, evaluate->camera.clip_to_view, 64);
    std::memcpy(p.clipToPrevClip, evaluate->camera.clip_to_previous, 64);
    std::memcpy(p.prevClipToClip, evaluate->camera.previous_to_clip, 64);
    for (int i = 0; i < 4; ++i) p.clipToLensClip[i][i] = 1.0f;
    std::memcpy(p.cameraPos, evaluate->camera.position, 12);
    std::memcpy(p.cameraUp, evaluate->camera.up, 12);
    std::memcpy(p.cameraRight, evaluate->camera.right, 12);
    std::memcpy(p.cameraFwd, evaluate->camera.forward, 12);
    p.jitterOffset[0] = evaluate->camera.jitter[0];
    p.jitterOffset[1] = evaluate->camera.jitter[1];
    p.mvecScale[0] = p.mvecScale[1] = 1.0f;
    p.cameraNear = evaluate->camera.near_plane;
    p.cameraFar = evaluate->camera.far_plane;
    p.cameraFOV = evaluate->camera.vertical_fov;
    const float projection_aspect = evaluate->camera.view_to_clip[5] / evaluate->camera.view_to_clip[0];
    p.cameraAspectRatio = std::isfinite(projection_aspect) && projection_aspect > 0.0f
        ? projection_aspect : float(evaluate->color.width) / std::max(1u,evaluate->color.height);
    p.cameraMotionIncluded = true;
    p.reset = evaluate->reset != 0;
    p.depthInverted = false;
    p.colorBuffersHDR = false;
    p.motionVectorsInvalidValue = std::numeric_limits<float>::max();
    p.multiFrameCount = multi_frame_count;
    p.multiFrameIndex = multi_frame_index;

    if (render_frame) {
        NVSDK_NGX_Parameter_SetULL(state.fg_parameters, NVSDK_NGX_DLSSG_Parameter_BackbufferFrameID, render_frame);
    }

    TransferScope transfer_scope(command);
    return Check("FG evaluation", NGX_VK_EVALUATE_DLSSG(command, state.fg_feature, state.fg_parameters, &images, &p)) ? 1 : 0;
}

void Shutdown() {
    ReleaseFgFeature();
    ReleaseFeature();
    state.fg_available = false;
    if (state.capabilities) {
        NVSDK_NGX_VULKAN_DestroyParameters(state.capabilities);
        state.capabilities = nullptr;
    }
    if (state.initialized) {
        NVSDK_NGX_VULKAN_Shutdown1(state.device);
        state.initialized = false;
        state.device = VK_NULL_HANDLE;
    }
    g_transfer_sync = {};
    g_transfer_cmd = VK_NULL_HANDLE;
}

const BbDlssApi api{
    BBPORT_DLSS_BRIDGE_ABI,
    Configure,
    InstanceExtensions,
    DeviceExtensions,
    Initialize,
    CreateFeature,
    Evaluate,
    ReleaseFeature,
    Shutdown,
    FgAvailable,
    FgMaxGenerated,
    FgExtensions,
    CreateFgFeature,
    EvaluateFg,
    ReleaseFgFeature,
};

} // namespace

extern "C" __declspec(dllexport) const BbDlssApi* BbDlssGetApi() {
    return &api;
}
