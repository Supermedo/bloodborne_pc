# Respawn loading on Windows

The initial estimate was **3–4 seconds after death**; the first marked capture
measures **13.84 seconds**. Loading through **Continue** is faster; teleportation
does not show the reported delay.
The supplied 1.09 executable contains a **12-second minimum loading timer** enabled
by the player-death flag. This explains a concrete difference between respawn and
Continue and closely matches the quiet interval in both captures. The Windows
loader now removes that minimum while retaining the game's readiness checks.
The user confirmed that the fix works well. A timed post-fix capture is not yet
available; neither capture below used this fix.

## Executable analysis and fix

The supplied `eboot.bin` reconstructs to the supported CUSA03173 1.09 loaded image,
SHA-256 `071df19c8880086d97182dbc057bc8cb37badaca57d9112683836b24a0444c0a`.
Addresses below are offsets in that loaded image, not offsets in the SELF file.

| Offset | Observed behavior |
| --- | --- |
| `0x18c4cb7` | The player branch of the character death handler sets `GameMan + 0x1522`. |
| `0x1944c20` | The map transition copies that flag into its request at `+0x94`. Subsequent copies place it at world-step `+0xe4`. |
| `0x1938b59` | Loading initialization tests the flag. If set, it loads **12.0 seconds** from `0x49280c8`; otherwise it uses zero. The countdown is stored at world-step `+0x278`. |
| `0x19387d9` | The world-step update decreases the countdown by frame delta time and clamps it at zero. |
| `0x193a581` | Loading cannot advance while the countdown is positive. The next branch independently requires the characters to be ready. |

The patch replaces only the eight-byte float-load instruction at `0x1938b64`
with a zeroing instruction and padding. It runs once, after selected XML patches
and before guest memory protection/execution. It changes the in-memory executable;
it does not edit the source `eboot.bin` or require preparation-cache deletion.

The loader checks the full initialization block, the countdown/readiness branches
and the original 12.0 constant before writing. Modified or unsupported instructions
cause a logged skip with no writes. Reapplying the same patch is harmless.
The death flag itself, the death presentation, resource-loading steps, character
readiness and the separate **15-second multiplayer timeout** remain unchanged.
There is no per-frame host hook or added gameplay polling.

Enabled by default. The game log reports:

```text
Respawn loading: 12-second minimum delay removed; readiness checks preserved
```

For an original-behavior comparison, put `BB_RESPAWN_DELAY_FIX=0` in the launcher's
Advanced environment overrides, restart, and remove it to enable the fix again.
Actual loading can still take time when resources are not ready; no new respawn
duration is promised before a gameplay comparison.

The native regression executes the original and patched initialization instructions
and the retained readiness branches through a Windows ABI adapter. It verifies
both death-flag values, the unchanged 15-second timeout, neighboring state, rejection
of truncated or modified images, an eight-byte-only change and repeat application.
It also passed against the supplied loaded image. A local replay is unavailable
because the other game assets are not present.

Validation: **34 native checks passed**, with one DLSS Frame Generation capability
check skipped. Loader integration also passed for default application, explicit
disable, conflicting instructions and an identical external XML write. None of
the 61 literal built-in 1.09 XML patches overlaps the guarded locations.

## First capture: October 9

`loading-20261009-005030-519237.zip` has SHA-256
`beb81468e5a4af742111d4e4f3464be16d834f1e41e8969f858450c4a40dce65`.
The user confirmed that marker 3 was pressed on the loading screen and that this
was an ordinary respawn at an unlocked lamp, not the first death tutorial.

| Marked interval | Capture seconds | Elapsed |
| --- | --- | --- |
| Continue | 19.2571–24.9317 | 5.6746 s |
| Death loading screen to controllable character | 49.9068–63.7461 | 13.8393 s |

The nearest process-counter samples show approximately 884.4 MiB of logical reads
in the Continue interval and 497.3 MiB in the respawn interval. Between samples
50.9778 and 60.9510, only approximately 1.48 MiB of reads completed. Active resource
post-processing clusters around capture seconds 50 and 62. The character and cloth
threads are recreated around second 50; recreating them does not take the whole
remaining interval.

The native frame log contains 6,583 flips over 55.1821 seconds. Its long stretch of
loading-screen rendering remains around 120 flips per second. Every logged GPU
buffer readback combined takes **9.41 ms** across the whole session. GPU tick waits
are at most 0.4% in the printed five-second report windows. These measurements do
not support a multi-second GPU queue/readback stall as the main explanation.

A quiet I/O counter interval does **not** distinguish a timer, a game task waiting
for a dependency, and an outstanding blocking file operation. There is no per-call
file latency or game instruction snapshot in this report. The guest wait table also
hits its 512-entry capacity in the first window, so absence of another wait site is
not proof that no such wait happened. No synchronization or timer bypass was applied.

`BUILD-INFO.txt` identifies an older Axis Order package. This is package metadata;
it does not verify the actual running executable if files were updated separately.
The game log reports PE timestamp `6ac7f5eb`. None of the later keyboard handoff or
command-data crash changes targets the game's respawn state machine.

The collector spends 45.45 wall-seconds inside sampling calls during a 69.90-second
session. This is elapsed time, **not** collector CPU time or proof that the collector
caused the loading delay. It nevertheless exposes avoidable diagnostic work: a
system-wide process/thread census on every sample and repeated queries for unnamed
threads. Schema 2 limits the census to twice per second and empty-name retries to
three per thread, while retaining approximately 100 ms counter samples and 10 ms
marker polling. Maximum sampler call duration is recorded as well. New threads can
take up to one census interval, approximately 500 ms, to appear in the report.

