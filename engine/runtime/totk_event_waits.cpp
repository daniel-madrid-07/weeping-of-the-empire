// totk_event_waits.cpp: three event waits keep the Switch's timing in game time at any frame rate.
//
// Both are AI-node updates (once per game frame, x0 = the node) that count consecutive "ready" frames in the int +0x20
// (old = +0x20, +0x20 = old + 1; a not-ready frame stores 0) and finish when old reaches a limit:
// ExecuteWaitCreateActor::updateImpl_ (main+0x1d97648; GameSystemActor's event root, EventTraverseAroundAndWait-
// CreateActor in 128 flows): nothing loading / composing / building for 61 frames (`cmp w8,#0x3c`): 2.0 s at 30 fps,
// 0.42 / 0.25 s at 144 / 240 (late pop-in at a cutscene's start);
// ExecuteEventWaitResourceStableForTerrainAndCave::updateImpl_ (main+0x1c17fb0; DmF_RT_GrandFinale, DmF_SY_UnderLayer,
// Dm_OT_0010, Dm_SY_0039): terrain resources stable for 6 frames (`cmp w8,#4`): 200 ms at 30 fps, 25 ms at 240
// (terrain LOD pop at the cut). The hooks keep a float twin of the count per node in game time: before the call +0x20
// = floor(elapsed - 1) with elapsed = the ready 30 Hz frames including this one (so the original's limit test sees
// what the Switch's would), after it 0 resets the twin and anything else adds the frame's 30 Hz ratio r. An outside
// write to +0x20 (the enter's reset) resyncs the twin. At 30 fps and below (r >= 1) the original exactly. gen.py
// --hook 1d97648,1c17fb0 in GEN_MAIN; off by default since v0.1.8.1 (#47: the game's original waits;
// TOTK_EVENT_WAITS=1 turns them on). tools/oracle/test_event_waits.py. The count logic is totk_count_in_game_time,
// shared with totk_pouch_player.cpp and totk_envsound.cpp. Its twins sit in a 64-slot table, least recently used out;
// a twin lost mid-count makes its node's next call count a whole frame.
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

// left: what the hook stored after the last call; used: the helper's call stamp at the node's last call (0: free)
struct Twin { u64 node; float elapsed; u32 left; u64 used; };
constexpr int kTwins = 64;   // event waits, the inventory Link, EnvSound's 2 one-shot emitters: a handful at once
Twin g_twins[kTwins];
u64 g_stamp;
std::atomic_flag g_lock = ATOMIC_FLAG_INIT;

// The node's twin; a new one (fresh) takes the least recently used slot. Evicting a node seen in the last
// 4 x kTwins calls (still counting, as far as the helper can tell) is logged: more live nodes than slots.
Twin &twin(u64 node, bool &fresh) {
    Twin *lru = &g_twins[0];
    ++g_stamp;
    for (Twin &t : g_twins) {
        if (t.node == node && t.used) { t.used = g_stamp; fresh = false; return t; }
        if (t.used < lru->used) lru = &t;
    }
    if (lru->used && g_stamp - lru->used < 4 * kTwins) {
        static int logged;
        if (logged++ < 20)
            printf("[event-waits] game-time count: twin of node %llx evicted %llu calls after its last use (more than %d "
                   "counting at once); its next call counts a whole frame\n", (unsigned long long)lru->node,
                   (unsigned long long)(g_stamp - lru->used), kTwins);
    }
    *lru = {node, 0.0f, ~0u, g_stamp};
    fresh = true;
    return *lru;
}

