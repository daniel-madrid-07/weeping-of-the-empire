/* The TotK data page: the words the patched game code reads its run-time settings from (games/totk/patches).
 *
 * The page sits right after main's loaded image: guest address = main base + TOTK_DATA_PAGE_OFFSET(main image size), 16
 * KiB; the code patches reach it with ADRP, so it must be at exactly that address. Slots marked "runtime" are written
 * by the host, "game" by the patched game code. Keep in step with games/totk/patches/sites_v100.py (DP_*).*/
#ifndef TOTK_DATA_PAGE_H
#define TOTK_DATA_PAGE_H

#include <stdint.h>

#define TOTK_DATA_PAGE_SIZE 0x4000u
/* main's loaded image size (text + rodata + data + bss, from the module's segment table) rounded up to 16 KiB. */
#define TOTK_DATA_PAGE_OFFSET(main_image_size) (((uint64_t)(main_image_size) + 0x3fffu) & ~(uint64_t)0x3fffu)

enum {
    TOTK_DP_FRAME_RATIO = 0,       /* runtime f32: the 30 Hz ratio (frame seconds * 30); read with +4 as one u64 */
    TOTK_DP_FRAME_SECONDS = 4,     /* runtime f32: frame seconds */
    TOTK_DP_RENDER_SIZE = 16,      /* runtime f32 w, f32 h: scene render size (feature render_res) */
    TOTK_DP_OUTPUT_SIZE = 32,      /* runtime u32 w, u32 h: the game's output / canvas size (feature output_res) */
    TOTK_DP_RENDERER = 40,         /* game    u64: the renderer main+0xc63064 lays out (feature output_res) */
    TOTK_DP_SWAPCHAIN_SIZE = 48,   /* runtime u32 w, u32 h: the swapchain size, read once at start (feature output_res) */
    TOTK_DP_ASPECT = 56,           /* runtime f32: camera aspect ratio, 0 = the game's own 16:9 (feature aspect) */
    TOTK_DP_RES_FLOOR = 64,        /* runtime f32: lowest dynamic-resolution scale, 1 = fixed, 0 = the game's own (dynres) */
    TOTK_DP_FOV_SCALE = 68,        /* runtime f32: camera field-of-view multiplier, 0 = the game's own (feature fov) */
    TOTK_DP_SHADOW_SCALE = 72,     /* runtime f32: depth shadow map size multiplier, 0 = the game's own (shadowres) */
    TOTK_DP_LOD_SCALE = 76,        /* runtime f32: model LOD distance multiplier, 0 = the game's own (feature lod) */
    TOTK_DP_MOUSE_SCALE = 80,      /* runtime f32: mouse camera multiplier, 0 = off: platform's stick emulation (mouse) */
    TOTK_DP_MOUSE_RATE = 84,       /* runtime f32: stick deflection per mouse count at the 30 Hz ratio (feature mouse) */
    TOTK_DP_PRESENTS = 88,         /* runtime u32: presents so far (the mouse veneer drops motion across skipped steps) */
    TOTK_DP_MOUSE_BEAT = 92,       /* game    u32: TOTK_DP_PRESENTS at the last camera input step (feature mouse) */
    TOTK_DP_MOUSE_LAST = 96,       /* game    f64 x, f64 y: the PC page's mouse totals already applied (feature mouse) */
    TOTK_DP_ACTOR_LOD = 112,       /* runtime f32: actor LOD area distance multiplier, 0 = the game's own (actor_lod) */
    TOTK_DP_TERRAIN_LOD = 116,     /* runtime f32: terrain LoD distance multiplier, 0 = the game's own (terrain_lod) */
    TOTK_DP_SIM_RADIUS = 120,      /* runtime f32: actor simulation distance multiplier, > 0, 1 = the game's own (sim_radius) */
    TOTK_DP_FOV_BASE = 124,        /* game    f32: the camera's own fovy (radians) at the last fov site (feature fov) */
    TOTK_DP_MOUSE_PITCH = 128,     /* runtime f32: mouse pitch gain g: the veneer turns the stick y by sign(v) sqrt(g |v|),
                                      v = dy * k (the camera pitches by y |y|); <= 0 = linear, the host shapes y (mouse) */
    TOTK_DP_MOUSE_PITCH_ACK = 132, /* game    u32: 1 = this key's mouse veneer shapes pitch (stored at every applied step,
                                      whatever the gain): until then the host shapes y itself and leaves the gain 0 */
    TOTK_DP_CAMERA = 136,          /* game    u64: the PlayerCamera of the last camera pre-update (feature mouse) */
    /* The mouse late latch's live block (the prototype; written only with TOTK_LATE_LATCH=1): the PC page's totals
       as of a QPC time, at 1 kHz, under a seqlock (seq odd while writing; read seq, data, seq). +176..+207 is the
       future per-consume block.*/
    TOTK_DP_LATCH_SEQ = 144,       /* runtime u32: the block's seq, odd while it is written */
    TOTK_DP_LATCH_FLAGS = 148,     /* runtime u32: bit 0 = the bow drawn, the Purah Pad camera or the Scope in use */
    TOTK_DP_LATCH_TOTALS = 152,    /* runtime f64 x, f64 y: the PC page's mouse totals (the TOTK_DP_MOUSE_LAST counter) */
    TOTK_DP_LATCH_QPC = 168,       /* runtime i64: QueryPerformanceCounter when those totals were read */
    /* platform's PlatPcPage (platform/include/platform/pc.h, 0x80 bytes): mouse totals, glyph style, overlay state.
       app binds it after totk_patches_init: plat_pc_page_bind(page + TOTK_DP_PC_OFFSET, sizeof(PlatPcPage)). */
    TOTK_DP_PC_OFFSET = 0x100,
    TOTK_DP_PC_SIZE = 0x80,
    TOTK_DP_PC_CLAIMED = TOTK_DP_PC_OFFSET + 0x64, /* PlatPcPage.guest_mouse_claimed: set by the mouse veneer (mouse) */
};

/* The mouse veneer drops the motion of a step that comes more than this many presents after the previous one, and the
 * host releases the claim (platform's stick emulation takes over) once no step ran for that long (sites_v100.py
 * MOUSE_STALE; verify_sites.py checks they match). */
#define TOTK_MOUSE_STALE_PRESENTS 3u

#endif
