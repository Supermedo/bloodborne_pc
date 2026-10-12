#!/usr/bin/env bash
# One-step setup on Arch-based systems (CachyOS, EndeavourOS, an Arch distrobox on Bazzite):
# installs the build dependencies, builds the port and, on NVIDIA GPUs, the DLSS bridge, runs
# the DLSS self-test and adds the launcher to the application menu.
#   bash packaging/linux/setup-arch.sh
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
packages=(gcc cmake ninja pkgconf git python python-gobject gtk4 libadwaita
          vulkan-headers vulkan-icd-loader sdl3 ffmpeg boost fmt robin-map xxhash
          glslang spirv-headers spirv-cross zydis miniz libx11)
missing=()
for package in "${packages[@]}"; do
    pacman -Qq "$package" >/dev/null 2>&1 || missing+=("$package")
done
if (( ${#missing[@]} )); then
    echo "Installing: ${missing[*]}"
    sudo pacman -S --needed --noconfirm "${missing[@]}"
fi
git submodule update --init --recursive
bash build.sh
if command -v nvidia-smi >/dev/null && nvidia-smi -L 2>/dev/null | grep -q RTX; then
    bash tools/build_dlss_linux.sh
    out/bb-dlss-selftest || echo 'DLSS self-test failed: the other upscalers still work.' >&2
else
    echo 'No GeForce RTX found: DLSS skipped (FSR 3/4 and TAA work on every GPU).'
fi
bash launcher/install-desktop.sh
echo 'Done. Start "Bloodborne" from the application menu, or: launcher/bb-launcher.sh'
