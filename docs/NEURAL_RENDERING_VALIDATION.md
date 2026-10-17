# Neural-rendering validation

The integration is based on Windows 1.8.2 at `fce0878` and the FG/Reflex
integration at `fd0986c`. These checks cover the optional NR addition; they do
not replace the earlier FG validation.

## Runtime and controls

Tested on Windows with an test GPU, driver tested driver, and user-supplied NR runtime
310.8.0.0. Model SHA-256:
`e67dee209320cdafe0e93e45675d7aa34323a53acc57a72b2e40a181581c989a`.
The model is not distributed in this change.

The standalone Vulkan smoke program creates a pass in one submitted command
buffer, waits for its completion marker, evaluates it, and reads the actual
RGBA16F pixels. A game-image crop was tested for eight evaluations per setting.
Every tested output was finite and nonblack. Typed parameter Set/Get on
`DLSSNR.Intensity` round-trips exactly.

| Control | Pixel result |
| --- | --- |
| Intensity 0 | RGB is bit-for-bit identical to input |
| Intensity 0.5 | Mean absolute RGB change 0.00727 |
| Intensity 1, Style 2 | Mean absolute RGB change 0.01452 |
| Styles 0 / 1 / 2 | Three distinct outputs |
| Tone 0 versus 1 | Distinct outputs |
| Structure 0 versus 1 | Distinct outputs |
| Automatic skin mask and skin strength | Accepted, finite output; the crop does not establish skin segmentation quality |

Example reproduction after building the adapter and smoke target:

```text
nr_smoke.exe --runtime MODEL_DIRECTORY --bridge ADAPTER_DIRECTORY --size 256 --frames 8 --style 2 --intensity 1
```

`--input` accepts a raw RGBA16F image of exactly `size * size * 8` bytes.
The program saves input/output `.fp16` and `.ppm` files. Intensity-zero runs
accept identity; ordinary runs reject unwritten, nonfinite or unchanged output.

## Gameplay and FG

Pre-upscale, post-upscale and Present stages ran in gameplay, including native
model size, half-size residual resolve, Ratio resolve with a nondefault white
point, mixed-stage passes and a four-pass chain. Native-size and multi-pass NR
cost additional GPU time; the input-scale control provides a cheaper option.
The user also exercised the in-game controls and reported normal operation.

The following steady portions begin 70 seconds into separate 120-second runs,
after title/loading activity. PresentMon measured display intervals, alongside
the source/presented counters. All these steady portions retained game focus.
They use ultrawide output on a 120 Hz display, 60 FPS source target, Reflex on,
and object motion enabled. These are one-scene checks, not general performance
guarantees.

| Path | Source / presented median FPS | Display interval p99 |
| --- | --- | --- |
| NR off, DLSS 2x FG | 59.94 / 119.88 | 8.3761 ms |
| Pre-upscale NR, half size, DLSS 2x FG | 59.93 / 119.88 | 8.3868 ms |
| Post-upscale NR, half size, DLSS 2x FG | 59.94 / 119.88 | 8.3788 ms |
| Present NR, half size, DLSS 2x FG | 59.94 / 119.88 | 8.3761 ms |
| Present NR, UI correction off, DLSS 2x FG (review fixes) | 59.94 / 119.88 | 8.4063 ms |
| Present NR, UI correction off, FSR 2x FG | 59.94 / 119.88 | 8.3821 ms |

The FSR row also exercised the toggle hotkey: enabled → disabled → enabled
was saved correctly and the model rebuilt successfully. FG continued afterward.
The HUD remained separate even with NR's UI-correction switch off.

An absent-model run reported the missing DLL and continued gameplay with the
original scene and DLSS 2x FG. The optional feature did not prevent startup.
The final game binary also completed an NR-enabled gameplay run and closed
normally through the window-close event, with exit code 0.

Several exploratory runs included manual camera/menu interaction or lost focus;
they are used for functional observations, not these pacing comparisons.
Long-duration memory growth, HDR gameplay, other GPUs/drivers and companion-GPU
offload have not been measured here. Multi-frame factors above 2x retain their
existing implementation; this NR validation concentrates on 2x.

The existing SR/FG adapter remains ABI 2 and byte-for-byte unchanged:
`92d6523708162b456eb906bb891b9a75118b79470901f765b155d3a69449952b`.
NR has a separate ABI-1 adapter.

## Build and CPU checks

- CLANG64 game build and separate MSVC NR adapter/smoke build pass.
- 69 existing focused Python tests pass, with one platform-related skip.
- 22 new launcher tests pass, including construction of the Graphics page,
  pass-tab add/delete controls and colors, custom hotkey capture/clear/cancel,
  saved bindings and physical key mapping on non-US layouts.
- 19 shader-math reference tests pass, including actual GLSL matrix orientation,
  published OkLab colour values, residual interpolation, transfer-zero identity,
  white-point normalization and finite HDR math.
- Nine existing native suites, the NR settings suite and the existing rejected
  SR/FG-adapter test pass.
- Existing FG shaders and all three new NR shaders compile and pass SPIR-V
  validation. The frozen launcher/Tk/control construction smoke passes.

Broad cross-platform test discovery is not claimed green: the known Windows
environment limitations recorded for the base integration remain outside this
change. No online multiplayer session or HDR display test was performed.

The tab styling, shorter labels and custom-hotkey editor were updated in a UI
follow-up. The gameplay measurements above predate that follow-up; its game
build, launcher tests and rendered UI were checked without another GPU run.

## Runtime 310.9.16 model presets

The preset follow-up passes 24 launcher tests and native settings checks,
including all three preset values through save/load, the 310.9.16.0 version
threshold, old/unknown-runtime fallback and disabled Optimized controls. Both
Python and native version readers were checked against the installed 310.8 DLL,
the supplied 310.9.16 DLL, a DLL-file override and a missing runtime.

Four isolated Vulkan readback runs used 256x256 synthetic input, eight evaluations,
Style 0 and full intensity. All produced finite, nonblack, changed output:

| Runtime / preset | Output SHA-256 |
| --- | --- |
| 310.9.16 / Automatic (0) | `21be53566e503783068a3271d1bfdfb9672c975594861e4860c4d63f9543d547` |
| 310.9.16 / Original (1) | `c32889ed137020b37e731bb5e76b1269cee2522bf2920dde9e74fecf62803e5c` |
| 310.9.16 / Optimized (2) | `21be53566e503783068a3271d1bfdfb9672c975594861e4860c4d63f9543d547` |
| 310.8 / Original (1) | `c32889ed137020b37e731bb5e76b1269cee2522bf2920dde9e74fecf62803e5c` |

For this input, Automatic selects the same output as Optimized in the new DLL;
Original matches the old DLL exactly, and differs from Optimized. This verifies
model selection and output, not gameplay performance or a general speedup.
Earlier gameplay and FG results above used runtime 310.8; they were not repeated
for this preset follow-up. The smoke tool accepts `--preset 0`, `1` or `2`.

## Windows 1.9.1 rebase

The NR changes are replayed on the paced FG/Reflex integration over upstream
Windows 1.9.1 (`b4dcc82`), excluding the upstream FG implementation. The NR bridge,
shader and compositor code is unchanged from the preset follow-up. The 43 neural
Python checks pass (24 launcher checks and 19 compositor-math checks). The new
1.9.1 executable has not been gameplay- or frame-pacing-tested; the GPU results
above remain attributed to their older executables. See REBASE_2026-10-18.md.
