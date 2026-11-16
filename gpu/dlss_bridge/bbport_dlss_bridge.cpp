// SPDX-FileCopyrightText: Copyright 2026 IFreemz (shadps4_dlss bridge), bbport contributors
// SPDX-License-Identifier: MIT
//
// bbport_dlss.dll: the only part of the project that uses the NVIDIA DLSS (NGX) SDK. Built with
// MSVC (the SDK's libraries are MSVC only); the port loads it at run time.
// Adapted from IFreemz/shadPS4-Bloodborne-DLSS-FSR (dlss_bridge).

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include "bbport_dlss_bridge.h" // Vulkan first: the NGX headers expect its types

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>

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
    NVSDK_NGX_FeatureDiscoveryInfo discovery_fg{}; // Frame Generation
    NVSDK_NGX_Parameter* capabilities{};
    NVSDK_NGX_Parameter* parameters{};
    NVSDK_NGX_Handle* feature{};
    BbDlssFeature feature_desc{};
    // Frame Generation: its own parameter map (NVIDIA's advice with two features).
    NVSDK_NGX_Parameter* fg_parameters{};
    NVSDK_NGX_Handle* fg_feature{};
    BbDlssgFeature fg_desc{};
    bool fg_device_supported{};
    int32_t fg_max_count{};
    // Extensions of both features, without duplicates.
    std::vector<VkExtensionProperties> instance_extensions, device_extensions;
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
    state.discovery_fg = state.discovery;
    state.discovery_fg.FeatureID = NVSDK_NGX_Feature_FrameGeneration;
    state.configured = true;
    return 1;
}

void Merge(std::vector<VkExtensionProperties>& into, const VkExtensionProperties* add,
           uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) {
        const auto same = std::find_if(into.begin(), into.end(), [&](const auto& have) {
            return std::strcmp(have.extensionName, add[i].extensionName) == 0;
        });
        if (same == into.end()) {
            into.push_back(add[i]);
        } else {
            same->specVersion = std::max(same->specVersion, add[i].specVersion);
        }
    }
}

