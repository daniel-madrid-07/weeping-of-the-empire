// present.c: the window's presentation. device.c owns the swapchain and the frame; this file adds, each off by
// default (settings in present.h):
//  - present timing: game present, window present and display times (VK_KHR_present_wait) -> the "[pace]" line every
//    2 s (frame time percentiles, 1% lows, replaced frames) and TOTK_PRESENT_LOG's CSV for tools/present/pacing.py;
//  - latency: at most TOTK_MAX_QUEUED frames presented but not yet on screen (a waitable swapchain, as DXGI's frame
//    latency object), and TOTK_FRAMES_IN_FLIGHT frames of GPU work behind the CPU;
//  - HDR output: the frame is drawn into an SDR canvas (everything that draws into "the window image" draws there, so
//    the post passes, OSD, overlay and screenshots are unchanged) and a last pass expands it (inverse tonemap) and
//    encodes it for an HDR10 (PQ, BT.2020) or scRGB (linear FP16) swapchain.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <SDL3/SDL.h>
#include <math.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../shim/las.h"
#include "vk.h"
#include <vulkan/vulkan_win32.h>   // (VK_EXT_full_screen_exclusive)
#include "present.h"

// ================================================================== settings
static atomic_int g_max_queued = 2, g_fif = 2, g_hdr_req;
static volatile float g_hdr_peak = 1000.0f, g_hdr_paper = 200.0f;
static float g_hdr_expand = 3.0f;
static bool g_hdr_gamma22;
static int g_stats;           // "[pace]" line
static FILE *g_log;           // TOTK_PRESENT_LOG
static SRWLOCK g_log_lock = SRWLOCK_INIT;

static int env_int(const char *n, int def, int lo, int hi) {
    const char *e = getenv(n);
    if (!e || !*e) return def;
    int v = atoi(e);
    return v < lo ? lo : v > hi ? hi : v;
}
static float env_float(const char *n, float def, float lo, float hi) {
    const char *e = getenv(n);
    if (!e || !*e) return def;
    float v = (float)atof(e);
    return v < lo ? lo : v > hi ? hi : v;
}
static int hdr_mode_of(const char *s) {
    return !s ? VKP_HDR_OFF : !strcmp(s, "hdr10") || !strcmp(s, "1") ? VKP_HDR10 : !strcmp(s, "scrgb") ? VKP_HDR_SCRGB : VKP_HDR_OFF;
}

static void cfg_init(void) {
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    BOOL pending;
    if (!InitOnceBeginInitialize(&once, 0, &pending, NULL) || !pending) return;
    atomic_store(&g_max_queued, env_int("TOTK_MAX_QUEUED", 2, 0, 3));
    atomic_store(&g_fif, env_int("TOTK_FRAMES_IN_FLIGHT", 2, 1, 3));
    atomic_store(&g_hdr_req, hdr_mode_of(getenv("TOTK_HDR")));
    g_hdr_peak = env_float("TOTK_HDR_PEAK", 1000.0f, 80.0f, 10000.0f);
    g_hdr_paper = env_float("TOTK_HDR_PAPER", 200.0f, 80.0f, 1000.0f);
    g_hdr_expand = env_float("TOTK_HDR_EXPAND", 3.0f, 1.0f, 12.0f);
    g_hdr_gamma22 = getenv("TOTK_HDR_GAMMA") && !strcmp(getenv("TOTK_HDR_GAMMA"), "2.2");
    const char *lp = getenv("TOTK_PRESENT_LOG");
    if (lp && *lp) {
        g_log = fopen(lp, "w");
        if (g_log) {
            setvbuf(g_log, NULL, _IOFBF, 1 << 20);
            fprintf(g_log, "# present log (engine/runtime/nvn/vulkan/present.c), ns of the QPC clock (SDL_GetTicksNS)\n"
                           "# g,<ns>,<frame s>: the game presented (nvnQueuePresentTexture, after its pacing); frame s = the\n"
                           "#   data page's frame seconds written for the next frame (0: unpatched main)\n"
                           "# p,<frame>,<present id>,<submit ns>,<paced ns>,<present ns>: window image presented (vkQueuePresentKHR returned)\n"
                           "# s,<frame>,<ns>: frame not shown (held / minimised)\n"
                           "# d,<present id>,<ns>: on screen (vkWaitForPresentKHR returned)\n"
                           "# l,<present id>,<game ns>,<r|f>: the game present it shows (nvnQueuePresentTexture time): its frame\n"
                           "#   (r) or the frame DLSS-G generated before it (f)\n");
        } else LOG("[present] cannot open TOTK_PRESENT_LOG %s\n", lp);
    }
    g_stats = g_log || (getenv("TOTK_PACE_STATS") && strcmp(getenv("TOTK_PACE_STATS"), "0"));
    InitOnceComplete(&once, 0, NULL);
}

void vk_present_set_max_queued(int n) { cfg_init(); atomic_store(&g_max_queued, n < 0 ? 0 : n > 3 ? 3 : n); }
int vk_present_max_queued(void) { cfg_init(); return atomic_load(&g_max_queued); }
void vk_present_set_frames_in_flight(int n) { cfg_init(); atomic_store(&g_fif, n < 1 ? 1 : n > 3 ? 3 : n); }
int vk_present_frames_in_flight(void) { cfg_init(); return atomic_load(&g_fif); }

static int g_hdr_mode;               // of the current swapchain
static bool g_hdr_sup[3], g_fmts_known;
void vk_present_set_hdr(int mode, float peak, float paper) {
    cfg_init();
    if (peak >= 80.0f && peak <= 10000.0f) g_hdr_peak = peak;
    if (paper >= 80.0f && paper <= 1000.0f) g_hdr_paper = paper;
    if (mode < VKP_HDR_OFF || mode > VKP_HDR_SCRGB) mode = VKP_HDR_OFF;
    if (atomic_exchange(&g_hdr_req, mode) != mode) vk_request_swapchain_recreate();
}
int vk_present_hdr_supported(int mode) {
    if (!g_fmts_known) return -1;
    return mode == VKP_HDR_OFF || (mode > 0 && mode < 3 && g_hdr_sup[mode]);
}
int vk_present_hdr_active(void) { return g_hdr_mode; }

static atomic_int g_mode_active = -1;   // (written on the render thread, read on the game thread: hle_vi.cpp)
int vk_present_mode_active(void) { return atomic_load(&g_mode_active); }
int vk_present_mode_supported(int mode) {
    if (!g_vk.surface) return -1;
    uint32_t n = 0;
    VkPresentModeKHR m[16];
    vkGetPhysicalDeviceSurfacePresentModesKHR(g_vk.phys, g_vk.surface, &n, NULL);
    if (n > 16) n = 16;
    vkGetPhysicalDeviceSurfacePresentModesKHR(g_vk.phys, g_vk.surface, &n, m);
    for (uint32_t i = 0; i < n; i++) if ((int)m[i] == mode) return 1;
    return 0;
}
static const char *mode_name(int m) {
    return m == VKP_IMMEDIATE ? "immediate" : m == VKP_MAILBOX ? "mailbox" : m == VKP_FIFO ? "fifo"
         : m == VKP_FIFO_RELAXED ? "fifo_relaxed" : "?";
}

// ================================================================== extensions (device.c create_instance / create_device)
static bool g_colorspace_ext, g_pw, g_hdr_meta;
static VkPhysicalDevicePresentWaitFeaturesKHR g_pw_f = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR,
                                                        .presentWait = VK_TRUE};
static VkPhysicalDevicePresentIdFeaturesKHR g_pid_f = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR,
                                                       .presentId = VK_TRUE};
static PFN_vkWaitForPresentKHR p_wait;
static PFN_vkSetHdrMetadataEXT p_hdr_meta;

// The HDR formats need VK_EXT_swapchain_colorspace on the instance: only when HDR may be used (TOTK_HDR, or the
// overlay, which can turn it on in game), so a default start enumerates the surface exactly as before.
// Exclusive fullscreen (shim_win.c window modes) needs VK_KHR_get_surface_capabilities2 for VK_EXT_full_screen_exclusive:
// only with TOTK_WINDOW_MODE=exclusive or the overlay.
static bool g_caps2_ext, g_fse_ext;
// Low latency (TOTK_REFLEX=1 | boost, the display.low_latency row): NVIDIA Reflex's sleep through VK_NV_low_latency2.
// Each game frame, before it starts (and samples input), the game thread calls vkLatencySleepNV and waits on its
// semaphore: the driver holds the frame back so it reaches the GPU just in time instead of queueing behind it.
static bool g_ll_ext;
static atomic_int g_ll_mode;   // 0 off, 1 on, 2 on + boost (GPU clocks kept up), 3 markers and timings only (no sleep)
static bool g_ll_kept_off;     // TOTK_REFLEX=0: not even with DLSS-G
// The mode in effect: DLSS-G turns Reflex on while the setting is off and the driver has it (NVIDIA requires Reflex with
// frame generation, FG + VRR on a 180 Hz panel: 62 real fps without it, 82 with it, input -> screen 45.1 ms against
// 38.2, generated frames bunched 8.9 % against 2.4 %).
static int ll_mode(void) {
    const int m = atomic_load(&g_ll_mode);
    return !m && g_ll_ext && !g_ll_kept_off && vk_ngx_fg_on() ? 1 : m;
}
int vk_present_instance_exts(const char **ext, int room) {
    cfg_init();
    const char *ov = getenv("NXR_PC_OVERLAY"), *wm = getenv("TOTK_WINDOW_MODE");
    bool overlay = ov && !strcmp(ov, "1");
    bool want_cs = atomic_load(&g_hdr_req) != VKP_HDR_OFF || overlay, want_c2 = (wm && !strcmp(wm, "exclusive")) || overlay;
    if ((!want_cs && !want_c2) || room < 1) return 0;
    uint32_t n = 0;
    bool cs = false, c2 = false;
    vkEnumerateInstanceExtensionProperties(NULL, &n, NULL);
    VkExtensionProperties *p = calloc(n ? n : 1, sizeof *p);
    vkEnumerateInstanceExtensionProperties(NULL, &n, p);
    for (uint32_t i = 0; i < n; i++) {
        cs |= !strcmp(p[i].extensionName, VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME);
        c2 |= !strcmp(p[i].extensionName, VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME);
    }
    free(p);
    int k = 0;
    if (want_cs && !cs) LOG("[present] HDR: no %s on this Vulkan instance\n", VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME);
    if (want_cs && cs && k < room) { ext[k++] = VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME; g_colorspace_ext = true; }
    if (want_c2 && c2 && k < room) { ext[k++] = VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME; g_caps2_ext = true; }
    return k;
}

