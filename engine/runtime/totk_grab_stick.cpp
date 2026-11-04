// totk_grab_stick.cpp: Ultrahand moves a held object as far as the mouse moved (v0.1.10: "while holding an
// object the mouse sensitivity drops a lot, moving things takes ages").
//
// ExecutePlayerUltraHand::updateUltraHand (main+0x1d28e94) reads two analog actions through the input mapper (main+0xaf69b0,
// hashes 0x8c5522fc / 0xdd849f22), negates each per the invert options, and copies them with the rest of a 0x28-byte block
// to the UltraHand object at +0x11c (X) / +0x120 (Y). UltraHand::calcGrab (main+0x172d1bc, x0 = that object, later in the
// frame) curves each axis, sign(v) |v|^p with p a MoveSpeed parameter (getters main+0x171323c for X at lr 0x172d6d4,
// main+0x1713290 for Y at lr 0x172d6f0; powf at 0x172d730 / 0x172d75c), and moves the held object by its MoveSpeeds x dt x
// those values, clamped to the game's height range. The values are a stick: a pad at full tilt is the top speed, and so is
// pc_mouse's right-stick emulation, which is clamped to 1 like a pad.
//
// pc_mouse writes the mouse's grab stick UNCLAMPED to the data page while Ultrahand holds with the mouse captured: TOTK_DP_GRAB_X / _Y (f32, a
// stick: may exceed 1) and TOTK_DP_GRAB_LIVE (the TOTK_DP_PRESENTS it was written for). This hook, around calcGrab, puts that pair at +0x11c /
// +0x120 for the call and the game's values back after it, when LIVE is this present or the one before: calcGrab then moves the object as far
// as the hand moved, at any rate (its own dt), and the camera keeps the clamped stick. The game applies the invert options before +0x11c; the
// page holds the stick before them (as a pad would be), so each axis's sign relation is learnt from the game's own value of the clamped
// emulation when both are clearly non-zero. The two exponents go to the page too (TOTK_DP_GRAB_PX / _PY), so pc_mouse can shape its stick to
// the game's curve, and TOTK_DP_GRAB_ACK = the present of each call that took the pair, so pc_mouse drains its owed counts at the unclamped
// rate only while the hook really consumes them. TOTK_GRAB_STICK=0: the game's values always. tools/oracle/test_grab_stick.py;
// games/totk/tests/verify_grab_stick.py checks calcGrab's words.
#include "rt.h"
#include "totk_globals.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" void* totk_data_page_current(void);   // games/totk/runtime/totk_patches.c: the data page, or NULL

namespace {
constexpr u64 kExpXCall = totk_main(0x172d6d4), kExpYCall = totk_main(0x172d6f0);   // calcGrab's exponent reads
// data_page.h: TOTK_DP_PRESENTS 88, TOTK_DP_GRAB_X 236, _Y 240, _LIVE 244, _PX 248, _PY 252, _ACK 384
constexpr unsigned kPresents = 88, kPageX = 236, kPageY = 240, kPageLive = 244, kPagePx = 248, kPagePy = 252, kPageAck = 384;
constexpr u64 kStickX = 0x11c, kStickY = 0x120;
bool float_ptr_ok(u64 p) { return p >= 0x10000 && p < (1ull << 47) && !(p & 3); }
float page_f32(const uint8_t *page, unsigned off) {
    const u32 b = __atomic_load_n((const u32 *)(page + off), __ATOMIC_ACQUIRE);
    float f;
    memcpy(&f, &b, 4);
    return f;
}
void publish_exp(unsigned off, u64 p) {
    uint8_t *page = (uint8_t *)totk_data_page_current();
    if (page && float_ptr_ok(p)) __atomic_store_n((u32 *)(page + off), ld32(p), __ATOMIC_RELAXED);
}
int g_sign_x, g_sign_y;   // game value = sign x the page's (the invert options), learnt; 0 = not yet: that axis keeps the game's
constexpr float kMaxStick = 4.0f;  // (calcGrab curves |v|^p: the feed past this is cut here as well as in pc_mouse)
}  // namespace

// The sign relation between the page's stick (before the invert options) and the game's (after): +1 / -1 when both are
// clearly non-zero, else the one learnt before, 0 while none was (tools/oracle/test_grab_stick.py).
extern "C" int totk_grab_sign(float game, float page, int was) {
    if (std::fabs(game) < 0.05f || std::fabs(page) < 0.05f) return was;   // (was 0: still unknown)
    return (game < 0) == (page < 0) ? 1 : -1;
}

