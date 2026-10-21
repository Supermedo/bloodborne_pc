# Steam Deck and native Linux

The port runs natively on Linux, including SteamOS on the Steam Deck, without Proton. The
build is Linux-native code from this repository; nothing Windows is involved.

## Steam Deck

### Setup

1. Build (below) and run `bash packaging/linux/bundle.sh`. This makes `dist/bbport-linux/`
   (about 260 MB).
2. Copy `dist/bbport-linux/` to the Deck, together with your game folder (the one with
   `eboot.bin`), for example to `/home/deck/Games/bbport-linux` and
   `/home/deck/Games/CUSA03173`. After a copy through a FAT or exFAT drive, run
   `chmod +x play.sh run.sh bin/*` in the bundle.
3. In Desktop Mode: Steam > Games > Add a Non-Steam Game > Browse, set the file type to "All
   files" and pick `play.sh`.
4. In the entry's Properties, set Launch Options and leave Compatibility (Proton) off:

   ```
   BB_GAME_DIR=/home/deck/Games/CUSA03173 %command%
   ```

   Add `BB_DATA_DIR=<folder>` to keep settings, saves and generated files somewhere other than
   `~/.local/share/bbport`.
5. Start it from Game Mode. The first start translates shaders and writes the menu files for a
   while.

### What play.sh does on a Deck

`play.sh` recognises a Deck by its DMI board vendor (Valve) and:

- **Settings on the first start.** Without a `bbport.ini` it writes 1280x720 output, FSR 3.1
  Quality (scene 854x480) and the FPS counter on. The PC defaults (1080p output, native AA)
  keep the Deck's GPU at its limit, below 30 FPS, where the game runs in slow motion.
- **Two-stage GPU pipeline off** (`BB_DRAW_PIPE=0`, unless set). With it on, the Deck crashed
  between 28 seconds and 3 minutes after loading a save with `Guest fault (signal 11) at guest
  offset 0x263b8e7, address 0x5300000000`, the guest heap corruption `docs/parallel_gpu.md`
  traces to the pipeline (`docs/WINDOWS_TROUBLESHOOTING.md`: "Two-stage GPU pipeline: Off").
  The pipeline is on by default with 8 or more CPU threads, which the Deck has.
- **X11 window** (`SDL_VIDEO_DRIVER=x11,wayland` when an X display exists). Steam gives a game
  its own controller layout only while that game's window has focus, and it only sees X11
  windows. With SDL3's native Wayland window in Desktop Mode, Steam kept its desktop
  mouse-and-keyboard layout and the game received no gamepad input.
- It prints `play.sh: Steam Deck 1, two-stage GPU pipeline BB_DRAW_PIPE=0` at start, which
  appears in Steam's log (`~/.local/share/Steam/logs/console-linux.txt`) with the rest of the
  game's output.

### Changing settings

- **In game:** the overlay menu, L3+R3 (both sticks pressed) or Insert: upscaler, preset,
  output resolution, "Show FPS counter" and the rest. Changes are saved to `bbport.ini`.
- **From the launch options:** `BB_FSR=quality`, `balanced`, `performance`,
  `ultra_performance`, `native` or `off` sets the upscaler mode in `bbport.ini` before the game
  starts, for example:

  ```
  BB_FSR=quality BB_GAME_DIR=/home/deck/Games/CUSA03173 %command%
  ```

- **Quit to Desktop** is in the title menu and in the in-game System menu.

### Measured on a Steam Deck OLED

Launched through Steam, the Hunter's Dream clinic from a save, played by hand, frame rate from
`BB_FPS_LOG=1` in 5-second windows. These runs were made with the 1.9.2-based build and the
two-stage pipeline on:

| Settings | Average | Lowest 5 s window | Windows under 30 FPS |
|---|---|---|---|
| 1080p output, native AA (PC defaults) | 22 and 24 FPS | 17 and 18 | all of the gameplay |
| 1280x720, FSR 3.1 Quality (Deck default) | 73 FPS | 45 | none |
| the same, native Wayland window | 51 FPS | 46 | none (the controller did not work) |

With the pipeline off (the Deck default now) the game ran in Game Mode for over ten
minutes after loading the save without the crash; its frame rate was not logged. The
pipeline's own comment puts its gain at about 18% on a Deck-like CPU.

## Building

```
docker compose -f packaging/linux/compose.yaml build        # once: Steam Runtime SDK + libraries
docker compose -f packaging/linux/compose.yaml run --rm build
```

The build runs in Valve's Steam Runtime 3 "sniper" SDK (Debian 11, glibc 2.31, gcc 14), the base
Steam Deck native games use, at the lowest CPU priority. `packaging/linux/deps.sh` builds the
libraries the SDK lacks or has too old as static archives; libstdc++ is linked statically and
SDL3 is built to load its audio and display back ends at run time. The output (`out/bb-probe`,
`out/gpu/libbbgpu.so`, `out/gpu/libSDL3.so.0`) needs glibc 2.30 or newer, the Vulkan loader,
X11 and zlib.

Tests: add `--test` (`... run --rm build nice -n 19 bash packaging/linux/build.sh --test`) for
the C test programs; `python3 -m unittest discover -s tests -p 'test_*.py'` runs the Python
tests (the Windows launcher's tests need Windows).

## Running on a Linux PC

```
BB_PREBUILT=1 BB_PROBE=out/bb-probe BB_GAME_DIR=/path/to/CUSA03173 bash run.sh
```

Without `BB_PREBUILT`, `run.sh` first runs `build.sh` on the host. Settings are read from
`bbport.ini` in `BB_DATA_DIR` (default: the source folder). The bundle's `play.sh` also works on
a PC.

## What was broken on Linux

1. miniupnpc (UPnP for online play) was found and linked only on Windows.
2. Abseil (`flat_hash_map` in the texture cache) reached the build only through MSYS2's protobuf.
3. xbyak came from the submodule only on Windows.
4. Older kernel headers lack three userfaultfd constants (`page_manager.cpp`).
5. A braced `vk::Extent2D` assignment is ambiguous with newer Vulkan-Hpp.
6. glibc before 2.34 keeps `dlopen` in libdl.
7. glibc before 2.34 has no `SYS_close_range`.
8. `run.sh` never set `BB_GPU_USER_DIR`, so the GPU caches went to an installed shadPS4's
   `~/.local/share/shadPS4`.
9. The GPU library created its user folders without their parents.
10. `run.sh` misread `live_resolution` from a `bbport.ini` with Windows line endings.
11. `tests/test_file_mods.c` no longer linked.
12. `run.sh` did not write the Quit to Desktop, Rebirth and field-of-view layers that `run.py`
    writes, while `patches.py` applied the first two's code by default.

`run.sh` also applies the "Frame-rate fix: blocked-movement check" (#33) like the Windows
release (off with `sprint_fix=0`).

## Not in the Linux build

- The pause-menu PC menu of the Windows release (`enhancements.py`, `menu_assets.py`,
  `BB_PC_MENU`) is not in this repository, so the Linux build does not have it.
- The 13-language Tkinter launcher and the updater are Windows only. The older GTK4 launcher
  (`launcher/bb-launcher.sh`) works with `run.sh` and needs libadwaita 1.4 or newer.
- NVIDIA DLSS is Windows only; FSR 3.1 and FSR 4 are native Vulkan and work on Linux.
- Rebirth at the Doll is applied on Linux but was not tried in game.
