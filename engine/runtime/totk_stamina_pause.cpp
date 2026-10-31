// totk_stamina_pause.cpp: the extra-stamina gauge's "up" pause during an event keeps the Switch's length at any frame
// rate.
//
// The gauge's event path FUN main+0x1a4f0ac (x0 = the gauge; from 0x1a4f080 and, while EventMgr::existsActiveEvent,
// 0x1a4f3f8: e.g. after a Stamina Vessel at a Goddess Statue) plays mc_ExtraGanbari_Up: state +0x78 2 starts the
// animation (state 3, counter +0x80 += 1) and state 3 counts +0x80 += 1.0 per call until 15, then state 4 and the
// ease. 15 FRAMES: 0.5 s at 30 fps, 0.10 / 0.06 s at 144 / 240. The non-event twin (0x1a4f57c) already adds r.
//
// The hook makes that +1 the frame's 30 Hz ratio r: in states 2 / 3, when the call added exactly 1.0 to +0x80, it
// stores the old value + r instead. At 30 fps and below (r >= 1) the original exactly. gen.py --hook 1a4f0ac in
// GEN_MAIN; TOTK_STAMINA_PAUSE=0: the original alone. tools/oracle/test_stamina_pause.py.
#include "rt.h"
#include "totk_globals.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
constexpr u64 kFrameDelta = 0x45577c0;   // GOT slot of the frame-delta setter's object (+8: 30 Hz ratio)
constexpr u64 kState = 0x78, kCounter = 0x80;

void stf(u64 a, float v) { u32 w; memcpy(&w, &v, 4); st32(a, w); }
}  // namespace

#ifdef _WIN32
void f_1a4f0ac_orig(Ctx *__restrict c);
void stamina_orig_stub(Ctx *__restrict) {}
#pragma comment(linker, "/alternatename:?f_1a4f0ac_orig@@YAXPEIAUCtx@@@Z=?stamina_orig_stub@@YAXPEIAUCtx@@@Z")
#else
__attribute__((weak)) void f_1a4f0ac_orig(Ctx *__restrict c);
#endif
void hook_f_1a4f0ac(Ctx *__restrict c) {   // the extra-stamina gauge's event update (this)
#ifndef _WIN32
    if (!f_1a4f0ac_orig) return;
#endif
    static const bool on = [] {
        const char *e = getenv("TOTK_STAMINA_PAUSE");
        const bool v = !e || strcmp(e, "0");
        printf("[stamina-pause] extra-stamina gauge pause %s\n", v ? "in game time" : "in frames, as the game (TOTK_STAMINA_PAUSE=0)");
        return v;
    }();
    const u64 g = c->x[0], fd = totk_global(kFrameDelta);
    const float r = on && totk_ptr_ok(g) && totk_ptr_ok(fd) ? ldf32(fd + 8) : 1.0f;
    if (!(r > 0.01f && r < 1.0f)) { f_1a4f0ac_orig(c); return; }
    const u32 st = ld32(g + kState);
    const float v0 = ldf32(g + kCounter);
    f_1a4f0ac_orig(c);
    if ((st == 2 || st == 3) && ldf32(g + kCounter) == v0 + 1.0f) stf(g + kCounter, v0 + r);
}
