// totk_swim_ease.cpp: Link's swim velocity keeps the Switch's ease at any frame rate.
//
// The swim-move physics (main+0x1d259f4: an ExecutePlayerSwimMove method its updateImpl_ calls once per FRAME) eases
// Link's swim velocity, node +0x24/+0x28/+0x2c, toward the stick's target once per call with no dt:
//   s = s9 (the stick length x a state factor) * main+0x16e4f90(the actor)   (a speed getter: no per-call ramp)
//   v += (s * dir - v) * 0.175        (main+0x1d25ac4..0x1d25b1c; dir at the caller's sp + 0x30; 0.175 at main+0x377671c)
// so the velocity settles (tau) in 170 ms at 30 fps and 85 / 43 / 21 ms at 60 / 120 / 240: swim starts, stops and
// turns snap.
//
// The hook (the getter, only at that call: lr main+0x1d25ac0, x21 an ExecutePlayerSwimMove node; every other caller
// keeps the original) pre-sets v = t + (v - t) * q / (1 - k), q = (1 - k)^r (r: the frame's 30 Hz ratio), t = s * dir as
// the caller computes it, so the caller's own step lands on t + (v - t) * q: the 30 Hz ease in game time. At 30 fps and
// below (r >= 1) the original exactly. gen.py --hook 16e4f90 in GEN_MAIN; TOTK_SWIM_EASE=0: the original alone.
// tools/oracle/test_swim_ease.py; games/totk/tests/verify_swim_ease.py runs the caller's own instructions.
#include "rt.h"
#include "totk_globals.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
constexpr u64 kSwimCall = totk_main(0x1d25ac0);   // return address of the swim-move physics' getter call
constexpr u64 kSwimVt = totk_main(0x43822a8);     // ExecutePlayerSwimMove's vtable (slot 1 getRuntimeTypeInfo 0x1d25e30,
                                                  // 26 updateImpl_ 0x1d258ec); not placed in 1.4.3: the call site alone
constexpr u64 kEaseK = totk_main(0x377671c);      // f32 0.175: the caller's per-call factor
constexpr u64 kVel = 0x24;                        // node: the eased velocity (3 floats)
constexpr u64 kDir = 0x30;                        // the caller's sp: the stick direction (3 floats)

void stf(u64 a, float v) { u32 w; memcpy(&w, &v, 4); st32(a, w); }
}  // namespace

// The fix for one call (tools/oracle/test_swim_ease.py drives it through the hook). s: the target speed as the caller
// makes it (s9 x the getter's result), k: its factor, r: the frame's 30 Hz ratio, dir: the stick direction, v: the
// node's velocity. Returns 0 where the original stays (v untouched), else 1 with v pre-set.
extern "C" int totk_swim_ease_step(float s, float k, float r, const float dir[3], float v[3]) {
    if (!(r > 0.01f && r < 1.0f) || !(k > 0.0f && k < 1.0f)) return 0;
    const double keep = pow(1.0 - (double)k, (double)r) / (1.0 - (double)k);   // (1-k)^(r-1): the caller's step x (1-k)
    for (int i = 0; i < 3; i++) {
        const double t = (double)(dir[i] * s);   // (the caller's float product)
        v[i] = (float)(t + ((double)v[i] - t) * keep);
    }
    return 1;
}

#ifdef _WIN32
void f_16e4f90_orig(Ctx *__restrict c);
void swim_ease_orig_stub(Ctx *__restrict) {}
#pragma comment(linker, "/alternatename:?f_16e4f90_orig@@YAXPEIAUCtx@@@Z=?swim_ease_orig_stub@@YAXPEIAUCtx@@@Z")
#else
__attribute__((weak)) void f_16e4f90_orig(Ctx *__restrict c);
#endif
void hook_f_16e4f90(Ctx *__restrict c) {   // the actor's swim speed getter(this) -> s0
#ifndef _WIN32
    if (!f_16e4f90_orig) return;
#endif
    const u64 lr = c->x[30], node = c->x[21], sp = c->sp;   // (the caller's x21 = its node, sp + 0x30 = the direction)
    const float s9 = c->v[9].f[0];                          // (the caller's stick x state factor, callee-saved)
    f_16e4f90_orig(c);
    if (lr != kSwimCall) return;
    static const bool on = [] {
        const char *e = getenv("TOTK_SWIM_EASE");
        const bool v = !e || strcmp(e, "0");
        printf("[swim-ease] swim velocity %s\n", v ? "eased in game time" : "eased per frame, as the game (TOTK_SWIM_EASE=0)");
        return v;
    }();
    const u64 fd = totk_frame_delta();
    // (another version: the factor must be placed; the vtable may not be, then the call site alone: its x21 is the node)
    if (!on || !totk_main_known(0x377671c) || !totk_ptr_ok(fd) || !totk_ptr_ok(node) ||
        (totk_main_known(0x43822a8) && ld64(node) != kSwimVt) || !totk_ptr_ok(sp + kDir))
        return;
    float dir[3], v[3];
    for (int i = 0; i < 3; i++) dir[i] = ldf32(sp + kDir + 4 * i), v[i] = ldf32(node + kVel + 4 * i);
    const float s = s9 * c->v[0].f[0];   // (fmul s0, s9, s0 at main+0x1d25ac4)
    if (!totk_swim_ease_step(s, ldf32(kEaseK), totk_frame_ratio(fd), dir, v)) return;
    for (int i = 0; i < 3; i++) stf(node + kVel + 4 * i, v[i]);
}
