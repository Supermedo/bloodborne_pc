# Frame generation and Reflex

This Windows build adds DLSS frame generation, FSR frame generation and NVIDIA Reflex to the existing temporal upscalers. Custom resolutions retain centered HUD composition and output-space mouse coordinates.

Choose the generation backend and multiplier in the launcher's Graphics section. The same choices appear in the Insert menu. Generation and Reflex changes are saved for the next game start; reconstruction presets retain their existing behavior. Frame generation defaults to Off.

The six additions are:

1. **DLSS 2x generation:** one interpolated frame between source frames, when supported by the NVIDIA runtime.
2. **NVIDIA Reflex:** Off, On and On + Boost, with input/simulation/render/present markers and driver pacing when the required Vulkan extensions are available.
3. **FSR 2x generation:** an independent backend, usable with the existing reconstruction choices when its Vulkan requirements are met.
4. **DLSS 3x/4x choices:** always selectable, with the effective count bounded by the runtime capability. Unsupported generation falls back to ordinary presentation.
5. **Conservative object motion:** keeps the existing object-motion option, but rejects ambiguous instanced batches rather than matching their transform history by draw order. Camera motion remains available for these batches.
6. **Diagnostics and CPU regressions:** source/presented counters, timing logs, matching guide/output captures and regression coverage for cadence, history, settings, Reflex input-cycle gating and diagnostic parsers.

Generation uses completed source frames and skips intermediate presentation slots whose deadlines have expired. It seeds SDK history after scene changes or serial gaps. Depth, motion and camera data are captured with the matching display buffer; leased frame resources remain owned until their GPU work and presentation complete.

Reflex treats an interpolation group as one application frame. Ordinary present markers
surround the whole group, while out-of-band present markers surround each physical
presentation, including its real endpoint. This retains a single source identity across
all generated images, following Vulkan's [frame-attribution rules](https://docs.vulkan.org/spec/latest/chapters/VK_KHR_surface/wsi.html#lowlatency-frame-attribution).

Reflex bounds unfinished source work with a small queue of accepted flip tickets.
Each accepted flip allocates a monotonic ticket. For generation, the presenter releases
that ticket after the matching render/evaluation timeline completes, before waiting for
presentation deadlines. Ordinary and loading frames retire at the presentation endpoint.
Retirement is idempotent and never releases the frame or SDK resources: their separate
leases stay alive until GPU work and presentation complete. The source-work cap starts
at two. After two seconds of consecutive source periods within 2% of the selected
frame-rate cap, it tightens to one to reduce queued latency. Eight consecutive periods
more than 3% slower restore two, keeping the GPU fed under heavier load. Missing
generation, discontinuities and cap changes reset that history. Capacity changes retain
accepted tickets and retirement history. `BB_REFLEX_MAX_QUEUED` overrides the adaptive
policy in the range 0-4; zero disables the application gate.

Simulation advances at each accepted application-frame boundary, even when the guest
does not poll input. For EOP flips, IRQ registration and all graphics command-buffer
pairs are enqueued under the captured frame identity before the cycle closes and the
next cycle is paced. Driver sleep and the source-work gate precede the next actual pad
sample. Input markers and CPU sample timestamps are emitted only for real pad reads.
The guest flip/submission boundary is serialized across guest threads; GPU, IRQ and
presentation workers never take that lock. On the GPU thread, deferred draw recording
drains before a scheduler identity change, so the boundary flush cannot submit a buffer
that deferred workers are still populating.

## Settings

| INI key | Environment variable | Values | Default |
| --- | --- | --- | --- |
| `frame_generation` | `BB_FRAME_GENERATION` | `off`, `2x`, `3x`, `4x` | `off` |
| `fg_backend` | `BB_FG_BACKEND` | `dlss`, `fsr` | `dlss` |
| `reflex` | `BB_REFLEX` | `off`, `on`, `boost` | `on` |

FSR supports Off/2x; selecting FSR clamps a higher multiplier to 2x. Unsupported Reflex remains inactive. Generation currently bypasses HDR output and scenes without valid depth/camera guides.

## Diagnostics

- `BB_FG_TIMING=1`: logs observed source and presentation rates, generated presentations and skipped slots. Presentation calls are not proof of displayed scanout cadence; compare with PresentMon when GPU testing is permitted.
- `BB_REFLEX_TIMING=1`: samples available driver latency reports once per second.
  These are not input-to-photon measurements. CPU input/present records remain
  per frame. `BB_REFLEX_DRIVER_TIMING_FULL=1` restores per-frame driver queries
  for detailed diagnosis; those queries can affect pacing under GPU load.
- `BB_REFLEX_TIMING=1` also logs `reflex cpu` intervals from the first pad sample
  of a guest simulation cycle to its real-frame present call. The guest's flip
  request carries this identity through GPU and presentation workers. This
  driver-independent interval excludes OS input delivery, driver queuing,
  scanout and panel response; it is not physical input-to-photon latency.
  The frame boundary gives each accepted application frame its own identity, while
  a frame without a pad read has no input sample interval. Generated presentations
  do not emit these CPU records. Historical traces can contain several real presents
  of one sampled cycle, so the analyzer reports the first and last presentation
  separately (`cpu_first_present_ms`, `cpu_last_present_ms`) and counts repeated
  records. Exact duplicate queries are collapsed; repeated records are not
  independent input events.
