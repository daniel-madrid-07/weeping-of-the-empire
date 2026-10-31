// totk_aal_commands.cpp: the game's audio commands survive frame rates above 200 fps.
//
// The game thread hands its audio commands (play, stop, pitch, volume, the stream requests) to aal's AudioRenderer
// thread as CommandLists. Once a game frame, AudioRendererController::calc calls main+0x9c2ef0. If the current list
// (CommandManager +0x40250) holds a command, that function takes a fresh list from the free ring (+0x40190 cap,
// +0x40198 slots, +0x401a0 push, +0x401a4 pop) and pushes the current one into ring A (+0x401b8 cap 16, +0x401c0
// slots, +0x401c8 push, +0x401cc pop). The AudioRenderer thread (main+0xa07ef4) pops ONE list per audio frame:
// 5 ms, 200 a second. When ring A is full the function still swaps in the fresh list but does not push the full
// one. Its commands are lost, and the list never goes back to the pool of 16
// (CommandManager::initialize main+0xf3bc58). The Switch never runs the game above 60 fps; at more than 200 game
// frames a second ring A fills, the pool drains in seconds, and game -> audio commands and the stream reads stop.
// That fits the new-game black screen at 240 fps (the opening's streams die, its cue never fires; 120 and 30 are
// fine), and sound can stop the same way in play above 200 fps.
//
// The hook returns without doing anything while ring A is full, so the commands stay in the current list (what the
// function does itself when no free list is left), and the next call with room pushes them. Below 200 fps ring A is
// never full and the original runs every call. TOTK_AAL_CMD_HOLD=0: the original alone, with the lists it drops
// counted. Either way runs of full-ring frames go to the log ("[aal]"), so a stall that fills the ring shows. gen.py
// --hook 9c2ef0 in GEN_MAIN. tools/oracle/test_aal_commands.py.
#include "rt.h"
#include "totk_globals.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
constexpr u64 kRingCap = 0x401b8, kRingPush = 0x401c8, kRingPop = 0x401cc;   // ring A, in CommandManager
constexpr u64 kFreePush = 0x401a0, kFreePop = 0x401a4;                         // the free ring
constexpr u64 kCurList = 0x40250;                                               // the list being filled
std::atomic<u64> g_held{0}, g_dropped{0};

bool hold_on() {
    static const bool on = [] {
        const char *e = getenv("TOTK_AAL_CMD_HOLD");
        return !e || strcmp(e, "0") != 0;
    }();
    return on;
}

// "[aal]" lines (a ring left full must show in the log). Held frames (the hook) and, with the hold off, the lists the
// original drops (ring A full, the current list not empty, a free list to swap in) are counted in runs: the first one at
// once, then one line at the end of a run at most every 2 s, with the runs, frames and longest run since the last line; a
// run that does not end (the audio thread stalled) gets a "still full" line every 5 s (the nit). Only the game thread
// calls 9c2ef0.
u64 now_ns() {
    return (u64)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
u64 still_after_ns() {   // TOTK_AAL_RING_STILL_MS: the interval of the still-full line (default 5 s; tests use less)
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
    void hit(s32 used, s32 cap) {
        run++, frames++;
        const u64 now = now_ns();
        if (run == 1) run_ns = still_ns = now;
        if (first) {
            first = false;
            printf("[aal] audio command ring A full (%d of %d): %s\n", used, cap, what);
        } else if (now - still_ns >= still_after_ns()) {   // a ring that never empties (audio thread stalled)
            still_ns = now;
            printf("[aal] ring A still full after %.1f s: %u frames %s in this run\n", (now - run_ns) / 1e9, run, what);
        }
    }
    void miss(u64 total) {
        if (!run) return;
        runs++;
        if (run > longest) longest = run;
        run = 0;
        const u64 now = now_ns();
        if (now - last_ns < 2000000000ull && last_ns) return;
        printf("[aal] ring A was full: %u frames %s in %u runs (longest %u) since the last line; %llu in all\n", frames,
               what, runs, longest, (unsigned long long)total);
        last_ns = now, runs = frames = longest = 0;
    }
};
RunLog g_held_log{"held, their commands kept for the next frame"};
RunLog g_drop_log{"dropped by the game (TOTK_AAL_CMD_HOLD=0): their commands lost, the lists leaked"};
}  // namespace

extern "C" u64 totk_aal_cmd_held(void) { return g_held.load(std::memory_order_relaxed); }
extern "C" u64 totk_aal_cmd_dropped(void) { return g_dropped.load(std::memory_order_relaxed); }

#ifdef _WIN32
void f_9c2ef0_orig(Ctx *__restrict c);
void aalcmd_orig_stub(Ctx *__restrict) {}
#pragma comment(linker, "/alternatename:?f_9c2ef0_orig@@YAXPEIAUCtx@@@Z=?aalcmd_orig_stub@@YAXPEIAUCtx@@@Z")
#else
__attribute__((weak)) void f_9c2ef0_orig(Ctx *__restrict c);
#endif

void hook_f_9c2ef0(Ctx *__restrict c) {   // flush the frame's command list (x0: a pointer to the controller pointer)
    if (totk_ptr_ok(c->x[0])) {
        const u64 ctl = ld64(c->x[0]);
        const u64 cm = totk_ptr_ok(ctl) ? ld64(ctl + 0x1a8) : 0;
        if (totk_ptr_ok(cm)) {
            const s32 cap = (s32)ld32(cm + kRingCap);
            const s32 used = (s32)(ld32(cm + kRingPush) - ld32(cm + kRingPop));
            const bool full = cap > 0 && used >= cap;
            const u64 pool = ld64(ctl + 0x1a0), cur = ld64(cm + kCurList);
            if (hold_on()) {
                if (full) {   // ring A full: keep the current list for the next call
                    // (as the original's own early returns do after its first step: the current list's state word
                    // +0x4010 set to 0, done under the cm+0x40270 lock there. Main has no load of that word, only
                    // stores (9c2ef0, a08b04, StreamThread::calc_, 21f18d8, 21f2fcc), and only this thread swaps the
                    // current list)
                    if (totk_ptr_ok(pool) && ld8(pool + 8) && totk_ptr_ok(cur)) st32(cur + 0x4010, 0);
                    g_held.fetch_add(1, std::memory_order_relaxed);
                    g_held_log.hit(used, cap);
                    return;
                }
                g_held_log.miss(g_held.load(std::memory_order_relaxed));
            } else {   // the original alone: count the lists it is about to drop
                const bool drop = full && totk_ptr_ok(pool) && ld8(pool + 8) && totk_ptr_ok(cur) && ld32(cur) != 0 &&
                                  (s32)(ld32(cm + kFreePush) - ld32(cm + kFreePop)) > 0;
                if (drop) {
                    g_dropped.fetch_add(1, std::memory_order_relaxed);
                    g_drop_log.hit(used, cap);
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
