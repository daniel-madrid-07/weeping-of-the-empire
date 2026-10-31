// totk_cook_settle.cpp: a cooking pot counts as set down after the Switch's settle time at any frame
// rate.
//
// ExecuteActorCookSetCheckOn::updateImpl_ (main+0x1b3ebac, x0 = the node, once per frame; only SpObj_CookSetOnFire_A_01,
// the portable pot) counts the int +0x20 down while the pot rests (weight check, not held): c = c > 0 ? c - 1 : 0 and
// "On" when it reaches 0 (0x1b3edfc); any failed check sets it back to 10 (0x1b3ee24). 10 FRAMES: 333 ms at 30 fps,
// 69 / 42 ms at 144 / 240, so the pot can be used while it still wobbles.
//
// The hook keeps the remaining 30 Hz frames per node as a float: a value the hook did not leave (the 10 of a failed
// check, the enter) restarts it; each call takes the frame's 30 Hz ratio r and stores 2 (not settled after this call)
// or 1 (settles on this call) for the original to count down, 0 once settled. At 30 fps and below (r >= 1) the
// original exactly. gen.py --hook 1b3ebac in GEN_MAIN; TOTK_COOK_SETTLE=0: the original alone.
// tools/oracle/test_cook_settle.py.
#include "rt.h"
#include "totk_globals.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
constexpr u64 kFrameDelta = 0x45577c0;   // GOT slot of the frame-delta setter's object (+8: 30 Hz ratio)
constexpr u64 kCount = 0x20;

struct Pot { u64 node; float left; u32 stored; };   // stored: what the original left after the hook's last call
Pot g_pots[16];
unsigned g_next;
std::atomic_flag g_lock = ATOMIC_FLAG_INIT;
}  // namespace

#ifdef _WIN32
void f_1b3ebac_orig(Ctx *__restrict c);
void cook_orig_stub(Ctx *__restrict) {}
#pragma comment(linker, "/alternatename:?f_1b3ebac_orig@@YAXPEIAUCtx@@@Z=?cook_orig_stub@@YAXPEIAUCtx@@@Z")
#else
__attribute__((weak)) void f_1b3ebac_orig(Ctx *__restrict c);
#endif
void hook_f_1b3ebac(Ctx *__restrict c) {   // ExecuteActorCookSetCheckOn::updateImpl_(this, ctx)
#ifndef _WIN32
    if (!f_1b3ebac_orig) return;
#endif
    static const bool on = [] {
        const char *e = getenv("TOTK_COOK_SETTLE");
        const bool v = !e || strcmp(e, "0");
        printf("[cook-settle] cooking pot settle %s\n", v ? "in game time" : "in frames, as the game (TOTK_COOK_SETTLE=0)");
        return v;
    }();
    const u64 n = c->x[0], fd = totk_global(kFrameDelta);
    const float r = on && totk_ptr_ok(n) && totk_ptr_ok(fd) ? ldf32(fd + 8) : 1.0f;
    if (!(r > 0.01f && r < 1.0f)) { f_1b3ebac_orig(c); return; }
    while (g_lock.test_and_set(std::memory_order_acquire)) {}
    Pot *p = nullptr;
    for (Pot &q : g_pots) if (q.node == n) p = &q;
    if (!p) p = &g_pots[g_next++ % 16], *p = {n, 0.0f, ~0u};
    const u32 v = ld32(n + kCount);
    if (v != p->stored) p->left = (float)v;   // a restart (a failed check's 10, the enter) or an outside write
    const float had = p->left, left = had - r;
    g_lock.clear(std::memory_order_release);   // (not held across the original: it calls into physics)
    st32(n + kCount, had <= 1e-4f ? 0 : left > 1e-4f ? 2 : 1);
    f_1b3ebac_orig(c);
    const u32 after = ld32(n + kCount);
    while (g_lock.test_and_set(std::memory_order_acquire)) {}
    for (Pot &q : g_pots)
        if (q.node == n) q.left = after == 10 ? 10.0f : left > 0.0f ? left : 0.0f, q.stored = after;
    g_lock.clear(std::memory_order_release);
}