The reports contain guest offsets but no instructions. The executable supplied
after these captures enabled the code analysis described above.

## Second capture: optimized collector

`loading-20261009-010932-812147.zip` has SHA-256
`7fdf612e4033b9edc71888c5b790e647f151de541b91e26261b4e88d234431b9`.
Its schema 2 collector hash matches the R2 add-on. Package metadata identifies the
Vulkan command-data crash fix; the logged PE timestamp `6ac80a95` also matches the
local executable in that package. This still does not substitute for hashing the
actual executable used on the reporting computer.

| Marked interval | Capture seconds | Elapsed |
| --- | --- | --- |
| Continue | 26.3314–29.9489 | 3.6175 s |
| Death loading screen to controllable character | 61.0819–74.6958 | 13.6139 s |

The respawn pause persists with R2: mean wall time inside a sampler call falls from
68.05 ms to 13.51 ms, while the marked respawn still takes approximately 13.6 seconds.
These are elapsed sampler call times, not CPU measurements. Different package
versions, caches and manual marker timing also prevent treating the two captures
as a controlled performance benchmark.

Nearest process samples show approximately 317.8 MiB of logical reads during
Continue and 266.8 MiB during respawn. The read-byte counter does not change between
capture seconds **61.8278 and 73.2525**, an **11.4247-second** interval. This confirms
the separation between two resource-loading bursts, without identifying whether a
timer, dependency or unfinished blocking I/O causes it.

The native frame log contains 7,772 flips over 64.9604 seconds. In native frame
clock seconds 50–60, the loading screen submits 240 flips per two-second window,
approximately 170 draws per flip, and no frame longer than 9.31 ms. All logged GPU
buffer readbacks together take 69.725 ms over the session. The guest wait table
again reaches 512 entries in its first window. These findings strengthen the
previous conclusion. The counters alone did not identify a safe code change;
the subsequent executable analysis isolated the minimum timer.

## What was checked

- The host runtime has no explicit death-specific loading pause. Its guest sleep
  imports already use high-resolution Windows waitable timers.
- Semaphore and condition-variable waits follow guest requests. Removing these
  waits would change synchronization rather than establish the source of the delay.
- Reusing or unmapping GPU resources can drain recording work and perform readbacks.
  These paths exist in `Rasterizer::InvalidateMemory`, `Rasterizer::UnmapMemory` and
  `BufferCache::ReadMemory`. Their presence does not show that they cause this report.
- The supplied executable was analyzed directly. The user confirmed the fix in
  gameplay; other assets are unavailable for a local replay or timed comparison.

## Capture a comparison

Use **Trace Respawn Loading.cmd** beside `Bloodborne.exe`. It works with the packaged
launcher and requires no separate Python installation.

1. With the game focused, press **F8** immediately before choosing **Continue**.
2. Press **F8** when the character becomes controllable.
3. After dying, press **F8** when the loading screen appears. Press **F8** again when
   the respawned character becomes controllable.
4. Repeat step 3 for two deaths in the same area, then close the game normally.

Keep the session short, approximately two minutes. Send `logs/loading-*.zip` under
the port's data folder. F8 observes a key press without blocking or injecting input;
the game retains its usual handling of that key. Markers are manual observations
polled approximately every 10 ms, plus human reaction time. Process and thread
counters are sampled approximately every 100 ms.

Normal launches keep these diagnostics disabled. The script passes diagnostic
options to this launch only and does not edit launcher settings, `bbport.ini`, game
patches, frame limits or synchronization. Diagnostic logging has overhead; compare
both loading paths within the same capture.

## Report contents and limits

| File | Contents |
| --- | --- |
| `markers.csv` | F8 observations in order. Pairs 1–2, 3–4 and so on delimit the manually marked loading intervals. |
| `process.csv` | Cumulative kernel/user CPU time and logical I/O counters for this launch's native game processes, sampled approximately every 100 ms. |
| `threads.csv` | Cumulative CPU time and available Windows names for those processes' threads. Creation times distinguish reused IDs. |
| `game.log` | Console output with capture receipt times, limited to 16 MiB while the output pipe continues to drain. |
| `frames.csv` | Existing per-flip frame timing, uploads, allocations, compilation and GPU waits. |
| `waits.log` | Existing guest wait call sites in approximately five-second report windows. |
| `readbacks.csv` | Existing GPU buffer readbacks, their duration and completion state. |
| `metadata.json` | Capture duration, exit code, observed process IDs, markers and sampler wall time. |

Process, thread and marker times share the capture clock. `game.log` timestamps
show when a line reached the collector; native buffering can delay a line.
The existing frame log starts at the first flip, and the readback log starts at
logger creation. Their `t_s` values have different origins and must not be directly
subtracted from marker times. Guest waits are aggregated across report windows.

Logical I/O includes cached file access and pipes; it is **not physical disk
throughput**. Summed thread CPU and wait times can exceed elapsed wall time because
threads run or wait concurrently. CPU counters show activity, not a guest call
stack. A quiet interval is evidence to investigate a wait or timer, not proof of one.

The script samples only the process tree it launches. It does not suspend threads,
read process memory, capture the screen or upload anything. The ZIP uses an explicit
file allowlist: it does not copy saves, settings, dumps or game assets. Console logs
can contain local paths. Missing native logs can indicate that launch preparation
failed or that explicit diagnostic overrides in Advanced settings took precedence.

The automated fixture checks actual Windows process/thread CPU and I/O sampling,
Unicode output paths and ZIP export through the packaged Python runtime. These
checks validate the diagnostic tool, not gameplay loading performance.
