/* Checks games/totk/runtime/totk_patches.c against a fake main image: what the data page holds after init, the frame
   step at several present rates, the output switch (renderer bit + oe message), the heap formula (same numbers as
   games/totk/overlays extra_kib) and the overlay set names. Exit 0 = pass. */
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include "totk/data_page.h"
#include "totk/totk_patches.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint32_t posted[8];
static int nposted;
static void post(uint32_t m) { if (nposted < 8) posted[nposted++] = m; }

static float f32_at(const uint8_t *p, unsigned off) { float v; memcpy(&v, p + off, 4); return v; }
static uint32_t u32_at(const uint8_t *p, unsigned off) { uint32_t v; memcpy(&v, p + off, 4); return v; }

int main(void) {
    enum { SPAN = 0x11000 };   /* a span that is not 16 KiB aligned: the page goes to 0x14000 */
    uint8_t *main_base = calloc(1, 0x14000 + TOTK_DATA_PAGE_SIZE + 0x1000);
    uint8_t *renderer = calloc(1, 0x1000);
    TotkSettings s;
    totk_settings_default(&s);
    CHECK(TOTK_DATA_PAGE_OFFSET(SPAN) == 0x14000, "page offset %#llx", (unsigned long long)TOTK_DATA_PAGE_OFFSET(SPAN));
    CHECK(TOTK_DATA_PAGE_OFFSET(0x4666000) == 0x4668000, "1.0.0 main page offset");
    CHECK((uint8_t *)totk_data_page(main_base, SPAN) == main_base + 0x14000, "totk_data_page");

    /* settings from the environment */
    TotkSettings e = s;
    _putenv("TOTK_OUTPUT_RES=3440x1440");
    _putenv("TOTK_RENDER_RES=2560x1440");
    CHECK(totk_settings_from_env(&e) == NULL, "env parse");
    CHECK(e.output_w == 3440 && e.max_w == 3440 && e.max_h == 1440 && e.render_w == 2560, "env values");
    _putenv("TOTK_OUTPUT_RES=1x");
    CHECK(totk_settings_from_env(&e) != NULL, "bad TOTK_OUTPUT_RES refused");
    _putenv("TOTK_OUTPUT_RES=");
    _putenv("TOTK_RENDER_RES=");
    _putenv("TOTK_ASPECT=21:9");
    e = s;
    CHECK(totk_settings_from_env(&e) == NULL && fabsf(e.aspect - 21.0f / 9.0f) < 1e-6f, "TOTK_ASPECT");
    _putenv("TOTK_ASPECT=9");
    CHECK(totk_settings_from_env(&e) != NULL, "TOTK_ASPECT out of range refused");
    _putenv("TOTK_ASPECT=");
    e = s;
    CHECK(e.res_floor == 1.0f, "dynamic resolution off by default");
    _putenv("TOTK_DYNAMIC_RES=on");
    CHECK(totk_settings_from_env(&e) == NULL && e.res_floor == 0.0f, "TOTK_DYNAMIC_RES=on");
    _putenv("TOTK_DYNAMIC_RES=0.75");
    CHECK(totk_settings_from_env(&e) == NULL && e.res_floor == 0.75f, "TOTK_DYNAMIC_RES=0.75");
    _putenv("TOTK_DYNAMIC_RES=off");
    CHECK(totk_settings_from_env(&e) == NULL && e.res_floor == 1.0f, "TOTK_DYNAMIC_RES=off");
    _putenv("TOTK_DYNAMIC_RES=0.1");
    CHECK(totk_settings_from_env(&e) != NULL, "TOTK_DYNAMIC_RES floor out of range refused");
    _putenv("TOTK_DYNAMIC_RES=");
    e = s;
    CHECK(fabs(e.min_frame_ms - 1000.0 / 240) < 1e-9, "frame step floor 1/240 s by default");
    _putenv("TOTK_MAX_FPS=480");
    CHECK(totk_settings_from_env(&e) != NULL, "TOTK_MAX_FPS above 240 refused");
    _putenv("TOTK_MAX_FPS=144");
    CHECK(totk_settings_from_env(&e) == NULL && fabs(e.min_frame_ms - 1000.0 / 144) < 1e-9, "TOTK_MAX_FPS=144");
    _putenv("TOTK_MAX_FPS=");
    e = s;
    _putenv("TOTK_TERRAIN_LOD=3");
    CHECK(totk_settings_from_env(&e) != NULL, "TOTK_TERRAIN_LOD=3 refused (crashes the world load)");
    _putenv("TOTK_TERRAIN_LOD=2");
    CHECK(totk_settings_from_env(&e) == NULL && e.terrain_lod_scale == 2.0f, "TOTK_TERRAIN_LOD=2");
    _putenv("TOTK_TERRAIN_LOD=");
    e = s;
    _putenv("TOTK_FOV=1.25");
    CHECK(totk_settings_from_env(&e) == NULL && e.fov_scale == 1.25f, "TOTK_FOV=1.25");
    _putenv("TOTK_FOV=1");
    CHECK(totk_settings_from_env(&e) == NULL && e.fov_scale == 0.0f, "TOTK_FOV=1 is the game's own");
    _putenv("TOTK_FOV=3");
    CHECK(totk_settings_from_env(&e) != NULL, "TOTK_FOV out of range refused");
    _putenv("TOTK_FOV=");

    /* init: 1440p swapchain, starting at 1080p */
    s.max_w = 2560, s.max_h = 1440;
    CHECK(totk_patches_init(main_base, SPAN, &s, post) == NULL, "init");
    uint8_t *page = main_base + 0x14000;
    CHECK(fabsf(f32_at(page, TOTK_DP_FRAME_SECONDS) - 1 / 60.0f) < 1e-7f && fabsf(f32_at(page, TOTK_DP_FRAME_RATIO) - 0.5f) < 1e-7f,
          "initial frame step 1/60");
    CHECK(f32_at(page, TOTK_DP_RENDER_SIZE) == 1600.0f && f32_at(page, TOTK_DP_RENDER_SIZE + 4) == 900.0f, "render size");
    CHECK(u32_at(page, TOTK_DP_OUTPUT_SIZE) == 1920 && u32_at(page, TOTK_DP_OUTPUT_SIZE + 4) == 1080, "output size");
    CHECK(u32_at(page, TOTK_DP_SWAPCHAIN_SIZE) == 2560 && u32_at(page, TOTK_DP_SWAPCHAIN_SIZE + 4) == 1440, "swapchain size");
    CHECK(f32_at(page, TOTK_DP_ASPECT) == 0.0f, "16:9 output keeps the game's aspect");
    CHECK(f32_at(page, TOTK_DP_RES_FLOOR) == 1.0f, "dynamic resolution floor 1 (fixed)");
    CHECK(f32_at(page, TOTK_DP_FOV_SCALE) == 0.0f, "field of view: the game's own");
    CHECK(f32_at(page, TOTK_DP_MOUSE_SCALE) == 1.0f && f32_at(page, TOTK_DP_MOUSE_RATE) == TOTK_MOUSE_RATE_DEFAULT,
          "mouse multiplier 1, rate default");
    CHECK(f32_at(page, TOTK_DP_ACTOR_LOD) == 0.0f && u32_at(page, TOTK_DP_CAMERA) == 0, "actor LOD: the game's own; no camera yet");
    CHECK(f32_at(page, TOTK_DP_TERRAIN_LOD) == 0.0f, "terrain LoD: the game's own");
    CHECK(f32_at(page, TOTK_DP_SIM_RADIUS) == 1.0f, "simulation distance: 1 (the game's own), never 0");

    /* frame step: steady rates (the EMA and the owed time settle), the clamps and a gap */
    const double rates[] = {30, 60, 90, 120, 144, 240, 600, 20, 10};
    uint64_t t = 10000000000ull;
    for (unsigned r = 0; r < sizeof rates / sizeof *rates; r++) {
        for (int i = 0; i < 3000; i++) totk_frame_step_update(t += (uint64_t)(1e9 / rates[r]));
        double want = 1.0 / rates[r];
        if (want < 1.0 / 240) want = 1.0 / 240;   /* TOTK_MAX_FPS default*/
        if (want > 1.0 / 15) want = 1.0 / 15;
        float sec = f32_at(page, TOTK_DP_FRAME_SECONDS), ratio = f32_at(page, TOTK_DP_FRAME_RATIO);
        CHECK(fabs(sec - want) < 2e-6 && fabs(ratio - want * 30) < 5e-5, "%g Hz: seconds %g ratio %g, want %g", rates[r], sec, ratio, want);
    }
    /* at most 3 % per frame: a 200 ms hitch at 60 Hz moves the next step by 3 %, not by the hitch */
    for (int i = 0; i < 3000; i++) totk_frame_step_update(t += 16666667);
    totk_frame_step_update(t += 200000000);
    float hitch = f32_at(page, TOTK_DP_FRAME_SECONDS);
    CHECK(hitch > 1 / 60.0f && hitch <= 1.031f / 60.0f, "hitch: step %g, want <= 1.03 / 60", hitch);
    for (int i = 0; i < 3000; i++) totk_frame_step_update(t += 16666667);   /* the hitch's 50 ms repaid */
    /* the clock keeps real time: jittery presents (10 / 23.3 ms) and a rate change, given time = real time within 5 ms */
    double given = 0, real = 0;
    for (int i = 0; i < 1200; i++) {
        uint64_t d = i < 600 ? (i & 1 ? 10000000 : 23333333) : 33333333;
        given += f32_at(page, TOTK_DP_FRAME_SECONDS);   /* the step the game had for the frame this present ends */
        totk_frame_step_update(t += d);
        real += d * 1e-9;
    }
    CHECK(fabs(given - real) < 0.005, "clock: given %.4f s for %.4f s real", given, real);
    /* short frames below the clamp are repaid too (review N14): 60 Hz with every 10th frame 1 ms, then steady */
    given = real = 0;
    for (int i = 0; i < 1200; i++) {
        uint64_t d = i < 600 && i % 10 == 9 ? 1000000 : 16666667;
        given += f32_at(page, TOTK_DP_FRAME_SECONDS);
        totk_frame_step_update(t += d);
        real += d * 1e-9;
    }
    CHECK(fabs(given - real) < 0.005, "short frames: given %.4f s for %.4f s real", given, real);
    /* a live frame-rate cap change (totk_frame_step_retarget): 240 -> 30 and 30 -> 240 reach the new step within 0.4 s
       and the clock is off by the ramp alone (< 0.2 s), then runs at 1.0x; without it (the control) 240 -> 30 is still
       slow motion 0.4 s later */
    for (int ctl = 1; ctl >= 0; ctl--) {
        const double pair[2][2] = {{240, 30}, {30, 240}};
        for (int k = 0; k < (ctl ? 1 : 2); k++) {
            const double from = pair[k][0], to = pair[k][1];
            for (int i = 0; i < 3000; i++) totk_frame_step_update(t += (uint64_t)(1e9 / from));
            if (!ctl) totk_frame_step_retarget();
            given = real = 0;
            double given_at = 0, real_at = 0, step_at = 0;
            for (int i = 0; i < (int)(to * 4); i++) {
                given += f32_at(page, TOTK_DP_FRAME_SECONDS);
                totk_frame_step_update(t += (uint64_t)(1e9 / to));
                real += 1 / to;
                if (!step_at && real >= 0.4) step_at = f32_at(page, TOTK_DP_FRAME_SECONDS), given_at = given, real_at = real;
            }
            if (ctl) {
                CHECK(step_at < 0.5 / to, "control, no retarget: %g -> %g Hz still slow 0.4 s later (step %g)", from, to, step_at);
                continue;
            }
            CHECK(fabs(step_at * to - 1) < 0.05, "retarget %g -> %g Hz: step %g after 0.4 s, want %g", from, to, step_at, 1 / to);
            CHECK(fabs(given_at - real_at) < 0.2, "retarget %g -> %g Hz: clock off by %.3f s after 0.4 s", from, to, given_at - real_at);
            CHECK(fabs((given - given_at) - (real - real_at)) < 0.01, "retarget %g -> %g Hz: then 1.0x (%.4f s for %.4f s)",
                  from, to, given - given_at, real - real_at);
        }
    }
    /* the fast window ends once converged, before its 60 presents: 1 s steady at 30 after a retarget from 240, then a
       40 ms frame moves the step by the hitch ramp's 3 %, not 25 %*/
    for (int i = 0; i < 3000; i++) totk_frame_step_update(t += 4166667);
    totk_frame_step_retarget();
    for (int i = 0; i < 30; i++) totk_frame_step_update(t += 33333333);
    float settled = f32_at(page, TOTK_DP_FRAME_SECONDS);
    totk_frame_step_update(t += 40000000);
    float jitter = f32_at(page, TOTK_DP_FRAME_SECONDS);
    CHECK(fabs(settled * 30 - 1) < 0.01 && jitter <= settled * 1.031f, "retarget window over after 1 s: %g -> %g",
          settled, jitter);
    for (int i = 0; i < 3000; i++) totk_frame_step_update(t += 16666667);
    float before = f32_at(page, TOTK_DP_FRAME_SECONDS);
    totk_frame_step_update(999000000000ull);   /* a 900+ s gap: no step change */
    CHECK(f32_at(page, TOTK_DP_FRAME_SECONDS) == before, "a gap leaves the step alone");
    totk_frame_step_pin(50);
    CHECK(fabsf(f32_at(page, TOTK_DP_FRAME_SECONDS) - 0.05f) < 1e-7f, "pin 50 ms");
    totk_frame_step_update(999100000000ull);
    CHECK(fabsf(f32_at(page, TOTK_DP_FRAME_SECONDS) - 0.05f) < 1e-7f, "pinned step holds");
    totk_frame_step_pin(0);

    /* mouse claim heartbeat (review D1): released once no camera step ran for more than TOTK_MOUSE_STALE_PRESENTS */
    uint32_t one = 1, beat = u32_at(page, TOTK_DP_PRESENTS);
    memcpy(page + TOTK_DP_PC_CLAIMED, &one, 4);
    memcpy(page + TOTK_DP_MOUSE_BEAT, &beat, 4);
    uint64_t tm = 999200000000ull;
    for (unsigned i = 0; i < TOTK_MOUSE_STALE_PRESENTS; i++) totk_frame_step_update(tm += 16666667);
    CHECK(u32_at(page, TOTK_DP_PRESENTS) == beat + TOTK_MOUSE_STALE_PRESENTS && u32_at(page, TOTK_DP_PC_CLAIMED) == 1,
          "claim held while the step is recent");
    totk_frame_step_update(tm += 16666667);
    CHECK(u32_at(page, TOTK_DP_PC_CLAIMED) == 0, "claim released after a stale beat");
    beat = u32_at(page, TOTK_DP_PRESENTS);   /* the step runs again and primes */
    memcpy(page + TOTK_DP_PC_CLAIMED, &one, 4);
    memcpy(page + TOTK_DP_MOUSE_BEAT, &beat, 4);
    totk_frame_step_update(tm += 16666667);
    CHECK(u32_at(page, TOTK_DP_PC_CLAIMED) == 1, "a fresh beat keeps the claim");

    /* output switching: before the game laid out (no renderer) and after */
    CHECK(totk_output_res_toggle() == 1 && totk_output_size_current() == ((uint64_t)1440 << 32 | 2560), "toggle to 1440p");
    CHECK(nposted == 0, "no message before the renderer exists");
    uint64_t r = (uint64_t)(uintptr_t)renderer;
    memcpy(page + TOTK_DP_RENDERER, &r, 8);   /* what the patched main+0xc630a8 stores */
    CHECK(totk_output_res_toggle() == 1 && totk_output_size_current() == ((uint64_t)1080 << 32 | 1920), "toggle back");
    CHECK(nposted == 1 && posted[0] == 30 && (u32_at(renderer, 0xca4) & 0x200), "renderer bit 0x200 + oe message 30");
    CHECK(totk_output_res_set(3840, 2160) == 0, "bigger than the swapchain refused");

    /* aspect */
    totk_aspect_set(21.0f / 9.0f);
    CHECK(fabsf(totk_aspect_current() - 21.0f / 9.0f) < 1e-6f, "aspect set");
    totk_aspect_set(0);
    CHECK(totk_aspect_current() == 0.0f, "aspect back to the game's own");

    /* dynamic resolution floor while running */
    totk_dynamic_res_set(0.0f);
    CHECK(f32_at(page, TOTK_DP_RES_FLOOR) == 0.0f && totk_dynamic_res_current() == 0.0f, "dynamic resolution on");
    totk_dynamic_res_set(0.8f);
    CHECK(f32_at(page, TOTK_DP_RES_FLOOR) == 0.8f, "dynamic resolution floor 0.8");
    totk_dynamic_res_set(7.0f);
    CHECK(f32_at(page, TOTK_DP_RES_FLOOR) == 1.0f, "floor clamped to 1");
    totk_dynamic_res_set(-1.0f);
    CHECK(f32_at(page, TOTK_DP_RES_FLOOR) == 0.0f, "negative floor = the game's own");
    totk_dynamic_res_set(1.0f);
    totk_fov_set(1.2f);
    CHECK(f32_at(page, TOTK_DP_FOV_SCALE) == 1.2f && totk_fov_current() == 1.2f, "fov 1.2");
    totk_fov_set(9.0f);
    CHECK(f32_at(page, TOTK_DP_FOV_SCALE) == 2.0f, "fov clamped to 2");
    totk_fov_set(1.0f);
    CHECK(f32_at(page, TOTK_DP_FOV_SCALE) == 0.0f, "fov 1 = the game's own");
    totk_shadow_res_set(2.0f);
    CHECK(f32_at(page, TOTK_DP_SHADOW_SCALE) == 2.0f && totk_shadow_res_current() == 2.0f, "shadow map x2");
    totk_shadow_res_set(1.0f);
    CHECK(f32_at(page, TOTK_DP_SHADOW_SCALE) == 0.0f, "shadow map 1 = the game's own");
    totk_shadow_res_set(4.0f);
    CHECK(f32_at(page, TOTK_DP_SHADOW_SCALE) == 2.0f, "shadow map clamped to 2 (review D3)");
    totk_shadow_res_set(0.0f);
    totk_lod_set(3.0f);
    CHECK(f32_at(page, TOTK_DP_LOD_SCALE) == 3.0f && totk_lod_current() == 3.0f, "lod x3");
    totk_lod_set(100.0f);
    CHECK(f32_at(page, TOTK_DP_LOD_SCALE) == 8.0f, "lod clamped to 8");
    totk_lod_set(0.0f);
    unsigned nrows;
    const TotkSettingRow *rows = totk_setting_rows(&nrows);
    CHECK(nrows == 8 && rows[0].slot == TOTK_DP_RES_FLOOR && rows[0].def == 1.0 && rows[5].slot == TOTK_DP_TERRAIN_LOD &&
          rows[6].slot == TOTK_DP_SIM_RADIUS && rows[7].slot == TOTK_DP_MOUSE_SCALE, "settings table");
    CHECK(rows[2].slot == TOTK_DP_SHADOW_SCALE && rows[2].max == 2.0, "shadow resolution row capped at 2");

    /* realtime off: the shipping step from the present interval */
    TotkSettings off = s;
    off.realtime = 0;
    totk_patches_init(main_base, SPAN, &off, post);
    totk_present_interval(2);
    totk_frame_step_update(5);
    CHECK(fabsf(f32_at(page, TOTK_DP_FRAME_RATIO) - 1.0f) < 1e-7f && fabsf(f32_at(page, TOTK_DP_FRAME_SECONDS) - 1 / 30.0f) < 1e-7f,
          "interval 2: ratio 1, 1/30 s");

    /* heap formula: the same KiB as games/totk/overlays extra_kib() */
    struct { unsigned w, h, rw, rh; unsigned long long kib; } heap[] = {
        {1920, 1080, 1600, 900, 0}, {2560, 1440, 1600, 900, 198 * 1024ull - 113664}, {3840, 2160, 1600, 900, 444 * 1024ull - 113664},
    };
    for (unsigned i = 0; i < sizeof heap / sizeof *heap; i++) {
        TotkSettings h = s;
        h.max_w = heap[i].w, h.max_h = heap[i].h, h.render_w = heap[i].rw, h.render_h = heap[i].rh;
        unsigned long long got = (unsigned long long)(totk_output_extra_bytes(&h) >> 10);
        CHECK(got == heap[i].kib, "%ux%u: %llu KiB, want %llu", heap[i].w, heap[i].h, got, heap[i].kib);
    }

    /* overlay sets */
    char names[2][64];
    TotkSettings o = s;
    o.output_w = o.max_w = 3440, o.output_h = o.max_h = 1440;
    int n = totk_overlay_sets(&o, names);
    CHECK(n == 2 && !strcmp(names[0], "aspect-3440x1440") && !strcmp(names[1], "output-3440x1440"), "ultrawide sets: %d %s %s", n, names[0], n > 1 ? names[1] : "");
    totk_settings_default(&o);
    n = totk_overlay_sets(&o, names);
    CHECK(n == 1 && !strcmp(names[0], "base"), "default sets");
    o.tex_cache = 2.0f;   /* texture cache: the same numbers as games/totk/overlays tex_extra_kib() */
    n = totk_overlay_sets(&o, names);
    CHECK(n == 2 && !strcmp(names[1], "output-1920x1080-tex200") && totk_output_extra_bytes(&o) == 501760ull << 10,
          "texture cache x2 at 1080p: %s, %llu KiB", n > 1 ? names[1] : "", (unsigned long long)(totk_output_extra_bytes(&o) >> 10));
    o.max_w = 2560, o.max_h = 1440, o.render_w = 2560, o.render_h = 1440, o.tex_cache = 1.5f;
    n = totk_overlay_sets(&o, names);
    CHECK(n == 2 && !strcmp(names[1], "output-2560x1440-scene-2560x1440-tex150"), "texture cache x1.5 with a scene: %s", n > 1 ? names[1] : "");
    CHECK(totk_output_extra_bytes(&o) >> 10 == 250880ull + (unsigned long long)(ceil(113664.0 * (1 + (2560.0 * 1440 / (1920.0 * 1080) - 1 +
          2560.0 * 1440 / (1600.0 * 900) - 1) * 1.6) / 1024.0) * 1024 - 113664), "texture cache x1.5 + 1440p scene: KiB");

    /* quality presets: every key is a setting (a live row, or one of the engine glue's restart rows) and every value
       lies in the row's range */
    {
        static const struct { const char *key; double min, max; } restart[] = {
            {"graphics.texture_cache", 1, 3}, {"graphics.grass_density", 0.25, 4},
            {"graphics.shadow_distance", 0.5, 4}, {"graphics.fog_distance", 0.5, 4}};
        unsigned np, nr;
        const TotkQualityPreset *ps = totk_quality_presets(&np);
        const TotkSettingRow *rows = totk_setting_rows(&nr);
        CHECK(np == 5 && !strcmp(ps[0].name, "Low") && !strcmp(ps[3].name, "Ultra") && !strcmp(ps[4].name, "Ultra+"),
              "presets: Low .. Ultra+ (%u)", np);
        for (unsigned i = 0; i < np; i++)
            for (unsigned j = 0; j < ps[i].count; j++) {
                const char *key = ps[i].values[j].key;
                double v = atof(ps[i].values[j].value), lo = -1, hi = -1;
                for (unsigned r = 0; r < nr; r++) if (!strcmp(rows[r].key, key)) lo = rows[r].min, hi = rows[r].max;
                for (unsigned r = 0; r < sizeof restart / sizeof *restart; r++)
                    if (!strcmp(restart[r].key, key)) lo = restart[r].min, hi = restart[r].max;
                CHECK(lo >= 0 && v >= lo && v <= hi, "preset %s: %s = %s (range %g..%g)", ps[i].name, key,
                      ps[i].values[j].value, lo, hi);
            }
    }

    printf("%s: %d checks, %d failures\n", failures ? "FAIL" : "pass", checks, failures);
    free(main_base);
    free(renderer);
    return failures ? 1 : 0;
}
