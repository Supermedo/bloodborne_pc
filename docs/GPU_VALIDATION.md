# Windows GPU validation

Frame-generation evidence is from an RTX 5080 on driver 617.14, FIFO presentation,
object motion enabled and copied saves in offline play, on the actual native ultrawide desktop
at approximately 120 Hz. Frame rates are PresentMon display intervals, not rates
inferred from a requested multiplier. The CPU interval below runs from a guest pad
sample to the real QueuePresent call; it excludes OS input delivery, driver
queuing, scanout and panel response and is a diagnostic, not input-to-photon
latency.

## DLSS reconstruction and uncapped delivery follow-up

The follow-up runtime is `66dc150`, probe SHA-256
`be8246e62a3a07f62b90cccb2c29b553de1a3ca8f4eac74616eab609ecdca0f5`.
The matching ABI-2 adapter and NVIDIA runtime DLLs are unchanged. The source
incorporates the reconstruction corrections and uncapped flip-delivery behavior
carried by #48; the source commits credit its contributor.

DLSS now uses its full RCAS slider range, finer texture sampling and linear-light
precision for sRGB input views. FSR/TAA sampling policies retain their previous
behavior. Uncapped PC frames can arrive between virtual vblank ticks rather than
snapping to the 480 Hz service clock. Fixed-timestep presets, the default source
cap, and the precise capped-frame limiter retain their existing behavior.

### Same-machine reconstruction comparison

RTX 5080, driver 617.14, the same copied offline save and stationary view,
DLSS Quality, FG Off, Reflex Off, Immediate presentation and sharpness 0.50.
Each run lasts 150 s; the final 60 s is measured. SDK input/output sizes match
across these runs. The submitted rates above the monitor refresh are throughput,
not full-frame scanout rates or input-latency measurements.

| Snapshot | Submitted FPS | Display-change p99 | p99.9 |
| --- | ---: | ---: | ---: |
| #48 `9495746`, two runs | 162.14-162.91 | 7.93-8.01 ms | 8.33-8.52 ms |
| Previous implementation `cc038d6`, three controls | 175.69-176.33 | 11.77-12.14 ms | 13.88-14.36 ms |
| Reconstruction-only control `c7975b4` | 165.49 | 13.04 ms | 14.00 ms |
| Follow-up `66dc150`, two runs | 164.56-164.78 | 7.98-8.01 ms | 8.24-8.41 ms |

The reconstruction-only control separates the additional reconstruction work
from the vblank scheduling correction. The final runtime recovers #48's interval
regularity and crisper stationary texture detail, while the finer sampling and
working sharpening cost throughput compared with the earlier, less expensive
reconstruction path. These are measurements of this scene and hardware, not a
universal speed or temporal-quality ranking.

### Frame generation and reconstruction checks

| Case | Source FPS (tail p50) | Display FPS | Display p99 / p99.9 |
| --- | ---: | ---: | --- |
| DLSS 2x / 60 cap | 59.98 | 119.98 | 8.372 / 8.426 ms |
| FSR 2x / 60 cap | 59.96 | 120.05 | 8.373 / 8.411 ms |
| DLSS 2x / 30 cap | 29.99 | 60.00 | 18.628 / 19.157 ms |
| FSR 2x / 30 cap | 29.99 | 60.00 | 18.552 / 19.291 ms |
| DLSS 3x / 40 cap | 39.98 | 119.98 | 9.637 / 10.273 ms |
| DLSS 4x / 30 cap | 29.96 | 120.00 | 9.525 / 10.492 ms |
| DLSS 2x / GPU constrained, 60 cap | 52.66 | 105.20 | 12.865 / 35.447 ms |
| DLSS 2x / 6K Balanced, 60 cap | 59.95 | 119.65 | 11.579 / 15.700 ms |
| DLSS 2x / Native AA, native ultrawide | 59.96 | 119.88 | 9.876 / 10.201 ms |
| DLSS 2x / matched 1440p | 59.97 | 120.01 | 8.371 / 8.392 ms |
| Installed DLSS 2x / 60 cap | 59.97 | 120.01 | 8.373 / 8.396 ms |
| Installed FSR 2x / 60 cap | 59.98 | 120.00 | 8.371 / 8.396 ms |

Generation/source ratios were checked from the final 30 pacing reports, alongside
PresentMon intervals, retained foreground ownership and short-window memory growth.
The native-AA case retained SDK quality mode 0. Three separate Quality/60 FPS
runs loaded sharpness 0, 0.5 and 2 at startup. Matched wall-region normalized
gradients were 0.06025, 0.06251 and 0.10117, with nearly constant mean luminance;
root inspection confirmed progressively stronger edges and normal world/HUD
composition. These are functional slider checks, not an objective quality ranking.

