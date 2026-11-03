// totk_glide_ease.cpp: gliding under a Cucco keeps the Switch's drift and turn at any frame rate.
//
// ExecutePlayerAnimalGlide's update (FUN main+0x1cc9470: the AI node of gliding while holding up a Cucco, AS command
// "ParasailGlide") eases Link's horizontal glide velocity, node +0x24/+0x28/+0x2c in metres per 30 Hz frame:
//   s = GameActor::approachTargetSpeed(T = ScaleWithGravity(0.35, 0.45) * stick length, a = 0.1, d = 0.003)
//       (main+0x16e4b80: from u = Player::getForwardSpeedMpf, min(u + a*r0, T) below T, max(u - d*r0, T) above,
//       r0 = the VFRMgr's delta, *getCounter())
//   v += (s * dir - v) * 0.1     (main+0x1cc9528..0x1cc9560, once per FRAME, no dt; dir = the stick in camera space)
// then Player::setForwardDirAndSpeed(normalize(v), |v|). The ramp-up is time-based already (the r0 lead times the
// 0.1 ease is 0.01 per 30 Hz frame at any rate), but the approach to a fixed target is not: after the stick is
// released Link stops drifting in 183 / 92 ms at 120 / 240 fps instead of 733 (2.7 m of drift -> 0.7 / 0.3 m), and a
// 90 degree turn reaches 45 degrees in 67 / 33 ms instead of 233.
//
// The hook (approachTargetSpeed, only at the glide's call site main+0x1cc9510: 30 other callers keep the original)
// gives that ease its 30 Hz rate: with q = 0.9^r per call (r: the frame's 30 Hz ratio), it returns s* = the target
// with the Switch's per-30-Hz lead (u + a*r0*K/(1-q), clamped to T as the original), and sets v = t* + (v - t*) * q/0.9
// (t* = s* * dir), so the caller's own v += (t* - v) * 0.1 lands on t* + (v - t*) * q: the exponential ease at
// 0.9 per 30 Hz frame and the ramp at 0.01 per 30 Hz frame, as at 30 fps. At 30 fps and below (r >= 1) the original
// exactly. gen.py --hook 16e4b80 in GEN_MAIN; TOTK_GLIDE_EASE=0: the original alone. tools/oracle/test_glide_ease.py.
#include "rt.h"
#include "totk_globals.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
constexpr u64 kGlideCall = totk_main(0x1cc9510);   // return address of ExecutePlayerAnimalGlide's call
constexpr u64 kGlideVt = totk_main(0x4376e68);     // its vtable (slot 1 getRuntimeTypeInfo 0x1cc9acc, 26 updateImpl_)
constexpr u64 kVel = 0x24;                          // node: the eased velocity (3 floats)
constexpr float kEase = 0.1f;                       // the caller's per-call factor (main+0x3776090)
// The paraglider (ExecutePlayerParasailGlide::updatePhysics, main+0x1d0d0a8; fps360's per-frame sweep, LEAD 11-03): the
// same ease after the same call, v(+0x24..0x2c) += (min(s, cap) * dir - v) * ScaleWithGravity(0.1, 0.1)
// (main+0x1d0d16c..0x1d0d1f0), dir at the caller's sp + 0x30, the cap 0.4 / 0.74 (main+0x3746b38, by the Player
// component's +0xb06 bit 3) unless the node's +0x18 bit 1 is set
constexpr u64 kParaCall = totk_main(0x1d0d16c);     // return address of its approachTargetSpeed call
constexpr u64 kParaVt = totk_main(0x437ec50);       // its vtable (slot 1 getRuntimeTypeInfo 0x1d0d7f0, 26 updateImpl_);
                                                    // not placed in 1.4.3: there the call site alone (its x20 is always the node)
constexpr u64 kParaCap = totk_main(0x3746b38);      // f32[2]: 0.4, 0.74

void stf(u64 a, float v) { u32 w; memcpy(&w, &v, 4); st32(a, w); }
}  // namespace

// The fix for one call (tools/oracle/test_glide_ease.py drives it through the hook). s: approachTargetSpeed's result,
// T / a / d: its target and steps, r / r0: the frame's 30 Hz ratio and VFR delta, dir: the stick direction, v: the
// node's velocity. Returns 0 where the original stays (s and v untouched), else 1 with *s and v updated.
// cap: the caller's own clamp of s before it eases toward s * dir (the paraglider's; INFINITY: none, the Cucco glide).
extern "C" int totk_glide_ease_step_cap(float *sp, float T, float a, float d, float r, float r0, float cap, const float dir[3],
                                        float v[3]) {
    if (!(r > 0.01f && r < 1.0f) || !(r0 > 0.0f)) return 0;
    const float s = *sp;
    const double q = pow(1.0 - (double)kEase, (double)r), lead = (double)r0 * kEase / (1.0 - q);   // per call
    double sn = s;
    if (s < T) sn = fmin((double)s - (double)a * r0 + (double)a * lead, (double)T);        // accelerating: u + a*lead
    else if (s > T) sn = fmax((double)s + (double)d * r0 - (double)d * lead, (double)T);   // slowing: u - d*lead
    float s2 = (float)sn;
    if (s2 > cap) s2 = cap;   // (fcmp / fcsel gt as the caller: then its own clamp leaves s2 alone)
    const double keep = q / (1.0 - (double)kEase);   // 0.9^(r-1): the caller's step multiplies by 0.9
    for (int i = 0; i < 3; i++) {
        const double t = (double)(s2 * dir[i]);   // (the caller's float product)
        v[i] = (float)(t + ((double)v[i] - t) * keep);
    }
    *sp = s2;
    return 1;
}
extern "C" int totk_glide_ease_step(float *sp, float T, float a, float d, float r, float r0, const float dir[3], float v[3]) {
    return totk_glide_ease_step_cap(sp, T, a, d, r, r0, INFINITY, dir, v);
}