int32_t InstanceExtensions(uint32_t* count, const VkExtensionProperties** extensions) {
    VkExtensionProperties* required{};
    uint32_t required_count{};
    if (!state.configured ||
        !Check("Instance extension query",
               NVSDK_NGX_VULKAN_GetFeatureInstanceExtensionRequirements(
                   &state.discovery, &required_count, &required)))
        return 0;
    state.instance_extensions.clear();
    Merge(state.instance_extensions, required, required_count);
    // Frame Generation's own needs; without them only DLSS is offered.
    if (NVSDK_NGX_SUCCEED(NVSDK_NGX_VULKAN_GetFeatureInstanceExtensionRequirements(
            &state.discovery_fg, &required_count, &required)))
        Merge(state.instance_extensions, required, required_count);
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
    uint32_t required_count{};
    if (!Check("Device extension query",
               NVSDK_NGX_VULKAN_GetFeatureDeviceExtensionRequirements(
                   instance, physical, &state.discovery, &required_count, &required)))
        return 0;
    state.device_extensions.clear();
    Merge(state.device_extensions, required, required_count);
    NVSDK_NGX_FeatureRequirement fg_support{};
    state.fg_device_supported =
        NVSDK_NGX_SUCCEED(NVSDK_NGX_VULKAN_GetFeatureRequirements(instance, physical,
                                                                  &state.discovery_fg,
                                                                  &fg_support)) &&
        fg_support.FeatureSupported == NVSDK_NGX_FeatureSupportResult_Supported &&
        NVSDK_NGX_SUCCEED(NVSDK_NGX_VULKAN_GetFeatureDeviceExtensionRequirements(
            instance, physical, &state.discovery_fg, &required_count, &required));
    if (state.fg_device_supported) {
        Merge(state.device_extensions, required, required_count);
    } else {
        Log(0, "Frame Generation is not supported on this GPU or driver (code 0x%x)",
            static_cast<unsigned>(fg_support.FeatureSupported));
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
    int fg_available{}, fg_needs_driver{};
    unsigned fg_max{};
    NVSDK_NGX_Parameter_GetI(state.capabilities, NVSDK_NGX_Parameter_FrameGeneration_Available,
                             &fg_available);
    NVSDK_NGX_Parameter_GetI(state.capabilities,
                             NVSDK_NGX_Parameter_FrameGeneration_NeedsUpdatedDriver,
                             &fg_needs_driver);
    if (NVSDK_NGX_FAILED(NVSDK_NGX_Parameter_GetUI(
            state.capabilities, NVSDK_NGX_DLSSG_Parameter_MultiFrameCountMax, &fg_max)))
        fg_max = 1;
    if (state.fg_device_supported && fg_available) {
        state.fg_max_count = static_cast<int32_t>(std::clamp(fg_max, 1u, 3u));
        Log(0, "Frame Generation available (up to %dx)", state.fg_max_count + 1);
    } else if (fg_needs_driver) {
        Log(1, "Frame Generation needs a newer NVIDIA driver");
    }
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

int32_t FrameGenMaxCount() {
    return state.initialized ? state.fg_max_count : 0;
}

void FrameGenRelease() {
    if (state.fg_feature) {
        NVSDK_NGX_VULKAN_ReleaseFeature(state.fg_feature);
        state.fg_feature = nullptr;
    }
    if (state.fg_parameters) {
        NVSDK_NGX_VULKAN_DestroyParameters(state.fg_parameters);
        state.fg_parameters = nullptr;
    }
}

int32_t FrameGenCreate(VkCommandBuffer command, const BbDlssgFeature* desc) {
    if (!state.initialized || state.fg_max_count < 1 || !desc)
        return 0;
    FrameGenRelease();
    if (!Check("Frame Generation parameter allocation",
               NVSDK_NGX_VULKAN_AllocateParameters(&state.fg_parameters)))
        return 0;
    // Depth and motion vectors are smaller than the backbuffer (the game's own size).
    NVSDK_NGX_Parameter_SetUI(state.fg_parameters, NVSDK_NGX_DLSSG_Parameter_Width, desc->width);
    NVSDK_NGX_Parameter_SetUI(state.fg_parameters, NVSDK_NGX_DLSSG_Parameter_Height, desc->height);
    NVSDK_NGX_Parameter_SetUI(state.fg_parameters, NVSDK_NGX_DLSSG_Parameter_InternalWidth,
                              desc->render_width);
    NVSDK_NGX_Parameter_SetUI(state.fg_parameters, NVSDK_NGX_DLSSG_Parameter_InternalHeight,
                              desc->render_height);
    NVSDK_NGX_Parameter_SetUI(state.fg_parameters, NVSDK_NGX_DLSSG_Parameter_DynamicResolution, 0);
    NVSDK_NGX_DLSSG_Create_Params settings{};
    settings.Width = desc->width;
    settings.Height = desc->height;
    settings.NativeBackbufferFormat = static_cast<unsigned>(desc->format);
    settings.RenderWidth = desc->render_width;
    settings.RenderHeight = desc->render_height;
    const auto result = NGX_VK_CREATE_DLSSG(command, 1, 1, &state.fg_feature,
                                            state.fg_parameters, &settings);
    if (!Check("Frame Generation feature creation", result) || !state.fg_feature) {
        state.fg_feature = nullptr;
        return 0;
    }
    state.fg_desc = *desc;
    Log(0, "Frame Generation %ux%u (depth and motion %ux%u)", desc->width, desc->height,
        desc->render_width, desc->render_height);
    return 1;
}

// Row-major, row vectors (D3D style: clip = view * m), depth 0 at the near plane.
void Perspective(float (&m)[4][4], float fov, float aspect, float near_z, float far_z) {
    std::memset(m, 0, sizeof(m));
    const float y = 1.0f / std::tan(fov * 0.5f);
    m[0][0] = y / aspect;
    m[1][1] = y;
    m[2][2] = far_z / (far_z - near_z);
    m[2][3] = 1.0f;
    m[3][2] = -near_z * far_z / (far_z - near_z);
}

void Inverse(float (&out)[4][4], const float (&m)[4][4]) {
    // Of the perspective matrix above.
    std::memset(out, 0, sizeof(out));
    out[0][0] = 1.0f / m[0][0];
    out[1][1] = 1.0f / m[1][1];
    out[2][3] = 1.0f / m[3][2];
    out[3][2] = 1.0f;
    out[3][3] = -m[2][2] / m[3][2];
}

void Identity(float (&m)[4][4]) {
    std::memset(m, 0, sizeof(m));
    for (int i = 0; i < 4; ++i)
        m[i][i] = 1.0f;
}

int32_t FrameGenEvaluate(VkCommandBuffer command, const BbDlssgEvaluate* evaluate) {
    if (!state.fg_feature || !state.fg_parameters || !evaluate)
        return 0;
    auto backbuffer = Wrap(evaluate->backbuffer, false);
    auto depth = Wrap(evaluate->depth, false);
    auto motion = Wrap(evaluate->motion, false);
    auto output = Wrap(evaluate->output, true);
    NVSDK_NGX_VK_DLSSG_Eval_Params resources{};
    resources.pBackbuffer = &backbuffer;
    resources.pDepth = &depth;
    resources.pMVecs = &motion;
    resources.pOutputInterpFrame = &output;
    NVSDK_NGX_Resource_VK hudless{};
    if (evaluate->hudless.image != VK_NULL_HANDLE) {
        hudless = Wrap(evaluate->hudless, false);
        resources.pHudless = &hudless;
    }
    NVSDK_NGX_DLSSG_Opt_Eval_Params options{};
    options.multiFrameCount = evaluate->multi_frame_count;
    options.multiFrameIndex = evaluate->multi_frame_index;
    const float aspect = float(state.fg_desc.render_width) / float(state.fg_desc.render_height);
    Identity(options.clipToLensClip);
    if (evaluate->has_matrices) {
        // The game's cameras: DLSS separates the camera's motion from the objects' with them
        // (the vectors hold both). A still camera here contradicted the vectors on every
        // camera turn and smeared the generated frames.
        std::memcpy(options.cameraViewToClip, evaluate->view_to_clip,
                    sizeof(options.cameraViewToClip));
        std::memcpy(options.clipToPrevClip, evaluate->clip_to_prev_clip,
                    sizeof(options.clipToPrevClip));
        std::memcpy(options.prevClipToClip, evaluate->prev_clip_to_clip,
                    sizeof(options.prevClipToClip));
    } else {
        // Unknown camera: a plausible projection that stood still.
        Perspective(options.cameraViewToClip, evaluate->camera_fov, aspect,
                    evaluate->camera_near, evaluate->camera_far);
        Identity(options.clipToPrevClip);
        Identity(options.prevClipToClip);
    }
    Inverse(options.clipToCameraView, options.cameraViewToClip);
    // bbport: units. nvsdk_ngx_params_dlssg.h asks for the jitter in clip space and for scales
    // that bring the vectors into [-1, 1] of the screen; this bridge gets both in render
    // pixels (bbport_dlss_bridge.h). The renderer shifts each vertex by jitter / (render size
    // / 2) in Vulkan's y-down NDC (SetProjectionJitter), so in the y-up clip space of the
    // matrices above the shift is 2 * jitter / size with y negated. Fed as pixels, half a
    // pixel of jitter read as a quarter of the screen and a vector of one pixel as a whole one.
    const float render_w = float(state.fg_desc.render_width);
    const float render_h = float(state.fg_desc.render_height);
    options.jitterOffset[0] = 2.0f * evaluate->jitter_x / render_w;
    options.jitterOffset[1] = -2.0f * evaluate->jitter_y / render_h;
    options.mvecScale[0] = 1.0f / render_w;
    options.mvecScale[1] = 1.0f / render_h;
    options.cameraUp[1] = 1.0f;
    options.cameraRight[0] = 1.0f;
    options.cameraFwd[2] = 1.0f;
    options.cameraNear = evaluate->camera_near;
    options.cameraFar = evaluate->camera_far;
    options.cameraFOV = evaluate->camera_fov;
    options.cameraAspectRatio = aspect;
    options.depthInverted = evaluate->depth_inverted != 0;
    options.cameraMotionIncluded = true;
    // No vector is invalid (0, the zero-initialised value, would mark every still pixel).
    options.motionVectorsInvalidValue = std::numeric_limits<float>::max();
    options.reset = evaluate->reset != 0 && evaluate->multi_frame_index == 1;
    return Check("Frame Generation evaluation",
                 NGX_VK_EVALUATE_DLSSG(command, state.fg_feature, state.fg_parameters,
                                       &resources, &options))
               ? 1
               : 0;
}

void Shutdown() {
    FrameGenRelease();
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

// ---- ABI 3: DLSS Super Resolution on Direct3D 12 -------------------------------------------
//
// The renderer copies colour, motion and depth into resources shared with this D3D12 device,
// signals input_fence, and waits for output_fence before it reads the output. DLSS runs here
// through NVIDIA's D3D12 NGX entry points, on an ordinary D3D12 command list.

template <class T> void SafeRelease(T*& p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

void Transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource,
                D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    if (before == after)
        return;
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = resource;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    list->ResourceBarrier(1, &b);
}

struct D3D12State {
    IDXGIFactory4* factory{};
    IDXGIAdapter1* adapter{};
    ID3D12Device* device{};
    ID3D12CommandQueue* queue{};
    struct Slot {
        ID3D12CommandAllocator* allocator{};
        ID3D12GraphicsCommandList* list{};
        uint64_t completed_at{};
    };
    std::array<Slot, 3> slots{};
    unsigned slot{};
    ID3D12Fence* fence{};       // output
    ID3D12Fence* input_fence{}; // signalled by Vulkan
    HANDLE event{}, fence_handle{}, input_handle{};
    uint64_t fence_value{};
    LUID luid{};
    bool ngx{};
    NVSDK_NGX_Parameter* capabilities{};
    // The resources of one configuration.
    BbDlss12Config config{};
    ID3D12Resource* color{};
    ID3D12Resource* motion{};
    ID3D12Resource* output{};
    ID3D12Resource* depth_buffer{};
    ID3D12Resource* depth{}; // private: DLSS reads a texture
    HANDLE color_handle{}, motion_handle{}, output_handle{}, depth_handle{};
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT depth_footprint{};
    NVSDK_NGX_Parameter* parameters{};
    NVSDK_NGX_Handle* feature{};
    bool created{};
} dx;

bool HrCheck(const char* operation, HRESULT hr) {
    if (FAILED(hr)) {
        Log(1, "D3D12: %s failed: 0x%08lx", operation, static_cast<unsigned long>(hr));
        return false;
    }
    return true;
}

bool WaitFence(uint64_t value) {
    if (!value || !dx.fence || dx.fence->GetCompletedValue() >= value)
        return true;
    if (!HrCheck("fence event", dx.fence->SetEventOnCompletion(value, dx.event)))
        return false;
    if (WaitForSingleObject(dx.event, 10000) != WAIT_OBJECT_0) {
        Log(1, "D3D12: GPU wait timed out (device status 0x%08lx)",
            static_cast<unsigned long>(dx.device->GetDeviceRemovedReason()));
        return false;
    }
    return true;
}

void ReleaseResources() {
    WaitFence(dx.fence_value);
    if (dx.feature) {
        NVSDK_NGX_D3D12_ReleaseFeature(dx.feature);
        dx.feature = nullptr;
    }
    if (dx.parameters) {
        NVSDK_NGX_D3D12_DestroyParameters(dx.parameters);
        dx.parameters = nullptr;
    }
    for (HANDLE* h : {&dx.color_handle, &dx.motion_handle, &dx.output_handle, &dx.depth_handle}) {
        if (*h)
            CloseHandle(*h);
        *h = nullptr;
    }
    SafeRelease(dx.color);
    SafeRelease(dx.motion);
    SafeRelease(dx.output);
    SafeRelease(dx.depth_buffer);
    SafeRelease(dx.depth);
    dx.created = false;
}

void D3D12Release() {
    ReleaseResources();
}

void D3D12Shutdown() {
    ReleaseResources();
    if (dx.capabilities) {
        NVSDK_NGX_D3D12_DestroyParameters(dx.capabilities);
        dx.capabilities = nullptr;
    }
    if (dx.ngx) {
        NVSDK_NGX_D3D12_Shutdown1(dx.device);
        dx.ngx = false;
    }
    for (auto& s : dx.slots) {
        SafeRelease(s.list);
        SafeRelease(s.allocator);
    }
    for (HANDLE* h : {&dx.fence_handle, &dx.input_handle, &dx.event}) {
        if (*h)
            CloseHandle(*h);
        *h = nullptr;
    }
    SafeRelease(dx.input_fence);
    SafeRelease(dx.fence);
    SafeRelease(dx.queue);
    SafeRelease(dx.device);
    SafeRelease(dx.adapter);
    SafeRelease(dx.factory);
    dx.fence_value = 0;
}

bool InitializeDevice(const uint8_t (&luid_bytes)[8]) {
    LUID luid{};
    std::memcpy(&luid, luid_bytes, sizeof(luid));
    if (dx.device && luid.LowPart == dx.luid.LowPart && luid.HighPart == dx.luid.HighPart)
        return true;
    D3D12Shutdown();
    dx.luid = luid;
    if (!HrCheck("DXGI factory", CreateDXGIFactory1(IID_PPV_ARGS(&dx.factory))) ||
        !HrCheck("adapter of the Vulkan device",
                 dx.factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&dx.adapter))) ||
        !HrCheck("device", D3D12CreateDevice(dx.adapter, D3D_FEATURE_LEVEL_12_0,
                                             IID_PPV_ARGS(&dx.device))))
        return false;
    DXGI_ADAPTER_DESC1 adapter_desc{};
    dx.adapter->GetDesc1(&adapter_desc);
    Log(0, "D3D12: device on %ls", adapter_desc.Description);
    D3D12_COMMAND_QUEUE_DESC q{};
    q.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (!HrCheck("queue", dx.device->CreateCommandQueue(&q, IID_PPV_ARGS(&dx.queue))))
        return false;
    for (unsigned i = 0; i < dx.slots.size(); ++i) {
        auto& s = dx.slots[i];
        if (!HrCheck("command allocator",
                     dx.device->CreateCommandAllocator(q.Type, IID_PPV_ARGS(&s.allocator))) ||
            !HrCheck("command list", dx.device->CreateCommandList(0, q.Type, s.allocator, nullptr,
                                                                  IID_PPV_ARGS(&s.list))))
            return false;
        s.list->Close();
    }
    if (!HrCheck("output fence",
                 dx.device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&dx.fence))) ||
        !HrCheck("input fence", dx.device->CreateFence(0, D3D12_FENCE_FLAG_SHARED,
                                                       IID_PPV_ARGS(&dx.input_fence))) ||
        !HrCheck("output fence handle", dx.device->CreateSharedHandle(
                                            dx.fence, nullptr, GENERIC_ALL, nullptr,
                                            &dx.fence_handle)) ||
        !HrCheck("input fence handle", dx.device->CreateSharedHandle(
                                           dx.input_fence, nullptr, GENERIC_ALL, nullptr,
                                           &dx.input_handle)))
        return false;
    dx.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!dx.event)
        return false;
    if (!Check("NGX D3D12 initialization",
               NVSDK_NGX_D3D12_Init_with_ProjectID(ProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM,
                                                   EngineVersion, state.data_directory.c_str(),
                                                   dx.device, &state.common)))
        return false;
    dx.ngx = true;
    if (!Check("NGX D3D12 capability query",
               NVSDK_NGX_D3D12_GetCapabilityParameters(&dx.capabilities)))
        return false;
    int available{};
    NVSDK_NGX_Parameter_GetI(dx.capabilities, NVSDK_NGX_Parameter_SuperSampling_Available,
                             &available);
    if (!available) {
        Log(1, "D3D12: DLSS Super Resolution is not available");
        return false;
    }
    Log(0, "D3D12: NGX ready");
    return true;
}