The runtime passes 70 Python checks (one optional dump-dependent check skipped),
ten native CPU suites on Windows, two shader/SPIR-V checks and the matched ABI-2
adapter table. The new guarded-table CTest rejects an unknown bridge version
and repeats cleanup without initializing a Vulkan device or NGX.

The installed checks launched the actual staged executable and matching DLLs.
The prior one-hour endurance and CPU latency evidence below retains its older
executable identity; it is not a one-hour or physical-latency validation of this
follow-up build.

Derived measurements: [comparison CSV](data/dlss-sr-fg-comparison-2026-10-15.csv).

The 8K configuration exceeded this 16 GB card's VRAM budget and spilled into
shared GPU memory. The working set plateaued, but source FPS fell to about 10.
It is retained as a capacity-limit case, not a successful 30-60 source-FPS test.
The constrained-load case is interpreted only after confirming that its working
set fits memory. Rare longer intervals remain: its display p99.9 is 35.45 ms.
No physical display modes were changed.

The stock #48 FSR run at the recorded snapshot steadily accumulated private and
GPU allocations and degraded over its 180 s run. Follow-up FSR allocation growth
is recorded separately in the CSV. The stock and follow-up 4K settings differ in
FG viewport fitting, so their memory sizes are not pixel-matched. The dedicated
1440p DLSS comparison uses equal generation dimensions. Missing provider
transient-retirement hooks are a source-supported explanation for the stock
FSR growth; no isolated patched-#48 A/B was performed.

Raw game data, saves, logs and captures remain local. The derived table records
source/probe identities, windows, cadence, memory and focus conditions without
machine paths. Still images and stationary pacing tests do not establish
sustained-combat image quality, online compatibility or physical input latency.

## Tested source identities

The core pacing, latency and endurance results below used runtime commit
`0a1ffa3`, probe SHA-256 prefix `295d3c67`. The integration build at `e09ff36`
(probe `36999455`) incorporates Windows v1.7.5, including its HUD layer and
launcher updates. Its focused regressions are recorded separately below; the older hour-long runs
are evidence for their exact executable, not this rebuild.
The frame-generation queue and cadence algorithms are unchanged by the merge.

Reflex On is the recommended tested mode. Twelve of thirteen bounded cases
passed their original criteria. The FSR 2x/60 FPS Reflex-Off control retained
120 displayed FPS but missed the strict 10 ms display-p99 criterion: 10.462 ms,
and 10.415 ms in a 300 second repeat. That limitation remains a failed control.

## Bounded measured results

Native 60 FPS source, 180 s, Reflex On, independent queue, explicit-ID
attribution (`out/posthoc/grouped-native-dlss-report.json`,
`out/posthoc/grouped-native-fsr-report.json`):

| Backend | Source FPS | Display FPS | Display p50 / p99 |
| --- | ---: | ---: | --- |
| DLSS 2x | 59.99 | 120.01 | 8.342 / 8.372 ms |
| FSR 2x | 59.99 | 119.95 | 8.342 / 8.373 ms |

The adaptive gate settled on the one-deep depth in both native runs (both depths
observed, ending on one) with no watchdog release.

Variable load, 420 s, live preset changes, both phases
(`out/posthoc/grouped-range-dlss-report.json`,
`out/posthoc/grouped-range-fsr-report.json`):

| Backend | Phase | Source FPS (p50) | Display FPS | Display p99 |
| --- | --- | ---: | ---: | ---: |
| DLSS | heavy | 45.6 | 90.9 | 14.56 / 14.50 ms |
| DLSS | performance | 59.9 | 119.8 | 9.71 / 9.63 ms |
| FSR | heavy | 42.6 | 84.8 | 16.69 / 17.12 ms |
| FSR | performance | 59.9 | 119.8 | 9.68 / 9.63 ms |

The phase columns list the two heavy windows and the two recovery windows. Both
backends returned to about 119.8 displayed FPS with no skipped frames and no gate
timeouts inside a phase. The FSR range run had one Reflex backlog watchdog
release, located at 14.46-14.66 s, before the first Continue pulse
(`out/posthoc/grouped-fsr-watchdog-location.json`); steady phase checks remain
strict.

A heavy 7680x4320 DLSS run held source p50 45.6 FPS at 90.85 displayed FPS
(display/source 1.99) with display p99 14.43 ms and no gate timeout
(`out/posthoc/grouped-heavy-report.json`).

Source-cap and multiplier cases, 150 s each, from the same audit:

| Case | Source FPS | Display FPS | Display p99 |
| --- | ---: | ---: | ---: |
| DLSS 2x, Reflex On, 30 cap | 29.99 | 60.00 | 18.63 ms |
| DLSS 2x, Reflex Off, 30 cap | 30.00 | 60.00 | 18.14 ms |
| FSR 2x, Reflex On, 30 cap | 29.99 | 60.00 | 18.60 ms |
| DLSS 3x, 40 cap | 39.97 | 119.98 | 9.51 ms |
| DLSS 4x, 30 cap | 29.98 | 120.00 | 9.51 ms |

