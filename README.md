# Bloodborne for Windows

**Bloodborne running natively on Windows 10 and 11, by [Supermedo](https://github.com/Supermedo) Mohammed Albarghouthi.**

The original PlayStation 4 game runs directly on your PC: its own x86-64 code runs natively,
and the graphics are translated to Vulkan. No emulator window, no setup scripts: unpack the zip,
start `Bloodborne.exe`, pick your game folder and press **PLAY**.

**[Download the latest version](https://github.com/Supermedo/bloodborne_pc/releases/latest)** · **[Join the Discord](https://discord.gg/yTMG8c4Bqm)**

> **No game files are included.** You need your own decrypted dump of Bloodborne
> (CUSA03173). Version 1.09 is needed for the community patches (60/90/unlocked FPS,
> resolution, effects); other versions run at 30 FPS.
> This project is not affiliated with Sony Interactive Entertainment or FromSoftware.

## Features

- **Launcher with every setting in one window**, in 13 languages: English, Arabic, Russian,
  Spanish, Portuguese, French, German, Italian, Polish, Turkish, Chinese, Japanese, Korean.
- **Updates from the launcher:** when a new version is out it tells you, and **Update**
  installs it. Your saves, settings and mods are kept.
- **NVIDIA DLSS** on GeForce RTX cards (RTX 20 series and newer).
- **AMD FSR 3.1 and FSR 4** upscaling, plus native-resolution TAA.
- **Unlocked frame rate** with a frame cap (up to 120 by default), or 30/60/90 FPS.
- **Output resolutions** from 720p to 4K, presets from Native AA to Ultra Performance.
- **Cheats page:** never die, stealth, silent footsteps, Rally that never fades, enemy control,
  and gameplay tweaks (camera distance, no camera auto-rotation, easier running, ragdoll
  physics).
- **Game effects** on and off: chromatic aberration, depth of field, motion blur, SSAO, the
  game's own AA, dynamic light shadows, screen-space reflections, model detail.
- **Mods and third-party patches**, loaded without changing your game files.
- **`Play Bloodborne.exe`** starts the game straight away with your saved settings (good for a
  desktop shortcut or Steam).
- **Controller and keyboard**, an in-game settings menu (Insert or L3+R3), name entry on
  screen, a desktop shortcut, and a button to clear the shader cache.

## Requirements

- Windows 10 (1903 or later) or Windows 11, 64-bit
- A graphics card with Vulkan 1.3 and a current driver
- About 6 GB of free memory (RAM + page file), 10 GB for 1440p or 4K output
- DLSS: NVIDIA GeForce RTX 20 series or newer

Nothing else to install: everything the game needs is in the zip.

## How to play

1. Download the zip from [Releases](https://github.com/Supermedo/bloodborne_pc/releases/latest)
   and unpack it anywhere.
2. Start `Bloodborne.exe`.
3. On **Game & effects**, choose your game folder (the one with `eboot.bin`).
4. Press **PLAY**.

In the game, **Insert** (or **L3+R3** on a controller) opens the settings menu.
Keyboard: WASD move, arrows camera, Space Cross, Left Shift Circle, E Square, Q Triangle,
1/3 L1/R1, R/F L2/R2, Z/C L3/R3, I/K/J/L d-pad, Enter Options, Tab touchpad.

## Known issues

- The character preview on the character creation screen stays empty. The character is
  created correctly and looks right in the game.
- Some AMD graphics cards still crash when the game world loads; fixes are in progress.
- Above about 120 FPS the game's movement slows down (a limit of the game itself): keep the
  frame cap at 120 or lower.

## Problems and feedback

Ask on the [Discord server](https://discord.gg/yTMG8c4Bqm), or open an [issue](https://github.com/Supermedo/bloodborne_pc/issues) and attach
`user\last_run.log` from the game folder, with your graphics card and what happened.
If the game shows only a black screen, try **Advanced → Clear shader cache** first.

## Linux (CachyOS, Arch, Bazzite, Steam Deck)

The port also runs natively on Linux, DLSS included. Tested on CachyOS with a GeForce RTX 5090
and NVIDIA driver 615.71.09.

### Playing: the AppImage

Like the Windows zip, the AppImage contains everything the game needs (the launcher, FSR 4 and
DLSS): no internet, no packages and no build tools, on any distribution.

1. Download `Bloodborne-bbport-x86_64.AppImage` from
   [Releases](https://github.com/TwoToneEddy/bloodborne_pc/releases).
2. Make it executable (`chmod +x Bloodborne-bbport-x86_64.AppImage`, or in the file manager's
   properties) and start it.
3. Choose your game folder (the one with `eboot.bin`) and press **PLAY**.

What it needs on your system:

- **Your own dump of the game** (CUSA03173, version 1.09 for the community patches), the same
  as on Windows.
- **A graphics card with Vulkan 1.3.** AMD and Intel drivers (Mesa) are inside the AppImage;
  NVIDIA cards need the **NVIDIA proprietary driver** installed on the system, which the
  AppImage uses.
- **For DLSS:** a GeForce RTX card (20 series or newer) with that NVIDIA driver, which provides
  `libnvidia-ngx.so.1` (on Arch: `nvidia-utils`; the open kernel modules are fine). Nouveau/NVK
  cannot run DLSS. FSR 3/4 and TAA work on every GPU.
- **FUSE** to mount the AppImage (installed on most desktops, Bazzite and SteamOS included).

Settings, saves and generated files go to `~/.local/share/bbport`; `--play` starts the game
with the saved settings without the launcher window (Steam's Game Mode). Steam (*Add a
Non-Steam Game*) and the Steam Deck: see [docs/original-readme](docs/original-readme/README.md).

### Building from source

Needs internet access (git submodules and NVIDIA's DLSS SDK, tag `v310.9.1`, from
github.com/NVIDIA/DLSS under NVIDIA's license; its `libnvidia-ngx-dlss.so.310.9.1` is copied
next to the game) and these build tools and libraries: gcc, cmake, ninja, pkgconf, git,
Python 3, Vulkan headers and loader, SDL3, FFmpeg, Boost, fmt, robin-map, xxHash, glslang,
SPIRV-Headers, SPIRV-Cross, Zydis, miniz and libX11; for the launcher PyGObject, GTK 4 and
libadwaita. Vulkan Memory Allocator and xbyak come as git submodules.

On Arch-based systems (CachyOS, EndeavourOS, Arch) one script installs the packages above with
pacman (it asks for sudo), builds the game, builds the DLSS bridge on GeForce RTX systems, runs
a DLSS self-test and adds the launcher to the application menu:

    git clone --recursive -b linux_port https://github.com/TwoToneEddy/bloodborne_pc
    cd bloodborne_pc
    bash packaging/linux/setup-arch.sh

On other distributions install the same packages under their names and run the script's steps
by hand. Then start **Bloodborne** from the application menu (or `launcher/bb-launcher.sh`).
Everything built goes to `out/`; run the script again after pulling updates. Separately:

- `bash build.sh`: builds the game (`out/bb-probe`).
- `bash tools/build_dlss_linux.sh`: builds `out/libbbport_dlss.so` and copies NVIDIA's DLSS
  library next to it. Without these two files DLSS is not offered and the other upscalers work.
- `out/bb-dlss-selftest`: checks DLSS without the game (prints `PASS: DLSS works on this
  system`). In the game's output (the launcher's **Log** page, or the terminal) DLSS prints
  `DLSS: ready` or the reason it is unavailable.

### Making a release AppImage

Push a tag named `linux-v<version>` (for example `git tag -a linux-v1.5.1 -m "..." && git push
origin linux-v1.5.1`): GitHub Actions (`.github/workflows/linux-release.yml`) builds the
AppImage and attaches it, with its SHA-256 checksum, to that tag's release, creating the
release if there is none. The game is compiled from scratch, so it takes a while; follow it on the repository's **Actions**
page. **Run workflow** there builds without releasing (the AppImage is a downloadable
artifact for a week). On a fork, enable workflows on the Actions page once first.

To build the same AppImage on your own machine:

    bash packaging/linux/release.sh

builds `dist/Bloodborne-bbport-x86_64.AppImage` (about 900 MB) from scratch: it downloads the
submodules, the FSR 4 assets and the DLSS SDK, builds the game and the DLSS bridge with the
libraries of a pinned nixpkgs, and packs them with their whole library closure. The build
machine needs internet, git, curl, readelf (binutils) and Nix: an installed `nix`, or the
single-file [nix-portable](https://github.com/DavHau/nix-portable) at
`~/.local/bin/nix-portable` (no root needed; its store goes to `~/.nix-portable`, several GB).
Run it in a separate checkout (`git worktree add ../bloodborne_pc-release linux_port`): its
`out/` then holds binaries that only run inside the AppImage. Upload the AppImage to a GitHub
release.

### DLSS options

- **DLSS model:** the launcher's **DLSS model** row (`dlss_preset=default|J|K|L|M` in
  `bbport.ini`, or the `BB_DLSS_PRESET` variable). `default` lets NVIDIA choose per mode.
  Proton tools such as dxvk-nvapi driver settings do not reach the native game.
- **NVIDIA's DLSS indicator** (version, model, resolutions, bottom left): the launcher's
  **DLSS indicator** switch, or start the game with `__NGX_SHOW_INDICATOR=1024`.

### Mods from the Windows BB Launcher

Mods work as on Windows (the launcher's **Mods folder**). The `CUSA03173-mods` folder that the
Windows BB Launcher for shadPS4 creates next to the game contains only links to `C:/` paths:
on Linux they are skipped with a warning. Point the launcher's **Mods folder** at the BB
Launcher's `Mods-Active` folder instead, where the mod files themselves are.

## Building from source (Windows)

See [packaging/windows/README.md](packaging/windows/README.md): MSYS2 CLANG64, `bash build.sh`,
then `bash packaging/windows/package.sh`. DLSS is built separately with
`packaging/windows/build_dlss.sh`.

## Credits

Built on [bbport](https://github.com/deadinside28/bloodborne_pc), the native Linux port of
Bloodborne by deadinside28, and on the [shadPS4](https://github.com/shadps4-emu/shadPS4)
renderer. The original README is in [docs/original-readme](docs/original-readme/README.md).

Also used: [FSR-Vulkan](https://github.com/FireBurn/FSR-Vulkan) and the AMD FidelityFX SDK
(FSR), FSR 4 assets from [FireBurn/Q2RTX](https://github.com/FireBurn/Q2RTX), the DLSS bridge
adapted from [IFreemz/shadPS4-Bloodborne-DLSS-FSR](https://github.com/IFreemz/shadPS4-Bloodborne-DLSS-FSR),
the [NVIDIA DLSS SDK](https://github.com/NVIDIA/DLSS) (`nvngx_dlss.dll` under NVIDIA's license),
[LibAtrac9](https://github.com/Thealexbarney/LibAtrac9), [SDL3](https://github.com/libsdl-org/SDL),
[FFmpeg](https://ffmpeg.org), [Dear ImGui](https://github.com/ocornut/imgui),
[sirit](https://github.com/shadps4-emu/sirit), [magic_enum](https://github.com/Neargye/magic_enum),
[miniz](https://github.com/richgel999/miniz), [xbyak](https://github.com/herumi/xbyak),
[Vulkan Memory Allocator](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator),
[MSYS2](https://www.msys2.org) / [LLVM](https://llvm.org), [PyInstaller](https://pyinstaller.org)
and [Pillow](https://python-pillow.org). Game patches by Kyo, Lance McDonald, auser1337,
illusion, emoose and the Bloodborne community.

NVIDIA, GeForce RTX and DLSS are trademarks of NVIDIA Corporation. The icon is original
artwork.

## License

GNU GPL v2 or later ([LICENSE](LICENSE)). Third-party components keep their own licenses
(see `licenses\` in the download).
