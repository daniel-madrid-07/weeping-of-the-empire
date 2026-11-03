// totk_placement_timers.cpp: the actor placement manager's two frame-counted timers keep the Switch's period at any
// frame rate.
//
// engine::actor::PlacementMgr::calc (main+0xa6fd14, x0 = the manager; called once per frame from ActorModule::calc
// main+0xa6dbc0) counts two float timers in FRAMES:
// - request cooldown +0x124: while the state +0x120 is 3, every call does t -= 1 when t > 0 (0xa6ff04..0xa6ff18);
//   with none of the flags 0x1250 in +0x11c and the placement status (0xa71ac8) at 3, a call that STARTED with t <= 0
//   raises flag 0x20 of +0x11c (a placement request, sent as event 'drpu' further down) and sets t = 15
//   (0xa6ff44..0xa6ff60): one request per 16 frames, 0.53 s at 30 fps;
// - refresh period +0xf0 (and its 6-frame hold-off +0xf8): unless flag 0x100000 of +0x11c is set or the slot table
//   +0xc8 is null (0xa6fd5c / 0xa6fd64: both skip to 0xa6ffc4), every call does +0xf8 -= 1 when it is > 0
//   (0xa700ec, and a change seen meanwhile is dropped), clamps +0xf0 to 90 while a global flag is set, then +0xf0 -= 1
//   (0xa6ffa8..0xa6ffb4); below 0, or on a change the slot scan saw, it rescans, sets +0xf0 = +0xf4 (the period),
//   +0xf8 = 6 and raises flag 0x800 (event 'trav').
// At 240 fps both ran 8x as often: 8x the placement requests and rescans, CPU time the frame rate pays for.
//
// The hook makes each decrement the frame's 30 Hz ratio r (as totk_get_info_delay.cpp): on the path where the original
// decrements a timer t > 0 (the same conditions, read before the call), it stores t - r + 1 first, and the original's
// -1 leaves t - r; the original's own tests (t <= 0, t < 0, the 90 clamp) then see game time. A timer the original has
// just restarted (15; +0xf4 and the hold-off 6 at a rescan) is set to n + 1 - r after the call, so the next firing
// comes at 30 fps' game time (16 / 30 s; (+0xf4 + 1) / 30 s, and a change rescans again after 7 / 30 s), to within one
// call. At 30 fps and below (r >= 1) the original
// exactly. gen.py --hook a6fd14 in GEN_MAIN; TOTK_PLACEMENT_TIMERS=0: the
// original alone. tools/oracle/test_placement_timers.py.
#include "rt.h"
#include "totk_globals.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
constexpr u64 kFlags = 0x11c, kState = 0x120, kCooldown = 0x124;       // request cooldown (state 3)
constexpr u64 kSlots = 0xc8, kPeriod = 0xf0, kHoldOff = 0xf8;          // refresh period and its hold-off
constexpr u32 kSkipTimers = 0x100000;                                   // +0x11c bit 20: both period timers skipped

void stf(u64 a, float v) { u32 w; memcpy(&w, &v, 4); st32(a, w); }
// t > 0 is decremented by the original: store t - r + 1 so that it leaves t - r
void scale(u64 a, float r) {
    const float t = ldf32(a);
    if (t > 0.0f) stf(a, t - r + 1.0f);
}
}  // namespace

#ifdef _WIN32
void f_a6fd14_orig(Ctx *__restrict c);
void placement_orig_stub(Ctx *__restrict) {}
#pragma comment(linker, "/alternatename:?f_a6fd14_orig@@YAXPEIAUCtx@@@Z=?placement_orig_stub@@YAXPEIAUCtx@@@Z")
#else
__attribute__((weak)) void f_a6fd14_orig(Ctx *__restrict c);
#endif
void hook_f_a6fd14(Ctx *__restrict c) {   // engine::actor::PlacementMgr::calc (this)
#ifndef _WIN32
    if (!f_a6fd14_orig) return;
#endif
    static const bool on = [] {
        const char *e = getenv("TOTK_PLACEMENT_TIMERS");
        const bool v = !e || strcmp(e, "0");
        printf("[placement] placement request / refresh timers %s\n",
               v ? "in game time" : "in frames, as the game (TOTK_PLACEMENT_TIMERS=0)");
        return v;
    }();
    const u64 m = c->x[0], fd = totk_frame_delta();
    float r = 1.0f;
    if (on && totk_ptr_ok(m) && totk_ptr_ok(fd)) r = totk_frame_ratio(fd);
    if (!(r > 0.01f && r < 1.0f)) {
        f_a6fd14_orig(c);
        return;
    }
    const bool cool = ld32(m + kState) == 3, timers = !(ld32(m + kFlags) & kSkipTimers) && ld64(m + kSlots);
    const bool due = cool && !(ldf32(m + kCooldown) > 0.0f);   // the original may fire the request on this call
    if (cool) scale(m + kCooldown, r);
    if (timers) {
        scale(m + kHoldOff, r);
        // (+0xf0 is decremented whatever its sign; only a positive one is moved here, so a timer already below 0
        // fires on this call as in the original)
        scale(m + kPeriod, r);
    }
    const float p = ldf32(m + kPeriod);   // what the original's decrement starts from
    f_a6fd14_orig(c);
    // A timer the original has just restarted: its frames count from the NEXT call, so at 30 fps the request repeats
    // every 16 calls (15 decrements and the firing call) and the refresh every +0xf4 + 1. Restarted at n + 1 - r, the
    // hook's r-steps reach the same game time (16 / 30 s, (+0xf4 + 1) / 30 s); at r = 1 that is n, the original.
    // (A request fired iff the cooldown was due and now reads 15; a rescan iff the period is not its decrement, p - 1
    // or the clamp's 89, but +0xf4 with the hold-off at 6. A rescan whose +0xf4 equals p - 1 keeps the original's
    // restart: one call early, once.)
    if (due && ldf32(m + kCooldown) == 15.0f) stf(m + kCooldown, 16.0f - r);
    const float pa = ldf32(m + kPeriod), f4 = ldf32(m + 0xf4);
    if (timers && ldf32(m + kHoldOff) == 6.0f && pa == f4 && pa != p - 1.0f && pa != 89.0f)
        stf(m + kPeriod, f4 + 1.0f - r), stf(m + kHoldOff, 7.0f - r);
}
