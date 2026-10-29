# FSR 4.1.1 on Windows

The current combined FSR 4.1 integration is documented in
[FSR 4.1 on Windows](FSR41_WINDOWS.md), including 4.1.2, the compact method dialog,
live switching, ranked defaults and exact support checks. The sections below
describe the original 4.1.1 integration and its historical measurements.

Select **FSR 4.1.1 (experimental)** in **Upscaler**, then choose the method in
**Graphics > FSR 4.1.1 backend** or the **Play** page's quick settings. Press the
normal **PLAY** button. `Play Bloodborne.exe` uses the same saved selection.
A method change takes effect on the next game start.

The **Insert / L3+R3** in-game overlay also provides **FSR 4.1.1 backend**.
Its pending selection is saved immediately when the menu closes, and the
existing **Apply and restart** action applies it. The running provider remains
active until that restart. The launcher and overlay share
`fsr411_backend=vk|dx12` in `bbport.ini`; older launcher-profile values migrate
when the INI key is absent. An explicit INI value takes precedence over that
legacy profile. Both the Graphics page and Play quick settings follow the
shared selection, including changes saved by the overlay while the launcher is open.

| Method | Provider | Installation |
|---|---|---|
| AMD DX12 | AMD's original FSR 4.1.1 runtime through a Vulkan/DX12 shared-texture bridge (buffer fallback) | Included in the Windows package |
| Vulkan (fsr4vk) | fsr4vk's INT8 FSR 4.1.1 implementation/model | Included in the Windows package; **Download fsr4vk** in Graphics repairs missing or changed files |

The launcher remembers the method independently of the upscaler. When another
upscaler is selected, both method controls show a disabled dash; returning to
FSR 4.1.1 restores the saved method. The controls stay synchronized. Existing
profiles keep their explicit method, including legacy launcher settings. When no
method has been saved, the launcher selects AMD DX12 on the selected AMD adapter
and Vulkan on other adapters. All new controls and
status messages cover the 13 launcher
languages; `vk` and `dx12` are language-independent stored values.

Both methods use the normal Windows launcher.
The pre-existing **FSR 4 assets** controls and `fsr4_shaders/` are for the older
FSR 4 v07 path, independent of these two providers.

## Availability and diagnostics

The launcher checks the selected Vulkan adapter, including `BB_GPU_ID`, rather
than a GPU-series allowlist. AMD DX12 requires an AMD adapter, Vulkan/DX12 shared
memory and GPU fences, and an FSR 4.1.1 provider reported by the installed AMD
runtime on that adapter. Vulkan checks the same device features and extensions
as the renderer. These capability checks run in an isolated helper process;
context creation is still checked by the renderer at the actual render size.

The fsr4vk DLL and bundled license files are verified separately. Missing or
changed files show **Vulkan (fsr4vk – download required)**. On a supported
adapter, selecting it asks **Download Vulkan files for FSR 4.1.1. now?** Both selectors use the
Graphics download function. Success verifies the files and selects Vulkan;
declining or a download failure retains the previous method. The existing
Graphics download button installs the files and retains the method preference.
An unsupported adapter keeps the Vulkan entry disabled. Captions expand
to fit their translation in both Graphics and Play quick settings.
Logs retain the affected file names.
A missing DX12 DLL produces a Windows-package reinstall instruction. Driver
feature, provider and DLL-loading failures have separate messages. **Recheck
support** refreshes the check after installation or a driver change.

Unsupported backend entries are disabled in both launcher selectors. A saved
selection remains visible, and PLAY stays disabled with an inline hint until
that method is available or another is selected. Startup rechecks files;
shortcuts open Graphics when the saved method is unavailable.
The in-game overlay also distinguishes an unavailable FSR 4.1.1 method from a
GPU limitation. Direct renderer starts retain the safe FSR 3.1 fallback and log
the actual cause, including missing provider files.

## Windows 1.9.2 integration

