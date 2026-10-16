# Local changes to gpu/third_party/fsr-vulkan

The submodule points at upstream FireBurn/FSR-Vulkan (`c64f093`). `build.sh` applies the
patches here to its working tree when they are not applied yet:

- `0001-...`: `BB_FSR4_PROFILE` (GPU time per FSR 4 pass) and `BB_FSR4_STATS` (driver
  statistics of each pass) in the FSR 4 v07 provider.
- `0002-...`: `#include <mutex>` in the FFX Vulkan backend; libc++ (the Windows MSYS2 CLANG64
  build) does not include it transitively.
- `0003-...`: orders repeated transfer clears of FSR history images, including
  clears that do not change the image layout.
- `0004-...`: preserves FSR debug-image view formats and resource usage.
- `0005-...`: associates FSR frame-generation transient views, descriptor sets
  and constant buffers with their render serial and retires them after GPU
  completion instead of retaining anonymous per-frame allocations.

An initialized submodule therefore appears modified after a build. These
changes are reproduced by the tracked patches; no separate vendor commit is
needed. On Windows, enable Git's `core.longpaths` for the SDK's generated files.
