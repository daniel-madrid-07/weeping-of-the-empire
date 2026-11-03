/* Run-time side of the TotK 1.0.0 patches: see include/totk/totk_patches.h and games/totk/README.md.
   Frame step, output size switching and the heap formula are ported from MacNativeGames (MIT)
   games/totk/runtime/game_patches.c (runtime_frame_step_update, runtime_output_res_toggle, runtime_output_res_extra_bytes). */
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS   /* getenv, sscanf: the inputs are short settings strings */
#endif
#include "../include/totk/totk_patches.h"
#include "../include/totk/data_page.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t *g_page;                          /* the data page, once totk_patches_init ran */
static void (*g_post_oe)(uint32_t);
static TotkSettings g_set;
static uint64_t g_last_present_ns;
static double g_step, g_smooth, g_debt;          /* the step written last, the EMA of the measured time, real - given */
static double g_hitch_lag = -1;                  /* a lone hitch's lag until the next present decides it (-1: none) */
static uint32_t g_pinned_us;                     /* atomic: diagnostics pin, 0 = measured */
static int g_retarget;                           /* atomic: presents left in a frame-rate cap change's fast ramp */
static int g_interval = 2;                       /* the game's present interval (realtime off) */
static int g_hold = 1;                           /* atomic: totk_frame_step_hold (held until the engine says otherwise) */
static int g_hold_frames, g_hold_clamped;        /* atomic: this hold's frames, and those shorter than its floor */
static unsigned g_follow_opened;                 /* step follow windows opened (frame_step_next; the present thread) */
static unsigned g_hitch_dropped;                 /* lone hitches not owed (frame_step_next; the present thread) */

static uint64_t pack_size(unsigned w, unsigned h) { return (uint64_t)h << 32 | w; }

/* v0.1.8 release blocker: gameplay steps under 1/240 s (dde084d4's 1/360 floor: 3 crashes in 4 exclusive v0.1.8 runs,
   v0.1.7 none. So the floor is 1/240 everywhere again (TOTK_MAX_FPS default and highest 240, the engine holds the step
   and paces at 240 at most, the 300 / 360 rows are hidden). TOTK_STEP_360=1: the 1/360 path (gameplay down to 1/360,
   held at 1/240 through loads), until the solver gets steps of 1/240 or more at any rate.*/
int totk_max_fps_highest(void) {
    const char *e = getenv("TOTK_STEP_360");
    return e && !strcmp(e, "1") ? 360 : TOTK_HOLD_FPS;
}

static uint64_t frame_step_bits(float seconds) {
    float ratio = seconds * 30.0f;
    uint32_t lo, hi;
    memcpy(&lo, &ratio, 4);
    memcpy(&hi, &seconds, 4);
    return (uint64_t)lo | (uint64_t)hi << 32;
}

/* The guest reads the two lanes independently; one 64-bit store keeps them a pair, and both stay finite and in range
   even if an update falls between the reads. */
static void put_frame_step(float seconds) {
    if (g_page) __atomic_store_n((uint64_t *)(g_page + TOTK_DP_FRAME_RATIO), frame_step_bits(seconds), __ATOMIC_RELEASE);
}

static void put_u64(unsigned off, uint64_t v) { __atomic_store_n((uint64_t *)(g_page + off), v, __ATOMIC_RELEASE); }

static void put_f32(unsigned off, float v) {
    uint32_t bits;
    memcpy(&bits, &v, 4);
    __atomic_store_n((uint32_t *)(g_page + off), bits, __ATOMIC_RELEASE);
}

void totk_settings_default(TotkSettings *s) {
    memset(s, 0, sizeof *s);
    s->realtime = 1;
    s->render_w = 1600, s->render_h = 900;
    s->output_w = 1920, s->output_h = 1080;
    s->max_w = 1920, s->max_h = 1080;
    s->aspect = 0;
    s->sim_scale = 1.0f;
    s->tex_cache = 1.0f;
    s->res_floor = 1.0f;
    s->mouse_scale = 1.0f;
    s->mouse_rate = TOTK_MOUSE_RATE_DEFAULT;
    s->min_frame_ms = 1000.0 / totk_max_fps_highest();   /* review N14: 1/120 ran the game 1.2x fast at 144 fps.*/
    s->max_frame_ms = 1000.0 / 15.0;
}

static int parse_size(const char *e, unsigned *w, unsigned *h) {
    char rest;
    return sscanf(e, "%ux%u%c", w, h, &rest) == 2;
}

