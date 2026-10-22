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
- **Guided first start:** five steps check your game files (a clean 1.09 `eboot.bin`, a complete
  folder) and your PC before the first start, set the picture for your hardware and your goal,
  set up keyboard or controller, bring your character along and sign you in for online play.
- **Simple or expert view:** the simple view shows the pages a player needs and sets everything
  technical by itself; the expert view adds every option. A switch at the bottom of the side bar.
- **Your goal instead of graphics jargon:** Smooth (up to 120 FPS), Balanced (60 FPS, the best
  picture) or Quiet (60 FPS, less work for the card); the settings for it come from your
  hardware, rated like the hardware check on [bbport.de](https://bbport.de/install/), each with
  its reason, and "Measure on this PC" checks them in the game.
- **Search** (Ctrl+F): settings and help by your own words ("stutters", "black screen",
  "controller").
- **Saves from other versions:** characters from shadPS4, Awaken's port, bbhost or a PS4 export
  are found and imported (your own saves are backed up first).
- **Help page:** checks your installation (a clean 1.09 `eboot.bin`, a complete game folder, the
  page file, the port's location) and shows the fix for each problem step by step; after a
  crash the launcher names the cause from the log. "Save a report" makes a zip for the Discord.
- **Game tests:** the launcher plays a fixed scene by itself and measures it, to find the best
  settings for 60, 90 or 120 FPS, to compare one setting with screenshots side by side, or to
  test a beta for crashes over hours; the report has every chart and the logs.
- **Updates from the launcher:** when a new version is out it tells you, and **Update**
  installs it. Your saves, settings and mods are kept.
- **NVIDIA DLSS** on GeForce RTX cards (RTX 20 series and newer).
- **AMD FSR 3.1 and FSR 4** upscaling, plus native-resolution TAA.
- **Windows AMD ML-FG 4.0.1** on supported RX 9000 GPUs, alongside FSR 3.1 frame
  generation. [Requirements, implementation and measurements](docs/ML_FRAME_GENERATION_WINDOWS.md).
- **Unlocked frame rate** with a frame cap (up to 120 by default), or 30/60/90 FPS.
- **Output resolutions** from 720p to 4K, presets from Native AA to Ultra Performance.
- **Cheats page:** never die, stealth, silent footsteps, Rally that never fades, enemy control,
  and gameplay tweaks (camera distance, no camera auto-rotation, easier running, ragdoll
  physics, the Japanese/Asian button layout with Circle to confirm and Cross to dodge).
- **Game effects** on and off: chromatic aberration, depth of field, motion blur, SSAO, the
  game's own AA, dynamic light shadows, screen-space reflections, model detail.
- **Field of view** from 30 to 90 degrees (the game has 43), for every camera of the game.
- **Mute in the background:** optionally silent while the game window is not in focus; the
  mouse is released then too.
- **Mods and third-party patches**, loaded without changing your game files.
- **DLC** from your own dump (The Old Hunters on base-game dumps), with a DLC folder setting.
- **Add to Steam** puts the game in your Steam library in one click.
- **`Play Bloodborne.exe`** starts the game straight away with your saved settings (good for a
  desktop shortcut).
- **Controller and keyboard**, an in-game settings menu (Insert or L3+R3), name entry on
  screen, a desktop shortcut, and a button to clear the shader cache.
- **Keyboard and mouse:** remap keys and buttons on the Controls page (up to three per action), key combinations such as
  Shift + left click, a Dark Souls III layout, mouse look, and a direct 1:1 mouse camera
  ([docs/INPUT.md](docs/INPUT.md)).
- **Text and voice language** chosen separately, Brazilian Portuguese text included.
- **Achievements page** in the launcher with the trophies you have earned, updated live.

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
2. Start `Bloodborne.exe`. The first time, a short guide checks your game folder (the one
   with `eboot.bin`) and your PC, and sets the picture for your hardware.
3. Press **PLAY**.

**DLC.** The Game of the Year edition (CUSA03173) already has The Old Hunters. With a base-game
dump, also dump your add-ons: each is a folder named like `SPEXPANSIONDLC03` with only
`sce_sys` inside, since their data ships with the 1.09 update. Put them in one folder and choose
it as **DLC folder** on **Game & effects**, or copy them to `user\addcont\<title id>\` (the
layout shadPS4 and bbport use).

In the game, **Insert** (or **L3+R3** on a controller) opens the settings menu.
Keyboard: WASD move, arrows camera, Space Cross, Left Shift Circle, E Square, V Triangle,
1/3 L1/R1, R/F L2/R2, Z L3, Q/C R3 (lock-on), I/K/J/L d-pad, Enter Options, Tab / Backspace
left / right touchpad half. Xbox controllers: Back + d-pad left / right clicks the left / right
touchpad half (gestures / key items), Back alone the left half.
The keyboard now works together with a connected controller instead of only without one.
All of this is configurable in `input.ini` (shadPS4's input config syntax: a Bloodborne file
made for shadPS4 can be copied over it), including mouse look: see [docs/INPUT.md](docs/INPUT.md).

## Exit-save snapshots and Chinese menus

On **Saves → Backups**, **Back up after every game exit** is optional and off by default.
It waits for the native game process to finish, then stores the final save, including after
a crash. There are no periodic copies while playing. Each exit has its own archive in
`save_backups`, shown in the existing backup browser for restoring, renaming or exporting;
exit archives are not pruned by the before-playing automatic-backup limit.

Exit archives record the native PID, exit code and SHA-256 of their files. The archive is
verified before publication and again before restoration. Legacy archives without a SHA-256
manifest remain readable. A failed backup is logged and never changes the game's exit code.
For direct `run.py` launches, set `BB_EXIT_BACKUP=1`. Killing the entire launcher/process
tree or losing power cannot guarantee a final snapshot.

Choosing **简体中文** for the launcher also selects Chinese for the Insert / L3+R3 menu,
independently of the game's text language. Direct launches can set `BB_UI_LANGUAGE=zh`.
The launcher prefers installed CJK fonts, and the native menu merges an installed system
CJK font with its embedded font. No Microsoft font or game asset is redistributed. Without
a usable system CJK font the native menu stays English and logs the reason.

The `chinese-ui-test` CMake target checks translation fallback, format arguments and CJK
font merging without a game dump or Vulkan device. Real glyph checks are explicitly skipped
when the test machine has no installed CJK font.

## Playing online (beta)

1. Register a shadNet account at [shadnet.shadps4.net](https://shadnet.shadps4.net).
2. In the launcher, open **Online co-op**, tick **Play online** and enter your account name
   (not your email) and password.
3. Press **Test connection**: all three lines should say "reachable". Then press **PLAY** and
   choose online at the title screen. The Hunter's Dream welcome message means you are online.

Everyone you want to play with needs game version 1.09 and the same co-op server. A private
shadNet server (from the [shadp2p](https://github.com/Wozzardman/shadp2p) project) works too:
put its address in **Server address**.

Playing with bbhost players on The Hunter's Dream's co-op pool is off in this port until it
works properly with their server. Please don't ask The Hunter's Dream for help with bbport: use
our [Discord server](https://discord.gg/yTMG8c4Bqm) instead.

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
Crashes on AMD RDNA2 cards, slow FSR 4 and online errors are collected in
[docs/WINDOWS_TROUBLESHOOTING.md](docs/WINDOWS_TROUBLESHOOTING.md).

## Building from source

See [packaging/windows/README.md](packaging/windows/README.md): MSYS2 CLANG64, `bash build.sh`,
then `bash packaging/windows/package.sh`. DLSS is built separately with
`packaging/windows/build_dlss.sh` (needs Visual Studio 2022).

## Credits

Built on [bbport](https://github.com/deadinside28/bloodborne_pc), the native Linux port of
Bloodborne by deadinside28, and on the [shadPS4](https://github.com/shadps4-emu/shadPS4)
renderer. The original README is in [docs/original-readme](docs/original-readme/README.md).

Online play uses the network, NP and shadNet code of [shadPS4](https://github.com/shadps4-emu/shadPS4)
0.20 and the shadNet server of the [shadPS4](https://shadps4.net) team, put together for this port
by **Awaken** ([BloodBorneButPc](https://github.com/Awakenchan/BloodBorneButPc)), whose online code
bbport now uses, including the backend for [The Hunter's Dream](https://thehuntersdream.com)
adapted from [bbhost](https://github.com/droogie/bbhost) by droogie (GPL-3.0-or-later; see
[gpu/bbnet/SHADNET-PORT.md](gpu/bbnet/SHADNET-PORT.md)). Thanks to Metr1k and The Hunter's Dream
team for their guidance, and to Wozzardman's [shadp2p](https://github.com/Wozzardman/shadp2p),
which bbport's first online version was built on.

The low-VRAM profile for 4 GB graphics cards comes from the 4 GB mod by **fixdag** and
**bashiku**.

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
