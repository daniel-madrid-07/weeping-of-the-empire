// The device the prompts follow with controls.glyphs = auto (hle_hid.cpp host_input) and the mouse pointer's "on purpose"
// test (hle_hid.cpp mouse_motion_note, pc_mouse.cpp's released pointer): what counts as using a device, and the guards
// that keep noise and two devices used together from flipping the prompts back and forth. Header only, no Windows:
// tools/engine/test_device_switch.cpp tests it.
#pragma once
#include <cstdlib>

namespace pcdev {

// A stick counts as used when it goes past a third of its travel (kOut) from rest; it counts again only after coming
// back under kIn. A worn stick resting near kOut, or noise around it, does not re-trigger at every read.
constexpr int kOut = 11000, kIn = 6000;
// held: the stick's state between reads (per device); a: its largest axis now. True on a push.
inline bool stick_push(bool& held, int a) {
    a = std::abs(a);
    if (a > kOut) { const bool p = !held; held = true; return p; }
    if (a < kIn) held = false;
    return false;
}

// The mouse moved on purpose: over kCounts counts within kWindowMs of its first count. A bumped desk or a sensor's drift
// (a few counts now and then, however long) never adds up to it; a slow deliberate move (160 counts/s) does.
constexpr double kCounts = 40, kWindowMs = 250;
struct MotionGate {
    double sum = 0, t0 = -1e18;
    bool add(double counts, double t_ms) {
        if (counts <= 0) return false;
        if (t_ms - t0 > kWindowMs) { sum = 0; t0 = t_ms; }
        if ((sum += counts) <= kCounts) return false;
        sum = 0; t0 = -1e18;
        return true;
    }
};

// The style shown (cur, -1 none yet): the last device used, at once, unless the previous change is under kDwellMs old.
// Then the newcomer waits (pending) and takes over when the dwell ends if nothing switched back meanwhile, so two devices
// used together (a pad's stick and the mouse camera) change the prompts at most twice a second instead of every read.
constexpr double kDwellMs = 500;
struct DeviceSwitch {
    int cur = -1, pending = -1;
    double flip = -1e18;
    // before note / settle: the style shown now, which something else may have moved (a pad unplugged). An outside
    // change drops what was waiting (it may be the pad that is gone) and counts as a change for the dwell.
    void sync(int shown, double t_ms) {
        if (shown == cur) return;
        cur = shown; pending = -1; flip = t_ms;
    }
    // a use of style at t; true when cur changed
    bool note(int style, double t_ms) {
        if (style == cur) { pending = -1; return false; }
        if (cur >= 0 && t_ms - flip < kDwellMs) { pending = style; return false; }
        cur = style; pending = -1; flip = t_ms;
        return true;
    }
    // every read: a waiting device takes over once the dwell is over; true when cur changed
    bool settle(double t_ms) {
        if (pending < 0 || t_ms - flip < kDwellMs) return false;
        cur = pending; pending = -1; flip = t_ms;
        return true;
    }
};

}  // namespace pcdev
