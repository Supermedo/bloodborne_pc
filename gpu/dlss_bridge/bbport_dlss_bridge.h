// SPDX-FileCopyrightText: Copyright 2026 IFreemz (shadps4_dlss bridge), bbport contributors
// SPDX-License-Identifier: MIT
//
// C interface between bbport and bbport_dlss.dll. The port contains no NVIDIA code: it loads the
// bridge at run time when it is present (next to bb-probe.exe, with nvngx_dlss.dll) and keeps
// its other upscalers when it is absent. Adapted from IFreemz/shadPS4-Bloodborne-DLSS-FSR.

#pragma once

#include <stdint.h>
#include <vulkan/vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif

// 2: DLSS Frame Generation (the FrameGen* members; an ABI 1 bridge has DLSS only).
// 3: DLSS Super Resolution on Direct3D 12 (the D3D12* members): the renderer copies its inputs
//    into textures shared with a D3D12 device and waits for the result on a shared fence, so
//    DirectX 12 add-ons that hook NVIDIA's D3D12 NGX calls see a regular D3D12 DLSS game.
#define BBPORT_DLSS_BRIDGE_ABI 3

typedef void (*BbDlssLogFn)(int warning, const char* message);

typedef struct BbDlssImage {
    VkImage image;
    VkImageView view;
    VkImageSubresourceRange range;
    VkFormat format;
    uint32_t width;
    uint32_t height;
} BbDlssImage;

typedef struct BbDlssFeature {
    uint32_t input_width;
    uint32_t input_height;
    uint32_t output_width;
    uint32_t output_height;
    int32_t quality; // 0 DLAA, 1 Quality, 2 Balanced, 3 Performance, 4 Ultra Performance
    int32_t depth_inverted;
    uint32_t preset; // NVSDK_NGX_DLSS_Hint_Render_Preset value, 0 = driver default
    int32_t hdr;     // 1: linear HDR colour with automatic exposure, 0: tonemapped colour
} BbDlssFeature;

typedef struct BbDlssEvaluate {
    BbDlssImage color;  // sampled
    BbDlssImage depth;  // sampled, hardware depth
    BbDlssImage motion; // sampled, render-resolution pixels, current to previous
    BbDlssImage output; // storage, general layout
    float jitter_x;
    float jitter_y;
    int32_t reset;
    float frame_ms;
    float sharpness; // 0..1; 0 leaves the output unsharpened
} BbDlssEvaluate;

// DLSS Frame Generation (nvngx_dlssg.dll): frames between two finished frames.
typedef struct BbDlssgFeature {
    uint32_t width;         // backbuffer: input and output colour
    uint32_t height;
    VkFormat format;
    uint32_t render_width;  // depth and motion vectors
    uint32_t render_height;
} BbDlssgFeature;

typedef struct BbDlssgEvaluate {
    BbDlssImage backbuffer; // sampled, general layout: the finished frame (UI included)
    BbDlssImage depth;      // sampled, hardware depth values (a depth or R32F image)
    BbDlssImage motion;     // sampled, render-resolution pixels, current to previous
    BbDlssImage output;     // storage, general layout: the generated frame
    BbDlssImage hudless;    // sampled: the backbuffer before the UI; image null when none
    float jitter_x;         // render pixels
    float jitter_y;
    float camera_near;
    float camera_far;
    float camera_fov;       // vertical, radians
    int32_t depth_inverted;
    int32_t reset;
    uint32_t multi_frame_count; // frames generated for this pair (1: 2x .. 3: 4x)
    uint32_t multi_frame_index; // 1 .. multi_frame_count, in order
    // Row-major, row vectors (v * m), D3D clip space (y up, depth 0 at the near plane): the
    // projection, current clip to previous clip and back. has_matrices 0: a camera that stood
    // still and a projection from camera_fov is assumed.
    int32_t has_matrices;
    float view_to_clip[4][4];
    float clip_to_prev_clip[4][4];
    float prev_clip_to_clip[4][4];
} BbDlssgEvaluate;

