/* engine (totk-recomp) glue for the TotK game patches: what the engine's runtime calls when its main image was patched by
   `python -m games.totk.patches engine`. Built with games/totk/runtime/totk_patches.c through
   engine/runtime/nvn/totk_game_patches.c; the call sites (engine/runtime):
   rt_main.cpp, after main is loaded and relocated:   totk_engine_boot(MAIN_BASE);
   hle_vi.cpp vi_present_pace (once per present):     totk_engine_interval(interval); totk_engine_present();
   hle_os_mem.cpp os_QueryMemoryInfo:                  mi[0] = kAvailable + totk_engine_extra_memory();
   A main image with `realtime` patched and no totk_engine_boot freezes the game (its frame step reads the data page,
   which stays 0). */
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS   /* getenv */
#endif
#include "../include/totk/data_page.h"
#include "../include/totk/totk_patches.h"
#include "../../../engine/runtime/pc_text.h"
#include "../../../engine/runtime/totk_version.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <windows.h>
#define setenv_(k, v) _putenv_s(k, v)
#define PATH_SEP ';'
#else
#include <time.h>
#include <unistd.h>
#define setenv_(k, v) setenv(k, v, 1)
#define PATH_SEP ':'
#endif

#ifndef TOTK_MAIN_SPAN   /* another game version's build passes its table's span (tools/engine/build.py) */
#define TOTK_MAIN_SPAN 0x4666000ull   /* main's loaded size, .bss included (v100.toml `span`) */
#endif

static TotkSettings g_s;
static int g_on;

static uint64_t now_ns(void) {
#ifdef _WIN32
    static LARGE_INTEGER f;
    LARGE_INTEGER t;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return (uint64_t)((double)t.QuadPart * 1e9 / (double)f.QuadPart);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#endif
}

/* `realtime` rewrites the frame step inside f_7a83e4 (its first patched word, main+0x7a83f4, was `scvtf s0, w1`);
   the engine's TOTK_SPEED_FIX hook rescales that step after the call, so a patched build boots with TOTK_SPEED_FIX=0
   (engine-patches.json `runtime_env`). Only a warning here, not a switch: TOTK_IMG can name another build's images
   (tools/engine/boot.py falls back to the newest image directory), so the loaded image does not prove what the exe
   was generated from. 1.4.3 has its own sites (games/totk/patches/sites_v143.py): its realtime's global ratio site,
   main+0x2daa54, was `ldr s0, [x19, #0x250]`. */
static int main_has_realtime(uint64_t main_base) {
    uint32_t w;
#if defined(TOTK_GAME_VERSION) && TOTK_GAME_VERSION == 143
    static const uint64_t kRealtimeSite143 = 0x2daa54;   /* a 1.4.3 offset already: no totk_main_off */
    memcpy(&w, (const void *)(uintptr_t)(main_base + kRealtimeSite143), 4);
    return w != 0xbd425260;
#else
    if (!totk_main_known(0x7a83f4)) return 0;   /* (a diagnostic build of another version: the site has no place) */
    memcpy(&w, (const void *)(uintptr_t)(main_base + totk_main_off(0x7a83f4)), 4);
    return w != 0x1e220020;
#endif
}

/* Another game version's set (sites_v143.py) has, of the Settings rows' features, only dynres, fov and shadowres, and
   no RomFS sets (the restart rows and the canvas shape need 1.0.0's): its build offers those live rows alone. Each split is
   an #ifdef that a 1.0.0 build drops, so its preprocessed code stays what it was (tools/engine/proof_totk_same.py). */
#if defined(TOTK_GAME_VERSION) && TOTK_GAME_VERSION != 100
#define TOTK_OTHER_VERSION 1
#endif

/* Default on since the all-on round (the play package runs the patched key): with the realtime step in main and
   TOTK_SPEED_FIX not given, the speed fix goes off here, before its hook's first call reads the variable. A
   TOTK_SPEED_FIX given explicitly wins (with a warning when it would rescale the step). The image is TOTK_IMG's: with
   a patched image under an unpatched exe (a tool's fallback to the newest image directory) this turns the fix off
   wrongly, which only costs game speed above 30 fps in that dev boot; the package and key exes load their own image. */
const char *totk_link_patches(void);   /* engine/runtime/exe_tag.cpp (exe_tag.h): NULL = no tag, "" = no patch set */

/* Is `name` one of the set's features? The set is "totk-fps60+realtime+..." (the patches file's name) or "fps60+realtime"
   (the tag's list): whole tokens between '+', the first one after an optional "totk-".*/
static int patch_set_has(const char *set, const char *name) {
    if (!strncmp(set, "totk-", 5)) set += 5;
    const size_t n = strlen(name);
    for (const char *t = set; *t;) {
        const char *end = strchr(t, '+');
        const size_t len = end ? (size_t)(end - t) : strlen(t);
        if (len == n && !strncmp(t, name, n)) return 1;
        if (!end) break;
        t = end + 1;
    }
    return 0;
}

static void speed_fix_default(uint64_t main_base) {
    const char *e = getenv("TOTK_SPEED_FIX");
    /* (v0.1.10, LEAD 12:54: a package's player.txt sets TOTK_SPEED_FIX=0 for its patched key; code without the realtime
       step then runs on the game's interval step, fps / 60 x speed. The speed fix's hook reads the variable at its first
       call, after this. What the CODE has comes from the exe's tag (exe_tag.h: the linked key's patch set; the 11-03 slot
       exe, an unpatched key under the patched image, passed the image test); an exe with no tag falls back to the
       image's words, and hle_vi.cpp's step check still watches in play.) */
    const char *linked = totk_link_patches();
    const int image = main_has_realtime(main_base), code = linked ? patch_set_has(linked, "realtime") : image;
    if (linked && code && !image)
        fprintf(stderr, "[totk] warning: the exe was linked with the realtime step (%s) but TOTK_IMG's main has none: "
                        "another build's image\n", linked);
    if (!code) {
        const char *ck = getenv("TOTK_STEP_CHECK");   /* (=0: the fixed step on purpose, an A/B) */
        if (e && !strcmp(e, "0") && !(ck && !strcmp(ck, "0"))) {
            setenv_("TOTK_SPEED_FIX", "1");
            fprintf(stderr, "[totk] WARNING: %s has NO realtime frame step but TOTK_SPEED_FIX=0 (a package's setting for "
                            "its patched key: wrong %s?): the speed fix is ON instead, else the game runs at fps / 60 x speed\n",
                    linked ? "the exe's linked code" : "main", linked ? "exe" : "image");
        }
        return;
    }
    if (!e) {
        setenv_("TOTK_SPEED_FIX", "0");
        fprintf(stderr, "[totk] main has the realtime frame step: TOTK_SPEED_FIX=0 (the engine's speed fix would rescale it)\n");
    } else if (strcmp(e, "0")) {
        fprintf(stderr, "[totk] warning: main has the realtime frame step and TOTK_SPEED_FIX=%s: the game runs at the "
                        "wrong speed above 30 fps (unset it or set 0)\n", e);
    }
}

/* The data page is main + 0x4668000 on 1.0.0 (16 KiB, after TOTK_MAIN_SPAN): engine commits main as roundup64K(mod0 + bss_end) = 0x4670000 bytes, so
   it lies in that mapping's slack. Checked rather than assumed, so another loader layout cannot make this a crash. */
static int data_page_mapped(uint64_t main_base) {
#ifdef _WIN32
    uint8_t *p = (uint8_t *)totk_data_page((void *)(uintptr_t)main_base, TOTK_MAIN_SPAN);
    MEMORY_BASIC_INFORMATION mi;
    for (uint8_t *q = p; q < p + 0x4000; q += 0x1000)
        if (!VirtualQuery(q, &mi, sizeof mi) || mi.State != MEM_COMMIT || !(mi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)))
            return 0;
