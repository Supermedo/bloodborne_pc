# <img src="launcher/gravity.ico" width="36" height="36" alt="Gravity Rush 2 Icon" valign="middle"> Gravity Rush 2 for Windows

**Gravity Rush 2 running natively on Windows 10 and 11.** Forked from [Supermedo's](https://github.com/Supermedo) PC port project.

The original PlayStation 4 game runs directly on your PC: its own x86-64 code runs natively,
and the graphics are translated to Vulkan. No emulator window, no setup scripts: unpack the release,
start the launcher (`Gravity Rush 2.exe` / `bbport_launcher_win.py`), pick your game folder and press **PLAY**.

> **No game files are included.** You need your own decrypted dump of Gravity Rush 2
> (e.g. CUSA03694 / CUSA04943).
> This project is not affiliated with Sony Interactive Entertainment or SIE Japan Studio.

## Features

- **Launcher with every setting in one window**, in multiple languages.
- **NVIDIA DLSS** on GeForce RTX cards (RTX 20 series and newer).
- **AMD FSR 3.1 and FSR 4** upscaling, plus native-resolution TAA.
- **Custom frame rate caps and display resolutions** from 720p to 4K.
- **Game effects and graphical settings** configuration.
- **Controller and keyboard & mouse support** with custom remapping.
- **Add to Steam** integration in one click.

## Requirements

- Windows 10 (1903 or later) or Windows 11, 64-bit
- An Intel Core 4th generation (Haswell, 2013) or newer, or an AMD Ryzen (AVX, BMI1, MOVBE, LZCNT support required)
- A graphics card with Vulkan 1.3 and a current driver
- About 8 GB of free memory (RAM + page file), 12–16 GB for 1440p or 4K output
- DLSS: NVIDIA GeForce RTX 20 series or newer

## How to play

1. Download the latest release package and unpack it anywhere.
2. Start the launcher executable.
3. Choose your decrypted Gravity Rush 2 game folder (the directory containing `eboot.bin`).
4. Configure your graphics and controls, then press **PLAY**.

## Building from source

See [packaging/windows/README.md](packaging/windows/README.md): MSYS2 CLANG64, `bash build.sh`,
then `bash packaging/windows/package.sh`.

## Credits

- Built on the native PC port base by [Supermedo](https://github.com/Supermedo) and [deadinside28](https://github.com/deadinside28).
- Powered by the [shadPS4](https://github.com/shadps4-emu/shadPS4) renderer and translation layers.
- Thanks to the contributors and upstream projects including [FSR-Vulkan](https://github.com/FireBurn/FSR-Vulkan), [SDL3](https://github.com/libsdl-org/SDL), [Dear ImGui](https://github.com/ocornut/imgui), and the PlayStation 4 homebrew/emulation community.

## License

GNU GPL v2 or later ([LICENSE](LICENSE)). Third-party components keep their own licenses.

