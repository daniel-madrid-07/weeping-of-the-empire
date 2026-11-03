// totk_event_waits.cpp: three event waits keep the Switch's timing in game time at any frame rate.
//
// Both are AI-node updates (once per game frame, x0 = the node) that count consecutive "ready" frames in the int +0x20 (old
// = +0x20, +0x20 = old + 1; a not-ready frame stores 0) and finish when old reaches a limit:
// ExecuteWaitCreateActor::updateImpl_ (main+0x1d97648; GameSystemActor's event root, EventTraverseAroundAndWait-
// CreateActor in 128 flows): nothing loading / composing / building for 61 frames (`cmp w8,#0x3c`): 2.0 s at 30 fps, 0.42 /
// 0.25 s at 144 / 240 (late pop-in at a cutscene's start); ExecuteEventWaitResourceStableForTerrainAndCave::updateImpl_
// (main+0x1c17fb0; DmF_RT_GrandFinale, DmF_SY_UnderLayer, Dm_OT_0010, Dm_SY_0039): terrain resources stable for 6 frames
// (`cmp w8,#4`): 200 ms at 30 fps, 25 ms at 240 (terrain LOD pop at the cut). The hooks (count_ready_in_game_time, v0.1.9;
// with EventContext::isLoadFinished below) leave the original its own count and finish, and only hold the count under the
// limit until the node has been ready for the Switch's number of 30 Hz frames of game time; a not-ready stretch restarts
// that clock once it lasts a whole 30 Hz frame. (v0.1.8 rewrote the count to game time instead, so at 240 fps a one-frame
// not-ready blip every few hundred frames restarted it for good: #47.) An outside write to +0x20 (the enter's reset)
// resyncs the twin. At 30 fps and below (r >= 1) the original exactly. gen.py --hook 1d97648,1c17fb0 in GEN_MAIN; off in
// v0.1.8.1 (#47), on again since v0.1.9 with this counting; TOTK_EVENT_WAITS=0: the original alone.
// tools/oracle/test_event_waits.py. The older count logic, totk_count_in_game_time, stays shared with totk_pouch_player.cpp
// and totk_envsound.cpp. Its twins sit in a 64-slot table, least recently used out; a twin lost mid-count makes its node's
// next call count a whole frame.
#include "rt.h"
#include "totk_globals.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
constexpr u64 kCount = 0x20;   // (both waits)

// left: what the hook stored after the last call; used: the helper's call stamp at the node's last call (0: free)
struct Twin { u64 node; float elapsed; u32 left; u64 used; float gap; };   // gap: count_ready_in_game_time's
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
        const bool v = !e || strcmp(e, "0");   // (on again since v0.1.9: #47 fixed by count_ready_in_game_time)
        printf("[event-waits] cutscene load / terrain / actor-equipment waits %s\n", v ? "counted in game time (the default)" : "counted in frames, as the game (TOTK_EVENT_WAITS=0)");
        return v;
    }();
    return on;
}

// TOTK_EVENT_WAITS_LOG=1 (diagnostic, v0.1.9 #47: with the hooks on, entering In-isa Shrine at 240 fps stalled): per
// hook (the original's address) and node, once a second while it is called, what each counted call did: the counter
// as found, as stored for the original, as the original left it, its return (w0), the twin's game-time frames and r,
// and how often the original reset the count meanwhile. The last line of a stalled wait names the hook and the step
// where its count stops.
bool log_on() {
    static const bool v = [] { const char *e = getenv("TOTK_EVENT_WAITS_LOG"); return e && strcmp(e, "0"); }();
    return v;
}
thread_local const char *t_hook;   // (set by this file's hooks for the log; pouch / envsound leave it null)
struct LogSlot { const void *orig; u64 node; u64 calls, resets; u32 most; u64 next_ms; };
LogSlot g_log[32];
std::atomic_flag g_log_lock = ATOMIC_FLAG_INIT;
u64 now_ms() {
    using namespace std::chrono;
    return (u64)duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}