// VK_KHR_present_id + VK_KHR_present_wait when something uses them (TOTK_PRESENT_LOG, TOTK_PACE_STATS, TOTK_MAX_QUEUED,
// the overlay); TOTK_PRESENT_WAIT=0 never. VK_EXT_hdr_metadata with the colorspace extension.
void vk_present_device_exts(const VkExtensionProperties *ep, uint32_t ne, const char **ext, uint32_t *n_ext) {
    cfg_init();
    bool pid = false, pw = false, meta = false;
    for (uint32_t i = 0; i < ne; i++) {
        pid |= !strcmp(ep[i].extensionName, VK_KHR_PRESENT_ID_EXTENSION_NAME);
        pw |= !strcmp(ep[i].extensionName, VK_KHR_PRESENT_WAIT_EXTENSION_NAME);
        meta |= !strcmp(ep[i].extensionName, VK_EXT_HDR_METADATA_EXTENSION_NAME);
    }
    const char *ov = getenv("NXR_PC_OVERLAY"), *off = getenv("TOTK_PRESENT_WAIT"), *rx = getenv("TOTK_REFLEX");
    bool want = (g_stats || atomic_load(&g_max_queued) > 0 || (ov && !strcmp(ov, "1")) || (rx && *rx && *rx != '0')) &&
                !(off && !strcmp(off, "0"));
    if (want && pid && pw) {
        VkPhysicalDevicePresentWaitFeaturesKHR qw = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR};
        VkPhysicalDevicePresentIdFeaturesKHR qi = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR, .pNext = &qw};
        VkPhysicalDeviceFeatures2 q = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &qi};
        vkGetPhysicalDeviceFeatures2(g_vk.phys, &q);
        g_pw = qi.presentId && qw.presentWait;
    }
    if (g_pw) {
        ext[(*n_ext)++] = VK_KHR_PRESENT_ID_EXTENSION_NAME;
        ext[(*n_ext)++] = VK_KHR_PRESENT_WAIT_EXTENSION_NAME;
    }
    g_hdr_meta = g_colorspace_ext && meta;
    if (g_hdr_meta) ext[(*n_ext)++] = VK_EXT_HDR_METADATA_EXTENSION_NAME;
    // VK_NV_low_latency2 (Reflex's sleep, below) with present ids, when the low-latency setting can be used
    bool ll = false;
    for (uint32_t i = 0; i < ne; i++) ll |= !strcmp(ep[i].extensionName, VK_NV_LOW_LATENCY_2_EXTENSION_NAME);
    g_ll_ext = ll && g_pw;
    g_ll_kept_off = rx && !strcmp(rx, "0");
    (void)vk_ngx_fg_on();   // (its setting read here, on the init thread: ll_mode() calls it from the render thread too)
    if (rx && *rx && *rx != '0') atomic_store(&g_ll_mode, !strcmp(rx, "boost") ? 2 : !strcmp(rx, "stats") ? 3 : !strcmp(rx, "jit") ? 4 : 1);
    if (g_ll_ext) ext[(*n_ext)++] = VK_NV_LOW_LATENCY_2_EXTENSION_NAME;
    for (uint32_t i = 0; i < ne && g_caps2_ext; i++)
        if (!strcmp(ep[i].extensionName, VK_EXT_FULL_SCREEN_EXCLUSIVE_EXTENSION_NAME)) g_fse_ext = true;
    if (g_fse_ext) ext[(*n_ext)++] = VK_EXT_FULL_SCREEN_EXCLUSIVE_EXTENSION_NAME;
    if (g_caps2_ext) LOG("[present] full-screen exclusive: %s\n", g_fse_ext ? "available" : "no VK_EXT_full_screen_exclusive");
    LOG("[present] low latency (VK_NV_low_latency2): %s, %s\n", g_ll_ext ? "available" : ll ? "off (no present wait)" : "not offered",
        atomic_load(&g_ll_mode) == 4 ? "the engine's own sleep (TOTK_REFLEX=jit)" : atomic_load(&g_ll_mode) == 2 ? "on + boost" :
        atomic_load(&g_ll_mode) ? (g_ll_ext ? "on" : "on: the engine's own sleep") : "off");
    LOG("[present] present wait: %s; HDR metadata: %s; HDR request: %s (peak %.0f nits, paper white %.0f nits)\n",
        g_pw ? "on" : want ? (pid && pw ? "off (features missing)" : "off (driver lacks it)") : "off (unused)",
        g_hdr_meta ? "on" : "off", atomic_load(&g_hdr_req) == VKP_HDR10 ? "hdr10" : atomic_load(&g_hdr_req) == VKP_HDR_SCRGB ? "scrgb" : "off",
        g_hdr_peak, g_hdr_paper);
}

void *vk_present_features(void *next) {
    if (!g_pw) return next;
    g_pw_f.pNext = next;
    g_pid_f.pNext = &g_pw_f;
    return &g_pid_f;
}

int vk_present_wait_supported(void) { return g_pw; }

// ================================================================== timing ("[pace]", TOTK_PRESENT_LOG)
// Intervals in ms of the last 2 s, per stream: the window presents (when vkQueuePresentKHR returned; held frames make
// a longer gap, as the player sees), the display (present wait: when the image reached the screen; a frame replaced
// in the mailbox before it was shown completes with the next one and counts as replaced) and the game's presents.
// Session histograms (0.05 ms bins up to 250 ms) give the 1% / 0.1% lows every 10 s.
enum { WN = 2048, HB = 5000 };
typedef struct {
    float v[WN];
    int n;
    uint64_t last;            // ns of the previous event
    uint32_t hist[HB];
    uint64_t total;
    double sum;
} Stream;
static Stream s_present, s_disp, s_game;
static SRWLOCK g_st_lock = SRWLOCK_INIT;
static int w_replaced, w_lat_n;
static double w_lat_sum;
static uint64_t s_replaced, s_shown_skips;

static void stream_add(Stream *s, uint64_t t) {
    if (s->last && t > s->last) {
        float ms = (float)((t - s->last) / 1e6);
        if (s->n < WN) s->v[s->n++] = ms;
        int b = (int)(ms * 20.0f);
        s->hist[b < HB ? b : HB - 1]++;
        s->total++;
        s->sum += ms;
    }
    s->last = t;
}

static int cmp_float(const void *a, const void *b) {
    float x = *(const float *)a, y = *(const float *)b;
    return x < y ? -1 : x > y;
}
// "n avg p50 p99 max sd" of a window (sorts it), into buf
static void stream_line(Stream *s, char *buf, size_t sz) {
    int n = s->n;
    if (!n) { snprintf(buf, sz, "0 frames"); return; }
    qsort(s->v, n, sizeof s->v[0], cmp_float);
    double sum = 0, sq = 0;
    for (int i = 0; i < n; i++) sum += s->v[i];
    double avg = sum / n;
    for (int i = 0; i < n; i++) sq += (s->v[i] - avg) * (s->v[i] - avg);
    int k = n / 100 > 0 ? n / 100 : 1;   // worst 1 % (at least one frame)
    double worst = 0;
    for (int i = n - k; i < n; i++) worst += s->v[i];
    worst /= k;
    snprintf(buf, sz, "%d avg %.2f p50 %.2f p99 %.2f max %.2f sd %.2f ms, 1%% low %.1f fps", n, avg, s->v[n / 2],
             s->v[(int)(n * 0.99) < n ? (int)(n * 0.99) : n - 1], s->v[n - 1], sqrt(sq / n), worst > 0 ? 1000.0 / worst : 0.0);
    s->n = 0;
}
// 1 % and 0.1 % lows of the session: mean of the slowest 1 % / 0.1 % frame times, as fps
static void stream_lows(const Stream *s, double *low1, double *low01, double *p99) {
    *low1 = *low01 = *p99 = 0;
    if (!s->total) return;
    uint64_t k1 = s->total / 100 ? s->total / 100 : 1, k01 = s->total / 1000 ? s->total / 1000 : 1, got = 0;
    double sum1 = 0, sum01 = 0;
    for (int b = HB - 1; b >= 0 && got < k1; b--) {
        if (!s->hist[b]) continue;
        uint64_t take = s->hist[b] < k1 - got ? s->hist[b] : k1 - got;
        double ms = (b + 0.5) / 20.0;
        uint64_t t01 = got < k01 ? (take < k01 - got ? take : k01 - got) : 0;
        sum01 += t01 * ms;
        sum1 += take * ms;
        got += take;
        if (got >= k1 && !*p99) *p99 = ms;
    }
    *low1 = 1000.0 / (sum1 / k1);
    *low01 = 1000.0 / (sum01 / k01);
}

static void log_row(const char *fmt, ...) {
    if (!g_log) return;
    va_list a;
    va_start(a, fmt);
    AcquireSRWLockExclusive(&g_log_lock);
    vfprintf(g_log, fmt, a);
    ReleaseSRWLockExclusive(&g_log_lock);
    va_end(a);
}

// Game clock against wall time (patched main: the game patches' realtime step, games/totk/runtime/totk_patches.c):
// the frame seconds on the data page (TOTK_DP_FRAME_SECONDS, +4) written at a present are the game time of the frame
// that follows, so each one is paired with the wall time to the next present. Intervals over 0.25 s (loading, a
// dragged window) are left out, as the step leaves them out (not owed). 1.000 = real time.
extern void *totk_data_page_current(void);   // totk_patches.c (NULL: unpatched main)
static double w_given, w_real, s_given, s_real;
static float g_last_step;

// hle_vi.cpp vi_present_pace: the game handed over a frame (after its own pacing)
void vk_pace_game_present(void) {
    cfg_init();
    if (!g_stats) return;
    uint64_t t = SDL_GetTicksNS();
    const uint8_t *page = totk_data_page_current();
    float step = page ? *(const volatile float *)(page + 4) : 0.0f;   // (0 until the patches write it)
    AcquireSRWLockExclusive(&g_st_lock);
    if (s_game.last && t > s_game.last && g_last_step > 0) {
        double dt = (t - s_game.last) / 1e9;
        if (dt <= 0.25) { w_given += g_last_step; w_real += dt; s_given += g_last_step; s_real += dt; }
    }
    g_last_step = step;
    stream_add(&s_game, t);
    ReleaseSRWLockExclusive(&g_st_lock);
    log_row("g,%llu,%.6f\n", (unsigned long long)t, step);
}

