// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: translations for the in-game overlay menu. English is the base language and the key
// of every string; an untranslated literal falls back to itself. bbport.ini key ui_language
// (or BB_UI_LANGUAGE) selects the language; the selector sits in the menu's top-right corner.

#pragma once

namespace BbText {

enum Language : int { English = 0, Russian, German, French, Spanish, Italian, LanguageCount };

/// Translation of an English literal for the active language (the literal itself when the
/// language is English, the entry is missing, or the translation is empty).
const char* Tr(const char* key);

/// Parse/format for the bbport.ini value: "en", "ru", "de", "fr", "es", "it".
int LanguageFromCode(const char* code);
const char* CodeFromLanguage(int language);
/// Endonym for the language selector: "English", "Русский", "Deutsch", ...
const char* LanguageName(int language);

/// Test/audit access to the table: EntryCount() strings, EntryText(i, English) is the key.
int EntryCount();
const char* EntryKey(int index);
const char* EntryText(int index, int language);

} // namespace BbText
