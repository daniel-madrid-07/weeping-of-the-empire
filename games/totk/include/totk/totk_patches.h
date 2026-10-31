/* Run-time side of the TotK patches (games/totk/patches): the host writes the values the patched game code reads from
 * the data page (data_page.h). Plain C, no OS calls besides getenv / stderr; every function is safe to call when the
 * matching code patch is absent (it then only writes a word nobody reads).
 *
 * Who calls what:
 *   start-up     totk_settings_default / totk_settings_from_env, then totk_patches_init() after main is mapped and the
 *                data page committed, before the game's first instruction. totk_output_extra_bytes() is added to the
 *                application memory pool, and the overlay sets totk_overlay_sets() names are loaded from <work>/overlays.
 *   each present totk_frame_step_update(now_ns) from the thread that presents the game's frame (the
 *   nvnQueuePresentTexture), with a monotonic clock in nanoseconds. present int. totk_present_interval(n) when the game
 *   calls nvnWindowSetPresentInterval (used with realtime off).
 *   hotkeys      totk_output_res_toggle() (platform hotkey 3), totk_aspect_set() / totk_aspect_cycle() (hotkey 4).
 *   overlay      totk_setting_rows(): app registers them in platform's settings registry and mirrors them into
 *                the data page; totk_*_set() remain for callers without platform.
 * */
#ifndef TOTK_PATCHES_H
#define TOTK_PATCHES_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct TotkSettings {
    int realtime;                       /* 1 (default): frame step from measured present times; 0: shipping step */
    unsigned render_w, render_h;        /* scene render size (default 1600x900, the game's docked size) */
    unsigned output_w, output_h;        /* the game's output (canvas) size at start (default 1920x1080) */
    unsigned max_w, max_h;              /* the swapchain: the largest output size (default = output) */
    float aspect;                       /* camera aspect ratio; 0 = the output's own shape when that is not 16:9 */
    float res_floor;                    /* lowest dynamic-resolution scale (default 1: the scene is always rendered at
                                           render_w x render_h); 0 = the game's own tiers (down to its lowest) */
    float fov_scale;                    /* camera vertical field-of-view multiplier (default 0 = the game's own) */
    float shadow_scale;                 /* depth shadow map size multiplier (default 0 = the game's own) */
    float lod_scale;                    /* model LOD distance multiplier (default 0 = the game's own) */
    float actor_lod_scale;              /* actor LOD area distance multiplier (default 0 = the game's own) */
    float terrain_lod_scale;            /* terrain LoD distance multiplier (default 0 = the game's own) */
    float tex_cache;                    /* texture cache (TextureHeap) multiplier, 1..3 (default 1; restart: HeapDef set) */
    float sim_scale;                    /* actor simulation (job push) distance multiplier (default 1 = the game's
                                           own; never 0: the patch divides by its square) */
    float mouse_scale;                  /* mouse camera multiplier (default 1); 0 = off: platform's stick emulation */
    float mouse_rate;                   /* stick deflection per mouse count at the 30 Hz ratio (calibration, default
                                           TOTK_MOUSE_RATE_DEFAULT) */
    double min_frame_ms, max_frame_ms;  /* clamp of the measured frame step (default 1000/240 .. 1000/15: sustained above
                                           240 FPS the game runs faster than real time, below 15 FPS slower) */
} TotkSettings;

/* Full right-stick deflection turns the camera by an unmeasured angle per 30 Hz frame; 0.02 assumes about 0.11 rad
 * (190 degrees a second), so one count at mouse sensitivity 1 turns about 0.0022 rad (2900 counts a turn). */
#define TOTK_MOUSE_RATE_DEFAULT 0.02f

void totk_settings_default(TotkSettings *s);

/* TOTK_REALTIME=0|1, TOTK_RENDER_RES=<w>x<h>, TOTK_OUTPUT_RES=<w>x<h>, TOTK_OUTPUT_MAX=<w>x<h>, TOTK_ASPECT=<w>:<h> |
 * <ratio> | 0, TOTK_MAX_FPS=<hz 30..240> (lower clamp of the frame step), TOTK_DYNAMIC_RES=off | on | <floor 0.25..1> (off: fixed
 * scene size, the default; on: the game's dynamic resolution), TOTK_FOV=<multiplier 0.5..2> | 0 (the game's own),
 * TOTK_SHADOW_RES=<multiplier 0.5..2> | 0 (the game's own), TOTK_LOD=<multiplier 0.5..8> | 0 (the game's own), TOTK_ACTOR_LOD=<multiplier 0.5..4> | 0 (the game's own),
 * TOTK_TERRAIN_LOD=<multiplier 0.5..2> | 0 (the game's own), TOTK_SIM_DISTANCE=<multiplier 0.5..4> (1 = the game's own),
 * TOTK_TEXTURE_CACHE=<multiplier 1..3 | percentage 100..300> (texture cache size, needs its HeapDef set),
 * TOTK_MOUSE_CAMERA=<multiplier 0..4> (0: platform's stick
 * emulation instead of the camera hook), TOTK_MOUSE_RATE=<stick per count, 0.001..1> (calibration). Returns NULL, or what was wrong (the settings keep the
 * values read before it). */
const char *totk_settings_from_env(TotkSettings *s);