static void pace_report_locked(uint64_t now);
static void pace_report(uint64_t now) {   // (from the FG present thread too: one caller at a time, the other skips)
    static SRWLOCK rep = SRWLOCK_INIT;
    if (!TryAcquireSRWLockExclusive(&rep)) return;
    pace_report_locked(now);
    ReleaseSRWLockExclusive(&rep);
}
static void pace_report_locked(uint64_t now) {
    static uint64_t last, last_session;
    if (!last) { last = last_session = now; return; }
    if (now - last < 2000000000ull) return;
    char a[160], b[160], c[160];
    AcquireSRWLockExclusive(&g_st_lock);
    stream_line(&s_present, a, sizeof a);
    stream_line(&s_disp, b, sizeof b);
    stream_line(&s_game, c, sizeof c);
    int rep = w_replaced;
    double lat = w_lat_n ? w_lat_sum / w_lat_n : -1;
    w_replaced = 0; w_lat_n = 0; w_lat_sum = 0;
    char clk[64] = "";   // game clock / wall (patched main's realtime step)
    if (w_real > 0) snprintf(clk, sizeof clk, " | game clock %.3f x wall", w_given / w_real);
    double s_clk = s_real > 0 ? s_given / s_real : 0;
    w_given = w_real = 0;
    bool session = now - last_session >= 10000000000ull;
    double l1 = 0, l01 = 0, p99 = 0, d1 = 0, d01 = 0, dp99 = 0;
    if (session) { stream_lows(&s_present, &l1, &l01, &p99); stream_lows(&s_disp, &d1, &d01, &dp99); }
    uint64_t tot = s_present.total, dtot = s_disp.total, srep = s_replaced, skips = s_shown_skips;
    ReleaseSRWLockExclusive(&g_st_lock);
    LOG("[pace] t=%.1f present: %s | display: %s, replaced %d, present->display %.1f ms | game: %s%s | %s, queued<=%d, fif %d%s\n",
        now / 1e9, a, g_pw ? b : "no present wait", rep, lat, c, clk,
        mode_name(atomic_load(&g_mode_active)), atomic_load(&g_max_queued),
        atomic_load(&g_fif), g_hdr_mode ? (g_hdr_mode == VKP_HDR10 ? ", hdr10" : ", scrgb") : "");
    if (session) {
        LOG("[pace] session: present %llu frames, 1%% low %.1f fps, 0.1%% low %.1f fps, p99 %.2f ms; display %llu, 1%% low %.1f, "
            "0.1%% low %.1f fps, p99 %.2f ms, replaced %llu; not shown %llu; game clock %.4f x wall\n", (unsigned long long)tot,
            l1, l01, p99, (unsigned long long)dtot, d1, d01, dp99, (unsigned long long)srep, (unsigned long long)skips, s_clk);
        last_session = now;
    }
    if (g_log) { AcquireSRWLockExclusive(&g_log_lock); fflush(g_log); ReleaseSRWLockExclusive(&g_log_lock); }
    last = now;
}

// ================================================================== present wait (display times, queued-frame limit)
typedef struct { uint64_t id, t_present; VkSwapchainKHR sc; } Pend;
enum { PEND = 64 };
static Pend g_pend[PEND];
static uint64_t g_pend_head, g_pend_tail;   // [tail, head) waiting to be seen on screen
static VkSwapchainKHR g_pw_busy;            // swapchain the waiter is inside vkWaitForPresentKHR on
static uint64_t g_last_id, g_shown_id;      // last present id given; highest one on screen (or given up on)
static SRWLOCK g_pw_lock = SRWLOCK_INIT;
static CONDITION_VARIABLE g_pw_cv = CONDITION_VARIABLE_INIT;

static void display_seen(uint64_t id, uint64_t t, uint64_t t_present) {
    AcquireSRWLockExclusive(&g_st_lock);
    if (s_disp.last && t - s_disp.last < 500000ull) { w_replaced++; s_replaced++; }   // completed with the next one: never shown
    else stream_add(&s_disp, t);
    w_lat_sum += (t - t_present) / 1e6;
    w_lat_n++;
    ReleaseSRWLockExclusive(&g_st_lock);
    log_row("d,%llu,%llu\n", (unsigned long long)id, (unsigned long long)t);
}

// The swapchain is externally synchronised for vkAcquireNextImageKHR, vkWaitForPresentKHR, vkQueuePresentKHR and
// vkCreateSwapchainKHR's oldSwapchain (vk.xml, SDK 1.4.357): the waiter below, the FG present thread and device.c's
// acquire / present / re-create take this lock around those calls. Held for one call each: the waiter and the acquire
// poll (timeout 0, 0.5 ms waits outside the lock) instead of blocking inside the driver, so a present never waits for
// them, and an acquire waiting for an image never holds up the present that frees one. LARECOMP_PW_POLL=0: the waiter's
// old blocking 20 ms waits.
static SRWLOCK g_sc_lock = SRWLOCK_INIT;
void vk_swapchain_lock(void) { AcquireSRWLockExclusive(&g_sc_lock); }
void vk_swapchain_unlock(void) { ReleaseSRWLockExclusive(&g_sc_lock); }
void vk_present_jit_wait(uint64_t ns);   // (below: low latency on any GPU)
static void wait_half_ms(HANDLE timer) {
    LARGE_INTEGER due = {.QuadPart = -5000};   // 0.5 ms (100 ns units, relative)
    if (timer && SetWaitableTimerEx(timer, &due, 0, NULL, NULL, NULL, 0)) WaitForSingleObject(timer, INFINITE);
    else Sleep(1);
}
// device.c vk_frame_begin (render thread): the next window image, polled under the lock up to timeout_ns
VkResult vk_present_acquire(VkSemaphore sem, uint64_t timeout_ns, uint32_t *image) {
    static HANDLE timer;
    if (!timer) timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    for (uint64_t t0 = SDL_GetTicksNS(), tries = 0;; tries++) {
        vk_swapchain_lock();
        VkResult r = vkAcquireNextImageKHR(g_vk.dev, g_vk.swapchain, 0, sem, VK_NULL_HANDLE, image);
        vk_swapchain_unlock();
        const uint64_t el = SDL_GetTicksNS() - t0;
        if ((r != VK_TIMEOUT && r != VK_NOT_READY) || el >= timeout_ns) {
            if (tries) vk_present_jit_wait(el);   // (waited for a window image: FIFO without the queued limit, low latency)
            return r;
        }
        wait_half_ms(timer);
    }
}
// The swapchain's lifetime: create_swapchain takes it exclusive around making and destroying one; the low-latency calls
// take it shared. Not g_sc_lock: the present thread holds that across vkQueuePresentKHR (a FIFO present can block up to a
// refresh), and the game thread's sleep waited behind it every frame.
static SRWLOCK g_sc_life = SRWLOCK_INIT;
void vk_swapchain_life_lock(void) { AcquireSRWLockExclusive(&g_sc_life); }
void vk_swapchain_life_unlock(void) { ReleaseSRWLockExclusive(&g_sc_life); }
static VkResult pw_wait(VkSwapchainKHR sc, uint64_t id) {
    static int poll = -1;
    if (poll < 0) { const char *e = getenv("LARECOMP_PW_POLL"); poll = !e || *e != '0'; }
    if (!poll) return p_wait(g_vk.dev, sc, id, 20000000ull);
    static HANDLE timer;
    if (!timer) timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    VkResult r;
    for (uint64_t t0 = SDL_GetTicksNS();;) {
        vk_swapchain_lock();
        r = p_wait(g_vk.dev, sc, id, 0);
        vk_swapchain_unlock();
        if (r != VK_TIMEOUT || SDL_GetTicksNS() - t0 >= 20000000ull) return r;
        wait_half_ms(timer);
    }
}

// One wait at a time, in 20 ms slices so a swapchain retire never waits long. An id not seen within 1 s is dropped.
static DWORD WINAPI pw_thread(void *arg) {
    (void)arg;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    for (;;) {
        AcquireSRWLockExclusive(&g_pw_lock);
        while (g_pend_tail == g_pend_head) SleepConditionVariableSRW(&g_pw_cv, &g_pw_lock, INFINITE, 0);
        Pend e = g_pend[g_pend_tail % PEND];
        g_pw_busy = e.sc;
        ReleaseSRWLockExclusive(&g_pw_lock);
        VkResult r = pw_wait(e.sc, e.id);
        uint64_t t = SDL_GetTicksNS();
        bool seen = false;
        AcquireSRWLockExclusive(&g_pw_lock);
        g_pw_busy = VK_NULL_HANDLE;
        bool still = g_pend_tail != g_pend_head && g_pend[g_pend_tail % PEND].id == e.id;   // (a retire drops them all)
        if (still && (r != VK_TIMEOUT || t - e.t_present > 1000000000ull)) {
            g_pend_tail++;
            if (e.id > g_shown_id) g_shown_id = e.id;
            seen = r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR;
        }
        WakeAllConditionVariable(&g_pw_cv);
        ReleaseSRWLockExclusive(&g_pw_lock);
        if (seen && g_stats) display_seen(e.id, t, e.t_present);
        if (r != VK_SUCCESS && r != VK_TIMEOUT && r != VK_SUBOPTIMAL_KHR) {
            static int warned;
            if (warned++ < 5) LOG("[present] vkWaitForPresentKHR: %d (present id %llu)\n", r, (unsigned long long)e.id);
        }
    }
    return 0;
}

void vk_present_device_ready(void) {
    if (g_pw) {
        p_wait = (PFN_vkWaitForPresentKHR)vkGetDeviceProcAddr(g_vk.dev, "vkWaitForPresentKHR");
        if (!p_wait) g_pw = false;
        else CloseHandle(CreateThread(NULL, 0, pw_thread, NULL, 0, NULL));
    }
    if (g_hdr_meta) p_hdr_meta = (PFN_vkSetHdrMetadataEXT)vkGetDeviceProcAddr(g_vk.dev, "vkSetHdrMetadataEXT");
}

// device.c vk_frame_end, before vkQueuePresentKHR: chains a present id (0: none)
static VkPresentIdKHR g_pid;
static uint64_t g_pid_val;
// device.c vk_frame_end (from backend_present): which game present a window present carries, for game -> display
// latency (the frame numbers of p rows also count re-presents and run ahead of the game's in async mode)
void vk_present_note(uint64_t id, uint64_t game_ns, bool generated) {
    if (id && game_ns) log_row("l,%llu,%llu,%c\n", (unsigned long long)id, (unsigned long long)game_ns, generated ? 'f' : 'r');
}

// (low latency: present ids per game frame, set when it turns on: 1, or with DLSS-G room for ngx.c's FG_MAX (3)
// generated frames below the real one's)
#define LL_FG_IDS 4
static atomic_int g_ll_scale = 1;
uint64_t vk_present_latency_id(void);   // (below)
uint64_t vk_present_chain(VkPresentInfoKHR *pi, bool generated) {
    uint64_t lid = vk_present_latency_id();   // (low latency: the game frame's id, which its markers carry; 0 none)
    if (lid) {
        // DLSS-G: the generated frames go out first and take the ids just below their real frame's (LL_FG_IDS a frame
        // then), so the real frame keeps the id its markers carry; the generated frame used to take that id itself and
        // leave the real frame none
        const uint64_t lo = lid - (uint64_t)(LL_FG_IDS - 1), next = g_last_id + 1;
        const uint64_t want = !generated ? lid : atomic_load(&g_ll_scale) == 1 ? 0 : next > lo ? next : lo;
        if (!want || want <= g_last_id || (generated && want >= lid)) return 0;   // (a held frame again, or no room)
        g_last_id = want - 1;
    } else if (!g_pw || (!g_stats && !atomic_load(&g_max_queued))) return 0;   // (nothing reads the display times)
    g_pid_val = ++g_last_id;
    g_pid = (VkPresentIdKHR){.sType = VK_STRUCTURE_TYPE_PRESENT_ID_KHR, .pNext = pi->pNext, .swapchainCount = 1, .pPresentIds = &g_pid_val};
    pi->pNext = &g_pid;
    return g_pid_val;
}

