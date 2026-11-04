// totk_ability_pitch.cpp: the abilities' camera turns up / down at the Switch's speed at any frame rate (public #66:
// "vertical camera sensitivity much higher only while using Ultrahand", 60 and 120 fps, not 30).
//
// With an ability active (Ultrahand,...) the player camera runs ExecutePlayerCameraSpecialPower's mode update
// (main+0x1ce6eb0, once per FRAME). Its yaw goes through engine::framework::VFRBlender (frame-rate blended); its pitch
// is a plain add per call, no dt:
//   bl main+0x1347600 (the mode's LatStickScale getter: &param + 0x5c) ; ldr s0, [x0]          (main+0x1ce78f8 / fc)
//   fmul s1, s0, s9 (s9 = -PlayerCamera +0xc2c) ; fmul s12, s12, s1 (s12 = the processed stick y)  (main+0x1ce7900 / 04)
// then s12 moves the normalized pitch. PlayerCamera +0xc2c is a parameter (the camera pre-update main+0xd057e4 stores
// the float a getter returns, no frame time), so the pitch turns per FRAME: 2x the Switch's speed at 60 fps, 4x at 120.
//
// The hook (the getter, only at that call, return address main+0x1ce78fc: every other read of the parameter keeps the
// original) points the caller at LatStickScale x r, r the frame's 30 Hz ratio (VFRMgr: real time), so each call turns
// r 30 Hz frames' worth: the Switch's degrees per second at any rate. At 30 fps and below (r >= 1) the original exactly
// (the caller reads the parameter itself). gen.py --hook 1347600 in GEN_MAIN; TOTK_ABILITY_PITCH=0: the original alone.
// tools/oracle/test_ability_pitch.py; games/totk/tests/verify_ability_pitch.py checks the caller's words.
//
// The live scales (v0.1.10, LEAD 13:37: Ultrahand with the mouse): the same mode update reads LatStickScale here and
// LngStickScale (getter main+0x1347654, return address main+0x1ce7684) for its yaw; both values go to the data page
// as read (TOTK_DP_ABIL_LAT / _LNG, data_page.h), so pc_mouse's ability gains divide by the active mode's scales, not
// the SpecialPower block's 1.5 / 0.8 (another mode, e.g. grabbing, may use other ones). Read-only: the yaw call keeps
// the game's value. gen.py --hook 1347654 too.
#include "rt.h"
#include "totk_globals.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" void* totk_data_page_current(void);   // games/totk/runtime/totk_patches.c: the data page, or NULL

namespace {
constexpr u64 kPitchCall = totk_main(0x1ce78fc);   // return address of the ability camera's LatStickScale call
constexpr u64 kYawCall = totk_main(0x1ce7684);     // ... and of its LngStickScale call
constexpr unsigned kPageLat = 228, kPageLng = 232;   // TOTK_DP_ABIL_LAT / TOTK_DP_ABIL_LNG (data_page.h)
// (p is a float's address, &param + 0x5c / + 0x64: 4-aligned, so not totk_ptr_ok's 8-byte test)
bool float_ptr_ok(u64 p) { return p >= 0x10000 && p < (1ull << 47) && !(p & 3); }
void publish(unsigned off, u64 p) {   // the scale the game just read, for pc_mouse
    uint8_t *page = (uint8_t *)totk_data_page_current();
    if (!page || !float_ptr_ok(p)) return;
    __atomic_store_n((u32 *)(page + off), ld32(p), __ATOMIC_RELAXED);
}
// the scaled factor for the caller to read: one per thread (as totk_footik_ease.cpp)
alignas(16) thread_local float t_lat;
thread_local bool t_turned;   // the mode update read Lat this frame (its turn path), before its yaw read
}  // namespace

