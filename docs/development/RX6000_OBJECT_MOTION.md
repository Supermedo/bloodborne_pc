# RX 6000 object motion through SSBOs

With **Object motion vectors** enabled, recognized Navi21/22/23/24 devices
automatically use ordinary Vulkan StorageBuffer descriptors for draw parameters
and vertex-position history. The existing launcher and overlay controls apply;
there is no additional user setting or required force parameter.

The previous raw GPU-address path could lose the device when loading the world
on these cards, so bbport disabled object motion there. A parameter-only reader
also reproduced the failure on RX 6800 XT / Adrenalin 26.10.1. Reading the same
data through an ordinary SSBO worked. This supports the access-path workaround;
the precise driver/compiler root cause is not established.

## Implementation

- `motion_storage.h` identifies Navi2x devices by PCI vendor/device ID. Other
  GPUs retain their existing path. Older AMD guards, required Vulkan features
  and the existing low-VRAM policy remain.
- Set1 binding0 is the readonly parameter buffer. Bindings1/2 reference fixed,
  bounded views of the existing history buffer: 64 MiB + 16 bytes each. They do
  not allocate additional history copies or require per-frame descriptor updates.
- Parameter indices and history accesses are bounded; history indexing uses
  64-bit arithmetic. Duplicate indexed vertex invocations use uint32 atomic
  component stores. Existing frame barriers and parameter-ring waits remain.
- This path uses motion varyings at locations24/25. Attribute collisions skip
  object-motion instrumentation for that draw, retaining camera motion.
- The native capability tool exposes `gpu.object_motion_ssbo` from the same
  C++ device policy. The launcher uses that field to lift its RX6000 guard.
  Missing/old metadata conservatively retains the guard. VRAM checks still apply.
- **Motion-vector cache** remains optional and off by default. Its startup mode,
  saved preference and launcher/overlay synchronization are unchanged.

This common renderer path is independent of the selected FSR version/backend.
It does not alter AMD's upscaler shaders, models, exposure or frame generation.

## Verification

Native tests exercise the actual emitted shaders and ObjectMotion buffer owner:
history bounds/readback, both history halves, parameter-ring reuse, previous GPU
writes, duplicate indices, instancing, normal/push set0 layouts and direct/threaded
recording. A private constructor overload allows the test to exercise SSBO on a
different GPU; the production constructor always selects from the physical GPU.
Cache pixel-readback tests cover both paths with caching off/on.

On RX9070 / Adrenalin26.10.1, 56 history bounds/data cases and 48 native frames
with two draws each passed, along with four cache cases. No additional SSBO-specific core findings or synchronization hazards were reported;
the inherited headless device-creation finding is described below. Eight backend modules (SSBO/BDA,
vertex/fragment, motion/plain) passed standalone SPIR-V validation.

On the earlier combined SDK2.4 prototype, an external RX6800XT /
Adrenalin26.10.1 tester completed all four gameplay
cases at 2560x1440/Quality, 60FPS, FG off:

| FSR4.1.2 method | Motion-vector cache | Result |
|---|---|---|
| AMD Vulkan | Off | Active provider4.1.2, normal exit |
| AMD DX12 | Off | Active provider4.1.2, normal exit |
| AMD Vulkan | On | Active provider4.1.2, normal exit |
| AMD DX12 | On | Active provider4.1.2, normal exit |

All four reports identify the tested renderer, automatic SSBO selection and
prepared previous-frame history. No device loss or upscaler fallback occurred.
The tester reported no visual anomalies and judged overall image quality better
than FSR3.1. This is subjective feedback from short runs on one GPU/driver.
The gameplay reports contain no numerical GPU motion-vector readback or uncapped
frame-time benchmark. Longer sessions, more GPUs, resolution changes and FG3.1
remain useful beta coverage.

