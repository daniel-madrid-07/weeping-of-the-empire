// totk_load_laps.cpp: the game's own load lap labels, logged with a timestamp. The Sequence AINB programs mark their
// load stages with SeqOneShotPrintLoadLapTimeForPatrol nodes (a "Label" string input at node +0x20, the usual tagged
// Input<String>: char* = *(ld64(node + 0x20) & ~3)), wrapped as PrintLoadLapTimeSectionForPatrol modules around the
// save load (BeforeInitializeCommon) and each banc change (ChangeBanc: the banc's destroy, load and after-change
// stages, each a start and an end label; LoadBancImpl: actor creation). In the release game the node's oneShot
// (main+0x27b6e64, vtable +0x110, called by SeqOneShotBase::enterImpl_ main+0x27af854; only this class uses it; gen.py
// --hook 27b6e64 in GEN_MAIN) only sets its result bits: the print is compiled out. With TOTK_LOAD_LAPS=1 the hook
// prints "[lap] <ms since start> (+<ms since the last lap>) <label>" to the log, then runs the original. Off by
// default: nothing printed.
//
// A banc change in progress (totk_banc_change, for hle_vi.cpp's loading pace): the hook reads every label, whatever
// TOTK_LOAD_LAPS says (a few nodes a load), and holds the change's number (1, 2, ...) from "Banc破棄開始" (destroy
// start) to "Banc読み込み終了" (load end) or "Banc切り替え後処理開始", 0 otherwise. That is the stretch the loading
// screen covers before the world is drawn again (boot-load.md s3, s6): the destroy, the Continue path's save load and
// the run-up to actor creation read little from the RomFS, so the pace's "1 MiB read" test missed them (lp_A: 1.3-1.6 s
// of a world load or a fast travel at the cap). TOTK_LOAD_BANC=0: always 0.
#include "rt.h"
#include "totk_globals.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
const auto g_start = std::chrono::steady_clock::now();   // (static init: process start, near enough)

bool laps_on() {
    static const bool on = [] {
        const char *e = getenv("TOTK_LOAD_LAPS");
        return e && *e && strcmp(e, "0") != 0;
    }();
    return on;
}

// the Label input's text, or "?" (Input<String> at node +0x20: a tagged pointer to a slot whose first word is the char*)
const char *lap_label(u64 node) {
    if (!totk_ptr_ok(node)) return "?";
    const u64 slot = ld64(node + 0x20) & ~3ull;
    if (!totk_ptr_ok(slot)) return "?";
    const u64 text = ld64(slot);
    if (text < 0x10000 || text >= (1ull << 47)) return "?";
    return (const char *)text;
}

std::atomic<int> g_banc{0};   // the banc change in progress: its number (1, 2, ...), 0 between them
int g_bancs;                    // (changes started so far; the hook runs on the sequence's thread only)

bool banc_on() {
    static const bool on = [] {
        const char *e = getenv("TOTK_LOAD_BANC");
        return !e || strcmp(e, "0") != 0;
    }();
    return on;
}

// the label against one the game uses (strncmp: the guest text is read up to the first difference or the end of k)
template <size_t N> bool is_label(const char *label, const char (&k)[N]) { return strncmp(label, k, N) == 0; }
}  // namespace

extern "C" int totk_banc_change(void) { return g_banc.load(std::memory_order_relaxed); }

#ifdef _WIN32
void f_27b6e64_orig(Ctx *__restrict c);
void laps_orig_stub(Ctx *__restrict) {}
#pragma comment(linker, "/alternatename:?f_27b6e64_orig@@YAXPEIAUCtx@@@Z=?laps_orig_stub@@YAXPEIAUCtx@@@Z")
#else
__attribute__((weak)) void f_27b6e64_orig(Ctx *__restrict c);
#endif

void hook_f_27b6e64(Ctx *__restrict c) {   // SeqOneShotPrintLoadLapTimeForPatrol's oneShot(this)
    if (banc_on()) {
        const char *label = lap_label(c->x[0]);
        if (is_label(label, "Banc\xe7\xa0\xb4\xe6\xa3\x84\xe9\x96\x8b\xe5\xa7\x8b"))   // "Banc破棄開始"
            g_banc.store(++g_bancs, std::memory_order_relaxed);
        else if (is_label(label, "Banc\xe8\xaa\xad\xe3\x81\xbf\xe8\xbe\xbc\xe3\x81\xbf\xe7\xb5\x82\xe4\xba\x86") ||   // "Banc読み込み終了"
                 is_label(label, "Banc\xe5\x88\x87\xe3\x82\x8a\xe6\x9b\xbf\xe3\x81\x88\xe5\xbe\x8c\xe5\x87\xa6\xe7\x90\x86\xe9\x96\x8b\xe5\xa7\x8b"))   // "Banc切り替え後処理開始"
            g_banc.store(0, std::memory_order_relaxed);
    }
    if (laps_on()) {
        static double last_ms = -1;
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - g_start).count();
        char label[96];
        snprintf(label, sizeof label, "%.95s", lap_label(c->x[0]));   // (%.95s: the guest read stops at 95 bytes too)
        printf("[lap] %9.1f ms (+%8.1f) %s\n", ms, last_ms < 0 ? 0.0 : ms - last_ms, label);
        last_ms = ms;
    }
#ifndef _WIN32
    if (!f_27b6e64_orig) return;
#endif
    f_27b6e64_orig(c);
}

// ---------------------------------------------------------------- resource loader threads (measurement switch)
// ares::System::initializeThreads (main+0x11276cc; gen.py --hook 11276cc in GEN_MAIN) creates the "ARes Loading %@"
// threads: w2 = their count, from ResourceModule's param (+0xa0, via ares::System::initialize's arg +0xc). The arrays
// are sized by it (mLoadingThreadArray, mLoadingThreadArgArray) and all of them share one load queue. Its
// ThreadInitializeArg holds per-thread core / priority entries inline, 16 of them (0x1001128298), and spreads the
// threads round-robin over the guest cores (0x1001128268). So 1..16 fit. TOTK_LOADER_THREADS=<n> (1..16) replaces the
// count. It is a measurement switch for the 2-vs-4 A/B. Unset: the game's own count. The count is logged once either
// way: "[ares] loader threads <n> (game <g>)".
#ifdef _WIN32
void f_11276cc_orig(Ctx *__restrict c);
#pragma comment(linker, "/alternatename:?f_11276cc_orig@@YAXPEIAUCtx@@@Z=?laps_orig_stub@@YAXPEIAUCtx@@@Z")
#else
__attribute__((weak)) void f_11276cc_orig(Ctx *__restrict c);
#endif

void hook_f_11276cc(Ctx *__restrict c) {   // ares::System::initializeThreads(this, heap, count)
    const unsigned game = (unsigned)c->x[2];
    unsigned n = game;
    if (const char *e = getenv("TOTK_LOADER_THREADS")) {
        const long v = strtol(e, nullptr, 10);
        if (v >= 1 && v <= 16) n = (unsigned)v;
    }
    printf("[ares] loader threads %u (game %u)\n", n, game);
    c->x[2] = n;
#ifndef _WIN32
    if (!f_11276cc_orig) return;
#endif
    f_11276cc_orig(c);
}
