// Steady unlocked's rung ladder (hle_vi.cpp steady_period): the rungs and the decisions, without clocks or globals, so
// tools/engine/pace_ladder_test.cpp runs them on simulated frames.
//
// Each rung is judged by what the frames do there, every 0.5 s over the last second (a rung's first second, and the
// first second after a gap such as a loading screen, is not judged; a promotion's first half second, then over its own
// frames: a probe that fails costs ~0.5 s of unpaced frames):
// - down when the frames held under 93 % of its rate or more than 10 % came a quarter period late. Short of the rate:
//   straight to the fastest rung the frames held (95 % of its rate): above the game's rate the pacer no longer waits,
//   so the frames come at that rate (a cascade of one rung a second took a heavy load-in to 30 in ~7 s);
// - up after a hold with 98 % of the rate and under 2 % late, one frame of either spared (at 30 fps 2 % is no frame: one
//   hitch restarted the hold). One rung after 4 judgements (2 s), or straight back to the rung it last held for 4 s
//   before a fall (the ceiling at the start) after 2 (1 s), "Unlocked, steady" started at 30 after the load-in and
//   climbed one rung per 2.5 s, ~20 s to 224 on a 240 Hz VRR panel.
// A promotion that falls back within 4 s is a probe that failed: the next climb to that rung waits 4 << falls judgements
// (4 s, 8... 32 s; forgotten after a minute without falls), and a failed return becomes the landing rung (no more jumps
// above what the frames showed). Frame times are not the measure: they grow as the rate falls (fixed-step physics: more
// steps a frame), so targets taken from them spiralled down.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>

// The rungs' periods in ns, fastest first, from the ceiling's period (cap) down to the floor's (30 fps; 20 or 15 with
// display.steady_floor, #23: a Vega 7 at 15-17 fps on a 60 Hz FIFO swapchain showed its frames 3, 4 or 5 refreshes
// each, where rungs of 3 and 4 refreshes hold them even): whole multiples of refresh (the window's monitor period x
// unit; 0 when unknown: the ceiling's period), or with tenth the ceiling's period / 0.9^k (TOTK_UNLOCK_STEADY=vrr).
// top: the ceiling is the first rung. Never empty.
static inline int pace_ladder_build(int64_t cap, int64_t refresh, bool tenth, bool top, int64_t out[32],
                                    int64_t floor = 1000000000ll / 30) {
    const int64_t r = refresh ? refresh : cap;
    int m = 0;
    if (top) out[m++] = cap;
    for (int i = 1; m < 32; i++) {
        const int64_t p = tenth ? (int64_t)(cap / std::pow(0.9, i - 1)) : r * i;
        if (p > floor + floor / 50) break;
        if (p >= cap - cap / 100 && (!m || p > out[m - 1])) out[m++] = p;
    }
    if (!m) out[m++] = cap;
    return m;
}

struct PaceLadder {
    static constexpr int64_t kMs = 1000000;
    struct Frame { int64_t t; bool late; };
    Frame ring[512] = {};
    unsigned n = 0;
    int64_t evaluated = 0, changed_at = 0, last = 0, fell_at = 0, target = 0, fell_from = 0;
    int64_t good = -1;   // the rung to go back to (-1: the first one)
    int ceil_was = 0, held = 0, falls = 0;
    bool probe = false;  // the rung came from a promotion and has not held 4 s yet
    int64_t excused[3] = {};   // the ends of the last stalls excused as hitches (frame: at most 3 within 10 s)
    unsigned excused_n = 0;
    int k = 0, l = 0;    // the last judgement's frames and late ones (for the log)

