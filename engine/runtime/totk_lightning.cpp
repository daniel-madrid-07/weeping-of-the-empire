// totk_lightning.cpp: thunderstorms keep the Switch's lightning rates at any frame rate.
//
// game::wm::LightningMgr::calc (main+0xa5f010, x0 = the manager, x1 = the weather calc arg, +4 = this frame's dt in
// 30 Hz units) runs once per game frame (ChemicalWorld::calcWorld -> wm::Module::createElements -> WorldMgrMainField
// vt+0x428 -> 0xa5e868). Its timers add dt, but in a storm it also rolls fixed chances PER CALL:
//   far bolt  0.013333 (0xa5fa44 -> flags +8 bit 0, a bolt 2 km out; the flags are cleared at entry: one-call events)
//   near strike 0.01   (0xa5fb50, state 1 -> +0xa0 = 90.0 and a random offset +0xa4/+0xa8: a strike lands near Link
//                       when that countdown, -dt per call at 0xa5f788 right after the rolls, reaches 0)
//   sky flash 0.01     (0xa5fac8 / 0xa5fb80 -> flags bit 3)
// so at 144 / 240 fps the horizon strobes (far bolts 1.9 / 3.2 per s instead of 0.4, flashes 1.4 / 2.4 instead of
// 0.3), while random strikes near Link almost stop (each new roll restarts the 3 s countdown: 0.019 / 0.002 per s
// instead of 0.12). The targeted strikes (state 2) run on the dt timer and are unaffected.
//
// The hook thins each roll that came true to the frame's 30 Hz ratio r (Bernoulli: kept with probability r, from its
// own generator so the game's random stream is untouched): the per-call chance becomes p * r, the Switch's chance per
// 30 Hz of game time. A thinned flag is cleared; a thinned near-strike roll is undone: the countdown and the offset
// get their values back, minus this call's dt, and if that countdown ran out on this call the strike lands as the
// original would have (+0x98 -> +0x88 = 1, +0x8c = the offset). At 30 fps and below (r >= 1) the original exactly.
// gen.py --hook a5f010 in GEN_MAIN; TOTK_LIGHTNING=0: the original alone. tools/oracle/test_lightning.py.
#include "rt.h"
#include "totk_globals.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
constexpr u64 kFrameDelta = 0x45577c0;   // GOT slot of the frame-delta setter's object (+8: 30 Hz ratio)
constexpr u64 kFlags = 0x8, kCountdown = 0xa0, kOffset = 0xa4, kTarget = 0x98;
constexpr u32 kFarBolt = 1, kFlash = 8;
constexpr float kStrikeDelay = 90.0f;    // 0xa5fbf4: 3 s in 30 Hz units

void stf(u64 a, float v) { u32 w; memcpy(&w, &v, 4); st32(a, w); }

u64 g_rng = 0x9e3779b97f4a7c15ull;
float uniform() {   // xorshift64*, [0, 1)
    g_rng ^= g_rng >> 12, g_rng ^= g_rng << 25, g_rng ^= g_rng >> 27;
    return (float)((g_rng * 0x2545f4914f6cdd1dull) >> 40) * (1.0f / 16777216.0f);
}
}  // namespace

extern "C" void totk_lightning_seed(unsigned long long s) { g_rng = s ? s : 1; }   // (the test's)

#ifdef _WIN32
void f_a5f010_orig(Ctx *__restrict c);
void lightning_orig_stub(Ctx *__restrict) {}
#pragma comment(linker, "/alternatename:?f_a5f010_orig@@YAXPEIAUCtx@@@Z=?lightning_orig_stub@@YAXPEIAUCtx@@@Z")
#else
__attribute__((weak)) void f_a5f010_orig(Ctx *__restrict c);
#endif
void hook_f_a5f010(Ctx *__restrict c) {   // game::wm::LightningMgr::calc(this, arg)
#ifndef _WIN32
    if (!f_a5f010_orig) return;
#endif
    static const bool on = [] {
        const char *e = getenv("TOTK_LIGHTNING");
        const bool v = !e || strcmp(e, "0");
        printf("[lightning] storm rolls %s\n", v ? "per 30 Hz of game time" : "per frame, as the game (TOTK_LIGHTNING=0)");
        return v;
    }();
    const u64 m = c->x[0], arg = c->x[1], fd = totk_global(kFrameDelta);
    const float r = on && totk_ptr_ok(m) && totk_ptr_ok(arg) && totk_ptr_ok(fd) ? ldf32(fd + 8) : 1.0f;
    if (!(r > 0.01f && r < 1.0f)) { f_a5f010_orig(c); return; }
    const float before = ldf32(m + kCountdown);
    const u64 offset = ld64(m + kOffset);
    f_a5f010_orig(c);
    const float dt = ldf32(arg + 4), after = ldf32(m + kCountdown);
    const u32 flags = ld32(m + kFlags);
    u32 keep = flags;
    if ((flags & kFarBolt) && uniform() >= r) keep &= ~kFarBolt;
    if ((flags & kFlash) && uniform() >= r) keep &= ~kFlash;
    if (keep != flags) st32(m + kFlags, keep);
    // a near strike scheduled on this call: the countdown restarted (90 - dt after this call's step) and a new offset
    const bool rolled = after == kStrikeDelay - dt && (ld64(m + kOffset) != offset || !(before > 0.0f && before - dt == after));
    if (!rolled || uniform() < r) return;
    st64(m + kOffset, offset);
    if (!(before > 0.0f)) { stf(m + kCountdown, before); return; }
    const float left = before - dt;
    stf(m + kCountdown, left);
    const u64 t = ld64(m + kTarget);
    if (left <= 0.0f && totk_ptr_ok(t)) st8(t + 0x88, 1), st64(t + 0x8c, offset);   // (0xa5f79c: it lands)
}