The original SDK2.4 Vulkan shader findings documented in
[the combined prototype report](https://github.com/rattensuppe/bloodborne_pc/blob/2a8ec7e435f3b22e031237a6a201fb572c83a411/docs/FSR24_UPSCALING.md#local-validation-findings-october-30-2026)
remain separate from SSBO validation.

Build the opt-in `motion-storage-test`, `motion-shader-test`,
`motion-history-test` and `object-motion-cache-test` targets for regression checks.
`test_motion_storage_launcher.py` checks the launcher guard without loading its UI.
`motion-shader-test <output-directory> --gpu` runs the native SSBO readbacks;
use private BB_USER_DIR/BB_GPU_USER_DIR test folders. Test-only
BB_OMV_TEST_SET0_PUSH/BB_OMV_TEST_THREADED select the recording variants.
These tests are excluded from normal application builds and never run in-game.

## Standalone review scope

This branch applies SSBO routing on the official Windows 1.9.7 base. It
retains the original motion-target cache lifecycle and existing upscaling
providers. SDK 2.4, RCAS fusion, LRU/deferred-release optimisation and the
scaled debug view are separate changes. The RX6800XT feedback above is
from the combined prototype; it is not a new hardware run of this branch.

On the standalone 1.9.7 branch, the Windows renderer/capability tools and native
tests build. RX9070 tests force the private SSBO fixture: 56 bounds cases,
48 native history frames, direct/threaded recording and normal/push set0 layouts
pass. Four original-cache readbacks (BDA/SSBO, off/on) have identical output;
all eight emitted SPIR-V modules pass standalone validation. The five launcher
metadata/guard tests pass.

Khronos validation reports `VUID-vkCreateDevice-ppEnabledExtensionNames-01387`
in headless device creation. The unchanged upstream instance code enables
`VK_KHR_swapchain` without a surface instance extension; a BDA-only control
reproduces the same finding. No additional SSBO validation findings or
synchronization hazards were reported. The separate headless correction in the
combined prototype is intentionally not folded into this fix. There is no new
RX6000 gameplay run or performance claim for this extracted branch.


## PR141 follow-up (November 1, 2026)

- Bind set1 after resource binding for indirect motion pipelines too. Parameter
  entry zero retains camera-only fallback; the descriptor interface remains valid.
- Check actual SPIR-V locations, including remapped fragment inputs and clip-distance
  emulation, before selecting compact SSBO varyings. Constant default inputs are exempt;
  unused non-default declarations still count. The shader emitter itself is unchanged.
- Optional selection logging now records the final choice. With BB_FRAME_STATS=1,
  600-frame summaries count selected/fallback selection attempts, including preparation
  workers; these are not submitted draw counts or performance measurements.
- Share the GPU/driver, VRAM, feature, SSBO-limit and buffer-allocation reasons between
  launcher and overlay in all 13 languages. The launcher reports only preflight reasons
  it can establish; renderer-only failures are reported by the overlay/log.
- Keep the existing motion-pipeline preload exclusion. LoadGraphicsPipeline rejects
  motion keys before loading any stage; shader build/profile guards remain. No cache
  format change or blanket cache invalidation is justified by the reported scenario.
- Preserve device selection, frame/ring/cache lifetime, SSBO/BDA memory access and
  atomics. A device-loss marker cannot reliably identify Object Motion as the cause;
  it is not used to silently change the saved preference.

The executable GPU regression adds an indirect zero-record draw after incompatible
set0 bindings, on ordinary/push descriptor layouts and direct/threaded recording.
These are forced-SSBO fixtures on RX9070, not new RX6000 gameplay evidence.
Current test results and remaining hardware coverage are recorded in the PR.

Fresh follow-up verification: a complete Windows renderer/capability build,
12 Python tests in each of normal/-O/-OO modes, 75 varying-location cases,
56 bounds/data cases, 48 history frames and four indirect descriptor-rebind
cases pass. All eight emitted shader modules are byte-identical to the original
PR141 fixture and pass SPIR-V validation. Four cache readbacks remain identical.
Khronos validation reports only the inherited headless VUID noted above, with
no additional findings or synchronization hazards. The standard ZIP passes
manifest/CRC and the actual frozen-launcher runtime check; the development
report is absent from that ZIP. No game was started for this follow-up.

This development report remains in the repository. The game ZIP contains only
the concise player guide at docs/RX6000_OBJECT_MOTION.md.
