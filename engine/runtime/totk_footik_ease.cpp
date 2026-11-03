// totk_footik_ease.cpp: the foot IK's ground height eases at the Switch's speed at any frame rate (fps360's per-frame
// sweep, found by rival-034).
//
// phive's foot IK graphics controller (main+0x9a396c, from GraphicsControllerSet::preUpdateMatrix main+0xcdea74 <-
// PhysicsComponent::calcGraphics: once per FRAME for every actor with a FootIKParam, Link first) takes the lowest /
// highest foot ray hit over its feet, then eases the component's ground height +0x70 toward it once per call, no dt:
//   bl main+0x9a7130 (FootIKParam's GroundPositionLerpFacror getter: &param + 0xf8) ; ldr s0, [x0]   (main+0x9a5a10)
//   [x11+0x70] += (target - [x11+0x70]) * s0                                                          (main+0x9a5ad0..e0)
// Link's factor is 0.5 (Phive/FootIKParam/Player): 90 % of a step's height in 110 ms at 30 fps, 55 / 28 / 14 ms at
// 60 / 120 / 240: the feet and pelvis snap onto stairs and slopes instead of the Switch's short settle.
//
// The hook (the getter, only at that call, return address main+0x9a5a14: every other read of the parameter keeps the
// original) points the caller at k' = 1 - (1 - k)^r, r the frame's 30 Hz ratio (VFRMgr: real time, as the Switch's one
// call per 1/30 s), so the ease covers (1 - k) of the way per 30 Hz frame of game time at any rate. At 30 fps and below
// (r >= 1) the original exactly (the caller reads the parameter itself). gen.py --hook 9a7130 in GEN_MAIN;
// TOTK_FOOTIK_EASE=0: the original alone. tools/oracle/test_footik_ease.py; games/totk/tests/verify_footik_ease.py
// checks the caller's words and that the factor reaches the ease untouched.
#include "rt.h"
#include "totk_globals.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
constexpr u64 kFootCall = totk_main(0x9a5a14);   // return address of the foot IK's getter call (then ldr s0, [x0])
// k' for the caller to read: guest addresses are host addresses (rt.h ld32; precedent totk_ai_integrators.cpp), one per
// thread (the graphics controllers may run on more than one worker: each call reads its own right after the getter)
alignas(16) thread_local float t_k;
}  // namespace

// The per-call factor for a frame of 30 Hz ratio r (tools/oracle/test_footik_ease.py). k itself where the original
// stays: r >= 1 (30 fps and below) or out of range, k outside (0, 1).
extern "C" float totk_footik_ease_k(float k, float r) {
    if (!(r > 0.01f && r < 1.0f) || !(k > 0.0f && k < 1.0f)) return k;
    return (float)(1.0 - pow(1.0 - (double)k, (double)r));
}

#ifdef _WIN32
void f_9a7130_orig(Ctx *__restrict c);
void footik_ease_orig_stub(Ctx *__restrict) {}
#pragma comment(linker, "/alternatename:?f_9a7130_orig@@YAXPEIAUCtx@@@Z=?footik_ease_orig_stub@@YAXPEIAUCtx@@@Z")
#else
__attribute__((weak)) void f_9a7130_orig(Ctx *__restrict c);
#endif
void hook_f_9a7130(Ctx *__restrict c) {   // FootIKParam's GroundPositionLerpFacror getter(this) -> x0 = &value
#ifndef _WIN32
    if (!f_9a7130_orig) return;
#endif
    const u64 lr = c->x[30];
    f_9a7130_orig(c);
    if (lr != kFootCall) return;
    static const bool on = [] {
        const char *e = getenv("TOTK_FOOTIK_EASE");
        const bool v = !e || strcmp(e, "0");
        printf("[footik-ease] foot IK ground height ease %s\n", v ? "in game time" : "per frame, as the game (TOTK_FOOTIK_EASE=0)");
        return v;
    }();
    const u64 fd = totk_frame_delta(), p = c->x[0];
    if (!on || !totk_ptr_ok(fd) || !totk_ptr_ok(p)) return;
    const float k = ldf32(p), k2 = totk_footik_ease_k(k, totk_frame_ratio(fd));
    if (k2 == k) return;
    t_k = k2;
    c->x[0] = (u64)(uintptr_t)&t_k;
}
