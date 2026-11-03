// totk_map_speed.cpp: the map's stick scroll at real-time speed. UIMapScreen's per-frame move (main+0xf15364, gen.py
// --hook f15364 in GEN_MAIN; x0 = the screen) eases the stick velocity +0x2e0 by 1 - 0.75^dt towards speed[zoom level] /
// zoom rate and the view +0x2d0 / +0x2d4 by 1 - 0.75^dt towards the target +0x2c8 / +0x2cc, dt = the frame-delta setter's
// ratio (+8 of the object at GOT slot main+0x45577c0: 1.0 per 30 Hz frame; the `realtime` key fills it from the measured
// frame time). Those two follow the clock, but the stick's step, target += stick * velocity, is applied once per frame
// without dt: the map scrolled 151 px/s at 31 FPS and 702 px/s at 142 FPS (speed3_report.py, 4.6x = the FPS ratio). The
// hook scales that step by k = min(dt, 1): 1 at 30 FPS (the Switch's speed, unchanged), 0.25 at 120. Below 30 FPS it
// stays the game's (k > 1 could step past the clamp).
//
// Which writes are the step: the stick path (main+0xf16ad0) is the only one that stores a non-zero velocity (the path
// without stick input stores 0), and its step per axis is at most the velocity (|stick| <= 1, then the clamp towards
// the target's bounds, which a scaled step stays inside: it lies between the old target and the clamped one). The
// other target writes (the snaps onto the view while +0xc1c is set, the jump in the block at main+0xf1578c, the stores
// after main+0xf166b8 and the clamp at main+0xf16a7c) are left
// alone: the hook scales only when +0xc1c was clear and both axes moved by at most the velocity. The click-to-place
// (totk_menu_cursor.cpp) and hle_hid's drag write the target outside this function: not part of the step.
// TOTK_MAP_SPEED=0: the original alone.
//
// Hero's Path playback: the footprint player (main+0xf0fdec, x0 = the screen, called once per frame from
// UIMapScreen's update) moves its float index +0xbb0 by a step per FRAME from the 3 x 4 table at main+0x37897fc (row
// = speed +0xbb4 if < 4 else 0, column = zoom level +0x2b4 if <= 3 else 0; main+0xf104f0..0xf10524 / 0xf10584), so
// the replay ran 2x / 4.8x / 8x fast at 60 / 144 / 240 FPS. hook_f_f0fdec scales that one table entry by k = min(dt,
// 1) for the call and restores it after (the image is mapped read-write; the table is read only there), so the step,
// the crossed-points walk after it (0xf10610..) and the end clamp all see the Switch's step per 30 Hz of game time.
// The pause timer +0xbb8 already counts dt. gen.py --hook f0fdec; TOTK_MAP_SPEED=0 turns it off too.
//
// The mouse on the map: hle_hid's mouse_read hands its raw counts here (totk_map_mouse_add) instead of the left stick, and
// this hook, before the original, moves the target by them as a desktop pointer would (totk_map_mouse.h: one count = one
// screen pixel at sensitivity 1, through the layout's scale at the zoom's current rate +0x2bc), clamped to Hyrule's maps;
// the original then eases the view there and clamps as it does for the stick. No fps term: the counts are taken once,
// whatever the frame rate. Only while the move's prerequisites hold (the snap flag clear, the rate placed and in range,
// the window's size known); otherwise mouse_read keeps the stick emulation (pc_mouse_stick_counts). TOTK_MAP_MOUSE=0: the
// stick emulation always.
#include "rt.h"
#include "totk_globals.h"
#include "totk_map_mouse.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

extern "C" bool pc_mouse_client_size(int* w, int* h);   // pc_mouse.cpp: the game window's client area (false: none)