// device.c vk_frame_end, after vkQueuePresentKHR: timing, then the latency limits (here, so the next frame starts
// after them: in sync mode this is the game thread just before vi_present_pace, in async mode the render thread,
// which the game follows one present behind)
// (The present itself: timing row and the present-wait entry. On the FG present thread for its presents.)
static void present_record(uint64_t frame, uint64_t id, VkResult r, uint64_t t_submit, uint64_t t_paced, VkSwapchainKHR sc) {
    uint64_t t = SDL_GetTicksNS();
    if (g_stats) {
        AcquireSRWLockExclusive(&g_st_lock);
        stream_add(&s_present, t);
        ReleaseSRWLockExclusive(&g_st_lock);
        log_row("p,%llu,%llu,%llu,%llu,%llu\n", (unsigned long long)frame, (unsigned long long)id, (unsigned long long)t_submit,
                (unsigned long long)t_paced, (unsigned long long)t);
        pace_report(t);
    }
    if (!id) return;
    AcquireSRWLockExclusive(&g_pw_lock);
    if (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR) {
        if (g_pend_head - g_pend_tail >= PEND) g_pend_tail++;   // (waiter stuck: oldest dropped)
        g_pend[g_pend_head++ % PEND] = (Pend){id, t, sc};
        WakeAllConditionVariable(&g_pw_cv);
    } else if (id > g_shown_id) g_shown_id = id;   // (not presented: nothing to wait for)
    ReleaseSRWLockExclusive(&g_pw_lock);
}
// (The latency limits, on the thread that makes the next frame. extra: generated frames that do not count against the
// queued-frame limit, so with frame generation it limits real frames.)
static void present_limits(uint64_t id, uint64_t seq, int extra) {
    const uint64_t t_lim = SDL_GetTicksNS();
    if (id) {
        AcquireSRWLockExclusive(&g_pw_lock);
        int mq = atomic_load(&g_max_queued);
        // (ids LL_FG_IDS a game frame under low latency with DLSS-G, however many generated frames went out: the limit
        // in ids)
        const int sc = atomic_load(&g_ll_scale);
        if (mq > 0) mq = sc > 1 && vk_present_latency_id() ? mq * sc : mq + extra;
        for (uint64_t t0 = SDL_GetTicksNS(); mq > 0 && g_last_id - g_shown_id >= (uint64_t)mq;) {
            uint64_t el = SDL_GetTicksNS() - t0;
            if (el >= 100000000ull) {   // (minimised, occluded: never block a frame for long)
                static int warned;
                if (warned++ < 10) LOG("[present] queued-frame limit: present %llu not on screen after 100 ms\n", (unsigned long long)g_shown_id + 1);
                break;
            }
            SleepConditionVariableSRW(&g_pw_cv, &g_pw_lock, (DWORD)((100000000ull - el) / 1000000ull) + 1, 0);
        }
        ReleaseSRWLockExclusive(&g_pw_lock);
    }
    // frames in flight: the GPU work of the frame fif - 1 back has to be done before the next frame starts
    static uint64_t fseq[8], fn;
    fseq[fn++ & 7] = seq;
    int fif = atomic_load(&g_fif);
    if (fif < 3 && fn >= (uint64_t)fif) vk_wait_seq(fseq[(fn - (uint64_t)fif) & 7]);
    vk_present_jit_wait(SDL_GetTicksNS() - t_lim);   // (a wait after the frame read its input: the engine's low latency)
}
void vk_present_done(uint64_t frame, uint64_t id, uint64_t seq, VkResult r, uint64_t t_submit, uint64_t t_paced) {
    present_record(frame, id, r, t_submit, t_paced, g_vk.swapchain);
    present_limits(id, seq, 0);
}

// device.c backend_present: a frame that was not shown (held, minimised)
void vk_present_skipped(uint64_t frame) {
    if (!g_stats) return;
    uint64_t t = SDL_GetTicksNS();
    AcquireSRWLockExclusive(&g_st_lock);
    s_shown_skips++;
    ReleaseSRWLockExclusive(&g_st_lock);
    log_row("s,%llu,%llu\n", (unsigned long long)frame, (unsigned long long)t);
    pace_report(t);
}

// ================================================================== exclusive fullscreen (shim_win.c window modes)
// The window thread names the monitor (NULL: not exclusive) and asks for a new swapchain; device.c makes it with
// vk_present_swapchain_pnext (application-controlled exclusive on that monitor) and vk_present_swapchain_ready acquires
// it while the window is in front. A lost mode (alt-tab, another fullscreen app) comes back from acquire / present as
// VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT: device.c makes a new swapchain (vk_present_lost).
static void *volatile g_excl_mon;
static bool g_excl_on;   // the current swapchain was made exclusive-capable
static volatile LONG g_excl_refused;
void vk_present_set_exclusive(void *hmonitor) {
    if (g_excl_mon == hmonitor) return;
    g_excl_mon = hmonitor;
    InterlockedExchange(&g_excl_refused, 0);   // (a new mode or monitor: try exclusive again)
    vk_request_swapchain_recreate();
}
static VkSurfaceFullScreenExclusiveWin32InfoEXT g_fse_w32 = {.sType = VK_STRUCTURE_TYPE_SURFACE_FULL_SCREEN_EXCLUSIVE_WIN32_INFO_EXT};
static VkSurfaceFullScreenExclusiveInfoEXT g_fse = {.sType = VK_STRUCTURE_TYPE_SURFACE_FULL_SCREEN_EXCLUSIVE_INFO_EXT,
                                                    .pNext = &g_fse_w32,
                                                    .fullScreenExclusive = VK_FULL_SCREEN_EXCLUSIVE_APPLICATION_CONTROLLED_EXT};
static VkSwapchainLatencyCreateInfoNV g_sc_lat = {.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_LATENCY_CREATE_INFO_NV, .latencyModeEnable = VK_TRUE};
const void *vk_present_swapchain_pnext(void) {
    void *m = g_excl_mon;
    g_excl_on = g_fse_ext && m && !g_excl_refused;
    if (g_excl_on) g_fse_w32.hmonitor = (HMONITOR)m;
    const void *fse = g_excl_on ? &g_fse : NULL;
    if (!g_ll_ext) return fse;
    g_sc_lat.pNext = fse;   // (every swapchain latency-capable: the setting turns the sleep on and off live)
    return &g_sc_lat;
}

// ================================================================== low latency (VK_NV_low_latency2)
// Markers: the driver's sleep works from them. A game frame is numbered on the game thread after its sleep (g_ll_frame);
// its present id is that number + g_ll_base (set when low latency turns on, so ids keep rising past those chained
// before). The game thread marks simulation start / input sample (after the sleep) and simulation end / render submit
// start (at its present); the number travels with the present (exec.c's AOP_PRESENT, or the game thread itself in sync
// mode) to vk_frame_end, which marks render submit end and present start / end and chains that id. Held frames get none.
static atomic_ullong g_ll_frame, g_ll_base, g_ll_present_frame;
static PFN_vkSetLatencyMarkerNV p_ll_mark;
static bool ll_on(void) { const int m = ll_mode(); return g_ll_ext && m > 0 && m < 4 && p_ll_mark; }
// (Under the swapchain's lifetime lock, shared: the render thread's create_swapchain destroys the old one under it
// exclusive, and two callers are on the game thread. None of the callers holds it.)
static void ll_mark(uint64_t frame, VkLatencyMarkerNV m) {
    if (!frame) return;
    AcquireSRWLockShared(&g_sc_life);
    VkSwapchainKHR sc = g_vk.swapchain;
    VkSetLatencyMarkerInfoNV mi = {.sType = VK_STRUCTURE_TYPE_SET_LATENCY_MARKER_INFO_NV,
                                   .presentID = frame * (uint64_t)atomic_load(&g_ll_scale) + atomic_load(&g_ll_base), .marker = m};
    if (sc) p_ll_mark(g_vk.dev, sc, &mi);
    ReleaseSRWLockShared(&g_sc_life);
}
uint64_t vk_present_latency_frame(void) { return ll_on() ? atomic_load(&g_ll_frame) : 0; }   // the game thread's frame
uint64_t vk_present_latency_id(void) {   // vk_present_chain: the present id of the frame being presented (0: none)
    uint64_t f = atomic_load(&g_ll_present_frame);
    return ll_on() && f ? f * (uint64_t)atomic_load(&g_ll_scale) + atomic_load(&g_ll_base) : 0;
}
void vk_present_set_frame(uint64_t frame) { atomic_store(&g_ll_present_frame, frame); }        // the frame being presented
void vk_present_latency_game_present(void) {   // the game thread, at its present
    if (!ll_on()) return;
    uint64_t f = atomic_load(&g_ll_frame);
    ll_mark(f, VK_LATENCY_MARKER_SIMULATION_END_NV);
    ll_mark(f, VK_LATENCY_MARKER_RENDERSUBMIT_START_NV);
}
// vk_frame_end of a game frame: after its last submit (0), before (1) and after (2) vkQueuePresentKHR
void vk_present_latency_present(int stage) {
    uint64_t f = atomic_load(&g_ll_present_frame);
    if (!ll_on() || !f) return;
    ll_mark(f, stage == 0 ? VK_LATENCY_MARKER_RENDERSUBMIT_END_NV : stage == 1 ? VK_LATENCY_MARKER_PRESENT_START_NV : VK_LATENCY_MARKER_PRESENT_END_NV);
}
// Every 10 s: the driver's timings of the frames reported since the last call (polled 4 times a second under the
// swapchain lock; the driver keeps the last 64), as the mean of simulation start -> GPU render end and -> present end
// (Reflex's PC latency, short of the display).
static void latency_report(VkSwapchainKHR sc) {
    static PFN_vkGetLatencyTimingsNV get;
    static uint64_t last_t, polled, last_id, n, sum_gpu, sum_pres;
    static VkLatencyTimingsFrameReportNV rep[64];
    if (!get) get = (PFN_vkGetLatencyTimingsNV)vkGetDeviceProcAddr(g_vk.dev, "vkGetLatencyTimingsNV");
    uint64_t t = SDL_GetTicksNS();
    if (!get || t - polled < 250000000ull) return;
    polled = t;
    for (int i = 0; i < 64; i++) rep[i] = (VkLatencyTimingsFrameReportNV){.sType = VK_STRUCTURE_TYPE_LATENCY_TIMINGS_FRAME_REPORT_NV};
    VkGetLatencyMarkerInfoNV gi = {.sType = VK_STRUCTURE_TYPE_GET_LATENCY_MARKER_INFO_NV, .timingCount = 64, .pTimings = rep};
    get(g_vk.dev, sc, &gi);
    for (uint32_t i = 0; i < gi.timingCount && i < 64; i++) {
        const VkLatencyTimingsFrameReportNV *r = &rep[i];
        if (r->presentID <= last_id || !r->simStartTimeUs || !r->gpuRenderEndTimeUs || !r->presentEndTimeUs) continue;
        last_id = r->presentID;
        if (r->gpuRenderEndTimeUs < r->simStartTimeUs || r->gpuRenderEndTimeUs - r->simStartTimeUs > 1000000) continue;
        sum_gpu += r->gpuRenderEndTimeUs - r->simStartTimeUs, sum_pres += r->presentEndTimeUs - r->simStartTimeUs, n++;
    }
    if (t - last_t >= 10000000000ull) {
        if (last_t && n) LOG("[present] latency (driver, %llu frames): simulation start -> GPU done %.2f ms, -> present done %.2f ms\n",
                             (unsigned long long)n, sum_gpu / 1000.0 / n, sum_pres / 1000.0 / n);
        else if (last_t) LOG("[present] latency (driver): no frame reports\n");
        last_t = t, n = sum_gpu = sum_pres = 0;
    }
}
void vk_present_set_low_latency(int mode) { atomic_store(&g_ll_mode, mode < 0 ? 0 : mode > 4 ? 4 : mode); }
int vk_present_low_latency_supported(void) { return g_ll_ext; }

