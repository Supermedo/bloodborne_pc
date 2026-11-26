# Offline Chalice Glyphs: catalog tools

Builds `data/local_chalice/catalog.bin` (see `docs/LOCAL_CHALICE.md`).

1. `make_catalog.py <catalog.json> <Noxde dungeons.json> <out v3 catalog.bin> [<Reborn catalogue.json>]`:
   glyphs, FormData and creator names, with the community notes (format LCCAT003).
   Sources: [BB.Chalices.Reborn](https://github.com/LL4nc33/BB.Chalices.Reborn) `catalogue.json`
   (MIT) and [Noxde's dungeons.json](https://gist.github.com/Noxde/a29f699f4175bf315d9bd4baeebefb66)
   by Noxde (the creator names in the catalog come from here).
2. `make_catalog_v4.py <dvdroot_ps4/msg of a 1.09 dump> <v3 catalog.bin> <out catalog.bin> [<test fixture>]`:
   turns each note into a description recipe of game text ids (`short_desc.py` holds the rules)
   and adds the language table and `messages.json` (format LCCAT005). The optional fixture holds
   game text and stays local: it is for checking the C++ composer (`src/lc_desc.cpp`) against
   `short_desc.py`.

`fmg_lookup.py` reads the game's DCX / BND4 / FMG text archives.
