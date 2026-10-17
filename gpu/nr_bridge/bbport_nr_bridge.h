// SPDX-FileCopyrightText: Copyright 2026 bbport contributors, bbhost contributors
// SPDX-License-Identifier: MIT
//
// C interface between bbport and bbport_nr_nvngx.dll: DLSS 5 Neural Rendering (NGX feature 18).
//
// The port contains no NVIDIA code. It loads this bridge at run time when it is present, and leaves
// Neural Rendering off when it is absent. Nothing here touches the NVIDIA runtime until the caller
// calls Initialize, so "NR off" costs one LoadLibrary and no device state.
//
// This is ABI 1 of a second, separate bridge. bbport_dlss.dll keeps ABI 2 and is untouched: the two
// features have different lifetimes (the model owns internal history the upscaler does not) and
// different failure modes, and sharing one ABI would make one feature's bug both features'.
//
// The model is an NGX *feature*, not a shader: it is created and evaluated, and it keeps its own
// temporal history internally. The caller owns the composite. Everything below follows from that.
//
// Three things about the runtime are not obvious and are worth stating where a caller will see them:
//
//   1. This DLL's file name must contain "nvngx.dll". The model resolves the module that owns its
//      caller's return address and rejects any whose path does not contain that string, before it
//      reads a single argument. bbport_nr_nvngx.dll satisfies it; a rename does not. Initialize
//      checks this and refuses with a log line rather than failing somewhere deep.
//   2. The model needs two NVIDIA vendor extensions on the Vulkan *device*:
//      VK_NVX_binary_import and VK_NVX_image_view_handle, plus VK_KHR_push_descriptor and
//      VK_KHR_buffer_device_address, with the bufferDeviceAddress *feature* enabled. A Vulkan
//      device's extension list is fixed at creation, so the caller must ask DeviceExtensions
//      before creating the device and append what it returns. Asking afterwards is a permanent no.
//   3. Creation records GPU uploads into the command buffer it is given. Evaluating in the same
//      submission runs the model before its weights have landed. CreatePass therefore records a
//      marker event and reports "built, not yet ready"; the caller submits that buffer, then polls
//      ReadyPass until it answers 1, and only then evaluates.

#pragma once

#include <stdint.h>
#include <vulkan/vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BBPORT_NR_BRIDGE_ABI 1

// How many independent model passes the bridge will hold. A caller runs a chain of up to this many,
// each with its own tuning and its own handle, which is what makes a multi-pass chain possible
// without the runtime's single-feature limitation.
#define BBPORT_NR_MAX_PASSES 4

typedef void (*BbNrLogFn)(int warning, const char* message);

// One image, described the way the model wants it. Layout-compatible with BbDlssImage on purpose:
// the renderer keeps one image description type and one way of filling it in.
//
// The model's own images are RGBA16_SFLOAT (VK_FORMAT_R16G16B16A16_SFLOAT) for colour in and out,
// RG16_SFLOAT (VK_FORMAT_R16G16_SFLOAT) for motion, and R32_SFLOAT (VK_FORMAT_R32_SFLOAT) for depth.
// `format` is checked against those on every evaluation; a mismatch is refused with a log line
// rather than handed to the driver.
typedef struct BbNrImage {
    VkImage image;
    VkImageView view;
    VkImageSubresourceRange range;
    VkFormat format;
    uint32_t width;
    uint32_t height;
} BbNrImage;

// Everything the model latches when a pass is built. These are read once, at creation: changing one
// means releasing and recreating the pass, which is why they are here and not on BbNrEvaluate.
//
// width/height are the model's working size, which need not be the frame's. The caller owns the
// scaling to and from it.
typedef struct BbNrTuning {
    uint32_t width;
    uint32_t height;
    uint32_t preset;        // DLSSNR.Hint.Render.Preset; 0 is the runtime default
    uint32_t style;         // DLSSNR.Style
    float intensity;        // DLSSNR.Intensity
    float structure;        // DLSSNR.LocalStructureStrength
    float tone;             // DLSSNR.LocalToneStrength
    float skin_structure;   // DLSSNR.SkinStructureStrength
    uint32_t auto_skin;     // DLSSNR.UseAutoMask: 1 lets the runtime build its own skin mask
    uint32_t ui_correction; // DLSSNR.UICorrection. Real, but only acts on a UI layer the game
                            // tagged through Streamline, which almost no title does; leave 0 and
                            // protect the HUD in the composite instead.
} BbNrTuning;

