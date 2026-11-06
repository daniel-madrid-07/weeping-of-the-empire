// TotK side of the hooks LASRecomp's Vulkan backend calls into its Link's Awakening runtime (widescreen, pots,
// debug tools, SMAA tables, output scaling). None of those features exist here: neutral answers that leave the
// game's own image untouched.
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "../output_scale.h"

uint64_t plat_now_ns(void) {    // monotonic ns (was in backend_null.c)
    static LARGE_INTEGER f;
    LARGE_INTEGER t;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return (uint64_t)((double)t.QuadPart * 1e9 / (double)f.QuadPart);
}

// Image letterboxed into the window at 16:9. Render targets at the game's sizes, or scaled by TOTK_RES_SCALE
// (textures.c tex_scalable: colour/depth targets of the scene's shape, output_scene_size; 1.3333 = 1600x900 -> 2133x1200,
// 1080p output -> 1440p; "auto" = window height / 1080 for a 16:9 scene, res_scale). Shaders see guest sizes through the rscale table (translate.c).
// Live (graphics.render_scale, output_set_res_scale): the value is read at each present, so a change takes the next
// frame; textures.c remakes each scaled target at its next use with its contents carried over (rescale_image), and
// exec.c's upscale program gets the form a start with that value would translate (knob variants).
static int parse_res_scale(const char *e) {   // milli-scale: 0 = "auto", 1000 = 1 (unset, empty or out of range)
    if (!e || !*e) return 1000;
    if (!strcmp(e, "auto")) return 0;
    double s = atof(e);
    return s < 0.5 || s > 4.0 ? 1000 : (int)lround(s * 1000.0);
}
static volatile long g_res_milli = -1;   // -1: TOTK_RES_SCALE not read yet (settings thread writes, present reads)
static volatile long g_res_scaled = -1;  // the value is not "1" / "1.0" / empty (exec.c tp_env's test, kept)
static char g_res_text[16];              // (the [render] line's "TOTK_RES_SCALE=<value>", which boot.py reads)
static void res_scale_init(void) {
    if (g_res_milli >= 0) return;
    const char *e = getenv("TOTK_RES_SCALE");
    g_res_scaled = e && *e && strcmp(e, "1") && strcmp(e, "1.0");
    snprintf(g_res_text, sizeof g_res_text, "%s", e && *e ? e : "unset");
    InterlockedExchange(&g_res_milli, parse_res_scale(e));
}
void output_set_res_scale(const char *v) {
    res_scale_init();
    g_res_scaled = v && *v && strcmp(v, "1") && strcmp(v, "1.0");
    snprintf(g_res_text, sizeof g_res_text, "%s", v && *v ? v : "unset");   // (log text only: a torn read misprints)
    InterlockedExchange(&g_res_milli, parse_res_scale(v));
}
// The scene's size: the TOTK_RENDER_RES (graphics.aspect_ratio's 21:9 / 32:9 / 16:10 scenes 3440x1440 / 5120x1440 /
// 1600x1000 or 2560x1600, the canvas's shape), else the game's 1600x900. textures.c scales the targets of the scene's
// shape (kind 1), so the render scale applies to any shape the same way (until 2026-11-02 only 16:9-shaped targets
// were scaled: at 32:9 that reached the game's few 16:9 targets and left the scene's, a mix of scaled and unscaled
// targets in one pass).
void output_scene_size(unsigned *w, unsigned *h) {
    static uint64_t wh;   // (w << 32 | h, published once: the present and a texture's first kind test may both ask first)
    uint64_t v = __atomic_load_n(&wh, __ATOMIC_ACQUIRE);
    if (!v) {
        const char *e = getenv("TOTK_RENDER_RES");
        unsigned x = 0, y = 0;
        if (!(e && sscanf(e, "%ux%u", &x, &y) == 2 && x >= 64 && y >= 64)) x = 1600, y = 900;
        v = (uint64_t)x << 32 | y;
        __atomic_store_n(&wh, v, __ATOMIC_RELEASE);
    }
    *w = (unsigned)(v >> 32), *h = (unsigned)v;
}
static bool scene_16x9(void) {
    unsigned w, h;
    output_scene_size(&w, &h);
    const double a = (double)w / h;
    return a > 1.74 && a < 1.81;
}

