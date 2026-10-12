# DLSS quality and frame generation on Windows

## Changes

DLSS now uses the existing RCAS pass for the full sharpening slider range. The
NGX sharpening input is deprecated and ignored by current DLSS runtimes; previously
the usual 0–1 slider values did not reach RCAS, while FSR applied sharpening itself.
DLSS texture sampling now uses NVIDIA's recommended mip bias
`log2(render/output) - 1`, including DLAA. Linear input sampled through an sRGB
view uses the HDR precision path instead of the encoded LDR path.

The jitter sign remains positive. A real NGX test reconstructing a stationary
high-frequency pattern measured MSE 0.00017311 with positive jitter and 0.00474968
with negative jitter. Flipping the sign would make reconstruction
about 27 times worse in this test. These numbers validate the sampling convention;
they are not a measurement of Bloodborne's visual quality.

## DLSS model presets

The launcher Graphics page and the in-game menu expose **DLSS model preset**,
separately from the Quality/Balanced/Performance render scale. Select Auto or
A, B, C, D, E, F, J, K, M, L. The preset is passed to NGX for every quality mode;
the DLSS DLL selects the corresponding model without a separate model switch.

| Selection | Model |
|---|---|
| Auto | The DLL's default for the selected quality mode |
| A, B, C, D | Legacy hints; removed from current SDKs and may be substituted by the DLL |
| E, F | Convolutional (CNN); deprecated by NVIDIA |
| J, K | Transformer |
| M, L | Second-generation transformer |

Changing the model preset in game recreates the DLSS feature after its previous
GPU work completes and resets temporal and frame-generation history. It does not
change render resolution or require a game restart. The choice is saved as
`dlss_preset=auto|A|B|C|D|E|F|J|K|M|L` in `bbport.ini`;
`BB_DLSS_PRESET` overrides it (letters or native NGX numeric hint values).
Invalid and reserved values select Auto. A-D are retained for compatibility,
not as a promise that a current DLL contains those original models.
The real NGX regression test exercises every selection and switching between
them, and checks that CNN and transformer selections produce distinct output.
With the validated 310.7 DLL, A-D produce the same output as Auto, confirming
the legacy fallback. Model quality still needs comparison in gameplay.

NVIDIA's [SDK preset definitions](https://github.com/NVIDIA-RTX/Streamline/blob/main/include/sl_dlss.h)
describe removed and deprecated hints. NVIDIA also describes the
[second-generation transformer models](https://www.nvidia.com/en-us/geforce/news/dlss-4-5-dynamic-multi-frame-gen-6x-2nd-gen-transformer-super-res/).
See the DLSS SR programming guide below for integration details.

## Frame generation

Frame generation has two selectable providers:

- **DLSS FG ×2**, through the native Vulkan NGX FrameGeneration API. Availability
  comes from NGX's GPU/driver capability queries. The Windows package includes
  `bbport_dlss.dll` ABI 2 and `nvngx_dlssg.dll` from NVIDIA's release SDK.
- **FSR 3.1.6 FG ×2**, through the existing FSR-Vulkan public frame interpolation
  and optical flow APIs. It can be used with DLSS, FSR or native TAA.

The game renders at its original cadence. The presentation thread displays the
intermediate frame before the current real frame, with half-frame spacing after
GPU completion. Only real frames advance the game's flip count. The overlay shows
the display rate and, when FG is active, the render rate separately.

Depth, motion and the pre-HUD scene are copied into owned resources before the
game can overwrite them. Hardware depth is sampled into R32_FLOAT for FSR's API.
Windows BGRA output is converted to RGBA before interpolation, preserving channel
order and satisfying the FSR provider's supported formats. The HUDless scene goes
through the same color decoding and output passes as the
final image. A premultiplied UI coverage texture preserves the game's final HUD
pixels on generated frames. Resource reuse waits for the GPU and both presentation
fences. Menus without valid scene guides, history resets, unsupported hardware,
HDR output and generation errors continue with real frames. A generator error is
latched until a mode or size change to avoid repeatedly submitting a failing pass.

The accompanying swapchain fixes consume suboptimal acquisitions correctly and
reset image indices after recreation. The device-owned diagnostic marker buffer
is also released at shutdown.

## Configuration

1. Select **Output resolution: 2560×1440**. The default 1920×1080 output would be
   stretched again on a 1440p display, regardless of the selected DLSS preset.
2. Select **DLSS**, start with **Quality** or **DLAA**, enable **Sharpening (RCAS)**
   and try strength **0.3–0.5**.
3. Enable object motion vectors and use SDR output. Select **DLSS FG ×2** or
   **FSR FG ×2** in the launcher Graphics page or the in-game menu (`Insert`).

FG defaults to **Off**. `bbport.ini` accepts `frame_generation=off|dlss|fsr`;
`BB_FRAME_GENERATION` overrides it. Upscaling and frame generation are independent
choices. Selecting DLSS FG on unsupported hardware leaves normal rendering active
and reports the reason. This implementation generates one intermediate frame;
it does not expose 3×/4× multi-frame generation or NVIDIA Reflex integration.

## Validation and limits

The GPU regression tests execute actual provider shaders and read the output back:

- Real NGX super resolution, jitter convention and temporal reconstruction.
- FSR interpolation of a moving object: 18 intermediate frames land at the exact
  temporal midpoint, rather than repeating a real frame. HUD pixels remain exact.
- Explicit history reset, disable/re-enable and output resizing in both directions.
- Threaded recording with asynchronous submissions, 42 real Win32 swapchain
  presentations and window resize. Khronos core and synchronization validation
  report no Vulkan errors in the test environment.

The Windows archive includes `bin\bb-framegen-test.exe`. Run it from a terminal
with argument `dlss` to exercise DLSS FG without game files;
no argument tests FSR, and `window` also tests the Windows swapchain. Exit code 77
means the selected provider is unavailable; a passing interpolation test prints
`PASS` and exits with code 0.

The DLSS FG execution test was skipped because NGX reported the feature as
unavailable in the test environment. Its bridge compiles with MSVC, and the real
NGX capability query is checked. **Gameplay and DLSS FG still require a playtest**;
no game dump was available during validation. Synthetic GPU tests
cannot establish the absence of artifacts in every Bloodborne scene or guarantee
twice the frame rate. The archive also includes the previous crash-dispatch fixes
and Windows minidump capture; see [crash analysis](crash-analysis-windows-v1.5.md).

References: [NVIDIA DLSS SR programming guide](https://github.com/NVIDIA/DLSS/blob/main/doc/DLSS_Programming_Guide_Release.pdf),
[NVIDIA DLSS FG programming guide](https://github.com/NVIDIA/DLSS/blob/main/doc/DLSS-FG%20Programming%20Guide.pdf),
[FSR-Vulkan](https://github.com/FireBurn/FSR-Vulkan).
