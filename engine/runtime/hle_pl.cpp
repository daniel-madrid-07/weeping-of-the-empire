// nn::pl shared fonts: the Switch's system fonts. TotK asks for them only in Korean and Chinese and waits until
// GetSharedFontLoadState says Loaded; unimplemented (0 = Loading) it never drew a first frame in those languages (boots
// rs-cjk-ko-0051 and rs-cjk-zh-hans-0051 on package 0051), and game.language follows Windows' display language, so every
// Korean or Chinese Windows player hung at the first start. The fonts come from the player's own firmware, optional in the
// Setup: the pipeline writes <extracted>/fonts/<type>-<name>.bfttf (pipeline/nx/fonts.py) next to romfs/ (TOTK_ROMFS);
// TOTK_SHARED_FONTS=<dir> points elsewhere. As on the console (pl:u's shared memory holds the decoded fonts), the address
// the game gets is the decoded font, loaded once per type and kept for the process. A type whose file is missing gets the
// standard font (type 0) instead. Without the firmware the Windows font for the game's language stands in. The files:
// pl_fonts.h.
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <vector>

#include "hle.h"
#include "pc_lang.h"
#include "pl_fonts.h"

namespace {
constexpr int kFontTypes = 6;   // nn::pl::SharedFontType: standard, zh-CN, zh-CN extension, zh-TW, ko, Nintendo extension
enum : u32 { kLoading = 0, kLoaded = 1 };   // nn::pl::SharedFontLoadState

struct Font { pl_fonts::bytes ttf; bool tried = false; };
Font g_fonts[kFontTypes];
Font g_windows;            // the Windows font standing in (one, for the language the first ask was made for)
std::mutex g_mtx;

std::string fonts_dir() {
    if (const char* e = getenv("TOTK_SHARED_FONTS"); e && *e) return e;
    const char* r = getenv("TOTK_ROMFS");
    if (!r || !*r) return "";
    std::filesystem::path p(r);
    if (!p.has_filename()) p = p.parent_path();   // (a trailing separator)
    return (p.parent_path() / "fonts").string();
}

std::string windir() {
#ifdef _WIN32
    const char* w = getenv("WINDIR");
    return w && *w ? w : "C:\\Windows";
#else
    return "";
#endif
}

const Font* firmware(int t) {   // g_mtx held; null when the type's file is missing or not a BFTTF
    Font& f = g_fonts[t];
    if (f.tried) return f.ttf.empty() ? nullptr : &f;
    f.tried = true;
    const std::string dir = fonts_dir(), path = pl_fonts::bfttf_path(dir, t);
    f.ttf = pl_fonts::bfttf_decode(pl_fonts::read_file(path));
    if (f.ttf.empty())
        printf("[pl] shared font %d: no usable %d-*.bfttf in %s (the firmware's fonts, extracted by the pipeline; "
               "TOTK_SHARED_FONTS=<dir>)\n", t, t, dir.empty() ? "(no TOTK_ROMFS)" : dir.c_str());
    else
        printf("[pl] shared font %d: %s (%zu bytes)\n", t, path.c_str(), f.ttf.size());
    return f.ttf.empty() ? nullptr : &f;
}

const Font* windows_font(const char* lang) {   // g_mtx held; the first call's language decides the file
    if (!g_windows.tried) {
        g_windows.tried = true;
        const std::string path = pl_fonts::windows_font_path(lang, windir());
        g_windows.ttf = pl_fonts::sfnt_face0(pl_fonts::read_file(path));
        printf("[pl] no firmware fonts: %s for %s %s\n", path.empty() ? "(no Windows folder)" : path.c_str(), lang,
               g_windows.ttf.empty() ? "is missing or not a font" : "stands in");
    }
    return g_windows.ttf.empty() ? nullptr : &g_windows;
}

const Font* font(u64 type) {   // the type's font, else the standard one, else the Windows one (each logged once), else null
    const int t = (int)(u32)type;
    if (t < 0 || t >= kFontTypes) return nullptr;
    const char* lang = pc_lang_code();   // (before g_mtx: pc_lang_code's first call takes it through pc_lang_fonts_present)
    std::lock_guard<std::mutex> l(g_mtx);
    if (const Font* f = firmware(t)) return f;
    static bool told[kFontTypes];
    const Font* f = t ? firmware(0) : nullptr;
    if (!f) f = windows_font(lang);
    if (f && !told[t]) {
        told[t] = true;
        printf("[pl] shared font %d: %s in its place\n", t, f == &g_windows ? "the Windows font" : "the standard font");
    }
    return f;
}
}  // namespace

// pc_lang.cpp, while it chooses the game's language (so it gets the code, never pc_lang_code() itself): Korean and Chinese
// keep their language when a font can show them: the firmware's (its standard font decodes) or the language's Windows
// font (its face decodes, pl_fonts::sfnt_face0); with neither, English. Both are kept for the game's later asks.
extern "C" int pc_lang_fonts_present(const char* lang) {   // (pc_lang.cpp's default yields to it)
    std::lock_guard<std::mutex> l(g_mtx);
    Font& f0 = g_fonts[0];
    const bool ok = pl_fonts::font_for_language(lang, fonts_dir(), windir(), &f0.ttf, &g_windows.ttf);
    f0.tried = true;
    if (f0.ttf.empty()) g_windows.tried = true;   // (the Windows font was read for this language: keep the result)
    printf("[pl] fonts for %s: %s\n", lang, !f0.ttf.empty() ? "the firmware's" : ok ? "the Windows font (no firmware)" : "none");
    return ok;
}

HLE_AS(pl_RequestSharedFontLoad, "_ZN2nn2pl21RequestSharedFontLoadENS0_14SharedFontTypeE") { font(c->x[0]); }
HLE_AS(pl_GetSharedFontLoadState, "_ZN2nn2pl22GetSharedFontLoadStateENS0_14SharedFontTypeE") {
    c->x[0] = font(c->x[0]) ? kLoaded : kLoading;
}
HLE_AS(pl_GetSharedFontAddress, "_ZN2nn2pl20GetSharedFontAddressENS0_14SharedFontTypeE") {
    const Font* f = font(c->x[0]);
    c->x[0] = f ? (u64)(uintptr_t)f->ttf.data() : 0;   // (guest memory is identity-mapped: rt.h)
}
HLE_AS(pl_GetSharedFontSize, "_ZN2nn2pl17GetSharedFontSizeENS0_14SharedFontTypeE") {
    const Font* f = font(c->x[0]);
    c->x[0] = f ? (u64)f->ttf.size() : 0;
}
