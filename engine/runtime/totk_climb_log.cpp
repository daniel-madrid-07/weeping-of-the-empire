// totk_climb_log.cpp: TOTK_DEBUG_CLIMB=1 (diagnostics, public #59 "higher frame rates than 30 make climbing
// slower"): what the climb's physics takes from the animation each second.
//
// ExecutePlayerClimb::updatePhysics (main+0x1cf621c) calls Player::getAdjustedRetargetPosAndRot (main+0xba6d48) at
// main+0x1cf6330: it reads the animation's root motion of this update (main+0x66689c(+0x20) position, +0x2c rotation),
// divides it by the frame-delta object's deltaFrame (the 30 Hz ratio x the time multiplier, [VFRMgr + 0]; a per-core
// copy at +0x10 + 0x28 n on some threads) and writes it to x1 / x2; the caller multiplies by 30 (m/s) and moves Link.
// Static reading finds that frame-rate independent IF the root motion of an update is the animation's motion over that
// update's game time. This logs, once a second while climbing, the mean ratio, the mean |position| the call returns
// (per 30 Hz frame: x 30 = m/s), the share of updates that returned zero motion, and the call count. Climbing at a 30 cap
// and then at 240 on the same wall: the m/s equal and Link still slower = the physics / controller side; the m/s lower at
// 240 = the animation's root motion per update is short of its game time (the animation side). Read-only (the call's
// outputs untouched); off by default. gen.py --hook ba6d48 in GEN_MAIN.
#include "rt.h"
#include "totk_globals.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
constexpr u64 kClimbCall = totk_main(0x1cf6334);   // return address of updatePhysics' call
struct Acc { double ratio = 0, len = 0, moved = 0; unsigned calls = 0, zeros = 0; };   // (moved: sum of len x ratio)
Acc g_acc;
std::chrono::steady_clock::time_point g_t0;
int g_lines;
}  // namespace

// One second's mean m/s from the sums (tools/oracle/test_climb_log.py): mean |position per 30 Hz frame| x 30.
extern "C" double totk_climb_mps(double len_sum, unsigned calls) { return calls ? len_sum / calls * 30.0 : 0.0; }

#ifdef _WIN32
void f_ba6d48_orig(Ctx *__restrict c);
void climb_log_orig_stub(Ctx *__restrict) {}
#pragma comment(linker, "/alternatename:?f_ba6d48_orig@@YAXPEIAUCtx@@@Z=?climb_log_orig_stub@@YAXPEIAUCtx@@@Z")
#else
__attribute__((weak)) void f_ba6d48_orig(Ctx *__restrict c);
#endif
void hook_f_ba6d48(Ctx *__restrict c) {   // Player::getAdjustedRetargetPosAndRot(this, Vec3f *pos, Vec3f *rot) -> w0
#ifndef _WIN32
    if (!f_ba6d48_orig) return;
#endif
    const u64 lr = c->x[30], pos = c->x[1];
    f_ba6d48_orig(c);
    if (lr != kClimbCall) return;
    static const bool on = [] {
        const char *e = getenv("TOTK_DEBUG_CLIMB");
        const bool v = e && *e && strcmp(e, "0");
        if (v) printf("[climb] logging the climb's root motion once a second (TOTK_DEBUG_CLIMB)\n");
        return v;
    }();
    if (!on || !(c->x[0] & 1) || !totk_ptr_ok(pos & ~7ull)) return;
    const float x = ldf32(pos), y = ldf32(pos + 4), z = ldf32(pos + 8);
    const double len = std::sqrt((double)x * x + (double)y * y + (double)z * z);
    const u64 fd = totk_frame_delta();
    g_acc.ratio += totk_ptr_ok(fd) ? totk_frame_ratio(fd) : 0;
    g_acc.len += std::isfinite(len) ? len : 0;
    g_acc.moved += std::isfinite(len) && totk_ptr_ok(fd) ? len * totk_frame_ratio(fd) : 0;
    g_acc.zeros += len == 0;
    g_acc.calls++;
    const auto now = std::chrono::steady_clock::now();
    if (g_t0.time_since_epoch().count() == 0) g_t0 = now;
    if (now - g_t0 < std::chrono::seconds(1)) return;
    // (as moved: each update's vector x its ratio is the root motion's own distance that update, summed over the real
    // time elapsed; at real-time speed it equals the scaled m/s)
    const double el = std::chrono::duration<double>(now - g_t0).count();
    if (g_lines++ < 600)
        printf("[climb] %.2f s: %u updates, ratio %.3f, root motion %.3f m/s as the call scales it (x 30 / ratio), %.3f m/s "
               "as moved per real second, zero-motion updates %u\n",
               el, g_acc.calls, g_acc.ratio / g_acc.calls, totk_climb_mps(g_acc.len, g_acc.calls), g_acc.moved / el,
               g_acc.zeros);
    g_acc = Acc{}, g_t0 = now;
}
