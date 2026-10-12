// SPDX-FileCopyrightText: Copyright 2026 IFreemz (shadps4_dlss bridge), bbport contributors
// SPDX-License-Identifier: MIT
//
// bbport_dlss.dll: the only part of the project that uses the NVIDIA DLSS (NGX) SDK. Built with
// MSVC (the SDK's libraries are MSVC only); the port loads it at run time.
// Adapted from IFreemz/shadPS4-Bloodborne-DLSS-FSR (dlss_bridge).

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
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
    NVSDK_NGX_Handle *generation{};
    NVSDK_NGX_Parameter *generation_parameters{};
    std::vector<VkExtensionProperties> instance_extensions, device_extensions;
    bool generation_supported{};
    BbDlssFeature feature_desc{};
    VkDevice device{};
    bool configured{};
    bool initialized{};
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
    state.instance_extensions.assign(required, required + *count);
    auto discovery = state.discovery;
    discovery.FeatureID = NVSDK_NGX_Feature_FrameGeneration;
    uint32_t fg_count{};
    if (NVSDK_NGX_SUCCEED(NVSDK_NGX_VULKAN_GetFeatureInstanceExtensionRequirements(
            &discovery, &fg_count, &required))) {
        for (uint32_t i = 0; i < fg_count; ++i)
            if (std::none_of(state.instance_extensions.begin(), state.instance_extensions.end(),
                             [&](const auto &x) {
                                 return !std::strcmp(x.extensionName, required[i].extensionName);
                             }))
                state.instance_extensions.push_back(required[i]);
    }
    *count = uint32_t(state.instance_extensions.size());
    *extensions = state.instance_extensions.data();
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
    state.device_extensions.assign(required, required + *count);
    auto discovery = state.discovery;
    discovery.FeatureID = NVSDK_NGX_Feature_FrameGeneration;
    support = {};
    state.generation_supported =
        NVSDK_NGX_SUCCEED(
            NVSDK_NGX_VULKAN_GetFeatureRequirements(instance, physical, &discovery, &support)) &&
        support.FeatureSupported == NVSDK_NGX_FeatureSupportResult_Supported;
    if (state.generation_supported) {
        uint32_t fg_count{};
        if (NVSDK_NGX_SUCCEED(NVSDK_NGX_VULKAN_GetFeatureDeviceExtensionRequirements(
                instance, physical, &discovery, &fg_count, &required))) {
            for (uint32_t i = 0; i < fg_count; ++i)
                if (std::none_of(state.device_extensions.begin(), state.device_extensions.end(),
                                 [&](const auto &x) {
                                     return !std::strcmp(x.extensionName,
                                                         required[i].extensionName);
                                 }))
                    state.device_extensions.push_back(required[i]);
        } else
            state.generation_supported = false;
    }
    *count = uint32_t(state.device_extensions.size());
    *extensions = state.device_extensions.data();
    return 1;
}