// VFRMgr::getCounter(this) -> the counter in use. Named as a function so another version's engine map aliases it (pinned)
// and places its address too: totk_main_known(0x850d7c) says whether this build has it.
void f_850d7c(Ctx *__restrict c);

#ifdef _WIN32
void f_16e4b80_orig(Ctx *__restrict c);
void glide_orig_stub(Ctx *__restrict) {}
#pragma comment(linker, "/alternatename:?f_16e4b80_orig@@YAXPEIAUCtx@@@Z=?glide_orig_stub@@YAXPEIAUCtx@@@Z")
#else
__attribute__((weak)) void f_16e4b80_orig(Ctx *__restrict c);
#endif
void f_1076be0(Ctx *__restrict c);   // phive::game::ScaleWithGravity(s0, s1) -> s0 (the paraglider's ease factor)

// The paraglider's call (kParaCall): the Cucco glide's fix with the caller's clamp, and only where its factor is the
// 0.1 (ScaleWithGravity(0.1, 0.1) as the caller takes it after the call; another value, a gravity scale, keeps the
// original). TOTK_PARAGLIDE_EASE=0: the original alone.
static void paraglide(Ctx *__restrict c, float T, float a, float d) {
    static const bool on = [] {
        const char *e = getenv("TOTK_PARAGLIDE_EASE");
        const bool v = !e || strcmp(e, "0");
        printf("[glide-ease] paraglider velocity %s\n",
               v ? "eased in game time" : "eased per frame, as the game (TOTK_PARAGLIDE_EASE=0)");
        return v;
    }();
    const u64 node = c->x[20], pl = c->x[21], sp = c->sp, fd = totk_frame_delta();   // (the caller's x20 = its node,
    // x21 = the Player component, sp + 0x30 = the stick direction)
    if (!on || !totk_main_known(0x850d7c) || !totk_main_known(0x1076be0) || !totk_main_known(0x3746b38) || !totk_ptr_ok(fd) ||
        !totk_ptr_ok(node) ||
        (totk_main_known(0x437ec50) && ld64(node) != kParaVt) || !totk_ptr_ok(pl) || !totk_ptr_ok(sp + 0x30))
        return;
    const V128 s0 = c->v[0], s1 = c->v[1];
    c->v[0] = vS(kEase), c->v[1] = vS(kEase);
    f_1076be0(c);
    const float k = c->v[0].f[0];
    c->x[0] = fd;
    f_850d7c(c);
    const u64 counter = c->x[0];
    c->v[0] = s0, c->v[1] = s1;
    if (k != kEase || counter != fd) return;
    const float cap = ld8(node + 0x18) & 2 ? INFINITY : ldf32(kParaCap + 4 * ((ld8(pl + totk_field(Player, 0xb06)) & 8) == 0));
    float dir[3], v[3];
    for (int i = 0; i < 3; i++) dir[i] = ldf32(sp + 0x30 + 4 * i), v[i] = ldf32(node + kVel + 4 * i);
    float s = c->v[0].f[0];
    if (!totk_glide_ease_step_cap(&s, T, a, d, totk_frame_ratio(fd), ldf32(counter), cap, dir, v)) return;
    for (int i = 0; i < 3; i++) stf(node + kVel + 4 * i, v[i]);
    c->v[0] = vS(s);
}

void hook_f_16e4b80(Ctx *__restrict c) {   // GameActor::approachTargetSpeed(this, s0 = T, s1 = a, s2 = d) -> s0
#ifndef _WIN32
    if (!f_16e4b80_orig) return;
#endif
    const u64 lr = c->x[30], node = c->x[21], fp = c->x[29];   // (the caller's x21 = its node, x29 = its frame)
    const float T = c->v[0].f[0], a = c->v[1].f[0], d = c->v[2].f[0];
    f_16e4b80_orig(c);
    if (lr == kParaCall) return paraglide(c, T, a, d);
    if (lr != kGlideCall) return;
    static const bool on = [] {
        const char *e = getenv("TOTK_GLIDE_EASE");
        const bool v = !e || strcmp(e, "0");
        printf("[glide-ease] Cucco glide velocity %s\n", v ? "eased in game time" : "eased per frame, as the game (TOTK_GLIDE_EASE=0)");
        return v;
    }();
    const u64 fd = totk_frame_delta();
    if (!on || !totk_main_known(0x850d7c) || !totk_ptr_ok(fd) || !totk_ptr_ok(node) || ld64(node) != kGlideVt ||
        !totk_ptr_ok(fp - 0x20))
        return;   // (x21 not the glide node: a codegen change; leave the original rather than write another object)
    // r0 as the original read it: *VFRMgr::getCounter(the VFRMgr), a pure lookup (f_850d7c; 1.4.3 main+0x3e6f6c,
    // pinned) that returns the object itself, whose +0 is the VFR delta, unless an alternative per-core counter is on:
    // then leave the original (the object's own layout differs by version; the call keeps the hook out of it)
    const V128 s0 = c->v[0];
    c->x[0] = fd;
    f_850d7c(c);
    const u64 counter = c->x[0];
    c->v[0] = s0;
    if (counter != fd) return;
    float dir[3], v[3];
    for (int i = 0; i < 3; i++) dir[i] = ldf32(fp - 0x20 + 4 * i), v[i] = ldf32(node + kVel + 4 * i);
    float s = c->v[0].f[0];
    if (!totk_glide_ease_step(&s, T, a, d, totk_frame_ratio(fd), ldf32(counter), dir, v)) return;
    for (int i = 0; i < 3; i++) stf(node + kVel + 4 * i, v[i]);
    c->v[0] = vS(s);
}