void wait_log(const void *orig, u64 off, u64 node, u32 found, u32 stored, u32 after, u64 ret, float e, float r) {
    while (g_log_lock.test_and_set(std::memory_order_acquire)) {}
    LogSlot *s = nullptr, *old = &g_log[0];
    for (LogSlot &l : g_log) {
        if (l.orig == orig && l.node == node) { s = &l; break; }
        if (l.next_ms < old->next_ms) old = &l;
    }
    if (!s) { s = old; *s = {orig, node, 0, 0, 0, 0}; }
    s->calls++;
    if (after == 0 && stored != 0) s->resets++;
    if (after > s->most) s->most = after;
    const u64 t = now_ms();
    const bool print = t >= s->next_ms;
    LogSlot l = *s;
    if (print) s->next_ms = t + 1000, s->calls = 0, s->resets = 0, s->most = 0;
    g_log_lock.clear(std::memory_order_release);
    if (print)
        printf("[event-waits] log: %s (orig %p) +%#llx node %llx: %llu calls, %llu resets, most %u; last: found %u -> stored %u -> "
               "after %u, ret %llu, game-time frames %.2f, r %.3f\n", t_hook ? t_hook : "other", l.orig, (unsigned long long)off,
               (unsigned long long)node, (unsigned long long)l.calls, (unsigned long long)l.resets, l.most, found, stored,
               after, (unsigned long long)(ret & 0xff), e, r);
}
}  // namespace

// A per-node int counting consecutive frames that the original compares (old = count, count = old + 1; 0 = reset)
// counted in game time: see the header. off: the counter's offset in x0; on: the caller's switch.
extern "C" void totk_count_in_game_time(Ctx *__restrict c, void (*orig)(Ctx *__restrict), u64 off, int on) {
    const u64 n = c->x[0], fd = totk_frame_delta();
    const float r = on && totk_ptr_ok(n) && totk_ptr_ok(fd) ? totk_frame_ratio(fd) : 1.0f;
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
    const u32 stored = e >= 1.0f ? (u32)floorf(e - 1.0f + 1e-4f) : 0;
    st32(n + off, stored);
    orig(c);
    const u32 after = ld32(n + off);
    if (log_on()) wait_log((const void *)orig, off, n, now, stored, after, c->x[0], e, r);
    while (g_lock.test_and_set(std::memory_order_acquire)) {}
    for (Twin &u : g_twins)                      // (the slot may have gone to another node meanwhile: then the next
        if (u.node == n && u.used) u.elapsed = after == 0 ? 0.0f : e, u.left = after;   // call resyncs from the counter)
    g_lock.clear(std::memory_order_release);
}