#else
    (void)main_base;
#endif
    return 1;
}

/* RomFS overlay sets (games/totk/overlays) for the chosen settings, opt-in: TOTK_PATCH_SETS=<directory of the sets>
   (<work>/overlays). Ported from our runtime's runtime/src/game_totk.c. They reach the game through the engine's
   TOTK_MODS (hle_fs.cpp: first listed wins; read at the first file the game opens, after this boot), ahead of the mods
   already listed: output-<w>x<h>[-scene-<w>x<h>][-tex<percent>] a bigger output / scene / texture cache (HeapDef; the
   pool grows by totk_engine_extra_memory); when not built, the settings step down first
   grass-<p> / shadow-<p> / fog-<p>               TOTK_GRASS_DENSITY / TOTK_SHADOW_DISTANCE / TOTK_FOG_DISTANCE
   fasttravel                                     TOTK_FAST_TRAVEL=1: the shorter fast-travel departure
   TOTK_PATCH_EXTRA_SETS=<a>,<b>                  further sets by name (tests)
   The layout set (base, or aspect-<w>x<h> for a canvas that is not 16:9) edits the quick-menu archive, which the
   engine's TOTK_OVERLAY also serves and which beats the mods; TOTK_OVERLAY is read before this boot, so an aspect
   layout needs TOTK_OVERLAY pointed at `python -m games.totk.overlays.engine_dir --sets aspect-<w>x<h>`'s output
   (said in the log). base is the overlay's own fix and needs nothing. Pad glyphs are the (TOTK_GLYPHS).*/
static char g_sets_root[512];

static int set_dir(const char *name, char *dir, size_t cap) {
    struct stat st;
    snprintf(dir, cap, "%s/%s", g_sets_root, name);
    return stat(dir, &st) == 0 && (st.st_mode & S_IFDIR);
}

/* TOTK_ASPECT_ROW=0: no graphics.aspect_ratio row and no ultrawide canvas, while the other restart rows work (a package
   with the sets but without the menu cursor's ultrawide fit).*/
static int aspect_row_off(void) {
    const char *e = getenv("TOTK_ASPECT_ROW");
    return e && e[0] == '0';
}

static unsigned scene_area(const char *name, unsigned *w, unsigned *h) {
    const char *s = strstr(name, "-scene-");
    return s && sscanf(s, "-scene-%ux%u", w, h) == 2 ? *w * *h : 0;
}

/* The largest built scene set for output <mw>x<mh> below `area` (1600x900 when none), Windows only (elsewhere 1600x900). */
static void best_built_scene(unsigned mw, unsigned mh, unsigned area, unsigned *bw, unsigned *bh) {
    *bw = 1600, *bh = 900;
#ifdef _WIN32
    char pattern[600];
    WIN32_FIND_DATAA fd;
    unsigned w, h;
    snprintf(pattern, sizeof pattern, "%s\\output-%ux%u-scene-*", g_sets_root, mw, mh);
    HANDLE f = FindFirstFileA(pattern, &fd);
    if (f == INVALID_HANDLE_VALUE) return;
    do {
        unsigned a = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? scene_area(fd.cFileName, &w, &h) : 0;
        if (a && a < area && a > *bw * *bh) *bw = w, *bh = h;
    } while (FindNextFileA(f, &fd));
    FindClose(f);
#else
    (void)mw, (void)mh, (void)area;
#endif
}

static void fit_output_set(void) {
    char names[2][64], dir[600];
    for (int guard = 0; guard < 8; guard++) {
        if (totk_overlay_sets(&g_s, names) < 2 || set_dir(names[1], dir, sizeof dir)) return;
        if (g_s.tex_cache > 1.0f) {   /* first drop the texture cache growth: the game runs fine on the shipped one */
            fprintf(stderr, "[totk] error: overlay set %s not built: the texture cache stays at the game's size (build "
                            "it: python -m games.totk.overlays --romfs <work>/extracted/v100 --out %s --texture-cache %g)\n",
                    names[1], g_sets_root, (double)g_s.tex_cache);
            g_s.tex_cache = 1.0f;
            continue;
        }
        unsigned rw = g_s.render_w, rh = g_s.render_h, mw = g_s.max_w, mh = g_s.max_h, bw, bh;
        if ((uint64_t)rw * rh > 1600u * 900u) {
            best_built_scene(mw, mh, rw * rh, &bw, &bh);
            g_s.render_w = bw, g_s.render_h = bh;
            fprintf(stderr, "[totk] error: overlay set %s not built: the scene falls back from %ux%u to %ux%u (build it: "
                            "python -m games.totk.overlays --romfs <work>/extracted/v100 --out %s --outputs %ux%u "
                            "--scenes %ux%u)\n", names[1], rw, rh, bw, bh, g_sets_root, mw, mh, rw, rh);
        } else {
            g_s.max_w = g_s.output_w = 1920, g_s.max_h = g_s.output_h = 1080;
            fprintf(stderr, "[totk] error: overlay set %s not built: the output falls back from %ux%u to 1920x1080 (build "
                            "it: python -m games.totk.overlays --romfs <work>/extracted/v100 --out %s --outputs %ux%u)\n",
                    names[1], mw, mh, g_sets_root, mw, mh);
        }
    }
}

static void add_set(char *list, size_t cap, const char *name) {
    char dir[600];
    if (!set_dir(name, dir, sizeof dir)) {
        fprintf(stderr, "[totk] warning: overlay set %s missing (%s)\n", name, dir);
        return;
    }
    size_t n = strlen(list);
    const char sep[2] = {n ? PATH_SEP : 0, 0};
    snprintf(list + n, cap - n, "%s%s", sep, dir);
    fprintf(stderr, "[totk] RomFS overlay set %s (TOTK_MODS)\n", name);
}

/* <prefix>-<percent> from a multiplier (2) or a percentage (200) in `var`; none at 100 %. */
static void add_percent_set(char *list, size_t cap, const char *var, const char *prefix, long lo, long hi) {
    const char *g = getenv(var);
    if (!g || !*g) return;
    double d = strtod(g, NULL);
    long pct = (long)((d <= 10.0 ? d * 100.0 : d) + 0.5);
    char v[64];
    if (pct < lo || pct > hi)
        fprintf(stderr, "[totk] warning: %s=%s out of range (%g..%g or %ld..%ld)\n", var, g, lo / 100.0, hi / 100.0, lo, hi);
    else if (pct != 100) {
        snprintf(v, sizeof v, "%s-%ld", prefix, pct);
        add_set(list, cap, v);
    }
}

static int layout_overlay(const char *set, char *dir, size_t cap);   /* (with graphics.aspect_ratio, below) */
static void build_shape(const char *const *sets, int n);

/* graphics.hud_layout for a w x h canvas: center / corner as chosen; auto (the default, and unset) by shape: at the
   screen's edges, but in the middle 16:9 on 48:9 (three 16:9 screens side by side, support #7: the edges are the outer
   screens' far sides). A file saved before keeps "corner" (a save writes every row): pc_settings.cpp moves it to auto
   once (graphics.hud_rev), which on 21:9 / 32:9 is the same HUD*/
static int hud_centered(unsigned w, unsigned h) {
    const char *hud = getenv("TOTK_HUD_LAYOUT");
    if (hud && !strcmp(hud, "center")) return 1;
    if (hud && !strcmp(hud, "corner")) return 0;
    return (uint64_t)w * 10 >= (uint64_t)h * 48;   /* (48:9's band starts at 4.8) */
}

