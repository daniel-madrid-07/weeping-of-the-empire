// totk_keese_probe.cpp: flying enemies' path probes keep the Switch's cadence at any frame rate.
//
// FlyingEnemyPathFindinUserUsePathfindingModule::updateImpl_ (main+0x1dbdcf0, x0 = the module, once per frame; the
// Keese family and the Gerudo-dungeon boss's junior zombies) is a CPU throttle that became frame-locked: with the phase
// n = +0x38c, its helpers raycast and step a waypoint when n % 4 == 2 (0x1dbe2c0, called at 0x1dbddbc) and skip
// visible waypoints when n % 15 == 0 (0x1dbe090, at 0x1dbdd8c), then it advances n = (n + 1) % 15 (0x1dbde58). At
// 30 fps that is 7.5 / 2 Hz; at 240, 60 / 16 Hz: 8x the raycasts per Keese and slightly earlier corner cuts.
//
// The hook advances the phase once per 30 Hz of game time: it sums the frame's 30 Hz ratio r per module, and on a call
// where the sum does not reach the next whole frame it shows the original the phase 1 (neither probe) and puts the
// real phase back afterwards. At 30 fps and below (r >= 1) the original exactly. gen.py --hook 1dbdcf0 in GEN_MAIN;
// TOTK_KEESE_PROBE=0: the original alone. tools/oracle/test_keese_probe.py.
#include "rt.h"
#include "totk_globals.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
constexpr u64 kFrameDelta = 0x45577c0;   // GOT slot of the frame-delta setter's object (+8: 30 Hz ratio)
constexpr u64 kPhase = 0x38c;
constexpr u32 kIdlePhase = 1;            // 1 % 4 != 2 and 1 % 15 != 0: no probe

struct Mod { u64 module; float acc; };
Mod g_mods[64];
unsigned g_next;
std::atomic_flag g_lock = ATOMIC_FLAG_INIT;
}  // namespace

#ifdef _WIN32
void f_1dbdcf0_orig(Ctx *__restrict c);
void keese_orig_stub(Ctx *__restrict) {}
#pragma comment(linker, "/alternatename:?f_1dbdcf0_orig@@YAXPEIAUCtx@@@Z=?keese_orig_stub@@YAXPEIAUCtx@@@Z")
#else
__attribute__((weak)) void f_1dbdcf0_orig(Ctx *__restrict c);
#endif
void hook_f_1dbdcf0(Ctx *__restrict c) {   // FlyingEnemyPathFindinUserUsePathfindingModule::updateImpl_(this)
#ifndef _WIN32
    if (!f_1dbdcf0_orig) return;
#endif
    static const bool on = [] {
        const char *e = getenv("TOTK_KEESE_PROBE");
        const bool v = !e || strcmp(e, "0");
        printf("[keese-probe] flying enemies' path probes %s\n", v ? "per 30 Hz of game time" : "per frame, as the game (TOTK_KEESE_PROBE=0)");
        return v;
    }();
    const u64 m = c->x[0], fd = totk_global(kFrameDelta);
    const float r = on && totk_ptr_ok(m) && totk_ptr_ok(fd) ? ldf32(fd + 8) : 1.0f;
    if (!(r > 0.01f && r < 1.0f)) { f_1dbdcf0_orig(c); return; }
    bool advance;
    while (g_lock.test_and_set(std::memory_order_acquire)) {}
    Mod *p = nullptr;
    for (Mod &q : g_mods) if (q.module == m) p = &q;
    if (!p) p = &g_mods[g_next++ % 64], *p = {m, 1.0f - r};   // (a new module probes on its first call, as the game)
    p->acc += r;
    if ((advance = p->acc >= 1.0f - 1e-4f)) p->acc -= 1.0f;
    g_lock.clear(std::memory_order_release);
    if (advance) { f_1dbdcf0_orig(c); return; }
    const u32 n = ld32(m + kPhase);
    st32(m + kPhase, kIdlePhase);
    f_1dbdcf0_orig(c);
    st32(m + kPhase, n);
}
