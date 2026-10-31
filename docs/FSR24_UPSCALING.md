# FSR SDK 2.4 upscaling on Windows

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
