# Scaled motion-vector debug view

The existing **Show motion vectors (debug)** overlay option also works on
the scaled RGBA8 UI path. The visualization runs after reconstruction/sharpening and
before the HUD, so upscaling no longer bypasses the debug branch.

Red/green represent the horizontal/vertical motion supplied to the upscaler;
8 render pixels per frame reach full intensity. Blue marks coverage in the object-motion
target. It does not certify that object vectors passed the separate depth
validity check. Alpha is retained. A missing or differently sized object target
does not contribute blue coverage.

This topic is extracted from the combined prototype's `2180512` onto official
Windows 1.9.7 (`b480526`). It preserves the existing upscaling providers,
sharpening and original motion-target cache lifecycle. SDK 2.4, SSBO routing,
LRU/deferred target release and camera/depth ghosting experiments are separate
changes. The original cache and UI resizing wait for GPU completion before
destroying their images/views; this view needs no cache-optimisation dependency.

The new pipeline is created lazily. With debug disabled, or outside its
RGBA8 UI target, the function returns before pipeline/view allocation and
dispatch. This is an internal image-format guard, not a test of the display HDR setting.

## Regression checks

Build `scaled-motion-debug-test` explicitly; it is excluded from normal builds.
It executes the production shader and checks every pixel, mapped render-space
motion, object coverage and alpha. Cases cover odd dimensions, absent/larger/
smaller object targets, WQHD/4K, Performance/Native-AA input sizes and returning
to a small output. Run in private test profiles without loading a game.

The fixture checks shader output and isolated resource transitions; it does
not replace a gameplay/HUD sight check.

## Original standalone validation, October 31, 2026

On RX9070 / Adrenalin26.10.1, the renderer/capability tools, debug target and
three relevant CPU tests build and pass. The actual embedded SPIR-V module
passes standalone validation. Ten pixel/alpha cases pass normally and again
with Khronos Core and Synchronization checks confirmed active: 20 readbacks.

Validation reports the existing headless-device
`VUID-vkCreateDevice-ppEnabledExtensionNames-01387`. The unchanged upstream
instance enables `VK_KHR_swapchain` without a surface extension; an unchanged
cache/BDA control reproduces it. No additional debug-view findings or
synchronization hazards were reported. The separate headless correction in
the combined prototype is not part of this topic.

The standard local Windows ZIP, program manifest, CRC and actual frozen
launcher imports pass. Cached DLSS bridge files are retained unchanged; this
is not a new NVIDIA hardware test. Earlier gameplay feedback belongs to the
combined view. No fresh gameplay/HUD test or performance benchmark was run
for this independently extracted branch.


## PR142 follow-up, November 1, 2026

`ObjectMotion::CurrentView(width, height)` rejects unwritten frames and
size-mismatched coverage while retaining the raw View/Image accessors for other
uses. CameraMotion and both debug branches use the checked view. This changes
diagnostic blue selection, not motion generation or depth acceptance for FSR.
Late reads still go through View/Image so the separate cache-lifetime change
can account for them when the topics are combined.

The shader fixture adds negative X, saturation at/above eight render pixels,
and zero motion with blue coverage. A real ObjectMotion owner regression writes
a blue target, advances frames, confirms the old pixel remains and rejects its
view as current. It also checks repeated and changing render sizes with cache
off/on. Pixel-size fixtures still allocate their own UI images; they do not prove
TemporalUpscaler resource reuse or HUD placement. The latter needs the separate
live-game check, including resolution changes and frame generation.

The full technical history stays in this repository. Player instructions are in
the Diagnostics section of FSR41_WINDOWS.md; no separate review report is packaged.
View-creation deduplication with the independent sharpening topic remains separate.
The two shader color mappings have reciprocal maintenance comments.

## Follow-up validation, November 1, 2026

The complete Windows renderer/capability build and three relevant CPU programs
pass. Thirteen shader pixel/alpha cases run normally and with Core/Sync validation
using cache off/on: 39 readbacks. Twelve owner-coverage sequences retain a real
blue pixel across an unwritten frame, reject its stale view and check mismatched,
repeated and changing sizes. Both visualization modules remain byte-identical to
the original branch. The inherited headless-device VUID above remains; no
additional findings or synchronization hazards were reported.

The standard ZIP passes manifest/CRC and the actual frozen-launcher check.
Player diagnostics are included in FSR41_WINDOWS.md; this development report is
excluded. Cached DLSS files remain unchanged; no NVIDIA hardware was tested.

RX9070 game checks confirm actual debug dispatches for FSR3.1, TAA and FSR4.1.2
Vulkan/DX12, including the scaled and legacy 1080p routes and live output changes
between 4K, WQHD and 1080p. FG3.1 and ML-FG4.0.2 activate; the latter reports its
real context and first GPU submission. Disabling the view produces no debug
dispatch. The test-only control/log hooks are outside this branch and package.

The user reports motion-dependent colors and normally visible HUD in the scaled
ML-FG view and normally visible HUD in the legacy view, with blue objects at rest.
These are functional visibility checks, not a colorimetric comparison or general
upscaler/frame-generation image-quality certification. Display HDR was not tested.
