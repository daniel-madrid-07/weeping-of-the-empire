// totk_walk_hold.cpp: Link's walk keeps the Switch's stop timing at any frame rate (Link avanza sin mover las
// piernas", also at 60 fps).
//
// SelectorPlayerSchedulerWalk's transition (main+0x21221f4) leaves Walk for its stop child when the left stick's
// length is <= 0.05 this frame AND last frame: its last branch calls Player::checkPreLeftStickLengthGreaterThan
// (main+0x16e4728, x0 = the Player component, s0 = 0.05: *(float *)*(Player + 0xa8) > s0) from main+0x21225a4, and
// only when that is false (and AS event byte +0x41 bit 4 is clear) it switches. PlayerPreLeftStickLength is last
// frame's length (ExecutePlayerCalcStick, main+0x1cd0260): a one-frame lag, the rest of the stop's latency is the
// pipeline's (pad read -> CalcStick -> scheduler -> AS slot), so the whole delay from the stick's release to the Wait
// pose is ~30 ms + 2.3 game frames (speed3 probe: 105 ms at 31 fps, 46 ms at 144). At 30 fps that is longer than the
// gap between taps of a fast W spam, so Walk never ends and the legs keep walking; at 60-240 fps Walk ends in every
// gap, Link brakes in the standing pose and the next tap's walk start (~90 ms, game time) never catches up: he slides
// with idle legs. The walk start and the braking slide are time-based already.
//
// The hook gives that one call site the Switch's delay in game time: the release's frames are summed in 30 Hz ratio r
// (the frame-delta object's +8, at most 1), once per game frame, and the check reports "still moving" until the sum
// reaches K = 2.3 - 0.3 r (to the nearest frame). With the stop's other 1.3 frames scaling with the frame rate, the
// release-to-stop delay is then 30 ms + (1.3 r + K - r) / 30 s = the 30 fps delay at every rate (test_walk_hold.py:
// within 5 % from 60 to 240 fps); at 30 fps (r = 1, K = 2) it is the original exactly (the second released frame
// stops). Every other caller of the shared check (GameActor::isStickMash, 0x1d1bf78, 0x1d1d16c, 0x1d2b4d0) gets the
// original. gen.py --hook 16e4728 in GEN_MAIN TOTK_WALK_HOLD=0: the original alone.
#include "rt.h"
#include "totk_globals.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" void *totk_data_page_current(void);   // games/totk/runtime/totk_patches.c: the data page, or NULL

namespace {
constexpr u64 kWalkStopCall = totk_main(0x21225a8);   // return address of the Walk scheduler's check
constexpr u64 kFrameDelta = 0x45577c0;                 // GOT slot of the frame-delta setter's object (+8: 30 Hz ratio)
constexpr u64 kPresents = 88;                          // data_page.h TOTK_DP_PRESENTS (u32, one per game present)

struct Hold { u64 player = 0; u32 frame = 0; float rel = 0; bool seen = false; };
Hold g_hold;   // (the scheduler runs on the game thread only)
}  // namespace

// Whether the stop is held this frame (tools/oracle/test_walk_hold.py drives it through the hook). player: the check's
// Player component; frame: the present count; r: the frame's 30 Hz ratio; original: what the check returned (true =
// still moving).
extern "C" int totk_walk_hold_step(u64 player, unsigned frame, float r, int original) {
    Hold &h = g_hold;
    if (!(r > 0.01f)) return 0;
    if (r > 1.0f) r = 1.0f;
    if (!h.seen || player != h.player || frame - h.frame > 1) h.rel = r;   // the first released frame (the stick
                                                                              // moved since the last call)
    else if (frame != h.frame) h.rel += r;           // the next frame of the same release (a second call: no change)
    h.player = player, h.frame = frame, h.seen = true;
    if (original) return 0;                          // last frame's length still above 0.05: the game holds it itself
    return h.rel < 2.3f - 0.3f * r - 0.5f * r;      // (K less half a frame: the stop lands on the nearest frame)
}

#ifdef _WIN32
void f_16e4728_orig(Ctx *__restrict c);
void walkhold_orig_stub(Ctx *__restrict) {}
#pragma comment(linker, "/alternatename:?f_16e4728_orig@@YAXPEIAUCtx@@@Z=?walkhold_orig_stub@@YAXPEIAUCtx@@@Z")
#else
__attribute__((weak)) void f_16e4728_orig(Ctx *__restrict c);
#endif
void hook_f_16e4728(Ctx *__restrict c) {   // Player::checkPreLeftStickLengthGreaterThan(this, s0)
#ifndef _WIN32
    if (!f_16e4728_orig) return;
#endif
    const u64 lr = c->x[30], player = c->x[0];
    f_16e4728_orig(c);
    if (lr != kWalkStopCall) return;
    static const bool on = [] {
        const char *e = getenv("TOTK_WALK_HOLD");
        const bool v = !e || strcmp(e, "0");
        printf("[walk-hold] Walk's stop %s\n", v ? "after the Switch's delay in game time" : "as the game (TOTK_WALK_HOLD=0)");
        return v;
    }();
    const u8 *page = (const u8 *)totk_data_page_current();
    const u64 fd = totk_global(kFrameDelta);
    if (!on || !page || !totk_ptr_ok(fd)) return;
    u32 frame;
    memcpy(&frame, page + kPresents, 4);
    if (totk_walk_hold_step(player, frame, ldf32(fd + 8), (int)(c->x[0] & 1)) && !(c->x[0] & 1)) {
        c->x[0] = 1;
        static int logs;
        if (logs < 3 && ++logs) printf("[walk-hold] stop held (frame %u)\n", frame);
    }
}
