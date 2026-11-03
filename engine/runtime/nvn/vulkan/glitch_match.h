// glitch.c's frame comparison, without Vulkan (tools/engine/glitch_match_test.cpp): the 160x90 luminance read-back of a
// presented frame as a summed-area table, compared with another as the mean absolute difference of their 16x9 grid
// cells, optionally with the second moved by a whole number of read-back pixels (a camera turn: integration's review,
// a frame turning see-through inside a 40 px/frame pan at 2560 wide was missed because the neighbours differ as much).
#pragma once
#include <math.h>
#include <stdbool.h>

#define GM_W 160
#define GM_H 90
#define GM_CW 16
#define GM_CH 9

typedef struct { double s[(GM_W + 1) * (GM_H + 1)]; } GmSum;   // s[y * (GM_W + 1) + x]: the sum of lum[0..y) x [0..x)

static inline void gm_sum(const float *lum, GmSum *t) {
    for (int x = 0; x <= GM_W; x++) t->s[x] = 0;
    for (int y = 1; y <= GM_H; y++) {
        double row = 0;
        t->s[y * (GM_W + 1)] = 0;
        for (int x = 1; x <= GM_W; x++) {
            row += lum[(y - 1) * GM_W + x - 1];
            t->s[y * (GM_W + 1) + x] = t->s[(y - 1) * (GM_W + 1) + x] + row;
        }
    }
}
static inline double gm_box(const GmSum *t, int x0, int y0, int x1, int y1) {   // (the sum over [x0, x1) x [y0, y1))
    const int w = GM_W + 1;
    return t->s[y1 * w + x1] - t->s[y0 * w + x1] - t->s[y1 * w + x0] + t->s[y0 * w + x0];
}
// Cell (x, y) of a against the same-size window of b at (x + dx, y + dy), over the cells whose window lies inside b;
// 1 (no match) when fewer than half the cells do
static inline float gm_dist(const GmSum *a, const GmSum *b, int dx, int dy) {
    const int cw = GM_W / GM_CW, ch = GM_H / GM_CH;
    double d = 0;
    int n = 0;
    for (int cy = 0; cy < GM_CH; cy++)
        for (int cx = 0; cx < GM_CW; cx++) {
            const int x0 = cx * cw, y0 = cy * ch, bx = x0 + dx, by = y0 + dy;
            if (bx < 0 || by < 0 || bx + cw > GM_W || by + ch > GM_H) continue;
            d += fabs(gm_box(a, x0, y0, x0 + cw, y0 + ch) - gm_box(b, bx, by, bx + cw, by + ch));
            n++;
        }
    return n * 2 < GM_CW * GM_CH ? 1.0f : (float)(d / ((double)n * cw * ch));
}
// The central cells only (x 2..13, y 2..6 of the 16x9 grid: the HUD sits on the edges), no shift; edge = the others
static inline float gm_dist_part(const GmSum *a, const GmSum *b, bool centre) {
    const int cw = GM_W / GM_CW, ch = GM_H / GM_CH;
    double d = 0;
    int n = 0;
    for (int cy = 0; cy < GM_CH; cy++)
        for (int cx = 0; cx < GM_CW; cx++) {
            if ((cy >= 2 && cy < GM_CH - 2 && cx >= 2 && cx < GM_CW - 2) != centre) continue;
            const int x0 = cx * cw, y0 = cy * ch;
            d += fabs(gm_box(a, x0, y0, x0 + cw, y0 + ch) - gm_box(b, x0, y0, x0 + cw, y0 + ch));
            n++;
        }
    return n ? (float)(d / ((double)n * cw * ch)) : 1.0f;
}
static inline float gm_best(const GmSum *a, const GmSum *b, int rx, int ry) {   // (the smallest over |dx| <= rx, |dy| <= ry)
    float m = 1.0f;
    for (int dy = -ry; dy <= ry; dy++)
        for (int dx = -rx; dx <= rx; dx++) {
            const float d = gm_dist(a, b, dx, dy);
            if (d < m) m = d;
        }
    return m;
}
// The verdict on frame s between pv and nx: > 0 the score of a frame that reverts (it differs from both neighbours by
// at least min while they match within a quarter of that), 0 none. moved: the neighbours matched only after the
// camera's move (each pair compared at its best shift: +-8 x +-3 read-back pixels a frame, twice that across two).
static inline float gm_judge(const GmSum *pv, const GmSum *s, const GmSum *nx, float min, float out[3], bool *moved) {
    float dp = gm_dist(s, pv, 0, 0), dn = gm_dist(s, nx, 0, 0), dnb = gm_dist(pv, nx, 0, 0);
    float dmin = dp < dn ? dp : dn;
    *moved = false;
    if (dmin < min) return 0;
    if (dnb * 4 > dmin) {
        dp = gm_best(s, pv, 8, 3), dn = gm_best(s, nx, 8, 3), dnb = gm_best(pv, nx, 16, 6);
        dmin = dp < dn ? dp : dn;
        if (dmin < min || dnb * 4 > dmin) return 0;
        *moved = true;
    }
    out[0] = dp, out[1] = dn, out[2] = dnb;
    return dmin / (dnb > 0.002f ? dnb : 0.002f);
}
