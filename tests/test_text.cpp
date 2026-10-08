// SPDX-License-Identifier: GPL-2.0-or-later
// The overlay translation table: every string must exist in every language, and a translated
// format string must keep the printf specifiers of its English key.
#include <cassert>
#include <cstdio>
#include <cstring>
#include "bbport_text.h"
#include "bbport_settings.h"

namespace {

// The next printf directive of a format string ("%..." up to the conversion), or false at the
// end of the string.
bool NextSpecifier(const char*& text, char* spec, size_t cap) {
    while (*text && *text != '%') {
        ++text;
    }
    if (!*text) {
        return false;
    }
    size_t n = 0;
    spec[n++] = *text++;
    if (*text == '%') { // a literal %%
        spec[n++] = *text++;
        spec[n] = 0;
        return true;
    }
    while (*text && !std::strchr("diouxXeEfgGcspr", *text) && n + 1 < cap) {
        spec[n++] = *text++;
    }
    if (*text && n + 1 < cap) {
        spec[n++] = *text++;
    }
    spec[n] = 0;
    return true;
}

bool SameSpecifiers(const char* a, const char* b) {
    char sa[32], sb[32];
    while (true) {
        const bool has_a = NextSpecifier(a, sa, sizeof(sa));
        const bool has_b = NextSpecifier(b, sb, sizeof(sb));
        if (has_a != has_b) {
            return false;
        }
        if (!has_a) {
            return true;
        }
        if (std::strcmp(sa, sb) != 0) {
            return false;
        }
    }
}

} // namespace

int main() {
    using namespace BbText;
    // bbport.ini codes round-trip; unknown or missing codes fall back to English.
    for (int i = 0; i < LanguageCount; ++i) {
        assert(LanguageFromCode(CodeFromLanguage(i)) == i);
        assert(LanguageName(i) && LanguageName(i)[0]);
    }
    assert(LanguageFromCode("") == English);
    assert(LanguageFromCode("nope") == English);
    assert(LanguageFromCode(nullptr) == English);
    assert(LanguageFromCode("ru") == Russian);
    assert(LanguageFromCode("it") == Italian);
    assert(LanguageFromCode("zh") == Chinese);
    assert(LanguageFromCode(CodeFromLanguage(1000)) == English); // out of range clamps

    // Every entry carries every language, and the format specifiers match the key.
    assert(EntryCount() > 40);
    for (int i = 0; i < EntryCount(); ++i) {
        const char* key = EntryKey(i);
        assert(key && key[0]);
        for (int language = 0; language < LanguageCount; ++language) {
            const char* text = EntryText(i, language);
            assert(text && text[0]);
            if (!SameSpecifiers(key, text)) {
                std::fprintf(stderr, "specifier mismatch (%s):\n  key:  %s\n  text: %s\n",
                             CodeFromLanguage(language), key, text);
                return 1;
            }
            if (language == English) {
                assert(std::strcmp(text, key) == 0);
            }
        }
    }

    // Tr follows the settings and falls back to the key itself.
    auto& s = BbSettings::Get();
    s.ui_language = English;
    assert(std::strcmp(Tr("Preset"), "Preset") == 0);
    assert(std::strcmp(Tr("no such key"), "no such key") == 0);
    s.ui_language = Russian;
    assert(std::strcmp(Tr("Preset"), "Пресет") == 0);
    s.ui_language = German;
    assert(std::strcmp(Tr("Close"), "Schließen") == 0);
    s.ui_language = French;
    assert(std::strcmp(Tr("Close"), "Fermer") == 0);
    s.ui_language = Spanish;
    assert(std::strcmp(Tr("Close"), "Cerrar") == 0);
    s.ui_language = Italian;
    assert(std::strcmp(Tr("Close"), "Chiudi") == 0);
    s.ui_language = Chinese;
    assert(std::strcmp(Tr("Close"), "关闭") == 0);
    s.ui_language = English;
    assert(EntryText(0, English) == EntryKey(0));

    std::puts("Overlay text: all languages complete, printf specifiers match PASS");
}
