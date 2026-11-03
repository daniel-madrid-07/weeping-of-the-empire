// Automatic frame generation (TOTK_FG_AUTO=1, default off; hle_vi.cpp vi_pace_wait): the generated frames per real one
// picked from the rate the game makes and the rate the panel shows, without clocks or globals, so
// tools/engine/fg_auto_test.cpp runs it on simulated frames.
//
// FG paces the real frames at the panel's rate / (k + 1), so a multiplier only helps while the game makes fewer real
// frames than that. Here k is chosen from the game's own frame time (its work, the pacer's waits left out): R = 1 /
// that, H = the panel's usable rate (the refresh, or VRR's margin under it):
// - k = 0 (no generated frame shown; the real frames at H) when R > 0.55 H: x2 would cap the real frames under R;
// - else k = H / R - 1, at least 1 and at most what was asked (the row's x2 / x3 / x4);
// - a change needs the same k at R +- 10 % (hysteresis) and held for 2 s (a count change restarts FG's history).
// The game's rate is a running mean (1 / 10 a frame) of its frame times under 200 ms (a load or a hitch is no rate).
#pragma once
#include <algorithm>
#include <cstdint>

struct FgAuto {
    static constexpr int64_t kMs = 1000000;
    int k = -1;            // the multiplier in use (-1: none yet)
    int want = -1;         // a different one the rate asks for, and since when
    int64_t want_at = 0;
    double mean_ns = 0;    // the game's frame time (running mean)

    static int pick(double r, int h, int max_k) {
        if (r > 0.55 * h) return 0;
        const int kk = (int)(h / r) - 1;
        return std::min(std::max(kk, 1), max_k);
    }
    // One real frame: h the panel's usable rate (fps), max_k the multiplier asked for (1..3), work_ns the game's time on
    // this frame (0: unknown), now (ns, monotonic). Returns the generated frames to show per real one (0..max_k).
    int frame(int h, int max_k, int64_t work_ns, int64_t now) {
        if (work_ns > 0 && work_ns < 200 * kMs) mean_ns = mean_ns ? mean_ns * 0.9 + work_ns * 0.1 : (double)work_ns;
        if (max_k < 1 || h <= 0) {   // (FG off, or no panel rate: what was asked)
            want = -1;
            return k = std::max(max_k, 0);
        }
        if (!mean_ns) return k < 0 ? (k = max_k) : (k = std::min(k, max_k));
        const double r = 1e9 / mean_ns;
        const int t = pick(r, h, max_k);
        if (k < 0) return k = t;
        if (k > max_k) k = max_k;
        if (t == k || pick(r * 1.1, h, max_k) != t || pick(r * 0.9, h, max_k) != t) { want = -1; return k; }
        if (want != t) { want = t, want_at = now; return k; }
        if (now - want_at >= 2000 * kMs) k = t, want = -1;
        return k;
    }
};
