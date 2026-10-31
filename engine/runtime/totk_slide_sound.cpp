// totk_slide_sound.cpp: Link's tumble-slide sound keeps the Switch's wobble and stop grace at any frame
// rate.
//
// ExecutePlayerSlippingDown (the ragdoll tumble / slide down a slope after a big hit, a frozen or paralysed fall, a
// crash off a shield or a wakeboard) holds an XLink sound in its per-frame tail main+0x1d23614 (x0 = the node, tail
// call of its updateImpl_ 0x1d232a0, its only caller). Two FLOAT FRAME COUNTERS drive it:
//   +0x58 phase: +1 per call while the ragdoll touches the ground; volume = 1 + 0.5 * min((v - 3) / 7, 1) *
//         cos(2 pi * phase / period), and when fmodf(phase, period) == 0 the period +0x60 (s32, 7 at the start) is
//         re-picked at random in 5..8;
//   +0x5c grace: +1 per call without contact; at >= 5 the sound stops (+0x64 = 0, phase 0); a contact call zeroes it.
// At 120 / 240 fps the wobble is a 15-48 Hz buzz instead of 4-6 Hz, and any airborne gap over 42 / 21 ms (instead of
// 167) stops and restarts the sound: a bumpy slope stutters.
//
// The hook makes both count the frame's 30 Hz ratio r. Before the call: grace g -> g + r - 1 (the original's +1 then
// gives g + r; a contact call stores 0 anyway); when this call is expected to slip (sound idle, or the last call
// slipped: g == 0), phase a -> t - 1 with t = a + r, where t stops on the period's next multiple k*p if it crosses it
// (so the original's fmodf == 0 re-picks the period exactly where the Switch does) and the remainder is added after.
// After the call, +0x5c tells which branch ran: 0 = slipped (phase = t + the remainder; a slip the hook did not expect
// gets a + r), else grace (phase and period restored, unless the grace ran out and the original stopped the sound).
// The only inexactness: at a contact <-> no-contact change, one call's volume phase is off by under one 30 Hz frame.
// At 30 fps and below (r >= 1) the original exactly. gen.py --hook 1d23614 in GEN_MAIN; TOTK_SLIDE_SOUND=0: the
// original alone. tools/oracle/test_slide_sound.py.
#include "rt.h"
#include "totk_globals.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
constexpr u64 kFrameDelta = 0x45577c0;   // GOT slot of the frame-delta setter's object (+8: 30 Hz ratio)
constexpr u64 kPhase = 0x58, kGrace = 0x5c, kPeriod = 0x60, kActive = 0x64;
constexpr float kGraceEnd = 5.0f;        // the original's stop (main+0x1d236e4)

void stf(u64 a, float v) { u32 w; memcpy(&w, &v, 4); st32(a, w); }
}  // namespace

#ifdef _WIN32
void f_1d23614_orig(Ctx *__restrict c);
void slidesound_orig_stub(Ctx *__restrict) {}
#pragma comment(linker, "/alternatename:?f_1d23614_orig@@YAXPEIAUCtx@@@Z=?slidesound_orig_stub@@YAXPEIAUCtx@@@Z")
#else
__attribute__((weak)) void f_1d23614_orig(Ctx *__restrict c);
#endif
void hook_f_1d23614(Ctx *__restrict c) {   // ExecutePlayerSlippingDown's sound tail (this)
#ifndef _WIN32
    if (!f_1d23614_orig) return;
#endif
    static const bool on = [] {
        const char *e = getenv("TOTK_SLIDE_SOUND");
        const bool v = !e || strcmp(e, "0");
        printf("[slide-sound] tumble-slide sound %s\n", v ? "timed in game time" : "timed in frames, as the game (TOTK_SLIDE_SOUND=0)");
        return v;
    }();
    const u64 n = c->x[0], fd = totk_global(kFrameDelta);
    const float r = on && totk_ptr_ok(n) && totk_ptr_ok(fd) ? ldf32(fd + 8) : 1.0f;
    if (!(r > 0.01f && r < 1.0f)) { f_1d23614_orig(c); return; }
    const float a = ldf32(n + kPhase), g = ldf32(n + kGrace);
    const s32 p = (s32)ld32(n + kPeriod);
    const bool slip = !ld8(n + kActive) || g == 0.0f;   // expected branch
    float carry = 0.0f;
    stf(n + kGrace, g + r - 1.0f);
    if (slip) {
        float t = a + r;
        if (ld8(n + kActive) && p > 0) {
            const float k = floorf(t / (float)p);
            if (k > floorf(a / (float)p)) carry = t - k * (float)p, t = k * (float)p;
        }
        stf(n + kPhase, t - 1.0f);
    }
    f_1d23614_orig(c);
    const float g2 = ldf32(n + kGrace);
    if (g2 == 0.0f) stf(n + kPhase, slip ? ldf32(n + kPhase) + carry : a + r);   // slipped
    else if (slip && g2 < kGraceEnd) stf(n + kPhase, a), st32(n + kPeriod, (u32)p);   // grace: undo the phase guess
}
