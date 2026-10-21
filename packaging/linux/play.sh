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
deck=0; [[ $(cat /sys/class/dmi/id/board_vendor 2>/dev/null) == Valve ]] && deck=1
# Steam Deck: the two-stage GPU pipeline off unless BB_DRAW_PIPE says otherwise. With it, the
# Deck crashed 28 s to 3 min into play with "Guest fault (signal 11) at guest offset 0x263b8e7,
# address 0x5300000000", the guest heap corruption docs/parallel_gpu.md traces to the pipeline
# (docs/WINDOWS_TROUBLESHOOTING.md: "Two-stage GPU pipeline: Off"). It is on by default with
# 8 or more CPU threads, which the Deck has; off costs some frame rate (about 18% there).
if (( deck )) && [[ -z ${BB_DRAW_PIPE:-} ]]; then export BB_DRAW_PIPE=0; fi
# First start on a Steam Deck: the settings measured there (1280x720 output, FSR 3.1 Quality,
# 96-97 FPS in the Hunter's Dream) instead of the PC defaults (1080p output, native AA), which
# keep the Deck's GPU at its limit. The FPS counter starts on; the in-game menu (L3+R3, or
# Insert) changes any of these and saves them to bbport.ini.
if [[ ! -e $BB_DATA_DIR/bbport.ini ]] && (( deck )); then
    printf 'upscaler=fsr3\npreset=1\noutput_res=1280x720\nshow_fps=1\n' > "$BB_DATA_DIR/bbport.ini"
fi
exec bash run.sh "$@"