int32_t Initialize(VkInstance instance, VkPhysicalDevice physical, VkDevice device,
                   PFN_vkGetInstanceProcAddr get_instance_proc,
                   PFN_vkGetDeviceProcAddr get_device_proc) {
    if (!state.configured || state.initialized)
        return 0;
    if (!Check("NGX initialization",
               NVSDK_NGX_VULKAN_Init_with_ProjectID(
                   ProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM, EngineVersion,
                   state.data_directory.c_str(), instance, physical, device, get_instance_proc,
                   get_device_proc, &state.common)))
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
    available = needs_driver = 0;
    NVSDK_NGX_Parameter_GetI(state.capabilities, NVSDK_NGX_Parameter_FrameGeneration_Available,
                             &available);
    NVSDK_NGX_Parameter_GetI(state.capabilities,
                             NVSDK_NGX_Parameter_FrameGeneration_NeedsUpdatedDriver, &needs_driver);
    state.generation_supported = state.generation_supported && available && !needs_driver;
    Log(0, "Frame Generation %s",
        state.generation_supported ? "available"
                                   : "unavailable (RTX 40+, driver and nvngx_dlssg.dll required)");
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
    return NVSDK_NGX_Create_ImageView_Resource_VK(image.view, image.image, image.range,
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

void ReleaseFrameGeneration() {
    if (state.generation)
        NVSDK_NGX_VULKAN_ReleaseFeature(state.generation);
    if (state.generation_parameters)
        NVSDK_NGX_VULKAN_DestroyParameters(state.generation_parameters);
    state.generation = nullptr;
    state.generation_parameters = nullptr;
}

int32_t FrameGenerationAvailable() { return state.initialized && state.generation_supported; }

int32_t CreateFrameGeneration(VkCommandBuffer command, uint32_t width, uint32_t height,
                              VkFormat format) {
    if (!FrameGenerationAvailable())
        return 0;
    ReleaseFrameGeneration();
    if (!Check("FG parameter allocation",
               NVSDK_NGX_VULKAN_AllocateParameters(&state.generation_parameters)))
        return 0;
    NVSDK_NGX_DLSSG_Create_Params desc{};
    desc.Width = width;
    desc.Height = height;
    desc.NativeBackbufferFormat = format;
    return Check("FG creation", NGX_VK_CREATE_DLSSG(command, 1, 1, &state.generation,
                                                    state.generation_parameters, &desc));
}

int32_t GenerateFrame(VkCommandBuffer command, const BbDlssGenerate *frame) {
    if (!state.generation || !frame)
        return 0;
    auto color = Wrap(frame->color, false), hudless = Wrap(frame->hudless, false),
         ui = Wrap(frame->ui, false);
    auto depth = Wrap(frame->depth, false), motion = Wrap(frame->motion, false),
         output = Wrap(frame->output, true);
    auto disable = NVSDK_NGX_Create_Buffer_Resource_VK(frame->disable_interpolation, 4, true);
    NVSDK_NGX_VK_DLSSG_Eval_Params images{};
    images.pBackbuffer = &color;
    images.pHudless = &hudless;
    images.pUI = &ui;
    images.pDepth = &depth;
    images.pMVecs = &motion;
    images.pOutputInterpFrame = &output;
    images.pOutputDisableInterpolation = &disable;
    NVSDK_NGX_DLSSG_Opt_Eval_Params params{};
    std::memcpy(params.cameraViewToClip, frame->camera.view_to_clip, 64);
    std::memcpy(params.clipToCameraView, frame->camera.clip_to_view, 64);
    std::memcpy(params.clipToPrevClip, frame->camera.clip_to_previous, 64);
    std::memcpy(params.prevClipToClip, frame->camera.previous_to_clip, 64);
    for (int i = 0; i < 4; ++i)
        params.clipToLensClip[i][i] = 1;
    std::memcpy(params.cameraPos, frame->camera.position, 12);
    std::memcpy(params.cameraUp, frame->camera.up, 12);
    std::memcpy(params.cameraRight, frame->camera.right, 12);
    std::memcpy(params.cameraFwd, frame->camera.forward, 12);
    params.jitterOffset[0] = frame->camera.jitter[0];
    params.jitterOffset[1] = frame->camera.jitter[1];
    params.mvecScale[0] = params.mvecScale[1] =
        1; // NGX takes pixels (unlike Streamline's normalized constants).
    params.cameraNear = frame->camera.near_plane;
    params.cameraFar = frame->camera.far_plane;
    params.cameraFOV = frame->camera.vertical_fov;
    params.cameraAspectRatio = float(frame->motion.width) / frame->motion.height;
    params.cameraMotionIncluded = true;
    params.reset = frame->reset != 0;
    params.motionVectorsInvalidValue = std::numeric_limits<float>::max();
    params.depthInverted = false;
    params.colorBuffersHDR = false;
    return Check("FG evaluation",
                 NGX_VK_EVALUATE_DLSSG(command, state.generation, state.generation_parameters,
                                       &images, &params));
}

void Shutdown() {
    ReleaseFrameGeneration();
    ReleaseFeature();
    if (state.capabilities) {
        NVSDK_NGX_VULKAN_DestroyParameters(state.capabilities);
        state.capabilities = nullptr;
    }
    if (state.initialized) {
        NVSDK_NGX_VULKAN_Shutdown1(state.device);
        state.initialized = false;
        state.device = VK_NULL_HANDLE;
    }
}

const BbDlssApi api{BBPORT_DLSS_BRIDGE_ABI,
                    Configure,
                    InstanceExtensions,
                    DeviceExtensions,
                    Initialize,
                    CreateFeature,
                    Evaluate,
                    ReleaseFeature,
                    Shutdown,
                    FrameGenerationAvailable,
                    CreateFrameGeneration,
                    GenerateFrame,
                    ReleaseFrameGeneration};

} // namespace

extern "C" __declspec(dllexport) const BbDlssApi* BbDlssGetApi() {
    return &api;
}