static void overlay_sets(void) {
    char names[2][64], list[4096] = "", v[160];
    int n = totk_overlay_sets(&g_s, names);
    if (n > 0 && strcmp(names[0], "base")) {   /* the canvas's final shape (fit_output_set may have stepped it back) */
        char dir[600], center[80];
        /* graphics.hud_layout = center (TOTK_HUD_LAYOUT): the set whose HUD stays in the middle 16:9, aspect-<w>x<h>-center
           (built for the ultrawide canvases); an overlay without it keeps the HUD at the edges. A canvas no wider than 16:9
           has no such set: asking would exit the game for a build aspect_sets.py refuses*/
        int centered = 0;
        snprintf(center, sizeof center, "%s-center", names[0]);
        if (hud_centered(g_s.output_w, g_s.output_h) && (uint64_t)g_s.output_w * 9 > (uint64_t)g_s.output_h * 16) {
            if (!layout_overlay(center, dir, sizeof dir)) {   /* (not built here: built now, if this install can) */
                const char *want[1] = {center};
                build_shape(want, 1);
            }
            if ((centered = layout_overlay(center, dir, sizeof dir))) {
                setenv_("TOTK_OVERLAY", dir);
                fprintf(stderr, "[totk] layout set %s (HUD in the middle 16:9): TOTK_OVERLAY=%s\n", center, dir);
            } else
                fprintf(stderr, "[totk] HUD position: the overlay has no %s layout: the HUD at the screen's edges\n", center);
        }
        if (!centered) {
            if (layout_overlay(names[0], dir, sizeof dir)) {
                setenv_("TOTK_OVERLAY", dir);
                fprintf(stderr, "[totk] layout set %s: TOTK_OVERLAY=%s\n", names[0], dir);
            } else
                fprintf(stderr, "[totk] error: layout set %s needs the overlay with its layouts (TOTK_OVERLAY-%s, "
                                "tools/engine/build.py overlay; or TOTK_OVERLAY=<python -m games.totk.overlays.engine_dir "
                                "--sets %s>): the HUD keeps the 16:9 layout\n", names[0], names[0], names[0]);
        }
    }
    for (int i = 1; i < n; i++) add_set(list, sizeof list, names[i]);
    add_percent_set(list, sizeof list, "TOTK_GRASS_DENSITY", "grass", 25, 400);
    {   /* (a canvas that is not 16:9: the shadow distance at x2 at most, as the renderer's shadow maps: textures.c
           shadow_wide_cap. TOTK_WIDE_SHADOW_CAP=0: no cap)*/
        const char *o = getenv("TOTK_OUTPUT_RES"), *sd = getenv("TOTK_SHADOW_DISTANCE"), *c = getenv("TOTK_WIDE_SHADOW_CAP");
        unsigned ow = 0, oh = 0;
        if (o && sscanf(o, "%ux%u", &ow, &oh) == 2 && oh && sd && *sd && !(c && *c == '0')) {
            const double a = (double)ow / oh, d = strtod(sd, NULL), m = d <= 10.0 ? d : d / 100.0;
            if ((a < 1.74 || a > 1.81) && m > 2.0) {
                fprintf(stderr, "[totk] shadow distance capped at x2 (asked x%g): the canvas %ux%u is not 16:9 "
                                "(TOTK_WIDE_SHADOW_CAP=0: no cap)\n", m, ow, oh);
                setenv_("TOTK_SHADOW_DISTANCE", "2");
            }
        }
    }
    add_percent_set(list, sizeof list, "TOTK_SHADOW_DISTANCE", "shadow", 25, 400);
    add_percent_set(list, sizeof list, "TOTK_FOG_DISTANCE", "fog", 50, 400);
    const char *fast = getenv("TOTK_FAST_TRAVEL");
    if (fast && atoi(fast) > 0) add_set(list, sizeof list, "fasttravel");
    const char *more = getenv("TOTK_PATCH_EXTRA_SETS");
    for (const char *s = more; s && *s;) {
        const char *e = strchr(s, ',');
        size_t len = e ? (size_t)(e - s) : strlen(s);
        if (len && len < sizeof v) { memcpy(v, s, len); v[len] = 0; add_set(list, sizeof list, v); }
        s = e ? e + 1 : s + len;
    }
    if (!*list) return;
    const char *mods = getenv("TOTK_MODS");
    if (mods && *mods) {
        size_t k = strlen(list);
        snprintf(list + k, sizeof list - k, "%c%s", PATH_SEP, mods);
    }
    setenv_("TOTK_MODS", list);
}

/* Settings rows in the engine's F10 overlay / settings.ini (engine/runtime/pc_settings.h, integration-2), added at boot
   and only when main carries the patches (the realtime step stands for the set): the live rows of totk_setting_rows()
   write their data page slot on the present thread (as our app's plat_setting_mirror did); the restart rows are the
   RomFS-set multipliers and the fast-travel switch, which reach the next start through their TOTK_* variable (they need TOTK_PATCH_SETS). A
   TOTK_* variable given for this run wins over the saved value of its live row. Not in a build without the engine's
   settings registry (TOTK_NO_PC_SETTINGS: the CPU harness). */
volatile int g_cutscene_16x9;   /* graphics.cutscene_aspect = 16:9 (below; the Vulkan present's totk_hooks.c reads it) */

#ifndef TOTK_NO_PC_SETTINGS
#include "../../../engine/runtime/pc_settings.h"
#include "../../../engine/runtime/pc_overlay.h"

static const struct { const char *key, *env; } k_row_env[] = {
    {"graphics.min_resolution", "TOTK_DYNAMIC_RES"}, {"graphics.fov", "TOTK_FOV"},
    {"graphics.shadow_resolution", "TOTK_SHADOW_RES"}, {"graphics.shadow_fixed", "TOTK_SHADOW_FIXED"},
    {"graphics.shadow_reach", "TOTK_SHADOW_REACH"},
    {"graphics.lod_distance", "TOTK_LOD"},
    {"graphics.actor_detail_distance", "TOTK_ACTOR_LOD"}, {"graphics.terrain_detail_distance", "TOTK_TERRAIN_LOD"},
    {"game.simulation_distance", "TOTK_SIM_DISTANCE"},
};
#define MAX_LIVE_ROWS 12
static pc_setting g_live[MAX_LIVE_ROWS];
static char g_live_def[MAX_LIVE_ROWS][24];
static unsigned g_live_slot[MAX_LIVE_ROWS];
static float g_live_min[MAX_LIVE_ROWS], g_live_max[MAX_LIVE_ROWS];

/* The multiplier slots whose veneers take 0 for the game's own value (the TOTK_* parsers and totk_*_set store 1 as 0):
   1.0 would run the patched path, e.g. the fov veneer's 2.75 rad cap, which the shipping code does not have. */
static int one_is_shipping(unsigned slot) {
    return slot == TOTK_DP_FOV_SCALE || slot == TOTK_DP_SHADOW_SCALE || slot == TOTK_DP_LOD_SCALE ||
           slot == TOTK_DP_ACTOR_LOD || slot == TOTK_DP_TERRAIN_LOD;
}