const char *totk_settings_from_env(TotkSettings *s) {
    const char *e;
    unsigned w, h;
    int max_set = 0;
    if ((e = getenv("TOTK_REALTIME")) && *e) s->realtime = strcmp(e, "0") != 0;
    if ((e = getenv("TOTK_RENDER_RES")) && *e && strcmp(e, "0")) {
        if (!parse_size(e, &w, &h) || w < 64 || h < 64 || w > 7680 || h > 4320) return "TOTK_RENDER_RES must be <w>x<h> (64..7680 x 64..4320)";
        s->render_w = w, s->render_h = h;
    }
    if ((e = getenv("TOTK_OUTPUT_MAX")) && *e && strcmp(e, "0")) {
        if (!parse_size(e, &w, &h) || w < 640 || h < 360 || w > 7680 || h > 4320) return "TOTK_OUTPUT_MAX must be <w>x<h> (640x360 .. 7680x4320)";
        s->max_w = w, s->max_h = h, max_set = 1;
    }
    if ((e = getenv("TOTK_OUTPUT_RES")) && *e && strcmp(e, "0")) {
        if (!parse_size(e, &w, &h) || w < 640 || h < 360 || w > 7680 || h > 4320) return "TOTK_OUTPUT_RES must be <w>x<h> (640x360 .. 7680x4320)";
        s->output_w = w, s->output_h = h;
        if (!max_set) s->max_w = w, s->max_h = h;
    }
    if (s->output_w > s->max_w || s->output_h > s->max_h) return "TOTK_OUTPUT_RES is bigger than TOTK_OUTPUT_MAX";
    if ((e = getenv("TOTK_ASPECT")) && *e) {
        double a = 0, x, y;
        char rest;
        if (!strcmp(e, "0")) a = 0;
        else if ((sscanf(e, "%lf:%lf%c", &x, &y, &rest) == 2 || sscanf(e, "%lfx%lf%c", &x, &y, &rest) == 2) && x > 0 && y > 0) a = x / y;
        else if (sscanf(e, "%lf%c", &x, &rest) == 1) a = x;
        else return "TOTK_ASPECT must be <w>:<h>, <w>x<h>, a ratio between 0.5 and 8, or 0";
        if (a != 0 && !(a >= 0.5 && a <= 8)) return "TOTK_ASPECT must be between 0.5 and 8";
        s->aspect = (float)a;
    }
    if ((e = getenv("TOTK_DYNAMIC_RES")) && *e) {
        double f;
        char rest;
        if (!strcmp(e, "off") || !strcmp(e, "0")) s->res_floor = 1.0f;
        else if (!strcmp(e, "on") || !strcmp(e, "1")) s->res_floor = 0.0f;
        else if (sscanf(e, "%lf%c", &f, &rest) == 1 && f >= 0.25 && f < 1) s->res_floor = (float)f;
        else return "TOTK_DYNAMIC_RES must be off (fixed scene size), on (the game's own) or a floor between 0.25 and 1";
    }
    if ((e = getenv("TOTK_FOV")) && *e) {
        double f;
        char rest;
        if (sscanf(e, "%lf%c", &f, &rest) != 1 || !(f == 0 || (f >= 0.5 && f <= 2))) return "TOTK_FOV must be a multiplier between 0.5 and 2, or 0 (the game's own)";
        s->fov_scale = f == 1 ? 0.0f : (float)f;
    }
    if ((e = getenv("TOTK_SHADOW_RES")) && *e) {
        double f;
        char rest;
        if (sscanf(e, "%lf%c", &f, &rest) != 1 || !(f == 0 || (f >= 0.25 && f <= 2))) return "TOTK_SHADOW_RES must be a multiplier between 0.25 and 2, or 0 (the game's own)";
        s->shadow_scale = f == 1 ? 0.0f : (float)f;
    }
    if ((e = getenv("TOTK_SHADOW_REACH")) && *e) {
        double f;
        char rest;
        if (sscanf(e, "%lf%c", &f, &rest) != 1 || !(f == 0 || (f >= 1 && f <= 4)))
            return "TOTK_SHADOW_REACH must be a multiplier between 1 and 4 (the near shadow cascades' reach), or 0 (the game's own)";
        s->shadow_reach = f == 1 ? 0.0f : (float)f;
    }
    if ((e = getenv("TOTK_SHADOW_FIXED")) && *e) {
        if (!strcmp(e, "1") || !strcmp(e, "on")) s->shadow_fixed = 1.0f;
        else if (!strcmp(e, "0") || !strcmp(e, "off") || !strcmp(e, "game")) s->shadow_fixed = 0.0f;
        else return "TOTK_SHADOW_FIXED must be 1 / on (the shadow map at its largest size) or 0 / off (the game's own)";
    }
    if ((e = getenv("TOTK_TEMP_UNIT")) && *e) {
        if (!strcmp(e, "c") || !strcmp(e, "C")) s->temp_unit = 1;
        else if (!strcmp(e, "f") || !strcmp(e, "F")) s->temp_unit = 2;
        else return "TOTK_TEMP_UNIT must be c, f or empty (the game's own)";
    }
    if ((e = getenv("TOTK_LOD")) && *e) {
        double f;
        char rest;
        if (sscanf(e, "%lf%c", &f, &rest) != 1 || !(f == 0 || (f >= 0.5 && f <= 8))) return "TOTK_LOD must be a multiplier between 0.5 and 8, or 0 (the game's own)";
        s->lod_scale = f == 1 ? 0.0f : (float)f;
    }
    if ((e = getenv("TOTK_ACTOR_LOD")) && *e) {
        double f;
        char rest;
        if (sscanf(e, "%lf%c", &f, &rest) != 1 || !(f == 0 || (f >= 0.5 && f <= 4))) return "TOTK_ACTOR_LOD must be a multiplier between 0.5 and 4, or 0 (the game's own)";
        s->actor_lod_scale = f == 1 ? 0.0f : (float)f;
    }
    if ((e = getenv("TOTK_TERRAIN_LOD")) && *e) {
        double f;
        char rest;
        if (sscanf(e, "%lf%c", &f, &rest) != 1 || !(f == 0 || (f >= 0.5 && f <= 2))) return "TOTK_TERRAIN_LOD must be a multiplier between 0.5 and 2, or 0 (the game's own); 3 crashes the world load";
        s->terrain_lod_scale = f == 1 ? 0.0f : (float)f;
    }
    if ((e = getenv("TOTK_TEXTURE_CACHE")) && *e) {
        double f;
        char rest;
        if (sscanf(e, "%lf%c", &f, &rest) != 1) return "TOTK_TEXTURE_CACHE must be a multiplier between 1 and 3";
        if (f > 10) f /= 100;   /* a percentage */
        if (!(f >= 1 && f <= 3)) return "TOTK_TEXTURE_CACHE must be a multiplier between 1 and 3 (or 100..300 %)";
        s->tex_cache = (float)(floor(f * 100 + 0.5) / 100);
    }
    if ((e = getenv("TOTK_SIM_DISTANCE")) && *e) {
        double f;
        char rest;
        if (sscanf(e, "%lf%c", &f, &rest) != 1 || !(f >= 0.5 && f <= 4)) return "TOTK_SIM_DISTANCE must be a multiplier between 0.5 and 4 (1 = the game's own)";
        s->sim_scale = (float)f;
    }
    if ((e = getenv("TOTK_MOUSE_CAMERA")) && *e) {
        double f;
        char rest;
        if (sscanf(e, "%lf%c", &f, &rest) != 1 || !(f >= 0 && f <= 4)) return "TOTK_MOUSE_CAMERA must be a multiplier between 0 (off) and 4";
        s->mouse_scale = (float)f;
    }
    if ((e = getenv("TOTK_MOUSE_RATE")) && *e) {
        double f;
        char rest;
        if (sscanf(e, "%lf%c", &f, &rest) != 1 || !(f >= 0.001 && f <= 1)) return "TOTK_MOUSE_RATE must be between 0.001 and 1";
        s->mouse_rate = (float)f;
    }
    if ((e = getenv("TOTK_MAX_FPS")) && *e) {
        double hz = atof(e);
        if (!(hz >= 30 && hz <= totk_max_fps_highest()))
            return totk_max_fps_highest() > TOTK_HOLD_FPS ? "TOTK_MAX_FPS must be 30..360"
                : "TOTK_MAX_FPS must be 30..240 (shorter steps corrupt Havok's solver; TOTK_STEP_360=1: 30..360)";
        s->min_frame_ms = 1000.0 / hz;
    }
    return NULL;
}

/* The ratio the camera gets: the setting, or the output's own shape; 0 within 1% of 16:9 (the game's own). */
static float effective_aspect(const TotkSettings *s, unsigned w, unsigned h) {
    double a = s->aspect > 0 ? s->aspect : (double)w / h;
    double r = a / (16.0 / 9.0);
    return r > 0.99 && r < 1.01 ? 0.0f : (float)a;
}

void *totk_data_page(void *main_base, uint64_t main_span) {
    return (uint8_t *)main_base + TOTK_DATA_PAGE_OFFSET(main_span);
}
void *totk_data_page_current(void) { return g_page; }
static uint8_t *g_main;   /* (totk_main_base: exec.c's LARECOMP_DRS_STRESS_LOG reads game globals through it) */
void *totk_main_base(void) { return g_main; }