The 3x and 4x cases logged presented/source ratios of 3.00 and 4.00.

Whole-mode Reflex comparison, CPU sample-to-present interval over the final 30 s
of matched scene pairs (`bounded-candidate-audit.json`, plus
`out/posthoc/grouped-boost-report.json` for the Boost case):

| Pair | On (p50 / p95 / p99) | Off (p50 / p95 / p99) |
| --- | --- | --- |
| DLSS native 60 | 27.02 / 29.13 / 29.81 ms | 50.55 / 51.43 / 51.80 ms |
| FSR native 60 | 26.67 / 29.64 / 30.54 ms | 57.55 / 58.43 / 58.68 ms |
| DLSS 30 cap | 85.20 / 86.10 / 86.80 ms | 118.58 / 119.62 / 120.13 ms |

Reflex On lowers the interval against its matched Reflex-Off control in all three
pairs. Boost (DLSS native 60) measured 26.33 / 29.08 / 29.74 ms, no meaningful
change from On. These are whole-mode intervals, not vendor-only or
input-to-photon figures.

## Design notes

The independent presentation queue belongs to the same queue family as the
graphics queue, so presentation copies can bypass the next source frame's draw
work without changing image ownership rules; the shared queue remains the
fallback. The adaptive depth starts at two, engages one only after sustained
cap-tight cadence to remove roughly a frame of queueing, and returns to two when
the source is measurably GPU-limited. In the 30 FPS and synthetic range cases,
this feedback occasionally alternates between one and two: tightening the gate
slows the measured cadence enough to expand it again. The 30 FPS DLSS case spends
about 11% of its time at one, so its CPU interval improvement is smaller than at
native 60 FPS. This is a bounded policy limitation, not evidence of sustained
one-frame queuing at every cap.

A targeted identity trace (`519f471`, probe `d38df55f`) compared 8,665 prepared
frames including 6,237 with generation guides: the scheduler identity matched the
accepted flip in every case while 6,236 generated frames carried stale
thread-local metadata. That supports carrying the accepted flip identity into
preparation (`out/posthoc/identity-trace-report.json`) and does not justify a
scheduler identity handover or a locking refactor. The later grouped-marker
change is included in the current bounded hardware results; inconsistent driver
GPU-end reports remain excluded from latency evidence.

Build and CPU regression checks collected with this freeze pass: 58 Python tests,
nine native suites, two shader/SPIR-V checks and the ABI-2 adapter table
(`out/fg-validation.json`). They say nothing about GPU cadence.

## Targeted validation and captures

Five 150 second core/synchronization-layer runs used the frozen candidate:
DLSS 2x and FSR 2x at 30 source FPS, DLSS-SR with generation disabled, and DLSS
3x at 40 / 4x at 30. The unused-attachment `08912` report and the targeted
attachment-LOAD read hazard are absent in all five. Captured depth is finite and
inside 0..1; motion is finite with no displacements above half the render width.
Generated and real presentation buffers have distinct hashes at every multiplier.
The full-world, hunter and HUD compositions were visually inspected. These
instrumented runs establish no pacing or image-quality guarantee for other scenes.

