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
        const u64 fd = totk_frame_delta();
        const float r = on && totk_ptr_ok(fd) ? totk_frame_ratio(fd) : 1.0f;
        if (r > 0.01f && r < 1.0f) c->v[0] = vS(c->v[0].f[0] * r);
    }
    f_120ec94_orig(c);
}

// 1.4.3 inlines the helper into a rewritten getNextVelocity (main+0x1d4ac34; x0 = the extension, w1 = playing, s0 =
// the mass m; the k ramp moved to its caller), so there is no helper call to hook there. Its two steps are still
// (500 - 400 k) / 30 and (500 - 400 k) / (30 m) per call with k = the float at x0 + 0x2c, read only for them. The hook
// on getNextVelocity (pinned to 1.4.3's, pins_v143) shows the call k' = 1.25 - r (1.25 - k), so 500 - 400 k' =
// r (500 - 400 k): both steps scaled by r exactly as the helper hook scales them on 1.0.0, then k is put back. On
// 1.0.0 (and any build without 1.4.3's layout) it is the original alone: the helper hook above does the work there.
extern "C" float totk_recall_k_scaled(float k, float r) { return 1.25f - r * (1.25f - k); }

#ifdef _WIN32
void f_171fafc_orig(Ctx *__restrict c);
void recallnext_orig_stub(Ctx *__restrict) {}
#pragma comment(linker, "/alternatename:?f_171fafc_orig@@YAXPEIAUCtx@@@Z=?recallnext_orig_stub@@YAXPEIAUCtx@@@Z")
#else
__attribute__((weak)) void f_171fafc_orig(Ctx *__restrict c);
#endif
void hook_f_171fafc(Ctx *__restrict c) {   // ReverseRecorderExtension::getNextVelocity(this, ...)
#ifndef _WIN32
    if (!f_171fafc_orig) return;
#endif
#if defined(TOTK_GAME_VERSION) && TOTK_GAME_VERSION == 143
    static const bool on = [] {
        const char *e = getenv("TOTK_RECALL_ACCEL");
        return !e || strcmp(e, "0");
    }();
    const u64 ext = c->x[0], fd = totk_frame_delta();
    const float r = on && totk_ptr_ok(ext) && totk_ptr_ok(fd) && (c->x[1] & 0xff) ? totk_frame_ratio(fd) : 1.0f;
    const float k = r > 0.01f && r < 1.0f ? ldf32(ext + 0x2c) : 0.0f;
    if (!(r > 0.01f && r < 1.0f) || !(k >= 0.0f && k <= 1.0f)) { f_171fafc_orig(c); return; }
    const float k2 = totk_recall_k_scaled(k, r);
    u32 w;
    memcpy(&w, &k2, 4);
    st32(ext + 0x2c, w);
    f_171fafc_orig(c);
    memcpy(&w, &k, 4);
    st32(ext + 0x2c, w);
#else
    f_171fafc_orig(c);
#endif
}