// ================================================================== low latency on any GPU (the engine's own sleep)
// What a frame waits for after it read its input only adds latency: the game thread's present throttle (exec.c: the
// render thread still busy with the frame before), the cap's wait at the present point (hle_vi.cpp) and the queued-frame
// and frames-in-flight limits (present_limits). The just-in-time sleep moves that wait before the input: before each
// game frame the game thread sleeps for what the frames just before still waited after theirs, less a cushion that
// keeps the render thread, the GPU and the cap's slot fed (down at once when the waits fall under the cushion, up by a
// quarter of the excess a frame). No extension: plain waits. TOTK_REFLEX=jit (mode 4) forces it; on / boost use it
// where VK_NV_low_latency2 is missing. Not with DLSS-G (its present thread paces the frames; Reflex there).
static bool jit_on(void) {
    const int m = ll_mode();
    return m == 4 || ((m == 1 || m == 2) && !g_ll_ext);
}
// The waits since the game thread last read them, by thread: in async exec the render thread's waits (present_limits,
// acquire, the cap at the present point) are what holds the game thread's throttle a frame later, the same slack seen
// twice, so the sleep follows the larger of the two, not their sum. Sync exec: all on the game thread.
static atomic_ullong g_jit_wg, g_jit_wr;
static atomic_ulong g_jit_tid;   // the game thread (jit_sleep's)
void vk_present_jit_wait(uint64_t ns) {
    if (!ns || !jit_on()) return;
    atomic_fetch_add(GetCurrentThreadId() == atomic_load(&g_jit_tid) ? &g_jit_wg : &g_jit_wr, ns);
}
static void jit_sleep(void) {
    static int64_t s;                // the sleep (ns)
    static uint64_t last, period;    // the game frame's interval (running mean)
    static uint64_t log_t, n, sum_s, sum_wg, sum_wr;
    static HANDLE timer;
    static int no_timer;
    atomic_store(&g_jit_tid, GetCurrentThreadId());
    const uint64_t wg = atomic_exchange(&g_jit_wg, 0), wr = atomic_exchange(&g_jit_wr, 0), t = SDL_GetTicksNS();
    if (last && t - last < 1000000000ull) period = period ? (period * 7 + (t - last)) / 8 : t - last;
    last = t;
    if (!jit_on() || no_timer || vk_ngx_fg_on() || !period || period > 50000000ull) { s = 0; return; }   // (off; loads, menus)
    if (!timer) {
        timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (!timer) {   // (Windows before 10 1803: no high-resolution timer; a 15.6 ms sleep would cost more than it saves)
            no_timer = 1;
            LOG("[present] low latency (engine): no high-resolution timer (error %lu), off\n", GetLastError());
            return;
        }
    }
    const int64_t cushion = period / 8 < 500000 ? 500000 : period / 8 > 2000000 ? 2000000 : (int64_t)(period / 8);
    const int64_t e = (int64_t)(wg > wr ? wg : wr) - cushion;
    s += e < 0 ? e : e / 4;
    if (s < 0) s = 0;
    if (s > (int64_t)(period * 3 / 4)) s = (int64_t)(period * 3 / 4);
    n++, sum_s += (uint64_t)s, sum_wg += wg, sum_wr += wr;
    if (t - log_t >= 10000000000ull) {
        if (log_t && n)
            LOG("[present] low latency (engine, any GPU): %llu frames, sleep %.2f ms, waits after input: game thread %.2f ms, "
                "render thread %.2f ms (cushion %.2f)\n", (unsigned long long)n, sum_s / 1e6 / (double)n,
                sum_wg / 1e6 / (double)n, sum_wr / 1e6 / (double)n, cushion / 1e6);
        log_t = t, n = sum_s = sum_wg = sum_wr = 0;
    }
    if (s < 200000) return;   // (under 0.2 ms: not worth a timer)
    LARGE_INTEGER due = {.QuadPart = -(s / 100)};   // (100 ns units, relative)
    if (SetWaitableTimerEx(timer, &due, 0, NULL, NULL, NULL, 0)) WaitForSingleObject(timer, 100);
}

// The game thread, after its present returns and before its next frame (hle_vi.cpp vi_present_pace): Reflex's sleep,
// or the engine's own (above). The sleep mode follows the setting and each new swapchain; under the lifetime lock
// (shared), the semaphore wait outside it.
void vk_present_latency_sleep(void) {
    static PFN_vkSetLatencySleepModeNV set_mode;
    static PFN_vkLatencySleepNV sleep;
    static VkSemaphore sem;
    static uint64_t val;
    static VkSwapchainKHR applied_sc;
    static int applied = -1;
    jit_sleep();
    if (!g_ll_ext || !g_vk.dev) return;
    int mode = ll_mode();
    if (mode == 4) mode = 0;   // (the engine's sleep: the driver's mode off)
    if (!mode && applied <= 0) return;
    if (!sem) {
        set_mode = (PFN_vkSetLatencySleepModeNV)vkGetDeviceProcAddr(g_vk.dev, "vkSetLatencySleepModeNV");
        sleep = (PFN_vkLatencySleepNV)vkGetDeviceProcAddr(g_vk.dev, "vkLatencySleepNV");
        p_ll_mark = (PFN_vkSetLatencyMarkerNV)vkGetDeviceProcAddr(g_vk.dev, "vkSetLatencyMarkerNV");
        VkSemaphoreTypeCreateInfo tc = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO, .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE};
        VkSemaphoreCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &tc};
        if (!set_mode || !sleep || vkCreateSemaphore(g_vk.dev, &ci, NULL, &sem) != VK_SUCCESS) { g_ll_ext = false; return; }
    }
    bool slept = false;
    AcquireSRWLockShared(&g_sc_life);
    VkSwapchainKHR sc = g_vk.swapchain;
    if (sc && (sc != applied_sc || mode != applied)) {
        VkLatencySleepModeInfoNV mi = {.sType = VK_STRUCTURE_TYPE_LATENCY_SLEEP_MODE_INFO_NV, .lowLatencyMode = mode == 1 || mode == 2,
                                       .lowLatencyBoost = mode == 2, .minimumIntervalUs = 0};
        VkResult r = set_mode(g_vk.dev, sc, &mi);
        if (mode && applied <= 0) {   // (ids above every one chained so far; LL_FG_IDS a frame with DLSS-G, vk_present_chain)
            atomic_store(&g_ll_scale, vk_ngx_fg_on() ? LL_FG_IDS : 1);
            atomic_store(&g_ll_base, g_last_id + 1);
        }
        if (mode != applied)
            LOG("[present] low latency %s (vkSetLatencySleepModeNV %d), present ids %d a frame\n",
                mode == 3 ? "markers only (TOTK_REFLEX=stats)" : mode == 2 ? "on + boost" : mode ? "on" : "off", r,
                atomic_load(&g_ll_scale));
        applied_sc = sc, applied = mode;
    }
    if (sc && mode) latency_report(sc);
    if (sc && (mode == 1 || mode == 2)) {
        VkLatencySleepInfoNV si = {.sType = VK_STRUCTURE_TYPE_LATENCY_SLEEP_INFO_NV, .signalSemaphore = sem, .value = ++val};
        slept = sleep(g_vk.dev, sc, &si) == VK_SUCCESS;
    }
    ReleaseSRWLockShared(&g_sc_life);
    if (slept) {
        VkSemaphoreWaitInfo wi = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, .semaphoreCount = 1, .pSemaphores = &sem, .pValues = &val};
        uint64_t t0 = SDL_GetTicksNS();
        vkWaitSemaphores(g_vk.dev, &wi, 100000000ull);   // (100 ms at most: a minimised window, a lost swapchain)
        static uint64_t sum, n, last;
        sum += SDL_GetTicksNS() - t0, n++;
        if (t0 - last > 10000000000ull) {   // every 10 s: how long the sleeps were
            if (last) LOG("[present] low latency: %llu sleeps, %.2f ms each on average\n", (unsigned long long)n, sum / 1e6 / (double)n);
            last = t0, sum = n = 0;
        }
    }
    if (mode && p_ll_mark) {   // the next game frame starts here, and reads its input
        uint64_t f = atomic_fetch_add(&g_ll_frame, 1) + 1;
        ll_mark(f, VK_LATENCY_MARKER_SIMULATION_START_NV);
        ll_mark(f, VK_LATENCY_MARKER_INPUT_SAMPLE_NV);
    }
}

// ================================================================== FG present thread With frame generation the render
// thread slept until each generated frame's due time and presented inline: a frame that came after its slot went out
// just before its real one (mailbox replaced it, or it showed for one refresh), and any wait there delayed the next
// frame. Now each window present goes to this thread, which presents it on a queue of its own (the same family;
// device.c) at its due time, while the render thread acquires, records and submits as before and goes on. With I = the
// real frames' interval (the cap's period while it paces them, or the mean of their arrivals if longer) and n generated
// frames per real one:
//  - generated frame k at k slots (I / (n + 1)) after the last real frame's arrival, or when it arrives if that is
//    later;
//  - the real frame as it comes (the cap paces it on the render thread first: vi_pace_present_point, TOTK_PACE_AT),
//    but not less than half a slot after the generated one;
//  - nothing waits more than one slot after it arrives (so a schedule ahead of the frames cannot hold every real frame
//    for a whole interval, 34 ms at 30 fps);
//  - any other present (FG paused: a menu, no scene; the start screen) at once.
// The queued-frame and frames-in-flight limits stay on the render thread (present_limits after the push). Opt-in,
// TOTK_FG_PRESENT_THREAD=1: in seven exclusive rounds it matched the inline presenter's latency but not its placement
// (Kakariko 4-7 % of generated frames bunched against 2-4 %).
typedef struct {
    VkSwapchainKHR sc;
    VkSemaphore wait;
    uint32_t img;
    int kind, k, n;   // kind: 0 not a game frame, 1 real, 2 generated (k of n)
    uint64_t id, frame, t_submit, t_push, ll_frame;
} PqItem;
enum { PQN = 16 };
static PqItem g_pq[PQN];
static uint64_t g_pq_head, g_pq_tail;   // [tail, head) to present
static bool g_pq_busy;                  // the thread is between taking an item and having presented it
static SRWLOCK g_pq_lock = SRWLOCK_INIT;
static CONDITION_VARIABLE g_pq_cv = CONDITION_VARIABLE_INIT;
static VkQueue g_pq_queue;
static atomic_int g_pq_result;          // swapchain trouble since the render thread last looked (0: none)