The renderer is not validation-clean. Legacy shader-interface reports and
`WRITE_AFTER_PRESENT` occur with generation disabled too. DLSS-SR's clear-to-blit
`READ_AFTER_WRITE` also reproduces in that control. DLSS FG additionally reports
`VkBufferDeviceAddressInfo-buffer-02601` for an NGX buffer usage; this warning is
absent from the generation-disabled control and is not classified as inherited.
The marker/acquire/present threading reports conflict with the explicit concurrent
swapchain-use exception in the [Vulkan marker specification](https://docs.vulkan.org/refpages/latest/refpages/source/vkSetLatencyMarkerNV.html).
Destroying the swapchain remains separately synchronized. Reports remain enabled
and are retained locally (`out/posthoc/grouped-layer-guide-report.json`).

## Endurance and limits

Both native hour-long runs completed 3600.33 s and verified minimize/restore
(11 s DLSS, 10 s FSR). The 1900-3400 s quiet window contains 300 process-memory
samples per backend, after the movement/loading sequence and window recovery.
Generation remained active at the endpoint, with no skipped slots in its final
60 samples. The world and HUD look normal after restoration.

| Backend | Quiet display FPS / p99 | Final recovery p99 | Private memory slope | Dedicated GPU slope |
| --- | --- | ---: | ---: | ---: |
| DLSS 2x | 119.99 / 8.370 ms | 8.370 ms | -0.004 MiB/min | 0.000 MiB/min |
| FSR 2x | 119.99 / 8.378 ms | 8.777 ms | +0.072 MiB/min | 0.000 MiB/min |

Private-memory early/late quiet medians differ by 0.32 MiB (DLSS) and 0.47 MiB
(FSR). Shared GPU memory is effectively flat; handles decrease. These runs show
no sustained memory growth in this window, not a universal absence of leaks.
Separate eight-minute runs on the same executable visually confirmed an enemy
attack, death, loading and respawn for both backends. A further FSR run on a
fallback desktop crashed during respawn in the guest game's heap-free path at
`0x20859c7`. A native-resolution FSR 2x repeat and the FG-disabled control both
completed death/respawn without reproducing it. Its cause remains unresolved;
these successful repeats do not establish that the fault is fixed or unrelated
to generation.

## Integrated build checks

The integrated executable retains the core binary from `e09ff36` (probe
`36999455`). The launcher-script follow-up `bc194dd` changes only unpatched-game
UI fallback and whitespace parsing for HUD position; its normal patched 1.09
path is unchanged. The script-only follow-up `7551c8b` separates native scene
targets from non-16:9 UI viewports, preventing inventory atlas corruption with
upscaling Off and Native AA. It preserves the requested output and SDK native-AA
mode, with a four-by-two pixel input adjustment. TAA retains its exact-size
contract and its existing non-16:9 menu aliasing limitation. CPU checks pass: 70 Python tests (one optional game-dump check
skipped), nine native suites, two shader/SPIR-V checks and the ABI-2 adapter
table. Frozen launcher graphics-control checks pass too.

Clean fixed-cadence regressions, reconstructed to the native ultrawide desktop,
Reflex On and object motion enabled:

| Mode | Source cap | Display FPS | Display p99 |
| --- | ---: | ---: | ---: |
| DLSS 2x | 60 | 120.00 | 8.52 ms |
| FSR 2x | 60 | 120.01 | 8.37 ms |
| DLSS 2x | 30 | 60.00 | 18.59 ms |
| FSR 2x | 30 | 60.00 | 18.65 ms |
| DLSS 3x | 40 | 119.99 | 9.61 ms |
| DLSS 4x | 30 | 120.00 | 9.63 ms |

The 60-cap cases ran 180 s with the last minute measured; the others ran 150 s
with the last minute measured. All held independent flip and passed their
original cadence/interval criteria, with no expired slots in the measurement
tail. An eight-minute integrated FSR run also visually confirmed an enemy attack,
death and respawn, including a full-width death backdrop and centered HUD.
Both 300 s changing-load runs passed as well. DLSS measured 45.8 source /
91.79 displayed FPS in its first heavy window (p99 14.46 ms), recovered to
119.78 displayed FPS (p99 9.88 ms), and returned to 45.8 source / 90.33 displayed
FPS (p99 16.29 ms). FSR measured 42.7 source / 85.72 displayed FPS (p99 16.17 ms),
recovered to 119.81 displayed FPS (p99 9.79 ms), and returned to 42.7 source /
85.36 displayed FPS (p99 16.74 ms). These are synthetic reconstruction-load
changes fitted to the native desktop, not a change to the physical display mode.
No measured phase contained expired slots or a backlog timeout; FSR had one
expired slot during a provider transition. The nine integrated cases pass their
original criteria. Two interrupted FSR attempts remain excluded and preserved.
Three 135 s automatic-resolution checks on the installed package verified clean
inventories and output-space Insert overlays with upscaling Off at a custom
ultrawide output, Off at the named 32:9 preset with edge-positioned HUD, and DLSS
Native AA with 2x generation. The Native AA case retained SDK quality 0 / preset 0
and active generation. These used the script fix `7551c8b`, whose changes do not
affect the earlier Balanced/preset or 16:9 changing-load cases. Two final 150 s runs from the actual installed stripped executable also passed.
DLSS measured 119.95 displayed FPS / 8.375 ms p99; FSR measured 120.01 FPS /
8.375 ms p99 in the post-120 s measurement windows. Both held about 60 source
FPS, a 2.00 presented/source ratio, full foreground ownership, an adaptive gate
settled at one and no gate timeouts or skipped slots in the measured tail.
Their output captures were visually inspected. No hour-long memory run has been
repeated on this integrated executable.

The Reflex comparisons measure the whole implemented mode, meaning driver pacing
plus the bounded accepted-source gate, rather than the isolated vendor Reflex
contribution; no isolated vendor latency benefit has been demonstrated. The CPU
sample-to-present interval is a diagnostic and is not input-to-photon latency.
Driver GPU-end reports are inconsistent with the application's logical grouping
and are not used as evidence. HDR generation, other GPUs and drivers, online
co-op, sustained combat, travel between regions and multi-hour stability remain
unestablished. Raw logs, saves and captures stay local.