// The three event waits that need N CONSECUTIVE ready calls (old = count, count = old + 1, finished when old >= limit; a
// not-ready call stores 0): ExecuteWaitCreateActor (+0x20, limit 60), the terrain-stable wait (+0x20, limit 5) and
// EventContext::isLoadFinished (+0x568, limit 60). v0.1.8 counted them through totk_count_in_game_time, which rewrites
// the count to the game-time frames: at 240 fps that is limit x 8 consecutive ready calls, and one not-ready call
// restarts it. A reason that blips not ready for a single frame every K frames (K < 488 at 240) then restarted the
// wait for good, while the original, needing 61 calls, still finished between blips (#47: entering In-isa Shrine at
// 240 stalled 4 / 4). Here the original keeps its own count and its own finish; the hook only holds the count below
// the limit until the node has been ready for limit + 1 frames of GAME time, and a not-ready stretch restarts that
// clock only once it lasts a whole 30 Hz frame (what the Switch's 30 fps could see; a shorter blip it would mostly
// miss). So the wait ends when the original would AND after the Switch's ready time: never before the Switch's,
// never later than the original's own window after it, and at 30 fps and below (r >= 1) the original exactly.
static void count_ready_in_game_time(Ctx *__restrict c, void (*orig)(Ctx *__restrict), u64 off, u32 limit, int on) {
    const u64 n = c->x[0], fd = totk_frame_delta();
    const float r = on && totk_ptr_ok(n) && totk_ptr_ok(fd) ? totk_frame_ratio(fd) : 1.0f;
    if (!(r > 0.01f && r < 1.0f)) { orig(c); return; }
    while (g_lock.test_and_set(std::memory_order_acquire)) {}
    bool fresh;
    Twin &t = twin(n, fresh);
    const u32 now = ld32(n + off);
    if (fresh) t.elapsed = now ? (float)now : 0.0f, t.gap = 0.0f;   // (lost mid-count: the count is the frames it saw)
    else if (now != t.left) t.elapsed = 0.0f, t.gap = 0.0f;      // reset or written outside the hook (the enter)
    const bool held = t.elapsed + r < (float)limit + 1.0f - 1e-4f;   // the Switch's ready time not reached by this call
    g_lock.clear(std::memory_order_release);
    const u32 stored = held && now > limit - 1 ? limit - 1 : now;
    if (stored != now) st32(n + off, stored);
    orig(c);
    const u32 after = ld32(n + off);
    if (log_on()) wait_log((const void *)orig, off, n, now, stored, after, c->x[0], t.elapsed, r);
    while (g_lock.test_and_set(std::memory_order_acquire)) {}
    for (Twin &u : g_twins) {
        if (u.node != n || !u.used) continue;
        if (after != 0) u.elapsed += r, u.gap = 0.0f;                // ready: one more frame's game time
        else if ((u.gap += r) >= 1.0f - 1e-4f) u.elapsed = 0.0f;   // not ready for a whole 30 Hz frame: restart
        u.left = after;
    }
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
    t_hook = "WaitCreateActor 1d97648";
    count_ready_in_game_time(c, f_1d97648_orig, kCount, 60, enabled());
    t_hook = nullptr;   // (a later pouch / envsound count on this thread logs as 'other')
}
void hook_f_1c17fb0(Ctx *__restrict c) {   // ExecuteEventWaitResourceStableForTerrainAndCave::updateImpl_(this)
#ifndef _WIN32
    if (!f_1c17fb0_orig) return;
#endif
    t_hook = "terrain-stable 1c17fb0";
    count_ready_in_game_time(c, f_1c17fb0_orig, kCount, 5, enabled());
    t_hook = nullptr;   // (a later pouch / envsound count on this thread logs as 'other')
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
    t_hook = "isLoadFinished 13bd680";
    count_ready_in_game_time(c, f_13bd680_orig, 0x568, 60, enabled());
    t_hook = nullptr;   // (a later pouch / envsound count on this thread logs as 'other')
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
// 10bc078; on by default again since v0.1.9; TOTK_EVENT_WAITS=0: the original alone.
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
    const u64 n = c->x[0], fd = totk_frame_delta();
    const float r = enabled() && totk_ptr_ok(n) && totk_ptr_ok(fd) ? totk_frame_ratio(fd) : 1.0f;
    if (!(r > 0.01f && r < 1.0f)) { f_10bc078_orig(c); return; }
    while (g_lock.test_and_set(std::memory_order_acquire)) {}
    bool fresh;
    Twin &t = twin(n, fresh);
    const u32 now = ld32(n + kEquipCount);
    if (fresh) t.elapsed = now >= kEquipLimit ? (float)now + 1.0f - r : (float)now;   // new or lost mid-count: from
                                                 // the counter; at the fallback a lost twin counts a whole frame, so
                                                 // a node evicted on every call still falls back (never pinned for good)
    else if (now != t.left) t.elapsed = (float)now;   // reset or written outside the hook
    const float e = t.elapsed;
    g_lock.clear(std::memory_order_release);
    // the Switch's fallback call is the 601st: it ends at 601 frames; this one ends at e + r
    const u32 in = now >= kEquipLimit && e + r < (float)kEquipLimit + 1.0f - 1e-4f ? kEquipLimit - 1 : now;
    if (in != now) st32(n + kEquipCount, in);
    f_10bc078_orig(c);
    const u32 after = ld32(n + kEquipCount);
    t_hook = "equipment 10bc078";
    if (log_on()) wait_log((const void *)&f_10bc078_orig, kEquipCount, n, now, in, after, c->x[0], e, r);
    t_hook = nullptr;
    while (g_lock.test_and_set(std::memory_order_acquire)) {}
    for (Twin &u : g_twins)
        if (u.node == n && u.used) u.elapsed = after == 0 ? 0.0f : after != in ? e + r : e, u.left = after;
    g_lock.clear(std::memory_order_release);
}
