// totk_event_waits.cpp: two event waits keep the Switch's debounce in game time at any frame rate.
//
// Both are AI-node updates (once per game frame, x0 = the node) that count consecutive "ready" frames in the int +0x20
// (old = +0x20, +0x20 = old + 1; a not-ready frame stores 0) and finish when old reaches a limit:
//   ExecuteWaitCreateActor::updateImpl_ (main+0x1d97648; GameSystemActor's event root, EventTraverseAroundAndWait-
//     CreateActor in 128 flows): nothing loading / composing / building for 61 frames (`cmp w8,#0x3c`): 2.0 s at
//     30 fps, 0.42 / 0.25 s at 144 / 240 (late pop-in at a cutscene's start);
//   ExecuteEventWaitResourceStableForTerrainAndCave::updateImpl_ (main+0x1c17fb0; DmF_RT_GrandFinale,
//     DmF_SY_UnderLayer, Dm_OT_0010, Dm_SY_0039): terrain resources stable for 6 frames (`cmp w8,#4`): 200 ms at
//     30 fps, 25 ms at 240 (terrain LOD pop at the cut).
// The hooks keep a float twin of the count per node in game time: before the call +0x20 = floor(elapsed - 1) with
// elapsed = the ready 30 Hz frames including this one (so the original's limit test sees what the Switch's would),
// after it 0 resets the twin and anything else adds the frame's 30 Hz ratio r. An outside write to +0x20 (the
// enter's reset) resyncs the twin. At 30 fps and below (r >= 1) the original exactly. gen.py --hook 1d97648,1c17fb0
// in GEN_MAIN; TOTK_EVENT_WAITS=0: the original alone. tools/oracle/test_event_waits.py. The count logic is
// totk_count_in_game_time, shared with totk_pouch_player.cpp.
#include "rt.h"
#include "totk_globals.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
constexpr u64 kFrameDelta = 0x45577c0;   // GOT slot of the frame-delta setter's object (+8: 30 Hz ratio)
constexpr u64 kCount = 0x20;   // (both waits)

struct Twin { u64 node; float elapsed; u32 left; };   // left: what the hook stored after the last call
Twin g_twins[16];
unsigned g_next;
std::atomic_flag g_lock = ATOMIC_FLAG_INIT;

Twin &twin(u64 node) {
    for (Twin &t : g_twins) if (t.node == node) return t;
    Twin &t = g_twins[g_next++ % 16];
    t = {node, 0.0f, ~0u};
    return t;
}

bool enabled() {
    static const bool on = [] {
        const char *e = getenv("TOTK_EVENT_WAITS");
        const bool v = !e || strcmp(e, "0");
        printf("[event-waits] cutscene load / terrain waits %s\n", v ? "counted in game time" : "counted in frames, as the game (TOTK_EVENT_WAITS=0)");
        return v;
    }();
    return on;
}
}  // namespace

// A per-node int counting consecutive frames that the original compares (old = count, count = old + 1; 0 = reset)
// counted in game time: see the header. off: the counter's offset in x0; on: the caller's switch.
extern "C" void totk_count_in_game_time(Ctx *__restrict c, void (*orig)(Ctx *__restrict), u64 off, int on) {
    const u64 n = c->x[0], fd = totk_global(kFrameDelta);
    const float r = on && totk_ptr_ok(n) && totk_ptr_ok(fd) ? ldf32(fd + 8) : 1.0f;
    if (!(r > 0.01f && r < 1.0f)) { orig(c); return; }
    while (g_lock.test_and_set(std::memory_order_acquire)) {}
    Twin &t = twin(n);
    const u32 now = ld32(n + off);
    if (now != t.left) t.elapsed = (float)now;    // reset or written outside the hook
    const float e = t.elapsed + r;               // ready frames including this one, if it is ready
    g_lock.clear(std::memory_order_release);     // (not held across the original: it calls into the actor / terrain
                                                 // managers, which take their own locks)
    st32(n + off, e >= 1.0f ? (u32)floorf(e - 1.0f + 1e-4f) : 0);
    orig(c);
    const u32 after = ld32(n + off);
    while (g_lock.test_and_set(std::memory_order_acquire)) {}
    for (Twin &u : g_twins)                      // (the slot may have gone to another node meanwhile: then the next
        if (u.node == n) u.elapsed = after == 0 ? 0.0f : e, u.left = after;   // call resyncs from the counter)
    g_lock.clear(std::memory_order_release);
}

#ifdef _WIN32
void f_1d97648_orig(Ctx *__restrict c);
void f_1c17fb0_orig(Ctx *__restrict c);
void waitcreate_orig_stub(Ctx *__restrict) {}
void waitterrain_orig_stub(Ctx *__restrict) {}
#pragma comment(linker, "/alternatename:?f_1d97648_orig@@YAXPEIAUCtx@@@Z=?waitcreate_orig_stub@@YAXPEIAUCtx@@@Z")
#pragma comment(linker, "/alternatename:?f_1c17fb0_orig@@YAXPEIAUCtx@@@Z=?waitterrain_orig_stub@@YAXPEIAUCtx@@@Z")
#else
__attribute__((weak)) void f_1d97648_orig(Ctx *__restrict c);
__attribute__((weak)) void f_1c17fb0_orig(Ctx *__restrict c);
#endif
void hook_f_1d97648(Ctx *__restrict c) {   // ExecuteWaitCreateActor::updateImpl_(this, ctx)
#ifndef _WIN32
    if (!f_1d97648_orig) return;
#endif
    totk_count_in_game_time(c, f_1d97648_orig, kCount, enabled());
}
void hook_f_1c17fb0(Ctx *__restrict c) {   // ExecuteEventWaitResourceStableForTerrainAndCave::updateImpl_(this)
#ifndef _WIN32
    if (!f_1c17fb0_orig) return;
#endif
    totk_count_in_game_time(c, f_1c17fb0_orig, kCount, enabled());
}