static void pq_wait_until(HANDLE timer, uint64_t due) {   // (high-resolution timer to 0.6 ms before, then spin)
    for (;;) {
        uint64_t now = SDL_GetTicksNS();
        if (now >= due) return;
        if (due - now <= 600000ull) break;
        LARGE_INTEGER rel = {.QuadPart = -(LONGLONG)((due - now - 600000ull) / 100)};
        if (timer && SetWaitableTimerEx(timer, &rel, 0, NULL, NULL, NULL, 0)) WaitForSingleObject(timer, INFINITE);
        else Sleep(1);
    }
    while (SDL_GetTicksNS() < due) YieldProcessor();
}

static DWORD WINAPI pq_thread(void *arg) {
    (void)arg;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    HANDLE timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    // (last real frame's arrival, the running mean of their intervals and the interval used; gen_t: last generated one
    // presented)
    uint64_t last_arrival = 0, mean = 0, interval = 0, gen_t = 0;
    // every 10 s: generated frames, how many arrived after their slot / presented over 1 ms past due; real frames and
    // their hold after arrival
    uint64_t st_t = 0, st_gen = 0, st_after = 0, st_real = 0, st_hold = 0, st_past = 0;
    for (;;) {
        AcquireSRWLockExclusive(&g_pq_lock);
        while (g_pq_tail == g_pq_head) SleepConditionVariableSRW(&g_pq_cv, &g_pq_lock, INFINITE, 0);
        PqItem e = g_pq[g_pq_tail % PQN];
        g_pq_busy = true;
        g_pq_tail++;
        ReleaseSRWLockExclusive(&g_pq_lock);
        uint64_t due = 0;
        {   // (the cap's period for the real frames when it paces them, else the running mean of their arrivals; the
            // longer of the two: a rung the game does not hold arrives slower than its period)
            extern int64_t vi_pace_period_ns(void);
            const int64_t pp = vi_pace_period_ns();
            interval = pp > 0 && (uint64_t)pp > mean ? (uint64_t)pp : mean;
        }
        if (e.kind == 1) {
            if (last_arrival && e.t_push > last_arrival) {
                uint64_t d = e.t_push - last_arrival;
                if (d < 100000000ull) mean = mean ? (mean * 4 + d) / 5 : d;
            }
            last_arrival = e.t_push;
            // (as it comes: the cap already paced it; held only when the generated frame went out less than half a slot
            // before, so that one still shows. A full slot after it held the real frames 3-7 ms, +5 ms game -> screen.)
            if (e.n > 0 && gen_t && interval) {
                uint64_t half = interval / (uint64_t)(e.n + 1) / 2;
                due = gen_t + half;
                if (due > e.t_push + half) due = e.t_push + half;
            }
            gen_t = 0;
        } else if (e.kind == 2 && interval) {
            uint64_t slot = interval / (uint64_t)(e.n + 1);
            // (k slots from the last real frame's arrival, not its present: the cap releases real frames on its own
            // clock, and a slot counted from the present carried every hold into the next frame, +6 ms game ->
            // screen. Aiming at the middle of a refresh interval did not help: in a window, present wait's times
            // are not on the refresh grid, so there was no phase to aim at.)
            due = e.k > 1 && gen_t ? gen_t + slot : last_arrival ? last_arrival + slot : e.t_push;
            if (due < e.t_push) due = e.t_push, st_after++;
            if (due > e.t_push + slot) due = e.t_push + slot;
            st_gen++;
        }
        if (due && due > SDL_GetTicksNS() && due - SDL_GetTicksNS() < 100000000ull) pq_wait_until(timer, due);
        uint64_t t_paced = SDL_GetTicksNS();
        if (e.kind == 2 && due && t_paced > due + 1000000ull) st_past++;
        if (e.kind == 2) gen_t = t_paced;
        VkPresentIdKHR pid = {.sType = VK_STRUCTURE_TYPE_PRESENT_ID_KHR, .swapchainCount = 1, .pPresentIds = &e.id};
        VkPresentInfoKHR pi = {.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .pNext = e.id ? &pid : NULL, .waitSemaphoreCount = 1,
                               .pWaitSemaphores = &e.wait, .swapchainCount = 1, .pSwapchains = &e.sc, .pImageIndices = &e.img};
        ll_mark(e.ll_frame, VK_LATENCY_MARKER_PRESENT_START_NV);
        vk_swapchain_lock();
        VkResult r = vkQueuePresentKHR(g_pq_queue, &pi);
        vk_swapchain_unlock();
        ll_mark(e.ll_frame, VK_LATENCY_MARKER_PRESENT_END_NV);
        present_record(e.frame, e.id, r, e.t_submit, t_paced, e.sc);
        if (r == VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT || r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) {
            int cur = atomic_load(&g_pq_result);   // (lost > out of date > suboptimal: what the render thread acts on)
            if (cur != VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT && (r != VK_SUBOPTIMAL_KHR || !cur)) atomic_store(&g_pq_result, r);
        } else if (r != VK_SUCCESS) {
            static int warned;
            if (warned++ < 10) LOG("[present] FG present thread: vkQueuePresentKHR %d\n", r);
        }
        if (e.kind == 1) st_real++, st_hold += t_paced - e.t_push;
        AcquireSRWLockExclusive(&g_pq_lock);
        g_pq_busy = false;
        WakeAllConditionVariable(&g_pq_cv);
        ReleaseSRWLockExclusive(&g_pq_lock);
        if (!st_t) st_t = t_paced;
        if (t_paced - st_t >= 10000000000ull) {
            if (st_gen)
                LOG("[present] FG present thread, %.1f s: %llu generated (%llu after their slot), %llu real held %.2f ms avg after "
                    "arriving, real interval %.2f ms, %llu generated over 1 ms past due\n", (t_paced - st_t) / 1e9,
                    (unsigned long long)st_gen, (unsigned long long)st_after, (unsigned long long)st_real,
                    st_real ? st_hold / 1e6 / (double)st_real : 0.0, interval / 1e6, (unsigned long long)st_past);
            st_t = t_paced, st_gen = st_after = st_real = st_hold = st_past = 0;
        }
    }
    return 0;
}

// device.c create_device: the second queue of the graphics family, asked for with TOTK_FG_PRESENT_THREAD=1 (VK_NULL_HANDLE:
// none; FG then presents inline, the default)
bool vk_present_queue_wanted(void) {
    const char *e = getenv("TOTK_FG_PRESENT_THREAD");
    return e && *e == '1';
}
void vk_present_queue_init(VkQueue q) {
    if (!q || !vk_present_queue_wanted()) return;
    g_pq_queue = q;
    LOG("[present] FG present thread on (TOTK_FG_PRESENT_THREAD=1)\n");
    CloseHandle(CreateThread(NULL, 0, pq_thread, NULL, 0, NULL));
}
bool vk_present_queue_on(void) { return g_pq_queue && vk_ngx_fg_on(); }
// device.c vk_frame_end instead of vkQueuePresentKHR (its submit already in the queue: vkq_drain before)
void vk_present_queue_push(uint32_t img, VkSemaphore wait, uint64_t id, uint64_t frame, uint64_t seq, uint64_t t_submit,
                           int kind, int k, int n) {
    PqItem e = {.sc = g_vk.swapchain, .wait = wait, .img = img, .kind = kind, .k = k, .n = n, .id = id, .frame = frame,
                .t_submit = t_submit, .t_push = SDL_GetTicksNS(),
                .ll_frame = kind == 1 && ll_on() ? atomic_load(&g_ll_present_frame) : 0};
    AcquireSRWLockExclusive(&g_pq_lock);
    while (g_pq_head - g_pq_tail >= PQN) SleepConditionVariableSRW(&g_pq_cv, &g_pq_lock, INFINITE, 0);   // (never with 8 images)
    g_pq[g_pq_head++ % PQN] = e;
    WakeAllConditionVariable(&g_pq_cv);
    ReleaseSRWLockExclusive(&g_pq_lock);
    present_limits(id, seq, kind ? n : 0);
}
// Before the swapchain is made again or the device waits idle (both need the queue and the swapchain to themselves):
// everything pushed presented. 500 ms at most (a present the compositor holds; then on as before).
void vk_present_queue_drain(void) {
    if (!g_pq_queue) return;
    AcquireSRWLockExclusive(&g_pq_lock);
    for (uint64_t t0 = SDL_GetTicksNS(); (g_pq_tail != g_pq_head || g_pq_busy) && SDL_GetTicksNS() - t0 < 500000000ull;)
        SleepConditionVariableSRW(&g_pq_cv, &g_pq_lock, 10, 0);
    ReleaseSRWLockExclusive(&g_pq_lock);
}
VkResult vk_present_queue_result(void) { return (VkResult)atomic_exchange(&g_pq_result, 0); }
// Exclusive refused (the window not in front, or acquire failed): the next swapchains are plain (borderless) until the
// window is activated again (shim_win.c calls vk_present_exclusive_retry). Before, an exclusive-capable swapchain that
// was never acquired came back from every present as MODE_LOST, and each new one was refused again: a new swapchain
// every frame.
void vk_present_exclusive_refused(void) {   // device.c: the swapchain was made again without the extension chain
    InterlockedExchange(&g_excl_refused, 1);
    g_excl_on = false;
}
void vk_present_exclusive_retry(void) {
    if (InterlockedExchange(&g_excl_refused, 0) && g_excl_mon) vk_request_swapchain_recreate();
}
static void exclusive_acquire(void) {
    static PFN_vkAcquireFullScreenExclusiveModeEXT acq;
    if (!g_excl_on) return;
    if (!acq) acq = (PFN_vkAcquireFullScreenExclusiveModeEXT)vkGetDeviceProcAddr(g_vk.dev, "vkAcquireFullScreenExclusiveModeEXT");
    extern int shim_window_active(void);
    VkResult r = !acq ? VK_ERROR_EXTENSION_NOT_PRESENT : !shim_window_active() ? VK_NOT_READY : acq(g_vk.dev, g_vk.swapchain);
    static int logs;
    if (logs++ < 20) LOG("[present] full-screen exclusive: %s (%d)\n", r == VK_SUCCESS ? "acquired" : "not acquired, borderless until the window is activated again", r);
    if (r != VK_SUCCESS) {
        InterlockedExchange(&g_excl_refused, 1);
        vk_request_swapchain_recreate();
    }
}
// device.c: acquire / present returned VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT
void vk_present_lost(void) {
    static int logs;
    if (logs++ < 20) LOG("[present] full-screen exclusive lost: new swapchain\n");
}

// ================================================================== swapchain (device.c create_swapchain)
uint32_t vk_present_image_count(const VkSurfaceCapabilitiesKHR *caps) {
    cfg_init();
    // (FG present thread: a generated and a real image wait there while the next frame acquires: two more)
    uint32_t count = (uint32_t)env_int("TOTK_SWAPCHAIN_IMAGES", (int)caps->minImageCount + (vk_present_queue_on() ? 3 : 1), 2, 8);
    if (count < caps->minImageCount) count = caps->minImageCount;
    if (caps->maxImageCount && count > caps->maxImageCount) count = caps->maxImageCount;
    return count > 8 ? 8 : count;
}

