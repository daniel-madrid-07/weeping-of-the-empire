// totk_likelike_spots.cpp: tar Like Likes keep their hiding spots for the Switch's time at any frame
// rate.
//
// ExecuteLikeLikeTarMetaAI::updateImpl_ (main+0x1c73fbc, x0 = the group's meta AI, once per frame; only
// AI/Enemy_LikeLike_Tar.utility.root.ainb) keeps 20 hiding-spot slots (stride 0x18 at +0xcb0: owner id, +4 counter,
// +8 active byte). For an active slot no member holds (state 2), counter += 1 per call, and on the call where it was
// already 100 the slot is released (0x1c7406c..0x1c74098): 3.3 s at 30 fps, 0.69 / 0.42 s at 144 / 240, so tar Like
// Likes reshuffle where they lurk up to 8x sooner.
//
// The hook keeps a float per slot counting unheld 30 Hz frames of game time. Before the call each active slot's
// counter shows floor(frames + r) - 1 (the frames including this call, less the one the original adds), so the
// original's `old >= 100` releases on the Switch's frame; after it, a counter one higher adds r, an unchanged one is
// put back, anything else (a release, a reset by the group AI) is taken as the new count. At 30 fps and below
// (r >= 1) the original exactly. gen.py --hook 1c73fbc in GEN_MAIN; TOTK_LIKELIKE_SPOTS=0: the original alone.
// tools/oracle/test_likelike_spots.py.
#include "rt.h"
#include "totk_globals.h"

#include <atomic>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
constexpr u64 kFrameDelta = 0x45577c0;   // GOT slot of the frame-delta setter's object (+8: 30 Hz ratio)
constexpr u64 kSlots = 0xcb0, kStride = 0x18, kCounter = 4, kActive = 8;
constexpr int kNum = 20;

struct Group { u64 ai; float frames[kNum]; };
Group g_groups[4];
unsigned g_next;
std::atomic_flag g_lock = ATOMIC_FLAG_INIT;
}  // namespace

#ifdef _WIN32
void f_1c73fbc_orig(Ctx *__restrict c);
void likelike_orig_stub(Ctx *__restrict) {}
#pragma comment(linker, "/alternatename:?f_1c73fbc_orig@@YAXPEIAUCtx@@@Z=?likelike_orig_stub@@YAXPEIAUCtx@@@Z")
#else
__attribute__((weak)) void f_1c73fbc_orig(Ctx *__restrict c);
#endif
void hook_f_1c73fbc(Ctx *__restrict c) {   // ExecuteLikeLikeTarMetaAI::updateImpl_(this)
#ifndef _WIN32
    if (!f_1c73fbc_orig) return;
#endif
    static const bool on = [] {
        const char *e = getenv("TOTK_LIKELIKE_SPOTS");
        const bool v = !e || strcmp(e, "0");
        printf("[likelike-spots] tar Like Like hiding spots %s\n", v ? "held in game time" : "held in frames, as the game (TOTK_LIKELIKE_SPOTS=0)");
        return v;
    }();
    const u64 ai = c->x[0], fd = totk_global(kFrameDelta);
    const float r = on && totk_ptr_ok(ai) && totk_ptr_ok(fd) ? ldf32(fd + 8) : 1.0f;
    if (!(r > 0.01f && r < 1.0f)) { f_1c73fbc_orig(c); return; }
    float frames[kNum];
    s32 shown[kNum];
    while (g_lock.test_and_set(std::memory_order_acquire)) {}
    Group *g = nullptr;
    for (Group &q : g_groups) if (q.ai == ai) g = &q;
    if (!g) {
        g = &g_groups[g_next++ % 4], g->ai = ai;
        for (int i = 0; i < kNum; i++) g->frames[i] = (float)(s32)ld32(ai + kSlots + i * kStride + kCounter);
    }
    memcpy(frames, g->frames, sizeof frames);
    g_lock.clear(std::memory_order_release);
    for (int i = 0; i < kNum; i++) {
        const u64 s = ai + kSlots + i * kStride;
        const s32 v = (s32)ld32(s + kCounter);
        if (v != (s32)floorf(frames[i] + 1e-4f)) frames[i] = (float)v;   // written outside the hook
        shown[i] = INT32_MIN;
        if (!ld8(s + kActive)) continue;
        const float e = frames[i] + r;   // unheld frames including this call, if it is unheld
        shown[i] = e >= 1.0f ? (s32)floorf(e - 1.0f + 1e-4f) : 0;
        st32(s + kCounter, (u32)shown[i]);
    }
    f_1c73fbc_orig(c);
    for (int i = 0; i < kNum; i++) {
        const u64 s = ai + kSlots + i * kStride;
        const s32 v = (s32)ld32(s + kCounter);
        if (shown[i] == INT32_MIN) frames[i] = (float)v;
        else if (v == shown[i] + 1) frames[i] += r;              // counted this call
        else if (v != shown[i]) frames[i] = (float)v;            // released / reset
        if (shown[i] != INT32_MIN && (v == shown[i] + 1 || v == shown[i]))
            st32(s + kCounter, (u32)(s32)floorf(frames[i] + 1e-4f));   // (held: put back; counted: the frames)
    }
    while (g_lock.test_and_set(std::memory_order_acquire)) {}
    for (Group &q : g_groups) if (q.ai == ai) memcpy(q.frames, frames, sizeof frames);
    g_lock.clear(std::memory_order_release);
}