const char *totk_patches_init(void *main_base, uint64_t main_span, const TotkSettings *s, void (*post_oe_message)(uint32_t)) {
    if (!main_base || !s) return "totk_patches_init: no main base or settings";
    if (s->output_w > s->max_w || s->output_h > s->max_h) return "totk_patches_init: output bigger than the swapchain";
    g_set = *s;
    g_post_oe = post_oe_message;
    g_page = totk_data_page(main_base, main_span);
    g_main = (uint8_t *)main_base;
    memset(g_page, 0, TOTK_DP_PC_OFFSET);   /* this version's slots; the PC page and the rest stay as committed (zero) */
    put_frame_step(1.0f / 60.0f);
    put_f32(TOTK_DP_RENDER_SIZE, (float)s->render_w);
    put_f32(TOTK_DP_RENDER_SIZE + 4, (float)s->render_h);
    put_u64(TOTK_DP_OUTPUT_SIZE, pack_size(s->output_w, s->output_h));
    put_u64(TOTK_DP_RENDERER, 0);
    put_u64(TOTK_DP_SWAPCHAIN_SIZE, pack_size(s->max_w, s->max_h));
    put_f32(TOTK_DP_ASPECT, effective_aspect(s, s->output_w, s->output_h));
    put_f32(TOTK_DP_RES_FLOOR, s->res_floor);
    put_f32(TOTK_DP_FOV_SCALE, s->fov_scale);
    put_f32(TOTK_DP_SHADOW_SCALE, s->shadow_scale);
    put_f32(TOTK_DP_SHADOW_FIXED, s->shadow_fixed);
    put_f32(TOTK_DP_SHADOW_SPLIT, s->shadow_reach);
    __atomic_store_n((uint32_t *)(g_page + TOTK_DP_TEMP_UNIT), s->temp_unit, __ATOMIC_RELEASE);
    put_f32(TOTK_DP_LOD_SCALE, s->lod_scale);
    put_f32(TOTK_DP_ACTOR_LOD, s->actor_lod_scale);
    put_f32(TOTK_DP_TERRAIN_LOD, s->terrain_lod_scale);
    put_f32(TOTK_DP_SIM_RADIUS, s->sim_scale > 0 ? s->sim_scale : 1.0f);
    put_f32(TOTK_DP_MOUSE_SCALE, s->mouse_scale);
    put_f32(TOTK_DP_MOUSE_RATE, s->mouse_rate);
    char dyn[48];
    if (s->res_floor >= 1.0f) snprintf(dyn, sizeof dyn, "off (fixed scene size)");
    else if (s->res_floor <= 0.0f) snprintf(dyn, sizeof dyn, "on (the game's own)");
    else snprintf(dyn, sizeof dyn, "on, floor %.0f %%", s->res_floor * 100.0);
    fprintf(stderr, "[totk] data page at %p: frame step %s, scene %ux%u, output %ux%u (swapchain %ux%u), aspect %.4f%s, "
            "dynamic resolution %s, field of view x%.2f, shadow map x%.2f%s, near shadow reach x%.2f, LOD distance x%.2f\n",
            (void *)g_page, s->realtime ? "real time" : "shipping (present interval)", s->render_w, s->render_h,
            s->output_w, s->output_h, s->max_w, s->max_h, totk_aspect_current(),
            totk_aspect_current() == 0 ? " (the game's own 16:9)" : "", dyn, s->fov_scale > 0 ? s->fov_scale : 1.0,
            s->shadow_scale > 0 ? s->shadow_scale : 1.0, s->shadow_fixed ? " (fixed size)" : "",
            s->shadow_reach > 0 ? s->shadow_reach : 1.0, s->lod_scale > 0 ? s->lod_scale : 1.0);
    return NULL;
}

void totk_present_interval(int interval) {
    if (interval < 1 || interval > 4) return;
    __atomic_store_n(&g_interval, interval, __ATOMIC_RELAXED);
}

void totk_frame_step_pin(double ms) {
    uint32_t us = ms > 0 ? (uint32_t)(ms * 1000.0 + 0.5) : 0;
    __atomic_store_n(&g_pinned_us, us, __ATOMIC_RELAXED);
    if (us) {
        put_frame_step((float)(us / 1e6));
        fprintf(stderr, "[totk] frame step pinned at %.3f ms (diagnostics)\n", us / 1000.0);
    } else {
        fprintf(stderr, "[totk] frame step back to measured time\n");
    }
}

/* A frame-rate cap change (the live display.fps_cap, present's apply): the presents that follow get the fast ramp
   below. The 3 %-per-frame limit is for hitches; on a deliberate 240 -> 30 switch it gave 1.5 s of slow motion (0.13x,
   1.3 s behind) and on 30 -> 240 a 5.6x burst, then 0.66 s ahead for good (the model of frame_step_next).*/
void totk_frame_step_retarget(void) {
    __atomic_store_n(&g_retarget, TOTK_RETARGET_PRESENTS, __ATOMIC_RELAXED);
}

/* The faults are in Havok's contact solver run during the load (main+0x286d50, its "TtSolveConstraints" /
   "TtSolveJacobians" scopes -> main+0x29a898 -> main+0x2972dc -> phive::CompactibleHeap::alloc main+0x12f9268; the
   neighbours main+0x2967f0 / +0x26d2a0 on the same path), while the world's new bodies are first solved. So while held (a
   load, the title, a light screen, and a settle time after them: hle_vi.cpp load_track) the step stays at or above
   1/TOTK_HOLD_FPS, and the time a held frame ran ahead is not owed (repaying it after the load would be slow motion). The
   hold's end moves the step back to the measured rate within ~10 frames (the retarget ramp), but only when the floor held
   the frames (a quarter of them or more shorter than it): at 240 fps or less (a 120 cap) the hold changed nothing, and the
   ramp dropped the owed time and followed the frames' jitter at up to 25 % a frame, a speed-up / slow-down after every
   fast travel; a lone catch-up frame under 1/240 is repaid as owed time. TOTK_HOLD_RETARGET=always: on every release, as
   before.*/
void totk_frame_step_hold(int on) {
    static int always = -1;
    if (always < 0) { const char *e = getenv("TOTK_HOLD_RETARGET"); always = e && !strcmp(e, "always"); }
    if (__atomic_exchange_n(&g_hold, on ? 1 : 0, __ATOMIC_RELAXED) == (on ? 1 : 0)) return;   /* (no change) */
    const int frames = __atomic_exchange_n(&g_hold_frames, 0, __ATOMIC_RELAXED);
    const int clamped = __atomic_exchange_n(&g_hold_clamped, 0, __ATOMIC_RELAXED);
    if (!on && (always || (frames && clamped * 4 >= frames))) totk_frame_step_retarget();   
}

