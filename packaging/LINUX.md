# Linux build

The Linux version is the native `bb-probe` (ELF) with `run.sh` and the GTK4 launcher
(`launcher/bbport_launcher.py`), packaged with Nix as an AppImage (Steam Deck and most desktops).

On a Linux PC with Nix:

    bash build.sh                 # nix-shell shell.nix is entered by itself
    bash packaging/appimage.sh    # dist/Bloodborne-bbport-x86_64.AppImage

`nix-build packaging` alone gives `result/bin/bbport` (launcher) and `result/bin/bbport-game` (the game with the
saved settings). Data (saves, `bbport.ini`, generated files): `~/.local/share/bbport`. Launcher settings:
`~/.config/bbport-launcher/settings.json`, the Linux launcher's own (not the Windows launcher's settings;
readable by the user only, as it holds the SuperMedo account's password).

## Online on Linux

The launcher's **Online** page: SuperMedo Server only (`launcher/bbport_linux_online.py`), with the
account (Sign in with Discord, an Online ID and password checked by the server, or a new account), UPnP, a
connection test and the party box (party code, join, kick, leave; SuperMedo Server's `/party/v1`). Cheats,
debug features and third-party patches stay off in online play (`patches.py` leaves them out); Play asks
first: online without them, or offline with them for this start. The launcher writes `user/host_overrides.json` for each
start, as `run.py` does on Windows; `user/host_overrides.custom.json` is used as it is, unless it is BBSC's.

**SuperMedo Seamless co-op is Windows only.** BBSC is a Windows program: `bbsc-launch.exe` starts the
Windows `bb-probe.exe` and injects `bbsc.dll` with `CreateRemoteThread`, and the DLL is built on `windows.h`,
WinHTTP and Winsock (CLSAMG22/BBSC, `emulator/mod`). It cannot start the Linux game, so the Online page shows
the choice as unavailable and has no Seamless difficulty or options. A user folder brought from Windows keeps
Seamless co-op's saves in `user/seamless-coop`; the launcher warns when that folder is chosen as the saves
folder (BBSC's goods there show as `?GoodsName?` without BBSC).

## Start steps from Windows

`run.sh` calls `scripts/linux_start.py`, which uses `run.py`'s own functions (so `run.py` ships in the
package) and answers through `out/linux-start.txt`: the menu layers below the mods (Quit to Desktop, PC
Settings, Rebirth at the Doll, BBSC's item names, the ultrawide HUD layout, the field of view), the trophy
pop-up's names and pictures (Pillow is in the package's Python), read-only saves made writable, the save
copy at start (launcher: Saves; once per launch, not again at an in-game restart), BBSC's bells put back in
characters played normally (never in a `seamless-coop` folder), `BB_UI_RES` for ultrawide and Sharp UI, the
low-VRAM profile, and the frame rate gate for a modified `eboot.bin`. A step that fails is left out and
the game starts anyway (without its texts, Quit to Desktop, PC Settings and Rebirth are off). The game view
`mods.py` makes is removed at exit through `mods.remove_overlay`, only when it is a `mod-game-*` folder that
is not the game folder; views a killed game left behind are removed at the next start while no `bb-probe` runs.
The launcher also has FreeSync / G-Sync (`BB_VRR`), Low latency (`BB_LOW_LATENCY`, Auto: NVIDIA Reflex +
Boost, off elsewhere) and the HUD position and field of view (`bbport.ini` `hud_position`, `fov`).

`packaging/appimage.sh` runs inside `nix-shell shell.nix` (it needs `readelf`) and needs the FSR 4 shaders
in `fsr4_shaders/` (not in git: the launcher's FSR 4 download, or a copy from a Windows install).

On a distribution without Nix that cannot install it (Bazzite, SteamOS): a distrobox with Nix inside, e.g.
`distrobox create -n bbport-build -i ubuntu:24.04`, then in it `sudo mkdir /nix && sudo chown $USER /nix`,
Nix's single-user install (`--no-daemon`) and the two commands above. With 8 GB of RAM, limit the compile
to about 4 jobs (a `ninja` wrapper with `-j4` first in `PATH`).

## Checked on Linux (Bazzite 44, GCC 16, Radeon RX 7600)

- `build.sh`, `build.sh --test` (all of the port's C tests) and `packaging/appimage.sh`; the AppImage opens
  the launcher.
- Linux-only build fixes this needed: `vk_instance.cpp` included `vk_low_latency.h` on Windows only,
  `vk_frame_generation.cpp` assigned `{w, h}` to a `vk::Extent2D` (ambiguous with newer Vulkan headers),
  `runtime_pad.c` missed `<errno.h>`, and the C tests' GPU stubs were behind `gpu/bbgpu.h`.

## Not yet checked on Linux

- The game itself on Linux since 1.9 (the C runtime and the GPU library: most work since then was tested
  on Windows only).
- UPnP with the miniupnpc of nixpkgs (it builds; 2.2.8 changed `UPNP_GetValidIGD`'s arguments).
- Online play itself: sign-in, summons and parties with a Windows player.
- 1.9.8.2 on Linux: the build, the start steps above in a real start (tested with stand-ins only:
  `tests/test_linux_start.py`), the trophy pictures, Rebirth and PC Settings in the game, Anti-Lag 2 on RADV.
