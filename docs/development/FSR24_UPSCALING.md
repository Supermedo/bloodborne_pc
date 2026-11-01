# SDK 2.4 upscaling: technical validation

The following records the original standalone integration and its test scope.
The follow-up section records subsequent corrections separately.

# Original integration record

For development only, `BB_FSR24_DEBUG=1` enables AMD's SDK input checker on
both 4.1.2 methods. It is off by default; leave it off for performance and
image-quality comparisons. Both methods log SDK messages and query the
SDK's total/aliasable GPU memory once after each context creation. These
figures describe the SDK effect, not total process VRAM. An unsupported
memory query is logged and does not disable upscaling.

On AMD devices with the SDK Vulkan DLL present, device creation enables the
advertised Cooperative Matrix and Float8 features plus `computeFullSubgroups`,
variable pointers, the Vulkan memory model and partially bound descriptors.
Each feature is enabled only when supported. Variable pointers require the
storage-buffer feature; device-scope memory requires the Vulkan memory model.
Float8 requires both Float8 feature bits and Cooperative Matrix support.
Absent features remain disabled: they are optional accelerators, not a new
RX-series restriction. Enabled bits are logged, without claiming which ML
model the SDK uses. Vulkan validation and real GPU comparisons remain required.

Restoring a missing Vulkan SDK DLL does not add features to an existing logical
device. If device setup was skipped at startup, the overlay reports **Restart
required** and preserves the selected method. Restart the game after repairing
the package. Replacing a DLL already loaded into a process also needs a restart;
retrying an effect does not unload that module.

FSR runtime failure uses FSR 3.1 for the current session while retaining the
selected FSR version/backend in `bbport.ini`. The counter reports the active
upscaler. The overlay offers an explicit retry; selecting another method also
retries with a new context. Resizing alone does not keep retrying a failed
dispatch. Restarting clears the session fallback. There is no automatic
second dispatch on an unclassified SDK/device error.

FSR 4.1.2 uses the public FidelityFX API from AMD's signed SDK 2.4 effect
libraries for both AMD Vulkan and AMD DX12. The older 4.1.1 methods remain.
The DX12 method still uses the existing Vulkan/DX12 texture and fence bridge.
Image preparation, color-space flags, motion vectors, jitter, host RCAS and
motion-cache behavior are unchanged by this integration.

`bin/fsr24/amd_fidelityfx_upscaler_vk.dll` and
`bin/fsr24/amd_fidelityfx_upscaler_dx12.dll` are pinned to AMD SDK 2.4.0,
commit `7539ae8b6568ad29ed78a753404351daf3b5cca7`. Packaging checks their exact
size and SHA256; `packaging/windows/fetch_fsr24_upscaling.py` restores the
original files. The SDK license is included in the Windows package.

The effect DLLs export the five public FFX functions. We call these exports
from their explicit module handles instead of using the central loader:
AMD's central loader first reuses a DLL of that name already in the process,
which might be the retained 4.1.1 DX12 runtime. Different folders alone would
not prevent that collision. The requested module path is checked after loading.
No internal driver exports, memory patches, model overrides or shader rewrites
are used by these 4.1.2 paths. The existing frame-generation path is separate.

Support checks enumerate the versions on the selected adapter. Creation uses
the exact queried provider ID and verifies the active context version and ID;
FSR 3 is never silently accepted as FSR 4.1.2. Missing package files, failed
runtime loading and unavailable GPU/driver providers remain distinct failures.
Launcher and overlay keep the four existing choices and saved preferences.

Successful 4.1.2 support queries are reused within one logical Vulkan device.
Methods have separate entries; runtime path, presence, size and modification
time changes invalidate an entry. Failed checks are not retained. An explicit
retry forces another query, and context creation still verifies the actual
provider version and ID. The launcher takes one driver fingerprint per family
snapshot; its results do not replace the checks in the game process.

The Windows 4.1.2 methods and 4.1.1 DX12 always use automatic exposure. The
overlay shows a disabled, checked control for them. It retains the saved
exposure preference for methods that allow changing it.

The upscaling API headers are scoped to its translation units. This standalone
topic retains the official frame-generation implementation and files. Shared
public headers, file verification and the headless-device fix are independent
foundation commits; the later SDK FG topic can start from that foundation.
RCAS/output fusion, target-cache retirement, SSBO, scaled motion visualization,
motion-target diagnostics and FSR Lite are separate topics.