// One evaluation. Every image is at the pass's working size, in the layouts the model expects:
// colour GENERAL, motion and depth SHADER_READ_ONLY_OPTIMAL, output GENERAL. The caller transitions
// them; Vulkan has no state promotion, so nothing here does it for you.
//
// Colour is GENERAL rather than SHADER_READ_ONLY because that is where the renderer already keeps the
// frame it hands over, and the model samples it from there. EvaluatePass does not check layouts: it
// checks formats, and a wrong layout is a validation error or a wrong picture rather than a refusal.
typedef struct BbNrEvaluate {
    BbNrImage color;  // sampled,  RGBA16_SFLOAT, the proxy the model is shown
    BbNrImage motion; // sampled,  RG16_SFLOAT, current-to-previous, in pixels
    BbNrImage depth;  // sampled,  R32_SFLOAT
    BbNrImage output; // storage,  RGBA16_SFLOAT, what the model writes
    uint32_t depth_inverted;
    uint32_t reset;   // 1 forgets the model's history: a cut, a load, a teleport
    float mv_scale_x; // the game's own motion vector encoding, passed through rather than derived
    float mv_scale_y;
} BbNrEvaluate;

typedef struct BbNrApi {
    uint32_t abi;  // BBPORT_NR_BRIDGE_ABI
    uint32_t size; // sizeof(BbNrApi), so a caller can refuse a bridge it does not understand

    // UTF-16. runtime_directory is the directory holding nvngx_dlssnr.dll (and usually _nvngx.dll);
    // data_directory is a writable directory the runtime may use for caches. Must be called before
    // anything else. Returns 1 on success.
    int32_t (*Configure)(const wchar_t* runtime_directory, const wchar_t* data_directory,
                         BbNrLogFn log);

    // The Vulkan extensions the model needs, to be added before device creation. The arrays stay
    // valid until Shutdown. Returns 1 on success.
    int32_t (*InstanceExtensions)(uint32_t* count, const VkExtensionProperties** extensions);
    int32_t (*DeviceExtensions)(VkInstance instance, VkPhysicalDevice physical, uint32_t* count,
                                const VkExtensionProperties** extensions);

    // Loads and initialises the runtime on this device. Call it only when Neural Rendering is on.
    // Returns 1 on success; 0 with a log line on any failure, including an unusable GPU or driver,
    // and never retries after failing.
    int32_t (*Initialize)(VkInstance instance, VkPhysicalDevice physical, VkDevice device,
                          PFN_vkGetInstanceProcAddr get_instance_proc,
                          PFN_vkGetDeviceProcAddr get_device_proc);

    // Builds pass `index` (0..BBPORT_NR_MAX_PASSES-1) at tuning->width x tuning->height, recording
    // its creation work and a completion marker into `command`. Returns 1 when the pass was built,
    // which is not the same as usable: the caller must submit this buffer and then poll ReadyPass.
    // Calling it again for an index that is already built rebuilds it, and requires that no
    // submitted work still references that pass.
    int32_t (*CreatePass)(VkCommandBuffer command, uint32_t index, const BbNrTuning* tuning);

    // 1 once the GPU has finished the creation work CreatePass recorded; 0 until then. Poll it after
    // submitting the creation buffer, before the first EvaluatePass.
    int32_t (*ReadyPass)(uint32_t index);

    // Runs pass `index` on `command`. Returns 1 when the model evaluated. Returns 0 without
    // evaluating when the pass is not ready yet, so a caller that polls ReadyPass first never sees
    // it. Transitions no image.
    int32_t (*EvaluatePass)(VkCommandBuffer command, uint32_t index, const BbNrEvaluate* evaluate);

    // Releases pass `index`. The caller must have waited for the GPU work that used it, exactly as
    // for bbport_dlss: freeing a feature under in-flight work removes the device.
    void (*ReleasePass)(uint32_t index);

    // Releases everything and shuts the runtime down. Safe to call more than once, and safe when
    // Initialize never ran.
    void (*Shutdown)(void);
} BbNrApi;

typedef const BbNrApi* (*BbNrGetApiFn)(void);

#ifdef __cplusplus
}
#endif
