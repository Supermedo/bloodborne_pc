#!/usr/bin/env bash
# Builds the Linux release: dist/Bloodborne-bbport-x86_64.AppImage, which needs no internet,
# no build tools and no packages on the player's system (any distribution, Bazzite and the
# Steam Deck included). It contains the game binaries, the launcher, FSR 4 assets and, for
# GeForce RTX cards, DLSS (only the NVIDIA driver comes from the host, as it must).
#   bash packaging/linux/release.sh
# Needs on the build machine: internet, git, curl, readelf, bash, and Nix: the system's nix if
# installed, otherwise nix-portable (~/.local/bin/nix-portable or NIX_PORTABLE; a single file,
# no root: github.com/DavHau/nix-portable). The binaries are built in the Nix environment
# (shell.nix) so that the AppImage carries every library they link.
# Build in a separate checkout (git worktree): out/ then holds Nix-linked binaries that only
# run from the AppImage.
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
# nixpkgs used for the build and the package, pinned for reproducible AppImages (nixos-unstable
# of 2026-10-08: the renderer needs Vulkan headers 1.4.357 or newer). BB_NIXPKGS overrides it.
nixpkgs_rev=e7439b6b14ad3cc35d05608ebca9bce01a25f5f8
export NIX_PATH=nixpkgs=${BB_NIXPKGS:-https://github.com/NixOS/nixpkgs/archive/$nixpkgs_rev.tar.gz}
shim=$(mktemp -d)
trap 'rm -rf "$shim"' EXIT
if ! command -v nix >/dev/null; then
    portable=${NIX_PORTABLE:-$HOME/.local/bin/nix-portable}
    [[ -x $portable ]] || { echo "Need nix or nix-portable ($portable)." >&2; exit 1; }
    # bwrap is faster and more reliable than nix-portable's proot fallback. nix-portable sets
    # its own NIX_PATH, so the pinned nixpkgs is passed with -I (after nix's subcommand).
    printf '#!/bin/sh\nNP_RUNTIME=${NP_RUNTIME:-bwrap} exec %q nix-shell -I %q "$@"\n' \
        "$portable" "$NIX_PATH" > "$shim/nix-shell"
    printf '#!/bin/sh\ncommand=$1; shift\nNP_RUNTIME=${NP_RUNTIME:-bwrap} exec %q nix "$command" -I %q "$@"\n' \
        "$portable" "$NIX_PATH" > "$shim/nix"
    chmod +x "$shim/nix" "$shim/nix-shell"
    export PATH=$shim:$PATH
fi
export NIX_CONFIG='experimental-features = nix-command flakes'
git submodule update --init --recursive
# Downloads first (host tools): the FSR 4 assets and NVIDIA's DLSS SDK.
bash tools/fetch_fsr4_assets.sh
sdk=$PWD/out/sdk/DLSS
if [[ ! -f $sdk/include/nvsdk_ngx_vk.h ]]; then
    mkdir -p out/sdk
    git clone --depth 1 --branch "${DLSS_SDK_TAG:-v310.9.1}" https://github.com/NVIDIA/DLSS.git "$sdk"
fi
# The game and the DLSS bridge, in the Nix environment.
nix-shell shell.nix --run 'BB_IN_NIX_SHELL=1 bash build.sh && bash tools/build_dlss_linux.sh'
bash packaging/appimage.sh
