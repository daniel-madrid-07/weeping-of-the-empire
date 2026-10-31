// totk_follow_average.cpp: NPCs walking with Link keep the Switch's 1-second smoothing at any frame
// rate.
//
// ExecuteCalculateAverageVec3f::updateImpl_ (main+0x1b7ba58, x0 = the node, once per frame; NpcFollowObj.utility.root:
// 23 actors that walk with Link, e.g. the Orchestra members and the Wanderers) pushes this frame's Vec3f (input
// +0x1a0) into a ring at +0x20 (data +0x20, capacity +0x28 = 30, head +0x2c, count +0x30; full: the oldest goes,
// 0x1b7bb50) and outputs the mean of the ring (+0x1a8). 30 SAMPLES PER FRAME: a 1 s window at 30 fps, 0.21 / 0.125 s
// at 144 / 240, so the follow target turns nervous.
//
// The hook samples the ring at 30 Hz of game time: it sums the frame's 30 Hz ratio r per node, and on a call where
// the sum does not reach the next whole frame it lets the original push and average (this frame's output still sees
// the current input), then takes the push back (head, count and the overwritten slot restored). At 30 fps and below
// (r >= 1) the original exactly. gen.py --hook 1b7ba58 in GEN_MAIN; TOTK_FOLLOW_AVERAGE=0: the original alone.
// tools/oracle/test_follow_average.py.
#include "rt.h"
#include "totk_globals.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
constexpr u64 kFrameDelta = 0x45577c0;   // GOT slot of the frame-delta setter's object (+8: 30 Hz ratio)
constexpr u64 kData = 0x20, kCap = 0x28, kHead = 0x2c, kCount = 0x30;

struct Node { u64 node; float acc; };
Node g_nodes[64];
unsigned g_next;
std::atomic_flag g_lock = ATOMIC_FLAG_INIT;
}  // namespace

#ifdef _WIN32
void f_1b7ba58_orig(Ctx *__restrict c);
void followavg_orig_stub(Ctx *__restrict) {}
#pragma comment(linker, "/alternatename:?f_1b7ba58_orig@@YAXPEIAUCtx@@@Z=?followavg_orig_stub@@YAXPEIAUCtx@@@Z")
#else
__attribute__((weak)) void f_1b7ba58_orig(Ctx *__restrict c);
#endif
void hook_f_1b7ba58(Ctx *__restrict c) {   // ExecuteCalculateAverageVec3f::updateImpl_(this)
#ifndef _WIN32
    if (!f_1b7ba58_orig) return;
#endif
    static const bool on = [] {
        const char *e = getenv("TOTK_FOLLOW_AVERAGE");
        const bool v = !e || strcmp(e, "0");
        printf("[follow-average] follower smoothing %s\n", v ? "sampled at 30 Hz of game time" : "sampled per frame, as the game (TOTK_FOLLOW_AVERAGE=0)");
        return v;
    }();
    const u64 n = c->x[0], fd = totk_global(kFrameDelta);
    const float r = on && totk_ptr_ok(n) && totk_ptr_ok(fd) ? ldf32(fd + 8) : 1.0f;
    const s32 cap = r > 0.01f && r < 1.0f ? (s32)ld32(n + kCap) : 0;
    const u64 data = cap > 0 ? ld64(n + kData) : 0;
    if (!cap || !totk_ptr_ok(data)) { f_1b7ba58_orig(c); return; }
    bool sample;
    while (g_lock.test_and_set(std::memory_order_acquire)) {}
    Node *p = nullptr;
    for (Node &q : g_nodes) if (q.node == n) p = &q;
    if (!p) p = &g_nodes[g_next++ % 64], *p = {n, 1.0f - r};   // (a new node samples on its first call)
    p->acc += r;
    if ((sample = p->acc >= 1.0f - 1e-4f)) p->acc -= 1.0f;
    g_lock.clear(std::memory_order_release);
    if (sample) { f_1b7ba58_orig(c); return; }
    const s32 head = (s32)ld32(n + kHead), count = (s32)ld32(n + kCount);
    if (head < 0 || head >= cap || count < 0 || count > cap) { f_1b7ba58_orig(c); return; }
    const s32 at = count < cap ? (head + count) % cap : head;   // the slot the push writes
    const u64 slot = data + (u64)at * 12;
    const u64 lo = ld64(slot);
    const u32 hi = ld32(slot + 8);
    f_1b7ba58_orig(c);
    st64(slot, lo), st32(slot + 8, hi);
    st32(n + kHead, (u32)head), st32(n + kCount, (u32)count);
}