// Which path the ability camera's mode update took: the TURN (it reads LatStickScale for the per-frame pitch, the hook
// above), the BOUNDED OFFSET (bit 8 of the word at its input block +0x154: the stick integrated into clamped +0x1d8 /
// +0x1dc), or the NO-STICK path (node +0x1e0 = 1: the stick exactly 0, state re-seeded). The flags are read at the yaw
// read (lr main+0x1ce7684; x19 = the node there), but the Lat read comes AFTER it in the same update, so each update is
// judged at the NEXT yaw read: the flags it had, and whether it read Lat since. Logged when the path changes, 60 lines
// at most TOTK_DEBUG_ABILCAM=0: never.
static void log_path(u64 node) {
    static int on = -1, lines, last = -1, prev_b1e0 = -1, prev_bit8 = -1;
    if (on < 0) { const char *e = getenv("TOTK_DEBUG_ABILCAM"); on = !(e && !strcmp(e, "0")); }
    const bool turned = t_turned;   // (the previous update's Lat read)
    t_turned = false;
    if (!on || lines >= 60 || !totk_ptr_ok(node)) { prev_b1e0 = -1; return; }
    const int b1e0 = prev_b1e0, bit8 = prev_bit8;   // the previous update, now complete
    const u64 block = ld64(node + 0x20);
    // (the mode word: the input block's +0x154 on 1.0.0, +0x15c on 1.4.3, fields_v143.json CameraStickInput; a version
    // without the field: the bit is not read)
    const u64 mode_off = totk_field_known(CameraStickInput, 0x154) ? totk_field(CameraStickInput, 0x154) : 0;
    prev_b1e0 = ld8(node + 0x1e0), prev_bit8 = mode_off && totk_ptr_ok(block) ? (int)(ld32(block + mode_off) >> 8 & 1) : -1;
    if (b1e0 < 0) return;   // (the first update seen, or after a bad node: judged at the next call)
    const int now = (turned ? 1 : 0) | b1e0 << 1 | (bit8 & 1) << 2 | (bit8 < 0) << 3;
    if (now == last) return;
    last = now, lines++;
    printf("[ability-cam] path: %s (turn read %d, node +0x1e0 %d, input mode word bit 8 %d)\n",
           b1e0 ? "no stick (re-seeded)" : bit8 > 0 ? "bounded offset" : turned ? "turn" : "other", (int)turned, b1e0, bit8);
}

// The factor the caller multiplies its per-frame pitch by, for a frame of 30 Hz ratio r (tools/oracle/
// test_ability_pitch.py). The parameter itself where the original stays: r >= 1 (30 fps and below) or out of range.
extern "C" float totk_ability_pitch_scale(float lat, float r) {
    if (!(r > 0.01f && r < 1.0f)) return lat;
    return lat * r;
}

#ifdef _WIN32
void f_1347600_orig(Ctx *__restrict c);
void ability_pitch_orig_stub(Ctx *__restrict) {}
#pragma comment(linker, "/alternatename:?f_1347600_orig@@YAXPEIAUCtx@@@Z=?ability_pitch_orig_stub@@YAXPEIAUCtx@@@Z")
#else
__attribute__((weak)) void f_1347600_orig(Ctx *__restrict c);
#endif
void hook_f_1347600(Ctx *__restrict c) {   // the camera mode parameter's LatStickScale getter(this) -> x0 = &value
#ifndef _WIN32
    if (!f_1347600_orig) return;
#endif
    const u64 lr = c->x[30];
    f_1347600_orig(c);
    if (lr != kPitchCall) return;
    publish(kPageLat, c->x[0]);
    t_turned = true;
    static const bool on = [] {
        const char *e = getenv("TOTK_ABILITY_PITCH");
        const bool v = !e || strcmp(e, "0");
        printf("[ability-pitch] the abilities' camera pitch %s\n", v ? "per 30 Hz of game time" : "per frame, as the game (TOTK_ABILITY_PITCH=0)");
        return v;
    }();
    const u64 fd = totk_frame_delta(), p = c->x[0];
    if (!on || !totk_ptr_ok(fd) || !float_ptr_ok(p)) return;
    const float lat = ldf32(p), s = totk_ability_pitch_scale(lat, totk_frame_ratio(fd));
    if (s == lat) return;
    t_lat = s;
    c->x[0] = (u64)(uintptr_t)&t_lat;
}

#ifdef _WIN32
void f_1347654_orig(Ctx *__restrict c);
void ability_lng_orig_stub(Ctx *__restrict) {}
#pragma comment(linker, "/alternatename:?f_1347654_orig@@YAXPEIAUCtx@@@Z=?ability_lng_orig_stub@@YAXPEIAUCtx@@@Z")
#else
__attribute__((weak)) void f_1347654_orig(Ctx *__restrict c);
#endif
void hook_f_1347654(Ctx *__restrict c) {   // the camera mode parameter's LngStickScale getter(this) -> x0 = &value
#ifndef _WIN32
    if (!f_1347654_orig) return;
#endif
    const u64 lr = c->x[30];
    f_1347654_orig(c);
    if (lr != kYawCall) return;
    publish(kPageLng, c->x[0]);
    log_path(c->x[19]);
}
