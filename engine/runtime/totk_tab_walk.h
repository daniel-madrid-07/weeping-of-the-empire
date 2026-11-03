// totk_tab_walk.h: the menu cursor's tab walk (totk_menu_cursor.cpp walk_step), L / R presses until a tab bar shows
// the tab asked for. Header-only and free of game reads, so tools/engine/test_tab_walk.py drives it with made-up tab
// readings; the caller reads the tab, the focus and the menu screen.
#pragma once
#include <stdint.h>

// kind 0: the pause menu's pouch tabs (PouchMgr's tab); kind 1: the Purah Pad's pages (the context's page, -1 through a
// page switch's focus gap). One press (up to 50 ms, then 50 ms released: over a frame each at 30 fps; a tab bar steps
// one valid tab per press, on its trigger frame), the next as soon as the tab has changed. It stops at the tab, after
// 12 presses or 4 s, when the tab bar is gone, or when two presses in a row changed nothing in 400 ms each (an invalid
// tab: greyed, the game skips it; at a low frame rate a 50 ms press can fall between two samples, hence one retry). The
// pouch's tabs do not wrap. A press ends early once the game has taken it: the tab moved, or a read went out without it
// (gap(): a tab switch's one-frame focus gap, which the caller does not walk through) or during TMS_TRANSITION. The
// game sees L / R released there, so pressing on is a second press and a second step: the 11-02 15:07 log, at 60 fps
// the 50 ms press spanned the switch's gap, walked 1, 2, 3, 4 for tab 3 and hunted 4, 3, 2, 1, 2, 3, 4 until its 12
// presses ran out. No press starts during a transition either. A walk that passes its tab twice stops (not reached):
// the tab is greyed or hidden, the game skips it, and without this the walk went on hunting L, R, L, R until its 12
// presses ran out (the 11-02 v0.1.7 report "L / R spam on Esc": Esc opens the pause menu and walks to the last tab).
// One pass is gone back over (a press counted twice).
struct TabWalk {
    int to = -1, kind = 0, phase = 0, presses = 0, last = -1, dead = 0, dir = 0, passes = 0;
    unsigned long long at_ms = 0, t = 0, press_at = 0;

    void start(int kind_, int to_, int cur, unsigned long long now) {
        *this = TabWalk{};
        to = to_; kind = kind_; at_ms = now; last = cur;
    }
    // A read of the pad went out without this walk's press (the caller returned before step())
    void gap(unsigned long long now) {
        if (to >= 0 && phase == 1) { phase = 2; t = now; }
    }
    // This read's L / R bits (HidNpadButton L 6 / R 7). cur: the tab now (-1: none); there: the tab bar's screen is
    // still open; transition: the menu screen reads TMS_TRANSITION. *ended (if given) is set when the walk ends here,
    // and reached tells whether it ended at the tab.
    uint64_t step(int cur, bool there, bool transition, unsigned long long now, bool* ended = nullptr,
                  bool* reached = nullptr) {
        if (to < 0) return 0;
        // (the Purah Pad's page reads -1 in a page switch's focus gap: wait, within the time limits)
        const bool unknown = cur < 0 && kind == 1;
        bool waiting = phase == 0 && presses > 0 && (cur == last || unknown);
        if (waiting && !unknown && now - press_at >= 400 && dead < 1) { dead++; waiting = false; }
        else if (!waiting && phase == 0) dead = 0;
        if ((cur < 0 && !unknown) || !there || now - at_ms > 4000 || presses > 12 ||
            (waiting && now - press_at >= (unknown ? 1000u : 400u))) {
            if (ended) *ended = true;
            if (reached) *reached = cur == to;
            to = -1;
            return 0;
        }
        if (phase == 0 && presses > 0 && cur >= 0 && cur != last && (dir > 0 ? cur > to : cur < to) && ++passes >= 2) {
            if (ended) *ended = true;
            if (reached) *reached = false;
            to = -1;
            return 0;
        }
        if (cur == to && phase != 1) {
            if (ended) *ended = true;
            if (reached) *reached = true;
            to = -1;
            return 0;
        }
        if (phase == 0 && !waiting && !transition) { last = cur; dir = to > cur ? 1 : -1; phase = 1; t = press_at = now; presses++; }
        if (phase == 1 && (cur != last || transition || now - t >= 50)) { phase = 2; t = now; }
        if (phase == 1) return to > last ? (1u << 7) : (1u << 6);
        if (phase == 2 && now - t >= 50) phase = 0;
        return 0;
    }
};