bool output_res_scaled(void) { res_scale_init(); return g_res_scaled > 0; }
// "auto" (the row's default since 11-05: "if I set my game to 1440p it is at 1440p"): the scene as large as its picture
// in the window, fitted by its shape (16:9 1600x900 in 2560x1440: x1.6, 2560x1440; in 3440x1440: the pillarboxed
// 2560x1440 too; 5120x1440 in a 1417-line window: 0.98, so 1). An integrated GPU or one under 8 GB (textures.c
// renderer_auto_scale_small; the row names it) keeps the pick of before: a 16:9 scene as tall as 900 / 1080 of the
// window (whatever TOTK_RENDER_RES gave it), another shape's scaled only as far as that height needs. With an upscaler
// on (DLSS, FSR, XeSS; ngx.c vk_ngx_auto_ratio) the window's picture / 1.5, its Quality input, on any GPU (the smaller
// of the two on a small one), down to x0.5 (1080p: 1280x720).
static double res_scale(double dw, double dh) {
    res_scale_init();
    const long m = g_res_milli;
    if (m == 0) {
        extern int renderer_auto_scale_small(void);
        extern void pc_settings_auto_scale_small(void);   // (pc_settings.cpp: the row's choice says so)
        extern float vk_ngx_auto_ratio(void);
        unsigned w, h;
        output_scene_size(&w, &h);
        const double r = vk_ngx_auto_ratio();
        double a = (dw / w < dh / h ? dw / w : dh / h) / r;
        if (renderer_auto_scale_small()) {
            static int told;
            if (!told++) pc_settings_auto_scale_small();
            double s = dh / 1080.0;
            if (!scene_16x9()) s *= 900.0 / h;
            if (r == 1.0 || s < a) a = s;
        }
        const double lo = r > 1.0 ? 0.5 : 1.0;
        return a < lo ? lo : a > 4.0 ? 4.0 : a;
    }
    return m / 1000.0;
}

void output_scale(double dw, double dh, bool reshape_allowed, double now, double gpu_ms, OutScale *o) {
    (void)reshape_allowed; (void)now; (void)gpu_ms;
    o->sx = o->sy = res_scale(dw, dh); o->shape = 1.0;
    {   // DLAA (ngx.c, the Upscaler row's "dlaa"): the scene at the output's size while it runs; the row's choice otherwise
        extern float vk_ngx_scene_scale(float), renderer_out_scale(void);
        const float w = vk_ngx_scene_scale(renderer_out_scale());
        if (w > 0) o->sx = o->sy = w > 4.0f ? 4.0 : w;
    }
    bool clamped = false;
    {   // (the size actually used: textures.c's limit for the scene's targets, so the line below names the real scene)
        extern float renderer_scale_limit(void);
        const double m = renderer_scale_limit();
        if (o->sx > m) o->sx = o->sy = m, clamped = true;
    }
    static double logged = -1;   // the render size actually used, once per change ("auto" follows the window; a window
    if (o->sx != logged) {       // the system clamps, e.g. 3840x2160 -> 2564x1415, gives 1.31, not 2: boot.py's summary)
        unsigned sw, sh;
        output_scene_size(&sw, &sh);
        printf("[render] scale %.3f (TOTK_RES_SCALE=%s): scene %.0fx%.0f, window %.0fx%.0f%s\n", o->sx, g_res_text,
               sw * o->sx, sh * o->sx, dw, dh, clamped ? " (clamped: the GPU's largest image)" : "");
        logged = o->sx;
    }
    double w = dw, h = dw * 9.0 / 16.0;
    if (h > dh) { h = dh; w = dh * 16.0 / 9.0; }
    o->vw = w; o->vh = h; o->vx = (dw - w) / 2; o->vy = (dh - h) / 2;
}
double output_scale_dynamic_factor(void) { return 1.0; }
static volatile double g_image_aspect;   // 0: 16:9 or unknown (render thread writes, the menu cursor reads; 8 aligned bytes)
void output_set_image_aspect(double a) { g_image_aspect = a > 0 && fabs(a - 16.0 / 9.0) > 0.01 ? a : 0; }
void output_fit_image(double dw, double dh, OutScale *o) {
    const double a = g_image_aspect;
    if (a <= 0) return;
    double w = dw, h = dw / a;
    if (h > dh) { h = dh; w = dh * a; }
    o->vx = (dw - w) / 2, o->vy = (dh - h) / 2, o->vw = w, o->vh = h;
}