namespace {
// (the snap flag and the path speed through totk_field: 1.4.3 moved that part of the screen by 0x90, +0xcac / +0xc44;
// the target, velocity and zoom stay)
constexpr u64 kTarget = 0x2c8, kVelocity = 0x2e0;
constexpr u64 kSnap = totk_field(MapScreen, 0xc1c);
constexpr u64 kPathSteps = 0x37897fc, kPathSpeed = totk_field(MapScreen, 0xbb4), kZoom = 0x2b4;   // the step table, its indexes

constexpr u64 kRate = totk_field(MapScreen, 0x2bc);   // the zoom's current (eased) rate

float rdf(u64 a) { return ldf32(a); }
void wrf(u64 a, float v) {
    u32 w;
    memcpy(&w, &v, 4);
    st32(a, w);
}
// The mouse's counts not yet applied (input thread -> the map's move), the hook's last frame, the sensitivity row
std::mutex g_mm_lock;
double g_mm_x, g_mm_y;
std::atomic<long long> g_mm_seen{-1000000};   // ms on the steady clock
std::atomic<float> g_mm_speed{-1.0f};
long long now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
bool mouse_direct_on() {
    static const bool on = [] { const char *e = getenv("TOTK_MAP_MOUSE"); return !e || strcmp(e, "0"); }();
    return on;
}
float mouse_speed() {
    float k = g_mm_speed.load(std::memory_order_relaxed);
    if (k < 0) {
        const char *e = getenv("TOTK_MOUSE_MAP_SPEED");
        k = e && *e && atof(e) > 0 ? (float)atof(e) : 1.0f;
        g_mm_speed.store(k, std::memory_order_relaxed);
    }
    return k;
}
// The counts into the target, before the original's move (it eases the view there and clamps as for the stick)
void mouse_apply(u64 scr) {
    if (!mouse_direct_on() || !totk_field_known(MapScreen, 0x2bc) || ld8(scr + kSnap) != 0) return;
    int cw = 0, ch = 0;
    const float rate = rdf(scr + kRate);
    float dx = 0, dz = 0;
    if (!pc_mouse_client_size(&cw, &ch) || !totk_map_mouse_step(1, 1, 1, cw, ch, rate, &dx, &dz)) return;
    const long long now = now_ms();
    // (live: mouse_read hands its counts here; after a pause, e.g. The map closed and quickly opened again, the counts
    // gathered since the last run are dropped rather than applied as one jump)
    const bool fresh = now - g_mm_seen.exchange(now, std::memory_order_relaxed) <= 250;
    double cx, cy;
    {
        std::lock_guard<std::mutex> l(g_mm_lock);
        cx = fresh ? g_mm_x : 0, cy = fresh ? g_mm_y : 0;
        g_mm_x = g_mm_y = 0;
    }
    if ((cx == 0 && cy == 0) || !totk_map_mouse_step(cx, cy, mouse_speed(), cw, ch, rate, &dx, &dz)) return;
    wrf(scr + kTarget, totk_map_mouse_clamp(rdf(scr + kTarget) + dx, 6000.0f));
    wrf(scr + kTarget + 4, totk_map_mouse_clamp(rdf(scr + kTarget + 4) + dz, 5000.0f));
    static int logs;
    if (logs < 5 && ++logs)
        printf("[map-mouse] %.0f, %.0f counts -> %.1f, %.1f world (rate %.3f, client %d x %d, sensitivity %.2f)\n", cx, cy,
               dx, dz, rate, cw, ch, mouse_speed());
}
}  // namespace

// hle_hid.cpp mouse_read on the map (input thread): the counts this read took. True while the map's move takes them
// (it ran in the last 250 ms with its prerequisites): the caller then sets no stick from the mouse.
extern "C" bool totk_map_mouse_add(double cx, double cy) {
    if (!mouse_direct_on() || now_ms() - g_mm_seen.load(std::memory_order_relaxed) > 250) {
        std::lock_guard<std::mutex> l(g_mm_lock);
        g_mm_x = g_mm_y = 0;   // (nothing stale for the next time the map takes them)
        return false;
    }
    std::lock_guard<std::mutex> l(g_mm_lock);
    g_mm_x += cx, g_mm_y += cy;
    return true;
}
// controls.mouse_map_speed (hle_hid.cpp's row, live)
extern "C" void totk_map_mouse_speed_set(const char *v) {
    g_mm_speed.store(v && *v && atof(v) > 0 ? (float)atof(v) : 1.0f, std::memory_order_relaxed);
}

// The step's scale, or 1 to leave the move as the original made it (pure: tools/oracle/test_map_speed.py mirrors it).
extern "C" float totk_map_step_scale(float dt, float vel, float dx, float dz, int snap_before) {
    if (!(dt > 0.01f && dt < 1.0f)) return 1.0f;   // 30 FPS or slower, or not a ratio: the game's own step
    if (snap_before || !(vel > 0.0f)) return 1.0f;   // a snap, or no stick step this frame
    const float lim = vel * 1.001f + 1e-4f;
    if (fabsf(dx) > lim || fabsf(dz) > lim) return 1.0f;   // more than one step: a jump or a clamp, not the stick
    return dt;
}

