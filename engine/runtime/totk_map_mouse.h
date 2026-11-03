// totk_map_mouse.h: the map's view centre from the mouse's motion, like a desktop pointer. TotK's map cursor is the view
// centre (UIMapScreen +0x2c8 / +0x2cc, the target the view eases to; the reticle stays at the screen's centre), so the
// map under the reticle moves by what the mouse moved, in screen pixels: one count = one screen pixel at sensitivity 1
// (Windows' pointer without acceleration). The screen pixels become the 1920 x 1080 layout's (the map is drawn into it,
// fitted to the window, letterboxed on another aspect), and those world units: the layout draws 3 x rate pixels per world
// unit at the zoom's current (eased) rate: doDraw_'s pane transform 0x10010cf53c and the update's clamp 5000 - 540 / (3
// rate) agree). Screen right = +X at +0x2c8; screen UP = + at +0x2cc. Counts in, world units out: the same total however
// many frames it is split over (no fps term). Header only (tools/engine/test_map_mouse.py); totk_map_speed.cpp applies it
// in UIMapScreen's per-frame move.
#pragma once

// The world step for cx / cy counts at sensitivity sens, the window's client area cw x ch and the zoom rate; false
// (nothing to step) when an input is out of range.
inline bool totk_map_mouse_step(double cx, double cy, double sens, int cw, int ch, float rate, float* dx, float* dz) {
    if (cw <= 0 || ch <= 0 || !(sens > 0) || !(rate > 0.005f && rate < 20.0f)) return false;
    const double drawn_w = cw * 9.0 < ch * 16.0 ? (double)cw : ch * 16.0 / 9.0;   // the layout's width on screen
    const double units = (1920.0 / drawn_w) / (3.0 * rate);                        // world units per screen pixel
    *dx = (float)(cx * sens * units);
    *dz = (float)(-cy * sens * units);   // (raw counts: + = the mouse toward the player, down the screen)
    return true;
}
// The target kept on Hyrule's maps (sky, surface and depths span about X +-6000, Z +-5000 world units); the game's
// own update clamps it further in near the edges
inline float totk_map_mouse_clamp(float v, float lim) { return v < -lim ? -lim : v > lim ? lim : v; }