For local comparisons retain the original 1.9.5+RCAS/cache build and use the
same copied save, camera and settings. Isolated tests establish API operation,
handoffs, switching and finite output; they do not establish movement quality,
absence of ghosting, or a game-performance improvement.

## Local validation findings, October 30, 2026

On an RX 9070 with Adrenalin 26.10.1, isolated checks exercise all four methods,
SDK checking, backend switches, repair/restart boundaries and delayed cache reads.
These checks do not establish gameplay image quality or performance.

Khronos validation layer 1.4.363 reports shader errors on the original SDK 2.4
Vulkan path. A test-only interceptor recorded 30 actual SDK shader submissions
and identified the calling, hash-pinned AMD effect DLL. Twelve modules failed
standalone SPIR-V validation; fourteen declared `CooperativeMatrixConversionsNV`.
The live validation also reports an internal `rw_reprojected_color` image write
with three components against a four-component view. This is not a clean Vulkan
validation pass. Shader bytes and models remain unchanged; no NV features are
forced. These findings need investigation with AMD and Khronos before release
approval.

The two DX12 methods, fsr4vk and the tested late-read/deferred cache cases have
no Vulkan validation errors after fixing a headless-only swapchain dependency.
This statement concerns their Vulkan work, not D3D12 debug-layer validation.

## Standalone review on Windows 1.9.7

The `feat/fsr-sdk24-upscaling` topic starts from official `b480526`.
Shared API headers, atomic size/SHA256 file verification and recursive package
manifests are in foundation commit `2943038`; the separate headless-device fix
is `238137e`. The independent `feat/mlfg-sdk24` topic starts at that second
commit without depending on the upscaling integration. The six common API headers and upscaling
header retain AMD's original blob IDs. The recursive manifest change is shared
with the standalone launcher-runtime topic.

The two extraction conflicts concern the runner and its readiness test. They
retain the current Windows Vulkan-driver diagnostic while removing obsolete
private-driver overrides. An incidental far-plane change in the old preference
commit is excluded; camera inputs and the existing SDR composition/RCAS remain
the official implementation. Seventeen SDK runtime/device/status source files
match the combined prototype exactly. FG sources, legacy headers and provider
files remain on the official implementation; this package adds only the two
upscaling SDK DLLs.

Fresh checks on RX 9070 / Adrenalin 26.10.1 pass the native build, nine CPU
programs and 151 Python tests in 21 separate processes. Generated overlay
catalogs match all 13 launcher languages. Thirty-one isolated runtime cases
cover four upscaling methods at 1080p/4K, live router changes, unavailable/failed
method rollback, debug input checking, preset changes, both DLL loading orders,
late-file repair/restart and both unchanged FG support queries. They do not
exercise the whole in-game composition, gameplay or FG interpolation.

Core/Synchronization validation is explicitly active; an intentional invalid
buffer proves message capture. Both DX12 paths and fsr4vk report no Vulkan
validation errors or synchronization hazards. The fresh SDK Vulkan capture
reproduces the findings above: 30 original AMD submissions, 12 failing separate
SPIR-V validation and 14 declaring `CooperativeMatrixConversionsNV`. This is
functional evidence with an open conformance issue, not a clean validation pass
or evidence that the issue causes ghosting, stutters or lower FPS.

The standard local Windows ZIP is checked for its manifest/CRC, exact SDK pins,
matching documentation and real frozen runtime. Existing DLSS files are retained
without a new NVIDIA test. Package/frozen checks remain distinct from the GPU
fixtures. No new game-performance, 1%/0.1%-low or movement-quality result is
claimed for this extracted topic.

## Overlay regression-test follow-up

The test-only follow-up restores the `runtime_restart` stub used by the PC
menu's separate translation unit and finds the settings window through its
stable `###settings` ID. The test observes restart requests and exercises
method selection, shared storage and restart boundaries through the actual
overlay widgets. Product overlay behaviour and SDK runtime sources are unchanged.

The shared foundation remains `2943038` plus `238137e`: common public API,
verified fetching, nested-runtime manifests and the separate headless fix.
The SR topic adds two upscaling DLLs; the FG topic owns its effect pin and
frame-generation headers/runtime. The nested manifest change matches PR #140.
The original AMD Vulkan findings above remain open; this follow-up does not
change shader bytes or turn the SDK SR topic into a clean conformance result.

