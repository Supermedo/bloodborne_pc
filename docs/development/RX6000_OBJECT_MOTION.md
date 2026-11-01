# RX 6000 object motion through SSBOs

With **Object motion vectors** enabled, recognized Navi 21/22/23/24 devices use
Vulkan storage-buffer descriptors for draw parameters and vertex-position history.
The existing controls apply; no extra user setting or force parameter is required.
The previous GPU-address parameter reader reproduced device loss on RX 6800 XT /
Adrenalin 26.10.1. Reading the same data through an SSBO worked. The precise
driver/compiler cause remains unproven.

## Implementation

- `motion_storage.h` selects the path by PCI vendor/device ID. Existing guards
  for other GPUs, required features and low VRAM remain.
- Set 1 binding 0 holds parameters; bindings 1/2 provide bounded views of the
  existing history buffer, 64 MiB + 16 bytes each. No extra history copies or
  per-frame descriptor updates are required.
- Parameter and history indices are bounded, with 64-bit history arithmetic.
  SSBO history uses uint32 atomic component stores for duplicate indexed vertex
  invocations. Existing frame barriers and parameter-ring waits remain.
- Direct and indirect motion pipelines bind set 1 after their resource bindings.
  Indirect draws retain the zero parameter record and camera-only fallback.
- Motion varyings use locations 24/25. Collision checks follow actual emitted
  locations, including fragment remapping and clip-distance emulation. A collision
  retains camera motion for that draw.
- Capability metadata exposes `gpu.object_motion_ssbo` from the same device policy.
  Missing/old metadata retains the launcher guard. Launcher preflight and overlay
  failure reasons share the named 13-language catalog in `bbport_lang_motion.py`.
- The motion-target cache remains separate, optional and off by default. Saved
  preferences, startup behavior and launcher/overlay synchronization remain.
- Motion-pipeline preload exclusion, GPU policy, SSBO/BDA memory access and atomics
  are preserved. This path is independent of the upscaler version/backend.

After a device loss, Help can suggest disabling Object motion vectors and
restarting for a comparison. The hint requires the log to show SSBO active at
the failure; it preserves the generic driver advice and does not change any
setting. It does not establish the cause or replace hardware testing.

## Regression checks

Build the opt-in `motion-storage-test`, `motion-shader-test`, `motion-history-test`
and `object-motion-cache-test` targets. `motion-shader-test <output-directory> --gpu`
exercises emitted shaders and the actual ObjectMotion owner. Use private
`BB_USER_DIR`/`BB_GPU_USER_DIR` directories. Test-only `BB_OMV_TEST_BDA` selects the
existing BDA reference; `BB_OMV_TEST_SET0_PUSH`/`BB_OMV_TEST_THREADED` select recording
variants. These controls belong to fixtures and do not run in-game.

RX 9070 / Adrenalin 26.10.1 forced-SSBO verification covered 56 bounds/data cases,
48 history frames, four indirect descriptor-rebind cases and four cache readbacks
(BDA/SSBO, off/on). All eight emitted shader modules matched the original fixture
and passed SPIR-V validation. The location checker covers 75 cases. With
`BB_FRAME_STATS=1`, 600-frame summaries count pipeline selection attempts,
including preparation workers; they are not submitted draw counts.

Headless device creation reports the inherited
`VUID-vkCreateDevice-ppEnabledExtensionNames-01387`: the unchanged instance code
enables `VK_KHR_swapchain` without a surface extension. The BDA reference reproduces
it. No additional SSBO findings or synchronization hazards were reported.
The separate headless correction is outside this PR.

Current build, launcher, catalog and packaging results are recorded in
[PR #141](https://github.com/Supermedo/bloodborne_pc/pull/141). Only the short player
guide is included in the game ZIP.

## Hardware coverage

An earlier combined SDK 2.4 prototype passed four short RX 6800 XT / Adrenalin
26.10.1 gameplay checks at 2560 x 1440 / Quality, 60 FPS, FG off: FSR 4.1.2 AMD
Vulkan/DX12, each with cache off/on. Logs showed active provider 4.1.2, automatic
SSBO selection, previous-frame history and normal exits. The tester reported no
visual anomalies. These runs are not a new standalone-branch test or an uncapped
frame-time benchmark. Fresh RX 6000 gameplay, particularly Navi 22/23, longer
sessions, resolution changes and FSR 3.1 FG remain useful coverage.

SDK 2.4, RCAS fusion, LRU/deferred-release caching and the scaled debug view are
separate changes. The original SDK Vulkan shader findings in the
[combined prototype report](https://github.com/rattensuppe/bloodborne_pc/blob/2a8ec7e435f3b22e031237a6a201fb572c83a411/docs/FSR24_UPSCALING.md#local-validation-findings-october-30-2026)
remain separate from SSBO validation. No new RX 6000 performance claim is made.
