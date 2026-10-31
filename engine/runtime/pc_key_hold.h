// The keyboard move keys' minimum hold (hle_hid.cpp keyboard_read, the left stick from WASD). Pure, no Windows:
// tools/engine/test_pc_key_hold.py tests it.
//
// Why: Wait -> Walk starts on the first frame with the stick past 0.05, and Link's motion on foot is the start clip's
// root motion. A key gives a square full-deflection pulse of 1-3 frames, which ends before the clip's first footfall;
// Wait then fades to standing (no stepping stop under 1 m/s) while the root motion still pushes him: a slide with idle
// legs. A thumb can't flick a stick that short, so the Switch never shows it. The rule: per axis, a key's press holds
// that direction for at least min_ms (default 150, TOTK_KEY_MIN_HOLD_MS, 0 = off) even if released sooner; the opposite
// key ends the hold at once (the new direction wins), a re-press of the same key starts it again (spam = one continuous
// deflection, the legs walk). The magnitude is the walk key's as it was while a key was last down (no ramp: a lower
// stick length starts a slower walk). Time-based: the same at any fps.
#pragma once
#include <cstdint>

namespace pckh {

struct Axis {
    bool neg = false, pos = false;   // the axis' two keys at the last step
    int out = 0;                     // the direction given: the newest press, held past its release until t + min_ms
    uint64_t t = 0;                  // that press, ms
};

// One axis at now_ms from its two keys (neg: A / S, pos: D / W): the direction the stick gets. The newest press wins,
// with both keys down too (W -> S rolled with an overlap turns at S's press); a key that was pressed and released
// within min_ms keeps its direction until then, over the other key still held.
inline int step(Axis &a, bool neg, bool pos, uint64_t now_ms, unsigned min_ms) {
    const bool en = neg && !a.neg, ep = pos && !a.pos;   // press edges
    a.neg = neg; a.pos = pos;
    if (!min_ms) { a.out = 0; return (int)pos - (int)neg; }   // off: as before (both keys = 0)
    if (en != ep) { a.out = ep ? 1 : -1; a.t = now_ms; }
    else if (en) { a.out = 0; a.t = 0; return 0; }           // (both pressed in the same read: as before, 0)
    const int down = (int)pos - (int)neg;                     // one key down: its direction; both or none: 0
    if (neg && pos) return a.out;                             // the newer of the two
    if (a.out && a.out != down && now_ms - a.t < min_ms) return a.out;   // a tap's hold (over the other key or none)
    a.out = down;
    return down;
}

struct Hold {
    Axis x, y;
    float walk = 1.0f;   // the magnitude while only holds are left
};

// Both axes, from the four keys: *x / *y out, the directions; *walk in the walk factor now, out the one to use.
inline void move(Hold &h, bool left, bool right, bool down, bool up, int *x, int *y, float *walk, uint64_t now_ms,
                 unsigned min_ms) {
    const bool keys = left || right || down || up;
    if (keys) h.walk = *walk;
    *x = step(h.x, left, right, now_ms, min_ms);
    *y = step(h.y, down, up, now_ms, min_ms);
    if (!keys && (*x || *y)) *walk = h.walk;   // (only holds left)
}

// A menu, the map or the overlay takes the keys, or the window lost the focus: no stick is held into it.
inline void cancel(Hold &h) { h.x = {}; h.y = {}; }

}  // namespace pckh
