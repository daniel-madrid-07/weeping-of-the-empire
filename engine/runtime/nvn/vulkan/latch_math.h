// latch.c's camera math, without Vulkan (tools/engine/test_latch.py builds it alone). cam: the game's VS cb8 block as
// floats: rows 0-2 = view (3x4, row-major, world -> view, looking down -Z), rows 4-7 = projection (4x4, row-major).
#pragma once
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define LL_LAGS 3   // (candidate lags between a camera step and its frame: 0..2 presents)

// Yaw: forward = Ry(yaw) (0, 0, -1) about the world's +Y; pitch: the forward's elevation. Degrees.
static inline double ll_cam_yaw(const float *c) { return atan2(c[8], c[10]) * 57.29577951308232; }   // (forward = -(view row 2))
static inline double ll_cam_pitch(const float *c) {
    double l = sqrt((double)c[8] * c[8] + (double)c[9] * c[9] + (double)c[10] * c[10]);
    return l > 0 ? asin(fmax(-1.0, fmin(1.0, -c[9] / l))) * 57.29577951308232 : 0;
}

// Rotation by deg degrees about the unit axis a (Rodrigues), row-major
static inline void ll_rot(const double a[3], double deg, double R[3][3]) {
    double t = deg * 0.017453292519943295, c = cos(t), s = sin(t), u = 1 - c;
    R[0][0] = c + a[0] * a[0] * u;        R[0][1] = a[0] * a[1] * u - a[2] * s; R[0][2] = a[0] * a[2] * u + a[1] * s;
    R[1][0] = a[1] * a[0] * u + a[2] * s; R[1][1] = c + a[1] * a[1] * u;        R[1][2] = a[1] * a[2] * u - a[0] * s;
    R[2][0] = a[2] * a[0] * u - a[1] * s; R[2][1] = a[2] * a[1] * u + a[0] * s; R[2][2] = c + a[2] * a[2] * u;
}
static inline void ll_mul3(double A[3][3], double B[3][3], double C[3][3]) {
    double T[3][3];
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) T[i][j] = A[i][0] * B[0][j] + A[i][1] * B[1][j] + A[i][2] * B[2][j];
    memcpy(C, T, sizeof T);
}
static inline bool ll_inv3(double A[3][3], double B[3][3]) {
    double d = A[0][0] * (A[1][1] * A[2][2] - A[1][2] * A[2][1]) - A[0][1] * (A[1][0] * A[2][2] - A[1][2] * A[2][0]) +
               A[0][2] * (A[1][0] * A[2][1] - A[1][1] * A[2][0]);
    if (fabs(d) < 1e-12) return false;
    B[0][0] = (A[1][1] * A[2][2] - A[1][2] * A[2][1]) / d; B[0][1] = (A[0][2] * A[2][1] - A[0][1] * A[2][2]) / d;
    B[0][2] = (A[0][1] * A[1][2] - A[0][2] * A[1][1]) / d; B[1][0] = (A[1][2] * A[2][0] - A[1][0] * A[2][2]) / d;
    B[1][1] = (A[0][0] * A[2][2] - A[0][2] * A[2][0]) / d; B[1][2] = (A[0][2] * A[1][0] - A[0][0] * A[1][2]) / d;
    B[2][0] = (A[1][0] * A[2][1] - A[1][1] * A[2][0]) / d; B[2][1] = (A[0][1] * A[2][0] - A[0][0] * A[2][1]) / d;
    B[2][2] = (A[0][0] * A[1][1] - A[0][1] * A[1][0]) / d;
    return true;
}

// The homography from a pixel's NDC with the camera turned by yaw / pitch degrees (ll_cam_yaw / ll_cam_pitch's senses)
// to its NDC in the rendered frame: K R K^-1, K = the projection's x / y / w rows on view directions (a turn about the
// eye moves directions only, so the projection's translation column drops out), R = the turn in the rendered view's axes:
// yaw about the world's +Y as that view sees it (view column 1), then pitch about the view's x.
static inline bool ll_homography(const float *cam, double yaw, double pitch, double H[3][3]) {
    const float *P = cam + 16;
    double K[3][3] = {{P[0], P[1], P[2]}, {P[4], P[5], P[6]}, {P[12], P[13], P[14]}}, Ki[3][3], Ry[3][3], Rp[3][3], R[3][3];
    if (!ll_inv3(K, Ki)) return false;
    double up[3] = {cam[1], cam[5], cam[9]}, l = sqrt(up[0] * up[0] + up[1] * up[1] + up[2] * up[2]), x[3] = {1, 0, 0};
    if (l < 1e-6) return false;
    for (int i = 0; i < 3; i++) up[i] /= l;
    ll_rot(up, yaw, Ry);
    ll_rot(x, pitch, Rp);
    ll_mul3(Ry, Rp, R);
    ll_mul3(K, R, H);
    ll_mul3(H, Ki, H);
    return true;
}

