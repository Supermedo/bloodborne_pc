# FSR 4.1.2 on Windows — AMD SDK 2.4

Choose **FSR 4.1 (experimental)**, then **FSR 4.1 Settings** in the launcher or
**Insert / L3+R3** overlay. FSR 4.1.2 provides AMD Vulkan and AMD DX12 methods;
the two FSR 4.1.1 methods remain available. Method changes apply live.

AMD lists Radeon RX 6000, 7000 and 9000 Series GPUs and selected Ryzen APUs
for FSR 4.1.2. Availability is checked for the actual GPU, driver, runtime files
and method. AMD DX12 also requires working Vulkan/DX12 resource/fence sharing.
See [AMD requirements](https://gpuopen.com/learn/amd-fsr-sdk-2-4-rx-6000-ryzen-apus/#requirements).

The package includes the original, signed SDK 2.4 effect libraries:

- `bin/fsr24/amd_fidelityfx_upscaler_vk.dll`
- `bin/fsr24/amd_fidelityfx_upscaler_dx12.dll`

The package manifest detects missing or changed program files. Restore the
complete matching package when a runtime is missing or damaged. Restart after
repairing a missing/previously loaded SDK DLL: retry cannot add features to an
existing Vulkan device or unload an already loaded module.

A runtime failure uses FSR 3.1 for the session while preserving the saved
version/method. The FPS counter shows the active upscaler. Use **Retry** in the
overlay, **Retry FSR** in the game's PC Graphics upscaler list when offered,
or choose another method. Restarting also clears the session fallback.
Motion-vector cache changes retain their restart requirement.

## Current Vulkan limitation

On RX9070 / Adrenalin 26.10.1, the unmodified SDK Vulkan path produces
SPIR-V/image-write validation findings. Their cause remains open. If AMD Vulkan
causes issues, use AMD DX12. These findings do not establish a specific gameplay
artifact or performance loss. Gameplay quality and performance depend on the
scene, hardware and settings; no universal fastest-method claim is made.

For a useful report, include GPU, driver, version/method, render/output sizes,
settings and the game log. Compare methods with identical settings/camera and
FG off first. Keep `BB_FSR24_DEBUG` off for normal play and comparisons.

Developer diagnostics, exact SDK pins and validation scope are in the source
repository under `docs/development/FSR24_UPSCALING.md`. This integration changes
upscaling; frame generation retains its independent implementation.
