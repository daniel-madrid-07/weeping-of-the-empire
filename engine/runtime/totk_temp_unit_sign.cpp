// totk_temp_unit_sign.cpp: the Purah Pad map's temperature sign follows Options > Display > Temperature unit.
//
// The number is the game patch tempunit (games/totk/patches/sites_v100.py). The sign beside it is a picture in the layout
// PaAppMapTemp2_00 (P_CIcon_00 / _01), textured by that layout's RegionLanguageType animation: its frame is the
// language index, frame 1 (English (US)) shows the Fahrenheit texture, every other frame the Celsius one (the same
// animation also picks the number + sign spacing: N_Align_00 for frames 0, 1, 5, 13, N_AlignSp_00 for the rest). The
// frame is set by a generic layout-animation binder, a virtual method run when a layout binds its animations
// (main+0x91a290: RegionLanguageType -> frame = the language singleton's index, then the animation's setFrame and
// apply, vtable +0xd0 / +0x28). Four other layouts have a RegionLanguageType animation of their own (Location_00's
// caption style, EnvironmentInfo_00 / PaMapRecycleBox_00 positions, PaAppMapComp_00 spacing), so the binder itself
// stays: this hook runs it, then, for PaAppMapTemp2_00 only (the layout's name, +0x30: the prefix of its animation
// files), sets the frame again: 1 for Fahrenheit; for Celsius 0 when the language is English (US) (Japanese's frame:
// the same no-space spacing as frame 1), else the language's own frame stays. A forced Fahrenheit in a language whose
// frame uses the spaced variant shows the no-space one (a spacing difference only). The binder runs when the layout
// binds, so a changed setting shows the next time the map layout is made, not at once.
// gen.py --hook 91a290 in GEN_MAIN; 1.4.3: no place (engine_map HOOK_FREE: the sign follows the language there).
#include "rt.h"
#include "totk_globals.h"

#include <cstdio>
#include <cstring>

extern "C" void *totk_data_page_current(void);   // games/totk/runtime/totk_patches.c: the data page, or NULL

namespace {
constexpr u64 kTempUnit = 212;                        // data_page.h TOTK_DP_TEMP_UNIT (u32)
constexpr u64 kLayoutName = 0x30;                     // the binder's object: its layout's name (const char *)
constexpr u64 kFindAnim = totk_main(0x7298f0);        // (layout, tag, 0) -> the layout's animation for the tag, or 0
constexpr u64 kRegionLanguageType = totk_main(0x370c21f);   // "RegionLanguageType" (the binder's own tag string)
constexpr u64 kSetFrame = 0xd0, kApply = 0x28;        // the animation's vtable slots, as the binder calls them
constexpr u32 kEnglishUs = 1;
constexpr char kTempLayout[] = "PaAppMapTemp2_00";
}  // namespace

// The frame to force, or -1 to leave the binder's: unit from the page, lang = the language index (-1: unknown).
extern "C" int totk_temp_sign_frame(unsigned unit, int lang) {
    if (unit == 2) return 1;                          // Fahrenheit: English (US)'s frame
    if (unit == 1 && lang == (int)kEnglishUs) return 0;   // Celsius where the game shows Fahrenheit
    return -1;                                        // automatic, or Celsius already
}

#ifdef _WIN32
void f_91a290_orig(Ctx *__restrict c);
void tempsign_orig_stub(Ctx *__restrict) {}
#pragma comment(linker, "/alternatename:?f_91a290_orig@@YAXPEIAUCtx@@@Z=?tempsign_orig_stub@@YAXPEIAUCtx@@@Z")
#else
__attribute__((weak)) void f_91a290_orig(Ctx *__restrict c);
#endif
void hook_f_91a290(Ctx *__restrict c) {   // the layout-animation binder (this)
#ifndef _WIN32
    if (!f_91a290_orig) return;
#endif
    const u64 self = c->x[0], lr = c->x[30];
    f_91a290_orig(c);
    const u8 *page = (const u8 *)totk_data_page_current();
    if (!page || !totk_main_known(0x7298f0) || !totk_main_known(0x370c21f)) return;
    u32 unit;
    memcpy(&unit, page + kTempUnit, 4);
    if (unit != 1 && unit != 2) return;
    const u64 name = ld64(self + kLayoutName);
    if (!totk_ptr_ok(name) || strncmp((const char *)name, kTempLayout, sizeof kTempLayout) != 0) return;
    const u64 lang_obj = totk_global(0x4562460);
    const int lang = totk_ptr_ok(lang_obj) && *(const volatile u8 *)(uintptr_t)(lang_obj + 0x40) ? (int)ld32(lang_obj) : -1;
    const int frame = totk_temp_sign_frame(unit, lang);
    if (frame < 0) return;
    const u64 x0 = c->x[0], x1 = c->x[1], x2 = c->x[2];
    c->x[0] = self, c->x[1] = kRegionLanguageType, c->x[2] = 0;
    call_ind(c, kFindAnim);
    const u64 anim = c->x[0];
    if (totk_ptr_ok(anim)) {
        c->x[0] = anim, c->v[0].f[0] = (float)frame;
        call_ind(c, ld64(ld64(anim) + kSetFrame));
        c->x[0] = anim;
        call_ind(c, ld64(ld64(anim) + kApply));
        static int logs;
        if (logs < 3 && ++logs)
            printf("[temp-unit] map temperature sign: frame %d (unit %u, language %d)\n", frame, unit, lang);
    }
    c->x[0] = x0, c->x[1] = x1, c->x[2] = x2, c->x[30] = lr;
}