// ABI 3: DLSS on a D3D12 device of the same adapter.
typedef struct BbDlss12Config {
    uint32_t render_width;
    uint32_t render_height;
    uint32_t output_width;
    uint32_t output_height;
    uint32_t color_format; // DXGI_FORMAT of the shared colour texture
    uint32_t depth_format; // DXGI_FORMAT of the depth texels in the shared buffer (R32F or R16)
    int32_t quality;       // as BbDlssFeature
    uint32_t preset;
    int32_t hdr;
    uint8_t adapter_luid[8]; // VkPhysicalDeviceIDProperties::deviceLUID
} BbDlss12Config;

typedef struct BbDlss12Shared {
    // NT handles (the bridge keeps them open until D3D12Release): import them in Vulkan as
    // VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT / ..._SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT.
    void* color;         // render size, color_format
    void* motion;        // render size, R16G16_FLOAT
    void* output;        // output size, R16G16B16A16_FLOAT
    void* depth_buffer;  // buffer: render_height rows of depth_row_pitch bytes
    uint64_t depth_buffer_size;
    uint32_t depth_row_pitch;
    uint32_t depth_texel_size;
    void* input_fence;   // Vulkan signals it when the inputs are copied
    void* output_fence;  // D3D12 signals it when the output is written
} BbDlss12Shared;

typedef struct BbDlss12Frame {
    float jitter_x;
    float jitter_y;
    int32_t reset;
    float frame_ms;
    float sharpness;
} BbDlss12Frame;

typedef struct BbDlssApi {
    uint32_t abi;
    // UTF-16 paths: the directory with nvngx_dlss.dll, and a writable data directory.
    int32_t (*Configure)(const wchar_t* dll_directory, const wchar_t* data_directory,
                         BbDlssLogFn log);
    // Vulkan extensions NGX needs (DLSS and, when the GPU has it, Frame Generation). The arrays
    // stay valid until Shutdown.
    int32_t (*InstanceExtensions)(uint32_t* count, const VkExtensionProperties** extensions);
    int32_t (*DeviceExtensions)(VkInstance instance, VkPhysicalDevice physical, uint32_t* count,
                                const VkExtensionProperties** extensions);
    int32_t (*Initialize)(VkInstance instance, VkPhysicalDevice physical, VkDevice device,
                          PFN_vkGetInstanceProcAddr get_instance_proc,
                          PFN_vkGetDeviceProcAddr get_device_proc);
    int32_t (*CreateFeature)(VkCommandBuffer command, const BbDlssFeature* feature);
    int32_t (*Evaluate)(VkCommandBuffer command, const BbDlssEvaluate* evaluate);
    // The caller must have waited for the GPU work that used the feature.
    void (*ReleaseFeature)(void);
    void (*Shutdown)(void);
    // ABI 2. Frames Frame Generation can make per pair of rendered frames after Initialize:
    // 0 unavailable, 1 (2x), 3 (up to 4x, GeForce RTX 50).
    int32_t (*FrameGenMaxCount)(void);
    int32_t (*FrameGenCreate)(VkCommandBuffer command, const BbDlssgFeature* feature);
    // Call multi_frame_count times per rendered frame, multi_frame_index 1, 2, ...
    int32_t (*FrameGenEvaluate)(VkCommandBuffer command, const BbDlssgEvaluate* evaluate);
    // The caller must have waited for the GPU work that used the feature.
    void (*FrameGenRelease)(void);
    // ABI 3. Creates the D3D12 device, the shared resources and the DLSS feature (again when
    // the configuration changes). The caller must have waited for the GPU work that used the
    // previous resources.
    int32_t (*D3D12Create)(const BbDlss12Config* config, BbDlss12Shared* shared);
    // Queues on the D3D12 queue: wait for input_fence >= input_value, evaluate DLSS, signal
    // output_fence with *output_value. Returns at once.
    int32_t (*D3D12Evaluate)(const BbDlss12Frame* frame, uint64_t input_value,
                             uint64_t* output_value);
    void (*D3D12Release)(void);
} BbDlssApi;

typedef const BbDlssApi* (*BbDlssGetApiFn)(void);

#ifdef __cplusplus
}
#endif
