# Windows performance audit — October 9, 2026

This pass removes avoidable work from launch preparation, shader preload, cache
writing and normal memory-fault handling. Gameplay FPS gains have not been
measured: local files include the executable and recorded traces, but not the
complete game assets needed for replay.

## Implemented changes

| Area | Finding | Change |
| --- | --- | --- |
| Repeat launch | Every launch parsed and linked the same executable again. | Reuse `out/boot-linked.bin` after SHA-256 verification of all relevant source files, preparation scripts and the complete linked image. |
| First launch | The old single-module linker produced `boot-libc.bin`, which the runtime did not use. | Run only preparation and the multi-module linker; it already includes libc and Fios. |
| Asset inventory | Preparation walked and inspected the entire asset tree for a diagnostic report. | Normal launches skip that walk. Standalone `prepare.py` retains the inventory unless `--no-resource-inventory` is passed. |
| Per-launch patches | XML patch compilation read the entire copied ELF to obtain its program headers. | Write compact `eboot-headers.bin` during preparation and verify it with the image. Patch compilation reads these headers; older standalone output falls back to the ELF. |
| Shader preload | Each pipeline reopened the SPIR-V file even when its shader module was already loaded. | Resolve the in-memory module first; read and compile only a missing permutation. Missing binaries leave no empty program entry. |
| Vulkan cache | Warmup ran before the driver pipeline-cache handle was created. | Create the handle first and use it for preload and runtime compilation. |
| Cache producer | Queued writes copied the entire shader/pipeline vector. | Transfer ownership into the queue without a payload copy. |
| Cache shutdown | Two mutexes and an unsynchronized request counter could disagree; stopping abandoned pending writes. | One mutex protects the queue and wait predicate. Shutdown drains accepted writes, including after a write failure. |
| Normal gameplay | Every tracked memory fault scanned the stack and updated diagnostic hash tables. | Collect fault sites only with `BB_FRAME_STATS=1`. Memory protection, dirty tracking and readback handling continue normally. |

Shader cache formats, image quality, frame limits, mouse controls and game timing
are unchanged by this pass. The previously validated respawn timer fix is retained.

## Measurements

Five runs per case; figures below are medians. These are component benchmarks,
not complete game startup times, respawn times or FPS comparisons.

| Workload | Before | After | Result |
| --- | ---: | ---: | --- |
| Queue 64 × 1 MiB cache payloads, including allocation/fill, worker started afterward | 27.722 ms | 11.597 ms | 58.2% less producer time; 64 payload copies reduced to zero |
| Prepare/link a generated 91 MiB guest image, repeat launch | 871.402 ms | 220.093 ms | 74.7% less preparation time; about 3.96× faster |
| Same generated image, fresh preparation and manifest creation | 871.402 ms | 843.532 ms | Similar first-preparation cost in this fixture |

The launch fixture uses tiny generated SELF/SFO inputs and a large guest-memory
image. A real executable adds source hashing and relocation work; storage and
cache state also affect results. The fixture establishes binary equivalence and
measures the preparation component, not real game loading. Full source hashing
and linked-image hashing remain intentional costs on every cache hit.

Reproduce with the native `cache-storage-test --benchmark` target and
`python tests/prepare_cache_smoke.py --benchmark`. The Python smoke test also runs
through packaged `Bloodborne.exe --script tests/prepare_cache_smoke.py`.

## Prepared-image cache behavior

- The key covers `eboot.bin`, `sce_sys/param.sfo`, `libc.prx`, `libSceFios2.prx`,
  preparation/linker/check scripts and whether game validation was bypassed.
- Relative labels permit identical temporary mod views to share an image.
  Executable/module modifications invalidate it; unrelated asset modifications
  are still supplied through the current mod view.
- The linked runtime image and compact patch headers are verified in full.
  Intermediate `boot.bin`, full ELF copies and diagnostic JSON reports are not
  consumed on a cache hit.
- Source changes during preparation prevent recording a success manifest.
  A failed rebuild removes the previous manifest; the next launch retries.
- Content profiles, selected XML patches and graphics settings run every launch.
  A cache created with `BB_SKIP_GAME_CHECK=1` cannot bypass a later strict check.
- `BB_PREPARE_CACHE=0` forces rebuilding. Removing `out/prepare-cache.json` has
  the same effect once. Existing installations populate it on their first launch.

## Remaining priorities from the recorded session

The second loading capture contains 7,772 flips over 64.96 seconds and reaches its
120 FPS cap. It cannot establish the renderer's uncapped throughput. Selecting
2,296 frames with at least 1,000 draws gives these averages:

| Counter | Mean per selected frame | Implication |
| --- | ---: | --- |
| Draws | 1,488 | Command preparation and recording remain relevant. |
| Page-protection calls | 0.829 ms | Investigate redundant transitions and batching while preserving dirty tracking. |
| Write-fault handling | 0.621 ms | Stack collection was unnecessary in normal runs and is now gated. Further changes need coherency tests. |
| Vulkan submissions | About 62 | Examine dependency boundaries before combining submissions. |
| Image / buffer uploads | 5.19 / 4.04 MiB | Check repeated uploads by resource identity and generation before retaining more data. |

Timing categories can overlap across threads; do not add them together. The draw
threshold is a heuristic, not a game-state marker. This capture had diagnostics
enabled, so it retains stack-collection overhead even with the new normal-run gate.

Other candidates are positioned asset reads on Windows, which currently save and
restore a shared file pointer, and persistence of the native Vulkan pipeline-cache
blob across processes. The former needs concurrent-read/close validation; the
latter needs driver-cache identity checks, atomic storage and a safe checkpoint
after pipeline-creation workers stop. Neither is changed without those checks.

## Validation

The native suite passes **36 checks**, with one DLSS Frame Generation capability
check skipped. It covers Vulkan startup, command recording in asynchronous,
synchronous and direct modes, oversized command payloads, temporal rendering,
input, respawn instructions and crash dumps. The cache worker regression verifies
payload contents, immediate-stop draining, concurrent overwrites and reopening.

Python tests cover cache invalidation by content despite unchanged sizes/timestamps,
script changes, corrupt/missing runtime output, failure recovery, mod-view reuse,
strict validation and per-launch settings/patch processing. The real preparation
smoke test compares the linked binary with the original three-step pipeline using
generated files. No game files or private diagnostic captures are distributed.