static void write_slot(int i, const char *value) {
    uint8_t *page = (uint8_t *)totk_data_page_current();
    if (!page || !value) return;
    float v = strtof(value, NULL);
    if (!(v >= g_live_min[i] && v <= g_live_max[i])) return;
    if (v == 1.0f && one_is_shipping(g_live_slot[i])) v = 0.0f;
    if (g_live_slot[i] == TOTK_DP_SHADOW_SPLIT) v = totk_shadow_reach_slot(v);   /* (with Shadow distance: #69) */
    uint32_t bits;
    memcpy(&bits, &v, 4);
    __atomic_store_n((uint32_t *)(page + g_live_slot[i]), bits, __ATOMIC_RELEASE);
}
/* The field of view slider: the multiplier, and the view in degrees it gives the camera right now (the fov veneer records
   the camera's own fovy at TOTK_DP_FOV_BASE; aiming and some cutscenes narrow or widen it). Unpatched, or before the first
   camera update: the multiplier alone. The Options row (type 67) shows the first number before a degree sign as "(N°)":
   keep "<number>° wide" first.*/
static const char *fov_format(float k) {
    static char buf[80];
    const uint8_t *page = (const uint8_t *)totk_data_page_current();
    if (!page) return "%.2fx";
    uint32_t bits[2] = {__atomic_load_n((const uint32_t *)(page + TOTK_DP_FOV_BASE), __ATOMIC_ACQUIRE),
                        __atomic_load_n((const uint32_t *)(page + TOTK_DP_ASPECT), __ATOMIC_ACQUIRE)};
    float base, aspect;
    memcpy(&base, &bits[0], 4), memcpy(&aspect, &bits[1], 4);
    if (!(base > 0.05f && base < 3.0f)) return "%.2fx";
    if (!(aspect > 0.2f && aspect < 8.0f)) aspect = 16.0f / 9.0f;   /* 0: the game's own 16:9 */
    double v = k > 0 && k != 1 ? base * k : base;
    if (v > 2.75) v = 2.75;   /* the veneer's ceiling (sites_v100.FOV_MAX) */
    const double h = 2.0 * atan(tan(v / 2.0) * aspect), deg = 57.29577951308232;
    snprintf(buf, sizeof buf, "%%.2fx  (%.0f\xc2\xb0 wide, %.0f\xc2\xb0 tall)", h * deg, v * deg);
    return buf;
}

/* The sun shadow map size row (0 / 1): its two choices by name (a format with no conversion) */
// (F10 shows a format through pc_tr: its Spanish is in pc_text_es.inc, PC_T marks it for check_texts)
static const char *shadow_fixed_format(float k) { return k >= 0.5f ? PC_T("Fixed (steady)") : PC_T("The game's (changes)"); }

#define APPLY(i) static void apply_##i(const char *v) { write_slot(i, v); }
APPLY(0) APPLY(1) APPLY(2) APPLY(3) APPLY(4) APPLY(5) APPLY(6) APPLY(7) APPLY(8) APPLY(9) APPLY(10) APPLY(11)
static void (*const k_apply[MAX_LIVE_ROWS])(const char *) = {apply_0, apply_1, apply_2, apply_3, apply_4, apply_5,
                                                             apply_6, apply_7, apply_8, apply_9, apply_10, apply_11};

static const char *const k_fast_values[] = {"0", "1", NULL}, *const k_fast_names[] = {"Normal", "Fast", NULL};
/* (the default is "window" since v0.1.9: at "16:9" an ultrawide window was pillarboxed; a 16:9 screen gets the same
   picture either way. pc_settings.cpp moves a file's old default 16:9 once, graphics.aspect_rev)*/
/* (3:2 last: totk_options_menu.cpp kWide and the in-game menu's choices list the values in this order) */
static const char *const k_wide_values[] = {"16:9", "window", "16:10", "21:9", "32:9", "48:9", "3:2", NULL},
                         *const k_wide_names[] = {"16:9 (the game's)", "The window's (3:2, 16:10, 21:9, 32:9, 48:9)", "16:10",
                                                  "21:9", "32:9", "48:9", "3:2", NULL};
static const char *const k_hud_values[] = {"auto", "corner", "center", NULL},
                         *const k_hud_names[] = {"Automatic (in the middle on 48:9)", "At the screen's edges", "In the middle 16:9", NULL};
static const pc_setting k_restart_rows[] = {
    {"graphics.aspect_ratio", "Graphics", "Game patches", "Aspect ratio",
     "The window's: on a 3:2, 16:10, 21:9, 32:9 or 48:9 window or monitor the game draws a canvas of that shape at start "
     "(1920x1280, 1920x1200, 3440x1440, 5120x1440 or 7680x1440), the HUD laid out for it: 21:9, 32:9 and 48:9 a wider view, 3:2 "
     "and 16:10 a taller one (the same height of view, a little less at the sides). Other shapes, and 16:9, keep the game's "
     "16:9 picture. 16:10 / 21:9 / 32:9 / 48:9 / 3:2: that canvas whatever the window (on another shape it is shown whole, with bars; 21:9, 32:9 and 48:9 are "
     "heavier than the window's own shape: the whole canvas is drawn, then shown smaller). Needs the game patches' "
     "RomFS sets (TOTK_PATCH_SETS) and the overlay's layout for that shape. At the next start: the canvas and the game "
     "files laid out for it are chosen as the game starts. Setup prepares the layouts of this PC's monitors' shapes; "
     "another shape's are made at the start after you pick it (about 30 s, once).", "window", k_wide_values,
     k_wide_names, 0, 0, 0, PC_SET_RESTART, "TOTK_WIDE", NULL, NULL, NULL},
    {"graphics.hud_layout", "Graphics", "Game patches", "HUD position (21:9, 32:9, 48:9)",
     "Where the HUD goes on a 21:9, 32:9 or 48:9 canvas: at the screen's edges, or in the middle 16:9 (closer to the eyes on "
     "a very wide screen). Menus are the same either way. Nothing changes at 16:9. At the next start: the game loads "
     "its interface layouts once, as it starts. Automatic (the default): in the middle on 48:9, at the edges otherwise.", "auto", k_hud_values,
     k_hud_names, 0, 0, 0, PC_SET_RESTART, "TOTK_HUD_LAYOUT", NULL, NULL, NULL},
    // (the reasons below: every file open of a run: each set's file is opened once a run, HeapDef, EnvPalette and the
    // shader package at boot, Grass.pack before the title, the travel scene at the first fast travel; hle_fs's sets
    // are fixed at the first open. live-settings)
    {"graphics.texture_cache", "Graphics", "Game patches", "Texture cache",
     "Multiplier of the game's texture cache (fewer blurry textures when turning back; 490 MiB more per step). Needs the "
     "game patches' RomFS sets (TOTK_PATCH_SETS). At the next start: the game sizes its memory once, as it starts.",
     "1", NULL, NULL, 1.0f, 3.0f, 0.5f, PC_SET_RESTART, "TOTK_TEXTURE_CACHE", NULL, NULL, NULL},
    {"graphics.grass_density", "Graphics", "Game patches", "Grass density",
     "Multiplier of grass, flowers and pebbles. Needs the game patches' RomFS sets (TOTK_PATCH_SETS)."
     " At the next start: the game reads its grass table once, as it starts.", "1", NULL, NULL,
     0.25f, 4.0f, 0.25f, PC_SET_RESTART, "TOTK_GRASS_DENSITY", NULL, NULL, NULL},
    {"graphics.shadow_distance", "Graphics", "Game patches", "Shadow distance",
     "Multiplier of the sharp shadow cascades' distances. Needs the game patches' RomFS sets (TOTK_PATCH_SETS)."
     " At the next start: the value is in the shader package, which the game reads once, as it starts.", "1",
     NULL, NULL, 0.25f, 4.0f, 0.25f, PC_SET_RESTART, "TOTK_SHADOW_DISTANCE", NULL, NULL, NULL},
    {"graphics.fog_distance", "Graphics", "Game patches", "Fog distance",
     "Multiplier of the distance fog's start and end (clearer distant mountains). Needs the game patches' RomFS sets "
     "(TOTK_PATCH_SETS). At the next start: the game reads its sky and fog palettes once, as it starts.", "1", NULL,
     NULL, 0.5f, 4.0f, 0.5f, PC_SET_RESTART, "TOTK_FOG_DISTANCE", NULL, NULL, NULL},
    {"game.fast_travel", "Game", "Game patches", "Fast-travel speed",
     "Fast: Link's departure when travelling is about 2.5 s shorter (its long waits halved, his dissolve as on the "
     "Switch); Normal is the game's own. Needs the game patches' RomFS sets (TOTK_PATCH_SETS). At the next start: the "
     "game loads the travel scene the first time you travel and keeps it.", "0", k_fast_values, k_fast_names, 0, 0,
     0, PC_SET_RESTART, "TOTK_FAST_TRAVEL", NULL, NULL, NULL},
};

