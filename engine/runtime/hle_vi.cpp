// HLE: nn::vi (display). One default display with one layer; the NVN window draws through our own backend, so the
// display, layer and native window are opaque tokens. The display's vsync SystemEvent is signalled by a host thread
// at 60 Hz (auto-clear): the game's main loop waits on it every frame.
#include "hle.h"
#include "totk_version.h"
#include "pace_ladder.h"
#include "totk_menu_cursor.h"   // (totk_menu_screen: the archive warm-up's governor, load_track)
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>
#define NOMINMAX
#include <windows.h>

void hle_sysevent_init(u64 ev, bool autoclear);
void hle_sysevent_signal(u64 ev);

static u64 g_display_tok[4], g_layer_tok[4], g_window_tok[8];   // stable addresses handed to the game
static std::atomic<u64> g_vsync_ev{0};
static std::atomic<u64> g_vsync_count{0};

static std::mutex g_vsync_mtx;
static std::condition_variable g_vsync_cv;

// The tick wait: a high-resolution waitable timer (Windows 10 1803+; ~0.5 ms precision without raising the global timer
// resolution). std::this_thread::sleep_until rides the default 15.6 ms scheduler tick, so ticks could land up to a tick
// late. TOTK_VSYNC_SLEEP=1: the old sleep_until (A/B). "[vsync]" logs the tick interval spread every 10 s.
// TOTK_PACE_SPIN_US=<n> (present workstream; default 0 = off): the timer wakes n us early and the rest is a spin on the
// clock, so a capped frame leaves within a few us of its slot instead of the timer's ~0.5 ms spread. Live
// (display.pace_spin, pc_window.cpp pace_spin_apply -> vi_set_pace_spin): the environment gives the first value only
// (-1: not read yet; a set before the first read wins; getenv races env_put on another thread).
static std::atomic<int> g_spin_us{-1};
static long spin_clamp(const char *v) { return v && *v ? std::min(std::max(atol(v), 0L), 4000L) : 0; }
static long pace_spin_us() {
    int v = g_spin_us.load();
    if (v < 0) { int want = -1; g_spin_us.compare_exchange_strong(want, (int)spin_clamp(getenv("TOTK_PACE_SPIN_US"))); v = g_spin_us.load(); }
    return v;
}
extern "C" void vi_set_pace_spin(const char *v) {
    g_spin_us.store((int)spin_clamp(v));
    printf("[pace] precise pacing: %ld us (live)\n", spin_clamp(v));
}
static void vsync_wait_until(std::chrono::steady_clock::time_point t) {
    static thread_local struct VsyncTimer {   // (per thread: vsync tick + TOTK_FPS > 60 pacing; closed when the thread exits)
        HANDLE h = getenv("TOTK_VSYNC_SLEEP") ? nullptr
            : CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        ~VsyncTimer() { if (h) CloseHandle(h); }
    } vt;
    HANDLE timer = vt.h;
    const long spin_us = pace_spin_us();   // (one value for the whole wait)
    auto now = std::chrono::steady_clock::now();
    if (t <= now) return;
    auto tw = t - std::chrono::microseconds(spin_us);
    if (tw > now) {
        if (!timer) std::this_thread::sleep_until(tw);
        else {
            LARGE_INTEGER due;
            due.QuadPart = -(LONGLONG)(std::chrono::duration_cast<std::chrono::nanoseconds>(tw - now).count() / 100);  // relative, 100 ns
            if (!SetWaitableTimerEx(timer, &due, 0, nullptr, nullptr, nullptr, 0)) std::this_thread::sleep_until(tw);
            else WaitForSingleObject(timer, INFINITE);
        }
    }
    while (spin_us && std::chrono::steady_clock::now() < t) YieldProcessor();
}

// ---------------------------------------------------------------- display-locked vsync (TOTK_VSYNC=display) The tick
// above is a free-running 60 Hz clock, never in phase with the monitor: a frame paced on it reaches the screen at
// whichever real vblank follows, and as the two drift (59.94 / 60.02 Hz panels) a frame is shown twice and the next
// dropped every few seconds. TOTK_VSYNC=display waits on the monitor's own vblank instead (D3DKMTWaitForVerticalBlankEvent
// on the adapter driving the game window's monitor) and signals the game's 60 Hz event every k-th vblank, k = refresh / 60
// rounded (60 -> 1, 120 -> 2, 240 -> 4). A refresh that is not a multiple of ~60 Hz (144, 165) keeps the clock tick for
// the event, but caps that divide it (TOTK_FPS=72 / 144 at 144 Hz) are paced on the vblanks (vi_present_pace). No VRR: a
// variable-refresh display has no fixed vblank to lock to; use the clock and FIFO there. The monitor is looked up again
// every 2 s (the window may move). Falls back to the clock if the calls fail.
typedef LONG (APIENTRY *PFN_D3DKMT)(void *);
struct kmt_open_hdc { HDC hdc; UINT adapter; LUID luid; UINT source; };
struct kmt_wait_vblank { UINT adapter; UINT device; UINT source; };
struct kmt_close { UINT adapter; };
extern "C" void *shim_window_hwnd(void);   // nvn/vulkan/shim_win.c
static std::atomic<u64> g_vblank_count{0};  // real vblanks seen (display mode)
static std::atomic<double> g_display_hz{0}; // the locked monitor's refresh (0: not locked)
static std::mutex g_vblank_mtx;
static std::condition_variable g_vblank_cv;

static bool vsync_display_mode() {
    static const bool on = getenv("TOTK_VSYNC") && !strcmp(getenv("TOTK_VSYNC"), "display");
    return on;
}

// Waits for the next vblank of the game window's monitor; false when that cannot be done (caller uses the clock).
static bool display_vblank_wait(double *hz) {
    static PFN_D3DKMT open_hdc, wait_vb, close_ad;
    static bool loaded, ok;
    static UINT adapter, source;
    static HMONITOR mon;
    static double mon_hz;
    static ULONGLONG checked;
    if (!loaded) {
        loaded = true;
        HMODULE g = LoadLibraryW(L"gdi32.dll");
        open_hdc = g ? (PFN_D3DKMT)GetProcAddress(g, "D3DKMTOpenAdapterFromHdc") : nullptr;
        wait_vb = g ? (PFN_D3DKMT)GetProcAddress(g, "D3DKMTWaitForVerticalBlankEvent") : nullptr;
        close_ad = g ? (PFN_D3DKMT)GetProcAddress(g, "D3DKMTCloseAdapter") : nullptr;
        if (!open_hdc || !wait_vb || !close_ad) printf("[vsync] display lock unavailable (no D3DKMT entry points)\n");
    }
    if (!open_hdc || !wait_vb || !close_ad) return false;
    ULONGLONG now = GetTickCount64();
    if (!ok || now - checked >= 2000) {
        checked = now;
        HWND w = (HWND)shim_window_hwnd();
        HMONITOR m = w ? MonitorFromWindow(w, MONITOR_DEFAULTTOPRIMARY) : MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
        MONITORINFOEXW mi = {};
        mi.cbSize = sizeof mi;
        DEVMODEW dm = {};
        dm.dmSize = sizeof dm;
        double hzn = GetMonitorInfoW(m, &mi) && EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) ? dm.dmDisplayFrequency : 0;
        if (m != mon || !ok || hzn != mon_hz) {
            if (ok) { kmt_close c = {adapter}; close_ad(&c); ok = false; }
            HDC dc = CreateDCW(nullptr, mi.szDevice, nullptr, nullptr);
            kmt_open_hdc o = {dc};
            ok = dc && open_hdc(&o) == 0;
            if (dc) DeleteDC(dc);
            adapter = o.adapter; source = o.source; mon = m; mon_hz = hzn;
            static int logs;
            if (logs++ < 20)
                printf("[vsync] display lock %s: %ls, %.0f Hz (EnumDisplaySettings; integer rate)\n", ok ? "on" : "failed", mi.szDevice, hzn);
        }
    }
    if (!ok) return false;
    kmt_wait_vblank wv = {adapter, 0, source};
    if (wait_vb(&wv) != 0) { ok = false; return false; }
    *hz = mon_hz;
    return true;
}

extern "C" bool oe_host_paused(void);   // hle_oe.cpp: pause on focus loss (the window out of focus, the option on)
extern "C" unsigned oe_resume_count(void);   // hle_oe.cpp: holds ended so far

