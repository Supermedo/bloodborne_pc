# Motion-target cache: lifetime and validation

The optional object-motion target cache retains an active image and three
inactive sizes. A monotonically increasing use serial selects the least
recently used inactive target, including repeated uses within one frame.
Cache off keeps the original single-target path. Default, launcher/overlay
preferences, shaders, motion history and resize clear/validity rules remain.

## GPU ownership

An outgoing target is stamped with the scheduler's current tick when stashed.
`Rasterizer::Draw` attaches a target before resolving pending vertex/index
uploads. An upload wait can submit that attachment barrier; the rendering
commands then use the target in a later submission. The stash tick conservatively
bounds those already-recorded uses, including reads after an earlier provider
submission. `Image()` and `View()` are read-only accessors; their recorded uses
are covered by the stash tick. The bound can retain an image longer than its
exact last use.

Eviction moves the allocation and view into a retirement queue if that tick
has not completed. Collection destroys the view before its image. The queue
holds at most two images and 128 MiB of actual VMA image allocation bytes,
additional to the four resident targets. Count/byte pressure waits for an older
retirement; an oversized image waits for its own completion. A failed allocation
drains pending retirements and retries before retaining the original allocation
failure behavior. Destruction finishes the scheduler before releasing resources.
The bound excludes driver heap/block reservations and the rest of the game's VRAM.

A local `SwapImage` helper exchanges all six `UniqueImage` ownership/accounting
fields. The upstream move operations omit metadata; this topic does not change
that shared type. Raw image/view handles must be recorded before a later target
size change. The Vulkan scheduler owns this lifetime: the source motion target
is not shared with the DX12 upscaling/FG bridge.

## Regression fixture

Build `bb-probe`, `object-motion-cache-test`, `ui-composition-test`,
`ui-composition-sharp-test` and `motion-history-test` in a configured GPU build.
`BB_CACHE_TEST_THREADED=0|1` chooses direct/deferred command recording in the
fixture; `BB_OBJECT_MOTION_CACHE` selects its normal startup cache policy.

The commands below run a focused 30-case subset. The full 78-case matrix is:

| Group | Combinations | Runs |
|---|---|---|
| Pixel/validity | 2 recording modes × selectors 1/2/3/6 × cache 0/1 × tiny/game/churn | 48 |
| Startup defaults | 2 recording modes × cache unset/invalid/true × tiny/churn | 12 |
| Lifetime/pressure | 2 recording modes × the 9 named special cases below, cache on | 18 |

```bash
for threaded in 0 1; do
    for mode in 0 1; do
        for scene in tiny game churn; do
            BB_CACHE_TEST_THREADED=$threaded BB_OBJECT_MOTION_CACHE=$mode \
                out/gpu/object-motion-cache-test.exe "$scene" 6 validation
        done
    done
    for scene in lru deferred budget oversized drain destruct late-view late-image split-write; do
        BB_CACHE_TEST_THREADED=$threaded BB_OBJECT_MOTION_CACHE=1 \
            out/gpu/object-motion-cache-test.exe "$scene" 6 validation
    done
done
```

Enable the Khronos validation layer and synchronization validation in the local
Vulkan layer configuration. The fixture checks every readback pixel, validity
flags, VMA allocation counts, LRU, blocked submissions, count/byte limits,
oversized targets, collection, teardown and late reads. The late-read cases
also call `Image()`/`View()` through a const reference and verify the stash tick
after the first size change, before evicting the target. The `split-write` case
submits the attachment barrier before the actual rendering clear/readback,
checks the stash tick before eviction, then holds that work with a real timeline
semaphore while evicting its size. This detects the old bound without freeing
an in-flight GPU resource. All assertions are explicit under NDEBUG.

The gate watchdog attempts a timed host release. Worker exceptions are stored
and reported by the parent thread; cleanup does not rethrow them. `gate-error`
signals successfully before injecting a reporting failure and exits through the
normal fixture error path. A device/driver failure can still prevent completion.

FSR selectors exercise the shared target owner; these tests do not dispatch the
upscalers or FG. A forced SSBO owner on RX9070 checks integration with #141,
not RX6000 hardware compatibility. Real out-of-memory is not injected.

## Scope of remaining work

Per-size content preservation and selecting a different cached size during
`PrepareRead` would change FSR inputs and need separate image tests. Age-based
eviction, alternate pixel formats and enabling the cache by default are also
separate decisions. Existing sewer measurements describe the original cache,
not a new performance claim for LRU/retirement.
## Validation on November 1, 2026

RX9070 / Adrenalin 26.10.1: the complete native renderer/capability build and
three CPU programs pass. The 78 isolated GPU cases cover 60 full pixel/validity
readbacks and 18 lifetime/pressure cases in direct/threaded recording. Cache
on/off and unset/invalid settings produce the same expected pixel hashes.
The split-submission case checks the delayed clear/readback and allocation
retention. Allocation counts return to baseline after teardown.

Removing only the stash-tick correction makes this regression fail in both
recording modes at the metadata check before eviction. Restoring it passes.
Two injected watchdog-reporting failures exit with code 1 through the normal
error handler. These controls run with NDEBUG, without assertion dialogs.

A private combination with current PR141 (`fc8158a`) passes 26 additional forced
SSBO-owner cache/readback/lifetime cases, including both split-write modes.
Renderer implementation and header merge automatically. The test fixture needs
the union of its lifetime helpers with PR141's private SSBO constructor helper.
The cache policy functions are identical in that combination; the standalone
source and renderer are restored afterward. This is not an RX6000 hardware
or new vertex-shader draw test.

Core and Synchronization validation are confirmed active. The unchanged base
headless instance reports `VUID-vkCreateDevice-ppEnabledExtensionNames-01387`
because it enables the device swapchain extension without an instance surface
extension. Cache-off controls reproduce it. No additional cache findings or
synchronization hazards occur. No game, area-transition, frame-pacing or FPS
benchmark is claimed by these isolated checks. Actual allocation failure is
reviewed, not forced by exhausting VRAM.

The standard Windows ZIP passes manifest/CRC, exact player-guide content and
actual frozen-launcher runtime checks (TLS, Pillow, Tk/Tcl and windowed startup).
The concise existing cache guide remains packaged; this technical report is
excluded. The package script is unchanged from the official base.