/* graphics.cutscene_aspect (live): on a 21:9 / 32:9 / 48:9 canvas, real-time cutscenes (and conversations: the game's events)
   wide like the game (default) or in the middle 16:9 with bars, the original framing (totk_hooks.c crops the present;
   pre-rendered movies are 16:9 video and always shown that way) */
static const char *const k_cut_values[] = {"wide", "16:9", NULL},
                         *const k_cut_names[] = {"Wide (more at the sides)", "16:9 (the original framing)", NULL};
static void cutscene_apply(const char *v) { g_cutscene_16x9 = v && !strcmp(v, "16:9"); }
static const pc_setting k_cutscene_row[] = {
    {"graphics.cutscene_aspect", "Graphics", "Game patches", "Cutscenes (21:9, 32:9, 48:9)",
     "On a 21:9, 32:9 or 48:9 canvas, real-time cutscenes and conversations: wide like the rest of the game, or the original "
     "16:9 framing with bars at the sides (for a scene that looks odd at the edges). Pre-rendered movies are 16:9 video: "
     "always shown in the middle with bars.", "wide", k_cut_values, k_cut_names, 0, 0, 0, 0, NULL, NULL, cutscene_apply,
     NULL, NULL},
};

/* Before the settings are read: the restart rows put their saved values into the environment here. Every restart row
   works through the RomFS sets (TOTK_PATCH_SETS): without them (the package, which ships none) they would do nothing,
   so they are not offered (the gate: "Aspect ratio" was visible and inert).*/
static int g_restart_rows;
static void add_restart_rows(void) {
#ifdef TOTK_OTHER_VERSION
    fprintf(stderr, "[totk] game-patch restart rows hidden: their RomFS sets are made from 1.0.0's files\n");
    return;
#endif
    const char *sets = getenv("TOTK_PATCH_SETS");
    if (!sets || !*sets) {
        fprintf(stderr, "[totk] game-patch restart rows hidden: TOTK_PATCH_SETS not set (aspect ratio, texture cache, "
                        "grass / shadow / fog distance, fast travel need its sets)\n");
        return;
    }
    g_restart_rows = (int)(sizeof k_restart_rows / sizeof *k_restart_rows);
    if (aspect_row_off()) {   /* k_restart_rows[0..1] are the aspect and HUD rows (checked: a reorder must not hide another) */
        if (!strcmp(k_restart_rows[0].key, "graphics.aspect_ratio") && !strcmp(k_restart_rows[1].key, "graphics.hud_layout")) {
            fprintf(stderr, "[totk] graphics.aspect_ratio / hud_layout rows off (TOTK_ASPECT_ROW=0): the canvas stays 16:9\n");
            g_restart_rows -= 2;
            pc_settings_add(k_restart_rows + 2, g_restart_rows);
            return;
        }
        fprintf(stderr, "[totk] error: TOTK_ASPECT_ROW=0 but the first restart rows are %s / %s, not graphics.aspect_ratio "
                        "/ hud_layout: every row kept (the canvas still stays 16:9)\n", k_restart_rows[0].key,
                k_restart_rows[1].key);
    }
    pc_settings_add(k_restart_rows, g_restart_rows);
    /* pc_settings puts a row into the environment only when the settings file or NX_SET_ names it, and wide_canvas reads
       TOTK_WIDE: a first start (no file yet) or a file without the line kept the game's 16:9 with no line said, whatever
       the window's shape (gate v0.1.9.1, the 3:2 boot). The row's own value then, its default "window" (with the overlay
       off the file is never read: left as it was) */
    const char *wide = getenv("TOTK_WIDE"), *row = pc_settings_get("graphics.aspect_ratio");
    if ((!wide || !*wide) && row && *row && !aspect_row_off() && pc_overlay_enabled()) setenv_("TOTK_WIDE", row);
    if (!aspect_row_off()) {   /* (live: the saved value at once; present reads it every frame) */
        pc_settings_add(k_cutscene_row, 1);
        cutscene_apply(pc_settings_get(k_cutscene_row[0].key));
    }
}

/* After totk_patches_init: the live rows, and their saved values into the slots unless the run's environment set one. */
static void add_live_rows(void) {
    unsigned n;
    const TotkSettingRow *rows = totk_setting_rows(&n);
    int k = 0;
    for (unsigned i = 0; i < n && k < MAX_LIVE_ROWS; i++) {
        const TotkSettingRow *r = &rows[i];
#ifdef TOTK_OTHER_VERSION
        if (strcmp(r->key, "graphics.min_resolution") && strcmp(r->key, "graphics.fov") &&
            strcmp(r->key, "graphics.shadow_resolution") && strcmp(r->key, "graphics.shadow_fixed"))
            continue;   /* (no site: sites_v143.py has dynres, fov and shadowres) */
#endif
        /* (the engine's own controls row, hle_hid.cpp, has this key and feeds the slot too, pc_mouse.cpp reload: a second
           row meant one meaning or the other by registration order)*/
        if (!strcmp(r->key, "controls.mouse_camera_scale")) continue;
        snprintf(g_live_def[k], sizeof g_live_def[k], "%g", r->def);
        g_live_slot[k] = r->slot, g_live_min[k] = (float)r->min, g_live_max[k] = (float)r->max;
        g_live[k] = (pc_setting){r->key, r->group, "Game patches", r->label, r->help, g_live_def[k], NULL, NULL,
                                 (float)r->min, (float)r->max, (float)r->step, 0, NULL, NULL, k_apply[k], NULL};
        if (!strcmp(r->key, "graphics.fov")) g_live[k].format = fov_format;
        if (!strcmp(r->key, "graphics.shadow_fixed")) g_live[k].format = shadow_fixed_format;
        k++;
    }
    pc_settings_add(g_live, k);
    for (int i = 0; i < k; i++) {
        const char *env = NULL;
        for (size_t j = 0; j < sizeof k_row_env / sizeof *k_row_env; j++)
            if (!strcmp(k_row_env[j].key, g_live[i].key)) env = getenv(k_row_env[j].env);
        if (env && *env) continue;
        write_slot(i, pc_settings_get(g_live[i].key));
    }
    fprintf(stderr, "[totk] %d live and %d restart game-patch settings in the overlay\n", k, g_restart_rows);
}
#else
static void add_restart_rows(void) {}
static void add_live_rows(void) {}
#endif

