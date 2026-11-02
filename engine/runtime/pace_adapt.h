// Adaptive unlocked pacing for VRR (hle_vi.cpp vi_pace_wait): a cap that follows what the scene holds, continuously,
// without clocks or globals, so tools/engine/pace_adapt_test.cpp runs it on simulated frames.
//
// Unlocked under the ceiling, each frame is shown when it is done: a scene that takes 5 ms one frame and 9 the next is
// shown at 5 / 9 / 6 / 10 ms, while the game's realtime step (an EMA of those times, games/totk frame_step_next) moves
// the world by ~7 ms each: the motion speeds up and slows down from frame to frame. A VRR panel shows any period, so
// the cap needs no rungs: it holds the period most frames meet and moves slowly, and every frame that meets it is shown
// at the same interval the step gives it.
//
// The measure is a frame's work: from the pacer's release of the frame before to this frame's arrival (the wait is not
// work). The period is a high quantile of the last frames' work plus a margin, never above the ceiling's rate nor under
// 30 fps. Heavier scenes take effect within a few frames (a quarter of the gap a frame); lighter ones over ~64 frames
// (1/64 of the gap + 0.25 % of the period a frame: ~0.5 s at 120 fps), so a short light stretch does not pull the cap up
// into the next heavy one. Work grows with the period (the game's physics steps cover more time a frame), but less than the period
// does, so the period settles where work + margin meets it instead of spiralling down (pace_ladder.h's frame-time
// rungs did spiral: their targets were the frames' rates, which include the waits).
#pragma once
#include <algorithm>
#include <cstdint>

struct PaceAdapt {
    static constexpr int64_t kMs = 1000000;
    static constexpr int kRing = 64;          // ~0.3-0.6 s of frames at 100-200 fps
    static constexpr double kQuantile = 0.9;  // the frames the period is set to meet
    static constexpr double kMargin = 0.04;   // over that quantile's work (timer wake-up, the next frame's spread)
    static constexpr double kRise = 0.0025;   // the period's fall a frame toward a lighter scene: this share of it + 1/64 of the gap
    int64_t work[kRing] = {};
    unsigned n = 0;
    int64_t period = 0, last = 0;

    // One frame at the pacer: arrival `now` (ns, monotonic), its work (ns: now minus the previous frame's release),
    // the ceiling's period `top` (ns) and the floor's `bottom` (ns, 1/30 s). Returns the period to pace this frame at.
    int64_t frame(int64_t now, int64_t w, int64_t top, int64_t bottom) {
        if (!period) period = top;
        const bool gap = last && now - last > 250 * kMs;   // (a load or a menu: the frames before it are another scene's)
        last = now;
        if (gap) { n = 0; return period = std::min(std::max(period, top), bottom); }   // (its work spans the gap)
        work[n++ % kRing] = std::max<int64_t>(w, 0);
        const unsigned k = std::min<unsigned>(n, kRing);
        if (k < 8) return period = std::min(std::max(period, top), bottom);
        int64_t s[kRing];
        std::copy(work, work + k, s);
        int64_t *q = s + (unsigned)(kQuantile * (k - 1) + 0.5);
        std::nth_element(s, q, s + k);
        const int64_t want = std::min(std::max((int64_t)(*q * (1 + kMargin)), top), bottom);
        if (want > period) period += std::max<int64_t>((want - period) / 4, 1);
        else if (want < period) period = std::max(want, period - std::max<int64_t>((int64_t)(period * kRise) + (period - want) / 64, 1));
        return period = std::min(std::max(period, top), bottom);
    }
};
