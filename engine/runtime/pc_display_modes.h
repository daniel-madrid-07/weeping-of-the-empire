// Display modes (shim_win.c, pc_window.cpp): the lists the settings offer for exclusive fullscreen, from the monitor's
// own modes (EnumDisplaySettings), and the mode a choice ends up as on a monitor. Pure C, no Windows:
// tools/engine/test_display_modes.py tests it.
#pragma once

typedef struct { int w, h, hz; } pcd_mode;

// The mode exclusive fullscreen sets for a choice (w / h 0: the desktop's size; hz 0: the desktop's rate): that size at
// that rate if the monitor has it, else the fastest rate it has under it (60 asked, 59 there: 59), else its fastest.
// 0: the monitor has no such size (the desktop mode stays).
static inline int pcd_pick(const pcd_mode *m, int n, pcd_mode desk, int w, int h, int hz, pcd_mode *out) {
    if (!w || !h) { w = desk.w; h = desk.h; }
    if (!hz) hz = desk.hz;
    int fastest = -1, under = -1;
    for (int i = 0; i < n; i++) {
        if (m[i].w != w || m[i].h != h) continue;
        if (m[i].hz == hz) { *out = m[i]; return 1; }
        if (m[i].hz < hz && (under < 0 || m[i].hz > m[under].hz)) under = i;
        if (fastest < 0 || m[i].hz > m[fastest].hz) fastest = i;
    }
    if (fastest < 0) return 0;
    *out = m[under >= 0 ? under : fastest];
    return 1;
}

// The sizes worth offering (1280 x 720 and up, each once), largest first; returns how many went into out.
static inline int pcd_sizes(const pcd_mode *m, int n, pcd_mode *out, int max) {
    int k = 0;
    for (int i = 0; i < n; i++) {
        if (m[i].w < 1280 || m[i].h < 720) continue;
        int dup = 0;
        for (int j = 0; j < k && !dup; j++) dup = out[j].w == m[i].w && out[j].h == m[i].h;
        if (dup) continue;
        int at = k < max ? k : max - 1;   // (full: the smallest so far gives way when this one is larger)
        if (k == max && (long long)m[i].w * m[i].h <= (long long)out[at].w * out[at].h) continue;
        out[at] = m[i]; out[at].hz = 0;
        if (k < max) k++;
        for (int j = at; j > 0; j--) {   // (insertion: area, then width, descending)
            long long a = (long long)out[j].w * out[j].h, b = (long long)out[j - 1].w * out[j - 1].h;
            if (a < b || (a == b && out[j].w <= out[j - 1].w)) break;
            pcd_mode t = out[j]; out[j] = out[j - 1]; out[j - 1] = t;
        }
    }
    return k;
}

// The refresh rates the monitor has at any size (50 Hz and up, each once), fastest first.
static inline int pcd_rates(const pcd_mode *m, int n, int *out, int max) {
    int k = 0;
    for (int i = 0; i < n; i++) {
        int hz = m[i].hz, dup = 0;
        if (hz < 50) continue;
        for (int j = 0; j < k && !dup; j++) dup = out[j] == hz;
        if (dup || k == max) continue;
        int j = k++;
        for (; j > 0 && out[j - 1] < hz; j--) out[j] = out[j - 1];
        out[j] = hz;
    }
    return k;
}