/* The step from one clamped present-to-present time (after Savage Wind's runtime/hle/framepace.c, MIT): an EMA (0.1) of the measured time, plus 2 % per frame of the time the game still
   owes (real - given), so the game clock keeps real time; the step changes at most 3 % per frame, so a hitch or an interval switch reaches physics and animation as a ramp, not a jump (the
   old five-frame average moved a 200 ms hitch's 10 ms into five steps). A gap (loading, the window dragged) is not owed and leaves the step alone. The step stays within [1/TOTK_MAX_FPS,
   1/15 s] (1/TOTK_HOLD_FPS while held, totk_frame_step_hold): below 15 fps the game slows down instead of taking huge physics steps; above TOTK_MAX_FPS (default and highest 240; 360 with
   TOTK_STEP_360=1) it would run fast, but only sustained rates above it do: single short frames are repaid through the owed time. After a cap change (totk_frame_step_retarget) the step
   follows the new rate within ~10 frames instead: EMA 0.5, up to 25 % per frame, and the switch's difference is not owed (repaying it would be the slow or fast motion it avoids). The game
   clock lags or leads real time once by the ramp (240 -> 30: 0.17 s), not by seconds. The fast window ends once it has converged (the step within 3 % of the EMA and the EMA within 3 % of
   the measured time, 3 presents in a row: at EMA 0.5 frame-time jitter would otherwise move the step up to 25 % a frame for the whole window), TOTK_RETARGET_PRESENTS at most. The frame
   rate also moves by itself: the camera turns into a heavier view and a GPU-bound 120 fps falls to 58. Under the 3 % ramp and the 0.05 EMA the step took ~1 s to follow, so the game ran at
   ~0.5x and then paid the owed time back fast: motion slowed and sped up at every camera move, never at a steady 30; at 60 a dip to 55 still ran ~0.9x for ~0.3 s ("almost imperceptible").
   So FOLLOW_RUN presents in a row more than FOLLOW_RATIO above (or below) the EMA open the same fast window (an isolated hitch, or frames alternating around the mean, never do), with one
   difference: the time it lags stays owed, since nobody chose the new rate (the clock keeps real time), and the steady EMA is 0.1 (0.05 before): smaller drifts follow in ~10 frames, the
   step's own jitter at a noisy steady rate grows from 1.5 % to 1.8 %. TOTK_STEP_FOLLOW=0: the 3 % ramp and the 0.05 EMA alone. A lone hitch is not owed (LEAD, v0.1.10): one frame over
   HITCH_RATIO x the EMA, with no other in the HITCH_LONE s before it and the next frame back under FOLLOW_RATIO x the EMA, is a freeze the player already saw; repaying its lag gave ~2 s
   at 1.03-1.05x after it (the v0.1.9 gate: the bow's first draw, a 117 ms present, owed 91 ms), a speed swing of its own. Its time is dropped (the game clock ends that much behind real
   time) and it moves neither the EMA nor the step. Decided on the next present: if that one is long too, the drop is sustained and the hitch is owed as before, as are hitches closer
   together than HITCH_LONE (repeating ones would otherwise slow the game for good). TOTK_STEP_HITCH=0: every hitch owed, as before.*/
#define FOLLOW_RATIO 1.1
#define FOLLOW_RUN 3
#define HITCH_RATIO 2.0   /* (the gate's hitch: a frame at twice the mean or more) */
#define HITCH_LONE 3.0
static double frame_step_next(double real, double lo, double hi) {
    double dt = real < lo ? lo : real > hi ? hi : real;
    if (!(g_step > 0)) g_step = g_smooth = dt;
    double up = 1.03, down = 0.97;
    static int settled;   /* fast-window presents in a row that were converged */
    static int follow;    /* presents left in a sustained rate change's fast window */
    static int run;       /* presents in a row measured above (> 0) or below (< 0) the EMA by FOLLOW_RATIO */
    static int follow_on = -1, hitch_on = -1;
    static double pending_real, pending_dt;     /* (the lone hitch's measured and clamped time; its lag: g_hitch_lag) */
    double pending = g_hitch_lag;
    static double since = HITCH_LONE;           /* real time since the last hitch */
    if (follow_on < 0) { const char *e = getenv("TOTK_STEP_FOLLOW"); follow_on = !(e && !strcmp(e, "0")); }
    if (hitch_on < 0) { const char *e = getenv("TOTK_STEP_HITCH"); hitch_on = !(e && !strcmp(e, "0")); }
    int left = __atomic_load_n(&g_retarget, __ATOMIC_RELAXED);
    if (pending >= 0) {
        if (dt > g_smooth * FOLLOW_RATIO) {   /* (not lone: owed, and in the EMA, as any other frame) */
            g_debt += pending;
            g_smooth += (pending_dt - g_smooth) * (follow_on ? 0.1 : 0.05);
        } else {
            const unsigned n = ++g_hitch_dropped;
            if (n <= 20 || n % 100 == 0)
                fprintf(stderr, "[totk] hitch not owed #%u: a %.1f ms frame against a %.2f ms mean (%.0f ms dropped)\n",
                        n, pending_real * 1e3, g_smooth * 1e3, pending * 1e3);
        }
        g_hitch_lag = -1;
    }
    const int hitch = dt > g_smooth * HITCH_RATIO;
    const int lone = hitch && hitch_on && since >= HITCH_LONE;
    since = hitch ? 0 : since + real;
    if (follow_on && left <= 0 && follow <= 0) {
        int s = dt > g_smooth * FOLLOW_RATIO ? 1 : dt * FOLLOW_RATIO < g_smooth ? -1 : 0;
        run = s && (run > 0) == (s > 0) ? run + s : s;
        if (run >= FOLLOW_RUN || run <= -FOLLOW_RUN) {
            const unsigned opened = ++g_follow_opened;
            follow = TOTK_RETARGET_PRESENTS, run = 0;
            if (opened <= 20 || opened % 100 == 0)
                fprintf(stderr, "[totk] step follow #%u: %.2f ms frames against a %.2f ms mean\n", opened, dt * 1e3,
                        g_smooth * 1e3);
        }
    }
    if (left > 0 || follow > 0) {
        g_smooth += (dt - g_smooth) * 0.5;
        up = 1.25, down = 1 / 1.25;
        settled = fabs(g_step / g_smooth - 1) < 0.03 && fabs(g_smooth / dt - 1) < 0.03 ? settled + 1 : 0;
        if (left > 0) {
            g_debt = 0;
            follow = 0;
            /* (a CAS, so a retarget stored meanwhile by another thread is not lost) */
            __atomic_compare_exchange_n(&g_retarget, &left, settled >= 3 ? 0 : left - 1, 0, __ATOMIC_RELAXED,
                                        __ATOMIC_RELAXED);
        } else {
            g_debt += real - g_step;             /* (owed, as below) */
            follow = settled >= 3 ? 0 : follow - 1;
        }
    } else if (lone) {
        settled = 0;
        g_hitch_lag = real - g_step, pending_real = real, pending_dt = dt;
    } else {
        settled = 0;
        g_smooth += (dt - g_smooth) * (follow_on ? 0.1 : 0.05);
        g_debt += real - g_step;                 /* the step written last was given for this frame; owed from the real
                                                    time, so frames shorter than the clamp are repaid too (review N14) */
    }
    if (g_debt > 0.5) g_debt = 0.5;           /* bounded (an interval switch 60 -> 30 owes ~0.25 s) */
    if (g_debt < -0.5) g_debt = -0.5;
    /* (the repayment at most +-5 % of the step: a hitch owed or a dip's lag comes back as a 1.05x stretch, not 1.2x) */
    double repay = g_debt * 0.02, most = g_smooth * 0.05;
    double step = g_smooth + (repay > most ? most : repay < -most ? -most : repay);
    if (step > g_step * up) step = g_step * up;
    if (step < g_step * down) step = g_step * down;
    if (step < lo) step = lo;
    if (step > hi) step = hi;
    return g_step = step;
}

/* The scene scale the renderer uses (main+0xc60994 stores dynamic scale * scene width / output width at +0xccc, the
   output width at +0xcc0, the scene width at +0xcc4): logged when it changes, at most once a second, so a boot log shows
   whether dynamic resolution lowered the scene. */
