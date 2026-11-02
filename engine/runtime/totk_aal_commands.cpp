// totk_aal_commands.cpp: the game thread leaves aal's AudioRenderer thread its command lists above 200
// fps.
//
// aal hands commands between threads in CommandLists: 16 of them (CommandManager::initialize main+0xf3bc58), with a
// free queue (CommandManager +0x40190 cap, +0x40198 slots, +0x401a0 push, +0x401a4 pop). Every producer works the same
// way: if its current list holds a command, it takes a fresh list from the free queue and pushes the current one into
// its target queue. With no list free it keeps its current list and pushes nothing. The producers are:
//   - the game thread, once a game frame: main+0x9c2ef0 (from AudioRendererController::calc), list +0x40250 -> queue A
//     (+0x401b8 cap, +0x401c8 push, +0x401cc pop);
//   - the AudioRenderer thread (main+0xa07ef4), once per 5 ms audio frame, for its own lists: the stream file requests
//     +0x40258 -> queue C (the StreamThread's reads), the deferred commands +0x40260 -> B, and D.
// The AudioRenderer thread returns one list from A per audio frame, 200 a second. Above 200 game frames a second the game
// thread takes every list that comes back before the audio thread's own swaps get one. The free queue sits at 0, and the
// file-request list is never pushed. The stream reads stop, the requests overflow and are dropped
// (StreamWithPrefetcherReader::read main+0xa1bedc), the opening's narration starves, its cue never fires, and the event
// holds the new-game fade: the black screen at 240 fps (8 long boots of 8; 180 and below fine).
//
// The hook keeps the game's commands in its current list for this frame (it returns without doing anything) while the
// game flushes faster than the audio thread drains (each of the last kFastCalls intervals between calls under 5 ms, the
// audio frame) AND queue A already holds kBacklog lists or more, or the free queue is down to kReserve or fewer. The
// audio thread's swaps then always find a list, and the game's commands wait a frame or two, as when no list is free. A
// hold is never longer than kMaxRun frames in a row (then the original runs once), except when A is full, where the
// original would lose the list. At 200 fps and below the rate test fails, so the original runs every frame whatever the
// queues hold. As the original's own returns do, a held call sets the current list's state word +0x4010 to 0 (main
// never loads that word). TOTK_AAL_CMD_HOLD=0: the original alone, with the lists it drops on a full A counted. Runs of
// held frames go to the log ("[aal]"). gen.py --hook 9c2ef0 in GEN_MAIN. tools/oracle/test_aal_commands.py.
#include "rt.h"
#include "totk_globals.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
constexpr u64 kRingCap = 0x401b8, kRingPush = 0x401c8, kRingPop = 0x401cc;   // queue A, in CommandManager
constexpr u64 kFreePush = 0x401a0, kFreePop = 0x401a4;                         // the free queue
constexpr u64 kCurList = 0x40250;                                               // the game's list being filled
constexpr s32 kBacklog = 2, kReserve = 4;   // hold while A holds >= 2 lists or <= 4 are free
constexpr u32 kMaxRun = 8;                  // then let one flush through (unless A is full)
constexpr int kFastCalls = 4;               // the game flushes faster than one list per audio frame:
constexpr u64 kFastNs = 5000000;            //   its last 4 call intervals all under 5 ms
std::atomic<u64> g_held{0}, g_dropped{0};
u32 g_run;                                  // held calls in a row (only the game thread calls 9c2ef0)
u64 g_last_call_ns, g_gaps[kFastCalls];     // the last call and the last kFastCalls intervals
int g_gap_at;

bool hold_on() {
    static const bool on = [] {
        const char *e = getenv("TOTK_AAL_CMD_HOLD");
        return !e || strcmp(e, "0") != 0;
    }();
    return on;
}