void widescreen_frame_end(uint64_t frame) { (void)frame; }
void widescreen_set_factor(float f) { (void)f; }
bool widescreen_allowed(void) { return false; }
// A canvas wider than 16:9 (the ultrawide, TOTK_OUTPUT_RES 3440x1440 / 5120x1440): its middle 16:9 only, pillarboxed
// (device.c fits the cropped image's 16:9 shape), while a pre-rendered movie plays (16:9 video, always) and, with
// graphics.cutscene_aspect = 16:9 (totk_engine.c's g_cutscene_16x9), while the game runs an event (a real-time cutscene
// or a conversation; pc_input.cpp's EventMgr test). A canvas taller than 16:9: a movie's middle 16:9 band, letterboxed.
// Otherwise, and on a 16:9 canvas, the whole image.
extern volatile int g_cutscene_16x9;   // totk_engine.c
extern int movie_playing(void);   // hle_movie.cpp: decoders started
extern int pc_in_event(void);     // pc_input.cpp
static double canvas_aspect(void) {
    static double a = -1;
    if (a < 0) {
        const char *e = getenv("TOTK_OUTPUT_RES");
        unsigned w = 0, h = 0;
        a = e && sscanf(e, "%ux%u", &w, &h) == 2 && h ? (double)w / h : 0;
    }
    return a;
}
extern void totk_window_shape_hint(uint64_t frame);   // totk_engine.c: "restart for the window's new shape"
void widescreen_present_crop(uint64_t frame, double shape, float *crop, double *vx, double *vy, double *vw, double *vh) {
    (void)frame; (void)shape; (void)vx; (void)vy; (void)vw; (void)vh;
    totk_window_shape_hint(frame);
    crop[0] = 0; crop[1] = 0; crop[2] = 1; crop[3] = 1;    // the whole image
    const double a = canvas_aspect();
    if (a > 0 && a < 16.0 / 9.0 - 0.01) {  // taller: a movie's middle 16:9 band, bars above and below
        const int movie = movie_playing() > 0;   // (no 16:9 cutscene framing: the scene is no taller than 16:9's)
        static int was_tall = -1;
        if (movie != was_tall) {
            printf("[present] %s\n", movie ? "movie: the canvas's middle 16:9, bars above and below" : "the whole canvas");
            was_tall = movie;
        }
        if (!movie) return;
        const float f = (float)(a / (16.0 / 9.0));
        crop[1] = (1.0f - f) / 2; crop[3] = f;
        return;
    }
    if (a < 16.0 / 9.0 + 0.01) return;
    const int movie = movie_playing() > 0, event = !movie && g_cutscene_16x9 && pc_in_event();
    static int was = -1;
    if (movie + 2 * event != was) {
        printf("[present] %s\n", movie ? "movie: the canvas's middle 16:9, bars at the sides"
                                 : event ? "event: the canvas's middle 16:9 (graphics.cutscene_aspect = 16:9)"
                                         : "the whole canvas");
        was = movie + 2 * event;
    }
    if (!movie && !event) return;
    const float f = (float)(16.0 / 9.0 / a);
    crop[0] = (1.0f - f) / 2; crop[2] = f;
}
void widescreen_backdrop_seen(uint64_t hash) { (void)hash; }
void widescreen_scene_ubo(const float *u, uint64_t size) { (void)u; (void)size; }

void fnprof_frame(uint64_t frame) { (void)frame; }
void warp_frame(uint64_t frame) { (void)frame; }
void reboot_test_frame(uint64_t frame) { (void)frame; }
void save_editor_frame(uint64_t frame) { (void)frame; }
volatile int g_debug_frame_request;

bool pot_level_active(void) { return false; }
bool pot_glow_active(void) { return false; }
bool pot_tint(const float *m, float tint[8], uint64_t mesh) { (void)m; (void)tint; (void)mesh; return false; }
bool pets_hide_item_cursor(void) { return false; }
void escape_bottle_tint(int kind, const float *ui, uint32_t ui_size, float tint[8]) { (void)kind; (void)ui; (void)ui_size; (void)tint; }

// SMAA 1x on the presented image (TOTK_AA=smaa, user 2026-10-17: "make it look less sharp with anti aliasing"):
// Jimenez et al., MIT (runtime/nvn/smaa/LICENSE.txt), files from LASRecomp's third_party/smaa. The reference shader
// source as a C string (tools/smaa_inc.py -> smaa/smaa_hlsl.inc) and its two lookup textures.
#include "../smaa/AreaTex.h"    // 160 x 560, R8G8
#include "../smaa/SearchTex.h"  // 64 x 16, R8
const char smaa_hlsl_src[] =
#include "../smaa/smaa_hlsl.inc"
    ;
const unsigned char *smaa_area_tex(void) { return areaTexBytes; }
const unsigned char *smaa_search_tex(void) { return searchTexBytes; }

float widescreen_effect_stretch(uint64_t fs_hash) { (void)fs_hash; return 1.0f; }
bool pot_instance_gold(const float *m) { (void)m; return false; }
void pot_gold_tint(float tint[8]) { (void)tint; }
