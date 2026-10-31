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
#include "rt.h"
#include "totk_globals.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
constexpr u64 kFrameDelta = 0x45577c0;   // GOT slot of the frame-delta setter's object (totk_pc_vk.cpp kTimeScale)
constexpr u64 kRatio = 8;                // its 30 Hz ratio, the map's dt
constexpr u64 kTarget = 0x2c8, kVelocity = 0x2e0, kSnap = 0xc1c;
constexpr u64 kPathSteps = 0x37897fc, kPathSpeed = 0xbb4, kZoom = 0x2b4;   // the Hero's Path step table, its indexes

float rdf(u64 a) { return ldf32(a); }
void wrf(u64 a, float v) {
    u32 w;
    memcpy(&w, &v, 4);
    st32(a, w);
}
}  // namespace

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
    if (!on || !totk_ptr_ok(scr)) return f_f15364_orig(c);
    const float tx = rdf(scr + kTarget), tz = rdf(scr + kTarget + 4);
    const int snap = ld8(scr + kSnap) != 0;
    f_f15364_orig(c);
    const u64 fd = totk_global(kFrameDelta);
    if (!totk_ptr_ok(fd)) return;
    const float nx = rdf(scr + kTarget), nz = rdf(scr + kTarget + 4);
    const float k = totk_map_step_scale(rdf(fd + kRatio), rdf(scr + kVelocity), nx - tx, nz - tz, snap);
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
    const u64 scr = c->x[0], fd = totk_global(kFrameDelta);
    const float dt = totk_ptr_ok(fd) ? rdf(fd + kRatio) : 1.0f;
    if (!on || !totk_ptr_ok(scr) || !(dt > 0.01f && dt < 1.0f)) return f_f0fdec_orig(c);
    const u64 e = totk_hero_path_step_addr(totk_main(kPathSteps), ld32(scr + kPathSpeed), ld32(scr + kZoom));
    const u32 saved = e < totk_main(kPathSteps) + 48 ? ld32(e) : 0;   // speed 3 reads past the 3 rows (zeros)
    if (!saved) return f_f0fdec_orig(c);  // never write outside the table
    wrf(e, rdf(e) * dt);
    f_f0fdec_orig(c);
    st32(e, saved);
}
