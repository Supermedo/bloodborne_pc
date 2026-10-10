# Bloodborne for Windows - Custom Achievement Edition

<img width="1118" height="773" alt="Screenshot 2026-10-10 100055" src="https://github.com/user-attachments/assets/6ad0ed29-1288-4976-b1ea-57e87f4b63bb" />


**Bloodborne running natively on Windows 10 and 11, by [Supermedo](https://github.com/Supermedo) Mohammed Albarghouthi.**
**Achievement System Integration by [pyL1nx](https://github.com/pyL1nx).**

The original PlayStation 4 game runs directly on your PC: its own x86-64 code runs natively, and the graphics are translated to Vulkan. No emulator window, no setup scripts: unpack the zip, start `Bloodborne.exe`, pick your game folder and press **PLAY**.

**[Download the latest version](https://github.com/pyL1nx/bloodborne_pc/releases/tag/V.1.0.0)**

> **No game files are included.** You need your own decrypted dump of Bloodborne (CUSA03173). Version 1.09 is needed for the community patches (60/90/unlocked FPS, resolution, effects); other versions run at 30 FPS. This project is not affiliated with Sony Interactive Entertainment or FromSoftware.

## Custom Achievement System

This fork includes a fully integrated achievement tracking system and a dedicated UI tab in the Windows launcher.

- **Live Tracking:** The launcher autonomously tracks and updates real trophy unlocks during gameplay, displaying all 40 trophies (earned and locked) on a dedicated page.
- **Persistent Saves:** Game runtime hooks write unlock events explicitly to `trophies.log` in your saves folder, ensuring progress is never lost between sessions.
- **Launcher Integration:** Unlocks are picked up dynamically from the game's live output, `trophies.log`, and `last_run.log`, meaning progress is tracked whether you use the launcher, Steam, or a desktop shortcut.
- **Platinum Logic:** The Platinum trophy is awarded automatically once all 33 base-game trophies are earned. The 6 Old Hunters DLC trophies do not count toward it, matching original console behavior.

*Note: For full tracking support, ensure you are using the compiled executable provided in the Releases tab alongside your decrypted game files.*

## Features

- **Online play (beta):** messages, bloodstains and wandering ghosts from other hunters through [The Hunter's Dream](https://thehuntersdream.com), and bells and summons through a shadNet server (shadPS4's public one by default).
- **Launcher with every setting in one window**, in 13 languages.
- **Updates from the launcher:** when a new version is out it tells you, and **Update** installs it. Your saves, settings and mods are kept.
- **NVIDIA DLSS** on GeForce RTX cards (RTX 20 series and newer).
- **AMD FSR 3.1 and FSR 4** upscaling, plus native-resolution TAA.
- **Unlocked frame rate** with a frame cap (up to 120 by default), or 30/60/90 FPS.
- **Output resolutions** from 720p to 4K, presets from Native AA to Ultra Performance.
- **Cheats page:** never die, stealth, silent footsteps, Rally that never fades, enemy control, and gameplay tweaks.
- **Game effects** on and off: chromatic aberration, depth of field, motion blur, SSAO, dynamic light shadows, screen-space reflections, model detail.
- **Mods and third-party patches**, loaded without changing your game files.
- **Add to Steam** puts the game in your Steam library in one click.

## Requirements

- Windows 10 (1903 or later) or Windows 11, 64-bit
- A graphics card with Vulkan 1.3 and a current driver
- About 6 GB of free memory (RAM + page file), 10 GB for 1440p or 4K output
- DLSS: NVIDIA GeForce RTX 20 series or newer

Nothing else to install: everything the game needs is in the zip.

## How to play

1. Download the zip from [Releases](https://github.com/pyL1nx/bloodborne_pc/releases/tag/V.1.0.0) and unpack it anywhere.
2. Start `Bloodborne.exe`.
3. On **Game & effects**, choose your game folder (the one with `eboot.bin`).
4. Press **PLAY**.

In the game, **Insert** (or **L3+R3** on a controller) opens the settings menu.

## Known issues

- The character preview on the character creation screen stays empty. The character is created correctly and looks right in the game.
- Some AMD graphics cards still crash when the game world loads; fixes are in progress.
- Online play is new. Messages, bloodstains and ghosts work; summoning other players with the bells has not been confirmed yet.
- Above about 120 FPS the game's movement slows down (a limit of the game itself): keep the frame cap at 120 or lower.

## Credits

Built on [bbport](https://github.com/deadinside28/bloodborne_pc), the native Linux port of Bloodborne by deadinside28, and on the [shadPS4](https://github.com/shadps4-emu/shadPS4) renderer. 

Achievement system integration developed by pyL1nx. 

Online play uses the network and PSN code of [shadp2p](https://github.com/Wozzardman/shadp2p) by Wozzardman, the shadNet server of the [shadPS4](https://shadps4.net) team, and [The Hunter's Dream](https://thehuntersdream.com) server by droogie and its team.

NVIDIA, GeForce RTX and DLSS are trademarks of NVIDIA Corporation. 

## License

GNU GPL v2 or later. Third-party components keep their own licenses.
