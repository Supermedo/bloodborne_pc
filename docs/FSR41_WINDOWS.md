# FSR 4.1 on Windows

The integration offers one **FSR 4.1 (experimental)** entry in **Upscaler**.
**FSR 4.1 Settings** lists four independently checked methods:

| Order | Method | Runtime |
|---|---|---|
| 1 | FSR 4.1.2 — AMD Vulkan | Packaged AMD SDK 2.4 Vulkan upscaler |
| 2 | FSR 4.1.2 — AMD DX12 | Packaged AMD SDK 2.4 DX12 upscaler, Vulkan/DX12 bridge |
| 3 | FSR 4.1.1 — AMD DX12 | Packaged AMD upscaler library, Vulkan/DX12 bridge |
| 4 | FSR 4.1.1 — Vulkan (fsr4vk) | Packaged fsr4vk provider and model |

Fresh settings use the first supported method in this order, otherwise FSR 3.1.
Selecting the family restores a supported remembered method first, then uses the
same order. Explicit saved selections are retained when support changes; the
launcher opens Graphics for an unavailable selection. It does not silently save
a replacement. Launcher and overlay share version/method in `bbport.ini`.

The family is enabled when any method is supported. Each method requires its own
positive preflight result. File presence alone does not establish support.
The 4.1.2 paths query available SDK providers for the selected device, then
verify the exact provider ID and version after context creation.
DX12 also requires matching Vulkan/DXGI adapter
LUID and shared memory/fences. fsr4vk checks device features separately from its
pinned DLL/license hashes. Its download button repairs missing or changed files.
No FSR 4.1 method is enabled solely from a GPU name or a driver version string.

Green means supported by preflight. Orange names missing driver functionality,
files or shared resources; red identifies an explicit GPU rejection. Gray means
checking, failed verification or a disabled provider. A failed check is not a
hardware exclusion. The renderer also validates context creation at the actual
render size. Provider activation is recorded in the game log.

Method selection in **Insert / L3+R3** applies live. If a requested method fails,
the running method can be restored and the overlay reports that outcome.
Motion-vector cache changes retain their restart requirement. All family/status
and help text is shared in the 13 launcher languages.

The 4.1.2 methods use the public SDK API, with separate effect libraries under
`bin/fsr24`. They do not use the previous internal driver ABI or import lookup
adjustment. [SDK pinning, loading and verification](FSR24_UPSCALING.md).
Installed-driver discovery contributes to support-cache invalidation; the
SDK can also expose official driver-provided versions. Existing profile, data-directory, save and cache
conventions remain unchanged. Actual support still depends on the selected GPU
and its driver.
Both ML-FG 4.0.1 and 4.0.2 remain separate choices with their existing RX 9000,
Windows 11, exact-provider and SDR requirements.

Regression fixtures cover failed loader retries, concurrent initialization, missing
API callbacks, provider-independent host sharpening and package validation.
