#!/usr/bin/env bash
# Starts the Linux build from this folder (packaging/linux/bundle.sh). For Steam: add this file
# as a non-Steam game, launch options: BB_GAME_DIR=/path/to/CUSA03173 %command%
# Settings, saves and generated files: ~/.local/share/bbport (BB_DATA_DIR).
cd -- "$(dirname -- "$(readlink -f -- "$0")")"
# Steam puts its own runtime's libraries first for non-Steam games; this build needs only the
# system's glibc, Vulkan loader, X11 and zlib and brings SDL3 along.
unset LD_LIBRARY_PATH
export BB_PREBUILT=1 BB_PROBE=bin/bb-probe
export BB_DATA_DIR=${BB_DATA_DIR:-${XDG_DATA_HOME:-$HOME/.local/share}/bbport}
export BB_FULLSCREEN=${BB_FULLSCREEN:-1}
mkdir -p "$BB_DATA_DIR"
exec bash run.sh "$@"