The PR incorporates the Windows 1.9.2 branch through
[`de2e38a`](https://github.com/Supermedo/bloodborne_pc/commit/de2e38af055f09f819946951075c6bb119e62ae6).
The Windows merge preserves saved provider and motion-vector cache preferences.
The original Vulkan FSR 3.1 frame-
generation path includes upstream's per-frame bridge-allocation retirement fix.
Build and package checks cover both providers and the original frame-generation
method; shader caches rebuild for upstream's updated shader format.

The performance tables below record the earlier 1.8.2 integrations. The 1.9.2
integration receives separate build, provider-output and extracted-package
checks. The published
[test.3](https://github.com/rattensuppe/bloodborne_pc/releases/tag/v1.8.2-fsr411-test.3)
remains the 1.8.2 snapshot with the optional cache checkbox.

## AMD DX12: runtime and packaging

`packaging/windows/package.sh` includes the unmodified
`bin/amd_fidelityfx_upscaler_dx12.dll` **4.1.1.2740**, from the official
[AMD FSR SDK v2.3.0 release](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/releases/tag/v2.3.0).
The DLL is installed beside `bb-probe.exe`. The repository contains the required
API headers; packaging retrieves the runtime from the pinned SDK archive.

`packaging/windows/fetch_fsr411_dx12.py` prepares a verified build cache under
`out/fsr411-dx12/`. The first package build downloads the official prebuilt SDK
archive (about 127 MB); subsequent builds reuse the verified DLL. It checks the
archive and exact member size/SHA-256 before replacing a cached runtime:

- Archive: `FidelityFX-Samples-v2.3.0-prebuilt.zip`
- Archive SHA-256: `f90890b9323bb2f4f2404ac4cdc9395e8495ecdac6f7aa0bcdf1ad1848422273`
- Member: `Samples/Upscalers/FidelityFX_FSR/dx12/x64/Release/amd_fidelityfx_upscaler_dx12.dll`
- DLL SHA-256: `d0dcccc74a43c44ba435b7a369b456e0970d8a4464e4bd683119b374f2c9fb46`

The package includes the full, unmodified SDK license, third-party notices and
source/download note under `licenses/AMD-FSR-SDK-*`. The runtime has AMD's
binary redistribution terms. The C headers under `gpu/fsr411_dx12/sdk/` are from the
same SDK release and retain AMD's MIT notices. Only the required headers are
vendored. The bridge is compiled into the renderer with MSYS2 CLANG64 and system
DX12/DXGI APIs.

For a source build, prepare and copy the runtime beside the source loader:

```bash
python packaging/windows/fetch_fsr411_dx12.py
cp out/fsr411-dx12/amd_fidelityfx_upscaler_dx12.dll out/
```

## Vulkan: packaging and repair

The provider is included in the Windows package under `fsr4vk/`. The launcher
download repairs missing or changed files in `<data>/fsr4vk` (the port folder by
default). `run.py` passes this folder as `BB_FSR4VK_DIR`. Both the launcher
updater and package refresh preserve it. Downloading and replacement are
disabled while the game runs.

The pinned download is **fsr4vk v0.4.3 with the AMD Windows fix**, published by
[0xCydral](https://github.com/0xCydral/bloodborne_pc/releases/tag/windows-v1.7.0-beta.2).
Unpatched v0.4.3 fails on the tested RX 9070 with
`descriptor binding exceeds queried layout size`; the fix removes variable
descriptor count flags from three fixed-size descriptor heaps. See
[upstream issue #1](https://github.com/dvj5411/fsr4vk/issues/1).

`scripts/fetch_fsr4vk.py` verifies the archive and every installed DLL/license,
supports interrupted downloads, and replaces files atomically:

- Archive SHA-256: `049e81577dd0836fd6fb24a2d7fb07d1276951987519d11251905b9e60456ebb`
- DLL SHA-256: `60a90b24f6789cd52c467471d5a66a0fa6d04e1b8af41095242f50367904f8b2`

The download includes provider licenses and a source/patch provenance note.
fsr4vk is GPLv3, its AMD-derived data MIT licensed, and zstd BSD licensed.
`packaging/windows/prepare_fsr4vk.py` prepares the same verified provider and
licenses in `out/fsr4vk/provider/`. It also fetches and verifies the exact patched
source archive (SHA-256
`23a05d16ca6116760bff4cd6dbf667bf38319eb20d0107c781e9d65a9b065c90`).
Subsequent builds reuse the verified cache. A failed provider or source check
stops packaging.

Standard packaging writes `dist/bbport-windows.zip` and the companion
`dist/fsr4vk-v0.4.3-amdfix-src.zip`. Publish both files together; the provider's
`SOURCE.txt` records the source archive name, original source URL and patch
provenance. The Windows ZIP includes the runtime, license texts and patch note.

### Coalesced output stores

On Navi 48 (`1002:7550`, tested on RX 9070), **3840x2160 Quality SDR**
(`2560x1440` input) automatically uses coalesced output stores in the known
v0.4.3 post shader. The host collects the four output pixels per invocation in
workgroup memory, then writes contiguous blocks, following the idea used by the
older FSR 4 post optimization. Original SPIR-V arithmetic, floating-point controls,
and writes to recurrent/history images are retained. The provider DLL, embedded
assets and verified download hashes are retained. The optimization activates
automatically within the existing Vulkan method.

The v0.4.3 DLL imports core Vulkan functions directly. During the first eligible
dispatch only, the host
temporarily redirects its `vkCreateShaderModule` import in memory, then restores
it. The rewrite is scoped to that loaded provider import. The on-disk DLL retains
its verified hash, and an existing third-party import hook is preserved. A
thread-local scope enables the rewrite only for the configuration
above; an exact-size/128-bit content fingerprint rejects other shader revisions.
An unavailable import or a rejected optimized module uses the original.
Changing eligibility recreates the context with the shader selected for the new
mode. Other GPUs, presets, outputs and HDR use the original
shaders. `BB_FSR4VK_POST=0` disables it at process startup for comparisons.

`tools/fsr411_post_lds.py` reproduces the small generated instruction-edit recipe
in `fsr4vk_post_edits.inc` using SPIRV-Tools and the pinned upstream shader.
Original shader SHA-256:
`590d5a48eaf7390059249b59c0b61e664cb6e3198380f0f011b12d165260e7b6`.
The recipe records the added instructions and replaced stores. GLSL round-tripping
was rejected because it changed
output values; the selected direct-SPIR-V rewrite passed full-image byte checks.

Frame retirement waits for the oldest of the eight provider slots before reuse.
A wait that splits a submission
refreshes the caller's command buffer. A separate in-game A/B showed similar FPS
for the retirement-only change. Both Vulkan variants in the post-shader comparison
use this retirement behavior.

```bash
python scripts/fetch_fsr4vk.py
python scripts/fetch_fsr4vk.py --check
```

`BB_FSR4VK_DIR` overrides the directory; `BB_FSR4VK=0` disables this provider.
For direct `bb-probe.exe` use, set the directory explicitly; otherwise it looks
for `fsr4vk` beside the executable. The adapter/downloader build on
[0xCydral's integration at 66576ef](https://github.com/0xCydral/bloodborne_pc/tree/66576efbbd272d0c97e04a9bdb5631455d530eae),
with input normalization, scheduler handoff and persistent asset storage here.

## Renderer integration and requirements

The launcher sets `BB_FSR411_BACKEND=dx12` or `vk` for the normal game-start
path, overriding inherited values. `BB_FSR411_NATIVE` is set consistently for
the native feature probe. The renderer chooses the requested Windows provider
and retains the saved method through fallback handling.

**AMD DX12** matches the Vulkan adapter's LUID to the DXGI adapter. It requires
an AMD adapter, the original DLL, shared device-local D3D12/Vulkan memory
and importable D3D12 fences as Vulkan timeline semaphores. It explicitly
selects the DLL's non-INT8 **4.1.1** provider, rejecting its legacy FSR 3
provider. Hardware/driver support is established through successful provider
enumeration and context creation. The validation below covers RX 9070.

Color, motion vectors and the result use shared D3D12 textures imported as
Vulkan images. Depth keeps the buffer route for the game's depth/stencil format.
This removes the DX12 buffer-to-color/motion copies and result-to-buffer copy.
Vulkan input image copies, depth transfer, sharpening and HUD composition remain.

The result image/view is passed directly to the existing SDR/HDR consumers,
avoiding an additional full-resolution RGBA16F result copy. External ownership is
retained until the last sharpening, HDR merge, SDR format blit or diagnostic read
has finished recording; `ReleaseOutput` then returns it to DX12. HDR composition
uses the applicable consumer path for FSR 3.1, FSR 4.x, TAA and DLSS.

Image-import support is checked for the actual formats, usage and extents when
creating a context. If unavailable, the original shared-buffer bridge remains
the compatibility path. `BB_FSR411_SHARED_TEXTURES=0` selects that route at process
startup for developer comparisons. The launcher retains its two method choices.

Vulkan signals an input fence; the D3D12 queue waits on it, executes FSR and signals
an output fence. The next Vulkan submission waits before consuming the result.
Synchronization at the API boundaries runs through the GPU fences.
Three DX12 allocator/list slots wait before reusing an occupied slot; resource
changes and teardown still wait for completion. The synchronous bridge remains
available only in the headless comparison test as a control.

### DX12 transfer-buffer sizing

Shared textures retain their format/row-pitch metadata while only buffer-copied
planes reserve space in the shared transfer buffer. The buffer-only fallback
keeps all four planes; a fully shared layout retains a 64 KiB minimum buffer.
At 2560x1440 input and 3840x2160 SDR output, the normal shared-texture SR path
uses **14.0625 MiB instead of 105.5 MiB** for this buffer, saving **91.4375 MiB**.
These are transfer-buffer allocations, rather than total process VRAM.

The 20 October 2026 checks cover real DX12 allocation sizes, plane dimensions,
row pitches and fallback layouts, plus paired synthetic GPU output comparisons.
The RX 9070 Warehouse check loaded the same save without adjusting the camera;
camera matrices/projection were verified in every 10 s warmup / 25 s measurement
phase. Two phases per mode found no FPS regression from this allocation change.

### Object-motion target reuse

Some scenes alternate full- and half-resolution object-motion attachments within
each frame (the tested sewer view alternated 2560x1440 and 1280x720). The original
single-target code finished the entire GPU and recreated the target twice per
frame. The profiler attributed these waits to target recreation.

A bounded cache keeps the active target plus up to three inactive sizes, with
the image, view, dimensions and layout together. A size change can reuse its
resident target. Evicting an inactive target when all slots are
occupied still waits safely; teardown retains its completion wait. Per-size-change
clear and validity behavior is preserved: returning to a cached size clears it
according to the existing rules. Existing image barriers and motion shader math remain.
This renderer is shared by upscalers using object-motion vectors, including
FSR 3.1 and the older FSR 4. **Motion-vector cache (experimental)** in Graphics
is **off by default**, preserving the existing allocation path. The checkbox
is available with FSR and object motion enabled, retains its saved choice and
applies at the next game start, including shortcut launches. Both FSR 4.1.1
methods support it. Cached targets consume extra VRAM; moving gameplay,
frame-pacing and memory-pressure comparisons remain follow-up work.
[Cache controls and reproduction](OBJECT_MOTION_CACHE.md).

**Vulkan (fsr4vk)** enables the provider's complete
[device-feature contract](https://github.com/dvj5411/fsr4vk/blob/v0.4.3/provider/INTEGRATION-CONTRACT.md)
only when its optional DLL is installed. It normalizes color to RGBA16F and
forward depth to reverse R32F, retains motion vectors, and retires the provider's
eight frame slots safely. Both paths propagate replacement command buffers to
callers after splitting a submission. Both support HDR/nonlinear scene inputs
and use the same existing host RCAS sharpening pass, followed by HUD composition.

The Windows providers use the device features described above. The original
Vulkan implementation remains available through its existing selection and feature
checks, including on Linux. The older FSR 4 path is retained. A rejected
requested provider follows the existing FSR 3.1 fallback with the reason in the
log/menu. Selection, quality preset and output setting remain intact. fsr4vk is
limited to standard outputs up to 3840x2160 (minimum 64x64); upstream documents
an unwritten final row/column at odd output sizes. Prefer the standard even sizes.

## Current comparison: sewer scene with both optimizations

Measured on **16 October 2026**, **Intel Core i7-11700B (11th Gen)**, **RX 9070
16 GB**, AMD **26.9.2 LLPC**, Windows 11. A stationary sewer view confirmed by the
tester, fullscreen **2560x1440 -> 3840x2160 Quality SDR**, host RCAS **0.50**,
object motion enabled, FPS cap/VSync off (Immediate). Cheats, mods, GPU profiling
and the live debugger were **off**. All rows below were measured in one process
with the bounded target cache enabled throughout.

| Upscaler / method | Mean FPS | Two phase means |
|---|---:|---|
| FSR 3.1 Vulkan, reference | **131.08** | 131.04 / 131.12 |
| FSR 4.1.1 AMD DX12, shared textures/direct output | **106.50** | 106.68 / 106.32 |
| FSR 4 Vulkan (v07), reference | **90.30** | 90.18 / 90.42 |
| FSR 4.1.1 Vulkan (fsr4vk), coalesced post stores | **85.41** | 85.22 / 85.60 |

Order: FSR 4 / fsr4vk / DX12 / FSR 3.1 / FSR 3.1 / DX12 / fsr4vk / FSR 4.
Each phase had **10 seconds warmup and 25 seconds measured**, five five-second
FPS reports per phase, two phases per method. Repeated phase means differed by
less than 0.5%; measured windows had no shader compilations or reported bridge
errors. Cache diagnostics confirmed two reuses per frame and no allocations or
full GPU waits due to target-size changes during those windows.
[Measurement data](benchmarks/fsr-sewer-20261016.json).

### Cache-only A/B in the sewer (separate run)

An earlier same-process comparison toggled **only** target reuse. DX12 already
used shared textures/direct output in both sides of this A/B; the graphics and
timing conditions above were otherwise the same. Order: DX12 old/new/new/old,
then FSR 3.1 old/new/new/old, again 10 seconds warmup + 25 seconds measured.

| Method | Without target reuse | With target reuse | Change |
|---|---:|---:|---:|
| FSR 4.1.1 AMD DX12 | 58.77 FPS | 108.84 FPS | +85.2% |
| FSR 3.1 Vulkan | 67.29 FPS | 133.22 FPS | +98.0% |

The original path allocated and finished the GPU twice per frame; the cache
reused both resident sizes. This table contains the earlier cache-only A/B run;
the four-method comparison above contains its own run's values.
[A/B data](benchmarks/object-motion-cache-20261016.json).

Both performance runs used experimental **1.8.2** integrations with developer-only
live switches. These cache-enabled results describe the optional experimental
configuration; the current PR defaults to the existing target allocation path.
The checkbox build received separate correctness and package checks.
The preserved **test.2** download enables the cache automatically.
Values are **mean FPS from
five-second reports** for the recorded scene, CPU, GPU and settings. Informal
gameplay checks covered several minutes on the RX 9070. The cache preserves the
selected image-quality settings.

## Historical Vulkan post-shader comparison: Hunter's Dream

Measured on **16 October 2026**, RX 9070, AMD 26.9.2 LLPC, Windows 11, same
stationary Hunter's Dream view in one process. Fullscreen SDR,
**2560x1440 -> 3840x2160 Quality**, host RCAS **0.50**, no FPS cap/VSync,
Immediate presentation. These measurements belong to the test build based on
`739e60d` / v1.8.1.

| Method | First phase mean FPS | Second phase mean FPS | Combined mean FPS |
|---|---:|---:|---:|
| FSR 4.1.1 Vulkan (fsr4vk) | 81.45 | 80.74 | **81.09** |
| FSR 4.1.1 AMD DX12, reference | 93.90 | 94.10 | **94.00** |
| Existing FSR 4 Vulkan (v07), reference | 85.99 | 85.73 | **85.86** |

The Vulkan row includes the new output-store optimization. An internal A/B
control with the original shader reached **69.29 FPS** in the same run; the new
shader raises it to **81.09 FPS (+17.0%)**. This improves the existing Vulkan
method automatically. FSR 4 and DX12 remain faster here.
Order: original / coalesced / FSR 4 / DX12 / DX12 / FSR 4 /
coalesced / original. Each phase settled for 15 seconds then measured for 40:
80 measured seconds and 16 five-second FPS reports per method, 64 reports total.
No shader/pipeline compilations in the measured reports; similar draw counts
(1126-1133/frame). Repeated phase means differ by less than 0.9% per method.
Values are mean FPS from five-second reports. See the
[measurement data](benchmarks/fsr411-vulkan-20261016.json).

Six independent synthetic complete-output comparisons were byte-identical
between original and coalesced shaders, including changing inputs/jitter and
batched submissions; synthetic motion vectors were zero. Informal gameplay checks
covered several minutes on the RX 9070.

The production integration performs the same rewrite in memory with preserved
original numeric IDs.
Final production-wrapper correctness and package checks are listed below;
the eight-phase game benchmark applies to the experimental test build above.

## Direct-input host sharpening (20 October 2026)

The Windows AMD DX12 and fsr4vk methods share the same host RCAS. For scaled
SDR composition, RCAS now reads the returned RGBA16F image directly and writes
a separate RGBA16F result. This removes the FP16-to-RGBA32F input blit while
retaining the shader arithmetic, strength, FP16 output rounding and final HUD
composition. The renderer records the output image's existing FP16 format
explicitly so both providers can use this path.

This selects the Windows FSR 4.1.1 providers with sharpening enabled and a
positive strength. The cache checkbox remains independent. At 1440p/4K the
scaled SDR path uses direct input; native-size 1080p composition, HDR and the
other upscalers retain their existing sharpening paths. The new 4K scratch
image contains 63.28125 MiB of pixel data instead of the old input copy's
126.5625 MiB; process VRAM also includes other images and retained allocations.

RX 9070 / i7-11700B, driver 26.9.2 LLPC, Windows 1.9.2, fixed **Warehouse above
the sewers near Eileen** camera. **4K/Quality SDR**, sharpening **0.50**, object
motion/cache and SSR on, LOD -2, game AA/chromatic aberration off, uncapped
Immediate presentation; cheats/mods off. Within each provider's process:
original/direct/direct/original, twice per variant, **10 s warmup + 25 s capture**.
The loaded view matrix and projection matched throughout all captures.

| FSR 4.1.1 method, FG off | Previous sharpening | Direct input | Change |
|---|---:|---:|---:|
| AMD DX12 | 91.30 Game-FPS | 94.65 Game-FPS | +3.67% |
| Vulkan (fsr4vk) | 76.39 Game-FPS | 78.80 Game-FPS | +3.15% |

The provider rows are separate A/B sessions; both retain their own reference.
[Phase data and full settings](benchmarks/fsr411-sharpen-warehouse-20261020.json).
The original 60-case GPU comparison produced bit-identical FP16 results; the
portable `fsr411-sharpen-test` extends it to 90 cases including 1080p and 1440p.
It compares the actual old blit/shader sequence against direct input, including
edge sizes, five strengths, finite SDR/large-value patterns, alpha and unchanged
input pixels. Build and run without game files:

```sh
ninja -C out/gpu fsr411-sharpen-test
out/gpu/fsr411-sharpen-test.exe
```

Live checks with both providers covered 1080p/1440p/4K output and 4K
Quality/Performance round trips, with native FPS output and captured images.
These are upscaler size changes within a 4K fullscreen SDR swapchain.
The #94 build additionally passed the same transitions with ML-FG and brief
movement/HUD gameplay checks. Frame-generation results are documented with
that PR's ML-FG implementation.

## Validation and reproduction

- Clean 1.8.2 shared-texture/direct-output integration: 16 complete output and
  compute-consumer hash comparisons match the saved buffer/copy controls. Coverage
  includes 4K SDR/HDR, FP16/RGBA8/BGRA8/sRGB/R11G11B10 color, D32/D32S8/D16 depth,
  changing inputs/motion/jitter, batched work, overwritten output and resized inputs.
- `object-motion-cache-test` checks all pixels and validity flags with repeated
  same-size/full/half-size targets and seven-size churn forcing eviction. Its
  tiny, game-size and churn cases match the legacy readback hashes with the
  variable unset, `0`, `1` and invalid. Allocation counts verify the chosen path.

- Windows CLANG64 renderer, loader and capability-tool build.
- `fsr4vk-post-test` checks device/mode guards, exact generated shader bytes,
  and rejection of changed/truncated modules without a Vulkan device.
- The final in-memory production rewrite passed six complete 4K SDR byte
  comparisons, including changing color, jitter and nonzero motion vectors,
  with submissions batched up to 16 frames. HDR and 1080p checks retained the
  original shader. A 1,200-frame stress test and repeated Quality/Balanced
  switches with unsubmitted work passed; switch-test output was byte-identical
  with optimization disabled/enabled. No import-restoration error was reported.
- Frozen Windows launcher and release package, including the pinned native DLL,
  license/notices and this document; fsr4vk remains a separate verified download.
- Downloader tests cover interrupted/corrupt Vulkan downloads, pinned archive
  and native member rejection, cache reuse and replacement protection.
- Launcher tests cover both methods, inherited environment overrides, preserved
  settings and complete translations in all 13 languages. Cache-control tests
  cover the default-off migration, FSR/object-motion availability, retained
  choice, saving, widget cleanup and shortcut application.
- `upscaler-support-test` checks independent availability/fallback without
  changing startup choice, preset or output resolution.
- Both methods were launched through the packaged launcher's normal PLAY path
  into the game world using copied saves. Logs confirmed the requested 4.1.1
  provider.
- Headless production-wrapper checks produce finite, nonconstant SDR/HDR output.
  Synchronous and GPU-fence DX12 output matched byte-for-byte at 1080p and 4K,
  including changing inputs and jitter, over 272 frames per test (16 warmup).
  This establishes image equivalence between the two native synchronization paths.

To rebuild and exercise the production wrappers without game files in CLANG64:

```bash
bash build.sh
python packaging/windows/fetch_fsr411_dx12.py
cp out/fsr411-dx12/amd_fidelityfx_upscaler_dx12.dll out/gpu/
python scripts/fetch_fsr4vk.py
ninja -C out/gpu fsr411-compare object-motion-cache-test upscaler-support-test
out/gpu/upscaler-support-test.exe
for mode in 0 1; do
    for scene in tiny game churn; do
        BB_OBJECT_MOTION_CACHE=$mode out/gpu/object-motion-cache-test.exe "$scene"
    done
done
BB_FSR4VK_DIR="$PWD/fsr4vk" BB_FSR411_NATIVE=1 \
  BB_FSR411_PIPELINE_TEST=1 BB_FSR411_VARY_INPUT=1 \
  out/gpu/fsr411-compare.exe async 2560x1440 3840x2160 ldr 256
```

Use `dx12` for the synchronous control, `vk` for fsr4vk; use `hdr` instead of
`ldr` for HDR inputs. `BB_FSR411_BENCH_DUMP=<file>` saves raw RGBA16F output for
byte comparison. `BB_FSR4VK_DIR` can point to an existing verified provider.
The test consumes the returned image/view with a compute pass and releases it
only after the last read. It prints raw-output and consumer-output hashes;
`BB_FSR411_BENCH_DUMP` also saves an RGBA8 consumer dump. Repeat identical native
runs with `BB_FSR411_SHARED_TEXTURES=0` and the default to compare the compatibility
and direct-output routes. Test-only format controls are `BB_SHARED_TEST_COLOR`
(`fp16`, `rgba8`, `rgba8-srgb`, `bgra8`, `bgra8-srgb`, `r11g11b10`) and
`BB_SHARED_TEST_DEPTH` (`d32`, `d32s8`, `d16`, otherwise `r32`).
`BB_SHARED_POISON_OUTPUT=1` overwrites consumed output before it is returned;
`BB_SHARED_RESIZE_INPUT=1` exercises input/context recreation.
Synthetic timing characterizes the isolated wrapper; the tables above cover
in-game performance.

To reproduce and validate the post rewrite as a maintainer, download the pinned
original shader from fsr4vk v0.4.3 and run with
SPIRV-Tools on PATH:

```bash
curl -L --fail -o original-post.spv \
  https://raw.githubusercontent.com/dvj5411/fsr4vk/7c04e511195bf4420a060d64df84d625a37457e0/assets/fsr411/colors/2160/quality/srgb/pass-27.spv
python tools/fsr411_post_lds.py original-post.spv optimized-post.spv \
  --header gpu/shadps4/video_core/renderer_vulkan/fsr4vk_post_edits.inc
ninja -C out/gpu fsr4vk-post-test
out/gpu/fsr4vk-post-test.exe original-post.spv optimized-post.spv
```

`BB_FSR411_BATCH_SIZE=16` exercises multiple dispatches in one command buffer.
Use otherwise identical `vk 2560x1440 3840x2160 ldr` runs with
`BB_FSR4VK_POST=0` and `BB_FSR4VK_POST=1` to compare raw RGBA16F dumps.

Validation covers the recorded RX 9070 configuration, the listed GPU regression
cases and the described gameplay checks. Both providers are evaluated with their
respective implementations/models.
