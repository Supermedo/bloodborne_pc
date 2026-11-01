# Experimental motion-vector cache

**Motion-vector cache (experimental)** in Graphics reuses object-motion render
targets. It is **off by default** and available with FSR 3.1, FSR 4 or FSR 4.1
and **Object motion vectors** enabled. It applies to Vulkan and DX12 methods.

The launcher and **Insert / L3+R3** overlay share the preference in `bbport.ini`.
Changes apply through **Apply and restart** or the next game start. The saved
choice is retained while another upscaler or disabled object motion makes the
option unavailable. Normal PLAY and shortcuts use the same startup choice.

Some scenes alternate full- and half-resolution motion targets within a frame.
Reusing these sizes avoids repeated allocation and full GPU completion waits.
The cache keeps up to four reusable targets, replacing the least recently used
inactive size. Evicted images/views remain alive until their GPU work completes.
Pending retirements are bounded to two images and 128 MiB of actual image
allocation bytes; pressure can require a targeted wait. Resident targets and
driver memory reservations are additional VRAM use.

Motion-vector calculations, clear/validity rules and image-quality settings are
preserved. Compare cache off/on after fresh starts at the same saved position
with identical graphics settings and camera. Include movement and area
transitions, and report FPS, frame times, VRAM and any image errors or crashes.

The [recorded sewer comparison](FSR411_WINDOWS.md#cache-only-ab-in-the-sewer-separate-run)
measures the original cache benefit. LRU/retirement improves replacement handling;
it does not establish another steady-scene FPS gain.

For direct engine launches, `BB_OBJECT_MOTION_CACHE=1` enables reuse. Unset, `0`
and other values select the single-target path. The startup log confirms
`Object motion: target cache on` or `off`; launcher-managed starts derive the
variable from the saved preference.

Technical implementation and regression coverage are documented in the source
repository under `docs/development/OBJECT_MOTION_CACHE.md`.
