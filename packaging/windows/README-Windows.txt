Bloodborne (bbport) for Windows
===============================

No game files are included. You need your own decrypted dump of Bloodborne CUSA03173
(the folder with eboot.bin, sce_module, sce_sys, dvdroot_ps4). Game version 1.09 is needed for
the community patches (60/90/unlocked FPS, resolution, effects); other versions run at 30 FPS.

Requirements
- Windows 10 (1903 or later) or Windows 11, 64-bit.
- An Intel Core of the 4th generation (Haswell, 2013) or newer, or an AMD Ryzen: the game uses
  instructions of the PS4's processor (AVX, BMI1, MOVBE, LZCNT). Intel 2nd and 3rd generation
  (Sandy and Ivy Bridge) run with translated instructions, best effort. On a processor without
  AVX (many Pentium and Celeron models) the game says what is missing.
- A Vulkan 1.3 graphics card with a current driver.
- About 6 GB of free memory commit (RAM + page file), 10 GB for 1440p/4K output.
  Nothing else: Python and the libraries are inside this folder.

Starting
- Bloodborne.exe opens the launcher. The first time, a short guide picks your game folder and
  the settings for your PC; then press PLAY. The "Play" page checks the game version, saves,
  graphics card and FSR 4 assets. The pages marked EXPERT in the side bar (All graphics, Game
  files, Cheats, Your PC, Advanced) appear with the "Expert view" switch at its bottom; the game
  folder can be changed later on "Game files".
- Play Bloodborne.exe starts the game straight away with the settings saved in the launcher,
  without opening it. Set things up once in Bloodborne.exe, then use Play Bloodborne.exe (or a
  shortcut to it, or "Add to Steam" on "Launcher customization"). If no game folder is chosen
  yet it opens the launcher. Bloodborne.exe --play does the same. The log goes to
  user\last_run.log.
- Launcher customization -> "Desktop shortcut" puts Bloodborne on the desktop.
- Updates: when a new version is out, the launcher shows it at the bottom left; "Update"
  downloads and installs it and opens the launcher again (saves, settings and mods are kept).
  Advanced -> "Check for updates" checks by hand.
- Launcher customization -> "Launcher language": English, Russian, Arabic, Spanish, Portuguese,
  French, German, Italian, Polish, Turkish, Chinese, Japanese, Korean (default: the Windows
  language).
- Gameplay -> "Text language" and "Voice language" are separate. Brazilian Portuguese
  text uses the game's porbr files; voice tracks can use a different language. "As in the game"
  keeps the original voice selection. A missing translated file falls back to the original.
- In the game, Insert (or L3+R3 on a gamepad) opens the port's menu (upscaler, resolution,
  effects). Keyboard: WASD move, arrows camera, Space Cross, Left Shift Circle, E Square,
  Q Triangle, 1/3 L1/R1, R/F L2/R2, Z/C L3/R3, I/K/J/L d-pad, Enter Options, Tab touchpad
  (left half), Backspace touchpad (right half). Xbox controllers: Back + d-pad left/right clicks
  the left/right touchpad half (gestures / key items), Back alone the left half.

Data
- Saves and shader caches: user\ next to Bloodborne.exe (the launcher can pick another folder).
- Settings: bbport.ini next to Bloodborne.exe; launcher options in %APPDATA%\bbport-launcher.
- Generated files (prepared game image, patches): out\.

Online play (beta)
- In the launcher open "Online play" and tick "Play online". Then choose who you play with:
  - The Hunter's Dream (the default): co-op together with bbhost players. Press "Sign in with
    Discord" once and enter the code on the page that opens.
  - shadNet server: register an account at https://shadnet.shadps4.net and enter the account
    name (not the email) and password.
  Press "Test connection", then PLAY and choose online at the title screen.
- Messages, bloodstains and ghosts come from The Hunter's Dream (https://thehuntersdream.com).
  Everyone in a session needs game version 1.09 and the same co-op pool.