    // One frame at the pacer: ceil the ceiling (fps), now its arrival (ns, monotonic, not 0), late a quarter period or
    // more after its slot; build(int64_t out[32]) -> count gives the rungs (pace_ladder_build; called per judgement).
    // Returns the rung's period (ns).
    template <class Build> int64_t frame(int ceil, int64_t now, bool late, Build build) {
        if (last && now - last > 250 * kMs) {   // (a gap: the frames before it are another scene's)
            for (auto &r : ring) r.t = 0;
            changed_at = now, held = 0;
        }
        last = now;
        ring[n++ & 511] = {now, late};
        if (ceil != ceil_was) {
            ceil_was = ceil, target = 1000000000ll / ceil, good = -1, held = falls = 0, fell_from = 0, probe = false;
            changed_at = now;
        }
        // (a promotion is judged after 0.5 s, on its own frames: a failed probe costs half a second of unpaced frames)
        const int64_t since = now - changed_at, window = std::min(since, 1000 * kMs);
        if (now - evaluated < 500 * kMs || since < (probe ? 500 : 1000) * kMs) return target;
        evaluated = now;
        k = l = 0;
        int64_t gap = 0, prev = 0, gap_end = 0;   // (the window's longest interval between two frames, and its end)
        for (unsigned i = n > 512 ? n - 512 : 0; i < n; i++) {
            const Frame &r = ring[i & 511];
            if (!r.t || now - r.t > window) continue;
            k++, l += r.late;
            if (prev && r.t - prev > gap) gap = r.t - prev, gap_end = r.t;
            prev = r.t;
        }
        int64_t lad[32];
        const int m = build(lad);
        // (15 frames to judge. Under 30 fps, display.steady_floor: 70 % of what the slowest rung shows in the window, 10 at
        // 15 fps over a second, 7 over a probe's half second: a 15 rung shows 14 or 15 frames a second, so a scene there
        // was never judged and never climbed back, nor went to the ladder's last rung after the floor was raised live; a
        // probe to 20 was judged only after a whole second of uneven frames. Rungs down to 30 fps keep the 15)
        const int64_t slow = std::max(lad[m - 1], target);
        if (k < (slow > 34 * kMs ? window * 7 / 10 / slow : 15)) return target;
        const double fps = k * 1e9 / window;   // (the frames' rate over the window)
        int cur = 0;
        while (cur + 1 < m && lad[cur] < target) cur++;
        int to = cur;
        if (falls && now - fell_at > 60000 * kMs) falls = 0;
        const double rate = 1e9 / lad[cur], miss = (double)l / k;
        // One long stall (a scene hitch: 4 rung periods or more) is not a rate the game cannot hold: the slots it swallowed
        // count as held when the frames around it make the rate. At DLSS-G / FSR FG's 60-real rung on a 120 Hz panel (rungs
        // 60 and 30 only) one 120 ms hitch in the judged second left 54 frames, under 93 %, and fell to 30; the next
        // hitches made every climb back a failed probe, held there ~30 s (FSR FG boot r47ffsrfg, +40.. +66 s; the same
        // hitches cost SR / off a dip of a few fps) A stall that keeps coming is the scene's rate, not a hitch: a 4th stall
        // within 10 s of 3 excused ones is judged without the excuse. Stalls, not judgements: each is in two judged windows
        // (every 0.5 s over 1 s), known by its end. At display.steady_floor's 20 fps rung a hitch is 200 ms or more; at 15
        // it would be 267 ms, past the 250 ms gap above, which already starts the judging over (and 15 is the floor:
        // nothing to fall to)
        bool hitch = gap >= 4 * lad[cur] && (k + (double)gap / lad[cur] - 1) * 1e9 / window >= 0.93 * rate;
        if (hitch && fps < 0.93 * rate && miss <= 0.10) {   // (only where the excuse applies: no slot spent otherwise)
            int recent = 0;
            bool known = false;
            for (int64_t e : excused) recent += e && now - e <= 10000 * kMs, known |= e == gap_end;
            if (!known) {
                if (recent >= 3) hitch = false;
                else excused[excused_n++ % 3] = gap_end;
            }
        }
        if ((fps < 0.93 * rate || miss > 0.10) && !(hitch && miss <= 0.10) && cur + 1 < m) {
            to = cur + 1;
            if (fps < 0.93 * rate)
                while (to + 1 < m && fps < 0.95 * (1e9 / lad[to])) to++;
            if (probe) {   // (falling from a rung just reached makes the next climb to it wait longer)
                falls = fell_from == lad[cur] ? std::min(falls + 1, 4) : 1;
                fell_from = lad[cur];
                if (good < 0 || good <= lad[cur]) good = lad[to];
            } else if (now - changed_at >= 4000 * kMs) good = lad[cur];   // (held, then a heavier scene)
            fell_at = now, held = 0, probe = false;
        } else if (fps + 1e9 / window >= 0.98 * rate && l <= 1 + k / 50 && cur > 0) {
            ++held;
            if (probe && now - changed_at >= 4000 * kMs) probe = false;
            int g = good < 0 ? 0 : -1;
            for (int i = 0; g < 0 && i < m; i++)
                if (lad[i] == good) g = i;   // (-1: not a rung since the refresh changed)
            auto need = [&](int i, int base) { return lad[i] == fell_from && falls ? 4 << falls : base; };
            if (g >= 0 && g < cur && held >= need(g, 2)) to = g;
            else if (held >= need(cur - 1, 4)) to = cur - 1;
            if (to < cur) held = 0, probe = true;
        } else held = 0;
        if (lad[to] != target) {
            // (under 1 %: the same rate, e.g. the ceiling's 1e9 / 144 against the exact refresh's period: no change)
            const bool same = std::abs(lad[to] - target) * 100 < target;
            target = lad[to];
            if (!same) {
                changed_at = now;
                for (auto &r : ring) r.t = 0;   // (the new rung is judged on its own frames)
            }
        }
        return target;
    }
};
