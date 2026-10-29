# Trophy localization and artwork references

The 40 trophy names and descriptions in `launcher/bbport_lang_trophies.py` follow the official
PlayStation Network (PSN) trophy wording for Bloodborne's `NPWR05818_00` catalog across all supported
launcher languages:
- Simplified Chinese (`zh`)
- Russian (`ru`)
- Japanese (`ja`)
- Korean (`ko`)
- German (`de`)
- French (`fr`)
- Spanish (`es`)
- Italian (`it`)
- Portuguese (`pt`)
- Polish (`pl`)
- Turkish (`tr`)

Translations are not machine-translated or editorial rephrasings of the English catalog. They preserve
official PSN terminology, punctuation, capitalization, and line breaks for each language, including
keeping the platinum title `Bloodborne` where specified by Sony.

## Provenance and sources

- **Chinese (`zh`)**: Verified against the public PSN list mirror on [PSNine](https://www.psnine.com/psngame/5818)
  and its 40 individual trophy pages (entries 1–40 corresponding to launcher IDs 0–39).
- **Japanese (`ja`)**: Verified against official FromSoftware / Sony Japanese PSN trophy listings and the
  [Bloodborne Japan Wiki](https://seesaawiki.jp/bloodbornewiki/).
- **Other supported languages (`ru`, `ko`, `de`, `fr`, `es`, `it`, `pt`, `pl`, `tr`)**: Verified against
  official PSN trophy database mirrors ([Exophase PS4 NPWR05818_00](https://www.exophase.com/game/bloodborne-ps4/trophies/)).
- PlayStation identifies Bloodborne as PS4 software playable on PS5; this is a unified trophy catalog.

No game archives, decryption keys, Sony proprietary binary data, or user account information are bundled.
Launcher-specific controls, progress filters, empty states, and notification chrome use the launcher's
standard modular translation tables.

## Trophy artwork

The achievement cards and unlock notifications read `TROP000.PNG` through `TROP039.PNG` directly from the
selected game's `sce_sys/trophy/trophy00.trp`. These PNG entries are stored unencrypted in the TRP archive;
encrypted metadata entries (`*.ESFM`) are skipped. No extraction, external downloads, or bundled game images
are needed. The archive is accessed read-only.

The launcher scales artwork dynamically with Pillow and dims locked trophies. Grade totals retain their cup
symbols. Missing, unsupported, or damaged artwork cleanly falls back to the grade cup icon, ensuring the page
remains fully functional even if no trophy archive is present. Windows packaging installs Pillow for the frozen
launcher.
