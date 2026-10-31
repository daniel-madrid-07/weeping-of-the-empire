// totk_get_info_delay.cpp: the extra info line under the get-item window keeps the Switch's timing at any frame
// rate.
//
// UIGetWindowScreen's per-frame update (main+0x1aa3e44, vtable 0x1004312f20 slot 86, x0 = the screen) runs a delay
// +0x264 before each step of its SimpleInfo line (LayoutMsg/SimpleInfo_00, animators "SimpleInfoIn" / "SimpleInfoOut"):
// the open (main+0x1aa5b90) sets {state +0x260 = 1, delay 10.0}; SimpleInfoIn at its end frame sets {state 2, delay
// 90.0} (main+0x1aa3f4c); then SimpleInfoOut (state 3). While the delay is > 0 the update only does delay -= 1.0 and
// returns (main+0x1aa3e9c..0x1aa3ec0): 10 + 90 FRAMES, so the line showed 3 s at 30 fps but 1.5 / 0.63 / 0.38 s at 60 /
// 144 / 240. The window itself waits for a button; only the line is timed.
//
// The hook makes that decrement the frame's 30 Hz ratio r: with the delay d > 0 it stores d - r + 1 before the call,
// and the original's -1 leaves d - r (the same > 0 test, the same return). At 30 fps and below (r >= 1) the original
// exactly. gen.py --hook 1aa3e44 in GEN_MAIN; TOTK_GET_INFO_DELAY=0: the original alone.
// tools/oracle/test_get_info_delay.py.
#include "rt.h"
#include "totk_globals.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
constexpr u64 kFrameDelta = 0x45577c0;   // GOT slot of the frame-delta setter's object (+8: 30 Hz ratio)
constexpr u64 kDelay = 0x264;

void stf(u64 a, float v) { u32 w; memcpy(&w, &v, 4); st32(a, w); }
}  // namespace

#ifdef _WIN32
void f_1aa3e44_orig(Ctx *__restrict c);
void getinfo_orig_stub(Ctx *__restrict) {}
#pragma comment(linker, "/alternatename:?f_1aa3e44_orig@@YAXPEIAUCtx@@@Z=?getinfo_orig_stub@@YAXPEIAUCtx@@@Z")
#else
__attribute__((weak)) void f_1aa3e44_orig(Ctx *__restrict c);
#endif
void hook_f_1aa3e44(Ctx *__restrict c) {   // UIGetWindowScreen's update (this)
#ifndef _WIN32
    if (!f_1aa3e44_orig) return;
#endif
    static const bool on = [] {
        const char *e = getenv("TOTK_GET_INFO_DELAY");
        const bool v = !e || strcmp(e, "0");
        printf("[get-info] get-item info line %s\n", v ? "timed in game time" : "timed in frames, as the game (TOTK_GET_INFO_DELAY=0)");
        return v;
    }();
    const u64 scr = c->x[0], fd = totk_global(kFrameDelta);
    if (on && totk_ptr_ok(scr) && totk_ptr_ok(fd)) {
        const float r = ldf32(fd + 8), d = ldf32(scr + kDelay);
        if (r > 0.01f && r < 1.0f && d > 0.0f) stf(scr + kDelay, d - r + 1.0f);
    }
    f_1aa3e44_orig(c);
}