// The largest shift, in pixels of a w x h image, that H gives the corners and the centre: under ~0.5 px a warp only
// trades a sharp frame for a resampled one.
static inline double ll_max_shift_px(double H[3][3], uint32_t w, uint32_t h) {
    static const double pt[5][2] = {{-1, -1}, {1, -1}, {-1, 1}, {1, 1}, {0, 0}};
    double mx = 0;
    for (int i = 0; i < 5; i++) {
        double q[3];
        for (int r = 0; r < 3; r++) q[r] = H[r][0] * pt[i][0] + H[r][1] * pt[i][1] + H[r][2];
        if (fabs(q[2]) < 1e-9) return 1e9;
        double dx = (q[0] / q[2] - pt[i][0]) * w * 0.5, dy = (q[1] / q[2] - pt[i][1]) * h * 0.5;
        mx = fmax(mx, sqrt(dx * dx + dy * dy));
    }
    return mx;
}

// Calibration: per candidate lag and axis (0 yaw, 1 pitch), the least-squares fit m = g d of the camera's measured turn
// between two frames (m, degrees) on the turn the consumed mouse totals predict at gain 1 (d = degrees per count x counts).
// g carries the sign and the axis's own scale against k (a warp by k alone would over- or undershoot and judder); whether
// the fit can be trusted is ll_cal_r2's. The lag is the one whose fits leave the smallest residual.
typedef struct { double sxx[LL_LAGS][2], sxy[LL_LAGS][2], syy[LL_LAGS][2]; uint32_t n; } LlCal;

static inline void ll_cal_add(LlCal *c, double d[LL_LAGS][2], const double m[2]) {
    for (int l = 0; l < LL_LAGS; l++)
        for (int a = 0; a < 2; a++) {
            c->sxx[l][a] += d[l][a] * d[l][a];
            c->sxy[l][a] += d[l][a] * m[a];
            c->syy[l][a] += m[a] * m[a];
        }
    c->n++;
}
static inline double ll_cal_resid(const LlCal *c, int l) {   // (squared degrees, both axes)
    double e = 0;
    for (int a = 0; a < 2; a++) e += c->sxx[l][a] > 1e-9 ? c->syy[l][a] - c->sxy[l][a] * c->sxy[l][a] / c->sxx[l][a] : c->syy[l][a];
    return e;
}
static inline int ll_cal_lag(const LlCal *c) {
    int best = 0;
    for (int l = 1; l < LL_LAGS; l++) if (ll_cal_resid(c, l) < ll_cal_resid(c, best)) best = l;
    return best;
}
// The fitted gain, or 0 when the axis has not moved enough to fit (under 1 square degree of predicted turn in all)
static inline double ll_cal_gain(const LlCal *c, int l, int a) { return c->sxx[l][a] >= 1.0 ? c->sxy[l][a] / c->sxx[l][a] : 0; }
// The share of the camera's turn the fit explains (uncentred R^2 = sxy^2 / (sxx syy)). A game that smooths its camera
// fits a gain at an integer lag badly (first-order smoothing a: R^2 ~ 1 - a^2), and a warp by that gain would judder; a
// gain off 1 alone is no problem (the pitch's veneer gain is 70/51 against the yaw's 70/30.
static inline double ll_cal_r2(const LlCal *c, int l, int a) {
    return c->sxx[l][a] > 1e-9 && c->syy[l][a] > 1e-9 ? c->sxy[l][a] * c->sxy[l][a] / (c->sxx[l][a] * c->syy[l][a]) : 0;
}
// The gain to warp an axis by, or 0: unfitted, explained under 95 %, or outside 0.3-3 in size
static inline double ll_cal_use(const LlCal *c, int l, int a) {
    double g = ll_cal_gain(c, l, a);
    return g && ll_cal_r2(c, l, a) >= 0.95 && fabs(g) >= 0.3 && fabs(g) <= 3 ? g : 0;
}