static void vsync_thread() {
    using clk = std::chrono::steady_clock;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    if (vsync_display_mode()) {   // one tick of the game's event per k real vblanks; the clock below on failure
        double hz = 0;
        u64 vb = 0;
        auto prev = clk::now();
        double gmin = 1e9, gmax = 0;
        while (display_vblank_wait(&hz)) {
            auto now = clk::now();
            int k = hz >= 75 ? (int)(hz / 60.0 + 0.5) : 1;
            bool locked = hz <= 0 || fabs(hz / k - 60.0) < 1.5;   // (unknown rate: every vblank)
            g_display_hz.store(hz > 0 ? hz : 60.0);
            { std::lock_guard<std::mutex> l(g_vblank_mtx); g_vblank_count++; }
            g_vblank_cv.notify_all();
            if (!locked) {   // 144 / 165 Hz: the event keeps a 60 Hz clock of its own, the vblanks still pace TOTK_FPS
                static bool told;
                if (!told) { told = true; printf("[vsync] %.0f Hz is not a multiple of 60: the 60 Hz event stays on the clock\n", hz); }
                static clk::time_point next = now;
                if (now < next) continue;
                next += std::chrono::nanoseconds(16666667);
                if (now > next) next = now + std::chrono::nanoseconds(16666667);
            } else if (++vb % k) continue;
            double g = std::chrono::duration<double, std::milli>(now - prev).count();
            prev = now;
            gmin = std::min(gmin, g); gmax = std::max(gmax, g);
            static u64 ticks;
            if (++ticks % 600 == 0) {
                printf("[vsync] %llu ticks, interval min %.2f max %.2f ms (display %.0f Hz / %d)\n", (unsigned long long)ticks,
                       gmin, gmax, hz, locked ? k : 0);
                gmin = 1e9; gmax = 0;
            }
            if (u64 ev = g_vsync_ev.load(); ev && !oe_host_paused()) {   // (paused: no tick, hle_oe.cpp)
                hle_sysevent_signal(ev);
                { std::lock_guard<std::mutex> l(g_vsync_mtx); g_vsync_count++; }
                g_vsync_cv.notify_all();
            }
        }
        g_display_hz.store(0);
        printf("[vsync] display lock lost: back to the 60 Hz clock\n");
    }
    auto next = clk::now(), prev = next;
    double gmin = 1e9, gmax = 0;
    u64 ticks = 0;
    for (;;) {
        next += std::chrono::nanoseconds(16666667);
        vsync_wait_until(next);
        auto now = clk::now();
        double g = std::chrono::duration<double, std::milli>(now - prev).count();
        prev = now;
        gmin = std::min(gmin, g); gmax = std::max(gmax, g);
        if (++ticks % 600 == 0) {
            printf("[vsync] %llu ticks, interval min %.2f max %.2f ms (%s)\n", (unsigned long long)ticks, gmin, gmax,
                   getenv("TOTK_VSYNC_SLEEP") ? "sleep_until" : "high-res timer");
            gmin = 1e9; gmax = 0;
        }
        if (u64 ev = g_vsync_ev.load(); ev && !oe_host_paused()) {   // (paused: no tick, hle_oe.cpp)
            hle_sysevent_signal(ev);
            { std::lock_guard<std::mutex> l(g_vsync_mtx); g_vsync_count++; }
            g_vsync_cv.notify_all();
        }
    }
}

// Present interval (nvnWindowSetPresentInterval; TotK sets 2 = 30 fps): on the Switch each presented frame stays on
// screen `interval` vblanks and the game blocks until its buffer comes back. Called by nvnQueuePresentTexture: waits
// until `interval` vsyncs have passed since the previous present (a late frame resyncs instead of catching up).
// TOTK_UNLOCK_FPS=1: up to the monitor's rate in gameplay, never unpaced (below, "unlocked frame rate").
// TOTK_SCAN_VFR=<present>: at that present, scan main's .data/.bss and the committed heap for the frame-timing
// struct (BotW's ksys::VFR keeps {u32 interval, f32 frameRate = 60 / interval, f32 frameTime = 1 / frameRate};
// TotK presents at interval 2 -> 30.0f, 1/30f adjacent) and log each hit with the words around it.
extern "C" u64 g_rt_heap_base;
extern "C" u64 kernel_heap_size(void);
extern "C" void rt_watch_write(u64 addr);
static void vfr_scan(void) {
    struct R { u64 a, b; } rs[2] = {{totk_main(0x40e0000), totk_main(0x4665c60)},
                                    {g_rt_heap_base, g_rt_heap_base + kernel_heap_size()}};
    int hits = 0;
    for (const R &r : rs) {
        if (!r.a || r.b <= r.a) continue;
        // TOTK_SCAN_VFR_ANY=1: every 1/30f (0x3D088889) on its own, not only after 30.0f
        static const bool any = getenv("TOTK_SCAN_VFR_ANY") != nullptr;
        for (u64 p = r.a & ~3ull; p + 8 <= r.b && hits < 80; p += 4) {
            const u32 *w = (const u32 *)p;
            if (any ? w[1] != 0x3D088889u : (w[0] != 0x41F00000u || w[1] != 0x3D088889u)) continue;
            hits++;
            printf("[vfr] 30.0f, 1/30f at %llx (%s):", (unsigned long long)p, r.a == rs[0].a ? "main data" : "heap");
            const u32 *b = (const u32 *)(p - 16);
            for (int i = 0; i < 24; i++) printf(" %08x", b[i]);
            printf("\n");
        }
    }
    printf("[vfr] scan done: %d hits (main data %llx-%llx, heap %llx +%llx)\n", hits, (unsigned long long)rs[0].a,
           (unsigned long long)rs[0].b, (unsigned long long)rs[1].a, (unsigned long long)(rs[1].b - rs[1].a));
    // TOTK_VFR_DUMP=<heap offset hex>: dump 0x400 bytes before/after that heap offset, and every 8-byte pointer into
    // that +-0x400 window held in main .data/.bss or the first 256 MiB of the heap (who owns the block)
    if (const char *e = getenv("TOTK_VFR_DUMP")) {
        u64 at = rs[1].a + strtoull(e, nullptr, 16), lo = at - 0x400, hi = at + 0x400;
        for (u64 p = lo; p < hi; p += 32) {
            const u32 *w = (const u32 *)p;
            printf("[vfr] %llx (+%llx):", (unsigned long long)p, (unsigned long long)(p - rs[1].a));
            for (int i = 0; i < 8; i++) printf(" %08x", w[i]);
            printf("\n");
        }
        int n = 0;
        for (int k = 0; k < 2; k++) {
            u64 a = rs[k].a, b = k ? rs[k].a + std::min<u64>(rs[k].b - rs[k].a, 256ull << 20) : rs[k].b;
            for (u64 p = a; p + 8 <= b && n < 60; p += 8) {
                u64 v = *(const u64 *)p;
                if (v < lo || v >= hi || (p >= lo && p < hi)) continue;
                n++;
                printf("[vfr] ptr at %llx (%s +%llx) -> %llx (block %+lld)\n", (unsigned long long)p, k ? "heap" : "main",
                       (unsigned long long)(p - (k ? rs[1].a : 0x7100000000ull)), (unsigned long long)v, (long long)(v - at));
            }
        }
    }
}

extern "C" volatile int g_dpad_switch_timing;   // hle_hid.cpp: a quick menu may open (d-pad held) -> Switch timing
extern "C" int vk_precompile_warmup_active(void);   // nvn/vulkan/exec.c: TOTK_WARMUP until "play now"
extern "C" void totk_engine_present(void);          // nvn/totk_game_patches.c: the game patches' frame step
extern "C" void totk_engine_interval(int interval);
// Cap TOTK_FPS on the locked monitor's vblanks when it divides the refresh (TOTK_VSYNC=display: 72 / 144 at 144 Hz,
// 120 at 240 Hz). False: not locked or not a divisor (the clock paces it).
static bool pace_on_vblanks(int fps) {
    double hz = g_display_hz.load();
    if (hz <= 0 || fps <= 0) return false;
    int n = (int)(hz / fps + 0.5);
    if (n < 1 || fabs(hz / n - fps) > fps * 0.005) return false;
    static u64 last;
    std::unique_lock<std::mutex> l(g_vblank_mtx);
    u64 want = last + (u64)n, now = g_vblank_count;
    if (now >= want + (u64)n) { last = now; return true; }   // late by a whole slot or more: resync, no wait
    if (now < want) g_vblank_cv.wait_for(l, std::chrono::milliseconds(100), [&] { return g_vblank_count >= want; });
    last = want;   // (a little late: keep the phase, the next slot is shorter)
    return true;
}

// Pacing at the present (present workstream; the default since the 12-boot A/B of 2026-10-23: frames within 2 ms of the
// median 72 -> 91 %, steady-frame jitter 1.97 -> 1.16 ms, 3 rounds of 3): the cap's wait is in device.c vk_frame_end,
// between the frame's GPU submit and vkQueuePresentKHR, instead of here (after the window present, before the game's
// next frame). The frame's CPU time then no longer shifts when it reaches the screen (presents leave on the slot, not
// slot + work), at the cost of up to one frame of input latency. Held frames are paced in backend_present. The interval
// used there is the game's last one (it changes only with the menus). TOTK_PACE_AT=frame: the old place (lower
// latency). Live (display.pace_at, pc_window.cpp pace_at_apply -> vi_set_pace_at; the environment gives the first value
// only, as above). The frame a change lands on can be paced twice or not at all (vi_present_pace and
// vi_pace_present_point read it on different threads); the next one is as set.
static std::atomic<int> g_pace_at{-1};   // 1 at the present, 0 before the frame
static bool pace_at_value(const char *v) { return !v || strcmp(v, "frame") != 0; }
static bool vi_pace_at_present() {
    int v = g_pace_at.load();
    if (v < 0) { int want = -1; g_pace_at.compare_exchange_strong(want, pace_at_value(getenv("TOTK_PACE_AT")) ? 1 : 0); v = g_pace_at.load(); }
    return v != 0;
}
extern "C" void vi_set_pace_at(const char *v) {
    g_pace_at.store(pace_at_value(v) ? 1 : 0);
    printf("[pace] pacing at the %s (live)\n", pace_at_value(v) ? "present" : "frame");
}
static std::atomic<int> g_pace_interval{2};

