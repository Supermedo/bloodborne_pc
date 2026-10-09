# Bloodborne for Windows — Legacy CPU Edition (Intel 3rd Gen / Ivy Bridge)

This is a fork of [Supermedo/bloodborne_pc](https://github.com/Supermedo/bloodborne_pc), based on
**Windows v1.5**. It makes the port playable on CPUs **without BMI1, LZCNT and MOVBE**, such as
Intel 3rd generation (Ivy Bridge) Core i3/i5/i7. It also fixes the crash when you **take damage** or
**press D-pad up** (blood bullets). Everything below the line is the original README.

### What this fork changes

- **Legacy CPU translations.** When the game image is prepared, 6,388 instructions this CPU lacks
  (BMI1 `andn`/`bextr`/`blsi`/`blsr`/`tzcnt`, `lzcnt`, `movbe`) are replaced by equivalent code,
  and libc's `strlen` is swapped for a version without BMI1. The game files are never changed.
  The profile **turns on by itself** when the CPU lacks those extensions and stays off on newer
  CPUs. Override it with `legacy_cpu=1` or `legacy_cpu=0` in `bbport.ini`, or `BB_LEGACY_CPU=1/0`.
  Files: `scripts/cpu_compat.py`, `tools/legacy-cpu-plan.json`, `scripts/link_libc.py`,
  `scripts/link_modules.py`; the plan generator is `tools/build_cpu_compat.py`.
- **Red-zone protection (damage / D-pad up crash).** The effect code that draws blood writes
  vertices into memory the GPU tracker write-protects. On Windows, each of those write faults
  makes the system write an exception frame just below the stack pointer. That overwrites the
  System V "red zone" where the game keeps a pointer, so the game reads NULL at `0x28ce9b5` and
  closes. 605 of the 607 affected store sites now run with the stack pointer moved 128 bytes
  lower (`patches/redzone.json`, applied by `scripts/link_modules.py`, Windows only;
  `BB_RED_ZONE=0` turns it off). The fix and the site list are from
  [PR #2](https://github.com/Supermedo/bloodborne_pc/pull/2) by **d4rksp4rt4n**. We confirmed that
  Ivy Bridge has the same crash, and the same fix removes it (`tests/test_redzone.py`).
- **Memory at outputs below 1080p.** Those scenes keep the PS4's 5056 MiB of direct memory.
  Upstream reserved 9152 MiB for any output other than 1080p, which a 16 GB PC may fail to reserve.
  Scenes above 1080p still get 9152 MiB (`run.py`).
- **Updates from this fork.** The launcher's update check reads this fork's releases, so **Update**
  never brings back the original scripts.

### Tested on

Intel Core i7-3770 · GeForce GTX 1650 4 GB · 16 GB RAM · Windows 10 · Bloodborne CUSA03173 v1.09.
Settings used: 30 FPS, upscaler off, output 1280×720, depth of field / motion blur / SSAO /
game AA / dynamic shadows / SSR off. The game held about 30 FPS, and damage and blood bullets no
longer close it. Intel 2nd generation (Sandy Bridge) and older AMD CPUs have **not** been tested.

### How to install

1. Download **Bloodborne-Windows-LegacyCPU.zip** from the
   [latest release](https://github.com/T0ug/bloodborne_pc_Ivy-Bridge/releases/latest) and unpack
   it into a new folder.
2. Start `Bloodborne.exe`, choose your game folder and press **PLAY**. The game output shows
   `CPU compatibility: translated 6388 instruction sites` and
   `Red zone: 605/607 guest store sites run red-zone-safe`.
3. Coming from another installation? Copy its `user` folder (your saves) into the new folder.

If you already have the original v1.5, **don't press "Update" in its launcher**: it installs the
upstream version, without this fork's scripts.

### Known issues

- Quitting to the title screen and loading the save again (Quit → Continue) can close the game
  with `Shader binary info not found` (exit code 23). It happened in our tests before the
  red-zone fix too, so the fix does not cause it. Restart the launcher to keep playing.
- 193 instructions are too short to translate safely: 58 `movbe` (an Ivy Bridge CPU would close the
  game if one is reached) and 135 `lzcnt`/`tzcnt` (may compute a different result). None of them
  caused a problem in testing. 42 red-zone stores could not be redirected.
- Online play from upstream v1.6 is not in this fork yet.

### Credits

[Supermedo](https://github.com/Supermedo) (the port) · **d4rksp4rt4n** (red-zone fix, PR #2) ·
[shadPS4](https://github.com/shadps4-emu/shadPS4) (graphics). License unchanged: GPL-2.0.
No game files are included or distributed.

---

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

## Building from source

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
