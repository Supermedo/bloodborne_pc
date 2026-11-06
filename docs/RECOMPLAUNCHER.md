# RecompLauncher integration (Windows)

The Windows package includes `recomplauncher.json` beside `Bloodborne.exe`. It provides
folder selection, windowless content preparation, direct game launch, common graphics
settings, save backup locations and save/log folder actions. The native launcher remains
available for advanced graphics, accounts, mods, input configuration and other options.

## Your game folder

Select your own decrypted **CUSA03173** dump with the **1.09 update applied**. Setup checks
`eboot.bin`, `sce_module/libc.prx`, `dvdroot_ps4`, title/update metadata and the loaded
executable's clean-image SHA-256. The accepted image hash is defined in
`scripts/clean_eboot.py`. This integration requires the clean executable; it never searches
for a replacement or repairs the source dump. Preparation cannot verify every asset in a
decrypted folder, and does not claim a whole-disc verification badge.

## Preparation and launch

RecompLauncher invokes these commands from the installation folder:

```text
Bloodborne.exe --prepare-content --game-dir "D:\Games\Bloodborne\CUSA03173"
Bloodborne.exe --recomplauncher-play --game-dir "D:\Games\Bloodborne\CUSA03173"
```

Both commands use inherited standard output/error and return an error without opening
the native launcher. Preparation prints `progress <0..1> <message>` lines and returns zero
only after the executable, module links, content profile and patches have been generated.
It does not start the game or perform GPU capability discovery. Direct launch returns the
native game's exit code; the existing `--play` behavior is unchanged.

Generated files live in `out/recomplauncher` under the data directory (the installation
folder by default). Setup uses a separate `out/recomplauncher.preparing` transaction and
only replaces a cache carrying this integration's ownership marker. A failed setup keeps
the previous cache. Direct launch requires that preparation has created a managed cache.
An interrupted transaction is preserved where necessary; keep it for
recovery or move it aside before retrying, after all preparation processes have stopped.
The cache must be separate from the source dump. Preparation does not delete unrelated
views beside the dump or in the system temporary directory.

The native runner still refreshes settings-dependent patches, mods and links on launch.
The setup cache is not a promise that later launches can skip native preparation.

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
`user/trophies.log`, `bbport.ini` and `input.ini` paths. RecompLauncher's own size/type
filters still apply. If you configured a custom user directory, its saves remain there;
adjust the manifest paths to match before relying on those actions or backups.
The global launcher settings JSON is deliberately excluded from backups because it can
contain account credentials. Settings editing preserves unrelated keys in that file.

Exit code 1 reports validation, preparation or startup failures; 2 reports invalid command
arguments; 130 reports an interrupted operation. Native game exits are passed through.

## Development checks

```text
python -m unittest discover -s tests -p test_recomplauncher.py
```

These tests cover preparation transactions and rollback, source/preference preservation,
title/hash validation, direct launch, path handling and the runner's preparation-only
boundary. They require no game files or graphics device. Windows packaging copies the
manifest and this guide into the release; PyInstaller includes the integration module.
