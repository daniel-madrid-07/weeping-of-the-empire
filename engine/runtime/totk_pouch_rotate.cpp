// totk_pouch_rotate.cpp: the inventory screen's Link model turns at the Switch's speed at any frame rate.
//
// PouchPlayer.direction.module.ainb turns the model in rotate mode with CurrentRotY += UI stick x 7 degrees per update
// (QueryControllerGetStick "UI" -> GetElements -> QueryMathMulF32 x 7 -> QueryMathAddF32 -> ExecuteBlackboardSetF32;
// none of them has VFR): 210 deg/s at full tilt at 30 fps, about 1000 at 144, 1680 at 240, on a pad and with the mouse
// drag (it feeds the same stick).
//
// QueryControllerGetStick's worker (main+0x1f61d6c, x0 = the query; from its enterImpl_ and updateImpl_) copies the
// controller's sticks into the query (+0x28 LeftStick from controller +0x120, +0x20 RightStick from +0x128) and its
// outputs point at those fields, so a consumer reads them after the call. The hook scales both by the frame's 30 Hz
// ratio r when the query's ControllerUserName (+0x38) is "UI": the rotation is then 7 x r degrees per update, 210
// deg/s at every rate. At 30 fps and below (r >= 1) the original exactly. gen.py --hook 1f61d6c in GEN_MAIN;
// TOTK_POUCH_ROTATE=0: the original alone. tools/oracle/test_pouch_rotate.py.
#include "rt.h"
#include "totk_globals.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
constexpr u64 kFrameDelta = 0x45577c0;   // GOT slot of the frame-delta setter's object (+8: 30 Hz ratio)
constexpr u64 kRight = 0x20, kLeft = 0x28, kUserName = 0x38;

void stf(u64 a, float v) { u32 w; memcpy(&w, &v, 4); st32(a, w); }

bool is_ui(u64 q) {   // ControllerUserName: a tagged pointer to the parameter's const char *
    const u64 slot = ld64(q + kUserName) & ~3ull;
    if (!totk_ptr_ok(slot)) return false;
    const u64 s = ld64(slot);
    return totk_ptr_ok(s) && ld8(s) == 'U' && ld8(s + 1) == 'I' && ld8(s + 2) == 0;
}
}  // namespace

#ifdef _WIN32
void f_1f61d6c_orig(Ctx *__restrict c);
void pouchrotate_orig_stub(Ctx *__restrict) {}
#pragma comment(linker, "/alternatename:?f_1f61d6c_orig@@YAXPEIAUCtx@@@Z=?pouchrotate_orig_stub@@YAXPEIAUCtx@@@Z")
#else
__attribute__((weak)) void f_1f61d6c_orig(Ctx *__restrict c);
#endif
void hook_f_1f61d6c(Ctx *__restrict c) {   // QueryControllerGetStick's worker (this) -> w0 = the controller was found
#ifndef _WIN32
    if (!f_1f61d6c_orig) return;
#endif
    const u64 q = c->x[0];
    f_1f61d6c_orig(c);
    static const bool on = [] {
        const char *e = getenv("TOTK_POUCH_ROTATE");
        const bool v = !e || strcmp(e, "0");
        printf("[pouch-rotate] UI stick in AI programs %s\n", v ? "per 30 Hz of game time" : "per frame, as the game (TOTK_POUCH_ROTATE=0)");
        return v;
    }();
    const u64 fd = totk_global(kFrameDelta);
    if (!on || !totk_ptr_ok(q) || !totk_ptr_ok(fd)) return;
    const float r = ldf32(fd + 8);
    if (!(r > 0.01f && r < 1.0f) || !is_ui(q)) return;
    for (u64 o : {kRight, kLeft, kRight + 4, kLeft + 4}) stf(q + o, ldf32(q + o) * r);
}
