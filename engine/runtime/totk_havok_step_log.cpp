// totk_havok_step_log.cpp: what Havok's solver is given each physics step (fps360, v0.1.9 diagnostic; off by default).
//
// v0.1.8: game steps under 1/240 s corrupt Havok's worker memory (TtSolveConstraints main+0x286d50 -> +0x29a898 ->
// +0x2972dc; the narrow phase's caches), 3 of 4 unlocked runs with the step only a few % under 1/240, none with the 1/240
// floor; nothing in the step setup has a threshold there. This logs that setup where it happens: the world's step
// controller (main+0x274a3c, x0 the controller, x1 the gravity vector, x2 the hknpSolverInfo, s0 dt, s1 the second dt)
// sets numSteps = clamp(ceil(120 dt), 1, 8) and tau = min(1, 0.6 (120 dt / n)^2) (hknpSolverInfo::setTauAndDamping
// main+0x10d9b88: tau +0, damping +8), then tail-calls hknpSolverInfo::setStepInfo (main+0x2588cc: dt +0x10, numSteps
// +0x78, microsteps +0x80), which also rescales +0x8c by (n dt_old) / (n_old dt) whenever the step or the count changes
// (at a clamped 1/240 it never does; under it, each frame). TOTK_HAVOK_STEP_LOG=1: per world and second "[havok-step]" min
// / max of each, the steps under 1/240, anything not finite at once; the first 24 calls in full. Every line flushed (a
// crash keeps it). Unset: the call, nothing else. gen.py --hook 274a3c in GEN_MAIN; another game version: no place
// (engine_map HOOK_FREE, diagnostic).
#include "rt.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace {
struct Window {   // one world's second
    u64 ctrl;
    unsigned calls, under, nonfinite;
    float dt_lo, dt_hi, tau_lo, tau_hi, f8c_lo, f8c_hi;
    int n_lo, n_hi;
    std::chrono::steady_clock::time_point start;
};
constexpr int kWorlds = 8;
Window g_win[kWorlds];
std::mutex g_mu;
unsigned g_calls;

float f32_at(u64 a) { float v; memcpy(&v, (const void *)(uintptr_t)a, 4); return v; }
s32 s32_at(u64 a) { s32 v; memcpy(&v, (const void *)(uintptr_t)a, 4); return v; }

void flush_window(Window &w) {
    if (!w.calls) return;
    printf("[havok-step] world %llx: %u steps, dt %.3f..%.3f ms (%u under 1/240), n %d..%d, tau %.4f..%.4f, "
           "+0x8c %g..%g%s\n", (unsigned long long)w.ctrl, w.calls, w.dt_lo * 1e3, w.dt_hi * 1e3, w.under, w.n_lo, w.n_hi,
           w.tau_lo, w.tau_hi, w.f8c_lo, w.f8c_hi, w.nonfinite ? " NOT FINITE" : "");
    fflush(stdout);
}
}  // namespace

// One step's values into its world's window (any thread); logs a finished second, the first calls and non-finite ones.
extern "C" void totk_havok_step_note(u64 ctrl, float dt_in, float dt2, float dt, int n, int micro, float tau, float f8c) {
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(g_mu);
    const bool bad = !std::isfinite(dt) || !std::isfinite(tau) || !std::isfinite(f8c) || !(dt > 0) || n < 1;
    if (g_calls++ < 24 || bad) {
        printf("[havok-step] %s world %llx: dt in %.4f ms (2nd %.4f), stepped %.4f ms, n %d, micro %d, tau %.5f, +0x8c %g\n",
               bad ? "NOT FINITE / BAD" : "step", (unsigned long long)ctrl, dt_in * 1e3, dt2 * 1e3, dt * 1e3, n, micro, tau, f8c);
        fflush(stdout);
    }
    Window *w = nullptr;
    for (Window &x : g_win)
        if (x.ctrl == ctrl || !x.ctrl) { w = &x; break; }
    if (!w) return;   // (more worlds than kWorlds: the first ones are logged)
    if (w->ctrl && now - w->start >= std::chrono::seconds(1)) { flush_window(*w); w->calls = 0; }
    if (!w->calls) {
        *w = Window{ctrl, 0, 0, 0, dt, dt, tau, tau, f8c, f8c, n, n, now};
    }
    w->calls++;
    w->under += dt < 1.0f / 240 * 0.999f;
    w->nonfinite += bad;
    w->dt_lo = fminf(w->dt_lo, dt), w->dt_hi = fmaxf(w->dt_hi, dt);
    w->tau_lo = fminf(w->tau_lo, tau), w->tau_hi = fmaxf(w->tau_hi, tau);
    w->f8c_lo = fminf(w->f8c_lo, f8c), w->f8c_hi = fmaxf(w->f8c_hi, f8c);
    w->n_lo = n < w->n_lo ? n : w->n_lo, w->n_hi = n > w->n_hi ? n : w->n_hi;
}

#ifdef _WIN32
void f_274a3c_orig(Ctx *__restrict c);
void havokstep_orig_stub(Ctx *__restrict) {}
#pragma comment(linker, "/alternatename:?f_274a3c_orig@@YAXPEIAUCtx@@@Z=?havokstep_orig_stub@@YAXPEIAUCtx@@@Z")
#else
__attribute__((weak)) void f_274a3c_orig(Ctx *__restrict c);
#endif
void hook_f_274a3c(Ctx *__restrict c) {   // the world's step controller (x0), hknpSolverInfo (x2), dt (s0, s1)
#ifndef _WIN32
    if (!f_274a3c_orig) return;
#endif
    static const bool on = [] { const char *e = getenv("TOTK_HAVOK_STEP_LOG"); return e && !strcmp(e, "1"); }();
    if (!on) { f_274a3c_orig(c); return; }
    const u64 ctrl = c->x[0], info = c->x[2];
    const float dt_in = c->v[0].f[0], dt2 = c->v[1].f[0];
    f_274a3c_orig(c);
    if (!info) return;
    totk_havok_step_note(ctrl, dt_in, dt2, f32_at(info + 0x10), s32_at(info + 0x78), s32_at(info + 0x80), f32_at(info),
                         f32_at(info + 0x8c));
}
