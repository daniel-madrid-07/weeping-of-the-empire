// step_check.h: does the game read the realtime step? The game's frame-delta setter stores ratio = interval / 2
// unpatched, the data page's ratio with the realtime patch; this votes on the two, frame by frame.
// tools/engine/test_step_check.py.
#pragma once
#include <cmath>

struct StepCheck {
    int verdict = 0;   // 1: the game reads the page's step, -1: it does not, 0: not yet known
    int reads = 0, misses = 0;
    // One frame: the game's ratio after its setter, the page's, the present interval it was called with. Only frames whose
    // page ratio is at least 20 % away from interval / 2 count (near the interval's own rate both read alike, and nothing
    // runs fast there); a game ratio within 5 % of the page's is a read (the present thread may have written the page
    // between the setter's read and this one: a small step change). 60 such frames decide: not read in 9 of 10 = -1.
    int vote(float game, float page, unsigned interval) {
        if (verdict) return verdict;
        const float nominal = interval * 0.5f;
        if (interval < 1 || !(page > 0.01f) || std::fabs(page - nominal) < 0.2f * nominal) return 0;
        if (std::fabs(game - page) <= 0.05f * page) reads++;
        else misses++;
        if (reads + misses < 60) return 0;
        verdict = misses * 10 >= (reads + misses) * 9 ? -1 : 1;
        return verdict;
    }
};