#ifdef _WIN32
void f_172d1bc_orig(Ctx *__restrict c);
void f_171323c_orig(Ctx *__restrict c);
void f_1713290_orig(Ctx *__restrict c);
void grab_stick_orig_stub(Ctx *__restrict) {}
#pragma comment(linker, "/alternatename:?f_172d1bc_orig@@YAXPEIAUCtx@@@Z=?grab_stick_orig_stub@@YAXPEIAUCtx@@@Z")
#pragma comment(linker, "/alternatename:?f_171323c_orig@@YAXPEIAUCtx@@@Z=?grab_stick_orig_stub@@YAXPEIAUCtx@@@Z")
#pragma comment(linker, "/alternatename:?f_1713290_orig@@YAXPEIAUCtx@@@Z=?grab_stick_orig_stub@@YAXPEIAUCtx@@@Z")
#else
__attribute__((weak)) void f_172d1bc_orig(Ctx *__restrict c);
__attribute__((weak)) void f_171323c_orig(Ctx *__restrict c);
__attribute__((weak)) void f_1713290_orig(Ctx *__restrict c);
#endif

void hook_f_171323c(Ctx *__restrict c) {   // a MoveSpeed parameter's getter(this) -> x0 = &value; at lr 0x172d6d4: X's exponent
#ifndef _WIN32
    if (!f_171323c_orig) return;
#endif
    const u64 lr = c->x[30];
    f_171323c_orig(c);
    if (lr == kExpXCall) publish_exp(kPagePx, c->x[0]);
}

void hook_f_1713290(Ctx *__restrict c) {   // ... at lr 0x172d6f0: Y's exponent
#ifndef _WIN32
    if (!f_1713290_orig) return;
#endif
    const u64 lr = c->x[30];
    f_1713290_orig(c);
    if (lr == kExpYCall) publish_exp(kPagePy, c->x[0]);
}

void hook_f_172d1bc(Ctx *__restrict c) {   // UltraHand::calcGrab(this, ...)
#ifndef _WIN32
    if (!f_172d1bc_orig) return;
#endif
    static const bool on = [] {
        const char *e = getenv("TOTK_GRAB_STICK");
        const bool v = !e || strcmp(e, "0");
        printf("[grab-stick] Ultrahand's held object %s\n", v ? "moved by the mouse's unclamped stick while it drives it"
                                                              : "moved by the game's stick only (TOTK_GRAB_STICK=0)");
        return v;
    }();
    const u64 obj = c->x[0];
    uint8_t *page = (uint8_t *)totk_data_page_current();
    if (!on || !page || !totk_ptr_ok(obj)) { f_172d1bc_orig(c); return; }
    const u32 presents = __atomic_load_n((const u32 *)(page + kPresents), __ATOMIC_ACQUIRE);
    const u32 live = __atomic_load_n((const u32 *)(page + kPageLive), __ATOMIC_ACQUIRE);
    if (!live || presents - live > 1) { f_172d1bc_orig(c); return; }
    const float px = page_f32(page, kPageX), py = page_f32(page, kPageY);
    if (!std::isfinite(px) || !std::isfinite(py)) { f_172d1bc_orig(c); return; }
    const u32 gx = ld32(obj + kStickX), gy = ld32(obj + kStickY);
    float fx, fy;
    memcpy(&fx, &gx, 4), memcpy(&fy, &gy, 4);
    g_sign_x = totk_grab_sign(fx, px, g_sign_x), g_sign_y = totk_grab_sign(fy, py, g_sign_y);
    // (an axis whose invert is not learnt yet keeps the game's value: never a first move the wrong way; and so does an
    // axis the still mouse feeds ~0: a pad stick used meanwhile is not cut off)
    const bool use_x = g_sign_x && std::fabs(px) >= 0.01f, use_y = g_sign_y && std::fabs(py) >= 0.01f;
    if (!use_x && !use_y) { f_172d1bc_orig(c); return; }
    const float wx = use_x ? std::fmax(-kMaxStick, std::fmin(kMaxStick, px)) * g_sign_x : fx;
    const float wy = use_y ? std::fmax(-kMaxStick, std::fmin(kMaxStick, py)) * g_sign_y : fy;
    u32 bx, by;
    memcpy(&bx, &wx, 4), memcpy(&by, &wy, 4);
    st32(obj + kStickX, bx), st32(obj + kStickY, by);
    __atomic_store_n((u32 *)(page + kPageAck), presents, __ATOMIC_RELEASE);   // (pc_mouse: the pair was taken)
    f_172d1bc_orig(c);
    st32(obj + kStickX, gx), st32(obj + kStickY, gy);   // (every other reader: the game's own)
}
