// totk_pouch_player.cpp: the inventory screen's Link model keeps the Switch's tab debounce at any frame
// rate.
//
// SelectorPouchPlayer's update (main+0x212b740, vtable 0x44db0e8 slot 34, x0 = the selector; the 3D Link on the
// pause-menu inventory screen, AI/PouchPlayer.root.ainb) counts the calls the inventory tab (PouchMgr::getActiveTab)
// differs from the committed one in the int +0x30 (same tab: 0) and commits on the 10th (`cmp w8,#9; b.lt`, 0x212b958;
// then he draws / sheathes for that category and the equipment is re-synced). 10 FRAMES: 333 ms at 30 fps, 69 / 42 ms
// at 144 / 240, so flicking through the tabs re-poses him on every tab passed instead of the ones you rest on.
//
// The hook counts those frames in game time with totk_event_waits.cpp's totk_count_in_game_time (a float per
// selector; before the call +0x30 = floor(frames + r) - 1, after it 0 resets and anything else adds r). The 5-frame
// equipment wait +0x40 in the same function is left as is. At 30 fps and below (r >= 1) the original exactly. gen.py
// --hook 212b740 in GEN_MAIN; TOTK_POUCH_PLAYER=0: the original alone. tools/oracle/test_pouch_player.py.
#include "rt.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" void totk_count_in_game_time(Ctx *__restrict c, void (*orig)(Ctx *__restrict), u64 off, int on);

#ifdef _WIN32
void f_212b740_orig(Ctx *__restrict c);
void pouchplayer_orig_stub(Ctx *__restrict) {}
#pragma comment(linker, "/alternatename:?f_212b740_orig@@YAXPEIAUCtx@@@Z=?pouchplayer_orig_stub@@YAXPEIAUCtx@@@Z")
#else
__attribute__((weak)) void f_212b740_orig(Ctx *__restrict c);
#endif
void hook_f_212b740(Ctx *__restrict c) {   // SelectorPouchPlayer's update (this, ...)
#ifndef _WIN32
    if (!f_212b740_orig) return;
#endif
    static const bool on = [] {
        const char *e = getenv("TOTK_POUCH_PLAYER");
        const bool v = !e || strcmp(e, "0");
        printf("[pouch-player] inventory Link's tab debounce %s\n", v ? "in game time" : "in frames, as the game (TOTK_POUCH_PLAYER=0)");
        return v;
    }();
    totk_count_in_game_time(c, f_212b740_orig, 0x30, on);
}