// A key without the hook never calls hook_f_f15364 and has no f_f15364_orig (as totk_pc_vk.cpp).
#ifdef _WIN32
void f_f15364_orig(Ctx *__restrict c);
void mapspeed_orig_stub(Ctx *__restrict) {}
#pragma comment(linker, "/alternatename:?f_f15364_orig@@YAXPEIAUCtx@@@Z=?mapspeed_orig_stub@@YAXPEIAUCtx@@@Z")
#else
__attribute__((weak)) void f_f15364_orig(Ctx *__restrict c);
#endif
void hook_f_f15364(Ctx *__restrict c) {   // UIMapScreen's per-frame move (this)
#ifndef _WIN32
    if (!f_f15364_orig) return;
#endif
    static const bool on = [] {
        const char *e = getenv("TOTK_MAP_SPEED");
        const bool v = !e || strcmp(e, "0");
        printf("[map-speed] stick scroll %s\n", v ? "at real-time speed (step * dt)" : "per frame (TOTK_MAP_SPEED=0)");
        return v;
    }();
    const u64 scr = c->x[0];
    if (totk_ptr_ok(scr) && totk_field_known(MapScreen, 0xc1c)) mouse_apply(scr);
    if (!on || !totk_ptr_ok(scr) || !totk_field_known(MapScreen, 0xc1c)) return f_f15364_orig(c);
    const float tx = rdf(scr + kTarget), tz = rdf(scr + kTarget + 4);
    const int snap = ld8(scr + kSnap) != 0;
    f_f15364_orig(c);
    const u64 fd = totk_frame_delta();
    if (!totk_ptr_ok(fd)) return;
    const float nx = rdf(scr + kTarget), nz = rdf(scr + kTarget + 4);
    const float k = totk_map_step_scale(rdf(fd + totk_field(TimeScale, 0x8)), rdf(scr + kVelocity), nx - tx, nz - tz, snap);
    if (k == 1.0f) return;
    wrf(scr + kTarget, tx + (nx - tx) * k);
    wrf(scr + kTarget + 4, tz + (nz - tz) * k);
    static int logs;
    if (logs < 3 && ++logs)
        printf("[map-speed] step (%.2f, %.2f) * %.3f\n", nx - tx, nz - tz, k);
}

// The Hero's Path step table entry the footprint player reads this frame (main+0xf10504..0xf10520).
extern "C" u64 totk_hero_path_step_addr(u64 table, u32 speed, u32 zoom) {
    return table + (speed < 4 ? speed * 16ull : 0) + (zoom <= 3 ? zoom * 4ull : 0);
}

#ifdef _WIN32
void f_f0fdec_orig(Ctx *__restrict c);
void heropath_orig_stub(Ctx *__restrict) {}
#pragma comment(linker, "/alternatename:?f_f0fdec_orig@@YAXPEIAUCtx@@@Z=?heropath_orig_stub@@YAXPEIAUCtx@@@Z")
#else
__attribute__((weak)) void f_f0fdec_orig(Ctx *__restrict c);
#endif
void hook_f_f0fdec(Ctx *__restrict c) {   // UIMapScreen's Hero's Path player (this)
#ifndef _WIN32
    if (!f_f0fdec_orig) return;
#endif
    static const bool on = [] {
        const char *e = getenv("TOTK_MAP_SPEED");
        return !e || strcmp(e, "0");
    }();
    const u64 scr = c->x[0], fd = totk_frame_delta();
    const float dt = totk_ptr_ok(fd) ? rdf(fd + totk_field(TimeScale, 0x8)) : 1.0f;
    if (!on || !totk_main_known(kPathSteps) || !totk_field_known(MapScreen, 0xbb4) || !totk_ptr_ok(scr) ||
        !(dt > 0.01f && dt < 1.0f))
        return f_f0fdec_orig(c);
    const u64 e = totk_hero_path_step_addr(totk_main(kPathSteps), ld32(scr + kPathSpeed), ld32(scr + kZoom));
    const u32 saved = e < totk_main(kPathSteps) + 48 ? ld32(e) : 0;   // speed 3 reads past the 3 rows (zeros)
    if (!saved) return f_f0fdec_orig(c);  // never write outside the table
    wrf(e, rdf(e) * dt);
    f_f0fdec_orig(c);
    st32(e, saved);
}
