// pc_lang.cpp: the game's language (pc_lang.h). Until 10-27 the engine answered "en-US" whatever Windows said (the
// integration runtime's hle_oe.c followed the Windows locale); now game.language decides, Windows by default.
#include "pc_lang.h"
#include "pc_settings.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace {

// TotK 1.0's languages (its 14 message archives): the Switch codes the game maps to them
const char *const V_LANG[] = {"", "en-US", "en-GB", "es", "es-419", "fr", "fr-CA", "de", "it", "nl", "ru", "ja", "ko",
                              "zh-Hans", "zh-Hant", nullptr};
const char *const N_LANG[] = {"Automatic (Windows' language)", "English (US)", "English (UK)", "Español (España)",
                              "Español (Latinoamérica)", "Français", "Français (Canada)", "Deutsch", "Italiano",
                              "Nederlands", "Russian", "Japanese", "Korean", "Chinese (Simplified)",
                              "Chinese (Traditional)", nullptr};

const pc_setting k_lang[] = {
    {"game.language", "Game", "Language", "Language", "TOTK_LANG: the game's texts and voices' subtitles, and the PC "
     "settings' own texts (English or Spanish). Automatic: Windows' display language if the game has it, else English.",
     "", V_LANG, N_LANG, 0, 0, 0, PC_SET_RESTART, "TOTK_LANG", nullptr, nullptr, nullptr},
};
struct AddLangRows {
    AddLangRows() { pc_settings_add(k_lang, (int)(sizeof k_lang / sizeof k_lang[0])); }
} g_add_lang_rows;

// A BCP-47 tag ("es-ES", "es-MX", "en-GB", "zh-TW") -> the game's code; English (US) when it has no such language
const char *code_of_tag(const char *tag) {
    char lang[8] = {}, region[16] = {};
    size_t n = strcspn(tag, "-_");
    for (size_t i = 0; i < n && i < sizeof lang - 1; i++) lang[i] = (char)tolower((unsigned char)tag[i]);
    if (tag[n]) {
        const char *r = tag + n + 1;
        for (size_t i = 0; r[i] && r[i] != '-' && r[i] != '_' && i < sizeof region - 1; i++) region[i] = (char)toupper((unsigned char)r[i]);
        if (!strcmp(region, "HANT") || !strcmp(region, "HANS")) {   // zh-Hant-TW: the script decides
            return !strcmp(region, "HANT") ? "zh-Hant" : "zh-Hans";
        }
    }
    if (!strcmp(lang, "es")) return !region[0] || !strcmp(region, "ES") ? "es" : "es-419";
    if (!strcmp(lang, "en")) {
        static const char *const gb[] = {"GB", "IE", "AU", "NZ", "ZA", "IN", "MT"};
        for (const char *g : gb) if (!strcmp(region, g)) return "en-GB";
        return "en-US";
    }
    if (!strcmp(lang, "fr")) return !strcmp(region, "CA") ? "fr-CA" : "fr";
    if (!strcmp(lang, "zh")) return !strcmp(region, "TW") || !strcmp(region, "HK") || !strcmp(region, "MO") ? "zh-Hant" : "zh-Hans";
    static const char *const same[] = {"de", "it", "nl", "ru", "ja", "ko"};
    for (const char *s : same) if (!strcmp(lang, s)) return s;
    return "en-US";
}

const char *windows_code(char *tag, size_t n) {
    snprintf(tag, n, "-");
#ifdef _WIN32
    WCHAR name[LOCALE_NAME_MAX_LENGTH];
    if (LCIDToLocaleName(MAKELCID(GetUserDefaultUILanguage(), SORT_DEFAULT), name, LOCALE_NAME_MAX_LENGTH, 0) > 0)
        WideCharToMultiByte(CP_UTF8, 0, name, -1, tag, (int)n, nullptr, nullptr);
#else
    const char *l = getenv("LANG");   // "es_ES.UTF-8"
    if (l && *l) snprintf(tag, n, "%.*s", (int)strcspn(l, "."), l);
#endif
    return code_of_tag(tag);
}

}  // namespace

// Korean and Chinese text needs a shared font (hle_pl.cpp): the firmware's when the player gave it, else the Windows font
// for the language (Malgun Gothic, Microsoft YaHei / JhengHei). With neither those languages fall back to English (US),
// with a log line, rather than the game waiting for fonts that never load. pc_lang_fonts_present is hle_pl.cpp's in the
// game; the CPU tests that build this file alone get the default below (a font is there) in its place. A link-time choice,
// not a pointer set during static initialisation, so a caller from another TU's static initialiser gets the real answer
// too.
#ifdef _WIN32
extern "C" int pc_lang_fonts_present(const char *lang);
extern "C" int pc_lang_fonts_present_default(const char *) { return 1; }
#pragma comment(linker, "/alternatename:pc_lang_fonts_present=pc_lang_fonts_present_default")
#else
extern "C" __attribute__((weak)) int pc_lang_fonts_present(const char *) { return 1; }
#endif

static const char *with_fonts(const char *c) {
    if ((strncmp(c, "ko", 2) && strncmp(c, "zh", 2)) || pc_lang_fonts_present(c)) return c;
    printf("[lang] %s needs a shared font (the firmware's, or the Windows font for the language) and none is there: "
           "English (US) instead\n", c);
    return "en-US";
}

extern "C" const char *pc_lang_code(void) {
    static const char *code = [] {
        const char *e = getenv("TOTK_LANG");
        for (int i = 1; e && *e && V_LANG[i]; i++)
            if (!strcmp(e, V_LANG[i])) {
                printf("[lang] %s (game.language)\n", V_LANG[i]);
                return with_fonts(V_LANG[i]);
            }
        char tag[96];
        const char *c = windows_code(tag, sizeof tag);
        printf("[lang] %s (Windows' display language %s)\n", c, tag);
        return with_fonts(c);
    }();
    return code;
}

extern "C" const char *pc_lang_key(void) {
    const char *c = pc_lang_code();
    if (!strncmp(c, "zh-", 3)) return c;
    static char two[3];
    if (!two[0]) { two[0] = c[0]; two[1] = c[1]; }
    return two;
}

extern "C" int pc_lang_es(void) { return pc_lang_code()[0] == 'e' && pc_lang_code()[1] == 's'; }

#ifdef NXR_PC_LANG_TEST
extern "C" const char *pc_lang_code_of_tag(const char *tag) { return code_of_tag(tag); }
#endif