static void log_scene_scale(uint64_t now_ns) {
    static uint64_t last_ns;
    static float last_scale = -1.0f;
    if (now_ns - last_ns < 1000000000ull) return;
    uint64_t r = __atomic_load_n((uint64_t *)(g_page + TOTK_DP_RENDERER), __ATOMIC_ACQUIRE);
    if (!r) return;
    float ccc, cc0, cc4, cc8;
    memcpy(&cc0, (const void *)(uintptr_t)(r + 0xcc0), 4);
    memcpy(&cc4, (const void *)(uintptr_t)(r + 0xcc4), 4);
    memcpy(&cc8, (const void *)(uintptr_t)(r + 0xcc8), 4);
    memcpy(&ccc, (const void *)(uintptr_t)(r + 0xccc), 4);
    if (!(cc4 > 0) || !(cc0 > 0)) return;
    float scale = ccc * cc0 / cc4;
    if (fabsf(scale - last_scale) < 0.005f) return;
    last_ns = now_ns, last_scale = scale;
    fprintf(stderr, "[totk] scene scale %.3f: %.0fx%.0f of %.0fx%.0f (dynamic resolution %s)\n", scale, cc4 * scale,
            cc8 * scale, cc4, cc8, totk_dynamic_res_current() >= 1.0f ? "off" : "on");
}

/* TOTK_DEBUG_DYNRES=1 (diagnostics: what feeds the game's dynamic-resolution controller): once a second, the
   ResolutionMgr (renderer +0x9f0) as game::gfx::ResolutionMgr::calcDynamicResolution (main+0xc62128) reads it. Before
   each call main+0xc60934 copies two per-frame sums into +0x6c / +0x74 (the previous ones to +0x70 / +0x78): +0x60c /
   +0x610 of the renderer's +0x900 entry, filled at main+0x9b25d0 from six and four 0x98-strided fields of a per-frame
   record (GPU pass timings, to be confirmed by this log). The controller's load is +0x48 = max(base +0x44, base + 16.5
   x ((A + B/2) - (A' + B'/2)) / 100000), base = a global + 3 (6 at the 20 FPS interval); +0x4c is the events'
   setGPULoad hint, +0x54 the level picked. */
static void log_dynres(uint64_t now_ns) {
    static int on = -1;
    static uint64_t last_ns;
    if (on < 0) { const char *e = getenv("TOTK_DEBUG_DYNRES"); on = e && *e && strcmp(e, "0"); }
    if (!on || now_ns - last_ns < 1000000000ull) return;
    last_ns = now_ns;
    uint64_t r = __atomic_load_n((uint64_t *)(g_page + TOTK_DP_RENDERER), __ATOMIC_ACQUIRE), m;
    if (!r) return;
    memcpy(&m, (const void *)(uintptr_t)(r + 0x9f0), 8);
    if (!m) return;
    uint32_t u[16];
    memcpy(u, (const void *)(uintptr_t)(m + 0x40), sizeof u);
    float f44, f48, f4c;
    memcpy(&f44, &u[1], 4), memcpy(&f48, &u[2], 4), memcpy(&f4c, &u[3], 4);
    fprintf(stderr, "[totk] dynres: level %d, load %.3f (base %.3f), hint %.3f, A %u (was %u), B %u (was %u), flags %#x\n",
            (int)u[5], f48, f44, f4c, u[11], u[12], u[13], u[14], u[15] & 0xff);
}

/* TOTK_DEBUG_CAMERA=1 (diagnostics, to find the camera's yaw / pitch): twice a second, the float fields of the
   PlayerCamera (data page +136) in its first 0x800 bytes that changed since the last sample, as offset=value. */
static void log_camera_fields(uint64_t now_ns) {
    static int on = -1;
    static uint64_t last_ns;
    static float prev[0x200];
    static uint64_t prev_cam;
    if (on < 0) { const char *e = getenv("TOTK_DEBUG_CAMERA"); on = e && *e && strcmp(e, "0"); }
    if (!on || now_ns - last_ns < 500000000ull) return;
    last_ns = now_ns;
    uint64_t cam = __atomic_load_n((uint64_t *)(g_page + TOTK_DP_CAMERA), __ATOMIC_ACQUIRE);
    if (!cam) return;
    float cur[0x200];
    memcpy(cur, (const void *)(uintptr_t)cam, sizeof cur);
    if (cam == prev_cam) {
        char line[2048];
        int n = snprintf(line, sizeof line, "[totk] camera %#llx changed:", (unsigned long long)cam), shown = 0;
        for (int i = 0; i < 0x200 && n < (int)sizeof line - 32 && shown < 48; i++) {
            if (cur[i] != prev[i] && isfinite(cur[i]) && fabsf(cur[i]) < 1e6f && fabsf(cur[i] - prev[i]) > 1e-5f) {
                n += snprintf(line + n, sizeof line - n, " %x=%.4g", i * 4, cur[i]);
                shown++;
            }
        }
        if (shown) fprintf(stderr, "%s\n", line);
    }
    memcpy(prev, cur, sizeof cur);
    prev_cam = cam;
}

/* Review D1: the mouse veneer runs only inside the PlayerCamera's stick step, so in the map, menus, cutscenes or another
 * camera mode it can no longer clear its claim, and platform would keep its stick emulation off. The claim is a
 * heartbeat instead: once no step ran for more than TOTK_MOUSE_STALE_PRESENTS presents, release it here (the veneer
 * primes again when the step comes back). Only 1 -> 0, so a prime racing this release just re-claims next step. */
static void release_stale_mouse(uint32_t presents) {
    uint32_t *claimed = (uint32_t *)(g_page + TOTK_DP_PC_CLAIMED), one = 1;
    if (!__atomic_load_n(claimed, __ATOMIC_ACQUIRE)) return;
    uint32_t beat = __atomic_load_n((uint32_t *)(g_page + TOTK_DP_MOUSE_BEAT), __ATOMIC_ACQUIRE);
    if (presents - beat > TOTK_MOUSE_STALE_PRESENTS)
        __atomic_compare_exchange_n(claimed, &one, 0u, 0, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED);
}

/* The game's speed: every 2 s of realtime play, the game time given (each step for the interval that followed it) per
   real second, its lowest and highest over the 0.25 s windows inside, the mean frame and step, the time still owed and
   the follow windows opened and the lone hitches not owed since the last line. A step that lags a frame-rate change
   shows as a 0.25 s window under ~0.95x and then one above ~1.05x (the owed time repaid); a lone hitch only as its own
   window's dip (the freeze), with no faster window after it. A gap (over 0.25 s: a load) or a pinned step is not
   counted, and the windows start again after it; the Switch timing (realtime off) logs nothing.*/