## Follow-up correction, November 1, 2026

The follow-up uses Windows 1.9.7 (`b480526`) and retains the official FG code.
Neutral method hints replace the previous Vulkan performance recommendation
in all 13 launcher/overlay languages. First-choice ranking and saved choices
remain unchanged. Player instructions are separated from this technical record.

The SDK wrapper rejects SDK 2.3 or mismatched backend IDs at compile time and
logs its resolved module/source once. A failed newly created Vulkan context is
destroyed before publication if creation/provider verification fails. This
cleanup precedes any dispatch; it does not destroy potentially in-flight work
after a general dispatch failure. Seven vendor headers and all runtime pins
remain unchanged.

The native PC menu now uses shared upscaler selection and offers a localized
Retry FSR action during session fallback. Its existing 800 ms settling filter
remains in place; retry cannot be serialized as an upscaler value. Tests cover
browsing/cancel, delayed retry, stale actions and every FSR 4 version. Native
menu labels include the previously missing FSR 4.1.2 entry. Other existing
native menu translations/visibility policy are unchanged.

The initial wrapper's selection/request is recorded before its discovery, so
the first frame need not construct the wrapper again. A concurrent explicit
selection remains detectable. Rendering math, preparation passes, host shaders,
depth direction, jitter, sharpening and motion cache are unchanged.

Verification: full native Windows build; 11 CPU programs; 162 Python tests;
positive/negative ABI compile controls; generated catalog; 31 isolated runtime
cases; standard ZIP manifest/CRC/pins; reproduced stripped native binaries;
actual frozen launcher imports; relocated frozen UI in 13 languages and an
independent real packaged-helper check of all four methods.

RX9070 / 26.10.1 retains the previous SDK Vulkan findings: the 30 captured AMD
shader modules are byte-identical to the earlier capture; 12 fail static
SPIR-V validation and 14 declare NV conversion capability. Core/Sync validation
reports the same five VUIDs and no synchronization hazards. The draft remains
open on this limitation; the findings do not establish a gameplay symptom.
No new RX6000 or NVIDIA test is claimed. The current official-vs-PR game comparison is recorded below;
the isolated checks themselves are not a performance measurement.

## Current-game comparison, November 1, 2026

The RX9070 / Adrenalin 26.10.1 comparison uses official Windows 1.9.7 source
`b480526` and this corrected standalone PR, built with the same CLANG64 compiler,
RelWithDebInfo (-O2), LTO/PGO off and identical private measurement hooks. Each
copy uses its matching launcher/start scripts. Both start from the same fixed
Warehouse save; loaded camera matrices and projection match. This is a source
comparison, not a comparison of different release-binary compiler settings.

Settings: 4K/Quality, FSR4.1.2, RCAS0.50, motion cache/SSR/jitter on, LOD -2,
game AA/chromatic aberration off, FG/cap/VSync off. Each method has two captures
with 10 seconds warmup and 25 seconds recording, in Vulkan/DX12/DX12/Vulkan order.
Pooled results use complete application Present intervals; the first interval
at each phase boundary is excluded. No GPU queries, image readbacks or AMD
debug checking run during the comparison.

| Method | Official FPS | PR FPS | Change | Official 1% low | PR 1% low | Official 0.1% low | PR 0.1% low |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| FSR4.1.2 AMD Vulkan | 98.01 | 98.06 | +0.05% | 79.74 | 79.80 | 77.21 | 76.29 |
| FSR4.1.2 AMD DX12 | 97.05 | 97.01 | -0.05% | 78.50 | 77.87 | 75.11 | 75.12 |

The means are effectively unchanged in this scene. Each pooled method contains
about 4,900 frames and only five frames in its 0.1% tail; these short stationary
captures do not establish a movement/stutter improvement. The startup-only
counter confirms two wrapper initializations in official source versus one
after the bookkeeping correction; startup duration was not benchmarked.

The user reports no new subjective abnormalities in the requested movement,
camera and HUD checks for Vulkan and DX12 at 4K/Quality with FG off. Both copies
exit regularly. Main installation, settings and active saves remain unchanged.
Private measurement hooks and the fixed personal-save fixture stay local.
This follow-up adds no FG/HDR/cross-device image-quality certification and does
not close the original SDK Vulkan conformance findings. The PR remains Draft.
