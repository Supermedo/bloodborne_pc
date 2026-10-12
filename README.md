# Bloodborne for Windows — Legacy CPU Edition (Intel 3rd Gen / Ivy Bridge)

This is a fork of [Supermedo/bloodborne_pc](https://github.com/Supermedo/bloodborne_pc) that tracks
its `windows` branch (version 1.7). Since 1.7 the original port runs on CPUs without BMI1, LZCNT
and MOVBE, such as Intel 3rd generation (Ivy Bridge) Core i3/i5/i7: that support came from this
fork ([PR #10](https://github.com/Supermedo/bloodborne_pc/pull/10)). The fork keeps a few extras on
top, listed below. Everything below the line is the original README.

### What this fork adds to the original

- **Video memory on the FPS counter.** With the FPS counter on (launcher or in-game menu, Insert /
  L3+R3), a second line shows `VRAM <in use> / <available> MB  peak <highest>`, so you can watch how
  close the game runs to your card's limit (`gpu/shim/bbport_overlay.cpp`).
- **Memory at outputs below 1080p.** Those scenes keep the PS4's 5056 MiB of direct memory; the
  original reserves 9152 MiB for any output other than 1080p, which a 16 GB PC may fail to reserve.
  Scenes above 1080p still get 9152 MiB (`run.py`).
- **Updates from this fork.** The launcher's update check reads this fork's releases
  (tags `windows-v<version>-legacy.<n>`), so **Update** keeps these extras.

### Already in the original (since 1.7)

- **Legacy CPU translations** (`scripts/cpu_compat.py`, `tools/legacy-cpu-plan.json`): 6,388
  instructions the CPU lacks are replaced by equivalent code when the game image is prepared, and
  libc's `strlen` by a version without BMI1. On by itself when CPUID lacks BMI1, LZCNT or MOVBE;
  `legacy_cpu=0/1` in `bbport.ini` or `BB_LEGACY_CPU=0/1` override it.
- **Damage / D-pad up crash fix** ([PR #2](https://github.com/Supermedo/bloodborne_pc/pull/2) by
  **d4rksp4rt4n**): store sites that fault on GPU-tracked memory run with the stack pointer moved
  128 bytes lower, so Windows no longer overwrites the game's red zone (`0x28ce9b5`). Ivy Bridge had
  the same crash.
- **Texture cache that frees video memory** (PRs #35 and #32): fixes `ErrorOutOfDeviceMemory` after
  some minutes on 4 GB cards. This fork shipped the same fix as
  [PR #4](https://github.com/Supermedo/bloodborne_pc/pull/4) in 1.5.2.

### Tested on

Intel Core i7-3770 · GeForce GTX 1650 4 GB · 16 GB RAM · Windows 10 · Bloodborne CUSA03173 v1.09.
About 30 FPS at 30 FPS / 720p / upscaler off; about 56 FPS at 60 FPS with FSR 3.1
(640×360 → 1280×720). Intel 2nd generation (Sandy Bridge) and older AMD CPUs have **not** been
tested.

### How to install

1. Download **Bloodborne-Windows-LegacyCPU.zip** from the
   [latest release](https://github.com/T0ug/bloodborne_pc_Ivy-Bridge/releases/latest) and unpack it
   into a new folder.
2. Start `Bloodborne.exe`, choose your game folder and press **PLAY**.
3. Coming from another installation? Copy its `user` folder (your saves) into the new folder.

### Known issues

- Quitting to the title screen and loading the save again (Quit → Continue) can close the game
  with `Shader binary info not found` (exit code 23). Open the launcher again.
- 193 instructions are too short to translate safely: 58 `movbe` (an Ivy Bridge CPU closes the game
  if one is reached) and 135 `lzcnt`/`tzcnt` (may compute a different result). None of them caused a
  problem in testing.
- The game reads only the first controller Windows lists. With two controllers connected, turn off
  the one you don't use (or set `SDL_GAMECONTROLLER_IGNORE_DEVICES` to its `0xVID/0xPID`).

### Credits

[Supermedo](https://github.com/Supermedo) (the port) · **d4rksp4rt4n** (damage crash fix, PR #2) ·
**gtggdd4mzb-source**, **devilismyfriend**, **parsifal295** (texture cache fixes) ·
[shadPS4](https://github.com/shadps4-emu/shadPS4) (graphics). License unchanged: GPL-2.0.
No game files are included or distributed.

---

# Bloodborne for Windows
<a href="https://trendshift.io/repositories/286547?utm_source=trendshift-badge&amp;utm_medium=badge&amp;utm_campaign=badge-trendshift-286547" target="_blank" rel="noopener noreferrer"><img src="https://trendshift.io/api/badge/trendshift/repositories/286547/daily?language=C%2B%2B" alt="Supermedo%2Fbloodborne_pc | Trendshift" width="250" height="55"/></a>

**Bloodborne running natively on Windows 10 and 11, by [Supermedo](https://github.com/Supermedo) Mohammed Albarghouthi.**

The original PlayStation 4 game runs directly on your PC: its own x86-64 code runs natively,
and the graphics are translated to Vulkan. No emulator window, no setup scripts: unpack the zip,
start `Bloodborne.exe`, pick your game folder and press **PLAY**.

**[Download the latest version](https://github.com/Supermedo/bloodborne_pc/releases/latest)** · **[Join the Discord](https://discord.gg/yTMG8c4Bqm)**  **[Join X,com](https://x.com/SuperMedoPorts)**
Check my other ports 
https://nativeretro.com/

> **No game files are included.** You need your own decrypted dump of Bloodborne
> (CUSA03173). Version 1.09 is needed for the community patches (60/90/unlocked FPS,
> resolution, effects); other versions run at 30 FPS.
> This project is not affiliated with Sony Interactive Entertainment or FromSoftware.

## Features

- **Online play (beta):** messages, bloodstains and wandering ghosts from other hunters through
  [The Hunter's Dream](https://thehuntersdream.com), and bells and summons through a shadNet
  server (shadPS4's public one by default).
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
- **Add to Steam** puts the game in your Steam library in one click.
- **`Play Bloodborne.exe`** starts the game straight away with your saved settings (good for a
  desktop shortcut).
- **Controller and keyboard**, an in-game settings menu (Insert or L3+R3), name entry on
  screen, a desktop shortcut, and a button to clear the shader cache.

## Requirements

- Windows 10 (1903 or later) or Windows 11, 64-bit
- An Intel Core 4th generation (Haswell, 2013) or newer, or an AMD Ryzen: the game uses
  instructions of the PS4's processor (AVX, BMI1, MOVBE, LZCNT). Intel 2nd and 3rd generation
  (Sandy and Ivy Bridge) run with translated instructions, best effort; processors without AVX
  (many Pentium and Celeron models) cannot run it
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

## Playing online (beta)

1. Register a shadNet account at [shadnet.shadps4.net](https://shadnet.shadps4.net).
2. In the launcher, open **Online co-op**, tick **Play online** and enter your account name
   (not your email) and password.
3. Press **Test connection**: all three lines should say "reachable". Then press **PLAY** and
   choose online at the title screen. The Hunter's Dream welcome message means you are online.

Everyone you want to play with needs game version 1.09 and the same co-op server. A private
shadNet server (from the [shadp2p](https://github.com/Wozzardman/shadp2p) project) works too:
put its address in **Server address**.

## Known issues

- Some AMD graphics cards may still crash when the game world loads; please send
  `user\last_run.log` if yours does.
- Online play is new. Messages, bloodstains, ghosts and summons with the bells work on the
  default server; please report how it goes on Discord.
- Keep the frame cap at 120 or lower. The sprint that stayed at half speed above 90 FPS is
  fixed, but the game's own timing above 120 FPS has not been checked everywhere.
- Intel 2nd and 3rd generation processors run with translated instructions (best effort):
  a few rarely used ones are not translated yet.

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

Online play uses the network and PSN code of [shadp2p](https://github.com/Wozzardman/shadp2p)
by Wozzardman (the shadPS4 Bloodborne co-op project), the shadNet server of the
[shadPS4](https://shadps4.net) team, and [The Hunter's Dream](https://thehuntersdream.com) server
by droogie and its team.

Also used: [FSR-Vulkan](https://github.com/FireBurn/FSR-Vulkan) and the AMD FidelityFX SDK
(FSR), FSR 4 assets from [FireBurn/Q2RTX](https://github.com/FireBurn/Q2RTX), the DLSS bridge
adapted from [IFreemz/shadPS4-Bloodborne-DLSS-FSR](https://github.com/IFreemz/shadPS4-Bloodborne-DLSS-FSR),
the [NVIDIA DLSS SDK](https://github.com/NVIDIA/DLSS) (`nvngx_dlss.dll` under NVIDIA's license),
[LibAtrac9](https://github.com/Thealexbarney/LibAtrac9), [SDL3](https://github.com/libsdl-org/SDL),
[FFmpeg](https://ffmpeg.org), [Dear ImGui](https://github.com/ocornut/imgui),
[sirit](https://github.com/shadps4-emu/sirit), [magic_enum](https://github.com/Neargye/magic_enum),
[miniz](https://github.com/richgel999/miniz), [xbyak](https://github.com/herumi/xbyak),
[wepoll](https://github.com/piscisaureus/wepoll), [cpp-httplib](https://github.com/yhirose/cpp-httplib),
[Protocol Buffers](https://protobuf.dev), [OpenSSL](https://www.openssl.org),
[miniupnpc](https://miniupnp.tuxfamily.org), [JSON for Modern C++](https://github.com/nlohmann/json),
[Vulkan Memory Allocator](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator),
[MSYS2](https://www.msys2.org) / [LLVM](https://llvm.org), [PyInstaller](https://pyinstaller.org)
and [Pillow](https://python-pillow.org). Game patches by Kyo, Lance McDonald, auser1337,
illusion, emoose and the Bloodborne community.

NVIDIA, GeForce RTX and DLSS are trademarks of NVIDIA Corporation. The icon is original
artwork.

## License

GNU GPL v2 or later ([LICENSE](LICENSE)). Third-party components keep their own licenses
(see `licenses\` in the download).
