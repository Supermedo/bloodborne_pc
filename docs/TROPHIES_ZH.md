# Chinese trophy text references

The 40 trophy names and descriptions in `bbport_lang.py` follow the Simplified
Chinese PSN trophy wording reproduced for Bloodborne's `NPWR05818_00` list.
They are not editorial translations of the English catalog. Retain the displayed
terminology, punctuation and line breaks, including the platinum title
`Bloodborne`, instead of replacing them with community-preferred game terms.

References checked on 2026-10-21:

- [Chinese PSN trophy list mirror](https://www.psnine.com/psngame/5818).
- The individual trophy pages, from
  [platinum](https://www.psnine.com/trophy/5818001) to
  [Laurence](https://www.psnine.com/trophy/5818040), were checked separately.
  Mirror entry numbers 1–40 correspond to launcher trophy IDs 0–39.
- [PlayStation's Bloodborne listing](https://www.playstation.com/en-us/games/bloodborne/)
  identifies the game as PS4 software playable on PS5; this is not a separate
  PS5 trophy catalog.

The wording was checked against the public PSN mirror, not an authenticated
PlayStation API response or a direct capture from a PS5 console. No trophy
archives, images, decryption keys or user account data are included.

Launcher-specific controls, help, progress, empty states and notification text
remain launcher translations; this document does not attribute those strings to
the PS5 system interface. Selecting Chinese uses launcher language `zh` and does
not change the game's text-language setting.

## Trophy artwork

The achievement cards and unlock notifications read `TROP000.PNG` through
`TROP039.PNG` directly from the selected game's `sce_sys/trophy/trophy00.trp`.
These PNG entries are unencrypted; encrypted metadata is skipped. No extraction,
keys, downloads or bundled game images are needed. The archive is read only.

The launcher scales the artwork with Pillow and dims locked trophies. Grade
totals retain their cup symbols. Missing, unsupported or damaged artwork falls
back to the grade cup, so the page still works without a trophy archive. The
Windows packaging script installs Pillow for the frozen launcher.