/* Writes the data page of main (guest == host addresses: main_base is main's load address, main_span its loaded size,
 * layout.json "span") and remembers it for the calls below. post_oe_message posts an nn::oe / applet message to the
 * game (30 = operation mode changed); NULL disables switching the output size while running. Returns NULL or an error. */
const char *totk_patches_init(void *main_base, uint64_t main_span, const TotkSettings *s,
                              void (*post_oe_message)(uint32_t message));

/* The data page's address for main_base / main_span (what totk_patches_init writes). */
void *totk_data_page(void *main_base, uint64_t main_span);
/* The data page totk_patches_init wrote, NULL before it ran (app: platform's PC page and setting mirrors). */
void *totk_data_page_current(void);

void totk_frame_step_update(uint64_t now_ns);
void totk_present_interval(int interval);
/* Diagnostics: pin the frame step to ms milliseconds (0 = back to measured time). */
void totk_frame_step_pin(double ms);
/* The frame-rate cap changed (live display.fps_cap): the next TOTK_RETARGET_PRESENTS presents move the realtime step to
 * the new rate within ~10 frames (25 % per frame, nothing owed) instead of the hitch ramp's 3 % (seconds of slow or
 * fast motion on 240 <-> 30). Any thread; call it when the new cap takes effect. */
#define TOTK_RETARGET_PRESENTS 60
void totk_frame_step_retarget(void);

/* Switch the game's output size while running (<= the swapchain size in both dimensions); the game lays out its crop,
 * canvas and UI again at its next display update. Returns 1 when requested. */
int totk_output_res_set(unsigned w, unsigned h);
/* Between 1920x1080 and the swapchain size. */
int totk_output_res_toggle(void);
uint64_t totk_output_size_current(void);   /* (h << 32) | w */

/* Camera aspect ratio (0 = the game's own 16:9). The UI follows only with the aspect overlay for that shape. */
void totk_aspect_set(float ratio);
float totk_aspect_current(void);

/* Dynamic resolution floor while running: 1 = fixed scene size, 0 = the game's own tiers, between: never below that
 * fraction of the scene size. Takes effect at the controller's next tier change (its scale is cached otherwise). */
void totk_dynamic_res_set(float floor);
float totk_dynamic_res_current(void);

/* Field-of-view multiplier while running (0 or 1 = the game's own; clamped to 0.5..2). Takes effect next frame. */
void totk_fov_set(float scale);
float totk_fov_current(void);

/* Depth shadow map size multiplier while running (0 or 1 = the game's own; clamped to 0.5..2: 2 = 4x the memory, and
 * the game's pools are not grown). The game reallocates the map at its next shadow draw. */
void totk_shadow_res_set(float scale);
float totk_shadow_res_current(void);

/* Model LOD distance multiplier while running (0 or 1 = the game's own; clamped to 0.5..8): each LOD level is kept k
 * times further away. Takes effect next frame. */
void totk_lod_set(float scale);
float totk_lod_current(void);

/* The run-time settings of the graphics patches, for platform's settings registry. app registers each row with
 * plat_setting_register (key, group, label, help, FLOAT, def / min / max / step, PERCENT when `percent`) and then
 * plat_setting_mirror(id, data_page + slot, PLAT_MIRROR_F32), after totk_patches_init (which writes the slots from
 * the environment first). Every slot takes the setting's value as is: 1.0 is the game's own for the multipliers. */
typedef struct TotkSettingRow {
    const char *key, *group, *label, *help;
    uint32_t slot;                      /* data page offset of the f32 */
    double def, min, max, step;
    int percent;                        /* show as a percentage */
} TotkSettingRow;
const TotkSettingRow *totk_setting_rows(unsigned *count);

/* Quality presets (Low / Medium / High / Ultra / Ultra+) over the patches' settings, for the overlay: each preset is a
 * list of (setting key, value) the overlay sets as if the player had (the live rows of totk_setting_rows() and the
 * engine glue's restart rows graphics.texture_cache / grass_density / shadow_distance / fog_distance). Low is the game
 * as shipped (its own dynamic resolution, every multiplier 1); Ultra+ puts each knob at its maximum. Settings that
 * change gameplay (game.simulation_distance), the field of view and the fog are the player's: no preset touches them.
 * Every value has its RomFS set built (python -m games.totk.overlays ... --texture-cache 1.5,2,3 --grass 2,3,4
 * --shadow 1.5,2,3,4). The levels are not measured yet: the costs per knob come from the end-of-phase round. */
typedef struct TotkPresetValue { const char *key, *value; } TotkPresetValue;
typedef struct TotkQualityPreset {
    const char *name;
    const TotkPresetValue *values;
    unsigned count;
} TotkQualityPreset;
const TotkQualityPreset *totk_quality_presets(unsigned *count);

/* Bytes the application memory pool must grow by for the settings' largest output and scene and the texture cache (the
 * HeapDef overlay grows the game's heaps by the same amount). Same formulas as games/totk/overlays extra_kib() and
 * tex_extra_kib(). */
uint64_t totk_output_extra_bytes(const TotkSettings *s);

/* Names of the overlay sets under <work>/overlays the runtime should load for these settings, in order (base or
 * aspect-<w>x<h>, then output-<w>x<h>[-scene-<w>x<h>][-tex<percent>] when the output is bigger than 1920x1080, the
 * scene bigger than 1600x900 or the texture cache grown). Returns how many were written. */
int totk_overlay_sets(const TotkSettings *s, char names[2][64]);

#ifdef __cplusplus
}
#endif

#endif