- `BB_REFLEX_TIMING=1` also logs the `reflex gate:` backlog state (cap, pending
  depth, cumulative timeouts) and a `reflex backlog: wait timed out` line when
  the gate deadline (150 ms) expires and drops a stale entry.
- `BB_FG_TRACE=1`: logs per-source GPU completion, deadlines and present-call
  spans. Keep detailed tracing off during ordinary play.
- Both generation backends submit their composition copy before its presentation
  deadline, wait for copy completion, then precisely pace the presentation. Each
  uses a second graphics queue in the same family when available, so copies do
  not wait behind the next source frame's draw work. The shared queue remains
  the fallback. `BB_FSR_PRESENT_QUEUE=0` or `BB_DLSS_PRESENT_QUEUE=0` disables the
  respective independent queue. `BB_FSR_GPU_PACING=0` or `BB_DLSS_GPU_PACING=0` restores the respective
  backend's former CPU-before-copy timing for diagnostic comparisons.
- Reflex uses explicit monotonic submission IDs when presentation has an independent
  queue, which is excluded from render attribution. The shared-queue fallback uses
  render markers so older presentation copies can retain their captured identity.
  `BB_REFLEX_MARKER_ATTRIBUTION=0` forces explicit IDs and `=1` forces render markers
  for diagnostic comparisons.
- `BB_FG_CAPTURE_DIR=<folder>`: captures one generated frame once its rendered serial reaches `BB_FG_CAPTURE_FRAME` (default 120). Files are written in `frame_<serial>` after the draw timeline completes. Readback can affect performance; keep it off during pacing measurements.
- `BB_REFLEX_SUBMIT_TRACE=1`: compares accepted flip IDs with thread-local metadata and
  the scheduler's recorded ID at preparation and submission. This is a detailed diagnostic;
  leave it disabled for ordinary gameplay and timing comparisons.

Captures contain `depth.bin` (R32 hardware depth in 0..1), `motion.bin` (RG16 current-to-previous motion in render pixels), scene/UI/display snapshots, generated images and `guides_manifest.json`. `ui_rgba8.raw` captures UI before the final display shader, in the same encoding as the HUDless scene. `presented_N.raw` and `presented_real.raw` contain the fitted images after post processing. The manifest identifies render/output/presentation sizes and formats, frame IDs, jitter and camera parameters. Use its format fields when decoding raw channel order.

Analyze a capture with `python tools/windows/analyze_fg_guides.py <capture-folder>/frame_<serial>`. Other scripts analyze PresentMon CSVs, Reflex logs and process-memory CSVs; use their `--help` for accepted formats.

`BB_RENDER_RES` and `BB_OUTPUT_RES` are paired diagnostic overrides. Keep the
configured `output_res` consistent with that output when using them: startup HUD
assets are generated from the configuration. Normal launcher and Insert-menu
resolution choices already use the same configuration.

At non-16:9 output sizes, native-resolution scene targets must differ from the
UI viewport so that the existing renderer can distinguish scene and menu passes.
Off and Native AA request a guest scene target four pixels narrower and two
pixels shorter than the output, then compose at the requested output size.
The SDK guide allocation can round that requested height up. Native AA retains its
native-AA SDK mode. This adjustment does not apply to TAA, whose current path
requires identical input and output dimensions; the existing non-16:9 TAA menu
aliasing remains a known limitation.

## DLSS reconstruction and uncapped delivery

DLSS uses the full 0..2 sharpening slider through RCAS after reconstruction;
NGX's deprecated sharpness parameter is zero. Finer DLSS texture sampling also
applies to Native AA, and sRGB input views select linear-light precision.
These corrections add rendering work compared with the former reconstruction
path. FSR and TAA retain their existing sharpening and sampler policies.

With a zero source-FPS limit, uncapped PC frames can present between virtual
vblank notifications rather than snapping to the service clock. Fixed-timestep
presets, the default source cap and the precise capped-frame limiter retain their
existing behavior. See GPU_VALIDATION.md for the matched reconstruction controls.

Unknown DLSS bridge versions are discarded before any cleanup callback can inspect
the table. A Windows CPU test uses a guarded, truncated table to exercise this
path. Use the matching supplied adapter: the competing ABI-2 FG layouts are not
interchangeable.

## Validation and package

Compilation, 70 Python tests, ten native CPU suites on Windows, two shader/SPIR-V checks
and the ABI-2 adapter table pass. Hardware validation distinguishes source cadence
from displayed cadence, and CPU pad-sample-to-present diagnostics from physical
input latency. Latest-candidate gameplay, changing-load, memory and recovery
results are tracked in [GPU validation](GPU_VALIDATION.md). A runtime change
requires new hardware evidence before it is described as validated.

The package includes the paired ABI-2 `bbport_dlss.dll`, `nvngx_dlss.dll`, `nvngx_dlssg.dll`, existing FSR assets, diagnostic tools and license notices. NVIDIA SDK terms, original MIT bridge notices and GPL-3.0 notices for adapted bbhost code are retained.