// The HDR request picks the swapchain format: HDR10 = 10-bit UNORM + ST 2084 (PQ), scRGB = FP16 + extended linear sRGB.
// Not offered (Windows HDR off on that monitor, no colorspace extension): SDR as before, said once per change.
void vk_present_pick_format(const VkSurfaceFormatKHR *fmts, uint32_t nf, VkSurfaceFormatKHR *sf) {
    cfg_init();
    int want = atomic_load(&g_hdr_req), got = -1, found[3] = {-1, -1, -1};
    for (uint32_t i = 0; i < nf; i++) {
        if ((fmts[i].format == VK_FORMAT_A2B10G10R10_UNORM_PACK32 || fmts[i].format == VK_FORMAT_A2R10G10B10_UNORM_PACK32) &&
            fmts[i].colorSpace == VK_COLOR_SPACE_HDR10_ST2084_EXT && found[VKP_HDR10] < 0) found[VKP_HDR10] = (int)i;
        if (fmts[i].format == VK_FORMAT_R16G16B16A16_SFLOAT && fmts[i].colorSpace == VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT &&
            found[VKP_HDR_SCRGB] < 0) found[VKP_HDR_SCRGB] = (int)i;
    }
    g_hdr_sup[VKP_HDR10] = found[VKP_HDR10] >= 0;
    g_hdr_sup[VKP_HDR_SCRGB] = found[VKP_HDR_SCRGB] >= 0;
    g_fmts_known = true;
    if (want != VKP_HDR_OFF) got = found[want];
    static int told = -2;
    if (got >= 0) *sf = fmts[got];
    if (want * 2 + (got >= 0) != told) {
        told = want * 2 + (got >= 0);
        if (want != VKP_HDR_OFF)
            LOG("[present] HDR %s: %s (surface: hdr10 %s, scrgb %s)\n", want == VKP_HDR10 ? "hdr10" : "scrgb",
                got >= 0 ? "on" : "not offered by the surface (is Windows HDR on for this monitor?), SDR",
                g_hdr_sup[VKP_HDR10] ? "yes" : "no", g_hdr_sup[VKP_HDR_SCRGB] ? "yes" : "no");
    }
    g_hdr_mode = got >= 0 ? want : VKP_HDR_OFF;
}

static VkImage g_real_img[8], g_canvas;
static VkImageView g_real_view[8], g_canvas_view;
static VmaAllocation g_canvas_mem;
static VkFormat g_real_fmt;
static uint32_t g_real_count;

// After vkCreateSwapchainKHR, before device.c destroys the old views and swapchain: no wait left on the old swapchain,
// the real views back in g_vk (device.c destroys those), the canvas freed.
void vk_present_swapchain_retire(void) {
    AcquireSRWLockExclusive(&g_pw_lock);
    g_pend_tail = g_pend_head;
    g_shown_id = g_last_id;
    while (g_pw_busy) SleepConditionVariableSRW(&g_pw_cv, &g_pw_lock, INFINITE, 0);
    WakeAllConditionVariable(&g_pw_cv);
    ReleaseSRWLockExclusive(&g_pw_lock);
    if (g_canvas) {
        for (uint32_t i = 0; i < g_real_count; i++) { g_vk.sc_images[i] = g_real_img[i]; g_vk.sc_views[i] = g_real_view[i]; }
        g_vk.sc_format = g_real_fmt;
        vk_defer_view(g_canvas_view);
        vk_defer_image(g_canvas, g_canvas_mem);
        g_canvas = VK_NULL_HANDLE; g_canvas_view = VK_NULL_HANDLE; g_canvas_mem = NULL;
    }
}

// The new swapchain's images and views are in g_vk. HDR: they move here, and g_vk gets the SDR canvas in every slot.
void vk_present_swapchain_ready(VkSurfaceFormatKHR sf, int mode) {
    atomic_store(&g_mode_active, mode);
    LOG("[present] swapchain %ux%u, %u images, %s, format %d colour space %d%s%s\n", g_vk.sc_extent.width, g_vk.sc_extent.height,
        g_vk.sc_count, mode_name(mode), sf.format, sf.colorSpace,
        g_hdr_mode == VKP_HDR10 ? " (HDR10)" : g_hdr_mode == VKP_HDR_SCRGB ? " (scRGB)" : "", g_excl_on ? ", exclusive-capable" : "");
    exclusive_acquire();
    if (!g_hdr_mode) return;
    g_real_fmt = sf.format;
    g_real_count = g_vk.sc_count;
    for (uint32_t i = 0; i < g_real_count; i++) { g_real_img[i] = g_vk.sc_images[i]; g_real_view[i] = g_vk.sc_views[i]; }
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                            .format = VK_FORMAT_B8G8R8A8_UNORM, .extent = {g_vk.sc_extent.width, g_vk.sc_extent.height, 1},
                            .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                     VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                            .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (vmaCreateImage(g_vk.vma, &ci, &ai, &g_canvas, &g_canvas_mem, NULL) != VK_SUCCESS) {
        LOG("[present] HDR canvas %ux%u: no memory; SDR\n", g_vk.sc_extent.width, g_vk.sc_extent.height);
        g_canvas = VK_NULL_HANDLE;
        g_hdr_mode = VKP_HDR_OFF;   // (the swapchain stays HDR-encoded: the next recreate goes back to SDR)
        atomic_store(&g_hdr_req, VKP_HDR_OFF);
        vk_request_swapchain_recreate();
        return;
    }
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = g_canvas, .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                .format = VK_FORMAT_B8G8R8A8_UNORM, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VK_CHECK(vkCreateImageView(g_vk.dev, &vi, NULL, &g_canvas_view));
    for (uint32_t i = 0; i < 8; i++) { g_vk.sc_images[i] = g_canvas; g_vk.sc_views[i] = g_canvas_view; }
    g_vk.sc_format = VK_FORMAT_B8G8R8A8_UNORM;
    if (p_hdr_meta) {   // mastering display = what the expansion aims at
        VkHdrMetadataEXT md = {.sType = VK_STRUCTURE_TYPE_HDR_METADATA_EXT,
                               .displayPrimaryRed = {0.708f, 0.292f}, .displayPrimaryGreen = {0.170f, 0.797f},
                               .displayPrimaryBlue = {0.131f, 0.046f}, .whitePoint = {0.3127f, 0.3290f},
                               .maxLuminance = g_hdr_peak, .minLuminance = 0.0f,
                               .maxContentLightLevel = g_hdr_peak, .maxFrameAverageLightLevel = g_hdr_paper};
        p_hdr_meta(g_vk.dev, 1, &g_vk.swapchain, &md);
    }
}

// ================================================================== HDR encode pass
// Input: the canvas, gamma-encoded SDR (the bytes the game wrote). Linear (sRGB curve, or TOTK_HDR_GAMMA=2.2), then
// an inverse tonemap on luminance: y' = y (1 + k y^e), k = peak / paper - 1, so black and mid-tones stay at paper white's
// scale (18 % grey: x1.01 at e = 3, 1000 / 200 nits), SDR white reaches the peak, and the hue is kept (one ratio for
// r, g, b). Then nits = paper * colour. HDR10: BT.709 -> BT.2020 primaries, PQ. scRGB: 1.0 = 80 nits, BT.709.
static const char *hdr_vs =
    "#version 450\n"
    "void main() {\n"
    "  vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);\n"
    "  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n";
static const char *hdr_fs =
    "#version 450\n"
    "layout(set = 0, binding = 0) uniform sampler2D t;\n"
    "layout(push_constant) uniform P { vec4 a; vec4 b; } pc;\n"   // a: mode, paper, peak, exponent; b.x: gamma 2.2
    "layout(location = 0) out vec4 o;\n"
    "vec3 to_linear(vec3 c) {\n"
    "  if (pc.b.x > 0.5) return pow(c, vec3(2.2));\n"
    "  return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), c));\n"
    "}\n"
    "vec3 pq(vec3 nits) {\n"
    "  vec3 y = pow(clamp(nits / 10000.0, 0.0, 1.0), vec3(0.1593017578125));\n"
    "  return pow((0.8359375 + 18.8515625 * y) / (1.0 + 18.6875 * y), vec3(78.84375));\n"
    "}\n"
    "void main() {\n"
    "  vec3 lin = to_linear(clamp(texelFetch(t, ivec2(gl_FragCoord.xy), 0).rgb, 0.0, 1.0));\n"
    "  float y = clamp(dot(lin, vec3(0.2126, 0.7152, 0.0722)), 0.0, 1.0);\n"
    "  float k = max(pc.a.z / pc.a.y - 1.0, 0.0);\n"
    "  vec3 nits = lin * (1.0 + k * pow(y, pc.a.w)) * pc.a.y;\n"
    "  if (pc.a.x < 1.5) {\n"
    "    const mat3 to2020 = mat3(0.6274, 0.0691, 0.0164, 0.3293, 0.9195, 0.0880, 0.0433, 0.0114, 0.8956);\n"
    "    o = vec4(pq(max(to2020 * nits, vec3(0.0))), 1.0);\n"
    "  } else o = vec4(nits / 80.0, 1.0);\n"
    "}\n";

static VkPipelineLayout g_hdr_layout;
static VkDescriptorSetLayout g_hdr_set_layout;

static VkPipeline hdr_pipeline(VkFormat fmt) {
    static struct { VkFormat f; VkPipeline p; } cache[4];
    for (int i = 0; i < 4; i++) if (cache[i].p && cache[i].f == fmt) return cache[i].p;
    if (!g_hdr_layout) {
        VkDescriptorSetLayoutBinding b = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL};
        VkDescriptorSetLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &b};
        VK_CHECK(vkCreateDescriptorSetLayout(g_vk.dev, &li, NULL, &g_hdr_set_layout));
        VkPushConstantRange r = {VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32};
        VkPipelineLayoutCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                         .pSetLayouts = &g_hdr_set_layout, .pushConstantRangeCount = 1, .pPushConstantRanges = &r};
        VK_CHECK(vkCreatePipelineLayout(g_vk.dev, &ci, NULL, &g_hdr_layout));
    }
    static VkShaderModule vs, fs;
    if (!vs) vs = vk_compile_glsl(hdr_vs, STAGE_VERTEX, "hdr_encode.vert");
    if (!fs) fs = vk_compile_glsl(hdr_fs, STAGE_FRAGMENT, "hdr_encode.frag");
    if (!vs || !fs) return VK_NULL_HANDLE;
    VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs, .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs, .pName = "main"}};
    VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                 .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo vps = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                 .polygonMode = VK_POLYGON_MODE_FILL, .lineWidth = 1};
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                               .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState cba = {.colorWriteMask = 0xF};
    VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1,
                                              .pAttachments = &cba};
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 2, .pDynamicStates = dyn};
    VkPipelineRenderingCreateInfo ri = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = 1,
                                        .pColorAttachmentFormats = &fmt};
    VkPipelineDepthStencilStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkGraphicsPipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &ri, .stageCount = 2,
                                       .pStages = stages, .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vps,
                                       .pRasterizationState = &rs, .pMultisampleState = &ms, .pDepthStencilState = &ds,
                                       .pColorBlendState = &cb, .pDynamicState = &dy, .layout = g_hdr_layout};
    VkPipeline p;
    VK_CHECK(vkCreateGraphicsPipelines(g_vk.dev, g_vk.pcache, 1, &pi, NULL, &p));
    for (int i = 0; i < 4; i++) if (!cache[i].p) { cache[i].f = fmt; cache[i].p = p; break; }
    return p;
}

