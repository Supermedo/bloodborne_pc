# Neural rendering

Optional DLSS Neural Rendering is configured in **Graphics → Neural rendering**,
below Upscaling, and in the in-game Insert menu. It is separate from super
resolution and frame generation. It is off by default.

This branch builds on the Windows 1.8.2 integration with paced DLSS/FSR frame
generation and Reflex. Neural rendering has its own adapter and interface;
`bbport_dlss.dll` and its ABI-2 interface are unchanged.

## Runtime

The native Vulkan backend uses `bbport_nr_nvngx.dll` and a compatible,
user-supplied `nvngx_dlssnr.dll` beside `bin/bb-probe.exe`. Alternatively,
`BB_NR_RUNTIME` selects the folder containing the model. The proprietary model
is not part of the source tree. An absent or unsupported adapter/model leaves
the original frame available and reports its reason in the game log and menu.

Enable NR before starting the game so the renderer can request its Vulkan
extensions. Once initialized, the toggle hotkey (keypad minus by default)
switches it off/on. Model creation and tuning changes need a completed upload
submission; frames pass through while a model warms up. Old features and images
are released only after completion. Disabling NR does not change the FG factor
or the game's frame-rate settings.

The initial backend runs on the game's rendering GPU. Companion-GPU
offload is not supported.

## Controls

- Shared input/output: Automatic/Original/Optimized model preset, input scale, Auto/Residual/
  Ratio resolve, UI correction, and the toggle hotkey.
  Use Set… to bind a custom keyboard key or Clear to disable the shortcut;
  keypad minus is the default. Escape cancels key capture.
- Four configurable passes: enabled state, execution point, style, intensity,
  tone, structure, automatic skin mask and skin structure. Within each stage,
  passes retain their list order. Additional passes require more GPU work and
  model memory.
- Execution points: before super resolution, after super resolution, and in
  the final UI/display composition before frame generation.
- Auto uses direct resolve at full model size and residual resolve below full
  size. Residual transfers the reconstructed model edit onto the original
  full-size image. Ratio exposes transfer strength, colour strength, maximum
  luminance ratio and white point.
- Model presets: **Automatic (0)** uses the installed DLL's default model;
  **Original (1)** selects the original neural-rendering model; **Optimized (2)**
  selects the smaller FP8 model and requires NR runtime **310.9.16.0 or newer**.
  Optimized is disabled for older or unreadable runtime versions. A saved or
  manually edited preset 2 uses Original on those runtimes without changing the
  saved choice. Version detection honors `BB_NR_RUNTIME` and reads file metadata
  without loading the model. Replace a DLL while the game is closed and restart;
  choosing a model during play recreates the feature at a completed frame boundary.
  The three NR styles are independent of these model presets.
- UI correction preserves HUD pixels from the original composition; earlier
  stages already execute before the HUD. The source alpha channel is preserved.
  Frame generation always keeps the HUD separate, including when this switch is
  off, so its HUD-free scene reference cannot accidentally contain interface pixels.

Settings use the `nr_*` keys in `bbport.ini`; each pass uses
`nr_pass1_*` through `nr_pass4_*`. The launcher and Insert menu share the same
keys. Slider tuning is committed on release rather than recreating the model
on every mouse movement. `BB_NEURAL=0/1` overrides the saved enable setting.

Mask groups and the reference's extra visual effects are not included.

## Source and implementation references

The user supplied a Skyrim Upscaler AIO Build 16 installation as a reference for
the control layout, configuration semantics, pass chain and resolve behavior.
No Skyrim plugin or proprietary shader source is included here. That renderer
uses D3D12 interop; this integration uses Vulkan directly.

The native API route and parameter contract were compared with the
[OptiScaler NR implementation](https://github.com/wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass/tree/1bd3909/OptiScaler/dlssnr)
and its forwarder. The residual/ratio approach follows the public
[RenoDX](https://github.com/clshortfuse/renodx)/OptiScaler/Magpie lineage, with an
independent GLSL implementation and standard sRGB, Lanczos and OkLab mathematics.
The NR bridge source carries its own notices.

## Validation

See [NEURAL_RENDERING_VALIDATION.md](NEURAL_RENDERING_VALIDATION.md) for the
runtime-output, controls, gameplay and FG regression checks. Earlier FG
measurements in `GPU_VALIDATION.md` identify earlier executables.

This optional Windows backend was tested on an RTX 5080 with runtime 310.8.
The feature-18 interface is not part of the public NGX SDK: other runtime
versions and GPUs need their own validation. HDR game output and companion-GPU
offload have not been validated; this backend uses the rendering GPU.