/* graphics.aspect_ratio = window (TOTK_WIDE=window): the canvas takes the window's shape at start, for the shapes whose
   sets are built (21:9 -> 3440x1440, 32:9 -> 5120x1440, 48:9 -> 7680x1440; the scene at the canvas size: the host render scale only scales
   16:9 targets). = 21:9 / 32:9 / 48:9: that canvas whatever the window's shape (present fits it whole, with bars). The window
   is not made yet (device.c ui_window_init runs later), so its size is read the way it will be made: fullscreen as shim_win.c settings_load and ui_window_init decide it (TOTK_WINDOW_MODE, else TOTK_FULLSCREEN,
   else borderless with the overlay on; a LARECOMP_WINDOW stays a window unless the mode is named) -> TOTK_EXCLUSIVE_SIZE
   or the monitor (TOTK_MONITOR's, else the primary); windowed -> LARECOMP_WINDOW (else ui_window_init's 1280x720). A
   TOTK_OUTPUT_RES given for the run wins. Taken only when the layout overlay for that shape is there (layout_overlay),
   else a 21:9 canvas would get the 16:9 HUD stretched over it. */

#ifdef _WIN32
typedef struct { int want, n; HMONITOR found; } MonPick;
static BOOL CALLBACK mon_enum(HMONITOR m, HDC dc, LPRECT r, LPARAM p) {
    (void)dc, (void)r;
    MonPick *k = (MonPick *)p;
    if (++k->n == k->want) { k->found = m; return FALSE; }
    return TRUE;
}
#endif

