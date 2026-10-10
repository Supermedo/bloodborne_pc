# Button prompts and gestures menu

By **Dommo**, from [Nexus Mods: Bloodborne, mod 30](https://www.nexusmods.com/bloodborne/mods/30)
(uploaded by goomab): *DS3 Style - Modern Xbox prompts*, *DS3 Style - Nintendo Switch prompts* and
*Stripped down gesture menu*. The mod page allows using its assets and uploading them elsewhere
with credit to the author, but not in anything sold.

Only what the mods drew is here, never the game's own files:

- `xbox/`, `switch/`: the 20 button icons (`KG_*.dds`, BC7 exactly as in the mods) and the strip
  of big prompts from the shared UI atlas (`atlas.dds`, scaled down to the game's atlas size).
- `gesturetop.json`: the edits that turn the game's `menu/gesturetop.gfx` into the mod's.
- `keyboard/`: keyboard prompts in the same layout, drawn by `tools/make_keyboard_prompts.py`
  for the keys of `src/runtime_pad.c`.

`scripts/menu_prompts.py` puts them into the player's own `menu/common.tpf.dcx` and
`menu/gesturetop.gfx` at each start; `tools/extract_prompt_assets.py` rebuilds the Xbox, Switch
and gestures files from the mods' ZIPs.
