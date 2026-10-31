// totk_roll_input.cpp: spinning the left stick counts at the Switch's angular speed at any frame rate.
//
// ExecutePlayerCalcStick (main+0x1cd0260, x0 = this) keeps the stick's roll: +0xe0 the stick's angle last frame, +0xe8
// the summed angle (PlayerStickRoll..., to the blackboard value at *(this + 0x78); |sum| >= 6.0737 = a full turn, the
// flag at *(this + 0xb8)), +0xec a VFRCounter (value, previous, step) that is the grace time. Each frame, with the
// stick's length >= 0.95, a change d of the angle (wrapped to +-pi) with 0.16755 < |d| < pi/2 is summed (same sign,
// clamped to +-6.0737) and resets the grace to 4.0; a change of ANY size against the sum's sign restarts the sum at d;
// any other frame counts the grace down by the frame delta (time-based) and, once it is spent, zeroes the sum. The
// lower bound is per FRAME: the slowest spin that counts is 0.16755 x 30 = 5.0 rad/s at 30 fps but 24 rad/s at 144 and
// 40 at 240, so at high rates a normal spin never adds up (main+0x1cd0948..0x1cd09b0, 0x1cd05cc..0x1cd05e4).
//
// The hook runs the original, then redoes the one frame the original rejected and the Switch would have counted: the
// lower bound becomes 0.16755 r (r = the frame's 30 Hz ratio, the frame-delta object's +8, at most 1), so the slowest
// counted spin is 5.0 rad/s at every rate, and the sum, the grace and the two outputs are written as the original's
// accept path writes them. The upper bound stays pi/2 per frame (a jump across the stick's centre, not a speed). At
// 30 fps and below (r >= 1) the band is empty: the original exactly. Not redone: the flick output (*(this + 0xa8):
// |angle - anchor +0xe4| > pi/6, |camera difference| > 2.094, |sum| < 2.094, length >= 0.05; 0x1cd0710 / 0x9b4): it
// is cleared when the redone sum reaches 2.094; a redone frame never lowers |sum| (an opposite change restarts it at d
// in the original too), so it never has to be set.
// gen.py --hook 1cd0260 in GEN_MAIN; TOTK_ROLL_INPUT=0: the original alone. tools/oracle/test_roll_input.py.
#include "rt.h"
#include "totk_globals.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
constexpr u64 kFrameDelta = 0x45577c0;   // GOT slot of the frame-delta setter's object (+8: 30 Hz ratio)
// the original's constants, bit-exact (main+0x3776234 / 0x37762f0 / 0x3776360 / 0x3776274 / 0x37761b4, 0x3775b84 /
// 0x3775f60)
constexpr u32 kMinBits = 0x3e2b92a7, kCapBits = 0x40c25c20, kFlickBits = 0x40060a92, kFullBits = 0x3f733333,
              kPiBits = 0x40490fdb, kTwoPiBits = 0x40c90fdb;

float bits(u32 b) { float f; memcpy(&f, &b, 4); return f; }
void stf(u64 a, float v) { u32 w; memcpy(&w, &v, 4); st32(a, w); }
bool addr_ok(u64 p) { return p >= 0x10000 && p < (1ull << 47); }   // a blackboard value (4- or 1-byte aligned)
}  // namespace

// The roll sum after a counted change d (the original's main+0x1cd096c..0x1cd0998).
extern "C" float totk_roll_input_sum(float acc, float d) {
    const float cap = bits(kCapBits);
    if ((acc >= 0 && d >= 0) || (acc <= 0 && d < 0)) {
        const float s = d + acc;
        return s < -cap ? -cap : fminf(s, cap);
    }
    return d;   // a reversal: the sum restarts at this frame's change
}

// Whether the original rejected this frame's change and the hook counts it: the stick's length len >= 0.95 and the
// wrapped change between 0.16755 r and 0.16755 (r = the 30 Hz ratio). *d_out: the change.
extern "C" int totk_roll_input_redo(float prev, float cur, float len, float r, float *d_out) {
    if (!(r > 0.01f) || !(r < 1.0f) || !(len >= bits(kFullBits))) return 0;
    float d = cur - prev;
    const float ad = fabsf(d);
    if (ad > bits(kPiBits)) d = d > 0 ? -(bits(kTwoPiBits) - ad) : bits(kTwoPiBits) - ad;
    const float m = fabsf(d), lo = bits(kMinBits);
    *d_out = d;
    return m > lo * r && !(m > lo);
}

#ifdef _WIN32
void f_1cd0260_orig(Ctx *__restrict c);
void rollinput_orig_stub(Ctx *__restrict) {}
#pragma comment(linker, "/alternatename:?f_1cd0260_orig@@YAXPEIAUCtx@@@Z=?rollinput_orig_stub@@YAXPEIAUCtx@@@Z")
#else
__attribute__((weak)) void f_1cd0260_orig(Ctx *__restrict c);
#endif
void hook_f_1cd0260(Ctx *__restrict c) {   // ExecutePlayerCalcStick's calc (this)
#ifndef _WIN32
    if (!f_1cd0260_orig) return;
#endif
    const u64 self = c->x[0];
    static const bool on = [] {
        const char *e = getenv("TOTK_ROLL_INPUT");
        const bool v = !e || strcmp(e, "0");
        printf("[roll-input] stick spin %s\n", v ? "at the Switch's angular speed" : "per frame, as the game (TOTK_ROLL_INPUT=0)");
        return v;
    }();
    const bool ok = on && totk_ptr_ok(self);
    const u32 prev = ok ? ld32(self + 0xe0) : 0;
    const float acc0 = ok ? ldf32(self + 0xe8) : 0;
    f_1cd0260_orig(c);
    if (!ok) return;
    const u32 cur = ld32(self + 0xe0);
    if (cur == prev) return;   // the roll was not evaluated (no stick this frame) or did not move
    const u64 fd = totk_global(kFrameDelta), len_p = ld64(self + 0x58);
    float d;
    if (!totk_ptr_ok(fd) || !addr_ok(len_p) ||
        !totk_roll_input_redo(bits(prev), bits(cur), ldf32(len_p), ldf32(fd + 8), &d))
        return;
    const float acc = totk_roll_input_sum(acc0, d);
    stf(self + 0xe8, acc);
    stf(self + 0xec, 4.0f), stf(self + 0xf0, 4.0f), stf(self + 0xf4, -1.0f);   // the grace, as the accept path sets it
    const u64 sum_p = ld64(self + 0x78), turn_p = ld64(self + 0xb8), flick_p = ld64(self + 0xa8);
    if (addr_ok(sum_p)) stf(sum_p, acc);
    if (addr_ok(turn_p)) st8(turn_p, fabsf(acc) >= bits(kCapBits));
    if (addr_ok(flick_p) && fabsf(acc) >= bits(kFlickBits)) st8(flick_p, 0);
    static int logs;
    if (logs < 3 && ++logs) printf("[roll-input] counted a %.3f rad change at ratio %.3f (sum %.2f)\n", d, ldf32(fd + 8), acc);
}