static int window_shape(unsigned *w, unsigned *h, const char **how) {
    const char *m = getenv("TOTK_WINDOW_MODE"), *f = getenv("TOTK_FULLSCREEN"), *lw = getenv("LARECOMP_WINDOW");
    int named = m && (!strcmp(m, "borderless") || !strcmp(m, "exclusive"));
#ifndef TOTK_NO_PC_SETTINGS
    int fs = m && *m ? strcmp(m, "windowed") != 0 : f && *f ? strcmp(f, "0") != 0 : pc_overlay_enabled();
#else
    int fs = m && *m ? strcmp(m, "windowed") != 0 : f && *f && strcmp(f, "0") != 0;
#endif
    fs = fs && (named || !lw) && !getenv("LARECOMP_HIDDEN");
    if (!fs) {
        *how = "window";
        *w = 1280, *h = 720;
        return !lw || sscanf(lw, "%ux%u", w, h) == 2;
    }
    const char *es = getenv("TOTK_EXCLUSIVE_SIZE");
    if (m && !strcmp(m, "exclusive") && es && sscanf(es, "%ux%u", w, h) == 2) { *how = "exclusive size"; return 1; }
#ifdef _WIN32
    /* (before SDL_Init makes the process DPI aware the size may be scaled; the shape is the same) */
    const char *mon = getenv("TOTK_MONITOR");
    MonPick k = {mon ? atoi(mon) : 0, 0, NULL};
    if (k.want > 0) EnumDisplayMonitors(NULL, NULL, mon_enum, (LPARAM)&k);
    HMONITOR hm = k.found ? k.found : MonitorFromPoint((POINT){0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi = {sizeof mi};
    if (!GetMonitorInfoW(hm, &mi)) return 0;
    *w = (unsigned)(mi.rcMonitor.right - mi.rcMonitor.left), *h = (unsigned)(mi.rcMonitor.bottom - mi.rcMonitor.top);
    *how = "monitor";
    return *w && *h;
#else
    return 0;   /* fullscreen elsewhere: the monitor is not known here */
#endif
}

/* The engine's overlay with the UI layouts of a set (aspect-<w>x<h>): <TOTK_OVERLAY>-<set> next to it (tools/engine/build.py
   overlay builds romfs-aspect-<w>x<h> beside romfs), unless TOTK_OVERLAY already names that set. hle_fs reads TOTK_OVERLAY
   at the game's first RomFS open, after this boot. */
static int layout_overlay(const char *set, char *dir, size_t cap) {
    const char *ov = getenv("TOTK_OVERLAY");
    if (!ov || !*ov) return 0;
    if (strstr(ov, set)) { snprintf(dir, cap, "%s", ov); return 1; }
    struct stat st;
    snprintf(dir, cap, "%s-%s", ov, set);
    return stat(dir, &st) == 0 && (st.st_mode & S_IFDIR);
}

/* A screen shape's layouts not built here (Setup builds its monitors' shapes and those built before: tools/engine/
   aspect_sets.py): TOTK_ASPECT_BUILD (player.txt of a Builder install) builds them now, ~30 s, and starts the game again;
   this run exits, before its window opens. Not asked again after a failed build (<TOTK_OVERLAY>-<set>.failed, which the
   next Setup clears): that shape keeps the game's 16:9. sets: "aspect-<w>x<h>[-center]", n of them. Returns when not
   built here. */
/* The markers of a shape build (1 = written "building", 0 = removed): written before the tool starts, so a build that
   never ends (the tool does not start, its console is closed, the PC sleeps) is not asked for again at every start,
   which would end every start before its window; the tool removes them when it has built the sets. Returns 0 when a
   mark could not be written (then none is left and no build starts: the shape keeps 16:9).*/
static int shape_marks(const char *ov, const char *const *sets, int n, int on) {
    char mark[700];
    for (int i = 0; i < n; i++) {
        snprintf(mark, sizeof mark, "%s-%s.failed", ov, sets[i]);
        if (!on) { remove(mark); continue; }
        FILE *f = fopen(mark, "w");
        if (!f || fputs("building (the build did not finish: see <work>/overlays/aspect-build.log)\n", f) < 0) {
            if (f) fclose(f);
            fprintf(stderr, "[totk] aspect ratio: %s cannot be written: the layouts are not built (the game's 16:9)\n", mark);
            shape_marks(ov, sets, i + 1, 0);
            return 0;
        }
        fclose(f);
    }
    return 1;
}

/* Why a shape's layouts were not built at this start, said on screen once the picture is up (totk_window_shape_hint):
   1 = the build cannot leave the job the game runs in, which would end it with the game; 2 = an earlier build failed or
   was stopped (its mark is there) */
static int g_shape_note;

#ifdef _WIN32
/* The game runs in a job that ends every process in it when the job closes (a launcher that wraps the games it starts:
   Steam, Playnite, a test harness): a build started from here dies with the game, leaving its mark (gate v0.1.9.1).
   Only the innermost job is read: an outer kill-on-close job around a plain inner one (nested jobs) is missed, and the
   build then starts in the jobs and dies with the game as before*/
static int job_ends_children(void) {
    BOOL in = FALSE;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION li;
    if (!IsProcessInJob(GetCurrentProcess(), NULL, &in) || !in) return 0;
    if (!QueryInformationJobObject(NULL, JobObjectExtendedLimitInformation, &li, sizeof li, NULL)) return 0;
    return (li.BasicLimitInformation.LimitFlags & JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE) != 0;
}
#endif

static void build_shape(const char *const *sets, int n) {
    const char *cmd = getenv("TOTK_ASPECT_BUILD"), *ov = getenv("TOTK_OVERLAY");
    if (!cmd || !*cmd || !ov || !*ov) return;
    struct stat st;
    char mark[700];
    for (int i = 0; i < n; i++) {
        snprintf(mark, sizeof mark, "%s-%s.failed", ov, sets[i]);
        if (stat(mark, &st) == 0) {
            char why[200] = "";
            FILE *f = fopen(mark, "r");
            if (f) {
                if (!fgets(why, sizeof why, f)) why[0] = 0;
                fclose(f);
            }
            why[strcspn(why, "\r\n")] = 0;
            fprintf(stderr, "[totk] aspect ratio: building %s did not succeed before (%s: %s; Setup tries again): not "
                            "asked again\n", sets[i], mark, !strncmp(why, "building", 8) ? "it was stopped before it ended"
                                                                                      : why);
            g_shape_note = 2;
            return;
        }
    }
#ifdef _WIN32
    static wchar_t line[16384];
    wchar_t exe[4096];
    const wchar_t *wcmd = _wgetenv(L"TOTK_ASPECT_BUILD");
    DWORD len = wcmd ? GetModuleFileNameW(NULL, exe, 4096) : 0;
    wchar_t *slash = len && len < 4096 ? wcsrchr(exe, L'\\') : NULL;
    if (!slash) return;
    *slash = 0;
    int at = swprintf(line, 16384, L"%ls", wcmd);
    for (int i = 0; i < n && at > 0; i++) at += swprintf(line + at, 16384 - at, L" --set %hs", sets[i] + 7);   /* (past "aspect-") */
    if (at <= 0 || swprintf(line + at, 16384 - at, L" --exe-dir \"%ls\" --pid %lu", exe, GetCurrentProcessId()) <= 0) return;
    STARTUPINFOW si = {sizeof si};
    PROCESS_INFORMATION pi = {0};
    if (!shape_marks(ov, sets, n, 1)) return;
    /* out of the game's job (CREATE_BREAKAWAY_FROM_JOB; no effect outside one). A job that does not let it leave refuses
       the start (ERROR_ACCESS_DENIED): then in the job, unless the job would end it with the game: no build then, this
       run goes on at 16:9 and says how to get the layouts (LEAD 15:22) */
    const DWORD flags = CREATE_NEW_CONSOLE | CREATE_NEW_PROCESS_GROUP;
    BOOL ok = CreateProcessW(NULL, line, NULL, NULL, FALSE, flags | CREATE_BREAKAWAY_FROM_JOB, NULL, NULL, &si, &pi);
    if (!ok && GetLastError() == ERROR_ACCESS_DENIED) {
        if (job_ends_children()) {
            fprintf(stderr, "[totk] aspect ratio: %s not built here, and the game runs in a job that ends what it starts "
                            "when it closes (another launcher started it): the build would end with it. Start the game "
                            "once from TotK Launcher.exe to build it (the game's 16:9 now)\n", sets[0]);
            shape_marks(ov, sets, n, 0);
            g_shape_note = 1;
            return;
        }
        ok = CreateProcessW(NULL, line, NULL, NULL, FALSE, flags, NULL, NULL, &si, &pi);
    }
    if (!ok) {
        fprintf(stderr, "[totk] aspect ratio: the layouts' build did not start (error %lu): the game's 16:9\n", GetLastError());
        shape_marks(ov, sets, n, 0);   /* (this run goes on: the next start may try again) */
        return;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
#else
    char line[8192], exe[4096];
    ssize_t len = readlink("/proc/self/exe", exe, sizeof exe - 1);
    char *slash = len > 0 ? (exe[len] = 0, strrchr(exe, '/')) : NULL;
    if (!slash) return;
    *slash = 0;
    int at = snprintf(line, sizeof line, "%s", cmd);
    for (int i = 0; i < n; i++) at += snprintf(line + at, sizeof line - at, " --set %s", sets[i] + 7);
    snprintf(line + at, sizeof line - at, " --exe-dir \"%s\" --pid %d &", exe, (int)getpid());
    if (!shape_marks(ov, sets, n, 1)) return;
    if (system(line) != 0) {
        shape_marks(ov, sets, n, 0);
        return;
    }
#endif
    fprintf(stderr, "[totk] aspect ratio: %s%s%s not built here: building it now (TOTK_ASPECT_BUILD); the game starts again "
                    "by itself\n", sets[0], n > 1 ? " and " : "", n > 1 ? sets[1] : "");
    fflush(stderr);
    fflush(stdout);
    _exit(0);
}

/* each shape's canvas and scene: 21:9 / 32:9 / 48:9 drawn whole at the canvas's size; 16:10 (laptops, handhelds) the game's
   own 1920x1080 output and 1600x900 scene made taller (overlays builds the output-1920x1200[-scene-1600x1000] heap sets;
   its UI layouts are the 2560x1600 set's, totk_layout_set); 3:2 (Surface, Framework: 2160x1440, 2256x1504, 2880x1920,
   3000x2000) the same, taller still: 1920x1280, its scene 1620x1080 (1600 x 1066.7 is no whole size; the scene keeps the
   canvas's exact 3:2), the 2160x1440 set's layouts. lo.. hi: the window shapes that take it*/
static const struct { const char *name, *canvas, *scene; double lo, hi; } k_shapes[] = {
    {"3:2", "1920x1280", "1620x1080", 1.45, 1.55},
    {"16:10", "1920x1200", "1600x1000", 1.55, 1.65},
    {"21:9", "3440x1440", "3440x1440", 2.2, 2.6},
    {"32:9", "5120x1440", "5120x1440", 3.3, 3.8},
    {"48:9", "7680x1440", "7680x1440", 4.8, 5.9},   /* three 16:9 screens (support #7), three 16:10 ones too */
};
static int shape_of(double a) {   /* k_shapes' index, -1 for any other shape (16:9 among them) */
    for (int i = 0; i < (int)(sizeof k_shapes / sizeof k_shapes[0]); i++)
        if (a >= k_shapes[i].lo && a < k_shapes[i].hi) return i;
    return -1;
}

static void wide_canvas(void) {
#ifdef TOTK_OTHER_VERSION
    return;   /* (no output_res, no layout overlay) */
#endif
    const char *wide = getenv("TOTK_WIDE"), *out = getenv("TOTK_OUTPUT_RES");
    int shape = -1;
    for (int i = 0; wide && i < (int)(sizeof k_shapes / sizeof k_shapes[0]); i++)
        if (!strcmp(wide, k_shapes[i].name)) shape = i;
    if (!wide || (strcmp(wide, "window") && shape < 0) || aspect_row_off()) return;
    if (out && *out) {
        fprintf(stderr, "[totk] aspect ratio: TOTK_OUTPUT_RES=%s given for this run: it wins over the window's shape\n", out);
        return;
    }
    const char *sets = getenv("TOTK_PATCH_SETS");
    if (!sets || !*sets) {   /* a bigger canvas on the shipped heaps crashes: its output set comes from there*/
        fprintf(stderr, "[totk] error: aspect ratio: TOTK_PATCH_SETS is not set (the canvas's heap set): the game's 16:9\n");
        return;
    }
    unsigned w = 0, h = 0;
    const char *how = "setting";
    if (shape < 0) {
        if (!window_shape(&w, &h, &how) || !h) {
            fprintf(stderr, "[totk] aspect ratio: the window's size is not known here: the game's 16:9\n");
            return;
        }
        double a = (double)w / h;
        shape = shape_of(a);
        if (shape < 0) {
            fprintf(stderr, "[totk] aspect ratio: the %s is %ux%u (%.3f), no 3:2, 16:10 or ultrawide shape: the game's 16:9\n",
                    how, w, h, a);
            return;
        }
    } else {
        sscanf(k_shapes[shape].canvas, "%ux%u", &w, &h);
    }
    const char *canvas = k_shapes[shape].canvas;
    double a = (double)w / h;
    char set[64], dir[600];
    unsigned cw = 0, ch = 0;
    sscanf(canvas, "%ux%u", &cw, &ch);
    totk_layout_set(cw, ch, set);
    if (!layout_overlay(set, dir, sizeof dir)) {
        /* (with the HUD-in-the-middle set too when that is chosen and missing: one build, one restart) */
        char center[80];
        snprintf(center, sizeof center, "%s-center", set);
        const char *want[2] = {set, center};
        build_shape(want, hud_centered(cw, ch) && cw * 9 > ch * 16 && !layout_overlay(center, dir, sizeof dir) ? 2 : 1);
        fprintf(stderr, "[totk] error: aspect ratio: the %s is %ux%u (%.3f) but the overlay has no %s layout (%s-%s; "
                        "tools/engine/build.py overlay builds it): the game's 16:9\n", how, w, h, a, set,
                getenv("TOTK_OVERLAY") ? getenv("TOTK_OVERLAY") : "TOTK_OVERLAY unset", set);
        return;
    }
    setenv_("TOTK_OUTPUT_RES", canvas);
    const char *scene = getenv("TOTK_RENDER_RES");
    if (!scene || !*scene) setenv_("TOTK_RENDER_RES", k_shapes[shape].scene);
    fprintf(stderr, "[totk] aspect ratio: the %s is %ux%u (%.3f): canvas %s, scene %s\n", how, w, h, a, canvas,
            getenv("TOTK_RENDER_RES"));
}

/* graphics.aspect_ratio = window (TOTK_WIDE=window, the default since v0.1.9): the canvas's shape is chosen at the start
   (wide_canvas above; its heap and layout sets are read once, so it cannot follow live). A window that takes another
   shape later (a borderless window moved to another monitor, a windowed one resized) keeps the start's picture, shown
   whole with bars, and one line says that a restart gives the new shape. Once per shape, after it has held for about a
   second (a drag passes through many). totk_hooks.c calls it from the present; every 15th frame looks. A shape whose
   layouts were not built at this start (build_shape, g_shape_note) is said here too, once, when the picture is up. */
#if defined(_WIN32) && !defined(TOTK_NO_PC_SETTINGS)
extern void *shim_window_hwnd(void);               /* nvn/vulkan/shim_win.c */
extern void osd_toast(const char *text, int ms);   /* hle_osd.cpp */
void totk_window_shape_hint(uint64_t frame) {
    static int on = -1, start = -2, last = -2, told = -2;
    static uint64_t since;
    if (g_shape_note && frame >= 120) {
        osd_toast(g_shape_note == 1 ? pc_tr("Screen shape: start the game once from TotK Launcher.exe to prepare its layout (16:9 until then)")
                                    : pc_tr("This screen shape's layout was not prepared: 16:9 until Setup runs again"), 12000);
        g_shape_note = 0;
    }
    if (on < 0) {
        const char *w = getenv("TOTK_WIDE");
        on = w && !strcmp(w, "window") && !aspect_row_off();
    }
    if (!on || frame % 15) return;
    HWND h = (HWND)shim_window_hwnd();
    RECT r;
    if (!h || IsIconic(h) || !GetClientRect(h, &r) || r.right <= 0 || r.bottom <= 0) return;
    const int cls = shape_of((double)r.right / r.bottom);
    if (start == -2) {   /* (the first look: the canvas this start chose, and the window as it is now) */
        const char *out = getenv("TOTK_OUTPUT_RES");
        unsigned cw = 0, ch = 0;
        start = told = out && sscanf(out, "%ux%u", &cw, &ch) == 2 && ch ? shape_of((double)cw / ch) : -1;
        last = cls, since = frame;
    }
    if (cls != last) { last = cls, since = frame; return; }
    if (cls == told || frame - since < 60) return;
    told = cls;
    if (cls == start) return;   /* (back to the start's shape: its picture fits again) */
    char msg[256];
    if (cls >= 0)
        snprintf(msg, sizeof msg, pc_tr("Wide picture: F10 > Restart the game now to use %s"), k_shapes[cls].name);
    else snprintf(msg, sizeof msg, "%s", pc_tr("The window changed shape: F10 > Restart the game now to fit it"));
    fprintf(stderr, "[totk] aspect ratio: the window is %ldx%ld now, the canvas was chosen for another shape at the "
                    "start: %s\n", r.right, r.bottom, msg);
    osd_toast(msg, 8000);
}
#else
void totk_window_shape_hint(uint64_t frame) { (void)frame; }
#endif

/* The run-time half could not start. Unpatched, that only costs the settings; with the realtime step in main the game
   would freeze at its first frame (the step reads a data page that stays 0), so stop here with the reason instead.*/
static void boot_failed(uint64_t main_base, const char *why) {
    if (main_has_realtime(main_base)) {
        fprintf(stderr, "[totk] fatal: %s, and main has the realtime frame step (it would freeze at the first frame)\n",
                why);
        fflush(stderr);
        exit(3);
    }
    fprintf(stderr, "[totk] %s: game patches' run-time half off\n", why);
}

void totk_engine_boot(uint64_t main_base) {
    if (!data_page_mapped(main_base)) {
        boot_failed(main_base, "the data page (after main's span) is not committed RW");
        return;
    }
    int patched = main_has_realtime(main_base);
    if (patched) add_restart_rows();   /* saved values into TOTK_* before they are read below */
    if (patched) wide_canvas();        /* graphics.aspect_ratio: TOTK_OUTPUT_RES / TOTK_RENDER_RES from the window's shape */
    totk_settings_default(&g_s);
    const char *err = totk_settings_from_env(&g_s);
    if (err) {
        fprintf(stderr, "[totk] %s; the defaults stay\n", err);
        totk_settings_default(&g_s);
    }
    const char *root = getenv("TOTK_PATCH_SETS");
    if (root && *root) {
        snprintf(g_sets_root, sizeof g_sets_root, "%s", root);
        fit_output_set();   /* before the pool grows and the data page is written */
    }
    speed_fix_default(main_base);
    {   /* the engine's frame cap against the realtime step's floor (review N14): sustained above it the game runs fast */
        const char *fps = getenv("TOTK_FPS"), *unlock = getenv("TOTK_UNLOCK_FPS"), *ceil = getenv("TOTK_UNLOCK_CEIL");
        double floor_hz = 1000.0 / g_s.min_frame_ms + 0.5;   /* 240 against 1000 / (1000 / 240.0) is not above it */
        if (unlock && (!ceil || atof(ceil) > floor_hz))   /* unlocked: up to TOTK_UNLOCK_CEIL, else the monitor's rate */
            fprintf(stderr, "[totk] warning: TOTK_UNLOCK_FPS can present above TOTK_MAX_FPS=%.0f (%s): sustained above it "
                            "the game runs faster than real time\n", floor_hz - 0.5,
                    ceil ? "TOTK_UNLOCK_CEIL" : "the monitor's refresh");
        else if (!unlock && fps && atof(fps) > floor_hz)
            fprintf(stderr, "[totk] warning: TOTK_FPS=%s is above TOTK_MAX_FPS=%.0f: sustained above it the game runs "
                            "faster than real time\n", fps, floor_hz - 0.5);
    }
    err = totk_patches_init((void *)(uintptr_t)main_base, TOTK_MAIN_SPAN, &g_s, NULL);
    if (err) {
        boot_failed(main_base, err);
        return;
    }
    if (patched) add_live_rows();
    if (*g_sets_root) {
        overlay_sets();
    } else {
        char sets[2][64];
        int n = totk_overlay_sets(&g_s, sets);
        for (int i = 0; i < n; i++)
            if (strcmp(sets[i], "base"))   /* base = the engine's own overlay (the same two fixes) */
                fprintf(stderr, "[totk] warning: RomFS overlay set %s wanted but TOTK_PATCH_SETS is not set\n", sets[i]);
    }
    g_on = 1;
}

void totk_engine_present(void) {
    if (g_on) totk_frame_step_update(now_ns());
}

void totk_engine_interval(int interval) {
    totk_present_interval(interval);
}

uint64_t totk_engine_extra_memory(void) {
    return g_on ? totk_output_extra_bytes(&g_s) : 0;
}
