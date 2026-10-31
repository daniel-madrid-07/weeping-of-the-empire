// The mouse pointer's confinement (pc_mouse.cpp): when the game confines it to its client area, and when a check finds
// the confinement Windows has must be redone or given back. Pure, no Windows: tools/engine/test_pc_clip.py tests it.
//
// The rule: while the game window is in front and not minimised, the pointer is confined in EVERY context, the menus,
// the map, the quick menus, Ultrahand, the settings overlay and a camera that is off included, so it can never reach
// another monitor. Windowed too: there once the window is armed, activated by the keyboard (Alt+Tab, the taskbar) or
// clicked inside its client area, so a click-activation on the title bar or a border stays a drag or a resize; and
// never during Windows' move / size loop (a title-bar drag, Alt+Space's Move / Size). Losing the focus (Alt+Tab, the
// Windows key, a click elsewhere that Windows allowed) and minimising always give it back. Windows drops a confinement
// on its own, so pc_mouse.cpp redoes it at every raw mouse event, and keeps a captured pointer near the centre
// (recentre below).
#pragma once

namespace pcclip {

struct State {
    bool foreground;   // the game window is the foreground window
    bool iconic;       // minimised (exclusive fullscreen on focus loss)
    bool fullscreen;   // borderless or exclusive
    bool captured;     // the camera has the pointer (hidden)
    bool armed;        // windowed: activated by the keyboard or clicked inside the client area since the activation
    bool moving;       // in Windows' move / size loop (WM_ENTERSIZEMOVE .. WM_EXITSIZEMOVE)
};
inline bool want(const State &s) {
    return s.foreground && !s.iconic && !s.moving && (s.fullscreen || s.captured || s.armed);
}

struct Rect { long l, t, r, b; };
inline bool same(const Rect &a, const Rect &b) { return a.l == b.l && a.t == b.t && a.r == b.r && a.b == b.b; }

enum Act { KEEP, CLIP, FREE };
// A check (every input read, and at each event that moves the window): want_r is the client area as Windows would keep
// it (cut to the virtual screen), have_r what GetClipCursor reports now, ours whether the last ClipCursor was the game's.
// CLIP: confine (again); FREE: give back a confinement the game set; KEEP: as it is (not ours to free: another program's).
inline Act check(bool wanted, const Rect &want_r, const Rect &have_r, bool ours) {
    if (wanted) return ours && same(want_r, have_r) ? KEEP : CLIP;
    return ours ? FREE : KEEP;
}

// Captured (the pointer hidden, the camera reads raw counts): the pointer is put back at the client centre once it is
// over a quarter of the client's width / height away from it, so it never rests at an edge. A confinement Windows drops
// between two mouse events then finds it far from every edge. The raw counts do not see the move.
inline bool recentre(const Rect &client, long x, long y) {
    const long cx = (client.l + client.r) / 2, cy = (client.t + client.b) / 2;
    const long qx = (client.r - client.l) / 4, qy = (client.b - client.t) / 4;
    return x < cx - qx || x > cx + qx || y < cy - qy || y > cy + qy;
}

}  // namespace pcclip