ID3D12GraphicsCommandList* BeginList() {
    auto& s = dx.slots[dx.slot];
    if (!WaitFence(s.completed_at) || FAILED(s.allocator->Reset()) ||
        FAILED(s.list->Reset(s.allocator, nullptr)))
        return nullptr;
    return s.list;
}

bool SubmitList(ID3D12GraphicsCommandList* list, uint64_t input_value) {
    if (!HrCheck("close command list", list->Close()))
        return false;
    if (input_value && !HrCheck("queue wait", dx.queue->Wait(dx.input_fence, input_value)))
        return false;
    ID3D12CommandList* lists[]{list};
    dx.queue->ExecuteCommandLists(1, lists);
    if (!HrCheck("queue signal", dx.queue->Signal(dx.fence, ++dx.fence_value)))
        return false;
    dx.slots[dx.slot].completed_at = dx.fence_value;
    dx.slot = (dx.slot + 1) % dx.slots.size();
    return true;
}

ID3D12Resource* SharedTexture(DXGI_FORMAT format, uint32_t width, uint32_t height, bool uav,
                              HANDLE* handle) {
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Flags = uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
    ID3D12Resource* resource{};
    if (!HrCheck("shared texture",
                 dx.device->CreateCommittedResource(
                     &heap, handle ? D3D12_HEAP_FLAG_SHARED : D3D12_HEAP_FLAG_NONE, &desc,
                     D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&resource))))
        return nullptr;
    if (handle && !HrCheck("shared texture handle",
                           dx.device->CreateSharedHandle(resource, nullptr, GENERIC_ALL, nullptr,
                                                         handle))) {
        resource->Release();
        return nullptr;
    }
    return resource;
}

