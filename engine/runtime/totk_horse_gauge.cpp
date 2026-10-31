// totk_horse_gauge.cpp: the horse gauge's bar tween keeps the Switch's speed at any frame rate.
//
// UIHorseGaugeScreen's update (main+0x1aa6a94, vtable 0x4313c28 slot 86, x0 = the screen, once per frame) moves the
// bar animator [+0x1a8] toward the target frame +0x1d0 by exactly one animation frame per call (0x1aa6d6c..0x1aa6e64:
// current +0x20 +-1, clamped to the target, then the animator's setFrame): the bar's fill / drain tween runs 2x / 4.8x
// / 8x at 60 / 144 / 240 fps.
//
// The hook gives that step the frame's 30 Hz ratio r: when the call moved the frame by exactly +-1 without reaching
// the target, it stores the old frame +- r instead (the original's setFrame has marked the animator this call; the
// next call steps from the corrected frame, so the bar moves r animation frames per call). At 30 fps and below
// (r >= 1) the original exactly. gen.py --hook 1aa6a94 in GEN_MAIN; TOTK_HORSE_GAUGE=0: the original alone.
// tools/oracle/test_horse_gauge.py.
#include "rt.h"
#include "totk_globals.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
constexpr u64 kFrameDelta = 0x45577c0;   // GOT slot of the frame-delta setter's object (+8: 30 Hz ratio)
constexpr u64 kBar = 0x1a8, kTarget = 0x1d0, kFrame = 0x20;

void stf(u64 a, float v) { u32 w; memcpy(&w, &v, 4); st32(a, w); }
}  // namespace

#ifdef _WIN32
void f_1aa6a94_orig(Ctx *__restrict c);
void horsegauge_orig_stub(Ctx *__restrict) {}
#pragma comment(linker, "/alternatename:?f_1aa6a94_orig@@YAXPEIAUCtx@@@Z=?horsegauge_orig_stub@@YAXPEIAUCtx@@@Z")
#else
__attribute__((weak)) void f_1aa6a94_orig(Ctx *__restrict c);
#endif
void hook_f_1aa6a94(Ctx *__restrict c) {   // UIHorseGaugeScreen's update (this)
#ifndef _WIN32
    if (!f_1aa6a94_orig) return;
#endif
    static const bool on = [] {
        const char *e = getenv("TOTK_HORSE_GAUGE");
        const bool v = !e || strcmp(e, "0");
        printf("[horse-gauge] horse gauge bar tween %s\n", v ? "in game time" : "per frame, as the game (TOTK_HORSE_GAUGE=0)");
        return v;
    }();
    const u64 s = c->x[0], fd = totk_global(kFrameDelta);
    const u64 bar = on && totk_ptr_ok(s) ? ld64(s + kBar) : 0;
    const float r = bar && totk_ptr_ok(bar) && totk_ptr_ok(fd) ? ldf32(fd + 8) : 1.0f;
    if (!(r > 0.01f && r < 1.0f)) { f_1aa6a94_orig(c); return; }
    const float f0 = ldf32(bar + kFrame);
    f_1aa6a94_orig(c);
    if (ld64(s + kBar) != bar) return;
    const float f1 = ldf32(bar + kFrame), t = ldf32(s + kTarget);
    if (f1 == t) return;                                   // reached (or clamped onto) the target: as the game
    if (f1 == f0 + 1.0f) stf(bar + kFrame, f0 + r);
    else if (f1 == f0 - 1.0f) stf(bar + kFrame, f0 - r);
}