// ---------------------------------------------------------------- unlocked frame rate (TOTK_UNLOCK_FPS=1) Never
// unpaced. "Unlocked" is:
// - gameplay: the clock at a ceiling, TOTK_UNLOCK_CEIL=<fps> (60..480), by default the game window's monitor refresh
//   (60..240; 120 when it cannot be read; looked up every 2 s);
// - loading screens, the title and other light screens: the game's own interval (Switch timing). Gameplay is told
//   from them by the render passes a frame (exec.c vk_exec_passes_total, a 4-present mean; boot.py's world anchor reads
//   the same): the loading screen draws 2, the fade into it 20-120, the title ~155 and the world 175-500 (boots
//   paceunl2_2247 / pace120_2237): gameplay after 1 s at >= 60, loading again below 30 (menus that light may pace too).
// The loading rule also holds for TOTK_FPS above 120 (loads at higher rates are not verified). TOTK_LOAD_PACE=0: no
// loading rule. TOTK_UNLOCK_FPS=raw: the old unpaced mode, diagnostics only (world loads crash).
extern "C" uint64_t vk_exec_passes_total(void);   // nvn/vulkan/exec.c
extern "C" volatile int g_title_phase;             // hle_fs.cpp: 1 until the first stage that is not the title's
extern "C" volatile int g_title_stage;             // hle_fs.cpp: the title's stage (TitleScene_*) is up
extern "C" volatile uint64_t g_title_burst_end;    // hle_hid.cpp: TOTK_TITLE_FPS until this present (an A / B on the title)
extern "C" uint64_t nvn_present_count(void);
static std::atomic<bool> g_gameplay{false};
// The loading screen's own pace (warmup 2026-10-31, boot-load.md s7: TotK's loads are frame-paced, 60 against 30 halves
// every stage, actor creation x0.66): while a load is on screen it runs at min(the monitor's refresh,
// 120) whatever the player's cap (30, 60, 90, 120, unlocked, above 120), and the cap is back with the first frame that
// draws more than the loading screen does, so a fade, the world and every menu keep the player's rate. Never unpaced. "A
// load on screen" (load_track): the last 4 presents drew 24 render passes or fewer (the loading screen 2-24, a fade
// 20-120, the world 175+), no menu screen, title left, the loading state of the rule below, and 1 MiB read from the RomFS
// since that screen came up (a light screen nothing loads behind keeps the cap). TOTK_LOAD_FPS=0: off (loads at the cap,
// as before); =<30..120>: that rate instead. Or a banc change the game marks itself (totk_load_laps.cpp: from its
// "Banc破棄開始" lap node to "Banc読み込み終了"): the destroy, the Continue path's save load and the run-up to actor creation read
// next to nothing from the RomFS, so the 1 MiB test started the pace 1.3-1.6 s late in every world load and fast travel
// at a 30 cap (lp_A; boot-load.md s9). Within one: fewer than 60 render passes on each of the last 4 presents (the
// loading screen 2-28, the world 140+), no menu screen, no video, and title left or the title's stage up (Continue / Load
// from the title: its LoadingFade_00 covers it; the boot's own title-scene load too: its TitleScene stage files raise
// g_title_stage, and it draws 2 passes of black). TOTK_LOAD_BANC=0: the 1 MiB test alone.
extern "C" int totk_banc_change(void);   // totk_load_laps.cpp
#ifdef _WIN32
static int window_monitor_hz();
#endif
static std::atomic<bool> g_load_fast{false};
static std::atomic<unsigned> g_pace_epoch{0};   // (+1 when the loading pace starts or ends: the speed fix restarts its mean)
static std::atomic<int64_t> g_paced_ns{0};       // paced_period's last period, and how many it has set (the seed after a
static std::atomic<unsigned> g_paced_seq{0};  // loading-pace change: the new pace's)
extern "C" unsigned long long hle_fs_read_bytes(void);   // hle_fs.cpp
extern "C" unsigned long long renderer_video_frames(void);   // nvn/vulkan/textures.c: decoded video frames so far
// TOTK_CAP30 (tools/engine/build.py, another game version whose build lost its frame-time hooks): nothing paces
// above the Switch's 30 fps, whatever the environment or settings.ini says: without those hooks every frame steps
// game time by 1/30, so a faster frame is a faster game.
static int load_fps(void) {   // the loading screen's rate, 0: off
#ifdef TOTK_CAP30
    return 0;
#endif
    static int set = -2;
    if (set == -2) {
        const char *e = getenv("TOTK_LOAD_FPS");
        set = e && *e ? std::min(std::max(atoi(e), 0), 120) : -1;
        if (set > 0 && set < 30) set = 30;
    }
    if (set >= 0) return set;
#ifdef _WIN32
    using clk = std::chrono::steady_clock;
    static clk::time_point checked;
    static int hz;
    const auto now = clk::now();
    if (checked.time_since_epoch().count() == 0 || now - checked >= std::chrono::seconds(2)) { checked = now; hz = window_monitor_hz(); }
    return hz >= 60 ? std::min(hz, 120) : 60;
#else
    return 60;
#endif
}
// The loading screen is up (the rule's loading state, title left): nvn/vulkan/preupload.c uploads the textures the guest
// creates meanwhile, so the world's first frame does not (hitch_kak1)
// (since kp_A: the load on screen below, not the rule's loading state, which lasts 1 s into the world: the pre-upload
// kept draining over the world's first frames, and its account started after them)
static std::atomic<bool> g_load_screen{false};
extern "C" int vi_loading_screen(void) { return g_load_screen.load(std::memory_order_relaxed); }
static std::atomic<int> g_unlock_ceil{120};
// No pacing above this many frames a second, whatever the cap (TOTK_AUDIO_FPS_MAX=<fps>; 0: none), on a key without the
// aal hold. Each game frame the game hands its audio commands to aal's AudioRenderer thread as a CommandList, taking a
// fresh one from a pool of 16; the audio thread returns one per 5 ms audio frame. Above 200 game frames a second the game
// takes every returned list first, the pool sits empty, and aal's jam watchdog (main+0x21ef964, from
// AudioRendererController::calc once a game frame) counts the starved frames: 300 in a row (10 s at the Switch's 30, 1.25
// s at 240) switch the stream request pool off for good. The stream reads stop; the new game's OpeningText narration
// stopped and its opening held the fade ~275 s. The fix is the hold in totk_aal_commands.cpp (hook 9c2ef0: the game's
// flush waits while the audio thread is short of lists), so once that hook has run with its hold on the default here is
// off (240 at 240 Hz again). Without it (a key without --hook 9c2ef0, TOTK_AAL_CMD_HOLD=0) the default is 180, a tenth
// under 200 (main 9e9a97e2). An explicit TOTK_AUDIO_FPS_MAX applies either way. Only the game frames are held
// (vi_pace_wait's last step); DLSS-G's real frames are game frames and under it anyway. A value below 30 counts as 30,
// one that is not a number as the default.
extern "C" bool totk_aal_cmd_hold_active(void);   // totk_aal_commands.cpp
static int audio_fps_max() {
    static const int m = [] {
        const char *e = getenv("TOTK_AUDIO_FPS_MAX");
        char *end = nullptr;
        const long v = e ? strtol(e, &end, 10) : 0;
        if (!e || end == e) return -180;   // (the default: negative, so the aal hold can lift it)
        return v <= 0 ? 0 : (int)std::min(std::max(v, 30L), 1000L);
    }();
    if (m >= 0) return m;
    if (!totk_aal_cmd_hold_active()) return -m;
    static bool told;   // (the unlocked line may have said "held at 180" before the hook's first call, at the title)
    if (!told) { told = true; printf("[pace] the aal hold is on (hook 9c2ef0): game frames no longer held at %d\n", -m); }
    return 0;
}
// The frame-rate setting, live. The environment at start (TOTK_FPS, TOTK_UNLOCK_FPS, TOTK_UNLOCK_STEADY, which
// pc_settings.cpp fps_env writes from display.fps_cap) gives the first values; the row sets them again as it changes
// (fps_apply -> vi_set_frame_rate), and every reader below takes them per frame. -1: not read yet (a set before the first
// read wins).
static std::atomic<int> g_fps_set{-1};   // TOTK_FPS: 0 the game's own 30, else the cap (60, 90, 120 ...)
static std::atomic<int> g_unlock{-1};    // 0 paced as set, 1 unlocked, 2 raw (unpaced; environment only)
static std::atomic<int> g_steady{-1};    // 0 off, 1 whole refreshes, 2 10 % steps (TOTK_UNLOCK_STEADY=vrr; environment only)
// (the environment is read only until the first value is in: getenv races env_put on another thread under the MSVC
// CRT)
template <class F> static int rate_get(std::atomic<int> &a, F from_env) {
    int v = a.load();
    if (v < 0) { int want = -1; a.compare_exchange_strong(want, from_env()); v = a.load(); }
    return v;
}
static int fps_setting() {
#ifdef TOTK_CAP30
    return 0;   // (above)
#endif
    return rate_get(g_fps_set, [] { const char *e = getenv("TOTK_FPS"); return e ? std::max(atoi(e), 0) : 0; });
}
static int unlock_mode() {   // 0 paced as set, 1 unlocked, 2 raw (unpaced)
#ifdef TOTK_CAP30
    return 0;   // (above)
#endif
    return rate_get(g_unlock, [] {
        const char *e = getenv("TOTK_UNLOCK_FPS");
        return !e || !*e || !strcmp(e, "0") ? 0 : !strcmp(e, "raw") ? 2 : 1;
    });
}
static int steady_setting() {
    return rate_get(g_steady, [] {
        const char *e = getenv("TOTK_UNLOCK_STEADY");
        return !e || !*e || !strcmp(e, "0") ? 0 : !strcmp(e, "vrr") ? 2 : 1;
    });
}
extern "C" int vi_fps_setting(void) { return fps_setting(); }   // hle_hid.cpp (quick menus), device.c (window title)
extern "C" int vi_unlock_setting(void) { return unlock_mode(); }
extern "C" void totk_frame_step_retarget(void);   // games/totk (totk_patches.h): the realtime step after a rate change
// display.fps_cap's values (pc_settings.cpp V_FPS): "30" the game's own, "60" / "90" / "120", "unlocked", "steady"
// (unlocked at a rate the frames hold; a start's TOTK_UNLOCK_STEADY=vrr keeps its 10 % steps)
extern "C" void vi_set_frame_rate(const char *v) {
    if (!v) return;
    const bool steady = !strcmp(v, "steady"), unlocked = steady || !strcmp(v, "unlocked");
    const int was_steady = steady_setting();
    g_fps_set.store(unlocked || !strcmp(v, "30") ? 0 : std::max(atoi(v), 0));
    g_steady.store(!steady ? 0 : was_steady == 2 ? 2 : 1);
    g_unlock.store(unlocked ? 1 : 0);
    // (the pacer takes the new values at its next frame; the realtime step ramps to the new rate at once instead of its
    // 3 %-a-frame hitch ramp: a live 240 -> 30 was 1.5 s of slow motion, 30 -> 240 a 5.6x burst)
    totk_frame_step_retarget();
    printf("[pace] frame rate: %s (live)\n", v);
}
static int window_monitor_hz() {   // the game window's monitor refresh (EnumDisplaySettings; 0: unknown)
    HWND w = (HWND)shim_window_hwnd();
    HMONITOR m = w ? MonitorFromWindow(w, MONITOR_DEFAULTTOPRIMARY) : MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFOEXW mi = {};
    mi.cbSize = sizeof mi;
    DEVMODEW dm = {};
    dm.dmSize = sizeof dm;
    return GetMonitorInfoW(m, &mi) && EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1
        ? (int)dm.dmDisplayFrequency : 0;
}
// "[display]" when the game window's monitor or its mode changes (checked every 2 s): its refresh now and the highest one
// it offers at that resolution.
#ifdef _WIN32
extern "C" void osd_toast(const char* text, int ms);   // hle_osd.cpp
#include "pc_settings.h"
#include "pc_text.h"
static char g_refresh_notice[256], g_refresh_key[32];  // display_note -> display_notice (both on the game thread)
// The window's monitor's exact refresh period in ns: the active display path whose source is that GDI device,
// targetInfo.refreshRate as numerator / denominator (59.94 Hz is not the 59 that dmDisplayFrequency says). Without it, a
// reported 23 / 29 / 47 / 59 / 71 / 119 / 143 / 239 is taken as (n + 1) x 1000 / 1001: a pace a hair fast is absorbed by
// FIFO's back-pressure, a slow one shows a frame twice now and then.
static std::atomic<int64_t> g_refresh_ns{0};
static int64_t exact_refresh_ns(const wchar_t *gdi_name, DWORD hz) {
    UINT32 np = 0, nm = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &np, &nm) == ERROR_SUCCESS && np) {
        std::vector<DISPLAYCONFIG_PATH_INFO> paths(np);
        std::vector<DISPLAYCONFIG_MODE_INFO> modes(nm);
        if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &np, paths.data(), &nm, modes.data(), nullptr) == ERROR_SUCCESS)
            for (UINT32 i = 0; i < np; i++) {
                DISPLAYCONFIG_SOURCE_DEVICE_NAME sn = {};
                sn.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
                sn.header.size = sizeof sn;
                sn.header.adapterId = paths[i].sourceInfo.adapterId;
                sn.header.id = paths[i].sourceInfo.id;
                const DISPLAYCONFIG_RATIONAL r = paths[i].targetInfo.refreshRate;
                if (DisplayConfigGetDeviceInfo(&sn.header) == ERROR_SUCCESS && !wcscmp(sn.viewGdiDeviceName, gdi_name) &&
                    r.Numerator && r.Denominator)
                    return (int64_t)(1e9 * r.Denominator / r.Numerator);
            }
    }
    if (hz <= 1) return 0;
    switch (hz) {
    case 23: case 29: case 47: case 59: case 71: case 119: case 143: case 239: return (int64_t)(1e9 * 1001.0 / ((hz + 1) * 1000.0));
    default: return (int64_t)(1e9 / hz);
    }
}
static void display_note(void) {
    using clk = std::chrono::steady_clock;
    static clk::time_point checked;
    static wchar_t last_dev[32];
    static DWORD last_hz, last_w;
    auto now = clk::now();
    if (checked.time_since_epoch().count() && now - checked < std::chrono::seconds(2)) return;
    checked = now;
    HWND w = (HWND)shim_window_hwnd();
    HMONITOR m = w ? MonitorFromWindow(w, MONITOR_DEFAULTTOPRIMARY) : MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFOEXW mi = {};
    mi.cbSize = sizeof mi;
    DEVMODEW cur = {};
    cur.dmSize = sizeof cur;
    if (!GetMonitorInfoW(m, &mi) || !EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &cur)) return;
    if (!wcscmp(last_dev, mi.szDevice) && last_hz == cur.dmDisplayFrequency && last_w == cur.dmPelsWidth) return;
    wcscpy_s(last_dev, mi.szDevice), last_hz = cur.dmDisplayFrequency, last_w = cur.dmPelsWidth;
    const int64_t rns = exact_refresh_ns(mi.szDevice, cur.dmDisplayFrequency);
    g_refresh_ns.store(rns);
    DWORD best = 0;
    DEVMODEW dm = {};
    dm.dmSize = sizeof dm;
    for (DWORD i = 0; EnumDisplaySettingsW(mi.szDevice, i, &dm); i++)
        if (dm.dmPelsWidth == cur.dmPelsWidth && dm.dmPelsHeight == cur.dmPelsHeight && dm.dmDisplayFrequency > best) best = dm.dmDisplayFrequency;
    // Windows' own setting is the desktop mode in the registry: in exclusive fullscreen the current mode is the one the game
    // set (shim_win.c, display.exclusive_refresh), so a 60 Hz pick on a 144 Hz monitor is not Windows' doing
    DEVMODEW desk = {};
    desk.dmSize = sizeof desk;
    if (!EnumDisplaySettingsW(mi.szDevice, ENUM_REGISTRY_SETTINGS, &desk) || desk.dmDisplayFrequency <= 1 || !desk.dmPelsWidth) desk = cur;
    DWORD dbest = 0;
    if (desk.dmPelsWidth == cur.dmPelsWidth && desk.dmPelsHeight == cur.dmPelsHeight) dbest = best;
    else
        for (DWORD i = 0; EnumDisplaySettingsW(mi.szDevice, i, &dm); i++)
            if (dm.dmPelsWidth == desk.dmPelsWidth && dm.dmPelsHeight == desk.dmPelsHeight && dm.dmDisplayFrequency > dbest) dbest = dm.dmDisplayFrequency;
    if (const char *e = getenv("TOTK_REFRESH_TOP")) best = dbest = (DWORD)atoi(e);   // (tests: a top rate this PC's monitors lack)
    const bool low = desk.dmDisplayFrequency > 1 && dbest > desk.dmDisplayFrequency + 1;
    char dsk[96] = "";
    if (desk.dmDisplayFrequency != cur.dmDisplayFrequency || desk.dmPelsWidth != cur.dmPelsWidth || desk.dmPelsHeight != cur.dmPelsHeight)
        snprintf(dsk, sizeof dsk, "; Windows' desktop mode %lux%lu at %lu Hz, up to %lu", desk.dmPelsWidth, desk.dmPelsHeight, desk.dmDisplayFrequency, dbest);
    printf("[display] %ls: %lux%lu at %lu Hz (%.3f Hz exact; the monitor offers up to %lu Hz at this size%s%s)\n", mi.szDevice,
           cur.dmPelsWidth, cur.dmPelsHeight, cur.dmDisplayFrequency, rns ? 1e9 / rns : 0.0, best, dsk,
           low ? ": raise it in Windows' display settings" : "");
    // The player sees it too: the banner, held until the title is up (display_notice), once per situation:
    // settings.ini's display.refresh_notice remembers "<now>/<top>" Hz it was shown for, so a monitor kept at 59 on
    // purpose is not nagged at every start; back at the top rate the key goes, and a later drop shows it again.
    // TOTK_REFRESH_NOTICE=0: never; =test: also at the top rate and whatever the key says (to check the banner).
    static bool told;
    const char *rn = getenv("TOTK_REFRESH_NOTICE");
    const bool test = rn && !strcmp(rn, "test");
    char seen[32] = "", now_s[32];
    snprintf(now_s, sizeof now_s, "%lu/%lu", desk.dmDisplayFrequency, dbest);
    pc_settings_raw("display.refresh_notice", seen, sizeof seen);
    if (!low && seen[0] && !test) pc_settings_raw_set("display.refresh_notice", nullptr), seen[0] = 0;
    if (!low && !test) g_refresh_notice[0] = 0;   // (raised before the title came up: nothing left to say)
    if (told || !(low || test) || (rn && rn[0] == '0')) return;
    told = true;
    if (!test && !strcmp(seen, now_s)) {
        printf("[display] refresh notice: already shown for %s Hz (settings.ini display.refresh_notice)\n", now_s);
        return;
    }
    snprintf(g_refresh_key, sizeof g_refresh_key, "%s", test ? "" : now_s);   // (remembered once shown: display_notice)
    snprintf(g_refresh_notice, sizeof g_refresh_notice,   // (UTF-8: hle_osd.cpp draws it with GDI's wide calls)
             pc_tr("Windows runs your monitor at %lu Hz: set it to %lu Hz in Settings > Display > Advanced display"),
             desk.dmDisplayFrequency, dbest);
}
// The refresh notice goes up once the title (or the game, past it) is on screen, for 15 s
static void display_notice(void) {
    if (!g_refresh_notice[0] || (g_title_phase && !g_title_stage)) return;
    osd_toast(g_refresh_notice, 15000);
    if (g_refresh_key[0]) pc_settings_raw_set("display.refresh_notice", g_refresh_key);
    printf("[display] refresh notice shown: %s\n", g_refresh_notice);
    g_refresh_notice[0] = 0;
}
#else
static std::atomic<int64_t> g_refresh_ns{0};
static void display_note(void) {}
static void display_notice(void) {}
#endif
extern "C" void vk_warm_present(int gameplay, int menu);   // nvn/vulkan/exec.c: the archive warm-up's governor
// A menu that pauses the world (the governor warms faster there): the title menu, a save list, the pause menu and its
// item popups, the Purah Pad and its map, System > Options, the recipe book. A menu's focus gap keeps the last answer.
static bool world_paused_menu(void) {
    static bool was;
    switch (totk_menu_screen() & 0xff) {   // (the screen | its sub << 8)
    case TMS_TITLE: case TMS_SAVE_LIST: case TMS_PAUSE: case TMS_ITEM_POPUP: case TMS_PURAH_PAD: case TMS_MAP:
    case TMS_OPTIONS: case TMS_RECIPE: return was = true;
    case TMS_TRANSITION: return was;
    default: return was = false;
    }
}
// Per game present (game thread): the gameplay / loading state and the ceiling, for vi_pace_wait (any thread).
static void load_track(void) {
    display_note();
    display_notice();
    vk_warm_present(g_gameplay.load(), world_paused_menu());   // (the state as of the last present: one frame behind)
    static const bool rule = !getenv("TOTK_LOAD_PACE") || strcmp(getenv("TOTK_LOAD_PACE"), "0") != 0;
    // (the gameplay / loading state is followed whatever the setting, so a change to unlocked or above 120 in game
    // starts from the right state instead of a second of loading pace)
    using clk = std::chrono::steady_clock;
    auto now = clk::now();
    if (unlock_mode() == 1) {
        static const int fixed = getenv("TOTK_UNLOCK_CEIL") ? std::min(std::max(atoi(getenv("TOTK_UNLOCK_CEIL")), 60), 480) : 0;
        static clk::time_point checked;
        if (checked.time_since_epoch().count() == 0 || now - checked >= std::chrono::seconds(2)) {
            const bool first = checked.time_since_epoch().count() == 0;
            checked = now;
            int hz = window_monitor_hz(), c = fixed ? fixed : hz ? std::min(std::max(hz, 60), 240) : 120;
            // (the ceiling stays the monitor's: DLSS-G halves it and VRR steady starts from its margin; vi_pace_wait holds
            // the game frames under audio_fps_max after those)
            if (first || c != g_unlock_ceil.load()) {
                char held[64] = "";
                if (audio_fps_max() && c > audio_fps_max()) snprintf(held, sizeof held, "; game frames held at %d: TOTK_AUDIO_FPS_MAX", audio_fps_max());
                printf("[pace] unlocked: up to %d fps in gameplay (%s%s)\n", c, fixed ? "TOTK_UNLOCK_CEIL" : hz ? "the monitor's refresh" : "monitor refresh unknown", held);
            }
            g_unlock_ceil.store(c);
        }
        if (!rule) { g_gameplay.store(true); return; }
    }
    static u64 last, ring[4];
    static unsigned n;
    static clk::time_point heavy_since;
    u64 p = vk_exec_passes_total();
    ring[n++ & 3] = last ? p - last : 0;   // (async exec counts on the render thread, a present late: the mean evens it)
    last = p;
    double ppf = (ring[0] + ring[1] + ring[2] + ring[3]) / 4.0;
    bool on = g_gameplay.load();
    if (ppf >= 60) {
        if (heavy_since.time_since_epoch().count() == 0) heavy_since = now;
    } else heavy_since = {};
    bool to = on ? ppf >= 30 : heavy_since.time_since_epoch().count() && now - heavy_since >= std::chrono::seconds(1);
    {   // a load on screen (g_load_fast, above)
        static u64 light_rd;
        static bool light_was, loading_seen;
        const u64 most = std::max(std::max(ring[0], ring[1]), std::max(ring[2], ring[3]));
        const bool light = most <= 24 && !g_title_phase && (totk_menu_screen() & 0xff) == TMS_NONE;
        const u64 rd = hle_fs_read_bytes();
        // (a video playing streams from the RomFS and draws little: not a load. Within 1 s of a decoded frame)
        static u64 video_seen;
        static clk::time_point video_t;
        if (const u64 vf = renderer_video_frames(); vf != video_seen) { video_seen = vf; video_t = now; }
        const bool video = video_t.time_since_epoch().count() && now - video_t < std::chrono::seconds(1);
        if (video) { loading_seen = false; light_rd = rd; }
        if (light && !light_was) light_rd = rd;
        light_was = light;
        if (!light) loading_seen = false;
        else if (!video && rd - light_rd >= (1u << 20)) loading_seen = true;
        g_load_screen.store(rule && light && loading_seen && !to, std::memory_order_relaxed);
        // (a change still open when gameplay comes back, its end label missed, is stale: never again until the next one)
        static int banc_stale;
        const int bc = totk_banc_change();
        if (bc && to && !on) banc_stale = bc;
        const bool banc = bc && bc != banc_stale && most < 60 && (!g_title_phase || g_title_stage) && !video &&
                          (totk_menu_screen() & 0xff) == TMS_NONE;
        const bool fast = rule && ((light && loading_seen) || banc) && !to && load_fps() > 0;
        if (fast != g_load_fast.load()) {
            g_load_fast.store(fast);
            g_pace_epoch.fetch_add(1);
            static int logs;
            if (logs++ < 200)
                printf("[pace] %s (%.0f render passes a frame, %.1f MiB read%s)\n",
                       fast ? "loading screen: its own pace" : "loading pace ends: the cap again", ppf, (rd - light_rd) / 1048576.0,
                       fast && banc && !(light && loading_seen) ? ", a banc change" : "");
            if (fast) printf("[pace] loading screen: %d fps (TOTK_LOAD_FPS=0: the cap)\n", load_fps());
        }
    }
    if (to != on) {
        g_gameplay.store(to);
        static int logs;
        if (logs++ < 100)
            printf("[pace] %s: %.0f render passes a frame -> %s\n", to ? "gameplay" : "loading / light screen", ppf,
                   to ? (unlock_mode() == 1 ? "unlocked" : "TOTK_FPS") : "the game's interval");
    }
}

