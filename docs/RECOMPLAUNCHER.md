# RecompLauncher integration (Windows)

The Windows package includes `recomplauncher.json` beside `Bloodborne.exe`. It provides
folder selection, windowless content preparation, direct game launch, common graphics
settings, save backup locations and save/log folder actions. The native launcher remains
available for advanced graphics, accounts, mods, input configuration and other options.

## Your game folder

Select your own decrypted Bloodborne dump of any retail edition (`patches.BLOODBORNE_IDS`:
US CUSA00900, EU CUSA00207, the Game of the Year editions such as CUSA03173, ...) with the
**1.09 update applied**. Setup checks `eboot.bin`, `sce_module/libc.prx`, `dvdroot_ps4`, the
title ID and the loaded executable's clean-image SHA-256 (`scripts/clean_eboot.py`), which is
the same 1.09 executable for every edition (Game of the Year dumps say APP_VER 01.00 over it).
This integration requires the clean executable; it never searches for a replacement or
repairs the source dump. Preparation cannot verify every asset in a decrypted folder, and
does not claim a whole-disc verification badge.

## Preparation and launch

RecompLauncher invokes these commands from the installation folder:

```text
Bloodborne.exe --prepare-content --game-dir=D:\Games\Bloodborne\CUSA03173
Bloodborne.exe --recomplauncher-play --game-dir=D:\Games\Bloodborne\CUSA03173
```

Both commands use inherited standard output/error and return an error without opening
the native launcher. Preparation prints `progress <0..1> <message>` lines and returns zero
only after the executable, module links, content profile and patches have been generated.
It does not start the game or perform GPU capability discovery. Direct launch returns the
native game's exit code; the existing `--play` behavior is unchanged.

RecompLauncher runs preparation again after every new release, without `--game-dir`
(`keeps_copy`). When this release already prepared the cache, it prints
`progress 1 Already prepared` and exits 0 without reading the game folder. Otherwise it
prepares again from the game folder the last preparation recorded; when that folder is gone,
it fails and RecompLauncher asks for the game folder again. The game itself still reads your
game folder while it plays.

Generated files live in `out/recomplauncher` under the data directory (the installation
folder by default). Setup uses a separate `out/recomplauncher.preparing` transaction and
only replaces a cache carrying this integration's ownership marker, which is written last
with the release's version. A failed setup keeps the previous cache. Direct launch requires
that preparation has created a managed cache. An interrupted transaction is preserved where
necessary; keep it for recovery or move it aside before retrying, after all preparation
processes have stopped. The cache must be separate from the source dump. Preparation does
not delete unrelated views beside the dump or in the system temporary directory.

The native runner still refreshes settings-dependent patches, mods and links on launch.
The setup cache is not a promise that later launches can skip native preparation.

## Updates

RecompLauncher installs this port's releases. When it starts the port (`RECOMPLAUNCHER=1`),
the launcher window does not check for updates when it opens, and its Check for updates,
Update and Other versions install buttons only say that RecompLauncher installs the updates.

## Settings and user data

The manifest exposes resolution, upscaler and quality, frame rate, full screen, V-Sync,
motion blur, depth of field, chromatic aberration and intro skipping. Values use the
native `bbport.ini` and `%APPDATA%/bbport-launcher/settings.json` formats. Unlocked frame
rate retains the native automatic cap; it is not an unlimited-frame-rate override.

The selected source path is passed for this session without replacing the game folder
saved in the native launcher. Native controller detection, button prompts, custom user/cache
paths and other saved preferences are retained. Full screen changes only when the user
chooses it in settings; no controller or full-screen environment overrides are added.

Save backup locations and folder actions target the default `user/savedata`,
`user/trophies.log`, `bbport.ini`, `input.ini` and the Seamless co-op saves
(`user/seamless-coop/savedata`, `seamless/user/savedata`). RecompLauncher's own size/type
filters still apply. If you configured a custom user directory, its saves remain there;
adjust the manifest paths to match before relying on those actions or backups.
The global launcher settings JSON is deliberately excluded from backups because it can
contain account credentials. Settings editing preserves unrelated keys in that file.

Exit code 1 reports validation, preparation or startup failures; 2 reports invalid command
arguments; 130 reports an interrupted operation. Native game exits are passed through.

## Platforms

Only the Windows package carries `recomplauncher.json`. The Linux AppImage (a pre-release
today) does not: its launcher has no `--prepare-content` or `--recomplauncher-play`. Running
the Windows package through Proton on Linux or the Steam Deck is untested.

## Development checks

```text
python -m unittest discover -s tests -p test_recomplauncher.py
```

These tests cover preparation transactions and rollback, the already-prepared check,
setup without the game folder, source/preference preservation, title/hash validation,
direct launch, path handling and the runner's preparation-only boundary. They require no
game files or graphics device. Windows packaging copies the manifest and this guide into
the release; PyInstaller includes the integration module.
