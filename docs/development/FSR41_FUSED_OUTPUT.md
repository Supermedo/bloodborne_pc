# Combined RCAS and SDR output

For FSR 4.1.1/4.1.2 through Vulkan or AMD DX12, positive host sharpening writes from
the provider's RGBA16F output into the RGBA8 UI target. This replaces the separate
FP16 sharpen target and nearest blit. The shader explicitly rounds through FP16
before RGBA8 conversion, retaining the prior two-step pixel result. Upscaler
history, motion vectors and subsequent HUD composition are unchanged.

Other providers, formats, sharpening off/zero, mismatched sizes and aliased
input/output retain the existing path. The method does not change settings or
the AMD provider. The dedicated GPU test compares every output component with
the pinned original direct-RCAS shader followed by a real nearest Vulkan blit,
including small/odd sizes, finite boundary/random values and five strengths.
An optional captured RGBA16F input can also be supplied to the test.

Build and run in a configured Windows CLANG64 environment:

```sh
ninja -C out/gpu bb-probe fsr41-fused-output-test
out/gpu/fsr41-fused-output-test.exe
```

The test retains a historical direct-RCAS SPIR-V reference and also builds the
current direct shader. Each is compared independently with fusion through a
real FP16-to-RGBA8 blit. The historical module detects shared arithmetic changes;
the current reference detects divergence between the two current paths.
Any intentional arithmetic change requires an explicit reference review.

The test prefers a capable discrete GPU. Set `BB_TEST_GPU_INDEX` to an explicit
Vulkan enumeration index to override it; the selected index/name are printed.
Unavailable requirements fail instead of switching away from an explicit GPU.
Checks and required operations remain active when `NDEBUG` is defined.

## Original standalone review, October 31, 2026

This topic is extracted from `bd23156` onto official Windows 1.9.7 (`b480526`).
The fusion function and shader are preserved. SDK 2.4, SSBO routing, motion-cache
optimisation and the scaled debug view are separate changes. User defaults,
RCAS strength mapping and the existing sharpening-off/zero paths are retained.

On RX9070 / Adrenalin26.10.1, the native renderer/provider tools and three
relevant CPU tests build and pass. The three existing non-fused shader variants
are byte-identical to newly compiled pristine-base shaders, and the pinned
reference matches the current direct-RCAS module. The fused SPIR-V validates.

There are 220 exact RGBA8 comparisons with unchanged FP16 inputs: 90 synthetic
cases normally, 90 with Core/Synchronization validation confirmed active and
40 replays of fresh GPU outputs from all four FSR4.1 methods at 1080p/4K.
The synthetic set covers tiny/odd/WQHD/4K sizes, finite boundary/random values
and strengths 0, 0.25, 0.5, 1 and 2. No validation errors or synchronization
hazards were reported by the standalone RCAS test. The backend-output checks
do not certify AMD's complete shader implementation.

The original DirectSharpen/ExtraSharpen functions are unchanged and the fusion
guards were reviewed separately. These isolated provider/replay tests do not
execute the complete in-game composition wrapper, HUD or FG. No fresh gameplay
or performance benchmark of this extracted branch is claimed.

The unrelated `shared-graphics-overlay-test` still has its pristine-base
`runtime_restart` link defect. It is recorded as a failure, not counted as a
passing check or repaired in this performance change. The GPU fixture routes
its own assertion failures to stderr rather than a modal CRT dialog; a missing
input file is verified to fail with a captured assertion.

The normal local Windows ZIP is checked separately, including its program
manifest, CRC and real frozen launcher. Cached DLSS files are retained without
a new NVIDIA hardware test.

## Earlier measurements

An earlier local 4K/Quality Warehouse comparison on RX9070 measured about 0.8%
more game FPS. Earlier beta checks also covered synthetic/captured inputs and
backend-output replays. Those historical results are not new measurements of
this 1.9.7 branch or claims for FG, HDR or other GPUs.

## Diagnostic interpretation

The production log reports the executed host-RCAS route on a transition or size
change: fused RGBA8 UI, direct FP16, separate FP32 copy, off or zero strength.
The state belongs to the upscaler instance and resets when the provider changes.
It does not log every frame or add a GPU synchronization point.

With fusion active, the optional frame dump `output` is the provider's
unsharpened RGBA16F image. `output_sdr` is the post-RCAS RGBA8 UI image, captured
before HUD composition. A dump message records this distinction. Other routes
retain their existing `output` meaning. Extra copying occurs only on a requested
dump; it must not be enabled for a performance comparison.

The format guard tests the internal RGBA8 UI image, not the display HDR setting.
The original 1080p `Run()` route is unchanged; an explicit startup render-size
override can use `RunScaled()` even with 1080p output. UI-view/dispatch refactoring,
1080p direct sharpening, RCAS strength changes and provider RGBA8 output are
separate follow-ups.

Cross-device pixel identity requires a device/driver test. The earlier exact
RX9070 comparisons do not establish universal format-conversion rounding or
NaN behavior. The bandwidth/VRAM estimates describe the eliminated intermediate
image and transfer; they are not new GPU-time or game-FPS measurements.

## Follow-up validation, November 1, 2026

The full Windows renderer/provider build and three relevant CPU programs pass.
Both historical and current direct references match fusion: 360 synthetic
comparisons, including a run with Core/Sync validation, and 80 fresh provider
output replays across all four FSR4.1 methods at 1080p/4K. An additional explicit
GPU-selection run passes ten comparisons. No differences, validation errors or
synchronization hazards were reported in these RCAS fixtures. The existing
direct-vs-copy sharpening regression also passes.

The test is compiled with NDEBUG and without -UNDEBUG. Eight malformed/missing
capture controls and two invalid GPU selectors fail explicitly without modal
assertion dialogs. The explicit valid GPU selector runs the requested RX9070.
All three pre-existing shader variants remain byte-identical to the pristine
base; the historical reference words are unchanged and the fused SPIR-V validates.

The standard Windows ZIP passes manifest/CRC and actual frozen-launcher checks.
Its FSR guide includes the short player explanation; this technical report is
excluded. Cached DLSS files are retained. No new NVIDIA or RX7900XT hardware
check has been performed. Current-game checks are recorded below; the isolated comparisons are not
a new performance measurement.

## Current-game functional checks, November 1, 2026

RX9070 game checks confirm 14 composition phases: all four FSR4.1 methods,
sharpening off and zero, 4K/WQHD/1080p changes, TAA/FSR3.1, FG3.1 and
ML-FG4.0.2. Actual dispatch records confirm fusion for positive FSR4.1 host RCAS
on the scaled route, its absence for off/zero/other upscalers, and the retained
FP32-copy route at native 1080p. ML-FG reports its real context and GPU submission.
The production path-transition messages are confirmed; the motion cache is
explicitly enabled in the renderer log.

A requested single-frame dump contains both provider FP16 and composed RGBA8
outputs at the correct sizes. The provider capture is finite; the composed
capture has alpha 255 throughout. The source records the latter before the HUD.
The user reports no image errors in the requested camera/movement/HUD checks
with ML-FG4.0.2 and then FG3.1. The game exits regularly. Private control/log hooks
are outside this branch and the public ZIP. These are functional and visual
checks, not a new FPS measurement, HDR test or cross-device image certification.
