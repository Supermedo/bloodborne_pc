# SDK 2.4 ML frame generation

ML-FG 4.0.2 uses the public FidelityFX API from the original, signed SDK 2.4
`bin/fsr24/amd_fidelityfx_framegeneration_dx12.dll`. The previous route obtained
4.0.2 through an older effect DLL's automatic driver-provider integration; it
already used the public FFX API. The new route pins the current effect DLL.

The existing Vulkan/DX12 shared-texture/fence bridge, compute overlap, frame
slots, SDR 2x presentation and HUD composition remain unchanged. This does not
add direct Vulkan FG or AMD's new swapchain/frame-pacing component. Upscaling
is an independent change and keeps its existing four methods.

The pinned DLL comes from SDK v2.4.0, commit
`7539ae8b6568ad29ed78a753404351daf3b5cca7`; packaging verifies size and SHA256.
`packaging/windows/fetch_fg402_sdk24.py` restores that exact file. Its pin is
independent from the upscaling pin. The existing SDK 2.4 license covers it.

We load the effect by explicit module path and resolve its five public FFX
exports. The path is checked to avoid reusing the retained, same-named 4.0.1
DLL beside the executable. Provider enumeration uses the selected D3D12 device.
The chosen 4.0.2 provider ID must be returned by the SDK, and the created context
must report both that ID and version 4.0.2. Other FG versions are not accepted.

The calls remain Configure -> PrepareV2 -> Generate. Camera position/basis,
pixel-space motion vectors, jitter, per-context frame IDs, reset flags and
the resource/fence lifecycle are preserved. Creation descriptors remain alive
for the context lifetime. The API version is 4.0.2 for the SDK route and stays
4.0.1 for the retained old DLL.

Both versions retain independent support results and saved choices. Existing
requirements stay in place: Windows 11, RX 9000, Shader Model 6.6, Vulkan/DX12
adapter matching and the existing memory/fence/queue checks. HDR still pauses
FG. Missing 4.0.2 files do not disable 4.0.1. Unavailable or failed 4.0.2 uses
the existing session fallback while retaining the saved selection.

SDK 4.0.2 registers error/warning messages for the frame-generation effect.
`BB_FSR24_DEBUG=1` additionally enables the SDK input checker; it remains off
by default. A GPU-memory query runs once after each verified context creation.
Its total/aliasable byte counts describe the SDK effect, not process VRAM or
the external shared-texture bridge. Unsupported diagnostic configuration or
memory queries are logged and do not disable FG. The legacy 4.0.1 path is
unchanged. Keep the checker off for performance and image comparisons.

Isolated checks cover exact provider identity, both module-loading orders,
moving synthetic input, 1080p/4K output, serial and overlapping operation,
repeated 401/402 changes, three in-flight slots, resets and error paths.
They do not establish game performance or visual quality during gameplay.
## Standalone review on Windows 1.9.7

This topic starts at shared foundation `238137e`: public SDK API headers,
verified effect-file fetching, nested manifests and the headless-device fix.
It adds only the SDK frame-generation effect. FSR 4.1.2 upscaling retains the
official driver-provider paths; SDK SR, RCAS/cache optimisations, SSBO and
scaled motion-view changes belong to separate review topics.

The SDK effect retains the existing DX12 interop bridge and 2x SDR output.
Isolated GPU and package checks do not establish gameplay quality, real
presentation pacing, RX 6000/7000 support or performance gains. Keep input
checking disabled for measurements. Review the shared foundation once when
combining the independent SDK SR and FG topics.
