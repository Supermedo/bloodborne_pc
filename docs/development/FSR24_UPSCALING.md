# SDK 2.4 upscaling: implementation and validation

## Runtime and API contract

FSR 4.1.2 AMD Vulkan and AMD DX12 use the public FidelityFX API from AMD's
original, signed SDK 2.4 effect libraries. The two FSR 4.1.1 methods remain.
DX12 uses the existing Vulkan/DX12 shared-texture and fence bridge. Preparation,
color-space flags, motion vectors, jitter, host RCAS and motion-cache policy
retain the existing behavior. Depth direction and far-plane handling are
separate work, unchanged by this migration.

SDK 2.4.0 commit `7539ae8b6568ad29ed78a753404351daf3b5cca7` supplies seven
unmodified API/upscale headers and separately pinned effect libraries:

- `bin/fsr24/amd_fidelityfx_upscaler_vk.dll`
- `bin/fsr24/amd_fidelityfx_upscaler_dx12.dll`

`packaging/windows/fsr24-upscaling.json` pins size, SHA256 and source Git blob.
`packaging/windows/fetch_fsr24_upscaling.py` verifies/restores original files;
the complete AMD redistribution license is packaged. SDK 2.4 include directories
are scoped to upscaling translation units. Compile-time guards reject SDK 2.3
headers and incompatible backend IDs. Separate SDK 2.3 headers remain for the
official FG implementation; this topic does not migrate FG.

The wrapper resolves all five public exports from an explicit module handle.
AMD's central loader may reuse the loaded 4.1.1 DX12 DLL with the same basename;
folders alone do not prevent that collision. The loaded path is verified and
logged once, including development override `BB_TEST_FSR24_DIR`. No internal
driver exports, shader rewriting or model overrides are used by these paths.

Support enumeration uses the selected adapter. Creation requests the queried
provider ID and verifies both ID and version. A new Vulkan context is published
only after verification, otherwise destroyed before dispatch. Missing files,
load failures and unavailable GPU/driver providers remain distinct failures.
That cleanup does not imply potentially in-flight work can be destroyed after
any dispatch error.

## Device features, caching and retry

On AMD, with the SDK Vulkan DLL present at device creation, supported Cooperative
Matrix, Float8, full-subgroup, variable-pointer, Vulkan memory-model and partially
bound descriptor features are enabled. Dependencies are checked: storage-buffer
pointers before variable pointers, memory model before device scope, and both
Float8 bits plus Cooperative Matrix before Float8. Unsupported features remain
disabled. These accelerators do not form an RX-series allow-list; logging enabled
bits does not establish which model the SDK selects.

Successful support checks are cached per logical device/method. Runtime path,
presence, size or mtime changes invalidate entries; failures are not cached.
Explicit retry re-queries, and each new context verifies its provider. Launcher
results never replace game-process checks. The first frame reuses the initial
wrapper; an explicit selection during discovery remains detectable.

Failure uses FSR 3.1 for the session while retaining the version/backend choice.
The counter shows the active upscaler. Overlay retry, native PC-menu **Retry FSR**,
another method or a new process can request another attempt. Resizing alone does
not keep retrying failed dispatches. The PC action observes the existing 800 ms
settle delay and never saves its sentinel. Native labels include FSR 4.1.2 and
translated retry text. Other menu localization remains separate.
The native list offers a new FSR 4.1 version only if a backend is supported.
Selection retains a usable saved backend, otherwise chooses the version's
supported preferred route. A saved unavailable choice stays visible; its explicit
retry retains that exact route. A stale unavailable list cannot overwrite it.

Repairing a missing SDK Vulkan DLL cannot add features to an existing logical
device. If setup was skipped, **Restart required** retains the selected method.
Replacing an already loaded DLL also requires restart; retry does not unload it.
Windows 4.1.2 and 4.1.1 DX12 use automatic exposure. Their overlay control is
disabled/checked while the stored preference for other methods is preserved.
Method advice is neutral in all 13 launcher/overlay languages; ranking and
stored selections remain unchanged.

## Diagnostics and Vulkan conformance