static void log_game_speed(double real, double given, int skip) {
    static double win_real, win_game, sub_real, sub_game, lo, hi;
    static unsigned frames, follow_was, hitch_was;
    if (skip) { win_real = win_game = sub_real = sub_game = 0, frames = 0; return; }
    if (!frames) lo = 1e9, hi = 0, follow_was = g_follow_opened, hitch_was = g_hitch_dropped;
    frames++, win_real += real, win_game += given, sub_real += real, sub_game += given;
    if (sub_real >= 0.25) {
        const double s = sub_game / sub_real;
        if (s < lo) lo = s;
        if (s > hi) hi = s;
        sub_real = sub_game = 0;
    }
    if (win_real < 2.0) return;
    fprintf(stderr, "[totk] game speed %.3fx over %.1f s (0.25 s windows %.2fx .. %.2fx), frames %.2f ms, step %.2f ms, "
            "owed %+.0f ms, follow %u, hitches not owed %u\n", win_game / win_real, win_real, lo, hi,
            win_real / frames * 1e3, win_game / frames * 1e3, g_debt * 1e3, g_follow_opened - follow_was,
            g_hitch_dropped - hitch_was);
    win_real = win_game = 0, frames = 0;   /* (the 0.25 s window under way goes on into the next line) */
}

/* The effectease patch's page values (per-frame sweep R5; sites_v100.py effectease), from the step the next frame runs
   (the ratio just stored): for r in (0.01, 1) the game's 0.01-per-frame ease becomes 1 - 0.99^r, the Switch's time
   constant, and its per-frame counts step by the 30 Hz ticks of game time the frames add up to (TOTK_DP_TICKS30 =
   ticks - 1: -1 on most frames above 30 fps, 0 on the frame that completes a tick). At 30 fps and below, or with
   TOTK_EFFECT_EASE=0, both are 0: the game's own k and step. */
static void put_eases(void) {
    static int on = -1;
    static double acc;   /* (game time not yet counted as a tick, in 30 Hz frames) */
    if (on < 0) {
        const char *e = getenv("TOTK_EFFECT_EASE");
        on = !e || strcmp(e, "0");
        fprintf(stderr, "[totk] altitude effect fade / burst %s\n", on ? "in game time" : "in frames, as the game (TOTK_EFFECT_EASE=0)");
    }
    uint32_t bits = __atomic_load_n((uint32_t *)(g_page + TOTK_DP_FRAME_RATIO), __ATOMIC_ACQUIRE);
    float r;
    memcpy(&r, &bits, 4);
    if (!on || !(r > 0.01f && r < 1.0f)) {
        put_f32(TOTK_DP_EASE_K001, 0.0f);
        __atomic_store_n((int32_t *)(g_page + TOTK_DP_TICKS30), 0, __ATOMIC_RELEASE);
        acc = 0;
        return;
    }
    double k = 1.0 - pow(1.0 - (double)0.01f, (double)r);
    put_f32(TOTK_DP_EASE_K001, (float)(k < 0 ? 0 : k > 1 ? 1 : k));
    acc += r;
    const int ticks = (int)acc;
    acc -= ticks;
    __atomic_store_n((int32_t *)(g_page + TOTK_DP_TICKS30), ticks - 1, __ATOMIC_RELEASE);
}

static void frame_step_update(uint64_t now_ns);

void totk_frame_step_update(uint64_t now_ns) {
    if (!g_page || !now_ns) return;
    frame_step_update(now_ns);
    put_eases();
}

static void frame_step_update(uint64_t now_ns) {
    uint32_t presents = __atomic_add_fetch((uint32_t *)(g_page + TOTK_DP_PRESENTS), 1u, __ATOMIC_ACQ_REL);
    release_stale_mouse(presents);
    log_scene_scale(now_ns);
    log_camera_fields(now_ns);
    log_dynres(now_ns);
    if (!g_set.realtime) {
        int n = __atomic_load_n(&g_interval, __ATOMIC_RELAXED);
        put_frame_step((float)n / 60.0f);   /* shipping: ratio = interval * 0.5, seconds = interval / 60 */
        return;
    }
    uint32_t pinned = __atomic_load_n(&g_pinned_us, __ATOMIC_RELAXED);
    if (pinned) {   /* (unpinned: ramp from it) */
        g_last_present_ns = now_ns, g_step = pinned / 1e6, g_debt = 0, g_hitch_lag = -1;
        log_game_speed(0, 0, 1);
        return;
    }
    if (!g_last_present_ns || now_ns <= g_last_present_ns) { g_last_present_ns = now_ns; return; }
    double dt = (double)(now_ns - g_last_present_ns) * 1e-9;
    g_last_present_ns = now_ns;
    if (dt > 0.25 || dt < 0.0005) { g_debt = 0, g_hitch_lag = -1, log_game_speed(0, 0, 1); return; }   /* a gap: not owed, the step stays */
    log_game_speed(dt, g_step, !(g_step > 0));
    double lo = g_set.min_frame_ms * 1e-3, hi = g_set.max_frame_ms * 1e-3;
    int hold = __atomic_load_n(&g_hold, __ATOMIC_RELAXED);
    int clamped = 0;
    if (hold && lo < 1.0 / TOTK_HOLD_FPS) {
        lo = 1.0 / TOTK_HOLD_FPS;
        clamped = dt < lo;
        __atomic_add_fetch(&g_hold_frames, 1, __ATOMIC_RELAXED);
        if (clamped) __atomic_add_fetch(&g_hold_clamped, 1, __ATOMIC_RELAXED);
    }
    double step = frame_step_next(dt, lo, hi);
    /* (held: running ahead because of the floor is not owed, totk_frame_step_hold; a frame the floor did not touch
       repays as any other, else a held fast travel at 120 fps with jitter ran 14 ms ahead in 5 s, test_totk_patches) */
    if (clamped && g_debt < 0) g_debt = 0;
    put_frame_step((float)step);
}

uint64_t totk_output_size_current(void) {
    return g_page ? __atomic_load_n((uint64_t *)(g_page + TOTK_DP_OUTPUT_SIZE), __ATOMIC_ACQUIRE) : pack_size(1920, 1080);
}

/* The game re-runs its display layout (main+0xc63064) when the operation mode changed or bit 0x200 of the renderer's
   +0xca4 is set (main+0x1185358, the handler of oe message 30). Setting the bit and posting the message takes the path a
   Switch takes when it is docked or undocked; the mode itself stays docked. */
int totk_output_res_set(unsigned w, unsigned h) {
    if (!g_page) return 0;
    if (w < 640 || h < 360 || w > g_set.max_w || h > g_set.max_h) {
        fprintf(stderr, "[totk] output %ux%u refused: the swapchain is %ux%u (TOTK_OUTPUT_MAX)\n", w, h, g_set.max_w, g_set.max_h);
        return 0;
    }
    put_u64(TOTK_DP_OUTPUT_SIZE, pack_size(w, h));
    put_f32(TOTK_DP_ASPECT, effective_aspect(&g_set, w, h));
    uint64_t renderer = __atomic_load_n((uint64_t *)(g_page + TOTK_DP_RENDERER), __ATOMIC_ACQUIRE);
    if (renderer && g_post_oe) {
        __atomic_fetch_or((uint32_t *)(uintptr_t)(renderer + 0xca4), 0x200u, __ATOMIC_SEQ_CST);
        g_post_oe(30);
    }
    fprintf(stderr, "[totk] output %ux%u%s\n", w, h,
            !renderer ? " (the game has not laid out its displays yet; it takes this size when it does)"
            : !g_post_oe ? " (no oe message hook: takes effect at the next mode change)" : " from the next layout");
    return 1;
}

