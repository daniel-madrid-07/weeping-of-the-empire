// totk_interval_min.cpp: the game's present-interval minimum follows the live frame-rate setting. TotK's adaptive
// controller (main+0x8f6bec, the object at Framework + 0x248: {min, max} u32 at +0 / +4, the current interval at
// +0x14) picks the present interval each frame between its minimum and maximum. The patch set's fps60
// (games/totk/patches/sites_v100.py) lets it lower the interval and, since 2026-10-27, reads the minimum from that
// field as the game does (it hard-coded 1 before, so "30" ran at 60 and present's clamp made the controller flap 1 <->
// 2 with its LOD / DRS state). This hook on engine::framework::Framework::procFrame_ (main+0x8f689c, x0 = the
// Framework; it calls the controller with Framework + 0x248) writes the minimum before each frame: at "30"
// (vi_fps_setting() 0, not unlocked) the game's own minimum, at least 2 (the Switch's controller); otherwise 1 (60 Hz,
// the engine's cap paces higher rates). A value the game stores itself is kept as its own minimum. Only on an image
// with the minimum-reading fps60 (its first site main+0x8f6c60 holds the game's `cmp w10, w8` and main+0x8f6c88 the
// nop); otherwise the field is left alone. gen.py --hook 8f689c in GEN_MAIN; TOTK_INTERVAL_MIN=0: the game's field
// untouched. At 60 / unlocked a minimum the game raises on purpose (a heavier scene, an FMV) is overridden to 1, as
// the old hard-coded fps60 did: if a cutscene or video ever runs fast or out of sync at 60, suspect this first.
#include "rt.h"
#include "totk_globals.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" int vi_fps_setting(void), vi_unlock_setting(void);   // hle_vi.cpp: the frame-rate setting, live

namespace {
constexpr u64 kController = 0x248;                       // in the Framework
// main+0x8f6c60 as the game ships it (the compare fps60 keeps; an older fps60 replaced it with a fixed minimum), as a
// hash: no word of the game in the source
constexpr u32 kSiteHash = 0xfbfd3857u, kNop = 0xd503201fu;
constexpr u32 word_hash(u32 h) {
    h *= 0x9E3779B1u; h ^= h >> 15; h *= 0x85EBCA77u; h ^= h >> 13;
    return h;
}
struct State { u32 game_min = 0, written = 0; bool seen = false; };
State g_st;   // (procFrame_ runs on the game's main thread)

bool fps60_reads_min() {
    static const bool on = [] {
        const bool v = word_hash(ld32(totk_main(0x8f6c60))) == kSiteHash && ld32(totk_main(0x8f6c88)) == kNop;
        printf("[interval-min] %s\n", v ? "fps60 reads the controller's minimum: set from the frame-rate setting"
                                        : "fps60 absent or with a fixed minimum: the game's field left alone");
        return v;
    }();
    return on;
}
}  // namespace

// The minimum to write (tools/oracle/test_interval_min.py): cur = the field now; at30 = the setting is "30".
extern "C" unsigned totk_interval_min_next(unsigned cur, int at30) {
    State &s = g_st;
    if (!s.seen || cur != s.written) s.game_min = cur, s.seen = true;   // first look, or the game stored its own
    const unsigned own = s.game_min >= 1 && s.game_min <= 4 ? s.game_min : 2;
    const unsigned v = at30 ? (own > 2 ? own : 2) : 1;
    s.written = v;
    return v;
}

#ifdef _WIN32
void f_8f689c_orig(Ctx *__restrict c);
void intervalmin_orig_stub(Ctx *__restrict) {}
#pragma comment(linker, "/alternatename:?f_8f689c_orig@@YAXPEIAUCtx@@@Z=?intervalmin_orig_stub@@YAXPEIAUCtx@@@Z")
#else
__attribute__((weak)) void f_8f689c_orig(Ctx *__restrict c);
#endif
void hook_f_8f689c(Ctx *__restrict c) {   // engine::framework::Framework::procFrame_(this)
#ifndef _WIN32
    if (!f_8f689c_orig) return;
#endif
    static const bool on = !getenv("TOTK_INTERVAL_MIN") || strcmp(getenv("TOTK_INTERVAL_MIN"), "0");
    const u64 fw = c->x[0];
    if (on && totk_ptr_ok(fw) && fps60_reads_min()) {
        const u64 ctl = fw + kController;
        const u32 cur = ld32(ctl);
        const u32 v = totk_interval_min_next(cur, vi_fps_setting() == 0 && vi_unlock_setting() == 0);
        if (v != cur) {
            st32(ctl, v);
            static int logs;
            if (logs < 20 && ++logs) printf("[interval-min] minimum %u -> %u\n", cur, v);
        }
    }
    f_8f689c_orig(c);
}
