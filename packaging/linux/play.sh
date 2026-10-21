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
# X11 first where there is an X display (Xwayland included): Steam only gives a game its own
# controller layout while that game's window has focus, and it can only see X11 windows. With
# a native Wayland window (SDL3's default on a Wayland desktop such as the Deck's Desktop Mode)
# Steam keeps its desktop mouse-and-keyboard layout and the game sees no gamepad input.
if [[ -z ${SDL_VIDEO_DRIVER:-} && -n ${DISPLAY:-} ]]; then export SDL_VIDEO_DRIVER=x11,wayland; fi
mkdir -p "$BB_DATA_DIR"
exec bash run.sh "$@"