bool enabled() {
    static const bool on = [] {
        const char *e = getenv("TOTK_EVENT_WAITS");
        const bool v = e && strcmp(e, "0");   // (#47, v0.1.8.1: off unless TOTK_EVENT_WAITS=1)
        printf("[event-waits] cutscene load / terrain / actor-equipment waits %s\n", v ? "counted in game time (TOTK_EVENT_WAITS=1)" : "counted in frames, as the game (the default; TOTK_EVENT_WAITS=1: in game time)");
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
    bool fresh;
    Twin &t = twin(n, fresh);
    const u32 now = ld32(n + off);
    if (fresh && now != 0) t.elapsed = (float)now + 1.0f - r;   // a twin lost mid-count (evicted): its fraction is
                                                 // unknown, so this call counts a whole frame, as the original: a node
                                                 // evicted on every call still advances (at the frame rate), never stalls
    else if (now != t.left) t.elapsed = (float)now;   // reset or written outside the hook
    const float e = t.elapsed + r;               // ready frames including this one, if it is ready
    g_lock.clear(std::memory_order_release);     // (not held across the original: it calls into the actor / terrain
                                                 // managers, which take their own locks)
    st32(n + off, e >= 1.0f ? (u32)floorf(e - 1.0f + 1e-4f) : 0);
    orig(c);
    const u32 after = ld32(n + off);
    while (g_lock.test_and_set(std::memory_order_acquire)) {}
    for (Twin &u : g_twins)                      // (the slot may have gone to another node meanwhile: then the next
        if (u.node == n && u.used) u.elapsed = after == 0 ? 0.0f : e, u.left = after;   // call resyncs from the counter)
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

// game::event::EventContext::isLoadFinished (main+0x13bd680, EventContext vtable 0x421eeb0 slot 18; x0 = the context,
// once a frame while the event loads): every not-ready reason ([isCreatingBaseProc 待ち], [isExistLoadingOrWait-
// ComposeModel 待ち], building, traverse) stores 0 in the int +0x568; once all are ready it counts "surroundings
// stable" calls ([周囲の安定確認中]: old = +0x568, +0x568 = old + 1, finished when old > 59, 0x13bd7cc `cmp w8,#0x3b;
// b.le`). 61 FRAMES: 2.0 s at 30 fps, 0.25 s at 240, so the cutscene starts while the surroundings still settle (the
// same pop-in as ExecuteWaitCreateActor's). Counted in game time with the shared twin. (+0x34 > 5400, the context's
// own time fallback, is untouched.)
#ifdef _WIN32
void f_13bd680_orig(Ctx *__restrict c);
void loadfinished_orig_stub(Ctx *__restrict) {}
#pragma comment(linker, "/alternatename:?f_13bd680_orig@@YAXPEIAUCtx@@@Z=?loadfinished_orig_stub@@YAXPEIAUCtx@@@Z")
#else
__attribute__((weak)) void f_13bd680_orig(Ctx *__restrict c);
#endif
void hook_f_13bd680(Ctx *__restrict c) {   // game::event::EventContext::isLoadFinished(this)
#ifndef _WIN32
    if (!f_13bd680_orig) return;
#endif
    totk_count_in_game_time(c, f_13bd680_orig, 0x568, enabled());
}

// game::event::Actor::initialize (main+0x10bc078; x0 = the event actor, called once a game frame until it returns
// true) waits for the actor's equipment (EquipmentUser: isCreationFinishedAll) but gives up after 600 calls: the int
// +0x408 (0 from EventMgrCallback::createActor; +1 at every not-yet call; 0 also runs the first-call setup) is
// compared `599 < +0x408` (0x10bc1b0: `cmp w8,#0x258; b.lt`) and the cutscene then starts without the equipment. 600
// FRAMES: 20 s at 30 fps, 2.5 s at 240, so a slow equipment load at a high frame rate starts the cutscene with
// weapons / shields popping in. The hook keeps a twin of the count in game time (elapsed = 30 Hz frames counted, r
// per counted call) and, while this call would end before 601 frames (the Switch's fallback call ends there), pins
// +0x408 at 599 before the call once it reached 600, so the fallback waits for 600 frames of game time; the counter's
// 0 and every value under 600 are the original's (the first-call setup is untouched). A twin lost mid-count starts at
// the counter (never later than the original). At 30 fps and below (r >= 1) the original exactly. gen.py --hook
// 10bc078; off by default (the original alone), TOTK_EVENT_WAITS=1 turns it on.
namespace {
constexpr u64 kEquipCount = 0x408;
constexpr u32 kEquipLimit = 600;
}  // namespace

#ifdef _WIN32
void f_10bc078_orig(Ctx *__restrict c);
void eventactor_orig_stub(Ctx *__restrict) {}
#pragma comment(linker, "/alternatename:?f_10bc078_orig@@YAXPEIAUCtx@@@Z=?eventactor_orig_stub@@YAXPEIAUCtx@@@Z")
#else
__attribute__((weak)) void f_10bc078_orig(Ctx *__restrict c);
#endif
void hook_f_10bc078(Ctx *__restrict c) {   // game::event::Actor::initialize(this)
#ifndef _WIN32
    if (!f_10bc078_orig) return;
#endif
    const u64 n = c->x[0], fd = totk_global(kFrameDelta);
    const float r = enabled() && totk_ptr_ok(n) && totk_ptr_ok(fd) ? ldf32(fd + 8) : 1.0f;
    if (!(r > 0.01f && r < 1.0f)) { f_10bc078_orig(c); return; }
    while (g_lock.test_and_set(std::memory_order_acquire)) {}
    bool fresh;
    Twin &t = twin(n, fresh);
    const u32 now = ld32(n + kEquipCount);
    if (fresh || now != t.left) t.elapsed = (float)now;   // new, reset, or lost mid-count: from the counter
    const float e = t.elapsed;
    g_lock.clear(std::memory_order_release);
    // the Switch's fallback call is the 601st: it ends at 601 frames; this one ends at e + r
    const u32 in = now >= kEquipLimit && e + r < (float)kEquipLimit + 1.0f - 1e-4f ? kEquipLimit - 1 : now;
    if (in != now) st32(n + kEquipCount, in);
    f_10bc078_orig(c);
    const u32 after = ld32(n + kEquipCount);
    while (g_lock.test_and_set(std::memory_order_acquire)) {}
    for (Twin &u : g_twins)
        if (u.node == n && u.used) u.elapsed = after == 0 ? 0.0f : after != in ? e + r : e, u.left = after;
    g_lock.clear(std::memory_order_release);
}
