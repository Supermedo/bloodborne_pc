# Native Linux build (and the Steam Deck without Proton)

## Starting point

The port ran on Linux before it ran on Windows: up to 0.3 (branch `master`) it was built with
`build.sh`, started with `run.sh`, and shipped an AppImage for the Steam Deck. The Windows port
was added beside it with `#ifdef _WIN32` branches, so the C runtime (`src/`) and the GPU library
(`gpu/`) still carry their POSIX code. The renderer is shadPS4's video core and shader
recompiler (`gpu/VENDOR.txt`), and shadPS4 supports Linux as a first-class platform. Since the
Windows port only Windows was built, and the Linux path stopped compiling in a few places; this
branch fixes those. It is build-system work, not a rewrite.

## Windows-only parts and their Linux counterparts

- **NVIDIA DLSS** (`gpu/dlss_bridge`, MSVC + DLSS SDK): not built on Linux; `vk_dlss.cpp`
  compiles to a disabled stub there. The Deck is AMD. A `dlopen` bridge for NVIDIA desktops
  would be possible later.
- **FSR 3.1 and FSR 4** (`gpu/third_party/fsr-vulkan`): unchanged, native Vulkan.
- **Launcher** (`launcher/bbport_launcher_win.py`, Tkinter, 13 languages): Windows only for
  now. Tkinter runs on Linux; what does not is about a dozen `ctypes.windll` calls, `msvcrt`,
  `os.startfile`, `creationflags`, `%APPDATA%` and the `.exe` names. On Linux the older GTK4
  launcher (`launcher/bb-launcher.sh`) works with `run.sh` but has fewer settings and needs
  libadwaita 1.4 or newer (`Adw.ToolbarView`), which Ubuntu 22.04-based desktops do not have.
- **Updater**: Windows only (it replaces `Bloodborne.exe`). There are no Linux releases yet.
- **`Play Bloodborne.exe`**: on Linux `packaging/linux/play.sh` (below) or
  `launcher/bb-launcher.sh --play`.
- **Win32 in the runtime and loader** (`src/compat_win.c`, the `_WIN32` branches of
  `runtime_*.c`, `bb-probe.rc`, the large-address and stack link options): not used on Linux;
  the original POSIX code is still there.
- **Win32 in the GPU library** (`bb_win_compat.h`, wepoll, Windows sockets and timers, the
  Windows certificate store): the POSIX branches already exist.
- **MSVC**: only the DLSS bridge uses it; everything else builds with Clang/MinGW on Windows and
  GCC on Linux.

## What was broken on Linux

1. miniupnpc (UPnP for online play) was found and linked only on Windows (`net_upnp.h` missing).
2. Abseil (`flat_hash_map` in the texture cache) reached the build only through MSYS2's protobuf.
3. xbyak came from the submodule only on Windows.
4. Older kernel headers lack `UFFD_USER_MODE_ONLY`, `UFFD_FEATURE_WP_HUGETLBFS_SHMEM` and
   `UFFD_FEATURE_WP_UNPOPULATED` (`page_manager.cpp`; the running kernel still reports support).
5. A braced `vk::Extent2D` assignment is ambiguous with newer Vulkan-Hpp
   (`vk_frame_generation.cpp`).
6. glibc before 2.34 keeps `dlopen` in libdl (`-ldl` for `bb-probe`).
7. glibc before 2.34 has no `SYS_close_range` (`probe.c`).
8. `run.sh` never set `BB_GPU_USER_DIR`, so the GPU caches went to `~/.local/share/shadPS4`,
   the folder of an installed shadPS4 (`run.py` sets it on Windows).
9. The GPU library created its user folders without their parents and aborted on an account
   without `~/.local/share` (`path_util.cpp`).
10. `run.sh` misread `live_resolution` from a `bbport.ini` with Windows (CRLF) line endings.
11. `tests/test_file_mods.c` no longer linked (missing `runtime_quit_on_open` stub).

Also new: `libbbgpu.so` finds a `libSDL3.so.0` next to it (`$ORIGIN` run path).

## Building

```
docker compose -f packaging/linux/compose.yaml build        # once: Steam Runtime SDK + libraries
docker compose -f packaging/linux/compose.yaml run --rm build
```

