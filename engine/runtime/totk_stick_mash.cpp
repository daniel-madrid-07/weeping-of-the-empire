// totk_stick_mash.cpp: spinning the stick to struggle (frozen, grabbed by a Like Like) counts as on the Switch at any
// frame rate.
//
// GameActor::isStickMash (main+0x16e4610, x0 = the actor, w0 = a mash this frame) reads the Player component P =
// *(*(actor + 0x220) + 0x3a8) and returns true on: a trigger bit (*(u16 *)*(P + 0x38) & 0x3275, one per press);
// else false with the stick in the dead zone (PlayerLeftStickLength *(P + 0x98) <= 0.05); else true when the stick
// just left it (PlayerPreLeftStickLength *(P + 0xa8) <= 0.05, via checkPreLeftStickLengthGreaterThan); else true when
// |wrap(PreRawLeftStickRadian *(P + 0x68) - RawLeftStickRadian *(P + 0x78))| > pi/16. That last test compares two
// consecutive FRAMES: the slowest spin that mashes is 5.9 rad/s at 30 fps, 28 at 144, 47 at 240, so from ~120 fps a
// spinning stick does nothing (ExecutePlayerReactionFrozen pauses Link's struggle, 0x1d115e4; ReactionEaten,
// 0x1d10f58, gets no struggle flag). Its callers run once per game frame.
//
// The hook gives the angle test the Switch's sampling: the stick's angle is compared across one 30 Hz frame of game
// time (the frames' 30 Hz ratios r summed, the frame-delta object's +8), so the threshold and the event rate (at most
// one angle mash per 30 Hz tick) are the Switch's at every rate. Between ticks the angle test is false. The trigger
// bits and the leave-the-dead-zone edge are the original's (events, frame-rate independent). At 30 fps and below
// (r >= 1) the original exactly. One tracker (Link: the only Player). gen.py --hook 16e4610 in GEN_MAIN;
// TOTK_STICK_MASH=0: the original alone. tools/oracle/test_stick_mash.py.
#include "rt.h"
#include "totk_globals.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" void *totk_data_page_current(void);   // games/totk/runtime/totk_patches.c: the data page, or NULL

namespace {
constexpr u64 kFrameDelta = 0x45577c0;   // GOT slot of the frame-delta setter's object (+8: 30 Hz ratio)
constexpr u64 kPresents = 88;            // data_page.h TOTK_DP_PRESENTS (u32, one per game present)
// the original's constants, bit-exact: 0.05 (main+0x3776494), pi (0x3775b84), 2 pi (0x3775f60), pi/16 (0x37761d4)
constexpr u32 kDeadBits = 0x3d4ccccd, kPiBits = 0x40490fdb, kTwoPiBits = 0x40c90fdb, kMashBits = 0x3e490fdb;

float bits(u32 b) { float f; memcpy(&f, &b, 4); return f; }
bool addr_ok(u64 p) { return p >= 0x10000 && p < (1ull << 47); }   // a blackboard value (4-byte aligned)

struct Tick { u64 player = 0; u32 frame = 0; float angle = 0, t = 0; bool live = false, result = false; };
Tick g_tick;   // (the reaction nodes run on the game thread only)
}  // namespace

// One evaluation of the angle test with the stick out on this frame and the last (tools/oracle/test_stick_mash.py
// drives it). player: the Player component; frame: the present count; pre / cur: last / this frame's stick angle;
// r: this frame's 30 Hz ratio (< 1). Returns whether the angle test mashes this frame.
extern "C" int totk_stick_mash_tick(u64 player, unsigned frame, float pre, float cur, float r) {
    Tick &k = g_tick;
    if (k.live && k.player == player && k.frame == frame) return k.result;   // a second caller in the same frame
    if (!k.live || k.player != player || frame - k.frame > 1) k.angle = pre, k.t = 0;   // a new spin: from last frame
    k.player = player, k.frame = frame, k.live = true, k.result = false;
    k.t += r > 1.0f ? 1.0f : r;
    if (k.t >= 0.999f) {   // a 30 Hz tick of game time since the angle was taken: the Switch's comparison
        float d = k.angle - cur;
        const float ad = fabsf(d);
        if (ad > bits(kPiBits)) d = d > 0 ? -(bits(kTwoPiBits) - ad) : bits(kTwoPiBits) - ad;
        k.result = fabsf(d) > bits(kMashBits);
        k.angle = cur, k.t -= 1.0f;
        if (k.t > 0.5f) k.t = 0;
    }
    return k.result;
}

// The stick left the angle test's domain (dead zone this frame or last): the next spin starts afresh.
extern "C" void totk_stick_mash_reset(void) { g_tick.live = false; }

#ifdef _WIN32
void f_16e4610_orig(Ctx *__restrict c);
void stickmash_orig_stub(Ctx *__restrict) {}
#pragma comment(linker, "/alternatename:?f_16e4610_orig@@YAXPEIAUCtx@@@Z=?stickmash_orig_stub@@YAXPEIAUCtx@@@Z")
#else
__attribute__((weak)) void f_16e4610_orig(Ctx *__restrict c);
#endif
void hook_f_16e4610(Ctx *__restrict c) {   // GameActor::isStickMash(this)
#ifndef _WIN32
    if (!f_16e4610_orig) return;
#endif
    const u64 actor = c->x[0];
    f_16e4610_orig(c);
    static const bool on = [] {
        const char *e = getenv("TOTK_STICK_MASH");
        const bool v = !e || strcmp(e, "0");
        printf("[stick-mash] stick spin struggle %s\n", v ? "sampled at 30 Hz of game time" : "per frame, as the game (TOTK_STICK_MASH=0)");
        return v;
    }();
    const u8 *page = (const u8 *)totk_data_page_current();
    const u64 fd = totk_global(kFrameDelta);
    if (!on || !page || !totk_ptr_ok(fd) || !totk_ptr_ok(actor)) return;
    const float r = ldf32(fd + 8);
    if (!(r > 0.01f) || !(r < 1.0f)) return;   // 30 fps and below: the original
    const u64 comp = ld64(actor + 0x220), p = totk_ptr_ok(comp) ? ld64(comp + 0x3a8) : 0;
    if (!totk_ptr_ok(p)) return;
    const u64 key_p = ld64(p + 0x38), len_p = ld64(p + 0x98), pre_len_p = ld64(p + 0xa8), pre_p = ld64(p + 0x68),
              cur_p = ld64(p + 0x78);
    if (!addr_ok(key_p) || !addr_ok(len_p) || !addr_ok(pre_len_p) || !addr_ok(pre_p) || !addr_ok(cur_p)) return;
    const float dead = bits(kDeadBits);
    if (!(ldf32(len_p) > dead) || !(ldf32(pre_len_p) > dead)) {   // the dead zone or its edge: the original's answer
        totk_stick_mash_reset();
        return;
    }
    u32 frame;
    memcpy(&frame, page + kPresents, 4);
    const bool angle = totk_stick_mash_tick(p, frame, ldf32(pre_p), ldf32(cur_p), r);
    const bool trigger = (ld16(key_p) & 0x3275) != 0;
    c->x[0] = trigger || angle;
    static int logs;
    if (angle && logs < 3 && ++logs) printf("[stick-mash] spin mash at frame %u (ratio %.3f)\n", frame, r);
}