int32_t D3D12Create(const BbDlss12Config* config, BbDlss12Shared* shared) {
    if (!state.configured || !config || !shared)
        return 0;
    if (!InitializeDevice(config->adapter_luid))
        return 0;
    ReleaseResources();
    dx.config = *config;
    const auto color_format = static_cast<DXGI_FORMAT>(config->color_format);
    const auto depth_format = static_cast<DXGI_FORMAT>(config->depth_format);
    dx.color = SharedTexture(color_format, config->render_width, config->render_height, false,
                             &dx.color_handle);
    dx.motion = SharedTexture(DXGI_FORMAT_R16G16_FLOAT, config->render_width,
                              config->render_height, false, &dx.motion_handle);
    dx.output = SharedTexture(DXGI_FORMAT_R16G16B16A16_FLOAT, config->output_width,
                              config->output_height, true, &dx.output_handle);
    dx.depth = SharedTexture(depth_format, config->render_width, config->render_height, false,
                             nullptr);
    if (!dx.color || !dx.motion || !dx.output || !dx.depth)
        return 0;
    // Depth crosses as a buffer: Vulkan cannot copy a depth aspect into a colour image.
    const D3D12_RESOURCE_DESC depth_desc = dx.depth->GetDesc();
    UINT rows{};
    UINT64 row_bytes{}, total{};
    dx.device->GetCopyableFootprints(&depth_desc, 0, 1, 0, &dx.depth_footprint, &rows,
                                     &row_bytes, &total);
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = total;
    buffer.Height = 1;
    buffer.DepthOrArraySize = 1;
    buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (!HrCheck("depth buffer", dx.device->CreateCommittedResource(
                                     &heap, D3D12_HEAP_FLAG_SHARED, &buffer,
                                     D3D12_RESOURCE_STATE_COMMON, nullptr,
                                     IID_PPV_ARGS(&dx.depth_buffer))) ||
        !HrCheck("depth buffer handle",
                 dx.device->CreateSharedHandle(dx.depth_buffer, nullptr, GENERIC_ALL, nullptr,
                                               &dx.depth_handle)))
        return 0;

    // The DLSS feature, created on the D3D12 queue.
    if (!Check("D3D12 parameter allocation", NVSDK_NGX_D3D12_AllocateParameters(&dx.parameters)))
        return 0;
    for (const char* key : {NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA,
                            NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality,
                            NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced,
                            NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance,
                            NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance,
                            NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraQuality})
        NVSDK_NGX_Parameter_SetUI(dx.parameters, key, config->preset);
    static constexpr std::array qualities{
        NVSDK_NGX_PerfQuality_Value_DLAA, NVSDK_NGX_PerfQuality_Value_MaxQuality,
        NVSDK_NGX_PerfQuality_Value_Balanced, NVSDK_NGX_PerfQuality_Value_MaxPerf,
        NVSDK_NGX_PerfQuality_Value_UltraPerformance};
    NVSDK_NGX_DLSS_Create_Params settings{};
    settings.Feature.InWidth = config->render_width;
    settings.Feature.InHeight = config->render_height;
    settings.Feature.InTargetWidth = config->output_width;
    settings.Feature.InTargetHeight = config->output_height;
    settings.Feature.InPerfQualityValue = qualities[std::clamp(config->quality, 0, 4)];
    settings.InFeatureCreateFlags =
        NVSDK_NGX_DLSS_Feature_Flags_MVLowRes |
        (config->hdr ? NVSDK_NGX_DLSS_Feature_Flags_IsHDR | NVSDK_NGX_DLSS_Feature_Flags_AutoExposure
                     : 0);
    ID3D12GraphicsCommandList* list = BeginList();
    if (!list)
        return 0;
    const auto result =
        NGX_D3D12_CREATE_DLSS_EXT(list, 1, 1, &dx.feature, dx.parameters, &settings);
    if (!SubmitList(list, 0) || !WaitFence(dx.fence_value) ||
        !Check("D3D12 DLSS feature creation", result) || !dx.feature) {
        dx.feature = nullptr;
        return 0;
    }
    *shared = {};
    shared->color = dx.color_handle;
    shared->motion = dx.motion_handle;
    shared->output = dx.output_handle;
    shared->depth_buffer = dx.depth_handle;
    shared->depth_buffer_size = total;
    shared->depth_row_pitch = dx.depth_footprint.Footprint.RowPitch;
    shared->depth_texel_size = static_cast<uint32_t>(row_bytes / config->render_width);
    shared->input_fence = dx.input_handle;
    shared->output_fence = dx.fence_handle;
    dx.created = true;
    Log(0, "D3D12: DLSS %ux%u -> %ux%u, quality %d%s", config->render_width,
        config->render_height, config->output_width, config->output_height, config->quality,
        config->hdr ? ", HDR input" : "");
    return 1;
}

