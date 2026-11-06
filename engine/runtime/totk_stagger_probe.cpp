// totk_stagger_probe.cpp: what the game's frame staggers cost at high frame rates (rival-034, v0.1.10 diagnostic: a
// shared 30 Hz counter for the staggers is coded only if this shows more than 0.1 ms of work per frame at 240 fps
// (LEAD 11-03 07:40).
//
// The two calls the staggers gate (1.0.0):
// - main+0x7d7a98 (AreaCullingMgr::updatePosRot, a low-confidence name): an actor whose turn it is (BaseProcMgr frame
//   counter, copied to ActorModule+0x154, % 10 == its +0x10 % 10) and that moved more than 1 unit or saw the culling
//   stamp change (0x7b030c / 0x7cf580 / 0x7d77b8);
// - main+0xe12b0c: the same % 60 turn in 0x7f5a84 (0x7f6380).
// At F fps both turns come F / 30 times as often as at 30. TOTK_STAGGER_PROBE=1: every 10 s (TOTK_STAGGER_PROBE_SECONDS)
// one "[stagger-probe]" line with each call's count and its total time (steady_clock: QPC on Windows), per second and
// per game frame (the BaseProcMgr frame counter's advance over the window). The time is summed over the threads that
// call (actor jobs run on several), so it is CPU time, not time on one thread's frame; each call's span holds one
// steady_clock read of the probe itself (~20-30 ns). Unset: the call, nothing else.
// gen.py --hook 7d7a98,e12b0c in GEN_MAIN; another game version: no place (engine_map HOOK_FREE, diagnostic).
#include "rt.h"
#include "totk_globals.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
using Clock = std::chrono::steady_clock;

struct Probe {
    const char *what;
    std::atomic<u64> calls{0}, ns{0};
};
Probe g_cull{"0x7d7a98 updatePosRot (% 10 turn)"}, g_e12{"0xe12b0c (% 60 turn)"};

std::atomic<s64> g_next{0};   // steady_clock ns of the next line (0: not started)
// the window's start, written by the one thread that wins g_next (the next reader comes a period later)
std::atomic<s64> g_start{0};
std::atomic<u32> g_frame0{0};
std::atomic<bool> g_frame0_ok{false};

s64 now_ns() { return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count(); }

int period_s() {
    static const int p = [] {
        const char *s = getenv("TOTK_STAGGER_PROBE_SECONDS");
        const int n = s ? atoi(s) : 0;
        return n > 0 ? n : 10;
    }();
    return p;
}

bool probe_on() {
    static const bool on = [] {
        const char *e = getenv("TOTK_STAGGER_PROBE");
        const bool v = e && !strcmp(e, "1");
        if (v) printf("[stagger-probe] on: the stagger-gated calls' count and time, a line every %d s\n", period_s());
        return v;
    }();
    return on;
}

// The game's frame number: shaft::baseproc::BaseProcMgr (the global at GOT slot main+0x4557790) +0x418, one up per game
// frame (main+0xab6df8, from ActorModule::calc). 1.0.0's offset: the hooks have no place in another version.
bool game_frame(u32 &n) {
    const u64 mgr = totk_global(0x4557790);
    if (!totk_ptr_ok(mgr)) return false;
    n = ld32(mgr + 0x418);
    return true;
}

void report(s64 t) {
    s64 next = g_next.load(std::memory_order_relaxed);
    if (next && t < next) return;
    if (!g_next.compare_exchange_strong(next, t + (s64)period_s() * 1000000000ll)) return;   // (another thread has it)
    u32 frame = 0;
    const bool frame_ok = game_frame(frame);
    if (next) {
        const double secs = (double)(t - g_start.load()) * 1e-9;
        const u32 frames = frame_ok && g_frame0_ok.load() ? frame - g_frame0.load() : 0;
        char line[512];
        int at = snprintf(line, sizeof line, "[stagger-probe] %.1f s", secs);
        if (frames) at += snprintf(line + at, sizeof line - at, ", %u game frames (%.1f fps)", frames, frames / secs);
        else at += snprintf(line + at, sizeof line - at, ", no game frame count");
        double ms_all = 0;
        for (Probe *p : {&g_cull, &g_e12}) {
            const u64 calls = p->calls.exchange(0), ns = p->ns.exchange(0);
            const double ms = ns * 1e-6;
            ms_all += ms;
            at += snprintf(line + at, sizeof line - at, "; %s: %llu calls (%.0f / s", p->what, (unsigned long long)calls,
                           calls / secs);
            if (frames) at += snprintf(line + at, sizeof line - at, ", %.2f / frame", (double)calls / frames);
            at += snprintf(line + at, sizeof line - at, "), %.2f ms (%.3f ms / s", ms, ms / secs);
            if (frames) at += snprintf(line + at, sizeof line - at, ", %.4f ms / frame", ms / frames);
            at += snprintf(line + at, sizeof line - at, ")");
        }
        if (frames) snprintf(line + at, sizeof line - at, "; both %.4f ms / frame (code the 30 Hz counter if > 0.1 at 240)",
                             ms_all / frames);
        printf("%s\n", line);
        fflush(stdout);
    } else {   // the first call: the window starts here (counts before it are dropped)
        g_cull.calls = 0, g_cull.ns = 0, g_e12.calls = 0, g_e12.ns = 0;
    }
    g_start.store(t), g_frame0.store(frame), g_frame0_ok.store(frame_ok);
}

template <void (*Orig)(Ctx *__restrict)>
void timed(Ctx *__restrict c, Probe &p) {
    const s64 t0 = now_ns();
    Orig(c);
    const s64 t1 = now_ns();
    p.calls.fetch_add(1, std::memory_order_relaxed);
    p.ns.fetch_add((u64)(t1 - t0), std::memory_order_relaxed);
    report(t1);
}
}  // namespace

#ifdef _WIN32
void f_7d7a98_orig(Ctx *__restrict c);
void f_e12b0c_orig(Ctx *__restrict c);
void staggerprobe_orig_stub(Ctx *__restrict) {}
#pragma comment(linker, "/alternatename:?f_7d7a98_orig@@YAXPEIAUCtx@@@Z=?staggerprobe_orig_stub@@YAXPEIAUCtx@@@Z")
#pragma comment(linker, "/alternatename:?f_e12b0c_orig@@YAXPEIAUCtx@@@Z=?staggerprobe_orig_stub@@YAXPEIAUCtx@@@Z")
#else
__attribute__((weak)) void f_7d7a98_orig(Ctx *__restrict c);
__attribute__((weak)) void f_e12b0c_orig(Ctx *__restrict c);
#endif
void hook_f_7d7a98(Ctx *__restrict c) {   // the % 10 turn's culling update (x0 the manager, x1 the actor's +0x5b8)
#ifndef _WIN32
    if (!f_7d7a98_orig) return;
#endif
    if (!probe_on()) { f_7d7a98_orig(c); return; }
    timed<f_7d7a98_orig>(c, g_cull);
}
void hook_f_e12b0c(Ctx *__restrict c) {   // the % 60 turn's call (x0 the actor)
#ifndef _WIN32
    if (!f_e12b0c_orig) return;
#endif
    if (!probe_on()) { f_e12b0c_orig(c); return; }
    timed<f_e12b0c_orig>(c, g_e12);
}