`BB_FSR24_DEBUG=1` enables SDK input checking for development only. Leave it off
for performance/image comparisons. SDK messages and total/aliasable effect-memory
queries are logged at context creation. These are not total process VRAM; an
unsupported query does not disable FSR.

RX 9070 / Adrenalin 26.10.1 with validation layer 1.4.363 reports findings on
the original SDK Vulkan path. A test-only interceptor identifies the pinned AMD
effect DLL and records 30 shader submissions: 12 modules fail standalone SPIR-V
validation; 14 declare `CooperativeMatrixConversionsNV`. An internal
`rw_reprojected_color` write supplies three components to a four-component view.
Repeated captures have byte-identical shaders. Core/Sync validation reports
five VUIDs and no synchronization hazards.

This is not a clean conformance pass. No unsupported NV features are forced;
shader/model bytes remain original. The cause and any integration contribution
remain unresolved. These observations do not establish ghosting, stalls or FPS
loss. Further isolation with AMD/Khronos is needed before declaring conformance;
the PR remains Draft. DX12 and fsr4vk pass Vulkan-work checks after the separate
headless swapchain-dependency fix; this is not D3D12 debug-layer validation.

## Regression scope

Build normal Windows targets plus `fsr24-api-test`, `fsr24-vulkan-features-test`,
`fsr-support-cache-test`, `fsr4-preferences-test`, `shared-graphics-test`,
`motion-history-test`, `fsr411-requirements-test`, `fg401-support-test`,
`fg401-preferences-test`, `shared-graphics-overlay-test`, `pc-menu-upscaler-test`.
ABI compile controls, Python launcher/package tests and generated 13-language
catalogs complement native tests. The overlay fixture uses the actual widgets.

The isolated runtime matrix covers four methods at 1080p/4K, routing, unavailable
and failed methods, input checking, presets, DLL load order, repair/restart and
unchanged FG support queries. It validates API operation and finite output,
not gameplay quality or FG interpolation. An intentional invalid buffer confirms
validation capture. Package checks cover exact pins, recursive manifests, CRC
and the actual frozen launcher in a Unicode path. Player help is packaged;
this technical document is excluded.

Shared foundations are `2943038` (headers, verified fetching, recursive manifest)
and `238137e` (headless fix). The manifest change is shared with #140. RCAS fusion,
target-cache retirement, SSBO, scaled motion view, diagnostics and Lite are
independent topics.

## Official-source comparison on November 1, 2026

RX 9070 / Adrenalin 26.10.1: official Windows 1.9.7 source `b480526` versus
this SDK topic, same CLANG64 RelWithDebInfo (-O2), LTO/PGO off. Matching start
scripts, identical saved Warehouse position and verified equal camera matrices
and projection; sources are compared with equal compiler settings.

Settings: 4K/Quality, RCAS 0.50, motion cache/SSR/jitter on, LOD -2; game AA/CA,
FG, FPS cap and VSync off. Per method: twice 10 s warmup + 25 s capture, in
Vulkan/DX12/DX12/Vulkan order. Present intervals are pooled excluding the first
at each phase boundary. No GPU queries, image readbacks or AMD debug checking
run during measurement.

| Method | Official FPS | SDK FPS | Change | Official 1% low | SDK 1% low | Official 0.1% low | SDK 0.1% low |
|---|---:|---:|---:|---:|---:|---:|---:|
| 4.1.2 AMD Vulkan | 98.01 | 98.06 | +0.05% | 79.74 | 79.80 | 77.21 | 76.29 |
| 4.1.2 AMD DX12 | 97.05 | 97.01 | -0.05% | 78.50 | 77.87 | 75.11 | 75.12 |

Mean FPS is effectively unchanged here. Each pooled method has about 4,900 frames
and just five in its 0.1% tail; movement/stutter improvements are not established.
The small Vulkan/DX12 gap is not an isolated measurement of bridge cost or a
universal ranking. Startup counts two wrapper initializations in official
source and one here; elapsed startup time was not measured. Short 4K/Quality
camera/movement/HUD checks with FG off reported no new visible issues. That
comparison does not certify FG/HDR/cross-device image quality or close V1.