int32_t D3D12Evaluate(const BbDlss12Frame* frame, uint64_t input_value, uint64_t* output_value) {
    if (!dx.created || !frame || !output_value)
        return 0;
    ID3D12GraphicsCommandList* list = BeginList();
    if (!list)
        return 0;
    constexpr auto read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    // Depth: buffer -> private texture.
    Transition(list, dx.depth_buffer, D3D12_RESOURCE_STATE_COMMON,
               D3D12_RESOURCE_STATE_COPY_SOURCE);
    Transition(list, dx.depth, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
    dst.pResource = dx.depth;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.pResource = dx.depth_buffer;
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint = dx.depth_footprint;
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    Transition(list, dx.depth_buffer, D3D12_RESOURCE_STATE_COPY_SOURCE,
               D3D12_RESOURCE_STATE_COMMON);
    Transition(list, dx.depth, D3D12_RESOURCE_STATE_COPY_DEST, read);
    Transition(list, dx.color, D3D12_RESOURCE_STATE_COMMON, read);
    Transition(list, dx.motion, D3D12_RESOURCE_STATE_COMMON, read);
    Transition(list, dx.output, D3D12_RESOURCE_STATE_COMMON,
               D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    NVSDK_NGX_D3D12_DLSS_Eval_Params parameters{};
    parameters.Feature.pInColor = dx.color;
    parameters.Feature.pInOutput = dx.output;
    parameters.Feature.InSharpness = frame->sharpness;
    parameters.pInDepth = dx.depth;
    parameters.pInMotionVectors = dx.motion;
    parameters.InRenderSubrectDimensions = {dx.config.render_width, dx.config.render_height};
    parameters.InJitterOffsetX = frame->jitter_x;
    parameters.InJitterOffsetY = frame->jitter_y;
    parameters.InReset = frame->reset;
    parameters.InMVScaleX = parameters.InMVScaleY = 1.0f;
    parameters.InPreExposure = parameters.InExposureScale = 1.0f;
    parameters.InFrameTimeDeltaInMsec = frame->frame_ms;
    // DirectX 12 add-ons hooked on NGX record their own passes here, on this command list.
    const bool ok = Check("D3D12 DLSS evaluation",
                          NGX_D3D12_EVALUATE_DLSS_EXT(list, dx.feature, dx.parameters,
                                                      &parameters));

    // Everything goes back to COMMON for Vulkan.
    Transition(list, dx.depth, read, D3D12_RESOURCE_STATE_COMMON);
    Transition(list, dx.color, read, D3D12_RESOURCE_STATE_COMMON);
    Transition(list, dx.motion, read, D3D12_RESOURCE_STATE_COMMON);
    Transition(list, dx.output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
               D3D12_RESOURCE_STATE_COMMON);
    // Submitted even when DLSS failed: Vulkan waits for the fence value.
    if (!SubmitList(list, input_value))
        return 0;
    *output_value = dx.fence_value;
    return ok ? 1 : 0;
}

void ShutdownAll() {
    D3D12Shutdown();
    Shutdown();
}

const BbDlssApi api{BBPORT_DLSS_BRIDGE_ABI, Configure,        InstanceExtensions, DeviceExtensions,
                    Initialize,             CreateFeature,    Evaluate,           ReleaseFeature,
                    ShutdownAll,            FrameGenMaxCount, FrameGenCreate,     FrameGenEvaluate,
                    FrameGenRelease,        D3D12Create,      D3D12Evaluate,      D3D12Release};

} // namespace

extern "C" __declspec(dllexport) const BbDlssApi* BbDlssGetApi() {
    return &api;
}
