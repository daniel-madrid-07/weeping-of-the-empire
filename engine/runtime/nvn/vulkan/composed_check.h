// The composed-window verdict (present.c cmp_check, the VRR row's FIFO): pure, so tools/engine/composed_check_test.c runs
// it on the CPU (tools/engine/test_composed_check.py). One ~2 s window: the on-screen intervals (us, present wait), the
// presents and seconds, the render thread's time held by the presentation engine (ms, all presents), the monitor's and
// the desktop compositor's rates (Hz; 0 unknown).
#pragma once
#include <math.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    double p10_ms, rate, held_ms;   // 10th percentile on-screen interval; presents a second; held per present
    unsigned grid, n;               // intervals within 1 ms of 1-4 compositor periods, of n
    bool comp;                      // the compositor holds the window below the monitor's rate
} CmpVerdict;

static CmpVerdict cmp_classify(const uint32_t *iv_in, unsigned n, double secs, uint64_t pres, double blk_ms, int hz, double dwm) {
    CmpVerdict v = {0};
    if (n > 256) n = 256;
    v.n = n;
    if (!n || !pres || secs <= 0 || hz <= 0) return v;
    uint32_t iv[256];
    for (unsigned i = 0; i < n; i++) iv[i] = iv_in[i];
    for (unsigned i = 1; i < n; i++)   // (insertion sort: 256 at most, every 2 s)
        for (unsigned j = i; j && iv[j - 1] > iv[j]; j--) { const uint32_t x = iv[j]; iv[j] = iv[j - 1]; iv[j - 1] = x; }
    v.p10_ms = iv[n / 10] / 1000.0;
    v.held_ms = blk_ms / (double)pres;
    v.rate = (double)pres / secs;
    const double per_ms = 1000.0 / hz;
    // (the on-screen times on the compositor's grid: a GPU-bound frame rate under real VRR also holds the render thread at
    // the window image, but its intervals spread around a rate near the compositor's, they do not sit on its periods.
    // Tight: within 0.6 ms of 1-4 periods for 75 % of them (present wait's own reading is polled every 0.5 ms))
    if (dwm > 0 && dwm < 0.75 * hz)
        for (unsigned i = 0; i < n; i++) {
            const double g = 1e6 / dwm, k = floor(iv[i] / g + 0.5);
            v.grid += k >= 1 && k <= 4 && fabs(iv[i] - k * g) <= 600.0;
        }
    v.comp = v.rate < 0.75 * hz && v.p10_ms >= 1.5 * per_ms && v.held_ms >= 0.25 * v.p10_ms && v.grid * 4 >= n * 3;
    return v;
}
