# Making mods for the port

The port loads loose-file mods (a folder with `dvdroot_ps4` inside, see [MODS.md](MODS.md)) without changing
the game files. A few of the game's files are also changed by the port itself at every start. This page lists
them and says what happens when your mod brings its own version, so a mod and the port both keep what they
change.

## The files the port changes

| File (under `dvdroot_ps4`) | What the port does | Your mod's version |
|---|---|---|
| `msg/<language>/menu.msgbnd.dcx` | adds the texts of Quit to Desktop, PC Settings and Rebirth (new text ids only) | always works: the port adds its texts to yours |
| `menu/optionsetting.gfx` | the System menu gets rows for Quit to Desktop and the three PC Settings lines | the rows are added to your movie; if its list cannot be grown, Quit to Desktop and PC Settings stay off |
| `menu/title.gfx`, `title_dlc.gfx`, `title_dlc_eu.gfx` | the title menu gets a Quit to Desktop row | as above, Quit to Desktop stays off if it cannot be added |
| `script/talk/m21_00_00_00.talkesdbnd.dcx` | the Doll's menu gets Rebirth | Rebirth is added to your script; without the Doll's menu in it, Rebirth stays off |
| `param/gameparam/gameparam.parambnd.dcx` | the field of view setting scales LockCamParam; Rebirth reads the weapons' requirements from EquipParamWeapon | your cameras are scaled and your requirements are used |
| `menu/*.gfx` | ultrawide outputs: the HUD's corner groups and full-screen backdrops are widened | your movie is widened; one that cannot be read stays as you made it |
| `msg/<language>/item.msgbnd.dcx` | read for the Discord status (place and boss names) | your names are shown |

The list itself is `scripts/port_files.py`; `run.py` and the launcher read it from there.

## How a start puts it together

1. The mods are applied over the game in the launcher's order (lower in the list wins a file, whole).
2. For each file in the table that a mod brings, the port makes its change on that mod's file instead of the
   game's (`mods.py --sources`).
3. The port's results go on top. Every other file of your mod is used exactly as it is.

When the port cannot make its change on your file, the feature is off for that start instead of half there
(no menu row without its text). The log says so, for example:

```text
Mods: My UI's dvdroot_ps4/menu/optionsetting.gfx cannot take PC Settings in the System menu: it is off for this start
```

The launcher shows the same under your mod on the Mods page, and the Play page names it. `out/mod-report.json`
has it as data. Players can also press **Start once without mods** to see whether a mod causes a problem.

## Hints

- Ship only the files you change. A full copy of `msg/` or `param/` works, but every file in it is one more
  place where another mod or a port update meets yours.
- Text mods and translations need nothing special: the port's texts are added to your archives in every
  language.
- If your mod replaces the System menu movie or the Doll's talk script, keep the game's list and the Doll's
  menu in it. Then the port can add its rows.
- `eboot.bin`, `sce_module` and `sce_sys` cannot be replaced by a mod. Code changes go through patch files
  (shadPS4/GoldHEN XML, Mods & patches > Third-party patches).