The container is Valve's Steam Runtime 3 "sniper" SDK (Debian 11, glibc 2.31, gcc 14), the base
Steam Deck native games are built on. `packaging/linux/deps.sh` builds the libraries the SDK
lacks or has too old (Vulkan-Headers 1.4.350, Abseil, fmt, Zydis, robin-map, nlohmann-json, VMA,
xxHash, Boost headers, protobuf 21, miniupnpc, OpenSSL 3, FFmpeg 7, SDL 3.4) as static archives
(SDL3 shared, loading its audio and display back ends at run time), and libstdc++ is linked
statically. The result needs glibc 2.30 or newer, the Vulkan loader, X11 and zlib, and runs
without Wine or Proton.

Output: `out/bb-probe`, `out/gpu/libbbgpu.so`, `out/gpu/libSDL3.so.0`.

Tests: add `--test` (`... run --rm build nice -n 19 bash packaging/linux/build.sh --test`) for
the six C test programs; `python3 -m unittest discover -s tests -p 'test_*.py'` runs the Python
tests.

## Running

From the source tree:

```
BB_PREBUILT=1 BB_PROBE=out/bb-probe BB_GAME_DIR=/path/to/CUSA03173 bash run.sh
```

(Without `BB_PREBUILT`, `run.sh` first runs `build.sh` on the host.) Settings are read from
`bbport.ini` in `BB_DATA_DIR` (default: the source folder).

As one folder to copy (for example to a Steam Deck): `bash packaging/linux/bundle.sh` makes
`dist/bbport-linux/` (about 260 MB); `play.sh` in it starts the game in prebuilt mode, fullscreen,
with settings, saves and generated files in `~/.local/share/bbport` (`BB_DATA_DIR`), and clears
Steam's `LD_LIBRARY_PATH`.

## Steam Deck

1. Copy `dist/bbport-linux/` and your game folder (the one with `eboot.bin`) to the Deck, e.g.
   `/home/deck/Games/bbport-linux` and `/home/deck/Games/CUSA03173`. After a copy through a
   FAT/exFAT drive, `chmod +x play.sh run.sh bin/*`.
2. In Desktop Mode: Steam > Games > Add a Non-Steam Game > Browse, file type "All files", pick
   `play.sh`.
3. Properties > Launch Options: `BB_GAME_DIR=/home/deck/Games/CUSA03173 %command%`; leave
   Compatibility (Proton) off.
4. For 720p set `output_res=1280x720` in `~/.local/share/bbport/bbport.ini` (or in the in-game
   menu, L3+R3). The first start translates shaders for a while.

## Measurements

Same spot (Hunter's Dream clinic after Continue), FSR 3.1, Immediate present mode, no frame cap,
two-minute (Deck) or one-minute (PC) windows.

- **PC**, Ryzen 5 9600X + RTX 3090 (driver 580.159.03), 1080p output with FSR Ultra Performance,
  fullscreen at 1280x720, two alternating runs each at similar load: native 218 and 234 FPS
  (median of the on-screen counter), the 1.9.2 Windows build under Proton Experimental (11.0)
  163 and 184 FPS. On this PC the game is CPU-bound (in a similar native run at 720p with FSR
  Quality, 217 FPS, the 3090 was about 46% busy). The two runs were not
  identical builds: Windows' `run.py` adds the in-game PC menu and the blocked-movement fix,
  which `run.sh` does not have yet.
- **Steam Deck OLED**, SteamOS 3.8.28, `play.sh` (no Proton), 1280x720 output:
  - FSR Quality (scene 854x480): 96-97 FPS, GPU 96% busy (10.3 ms per frame), CPU about 2.9 GHz,
    APU about 14.8 W. Limited by the GPU.
  - FSR Ultra Performance (scene 426x240): about 123 FPS, but it crashed twice after 1-3
    minutes with `Guest fault (signal 11) at guest offset 0x263b8e7, address 0x5300000000`.
  - Not compared with Proton on the Deck.

## Known issues

- FSR Ultra Performance crashes on the Deck after a few minutes (above); FSR Quality did not
  crash in testing. Not investigated yet.
- `run.sh` lacks two things `run.py` does on Windows: the in-game PC menu files
  (`scripts/enhancements.py`) and the "Frame-rate fix: blocked-movement check" patch.
- The 13-language Tkinter launcher and the updater are Windows only; the GTK4 launcher needs
  libadwaita 1.4.
- Running `bin/bb-probe` directly (not through `run.sh`) still creates `~/.local/share/shadPS4`.
- Windows builds were not re-tested with these changes.