int totk_output_res_toggle(void) {
    if (!g_page) return 0;
    if (g_set.max_w == 1920 && g_set.max_h == 1080) {
        fprintf(stderr, "[totk] output toggle needs a swapchain bigger than 1920x1080 (TOTK_OUTPUT_MAX)\n");
        return 0;
    }
    if (effective_aspect(&g_set, g_set.max_w, g_set.max_h) != 0) {   /* the UI overlay is laid out for that shape */
        fprintf(stderr, "[totk] output toggle is for 16:9 swapchains (this one is %ux%u)\n", g_set.max_w, g_set.max_h);
        return 0;
    }
    int full = totk_output_size_current() == pack_size(g_set.max_w, g_set.max_h);
    return full ? totk_output_res_set(1920, 1080) : totk_output_res_set(g_set.max_w, g_set.max_h);
}

void totk_aspect_set(float ratio) {
    if (!g_page) return;
    TotkSettings s = g_set;
    s.aspect = ratio;
    uint64_t cur = totk_output_size_current();
    put_f32(TOTK_DP_ASPECT, effective_aspect(&s, (unsigned)cur, (unsigned)(cur >> 32)));
    g_set.aspect = ratio;
}

float totk_aspect_current(void) {
    if (!g_page) return 0;
    uint32_t bits = __atomic_load_n((uint32_t *)(g_page + TOTK_DP_ASPECT), __ATOMIC_ACQUIRE);
    float v;
    memcpy(&v, &bits, 4);
    return v;
}

void totk_dynamic_res_set(float floor) {
    if (!(floor >= 0.0f)) floor = 0.0f;   /* NaN and negatives: the game's own */
    if (floor > 1.0f) floor = 1.0f;
    g_set.res_floor = floor;
    if (g_page) put_f32(TOTK_DP_RES_FLOOR, floor);
}

float totk_dynamic_res_current(void) {
    if (!g_page) return g_set.res_floor;
    uint32_t bits = __atomic_load_n((uint32_t *)(g_page + TOTK_DP_RES_FLOOR), __ATOMIC_ACQUIRE);
    float v;
    memcpy(&v, &bits, 4);
    return v;
}

void totk_fov_set(float scale) {
    if (!(scale > 0.0f) || scale == 1.0f) scale = 0.0f;   /* 0, 1, NaN, negatives: the game's own */
    else if (scale < 0.5f) scale = 0.5f;
    else if (scale > 2.0f) scale = 2.0f;
    g_set.fov_scale = scale;
    if (g_page) put_f32(TOTK_DP_FOV_SCALE, scale);
}

float totk_fov_current(void) {
    if (!g_page) return g_set.fov_scale;
    uint32_t bits = __atomic_load_n((uint32_t *)(g_page + TOTK_DP_FOV_SCALE), __ATOMIC_ACQUIRE);
    float v;
    memcpy(&v, &bits, 4);
    return v;
}

void totk_shadow_res_set(float scale) {
    if (!(scale > 0.0f) || scale == 1.0f) scale = 0.0f;   /* 0, 1, NaN, negatives: the game's own */
    else if (scale < 0.25f) scale = 0.25f;   /* (integrated GPUs: 1/16 the memory; the game keeps each size >= 32, x8) */
    else if (scale > 2.0f) scale = 2.0f;   /* the game's pools are not grown: 2 = 4x the memory (review D3) */
    g_set.shadow_scale = scale;
    if (g_page) put_f32(TOTK_DP_SHADOW_SCALE, scale);
}

float totk_shadow_res_current(void) {
    if (!g_page) return g_set.shadow_scale;
    uint32_t bits = __atomic_load_n((uint32_t *)(g_page + TOTK_DP_SHADOW_SCALE), __ATOMIC_ACQUIRE);
    float v;
    memcpy(&v, &bits, 4);
    return v;
}

void totk_temp_unit_set(unsigned unit) {
    g_set.temp_unit = unit <= 2 ? unit : 0;
    if (g_page) __atomic_store_n((uint32_t *)(g_page + TOTK_DP_TEMP_UNIT), g_set.temp_unit, __ATOMIC_RELEASE);
}

void totk_lod_set(float scale) {
    if (!(scale > 0.0f) || scale == 1.0f) scale = 0.0f;   /* 0, 1, NaN, negatives: the game's own */
    else if (scale < 0.5f) scale = 0.5f;
    else if (scale > 8.0f) scale = 8.0f;
    g_set.lod_scale = scale;
    if (g_page) put_f32(TOTK_DP_LOD_SCALE, scale);
}

float totk_lod_current(void) {
    if (!g_page) return g_set.lod_scale;
    uint32_t bits = __atomic_load_n((uint32_t *)(g_page + TOTK_DP_LOD_SCALE), __ATOMIC_ACQUIRE);
    float v;
    memcpy(&v, &bits, 4);
    return v;
}

static const TotkSettingRow g_rows[] = {
    {"graphics.min_resolution", "", "Minimum render resolution",  // (no page: set through the "Dynamic resolution")
     "100 %: the scene is always rendered at the render resolution (dynamic resolution off). 0 %: the game's own dynamic "
     "resolution. In between: dynamic, never below that fraction.", TOTK_DP_RES_FLOOR, 1.0, 0.0, 1.0, 0.05, 1},
    {"graphics.fov", "Graphics", "Field of view",
     "Multiplier of the camera's field of view (1 = the game's). The degrees are the camera's view right now: aiming "
     "and some cutscenes narrow or widen it, and the multiplier keeps their proportion. Wider costs some GPU and CPU "
     "(more of the world on screen).", TOTK_DP_FOV_SCALE, 1.0, 0.5, 2.0, 0.05, 0},
    {"graphics.shadow_resolution", "Graphics", "Shadow resolution",
     "Multiplier of the shadow map's width and height (2 = sharper, 4x the memory).", TOTK_DP_SHADOW_SCALE, 1.0, 0.25,
     2.0, 0.25, 0},   /* (0.25: integrated GPUs, 1/16 the memory; the help text is a translation key, kept) */
    {"graphics.shadow_fixed", "Graphics", "Sun shadow map size",
     "The game's: the sun's shadow map shrinks and grows with the area its cascades cover, so near the ground foliage "
     "shadows turn coarse, and every size change moves the shadow texels (leaf shadows jump instead of sliding). Fixed: "
     "always the largest size the game picks (times the shadow resolution), steady and sharper; costs some GPU.",
     TOTK_DP_SHADOW_FIXED, 0.0, 0.0, 1.0, 1.0, 0},
    {"graphics.shadow_reach", "Graphics", "Near shadow reach",
     "Multiplier of how far the sun's sharpest shadow cascades reach (1 = the game's: the first two hand over 32 m and "
     "64 m away, where the change in sharpness shows as a line, mostly at 4K). 2 moves those lines to 64 m and 128 m; "
     "the far shadows stay. The same shadow map then covers more ground, so near shadows get softer by that factor: "
     "raise Shadow resolution with it. Costs a little GPU and CPU (more objects drawn into the near shadow maps).",
     TOTK_DP_SHADOW_SPLIT, 1.0, 1.0, 4.0, 0.25, 0},
    {"graphics.lod_distance", "Graphics", "Detail distance",
     "Multiplier of the distance at which models switch to lower detail (2 = full detail twice as far).",
     TOTK_DP_LOD_SCALE, 1.0, 0.5, 8.0, 0.25, 0},
    {"graphics.actor_detail_distance", "Graphics", "Actor detail distance",
     "Multiplier of the distances at which characters, enemies and objects start skipping update, animation and draw "
     "frames (2 = full rate twice as far; costs CPU).", TOTK_DP_ACTOR_LOD, 1.0, 0.5, 4.0, 0.25, 0},
    {"graphics.terrain_detail_distance", "Graphics", "Terrain detail distance",
     "Multiplier of the distances at which the ground and water switch to coarser levels (2 = full detail twice as far; "
     "costs CPU and GPU).", TOTK_DP_TERRAIN_LOD, 1.0, 0.5, 2.0, 0.25, 0},   /* x3 crashes FUN_ce54a0 at the world load */
    {"game.simulation_distance", "Game", "Simulation distance",
     "Multiplier of the distance from the camera at which characters, enemies, objects and their physics stop being "
     "updated (1 = the game's; 1.5-2 = high). Costs CPU: more actors are simulated every frame, and distant ones "
     "keep reacting (fights, physics) instead of freezing.", TOTK_DP_SIM_RADIUS, 1.0, 0.5, 4.0, 0.25, 0},
    {"controls.mouse_camera_scale", "Controls", "Mouse camera (direct)",
     "Multiplier of the camera turn the game itself applies per mouse count (on top of the mouse sensitivity). 0 = off: "
     "the mouse turns the camera through emulated right-stick deflection instead.", TOTK_DP_MOUSE_SCALE, 1.0, 0.0, 4.0,
     0.05, 0},
};