- Please don't ask The Hunter's Dream for help with bbport: use our Discord server
  (https://discord.gg/yTMG8c4Bqm) instead.

Cheats
- The "Cheats" page (expert view) has cheats (never die, enemies do not see or hear you,
  Rally never fades, control the targeted enemy) and gameplay tweaks (no Rally, camera further
  away, no camera auto-rotation, run with less stick tilt, ragdoll physics, the Japanese/Asian
  button layout: Circle confirms and interacts, Cross cancels and dodges, and the button icons
  follow). They are game patches for 1.09, applied when the game starts.

Problems
- Help checks the game folder and this PC and has the known problems with their fixes; after a
  game that ended with an error it names the cause its log shows.
- Black screen at start: Help -> "Only a black screen" -> "Clear the shader cache", then start
  again (the first minutes stutter while the cache is rebuilt).
- Help -> "Save a report" makes the zip the helpers ask for (the log, the settings without
  passwords, this PC): attach it to any bug report.

Upscaling
- FSR 4.1.1 (experimental): choose FSR 4.1.1 in Upscaler, then AMD DX12 or
  Vulkan (fsr4vk) on "All graphics", and press PLAY. A method
  change applies at the next game start. AMD DX12 uses the included original AMD
  runtime through a shared GPU-fence bridge. Vulkan uses the included fsr4vk INT8
  provider with the AMD Windows fix. All graphics -> Download fsr4vk repairs missing
  or changed files (about 14 MB). Files in fsr4vk\ survive launcher updates.
  Existing profiles keep their saved method; new AMD profiles default to DX12.
  Standard outputs up to 3840 x 2160 were tested. See docs\FSR411_WINDOWS.md
  for hardware requirements and validation; these providers need no Linux setup.
- DLSS: NVIDIA GeForce RTX 20 series or newer with a current driver (All graphics ->
  Upscaler -> DLSS, or the in-game menu). On other GPUs the option is greyed out.
- All graphics -> "Motion-vector cache (experimental)" is off by default. With FSR and
  object motion vectors enabled, it can reuse render targets to reduce GPU waits,
  at the cost of additional VRAM. Applies at the next game start, including shortcuts.
  See docs\OBJECT_MOTION_CACHE.md for comparison steps. The published test.2 build
  retains its earlier automatic-cache behavior.
- FSR 3.1 works on every GPU. FSR 4 needs its assets in fsr4_shaders\ (included in this
  package, or All graphics -> "Download FSR 4 assets"); GPUs without the required shader
  features fall back to FSR 3.1 by themselves.

Frame generation
- All graphics -> Frame generation offers AMD ML-FG 4.0.1 on a supported RX 9000,
  alongside the existing FSR 3.1 and DLSS methods. ML-FG generates one intermediate
  image per game frame (2x, SDR). The launcher checks the selected GPU, Windows 11,
  driver/runtime and GPU sharing; an unsupported saved selection falls back to FSR 3.1.
  With HDR output active, bbport displays rendered game frames and skips FG.
  Details and measurements: docs\ML_FRAME_GENERATION_WINDOWS.md.

Mods and patches
- Put each mod in its own folder under mods\ (dvdroot_ps4\..., or chr\, parts\, ... directly);
  enable and order them on "Mods & patches". The game files are never changed. Without Windows
  Developer Mode the port links folders with junctions and files with hard links; when the game
  is on another drive, the temporary mod view is made next to the game folder.
- Third-party patches: shadPS4/GoldHEN XML files for 1.09 in patches\.

Credits
- Online play: shadPS4 0.20's network code and shadNet by the shadPS4 team, put together by
  Awaken (https://github.com/Awakenchan/BloodBorneButPc) with the backend for The Hunter's Dream
  from bbhost by droogie (GPL-3.0-or-later); thanks to Metr1k and The Hunter's Dream team.
  The first online version was built on shadp2p by Wozzardman.
- bbport (the Linux port this is built on): https://github.com/deadinside28/bloodborne_pc
- Windows port: https://github.com/Supermedo/bloodborne_pc
- The full list of projects and patch authors is in README.md (Credits and licenses).
- License: the port's own code is GPL-2.0-or-later; with its GPL-3.0-or-later parts (the online
  code from bbhost, the FSR 4.1 launcher files) the program as distributed is GPL-3.0-or-later
  (licenses\GPL-3.0-bbhost-online-code.txt).