// "[aal]" lines: held frames and, with the hold off, the lists the original drops on a full A are counted in runs:
// the first one at once, then one line at the end of a run at most every 2 s, with the runs, frames and longest run
// since the last line; a run that does not end gets a "still" line every 5 s.
u64 now_ns() {
    return (u64)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
u64 still_after_ns() {   // TOTK_AAL_RING_STILL_MS: the interval of the "still" line (default 5 s; tests use less)
    static const u64 v = [] {
        const char *e = getenv("TOTK_AAL_RING_STILL_MS");
        const long long ms = e ? atoll(e) : 0;
        return (u64)(ms > 0 ? ms : 5000) * 1000000ull;
    }();
    return v;
}
struct RunLog {
    const char *what;
    u32 run = 0, runs = 0, frames = 0, longest = 0;
    u64 last_ns = 0, run_ns = 0, still_ns = 0;
    bool first = true;
    void hit(s32 used, s32 freen) {
        run++, frames++;
        const u64 now = now_ns();
        if (run == 1) run_ns = still_ns = now;
        if (first) {
            first = false;
            printf("[aal] audio command lists short (queue A %d, free %d): %s\n", used, freen, what);
        } else if (now - still_ns >= still_after_ns()) {
            still_ns = now;
            printf("[aal] still after %.1f s: %u frames %s in this run\n", (now - run_ns) / 1e9, run, what);
        }
    }
    void miss(u64 total) {
        if (!run) return;
        runs++;
        if (run > longest) longest = run;
        run = 0;
        const u64 now = now_ns();
        if (now - last_ns < 2000000000ull && last_ns) return;
        printf("[aal] %u frames %s in %u runs (longest %u) since the last line; %llu in all\n", frames, what, runs, longest,
               (unsigned long long)total);
        last_ns = now, runs = frames = longest = 0;
    }
};
// Records this call's interval; true when the last kFastCalls intervals were all under kFastNs (the game flushing more
// than once per 5 ms audio frame, i.e. above 200 fps).
bool game_fast() {
    const u64 now = now_ns();
    if (g_last_call_ns) g_gaps[g_gap_at++ % kFastCalls] = now - g_last_call_ns;
    g_last_call_ns = now;
    if (g_gap_at < kFastCalls) return false;
    for (u64 g : g_gaps)
        if (g >= kFastNs) return false;
    return true;
}
RunLog g_held_log{"held, their commands kept for the next frame"};
RunLog g_drop_log{"dropped by the game on a full queue A (TOTK_AAL_CMD_HOLD=0)"};
}  // namespace

extern "C" u64 totk_aal_cmd_held(void) { return g_held.load(std::memory_order_relaxed); }
// The hook ran (this key has gen.py --hook 9c2ef0) with its hold on: hle_vi.cpp's 180 fps hold is then not needed
static std::atomic<bool> g_hook_seen{false};
extern "C" bool totk_aal_cmd_hold_active(void) { return g_hook_seen.load(std::memory_order_relaxed) && hold_on(); }
extern "C" u64 totk_aal_cmd_dropped(void) { return g_dropped.load(std::memory_order_relaxed); }

#ifdef _WIN32
void f_9c2ef0_orig(Ctx *__restrict c);
void aalcmd_orig_stub(Ctx *__restrict) {}
#pragma comment(linker, "/alternatename:?f_9c2ef0_orig@@YAXPEIAUCtx@@@Z=?aalcmd_orig_stub@@YAXPEIAUCtx@@@Z")
#else
__attribute__((weak)) void f_9c2ef0_orig(Ctx *__restrict c);
#endif

void hook_f_9c2ef0(Ctx *__restrict c) {   // flush the frame's command list (x0: a pointer to the controller pointer)
    if (!g_hook_seen.load(std::memory_order_relaxed)) g_hook_seen.store(true, std::memory_order_relaxed);
    const bool fast = game_fast();
    if (totk_ptr_ok(c->x[0])) {
        const u64 ctl = ld64(c->x[0]);
        const u64 cm = totk_ptr_ok(ctl) ? ld64(ctl + 0x1a8) : 0;
        if (totk_ptr_ok(cm)) {
            const s32 cap = (s32)ld32(cm + kRingCap);
            const s32 used = (s32)(ld32(cm + kRingPush) - ld32(cm + kRingPop));
            const s32 freen = (s32)(ld32(cm + kFreePush) - ld32(cm + kFreePop));
            const bool full = cap > 0 && used >= cap;
            const u64 pool = ld64(ctl + 0x1a0), cur = ld64(cm + kCurList);
            if (hold_on()) {
                if (full || (fast && (used >= kBacklog || freen <= kReserve) && g_run < kMaxRun)) {
                    if (totk_ptr_ok(pool) && ld8(pool + 8) && totk_ptr_ok(cur)) st32(cur + 0x4010, 0);
                    g_run++;
                    g_held.fetch_add(1, std::memory_order_relaxed);
                    g_held_log.hit(used, freen);
                    return;
                }
                g_run = 0;
                g_held_log.miss(g_held.load(std::memory_order_relaxed));
            } else {   // the original alone: count the lists it is about to lose on a full A
                const bool drop = full && totk_ptr_ok(pool) && ld8(pool + 8) && totk_ptr_ok(cur) && ld32(cur) != 0 &&
                                  freen > 0;
                if (drop) {
                    g_dropped.fetch_add(1, std::memory_order_relaxed);
                    g_drop_log.hit(used, freen);
                } else {
                    g_drop_log.miss(g_dropped.load(std::memory_order_relaxed));
                }
            }
        }
    }
#ifndef _WIN32
    if (!f_9c2ef0_orig) return;
#endif
    f_9c2ef0_orig(c);
}

