# Linux source build

The native Linux build uses the original x86-64 game executable and the Vulkan
renderer. Use your own extracted, decrypted Bloodborne dump (CUSA03173). A clean
version 1.09 executable is needed for all frame-rate and resolution patches.
The loader does not open PKG archives. Keep game files outside your contribution.

## Dependencies

Use a C++23 compiler, CMake 3.24 or newer, Ninja, pkg-config, Python 3, and the
following development libraries. On Fedora 44:

```sh
sudo dnf install git gcc gcc-c++ cmake ninja-build pkgconf-pkg-config python3 \
  vulkan-loader-devel SDL3-devel ffmpeg-free-devel fmt-devel boost-devel \
  robin-map-devel xxhash-devel VulkanMemoryAllocator-devel glslang zydis-devel \
  libX11-devel xbyak-devel protobuf-devel protobuf-compiler openssl-devel \
  zlib-ng-compat-devel miniupnpc-devel json-devel
```

On Bazzite, install these in a Fedora Distrobox instead of changing the host:

```sh
distrobox create --name bloodborne-dev --image registry.fedoraproject.org/fedora:44
distrobox enter bloodborne-dev
```

Run the dependency installation and build commands inside the container.
Distrobox shares the checkout and desktop session; the Vulkan driver must also
be available inside it. `vulkaninfo --summary` (from `vulkan-tools`) should show
your real GPU. A headless build container alone does not validate game graphics.

Alternatively, `nix-shell shell.nix` supplies the native build dependencies.
The GTK launcher has its own environment in `launcher/shell.nix`.

## Build and start

```sh
git submodule update --init --recursive
bash build.sh --test
BB_GAME_DIR=/path/to/CUSA03173 bash run.sh
```

The build produces `out/bb-probe`, `out/bb-gpu-capabilities` and
`out/gpu/libbbgpu.so`. Keep the `gpu` directory beside the executable.
`BB_BUILD_JOBS` sets the maximum number of parallel Ninja jobs (default: Ninja's own).
For example, `BB_BUILD_JOBS=2 BB_LTO=OFF bash build.sh --test` reduces memory
use during compilation and linking. LTO is enabled by default.

Run from the desktop session so SDL can access the display and audio devices.
Settings are stored in `bbport.ini`; saves and caches are under `user/`.
`bash launcher/bb-launcher.sh` starts the GTK4 launcher when Python PyGObject,
GTK4 and libadwaita are installed, or uses Nix to supply them.

The port checks each group of patch sites before applying patches. A modified
1.09 executable can disable FPS or resolution patches even when its metadata
reports the expected version. Use a clean dump to enable all options; do not
change metadata or bypass these checks.