const TotkSettingRow *totk_setting_rows(unsigned *count) {
    if (count) *count = sizeof g_rows / sizeof *g_rows;
    return g_rows;
}

#define PRESET(name, minres, shadowres, lod, actor, terrain, tex, grass, shadowdist)                                     \
    static const TotkPresetValue name[] = {                                                                             \
        {"graphics.min_resolution", minres},        {"graphics.shadow_resolution", shadowres},                          \
        {"graphics.lod_distance", lod},             {"graphics.actor_detail_distance", actor},                          \
        {"graphics.terrain_detail_distance", terrain}, {"graphics.texture_cache", tex},                                 \
        {"graphics.grass_density", grass},          {"graphics.shadow_distance", shadowdist},                           \
    };
/*       name       min res  shadow res  LOD    actor  terrain  texture  grass  shadow distance */
PRESET(k_low,      "0",     "1",        "1",   "1",   "1",     "1",     "1",   "1")
PRESET(k_medium,   "1",     "1",        "1.5", "1.5", "1.5",   "1.5",   "1",   "1.5")
PRESET(k_high,     "1",     "2",        "2",   "2",   "2",     "2",     "2",   "2")
PRESET(k_ultra,    "1",     "2",        "4",   "3",   "2",     "3",     "3",   "3")   /* terrain x3 crashes: 2 */
PRESET(k_ultraplus, "1",    "2",        "8",   "4",   "2",     "3",     "4",   "4")   /* each knob at its maximum */
#undef PRESET

static const TotkQualityPreset g_presets[] = {
    {"Low", k_low, sizeof k_low / sizeof *k_low},
    {"Medium", k_medium, sizeof k_medium / sizeof *k_medium},
    {"High", k_high, sizeof k_high / sizeof *k_high},
    {"Ultra", k_ultra, sizeof k_ultra / sizeof *k_ultra},
    {"Ultra+", k_ultraplus, sizeof k_ultraplus / sizeof *k_ultraplus},
};

const TotkQualityPreset *totk_quality_presets(unsigned *count) {
    if (count) *count = sizeof g_presets / sizeof *g_presets;
    return g_presets;
}

/* DynamicTextureAllocatorMem (113664 KiB shipped) grows with the output's pixels above 1920x1080, and with a scene
   bigger than 1600x900 (with a 1.6 margin), rounded up to whole MiB; Graphics and the application pool grow by the same. */
static uint64_t output_extra_bytes(const TotkSettings *s) {
    double out = (double)s->max_w * s->max_h / (1920.0 * 1080.0) - 1.0;
    int scene = (double)s->render_w * s->render_h > 1600.0 * 900.0;
    double grow = (out > 0 ? out : 0) + (scene ? (double)s->render_w * s->render_h / (1600.0 * 900.0) - 1.0 : 0);
    if (grow <= 0) return 0;
    if (scene) grow *= 1.6;
    uint64_t kib = (uint64_t)ceil(113664.0 * (1.0 + grow) / 1024.0) * 1024 - 113664;
    return kib << 10;
}

/* TextureHeap (501760 KiB shipped; agl::TextureDataMgr's texture cache) and its parent ゲームグラフィックス grow by
   (multiplier - 1) x the shipped size, rounded up to whole MiB. */
static uint64_t tex_extra_bytes(const TotkSettings *s) {
    if (!(s->tex_cache > 1.0f)) return 0;
    double m = s->tex_cache < 3.0f ? s->tex_cache : 3.0;
    return (uint64_t)ceil(501760.0 * (m - 1.0) / 1024.0) * 1024 << 10;
}

uint64_t totk_output_extra_bytes(const TotkSettings *s) {
    return output_extra_bytes(s) + tex_extra_bytes(s);
}

int totk_layout_set(unsigned w, unsigned h, char name[64]) {
    /* the shapes games/totk/overlays builds by default (DEFAULT_ASPECTS): its layouts depend on the shape alone */
    static const unsigned built[][2] = {{3440, 1440}, {5120, 1440}, {7680, 1440}, {2560, 1600}};
    const double a = h ? (double)w / h : 0;
    if (a > 16.0 / 9.0 * 0.99 && a < 16.0 / 9.0 * 1.01) return 0;
    for (unsigned i = 0; i < sizeof built / sizeof built[0]; i++) {
        const double b = (double)built[i][0] / built[i][1];
        if (a > b * 0.99 && a < b * 1.01) w = built[i][0], h = built[i][1];
    }
    snprintf(name, 64, "aspect-%ux%u", w, h);
    return 1;
}

int totk_overlay_sets(const TotkSettings *s, char names[2][64]) {
    int n = 0;
    if (totk_layout_set(s->output_w, s->output_h, names[n]))   /* the UI laid out for the canvas's shape */
        n++;
    else
        snprintf(names[n++], 64, "base");
    if (totk_output_extra_bytes(s)) {
        int scene = (double)s->render_w * s->render_h > 1600.0 * 900.0;
        char tex[16] = "";
        if (tex_extra_bytes(s)) snprintf(tex, sizeof tex, "-tex%d", (int)(s->tex_cache * 100.0f + 0.5f));
        if (scene) snprintf(names[n++], 64, "output-%ux%u-scene-%ux%u%s", s->max_w, s->max_h, s->render_w, s->render_h, tex);
        else snprintf(names[n++], 64, "output-%ux%u%s", s->max_w, s->max_h, tex);
    }
    return n;
}