// ---------------------------------------------------------------- aal's jam watchdog in time, not frames
// AudioRendererController::calc (main+0x9c2964; once a game frame from aal::System::calc, and in aal's resource-release
// wait loops) ends by tail-calling main+0x9c2b70, after the frame's flush (9c2ef0) and the source / bus / stream-cache
// calcs. 9c2b70 keeps a jam counter at this+0x1d8; the hook is on it, so it reads exactly the state its check reads. On
// a call with the command buffer manager enabled (ld64(this+0x1a0)+8), the free queue empty (CommandManager
// ld64(this+0x1a8): push +0x401a0 == pop +0x401a4) and queue A holding exactly 12 (+0x401c8 - +0x401cc), it adds 1; at
// every multiple of 300 it runs the queued lists, switches commands off for good (main+0x21ef964: enabled = 0) and
// stops the sources. A call with the free queue not empty sets it to 0. 300 calls are 10 s at the Switch's 30 fps, 1.25
// s at 240. The hook times the jam instead: while that condition holds it adds the time since the previous call (at
// most 100 ms a call, so a pause or a stall between calls does not count) and keeps the counter below 299 until the jam
// has lasted kJamSeconds (10 s, TOTK_AAL_WATCHDOG_S); then it sets 299, so the original's own +1 reaches 300 and the
// game's own code does the rest. The counter still runs from 0 as before up to 298 (its jam == 3 step keeps its place).
// A counter at -1 (the game's "off") is left alone. TOTK_AAL_WATCHDOG=0: the original's frame count alone. gen.py
// --hook 9c2b70.
namespace {
u64 g_wd_last_ns;        // the previous call (only aal's calc paths call it, one at a time under the game's own use)
double g_wd_jam_s;       // how long the current jam has lasted
std::atomic<u64> g_wd_fired{0};

bool watchdog_timed() {
    static const bool on = [] {
        const char *e = getenv("TOTK_AAL_WATCHDOG");
        return !e || strcmp(e, "0") != 0;
    }();
    return on;
}
double jam_seconds() {
    static const double v = [] {
        const char *e = getenv("TOTK_AAL_WATCHDOG_S");
        const double s = e ? atof(e) : 0;
        return s > 0 ? s : 10.0;
    }();
    return v;
}
}  // namespace

extern "C" u64 totk_aal_watchdog_fired(void) { return g_wd_fired.load(std::memory_order_relaxed); }

#ifdef _WIN32
void f_9c2b70_orig(Ctx *__restrict c);
#pragma comment(linker, "/alternatename:?f_9c2b70_orig@@YAXPEIAUCtx@@@Z=?aalcmd_orig_stub@@YAXPEIAUCtx@@@Z")
#else
__attribute__((weak)) void f_9c2b70_orig(Ctx *__restrict c);
#endif

void hook_f_9c2b70(Ctx *__restrict c) {   // the watchdog step AudioRendererController::calc ends with (this)
    const u64 now = now_ns();
    const u64 gap = g_wd_last_ns ? now - g_wd_last_ns : 0;
    g_wd_last_ns = now;
    const u64 self = c->x[0];
    if (watchdog_timed() && totk_ptr_ok(self)) {
        const u64 cbm = ld64(self + 0x1a0), cm = ld64(self + 0x1a8);
        if (totk_ptr_ok(cbm) && totk_ptr_ok(cm) && ld8(cbm + 8)) {
            const bool free_empty = ld32(cm + kFreePush) == ld32(cm + kFreePop);
            const bool a12 = (s32)(ld32(cm + kRingPush) - ld32(cm + kRingPop)) == 12;
            const u64 cnt = ld64(self + 0x1d8);
            if (!free_empty) {
                g_wd_jam_s = 0;   // (the original sets the counter to 0 on this call)
            } else if (a12 && cnt != ~0ull) {
                g_wd_jam_s += (gap < 100000000ull ? gap : 100000000ull) / 1e9;
                if (g_wd_jam_s >= jam_seconds()) {
                    if ((cnt + 1) % 300 != 0) st64(self + 0x1d8, 299);   // the original's +1 fires it now
                    g_wd_fired.fetch_add(1, std::memory_order_relaxed);
                    printf("[aal] audio command jam for %.1f s: the game's watchdog switches commands off (TOTK_AAL_WATCHDOG)\n",
                           g_wd_jam_s);
                    g_wd_jam_s = 0;
                } else if (cnt >= 299) {
                    st64(self + 0x1d8, 298);   // not yet: the original's +1 stays under 300
                }
            }
        }
    }
#ifndef _WIN32
    if (!f_9c2b70_orig) return;
#endif
    f_9c2b70_orig(c);
}