static void barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                               .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
                               .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                               .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                               .oldLayout = from, .newLayout = to, .image = img,
                               .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
                               .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b};
    vkCmdPipelineBarrier2(cb, &di);
}

// device.c vk_frame_end, in place of the window image's PRESENT_SRC barrier: false when HDR is off (nothing recorded).
// The canvas is in COLOR_ATTACHMENT_OPTIMAL, outside rendering.
bool vk_present_hdr_encode(VkCommandBuffer cb, uint32_t image) {
    if (!g_hdr_mode || !g_canvas || image >= g_real_count) return false;
    VkPipeline p = hdr_pipeline(g_real_fmt);
    VkImage dst = g_real_img[image];
    barrier(cb, g_canvas, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    barrier(cb, dst, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo ca = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = g_real_view[image],
                                    .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                    .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, g_vk.sc_extent}, .layerCount = 1,
                          .colorAttachmentCount = 1, .pColorAttachments = &ca};
    vkCmdBeginRendering(cb, &ri);
    if (p) {
        VkViewport vp = {0, 0, (float)g_vk.sc_extent.width, (float)g_vk.sc_extent.height, 0, 1};
        VkRect2D sc = {{0, 0}, g_vk.sc_extent};
        vkCmdSetViewport(cb, 0, 1, &vp);
        vkCmdSetScissor(cb, 0, 1, &sc);
        VkDescriptorSet set = vk_alloc_set(g_hdr_set_layout);
        VkDescriptorImageInfo ii = {.sampler = vk_default_sampler(false), .imageView = g_canvas_view,
                                    .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet w = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 0, .descriptorCount = 1,
                                  .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii};
        vkUpdateDescriptorSets(g_vk.dev, 1, &w, 0, NULL);
        float pc[8] = {(float)g_hdr_mode, g_hdr_paper, g_hdr_peak, g_hdr_expand, g_hdr_gamma22 ? 1.0f : 0.0f, 0, 0, 0};
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_hdr_layout, 0, 1, &set, 0, NULL);
        vkCmdPushConstants(cb, g_hdr_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, pc);
        vkCmdDraw(cb, 3, 1, 0, 0);
    }   // (no pipeline: the shader failed to build, logged; the frame is black rather than wrong)
    vkCmdEndRendering(cb);
    barrier(cb, dst, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    { extern void vk_dyn_invalidate(void); vk_dyn_invalidate(); }
    return true;
}

// ================================================================== HDR probe (TOTK_HDR_PROBE=<frame>[,<frame>...])
// The encode pass above on chosen frames into offscreen HDR10 (A2B10G10R10, PQ) and scRGB (FP16) targets, whether or not
// the swapchain is HDR (this PC's monitors offer none), with the settings in force. The SDR frame and both encodings are
// read back and written as raw files beside the log (hdr_probe_<frame>_sdr.bgra / _hdr10.a2b10g10r10 / _scrgb.rgba16f
// and .json) for tools/present/hdr_check.py, which decodes them and checks them against the curve. device.c
// vk_frame_end calls it before the encode and saves after the frame's GPU work is done.
typedef struct { VkImage img; VmaAllocation mem; VkImageView view; } ProbeImg;
static ProbeImg probe_img(VkFormat f, VkImageUsageFlags usage, uint32_t w, uint32_t h) {
    ProbeImg p = {0};
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = f,
                            .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage, .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (vmaCreateImage(g_vk.vma, &ci, &ai, &p.img, &p.mem, NULL) != VK_SUCCESS) return (ProbeImg){0};
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = p.img, .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                .format = f, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VK_CHECK(vkCreateImageView(g_vk.dev, &vi, NULL, &p.view));
    return p;
}
static void probe_free(ProbeImg *p) {
    if (p->view) vk_defer_view(p->view);
    if (p->img) vk_defer_image(p->img, p->mem);
    *p = (ProbeImg){0};
}
static VkSpan g_probe_out[3];
static uint64_t g_probe_frame;
static uint32_t g_probe_w, g_probe_h;
static VkFormat g_probe_src_fmt;
static uint64_t g_probe_at[16];   // the frames asked for, in order; a probe runs at the first free frame at or after one
static int g_probe_n, g_probe_next;
static bool probe_wanted(uint64_t frame) {
    static int init;
    if (!init) {
        init = 1;
        const char *e = getenv("TOTK_HDR_PROBE");
        for (char *q = (char *)e; q && *q && g_probe_n < 16;) {
            g_probe_at[g_probe_n++] = strtoull(q, &q, 10);
            while (*q == ',' || *q == ' ') q++;
        }
    }
    return g_probe_next < g_probe_n && frame >= g_probe_at[g_probe_next];
}
// src: the finished SDR frame (the window image or the HDR canvas), in COLOR_ATTACHMENT_OPTIMAL; true when recorded.
// busy: this frame already reads back (a shot): vk_readback is one buffer at offset 0, so the probe waits a frame.
bool vk_present_hdr_probe(VkCommandBuffer cb, VkImage src, VkFormat src_fmt, uint64_t frame, bool busy) {
    if (busy || !probe_wanted(frame) || g_probe_frame) return false;
    g_probe_next++;
    const uint32_t w = g_vk.sc_extent.width, h = g_vk.sc_extent.height;
    ProbeImg in = probe_img(VK_FORMAT_B8G8R8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, w, h);
    ProbeImg t[2] = {probe_img(VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, w, h),
                     probe_img(VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, w, h)};
    const VkFormat tf[2] = {VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_FORMAT_R16G16B16A16_SFLOAT};
    VkPipeline pl[2] = {hdr_pipeline(tf[0]), hdr_pipeline(tf[1])};
    if (!in.img || !t[0].img || !t[1].img || !pl[0] || !pl[1]) {
        LOG("[present] HDR probe at frame %llu: no images or pipelines\n", (unsigned long long)frame);
        probe_free(&in), probe_free(&t[0]), probe_free(&t[1]);
        return false;
    }
    // (one readback for the three: vk_readback hands out the same buffer at offset 0 each call)
    const uint64_t n4 = (uint64_t)w * h * 4;
    VkSpan all = vk_readback(n4 * 4);
    for (int i = 0; i < 3; i++) {
        g_probe_out[i] = all;
        g_probe_out[i].off = all.off + n4 * (uint64_t)i;
        g_probe_out[i].ptr = (uint8_t *)all.ptr + n4 * (uint64_t)i;
    }
    VkBufferImageCopy rg = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {w, h, 1}};
    // the SDR frame: read back, and copied into a sampled image for the encode
    barrier(cb, src, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    barrier(cb, in.img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    rg.bufferOffset = g_probe_out[0].off;
    vkCmdCopyImageToBuffer(cb, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_probe_out[0].buf, 1, &rg);
    VkImageCopy ic = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                      .extent = {w, h, 1}};
    vkCmdCopyImage(cb, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, in.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &ic);
    barrier(cb, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    barrier(cb, in.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    VkDescriptorSet set = vk_alloc_set(g_hdr_set_layout);
    VkDescriptorImageInfo ii = {.sampler = vk_default_sampler(false), .imageView = in.view, .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet wr = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 0, .descriptorCount = 1,
                               .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii};
    vkUpdateDescriptorSets(g_vk.dev, 1, &wr, 0, NULL);
    for (int i = 0; i < 2; i++) {   // the same pass as vk_present_hdr_encode, mode HDR10 then scRGB
        barrier(cb, t[i].img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        VkRenderingAttachmentInfo ca = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = t[i].view,
                                        .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
        VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, {w, h}}, .layerCount = 1,
                              .colorAttachmentCount = 1, .pColorAttachments = &ca};
        vkCmdBeginRendering(cb, &ri);
        VkViewport vp = {0, 0, (float)w, (float)h, 0, 1};
        VkRect2D sc = {{0, 0}, {w, h}};
        vkCmdSetViewport(cb, 0, 1, &vp);
        vkCmdSetScissor(cb, 0, 1, &sc);
        float pc[8] = {(float)(i + 1), g_hdr_paper, g_hdr_peak, g_hdr_expand, g_hdr_gamma22 ? 1.0f : 0.0f, 0, 0, 0};
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pl[i]);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_hdr_layout, 0, 1, &set, 0, NULL);
        vkCmdPushConstants(cb, g_hdr_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, pc);
        vkCmdDraw(cb, 3, 1, 0, 0);
        vkCmdEndRendering(cb);
        barrier(cb, t[i].img, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        rg.bufferOffset = g_probe_out[i + 1].off;
        vkCmdCopyImageToBuffer(cb, t[i].img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_probe_out[i + 1].buf, 1, &rg);
    }
    { extern void vk_dyn_invalidate(void); vk_dyn_invalidate(); }
    probe_free(&in), probe_free(&t[0]), probe_free(&t[1]);   // (deferred: freed once this submission is done)
    g_probe_frame = frame, g_probe_w = w, g_probe_h = h, g_probe_src_fmt = src_fmt;
    return true;
}
// After the probed frame's GPU work is done (device.c waits for its submission): the files
void vk_present_hdr_probe_save(void) {
    if (!g_probe_frame) return;
    static const char *ext[3] = {"sdr.bgra", "hdr10.a2b10g10r10", "scrgb.rgba16f"};
    const uint64_t size[3] = {(uint64_t)g_probe_w * g_probe_h * 4, (uint64_t)g_probe_w * g_probe_h * 4, (uint64_t)g_probe_w * g_probe_h * 8};
    char path[96];
    for (int i = 0; i < 3; i++) {
        snprintf(path, sizeof path, "hdr_probe_%llu_%s", (unsigned long long)g_probe_frame, ext[i]);
        FILE *f = fopen(path, "wb");
        if (f) { fwrite(g_probe_out[i].ptr, 1, size[i], f); fclose(f); }
    }
    snprintf(path, sizeof path, "hdr_probe_%llu.json", (unsigned long long)g_probe_frame);
    FILE *jf = fopen(path, "w");
    if (jf) {
        FILE *f = jf;
        fprintf(f, "{\"frame\": %llu, \"w\": %u, \"h\": %u, \"src_format\": %d, \"paper\": %g, \"peak\": %g, \"expand\": %g, "
                   "\"gamma22\": %d, \"swapchain_hdr\": %d}\n", (unsigned long long)g_probe_frame, g_probe_w, g_probe_h,
                (int)g_probe_src_fmt, g_hdr_paper, g_hdr_peak, g_hdr_expand, g_hdr_gamma22 ? 1 : 0, g_hdr_mode);
        fclose(f);
    }
    LOG("[present] HDR probe: frame %llu written (hdr_probe_%llu_*, %ux%u, paper %.0f, peak %.0f nits)\n",
        (unsigned long long)g_probe_frame, (unsigned long long)g_probe_frame, g_probe_w, g_probe_h, g_hdr_paper, g_hdr_peak);
    g_probe_frame = 0;
}
