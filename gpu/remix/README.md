# Official RTX Remix SDK backend

This directory integrates NVIDIA's unmodified RTX Remix C SDK **0.6.4**, pinned
to the runtime shipped with **RTX Remix 1.5.2**. `include/remix/remix_c.h` is taken
from runtime commit `b81a7b566b1eeb9edb4dc2b3c9d3972e0f253ad4`; its copyright and
license are retained. Runtime binaries are installed separately and are never
substituted for the system Direct3D library.

The backend submits camera, PBR materials, indexed meshes, transforms and lights
to the official renderer. Shared output uses a D3D9 texture's exported Win32 KMT
memory handle, not the raw `VkImage` returned by the SDK. The consumer must wait
for completion before accessing the output and finish reading it before the next
frame or resize. The SDK owns path tracing, scene acceleration and denoising.

This backend is **not a completed Bloodborne scene adapter**. Game geometry,
material semantics, map lifetime and dynamic scene extraction still need to be
connected and validated. No gameplay RTX switch is exposed until those inputs
are complete. `bb-remix-check.exe` renders a synthetic diagnostic scene and
checks actual pixels; it is not a Bloodborne compatibility benchmark.
