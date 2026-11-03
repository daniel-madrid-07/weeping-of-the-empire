// totk_effect_sway.cpp: the 'Basic2' effect sway spring keeps the Switch's dynamics at any frame rate.
//
// game::effect::CallbackEmitterPostCalc_Basic2 (main+0x13b0da0), for an emitter whose resource asks for sway (its
// Initialize made the 0x24-byte state S, main+0x13b0c98), runs a damped spring per call: the anchor p (S+0, the emitter's
// point), a lagging point q (S+0xc) and its velocity u (S+0x18, a displacement per call): q += u (0x13b13d0), then
// u = k2 (u + k1 (p - q)) (0x13b1158..0x13b11ac), then q is kept within the resource's max distance of p. k1, k2 come
// from the resource (`ldp s10, s9, [x8, #0x40]` at 0x13b1048) and are per CALL: at 240 fps the spring was 64x as stiff
// and its damping 8x as fast in game time, so swaying effects (a carried torch's flame, a fire arrow) lagged and
// swung differently from the Switch.
//
// The hook gives one call the constants k1', k2' that move the spring's two modes by the r-th power of what one 30 fps
// call does (r the frame's 30 Hz ratio; totk_effect_sway_k below): 1 / r calls of it = one call at 30 fps, so the sway
// keeps the Switch's stiffness and damping in game time. It is on the callee main+0xcc2c2c at its call right after the
// k1 / k2 load (lr main+0x13b10a0; s9 / s10 are callee-saved, so the caller reads them back from the context). Every
// other call of 0xcc2c2c is the original. At 30 fps and below (r >= 1) the original exactly. gen.py --hook cc2c2c in
// GEN_MAIN; TOTK_EFFECT_SWAY=0: the original alone. tools/oracle/test_effect_sway.py.
#include "rt.h"
#include "totk_globals.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
constexpr u64 kSwayCall = totk_main(0x13b10a0);   // return address of the call right after the spring's k1 / k2 load
}  // namespace

// The spring's per-call constants at the 30 Hz ratio r (exported for the stand-in test). One call maps (q, u) by
// M = [[1, 1], [-k2 k1, k2 (1 - k1)]] (q += u, then u = k2 (u + k1 (p - q))): det M = k2, tr M = 1 + k2 (1 - k1). The
// per-call M' of the same form with det' = det^r and tr' = the sum of M's eigenvalues^r has M's eigenvalues^r: r calls
// of it move the spring's modes as one call at 30 fps. So k2' = k2^r and k1' = 1 - (tr' - 1) / k2'. A pair of real
// eigenvalues not both positive (no real r-th power) falls back to the semi-implicit Euler step at r: k1' = k1 r^2.
extern "C" void totk_effect_sway_k(float k1, float k2, float r, float *k1r, float *k2r) {
    *k1r = k1, *k2r = k2;
    if (!(k2 > 0.0f && k2 < 1.0f)) {
        *k1r = (float)((double)k1 * r * r);
        return;
    }
    const double tr = 1.0 + k2 * (1.0 - (double)k1), det = k2, disc = tr * tr - 4 * det, b = pow(det, (double)r);
    double trr;
    if (disc < 0) {   // complex pair rho e^(+-i theta)
        const double rho = sqrt(det), th = acos(fmax(-1.0, fmin(1.0, tr / (2 * rho))));
        trr = 2 * pow(rho, (double)r) * cos(r * th);
    } else {
        const double l1 = (tr + sqrt(disc)) / 2, l2 = (tr - sqrt(disc)) / 2;
        if (!(l1 > 0 && l2 > 0)) {
            *k1r = (float)((double)k1 * r * r), *k2r = (float)b;
            return;
        }
        trr = pow(l1, (double)r) + pow(l2, (double)r);
    }
    *k2r = (float)b;
    *k1r = (float)(1.0 - (trr - 1.0) / b);
}

#ifdef _WIN32
void f_cc2c2c_orig(Ctx *__restrict c);
void effect_sway_orig_stub(Ctx *__restrict) {}
#pragma comment(linker, "/alternatename:?f_cc2c2c_orig@@YAXPEIAUCtx@@@Z=?effect_sway_orig_stub@@YAXPEIAUCtx@@@Z")
#else
__attribute__((weak)) void f_cc2c2c_orig(Ctx *__restrict c);
#endif
void hook_f_cc2c2c(Ctx *__restrict c) {
#ifndef _WIN32
    if (!f_cc2c2c_orig) return;
#endif
    const u64 lr = c->x[30];
    f_cc2c2c_orig(c);
    if (lr != kSwayCall) return;
    static const bool on = [] {
        const char *e = getenv("TOTK_EFFECT_SWAY");
        const bool v = !e || strcmp(e, "0");
        printf("[effect-sway] effect sway spring %s\n", v ? "in game time" : "per frame, as the game (TOTK_EFFECT_SWAY=0)");
        return v;
    }();
    const u64 fd = totk_frame_delta();
    if (!on || !totk_ptr_ok(fd)) return;
    const float r = totk_frame_ratio(fd);
    if (!(r > 0.01f && r < 1.0f)) return;
    float k1, k2;
    totk_effect_sway_k(c->v[10].f[0], c->v[9].f[0], r, &k1, &k2);
    c->v[10].f[0] = k1, c->v[9].f[0] = k2;
}
