// totk_recall_accel.cpp: Recall's playback keeps the Switch's acceleration limit at any frame rate.
//
// ReverseRecorderExtension::getNextVelocity (main+0x171fafc, once per frame from SpecialPowerReceiver::
// preCalcReverseRecorder) moves the object's linear and angular velocity toward the recorded path's with the helper
// main+0x120ec94 (x0 = the vector, moved toward x1 by at most s0), with the step (500 - 400 k) / 30 (angular: (500/m +
// (100/m - 500/m) k) / 30): a 30 Hz frame baked in, no dt. So the limit on the velocity change is 2x / 4.8x / 8x the
// Switch's at 60 / 144 / 240 fps: a Recall's start and its sharp reversals snap. The recording and the playback
// interpolation are time-based.
//
// The hook scales s0 by the frame's 30 Hz ratio r at those two call sites only (lr main+0x171fbe8 / 0x171fc18; the
// helper's other callers keep the original): the same acceleration per second as at 30 fps. At 30 fps and below
// (r >= 1) the original exactly. gen.py --hook 120ec94 in GEN_MAIN; TOTK_RECALL_ACCEL=0: the original alone.
// tools/oracle/test_recall_accel.py.
#include "rt.h"
#include "totk_globals.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
constexpr u64 kLinearCall = totk_main(0x171fbe8), kAngularCall = totk_main(0x171fc18);   // return addresses
constexpr u64 kFrameDelta = 0x45577c0;   // GOT slot of the frame-delta setter's object (+8: 30 Hz ratio)
}  // namespace

#ifdef _WIN32
void f_120ec94_orig(Ctx *__restrict c);
void recall_orig_stub(Ctx *__restrict) {}
#pragma comment(linker, "/alternatename:?f_120ec94_orig@@YAXPEIAUCtx@@@Z=?recall_orig_stub@@YAXPEIAUCtx@@@Z")
#else
__attribute__((weak)) void f_120ec94_orig(Ctx *__restrict c);
#endif
void hook_f_120ec94(Ctx *__restrict c) {   // move x0's vector toward x1's by at most s0; w0 = reached
#ifndef _WIN32
    if (!f_120ec94_orig) return;
#endif
    if (c->x[30] == kLinearCall || c->x[30] == kAngularCall) {
        static const bool on = [] {
            const char *e = getenv("TOTK_RECALL_ACCEL");
            const bool v = !e || strcmp(e, "0");
            printf("[recall-accel] Recall velocity step %s\n", v ? "per 30 Hz of game time" : "per frame, as the game (TOTK_RECALL_ACCEL=0)");
            return v;
        }();
        const u64 fd = totk_global(kFrameDelta);
        const float r = on && totk_ptr_ok(fd) ? ldf32(fd + 8) : 1.0f;
        if (r > 0.01f && r < 1.0f) c->v[0] = vS(c->v[0].f[0] * r);
    }
    f_120ec94_orig(c);
}