extern "C" void vk_pace_game_present(void);   // nvn/vulkan/present.c: the game's present time (TOTK_PRESENT_LOG / [pace])
extern "C" void vk_present_latency_sleep(void);   // nvn/vulkan/present.c (VK_NV_low_latency2; nothing when off)
static void vi_present_book(int interval);
static void vi_pace_wait(int interval);
static thread_local bool t_pace_held;   // vi_pace_wait held this call for a pause on focus loss (hle_oe.cpp)
extern "C" void vi_present_pace(int interval) {
    g_pace_interval.store(interval);
    load_track();
    vi_present_book(interval);
    if (!vi_pace_at_present()) vi_pace_wait(interval);
    vk_pace_game_present();
    vk_present_latency_sleep();   // (low latency on: Reflex's sleep before the next frame samples input)
}
extern "C" void vk_present_jit_wait(uint64_t ns);   // nvn/vulkan/present.c: a wait after input (low latency, any GPU)
extern "C" void vi_pace_present_point(void) {
    if (!vi_pace_at_present()) return;
    const auto t0 = std::chrono::steady_clock::now();
    t_pace_held = false;
    vi_pace_wait(g_pace_interval.load());
    if (t_pace_held) return;  // (a pause is no pacing wait: jit_sleep would sleep 3/4 of a period after it)
    vk_present_jit_wait((uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
}

static void vi_present_book(int interval) {   // per present: the game patches' frame step and the VFR / watch tools
    totk_engine_interval(interval);
    totk_engine_present();
    {
        static const long scan_at = getenv("TOTK_SCAN_VFR") ? atol(getenv("TOTK_SCAN_VFR")) : 0;
        static long presents;
        if (++presents == scan_at) vfr_scan();
    }
    {   // TOTK_VFR_SET=<delta frames>: every present, write {d, d / 30} into the 3 per-core {delta frame, delta time}
        // pairs main+0x4661598 points at; logs once per 60 presents whether the game rewrote them since the last
        // present (A/B lever test for frame-rate independent speed)
        static const long watch_at = getenv("TOTK_VFR_WATCH") ? atol(getenv("TOTK_VFR_WATCH")) : 0;  // =<present>: hardware
        static long wn;                                                                               // write watch on pair 0
        if (watch_at && ++wn == watch_at) {
            u64 p = *(const u64 *)totk_main(0x4661598);
            if (p) rt_watch_write(p);
        }
        // TOTK_WATCH=<hex address>: hardware write watch on any address from present 30, re-armed every 300 presents so
        // threads created later are covered too
        static const u64 watch_addr = getenv("TOTK_WATCH") ? strtoull(getenv("TOTK_WATCH"), nullptr, 16) : 0;
        static long wa;
        if (watch_addr && ++wa >= 30 && (wa - 30) % 300 == 0) rt_watch_write(watch_addr);
        static const char *vs = getenv("TOTK_VFR_SET");
        static long n;
        static float last_d = -1;
        if (vs) {
            float d = (float)atof(vs);
            u64 p = *(const u64 *)totk_main(0x4661598);
            if (p >= g_rt_heap_base && p < g_rt_heap_base + kernel_heap_size()) {
                float *f = (float *)p;
                if (++n % 60 == 0)
                    printf("[vfr] pairs before write: %g %g | %g %g | %g %g (last written %g)\n", f[0], f[1], f[2], f[3], f[4], f[5], last_d);
                for (int i = 0; i < 3; i++) { f[i * 2] = d; f[i * 2 + 1] = d / 30.0f; }
                last_d = d;
            }
        }
    }
}

// ---------------------------------------------------------------- steady unlocked (TOTK_UNLOCK_STEADY, off by default)
// Unlocked at a ceiling the game cannot hold swings with the scene, and frames that just miss it come unevenly (castle
// spot: sd 1.7-2.0 ms unlocked against 0.9-1.0 at a 120 cap). Steady paces at a rung of a ladder below the ceiling and
// judges each rung by what the frames do there (pace_ladder.h: when it goes down, up, and back to the rung it held
// before a fall). TOTK_UNLOCK_STEADY=1 (the F10 row's "Unlocked, steady"): rungs of whole refresh periods of the
// window's monitor (each frame on a whole number of refreshes); =vrr: steps of 10 % (experiments only, below). Never
// under 30.
static bool steady_on() {   // steady unlocked (the gate, every frame; the ladder kind is read per evaluation)
    return steady_setting() != 0;
}
// 1 whole refreshes (VRR too: 120 / 80 on a 240 Hz panel are fine there); 2 10 % steps, TOTK_UNLOCK_STEADY=vrr only:
// at the castle spot they kept probing (30-37 rung changes in 150 s, 98-115 fps, sd 2.4-3.9 ms against 0.9-1.9 at a
// 120 cap), so the F10 VRR row no longer picks them.
static int steady_mode() {
    return steady_setting() == 2 ? 2 : 1;
}
// The rung's period in ns (0: off). Rungs, from the ceiling's period up to 1/30 s: whole refresh periods of the window's
// monitor (n x its period, not the ceiling divided by n: on a 144 Hz panel 120 and 80 fps are not whole refreshes), or
// the ceiling's period / 0.9^k with VRR (any rate); the ceiling's own divisors when the refresh is unknown. unit: refresh
// periods per step (2 with DLSS-G: a real frame and its generated one each on a whole refresh). top: the ceiling is VRR's
// margin under the refresh (vi_pace_wait), the ladder's first rung, whole refreshes below it (the 1-refresh rung is above
// that ceiling and would leave 120 as the first rung on a 240 Hz panel). The decisions: pace_ladder.h (a rung change
// retargets the game's realtime step: vi_pace_wait, below).
static int64_t steady_period(int ceil, bool late, std::chrono::steady_clock::time_point now, int unit = 1, bool top = false) {
    static PaceLadder ladder;
    static int logs;
    const int mode = steady_mode();
    const int64_t rns = mode == 1 ? g_refresh_ns.load() * unit : 0;   // (display_note's, exact)
    const int64_t was = ladder.target;
    const int64_t p = ladder.frame(ceil, (int64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count(),
                                   late, [&](int64_t out[32]) { return pace_ladder_build(1000000000ll / ceil, rns, mode == 2, top, out); });
    if (was && std::abs(p - was) * 100 >= was) {
        const int hz = rns ? (int)(1e9 / rns + 0.5) : 0;
        if (logs++ < 200)
            printf("[pace] steady: %.1f -> %.1f fps (%d frames in the last second, %.0f %% late by a quarter period, ceiling %d, %s)\n",
                   1e9 / was, 1e9 / p, ladder.k, ladder.k ? ladder.l * 100.0 / ladder.k : 0.0, ceil,
                   mode == 2 ? "10 % steps" : hz > 1 ? "whole refreshes" : "refresh unknown");
    }
    return p;
}

#ifdef _WIN32
extern "C" bool vk_ngx_fg_on(void);   // nvn/vulkan/ngx.c: DLSS-G set up (TOTK_DLSS=fg / fgtest and NGX ready)
extern "C" int vk_present_mode_active(void);   // nvn/vulkan/present.c: the current swapchain's mode (2: FIFO; -1 none)
// VRR: the display.vrr row on and the swapchain FIFO (pc_window.cpp present_of_rows; the swapchain's own mode, so the
// pacing follows it across a row change and its re-create). A hand-set LARECOMP_VK_PRESENT=fifo without the row is plain
// vsync. The row is read once a second (live).
static bool present_vrr() {
    static bool row;
    static std::chrono::steady_clock::time_point read;
    const auto now = std::chrono::steady_clock::now();
    if (now - read >= std::chrono::seconds(1) || !read.time_since_epoch().count()) {
        read = now;
        const char *v = pc_settings_get("display.vrr");
        row = v && !strcmp(v, "on");
    }
    return row && vk_present_mode_active() == 2;
}
#endif
// The clock pacer's period for FG's real frames (ns; 0: not pacing them on the clock): the FG present thread's real
// interval right after a rung change, which its running mean of the arrivals takes ~10 frames to reach.
static std::atomic<int64_t> g_pace_period_ns{0};
extern "C" int64_t vi_pace_period_ns(void) { return g_pace_period_ns.load(); }
// The period paced this frame (ns; the vsync path's: its interval of the 60 Hz tick). One that changes by 1 % or more (a
// steady rung, gameplay back at its rung after a load or a menu, Switch timing while the d-pad is held, a new ceiling)
// retargets the game's realtime step like a cap change: the 3 %-a-frame hitch ramp gave a steady 30 -> 224 return a
// burst and 120 -> 60 a stretch of slow motion
static void paced_period(int64_t ns) {
    static int64_t paced;
    if (paced && std::abs(ns - paced) * 100 >= paced) totk_frame_step_retarget();
    paced = ns;
    g_paced_ns.store(ns, std::memory_order_relaxed);
    g_paced_seq.fetch_add(1, std::memory_order_release);
}
static void vi_pace_wait(int interval) {
    static u64 last;
    // Paused on focus loss (hle_oe.cpp oe_host_paused): the game's present waits here until the window is in front
    // again, whatever the pacing below (an unlocked rate never waits for the vsync the hold stops)
    if (oe_host_paused()) {
        t_pace_held = true;
        while (oe_host_paused()) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        std::lock_guard<std::mutex> l(g_vsync_mtx);
        last = g_vsync_count;   // (a frame at once, not a catch-up)
    }
    // TOTK_FPS=60: pace at every vsync (interval 1) instead of the game's 2 (needs the speed fix above for 100 % speed)
    const int fps_set = fps_setting();   // (live: the display.fps_cap row)
    static const bool rule = !getenv("TOTK_LOAD_PACE") || strcmp(getenv("TOTK_LOAD_PACE"), "0") != 0;
    // unlocked: the ceiling in gameplay; unlocked or above 120 (loading rule): the game's interval on light screens
    int fps = unlock_mode() == 1 ? (g_gameplay.load() ? g_unlock_ceil.load() : 0)
            : fps_set > 120 && rule && !g_gameplay.load() ? 0 : fps_set;
    const int load_pace = g_load_fast.load() ? load_fps() : 0;   // (the loading screen's own pace, g_load_fast)
    if (load_pace) fps = load_pace;
    // DLSS-G (the presenter: one generated frame between real ones): the cap is the frames on screen, so the real frames
    // pace at half of it (120 on a 120 Hz monitor: 60 real + 60 generated; at the cap itself they would put 240 on a 120
    // Hz screen, dropped by mailbox or queued by FIFO). Never under 30 real. TOTK_FG_CAP=real: the cap paces the real
    // frames (the output twice it), for A/Bs. The game's own 30 is left alone.... and the real frames pace by steady
    // unlocked's rungs below that (2 refresh periods a step): when the game cannot hold half the cap, its generated
    // frames came late, out 3-4 ms before the real one and replaced on screen. With a rung the real frames hold, each
    // generated frame keeps its half interval, and no wait is added on the render thread beyond the present's own cap.
    bool fg_steady = false, fg_vrr = false, vrr_top = false;
#ifdef _WIN32
    {
        static const bool cap_real = getenv("TOTK_FG_CAP") && !strcmp(getenv("TOTK_FG_CAP"), "real");
        // (vk_ngx_fg_on: TOTK_DLSS=fg or TOTK_DLSS_FG=1, the Options row graphics.frame_gen; not fgtest, where every frame
        // shown is a generated one)
        static const bool fgtest = getenv("TOTK_DLSS") && !strcmp(getenv("TOTK_DLSS"), "fgtest");
        // Unlocked with VRR: the ceiling stays under the panel's maximum by Reflex's margin, hz - hz^2 / 3600 (at the
        // refresh itself FIFO holds each frame for the next vblank and queues the ones behind it: latency, the panel no
        // faster). Steady unlocked and FG (below) start from this ceiling.
        if (unlock_mode() == 1 && fps > 0 && present_vrr()) {
            const int64_t rns = g_refresh_ns.load();
            const int hz = rns ? (int)(1e9 / rns + 0.5) : 0;
            if (hz >= 60 && hz - hz * hz / 3600 < fps) fps = hz - hz * hz / 3600, vrr_top = true;
        }
        if (!fgtest && !cap_real && fps >= 60 && vk_ngx_fg_on()) {
            // With VRR (the display.vrr row: FIFO) the panel shows each frame as it comes, so the whole-refresh rungs only
            // cost rate (a game holding 100 real fps fell to the 60 rung: 120 on screen against 130 without FG); there the
            // frames on screen stay under the panel's maximum by Reflex's margin, hz - hz^2 / 3600 (116 at 120 Hz, 138 at
            // 144, 171 at 180, 224 at 240: FIFO queues above it, and the spread of FG's frames grows with the rate), and
            // the real ones at half that, whatever the game holds: no rungs, steady unlocked's included. A fixed refresh
            // keeps the rungs: each frame on a whole refresh.
            const int64_t rns = g_refresh_ns.load();
            const int hz = rns ? (int)(1e9 / rns + 0.5) : 0;
            fg_vrr = present_vrr();
            if (fg_vrr && hz >= 60) fps = std::min(fps, hz - hz * hz / 3600);
            fps = std::max(fps / 2, 30);
            fg_steady = !fg_vrr;
        }
    }
#endif
    g_pace_period_ns.store(0);   // (set again below while the clock pacer paces FG's real frames)
    // TOTK_TITLE_FPS=<fps>: the start-up's black screens before the title's stage, and on the title a window of presents
    // after each A or B (hle_hid.cpp kTitleBurst: the transition that press starts), at <fps> when that is above the cap,
    // until the first stage that is not the title's (Continue / Load / New Game taken). A title, menu or dialog waiting
    // for input stays at the cap: their fade-ins, floating glyphs and cursor pulses count frames. The transitions count
    // frames too: first A -> Continue taken 3.7 s at 120 fps against 7.8 s at 30.
#ifdef TOTK_CAP30
    static const int title_fps = 0;   // (load_fps)
#else
    static const int title_fps = getenv("TOTK_TITLE_FPS") ? std::min(std::max(atoi(getenv("TOTK_TITLE_FPS")), 0), 480) : 0;
#endif
    if (title_fps > fps && g_title_phase) {
        static bool was_on;
        bool on = !g_title_stage || nvn_present_count() < g_title_burst_end;
        if (on != was_on) {
            was_on = on;
            if (on) printf("[pace] title: %d fps (TOTK_TITLE_FPS) from present %llu\n", title_fps, (unsigned long long)nvn_present_count());
            else printf("[pace] title: the cap from present %llu\n", (unsigned long long)nvn_present_count());
        }
        if (on) fps = title_fps;
    }
    if (audio_fps_max() && fps > audio_fps_max()) fps = audio_fps_max();   // (a TOTK_FPS or title rate above it; see audio_fps_max)
    // TOTK_WARMUP=1 (shader warm-up only, tools/linux/warmup.sh, 2026-10-17): the game is paced at 10 fps so its own threads
    // (and their share of the Steam Deck's power budget) go to the shader compile workers; only the title screen runs.
    // ("play now" on the warm-up screen ends this pacing: exec.c vk_precompile_play_now)
    if (vk_precompile_warmup_active()) {
        using clk = std::chrono::steady_clock;
        static clk::time_point wnext;
        const auto period = std::chrono::milliseconds(100);
        auto now = clk::now();
        if (wnext.time_since_epoch().count() == 0 || now > wnext + period) wnext = now;
        else vsync_wait_until(wnext);
        wnext += period;
        return;
    }
    if (unlock_mode() == 2) {
        static bool told;
        if (!told) { told = true; printf("[pace] TOTK_UNLOCK_FPS=raw: unpaced (diagnostics; world loads can crash)\n"); }
        return;
    }
    // TOTK_FPS=90/120/144... (above the 60 Hz vsync tick) or 31..59 (e.g. 40 / 45 / 50 for a Steam Deck screen set to that
    // refresh rate): pace on the clock (high-res timer); a late frame
    // (a quick menu, d-pad held: Switch timing below like at 60 fps, else its background goes black; 2026-10-17)
    if ((fps > 60 || (fps > 30 && fps < 60)) && !g_dpad_switch_timing) {
        if (vsync_display_mode() && pace_on_vblanks(fps)) return;
        // Catch-up is limited to period / 16 (timer wake-up jitter): a frame that ends late moves the next deadline to its
        // own present + period. With the full catch-up a late frame (up to one period) let the next ones through without a
        // wait: in the 120-cap rounds (eop1024 Kakariko / Depths lightroot, 1080p) 15 % of presents came sooner than the
        // 8.33 ms period, down to 2.2 ms, in bursts after each slow frame (two frames inside one 120 Hz refresh).
        using clk = std::chrono::steady_clock;
        static clk::time_point next;
        static int64_t steady_ns;
        auto now = clk::now();
        const bool rungs = (unlock_mode() == 1 && g_gameplay.load() && steady_on() && !fg_vrr) || fg_steady;
        if (rungs && next.time_since_epoch().count()) {
            // (late: this frame came a quarter period or more after its slot, the deadline the last one set; smaller
            // misses are routine at a rate the game holds: a 120 cap held 117 fps with 28 % of frames a little late)
            const auto slack = std::chrono::nanoseconds((steady_ns ? steady_ns : 1000000000ll / fps) / 4);
            steady_ns = steady_period(fps, now > next + slack, now, fg_steady ? 2 : 1, vrr_top && !fg_steady);
        } else steady_ns = 0;
        const auto period = std::chrono::nanoseconds(steady_ns ? steady_ns : 1000000000ll / fps);
        paced_period((int64_t)period.count());
        g_pace_period_ns.store(fg_steady ? (int64_t)period.count() : 0);
        if (next.time_since_epoch().count() == 0 || now > next + period) next = now;
        else if (now < next) { vsync_wait_until(next); now = next; }
        next = std::max(next + period, now + period - period / 16);
        return;
    }
    if (fps >= 60 && interval > 1 && !g_dpad_switch_timing) interval = 1;
    // "30 (the game's own)" is the Switch's 30: two vblanks a frame at least, whatever interval the window has now. The
    // game's own 3 stays; the step follows measured time, so 30 is real time. Loads at unlocked or above 120 keep the
    // game's interval.
    if (!fps_set && unlock_mode() == 0 && interval < 2 && !load_pace) interval = 2;
    // (Switch timing = 30 fps: with Dynamic resolution on, the scaled GPU clock puts TotK in its interval-3 mode for the
    // whole session (user 2026-10-19 logs: DYNRES 1 -> interval 3, DYNRES 0 -> 2), and the quick menus ran at 20 fps)
    if (g_dpad_switch_timing && interval > 2) interval = 2;
    if (interval < 1 || !g_vsync_ev.load()) return;
    paced_period(interval * 16666667ll);
    std::unique_lock<std::mutex> l(g_vsync_mtx);
    u64 want = last + (u64)interval;
    if (g_vsync_count < want) g_vsync_cv.wait_for(l, std::chrono::milliseconds(100), [&] { return g_vsync_count >= want; });
    last = g_vsync_count;
}

// ---------------------------------------------------------------- game speed independent of frame rate TotK's
// frame-delta setter main+0x7a83e4 (recompiled with gen.py --hook 7a83e4): called once per game frame as f(this, w1 =
// present interval 2). It stores ratio = interval / 2 and frameTime = interval / 60 at this+8, then deltaFrame = ratio
// * speed multiplier, deltaTime = deltaFrame / 30 at this+0 and per core at this+16, +56, +96. With the fixed interval
// every frame advanced 1/30 s of game time: 2.06x speed at 62 fps. The hook scales those outputs by k = measured frame
// time / (interval / 60). Astro Bot lessons: the raw frame time is clamped to [1/240, 1/20] s (a load stall must not
// inject a huge step, physics dislikes > 1/20 s) and smoothed with an EMA of 0.08 (instant jumps crashed Astro).
// TOTK_SPEED_FIX=0: old behaviour (fixed step); [speedfix] logs k every 300 calls.
void f_7a83e4_orig(Ctx* __restrict c);
void hook_f_7a83e4(Ctx* __restrict c) {
    static const bool on = !getenv("TOTK_SPEED_FIX") || strcmp(getenv("TOTK_SPEED_FIX"), "0");
    u64 self = c->x[0];
    u32 interval = (u32)c->x[1];
    f_7a83e4_orig(c);
    if (interval >= 1 && interval <= 4) {   // [timemult] the game's time multiplier (deltaFrame / ratio) and call gaps, on change:
        static u64 tself; static float tm = -1; static std::chrono::steady_clock::time_point tl; static int tlogs;   // which menus
        const float *g = (const float *)self;  // pause or slow the world
        auto tn = std::chrono::steady_clock::now();
        if (!tself) tself = self;
        if (self == tself) {
            float m = g[2] > 0 ? g[0] / g[2] : -1;
            double gap = tm < 0 ? 0 : std::chrono::duration<double, std::milli>(tn - tl).count();
            if ((fabsf(m - tm) > 0.01f || gap > 100) && tlogs++ < 400)
                printf("[timemult] %.3f -> %.3f (gap %.0f ms, dpad timing %d)\n", tm, m, gap, (int)g_dpad_switch_timing);
            tm = m; tl = tn;
        }
    }
    if (!on || interval < 1 || interval > 4) return;
    static struct { u64 self; std::chrono::steady_clock::time_point last; double ema; u64 calls; unsigned resumes, epoch, seq; bool reseed; } st[8];
    int i = 0;
    while (i < 8 && st[i].self && st[i].self != self) i++;
    if (i == 8) return;
    auto now = std::chrono::steady_clock::now();
    double nominal = interval / 60.0;
    if (!st[i].self) { st[i].self = self; st[i].last = now; st[i].ema = nominal; st[i].resumes = oe_resume_count(); st[i].epoch = g_pace_epoch.load(); return; }
    double dt = std::chrono::duration<double>(now - st[i].last).count();
    st[i].last = now;
    // (a pause on focus loss ended since the last call: that gap is no game step; the smoothed one stands for it)
    if (const unsigned r = oe_resume_count(); r != st[i].resumes) { st[i].resumes = r; dt = st[i].ema; }
    if (g_dpad_switch_timing) {   // (game paced at its own interval: no scaling; resume at 60)
        st[i].ema = 1.0 / 60;
        if (interval > 2) {   // (paced at 2 instead, above: the outputs interval 2 gives, so ratio 1 as on the Switch)
            float *f = (float *)self;
            static const int all[] = {0, 1, 4, 5, 14, 15, 24, 25, 2, 3};
            for (int j : all) f[j] *= 2.0f / interval;
        }
        return;
    }
    dt = std::min(std::max(dt, 1.0 / 240), 1.0 / 20);
    // (the loading pace started or ended: the mean starts again from the NEW pace's period, the first one the pacer set
    // after the change, else the world's first frames after a load at 120 would step a quarter of 1/30 s at a 30 cap until
    // the mean caught up: slow motion into the fade. The boundary frame's own dt is still the old pace)
    if (const unsigned ep = g_pace_epoch.load(); ep != st[i].epoch) {
        st[i].epoch = ep;
        st[i].seq = g_paced_seq.load(std::memory_order_acquire);
        st[i].reseed = true;
    }
    if (st[i].reseed && g_paced_seq.load(std::memory_order_acquire) != st[i].seq) {
        const double p = g_paced_ns.load(std::memory_order_relaxed) / 1e9;
        st[i].reseed = false;
        if (p > 0) { st[i].ema = std::min(std::max(p, 1.0 / 240), 1.0 / 20); dt = st[i].ema; }
    }
    st[i].ema += 0.08 * (dt - st[i].ema);
    float k = (float)(st[i].ema / nominal);
    if (++st[i].calls % 300 == 0)
        printf("[speedfix] timer %d (%llx): interval %u raw dt %.2f ms ema %.2f ms -> k %.3f\n", i, (unsigned long long)self,
               interval, dt * 1e3, st[i].ema * 1e3, k);
    float *f = (float *)self;
    static const int idx[] = {0, 1, 4, 5, 14, 15, 24, 25, 2, 3};  // this+0/+4, +16/+20, +56/+60, +96/+100, (+8/+12)
    // A/B modes: 2 = deltas only (keep ratio + frame time), 3 = deltas + frame time (keep ratio), 4 = deltas + ratio
    static const int mode = getenv("TOTK_SPEED_FIX") ? atoi(getenv("TOTK_SPEED_FIX")) : 1;
    // mode 5: as 1, but the ratio stays unscaled while the game's time multiplier (deltaFrame / ratio, read before
    // scaling) is below 1 = world paused or slowed (weapon quick menu: a ratio of 0.5 made TotK skip the scene in the
    // frame that captures the menu background -> black menu)
    bool keep_ratio = false;
    if (mode == 5 && f[2] > 0) {
        float mult = f[0] / f[2];
        keep_ratio = mult < 0.99f;
        static bool was; static int logs;
        if (keep_ratio != was && logs++ < 20) printf("[speedfix] time multiplier %.3f: ratio %s\n", mult, keep_ratio ? "kept" : "scaled");
        was = keep_ratio;
    }
    for (int j = 0; j < 8; j++) f[idx[j]] *= k;
    if (mode != 2 && mode != 3 && !keep_ratio) f[2] *= k;
    if (mode != 2 && mode != 4) f[3] *= k;
}

HLE_AS(vi_Initialize, "_ZN2nn2vi10InitializeEv") {}
HLE_AS(vi_OpenDefaultDisplay, "_ZN2nn2vi18OpenDefaultDisplayEPPNS0_7DisplayE") {
    *(u64*)c->x[0] = (u64)g_display_tok; c->x[0] = 0;
}
HLE_AS(vi_CreateLayer, "_ZN2nn2vi11CreateLayerEPPNS0_5LayerEPNS0_7DisplayE") {
    *(u64*)c->x[0] = (u64)g_layer_tok; c->x[0] = 0;
}
HLE_AS(vi_GetNativeWindow, "_ZN2nn2vi15GetNativeWindowEPPvPNS0_5LayerE") {
    *(u64*)c->x[0] = (u64)g_window_tok; c->x[0] = 0;
}
HLE_AS(vi_GetDisplayVsyncEvent, "_ZN2nn2vi20GetDisplayVsyncEventEPNS_2os15SystemEventTypeEPNS0_7DisplayE") {
    u64 ev = c->x[0];
    hle_sysevent_init(ev, true);
    static std::once_flag once;
    g_vsync_ev = ev;
    std::call_once(once, [] { std::thread(vsync_thread).detach(); });
    printf("[vi] vsync event %llx (60 Hz host timer)\n", ev);
    c->x[0] = 0;
}
