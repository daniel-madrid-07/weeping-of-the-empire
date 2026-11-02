// font_check.h: a Windows font file for the F10 overlay's ImGui atlas only when it reads as a font (rival parity:
// totk-pc-recomp #176, a damaged or unusual system font closed the game a few seconds after start). ImGui's atlas build
// asserts (IM_ASSERT_USER_ERROR -> assert -> abort, our builds keep asserts) when stb_truetype cannot init a font, and a
// cut-off file can pass the init and read past its end. So: the sfnt / .ttc table directory inside the file
// (pl_fonts::sfnt_face0, a collection's first face on its own), then stb_truetype's own init, the check ImGui asserts on,
// and the ASCII glyphs the menu draws (loca / glyf bounds, under a fault guard). Its own static stb_truetype copy (as
// hle_osd.cpp has).
// Header-only so tools/engine/font_check_test.cpp checks it without the overlay.
#pragma once
#include "pl_fonts.h"

#ifndef STB_TRUETYPE_IMPLEMENTATION
#define STB_TRUETYPE_IMPLEMENTATION
#endif
#define STBTT_STATIC
#include "linux/stb_truetype.h"
#ifdef _WIN32
#include <windows.h>   // (__try / EXCEPTION_EXECUTE_HANDLER)
#endif

// A table's length in the sfnt's directory (0: none; sfnt_face0 checked the directory and every table against the size)
inline uint32_t font_check_table_len(const uint8_t *f, const char *tag) {
    const unsigned n = (unsigned)f[4] << 8 | f[5];
    for (unsigned i = 0; i < n; i++)
        if (!memcmp(f + 12 + 16 * i, tag, 4)) return pl_fonts::be32(f + 12 + 16 * i + 12);
    return 0;
}
// stb_truetype's init (the check ImGui asserts on), then each ASCII glyph the menu draws: its loca entry inside loca and
// its outline inside glyf (a TrueType font), and its box read. Plain data only, so a fault inside stb_truetype on a
// damaged font (CFF outlines have no loca to bound) is caught here and the font skipped.
inline bool font_check_probe(const uint8_t *f) {
#ifdef _WIN32
    __try {
#endif
        stbtt_fontinfo info;
        if (!stbtt_InitFont(&info, f, stbtt_GetFontOffsetForIndex(f, 0))) return false;
        if (!stbtt_FindGlyphIndex(&info, 'A')) return false;   // (a font without Latin letters draws the menu as boxes)
        const uint32_t loca_len = font_check_table_len(f, "loca"), glyf_len = font_check_table_len(f, "glyf");
        const uint32_t entry = info.indexToLocFormat ? 4 : 2;
        for (int ch = 0x20; ch < 0x7F; ch++) {
            const int g = stbtt_FindGlyphIndex(&info, ch);
            if (g < 0 || g >= info.numGlyphs) return false;
            if (info.glyf) {
                if ((uint64_t)(g + 2) * entry > loca_len) return false;
                const uint8_t *l = f + info.loca + (size_t)g * entry;
                const uint32_t g1 = entry == 4 ? pl_fonts::be32(l) : 2u * ((uint32_t)l[0] << 8 | l[1]);
                const uint32_t g2 = entry == 4 ? pl_fonts::be32(l + 4) : 2u * ((uint32_t)l[2] << 8 | l[3]);
                if (g1 > g2 || g2 > glyf_len) return false;
            }
            int x0, y0, x1, y1;
            stbtt_GetGlyphBox(&info, g, &x0, &y0, &x1, &y1);
        }
        return true;
#ifdef _WIN32
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#endif
}

// The font's bytes, ready for AddFontFromMemoryTTF (keep them alive: FontDataOwnedByAtlas = false), or empty when the
// file is missing, damaged or no font
inline pl_fonts::bytes font_check_file(const std::string &path) {
    pl_fonts::bytes f = pl_fonts::sfnt_face0(pl_fonts::read_file(path));
    if (f.empty() || !font_check_probe(f.data())) return {};
    return f;
}
