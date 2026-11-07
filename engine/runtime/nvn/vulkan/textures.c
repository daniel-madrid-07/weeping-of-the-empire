// Vulkan textures: NVN formats, images, change-detected uploads from guest memory, views and samplers
// (counterpart of the texture half of metal.m and the view helpers of renderer.m).
#include <SDL3/SDL_timer.h>
#include <float.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>
#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#ifdef _MSC_VER
#include <intrin.h>
#else
#include <x86intrin.h>
#endif

#include "contact.h"
#include "vk.h"
#include "../../cpu_place.h"   // nxrecomp: which cores each pool runs on
#include "shaders.h"
#include "vram_fit.h"   // (res_real_budget: the room another process leaves)
#include "astc_comp.h"
#include "bc3_comp.h"
#include "../bc3enc.h"
#include "../astc.h"
#include <stdatomic.h>
static void astc_decode_par(const uint8_t *blocks, uint32_t w, uint32_t h, uint32_t bw, uint32_t bh, bool srgb, uint8_t *out);
static bool astc_decode_par_src(const uint8_t *src, bool linear, uint64_t stride, uint32_t wb, uint32_t hb, uint32_t bh_log2,
                                uint32_t bd_log2, uint32_t w, uint32_t h, uint32_t bw, uint32_t bh, bool srgb, uint8_t *out);
#include "../shim/las.h"  // TotK: was ../renderer.h
#include "../shim/las.h"  // TotK: was ../texpatch.h
extern int vk_up_kind;   // (exec.c upload stats by kind)

// ================================================================== formats
typedef struct { VkFormat vf; uint8_t expand_rgb, alpha_one, perm; } VkFmt;

// The 16-bit packed formats name their channels from the low bits up (NVN RGBA8: R in byte 0 Maxwell's TIC formats
// A4B4G4R4 / B5G6R5 / A1B5G5R5, red lowest), Vulkan's from the high bits down (R5G6B5: red highest).
// LARECOMP_FMT16=<mask> (default 3; 0 = the old high-red table): bit 0 RGB565 (0x3C) as B5G6R5, bit 1 RGB5A1 (0x3B) as
// A1R5G5B5 with red and blue swapped in the view (A1B5G5R5 itself needs maintenance5), bit 2 (off: no TotK image seen)
// RGBA4 (0x39) as R4G4B4A4 read reversed (abgr). perm is the sampled views' channel order; attachments keep the
// identity (logged once if one is rendered)
enum { PERM_NONE, PERM_SWAP_RB, PERM_REVERSE };
static int fmt16_mask(void) {
    static int m = -1;
    if (m < 0) { const char *e = getenv("LARECOMP_FMT16"); m = e ? (int)strtol(e, NULL, 0) : 3; }
    return m;
}

static VkFmt fmt_of(uint32_t f) {
    int m16 = fmt16_mask();
    if (m16) {
        if (f == 0x3C && (m16 & 1)) return (VkFmt){VK_FORMAT_B5G6R5_UNORM_PACK16};
        if (f == 0x3B && (m16 & 2)) return (VkFmt){VK_FORMAT_A1R5G5B5_UNORM_PACK16, 0, 0, PERM_SWAP_RB};
        if (f == 0x39 && (m16 & 4)) return (VkFmt){VK_FORMAT_R4G4B4A4_UNORM_PACK16, 0, 0, PERM_REVERSE};
    }
    switch (f) {
    case 0x01: return (VkFmt){VK_FORMAT_R8_UNORM};
    case 0x02: return (VkFmt){VK_FORMAT_R8_SNORM};
    case 0x03: return (VkFmt){VK_FORMAT_R8_UINT};
    case 0x04: return (VkFmt){VK_FORMAT_R8_SINT};
    case 0x05: return (VkFmt){VK_FORMAT_R16_SFLOAT};
    case 0x06: return (VkFmt){VK_FORMAT_R16_UNORM};
    case 0x07: return (VkFmt){VK_FORMAT_R16_SNORM};
    case 0x08: return (VkFmt){VK_FORMAT_R16_UINT};
    case 0x09: return (VkFmt){VK_FORMAT_R16_SINT};
    case 0x0A: return (VkFmt){VK_FORMAT_R32_SFLOAT};
    case 0x0B: return (VkFmt){VK_FORMAT_R32_UINT};
    case 0x0C: return (VkFmt){VK_FORMAT_R32_SINT};
    case 0x0D: return (VkFmt){VK_FORMAT_R8G8_UNORM};
    case 0x0E: return (VkFmt){VK_FORMAT_R8G8_SNORM};
    case 0x0F: return (VkFmt){VK_FORMAT_R8G8_UINT};
    case 0x10: return (VkFmt){VK_FORMAT_R8G8_SINT};
    case 0x11: return (VkFmt){VK_FORMAT_R16G16_SFLOAT};
    case 0x12: return (VkFmt){VK_FORMAT_R16G16_UNORM};
    case 0x13: return (VkFmt){VK_FORMAT_R16G16_SNORM};
    case 0x14: return (VkFmt){VK_FORMAT_R16G16_UINT};
    case 0x15: return (VkFmt){VK_FORMAT_R16G16_SINT};
    case 0x16: return (VkFmt){VK_FORMAT_R32G32_SFLOAT};
    case 0x17: return (VkFmt){VK_FORMAT_R32G32_UINT};
    case 0x18: return (VkFmt){VK_FORMAT_R32G32_SINT};
    case 0x19: return (VkFmt){VK_FORMAT_R8G8B8A8_UNORM, 1, 1};  // RGB8: expanded on upload
    case 0x25: return (VkFmt){VK_FORMAT_R8G8B8A8_UNORM};
    case 0x26: return (VkFmt){VK_FORMAT_R8G8B8A8_SNORM};
    case 0x27: return (VkFmt){VK_FORMAT_R8G8B8A8_UINT};
    case 0x28: return (VkFmt){VK_FORMAT_R8G8B8A8_SINT};
    case 0x29: return (VkFmt){VK_FORMAT_R16G16B16A16_SFLOAT};
    case 0x2A: return (VkFmt){VK_FORMAT_R16G16B16A16_UNORM};
    case 0x2B: return (VkFmt){VK_FORMAT_R16G16B16A16_SNORM};
    case 0x2C: return (VkFmt){VK_FORMAT_R16G16B16A16_UINT};
    case 0x2D: return (VkFmt){VK_FORMAT_R16G16B16A16_SINT};
    case 0x2E: return (VkFmt){VK_FORMAT_R32G32B32A32_SFLOAT};
    case 0x2F: return (VkFmt){VK_FORMAT_R32G32B32A32_UINT};
    case 0x30: return (VkFmt){VK_FORMAT_R32G32B32A32_SINT};
    case 0x31: return (VkFmt){VK_FORMAT_S8_UINT};
    case 0x32: {   // LARECOMP_D16_AS_D32=1: A/B for depth-EQUAL material passes (TotK lighting quads vs a D16 class buffer)
        static int d32 = -1;
        if (d32 < 0) d32 = getenv("LARECOMP_D16_AS_D32") != NULL;
        return (VkFmt){d32 ? VK_FORMAT_D32_SFLOAT : VK_FORMAT_D16_UNORM};
    }
    case 0x33: return (VkFmt){VK_FORMAT_D32_SFLOAT};  // DEPTH24 (as on Metal: no 24-bit depth there)
    case 0x34: return (VkFmt){VK_FORMAT_D32_SFLOAT};
    case 0x35: return (VkFmt){VK_FORMAT_D32_SFLOAT_S8_UINT};
    case 0x36: return (VkFmt){VK_FORMAT_D32_SFLOAT_S8_UINT};
    case 0x37: return (VkFmt){VK_FORMAT_R8G8B8A8_SRGB, 0, 1};
    case 0x38: return (VkFmt){VK_FORMAT_R8G8B8A8_SRGB};
    case 0x39: return (VkFmt){VK_FORMAT_R4G4B4A4_UNORM_PACK16};   // Metal ABGR4Unorm
    case 0x3A: return (VkFmt){VK_FORMAT_R5G6B5_UNORM_PACK16};     // Metal B5G6R5Unorm
    case 0x3B: return (VkFmt){VK_FORMAT_R5G5B5A1_UNORM_PACK16};   // Metal A1BGR5Unorm
    case 0x3C: return (VkFmt){VK_FORMAT_R5G6B5_UNORM_PACK16};
    case 0x3D: return (VkFmt){VK_FORMAT_A2B10G10R10_UNORM_PACK32};
    case 0x3E: return (VkFmt){VK_FORMAT_A2B10G10R10_UINT_PACK32};
    case 0x3F: return (VkFmt){VK_FORMAT_B10G11R11_UFLOAT_PACK32};
    case 0x40: return (VkFmt){VK_FORMAT_E5B9G9R9_UFLOAT_PACK32};
    case 0x41: case 0x42: return (VkFmt){VK_FORMAT_BC1_RGBA_UNORM_BLOCK};
    case 0x43: return (VkFmt){VK_FORMAT_BC2_UNORM_BLOCK};
    case 0x44: return (VkFmt){VK_FORMAT_BC3_UNORM_BLOCK};
    case 0x45: case 0x46: return (VkFmt){VK_FORMAT_BC1_RGBA_SRGB_BLOCK};
    case 0x47: return (VkFmt){VK_FORMAT_BC2_SRGB_BLOCK};
    case 0x48: return (VkFmt){VK_FORMAT_BC3_SRGB_BLOCK};
    case 0x49: return (VkFmt){VK_FORMAT_BC4_UNORM_BLOCK};
    case 0x4A: return (VkFmt){VK_FORMAT_BC4_SNORM_BLOCK};
    case 0x4B: return (VkFmt){VK_FORMAT_BC5_UNORM_BLOCK};
    // 0x4C = signed RGTC2; LARECOMP_BC5_UNORM=1 decodes it unsigned (see metal.m / notes/PLAN.md)
    case 0x4C: {
        static int unorm = -1;   // (getenv per call was a measurable cost: it takes the CRT's environment lock and scans)
        if (unorm < 0) unorm = getenv("LARECOMP_BC5_UNORM") != NULL;
        return (VkFmt){unorm ? VK_FORMAT_BC5_UNORM_BLOCK : VK_FORMAT_BC5_SNORM_BLOCK};
    }
    case 0x4D: return (VkFmt){VK_FORMAT_BC7_UNORM_BLOCK};
    case 0x4E: return (VkFmt){VK_FORMAT_BC7_SRGB_BLOCK};
    case 0x4F: return (VkFmt){VK_FORMAT_BC6H_SFLOAT_BLOCK};
    case 0x50: return (VkFmt){VK_FORMAT_BC6H_UFLOAT_BLOCK};
    case 0x6D: return (VkFmt){VK_FORMAT_R8G8B8A8_UNORM, 0, 1};
    case 0x71: return (VkFmt){VK_FORMAT_R16G16B16A16_SFLOAT, 0, 1};
    case 0x76: return (VkFmt){VK_FORMAT_R32G32B32A32_SFLOAT, 0, 1};
    case 0x79: return (VkFmt){VK_FORMAT_ASTC_4x4_UNORM_BLOCK};
    case 0x7A: return (VkFmt){VK_FORMAT_ASTC_5x4_UNORM_BLOCK};
    case 0x7B: return (VkFmt){VK_FORMAT_ASTC_5x5_UNORM_BLOCK};
    case 0x7C: return (VkFmt){VK_FORMAT_ASTC_6x5_UNORM_BLOCK};
    case 0x7D: return (VkFmt){VK_FORMAT_ASTC_6x6_UNORM_BLOCK};
    case 0x7E: return (VkFmt){VK_FORMAT_ASTC_8x5_UNORM_BLOCK};
    case 0x7F: return (VkFmt){VK_FORMAT_ASTC_8x6_UNORM_BLOCK};
    case 0x80: return (VkFmt){VK_FORMAT_ASTC_8x8_UNORM_BLOCK};
    case 0x81: return (VkFmt){VK_FORMAT_ASTC_10x5_UNORM_BLOCK};
    case 0x82: return (VkFmt){VK_FORMAT_ASTC_10x6_UNORM_BLOCK};
    case 0x83: return (VkFmt){VK_FORMAT_ASTC_10x8_UNORM_BLOCK};
    case 0x84: return (VkFmt){VK_FORMAT_ASTC_10x10_UNORM_BLOCK};
    case 0x85: return (VkFmt){VK_FORMAT_ASTC_12x10_UNORM_BLOCK};
    case 0x86: return (VkFmt){VK_FORMAT_ASTC_12x12_UNORM_BLOCK};
    case 0x87: return (VkFmt){VK_FORMAT_ASTC_4x4_SRGB_BLOCK};
    case 0x88: return (VkFmt){VK_FORMAT_ASTC_5x4_SRGB_BLOCK};
    case 0x89: return (VkFmt){VK_FORMAT_ASTC_5x5_SRGB_BLOCK};
    case 0x8A: return (VkFmt){VK_FORMAT_ASTC_6x5_SRGB_BLOCK};
    case 0x8B: return (VkFmt){VK_FORMAT_ASTC_6x6_SRGB_BLOCK};
    case 0x8C: return (VkFmt){VK_FORMAT_ASTC_8x5_SRGB_BLOCK};
    case 0x8D: return (VkFmt){VK_FORMAT_ASTC_8x6_SRGB_BLOCK};
    case 0x8E: return (VkFmt){VK_FORMAT_ASTC_8x8_SRGB_BLOCK};
    case 0x8F: return (VkFmt){VK_FORMAT_ASTC_10x5_SRGB_BLOCK};
    case 0x90: return (VkFmt){VK_FORMAT_ASTC_10x6_SRGB_BLOCK};
    case 0x91: return (VkFmt){VK_FORMAT_ASTC_10x8_SRGB_BLOCK};
    case 0x92: return (VkFmt){VK_FORMAT_ASTC_10x10_SRGB_BLOCK};
    case 0x93: return (VkFmt){VK_FORMAT_ASTC_12x10_SRGB_BLOCK};
    case 0x94: return (VkFmt){VK_FORMAT_ASTC_12x12_SRGB_BLOCK};
    case 0x95: case 0x96: case 0x97: return (VkFmt){VK_FORMAT_R5G5B5A1_UNORM_PACK16};
    case 0x98: return (VkFmt){VK_FORMAT_B8G8R8A8_UNORM, 0, 1};
    case 0x99: return (VkFmt){VK_FORMAT_B8G8R8A8_UNORM};
    case 0x9A: return (VkFmt){VK_FORMAT_B8G8R8A8_SRGB, 0, 1};
    case 0x9B: return (VkFmt){VK_FORMAT_B8G8R8A8_SRGB};
    default: return (VkFmt){VK_FORMAT_R8G8B8A8_UNORM};
    }
}

bool renderer_format_is_depth(uint32_t f) { return f >= 0x31 && f <= 0x36; }
bool renderer_format_has_stencil(uint32_t f) { return f == 0x31 || f == 0x35 || f == 0x36; }
VkFormat vk_format(uint32_t f) { return fmt_of(f).vf; }
bool vk_format_alpha_one(uint32_t f) { return fmt_of(f).alpha_one; }
bool renderer_format_alpha_one(uint32_t f) { return fmt_of(f).alpha_one; }

static VkImageAspectFlags aspect_of(uint32_t f) {
    if (f == 0x31) return VK_IMAGE_ASPECT_STENCIL_BIT;
    if (f == 0x35 || f == 0x36) return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    if (renderer_format_is_depth(f)) return VK_IMAGE_ASPECT_DEPTH_BIT;
    return VK_IMAGE_ASPECT_COLOR_BIT;
}

static bool format_supports(VkFormat f, VkFormatFeatureFlags want) {
    // (ASTC only with textureCompressionASTC_LDR enabled: a format query can report it while the feature is off)
    if (f >= VK_FORMAT_ASTC_4x4_UNORM_BLOCK && f <= VK_FORMAT_ASTC_12x12_SRGB_BLOCK && !g_vk.astc_ldr) return false;
    VkFormatProperties p;
    vkGetPhysicalDeviceFormatProperties(g_vk.phys, f, &p);
    return (p.optimalTilingFeatures & want) == want;
}

// graphics.compact_textures (ram-diet, RIVAL-034: the rival's astc_bc for GPUs with 4 GB or less). On a GPU without ASTC
// the decoded levels are RGBA8, 4 bytes a texel where TotK's ASTC is 0.25 to 1: the world's textures are then most of the
// VRAM, and a 4 GB card pages. LARECOMP_ASTC_BC3=1 keeps them as BC3 instead (1 byte a texel:../bc3enc.h on the CPU,
// bc3_encode.comp on the GPU), which is lossy (fine colour detail softens, smooth gradients stay), so it is never on by
// default where the room is there. Unset (the row's Automatic): on for a discrete GPU whose VRAM (largest device-local
// heap) is 4.5 GB or less; an integrated GPU has no VRAM of its own (its heap is the shared RAM or a BIOS carve-out,
// which says nothing of the room) and keeps RGBA8. 0: off. Decided once, at the first ASTC image (render thread), and
// only with the GPU encoder ready: without it every level would encode on the CPU (~90 ms a 2048x2048 one), so RGBA8
// stays.
static bool ag_on(void);
static bool bc_ready(void);
static bool astc_bc3_on(void) {
    static int on = -1;
    if (on >= 0) return on;
    const char *e = getenv("LARECOMP_ASTC_BC3");
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(g_vk.phys, &mp);
    uint64_t heap = 0;
    for (uint32_t i = 0; i < mp.memoryHeapCount; i++)
        if ((mp.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) && mp.memoryHeaps[i].size > heap) heap = mp.memoryHeaps[i].size;
    bool discrete = g_vk.props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
    bool want = e && *e ? *e == '1' : discrete && heap <= (4608ull << 20);
    VkFormatFeatureFlags need = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    bool fmt = want && format_supports(VK_FORMAT_BC3_UNORM_BLOCK, need) && format_supports(VK_FORMAT_BC3_SRGB_BLOCK, need);
    on = fmt && ag_on() && bc_ready();
    LOG("[tex] compact textures (graphics.compact_textures, LARECOMP_ASTC_BC3=%s): %s (%s GPU, %.1f GB VRAM)\n", e && *e ? e : "auto",
        on ? "ASTC levels kept as BC3, a quarter of RGBA8" : !want ? "off, ASTC levels decoded to RGBA8"
        : !fmt ? "wanted, but this GPU cannot sample BC3: RGBA8" : "wanted, but no GPU decode / encode (LARECOMP_ASTC_GPU=0 or a failed pipeline): RGBA8",
        discrete ? "a discrete" : "an integrated / other", heap / 1073741824.0);
    return on;
}
// totk_hooks.c (graphics.render_scale "auto": the 3D scene at the window's resolution): a GPU that keeps the old,
// smaller pick (the window height / 1080 of the game's 900 lines, x0.83): an integrated one, or a discrete one under
// 8 GB (the first-run screen's line between High and Ultra, pc_overlay.cpp recommend_preset). Decided once, at the
// first present (the device exists); LARECOMP_AUTO_WINDOW=1 the window anyway, 0 the old pick on any GPU (A/B arms).
int renderer_auto_scale_small(void) {
    static int small = -1;
    if (small >= 0) return small;
    if (!g_vk.phys) return 0;   // (totk_menu_cursor.cpp asks too, on a game thread: not before the device)
    const char *e = getenv("LARECOMP_AUTO_WINDOW");
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(g_vk.phys, &mp);
    uint64_t heap = 0;
    for (uint32_t i = 0; i < mp.memoryHeapCount; i++)
        if ((mp.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) && mp.memoryHeaps[i].size > heap) heap = mp.memoryHeaps[i].size;
    const bool discrete = g_vk.props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
    const bool gpu = !discrete || heap < (7680ull << 20);   // (an "8 GB" card reports a little under 8192 MiB)
    small = e && *e ? *e == '0' : gpu;
    LOG("[render] auto render resolution (LARECOMP_AUTO_WINDOW=%s): %s (%s GPU, %.1f GB VRAM)\n", e && *e ? e : "unset",
        small ? "the window height / 1080 of the game's 900 lines (x0.83 of the window)" : "the window's resolution",
        discrete ? "a discrete" : "an integrated / other", heap / 1073741824.0);
    return small;
}

// ================================================================== output resolution / per-frame clears
// Same rules as metal.m (see the comments there).
static float g_rs_x = 1, g_rs_y = 1;
void renderer_get_scale(float *sx, float *sy) { *sx = g_rs_x; *sy = g_rs_y; }
// vk_render_epoch (the dlss worker's SR features): moves on, monotonically, each time the render scale really changes, and
// only after renderer_get_scale returns the new one; also when graphics.native_ui changes the canvas (renderer_out_scale)
static uint32_t g_render_epoch;
// The largest render scale the scene's targets allow: within the GPU's largest 2D image (32:9's 5120 at x4 = 20480 is past
// the usual 16384: image creation would fail and end the run). totk_hooks.c output_scale clamps to it before its
// [render] line; renderer_set_scale again, the one every scaled target follows.
float renderer_scale_limit(void) {
    extern void output_scene_size(unsigned *w, unsigned *h);
    unsigned w, h;
    output_scene_size(&w, &h);
    const float lim = (float)g_vk.props.limits.maxImageDimension2D;
    return lim > 0 ? fminf(lim / (float)w, lim / (float)h) : 4.0f;
}
void renderer_set_scale(float sx, float sy) {
    {   // (clamped to renderer_scale_limit; a scaled scene above ~16 Mpx a target is said once: VRAM)
        extern void output_scene_size(unsigned *w, unsigned *h);
        unsigned w, h;
        output_scene_size(&w, &h);
        const float m = renderer_scale_limit();
        static float said_clamp = -1, said_big = -1;
        if (sx > m || sy > m) {
            if (said_clamp != sx) LOG("[render] scale %.3f clamped to %.3f: the scene %ux%u would pass the GPU's largest image (%u)\n",
                                      sx, m, w, h, g_vk.props.limits.maxImageDimension2D);
            said_clamp = sx;
            sx = sx > m ? m : sx, sy = sy > m ? m : sy;
        }
        const double px = (double)w * sx * h * sy;
        if (px > 16.0e6 && said_big != sx) {
            LOG("[render] scale %.3f: the scene's targets are %.0fx%.0f (%.1f Mpx each): expect a large VRAM use\n", sx,
                w * sx, h * sy, px / 1e6);
            said_big = sx;
        }
    }
    if (sx == g_rs_x && sy == g_rs_y) return;
    g_rs_x = sx; g_rs_y = sy;
    __atomic_add_fetch(&g_render_epoch, 1, __ATOMIC_RELEASE);
}
uint32_t vk_render_epoch(void) { return __atomic_load_n(&g_render_epoch, __ATOMIC_ACQUIRE); }
uint64_t renderer_present_count(void);

// Output canvas (TOTK_OUT_SCALE, 2026-10-17): TotK's 1920x1080 targets (the canvas its upscale pass writes and the UI draws on,
// the window textures, the menus' layers and 3D model buffers) at the window's resolution (device.c: viewport height / 1080), so
// the scene reaches the window in one resample (exec.c, the upscale pass) and the UI draws at full size. On by default
// 2026-10-27.. 11-03; off by default again since 2026-11-03. TOTK_OUT_SCALE=1 / the Options row graphics.native_ui = On: the
// canvas at the window's resolution; unset / 0: the old stretched canvas. Active only when the window's 16:9 picture is taller
// than 1080 lines when first asked (the game's first 1920x1080 target or the upscale program, after the start's window mode is
// applied): at 1080 or less the canvas is the game's own size anyway, and every target and the upscale pass stay exactly as they
// were. Decided once, like the textures' sizes: a window made larger later keeps the start's choice until a restart, or until
// the row changes: graphics.native_ui is live (renderer_set_native_ui, applied at the next present: renderer_set_out_scale), the
// canvas targets remade between two frames with their contents (tex_live_check), the upscale program in the form a start would
// give it (exec.c knobs).
static float g_out_s = 1;
static int g_out_on = -1;   // TOTK_OUT_SCALE, then the live row (render thread)
static int out_scale_on(void) {
    // (on by default again since #62's CPU-filled canvas textures are kept at the game's size, cpu1x_demote: off, the
    // 3D scene's upscale and DLSS / DLAA ended at 1080p above it, stretched to the window)
    if (g_out_on < 0) { const char *e = getenv("TOTK_OUT_SCALE"); g_out_on = e && *e ? strcmp(e, "0") != 0 : 1; }
    return g_out_on;
}
// shim_win.c / shim_sdl3.c: the monitor's in fullscreen, else the client; false until the start's window mode is applied
// (waiting up to wait_ms; a borderless start reads as the blank 1280 x 720 window before that)
bool shim_window_target_size(int *w, int *h, int wait_ms);
VkExtent2D vk_frame_extent(void);   // device.c: the swapchain's size
static int g_out_state;             // 0 undecided, 3 being decided, 1 off, 2 on: published (release) after g_out_s
// Once, on whichever thread asks first: the game's first 1920x1080 target, a pipeline worker translating the upscale
// program (the start-up precompile included), or the first present
static void out_scale_decide(void) {
    int a = 0, w = 0, h = 0;
    const char *how = "the window", *off = "off (graphics.native_ui Off / TOTK_OUT_SCALE=0)", *wide = getenv("TOTK_OUTPUT_RES");
    uint64_t t0 = SDL_GetTicksNS();
    if (wide && *wide && strcmp(wide, "0")) off = "off, TOTK_OUTPUT_RES's canvas";
    else if (out_scale_on()) {
        // (not the swapchain first: it can keep the windowed size for a few frames after the switch to borderless)
        if (!shim_window_target_size(&w, &h, 3000)) {
            VkExtent2D e = vk_frame_extent();
            w = (int)e.width, h = (int)e.height, how = "the swapchain: the start's window mode not applied after 3 s";
        }
        double vh = w * 9.0 / 16.0 < h ? w * 9.0 / 16.0 : h;
        a = vh > 1080.5;
        off = "off, the picture is 1080 lines or less";
        // the scale too, once (device.c's viewport height / 1080 at the time): a canvas made while the window still had
        // its windowed size would otherwise disagree with the scale its draws get later
        if (a) g_out_s = (float)(vh / 1080.0 > 4.0 ? 4.0 : vh / 1080.0);
    }
    __atomic_store_n(&g_out_state, a ? 2 : 1, __ATOMIC_RELEASE);
    LOG("[tex] output canvas at the window's resolution (TOTK_OUT_SCALE): %s (%s %dx%d, x%.3f; decided in %.1f ms)\n",
        a ? "on" : off, how, w, h, g_out_s, (SDL_GetTicksNS() - t0) / 1e6);
}
int renderer_out_scale_active(void) {   // (exec.c: the upscale pass resamples to the canvas when it is)
    int s = __atomic_load_n(&g_out_state, __ATOMIC_ACQUIRE);
    if (s == 1 || s == 2) return s == 2;
    // (a once: the first caller decides, the others wait for it; no pthread_once in the runtime's pthread subset)
    int expect = 0;
    if (__atomic_compare_exchange_n(&g_out_state, &expect, 3, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) out_scale_decide();
    while ((s = __atomic_load_n(&g_out_state, __ATOMIC_ACQUIRE)) == 3) SDL_Delay(1);
    return s == 2;
}
// ngx.c (DLSS features made ahead): the canvas scale, 1 when off
float renderer_out_scale(void) { return renderer_out_scale_active() ? g_out_s : 1.0f; }
// Live settings that change how targets are made (graphics.native_ui and graphics.shadow_scale here,
// graphics.contact_shadows' STORAGE usage in contact.c): g_tex_gen moves on at a present, and each image is checked
// against the new state at its next use (tex_live_check, tex_backend). The recycle pool is emptied then: its images
// were made for the old state.
static uint32_t g_tex_gen;
static int g_tex_bump;      // (any thread: renderer_tex_remake) a remake check from the next present
static int g_out_req = -1;  // (settings thread) graphics.native_ui's new value: -1 none, 0 off, 1 on
static void recycle_flush(void);
static void samplers_tick(void);
static float g_shadow_s;   // (graphics.shadow_scale, below)
static int g_shadow_req;
static float shadow_scale(void);
static float shadow_vram_cap(float s);
// (exec.c, the drawing thread's knobs: the decision if made, never waiting for it: off until then)
int renderer_out_scale_peek(void) { return __atomic_load_n(&g_out_state, __ATOMIC_ACQUIRE) == 2; }
void renderer_tex_remake(void) { __atomic_store_n(&g_tex_bump, 1, __ATOMIC_RELEASE); }
void renderer_set_native_ui(int on) { __atomic_store_n(&g_out_req, on ? 1 : 0, __ATOMIC_RELEASE); }
// device.c, every present (render thread): the decision by the first one at the latest; then the live row's change,
// decided as out_scale_decide does at a start, in this window (s: its 16:9 picture height / 1080, clamped to 1 .. 4)
void renderer_set_out_scale(float s) {
    const int was = renderer_out_scale_active();
    const int req = __atomic_exchange_n(&g_out_req, -1, __ATOMIC_ACQ_REL);
    if (req >= 0) {
        out_scale_on();
        g_out_on = req;
        const char *wide = getenv("TOTK_OUTPUT_RES");
        const int a = req && !(wide && *wide && strcmp(wide, "0")) && s > 1.0005f;   // (vh > 1080.5, as at a start)
        if (a) g_out_s = s > 4.0f ? 4.0f : s;
        __atomic_store_n(&g_out_state, a ? 2 : 1, __ATOMIC_RELEASE);
        LOG("[tex] output canvas at the window's resolution (graphics.native_ui, live): %s (x%.3f)\n", a ? "on" : "off", g_out_s);
        if (a || was) {
            __atomic_store_n(&g_tex_bump, 1, __ATOMIC_RELEASE);
            __atomic_add_fetch(&g_render_epoch, 1, __ATOMIC_RELEASE);   // (vk_render_epoch: DLSS's output, the canvas, too)
        }
    }
    {
        const float sr0 = __atomic_exchange_n(&g_shadow_req, 0, __ATOMIC_ACQ_REL) / 1000.0f;
        const float sr = sr0 > 0 ? shadow_vram_cap(sr0) : 0;   // (here, on the render thread: the device is known)
        if (sr > 0 && sr != shadow_scale()) {
            LOG("[tex] shadow maps at x%.0f (graphics.shadow_scale, live; was x%.0f)\n", sr, g_shadow_s);
            g_shadow_s = sr;
            __atomic_store_n(&g_tex_bump, 1, __ATOMIC_RELEASE);   // (maps at 1 are no scaled kind: tex_live_check)
        }
    }
    if (__atomic_exchange_n(&g_tex_bump, 0, __ATOMIC_ACQ_REL)) {
        g_tex_gen++;
        recycle_flush();
    }
    samplers_tick();
}

// Shadow maps: square depth render targets (TotK: the 800x800 x4 sun cascades, D32) rendered at TOTK_SHADOW_SCALE x their
// size (default 2; 1 = the game's own). The user: "theres still alot of shimmering on the shadows". Shaders see the guest
// size through the rscale table like every scaled target (translate.c). Live (graphics.shadow_scale,
// renderer_set_shadow_scale): applied at the next present like the render scale, each shadow map remade at its next use
// with its contents (rescale_image; maps at 1 are no scaled kind: tex_live_check).
static float g_shadow_s = -1;   // (render thread)
static int g_shadow_req;        // (settings thread) the new scale x 1000; 0: none
// A scene that is not 16:9 (graphics.aspect_ratio's 21:9 / 32:9 / 16:10) keeps the shadow maps at x2 at most, and the game
// patches' shadow distance at x2 (totk_engine.c), while 32:9 at the defaults looked right. TOTK_WIDE_SHADOW_CAP=0: no cap.
static float shadow_wide_cap(float s) {
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("TOTK_WIDE_SHADOW_CAP");
        extern void output_scene_size(unsigned *w, unsigned *h);
        unsigned w, h;
        output_scene_size(&w, &h);
        const float a = (float)w / (float)h;
        on = !(e && *e == '0') && (a < 1.74f || a > 1.81f);
    }
    if (!on || s <= 2.0f) return s;
    static int told;
    if (!told++) LOG("[tex] shadow maps capped at x2 (asked x%g): the scene is not 16:9 (TOTK_WIDE_SHADOW_CAP=0: no cap)\n", s);
    return 2.0f;
}
// A shadow scale the card's VRAM can hold (D#83b, GTX 1650 4 GB, x4: the sun's 2880 x 2880 x 2-layer map, 2 levels, became
// one 11520 x 11520 target of 677 MB; usage reached the 3683 MB budget and the NVIDIA driver faulted inside itself at
// every start). That map at x s is ~42 MB x s^2 (D16); it may take a sixteenth of the largest device-local heap at most
// (the other shadow maps and the recycle pool take as much again), so a 4 GB card gets x2, 6-8 GB x3, 12 GB and up x4 (the
// scale the next whole step down; never below 1). Residency can't relieve it: the map is drawn every frame. Read once (the
// heap's size, not the moving budget, so the scale doesn't change under the player). An integrated GPU's heap is shared
// RAM: no cap. TOTK_SHADOW_VRAM_CAP=0: no cap.
static float shadow_vram_cap(float s) {
    static float max_s = 0;
    if (max_s == 0 && !g_vk.phys) return s;   // (before the device: decided at its first use on the render thread)
    if (max_s == 0) {
        const char *e = getenv("TOTK_SHADOW_VRAM_CAP");
        max_s = 4.0f;
        if (!(e && *e == '0') && g_vk.phys && g_vk.props.deviceType != VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) {
            VkPhysicalDeviceMemoryProperties mp;
            vkGetPhysicalDeviceMemoryProperties(g_vk.phys, &mp);
            uint64_t heap = 0;
            for (uint32_t i = 0; i < mp.memoryHeapCount; i++)
                if ((mp.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) && mp.memoryHeaps[i].size > heap) heap = mp.memoryHeaps[i].size;
            const double map1 = 2880.0 * 2880.0 * 2.0 * 2.0 * 1.25;   // (bytes at x1: D16, 2 layers, 2 levels; 664 MB at x4)
            const double m = heap ? floor(sqrt((double)heap / 16.0 / map1)) : 4.0;
            max_s = m < 1.0 ? 1.0f : m > 4.0 ? 4.0f : (float)m;
        }
    }
    if (s <= max_s) return s;
    static float told;
    if (told != s) {
        told = s;
        LOG("[tex] shadow maps capped at x%g (asked x%g): the card's VRAM can't also hold the sun's shadow map at x%g "
            "(~%.0f MB; a sixteenth of its memory at most; TOTK_SHADOW_VRAM_CAP=0: no cap)\n", (double)max_s, (double)s,
            (double)s, 2880.0 * 2880.0 * 2.0 * 2.0 * 1.25 * s * s / 1048576.0);
    }
    return max_s;
}
static float shadow_parse(const char *e) {
    const float s = e && *e ? (float)atof(e) : 2.0f;
    return shadow_wide_cap(s >= 1.0f && s <= 4.0f ? s : 1.0f);
}
static float shadow_scale(void) {
    if (g_shadow_s < 0 && !g_vk.phys) return shadow_parse(getenv("TOTK_SHADOW_SCALE"));   // (no device yet: not latched)
    if (g_shadow_s < 0) {
        g_shadow_s = shadow_vram_cap(shadow_parse(getenv("TOTK_SHADOW_SCALE")));
        LOG("[tex] shadow maps at x%g (graphics.shadow_scale / TOTK_SHADOW_SCALE)\n", g_shadow_s);   // (public #35: the log said only the game patch's)
    }
    return g_shadow_s;
}
void renderer_set_shadow_scale(const char *v) { __atomic_store_n(&g_shadow_req, (int)lroundf(shadow_parse(v) * 1000.0f), __ATOMIC_RELEASE); }
// Shadow rungs (camera-turn hitches, P0): the game sizes the sun's shadow map with the view (graphics.shadow_fixed: "the
// game's changes its size with the view"), so turning the camera finalizes it and makes it again at a new size: 1856.. A
// shadow map's host size is its scaled size rounded up to a rung, SHADOW_RUNGS a doubling (4: 1280, 1536, 1792, 2048,
// 2560...; powers of two exact), and the pool matches the rung, so the view's small steps reuse one image. Its scale is
// then its own ratio (b->sx: rung / guest, as every mapping since 1d0fa529 takes the allocation's own ratio): the map
// renders up to one rung finer than asked (at 4 rungs at most 1.25x the side just past a rung, 1.0x on one), never
// coarser: no visual loss, some shadow-pass GPU and memory for no re-creation. The cost: at the players' default x2
// every player has rungs, up to +56 % shadow-map pixels just past a rung (~+20 % on average) every frame; the release
// gate compares fps against LARECOMP_SHADOW_RUNGS=0 on the camera-turn route, and 8 a doubling (+27 % at most) is the
// fallback if it costs. LARECOMP_SHADOW_RUNGS=<1|2|4|8> a doubling, 0 = the exact size (old). Shadow maps at x1 are no
// scaled kind and keep their exact size unless LARECOMP_SHADOW_RUNGS1=1 makes them kind 2 at x1 with rungs.
static int shadow_rungs(void) {
    static int n = -1;
    if (n < 0) {
        const char *e = getenv("LARECOMP_SHADOW_RUNGS");
        const int v = e && *e ? atoi(e) : 4;
        n = v == 1 || v == 2 || v == 4 || v == 8 ? v : 0;
    }
    return n;
}
static bool shadow_rungs_at1(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("LARECOMP_SHADOW_RUNGS1"); on = shadow_rungs() && e && *e == '1'; }
    return on;
}
static uint32_t shadow_rung(uint32_t n) {
    const int k = shadow_rungs();
    if (!k || n <= 256) return n;
    uint32_t o = 1;
    while (o * 2 < n) o *= 2;   // (o < n <= 2o)
    const uint32_t step = o / (uint32_t)k, r = (n + step - 1) / step * step;
    return r > g_vk.props.limits.maxImageDimension2D ? n : r;
}
// Promoted targets: TotK renders some non-compressible targets in one pass with a scaled one of the same size (fs dd9edc34:
// an R8 1600x900 mask with the scene's depth buffer, every frame; fs 7e405a3d: R11G11B10F + R8 800x450 while loading).
// begin_pass renders the smallest attachment extent, so at TOTK_RES_SCALE=2 the pass used a quarter of the scaled attachment.
// exec.c begin_pass promotes the unscaled one (storage address + size) to the scene's scale from its next backend (TotK
// re-creates its targets every frame). LARECOMP_NO_PROMOTE=1: off. The shadow-map kind too (2026-10-30: a square 512x512 D32
// classed as a shadow map, at TOTK_SHADOW_SCALE 4 = 2048x2048, drawn with an unscaled R8 512x512 colour target: fs c509b81e /
// 6c4e6d40 / 915cd25e / 123a608c rendered at 1.0 into the top-left sixteenth of the depth, testing against the wrong texels):
// the colour target takes the depth's kind. LARECOMP_PROMO_SHADOW=0: scene scale only (old). The promotion and depth-grow
// tables (below): an entry per storage address + guest size + format, written by exec.c begin_pass on the render thread, read
// where images are made. A full table reuses its least recently matched entry (P1.4: they kept their first 64 entries for
// good and dropped later ones silently, so every later pair stayed mixed each frame; the gate's boots made at most 3). A
// rewritten entry goes through an odd version, so a reader never takes half of one.
#define KT_MAX 256
typedef struct { uint32_t ver, w, h, fmt, a, b; uint64_t addr, used; } KtEnt;
typedef struct { KtEnt e[KT_MAX]; int n; const char *what; } KTab;
static bool kt_get(KTab *k, const NvnTexture *t, uint32_t *a, uint32_t *b) {
    const int n = __atomic_load_n(&k->n, __ATOMIC_ACQUIRE);
    for (int i = 0; i < n; i++) {
        KtEnt *e = &k->e[i];
        if (__atomic_load_n(&e->addr, __ATOMIC_RELAXED) != t->addr) continue;
        const uint32_t v = __atomic_load_n(&e->ver, __ATOMIC_ACQUIRE);
        const uint64_t ad = __atomic_load_n(&e->addr, __ATOMIC_RELAXED);
        const uint32_t w = __atomic_load_n(&e->w, __ATOMIC_RELAXED), h = __atomic_load_n(&e->h, __ATOMIC_RELAXED);
        const uint32_t f = __atomic_load_n(&e->fmt, __ATOMIC_RELAXED), ea = __atomic_load_n(&e->a, __ATOMIC_RELAXED);
        const uint32_t eb = __atomic_load_n(&e->b, __ATOMIC_RELAXED);
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if ((v & 1) || __atomic_load_n(&e->ver, __ATOMIC_RELAXED) != v || ad != t->addr || w != t->width || h != t->height ||
            f != t->format)
            continue;
        __atomic_store_n(&e->used, vk_frame_counter(), __ATOMIC_RELAXED);
        if (a) *a = ea;
        if (b) *b = eb;
        return true;
    }
    return false;
}
static void kt_write(KtEnt *e, const NvnTexture *t, uint32_t a, uint32_t b) {   // (the render thread only)
    const uint32_t v = e->ver;
    __atomic_store_n(&e->ver, v + 1, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    __atomic_store_n(&e->addr, t->addr, __ATOMIC_RELAXED);
    __atomic_store_n(&e->w, t->width, __ATOMIC_RELAXED);
    __atomic_store_n(&e->h, t->height, __ATOMIC_RELAXED);
    __atomic_store_n(&e->fmt, t->format, __ATOMIC_RELAXED);
    __atomic_store_n(&e->a, a, __ATOMIC_RELAXED);
    __atomic_store_n(&e->b, b, __ATOMIC_RELAXED);
    __atomic_store_n(&e->used, vk_frame_counter(), __ATOMIC_RELAXED);
    __atomic_store_n(&e->ver, v + 2, __ATOMIC_RELEASE);
}
static void kt_put(KTab *k, const NvnTexture *t, uint32_t a, uint32_t b) {   // (t's own entry updated, else a new one)
    for (int i = 0; i < k->n; i++)
        if (k->e[i].addr == t->addr && k->e[i].w == t->width && k->e[i].h == t->height && k->e[i].fmt == t->format) {
            kt_write(&k->e[i], t, a, b);
            return;
        }
    if (k->n < KT_MAX) {
        kt_write(&k->e[k->n], t, a, b);
        __atomic_store_n(&k->n, k->n + 1, __ATOMIC_RELEASE);
        return;
    }
    int lru = 0;
    for (int i = 1; i < KT_MAX; i++)
        if (__atomic_load_n(&k->e[i].used, __ATOMIC_RELAXED) < __atomic_load_n(&k->e[lru].used, __ATOMIC_RELAXED)) lru = i;
    static int nlog;
    if (nlog++ < 4) LOG("[tex] %s table full (%d entries): its least recently matched entry is reused\n", k->what, KT_MAX);
    kt_write(&k->e[lru], t, a, b);
}
static KTab g_promo = {.what = "promotion"};
static int tex_promoted(const NvnTexture *t) {   // (the kind it was promoted to, 0 = not promoted)
    uint32_t kind;
    return kt_get(&g_promo, t, &kind, NULL) ? (int)kind : 0;
}
void renderer_texture_promote_kind(NvnTexture *t, int kind) {
    static int off = -1;
    if (off < 0) off = getenv("LARECOMP_NO_PROMOTE") != NULL;
    if (off || !t || !t->addr || t->samples > 1 || t->target != 1 || tex_promoted(t)) return;
    kt_put(&g_promo, t, (uint32_t)kind, 0);
    LOG("[tex] promoted to the %s scale (rendered next to a scaled target of its size): addr %llx %ux%u f%#x flags %#x\n",
        kind == 2 ? "shadow maps'" : "scene", (unsigned long long)t->addr, t->width, t->height, t->format, t->flags);
}
void renderer_texture_promote(NvnTexture *t) { renderer_texture_promote_kind(t, 1); }
static void destroy_image(TexBackend *b);
static void create_image(NvnTexture *t, TexBackend *b);
// A remade image's old contents, scaled into the new one by a blit on cb (filtered colour, nearest depth / integer):
// rescale_image's, and a promoted target's (since P1.4 promoted in the pass that first meets its scaled partner, it may
// load there what it rendered unscaled earlier in the frame). Images a blit cannot carry (multisampled, block-compressed
// shadows, depth / colour alternates, 3D, formats without blit support) start blank. LARECOMP_RESCALE_CARRY=0: never.
typedef struct { VkImage img; VkFormat fmt; VkImageAspectFlags aspect; uint32_t w, h, levels, layers; bool ok; } Carry;
static Carry carry_take(const TexBackend *b) {   // (before destroy_image, which frees the old image after this submission)
    static int carry_on = -1;
    if (carry_on < 0) { const char *e = getenv("LARECOMP_RESCALE_CARRY"); carry_on = !e || *e != '0'; }
    return (Carry){b->image, b->format, b->aspect, b->width, b->height, b->levels, b->layers,
                   carry_on && b->image && b->samples <= 1 && !b->blk && !b->alt.image && b->depth <= 1 &&
                       format_supports(b->format, VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT)};
}
static bool carry_blit(VkCommandBuffer cb, const Carry *c, const TexBackend *b, bool *linear) {
    if (!c->ok || !b->image || b->format != c->fmt || b->aspect != c->aspect || b->samples > 1) return false;
    const bool depth = (c->aspect & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) != 0;
    *linear = !depth && format_supports(c->fmt, VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) &&
              (c->w != b->width || c->h != b->height);
    const uint32_t nl = c->levels < b->levels ? c->levels : b->levels, nly = c->layers < b->layers ? c->layers : b->layers;
    VkImageBlit bl[16];
    uint32_t n = 0;
    for (uint32_t lv = 0; lv < nl && n < 16; lv++) {
        const int32_t sw = (int32_t)(c->w >> lv ? c->w >> lv : 1), sh = (int32_t)(c->h >> lv ? c->h >> lv : 1);
        const int32_t dw = (int32_t)(b->width >> lv ? b->width >> lv : 1), dh = (int32_t)(b->height >> lv ? b->height >> lv : 1);
        bl[n++] = (VkImageBlit){.srcSubresource = {c->aspect, lv, 0, nly}, .srcOffsets = {{0, 0, 0}, {sw, sh, 1}},
                                .dstSubresource = {c->aspect, lv, 0, nly}, .dstOffsets = {{0, 0, 0}, {dw, dh, 1}}};
    }
    vk_full_barrier(cb);   // (the old image's last writes; the new image's start-up clear)
    vkCmdBlitImage(cb, c->img, VK_IMAGE_LAYOUT_GENERAL, b->image, VK_IMAGE_LAYOUT_GENERAL, n, bl, *linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
    vk_full_barrier(cb);
    return true;
}
// An unscaled backend remade at kind's scale, its contents carried (carry_blit, in order on the frame's command buffer),
// else cleared to zero; true if carried
static bool remake_scaled(NvnTexture *t, TexBackend *b, int kind, bool *linear) {
    extern uint32_t vk_view_gen;
    const Carry c = carry_take(b);
    destroy_image(b);
    b->scale_kind = (uint8_t)kind;
    b->scalable = true;
    create_image(t, b);
    vk_view_gen++;   // (exec.c's per-frame view memo is keyed by the texture object)
    VkCommandBuffer cb = vk_cmd();
    const bool carried = carry_blit(cb, &c, b, linear);
    if (!carried) {
        vk_full_barrier(cb);
        VkImageSubresourceRange rg = {b->aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
        if (b->aspect & VK_IMAGE_ASPECT_COLOR_BIT) {
            VkClearColorValue z = {{0}};
            vkCmdClearColorImage(cb, b->image, VK_IMAGE_LAYOUT_GENERAL, &z, 1, &rg);
        } else {
            VkClearDepthStencilValue z = {0};
            vkCmdClearDepthStencilImage(cb, b->image, VK_IMAGE_LAYOUT_GENERAL, &z, 1, &rg);
        }
        vk_full_barrier(cb);
    }
    return carried;
}

// CPU-filled textures at the game's size. A texture of a scaled shape (the output canvas's 1920x1080, kind 3; a
// scene-shaped 2D target, kind 1) whose first contents come from guest memory, not from a draw, got the guest texels in
// the top-left of its scaled image only (upload_texture_'s 'fills its top-left' line), so every sample read them
// stretched by the scale, the rest blank. The reporter's 2560x1440 log (x1.333): R8 1920x1080 textures (flags 0) filled
// from the CPU next to the text strings' own; text broke on entering an area, and Native UI Off (no kind 3: those are
// unscaled then) fixed it. Such a texture is made at the game's size instead (vk_tex, before its first upload), so it is
// sampled exactly as the game wrote it. If a pass then renders into it, it goes back to its scale (contents carried) and
// stays there (exec.c begin_pass, renderer_texture_apply_promotion). Keyed as the promotion table (storage address +
// guest size + format): TotK re-creates its objects every frame. The canvas only by default (the evidence is there; a
// scene-shaped target sampled before it is rendered, an alias miss, would cost two remakes per storage address);
// LARECOMP_CPU_FILL_1X=1: the render scale's targets too, =0: off (old).
static KTab g_cpu1x = {.what = "CPU-filled"};   // (a = 1: at the game's size; b = 1: rendered since, scaled for good)
static int tex_scale_kind(const NvnTexture *t);
static int cpu1x_mode(void) {   // 0 off, 3 the canvas (kind 3), 1 kinds 3 and 1
    static int m = -1;
    if (m < 0) { const char *e = getenv("LARECOMP_CPU_FILL_1X"); m = !e || !*e ? 3 : !strcmp(e, "0") ? 0 : !strcmp(e, "1") ? 1 : 3; }
    return m;
}
static bool tex_cpu1x(const NvnTexture *t) {
    uint32_t a;
    return __atomic_load_n(&g_cpu1x.n, __ATOMIC_RELAXED) && kt_get(&g_cpu1x, t, &a, NULL) && a;
}
// (vk_tex, the render thread) b about to take its first contents from guest memory
static void cpu1x_demote(NvnTexture *t, TexBackend *b) {
    const int m = cpu1x_mode();
    if (!m || b->uploaded || !b->scalable || (b->scale_kind != 3 && (m == 3 || b->scale_kind != 1)) || t->target != 1 ||
        !b->image || b->samples > 1 || b->blk || b->pending_rgba || !(b->aspect & VK_IMAGE_ASPECT_COLOR_BIT))
        return;
    uint32_t a, rendered = 0;
    if (kt_get(&g_cpu1x, t, &a, &rendered) && rendered) return;
    kt_put(&g_cpu1x, t, 1, 0);
    extern uint32_t vk_view_gen;
    const int kind = b->scale_kind;
    const uint32_t ow = b->width, oh = b->height;
    destroy_image(b);   // (nothing to carry: never rendered, copied or cleared into, never uploaded)
    b->scale_kind = 0;
    b->scalable = false;
    create_image(t, b);
    vk_view_gen++;
    static int nlog;
    if (nlog++ < 16)
        LOG("[tex] filled from the CPU before any draw: made at the game's size (%ux%u, not the %s's %ux%u): addr %llx f%#x flags %#x\n",
            t->width, t->height, kind == 3 ? "output canvas" : "render scale", ow, oh, (unsigned long long)t->addr, t->format, t->flags);
}
// exec.c copy_buffer_to_texture, before the copy marks it: a never-rendered texture taking its first contents from a
// buffer copy is one filled from guest memory too (b->uploaded stays false for a copy, so the test is gpu_owned there):
// the same rule, made at the game's size (#62 follow-up, ram-diet / hdr)
void renderer_texture_buf_first(NvnTexture *t) {
    TexBackend *b = t ? (TexBackend *)t->backend : NULL;
    if (b && !b->gpu_owned) cpu1x_demote(t, b);
}
// (begin_pass) a CPU-filled texture at the game's size now rendered into: back to its scale
static bool cpu1x_back(NvnTexture *t, TexBackend *b) {
    if (!tex_cpu1x(t)) return false;
    kt_put(&g_cpu1x, t, 0, 1);
    const int k = tex_scale_kind(t);
    if (!k || !b->image || b->samples > 1 || b->blk || b->pending_rgba) return false;   // (its next backend takes its kind)
    bool linear = false;
    const bool carried = remake_scaled(t, b, k, &linear);
    static int nlog;
    if (nlog++ < 16)
        LOG("[tex] CPU-filled texture now rendered into: back to its scale, %ux%u -> %ux%u f%#x (addr %llx), %s\n", t->width, t->height,
            b->width, b->height, t->format, (unsigned long long)t->addr,
            carried ? (linear ? "contents carried (filtered)" : "contents carried (nearest)") : "cleared");
    return true;
}

// (exec.c begin_pass, outside a render pass) a promoted target whose object kept its unscaled backend is reallocated
// at the scene's scale, its contents carried (carry_blit, in order on the frame's command buffer), else cleared to
// zero
bool renderer_texture_apply_promotion(NvnTexture *t) {
    TexBackend *b = t ? (TexBackend *)t->backend : NULL;
    if (b && !b->scalable && cpu1x_back(t, b)) return true;
    if (!b || b->scalable || !g_promo.n || !b->image || b->samples > 1 || b->blk || b->pending_rgba || !tex_promoted(t)) return false;
    bool linear = false;
    const bool carried = remake_scaled(t, b, tex_promoted(t), &linear);
    static int nlog;
    if (nlog++ < 32)
        LOG("[tex] promoted target reallocated: %p addr %llx %ux%u f%#x -> %ux%u, %s\n", (void *)t, (unsigned long long)t->addr, t->width,
            t->height, t->format, b->width, b->height, carried ? (linear ? "contents carried (filtered)" : "contents carried (nearest)") : "cleared");
    return true;
}

// A scaled depth drawn with a colour target's level of the same guest size but a larger host extent (2026-10-31: fs 6b2da6dd
// draws level 1 of the 400x225 colour target with a standalone 200x112 depth; on the host that level is floor(450 / 2) = 225
// rows, the depth 2 x 112 = 224, so the render area cut the level's last row, never written in that pass, and the bloom
// chain pulled the stale row in as a cyan band at the bottom; at 1.333 the same pass was the ALLOC_CEIL case). exec.c
// begin_pass records the colour level's host extent here; the depth is then allocated at least that large (its own ratio
// follows the allocation, as every scaled image's). LARECOMP_DEPTH_GROW=0: off. The extent is kept in guest units (the host
// extent over the scale it was seen at) and turned back at the scale of the moment: kept in host pixels, a live render-scale
// change (2 -> 1.5) left the depth at the old scale's size, its ratio 2.00 in a 1.50 pass, until the scale came back (public
// #46 with DLSS: the log's '[pass] mixed scales: attachment depth1 200x112... at 2.00, pass at 1.50' every pass from the
// change on).
static KTab g_grow = {.what = "depth-grow"};  // (format in the key; a / b = the guest-unit extent's float bits)
static float kind_sx(int kind), kind_sy(int kind);
static uint32_t f2u(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static float u2f(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static bool tex_grown(const NvnTexture *t, int kind, uint32_t *hw, uint32_t *hh) {   // (at kind's scale now)
    uint32_t a, b;
    if (!kt_get(&g_grow, t, &a, &b)) return false;
    *hw = (uint32_t)ceilf(u2f(a) * kind_sx(kind) - 1e-3f);
    *hh = (uint32_t)ceilf(u2f(b) * kind_sy(kind) - 1e-3f);
    return true;
}
bool renderer_texture_grow(NvnTexture *t, uint32_t hw, uint32_t hh) {
    static int off = -1;
    if (off < 0) { const char *e = getenv("LARECOMP_DEPTH_GROW"); off = e && !strcmp(e, "0"); }
    TexBackend *b = t ? (TexBackend *)t->backend : NULL;
    if (off || !b || !b->scalable || !b->image || b->samples > 1 || b->blk || b->pending_rgba || t->target != 1) return false;
    if (b->width >= hw && b->height >= hh) return false;
    const float rsx = kind_sx(b->scale_kind), rsy = kind_sy(b->scale_kind);   // (the scale this host extent is at)
    const float nw = rsx > 0 ? (float)hw / rsx : (float)hw, nh = rsy > 0 ? (float)hh / rsy : (float)hh;
    uint32_t ga, gb;
    if (!kt_get(&g_grow, t, &ga, &gb)) kt_put(&g_grow, t, f2u(nw), f2u(nh));
    else kt_put(&g_grow, t, f2u(nw > u2f(ga) ? nw : u2f(ga)), f2u(nh > u2f(gb) ? nh : u2f(gb)));   // (the largest, in guest units)
    const uint32_t bw = b->width, bh = b->height;
    extern uint32_t vk_view_gen;
    destroy_image(b);
    create_image(t, b);
    vk_view_gen++;
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);
    VkImageSubresourceRange rg = {b->aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
    if (b->aspect & VK_IMAGE_ASPECT_COLOR_BIT) {
        VkClearColorValue z = {{0}};
        vkCmdClearColorImage(cb, b->image, VK_IMAGE_LAYOUT_GENERAL, &z, 1, &rg);
    } else {
        VkClearDepthStencilValue z = {0};
        vkCmdClearDepthStencilImage(cb, b->image, VK_IMAGE_LAYOUT_GENERAL, &z, 1, &rg);
    }
    vk_full_barrier(cb);
    static int nlog;
    if (nlog++ < 32)
        LOG("[tex] depth grown to its colour level: %p addr %llx %ux%u f%#x %ux%u -> %ux%u\n", (void *)t, (unsigned long long)t->addr,
            t->width, t->height, t->format, bw, bh, b->width, b->height);
    return true;
}

// The shape of the targets the render scale reaches (kind 1): the scene's (totk_hooks.c output_scene_size), within the
// 1.74.. 1.81 band 16:9 always had (-2.1 % / +1.8 %: the game's odd sizes, 1600x900's halves and quarters rounded). A
// 16:9 scene keeps those exact bounds; 21:9 / 32:9 / 16:10 scenes get the same band round their own ratio, so the
// render scale reaches their scene-sized targets instead of the game's few 16:9 ones.
static float g_shape_lo, g_shape_hi;
static int g_shape_ready;   // (both bounds written before it: any thread may ask first, all write the same values)
static void shape_band(void) {
    if (__atomic_load_n(&g_shape_ready, __ATOMIC_ACQUIRE)) return;
    extern void output_scene_size(unsigned *w, unsigned *h);
    unsigned w, h;
    output_scene_size(&w, &h);
    const float a = (float)w / (float)h;
    float lo = 1.74f, hi = 1.81f;
    if (a <= 1.74f || a >= 1.81f) lo = a * (1.74f / (16.0f / 9.0f)), hi = a * (1.81f / (16.0f / 9.0f));
    g_shape_lo = lo, g_shape_hi = hi;
    if (!__atomic_exchange_n(&g_shape_ready, 1, __ATOMIC_ACQ_REL) && lo != 1.74f)
        LOG("[tex] render scale: targets shaped %.3f .. %.3f (the scene %ux%u)\n", lo, hi, w, h);
}

// 0 = rendered at the game's size, 1 = colour/depth target of the scene's shape (render scale), 2 = shadow map
static int tex_scale_kind(const NvnTexture *t) {
    // (cached: a getenv per call - per sampled texture per draw - was 0.24 ms a frame of the render thread in ucrtbase,
    // the a1 boot at ~99 fps)
    static int no_scale = -1;
    if (no_scale < 0) no_scale = getenv("LARECOMP_NO_SCALE") != NULL;
    if (no_scale) return 0;
    // (every 1920x1080 2D target, compressible or not, colour or depth: the inventory draws 6 targets at once, a compressible
    // one at the scene's scale next to the canvas's -> only the top-left 2/3 drawn)
    if (t->samples <= 1 && t->target == 1 && t->width == 1920 && t->height == 1080 && renderer_out_scale_active()) {
        if (tex_cpu1x(t)) return 0;   // (filled from the CPU: at the game's size, cpu1x_demote)
        static int nlog;
        if (nlog++ < 8) LOG("[tex] output canvas scale: addr %llx f%#x flags %#x\n", (unsigned long long)t->addr, t->format, t->flags);
        return 3;
    }
    if (t->samples <= 1 && t->target == 1 && __atomic_load_n(&g_promo.n, __ATOMIC_RELAXED)) { const int pk = tex_promoted(t); if (pk) return pk; }
    if (t->samples > 1 || !(t->flags & 8)) return 0;
    if ((t->target == 1 || t->target == 4) && t->width == t->height && t->width >= 256 && renderer_format_is_depth(t->format) &&
        (shadow_scale() != 1.0f || shadow_rungs_at1()))
        return 2;
    // 2D and 2D-array targets: a pass mixes them (TotK's 400x225 shadow-mask pass has a 2-layer array target next to 2D
    // ones), and begin_pass renders the minimum attachment extent, so an unscaled array clipped the pass to the top-left
    // 1/scale^2 of the screen (TOTK_RES_SCALE=2 lit only the top-left quarter)
    if ((t->target != 1 && t->target != 4) || t->width < 32 || t->height < 16) return 0;
    shape_band();
    float a = (float)t->width / (float)t->height;
    // (an entry counts here only where kind 1 is demoted too: a canvas entry stays scaled after a live Native UI Off)
    return a > g_shape_lo && a < g_shape_hi && !(t->target == 1 && cpu1x_mode() == 1 && tex_cpu1x(t)) ? 1 : 0;
}
static bool tex_scalable(const NvnTexture *t) { return tex_scale_kind(t) != 0; }
static float kind_sx(int kind) { return kind == 1 ? g_rs_x : kind == 2 ? shadow_scale() : kind == 3 ? g_out_s : 1.0f; }
static float kind_sy(int kind) { return kind == 1 ? g_rs_y : kind == 2 ? shadow_scale() : kind == 3 ? g_out_s : 1.0f; }
// exec.c promote_partners: the scale t's image will be made at (no backend yet: the pass's attachment loop makes it)
void renderer_texture_planned_scale(const NvnTexture *t, float *sx, float *sy) {
    const int k = tex_scale_kind(t);
    *sx = kind_sx(k), *sy = kind_sy(k);
}

bool renderer_texture_first_write(NvnTexture *t) {
    TexBackend *b = t ? (TexBackend *)t->backend : NULL;
    uint64_t total_frames = renderer_present_count();
    if (!b || !b->presented || (b->sx == b->sy) || b->touched_frame == total_frames + 1) return false;
    b->touched_frame = total_frames + 1;
    return true;
}

static uint32_t g_level_gen = 1;
void vk_level_change_check(void) {
    extern const char *game_level_name(void);
    static char last[64];
    const char *lv = game_level_name();
    if (strncmp(lv, last, sizeof last - 1) != 0) {
        strlcpy(last, lv, sizeof last);
        if (!strncmp(lv, "TelephoneBox", 12) || getenv("LARECOMP_CLEAR_ALL_LEVELS")) g_level_gen++;
    }
}

bool renderer_texture_frame_fresh(NvnTexture *t) {
    static int on = -1;
    // TotK: OFF by default (LARECOMP_FRAME_CLEAR=1 = the LASRecomp behaviour). Real hardware never clears a target
    // implicitly; TotK redraws only some 32-px tiles of its half-res lighting targets per pass and keeps the rest, so
    // this clear left black tiles = the "squarey shadows" / patchy torch light.
    if (on < 0) on = getenv("LARECOMP_FRAME_CLEAR") != NULL;
    TexBackend *b = t ? (TexBackend *)t->backend : NULL;
    uint64_t total_frames = renderer_present_count();
    if (!on || !b || !b->scalable || b->frame_cleared == total_frames + 1) return false;
    b->frame_cleared = total_frames + 1;
    return true;
}

bool renderer_texture_level_fresh(NvnTexture *t) {
    TexBackend *b = t ? (TexBackend *)t->backend : NULL;
    static int keep = -1;
    if (keep < 0) keep = getenv("LARECOMP_KEEP_TARGETS") != NULL;
    if (!b || keep || b->level_gen == g_level_gen) return false;
    if (!b->scalable) { b->level_gen = g_level_gen; return false; }
    bool fresh = b->level_gen != 0;
    b->level_gen = g_level_gen;
    return fresh;
}

void renderer_texture_mark_presented(NvnTexture *t) {
    TexBackend *b = t ? (TexBackend *)t->backend : NULL;
    if (b) b->presented = true;
}

bool renderer_texture_presented(NvnTexture *t) {
    TexBackend *b = t ? (TexBackend *)t->backend : NULL;
    return b && b->presented;
}

void renderer_texture_scale(NvnTexture *t, float *sx, float *sy) {
    TexBackend *b = t ? (TexBackend *)t->backend : NULL;
    if (b && b->scalable) { *sx = b->sx; *sy = b->sy; }
    else { *sx = 1; *sy = 1; }
}
static NvnTexture *alias_writer(NvnTexture *t);
// a sampled texture's scale = that of the image its view shows (vk_tex: another object's rendering at the same storage
// wins), not of the sampled object's own backend, which can be missing or of another kind
void renderer_texture_sample_scale(NvnTexture *t, float *sx, float *sy) {
    NvnTexture *w = t ? alias_writer(t) : NULL;
    renderer_texture_scale(w ? w : t, sx, sy);
}

// ================================================================== images
void vk_image_init_layout(VkCommandBuffer cb, VkImage img, VkImageAspectFlags aspect) {
    VkImageMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                               .srcStageMask = VK_PIPELINE_STAGE_2_NONE, .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                               .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                               .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL, .image = img,
                               .subresourceRange = {aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS},
                               .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b};
    vkCmdPipelineBarrier2(cb, &di);
}

// Images destroyed by the guest are kept until the submissions that may use them have completed.
typedef struct { VkImage img; VmaAllocation mem; VkImageView view; uint64_t seq; VkBuffer buf; } Dead;
static Dead *dead;
static int ndead, cap_dead;
static void views_forget(TexBackend *b);

// At most LARECOMP_FREE_FRAME=<images>,<MB> images / buffers freed a frame (default 48,256; 0 = all at once, as before).
// A residency round (512 textures) plus the recycle pool's frees came due together, one vk_collect_dead freed them all:
// their 333-422 ms frames from bulk drops, fixed the same way). What waits is freed at the next submissions; views always
// go.
VkDeviceSize vma_shim_size(VmaAllocation al);
static void vk_collect_dead_(bool all);
void vk_collect_dead(void) { vk_collect_dead_(false); }
static void vk_collect_dead_(bool all) {
    static int lim_n = -1;
    static uint64_t lim_b, frame, n_fr, b_fr;
    if (lim_n < 0) {
        const char *e = getenv("LARECOMP_FREE_FRAME");
        lim_n = 48; lim_b = 256ull << 20;
        if (e) { lim_n = atoi(e); const char *c = strchr(e, ','); lim_b = (c ? strtoull(c + 1, NULL, 10) : 256ull) << 20; }
    }
    const uint64_t fc = vk_frame_counter();
    if (fc != frame) { frame = fc; n_fr = b_fr = 0; }
    uint64_t done = vk_completed_seq();
    int k = 0, held = 0;
    for (int i = 0; i < ndead; i++) {
        if (dead[i].seq > done) { dead[k++] = dead[i]; continue; }
        if (dead[i].view) { vkDestroyImageView(g_vk.dev, dead[i].view, NULL); continue; }
        // (next time; a backlog over 512 is freed anyway, so a steady stream above the limit never piles up)
        if (!all && lim_n > 0 && (n_fr >= (uint64_t)lim_n || b_fr >= lim_b) && held < 512) { dead[k++] = dead[i]; held++; continue; }
        n_fr++; b_fr += vma_shim_size(dead[i].mem);
        if (dead[i].buf) vmaDestroyBuffer(g_vk.vma, dead[i].buf, dead[i].mem);
        else vmaDestroyImage(g_vk.vma, dead[i].img, dead[i].mem);
    }
    ndead = k;
}

uint64_t prof_img_n, prof_img_ns;  // (exec.c LARECOMP_VK_PROF) images created and the time spent creating them
uint64_t cum_img_n, cum_img_ns;    // the same, never reset (exec.c hitch log)
// This frame's slowest image creation and texture upload (identify + hash + deswizzle / decode / staging), what they were:
// exec.c's hitch log names them (stream-hitch: player logs gave only the frame's sums), then resets them each frame.
uint64_t hf_img_worst_ns, hf_up_worst_tk;
char hf_img_worst[112], hf_up_worst[112];
uint64_t cum_img_slow_n, cum_img_slow_ns, img_slow_worst_ns;   // (creations of 8 ms or more; the worst since device.c's last line)
char img_slow_worst[96];
static void create_image_(NvnTexture *t, TexBackend *b);
// CUBE_COMPATIBLE on square 2D arrays of 6+ layers that are no cube textures. kak.cap on the RDNA2 iGPU: 12+ such arrays
// (the terrain material arrays, 1024x1024 x121, 128x128 x118, 512x512 x12...) and no cube view of any 2D array. Off
// NVIDIA only cube / cube-array textures get the flag (a cube view of a flagless array gets the dummy view, logged);
// LARECOMP_CUBE_COMPAT=all / =cube forces either rule.
static int cube_compat_all(void) {
    static int v = -1;
    if (v < 0) {
        extern bool maxwell_nvidia;
        const char *e = getenv("LARECOMP_CUBE_COMPAT");
        v = e ? !strcmp(e, "all") : maxwell_nvidia;
    }
    return v;
}
static uint64_t oom_relief(void);   // (recycle section: out of device memory at a creation)
static bool dac_listed(const NvnTexture *t);
static VkFormat dac_colour(VkFormat f);
// LARECOMP_IMG_SPLIT_MS=<ms> (the bow's first-aim shadow image, 98 MB in 80 ms where 128 MB took 14): a creation of <ms>
// or more logs where its time went. create_image_ fills [0] vk_image_create_relieved (shim_vma.c's vma_shim_split splits
// its last vmaCreateImage; the rest is the out-of-memory relief and its first try), [1] vk_upload_cmd, [2] the layout
// init, [3] the depth zeroing's recording. 0 / unset: no line (the timers run either way, a few clock reads a creation).
static _Thread_local uint64_t img_split_ns[4];
static void create_image(NvnTexture *t, TexBackend *b) {
    extern uint64_t prof_up_ns;   // (~0: profiling off)
    uint64_t t0 = SDL_GetTicksNS();
    img_split_ns[0] = img_split_ns[1] = img_split_ns[2] = img_split_ns[3] = 0;
    create_image_(t, b);
    uint64_t dt = SDL_GetTicksNS() - t0;
    static long split_ms = -1;
    if (split_ms < 0) { const char *e = getenv("LARECOMP_IMG_SPLIT_MS"); split_ms = e ? atol(e) : 0; }
    if (split_ms > 0 && dt >= (uint64_t)split_ms * 1000000ull) {
        extern _Thread_local uint64_t vma_shim_split[4];
        const uint64_t *v = vma_shim_split, shim = v[0] + v[1] + v[3];
        LOG("[tex] image creation %.1f ms (%ux%ux%u fmt %#x layers %u): vkCreateImage %.1f, memory %.1f (of it vkAllocateMemory "
            "%.1f), bind %.1f, relief / first try %.1f, upload cmd %.1f, layout %.1f, zeroing %.1f, other %.1f ms\n",
            dt / 1e6, b->width, b->height, b->depth, t->format, b->layers, v[0] / 1e6, v[1] / 1e6, v[2] / 1e6, v[3] / 1e6,
            img_split_ns[0] > shim ? (img_split_ns[0] - shim) / 1e6 : 0.0, img_split_ns[1] / 1e6, img_split_ns[2] / 1e6,
            img_split_ns[3] / 1e6,
            dt > img_split_ns[0] + img_split_ns[1] + img_split_ns[2] + img_split_ns[3]
                ? (dt - img_split_ns[0] - img_split_ns[1] - img_split_ns[2] - img_split_ns[3]) / 1e6 : 0.0);
    }
    if (dt > hf_img_worst_ns) {
        extern VkDeviceSize vma_shim_size(VmaAllocation al);
        hf_img_worst_ns = dt;
        snprintf(hf_img_worst, sizeof hf_img_worst, "%ux%ux%u fmt %#x levels %u layers %u, %.1f MB", b->width, b->height, b->depth,
                 t->format, b->levels, b->layers, vma_shim_size(b->mem) / 1048576.0);
    }
    __atomic_add_fetch(&cum_img_n, 1, __ATOMIC_RELAXED); __atomic_add_fetch(&cum_img_ns, dt, __ATOMIC_RELAXED);
    if (prof_up_ns != ~0ull) { prof_img_n++; prof_img_ns += dt; }
    if (dt >= 8000000ull) {   // (device.c's "[vram] images" line: creations a frame feels, below the 50 ms hitch lines too)
        cum_img_slow_n++;
        cum_img_slow_ns += dt;
        if (dt > img_slow_worst_ns) {
            img_slow_worst_ns = dt;
            snprintf(img_slow_worst, sizeof img_slow_worst, "%ux%ux%u fmt %#x layers %u", b->width, b->height, b->depth, t->format, b->layers);
        }
    }
}
// (scalable kinds) a shadow map's image is made at a rung (shadow_rung)
static bool rung_kind(const NvnTexture *t, int kind) { return kind == 2 && shadow_rungs() && t->levels <= 1; }
// A scaled image's host extent (create_image_, and the recycle pool's match for shadow rungs): the guest size at the kind's
// scale, rounded UP: every guest pixel a scaled image partly covers exists on the host. Rounded to nearest, a 200x112 depth
// at 1.333 was 267x149 while the 400x225 colour target's level 1 it is drawn with is 266x150, so the render area (the
// smaller) cut the level's last row: never written in that pass (fs 6b2da6dd, every frame at 1.333; exact at 2.0), stale
// content a bilinear read then pulls in at the region's bottom edge. Since 1d0fa529 every mapping uses the allocation's own
// ratio, so the extra partial row / column is consistent. LARECOMP_ALLOC_CEIL=0: nearest.
static void scaled_extent(const NvnTexture *t, int kind, uint32_t *w, uint32_t *h) {
    static int aceil = -1;
    if (aceil < 0) { const char *e = getenv("LARECOMP_ALLOC_CEIL"); aceil = !e || strcmp(e, "0"); }
    const float sx = kind_sx(kind), sy = kind_sy(kind);
    uint32_t tw = t->width ? t->width : 1, th = t->height ? t->height : 1;
    if (aceil) { tw = (uint32_t)ceilf(tw * sx - 1e-3f); th = (uint32_t)ceilf(th * sy - 1e-3f); }
    else { tw = (uint32_t)lroundf(tw * sx); th = (uint32_t)lroundf(th * sy); }
    if (tw < 1) tw = 1;
    if (th < 1) th = 1;
    if (rung_kind(t, kind)) { tw = shadow_rung(tw); th = shadow_rung(th); }
    *w = tw; *h = th;
}
static void create_image_(NvnTexture *t, TexBackend *b) {
    b->snap_tag = 0;   // (exec.c snapshot delta: a new image is no snapshot's mirror)
    b->parked = false;
    VkFmt mf = fmt_of(t->format);
    b->nsx = b->sx = b->scalable ? kind_sx(b->scale_kind) : 1;
    b->nsy = b->sy = b->scalable ? kind_sy(b->scale_kind) : 1;
    uint32_t tw = t->width ? t->width : 1, th = t->height ? t->height : 1;
    if (b->scalable) {
        scaled_extent(t, b->scale_kind, &tw, &th);
        uint32_t gw, gh;   // (renderer_texture_grow: at least its colour level's extent)
        if (__atomic_load_n(&g_grow.n, __ATOMIC_RELAXED) && tex_grown(t, b->scale_kind, &gw, &gh)) { if (gw > tw) tw = gw; if (gh > th) th = gh; }
        if (rung_kind(t, b->scale_kind)) {   // (a shadow rung: the image's own ratio is its scale)
            b->sx = (float)tw / (float)(t->width ? t->width : 1);
            b->sy = (float)th / (float)(t->height ? t->height : 1);
        }
    }
    VkImageType type = VK_IMAGE_TYPE_2D;
    uint32_t layers = 1, depth = 1;
    switch (t->target) {
    case 0: type = VK_IMAGE_TYPE_1D; th = 1; break;
    case 2: type = VK_IMAGE_TYPE_3D; depth = t->depth ? t->depth : 1; break;
    case 3: type = VK_IMAGE_TYPE_1D; layers = t->height ? t->height : 1; th = 1; break;
    case 4: case 6: layers = t->depth ? t->depth : 1; break;
    case 8: layers = 6; break;
    case 9: layers = t->depth ? t->depth : 6; break;
    default: break;
    }
    uint32_t levels = t->levels ? t->levels : 1;
    {   // never more levels than the size allows (scaled targets, odd guest values)
        uint32_t m = 1, sz = tw > th ? tw : th;
        if (depth > sz) sz = depth;
        while (sz > 1) { sz >>= 1; m++; }
        if (levels > m) levels = m;
    }
    uint32_t samples = t->samples > 1 ? t->samples : 1;
    if (samples > 1) levels = 1;
    VkFormat vf = mf.vf;
    VkFormatFeatureFlags need = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    b->astc_bw = b->astc_bh = 0;
    b->astc_bc3 = false;
    bool depthfmt = renderer_format_is_depth(t->format);
    if (depthfmt) {
        // Depth / stencil images are also attachments: S8_UINT is optional by spec and only one of D24_UNORM_S8_UINT /
        // D32_SFLOAT_S8_UINT is guaranteed; D16 / D32 are mandatory. A missing one takes the nearest format that has
        // stencil or the other combined one; logged once per format.
        need |= VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT;
        static int force = -1;   // LARECOMP_FORCE_DS_SUBST=1: substitute even where the format exists (VVL on this PC)
        if (force < 0) force = getenv("LARECOMP_FORCE_DS_SUBST") && *getenv("LARECOMP_FORCE_DS_SUBST") == '1';
        if (force || !format_supports(vf, need)) {
            VkFormat alt[2] = {VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED};
            if (vf == VK_FORMAT_S8_UINT && g_vk.sep_ds_layouts)   // (stencil-only barriers on the combined image: device.c)
                alt[0] = VK_FORMAT_D32_SFLOAT_S8_UINT, alt[1] = VK_FORMAT_D24_UNORM_S8_UINT;
            else if (vf == VK_FORMAT_D32_SFLOAT_S8_UINT) alt[0] = VK_FORMAT_D24_UNORM_S8_UINT;
            else if (vf == VK_FORMAT_D16_UNORM) alt[0] = VK_FORMAT_D32_SFLOAT;
            VkFormat got = VK_FORMAT_UNDEFINED;
            for (int i = 0; i < 2 && !got; i++) if (alt[i] && format_supports(alt[i], need)) got = alt[i];
            static VkFormat logged[8];
            int li = 0;
            while (li < 8 && logged[li] && logged[li] != vf) li++;
            if (li < 8 && !logged[li]) {
                logged[li] = vf;
                LOG("[vk] depth / stencil format %d (nvn %#x) %s: %s %d\n", vf, t->format,
                    force ? "substituted (LARECOMP_FORCE_DS_SUBST)" : "cannot be a sampled attachment on this GPU",
                    got ? "using" : "no substitute, keeping", got ? got : vf);
            }
            if (got) vf = got;
        }
    } else if (!format_supports(vf, need)) {
        bool astc = vf >= VK_FORMAT_ASTC_4x4_UNORM_BLOCK && vf <= VK_FORMAT_ASTC_12x12_SRGB_BLOCK;
        const FormatInfo *fi = t->layout.fmt;
        if (astc && fi && fi->bpb == 16) {  // GPUs without ASTC (desktop): decoded into an RGBA8 image of the same colour space
            b->astc_bw = fi->bw;
            b->astc_bh = fi->bh;
            b->astc_srgb = t->format >= 0x87;
            vf = b->astc_srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
            // (compact textures: colour (sRGB) images of 256 x 256 or more in area (base level). tools/engine/bc3_survey.py
            // over 1500 of the game's ASTC images: 99.7 % of their texels are in those. The smaller ones held most of the
            // worst BC3 cases (the UI's 27 x 27 icons at 30-35 dB), and the UNORM ones are data: normal maps (_Nrm, the
            // terrain's NrmTrs) at 24.5-34 dB, masks; BC3's colour block bends normals, so those stay RGBA8)
            if (astc_bc3_on() && b->astc_srgb && (uint64_t)t->width * t->height >= 256 * 256)
                vf = b->astc_srgb ? VK_FORMAT_BC3_SRGB_BLOCK : VK_FORMAT_BC3_UNORM_BLOCK;
        } else {
            static int nlog;
            if (nlog++ < 20) LOG("[vk] format %d (nvn %#x) not supported: RGBA8 instead\n", vf, t->format);
            vf = VK_FORMAT_R8G8B8A8_UNORM;
        }
    }
    if (depthfmt && dac_listed(t) && dac_colour(vf) &&   // (a depth-format texture this storage renders colour into: below)
        format_supports(dac_colour(vf), VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT))
        vf = dac_colour(vf), depthfmt = false;
    VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (depthfmt) usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    else if (format_supports(vf, VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)) usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    // (render targets the game also samples while they are bound: exec.c declares those feedback loops, device.c)
    if ((g_vk.feedback_loop || g_vk.fbl_layout) && (usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)))
        usage |= VK_IMAGE_USAGE_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT;
    // (contact.c: with TOTK_CONTACT_SHADOWS on, the scene colour the contact-shadow march writes in place)
    if ((usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) && contact_wants_storage(vf) && format_supports(vf, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT))
        usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    VkImageCreateFlags flags = 0;
    if (!depthfmt) flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;  // format views (NVN texture views, raw sRGB views)
    if (type == VK_IMAGE_TYPE_2D && layers >= 6 && tw == th && samples == 1 && (cube_compat_all() || t->target == 8 || t->target == 9))
        flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    // 3D render targets (TotK renders mips / slices of 3D volumes): slices are drawn through 2D views
    if (type == VK_IMAGE_TYPE_3D && (usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)) flags |= VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT;
    // (and sampled through a 2D view of slice 0 where a shader's lookup is 2D: vk_view, device.c view2d_of_3d)
    if (type == VK_IMAGE_TYPE_3D && g_vk.view2d_of_3d && (usage & VK_IMAGE_USAGE_SAMPLED_BIT)) flags |= VK_IMAGE_CREATE_2D_VIEW_COMPATIBLE_BIT_EXT;
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .flags = flags, .imageType = type, .format = vf,
                            .extent = {tw, th, depth}, .mipLevels = levels, .arrayLayers = layers,
                            .samples = (VkSampleCountFlagBits)samples, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage,
                            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    extern _Thread_local int vma_shim_tag, vma_shim_cold;
    vma_shim_tag = b->astc_bw ? 1 : 2;
    // (shim_vma.c residency priority: a sampled texture the CPU fills, not a render target nor a scaled one, is what WDDM
    // may demote first when the card is short; P1.1)
    vma_shim_cold = !(t->flags & 8) && !tex_scalable(t) && samples <= 1;
    // (oom_relief: free what is held for nothing, once, then the old fallback)
    img_split_ns[0] = SDL_GetTicksNS();
    VkResult r = vk_image_create_relieved(&ci, &ai, &b->image, &b->mem, "nvn fmt %#x %ux%ux%u target %u", t->format, t->width,
                                           t->height, t->depth, t->target);
    img_split_ns[0] = SDL_GetTicksNS() - img_split_ns[0];
    if (r != VK_SUCCESS) {
        LOG("[vk] image creation failed (%d): nvn fmt %#x %ux%ux%u target %u\n", r, t->format, t->width, t->height, t->depth, t->target);
        ci.format = vf = VK_FORMAT_R8G8B8A8_UNORM;
        ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                   VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        ci.flags &= ~VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
        VK_CHECK(vmaCreateImage(g_vk.vma, &ci, &ai, &b->image, &b->mem, NULL));
        depthfmt = false;
    }
    vma_shim_tag = 0;
    vma_shim_cold = 0;
    b->format = vf;
    b->astc_bc3 = b->astc_bw && (vf == VK_FORMAT_BC3_UNORM_BLOCK || vf == VK_FORMAT_BC3_SRGB_BLOCK);   // (not after the RGBA8 fallback)
    b->usage = ci.usage;
    b->cflags = ci.flags;
    b->width = tw; b->height = th; b->depth = depth; b->layers = layers; b->levels = levels; b->samples = samples;
    b->aspect = depthfmt ? aspect_of(t->format) : VK_IMAGE_ASPECT_COLOR_BIT;
    b->base_type = type == VK_IMAGE_TYPE_1D ? (layers > 1 ? VK_IMAGE_VIEW_TYPE_1D_ARRAY : VK_IMAGE_VIEW_TYPE_1D)
                 : type == VK_IMAGE_TYPE_3D ? VK_IMAGE_VIEW_TYPE_3D
                 : t->target == 8 ? VK_IMAGE_VIEW_TYPE_CUBE : t->target == 9 ? VK_IMAGE_VIEW_TYPE_CUBE_ARRAY
                 : layers > 1 || t->target == 4 || t->target == 6 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    uint64_t t0 = SDL_GetTicksNS();
    VkCommandBuffer ucb = vk_upload_cmd();
    img_split_ns[1] = SDL_GetTicksNS() - t0;
    vk_image_init_layout(ucb, b->image, b->aspect);
    img_split_ns[2] = SDL_GetTicksNS() - t0 - img_split_ns[1];
    if (depthfmt) {
        // New depth / stencil images start zeroed (as fresh memory reads on Apple GPUs). The game builds its depth
        // pyramid with depth-only copies, so the stencil of those images was never written: whatever the memory
        // held, different per image and per run, then failed or passed the depth-of-field passes' stencil test in
        // patches (flicker that came and went between runs, every other frame, below 1x internal resolution).
        VkCommandBuffer cb = vk_upload_cmd();
        VkClearDepthStencilValue zero = {0.0f, 0};
        VkImageSubresourceRange rg = {b->aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
        vkCmdClearDepthStencilImage(cb, b->image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &rg);
        VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                               .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                               .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
        VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
        vkCmdPipelineBarrier2(cb, &di);
    }
    img_split_ns[3] = SDL_GetTicksNS() - t0 - img_split_ns[1] - img_split_ns[2];
}

static void defer(Dead d) {
    if (ndead == cap_dead) { cap_dead = cap_dead ? cap_dead * 2 : 256; dead = realloc(dead, cap_dead * sizeof *dead); }
    d.seq = vk_pending_seq();
    dead[ndead++] = d;
}

void vk_defer_image(VkImage img, VmaAllocation mem) { defer((Dead){.img = img, .mem = mem}); }
void vk_defer_view(VkImageView v) { defer((Dead){.view = v}); }

void vk_dl_note_upload(void *img, uint32_t w, uint32_t h, uint32_t lv);  // (exec.c: LARECOMP_VK_DEVLOST_TRACE)

static uint32_t level_w(const TexBackend *b, uint32_t lv) { uint32_t v = b->width >> lv; return v ? v : 1; }
static uint32_t level_h(const TexBackend *b, uint32_t lv) { uint32_t v = b->height >> lv; return v ? v : 1; }

static uint64_t park_bytes, park_live;   // (residency park_move: rendered images in system memory now)
static void destroy_image(TexBackend *b) {
    views_forget(b);
    if (b->parked) { park_bytes -= vma_shim_size(b->mem); park_live--; b->parked = false; }
    if (b->image || b->mem) defer((Dead){.img = b->image, .mem = b->mem});  // (never the same allocation twice)
    b->image = VK_NULL_HANDLE;
    b->mem = VK_NULL_HANDLE;
    if (b->blk) { destroy_image(b->blk); free(b->blk); b->blk = NULL; }
    if (b->twin) { destroy_image(b->twin); free(b->twin); b->twin = NULL; }
    b->twin_valid = false;
    if (b->alt.image || b->alt.mem) defer((Dead){.img = b->alt.image, .mem = b->alt.mem});
    memset(&b->alt, 0, sizeof b->alt);
}

// ---- depth twins: TotK renders depth into R32F colour targets (1600x900 f0xa) and samples them with depth compare. A
// colour format has no SAMPLED_IMAGE_DEPTH_COMPARISON: NVIDIA compares anyway, other drivers need not (broken shadows on
// AMD / Intel). Such a target gets a depth-format twin (R32F -> D32_SFLOAT, R16_UNORM -> D16_UNORM: the same bits)
// holding a copy of its latest contents (image -> buffer -> depth image): exec.c copies after every pass that drew into
// it and, before a draw / dispatch that compare-samples it, whenever another write (copy, upload, clear, re-creation)
// left the twin stale (twin_valid). LARECOMP_DEPTH_TWIN=1 on, =0 off; default on except on NVIDIA's driver, which
// compares colour images (there the copies would be pure cost). LARECOMP_DEPTH_TWIN_CHECK=1: the first 8 copies' value
// range is read back and logged (D32_SFLOAT outside [0,1] needs VK_EXT_depth_range_unrestricted, enabled when present:
// device.c).
static VkBuffer twin_buf;
static VmaAllocation twin_buf_mem;
static uint64_t twin_buf_size;
uint64_t vk_twin_copies;   // (device.c stats window)
static VkFormat twin_format(VkFormat f) {
    return f == VK_FORMAT_R32_SFLOAT ? VK_FORMAT_D32_SFLOAT : f == VK_FORMAT_R16_UNORM ? VK_FORMAT_D16_UNORM : VK_FORMAT_UNDEFINED;
}
bool vk_depth_twin_format(VkFormat f) { return twin_format(f) != VK_FORMAT_UNDEFINED; }   // (exec.c: a colour format a compare can mean)
static int twin_on(void) {
    static int on = -1;
    if (on < 0) {
        extern bool maxwell_nvidia;
        const char *e = getenv("LARECOMP_DEPTH_TWIN");
        on = e ? *e == '1' : !maxwell_nvidia;
        LOG("[tex] depth twins for compare-sampled colour targets: %s\n", on ? "on" : e ? "off (LARECOMP_DEPTH_TWIN=0)" : "off (NVIDIA compares colour images)");
    }
    return on;
}
// per level: 4-byte aligned buffer offsets (depth / stencil copies need multiples of 4: odd D16 mips)
static uint64_t twin_layout(const TexBackend *b, uint64_t off[16]) {
    uint64_t n = 0, tb = b->format == VK_FORMAT_R32_SFLOAT ? 4 : 2;
    for (uint32_t l = 0; l < b->levels && l < 16; l++) {
        off[l] = n;
        n += ((uint64_t)level_w(b, l) * level_h(b, l) * b->layers * tb + 3) & ~3ull;
    }
    return n;
}
// The twin a compare sample of b would read (created on first need). NULL: no twin for this image.
static TexBackend *twin_of(TexBackend *b) {
    VkFormat tf = twin_format(b->format);
    if (!tf || !twin_on() || !b->image || b->samples != 1 || b->base_type == VK_IMAGE_VIEW_TYPE_3D) return NULL;
    if (b->twin) return b->twin;
    // (a sampled depth format supports compare: FORMAT_FEATURE_2_SAMPLED_IMAGE_DEPTH_COMPARISON is set for them)
    if (!format_supports(tf, VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)) return NULL;
    TexBackend *d = calloc(1, sizeof *d);
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = tf,
                            .extent = {b->width, b->height, 1}, .mipLevels = b->levels, .arrayLayers = b->layers,
                            .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (vk_image_create_relieved(&ci, &ai, &d->image, &d->mem, "depth twin vk fmt %d %ux%u x%u", tf, b->width, b->height,
                                  b->layers) != VK_SUCCESS) { free(d); return NULL; }
    d->format = tf; d->width = b->width; d->height = b->height; d->depth = 1; d->layers = b->layers; d->levels = b->levels;
    d->samples = 1; d->aspect = VK_IMAGE_ASPECT_DEPTH_BIT; d->base_type = b->base_type; d->usage = ci.usage; d->gpu_owned = true;
    vk_image_init_layout(vk_upload_cmd(), d->image, d->aspect);
    b->twin = d;
    b->twin_valid = false;
    static int nlog;
    if (nlog++ < 20) LOG("[tex] depth twin: %ux%u x%u levels %u fmt %d sampled with depth compare -> %d copy\n", b->width, b->height,
                         b->layers, b->levels, b->format, tf);
    return d;
}
// exec.c, before a draw / dispatch (outside a pass) for each compare-sampled image: a copy is due now
bool vk_depth_twin_stale(TexBackend *b) { return twin_of(b) && !b->twin_valid; }
// The twin's view for a compare sample (the descriptor's level / layer range, as vk_view), or VK_NULL_HANDLE: not filled
// yet / no twin (the colour view stays, as before)
VkImageView vk_depth_twin_view(TexBackend *b, const NvnTextureView *v, bool has_view, int textype) {
    if (textype != TEXTYPE_2D && textype != TEXTYPE_2D_ARRAY) return VK_NULL_HANDLE;
    TexBackend *d = twin_of(b);
    if (!d || !b->twin_valid) return VK_NULL_HANDLE;
    if (!has_view) v = NULL;
    uint32_t lv0 = v && (v->setmask & 1) ? v->base_level : 0;
    uint32_t nlv = v && (v->setmask & 1) && v->num_levels ? v->num_levels : d->levels - (lv0 < d->levels ? lv0 : 0);
    uint32_t ly0 = v && (v->setmask & 2) ? v->min_layer : 0;
    uint32_t nly = v && (v->setmask & 2) && v->num_layers ? v->num_layers : d->layers - (ly0 < d->layers ? ly0 : 0);
    if (lv0 >= d->levels) lv0 = d->levels - 1;
    if (lv0 + nlv > d->levels) nlv = d->levels - lv0;
    if (ly0 >= d->layers) ly0 = d->layers - 1;
    if (ly0 + nly > d->layers) nly = d->layers - ly0;
    if (textype == TEXTYPE_2D) nly = 1;
    static const VkComponentMapping id = {0};
    return vk_image_view(d, textype == TEXTYPE_2D ? VK_IMAGE_VIEW_TYPE_2D : VK_IMAGE_VIEW_TYPE_2D_ARRAY, d->format, lv0, nlv, ly0, nly, id,
                         VK_IMAGE_ASPECT_DEPTH_BIT);
}
static struct { VkBuffer buf; VmaAllocation mem; void *p; uint64_t seq, n; uint32_t w, h; VkFormat f; } twin_chk[8];
static int twin_nchk;
void vk_depth_twin_check_poll(void) {   // (also each stats window, device.c: a copy may be the last one)
    for (int i = 0; i < twin_nchk; i++) {
        if (!twin_chk[i].buf || twin_chk[i].seq > vk_completed_seq()) continue;
        const void *p = twin_chk[i].p;
        double lo = 1e30, hi = -1e30;
        uint64_t nan = 0;
        for (uint64_t k = 0; p && k < twin_chk[i].n; k++) {
            double x = twin_chk[i].f == VK_FORMAT_R32_SFLOAT ? ((const float *)p)[k] : ((const uint16_t *)p)[k] / 65535.0;
            if (x != x) { nan++; continue; }
            if (x < lo) lo = x;
            if (x > hi) hi = x;
        }
        LOG("[tex] depth twin check: %ux%u fmt %d level 0 values %g .. %g (%llu NaN)%s\n", twin_chk[i].w, twin_chk[i].h, twin_chk[i].f, lo, hi,
            (unsigned long long)nan, lo < 0 || hi > 1 || nan ? (g_vk.depth_unrestricted ? ": outside [0,1], kept (depth_range_unrestricted)"
                                                                    : ": OUTSIDE [0,1], undefined in the twin on this driver") : "");
        defer((Dead){.buf = twin_chk[i].buf, .mem = twin_chk[i].mem});
        twin_chk[i].buf = VK_NULL_HANDLE;
    }
}
void vk_depth_twin_copy(VkCommandBuffer cb, TexBackend *b) {
    TexBackend *d = b->twin;
    if (b->base_type == VK_IMAGE_VIEW_TYPE_3D) return;   // (a 3D image's twin is its slice copy: slice_twin_view)
    if (!d || !d->image || !b->image || d->width != b->width || d->height != b->height || d->levels != b->levels || d->layers != b->layers) return;
    uint64_t off[16], n = twin_layout(b, off);
    if (n > twin_buf_size) {
        if (twin_buf) defer((Dead){.buf = twin_buf, .mem = twin_buf_mem});
        VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = n,
                                 .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        if (vmaCreateBuffer(g_vk.vma, &bi, &ai, &twin_buf, &twin_buf_mem, NULL) != VK_SUCCESS) { twin_buf = VK_NULL_HANDLE; twin_buf_size = 0; return; }
        twin_buf_size = n;
    }
    VkBufferImageCopy rg[16];
    uint32_t nlv = b->levels < 16 ? b->levels : 16;
    if (g_vk.maint8) {   // (VK_KHR_maintenance8: R32F -> D32 / R16 -> D16 in one image copy, no buffer round trip)
        VkImageCopy ic[16];
        for (uint32_t l = 0; l < nlv; l++)
            ic[l] = (VkImageCopy){.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, l, 0, b->layers}, .dstSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, l, 0, b->layers},
                                  .extent = {level_w(b, l), level_h(b, l), 1}};
        vk_full_barrier(cb);
        vkCmdCopyImage(cb, b->image, VK_IMAGE_LAYOUT_GENERAL, d->image, VK_IMAGE_LAYOUT_GENERAL, nlv, ic);
        vk_full_barrier(cb);
        b->twin_valid = true;
        if (++vk_twin_copies <= 4) LOG("[tex] depth twin copy #%llu: %ux%u fmt %d (image copy, maintenance8)\n", (unsigned long long)vk_twin_copies,
                                       b->width, b->height, b->format);
        return;
    }
    for (uint32_t l = 0; l < nlv; l++)
        rg[l] = (VkBufferImageCopy){.bufferOffset = off[l], .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, l, 0, b->layers},
                                    .imageExtent = {level_w(b, l), level_h(b, l), 1}};
    vk_full_barrier(cb);
    vkCmdCopyImageToBuffer(cb, b->image, VK_IMAGE_LAYOUT_GENERAL, twin_buf, nlv, rg);
    vk_full_barrier(cb);
    for (uint32_t l = 0; l < nlv; l++) rg[l].imageSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    vkCmdCopyBufferToImage(cb, twin_buf, d->image, VK_IMAGE_LAYOUT_GENERAL, nlv, rg);
    static int check = -1;
    if (check < 0) { const char *e = getenv("LARECOMP_DEPTH_TWIN_CHECK"); check = e && *e == '1'; }
    if (check) {
        vk_depth_twin_check_poll();
        if (twin_nchk < 8) {   // level 0, layer 0 into a host buffer, read once the submission completes
            uint64_t cnt = (uint64_t)b->width * b->height, sz = cnt * (b->format == VK_FORMAT_R32_SFLOAT ? 4 : 2);
            VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = sz, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
            VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                                          .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT};
            VmaAllocationInfo info;
            int i = twin_nchk;
            if (vmaCreateBuffer(g_vk.vma, &bi, &ai, &twin_chk[i].buf, &twin_chk[i].mem, &info) == VK_SUCCESS) {
                twin_chk[i].p = info.pMappedData;
                VkBufferCopy bc = {off[0], 0, sz};
                vkCmdCopyBuffer(cb, twin_buf, twin_chk[i].buf, 1, &bc);
                twin_chk[i].seq = vk_pending_seq(); twin_chk[i].n = cnt; twin_chk[i].w = b->width; twin_chk[i].h = b->height;
                twin_chk[i].f = b->format;
                twin_nchk++;
            }
        }
    }
    vk_full_barrier(cb);
    b->twin_valid = true;
    if (++vk_twin_copies <= 4) LOG("[tex] depth twin copy #%llu: %ux%u fmt %d\n", (unsigned long long)vk_twin_copies, b->width, b->height, b->format);
}

// ---- depth-format colour targets (AMD VVL on kak.cap: VUIDs 06582, 06087, 04727, 06062): TotK renders colour into textures of
// NVN depth formats (DEPTH32F 1600x900 / 800x450 / 320x180 / 200x112 / 100x56 and D16 800x450 / 1600x900 / 800x800, mostly as the
// only attachment of the pass: a depth pyramid / linear-depth chain). A depth image cannot be a colour attachment (NVIDIA renders
// it anyway; elsewhere the writes are undefined). Such a texture's image moves to the colour format with the same bits (D32_SFLOAT
// -> R32_SFLOAT, D16_UNORM -> R16_UNORM) when it is bound as a colour target, and back when it is bound as the depth attachment
// (renderer_texture_target_kind, exec.c begin_pass): the other kind's image is kept (TexBackend.alt), so a move is a swap and a
// copy of the contents, made once per role change. Its storage is listed while its last role is colour, so the objects TotK
// re-creates there every frame start in the colour format (a texture only ever drawn as colour costs no copy). A compare sample of
// the colour image takes the depth twin path. Default on except on NVIDIA's driver (it renders depth images as colour: no copies
// there); LARECOMP_DEPTH_AS_COLOUR=1 on, =0 off (the depth image as the colour attachment, as before).
#define DAC_MAX 128
static struct { uint64_t addr; uint32_t w, h, f; } g_dac[DAC_MAX];
static int g_ndac;
static pthread_mutex_t g_dac_lock = PTHREAD_MUTEX_INITIALIZER;
static int dac_on(void) {
    static int on = -1;
    if (on < 0) {
        extern bool maxwell_nvidia;
        const char *e = getenv("LARECOMP_DEPTH_AS_COLOUR");
        on = e ? *e == '1' : !maxwell_nvidia;
        LOG("[tex] depth-format colour targets: %s\n", on ? "the colour format (copies on a role change)" :
            e ? "the depth image (LARECOMP_DEPTH_AS_COLOUR=0)" : "the depth image (NVIDIA renders depth images as colour)");
    }
    return on;
}
static VkFormat dac_colour(VkFormat f) {
    return f == VK_FORMAT_D32_SFLOAT ? VK_FORMAT_R32_SFLOAT : f == VK_FORMAT_D16_UNORM ? VK_FORMAT_R16_UNORM : VK_FORMAT_UNDEFINED;
}
static VkFormat dac_depth(VkFormat f) {
    return f == VK_FORMAT_R32_SFLOAT ? VK_FORMAT_D32_SFLOAT : f == VK_FORMAT_R16_UNORM ? VK_FORMAT_D16_UNORM : VK_FORMAT_UNDEFINED;
}
static int dac_find(const NvnTexture *t) {   // (under g_dac_lock)
    for (int i = 0; i < g_ndac; i++)
        if (g_dac[i].addr == t->addr && g_dac[i].w == t->width && g_dac[i].h == t->height && g_dac[i].f == t->format) return i;
    return -1;
}
static bool dac_listed(const NvnTexture *t) {
    if (!g_ndac || !t->addr || !renderer_format_is_depth(t->format)) return false;
    pthread_mutex_lock(&g_dac_lock);
    bool r = dac_find(t) >= 0;
    pthread_mutex_unlock(&g_dac_lock);
    return r;
}
static void dac_list(const NvnTexture *t, bool add) {
    if (!t->addr) return;
    pthread_mutex_lock(&g_dac_lock);
    int i = dac_find(t);
    if (add && i < 0 && g_ndac < DAC_MAX) g_dac[g_ndac++] = (typeof(g_dac[0])){t->addr, t->width, t->height, t->format};
    if (!add && i >= 0) g_dac[i] = g_dac[--g_ndac];
    pthread_mutex_unlock(&g_dac_lock);
}
static VkBuffer dac_buf;
static VmaAllocation dac_buf_mem;
static uint64_t dac_buf_size;
uint64_t vk_dac_moves;   // (device.c stats window)
// t is bound as a colour target (colour) or as the depth attachment (!colour) of the pass begin_pass opens next: an image
// of a depth NVN format moves to the kind that attachment needs. Outside a render pass. True: moved.
bool renderer_texture_target_kind(NvnTexture *t, bool colour) {
    if (!t || !renderer_format_is_depth(t->format) || !dac_on()) return false;
    if (t->samples > 1) return false;
    dac_list(t, colour);   // (an image made now follows the listing: its storage's last role)
    // the image the pass will use, made or taken from the recycle pool now (a pooled image of this storage may be of the
    // other kind: it is moved below, a swap once it has both)
    TexBackend *b = vk_tex(t, true);
    if (!b || !b->image) return false;
    if (b->samples > 1 || b->blk || b->pending_rgba || b->base_type == VK_IMAGE_VIEW_TYPE_3D) return false;
    bool is_colour = b->aspect == VK_IMAGE_ASPECT_COLOR_BIT;
    if (is_colour == colour || (!is_colour && b->aspect != VK_IMAGE_ASPECT_DEPTH_BIT)) return false;   // (stencil formats stay)
    VkFormat nf = colour ? dac_colour(b->format) : dac_depth(b->format);
    if (!nf || !format_supports(nf, VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT |
                                        (colour ? VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT : VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)))
        return false;
    dac_list(t, colour);
    views_forget(b);
    if (b->twin) { destroy_image(b->twin); free(b->twin); b->twin = NULL; }
    b->twin_valid = false;
    typeof(b->alt) cur = {b->image, b->mem, b->format, b->aspect, b->usage, b->cflags};
    bool made = false;
    if (b->alt.image && b->alt.format == nf) {   // swap in the kept image of the other kind
        b->image = b->alt.image, b->mem = b->alt.mem, b->format = b->alt.format, b->aspect = b->alt.aspect;
        b->usage = b->alt.usage, b->cflags = b->alt.cflags;
    } else {   // first move of this backend: make the other kind (create_image follows the listing above)
        if (b->alt.image || b->alt.mem) defer((Dead){.img = b->alt.image, .mem = b->alt.mem});
        b->image = VK_NULL_HANDLE, b->mem = VK_NULL_HANDLE;
        create_image(t, b);
        if (!b->image || b->format != nf) {   // (refused after all: back to the old image)
            if (b->image || b->mem) defer((Dead){.img = b->image, .mem = b->mem});
            b->image = cur.image, b->mem = cur.mem, b->format = cur.format, b->aspect = cur.aspect, b->usage = cur.usage, b->cflags = cur.cflags;
            memset(&b->alt, 0, sizeof b->alt);
            dac_list(t, !colour);
            return false;
        }
        made = true;
    }
    b->alt = cur;
    extern uint32_t vk_view_gen;
    vk_view_gen++;   // (exec.c's per-frame view memo is keyed by the texture object)
    // the contents: old image -> a buffer -> new image (the same bits: 4 / 2 bytes per texel, each level and layer)
    uint32_t w = b->width, h = b->height, lv = b->levels < 16 ? b->levels : 16, ly = b->layers;
    uint64_t tb = nf == VK_FORMAT_R32_SFLOAT || nf == VK_FORMAT_D32_SFLOAT ? 4 : 2, off[16], n = 0;
    for (uint32_t l = 0; l < lv; l++) {
        off[l] = n;
        n += ((uint64_t)(w >> l ? w >> l : 1) * (h >> l ? h >> l : 1) * ly * tb + 3) & ~3ull;
    }
    if (n > dac_buf_size) {
        if (dac_buf) defer((Dead){.buf = dac_buf, .mem = dac_buf_mem});
        VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = n,
                                 .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        if (vmaCreateBuffer(g_vk.vma, &bi, &ai, &dac_buf, &dac_buf_mem, NULL) != VK_SUCCESS) dac_buf = VK_NULL_HANDLE, n = dac_buf_size = 0;
        else dac_buf_size = n;
    }
    if (g_vk.maint8) {   // (VK_KHR_maintenance8: depth <-> matching colour in one image copy)
        VkImageCopy ic[16];
        for (uint32_t l = 0; l < lv; l++)
            ic[l] = (VkImageCopy){.srcSubresource = {cur.aspect, l, 0, ly}, .dstSubresource = {b->aspect, l, 0, ly},
                                  .extent = {w >> l ? w >> l : 1, h >> l ? h >> l : 1, 1}};
        VkCommandBuffer cb = vk_cmd();
        vk_full_barrier(cb);
        vkCmdCopyImage(cb, cur.image, VK_IMAGE_LAYOUT_GENERAL, b->image, VK_IMAGE_LAYOUT_GENERAL, lv, ic);
        vk_full_barrier(cb);
        n = 0;
    }
    if (n) {
        VkBufferImageCopy rg[16];
        for (uint32_t l = 0; l < lv; l++)
            rg[l] = (VkBufferImageCopy){.bufferOffset = off[l], .imageSubresource = {cur.aspect, l, 0, ly},
                                        .imageExtent = {w >> l ? w >> l : 1, h >> l ? h >> l : 1, 1}};
        VkCommandBuffer cb = vk_cmd();
        vk_full_barrier(cb);
        vkCmdCopyImageToBuffer(cb, cur.image, VK_IMAGE_LAYOUT_GENERAL, dac_buf, lv, rg);
        vk_full_barrier(cb);
        for (uint32_t l = 0; l < lv; l++) rg[l].imageSubresource.aspectMask = b->aspect;
        vkCmdCopyBufferToImage(cb, dac_buf, b->image, VK_IMAGE_LAYOUT_GENERAL, lv, rg);
        vk_full_barrier(cb);
    }
    vk_dac_moves++;
    static int nlog;
    if (nlog++ < 24)
        LOG("[tex] depth-format target bound as %s: %p addr %llx %ux%u nvn f%#x -> vk %d (%s)\n", colour ? "colour" : "depth again",
            (void *)t, (unsigned long long)t->addr, t->width, t->height, t->format, nf, made ? "made" : "swapped");
    return true;
}

// ---- block-compressed render targets. Vulkan cannot render to a BC image, so the pass draws into an uncompressed "block
// shadow" image (w/4 x h/4 x depth, the view's format) and exec.c copies each rendered level/slice into the BC image
// after the pass (vkCmdCopyImage between size-compatible formats). LARECOMP_NO_BC_RT=1: dropped (not rendered).
static bool is_bc(VkFormat f) { return f >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && f <= VK_FORMAT_BC7_SRGB_BLOCK; }
static uint32_t bc_block_bytes(VkFormat f) {
    return f <= VK_FORMAT_BC1_RGBA_SRGB_BLOCK || f == VK_FORMAT_BC4_UNORM_BLOCK || f == VK_FORMAT_BC4_SNORM_BLOCK ? 8 : 16;
}
// BC compatibility class (Vulkan: BC1_RGB, BC1_RGBA, BC2 .. BC7 are each their own class; UNORM / SRGB / SNORM share one)
static int bc_class(VkFormat f) { return (int)(f - VK_FORMAT_BC1_RGB_UNORM_BLOCK) / 2; }
static int bc_class_views(void) {   // (vk_view: views across BC kinds read in the image's format; LARECOMP_BC_CLASS_VIEW=0 off)
    static int on = -1;
    if (on < 0) { const char *e = getenv("LARECOMP_BC_CLASS_VIEW"); on = !(e && *e == '0'); }
    return on;
}
static uint32_t texel_bytes(VkFormat f) {
    switch (f) {
    case VK_FORMAT_R16G16B16A16_UINT: case VK_FORMAT_R16G16B16A16_SINT: case VK_FORMAT_R16G16B16A16_SFLOAT:
    case VK_FORMAT_R32G32_UINT: case VK_FORMAT_R32G32_SINT: case VK_FORMAT_R32G32_SFLOAT: return 8;
    case VK_FORMAT_R32G32B32A32_UINT: case VK_FORMAT_R32G32B32A32_SINT: case VK_FORMAT_R32G32B32A32_SFLOAT: return 16;
    default: return 0;
    }
}

static TexBackend *block_shadow(NvnTexture *t, TexBackend *b, VkFormat vf) {
    static int off = -1;
    if (off < 0) off = getenv("LARECOMP_NO_BC_RT") != NULL;
    // (not for decoded ASTC: a BC3 one, compact textures, would take the game's ASTC block writes as BC3 blocks; as with
    // the RGBA8 one, no shadow)
    if (off || !texel_bytes(vf) || texel_bytes(vf) != bc_block_bytes(b->format) || b->samples > 1 || b->astc_bw) return NULL;
    if (b->blk && b->blk->format == vf) return b->blk;
    if (b->blk) { destroy_image(b->blk); free(b->blk); b->blk = NULL; }
    TexBackend *s = calloc(1, sizeof *s);
    bool is3d = b->base_type == VK_IMAGE_VIEW_TYPE_3D;
    uint32_t w = (b->width + 3) / 4, h = (b->height + 3) / 4, d = is3d ? b->depth : 1;
    uint32_t m = 1, sz = w > h ? w : h;
    if (d > sz) sz = d;
    while (sz > 1) { sz >>= 1; m++; }
    uint32_t levels = b->levels < m ? b->levels : m;
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                            .flags = is3d ? VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT : 0,
                            .imageType = is3d ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D, .format = vf, .extent = {w, h, d},
                            .mipLevels = levels, .arrayLayers = is3d ? 1 : b->layers, .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                     VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    extern _Thread_local int vma_shim_tag;
    vma_shim_tag = 3;
    VkResult sr_ = vk_image_create_relieved(&ci, &ai, &s->image, &s->mem, "block shadow vk fmt %d %ux%ux%u", vf, w, h, d);
    vma_shim_tag = 0;
    if (sr_ != VK_SUCCESS) {
        LOG("[vk] block shadow creation failed: %ux%ux%u vk fmt %d\n", w, h, d, vf);
        free(s);
        return NULL;
    }
    s->format = vf; s->usage = ci.usage; s->cflags = ci.flags;
    s->width = w; s->height = h; s->depth = d; s->layers = ci.arrayLayers; s->levels = levels; s->samples = 1;
    s->aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    s->base_type = is3d ? VK_IMAGE_VIEW_TYPE_3D : b->base_type;
    s->gpu_owned = true;
    vk_image_init_layout(vk_upload_cmd(), s->image, s->aspect);
    b->blk = s;
    LOG("[vk] block shadow for BC render target %ux%ux%u vk fmt %d addr %llx: %ux%ux%u vk fmt %d, %u levels\n", t->width,
        t->height, t->depth, b->format, (unsigned long long)t->addr, w, h, d, vf, levels);
    return s;
}

TexBackend *vk_render_backend(NvnTexture *t) {
    TexBackend *b = t->backend;
    return b && b->blk && is_bc(b->format) ? b->blk : b;
}

// Live game textures: an in-place reboot (vk_textures_reset) destroys them all.
static NvnTexture **g_live;
static int g_nlive, g_cap_live;
static void live_add(NvnTexture *t, TexBackend *b) {
    if (g_nlive == g_cap_live) { g_cap_live = g_cap_live ? g_cap_live * 2 : 1024; g_live = realloc(g_live, g_cap_live * sizeof *g_live); }
    g_live[g_nlive++] = t;
    b->reg = g_nlive;
}
static void live_remove(TexBackend *b) {
    if (!b->reg) return;
    NvnTexture *last = g_live[--g_nlive];
    g_live[b->reg - 1] = last;
    if (last->backend) ((TexBackend *)last->backend)->reg = b->reg;
    b->reg = 0;
}

// ---- image recycling. TotK initializes and finalizes ~30 transient render textures per frame. A finalized texture's image
// goes to a pool; a new texture of the same shape takes it with its views. Over the same storage its contents are kept
// (they are that memory's); over other storage depth starts zeroed and colour is cleared by its first pass when a pass
// writes it first (fresh_color). LARECOMP_VK_NO_RECYCLE=1 disables.
#define RECYCLE_MAX 512
typedef struct { TexBackend *b; uint32_t format, target, w, h, d, levels, samples; bool scalable; float sx, sy; uint64_t seq, addr; bool gpu_owned; uint64_t put_ns;
                 uint64_t ww; bool ww_ok; } Recycled;   // ww: write-watch level at the put (LARECOMP_RECYCLE_RECHECK, recycle_get)
static Recycled g_recycle[RECYCLE_MAX];
static int g_nrecycle;
static uint64_t g_recycle_bytes;   // (device memory held by pooled images: "[vram]" line)
uint64_t vk_recycle_bytes(int *n) { if (n) *n = g_nrecycle; return g_recycle_bytes; }
VkDeviceSize vma_shim_size(VmaAllocation al);
static pthread_mutex_t g_recycle_lock = PTHREAD_MUTEX_INITIALIZER;
uint64_t prof_img_reused, prof_img_same_addr;
uint64_t cum_img_reused, cum_img_unpooled;   // (never reset: device.c's "[vram] images" line, player logs included)
uint64_t shadow_miss[7];   // (recycle_get: shadow maps that took no pooled image, by why; never reset, device.c's "[vram] images" line)
uint64_t shadow_open_picks, shadow_fresh_cleared;   // (recycle_get: rungs taken from the open submission / at a new size, zeroed)
static bool shadow_open_pick_on(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("LARECOMP_SHADOW_OPEN_PICK"); on = !(e && *e == '0'); }
    return on;
}
static int recycle_off(void) {
    static int off = -1;
    if (off < 0) off = getenv("LARECOMP_VK_NO_RECYCLE") != NULL;
    return off;
}
// Same-storage re-check (LARECOMP_RECYCLE_RECHECK, default 1 since cand6 (LEAD 09:07), =0 off FIX_PROGRAM_1106 item c, ONEFRAME_REPORT
// #3). Arm 11-06 (field 15 min, 8181bbf17): 8394 of 27548 CPU images taken back at the same storage had new bytes at first use (mostly
// 260x260 terrain tiles), late-fill lines 21 -> 1, fps unchanged (median 120.0 / 119.9, 1% low 109 / 110). recycle_get gives a new
// object over the storage of a finalized one that storage's pooled image, with its upload state (uploaded, data_hash, ww_seen,
// checked_frame). That state describes the storage: one checked earlier in the frame is not asked again in it, and a non-dynamic one
// is asked every 8th frame. A tile streamed into reused storage then samples the previous tile's texels at N (wrong but plausible
// terrain), and new bytes are only seen at a later check. =1: a CPU-filled image (not GPU-owned) taken back at the same storage is
// checked at the new object's first use, as a stream pre-upload's first use is: the write watch since the last hash, shader writes
// since it, else a full hash; uploaded again only if its bytes changed (no blind re-upload; cost: one watch query or hash per recycled
// image). A GPU-owned, unscaled colour image is only counted when the CPU wrote its storage since the put ("[recycle]" line). =2: that
// GPU-owned image is a CPU texture from then on (gpu_owned off, uploaded at its first use), when it was finalized in an earlier
// submission (the upload buffer runs before the open submission's draws). The watch is asked on the storage's interior pages only
// (rc_own_ww): a neighbour's write-back on a shared edge page is ours. With the default at 1 (no variable) only the CPU-image check
// runs: the GPU-owned watch (a sweep per put and per get, 749017 picks in the arm's 15 min) and the periodic counts line are for a set
// variable (=1 arm, =2) only.
static bool g_rc_set;   // (LARECOMP_RECYCLE_RECHECK set: GPU-owned watch + "[recycle] ... since the last line")
static int recycle_recheck(void) {
    static int lv = -1;
    if (lv < 0) {
        const char *e = getenv("LARECOMP_RECYCLE_RECHECK");
        g_rc_set = e != NULL;
        lv = e ? atoi(e) : 1;
        // (the gate's line: '[recycle] same-storage re-check on (default 1)' in a package run without the variable)
        if (lv) LOG("[recycle] same-storage re-check on (%s%d): a new object on a pooled image of its storage is checked at its first use%s\n",
                    e ? "LARECOMP_RECYCLE_RECHECK=" : "default ", lv, lv >= 2 ? "; a GPU-owned one whose storage the CPU wrote since is a CPU texture" : "");
    }
    return lv;
}
static uint64_t st_rc_cpu, st_rc_new, st_rc_own, st_rc_own_cpu, st_rc_own_drop;
extern int heap_ww_changed(uint64_t addr, uint64_t size, uint64_t *seen);
// (a GPU-owned image's storage asked on its interior pages only: our write-back of a neighbour sharing an edge page
// (wb_swizzle; wb_neighbours absorbs it only for pending batches) bumps that page's epoch as a CPU write would, while a
// streamed tile or a defrag memcpy writes the whole range. -1: no interior page, as no watch)
static int rc_own_ww(uint64_t addr, uint64_t size, uint64_t *seen) {
    const uint64_t a = (addr + 0xFFF) & ~0xFFFull, e = (addr + size) & ~0xFFFull;
    return e > a ? heap_ww_changed(a, e - a, seen) : -1;
}
// device.c's "[vram] images" line: the re-check's counts since the last line (an arm reads a rate, not silence; 375-c SHOULD 1)
void vk_recycle_recheck_line(void) {
    if (!recycle_recheck() || !g_rc_set) return;
    static uint64_t l_c, l_n, l_o, l_oc, l_od;
    LOG("[recycle] same-storage re-check since the last line: %llu CPU images checked at first use, %llu had new bytes; %llu GPU-owned "
        "picks watched, %llu with CPU writes since the put, %llu made CPU textures\n",
        (unsigned long long)(st_rc_cpu - l_c), (unsigned long long)(st_rc_new - l_n), (unsigned long long)(st_rc_own - l_o),
        (unsigned long long)(st_rc_own_cpu - l_oc), (unsigned long long)(st_rc_own_drop - l_od));
    l_c = st_rc_cpu; l_n = st_rc_new; l_o = st_rc_own; l_oc = st_rc_own_cpu; l_od = st_rc_own_drop;
}
// (an image the recheck may act on when GPU-owned: unscaled colour; render-resolution targets and depth never take guest bytes)
static bool rc_own_kind(const TexBackend *b) { return b->aspect == VK_IMAGE_ASPECT_COLOR_BIT && !b->scalable; }
static void free_backend(TexBackend *b) { destroy_image(b); free(b->pending_rgba); free(b); }
// Pool byte cap. A fixed 512 MB could not hold what aiming the bow makes, each aim added ~1.6-2.1 GB of render targets in
// 44-172 new images (rendered 1163 -> 3309 MB at the 4th aim, 172 images created), the game finalized them minutes later,
// the pool kept 512 MB of them, and the next aim created them all again (104 / 59 ms aim frames). With room under the VRAM
// budget the pool may hold 3/4 of that room (at least 512 MB, at most LARECOMP_VK_RECYCLE_MAX_MB, default 3072); the
// residency tick recomputes it, and short of VRAM or commit it is 512 MB again.
static uint64_t g_recycle_cap = 512ull << 20;
static int recycle_cap_fixed(void) {   // 1: LARECOMP_VK_RECYCLE_MB set (g_recycle_cap is it)
    static int f = -1;
    if (f < 0) {
        const char *e = getenv("LARECOMP_VK_RECYCLE_MB");
        f = e != NULL;
        if (e) g_recycle_cap = (uint64_t)atol(e) << 20;
    }
    return f;
}
static uint64_t recycle_cap(void) { recycle_cap_fixed(); return g_recycle_cap; }
// The hot transients: short of VRAM the cap fell to 512 MB and the pool to 0-177 MB, so the targets TotK finalizes and makes
// again every frame were freed at each put and made again at each get (349 / 847 image creations per [vram] line against 33
// at 16:9; 19 / 42 fps): those frees saved nothing. The bytes taken back from the pool each frame that had been pooled under
// 2 s, and the bytes the cap evicted that had been pooled under 2 s (the two make the frame's transient set: a pool smaller
// than the set hits ~never), are kept for 2 s; the cap never goes under 5/4 of their largest frame (at most half the
// budget's top), short or not, so the pool holds one frame's transients and the shortage falls on idle textures and mirrors.
// LARECOMP_POOL_HOT=0: no floor (old).
static uint64_t hot_frame, hot_bytes, hot_ring_b[64], hot_ring_t[64];
static int hot_ring_n;
static uint64_t hot_floor(void) {   // (render thread) 5/4 of the largest frame's transients in the last 2 s; 0 = off
    static int on = -1;
    if (on < 0) { const char *e = getenv("LARECOMP_POOL_HOT"); on = !(e && *e == '0'); }
    if (!on) return 0;
    const uint64_t now = SDL_GetTicksNS();
    uint64_t m = 0;
    for (int i = 0; i < hot_ring_n; i++) if (now - hot_ring_t[i] <= 2000000000ull && hot_ring_b[i] > m) m = hot_ring_b[i];
    return m / 4 * 5;
}
static void hot_note(uint64_t bytes, uint64_t put_ns) {   // (recycle_get: an image taken back; recycle_put: one the cap evicted)
    const uint64_t f = vk_frame_counter(), now = SDL_GetTicksNS();
    if (f != hot_frame) {   // (the frame before ended: its sum into the ring, oldest out)
        if (hot_bytes) {
            if (hot_ring_n == 64) { memmove(hot_ring_b, hot_ring_b + 1, 63 * 8); memmove(hot_ring_t, hot_ring_t + 1, 63 * 8); hot_ring_n--; }
            hot_ring_b[hot_ring_n] = hot_bytes; hot_ring_t[hot_ring_n++] = now;
        }
        hot_frame = f;
        hot_bytes = 0;
    }
    if (now - put_ns < 2000000000ull) hot_bytes += bytes;
}
static bool pool_rung(const Recycled *r);
static uint64_t shadow_pool_keep_mb(void);
static void recycle_cap_update(uint64_t usage, uint64_t budget, uint64_t headroom, bool short_) {
    if (recycle_cap_fixed()) return;
    static uint64_t cmax;
    if (!cmax) { const char *e = getenv("LARECOMP_VK_RECYCLE_MAX_MB"); cmax = (uint64_t)(e ? atol(e) : 3072) << 20; if (cmax < (512ull << 20)) cmax = 512ull << 20; }
    uint64_t pool = g_recycle_bytes, rungs = 0;
    pthread_mutex_lock(&g_recycle_lock);   // (pooled parked images are system memory: not part of the driver's VRAM usage)
    for (int i = 0; i < g_nrecycle; i++) {
        if (g_recycle[i].b->parked) pool -= vma_shim_size(g_recycle[i].b->mem);
        else if (pool_rung(&g_recycle[i])) rungs += vma_shim_size(g_recycle[i].b->mem);   // (the shadow maps' rungs, below)
    }
    pthread_mutex_unlock(&g_recycle_lock);
    uint64_t rest = usage > pool ? usage - pool : 0, top = budget > 2 * headroom ? budget - 2 * headroom : 0;
    uint64_t c = !short_ && top > rest ? (top - rest) / 4 * 3 : 0;
    g_recycle_cap = c < (512ull << 20) ? 512ull << 20 : c > cmax ? cmax : c;
    uint64_t hf = hot_floor();
    if (hf > top / 2) hf = top / 2;   // (at most half the budget's top: a load's burst cannot pin the pool while short; 344-c)
    // (plus the pooled shadow-map rungs the keep holds, up to LARECOMP_SHADOW_POOL_KEEP_MB: they sit inside the same cap,
    // so without them the transients got cap - rungs, less than their set, and thrashed again)
    const uint64_t keep_raw = shadow_pool_keep_mb();
    if (hf) hf += rungs < keep_raw ? rungs : keep_raw;   // (only with pool-hot's floor on: LARECOMP_POOL_HOT=0 = the old cap; 359-c)
    const bool floored = hf > g_recycle_cap;
    if (floored) g_recycle_cap = hf;   // (never under one frame's transients: hot_floor)
    if (floored && short_) {   // (amd-compat: say it loudly when the floor holds memory the budget wanted; once per 10 s)
        static uint64_t said_ns;
        const uint64_t now = SDL_GetTicksNS();
        if (!said_ns || now - said_ns >= 10000000000ull) {
            said_ns = now;
            LOG("[vram] short of VRAM: one frame's transient targets (and the kept shadow-map rungs) need %llu MB of pool and the budget leaves %llu MB for it "
                "(usage %llu of %llu MB): the pool keeps them (no re-creation thrash), the rest is over budget; a lower "
                "graphics.render_scale or aspect ratio fits\n", (unsigned long long)(hf >> 20), (unsigned long long)(c >> 20),
                (unsigned long long)(usage >> 20), (unsigned long long)(budget >> 20));
        }
    }
    static uint64_t logged;
    static int nlog;
    if ((g_recycle_cap > logged + (256ull << 20) || g_recycle_cap + (256ull << 20) < logged) && (nlog++ < 50 || nlog % 100 == 0)) {
        LOG("[vram] recycle pool cap %llu MB (pool %llu MB, usage %llu of budget %llu MB%s%s)\n", (unsigned long long)(g_recycle_cap >> 20),
            (unsigned long long)(pool >> 20), (unsigned long long)(usage >> 20), (unsigned long long)(budget >> 20), short_ ? ", short" : "",
            floored ? ", held at 5/4 of a frame's transients (LARECOMP_POOL_HOT) + the kept shadow-map rungs" : "");
        logged = g_recycle_cap;
    }
}
// LARECOMP_VK_RECYCLE_KEEP_MS (default 1000; 0 = the old rule): pooled images younger than that stay through a residency round
static uint64_t recycle_keep_ns(void) {
    static int64_t k = -1;
    if (k < 0) { const char *e = getenv("LARECOMP_VK_RECYCLE_KEEP_MS"); k = (int64_t)(e ? atol(e) : 1000) * 1000000; }
    return (uint64_t)k;
}
// Bow aim: while the aim zooms, the game sizes the sun's shadow map with the view, a new rung image at each step (3584^2 /
// 4096^2 x4 at shadow scale x4: 98 / 128 MB, 8-80 ms each under VRAM contention); the first aim made ~29 such images in 12 s
// (+3.1 GB GPU-written, over the OS budget, 145 -> 50-97 fps) because the byte cap, short of VRAM, evicted each pooled map
// oldest-first as any other image (its 4096 map of frame 9109 was made again at the aim, 27k frames later). Pooled shadow
// maps' rung images now stay through the byte cap and the residency rounds while together they hold at most
// LARECOMP_SHADOW_POOL_KEEP_MB (default 384: the four rungs a x4 aim steps through, 50 + 72 + 98 + 128 MB; 0 = the old
// rule). Out of device memory (oom_relief) still frees them. (at most half the pool's cap: 256 MB of the short 512; within
// it the most recently pooled rungs stay, the oldest go first)
static uint64_t shadow_pool_keep_mb(void) {   // (the setting, bytes)
    static int64_t k = -1;
    if (k < 0) { const char *e = getenv("LARECOMP_SHADOW_POOL_KEEP_MB"); k = (int64_t)(e ? atol(e) : 384) << 20; }
    return (uint64_t)k;
}
static uint64_t shadow_pool_keep(void) {
    const uint64_t k = shadow_pool_keep_mb(), cap = recycle_cap();
    return cap && k > cap / 2 ? cap / 2 : k;
}
static bool pool_rung(const Recycled *r) { return r->scalable && r->b->scale_kind == 2 && shadow_rungs() && r->levels <= 1; }
static uint64_t pool_rung_bytes(void) {   // (under g_recycle_lock)
    uint64_t n = 0;
    for (int i = 0; i < g_nrecycle; i++) if (pool_rung(&g_recycle[i])) n += vma_shim_size(g_recycle[i].b->mem);
    return n;
}
static bool recycle_put(NvnTexture *t, TexBackend *b) {
    if (recycle_off() || !b->image || b->pending_rgba) return false;
    // Allocation pressure: every pooled image is one vkAllocateMemory of the VMA stand-in, and the Windows driver fails
    // near 4096 live allocations. 3-min boots reached 2,500 and climbing (gc-k1c0, gc-k4d; up to 464 of them pooled), so
    // past LARECOMP_VK_ALLOC_SOFT (3000) live allocations a finalized image is freed instead of pooled.
    static int64_t soft = -1;
    if (soft < 0) { const char *e = getenv("LARECOMP_VK_ALLOC_SOFT"); soft = e ? atoll(e) : 3000; }
    extern uint32_t vma_shim_live(void);
    // Over it, finalized images are still pooled, but the pool keeps at most 64 (oldest freed first LARECOMP_VK_SOFT_EVICT=0:
    // every finalized image freed, the old way). TotK re-creates the same transient targets every frame: pooled, each comes
    // back next frame with no new allocation; freed, each one is allocated again, so refusing the pool never lowered the live
    // count, it only added a vkCreateImage per target. What the limit bounds is the pool's growth.
    static int soft_evict = -1;
    if (soft_evict < 0) { const char *e = getenv("LARECOMP_VK_SOFT_EVICT"); soft_evict = !(e && *e == '0'); }
    if (soft && vma_shim_live() > (uint64_t)soft) {
        static int nlog;
        if (nlog++ < 4) LOG("[vram] %u live allocations, over LARECOMP_VK_ALLOC_SOFT=%lld: %s\n", vma_shim_live(), (long long)soft,
                            soft_evict ? "the recycle pool keeps at most 64 images" : "finalized images are freed, not pooled");
        if (!soft_evict) { cum_img_unpooled++; return false; }
        TexBackend *old[8];
        int nold = 0;
        pthread_mutex_lock(&g_recycle_lock);
        while (g_nrecycle >= 64 && nold < 8) {
            old[nold] = g_recycle[0].b;
            memmove(g_recycle, g_recycle + 1, (size_t)(g_nrecycle - 1) * sizeof *g_recycle);
            g_nrecycle--;
            g_recycle_bytes -= vma_shim_size(old[nold++]->mem);
        }
        pthread_mutex_unlock(&g_recycle_lock);
        for (int i = 0; i < nold; i++) free_backend(old[i]);
    }
    extern uint32_t vk_view_gen;
    vk_view_gen++;   // exec.c's per-frame view memo is keyed by the NvnTexture pointer, which the guest may reuse
    // Byte cap. Oldest go first. The cap follows the VRAM budget's free room (recycle_cap_update, residency tick);
    // LARECOMP_VK_RECYCLE_MB=<n>: a fixed cap (0 = none; 512 = the old default).
    const uint64_t cap = recycle_cap();
    uint64_t ww = 0;   // (LARECOMP_RECYCLE_RECHECK: the CPU's writes into a GPU-owned image's storage from here on, recycle_get)
    const bool ww_ok = recycle_recheck() && g_rc_set && b->gpu_owned && rc_own_kind(b) && t->addr &&
                       rc_own_ww(t->addr, tegra_total_size(&t->layout), &ww) >= 0;
    TexBackend *evict[8];
    uint64_t ev_put[8];
    int nev = 0;
    pthread_mutex_lock(&g_recycle_lock);
    g_recycle_bytes += vma_shim_size(b->mem);
    const uint64_t keep = shadow_pool_keep();
    uint64_t rung_bytes = keep && cap && g_recycle_bytes > (uint64_t)cap ? pool_rung_bytes() : 0, kept = 0;
    while (g_nrecycle && nev < 8 && (g_nrecycle == RECYCLE_MAX || (cap && g_recycle_bytes > (uint64_t)cap))) {
        int v = 0;   // (the oldest, past the shadow maps' kept rungs: shadow_pool_keep; a full table takes the oldest)
        if (g_nrecycle < RECYCLE_MAX)
            while (v < g_nrecycle && rung_bytes && rung_bytes <= keep && pool_rung(&g_recycle[v])) v++;
        if (v) kept = rung_bytes;
        if (v == g_nrecycle) break;   // (only kept rungs left)
        const uint64_t sz = vma_shim_size(g_recycle[v].b->mem);
        if (pool_rung(&g_recycle[v])) rung_bytes = rung_bytes > sz ? rung_bytes - sz : 0;
        ev_put[nev] = g_recycle[v].put_ns;
        evict[nev++] = g_recycle[v].b;
        memmove(&g_recycle[v], &g_recycle[v + 1], (size_t)(g_nrecycle - 1 - v) * sizeof *g_recycle);
        g_nrecycle--;
        g_recycle_bytes -= sz;
    }
    g_recycle[g_nrecycle++] = (Recycled){b, t->format, t->target, t->width, t->height, t->depth, t->levels, t->samples, b->scalable, b->nsx, b->nsy,
                                         vk_pending_seq(), t->addr, b->gpu_owned, SDL_GetTicksNS(), ww, ww_ok};
    pthread_mutex_unlock(&g_recycle_lock);
    if (kept) {   // (the gate's bow-aim check: 1st, 2nd, 4th, 8th ... time)
        static uint64_t nkept;
        const uint64_t n = ++nkept;
        if (!(n & (n - 1)))
            LOG("[vram] recycle pool over its cap %llu MB: %.0f MB of shadow-map rungs kept (LARECOMP_SHADOW_POOL_KEEP_MB; %llu times so far)\n",
                (unsigned long long)(cap >> 20), kept / 1048576.0, (unsigned long long)n);
    }
    // (a young eviction, pooled under 2 s, is a transient freed for nothing: with the set over the cap, oldest-first
    // eviction and TotK's same order every frame make the pool a FIFO that hits ~never, so the set is hits + these)
    for (int i = 0; i < nev; i++) { hot_note(vma_shim_size(evict[i]->mem), ev_put[i]); free_backend(evict[i]); }
    return true;
}
static bool g_recycle_same;   // (debug log: the last recycle_get kept the same storage's image)
static bool recycle_shape(const Recycled *r, const NvnTexture *t, bool sc) {
    uint32_t gw, gh;   // (a grown depth, renderer_texture_grow: never handed an image of the size before it grew; the
                       // game re-creates the object every frame and the pool kept giving it the short one)
    const int k = sc ? tex_scale_kind(t) : 0;
    if (sc && __atomic_load_n(&g_grow.n, __ATOMIC_RELAXED) && tex_grown(t, k, &gw, &gh) && (r->b->width < gw || r->b->height < gh)) return false;
    if (r->format != t->format || r->target != t->target || r->d != t->depth || r->levels != t->levels || r->samples != t->samples ||
        r->scalable != sc || (sc && (r->sx != kind_sx(k) || r->sy != kind_sy(k))))
        return false;
    if (sc && rung_kind(t, k) && r->b->scale_kind == k) {   // (a shadow map: the image of its rung, whatever the guest size)
        uint32_t hw, hh;
        scaled_extent(t, k, &hw, &hh);
        return r->b->width == hw && r->b->height == hh;
    }
    return r->w == t->width && r->h == t->height;
}
// a recycled shadow rung's scale for the texture it now backs (its own ratio, as create_image_ sets it)
static void rung_ratio(const NvnTexture *t, TexBackend *b) {
    if (!b->scalable || !rung_kind(t, b->scale_kind)) return;
    b->sx = (float)b->width / (float)(t->width ? t->width : 1);
    b->sy = (float)b->height / (float)(t->height ? t->height : 1);
}
static TexBackend *recycle_get(NvnTexture *t) {
    if (recycle_off()) return NULL;
    // Same storage first: an NVN texture is a view of pool memory, so a new object over the storage of a finalized one
    // sees what that one rendered. TotK re-creates its render targets every frame and reads the previous frame's
    // results through the new objects. The image used at this address with this shape comes back with its contents and
    // GPU ownership (no upload of the stale guest copy). Other shapes/addresses take the OLDEST pooled image, leaving
    // recent ones to their own storage. LARECOMP_VK_RECYCLE_ANY=1 = the old shape-only, newest-first reuse.
    static int any = -1;
    if (any < 0) any = getenv("LARECOMP_VK_RECYCLE_ANY") != NULL;
    bool sc = tex_scalable(t);
    TexBackend *b = NULL;
    bool same = false, owned = false;
    uint64_t cur = vk_pending_seq();
    // (a shadow rung taken as a fresh texture is zeroed when taken, on the main command buffer outside a pass, below: while
    // a pass is being recorded no such pick, the texture gets a new image as before)
    const bool rung_t = sc && rung_kind(t, tex_scale_kind(t)), can_clear = !vk_pass_open();
    pthread_mutex_lock(&g_recycle_lock);
    int pick = -1;
    if (!any && t->addr)
        for (int i = g_nrecycle - 1; i >= 0; i--) {
            Recycled *r = &g_recycle[i];
            if (r->addr == t->addr && recycle_shape(r, t, sc) && (r->gpu_owned || r->seq < cur)) {
                // (a shadow rung at a new guest size is a fresh texture on the same image: zeroed when taken, never the
                // old size's depth at the old ratio)
                const bool sz = r->w == t->width && r->h == t->height;
                if (rung_t && !sz && !can_clear) continue;
                pick = i; same = sz; owned = r->gpu_owned;
                break;
            }
        }
    if (pick < 0) {
        for (int k = 0; k < g_nrecycle; k++) {
            int i = any ? g_nrecycle - 1 - k : k;   // (old behaviour: newest first)
            Recycled *r = &g_recycle[i];
            if (!recycle_shape(r, t, sc) ||
                r->seq >= cur ||   // finalized in the open submission: its last uses may still be recorded after our uploads run
                r->b->parked ||    // (in system memory: only its own storage takes it back, for its contents; residency park)
                (rung_t && !can_clear && (r->b->aspect & VK_IMAGE_ASPECT_DEPTH_BIT)))   // (zeroed when taken: not in a pass)
                continue;
            pick = i;
            break;
        }
    }
    // A shadow rung finalized in the open submission (gate g19_see_kak_169: the game finalizes the sun shadow map and makes
    // its next size in the same frame, so the rule above refused the rung's own image and a 72 MB image was made again,
    // 10-16 ms, then ~50 GPU-slow frames). Taken only when it is a depth image the GPU rendered (a render target: nothing
    // uploads guest texels into it), and given fresh state below: its pass ended, then on this submission's main command
    // buffer (where those uses are recorded) a barrier from UNDEFINED (contents discarded) and a clear to zero, then a full
    // barrier; so it never shows the old depth and nothing writes it before those uses ran. LARECOMP_SHADOW_OPEN_PICK=0:
    // refused.
    bool open_pick = false;
    if (pick < 0 && !any && rung_t && can_clear && shadow_open_pick_on())
        for (int i = 0; i < g_nrecycle; i++) {
            const Recycled *r = &g_recycle[i];
            if (r->seq >= cur && r->gpu_owned && !r->b->parked && (r->b->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) &&
                r->b->scale_kind == 2 && recycle_shape(r, t, sc)) { pick = i; open_pick = true; break; }
        }
    if (pick < 0 && rung_t) {   // (why a shadow map found no image: device.c's "[vram] images" line)
        // 0 its rung finalized in the open submission, same address; 1 the same, another address; 2 parked; 3 only other
        // rungs of its format / layers; 4 none; 5 a pass was being recorded (in place of 0 / 1 / 6); 6 its rung matched,
        // unparked, from an earlier submission, yet not taken
        bool f[7] = {0};
        for (int i = 0; i < g_nrecycle; i++) {
            const Recycled *r = &g_recycle[i];
            if (r->b->scale_kind != 2 || r->format != t->format || r->d != t->depth) continue;
            f[!recycle_shape(r, t, sc) ? 3 : r->b->parked ? 2 : r->seq >= cur ? (r->addr == t->addr ? 0 : 1) : 6] = true;
        }
        int why = f[0] ? 0 : f[1] ? 1 : f[6] ? 6 : f[2] ? 2 : f[3] ? 3 : 4;
        if (!can_clear && (why <= 1 || why == 6)) why = 5;
        shadow_miss[why]++;
    }
    uint64_t pick_put_ns = 0, pick_seq = 0, pick_ww = 0;
    bool pick_ww_ok = false;
    if (pick >= 0) {
        b = g_recycle[pick].b;
        pick_put_ns = g_recycle[pick].put_ns;
        pick_seq = g_recycle[pick].seq, pick_ww = g_recycle[pick].ww, pick_ww_ok = g_recycle[pick].ww_ok;
        g_recycle_bytes -= vma_shim_size(b->mem);
        memmove(&g_recycle[pick], &g_recycle[pick + 1], (size_t)(g_nrecycle - 1 - pick) * sizeof *g_recycle);
        g_nrecycle--;
    }
    pthread_mutex_unlock(&g_recycle_lock);
    if (!b) return NULL;
    hot_note(vma_shim_size(b->mem), pick_put_ns);   // (the frame's transient set: recycle_cap_update's floor)
    prof_img_reused++;
    cum_img_reused++;
    if (same) {   // same memory: keep contents and state (only the live-list slot is new)
        b->reg = 0;   // (write-back state kept: a target re-created over the same storage every frame stays "per-frame";
        b->zero_on_write = false;
        b->fresh_color = false;   // (its contents are this storage's own)
        b->gpu_owned = owned;
        prof_img_same_addr++;
        g_recycle_same = true;
        rung_ratio(t, b);
        const int rc = recycle_recheck();
        if (rc && !owned) {   // (checked at its first use: upload_texture_)
            b->recycle_check = true;
            b->checked_frame = 0;
            st_rc_cpu++;
        } else if (rc && owned && pick_ww_ok) {
            st_rc_own++;
            uint64_t seen = pick_ww;
            if (rc_own_ww(t->addr, tegra_total_size(&t->layout), &seen) > 0) {
                st_rc_own_cpu++;
                const bool act = rc >= 2 && pick_seq < cur;   // (finalized in the open submission: its draws there run after our upload)
                if (act) {
                    b->gpu_owned = b->uploaded = false;
                    b->twin_valid = false;
                    st_rc_own_drop++;
                }
                if (st_rc_own_cpu <= 64 || !(st_rc_own_cpu & (st_rc_own_cpu - 1)))   // (the first 64, then the 128th, 256th ...)
                    LOG("[recycle] same-storage GPU-owned image at %llx %ux%u f%#x: the CPU wrote its storage since the put%s (frame %llu; %llu of %llu owned picks, %llu made CPU textures)\n",
                        (unsigned long long)t->addr, t->width, t->height, t->format,
                        act ? ": a CPU texture now" : rc >= 2 ? ": kept, finalized in the open submission" : ": kept (=2 acts)",
                        (unsigned long long)vk_frame_counter(), (unsigned long long)st_rc_own_cpu, (unsigned long long)st_rc_own,
                        (unsigned long long)st_rc_own_drop);
            }
        }
        return b;
    }
    // fresh texture state on the same image and views
    b->wb_frame = 0; b->wb_state = 0;
    b->gpu_owned = b->uploaded = b->dynamic = b->cpu_dirty = b->presented = false;
    b->b2t_levels = 0;
    b->data_hash = b->sample_hash = b->checked_frame = b->full_frame = b->touched_frame = b->frame_cleared = 0;
    b->level_gen = 0; b->tp = 0; b->tp_gen = 0; b->reg = 0; b->recycle_check = false;
    vk_full_barrier(vk_upload_cmd());
    // depth / stencil start zeroed as new images do (create_image_): cleared by the first pass that renders to it
    b->zero_on_write = (b->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) != 0 || (b->aspect == VK_IMAGE_ASPECT_STENCIL_BIT && b->format != VK_FORMAT_S8_UINT);
    b->fresh_color = b->aspect == VK_IMAGE_ASPECT_COLOR_BIT;
    if (b->scalable && rung_kind(t, b->scale_kind) && (b->aspect & VK_IMAGE_ASPECT_DEPTH_BIT)) {
        // (a shadow rung taken as a fresh texture: from the open submission (open_pick) or at a new guest size. Zeroed
        // now, not at its first write, so no read before that write sees the old map, outside a pass: from UNDEFINED,
        // then the clear, then a full barrier. The GPU owns it from here: no guest upload, which the upload buffer
        // would run before those uses.)
        vk_end_pass();
        VkCommandBuffer cb = vk_cmd();
        const VkImageSubresourceRange rg = {b->aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
        VkImageMemoryBarrier2 ib = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                                    .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                    .srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                                    .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                    .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                                    .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL, .image = b->image,
                                    .subresourceRange = rg, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED};
        VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &ib};
        vkCmdPipelineBarrier2(cb, &di);
        const VkClearDepthStencilValue zero = {0.0f, 0};
        vkCmdClearDepthStencilImage(cb, b->image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &rg);
        vk_full_barrier(cb);
        b->gpu_owned = true;
        b->zero_on_write = false;
        if (open_pick) shadow_open_picks++;
        else shadow_fresh_cleared++;
    }
    rung_ratio(t, b);
    return b;
}
// Out of device memory making an image (support #22, GTX 1650 Super 4 GB, v0.1.8.2: '[vk] image creation failed (-2):
// nvn fmt 0x32 944x944x4', then the RGBA8 retry's FATAL, every few minutes, with 321 MB of finalized images still in the
// recycle pool and usage at 3.9 of the heap's 3.93 GB): before that, once, wait for the submitted work, free the pool's
// images finalized before the open submission (no recorded or pending work uses them: their frees need not wait, inside a
// block too, where the next image then fits), the frees the per-frame limit held back, and the idle spares, and try
// again. A frame's stall instead of a crash. LARECOMP_OOM_RELIEF=0: straight to the old fallback.
static uint64_t oom_relief(void) {
    const uint64_t cur = vk_pending_seq();
    vk_wait_seq(cur - 1);
    static TexBackend *old[RECYCLE_MAX];
    int n = 0, k = 0;
    uint64_t pool = 0;
    pthread_mutex_lock(&g_recycle_lock);
    for (int i = 0; i < g_nrecycle; i++) {
        if (g_recycle[i].seq < cur) {
            const uint64_t sz = vma_shim_size(g_recycle[i].b->mem);
            old[n++] = g_recycle[i].b;
            g_recycle_bytes -= sz;
            pool += sz;
        } else g_recycle[k++] = g_recycle[i];
    }
    g_nrecycle = k;
    pthread_mutex_unlock(&g_recycle_lock);
    const int d0 = ndead;
    for (int i = 0; i < n; i++) free_backend(old[i]);
    for (int i = d0; i < ndead; i++) dead[i].seq = 0;   // (finalized before `cur`, its submissions waited for above)
    const int held = ndead;
    vk_collect_dead_(true);
    extern void vma_shim_drop_spares(void);
    vma_shim_drop_spares();
    static int nlog;
    if (nlog++ < 16)
        LOG("[vram] out of device memory: waited for the GPU, freed %d pooled images (%.0f MB) and %d held frees, dropped the "
            "spares (LARECOMP_OOM_RELIEF)\n", n, pool / 1048576.0, held - ndead);
    return pool;
}
// vmaCreateImage, and on VK_ERROR_OUT_OF_DEVICE_MEMORY once oom_relief and a second try (LARECOMP_OOM_RELIEF=0: no
// relief). The image sites on the render thread use it: here the game's images (create_image_, then its RGBA8 fallback),
// the depth and slice twins (a failure there drops the twin: the colour view is used), the block shadows (a failure leaves
// the BC target undrawn) and the 1x1 dummies; exec.c's snapshots, copy sources, post images and SMAA LUTs, latch.c's
// late-latch warp image and ngx.c's DLSS / FG images (fatal as before, after the relief). All on the render thread. `what`
// (printf) names the image, formatted only then. LARECOMP_OOM_INJECT=<n> (test, the idea): the n-th call (from 1) fails
// once as out of device memory before any creation, so one boot on any card runs the relief and the retry (a real out of
// device memory needs a small card).
VkResult vk_image_create_relieved(const VkImageCreateInfo *ci, const VmaAllocationCreateInfo *ai, VkImage *img, VmaAllocation *mem,
                                  const char *what, ...) {
    static long inject = -1;
    static _Atomic long calls;
    if (inject < 0) { const char *e = getenv("LARECOMP_OOM_INJECT"); inject = e ? atol(e) : 0; }
    const bool injected = inject > 0 && atomic_fetch_add(&calls, 1) + 1 == inject;
    VkResult r = injected ? VK_ERROR_OUT_OF_DEVICE_MEMORY : vmaCreateImage(g_vk.vma, ci, ai, img, mem, NULL);
    if (r != VK_ERROR_OUT_OF_DEVICE_MEMORY) return r;
    static int relief = -1;
    if (relief < 0) { const char *e = getenv("LARECOMP_OOM_RELIEF"); relief = !(e && *e == '0'); }
    if (!relief) return r;
    oom_relief();
    r = vmaCreateImage(g_vk.vma, ci, ai, img, mem, NULL);
    static int nlog;
    if (nlog++ < 16) {
        char name[160];
        va_list ap;
        va_start(ap, what);
        vsnprintf(name, sizeof name, what, ap);
        va_end(ap);
        LOG("[vk] image creation out of device memory (%s%s): after the relief %s\n", name, injected ? "; LARECOMP_OOM_INJECT" : "",
            r == VK_SUCCESS ? "made" : "still failed");
    }
    return r;
}
static void recycle_flush(void) {
    pthread_mutex_lock(&g_recycle_lock);
    int n = g_nrecycle;
    g_nrecycle = 0;
    g_recycle_bytes = 0;
    pthread_mutex_unlock(&g_recycle_lock);
    for (int i = 0; i < n; i++) free_backend(g_recycle[i].b);
}

// A target whose size changed (graphics.render_scale / native_ui live, "auto" following the window) or whose usage a
// live setting changed (contact shadows' STORAGE): a new image, the old one's contents carried over by a blit (filtered
// colour, nearest depth / integer) on the upload command buffer, which runs ahead of this submission's draws. So the
// paused scene behind the menus and the temporal history survive a change made in the Options. Images a blit cannot
// carry (multisampled, block-compressed shadows, depth / colour alternates, 3D, formats without blit support) start
// blank, as every remake did before. LARECOMP_RESCALE_CARRY=0: never carried.
static void rescale_image(NvnTexture *t, TexBackend *b) {
    const Carry c = carry_take(b);
    destroy_image(b);   // (the old image is freed after this submission, which holds the copy: defer)
    create_image(t, b);
    bool linear = false;
    if (!carry_blit(vk_upload_cmd(), &c, b, &linear)) return;
    const uint32_t ow = c.w, oh = c.h;
    const VkFormat of = c.fmt;
    static int nlog;
    if (nlog++ < 16)
        LOG("[tex] remade %ux%u -> %ux%u f%#x, contents carried (%s)\n", ow, oh, b->width, b->height, (unsigned)of, linear ? "filtered" : "nearest");
}
// Once per g_tex_gen (a live setting changed how targets are made): the 1920x1080 targets in or out of the output
// canvas (graphics.native_ui), the shadow maps in or out of kind 2 (graphics.shadow_scale to or from 1), the scene
// colour with or without STORAGE (graphics.contact_shadows)
static void tex_live_check(NvnTexture *t, TexBackend *b) {
    b->remake_gen = g_tex_gen;
    const int k = tex_scale_kind(t);
    if ((k == 3) != (b->scale_kind == 3) || (k == 2) != (b->scale_kind == 2)) { b->scale_kind = (uint8_t)k; b->scalable = k != 0; }
    const bool st_want = (b->usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) && contact_wants_storage(b->format) &&
                         format_supports(b->format, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT);
    const float wx = b->scalable ? kind_sx(b->scale_kind) : 1.0f, wy = b->scalable ? kind_sy(b->scale_kind) : 1.0f;
    if (b->image && (b->nsx != wx || b->nsy != wy || st_want != ((b->usage & VK_IMAGE_USAGE_STORAGE_BIT) != 0))) rescale_image(t, b);
}

// ---- public #16 (grass that will not stay cut): the grass cut masks 'MowS' / 'MowL' (256^2 / 512^2) and their '(CPU)'
// copies (128^2 / 256^2, created with memory attribute 8), agl format 0x1d = NVN RGBA8 0x25. Which way the CPU copies'
// texels go is not known; each says it once in "[mow]" lines: its label, flags and pool when made, then its first render
// target / copy destination, sample, upload from guest memory (first, changed: what guest memory held), write-back and
// the end of its write-backs, and a CPU write to a page of its storage while the GPU owns its image (no upload then: the
// GPU's image wins; the heap write watch, once every MOW_POLL frames on its own epoch, so an object sharing an edge 4 KB
// page can show too, and a shader's store does not). Tracked: a "Mow" label, or a square 128 / 256 one-level RGBA8
// texture in a CPU-visible pool (the copies' shape; other textures of that shape show too, MOW_MAX at most). A texture
// that has said everything, or is MOW_FRAMES frames old, is dropped; the hooks test g_mow_live, then a tracked texture's
// shape, before they look anything up. LARECOMP_MOW_LOG=0: off.
enum { MOW_RENDER = 1, MOW_COPY = 2, MOW_SAMPLE = 4, MOW_UP_FIRST = 8, MOW_UP_CHANGED = 16, MOW_WB = 32, MOW_WB_STOP = 64,
       MOW_CPU_OWNED = 128, MOW_ALL = 255 };
#define MOW_MAX 16
#define MOW_FRAMES 36000u   // (10 minutes at 60: the cut happens in play, minutes after the masks are made at load)
#define MOW_POLL 60u
typedef struct { NvnTexture *t; uint64_t addr, frame, ww, polled; uint8_t seen; } MowRec;
static MowRec g_mow[MOW_MAX];
static int g_mow_n, g_mow_live;
static bool mow_shape(const NvnTexture *t) {
    return t->format == 0x25 && t->width == t->height && (t->width == 128 || t->width == 256);
}
static void mow_track(NvnTexture *t, TexBackend *b) {
    static int on = -1, full;
    if (on < 0) { const char *e = getenv("LARECOMP_MOW_LOG"); on = !(e && *e == '0'); }
    const bool label = !strncmp(t->label, "Mow", 3);
    if (!on || !t->addr || !(label || (mow_shape(t) && t->levels <= 1 && t->pool && (t->pool->flags & 0x6)))) return;
    for (int i = 0; i < g_mow_n; i++) if (g_mow[i].t == t && g_mow[i].addr == t->addr) return;
    if (g_mow_n == MOW_MAX) { if (!full++) LOG("[mow] more textures of the masks' shape: only the first %d tracked\n", MOW_MAX); return; }
    g_mow[g_mow_n++] = (MowRec){t, t->addr, vk_frame_counter(), 0, 0, 0};
    g_mow_live++;
    LOG("[mow] made %p \"%s\" addr %llx +%llu %ux%u f%#x levels %u flags %#x %s, pool flags %#x (%s), Vulkan format %d "
        "(frame %llu)\n", (void *)t, t->label, (unsigned long long)t->addr, (unsigned long long)tegra_total_size(&t->layout),
        t->width, t->height, t->format, t->levels, t->flags, nvn_flags_linear(t->flags) ? "linear" : "block-linear",
        t->pool ? t->pool->flags : 0, !t->pool ? "none" : t->pool->flags & 0x4 ? "CPU-cached" : t->pool->flags & 0x2 ? "CPU-uncached" :
        "no CPU access", (int)b->format, (unsigned long long)vk_frame_counter());
}
static void mow_say(NvnTexture *t, MowRec *r, int ev, uint64_t frame) {
    r->seen |= (uint8_t)ev;
    const TexBackend *b = t->backend;
    LOG("[mow] %p %ux%u: %s, owned %d uploaded %d (frame %llu, %llu after made)\n", (void *)t, t->width, t->height,
        ev == MOW_RENDER ? "render target" : ev == MOW_COPY ? "copy / clear destination" : ev == MOW_SAMPLE ? "sampled or copy source" :
        ev == MOW_UP_FIRST ? "first upload from guest memory" : ev == MOW_UP_CHANGED ? "guest memory changed: uploaded again (a CPU "
        "write, or a shader's store)" : ev == MOW_WB ? "written back to guest memory" : ev == MOW_WB_STOP ? "rendered again in a "
        "later frame: no more write-backs" : "the CPU wrote a page of its storage while the GPU owns the image: not uploaded",
        b ? b->gpu_owned : -1, b ? b->uploaded : -1, (unsigned long long)frame, (unsigned long long)(frame - r->frame));
    if (ev & (MOW_UP_FIRST | MOW_UP_CHANGED | MOW_WB | MOW_CPU_OWNED)) {   // (once per texture and event: what guest memory holds)
        const uint8_t *m = (const uint8_t *)(uintptr_t)t->addr;
        const uint64_t n = tegra_total_size(&t->layout);
        uint64_t nz = 0;
        for (uint64_t k = 0; k < n; k++) nz += m[k] != 0;
        LOG("[mow]   %llu of %llu bytes in guest memory nonzero\n", (unsigned long long)nz, (unsigned long long)n);
    }
}
static void mow_note(NvnTexture *t, int ev) {
    const uint64_t frame = vk_frame_counter();
    for (int i = 0; i < g_mow_n; i++) {
        MowRec *r = &g_mow[i];
        if (r->t != t || r->addr != t->addr) continue;
        if (!(r->seen & ev)) mow_say(t, r, ev, frame);
        if (ev == MOW_WB) r->polled = 0;   // (our own write-back wrote the storage: the watch is armed again after it)
        const TexBackend *b = t->backend;
        if (b && b->gpu_owned && !(r->seen & MOW_CPU_OWNED) && frame >= r->polled + MOW_POLL) {
            // (its own epoch: the watch's page epochs are shared, so the upload path's check is not disturbed)
            extern int heap_ww_changed(uint64_t addr, uint64_t size, uint64_t *seen);
            const int ch = heap_ww_changed(t->addr, tegra_total_size(&t->layout), &r->ww);
            if (ch > 0 && r->polled) mow_say(t, r, MOW_CPU_OWNED, frame);
            r->polled = frame;   // (the first poll arms the epoch)
        }
        if (r->seen == MOW_ALL || frame > r->frame + MOW_FRAMES) { r->t = NULL; g_mow_live--; }   // (said everything, or past its window)
        break;
    }
}
#define MOW_NOTE(t, ev) do { if (g_mow_live && (mow_shape(t) || (t)->label[0] == 'M')) mow_note((t), (ev)); } while (0)

bool tex_watched(uint64_t addr);
static void residency_restore(NvnTexture *t, TexBackend *b);
static TexBackend *tex_backend(NvnTexture *t) {
    TexBackend *b = t->backend;
    if (b) {
        b->last_use = vk_frame_counter();
        if (b->evicted) residency_restore(t, b);
        if (b->remake_gen != g_tex_gen) tex_live_check(t, b);
        else if (b->scalable && (b->nsx != kind_sx(b->scale_kind) || b->nsy != kind_sy(b->scale_kind)))  // output resolution changed: reallocate
            rescale_image(t, b);
        return b;
    }
    g_recycle_same = false;
    b = recycle_get(t);
    bool reused = b != NULL;
    if (!b) {
        vk_collect_dead();
        b = calloc(1, sizeof *b);
        b->scale_kind = (uint8_t)tex_scale_kind(t);
        b->scalable = b->scale_kind != 0;
        b->remake_gen = g_tex_gen;
        create_image(t, b);
        b->fresh_color = b->aspect == VK_IMAGE_ASPECT_COLOR_BIT;   // (new memory: undefined contents, see renderer_texture_unwritten)
    }
    t->backend = b;
    b->last_use = vk_frame_counter();
    live_add(t, b);
    mow_track(t, b);
    extern bool g_debug_frame;
    if (g_debug_frame || tex_watched(t->addr))
        LOG("[tex] backend for %p addr %llx %ux%u f%#x: %s image %p\n", (void *)t, (unsigned long long)t->addr, t->width, t->height,
            t->format, reused ? (g_recycle_same ? "same-storage" : "recycled") : "new", (void *)b->image);
    return b;
}

// ---- aliases: NVN textures are views of pool memory. TotK renders an image through one texture object and samples
// it through another over the same storage. The last texture object rendered to at an address is its writer; a
// sampled object with the same address, size and texel size uses the writer's image. LARECOMP_VK_NO_ALIAS=1
// disables.
#define ALIAS_TAB 4096
static struct { uint64_t addr; NvnTexture *t; } g_alias[ALIAS_TAB];
static uint32_t alias_slot(uint64_t addr) { return (uint32_t)((addr >> 8) * 0x9E3779B97F4A7C15ull >> 52) & (ALIAS_TAB - 1); }
// A new writer at an address changes what its sampled objects show (image view) and their sample scale (rscale rows
// 1..n). exec.c's per-frame view memo keys by the writer (renderer_texture_alias_writer); the stage_set memo and the
// fragment-texture cache hold whole sets / image infos and key by vk_alias_gen. LARECOMP_ALIAS_GEN=0: no bump (old).
uint32_t vk_alias_gen;
uint64_t prof_alias_writer_changes;
static void alias_writer_changed(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("LARECOMP_ALIAS_GEN"); on = !e || *e != '0'; }
    if (!on) return;
    vk_alias_gen++;
    prof_alias_writer_changes++;
}
static void alias_note_writer(NvnTexture *t) {
    if (!t->addr) return;
    uint32_t s = alias_slot(t->addr);
    int freeslot = -1;
    for (int p = 0; p < 32; p++, s = (s + 1) & (ALIAS_TAB - 1)) {
        if (g_alias[s].addr == t->addr) {
            if (g_alias[s].t != t) { g_alias[s].t = t; alias_writer_changed(); }
            return;
        }
        if (!g_alias[s].t && freeslot < 0) freeslot = (int)s;   // empty or forgotten
        if (!g_alias[s].addr) break;
    }
    if (freeslot >= 0) { g_alias[freeslot].addr = t->addr; g_alias[freeslot].t = t; alias_writer_changed(); }
}
static void alias_forget(NvnTexture *t) {
    if (!t->addr) return;
    uint32_t s = alias_slot(t->addr);
    for (int p = 0; p < 32; p++, s = (s + 1) & (ALIAS_TAB - 1))
        if (g_alias[s].t == t) { g_alias[s].t = NULL; alias_writer_changed(); return; }   // (slot kept: probe chains stay intact; reused by the next writer)
}
// Alias misses (fps360 11-03: the gate's 'camera-elsewhere' frames show an OLDER scene image for one frame, the HUD
// current): a sampled object whose storage another object rendered last, but whose writer is gone (finalized) or no
// longer matches it (size / texel layout), reads its OWN image, last rendered whenever it was itself a writer, or its
// guest bytes. Counted per stats window, the first sampled objects logged ('[tex] alias miss'), log only.
static int g_amiss;   // (alias_writer -> vk_tex) 0: none, 1: the writer at the address is gone / not GPU-owned, 2: its shape differs
static NvnTexture *g_amiss_w;
static uint64_t st_amiss[2][2];   // [writer gone, shape differs][own image rendered earlier, guest bytes]
static uint64_t st_amiss_wbfmt;   // (of those 'shape differs': a written-back writer seen through another format)
// A writer whose last rendering was written back to its storage (wb_state 2: textures.c write-back, the terrain tiles'
// 260x260 RG8 normal maps) does not stand in for an object of ANOTHER format at its address: that object reads the
// storage. hdr 11-05: soak hits field 86985 / 147546, y4000 17294 / 22600, town 119355): the game hands such a tile's
// storage to a 260x260 R16 height tile (same size, 2 bytes a texel, so the shape test above matched) while the normal
// map's object is still the address's writer, and the terrain's vertex shader read the normal map as heights. The storage
// is exact either way: unchanged, it holds the written-back texels in the guest layout (what hardware would show through
// any format); reused, it holds the new texture's bytes. LARECOMP_ALIAS_WB_FMT=0: the writer stands in (the old rule).
static bool alias_wb_fmt_on(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("LARECOMP_ALIAS_WB_FMT"); on = !(e && *e == '0'); }
    return on;
}
static NvnTexture *alias_writer(NvnTexture *t) {
    static int off = -1;
    if (off < 0) off = getenv("LARECOMP_VK_NO_ALIAS") != NULL;
    g_amiss = 0;
    if (off || !t->addr) return NULL;
    uint32_t s = alias_slot(t->addr);
    for (int p = 0; p < 32; p++, s = (s + 1) & (ALIAS_TAB - 1)) {
        if (g_alias[s].addr != t->addr) { if (!g_alias[s].addr) return NULL; continue; }
        NvnTexture *w = g_alias[s].t;
        if (w == t) return NULL;
        if (!w || !w->backend || !((TexBackend *)w->backend)->gpu_owned) { g_amiss = 1, g_amiss_w = w; return NULL; }
        const FormatInfo *fa = t->layout.fmt, *fb = w->layout.fmt;
        if (w->width != t->width || w->height != t->height || w->target != t->target || w->samples != t->samples || !fa || !fb ||
            fa->bpb != fb->bpb || fa->bw != fb->bw || fa->bh != fb->bh ||
            renderer_format_is_depth(w->format) != renderer_format_is_depth(t->format)) {
            g_amiss = 2, g_amiss_w = w;
            return NULL;
        }
        // (only a CPU-filled object: one with a GPU-owned image of its own would read that older image, not the storage)
        if (w->format != t->format && ((TexBackend *)w->backend)->wb_state == 2 && alias_wb_fmt_on() &&
            !(t->backend && ((TexBackend *)t->backend)->gpu_owned)) {
            g_amiss = 2, g_amiss_w = w;   // (counted with 'shape differs', and apart: st_amiss_wbfmt)
            st_amiss_wbfmt++;
            static NvnTexture *said[64];   // (the first 64 sampled objects, one line each)
            static int nsaid;
            int i = 0;
            while (i < nsaid && said[i] != t) i++;
            if (i == nsaid && nsaid < 64) {
                said[nsaid++] = t;
                LOG("[tex] alias: %p (%ux%u f%#x) at %llx reads its storage, not writer %p (f%#x, written back): another format\n",
                    (void *)t, t->width, t->height, t->format, (unsigned long long)t->addr, (void *)w, w->format);
            }
            return NULL;
        }
        // one line per distinct sampled object (open-addressed set, <= 8 probes: this runs per sampled texture per draw;
        // the old linear scan of up to 256 entries was 2.2 % of the render thread)
        static NvnTexture *logged[512];
        uint32_t hs = (uint32_t)((uintptr_t)t >> 4) * 0x9E3779B1u >> 23;
        for (int i = 0; i < 8; i++) {
            NvnTexture **e = &logged[(hs + i) & 511];
            if (*e == t) break;
            if (!*e) {
                *e = t;
                LOG("[tex] alias: %p (%ux%u f%#x) samples writer %p (f%#x) at %llx\n", (void *)t, t->width, t->height, t->format,
                    (void *)w, w->format, (unsigned long long)t->addr);
                break;
            }
        }
        return w;
    }
    return NULL;
}

void vk_tex_alias_miss_stats(void) {   // (exec.c, beside the stats line)
    const uint64_t *m = &st_amiss[0][0];
    if (m[0] + m[1] + m[2] + m[3])
        LOG("[tex] alias misses: writer gone %llu (own image %llu, guest bytes %llu), shape differs %llu (own image %llu, guest bytes %llu)\n",
            (unsigned long long)(m[0] + m[1]), (unsigned long long)m[0], (unsigned long long)m[1], (unsigned long long)(m[2] + m[3]),
            (unsigned long long)m[2], (unsigned long long)m[3]);
    if (st_amiss_wbfmt)
        LOG("[tex] alias: written-back writers not standing in for another format: %llu lookups (LARECOMP_ALIAS_WB_FMT=0: they do)\n",
            (unsigned long long)st_amiss_wbfmt);
    memset(st_amiss, 0, sizeof st_amiss);
    st_amiss_wbfmt = 0;
}
bool renderer_texture_gpu_owned(NvnTexture *t) {
    const TexBackend *b = t ? (const TexBackend *)t->backend : NULL;
    return b && b->gpu_owned;
}
// another object's live rendering at t's storage (same size and texel layout) would stand in when t is sampled
bool renderer_texture_has_writer(NvnTexture *t) { return alias_writer(t) != NULL; }
NvnTexture *renderer_texture_alias_writer(NvnTexture *t) { return alias_writer(t); }   // (exec.c resolve_feedback)

static void wb_forget(NvnTexture *t);
static void lr_forget(NvnTexture *t);   // (LARECOMP_TEX_SUBMIT_RECHECK's list)
void backend_texture_destroyed(NvnTexture *t) {
    extern void vk_preupload_forget(NvnTexture *t);
    vk_preupload_forget(t);   // (preupload.c's queue: before the NvnTexture is freed)
    alias_forget(t);
    wb_forget(t);
    lr_forget(t);
    TexBackend *b = t->backend;
    extern bool g_debug_frame;
    if ((g_debug_frame || tex_watched(t->addr)) && b)
        LOG("[tex] finalize %p addr %llx %ux%u f%#x image %p\n", (void *)t, (unsigned long long)t->addr, t->width, t->height, t->format,
            (void *)b->image);
    if (b) {
        live_remove(b);
        contact_forget(b);   // (contact.c: pooled or freed, the pointer may come back as another texture's)
        vk_hue_forget(b);    // (exec.c's pending HDR hue capture, the same)
        if (!recycle_put(t, b)) free_backend(b);
    }
    t->backend = NULL;
}

// Reboot (device.c backend_reset; GPU idle): every game texture and its views go.
void vk_textures_reset(void) {
    int n = g_nlive;
    while (g_nlive > 0) backend_texture_destroyed(g_live[g_nlive - 1]);
    recycle_flush();
    vk_collect_dead_(true);   // (GPU idle: all of it)
    LOG("[vk] reboot: %d textures released\n", n);
}

// ================================================================== VRAM residency. A CPU-uploaded sampled texture is a
// copy of guest memory. When the device-local heap nears the OS budget (VK_EXT_memory_budget; without it, our own
// allocations against 85 % of the heap), the least recently used of those that have been idle for LARECOMP_VRAM_IDLE_MS
// (default 4000) drop their image; the next use re-creates it and uploads the guest data again: the same texels, so no
// visual change, only that upload's time. Rendered or GPU-written images (gpu_owned, block shadows, video, depth),
// multisampled ones and textures whose storage shaders wrote since their last hash (their guest copy may lag) are never
// evicted. Pooled recycle images go first. Nothing happens while usage stays under the budget (the RTX 5080: 15 GB budget,
// <= 8 GB used). LARECOMP_VRAM_RESIDENCY=0: off. LARECOMP_VRAM_BUDGET_MB=<n>: use n MB as the budget (an 8 GB card's ~6.5
// GB on a larger one, for measurements).
static uint64_t res_evicted_n, res_evicted_bytes, res_restored_n, res_restored_bytes, res_rounds;
static void views_forget_evicted(void);   // (views section: one scan of the view table for a round's evicted images)
uint32_t vma_shim_heaps(VmaAllocator a, uint64_t *bytes, uint32_t *count, uint64_t *size, uint32_t *flags);
static bool res_evictable(NvnTexture *t, TexBackend *b) {
    if (!b->image || !b->mem || !b->uploaded || b->gpu_owned || b->blk || b->pending_rgba || b->presented || b->samples != 1 ||
        b->wb_state || (b->aspect & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) || !t->addr || t->target == 10 ||
        !t->layout.fmt || !t->layout.fmt->bpb)
        return false;
    extern bool vk_heap_gpu_written_since(uint64_t addr, uint64_t size, uint64_t since);
    return b->full_seq && !vk_heap_gpu_written_since(t->addr, tegra_total_size(&t->layout), b->full_seq);
}
static void residency_restore(NvnTexture *t, TexBackend *b) {
    b->evicted = false;
    create_image(t, b);   // (uploaded = false: vk_tex's upload check copies the whole texture from guest memory)
    res_restored_n++;
    res_restored_bytes += vma_shim_size(b->mem);
}
// [vram] line: live game textures' images by kind. sampled = CPU-uploaded (evictable kinds), idle = of those, unused for
// idle_frames frames; rendered = everything else (render targets, GPU-written, depth, block shadows' parents).
void vk_tex_vram_split(uint64_t idle_frames, uint64_t bytes[3], uint32_t count[4]) {
    uint64_t now = vk_frame_counter();
    memset(bytes, 0, 3 * sizeof *bytes);
    memset(count, 0, 4 * sizeof *count);
    for (int i = 0; i < g_nlive; i++) {
        NvnTexture *t = g_live[i];
        TexBackend *b = t->backend;
        if (!b) continue;
        if (b->evicted) { count[3]++; continue; }
        uint64_t sz = vma_shim_size(b->mem) + (b->blk ? vma_shim_size(b->blk->mem) : 0);
        bool s = b->image && b->uploaded && !b->gpu_owned && !b->blk && !b->presented && b->samples == 1 &&
                 !(b->aspect & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT));
        int k = s ? 0 : 2;
        bytes[k] += sz; count[k]++;
        if (s && now - b->last_use >= idle_frames) { bytes[1] += sz; count[1]++; }
    }
}
// "[vram] slack" line (vram-fit): of the rendered kind (not evictable), the images unused for idle_frames frames
uint64_t vk_tex_rendered_idle(uint64_t idle_frames, uint32_t *count) {
    uint64_t now = vk_frame_counter(), bytes = 0;
    *count = 0;
    for (int i = 0; i < g_nlive; i++) {
        TexBackend *b = g_live[i]->backend;
        if (!b || b->evicted || b->parked || now - b->last_use < idle_frames) continue;
        bool s = b->image && b->uploaded && !b->gpu_owned && !b->blk && !b->presented && b->samples == 1 &&
                 !(b->aspect & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT));
        if (s) continue;
        bytes += vma_shim_size(b->mem) + (b->blk ? vma_shim_size(b->blk->mem) : 0);
        ++*count;
    }
    return bytes;
}
uint64_t vk_residency_idle_frames(void);   // (below: frames in the last LARECOMP_VRAM_IDLE_MS)
// Memory pressure as residency last measured it (every 250 ms, render thread): bytes still to free to get back under the
// VRAM budget's headroom or above twice the commit floor once this round's pool frees and evictions are counted; 0 = none,
// and 0 while a round's frees are pending (cool-down).
static volatile uint64_t res_pressure, res_need_ns;
static volatile bool res_pressure_commit;
uint64_t vk_residency_pressure(bool *commit) { if (commit) *commit = res_pressure_commit; return res_pressure; }
// SDL_GetTicksNS() of the last measurement that found pressure (need > 0, before this round's frees; 0 = never): VRAM or
// commit is short even when the round's own frees answer it
uint64_t vk_residency_need_ns(void) { return res_need_ns; }
// short now, or a measurement found pressure within the last 2 s (res_pressure reads 0 in a round's cool-down and once the
// round's frees answer it on paper, before they complete): shim_vma makes no big block or spare then
bool vk_residency_tight(void) { uint64_t t = res_need_ns; return res_pressure || (t && SDL_GetTicksNS() - t < 2000000000ull); }
static int res_cmp(const void *a, const void *b) {
    uint64_t x = ((const TexBackend *)((NvnTexture *const *)a)[0]->backend)->last_use;
    uint64_t y = ((const TexBackend *)((NvnTexture *const *)b)[0]->backend)->last_use;
    return x < y ? -1 : x > y;
}
static uint64_t res_hist_frame[32], res_hist_ns[32];   // (one sample every 250 ms: frame <-> time)
static int res_hist_n;
uint64_t vk_residency_idle_frames(void) {
    static int64_t idle_ns = -1;
    if (idle_ns < 0) { const char *e = getenv("LARECOMP_VRAM_IDLE_MS"); idle_ns = (int64_t)(e ? atol(e) : 4000) * 1000000; }
    uint64_t now = SDL_GetTicksNS(), fr = vk_frame_counter();
    for (int i = res_hist_n - 1; i >= 0; i--)
        if (now - res_hist_ns[i] >= (uint64_t)idle_ns) return fr - res_hist_frame[i];
    return ~0ull;   // not enough history yet: nothing is idle
}
static uint64_t res_frames_in(uint64_t ns) {   // frames presented in the last ns (~0: not enough history)
    uint64_t now = SDL_GetTicksNS(), fr = vk_frame_counter();
    for (int i = res_hist_n - 1; i >= 0; i--)
        if (now - res_hist_ns[i] >= ns) return fr - res_hist_frame[i];
    return ~0ull;
}
// ---- parking (vram-fit 2026-11-02; the 4K reports: an 8-10 GB card whose rendered images alone pass the budget). Rendered
// images (render targets, GPU-written) cannot be evicted: their contents exist only in VRAM. When the budget is short and
// nothing sampled is left to evict, WDDM demotes allocations of its own choosing to system memory, hot ones included
// (the frame rate drops and stays down). Instead the least recently used rendered images idle for LARECOMP_VRAM_IDLE_MS
// are moved there by us: a copy of the image in system memory (a memory type the GPU samples and renders through PCIe:
// NVIDIA's type 0, AMD's GTT) replaces it in place, same format, size and contents, so nothing drawn changes and the
// next use costs no wait (no copy back on the critical path; the reads are slower over PCIe while it stays there). Once
// the budget has room again (5 s after the last shortage, 3 x headroom free) park_return moves the recently used ones
// back, at most 64 MB a round. Colour, single-sample images only, not presented / block shadows / video / write-back
// pending / a depth texture's other kind. LARECOMP_VRAM_PARK=0: off.
static uint64_t park_n, park_nbytes, park_ret_n, park_ret_bytes;
static bool park_ok(void) {   // (an integrated GPU: system memory is all it has, nothing to gain)
    static int on = -1;
    if (on < 0) { const char *e = getenv("LARECOMP_VRAM_PARK"); on = !(e && *e == '0') && g_vk.props.deviceType != VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU; }
    return on;
}
static bool res_parkable(const NvnTexture *t, const TexBackend *b) {
    return b->image && b->mem && b->gpu_owned && !b->parked && !b->blk && !b->pending_rgba && !b->presented && b->samples == 1 &&
           b->aspect == VK_IMAGE_ASPECT_COLOR_BIT && b->wb_state != 1 && b->wb_state != 4 && !b->fbl_in && !b->alt.image && t->addr;
}
static uint32_t mem_types(bool vram) {   // the memory types on device-local heaps (vram) or the others
    static uint32_t bits[2];
    static bool got;
    if (!got) {
        VkPhysicalDeviceMemoryProperties mp;
        vkGetPhysicalDeviceMemoryProperties(g_vk.phys, &mp);
        for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
            bits[(mp.memoryHeaps[mp.memoryTypes[i].heapIndex].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0] |= 1u << i;
        got = true;
    }
    return bits[vram];
}
static uint64_t park_refused;   // (images no memory type of the other side takes, or none free)
// b's image copied into a new one in system memory (to_host) or VRAM, which takes its place; the old one is freed once this
// submission (the copy) completed. The copy is in the upload command buffer, which runs before the open submission's other
// work: safe because b has been idle for LARECOMP_VRAM_IDLE_MS (the real guarantee), and because the round runs after the
// frame's submissions (device.c), with the fronts and the vkq record thread joined by then. true: moved; false: no memory
// type of that side takes this image, or none had room (nothing changed)
static bool park_move(TexBackend *b, bool to_host) {
    VkImageType ty = b->base_type == VK_IMAGE_VIEW_TYPE_1D || b->base_type == VK_IMAGE_VIEW_TYPE_1D_ARRAY ? VK_IMAGE_TYPE_1D
                   : b->base_type == VK_IMAGE_VIEW_TYPE_3D ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .flags = b->cflags, .imageType = ty, .format = b->format,
                            .extent = {b->width, b->height, b->depth}, .mipLevels = b->levels, .arrayLayers = b->layers,
                            .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = b->usage,
                            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    // (the side's types only: AMD's GTT types carry more flags than its VRAM one, so a preference alone would pick VRAM)
    VmaAllocationCreateInfo ai = {.usage = to_host ? VMA_MEMORY_USAGE_AUTO_PREFER_HOST : VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
                                  .memoryTypeBits = mem_types(!to_host)};
    VkImage img;
    VmaAllocation mem;
    extern _Thread_local int vma_shim_tag;
    vma_shim_tag = 2;
    VkResult r = ai.memoryTypeBits ? vmaCreateImage(g_vk.vma, &ci, &ai, &img, &mem, NULL) : VK_ERROR_FEATURE_NOT_PRESENT;
    vma_shim_tag = 0;
    if (r != VK_SUCCESS) {
        if (park_refused++ < 8)
            LOG("[vram] park: no %s memory for a %ux%u format %d image (%d): it stays\n", to_host ? "system" : "device-local", b->width,
                b->height, b->format, r);
        return false;
    }
    VkCommandBuffer cb = vk_upload_cmd();
    vk_image_init_layout(cb, img, b->aspect);
    vk_full_barrier(cb);
    VkImageCopy rg[32];
    const uint32_t nl = b->levels < 32 ? b->levels : 32;
    for (uint32_t lv = 0; lv < nl; lv++) {
        uint32_t d = ty == VK_IMAGE_TYPE_3D && b->depth >> lv ? b->depth >> lv : 1;
        rg[lv] = (VkImageCopy){.srcSubresource = {b->aspect, lv, 0, b->layers}, .dstSubresource = {b->aspect, lv, 0, b->layers},
                               .extent = {level_w(b, lv), level_h(b, lv), d}};
    }
    vkCmdCopyImage(cb, b->image, VK_IMAGE_LAYOUT_GENERAL, img, VK_IMAGE_LAYOUT_GENERAL, nl, rg);
    vk_full_barrier(cb);
    defer((Dead){.img = b->image, .mem = b->mem});
    if (b->parked) { park_bytes -= vma_shim_size(b->mem); park_live--; }
    b->image = img; b->mem = mem; b->parked = to_host;
    if (to_host) { park_bytes += vma_shim_size(mem); park_live++; }
    b->snap_tag = 0;      // (exec.c snapshot delta: the new image is no snapshot's mirror)
    b->smark_ok = false;
    return true;          // (twin_valid stays: the twin holds the same contents)
}
static void views_forget_moved(void);   // (views section: one scan of the view table for a round's moved images)
static void park_forget_moved(int n, uint64_t bytes) {
    for (int i = 0; i < g_nlive; i++) {
        TexBackend *b = g_live[i]->backend;
        if (b && b->park_moved) contact_forget(b), vk_hue_forget(b);   // (contact.c / exec.c keep image pointers: told, as views_forget does)
    }
    views_forget_moved();
    static uint64_t nlog;
    if (n > 0) { park_n += (uint64_t)n; park_nbytes += bytes; }
    else { park_ret_n += (uint64_t)-n; park_ret_bytes += bytes; }
    if (nlog++ < 50 || nlog % 50 == 0)
        LOG("[vram] park: %d rendered images (%llu MB) %s; in system memory now %llu (%llu MB); since start parked %llu (%llu MB), "
            "returned %llu (%llu MB)\n", n > 0 ? n : -n, (unsigned long long)(bytes >> 20),
            n > 0 ? "idle, moved to system memory (VRAM short, nothing sampled left to evict)" : "moved back to VRAM (room again)",
            (unsigned long long)park_live, (unsigned long long)(park_bytes >> 20), (unsigned long long)park_n,
            (unsigned long long)(park_nbytes >> 20), (unsigned long long)park_ret_n, (unsigned long long)(park_ret_bytes >> 20));
}
// Room again (no pressure in this measurement, none in the last 5 s, and the image fits with 3 x headroom to spare): the
// parked images used since parking go back first (most recently used first), then the others; at most 64 MB of copies a
// round (one image at least)
static void park_return(uint64_t usage, uint64_t budget, uint64_t headroom, uint64_t now) {
    if (!budget || (res_need_ns && now - res_need_ns < 5000000000ull)) return;
    static NvnTexture **pc;
    static int cap_pc;
    int np = 0;
    for (int i = 0; i < g_nlive; i++) {
        TexBackend *b = g_live[i]->backend;
        if (!b || !b->parked || b->wb_state == 1 || b->wb_state == 4 || b->fbl_in) continue;
        if (np == cap_pc) { cap_pc = cap_pc ? cap_pc * 2 : 256; pc = realloc(pc, cap_pc * sizeof *pc); }
        pc[np++] = g_live[i];
    }
    if (np > 1) qsort(pc, np, sizeof *pc, res_cmp);   // (oldest use first: taken from the end)
    int n = 0;
    uint64_t moved = 0;
    for (int i = np - 1; i >= 0 && (!n || moved < (64ull << 20)); i--) {
        TexBackend *b = pc[i]->backend;
        uint64_t sz = vma_shim_size(b->mem);
        if (usage + moved + sz + 3 * headroom > budget) break;
        if (!park_move(b, false)) break;
        b->park_moved = true;
        moved += sz; n++;
    }
    if (n) park_forget_moved(-n, moved);
}
// "[vram] slack" line (device.c): parked now, and since the start
uint64_t vk_park_bytes(uint32_t *n, uint64_t *parked, uint64_t *returned, uint32_t *used_n, uint64_t *used_bytes) {
    *n = (uint32_t)park_live; *parked = park_n; *returned = park_ret_n;
    *used_n = 0; *used_bytes = 0;
    const uint64_t idle = vk_residency_idle_frames(), fr = vk_frame_counter();   // (used since parked: within the idle window)
    for (int i = 0; park_live && idle != ~0ull && i < g_nlive; i++) {
        TexBackend *b = g_live[i]->backend;
        if (b && b->parked && fr - b->last_use < idle) { ++*used_n; *used_bytes += vma_shim_size(b->mem); }
    }
    return park_bytes;
}
// A shortage that lasts: parked images used in the last second (hot again: read over PCIe every frame) go back to VRAM
// within the round, paid for by the round's other frees: their bytes are added to what the round frees (pool, idle
// sampled, idle rendered parked), and they move back once the round freed that much. At most 64 MB a round, most
// recently used first. Returns their bytes (0: none); hot[] / *nhot: which.
static uint64_t park_hot(NvnTexture **hot, int cap, int *nhot) {
    *nhot = 0;
    const uint64_t f1 = res_frames_in(1000000000ull), fr = vk_frame_counter();
    if (!park_live || f1 == ~0ull) return 0;
    uint64_t bytes = 0;
    for (int i = 0; i < g_nlive && *nhot < cap; i++) {
        TexBackend *b = g_live[i]->backend;
        if (!b || !b->parked || fr - b->last_use >= f1 || b->wb_state == 1 || b->wb_state == 4 || b->fbl_in) continue;
        hot[(*nhot)++] = g_live[i];
    }
    if (*nhot > 1) qsort(hot, *nhot, sizeof *hot, res_cmp);   // (oldest use first)
    for (int i = 0, j = *nhot - 1; i < j; i++, j--) { NvnTexture *t = hot[i]; hot[i] = hot[j]; hot[j] = t; }   // (most recent first)
    int k = 0;
    while (k < *nhot && (!k || bytes < (64ull << 20))) bytes += vma_shim_size(((TexBackend *)hot[k++]->backend)->mem);
    *nhot = k;
    return bytes;
}
// (res_real_budget, Windows: ours demoted in the last 10 s or the room the other processes leave being fitted; residency's
// block shedding, step 2b, runs only then)
static bool res_demoting;
static uint64_t res_shed_blocks, res_shed_bytes;
#ifdef _WIN32
// The OS budget follows window focus, not the memory (vram-fit 2026-11-02: 15.2 GB while the game window is active, 11.5
// then 8.6 GB a few lines after each "window inactive", back on activation, so the game runs on the background budget).
// With DLSS SR + FG the game's usage (8.4 GB) crossed the 8.6 GB background budget: 60 residency rounds evicted and
// re-uploaded textures mid-play, 44 GPU-wait hitches, with half the card free. Measured instead: the adapter's dedicated
// memory minus what the other processes hold resident (D3DKMT statistics, as Task Manager), less a margin of 1/32 of it
// (at least 512 MB), and never above the largest budget the OS gave (the active window's) or 15/16 of the memory. That is
// used when it is above the OS budget and the video memory manager held no more than 64 MB of ours demoted in the last 10
// s (demotion = real contention: the OS budget counts then). LARECOMP_VRAM_REAL_BUDGET=0: the OS budget alone (old).
static uint64_t res_real_budget(uint64_t budget, uint64_t usage) {
    static int on = -1, luid_ok;
    static uint8_t luid[8];
    static uint64_t bmax, logged_eff, contended_ns;
    if (on < 0) {
        const char *e = getenv("LARECOMP_VRAM_REAL_BUDGET");
        on = !(e && *e == '0');
        VkPhysicalDeviceIDProperties id = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
        VkPhysicalDeviceProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &id};
        vkGetPhysicalDeviceProperties2(g_vk.phys, &p2);
        luid_ok = id.deviceLUIDValid;
        memcpy(luid, id.deviceLUID, sizeof luid);
    }
    if (budget > bmax) bmax = budget;
    if (!on || !luid_ok) return budget;
    extern bool shim_adapter_vram(const uint8_t luid[8], uint64_t *resident, uint64_t *size, uint64_t *ours, uint64_t *demoted);
    uint64_t all, size, ours, demoted;
    if (!shim_adapter_vram(luid, &all, &size, &ours, &demoted)) return budget;
    // (the driver's count of ours may run ahead of the OS statistics; it also counts ours demoted, which 'all', resident
    // bytes only, does not: only its resident part, usage - demoted, else others came out short by the demoted bytes during
    // a demotion and the latched room too high)
    const uint64_t ours_res = usage > demoted ? usage - demoted : 0;
    if (ours < ours_res) ours = ours_res;
    const uint64_t others = all > ours ? all - ours : 0, margin = size / 32 > (512ull << 20) ? size / 32 : 512ull << 20;
    uint64_t room = size > others + margin ? size - others - margin : 0;
    const uint64_t cap = bmax > size / 16 * 15 ? bmax : size / 16 * 15;   // (a session that never had the focus: 15/16)
    if (room > cap) room = cap;
    // (ours in system memory now for lack of room; a cumulative count would only keep the OS budget: the old behaviour).
    // Held 10 s: evictions bring the demoted bytes down at once, and the next reading would flip back
    const uint64_t now = SDL_GetTicksNS();
    {   // (ours still demoted while the room would hold it all again: the blocks' priorities asked anew, at most every 2 s;
        // shim_vma.c vma_shim_reassert, P1.1: the video memory manager otherwise brings them back only lazily)
        // Room in the OS budget too, with 128 MB to spare (public #86, RX 5600 XT on v0.1.9.1: the OS gave 4022 MB of the
        // 6103 MB card, ours held all of it; the card's room said yes, so it asked every 2 s while the budget was full, in
        // the 15-24 fps stretch). An ask that brought nothing back doubles the wait (2 .. 64 s) until ours is back.
        static uint64_t reassert_ns, reassert_wait = 2000000000ull, reassert_demoted;
        if (demoted <= (64ull << 20)) reassert_wait = 2000000000ull;
        if (g_vk.pageable && demoted > (64ull << 20) && room > ours_res + demoted && budget > ours_res + demoted + (128ull << 20) &&
            now - reassert_ns > reassert_wait) {
            extern uint32_t vma_shim_reassert(VmaAllocator a);
            if (reassert_ns && demoted >= reassert_demoted && reassert_wait < 60000000000ull) reassert_wait *= 2;
            reassert_ns = now;
            reassert_demoted = demoted;
            const uint32_t nb = vma_shim_reassert(g_vk.vma);
            static int nlog;
            if (nlog++ < 20) LOG("[vram] %llu MB of ours demoted with room for it back: %u blocks' residency priority asked again"
                                 " (next ask in %llu s at the earliest)\n", (unsigned long long)(demoted >> 20), nb,
                                 (unsigned long long)(reassert_wait / 1000000000ull));
        }
    }
    if (demoted > (64ull << 20)) contended_ns = now;
    const bool contended = contended_ns && now - contended_ns < 10000000000ull;
    uint64_t eff = !contended && room > budget ? room : budget;
    // Another app's VRAM (camera-turn P0: the OS budget 12,023 MB while 6,759 MB of the 15,977 were other processes', so
    // ~8.7 GB of room: at 8.4 GB used residency saw no shortage and kept a 2.2 GB recycle pool, and the video memory
    // manager went on demoting ours, 26 GPU-bound 40-85 ms frames). A demotion latches "fit the room": the budget is then
    // min(the OS budget, the room), so the pool and idle textures go first, as for any shortage (vram_fit.h: the room falls
    // at once, rises only after 10 s 256 MB higher, lets go at the OS budget; a PC where nothing else holds VRAM keeps its
    // budget). LARECOMP_VRAM_FIT_ROOM=0: the OS budget under contention (old).
    static int fit_on = -1;
    static VramFit fit;
    if (fit_on < 0) { const char *e = getenv("LARECOMP_VRAM_FIT_ROOM"); fit_on = !(e && *e == '0'); }
    if (fit_on) eff = vram_fit_step(&fit, room, demoted > (64ull << 20), budget, eff, now);
    res_demoting = contended || fit.room;
    if (eff > logged_eff + (256ull << 20) || eff + (256ull << 20) < logged_eff) {
        static int nlog;
        if (nlog++ < 100)
            LOG("[vram] budget: the OS gives %llu MB, the adapter has %llu MB with %llu MB held by other processes%s%s: residency uses "
                "%llu MB (LARECOMP_VRAM_REAL_BUDGET)\n", (unsigned long long)(budget >> 20), (unsigned long long)(size >> 20),
                (unsigned long long)(others >> 20), contended ? ", ours being demoted (contention)" : "",
                fit.room ? ", fitting the room they leave (LARECOMP_VRAM_FIT_ROOM)" : "", (unsigned long long)(eff >> 20));
        logged_eff = eff;
    }
    return eff;
}
#endif
// Once a frame on the render thread (device.c, after the frame's submissions).
void vk_residency_tick(void) {
    static int on = -1;
    static uint64_t budget_override;
    if (on < 0) {
        const char *e = getenv("LARECOMP_VRAM_RESIDENCY");
        on = !(e && *e == '0');
        e = getenv("LARECOMP_VRAM_BUDGET_MB");
        budget_override = e ? (uint64_t)atoll(e) << 20 : 0;
        LOG("[vram] residency: %s%s, budget %s\n", on ? "on" : "off (LARECOMP_VRAM_RESIDENCY=0)",
            g_vk.mem_budget ? "" : " (no VK_EXT_memory_budget: our allocations against 85 % of the heap)",
            budget_override ? "from LARECOMP_VRAM_BUDGET_MB" : "the driver's");
    }
    if (!on) return;
    static uint64_t last_ns, cool_until;
    uint64_t now = SDL_GetTicksNS();
    if (now - last_ns < 250000000ull) return;
    last_ns = now;
    if (res_hist_n == 32) { memmove(res_hist_frame, res_hist_frame + 1, 31 * sizeof *res_hist_frame); memmove(res_hist_ns, res_hist_ns + 1, 31 * sizeof *res_hist_ns); res_hist_n--; }
    res_hist_frame[res_hist_n] = vk_frame_counter(); res_hist_ns[res_hist_n++] = now;
    // (the last round's images are freed once their submissions complete; until the next measurement nothing is asked of
    // the other consumers: the figure they read is always one nobody has answered yet)
    if (now < cool_until) { res_pressure = 0; return; }
    VkPhysicalDeviceMemoryBudgetPropertiesEXT bud = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
    VkPhysicalDeviceMemoryProperties2 mp = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2, .pNext = g_vk.mem_budget ? &bud : NULL};
    vkGetPhysicalDeviceMemoryProperties2(g_vk.phys, &mp);
    int h = -1;
    for (uint32_t i = 0; i < mp.memoryProperties.memoryHeapCount; i++)
        if ((mp.memoryProperties.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) &&
            (h < 0 || mp.memoryProperties.memoryHeaps[i].size > mp.memoryProperties.memoryHeaps[h].size))
            h = (int)i;
    if (h < 0) return;
    uint64_t usage, budget;
    if (g_vk.mem_budget) { usage = bud.heapUsage[h]; budget = bud.heapBudget[h]; }
    else {
        uint64_t hb[VK_MAX_MEMORY_HEAPS] = {0}, hs[VK_MAX_MEMORY_HEAPS];
        uint32_t hc[VK_MAX_MEMORY_HEAPS], hf[VK_MAX_MEMORY_HEAPS];
        vma_shim_heaps(g_vk.vma, hb, hc, hs, hf);
        usage = hb[h]; budget = mp.memoryProperties.memoryHeaps[h].size / 100 * 85;
    }
    // An integrated GPU has no VRAM of its own: its device-local heap is either the shared RAM (this PC's RDNA2 iGPU: 22
    // GB) or, on some drivers, only the BIOS carve-out (512 MB - 2 GB) beside a host heap that is the same RAM at the same
    // speed. Measured against a carve-out the residency would evict and re-upload (re-decode) textures every round, and
    // the mirrors would release and re-make their blocks, at no gain. There the VRAM test is off: the commit floor below
    // (the real limit on a shared-memory PC) stays. (Reddit, Ryzen 7 4700U / Vega 7, 16 GB: 1-2 s per frame; whether
    // this was it needs that log.) LARECOMP_VRAM_UMA_BUDGET=1: measure the device-local heap there too.
    if (g_vk.props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU && !budget_override) {
        static int uma_heap = -1;
        if (uma_heap < 0) { const char *e = getenv("LARECOMP_VRAM_UMA_BUDGET"); uma_heap = e && *e == '1'; }
        if (!uma_heap) budget = 0;
    }
    if (budget_override) budget = budget_override;
#ifdef _WIN32
    if (budget && !budget_override && g_vk.mem_budget) budget = res_real_budget(budget, usage);
#endif
    uint64_t headroom = budget / 16 > (256ull << 20) ? budget / 16 : 256ull << 20;
    uint64_t need = budget && usage + headroom > budget ? usage - (budget > 2 * headroom ? budget - 2 * headroom : 0) : 0, freed = 0;
    const uint64_t vneed = need;
    uint64_t cneed = 0, cavail = 0, cfloor_b = 0;
#ifdef _WIN32
    // System commit (Windows): WDDM charges every MB of device memory to the process commit too, so on a 16 GB PC VRAM
    // competes with RAM. When the commit headroom drops under LARECOMP_COMMIT_FLOOR_MB (default: the larger of 2 GB and
    // 1/16 of the commit limit), the same eviction runs until the headroom is back to twice the floor.
    {
        extern void shim_commit_room(uint64_t *avail, uint64_t *limit);
        static int64_t cfloor = -1;
        if (cfloor == -1) { const char *e = getenv("LARECOMP_COMMIT_FLOOR_MB"); cfloor = e ? (int64_t)atoll(e) << 20 : -2; }
        uint64_t lim = 0;
        shim_commit_room(&cavail, &lim);
        uint64_t fl = cfloor >= 0 ? (uint64_t)cfloor : lim / 16 > (2048ull << 20) ? lim / 16 : 2048ull << 20;
        if (fl && cavail < fl) cneed = 2 * fl - cavail;
        cfloor_b = fl;
    }
#endif
    if (cneed > need) need = cneed;
    recycle_cap_update(usage, budget, headroom, vneed || cneed);
    res_pressure = need;
    if (need) res_need_ns = now;
    res_pressure_commit = cneed != 0;
    if (!need) { if (park_live) park_return(usage, budget, headroom, now); return; }
    extern void vma_shim_drop_spares(void);
    vma_shim_drop_spares();   // (idle spare blocks first: 256 MB each held for nothing while short)
    const uint64_t need0 = need;
    NvnTexture *hot[16];
    int nhot = 0;
    uint64_t hotb = vneed && !cneed && park_ok() ? park_hot(hot, 16, &nhot) : 0;
    need += hotb;
    // 1. the recycle pool, oldest first, at most 256 MB a round, and not the images pooled in the last second: those are the
    // transient targets the game finalizes and makes again every frame, so freeing them only moved their creation (on the
    // render thread, under the same pressure) to the next frame. Only pooled images that give memory back go
    // (vma_shim_free_gain, as for the textures in 2.): one of its own, or a block whose live images are all pooled ones, as a
    // group. One whose block keeps live neighbours stays pooled (freeing it returned nothing and lost the next creation its
    // image). LARECOMP_VRAM_EVICT_ALL=1: oldest first.
    static int evict_all = -1;
    if (evict_all < 0) { const char *e = getenv("LARECOMP_VRAM_EVICT_ALL"); evict_all = e && *e == '1'; }
    extern uint32_t vma_shim_free_gain(VmaAllocator a, VmaAllocation const *al, int n, uint32_t *grp, uint64_t *gain);
    int npool = 0;
    uint64_t pfreed = 0;
    const uint64_t keep_ns = recycle_keep_ns();
    {
        static TexBackend *pold[RECYCLE_MAX];
        static VmaAllocation pal[RECYCLE_MAX];
        static uint32_t pgrp[RECYCLE_MAX];
        static uint64_t pgain[RECYCLE_MAX];
        static bool ptake[RECYCLE_MAX];
        int np = 0;
        pthread_mutex_lock(&g_recycle_lock);
        int na = 0;   // (the aged ones: the oldest first, g_recycle's order)
        while (na < g_nrecycle && now - g_recycle[na].put_ns >= keep_ns) pal[na] = g_recycle[na].b->mem, ptake[na] = false, na++;
        if (na && freed < need) {
            if (!evict_all) vma_shim_free_gain(g_vk.vma, pal, na, pgrp, pgain);
            const uint64_t keep = shadow_pool_keep();   // (the shadow maps' kept rungs stay: shadow_pool_keep)
            uint64_t rung_bytes = keep ? pool_rung_bytes() : 0;
            for (int i = 0; i < na && freed < need && pfreed < (256ull << 20); i++) {
                if (ptake[i] || !(evict_all || pgrp[i] || pgain[i])) continue;
                if (rung_bytes && rung_bytes <= keep) {  // (a kept rung, or a free_gain group holding one, stays whole)
                    bool held = pool_rung(&g_recycle[i]);
                    for (int j = i + 1; !held && !evict_all && pgrp[i] && j < na; j++) held = pgrp[j] == pgrp[i] && pool_rung(&g_recycle[j]);
                    if (held) continue;
                }
                for (int j = i; j < na; j++) {
                    if (j != i && (evict_all || !pgrp[i] || pgrp[j] != pgrp[i])) continue;   // (a group: all its images)
                    TexBackend *b = g_recycle[j].b;
                    if (pool_rung(&g_recycle[j])) {
                        const uint64_t sz = vma_shim_size(b->mem);
                        rung_bytes = rung_bytes > sz ? rung_bytes - sz : 0;
                    }
                    ptake[j] = true;
                    if (!b->parked) freed += evict_all ? vma_shim_size(b->mem) : pgain[j];   // (a parked one: system memory)
                    pfreed += vma_shim_size(b->mem);
                    if (evict_all || !pgrp[i]) break;
                }
            }
            int k = 0;
            for (int i = 0; i < g_nrecycle; i++) {
                if (i < na && ptake[i]) { pold[np++] = g_recycle[i].b; g_recycle_bytes -= vma_shim_size(g_recycle[i].b->mem); continue; }
                g_recycle[k++] = g_recycle[i];
            }
            g_nrecycle = k;
        }
        pthread_mutex_unlock(&g_recycle_lock);
        for (int i = 0; i < np; i++) free_backend(pold[i]);
        npool = np;
    }
    // 2. idle sampled textures, least recently used first: at most 128 textures / 256 MB a round (one view-table scan),
    // a round every 250 ms while still short (was 512 / 512 MB a second: the frees and the re-creations after them came
    // as one burst, the 499 ms image right after a 512-texture round)
    uint64_t idle = vk_residency_idle_frames(), fr = vk_frame_counter();
    static NvnTexture **cand;
    static int cap_cand;
    int nc = 0;
    if (freed < need && idle != ~0ull)
        for (int i = 0; i < g_nlive; i++) {
            NvnTexture *t = g_live[i];
            TexBackend *b = t->backend;
            if (!b || b->evicted || fr - b->last_use < idle || !res_evictable(t, b)) continue;
            if (nc == cap_cand) { cap_cand = cap_cand ? cap_cand * 2 : 1024; cand = realloc(cand, cap_cand * sizeof *cand); }
            cand[nc++] = t;
        }
    if (nc > 1) qsort(cand, nc, sizeof *cand, res_cmp);
    // Only what gives device memory back (shim_vma.c vma_shim_free_gain): a texture with its own allocation, or every
    // texture of an image block whose textures are all idle (the block goes, as a group, past the round's 128 if it must).
    // A texture whose block keeps live neighbours returns nothing when freed: the usage stayed flat while every round
    // evicted 128 more and they came back on use. LARECOMP_VRAM_EVICT_ALL=1: every idle texture, least recently used
    // first.
    static VmaAllocation *cal;
    static uint32_t *cgrp;
    static uint64_t *cgain;
    static int *ev, cap_g;
    if (nc > cap_g) {
        cap_g = cap_cand;
        cal = realloc(cal, cap_g * sizeof *cal); cgrp = realloc(cgrp, cap_g * sizeof *cgrp);
        cgain = realloc(cgain, cap_g * sizeof *cgain); ev = realloc(ev, cap_g * sizeof *ev);
    }
    int nleft = 0;   // (idle ones that would give nothing back: left in place)
    if (!evict_all && nc) {
        for (int i = 0; i < nc; i++) cal[i] = ((TexBackend *)cand[i]->backend)->mem;
        vma_shim_free_gain(g_vk.vma, cal, nc, cgrp, cgain);
    }
    int nev = 0;
    uint64_t evb = 0;
    bool gain_left = false;   // (an idle texture that would give memory back is still in place after the round)
    for (int i = 0; i < nc; i++) {
        TexBackend *b = cand[i]->backend;
        if (b->evicted) continue;   // (taken with its block below)
        const bool gives = evict_all || cgrp[i] || cgain[i];
        if (!gives) { nleft++; continue; }
        if (freed >= need || nev >= 128 || evb >= (256ull << 20)) { gain_left = true; continue; }
        if (evict_all || !cgrp[i]) {
            uint64_t sz = vma_shim_size(b->mem);
            b->evicted = true;   // (marks it for the view scan below)
            freed += evict_all ? sz : cgain[i]; evb += sz; ev[nev++] = i;
            continue;
        }
        for (int j = i; j < nc; j++)   // (the whole block: its gain once, every texture in it)
            if (cgrp[j] == cgrp[i]) {
                TexBackend *bj = cand[j]->backend;
                bj->evicted = true;
                freed += cgain[j]; evb += vma_shim_size(bj->mem); ev[nev++] = j;
            }
    }
    if (nev) {
        views_forget_evicted();
        for (int k = 0; k < nev; k++) {
            TexBackend *b = cand[ev[k]]->backend;
            defer((Dead){.img = b->image, .mem = b->mem});
            b->image = VK_NULL_HANDLE; b->mem = VK_NULL_HANDLE;
            b->uploaded = false; b->data_hash = b->sample_hash = 0;
            b->checked_frame = b->full_frame = b->full_seq = 0; b->gpuw_skips = 0;
        }
        res_evicted_n += nev; res_evicted_bytes += evb;
    }
    // 2b. Block shedding (v0.1.9.1 P1.1 with ram-diet): while another process's VRAM gets ours demoted, idle textures
    // that share 32 MB blocks with live ones gave nothing back (3222 idle left: their blocks hold live ones', 1.15 GB
    // idle, 31-37 fps for 16 s at GPU waits of 1.3 s / 2 s). Once a round, the small block whose textures are ALL
    // re-uploadable from guest memory (res_evictable: CPU-sourced, no GPU write since their hash) and at least half idle
    // goes whole: its live textures too, which come back from guest memory at their next use (at most 32 MB a round, half
    // of it at most live), so the block itself returns to the driver. LARECOMP_VRAM_SHED=0: off.
    static int shed_on = -1;
    if (shed_on < 0) { const char *e = getenv("LARECOMP_VRAM_SHED"); shed_on = !(e && *e == '0'); }
    if (shed_on && freed < need && vneed && res_demoting && idle != ~0ull && !evict_all) {
        int ns = 0;
        for (int i = 0; i < g_nlive; i++) {
            NvnTexture *t = g_live[i];
            TexBackend *b = t->backend;
            if (!b || b->evicted || !res_evictable(t, b)) continue;
            if (ns == cap_cand) { cap_cand = cap_cand ? cap_cand * 2 : 1024; cand = realloc(cand, cap_cand * sizeof *cand); }
            cand[ns++] = t;
        }
        // (and the pooled images pooled for keep_ns or more, never the hot transients of the last second: a block of
        // idle textures and old pooled images goes whole too; public #77, 8 GB, 425 MB pooled with usage over budget and
        // 'freed 0 pooled images', their blocks holding textures. Entries ns .. ns + np - 1, idle by definition.)
        static TexBackend *pool_b[RECYCLE_MAX];
        int np = 0;
        pthread_mutex_lock(&g_recycle_lock);
        for (int i = 0; i < g_nrecycle && np < RECYCLE_MAX; i++)
            if (now - g_recycle[i].put_ns >= keep_ns && !g_recycle[i].b->parked && g_recycle[i].b->mem) pool_b[np++] = g_recycle[i].b;
        pthread_mutex_unlock(&g_recycle_lock);
        if (ns + np > cap_g) {
            cap_g = ns + np > cap_cand ? ns + np : cap_cand;
            cal = realloc(cal, cap_g * sizeof *cal); cgrp = realloc(cgrp, cap_g * sizeof *cgrp);
            cgain = realloc(cgain, cap_g * sizeof *cgain); ev = realloc(ev, cap_g * sizeof *ev);
        }
        for (int i = 0; i < ns; i++) cal[i] = ((TexBackend *)cand[i]->backend)->mem;
        for (int i = 0; i < np; i++) cal[ns + i] = pool_b[i]->mem;
        const uint32_t ng = ns + np ? vma_shim_free_gain(g_vk.vma, cal, ns + np, cgrp, cgain) : 0;
        static uint64_t *g_blk, *g_tot, *g_idle;
        static uint32_t cap_ng;
        if (ng > cap_ng) {
            cap_ng = ng;
            g_blk = realloc(g_blk, (ng + 1) * 8); g_tot = realloc(g_tot, (ng + 1) * 8); g_idle = realloc(g_idle, (ng + 1) * 8);
        }
        uint32_t best = 0;
        if (ng) {
            memset(g_blk, 0, (ng + 1) * 8); memset(g_tot, 0, (ng + 1) * 8); memset(g_idle, 0, (ng + 1) * 8);
            for (int i = 0; i < ns + np; i++) {   // (groups are the blocks every live allocation of which is a candidate)
                const uint32_t g = cgrp[i];
                if (!g || g > ng) continue;
                const TexBackend *b = i < ns ? cand[i]->backend : pool_b[i - ns];
                const uint64_t sz = vma_shim_size(b->mem);
                if (cgain[i]) g_blk[g] = cgain[i];   // (the block's size, on its first member)
                g_tot[g] += sz;
                if (i >= ns || fr - b->last_use >= idle) g_idle[g] += sz;
            }
            for (uint32_t g = 1; g <= ng; g++)
                // (each frees its whole block: the one with the fewest live bytes re-uploads least at its next use, ASTC
                // decodes included)
                if (g_blk[g] && g_blk[g] <= (32ull << 20) && 2 * g_idle[g] >= g_tot[g] &&
                    (!best || g_tot[g] - g_idle[g] < g_tot[best] - g_idle[best])) best = g;
        }
        if (best) {
            int nsh = 0, npo = 0;
            uint64_t shb = 0;
            for (int i = 0; i < ns; i++) {
                if (cgrp[i] != best) continue;
                TexBackend *b = cand[i]->backend;
                b->evicted = true;
                shb += vma_shim_size(b->mem);
                ev[nsh++] = i;
            }
            for (int i = 0; i < np; i++) {   // (its pooled images leave the pool: freed as any pooled image is)
                if (cgrp[ns + i] != best) continue;
                TexBackend *pb = pool_b[i];
                bool out = false;
                pthread_mutex_lock(&g_recycle_lock);
                for (int k = 0; k < g_nrecycle; k++)
                    if (g_recycle[k].b == pb) {
                        g_recycle_bytes -= vma_shim_size(pb->mem);
                        memmove(&g_recycle[k], &g_recycle[k + 1], (size_t)(g_nrecycle - 1 - k) * sizeof *g_recycle);
                        g_nrecycle--;
                        out = true;
                        break;
                    }
                pthread_mutex_unlock(&g_recycle_lock);
                if (out) { shb += vma_shim_size(pb->mem); free_backend(pb); npo++; }
            }
            views_forget_evicted();
            for (int k = 0; k < nsh; k++) {
                TexBackend *b = cand[ev[k]]->backend;
                defer((Dead){.img = b->image, .mem = b->mem});
                b->image = VK_NULL_HANDLE; b->mem = VK_NULL_HANDLE;
                b->uploaded = false; b->data_hash = b->sample_hash = 0;
                b->checked_frame = b->full_frame = b->full_seq = 0; b->gpuw_skips = 0;
            }
            freed += g_blk[best];
            res_evicted_n += nsh; res_evicted_bytes += shb;
            res_shed_blocks++; res_shed_bytes += g_blk[best];
            static uint64_t said_ns;
            if (!said_ns || now - said_ns >= 10000000000ull) {
                said_ns = now;
                LOG("[vram] block shed while ours is being demoted: %d textures and %d pooled images (%.1f MB, %.1f MB of them "
                    "idle) of a %llu MB block back to the driver; %llu blocks / %llu MB so far (LARECOMP_VRAM_SHED)\n", nsh, npo, shb / 1048576.0,
                    g_idle[best] / 1048576.0, (unsigned long long)(g_blk[best] >> 20), (unsigned long long)res_shed_blocks,
                    (unsigned long long)(res_shed_bytes >> 20));
            }
        }
    }
    // 3. rendered images idle as long (vram-fit): moved to system memory, see park_move. Only for a VRAM shortage (system
    // memory is commit too: under the commit floor it would only move the problem, never below twice the floor, within
    // the round's 128 images and at most 64 MB of copies a round (PCIe time on the GPU: ~2.5 ms a round).
    int npk = 0;
    uint64_t pkb = 0;
    if (freed < need && vneed && !cneed && !gain_left && park_ok() && idle != ~0ull && nev < 128) {   // (no idle sampled one left that gives memory back)
        nc = 0;
        int refused = 0;
        for (int i = 0; i < g_nlive; i++) {
            NvnTexture *t = g_live[i];
            TexBackend *b = t->backend;
            if (!b || b->evicted || fr - b->last_use < idle || !res_parkable(t, b)) continue;
            if (nc == cap_cand) { cap_cand = cap_cand ? cap_cand * 2 : 1024; cand = realloc(cand, cap_cand * sizeof *cand); }
            cand[nc++] = t;
        }
        if (nc > 1) qsort(cand, nc, sizeof *cand, res_cmp);
        // (only what gives VRAM back, as in 2.: a moved image whose big block keeps live neighbours freed nothing and was
        // then read from system memory; a block's images move together, a group of at most 64 MB of copies.
        // LARECOMP_VRAM_EVICT_ALL=1: least recently used first, as before)
        if (nc > cap_g) {
            cap_g = cap_cand;
            cal = realloc(cal, cap_g * sizeof *cal); cgrp = realloc(cgrp, cap_g * sizeof *cgrp);
            cgain = realloc(cgain, cap_g * sizeof *cgain); ev = realloc(ev, cap_g * sizeof *ev);
        }
        if (!evict_all && nc) {
            for (int i = 0; i < nc; i++) cal[i] = ((TexBackend *)cand[i]->backend)->mem;
            vma_shim_free_gain(g_vk.vma, cal, nc, cgrp, cgain);
        }
        for (int i = 0; i < nc && freed < need && nev + npk < 128 && (!npk || pkb < (64ull << 20)); i++) {
            TexBackend *b = cand[i]->backend;
            if (b->park_moved || !(evict_all || cgrp[i] || cgain[i])) continue;
            int ng = 0;   // (this one, or its block's group)
            uint64_t gb = 0;
            for (int j = i; j < nc; j++)
                if (j == i || (!evict_all && cgrp[i] && cgrp[j] == cgrp[i])) ev[ng++] = j, gb += vma_shim_size(((TexBackend *)cand[j]->backend)->mem);
            if (ng > 1 && pkb + gb > (64ull << 20)) continue;   // (a block too large to copy this round)
            if (cfloor_b && cavail < 2 * cfloor_b + pkb + gb) break;
            uint64_t fg = 0;
            int moved = 0;
            for (int k = 0; k < ng; k++, moved++) {
                TexBackend *bj = cand[ev[k]]->backend;
                if (!park_move(bj, true)) { refused++; break; }
                bj->park_moved = true;
                fg += evict_all ? vma_shim_size(bj->mem) : cgain[ev[k]]; pkb += vma_shim_size(bj->mem); npk++;
            }
            if (moved == ng) freed += fg;   // (a group cut short by a refusal: its block stays, nothing back)
            if (refused >= 2) break;
        }
        if (npk) park_forget_moved(npk, pkb);
    }
    if (nhot && freed >= need) {   // (the round paid for them: back to VRAM)
        int nb = 0;
        uint64_t bb = 0;
        for (int i = 0; i < nhot; i++) {
            TexBackend *b = hot[i]->backend;
            uint64_t sz = vma_shim_size(b->mem);
            if (!park_move(b, false)) break;
            b->park_moved = true;
            bb += sz; nb++;
        }
        if (nb) park_forget_moved(-nb, bb);
        freed -= bb;
    }
    need = need0;
    res_pressure = freed < need ? need - freed : 0;   // (what this round could not free: the mirrors' share, vk_mirror_release_tick)
    if (npool || nev || npk) {
        res_rounds++;
        cool_until = now + (freed < need ? 250000000ull : 1000000000ull);   // (still short: the next tick goes on)
        if (res_rounds <= 50 || res_rounds % 50 == 0)
            LOG("[vram] residency: heap %d usage %llu MB of budget %llu MB, commit headroom %llu MB%s: freed %d pooled images, evicted %d "
                "idle textures (%llu MB, idle >= %llu frames; %d idle left: their blocks hold live ones); since start evicted %llu "
                "(%llu MB), re-created on use %llu (%llu MB)\n", h,
                (unsigned long long)(usage >> 20), (unsigned long long)(budget >> 20), (unsigned long long)(cavail >> 20),
                cneed ? " (low)" : "", npool, nev, (unsigned long long)(evb >> 20), (unsigned long long)idle, nleft,
                (unsigned long long)res_evicted_n, (unsigned long long)(res_evicted_bytes >> 20), (unsigned long long)res_restored_n,
                (unsigned long long)(res_restored_bytes >> 20));
    } else {
        static uint64_t nlog_short;
        if (nlog_short++ % 40 == 0)
            LOG("[vram] residency: heap %d usage %llu MB of budget %llu MB, commit headroom %llu MB%s, and nothing idle to evict "
                "(rendered / recent images; %d idle ones share blocks with live ones)\n", h, (unsigned long long)(usage >> 20),
                (unsigned long long)(budget >> 20), (unsigned long long)(cavail >> 20), cneed ? " (low)" : "", nleft);
        cool_until = now + 2000000000ull;
    }
}

// ================================================================== uploads (same change detection as metal.m)
static uint64_t full_hash_budget, full_hash_budget_frame;
static uint64_t hash_tex(const uint8_t *p, uint64_t n);   // (with the upload pool, below)
static bool deswizzle_par(const uint8_t *src, uint8_t *dst, uint32_t wb, uint32_t hb, uint32_t bpb, uint32_t bh_log2, uint32_t bd_log2);
static void desw_note(uint64_t bytes, bool split, uint64_t ticks);

// Four independent lanes (one serial multiply chain ran at ~1.5 bytes/cycle; dynamic textures are hashed every
// frame). Values are only compared within a run.
static uint64_t hash_full(const uint8_t *p, uint64_t n) {
    uint64_t h0 = 0x9E3779B97F4A7C15ull ^ n, h1 = 0xC2B2AE3D27D4EB4Full, h2 = 0x165667B19E3779F9ull, h3 = 0x27D4EB2F165667C5ull;
    uint64_t i = 0;
    for (; i + 32 <= n; i += 32) {
        uint64_t v[4];
        memcpy(v, p + i, 32);
        h0 = (h0 ^ v[0]) * 0x100000001B3ull; h0 ^= h0 >> 29;
        h1 = (h1 ^ v[1]) * 0x100000001B3ull; h1 ^= h1 >> 29;
        h2 = (h2 ^ v[2]) * 0x100000001B3ull; h2 ^= h2 >> 29;
        h3 = (h3 ^ v[3]) * 0x100000001B3ull; h3 ^= h3 >> 29;
    }
    uint64_t h = h0 ^ (h1 * 0x9E3779B97F4A7C15ull) ^ (h2 * 0xC2B2AE3D27D4EB4Full) ^ (h3 * 0x165667B19E3779F9ull);
    for (; i < n; i++) h = (h ^ p[i]) * 0x100000001B3ull;
    return h ^ (h >> 31);
}

static uint64_t fnv_sample(const uint8_t *p, uint64_t n) {
    uint64_t h = 1469598103934665603ull;
    uint64_t step = n > 4096 ? n / 4096 : 1;
    for (uint64_t i = 0; i < n; i += step) { h ^= p[i]; h *= 1099511628211ull; }
    return h ^ n;
}

static pthread_mutex_t pending_lock = PTHREAD_MUTEX_INITIALIZER;

void vk_feedback_epoch_bump(void);   // (exec.c: a CPU write reaching an image ends the self-copy snapshots' reuse)
static VkCommandBuffer up_cmd(TexBackend *b, bool *inl);   // (LARECOMP_UPLOAD_INLINE, below)
static void upload_pending(TexBackend *b) {
    if (b->gpu_owned) vk_feedback_epoch_bump();   // (only images the GPU renders: what a self-write draw can sample)
    pthread_mutex_lock(&pending_lock);
    void *px = b->pending_rgba;
    uint32_t w = b->pending_w, h = b->pending_h, stride = b->pending_stride;
    b->pending_rgba = NULL;
    pthread_mutex_unlock(&pending_lock);
    if (!px) return;
    if (w > b->width) w = b->width;
    if (h > b->height) h = b->height;
    vk_up_kind = 4; VkSpan s = vk_transient_copy(px, (uint64_t)stride * h, 16); vk_up_kind = 0;
    free(px);
    VkBufferImageCopy rg = {.bufferOffset = s.off, .bufferRowLength = stride / 4,
                            .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {w, h, 1}};
    bool inl;
    VkCommandBuffer cb = up_cmd(b, &inl);
    vk_dl_note_upload((void *)b->image, rg.imageExtent.width, rg.imageExtent.height, rg.imageSubresource.mipLevel);
                vkCmdCopyBufferToImage(cb, s.buf, b->image, VK_IMAGE_LAYOUT_GENERAL, 1, &rg);
    if (inl) vk_full_barrier(cb);   // (the draws after it read the new texels)
}

uint64_t prof_tex_checks, prof_tex_samples, prof_tex_full, prof_tex_full_bytes, prof_tex_dyn, prof_tex_ww_clean, prof_tex_gpuw_skip;
// (LARECOMP_VK_PROF) full-hashed textures: "[texhash]" every 600 frames lists the 10 with the most hashed bytes, with how
// often their contents had changed and the hash / upload time (are "dynamic" textures really rewritten every frame?)
extern uint64_t prof_up_ns;
static struct { TexBackend *b; uint64_t addr, bytes, hashes, changes, hash_ns, up_ns; uint32_t w, h, fmt, flags; } th[64];
static uint64_t th_frame;
static void texhash_note(NvnTexture *t, TexBackend *b, uint64_t total, bool changed, uint64_t hash_ns, uint64_t up_ns) {
    int i = 0;
    while (i < 64 && th[i].b && th[i].b != b) i++;
    if (i == 64) return;
    if (!th[i].b) { th[i].b = b; th[i].addr = t->addr; th[i].w = t->width; th[i].h = t->height; th[i].fmt = t->format; th[i].flags = t->flags; }
    th[i].bytes += total; th[i].hashes++; th[i].changes += changed; th[i].hash_ns += hash_ns; th[i].up_ns += up_ns;
}
static void texhash_report(uint64_t frame) {
    if (!th_frame) th_frame = frame;
    if (frame - th_frame < 600) return;
    LOG("[texhash] frames %llu..%llu, top full-hashed textures:\n", (unsigned long long)th_frame, (unsigned long long)frame);
    for (int k = 0; k < 10; k++) {
        int best = -1;
        for (int i = 0; i < 64; i++) if (th[i].b && th[i].hashes && (best < 0 || th[i].bytes > th[best].bytes)) best = i;
        if (best < 0) break;
        LOG("[texhash]   %llx %ux%u fmt %#x flags %#x: %llu hashes, %llu changed, %.0f MB, hash %.1f ms, upload %.1f ms\n",
            (unsigned long long)th[best].addr, th[best].w, th[best].h, th[best].fmt, th[best].flags, (unsigned long long)th[best].hashes,
            (unsigned long long)th[best].changes, th[best].bytes / 1048576.0, th[best].hash_ns / 1e6, th[best].up_ns / 1e6);
        th[best].hashes = 0;
    }
    memset(th, 0, sizeof th);
    th_frame = frame;
}

bool tex_watched(uint64_t addr);
static void tex_watch_note(NvnTexture *t, TexBackend *b, const char *what);

// ASTC levels decode on the GPU. The level's raw guest blocks (block-linear or pitch: 1/16 of its RGBA8 size at 8x8) go to
// staging, a compute pass (astc_comp.h: astc.c ported to GLSL, bit-exact against it in engine/tools/astcgpu) writes the
// texels to the scratch below (or a staging span), and the copies into the image follow. The CPU keeps only the memcpy
// (decode was 244 ms of a 121-layer 1024x1024 8x8 array's 340). =check: each level also decodes on the CPU and the two are
// compared byte for byte once the GPU is done ("[astc-gpu] check"; at most 64 levels / 256 MB in flight, the rest go
// unchecked).
static uint64_t up_tk_hash, up_ns_alloc, up_ns_decode;
// (a level's CPU-side copy that could not be allocated, the PC out of memory: the level keeps its old contents and the
// game goes on, instead of a write through NULL; rival-parity, their #347 / #94 low-memory crashes: the RGB8 expand
// copy too)
static void level_skipped(const char *what, uint32_t w, uint32_t h) {
    static int n;
    if (n++ < 8) LOG("[tex] %s level %ux%u: no memory for its CPU copy, skipped (the PC is out of memory)\n", what, w, h);
}   // (LARECOMP_UPLOAD_LOG, below; the hash in TSC ticks)
// upload_texture_'s render-thread time, never reset (exec.c [hitch] "uploads" line: the spawn frame's 97.6 MB): write-watch
// queries, identify + hash (sampled and full), deswizzle / staging copy / decode records; textures fully hashed, textures
// sent. The times are TSC ticks (up_ns_per_tick converts): upload_texture_ runs for every bound texture every frame,
// thousands a frame at 4K, and SDL_GetTicksNS (QPC) there cost about as much as the per-draw cuts.
uint64_t cum_up_ww_tk, cum_up_hash_tk, cum_up_copy_tk, cum_up_full, cum_up_sent;
uint64_t cum_view_tk, cum_view_n;   // (vk_image_view cache misses: vkCreateImageView, TSC ticks; [hitch] first-use line)
double up_ns_per_tick(void) {   // (render thread: calibrated over 2 ms at the first call, then over the whole run)
    static uint64_t t0, n0;
    static double r;
    uint64_t t = __rdtsc(), n = SDL_GetTicksNS();
    if (!t0) {
        t0 = t; n0 = n;
        while ((n = SDL_GetTicksNS()) - n0 < 2000000) {}
        t = __rdtsc();
    }
    if (t > t0 && n > n0) r = (double)(n - n0) / (double)(t - t0);
    return r;
}
static int ag_mode = -1;   // 0 off (CPU), 1 GPU, 2 GPU + CPU check
static VkPipeline ag_pipe;
static VkPipelineLayout ag_layout;
static VkDescriptorSetLayout ag_dsl;
static VkDescriptorPool ag_pool;
static struct { VkBuffer src, dst; VkDescriptorSet set; } ag_sets[256];  // (staging chunks are never freed: a set per pair stays valid)
static int ag_nsets;
typedef struct { uint32_t src_off, dst_off, width, height, bw, bh, srgb, linear, stride, wb, hb, bh_log2, bd_log2; } AgPush;
typedef struct { VkBuffer buf; VkBufferImageCopy rg; int chk; uint32_t w, h; uint64_t bc_off; } AgCopy;
typedef struct { AgCopy *c; uint32_t n, cap; bool bound, fenced; uint64_t used, bc_used; VkDescriptorSet set; } AgBatch;
// The texels go to a 64 MB device-local scratch, copied into the image whenever it fills (dispatches, barrier,
// copies, barrier, ...): a 121-layer 1024x1024 array is 484 MB of RGBA8, which in staging spans grew the transient
// ring by as much host-visible memory in one frame. Levels over 64 MB (or no scratch) use staging spans.
#define AG_SCR (64ull << 20)
static VkBuffer ag_scr;
static VmaAllocation ag_scr_mem;
// Compact textures (astc_bc3_on): the decoded texels in the scratch are encoded to BC3 (bc3_encode.comp) into this
// device-local buffer, about a quarter of the scratch, and copied into the image from there. A level whose blocks would
// not fit it, or that does not fit the scratch, decodes on the CPU and is encoded there (astc_copy_level).
#define BC_OUT (20ull << 20)
static VkBuffer bc_out;
static VmaAllocation bc_out_mem;
static VkPipeline bc_pipe;
static VkPipelineLayout bc_layout;
typedef struct { uint32_t src_off, dst_off, row, w, h, y0, bx, nblk; } BcPush;
typedef struct { uint64_t seq, size; VkBuffer rb; VmaAllocation mem; uint8_t *ptr, *cpu; uint32_t w, h, lv, layer, bw, bh; } AgCheck;
static AgCheck ag_chk[64];
static int ag_nchk;
static uint64_t ag_chk_bytes, ag_chk_done, ag_chk_bad, ag_chk_mb;

static bool ag_on(void) {
    if (ag_mode >= 0) return ag_mode > 0;
    ag_mode = 0;
    const char *e = getenv("LARECOMP_ASTC_GPU");
    if (e && !strcmp(e, "0")) return false;
    size_t nw;
    uint32_t *spv = vk_glsl_to_spirv(astc_decode_comp, STAGE_COMPUTE, "astc_decode", &nw);
    if (!spv) { LOG("[astc-gpu] the decode shader did not compile: ASTC stays on the CPU\n"); return false; }
    VkShaderModule m = vk_module(spv, nw);
    free(spv);
    VkDescriptorSetLayoutBinding bd[2];
    for (uint32_t i = 0; i < 2; i++) bd[i] = (VkDescriptorSetLayoutBinding){i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL};
    VkDescriptorSetLayoutCreateInfo dl = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 2, .pBindings = bd};
    VK_CHECK(vkCreateDescriptorSetLayout(g_vk.dev, &dl, NULL, &ag_dsl));
    VkPushConstantRange pc = {VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(AgPush)};
    VkPipelineLayoutCreateInfo pl = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &ag_dsl,
                                     .pushConstantRangeCount = 1, .pPushConstantRanges = &pc};
    VK_CHECK(vkCreatePipelineLayout(g_vk.dev, &pl, NULL, &ag_layout));
    VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                                                .module = m, .pName = "main"},
                                      .layout = ag_layout};
    VK_CHECK(vk_engine_compute_pipeline("texture decode ag", &ci, &ag_pipe));
    vkDestroyShaderModule(g_vk.dev, m, NULL);
    VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2 * 256};
    VkDescriptorPoolCreateInfo dp = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 256, .poolSizeCount = 1, .pPoolSizes = &ps};
    VK_CHECK(vkCreateDescriptorPool(g_vk.dev, &dp, NULL, &ag_pool));
    VkBufferCreateInfo sb = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = AG_SCR,
                             .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT};
    VmaAllocationCreateInfo sa = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, .requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT};
    if (vmaCreateBuffer(g_vk.vma, &sb, &sa, &ag_scr, &ag_scr_mem, NULL) != VK_SUCCESS) ag_scr = VK_NULL_HANDLE;
    ag_mode = e && !strcmp(e, "check") ? 2 : 1;
    LOG("[astc-gpu] ASTC levels decode on the GPU (compute)%s\n", ag_mode == 2 ? ", each also on the CPU and compared" : "");
    return true;
}

// The set binding two staging chunks whole (levels address them by offset, in the push constants).
static VkDescriptorSet ag_set(VkBuffer src, VkBuffer dst) {
    for (int i = 0; i < ag_nsets; i++)
        if (ag_sets[i].src == src && ag_sets[i].dst == dst) return ag_sets[i].set;
    VkDescriptorSet set;
    VkDescriptorSetAllocateInfo a = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = ag_pool, .descriptorSetCount = 1,
                                     .pSetLayouts = &ag_dsl};
    if (ag_nsets == 256 || vkAllocateDescriptorSets(g_vk.dev, &a, &set) != VK_SUCCESS) {
        static int told;
        if (!told++) LOG("[astc-gpu] descriptor sets ran out (256 staging chunk pairs): further levels decode on the CPU\n");
        return VK_NULL_HANDLE;
    }
    VkDescriptorBufferInfo bi[2] = {{src, 0, VK_WHOLE_SIZE}, {dst, 0, VK_WHOLE_SIZE}};
    VkWriteDescriptorSet w[2];
    for (uint32_t i = 0; i < 2; i++)
        w[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = i, .descriptorCount = 1,
                                      .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &bi[i]};
    vkUpdateDescriptorSets(g_vk.dev, 2, w, 0, NULL);
    ag_sets[ag_nsets++] = (typeof(ag_sets[0])){src, dst, set};
    return set;
}

// =check: compare the levels whose submission finished.
static void ag_check_poll(void) {
    uint64_t done = vk_completed_seq();
    for (int i = 0; i < ag_nchk;) {
        AgCheck *c = &ag_chk[i];
        if (!c->seq || c->seq > done) { i++; continue; }
        if (memcmp(c->ptr, c->cpu, c->size)) {
            uint64_t k = 0;
            while (c->ptr[k] == c->cpu[k]) k++;
            k /= 4;
            if (ag_chk_bad++ < 8)
                LOG("[astc-gpu] check: MISMATCH %ux%u level %u layer %u astc %ux%u: first at texel (%llu, %llu) gpu %08x cpu %08x\n", c->w, c->h,
                    c->lv, c->layer, c->bw, c->bh, (unsigned long long)(k % c->w), (unsigned long long)(k / c->w), ((uint32_t *)c->ptr)[k],
                    ((uint32_t *)c->cpu)[k]);
        }
        ag_chk_done++;
        ag_chk_mb += c->size;
        if (ag_chk_done % 64 == 0)
            LOG("[astc-gpu] check: %llu levels compared (%llu MB), %llu differ\n", (unsigned long long)ag_chk_done,
                (unsigned long long)(ag_chk_mb >> 20), (unsigned long long)ag_chk_bad);
        vmaDestroyBuffer(g_vk.vma, c->rb, c->mem);
        free(c->cpu);
        ag_chk_bytes -= c->size;
        *c = ag_chk[--ag_nchk];
    }
}

static void ag_copy_out(VkCommandBuffer cb, AgBatch *bt, const TexBackend *b);

// The BC3 encode pass (compact textures), made when astc_bc3_on decides: false = compact textures stay off.
static bool bc_ready(void) {
    static int st = -1;
    if (st >= 0) return st;
    st = 0;
    if (!ag_scr) { LOG("[astc-gpu] no decode scratch: compact textures stay off\n"); return false; }
    size_t nw;
    uint32_t *spv = vk_glsl_to_spirv(bc3_encode_comp, STAGE_COMPUTE, "bc3_encode", &nw);
    if (!spv) { LOG("[astc-gpu] the BC3 encode shader did not compile: compact textures stay off\n"); return false; }
    VkShaderModule m = vk_module(spv, nw);
    free(spv);
    VkPushConstantRange pc = {VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(BcPush)};
    VkPipelineLayoutCreateInfo pl = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &ag_dsl,
                                     .pushConstantRangeCount = 1, .pPushConstantRanges = &pc};
    VK_CHECK(vkCreatePipelineLayout(g_vk.dev, &pl, NULL, &bc_layout));
    VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                                                .module = m, .pName = "main"},
                                      .layout = bc_layout};
    VK_CHECK(vk_engine_compute_pipeline("texture decode bc", &ci, &bc_pipe));
    vkDestroyShaderModule(g_vk.dev, m, NULL);
    VkBufferCreateInfo sb = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = BC_OUT,
                             .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT};
    VmaAllocationCreateInfo sa = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, .requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT};
    if (vmaCreateBuffer(g_vk.vma, &sb, &sa, &bc_out, &bc_out_mem, NULL) != VK_SUCCESS) {
        LOG("[astc-gpu] no memory for the BC3 encode buffer: compact textures stay off\n");
        return false;
    }
    if (!ag_set(ag_scr, bc_out)) { LOG("[astc-gpu] no descriptor set for the BC3 encode pass: compact textures stay off\n"); return false; }
    st = 1;
    LOG("[astc-gpu] compact textures: decoded levels encode to BC3 on the GPU (compute)\n");
    return true;
}

// The copy of a level's BC3 blocks ((w + 3) / 4 a row, from off) into the image. An extent that stops short of the
// level's edge must be whole blocks (Vulkan): a clamped one is cut to a multiple of 4.
static VkBufferImageCopy bc_region(VkDeviceSize off, uint32_t w, uint32_t h, VkExtent3D ext, const TexBackend *b, uint32_t lv,
                                   uint32_t layer) {
    if (ext.width != level_w(b, lv)) ext.width &= ~3u;
    if (ext.height != level_h(b, lv)) ext.height &= ~3u;
    return (VkBufferImageCopy){.bufferOffset = off, .bufferRowLength = (w + 3) & ~3u, .bufferImageHeight = (h + 3) & ~3u,
                               .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, lv, layer, 1}, .imageExtent = ext};
}

// One ASTC level on the GPU: false = not taken (the CPU decodes it).
static bool ag_level(VkCommandBuffer cb, AgBatch *bt, const uint8_t *src, bool linear, uint32_t stride, const LevelLayout *l,
                     const TexBackend *b, uint32_t lv, uint32_t layer) {
    uint32_t nblk = l->wb * l->hb;
    if (l->block_depth_log2 || (linear && (stride & 3)) || (nblk + 63) / 64 > 65535) return false;
    uint32_t rows = 8u << l->block_height_log2;  // block-linear: whole GOB blocks, rows of (wb * 16 + 63) / 64 GOBs
    uint64_t n = linear ? (uint64_t)(l->hb - 1) * stride + l->wb * 16 : (uint64_t)((l->wb * 16 + 63) / 64) * 64 * ((l->hb + rows - 1) / rows * rows);
    uint64_t size = (uint64_t)l->w * l->h * 4;
    bool scr = ag_scr && size <= AG_SCR;
    uint64_t bcs = (uint64_t)((l->w + 3) / 4) * ((l->h + 3) / 4) * 16;   // (compact textures: its BC3 blocks)
    if (b->astc_bc3 && (!scr || bcs > BC_OUT || !bc_ready())) return false;
    if (scr && (bt->used + size > AG_SCR || (b->astc_bc3 && bt->bc_used + bcs > BC_OUT))) ag_copy_out(cb, bt, b);
    uint64_t t0 = SDL_GetTicksNS();
    vk_up_kind = 4;
    VkSpan s = vk_transient(n, 16), d = scr ? (VkSpan){ag_scr, bt->used, NULL} : vk_transient(size, 16);
    vk_up_kind = 0;
    VkDescriptorSet set = ag_set(s.buf, d.buf);
    uint64_t t1 = SDL_GetTicksNS();
    up_ns_alloc += t1 - t0;   // (LARECOMP_UPLOAD_LOG: staging = the two spans + their set, decode = the block copy)
    if (!set) return false;
    memcpy(s.ptr, src, n);
    up_ns_decode += SDL_GetTicksNS() - t1;
    if (scr) bt->used += (size + 15) & ~15ull;
    if (!bt->fenced) { vk_full_barrier(cb); bt->fenced = true; }   // (the scratch's previous copies have read it)
    if (!bt->bound) { vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, ag_pipe); bt->bound = true; }
    if (set != bt->set) { vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, ag_layout, 0, 1, &set, 0, NULL); bt->set = set; }
    AgPush pc = {(uint32_t)(s.off / 4), (uint32_t)(d.off / 4), l->w, l->h, b->astc_bw, b->astc_bh, b->astc_srgb, linear, stride, l->wb, l->hb,
                 l->block_height_log2, l->block_depth_log2};
    vkCmdPushConstants(cb, ag_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
    vkCmdDispatch(cb, (nblk + 63) / 64, 1, 1);
    if (bt->n == bt->cap) { bt->cap = bt->cap ? bt->cap * 2 : 64; bt->c = realloc(bt->c, bt->cap * sizeof *bt->c); }
    AgCopy *c = &bt->c[bt->n++];
    c->buf = d.buf;
    c->rg = (VkBufferImageCopy){.bufferOffset = d.off, .bufferRowLength = l->w, .bufferImageHeight = l->h,
                                .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, lv, layer, 1},
                                .imageExtent = {l->w < level_w(b, lv) ? l->w : level_w(b, lv), l->h < level_h(b, lv) ? l->h : level_h(b, lv), 1}};
    c->chk = -1;
    c->w = l->w, c->h = l->h;
    if (b->astc_bc3) { c->bc_off = bt->bc_used; bt->bc_used += (bcs + 15) & ~15ull; }
    if (ag_mode == 2 && ag_nchk < 64 && ag_chk_bytes + size <= (256ull << 20)) {
        AgCheck *k = &ag_chk[ag_nchk];
        VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                                      .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT};
        VmaAllocationInfo info;
        if (vmaCreateBuffer(g_vk.vma, &bi, &ai, &k->rb, &k->mem, &info) == VK_SUCCESS) {
            uint8_t *blocks = malloc((uint64_t)nblk * 16);
            if (linear) for (uint32_t y = 0; y < l->hb; y++) memcpy(blocks + (uint64_t)y * l->wb * 16, src + (uint64_t)y * stride, l->wb * 16);
            else tegra_deswizzle(src, blocks, l->wb, l->hb, 1, 16, l->block_height_log2, l->block_depth_log2);
            k->cpu = malloc(size);
            astc_decode_par(blocks, l->w, l->h, b->astc_bw, b->astc_bh, b->astc_srgb, k->cpu);
            free(blocks);
            k->ptr = info.pMappedData;
            k->size = size;
            k->seq = 0;   // (set when the copy is recorded)
            k->w = l->w, k->h = l->h, k->lv = lv, k->layer = layer, k->bw = b->astc_bw, k->bh = b->astc_bh;
            ag_chk_bytes += size;
            c->chk = ag_nchk++;
        }
    }
    return true;
}

// The decoded texels so far into the image (and into the check buffers); the scratch is free again after a barrier.
static void ag_copy_out(VkCommandBuffer cb, AgBatch *bt, const TexBackend *b) {
    if (!bt->n) return;
    vk_full_barrier(cb);
    if (b->astc_bc3) {   // (compact textures: the texels to BC3 blocks in bc_out, which the copies then read)
        VkDescriptorSet set = ag_set(ag_scr, bc_out);   // (made by bc_ready)
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, bc_pipe);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, bc_layout, 0, 1, &set, 0, NULL);
        for (uint32_t i = 0; i < bt->n; i++) {
            AgCopy *c = &bt->c[i];
            BcPush pc = {(uint32_t)(c->rg.bufferOffset / 4), (uint32_t)(c->bc_off / 4), c->w, c->w, c->h, 0, (c->w + 3) / 4,
                         (c->w + 3) / 4 * ((c->h + 3) / 4)};
            vkCmdPushConstants(cb, bc_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
            vkCmdDispatch(cb, (pc.nblk + 63) / 64, 1, 1);
        }
        vk_full_barrier(cb);
        bt->bound = false;   // (the decode pipeline and its set are bound again for the next level)
        bt->set = VK_NULL_HANDLE;
    }
    // one copy command per run of levels in the same buffer (a 121-layer array was 1331 commands, ~80 ms recording)
    VkBufferImageCopy rg[256];
    for (uint32_t i = 0, n = 0; i < bt->n; i++) {
        AgCopy *c = &bt->c[i];
        VkBuffer from = b->astc_bc3 ? bc_out : c->buf;
        vk_dl_note_upload((void *)b->image, c->rg.imageExtent.width, c->rg.imageExtent.height, c->rg.imageSubresource.mipLevel);
        VkBufferImageCopy r = b->astc_bc3 ? bc_region(c->bc_off, c->w, c->h, c->rg.imageExtent, b, c->rg.imageSubresource.mipLevel,
                                                      c->rg.imageSubresource.baseArrayLayer) : c->rg;
        if (r.imageExtent.width && r.imageExtent.height) rg[n++] = r;
        if (n && (n == 256 || i + 1 == bt->n || (b->astc_bc3 ? bc_out : bt->c[i + 1].buf) != from)) {
            vkCmdCopyBufferToImage(cb, from, b->image, VK_IMAGE_LAYOUT_GENERAL, n, rg);
            n = 0;
        }
    }
    for (uint32_t i = 0; i < bt->n; i++) {
        AgCopy *c = &bt->c[i];
        if (c->chk >= 0) {
            AgCheck *k = &ag_chk[c->chk];
            VkBufferCopy bc = {c->rg.bufferOffset, 0, k->size};
            vkCmdCopyBuffer(cb, c->buf, k->rb, 1, &bc);
            k->seq = vk_pending_seq();
        }
    }
    bt->n = 0;
    bt->used = bt->bc_used = 0;
    bt->fenced = false;
}

// After a texture's levels.
static void ag_flush(VkCommandBuffer cb, AgBatch *bt, const TexBackend *b) {
    ag_copy_out(cb, bt, b);
    free(bt->c);
    *bt = (AgBatch){0};
}

// A level the CPU decoded (RGBA8 in the span s) into the image: as it is, or (compact textures) encoded to BC3 here
// first (bc3enc.h: 0.35 us a block, ~90 ms a 2048x2048 level, so only these rare ones: texpatch levels, 3D or odd-pitch
// levels, LARECOMP_ASTC_GPU=0; the GPU-decoded ones encode on the GPU, ag_copy_out).
static void astc_copy_level(VkCommandBuffer cb, VkSpan s, const LevelLayout *l, const TexBackend *b, uint32_t lv, uint32_t layer) {
    VkExtent3D ext = {l->w < level_w(b, lv) ? l->w : level_w(b, lv), l->h < level_h(b, lv) ? l->h : level_h(b, lv), 1};
    vk_dl_note_upload((void *)b->image, ext.width, ext.height, lv);
    if (!b->astc_bc3) {
        VkBufferImageCopy rg = {.bufferOffset = s.off, .bufferRowLength = l->w, .bufferImageHeight = l->h,
                                .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, lv, layer, 1}, .imageExtent = ext};
        vkCmdCopyBufferToImage(cb, s.buf, b->image, VK_IMAGE_LAYOUT_GENERAL, 1, &rg);
        return;
    }
    uint64_t t0 = SDL_GetTicksNS();
    vk_up_kind = 4; VkSpan o = vk_transient((uint64_t)((l->w + 3) / 4) * ((l->h + 3) / 4) * 16, 16); vk_up_kind = 0;
    bc3_encode(s.ptr, l->w, l->w, l->h, o.ptr);
    up_ns_decode += SDL_GetTicksNS() - t0;
    VkBufferImageCopy rg = bc_region(o.off, l->w, l->h, ext, b, lv, layer);
    if (rg.imageExtent.width && rg.imageExtent.height) vkCmdCopyBufferToImage(cb, o.buf, b->image, VK_IMAGE_LAYOUT_GENERAL, 1, &rg);
}

// Uploads in submission order (LARECOMP_UPLOAD_INLINE=1, barriers 11-06, FIX_PROGRAM_1106 item d; default off until its
// arm): a texture's copy goes into the submission's upload buffer, which runs before ALL of the submission's draws, also
// the ones recorded before the copy. When draws of this submission already sampled the texture (use_seq) and the copy
// brings new bytes (WriteTexels, a video frame, a frame boundary inside one submission), those earlier draws read the
// new bytes on the GPU; on hardware they ran before the game wrote them. With the switch, exec.c ups_refresh asks such a
// texture again before a draw's / dispatch's pass begins (at the draw, texture_info runs inside the open pass), and its
// copy goes into the main command buffer there, between two full barriers (exec.c vk_upload_inline_cmd). Counted either
// way ("[tex] in-use uploads"): switch off, how often the hole is hit at all. What it changes besides the order: a
// WriteTexels / video frame reaches the next draw that samples the texture, where without it the texture_info caches
// (per frame) can keep the old bytes to the next frame. With async replay the game may have written them for a later
// frame than the one replayed: the arm says which is better. (per thread: with LARECOMP_ASYNC_EXEC=0 the game's threads
// record; another thread's scan number equal to a stamp only sends that upload to the upload buffer, as without the
// switch)
static __thread bool ui_inline;   // (vk_tex_inline_use's upload: copies go to the main command buffer)
static __thread bool ui_presubmit;   // (vk_tex_presubmit's re-check: the upload buffer on purpose, not counted)
static __thread uint64_t ui_draw;   // (ups_refresh's scans: a scan's own stamp is this draw's use, not an earlier draw's)
static uint64_t st_ui_ahead, st_ui_inline, st_ui_asked;
static uint64_t ui_gen, ui_sub_gen;   // WriteTexels / video frames posted (any thread); the count when the last submission went
bool vk_tex_inline_on(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("LARECOMP_UPLOAD_INLINE");
        on = e && *e == '1';
        if (on) LOG("[tex] uploads in submission order (LARECOMP_UPLOAD_INLINE=1): a texture draws of this submission sampled gets its new bytes between them and the next draw\n");
    }
    return on;
}
// exec.c ups_refresh, once per draw / dispatch: may a texture this submission sampled have new bytes due? Only after a
// WriteTexels / video frame since the last submission went, or a frame boundary inside this one.
bool vk_tex_inline_due(void) {
    static uint64_t seq, frame;
    const uint64_t s = vk_pending_seq(), f = vk_frame_counter();
    if (s != seq) seq = s, frame = f;
    ui_draw++;
    return f != frame || __atomic_load_n(&ui_gen, __ATOMIC_ACQUIRE) != ui_sub_gen;
}
static VkCommandBuffer up_cmd(TexBackend *b, bool *inl) {   // (the command buffer an upload of b records into)
    *inl = false;
    if (b->use_seq != vk_pending_seq() || b->use_draw == ui_draw || ui_presubmit) return vk_upload_cmd();
    if (ui_inline) {
        extern VkCommandBuffer vk_upload_inline_cmd(void);
        st_ui_inline++;
        *inl = true;
        return vk_upload_inline_cmd();
    }
    st_ui_ahead++;
    static uint64_t nlog;
    if (nlog++ < 8 || (st_ui_ahead & 1023) == 0)
        LOG("[tex] in-use uploads: %llu went ahead of draws of their submission that sampled them, %llu in order (LARECOMP_UPLOAD_INLINE), %llu asked before a pass\n",
            (unsigned long long)st_ui_ahead, (unsigned long long)st_ui_inline, (unsigned long long)st_ui_asked);
    return vk_upload_cmd();
}
// exec.c ups_refresh, before a draw's / dispatch's pass begins, for each texture it samples: the first use in this
// submission is stamped (its upload, at the draw, goes before it on the upload buffer); a texture earlier draws sampled
// is asked now, as vk_tex would at the draw, so a copy with new bytes lands between those draws and this one
static void upload_texture(NvnTexture *t, TexBackend *b);
void vk_tex_inline_use(NvnTexture *t, bool due) {
    TexBackend *b = t->backend;
    if (!b) return;
    const uint64_t s = vk_pending_seq();
    if (b->use_seq != s) { b->use_seq = s; b->use_draw = ui_draw; return; }
    if (!due || b->use_draw == ui_draw || b->evicted) return;
    if (!b->pending_rgba && (b->gpu_owned || (!b->cpu_dirty && b->checked_frame == vk_frame_counter()))) return;   // (upload_texture_'s first test)
    if (alias_writer(t)) return;   // (another object's rendering is what draws sample: vk_tex uploads nothing)
    st_ui_asked++;
    ui_inline = true;
    if (b->pending_rgba) upload_pending(b);
    if (!b->gpu_owned) upload_texture(t, b);
    ui_inline = false;
}

// LARECOMP_UPLOAD_LOG=1: every texture upload over 20 ms, split into its parts.
static void upload_texture_(NvnTexture *t, TexBackend *b);
// Submit re-check (LARECOMP_TEX_SUBMIT_RECHECK=1, default off until its A/B; hdr 11-05): a CPU texture uploaded (first
// time, or new bytes) while this submission was recorded is asked again right before the submission goes to the GPU;
// pages the game wrote since its hash (write watch) are uploaded again into the same submission's upload buffer, which
// runs before its draws. The replay records a list soon after the game submits it and the game may still be filling a
// texture it already draws with (hardware's GPU reads it later): cand2 c10_field 7287, sand drawn as grass for one
// frame, a texture first uploaded at N and with new bytes at N + 1. It narrows the window to the submission's end; it
// does not close it.
static NvnTexture *g_lr[256];
static int g_lrn;
static uint64_t st_lr_checked, st_lr_reup, st_lr_over;   // (over: past the list's 256 in one submission, not asked)
static int lr_on(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("LARECOMP_TEX_SUBMIT_RECHECK");
        on = e && *e == '1';
        if (on) LOG("[tex] submit re-check on (LARECOMP_TEX_SUBMIT_RECHECK=1): textures uploaded in a submission are asked again before it is submitted\n");
    }
    return on;
}
static void lr_note(NvnTexture *t) {
    for (int i = 0; i < g_lrn; i++) if (g_lr[i] == t) return;
    if (g_lrn < (int)(sizeof g_lr / sizeof *g_lr)) g_lr[g_lrn++] = t;
    else st_lr_over++;
}
static void lr_forget(NvnTexture *t) {
    for (int i = 0; i < g_lrn; i++) if (g_lr[i] == t) { g_lr[i] = g_lr[--g_lrn]; i--; }
}
static void upload_texture(NvnTexture *t, TexBackend *b);
void vk_tex_presubmit(void) {   // (exec.c vk_submit_extra, render thread, before the upload buffer ends)
    ui_sub_gen = __atomic_load_n(&ui_gen, __ATOMIC_ACQUIRE);   // (LARECOMP_UPLOAD_INLINE: what the next submission's draws may find new)
    if (!g_lrn) return;
    extern int heap_ww_changed(uint64_t addr, uint64_t size, uint64_t *seen);
    NvnTexture *v[256];
    const int n = g_lrn;
    memcpy(v, g_lr, n * sizeof *v);
    g_lrn = 0;
    for (int i = 0; i < n; i++) {
        NvnTexture *t = v[i];
        TexBackend *b = t->backend;
        if (!b || !b->uploaded || b->gpu_owned) continue;
        st_lr_checked++;
        uint64_t seen = b->ww_seen;
        if (heap_ww_changed(t->addr, tegra_total_size(&t->layout), &seen) <= 0) continue;
        b->ww_seen = seen;
        b->cpu_dirty = true;   // (a full hash now; uploaded again if its bytes changed)
        const uint64_t h0 = b->data_hash;
        ui_presubmit = true;   // (the upload buffer on purpose: all of the submission's draws take the newest bytes)
        upload_texture(t, b);
        ui_presubmit = false;
        st_lr_reup += b->data_hash != h0;
    }
    g_lrn = 0;   // (the re-uploads above noted themselves again: this submission is done with them)
    static uint64_t last;
    if (st_lr_checked - last >= 20000) {
        last = st_lr_checked;
        LOG("[tex] submit re-check: %llu textures asked again, %llu had new bytes (uploaded again before their draws ran), %llu past the list (not asked)\n",
            (unsigned long long)st_lr_checked, (unsigned long long)st_lr_reup, (unsigned long long)st_lr_over);
    }
}
static void upload_texture(NvnTexture *t, TexBackend *b) {
    if (b && b->gpu_owned) vk_feedback_epoch_bump();
    static int on = -1;
    if (on < 0) on = getenv("LARECOMP_UPLOAD_LOG") && strcmp(getenv("LARECOMP_UPLOAD_LOG"), "0");
    if (!on) { upload_texture_(t, b); return; }
    up_tk_hash = up_ns_alloc = up_ns_decode = 0;
    uint64_t t0 = SDL_GetTicksNS();
    upload_texture_(t, b);
    uint64_t dt = SDL_GetTicksNS() - t0;
    if (dt > 20000000ull) {
        double hash_ns = up_tk_hash * up_ns_per_tick();
        LOG("[upload] %ux%u fmt %#x levels %u layers %u astc %ux%u: %.1f ms = identify + hash %.1f, staging %.1f, decode / copy %.1f, rest %.1f\n",
            t->width, t->height, t->format, t->layout.levels, t->layout.layers, b->astc_bw, b->astc_bh, dt / 1e6, hash_ns / 1e6,
            up_ns_alloc / 1e6, up_ns_decode / 1e6, (dt - hash_ns - up_ns_alloc - up_ns_decode) / 1e6);
    }
}
static void upload_texture_(NvnTexture *t, TexBackend *b) {
    if (ag_mode < 0) ag_on();   // (LARECOMP_ASTC_GPU: its shader compiles at the first upload, during boot, not in the world)
    if (!t->addr || t->target == 10) return;
    uint64_t frame = vk_frame_counter();
    bool cpu_dirty = b->cpu_dirty;
    b->cpu_dirty = false;
    if (b->uploaded && b->checked_frame == frame && !cpu_dirty) return;
    // (a texture pre-uploaded on the loading screen, preupload.c: its first real use is a check frame, and asks the write
    // watch however long ago the pre-upload was, so a texel written after it is uploaded now, as without it)
    const bool pre_first = b->preuploaded;
    // (LARECOMP_RECYCLE_RECHECK: a new object on a pooled image of its storage, recycle_get; its first use is a check frame
    // as a pre-upload's is)
    const bool rc_first = b->recycle_check;
    if (b->uploaded && !b->dynamic && !pre_first && !rc_first && ((frame + ((uintptr_t)b >> 6)) & 7) && !cpu_dirty) return;
    if (pre_first) {
        extern void vk_preupload_used(void);
        b->preuploaded = false;
        vk_preupload_used();
    }
    b->recycle_check = false;
    const FormatInfo *fi = t->layout.fmt;
    if (!fi || !fi->bpb) return;
    VkFmt mf = fmt_of(t->format);
    uint64_t total = tegra_total_size(&t->layout);
    b->checked_frame = frame;
    prof_tex_checks++;
    if (t->flags & 0x10) b->dynamic = true;
    if (full_hash_budget_frame != frame) { full_hash_budget_frame = frame; full_hash_budget = 4 << 20; }
    const uint8_t *mem = (const uint8_t *)(uintptr_t)t->addr;
    static int always_full = -1;
    if (always_full < 0) always_full = getenv("LARECOMP_TEX_FULLHASH") ? 1 : 0;
    bool full = b->dynamic || !b->uploaded || always_full || cpu_dirty;
    int lf_why = cpu_dirty ? 1 : 0;   // (why this check hashed in full / saw new bytes: the late-fill line below)
    // Guest heap write watch (hle_os_mem.cpp heap_ww_changed): an uploaded texture whose pages the CPU has not written since its last
    // check is unchanged, without hashing. GPU writes into the heap are invisible to the watch, so a full hash still runs every 60 frames
    // (dynamic) / 120 frames (others). TOTK_HEAP_WW=0: off. LARECOMP_TEX_GPUW=1 (default since 2026-10-26; =0 off): the periodic hash is
    // due only to catch GPU writes; when no shader wrote the texture's pages since its last full hash (exec.c vk_heap_gpu_written_since:
    // the storage buffers writing shaders bound), it is re-armed instead and the write-watch check below decides (the periodic rehash,
    // hash_full, 0.13 ms a frame in Kakariko); only where the watch answers, and every 4th period still hashes =2: where it would skip,
    // it hashes anyway (as =0) and, when the CPU did not write the pages either (write watch clean), a changed hash is a GPU write the
    // gdev_blk premise missed: counted and logged
    static int gpuw = -1;
    if (gpuw < 0) { const char *e = getenv("LARECOMP_TEX_GPUW"); gpuw = e ? atoi(e) : 1; if (gpuw) LOG("[tex] periodic rehash only after GPU writes (%s)\n", gpuw == 2 ? "check mode" : "on"); }
    int gpuw_check = 0;   // 1: =2 would have skipped and the CPU did not write the texture
    if (gpuw && b->uploaded && !cpu_dirty && !always_full && b->full_seq && frame - b->full_frame >= (b->dynamic ? 60u : 120u)) {
        extern bool vk_heap_gpu_written_since(uint64_t addr, uint64_t size, uint64_t since);
        if (!vk_heap_gpu_written_since(t->addr, total, b->full_seq)) {
            extern int heap_ww_changed(uint64_t addr, uint64_t size, uint64_t *seen);
            if (gpuw == 1) {
                // Skipped only where the write watch answers: unknown (Linux default, TOTK_HEAP_WW=0, outside the
                // committed heap) re-armed every period would leave the 4 KB sample as the only check, for good
                uint64_t w0 = __rdtsc();
                int ch = heap_ww_changed(t->addr, total, &b->ww_seen);
                cum_up_ww_tk += __rdtsc() - w0;
                // Every 4th period still hashes (LARECOMP_TEX_GPUW_EVERY=N): every STG is a raw address (translate.c), so
                // a store outside the storage buffers the shader has bound writes the heap unmarked (exec.c
                // vk_heap_gpu_written_since); this bounds such a miss to N periods (480 frames for others)
                static int every = -1;
                if (every < 0) { const char *e = getenv("LARECOMP_TEX_GPUW_EVERY"); every = e && atoi(e) > 0 ? (atoi(e) > 255 ? 255 : atoi(e)) : 4; }
                if (ch == 0 && b->gpuw_skips + 1 < every) { b->full_frame = frame; b->gpuw_skips++; prof_tex_gpuw_skip++; }   // (the write-watch check below finds it clean)
                else if (ch > 0) full = true, lf_why |= 2;
                // (tally without LARECOMP_VK_PROF, which light boots drop: periods due with no shader write, by outcome)
                static uint64_t per, sk, unk, wr;
                per++; sk += ch == 0 && b->full_frame == frame; unk += ch < 0; wr += ch > 0;
                if (per % 10000 == 0)
                    LOG("[tex] TEX_GPUW: %llu periods due, %llu skipped, %llu hashed (write watch unknown %llu, written %llu, every-%dth %llu)\n",
                        (unsigned long long)per, (unsigned long long)sk, (unsigned long long)(per - sk), (unsigned long long)unk,
                        (unsigned long long)wr, every, (unsigned long long)(per - sk - unk - wr));
            } else {
                uint64_t w0 = __rdtsc();
                gpuw_check = heap_ww_changed(t->addr, total, &b->ww_seen) == 0;
                cum_up_ww_tk += __rdtsc() - w0;
                full = true;
            }
        }
    }
    if (b->uploaded && !cpu_dirty && !always_full && (frame - b->full_frame < (b->dynamic ? 60u : 120u) || pre_first || rc_first)) {
        extern int heap_ww_changed(uint64_t addr, uint64_t size, uint64_t *seen);
        uint64_t w0 = __rdtsc();
        int ch = heap_ww_changed(t->addr, total, &b->ww_seen);
        cum_up_ww_tk += __rdtsc() - w0;
        // (a pre-uploaded texture's first use: shaders write the heap in play and the CPU write watch cannot see it, so a
        // shader write since the pre-upload's hash is a full check too)
        if (ch == 0 && (pre_first || rc_first) && b->full_seq) {
            extern bool vk_heap_gpu_written_since(uint64_t addr, uint64_t size, uint64_t since);
            if (vk_heap_gpu_written_since(t->addr, total, b->full_seq)) ch = 1, lf_why |= 4;
        }
        if (ch == 0 && !(b->tp > 0 && b->tp_gen != texpatch_generation())) { prof_tex_ww_clean++; return; }
        if (ch > 0) full = true, lf_why |= lf_why & 4 ? 0 : 2;   // written: hash it now (dynamic or not)
        // (no watch here: a full check, as the first upload it stands for. A recycled pick by default takes the sample hash
        // below instead, set at every full hash: no unbudgeted full hash per pick on Linux / Wine, 378-c)
        if (ch < 0 && (pre_first || (rc_first && g_rc_set))) full = true;
    }
    if (!full && frame - b->full_frame >= 120 && full_hash_budget >= total) { full = true; full_hash_budget -= total; lf_why |= 8; }
    uint64_t uh0 = __rdtsc();   // (TSC ticks, as cum_up_hash_tk)
    if (!b->tp) b->tp = texpatch_identify(t) + 1;  // (button prompts / Hero Mode icon: re-uploaded when their look changes)
    bool restyle = b->tp > 0 && b->uploaded && b->tp_gen != texpatch_generation();
    uint64_t uh1 = uh0;   // (cum_up_hash_tk: what is not counted yet)
    if (!full && !restyle) {
        prof_tex_samples++;
        uint64_t sh = fnv_sample(mem, total);
        uh1 = __rdtsc();
        cum_up_hash_tk += uh1 - uh0;
        if (sh == b->sample_hash) return;
        lf_why |= 16;
    }
    prof_tex_full++; prof_tex_full_bytes += total; if (b->dynamic) prof_tex_dyn++;
    const bool tprof = prof_up_ns != ~0ull;
    uint64_t th0 = tprof ? SDL_GetTicksNS() : 0;
    if (!b->uploaded) {   // (the write watch armed before the first hash: a texture pre-uploaded on the loading screen,
                          // preupload.c, is then clean at its first real use, unless written after this, which shows)
        extern int heap_ww_changed(uint64_t addr, uint64_t size, uint64_t *seen);
        heap_ww_changed(t->addr, total, &b->ww_seen);
        extern void vk_preupload_first_sight(NvnTexture *t, uint64_t bytes);
        vk_preupload_first_sight(t, total);   // (preupload.c's account of a load: what it did not take)
    }
    // (TOTK_GLITCH_DETECT: the write watch's level before the read, so a CPU write during the hash and copy below shows:
    // TXE_TORN, a texture uploaded while the game was writing it. A local level: b->ww_seen and the page stamps unchanged)
    extern bool vk_glitch_on(void);
    extern int heap_ww_changed(uint64_t addr, uint64_t size, uint64_t *seen);
    uint64_t torn_seen = 0;
    const bool torn_watch = vk_glitch_on() && heap_ww_changed(t->addr, total, &torn_seen) >= 0;
    uint64_t h = hash_tex(mem, total);
    if (gpuw_check) {   // (LARECOMP_TEX_GPUW=2)
        static uint64_t n, missed;
        n++;
        if (h != b->data_hash) {
            missed++;
            if (missed <= 20) LOG("[tex] TEX_GPUW check: the skip would have missed a change: %ux%u f%#x at %llx +%llu (%llu of %llu)\n",
                                  t->width, t->height, t->format, (unsigned long long)t->addr, (unsigned long long)total,
                                  (unsigned long long)missed, (unsigned long long)n);
        }
        if (n % 20000 == 0) LOG("[tex] TEX_GPUW check: %llu would-skips, %llu missed a change\n", (unsigned long long)n, (unsigned long long)missed);
    }
    b->full_frame = frame;
    extern uint64_t vk_heap_settled_seq(void);
    b->full_seq = vk_heap_settled_seq();   // (not the open submission: writes of earlier ones still in flight land after this hash)
    b->gpuw_skips = 0;
    b->sample_hash = fnv_sample(mem, total);
    uint64_t uh2 = __rdtsc();
    up_tk_hash += uh2 - uh0;
    cum_up_hash_tk += uh2 - uh1;
    cum_up_full++;
    uint64_t th1 = tprof ? SDL_GetTicksNS() : 0;
    if (tprof) texhash_report(frame);
    if (b->uploaded && h == b->data_hash && !restyle) { if (tprof) texhash_note(t, b, total, false, th1 - th0, 0); return; }
    if (tprof) texhash_note(t, b, total, b->uploaded, th1 - th0, 0);
    if (rc_first && b->uploaded && h != b->data_hash) {
        st_rc_new++;
        if (st_rc_new <= 64 || !(st_rc_new & (st_rc_new - 1)))   // (the first 64, then the 128th, 256th ...)
            LOG("[recycle] same-storage new bytes at the new object's first use: %p addr %llx %ux%u f%#x flags %#x (frame %llu; %llu of %llu re-checks)\n",
                (void *)t, (unsigned long long)t->addr, t->width, t->height, t->format, t->flags, (unsigned long long)frame,
                (unsigned long long)st_rc_new, (unsigned long long)st_rc_cpu);
    }
    if (b->uploaded && h != b->data_hash && !rc_first && frame - b->first_up_frame <= 8 && vk_glitch_on()) {
        // Late fill (hdr 11-05: cand2 c10_field 7287, sand drawn as grass for one frame, two textures first uploaded in
        // frame N and one of them uploaded again with new bytes at N + 1): new bytes within 8 frames of the first upload.
        // Says who wrote them (the check that saw it) and how the first upload came, once per texture.
        vk_txe_note(TXE_LATEFILL, t);
        static uint64_t n;
        if (n++ < 200)
            LOG("[tex] late fill: %p addr %llx %ux%u f%#x flags %#x: new bytes %llu frame(s) after its first upload (%s); "
                "seen by %s%s%s%s%s (TOTK_GLITCH_DETECT)\n", (void *)t, (unsigned long long)t->addr, t->width, t->height, t->format,
                t->flags, (unsigned long long)(frame - b->first_up_frame), b->first_pre ? "a stream pre-upload" : "at a draw",
                lf_why & 1 ? "WriteTexels " : "", lf_why & 2 ? "CPU write watch " : "", lf_why & 4 ? "shader-written heap " : "",
                lf_why & 8 ? "periodic full hash " : "", lf_why & 16 ? "sample hash" : "");
    }
    if (b->uploaded && h != b->data_hash && !pre_first && !rc_first) b->dynamic = true;   // (texels that came after a pre-upload: its data;
                                                                                         // a recycled image's)
    if (b->uploaded && h != b->data_hash && pre_first) {
        extern void vk_preupload_changed(uint64_t bytes);
        vk_preupload_changed(total);   // (preupload.c's account)
    }
    if (b->tp > 0) b->tp_gen = texpatch_generation();
    MOW_NOTE(t, b->uploaded ? MOW_UP_CHANGED : MOW_UP_FIRST);
    if (b->uploaded) vk_txe_note(TXE_CHANGED, NULL);   // (new bytes in an uploaded texture; counted only: several a frame would
                                                       // fill the frame's object list before the rare kinds)
    b->data_hash = h;
    if (!b->uploaded || rc_first) b->first_up_frame = frame, b->first_pre = pre_first || b->preuploaded;
    b->uploaded = true;
    if (lr_on()) lr_note(t);   // (LARECOMP_TEX_SUBMIT_RECHECK: asked again before this submission goes)
    b->twin_valid = false;   // (new texels: a depth twin is copied again before its next compare sample)
    if (!t->layout.pitch && t->layout.depth == 1) {   // LARECOMP_NVN_BLOCK_HEIGHT's blast radius: CPU data the two rules lay out differently
        uint32_t hb = (t->layout.height + t->layout.fmt->bh - 1) / t->layout.fmt->bh;
        uint32_t o = tegra_old_block_height_log2(hb), n = tegra_nvn_block_height_log2(hb);
        static uint64_t seen[512];
        static int nseen;
        uint64_t key = (uint64_t)t->format << 48 | (uint64_t)t->width << 24 | t->height;
        int i = 0;
        while (o != n && i < nseen && seen[i] != key) i++;
        if (o != n && i == nseen && nseen < 512) {
            seen[nseen++] = key;
            LOG("[tex] CPU upload, block height rules differ: fmt %#x %ux%u levels %u layers %u: in use %u GOBs (old %u, NVN %u)%s\n",
                t->format, t->width, t->height, t->layout.levels, t->layout.layers, 1u << t->layout.block_height_log2, 1u << o, 1u << n,
                t->layout.block_height_log2 != o && t->layout.block_height_log2 != n ? " (packaged)" : "");
        }
    }
    if (tex_watched(t->addr)) {
        uint64_t nz = 0;
        for (uint64_t i = 0; i < total; i++) nz += mem[i] != 0;
        char what[64];
        snprintf(what, sizeof what, "CPU upload %llu/%llu bytes nonzero", (unsigned long long)nz, (unsigned long long)total);
        tex_watch_note(t, b, what);
        LOG("[texw]   flags %#x stride %u target %u levels %u layout bh_log2 %u bd_log2 %u layers %u\n", t->flags, t->stride, t->target,
            t->layout.levels, t->layout.block_height_log2, t->layout.block_depth_log2, t->layout.layers);
        char path[96];   // raw guest bytes (offline: try linear / other block shapes, tools side)
        snprintf(path, sizeof path, "build/texw_%llx.bin", (unsigned long long)t->addr);
        FILE *f = fopen(path, "wb");
        if (f) { fwrite(mem, 1, total, f); fclose(f); LOG("[texw]   raw bytes -> %s\n", path); }
    }
    // unsupported format: nothing sensible to copy
    if (b->format != mf.vf && !b->astc_bw && !(mf.vf == VK_FORMAT_S8_UINT && b->aspect == VK_IMAGE_ASPECT_STENCIL_BIT)) return;
    uint64_t uc0 = __rdtsc();
    bool inl;
    VkCommandBuffer cb = up_cmd(b, &inl);
    uint32_t layers = t->layout.layers ? t->layout.layers : 1;
    if (layers > b->layers && b->depth == 1) layers = b->layers;
    uint64_t layer_size = tegra_layer_size(&t->layout);
    bool linear = nvn_flags_linear(t->flags);
    uint32_t levels = t->layout.levels < b->levels ? t->layout.levels : b->levels;
    AgBatch agb = {0};
    if (b->astc_bw && ag_mode == 2) ag_check_poll();
    for (uint32_t layer = 0; layer < layers; layer++) {
        for (uint32_t lv = 0; lv < levels; lv++) {
            LevelLayout l = tegra_level(&t->layout, lv);
            if (b->astc_bw) {  // blocks -> linear block rows -> RGBA8 texels
                const uint8_t *src = (const uint8_t *)(uintptr_t)t->addr + layer * layer_size + l.offset;
                if (!(b->tp > 0 && layer == 0)) {   // (texpatch works on the level copy: those keep the path below)
                    if (ag_on() && ag_level(cb, &agb, src, linear, t->stride, &l, b, lv, layer)) continue;
                    uint64_t ua0 = SDL_GetTicksNS();
                    vk_up_kind = 4; VkSpan s = vk_transient((uint64_t)l.w * l.h * 4, 16); vk_up_kind = 0;
                    uint64_t ua1 = SDL_GetTicksNS(); up_ns_alloc += ua1 - ua0;
                    bool par_ok = astc_decode_par_src(src, linear, t->stride, l.wb, l.hb, l.block_height_log2, l.block_depth_log2, l.w, l.h,
                                            b->astc_bw, b->astc_bh, b->astc_srgb, s.ptr);
                    up_ns_decode += SDL_GetTicksNS() - ua1;
                    if (par_ok) {
                        astc_copy_level(cb, s, &l, b, lv, layer);
                        continue;
                    }
                    uint8_t *blocks = malloc((uint64_t)l.wb * l.hb * 16);
                    if (!blocks) { level_skipped("ASTC", l.w, l.h); continue; }
                    if (linear) for (uint32_t y = 0; y < l.hb; y++) memcpy(blocks + (uint64_t)y * l.wb * 16, src + (uint64_t)y * t->stride, l.wb * 16);
                    else tegra_deswizzle(src, blocks, l.wb, l.hb, 1, 16, l.block_height_log2, l.block_depth_log2);
                    astc_decode_par(blocks, l.w, l.h, b->astc_bw, b->astc_bh, b->astc_srgb, s.ptr);
                    free(blocks);
                    astc_copy_level(cb, s, &l, b, lv, layer);
                    continue;
                }
                uint8_t *blocks = malloc((uint64_t)l.wb * l.hb * 16);
                if (!blocks) { level_skipped("ASTC", l.w, l.h); continue; }
                if (linear) for (uint32_t y = 0; y < l.hb; y++) memcpy(blocks + (uint64_t)y * l.wb * 16, src + (uint64_t)y * t->stride, l.wb * 16);
                else if (!deswizzle_par(src, blocks, l.wb, l.hb, 16, l.block_height_log2, l.block_depth_log2))
                    tegra_deswizzle(src, blocks, l.wb, l.hb, 1, 16, l.block_height_log2, l.block_depth_log2);
                if (b->tp > 0 && layer == 0) texpatch_apply(b->tp - 1, lv, blocks, l.w, l.h, l.wb * 16);
                vk_up_kind = 4; VkSpan s = vk_transient((uint64_t)l.w * l.h * 4, 16); vk_up_kind = 0;
                astc_decode_par(blocks, l.w, l.h, b->astc_bw, b->astc_bh, b->astc_srgb, s.ptr);
                free(blocks);
                astc_copy_level(cb, s, &l, b, lv, layer);
                continue;
            }
            uint32_t row = l.wb * fi->bpb;
            uint64_t sz = (uint64_t)row * l.hb * l.d;
            bool expand = mf.expand_rgb && fi->bpb == 3;
            uint32_t out_row = expand ? l.wb * 4 : row;
            vk_up_kind = 4; VkSpan s = vk_transient((uint64_t)out_row * l.hb * l.d, 16); vk_up_kind = 0;
            uint8_t *lin = expand ? malloc(sz) : s.ptr;
            if (!lin) { level_skipped("RGB8", l.w, l.h); continue; }
            const uint8_t *src = (const uint8_t *)(uintptr_t)t->addr + layer * layer_size + l.offset;
            if (linear) {
                for (uint32_t y = 0; y < l.hb; y++) memcpy(lin + (uint64_t)y * row, src + (uint64_t)y * t->stride, row);
            } else {
                uint64_t d0 = __rdtsc();
                bool split = l.d == 1 && deswizzle_par(src, lin, l.wb, l.hb, fi->bpb, l.block_height_log2, l.block_depth_log2);
                if (!split) tegra_deswizzle(src, lin, l.wb, l.hb, l.d, fi->bpb, l.block_height_log2, l.block_depth_log2);
                if (sz >= (1ull << 20)) desw_note(sz, split, __rdtsc() - d0);
            }
            if (b->tp > 0 && layer == 0) texpatch_apply(b->tp - 1, lv, lin, l.w, l.h, row);
            if (expand) {  // RGB8 -> RGBA8
                uint8_t *o = s.ptr;
                uint64_t n = (uint64_t)l.wb * l.hb * l.d;
                for (uint64_t i = 0; i < n; i++) { o[i * 4] = lin[i * 3]; o[i * 4 + 1] = lin[i * 3 + 1]; o[i * 4 + 2] = lin[i * 3 + 2]; o[i * 4 + 3] = 255; }
                free(lin);
            }
            uint32_t w = l.w, hh = l.h, d = l.d;
            // the copy stays inside the image's level: a texture allocated at the internal resolution (16:9, render
            // target sized) can still be filled from the CPU - the save slots' screenshots on the title screen. Its
            // full-size data overran the scaled image (only the width was clamped): GPU writes past the allocation,
            // a device loss below 1x after a seed switch.
            if (w > level_w(b, lv)) w = level_w(b, lv);
            if (hh > level_h(b, lv)) hh = level_h(b, lv);
            if (b->scalable && (w < level_w(b, lv) || hh < level_h(b, lv))) {
                // (above 1x the guest texels fill only the top-left of the scaled level: public #24's movie plane, which
                // came by a buffer copy, exec.c buf2tex_scaled; says whether a CPU-filled one shows up too)
                static int nlog;
                if (nlog++ < 8)
                    LOG("[tex] CPU upload into a scaled texture fills its top-left %ux%u of %ux%u: addr %llx %ux%u f%#x flags %#x kind %d\n",
                        w, hh, level_w(b, lv), level_h(b, lv), (unsigned long long)t->addr, t->width, t->height, t->format, t->flags, b->scale_kind);
            }
            VkBufferImageCopy rg = {.bufferOffset = s.off, .bufferRowLength = l.wb * fi->bw, .bufferImageHeight = l.hb * fi->bh,
                                    .imageSubresource = {b->aspect & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : b->aspect, lv,
                                                         b->depth > 1 ? 0 : layer, 1},
                                    .imageExtent = {w, b->base_type == VK_IMAGE_VIEW_TYPE_1D || b->base_type == VK_IMAGE_VIEW_TYPE_1D_ARRAY ? 1 : hh,
                                                    b->depth > 1 ? d : 1}};
            if (b->aspect == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) continue;  // packed D/S: no CPU layout
            vk_dl_note_upload((void *)b->image, rg.imageExtent.width, rg.imageExtent.height, rg.imageSubresource.mipLevel);
                vkCmdCopyBufferToImage(cb, s.buf, b->image, VK_IMAGE_LAYOUT_GENERAL, 1, &rg);
        }
    }
    ag_flush(cb, &agb, b);
    if (inl) vk_full_barrier(cb);   // (the draws after it read the new texels)
    const uint64_t uc1 = __rdtsc();
    cum_up_copy_tk += uc1 - uc0;
    cum_up_sent++;
    if (torn_watch && heap_ww_changed(t->addr, total, &torn_seen) > 0) vk_txe_note(TXE_TORN, t);
    if (uc1 - uh0 > hf_up_worst_tk) {
        hf_up_worst_tk = uc1 - uh0;
        snprintf(hf_up_worst, sizeof hf_up_worst, "%ux%ux%u fmt %#x levels %u layers %u, %.1f MB%s", t->width, t->height, t->depth,
                 t->format, levels, layers, total / 1048576.0, b->astc_bw ? (ag_mode ? ", ASTC on the GPU" : ", ASTC on the CPU") : "");
    }
}

// LARECOMP_TEX_WATCH=<addr hex>[,<addr hex>...]: log uploads, render-target use, copies and samples of the textures
// at those storage addresses ("[texw]"), to find who fills a texture (e.g. The 3D atmosphere LUT at 1061c5e000).
static uint64_t tw_extra[1024], tw_size[1024];   // added at runtime (LARECOMP_TEX_WATCH_FMT, objects.c nvnTextureInitialize; 64 ran out on terrain tiles)
static int tw_nextra;
void tex_watch_add(uint64_t addr, uint64_t size) {
    for (int i = 0; i < tw_nextra; i++) if (tw_extra[i] == addr) return;
    if (tw_nextra < 1024) { tw_size[tw_nextra] = size; tw_extra[tw_nextra++] = addr; }
}
// [a, a+n) overlaps a texture watched by format (memory copies into / out of its storage)
bool tex_watched_range(uint64_t a, uint64_t n) {
    for (int i = 0; i < tw_nextra; i++) if (a < tw_extra[i] + tw_size[i] && tw_extra[i] < a + n) return true;
    return false;
}
bool tex_watched(uint64_t addr) {
    for (int i = 0; i < tw_nextra; i++) if (tw_extra[i] == addr) return true;
    static uint64_t w[8];
    static int n = -1;
    if (n < 0) {
        n = 0;
        const char *e = getenv("LARECOMP_TEX_WATCH");
        while (e && *e && n < 8) {
            char *end;
            w[n++] = strtoull(e, &end, 16);
            e = *end == ',' ? end + 1 : "";
        }
    }
    for (int i = 0; i < n; i++)
        if (w[i] == addr) return true;
    return false;
}

static void tex_watch_note(NvnTexture *t, TexBackend *b, const char *what) {
    static int nlog;
    if (nlog++ >= 3000) return;   // (was 200: a watch by format (many tiles) used it up at load)
    LOG("[texw] %s addr %llx %ux%ux%u f%#x layers %u frame %llu owned %d uploaded %d\n", what, (unsigned long long)t->addr,
        t->width, t->height, t->depth, t->format, b ? b->layers : 0, (unsigned long long)vk_frame_counter(), b ? b->gpu_owned : -1,
        b ? b->uploaded : -1);
}

// LARECOMP_LOG_RT_FIRST=1: the first GPU write (render / copy target) to each (storage, format, size, levels), with the
// storage range, to find which object fills memory that another object later samples ("[rtfirst]"; 2026-10-16: terrain
// normal maps sampled at addresses no watched object ever rendered).
void rt_first_note(NvnTexture *t, const char *how) {
    static int on = -1;
    if (on < 0) on = getenv("LARECOMP_LOG_RT_FIRST") != NULL;
    if (!on || !t->addr) return;
    static uint64_t seen[1 << 16];
    static int nlog;
    uint64_t k = t->addr ^ ((uint64_t)t->format << 48) ^ ((uint64_t)t->width << 32) ^ ((uint64_t)t->height << 20) ^ t->levels;
    if (!k) k = 1;
    uint32_t s = (uint32_t)(k * 0x9E3779B97F4A7C15ull >> 48);
    for (int p = 0; p < 64; p++, s = (s + 1) & 0xffff) {
        if (seen[s] == k) return;
        if (!seen[s]) { seen[s] = k; break; }
    }
    if (nlog++ >= 20000) return;
    LOG("[rtfirst] %s addr %llx +%llx %ux%ux%u f%#x lv %u flags %#x target %u frame %llu obj %p\n", how, (unsigned long long)t->addr,
        (unsigned long long)tegra_total_size(&t->layout), t->width, t->height, t->depth, t->format, t->levels, t->flags, t->target,
        (unsigned long long)vk_frame_counter(), (void *)t);
}

// ---- write-back. An NVN texture is a view of pool memory: on hardware its rendered texels ARE its storage, and the
// CPU may move them. TotK renders each terrain tile's normal map once (260x260 RG8, 9 levels, flags 0x200), finalizes
// the object, moves the storage to a lower address with the CPU (heap compaction) and samples it through a new object
// there. Our rendering lives only in the VkImage, so a rendered texture the CPU could move (not COMPRESSIBLE 0x8 /
// DISPLAY 0x1: the game's per-frame targets are all 0x8) gets its texels copied back into its storage in the guest
// layout at the end of the execute that rendered it (one readback + one GPU wait per execute that has any, when the GPU
// would have written them on hardware). A target rendered again in a later frame is a per-frame target and is not
// written back again. LARECOMP_VK_WRITEBACK=0: off.
static NvnTexture *g_wb_pending[512];
static int g_wb_npending;
static uint64_t g_wb_n, g_wb_bytes, g_wb_ns, g_wb_flushes;
// Deferred write-back (LARECOMP_WB_DEFER=1, default off until its A/B; hdr 11-05): the flush above waited for the whole GPU
// queue (renderer_flush) at every new terrain tile: ~2.7 ms on an idle RTX 5080, 40-210 ms a tile with the GPU busy (the 9.3
// field soak). With it the copies go to a batch's own read-back buffer, the submission is only submitted, and the texels
// reach the guest storage once the GPU has done it: at the next execute or present that sees it done (render thread), or
// before any SyncWait / QueueFinish that waited for it returns to the game (game thread: renderer_wait_seq / renderer_sync),
// which is where the game may read or move the storage. So the game never gets past a fence covering the render without the
// texels, as with the full wait. A texture rendered again in a later frame before its batch is done becomes a per-frame
// target (wb_state 3) and its first rendering still lands; in the same frame its newer rendering replaces it (the next
// flush); one finalized meanwhile is dropped (wb_forget). Batches hold up to WBB_T textures, WBB_N in flight; anything that
// does not fit takes the full wait as before. wb_state 4: copied, its batch not done yet. g_wbb_mu guards the batches and the
// wb_state of their textures.
#define WBB_N 8
#define WBB_T 64
typedef struct {
    VkBuffer buf;
    VmaAllocation mem;
    uint8_t *ptr;
    uint64_t cap, seq;
    bool busy;
    int n;
    NvnTexture *t[WBB_T];
    uint64_t off[WBB_T];
    uint32_t levels[WBB_T];
    uint64_t ww[WBB_T];  // (the write watch's level for its storage at the record)
    bool cpu[WBB_T];      // (the game wrote its storage, seen before a neighbour's texels went into a shared page)
} WbBatch;
static WbBatch g_wbb[WBB_N];
static pthread_mutex_t g_wbb_mu = PTHREAD_MUTEX_INITIALIZER;
static int g_wbb_busy;   // (batches in flight: the polls skip the lock while 0)
static uint64_t g_wbb_done_rt, g_wbb_done_sync, g_wbb_fallback, g_wbb_dropped, g_wbb_cpu_skip;
extern int heap_ww_changed(uint64_t addr, uint64_t size, uint64_t *seen);   // (hle_os_mem.cpp)
static int wb_defer_on(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("LARECOMP_WB_DEFER");
        on = e && *e == '1';
        if (on) LOG("[wb] deferred write-back on (LARECOMP_WB_DEFER=1): texels reach the storage when the GPU is done, no full wait\n");
    }
    return on;
}
static void wb_forget(NvnTexture *t) {   // (queued outside an execute, e.g. by a present, then finalized)
    for (int i = 0; i < g_wb_npending; i++)
        if (g_wb_pending[i] == t) { g_wb_pending[i] = g_wb_pending[--g_wb_npending]; i--; }
    if (!__atomic_load_n(&g_wbb_busy, __ATOMIC_ACQUIRE)) return;
    pthread_mutex_lock(&g_wbb_mu);
    for (int k = 0; k < WBB_N; k++)
        for (int i = 0; g_wbb[k].busy && i < g_wbb[k].n; i++)
            if (g_wbb[k].t[i] == t) { g_wbb[k].t[i] = NULL; g_wbb_dropped++; }
    pthread_mutex_unlock(&g_wbb_mu);
}
static int wb_on(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("LARECOMP_VK_WRITEBACK"); on = !(e && *e == '0'); }
    return on;
}
static bool rb_candidate(NvnTexture *t, TexBackend *b);
static bool wb_candidate(NvnTexture *t, TexBackend *b) {
    const FormatInfo *fi = t->layout.fmt;
    if (rb_candidate(t, b)) return false;   // (a CPU-read target: written back after each pass, without a wait, below)
    if (!t->addr || (t->flags & (0x1 | 0x8)) || t->target != 1 || t->samples > 1) return false;
    if (!b->image || b->samples > 1 || b->astc_bw || b->blk || b->pending_rgba || b->aspect != VK_IMAGE_ASPECT_COLOR_BIT) return false;
    if (b->layers > 1 || b->depth > 1) return false;
    if (!fi || !fi->bpb || fi->bpb == 3 || fi->bw != 1 || fi->bh != 1 || b->format != fmt_of(t->format).vf) return false;
    if (tegra_total_size(&t->layout) > (4u << 20)) return false;
    if (b->scalable && (b->sx != 1 || b->sy != 1)) {
        static int said;
        if (t->width == 1920 && t->height == 1080 && renderer_out_scale_active() && !said++)
            LOG("[wb] %llx 1920x1080 f%#x would be written back but is at the output canvas scale (TOTK_OUT_SCALE)\n",
                (unsigned long long)t->addr, t->format);
        return false;
    }
    return true;
}
static void wb_note_render(NvnTexture *t, TexBackend *b) {
    if (!wb_on() || !b || b->wb_state == 3) return;
    uint64_t f1 = vk_frame_counter() + 1;
    if (b->wb_state == 1) return;                                        // queued already in this execute
    if (b->wb_state == 4) {   // (its batch not done yet)
        // Rendered again in a later frame: a per-frame target from now on (wb_state 3), but its first rendering still
        // lands when the batch is done, as the full wait wrote it at once. In the same frame: the newer texels replace
        // it (dropped from the batch, queued again below: the next flush has them).
        pthread_mutex_lock(&g_wbb_mu);
        const bool later = b->wb_state == 4 && b->wb_frame != f1;
        if (b->wb_state == 4 && !later)
            for (int k = 0; k < WBB_N; k++)
                for (int i = 0; g_wbb[k].busy && i < g_wbb[k].n; i++)
                    if (g_wbb[k].t[i] == t) { g_wbb[k].t[i] = NULL; g_wbb_dropped++; }
        if (b->wb_state == 4) b->wb_state = later ? 3 : 0;
        pthread_mutex_unlock(&g_wbb_mu);
        if (later) { MOW_NOTE(t, MOW_WB_STOP); return; }
    }
    if (b->wb_state == 2 && b->wb_frame != f1) { b->wb_state = 3; MOW_NOTE(t, MOW_WB_STOP); return; }  // rendered again in a later frame
    if (!wb_candidate(t, b)) { b->wb_state = 3; return; }
    if (g_wb_npending == (int)(sizeof g_wb_pending / sizeof *g_wb_pending)) return;   // (full: not mid-pass; a later render queues it)
    b->wb_state = 1;
    b->wb_frame = f1;
    g_wb_pending[g_wb_npending++] = t;
}
// One texture's read-back texels (levels from src) into its guest storage, in the guest layout; returns the bytes
static uint64_t wb_swizzle(NvnTexture *t, uint32_t levels, const uint8_t *src) {
    const FormatInfo *fi = t->layout.fmt;
    bool linear = nvn_flags_linear(t->flags);
    uint64_t o = 0;
    for (uint32_t lv = 0; lv < levels; lv++) {
        LevelLayout l = tegra_level(&t->layout, lv);
        uint8_t *dst = (uint8_t *)(uintptr_t)t->addr + l.offset;
        uint32_t row = l.wb * fi->bpb;
        if (linear) for (uint32_t y = 0; y < l.hb; y++) memcpy(dst + (uint64_t)y * t->stride, src + o + (uint64_t)y * row, row);
        else tegra_swizzle(src + o, dst, l.wb, l.hb, l.d, fi->bpb, l.block_height_log2, l.block_depth_log2);
        o += ((uint64_t)row * l.hb * l.d + 15) & ~15ull;
    }
    return o;
}
static void wb_note_done(NvnTexture *t, uint32_t levels, uint64_t bytes, bool render_thread) {
    if (render_thread) { MOW_NOTE(t, MOW_WB); vk_txe_note(TXE_WB, t); }   // (render-thread state: not from a game thread)
    __atomic_add_fetch(&g_wb_bytes, bytes, __ATOMIC_RELAXED);   // (a game thread too: LARECOMP_WB_DEFER)
    if (tex_watched(t->addr) || g_wb_n < 4 || !(g_wb_flushes % 64))
        LOG("[wb] %p addr %llx %ux%u f%#x flags %#x lv %u: %llu bytes written back (frame %llu)\n", (void *)t, (unsigned long long)t->addr,
            t->width, t->height, t->format, t->flags, levels, (unsigned long long)bytes, (unsigned long long)vk_frame_counter());
    __atomic_add_fetch(&g_wb_n, 1, __ATOMIC_RELAXED);
}
// Pending textures sharing a 4 KB page with t, around t's own swizzle (g_wbb_mu held): our write into a shared page must
// not read as the game's at their turn. Before it: one the game already wrote is marked (skipped at its turn), the others
// take the level now; after it: they take the level again, which absorbs t's write.
static bool wb_share_page(const NvnTexture *a, const NvnTexture *b) {
    const uint64_t a0 = a->addr & ~0xFFFull, a1 = (a->addr + tegra_total_size(&a->layout) + 0xFFF) & ~0xFFFull;
    const uint64_t b0 = b->addr & ~0xFFFull, b1 = (b->addr + tegra_total_size(&b->layout) + 0xFFF) & ~0xFFFull;
    return a0 < b1 && b0 < a1;
}
static void wb_neighbours(const NvnTexture *t, bool after) {
    for (int k = 0; k < WBB_N; k++)
        for (int i = 0; g_wbb[k].busy && i < g_wbb[k].n; i++) {
            NvnTexture *y = g_wbb[k].t[i];
            if (!y || y == t || g_wbb[k].cpu[i] || !wb_share_page(t, y)) continue;
            uint64_t seen = g_wbb[k].ww[i];
            const int ch = heap_ww_changed(y->addr, tegra_total_size(&y->layout), &seen);
            if (!after && ch > 0) g_wbb[k].cpu[i] = true;
            else g_wbb[k].ww[i] = seen;
        }
}
// Batches whose submission the GPU has done (seq <= done) into the guest storage, oldest first. render_thread: the render
// thread's poll (each execute); else a game thread in renderer_wait_seq / renderer_sync after its GPU wait.
void vk_writeback_complete(uint64_t done, bool render_thread) {
    if (!__atomic_load_n(&g_wbb_busy, __ATOMIC_ACQUIRE)) return;
    pthread_mutex_lock(&g_wbb_mu);
    for (;;) {
        WbBatch *w = NULL;
        for (int k = 0; k < WBB_N; k++)
            if (g_wbb[k].busy && g_wbb[k].seq <= done && (!w || g_wbb[k].seq < w->seq)) w = &g_wbb[k];
        if (!w) break;
        for (int i = 0; i < w->n; i++) {
            NvnTexture *t = w->t[i];
            if (!t) continue;
            TexBackend *b = t->backend;
            // The game wrote the storage after the record (a lagged SyncWait let it reuse the storage before this batch
            // was done): its bytes win; the texels would land over them as a lasting wrong tile
            uint64_t seen = w->ww[i];
            if (w->cpu[i] || heap_ww_changed(t->addr, tegra_total_size(&t->layout), &seen) > 0) {
                g_wbb_cpu_skip++;
                // (2 = the full wait's end state: the storage is the truth, so alias_writer's other-format rule keeps a
                // reused storage's new texture off this writer's image)
                if (b && b->wb_state == 4) b->wb_state = 2;
                continue;
            }
            wb_neighbours(t, false);
            const uint64_t bytes = wb_swizzle(t, w->levels[i], w->ptr + w->off[i]);
            wb_neighbours(t, true);
            if (b && b->wb_state == 4) b->wb_state = 2;   // (3: rendered again in a later frame meanwhile, stays 3)
            wb_note_done(t, w->levels[i], bytes, render_thread);
        }
        if (render_thread) g_wbb_done_rt++; else g_wbb_done_sync++;
        w->busy = false, w->n = 0;
        __atomic_sub_fetch(&g_wbb_busy, 1, __ATOMIC_RELEASE);
    }
    pthread_mutex_unlock(&g_wbb_mu);
}
// A free batch with room for `need` bytes (render thread; a busy one is never resized), or NULL
static WbBatch *wbb_take(uint64_t need) {
    WbBatch *w = NULL;
    pthread_mutex_lock(&g_wbb_mu);
    for (int k = 0; k < WBB_N && !w; k++) if (!g_wbb[k].busy && g_wbb[k].cap >= need) w = &g_wbb[k];
    for (int k = 0; k < WBB_N && !w; k++) if (!g_wbb[k].busy) w = &g_wbb[k];
    pthread_mutex_unlock(&g_wbb_mu);
    if (!w || w->cap >= need) return w;
    if (w->buf) vmaDestroyBuffer(g_vk.vma, w->buf, w->mem);   // (not busy: its last copy is done and consumed)
    w->buf = VK_NULL_HANDLE, w->ptr = NULL, w->cap = 0;
    const uint64_t cap = (need + (1 << 20) - 1) & ~((1ull << 20) - 1);
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = cap, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                                  .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT};
    VmaAllocationInfo info;
    if (vmaCreateBuffer(g_vk.vma, &bi, &ai, &w->buf, &w->mem, &info) != VK_SUCCESS || !info.pMappedData) {
        if (w->buf) vmaDestroyBuffer(g_vk.vma, w->buf, w->mem);
        w->buf = VK_NULL_HANDLE;
        return NULL;
    }
    w->ptr = (uint8_t *)info.pMappedData, w->cap = cap;
    return w;
}
void vk_writeback_flush(void) {
    if (!g_wb_npending) return;
    extern void renderer_flush(void), renderer_submit(void);
    uint64_t t0 = SDL_GetTicksNS();
    uint64_t off[512], need = 0;
    for (int i = 0; i < g_wb_npending; i++) {
        NvnTexture *t = g_wb_pending[i];
        TexBackend *b = t->backend;
        uint32_t levels = t->layout.levels < b->levels ? t->layout.levels : b->levels;
        off[i] = need;
        for (uint32_t lv = 0; lv < levels; lv++) {
            LevelLayout l = tegra_level(&t->layout, lv);
            need += ((uint64_t)l.wb * t->layout.fmt->bpb * l.hb * l.d + 15) & ~15ull;
        }
    }
    // (deferred only where the write watch answers for every texture: without it a game write before the batch is done
    // could not be told apart, so Linux by default and storage outside the heap take the full wait)
    uint64_t wwl[WBB_T];
    bool watched = wb_defer_on() && g_wb_npending <= WBB_T;
    for (int i = 0; watched && i < g_wb_npending; i++) {
        NvnTexture *t = g_wb_pending[i];
        wwl[i] = 0;
        watched = heap_ww_changed(t->addr, tegra_total_size(&t->layout), &wwl[i]) >= 0;
    }
    WbBatch *wb = watched ? wbb_take(need) : NULL;
    if (wb_defer_on() && !wb) g_wbb_fallback++;
    vk_end_pass();
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);
    VkSpan out = wb ? (VkSpan){.buf = wb->buf, .off = 0, .ptr = wb->ptr} : vk_readback(need);
    for (int i = 0; i < g_wb_npending; i++) {
        NvnTexture *t = g_wb_pending[i];
        TexBackend *b = t->backend;
        uint32_t levels = t->layout.levels < b->levels ? t->layout.levels : b->levels;
        uint64_t o = off[i];
        for (uint32_t lv = 0; lv < levels; lv++) {
            LevelLayout l = tegra_level(&t->layout, lv);
            VkBufferImageCopy c = {.bufferOffset = out.off + o, .bufferRowLength = l.wb, .bufferImageHeight = l.hb,
                                   .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, lv, 0, 1},
                                   .imageExtent = {l.w < level_w(b, lv) ? l.w : level_w(b, lv), l.h < level_h(b, lv) ? l.h : level_h(b, lv), 1}};
            vkCmdCopyImageToBuffer(cb, b->image, VK_IMAGE_LAYOUT_GENERAL, out.buf, 1, &c);
            o += ((uint64_t)l.wb * t->layout.fmt->bpb * l.hb * l.d + 15) & ~15ull;
        }
    }
    if (wb) {   // (deferred: the batch is done by vk_writeback_complete once the GPU has run this submission)
        const uint64_t seq = vk_pending_seq();
        pthread_mutex_lock(&g_wbb_mu);
        wb->seq = seq, wb->n = g_wb_npending;
        for (int i = 0; i < g_wb_npending; i++) {
            NvnTexture *t = g_wb_pending[i];
            TexBackend *b = t->backend;
            wb->t[i] = t;
            wb->off[i] = off[i];
            wb->levels[i] = t->layout.levels < b->levels ? t->layout.levels : b->levels;
            wb->ww[i] = wwl[i];
            wb->cpu[i] = false;
            b->wb_state = 4;
        }
        wb->busy = true;
        __atomic_add_fetch(&g_wbb_busy, 1, __ATOMIC_RELEASE);
        pthread_mutex_unlock(&g_wbb_mu);
        renderer_submit();
    } else {
        renderer_flush();
        for (int i = 0; i < g_wb_npending; i++) {
            NvnTexture *t = g_wb_pending[i];
            TexBackend *b = t->backend;
            uint32_t levels = t->layout.levels < b->levels ? t->layout.levels : b->levels;
            // (a deferred batch's tile may share a page: our write must not read as the game's at its turn, MUST 2)
            const bool nb = __atomic_load_n(&g_wbb_busy, __ATOMIC_ACQUIRE) != 0;
            if (nb) { pthread_mutex_lock(&g_wbb_mu); wb_neighbours(t, false); }
            const uint64_t bytes = wb_swizzle(t, levels, (const uint8_t *)out.ptr + off[i]);
            if (nb) { wb_neighbours(t, true); pthread_mutex_unlock(&g_wbb_mu); }
            b->wb_state = 2;
            wb_note_done(t, levels, bytes, true);
        }
    }
    g_wb_ns += SDL_GetTicksNS() - t0;
    g_wb_flushes++;
    if (g_wb_flushes <= 8 || !(g_wb_flushes % 64))
        LOG("[wb] flush %llu: %d textures, %llu KB (total %llu textures, %llu MB, %.1f ms)%s\n", (unsigned long long)g_wb_flushes,
            g_wb_npending, (unsigned long long)(need >> 10), (unsigned long long)g_wb_n, (unsigned long long)(g_wb_bytes >> 20),
            g_wb_ns / 1e6, wb ? " deferred" : "");
    if (wb_defer_on() && !(g_wb_flushes % 64))
        LOG("[wb] deferred: batches done on the render thread %llu, at a game wait %llu; full waits (no batch free / too many / "
            "no write watch) %llu; textures dropped (rendered again / finalized first) %llu, skipped (the game wrote the storage "
            "first) %llu\n", (unsigned long long)g_wbb_done_rt, (unsigned long long)g_wbb_done_sync, (unsigned long long)g_wbb_fallback,
            (unsigned long long)g_wbb_dropped, (unsigned long long)g_wbb_cpu_skip);
    g_wb_npending = 0;
}

// ---- CPU-read targets (public #16: the Purah Pad camera recognised nothing, on every GPU). The camera's compendium target comes from a GPU pick: ModelSceneExtension's
// analyze passes reduce the picture-book models under the reticle into three small targets (4x16-bit uint slot pointer, two float screen boxes; 1x1 linear in B's v0.1.8
// log), and the next frame the CPU invalidates their pool range and reads texel (0,0) from their storage, without a wait (calcViewGPU_ 0x10009b2968, through
// agl::TextureData::invalidateCPUCache 0x10016326a4: nvnMemoryPoolInvalidateMappedRange when the pool is CPU_CACHED). Our rendering lived only in the VkImage, so the
// CPU read zeros = no target. They are rendered every frame (double-buffered), so the one-shot write-back above drops them. A small colour target in a CPU_CACHED pool
// instead has its texels copied, right after each pass that rendered it, into a staging buffer of ours (a GPU copy, no wait; three generations a target, so the GPU
// writes one while the CPU may read another), and the guest storage is written only when the game asks for it: its nvnMemoryPoolInvalidateMappedRange over the target's
// bytes (objects.c -> vk_cpu_read_serve), on the game thread, from the newest generation the GPU has finished. The guest bytes are then written at the moment the game
// itself declares them live and about to be read, only inside the range it named, only the target's own texels (its guest-layout runs). The first version copied
// straight into the guest heap whenever the GPU got there; SyncWait's lag + ticket lag (objects.c) return the game's wait on frame N's fence once only frame N-1 is done
// on the GPU, so the game could free or reuse the pool memory first and the copy then wrote the target's texels into whatever lived there (found while chasing B's
// v0.1.8 crash; not its cause). The data the CPU gets is that of the newest finished frame (one or two frames older than on hardware; the camera's pick does not mind).
// A generation older than RB_MAX_AGE presents is never served. A target in a CPU_UNCACHED pool is not served (the game reads it without an invalidate: logged). Only
// whole small targets, one level: a few dozen bytes a pass. On by default since v0.1.9; LARECOMP_VK_CPU_READ_RT=0 turns it off. Known residual (low, saves-compat): a
// CPU write into such a target after its render and the game's invalidate is overwritten by the older GPU texels.
#define RB_MAX_TEXELS 256u
#define RB_MAX_REGIONS 256
#define RB_MAX_RUNS RB_MAX_REGIONS   // (guest-layout runs a target: block-linear 16-byte runs, linear one per row)
#define RB_SLOTS 16
#define RB_GENS 3
#define RB_MAX_AGE 8
typedef struct {
    uint64_t addr;               // the target's guest storage (0: free slot)
    uint32_t size;               // bytes of one generation (the runs' end)
    VkBuffer buf;
    VmaAllocation mem;
    uint8_t *map;
    uint32_t nrun, run_off[RB_MAX_RUNS], run_len[RB_MAX_RUNS];   // guest-layout runs (offsets from addr)
    uint64_t seq[RB_GENS];       // the submission holding each generation's copy (UINT64_MAX: being written / none)
    uint64_t frame[RB_GENS];     // the present it was rendered in
    uint32_t next;               // the generation the next copy goes to
    uint64_t last_note;          // the present of the last pass that rendered it
    uint32_t inc;                // incarnation: bumped when it is dropped or made for another target (a pass's copy into
                                 // it from before then is not stamped, vk_cpu_read_flush)
} RbSlot;
static RbSlot g_rbs[RB_SLOTS];
static pthread_mutex_t g_rb_lock = PTHREAD_MUTEX_INITIALIZER;   // (g_rbs: the render thread's notes / flushes, the game's serves)
typedef struct { TexBackend *b; VkImage image; VkBuffer buf; int slot; uint32_t gen, first, n, inc; } RbCopy;
static RbCopy g_rb[8];
static VkBufferImageCopy g_rb_reg[RB_MAX_REGIONS];
static int g_rb_n;
static uint32_t g_rb_nreg;
static uint64_t g_rb_copies, g_rb_served;
static int rb_on(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("LARECOMP_VK_CPU_READ_RT");
        on = !(e && *e == '0');
        LOG(on ? "[cpu-read] small CPU-cached targets kept by the GPU after each pass, written to guest storage when the game "
                 "invalidates their range to read them (LARECOMP_VK_CPU_READ_RT=0: off)\n"
               : "[cpu-read] off (LARECOMP_VK_CPU_READ_RT=0): CPU reads of rendered targets see zeros (the Purah Pad camera)\n");
    }
    return on;
}
static bool rb_candidate(NvnTexture *t, TexBackend *b) {
    const FormatInfo *fi = t->layout.fmt;
    if (!rb_on() || !t->addr || !t->pool || !(t->pool->flags & (0x2 | 0x4)) || t->target != 1 || t->samples > 1) return false;
    if (!b->image || b->samples > 1 || b->astc_bw || b->blk || b->pending_rgba || b->aspect != VK_IMAGE_ASPECT_COLOR_BIT) return false;
    if (b->layers > 1 || b->depth > 1 || t->layout.levels != 1 || (uint64_t)t->width * t->height > RB_MAX_TEXELS) return false;
    if (!fi || !fi->bpb || fi->bpb > 16 || (fi->bpb & (fi->bpb - 1)) || fi->bw != 1 || fi->bh != 1 || b->format != fmt_of(t->format).vf)
        return false;
    if (b->scalable && (b->sx != 1 || b->sy != 1)) return false;
    LevelLayout l = tegra_level(&t->layout, 0);
    return !nvn_flags_linear(t->flags) || (t->stride % fi->bpb == 0 && t->stride >= (uint64_t)l.wb * fi->bpb);
}
// The slot of target t at its runs (made or re-made when its storage or layout changed); -1: none free. Under g_rb_lock.
static int rb_slot(NvnTexture *t, const uint32_t *off, const uint32_t *len, uint32_t nrun) {
    uint32_t size = 0;
    for (uint32_t i = 0; i < nrun; i++) if (off[i] + len[i] > size) size = off[i] + len[i];
    size = (size + 255) & ~255u;
    int s = -1, freei = -1, oldest = -1;
    for (int i = 0; i < RB_SLOTS; i++) {
        if (g_rbs[i].addr == t->addr) { s = i; break; }
        if (!g_rbs[i].addr && freei < 0) freei = i;
        if (g_rbs[i].addr && (oldest < 0 || g_rbs[i].last_note < g_rbs[oldest].last_note)) oldest = i;
    }
    // (a slot not rendered for a while is taken over: its generations are all finished or never will be)
    if (s < 0) s = freei >= 0 ? freei : oldest >= 0 && renderer_present_count() > g_rbs[oldest].last_note + 2 * RB_MAX_AGE ? oldest : -1;
    if (s < 0) return -1;
    RbSlot *r = &g_rbs[s];
    bool same = r->addr == t->addr && r->size == size && r->nrun == nrun && !memcmp(r->run_off, off, nrun * sizeof *off) &&
                !memcmp(r->run_len, len, nrun * sizeof *len);
    if (!same) {
        if (r->buf) { defer((Dead){.buf = r->buf, .mem = r->mem}); r->buf = VK_NULL_HANDLE; r->map = NULL; }
        VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = (VkDeviceSize)size * RB_GENS,
                                 .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                                      .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT};
        VmaAllocationInfo info;
        if (vmaCreateBuffer(g_vk.vma, &bi, &ai, &r->buf, &r->mem, &info) != VK_SUCCESS) {
            const uint32_t inc = r->inc + 1;   // (kept moving: a copy noted before still names the old incarnation)
            memset(r, 0, sizeof *r);
            r->inc = inc;
            for (int g = 0; g < RB_GENS; g++) r->seq[g] = UINT64_MAX;
            return -1;
        }
        r->map = info.pMappedData;
        r->addr = t->addr, r->size = size, r->nrun = nrun, r->next = 0, r->inc++;
        memcpy(r->run_off, off, nrun * sizeof *off), memcpy(r->run_len, len, nrun * sizeof *len);
        for (int g = 0; g < RB_GENS; g++) r->seq[g] = UINT64_MAX, r->frame[g] = 0;
    }
    return s;
}
void vk_cpu_read_begin(void) { g_rb_n = 0; g_rb_nreg = 0; }
void vk_cpu_read_note(NvnTexture *t, TexBackend *b) {
    if (g_rb_n == (int)(sizeof g_rb / sizeof *g_rb) || !rb_candidate(t, b)) return;
    if (!(t->pool->flags & 0x4)) {   // (CPU_UNCACHED: read without an invalidate, so never served)
        static int said;
        if (!said++) LOG("[cpu-read] %llx %ux%u f%#x in a CPU-uncached pool (flags %#x): not served (no invalidate to answer)\n",
                         (unsigned long long)t->addr, t->width, t->height, t->format, t->pool->flags);
        return;
    }
    const FormatInfo *fi = t->layout.fmt;
    const uint32_t bpb = fi->bpb;
    LevelLayout l = tegra_level(&t->layout, 0);
    const uint32_t w = l.w < level_w(b, 0) ? l.w : level_w(b, 0), h = l.h < level_h(b, 0) ? l.h : level_h(b, 0);
    const bool linear = nvn_flags_linear(t->flags);
    const uint32_t chunk = 16 / bpb;   // (block-linear: runs of 16 contiguous bytes, one region each)
    const uint32_t nreg = linear ? 1 : h * ((w + chunk - 1) / chunk), nrun = linear ? h : nreg;
    if (g_rb_nreg + nreg > RB_MAX_REGIONS || nrun > RB_MAX_RUNS) return;
    uint32_t off[RB_MAX_RUNS], len[RB_MAX_RUNS];
    VkBufferImageCopy reg[RB_MAX_RUNS];
    if (linear) {
        for (uint32_t y = 0; y < h; y++) off[y] = (uint32_t)(y * t->stride), len[y] = w * bpb;
        reg[0] = (VkBufferImageCopy){.bufferRowLength = (uint32_t)(t->stride / bpb), .bufferImageHeight = h,
                                     .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {w, h, 1}};
    } else {
        uint32_t k = 0;
        for (uint32_t y = 0; y < h; y++)
            for (uint32_t x = 0; x < w; x += chunk, k++) {
                const uint32_t cw = w - x < chunk ? w - x : chunk;
                off[k] = (uint32_t)tegra_block_linear_offset(x * bpb, y, l.wb, bpb, l.block_height_log2), len[k] = cw * bpb;
                reg[k] = (VkBufferImageCopy){.bufferOffset = off[k], .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                             .imageOffset = {(int32_t)x, (int32_t)y, 0}, .imageExtent = {cw, 1, 1}};
            }
    }
    pthread_mutex_lock(&g_rb_lock);
    // (the game finalized the texture or its pool: this pass was recorded before that, and its storage may be another
    // resource's by now; vk_cpu_read_forget dropped its slot, so it is not made again. Read under the lock the forget
    // takes after setting them.)
    if (t->fin_queued || t->zombie || t->pool->finalized) { pthread_mutex_unlock(&g_rb_lock); return; }
    const int s = rb_slot(t, off, len, nrun);
    uint32_t gen = 0, inc = 0;
    VkBuffer sbuf = VK_NULL_HANDLE;
    if (s >= 0) {
        RbSlot *r = &g_rbs[s];
        gen = r->next, r->next = (r->next + 1) % RB_GENS;
        r->seq[gen] = UINT64_MAX;   // (being written: not served until its submission is done, vk_cpu_read_flush)
        r->last_note = renderer_present_count();
        inc = r->inc, sbuf = r->buf;
    }
    pthread_mutex_unlock(&g_rb_lock);
    if (s < 0) {
        static int said;
        if (!said++) LOG("[cpu-read] no free slot for %llx (%d targets kept)\n", (unsigned long long)t->addr, RB_SLOTS);
        return;
    }
    const VkDeviceSize base = (VkDeviceSize)gen * g_rbs[s].size;
    for (uint32_t i = 0; i < nreg; i++) reg[i].bufferOffset += base, g_rb_reg[g_rb_nreg + i] = reg[i];
    g_rb[g_rb_n++] = (RbCopy){b, b->image, sbuf, s, gen, g_rb_nreg, nreg, inc};
    g_rb_nreg += nreg;
    static uint64_t seen[16];
    static int nseen;
    bool known = false;
    for (int i = 0; i < nseen; i++) known |= seen[i] == t->addr;
    if (!known && nseen < 16) {
        seen[nseen++] = t->addr;
        LOG("[cpu-read] %llx %ux%u f%#x flags %#x pool flags %#x %s: kept after each pass, to guest storage at the game's "
            "invalidate (LARECOMP_VK_CPU_READ_RT)\n", (unsigned long long)t->addr, t->width, t->height, t->format, t->flags,
            t->pool->flags, linear ? "linear" : "block-linear");
    }
}
void vk_cpu_read_flush(VkCommandBuffer cb) {
    if (!g_rb_n) return;
    vk_full_barrier(cb);   // (the pass's attachment writes -> the copies' reads)
    const uint64_t seq = vk_pending_seq(), frame = renderer_present_count();
    bool copied[8] = {false};
    for (int i = 0; i < g_rb_n; i++) {
        const RbCopy *r = &g_rb[i];
        if (r->b->image != r->image || r->b->fbl_in) continue;   // (re-created since the pass began / still in the feedback layout)
        vkCmdCopyImageToBuffer(cb, r->image, VK_IMAGE_LAYOUT_GENERAL, r->buf, r->n, g_rb_reg + r->first);   // (deferred if re-made)
        copied[i] = true;
        if (!(g_rb_copies++ & 0x3FFF)) LOG("[cpu-read] %llu target copies so far, %llu served (frame %llu)\n",
                                           (unsigned long long)g_rb_copies, (unsigned long long)g_rb_served,
                                           (unsigned long long)vk_frame_counter());
    }
    // (the copies -> the host's reads once the submission completed; and the image's next writes after these reads)
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
                           .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                           .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT | VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT | VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    vkCmdPipelineBarrier2(cb, &di);
    pthread_mutex_lock(&g_rb_lock);   // (each generation served once its submission is done; a skipped copy never)
    for (int i = 0; i < g_rb_n; i++)
        if (copied[i] && g_rbs[g_rb[i].slot].inc == g_rb[i].inc)
            g_rbs[g_rb[i].slot].seq[g_rb[i].gen] = seq, g_rbs[g_rb[i].slot].frame[g_rb[i].gen] = frame;
    pthread_mutex_unlock(&g_rb_lock);
    g_rb_n = 0;
    g_rb_nreg = 0;
}
// objects.c nvnTextureFinalize / nvnMemoryPoolFinalize, on the game thread (after marking the texture / pool, so a note
// replayed later does not make the slot again): the targets kept at [addr, addr + size) are dropped before the game can
// free or reuse that storage, so nothing is served into it afterwards. The staging buffer stays with the slot (re-made,
// deferred, by the render thread when it is taken again).
void vk_cpu_read_forget(uint64_t addr, uint64_t size) {
    if (!rb_on() || !size) return;
    pthread_mutex_lock(&g_rb_lock);
    for (int i = 0; i < RB_SLOTS; i++) {
        RbSlot *r = &g_rbs[i];
        if (!r->addr || r->addr < addr || r->addr - addr >= size) continue;
        r->addr = 0, r->nrun = 0, r->inc++;
        for (int g = 0; g < RB_GENS; g++) r->seq[g] = UINT64_MAX;
    }
    pthread_mutex_unlock(&g_rb_lock);
}
// objects.c nvnMemoryPoolInvalidateMappedRange, on the game thread: the game is about to read [addr, addr + size) of a
// CPU-cached pool. Each kept target whose runs all lie inside gets them from its newest generation the GPU has finished
// (rendered at most RB_MAX_AGE presents ago); nothing outside the target's runs nor outside the range is written.
void frame_snap_note(uint64_t addr, uint64_t len);   // frame_snap.c
void vk_cpu_read_serve(uint64_t addr, uint64_t size) {
    if (!rb_on() || !size) return;
    const uint64_t done = vk_completed_seq(), now = renderer_present_count();
    pthread_mutex_lock(&g_rb_lock);
    for (int i = 0; i < RB_SLOTS; i++) {
        RbSlot *r = &g_rbs[i];
        if (!r->addr || !r->nrun || r->addr < addr || r->addr - addr >= size) continue;
        bool inside = true;
        for (uint32_t k = 0; k < r->nrun && inside; k++) inside = r->addr + r->run_off[k] + r->run_len[k] <= addr + size;
        int g = -1;
        for (int j = 0; j < RB_GENS; j++)
            if (r->seq[j] != UINT64_MAX && r->seq[j] <= done && (g < 0 || r->seq[j] > r->seq[g])) g = j;
        if (!inside || g < 0 || now > r->frame[g] + RB_MAX_AGE) continue;
        // (host allocations are HOST_COHERENT: shim_vma.c; the barrier in vk_cpu_read_flush made the copy host-visible)
        const uint8_t *src = r->map + (size_t)g * r->size;
        for (uint32_t k = 0; k < r->nrun; k++) {
            memcpy((void *)(uintptr_t)(r->addr + r->run_off[k]), src + r->run_off[k], r->run_len[k]);
            frame_snap_note(r->addr + r->run_off[k], r->run_len[k]);   // (frameguard: a guest write after a capture: live)
        }
        g_rb_served++;
    }
    pthread_mutex_unlock(&g_rb_lock);
}

TexBackend *vk_tex(NvnTexture *t, bool for_render) {
    if (for_render) rt_first_note(t, "render");
    MOW_NOTE(t, for_render ? MOW_RENDER : MOW_SAMPLE);
    if (tex_watched(t->addr)) {   // every render; each sampled object's first sample (was: the first 20 samples of the run)
        static NvnTexture *seen[4096];
        static int nseen;
        bool first = !for_render;
        for (int i = 0; first && i < nseen; i++) if (seen[i] == t) first = false;
        if (first && nseen < 4096) seen[nseen++] = t;
        if (for_render || first) tex_watch_note(t, tex_backend(t), for_render ? "render" : "sample");
    }
    if (!for_render) {   // sampled / copy source: another object's rendering at the same storage wins
        NvnTexture *w = alias_writer(t);
        if (w) return tex_backend(w);
        if (g_amiss) {   // (counted: alias misses above)
            const TexBackend *ob = t->backend;
            const int own = ob && ob->gpu_owned ? 0 : 1;
            st_amiss[g_amiss - 1][own]++;
            vk_txe_note(g_amiss == 1 ? (own ? TXE_AM_GONE_GUEST : TXE_AM_GONE_OWN) : (own ? TXE_AM_SHAPE_GUEST : TXE_AM_SHAPE_OWN), t);
            static NvnTexture *logged[512];   // (one line per sampled object: an open-addressed set, <= 8 probes, as alias_writer's)
            static int nlogged;
            bool fresh = false;
            const uint32_t hs = (uint32_t)((uintptr_t)t >> 4) * 0x9E3779B1u >> 23;
            for (int i = 0; i < 8 && nlogged < 256; i++) {
                NvnTexture **e = &logged[(hs + i) & 511];
                if (*e == t) break;
                if (!*e) { *e = t; nlogged++; fresh = true; break; }
            }
            if (fresh) {
                const NvnTexture *aw = g_amiss_w;
                LOG("[tex] alias miss: %p (%ux%u f%#x) at %llx sampled its %s: writer %p %s (%ux%u f%#x)\n", (void *)t, t->width, t->height,
                    t->format, (unsigned long long)t->addr, own ? "guest bytes" : "own image (rendered earlier)", (void *)aw,
                    g_amiss == 1 ? "gone / not rendered" : "of another shape", aw ? aw->width : 0, aw ? aw->height : 0, aw ? aw->format : 0);
            }
        }
    }
    TexBackend *b = tex_backend(t);
    if (b->pending_rgba) upload_pending(b);
    if (for_render) { b->gpu_owned = true; b->twin_valid = false; alias_note_writer(t); wb_note_render(t, b); }   // (twin: refreshed after the pass)
    else if (!b->gpu_owned) {
        extern uint64_t prof_up_ns;
        uint64_t t0 = prof_up_ns != ~0ull ? SDL_GetTicksNS() : 0;
        if (!b->uploaded && b->scalable) cpu1x_demote(t, b);   // (#62: a scaled shape filled from the CPU, at the game's size)
        upload_texture(t, b);
        if (prof_up_ns != ~0ull) prof_up_ns += SDL_GetTicksNS() - t0;
    }
    b->use_seq = vk_pending_seq();   // (after its upload: that one, if any, comes before this use; LARECOMP_UPLOAD_INLINE)
    b->use_draw = ~0ull;
    return b;
}

void renderer_mark_gpu_owned(NvnTexture *t) { TexBackend *b = tex_backend(t); b->gpu_owned = true; b->twin_valid = false; MOW_NOTE(t, MOW_COPY); }   // (copy / clear destinations)
bool renderer_is_gpu_owned(NvnTexture *t) { return tex_backend(t)->gpu_owned; }
// exec.c begin_pass, before it marks the target: a colour image with no contents of its own yet (new, or recycled from
// other storage: fresh_color) that nothing has uploaded or written (a copy / clear / host frame sets gpu_owned, a sample
// uploads). Its first pass then clears it instead of loading what the memory held. Single-shot: the pass takes the flag.
bool renderer_texture_unwritten(NvnTexture *t) {
    TexBackend *b = tex_backend(t);
    const bool u = b->fresh_color && !b->gpu_owned && !b->uploaded;
    b->fresh_color = false;
    return u;
}
void backend_pool_written(uint64_t addr, uint64_t size) { (void)addr; (void)size; }
// nvnTextureWriteTexels (objects.c): the next use re-hashes the whole texture (a small region can miss the sampled
// hash, and unchanged textures are only checked every 8th frame).
void backend_texture_cpu_written(NvnTexture *t) {
    if (t->backend) ((TexBackend *)t->backend)->cpu_dirty = true;
    __atomic_add_fetch(&ui_gen, 1, __ATOMIC_RELEASE);   // (LARECOMP_UPLOAD_INLINE: the submission's draws ask again)
}

// Host-produced texel data (decoded video, any thread): applied on the render thread at the texture's next use.
static _Atomic unsigned long long video_frames;   // (hle_vi.cpp: a video playing is not a load on screen)
unsigned long long renderer_video_frames(void) { return video_frames; }
void renderer_note_video_frame(void) { video_frames++; }   // (hle_movie.cpp: a decoded frame the game took)
void renderer_upload_rgba(NvnTexture *t, const void *rgba, uint32_t w, uint32_t h, uint32_t stride) {
    video_frames++;
    TexBackend *b = t->backend;
    if (!b) return;  // the game binds the texture before decoding into it; first frames may arrive earlier
    void *copy = malloc((size_t)stride * h);
    memcpy(copy, rgba, (size_t)stride * h);
    pthread_mutex_lock(&pending_lock);
    free(b->pending_rgba);
    b->pending_rgba = copy;
    b->pending_w = w; b->pending_h = h; b->pending_stride = stride;
    b->gpu_owned = true;
    pthread_mutex_unlock(&pending_lock);
    __atomic_add_fetch(&ui_gen, 1, __ATOMIC_RELEASE);   // (LARECOMP_UPLOAD_INLINE: the submission's draws ask again)
}

// ================================================================== views
#define VIEW_TAB 16384
typedef struct { TexBackend *b; uint64_t k1, k2; VkImageView view; } ViewEnt;
static ViewEnt view_tab[VIEW_TAB];

// Views may still be referenced by submitted or pending work: destroyed once that has completed.
void vk_views_forget(TexBackend *b) { views_forget(b); }
uint32_t vk_view_gen;  // bumped whenever a cached view may be destroyed (exec.c's per-frame texture memo)
static void views_forget(TexBackend *b) {
    contact_forget(b);   // (contact.c: its remembered G-buffer images; the image is being destroyed or replaced)
    vk_hue_forget(b);    // (exec.c: a pending HDR hue capture of it)
    vk_view_gen++;
    for (int i = 0; i < VIEW_TAB; i++)
        if (view_tab[i].b == b) { defer((Dead){.view = view_tab[i].view}); view_tab[i].b = NULL; view_tab[i].view = VK_NULL_HANDLE; }
}

static void views_forget_moved(void) {   // (residency park_move: the images moved this round, flag cleared)
    vk_view_gen++;
    for (int i = 0; i < VIEW_TAB; i++)
        if (view_tab[i].b && view_tab[i].b->park_moved) { defer((Dead){.view = view_tab[i].view}); view_tab[i].b = NULL; view_tab[i].view = VK_NULL_HANDLE; }
    for (int i = 0; i < g_nlive; i++) { TexBackend *b = g_live[i]->backend; if (b) b->park_moved = false; }
}
static void views_forget_evicted(void) {
    vk_view_gen++;
    for (int i = 0; i < VIEW_TAB; i++)
        if (view_tab[i].b && view_tab[i].b->evicted) { defer((Dead){.view = view_tab[i].view}); view_tab[i].b = NULL; view_tab[i].view = VK_NULL_HANDLE; }
}

VkImageView vk_image_view(TexBackend *b, VkImageViewType type, VkFormat fmt, uint32_t lv0, uint32_t nlv, uint32_t ly0,
                          uint32_t nly, VkComponentMapping sw, VkImageAspectFlags aspect) {
    uint64_t k1 = (uint64_t)type | (uint64_t)fmt << 8 | (uint64_t)lv0 << 40 | (uint64_t)nlv << 48 | (uint64_t)aspect << 56;
    uint64_t k2 = (uint64_t)ly0 | (uint64_t)nly << 16 | (uint64_t)sw.r << 32 | (uint64_t)sw.g << 40 | (uint64_t)sw.b << 48 | (uint64_t)sw.a << 56;
    uint64_t h = ((uint64_t)(uintptr_t)b * 0x9E3779B97F4A7C15ull) ^ (k1 * 0xC2B2AE3D27D4EB4Full) ^ (k2 * 0x165667B19E3779F9ull);
    uint32_t slot = (uint32_t)(h ^ (h >> 32)) & (VIEW_TAB - 1);
    for (int probe = 0; probe < 16; probe++, slot = (slot + 1) & (VIEW_TAB - 1)) {
        ViewEnt *e = &view_tab[slot];
        if (e->b == b && e->k1 == k1 && e->k2 == k2) return e->view;
        if (!e->b) break;
    }
    extern uint64_t prof_views_created;
    prof_views_created++;
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = b->image, .viewType = type,
                                .format = fmt, .components = sw, .subresourceRange = {aspect, lv0, nlv, ly0, nly}};
    // (an EXTENDED_USAGE image with storage, exec.c's snapshot copies: a view in a format without storage is sampled only)
    VkImageViewUsageCreateInfo vu = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO, .usage = b->usage & ~VK_IMAGE_USAGE_STORAGE_BIT};
    if ((b->cflags & VK_IMAGE_CREATE_EXTENDED_USAGE_BIT) && (b->usage & VK_IMAGE_USAGE_STORAGE_BIT) &&
        fmt != VK_FORMAT_R32_UINT && fmt != VK_FORMAT_R32G32_UINT && fmt != VK_FORMAT_R32G32B32A32_UINT)
        vi.pNext = &vu;
    VkImageView v;
    uint64_t v0 = __rdtsc();
    VkResult vr = vkCreateImageView(g_vk.dev, &vi, NULL, &v);
    cum_view_tk += __rdtsc() - v0; cum_view_n++;
    if (vr != VK_SUCCESS) {
        static int nfail;
        if (nfail++ < 20) LOG("[vk] view failed: type %d fmt %d (image fmt %d) levels %u+%u layers %u+%u\n", type, fmt, b->format, lv0, nlv, ly0, nly);
        return VK_NULL_HANDLE;
    }
    ViewEnt *e = &view_tab[slot];
    if (e->b) { defer((Dead){.view = e->view}); vk_view_gen++; }  // evict (16 probes taken)
    *e = (ViewEnt){b, k1, k2, v};
    return v;
}

static VkImageViewType view_type_for(int textype) {
    switch (textype) {
    case TEXTYPE_1D: return VK_IMAGE_VIEW_TYPE_1D;
    case TEXTYPE_3D: return VK_IMAGE_VIEW_TYPE_3D;
    case TEXTYPE_CUBE: return VK_IMAGE_VIEW_TYPE_CUBE;
    case TEXTYPE_2D_ARRAY: return VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    case TEXTYPE_CUBE_ARRAY: return VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
    case TEXTYPE_1D_ARRAY: return VK_IMAGE_VIEW_TYPE_1D_ARRAY;
    default: return VK_IMAGE_VIEW_TYPE_2D;
    }
}

static const VkComponentMapping IDENTITY = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                                            VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};

// LARECOMP_TEX_FIRST_MS=<ms> (default 0 = off, opt-in, the title freeze: at 4K the frame after the title's first one
// creates ~147 images and uploads 208 MB, ~80 of its 100 ms on the render thread): while the title is up (g_title_phase),
// a frame's first sightings of sampled textures (image creation + first upload) stop once they have taken this much
// render-thread time; a block-compressed texture first seen after that is sampled as the 1x1 zero dummy this frame and
// created on a later one. The frame's first sighting always runs, so every texture gets there. Compressed formats only
// (material textures: never depth, integer or render-target data; a texture another object renders into is not a first
// sighting here). Never in the world: TAA would keep the zero texels in its history for 8-16 frames, a dark flash on
// streamed objects; the title's burst is under its fade-in. Off by default since 2026-10-27: a deferred texture changes
// what a fade-in frame draws.
extern volatile int g_title_phase;
static uint64_t first_ns, first_frame, first_deferred_n, first_deferred_frames;
static uint32_t first_deferred;
static uint64_t first_budget_ns(void) {
    static int64_t v = -1;
    if (v < 0) {
        const char *e = getenv("LARECOMP_TEX_FIRST_MS");
        v = (int64_t)((e ? atof(e) : 0.0) * 1e6);
        if (v < 0) v = 0;
        LOG("[tex] first-sighting budget: %.1f ms of texture creation + first upload a frame%s (LARECOMP_TEX_FIRST_MS)\n", v / 1e6, v ? "" : ": off");
    }
    return (uint64_t)v;
}
static bool first_defer(NvnTexture *t) {
    uint64_t fr = vk_frame_counter();
    if (fr != first_frame) {
        if (first_deferred) {
            first_deferred_frames++;
            if (first_deferred_frames <= 100 || first_deferred_frames % 100 == 0)
                LOG("[tex] first sightings: frame %llu deferred %u textures (%.1f ms spent; %llu deferrals in %llu frames)\n",
                    (unsigned long long)first_frame, first_deferred, first_ns / 1e6, (unsigned long long)first_deferred_n,
                    (unsigned long long)first_deferred_frames);
        }
        first_frame = fr; first_ns = 0; first_deferred = 0;
    }
    if (first_ns < first_budget_ns()) return false;
    VkFormat f = vk_format(t->format);
    if (f < VK_FORMAT_BC1_RGB_UNORM_BLOCK || f > VK_FORMAT_ASTC_12x12_SRGB_BLOCK) return false;
    first_deferred++; first_deferred_n++;
    return true;
}

// render: attachment view (one level, one layer). Otherwise a sampled view of type want_textype (TEXTYPE_*),
// with the NVN component swizzle, converting between compatible types the way renderer.m does.
static _Thread_local TexBackend *g_view_on;   // (vk_view_on: the image to view instead of the texture's own)
// A 2D lookup on a 3D texture where the device has no VK_EXT_image_2d_view_of_3d (older AMD / Intel Windows drivers, some
// iGPUs; public #16, Link's warp dissolve): a 2D copy of slice 0, base level, in the 3D image's twin slot (depth twins
// skip 3D images) and refreshed whenever a write left it stale (twin_valid: uploads, renders, copies, clears and
// re-creation already clear it). The copy goes into the upload command buffer, which runs before the submission's
// draws: a CPU-uploaded pattern (the dissolve's) is current; a 3D image the GPU writes is one submission behind.
static VkImageView slice_twin_view(NvnTexture *t, TexBackend *b, VkFormat fmt, VkComponentMapping sw, VkImageAspectFlags aspect) {
    if (!b->image || b->samples != 1 || (b->aspect & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT))) return VK_NULL_HANDLE;
    TexBackend *d = b->twin;
    if (d && (d->width != b->width || d->height != b->height || d->format != b->format)) {   // (re-created at another shape)
        destroy_image(d);
        free(d);
        d = b->twin = NULL;
        b->twin_valid = false;
    }
    if (!d) {
        d = calloc(1, sizeof *d);
        VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .flags = b->cflags & VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT,
                                .imageType = VK_IMAGE_TYPE_2D, .format = b->format, .extent = {b->width, b->height, 1}, .mipLevels = 1,
                                .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                                .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        if (vk_image_create_relieved(&ci, &ai, &d->image, &d->mem, "slice twin vk fmt %d %ux%u", b->format, b->width, b->height) !=
            VK_SUCCESS) { free(d); return VK_NULL_HANDLE; }
        d->format = b->format; d->width = b->width; d->height = b->height; d->depth = 1; d->layers = 1; d->levels = 1;
        d->samples = 1; d->aspect = b->aspect; d->base_type = VK_IMAGE_VIEW_TYPE_2D; d->usage = ci.usage; d->cflags = ci.flags;
        d->gpu_owned = true;
        vk_image_init_layout(vk_upload_cmd(), d->image, d->aspect);
        b->twin = d;
        b->twin_valid = false;
        static int nlog;
        if (nlog++ < 16) LOG("[tex] 2D lookup on a 3D texture (%ux%ux%u fmt %#x): slice 0 through a 2D copy (no VK_EXT_image_2d_view_of_3d)\n",
                             t->width, t->height, t->depth, t->format);
    }
    if (!b->twin_valid) {
        VkCommandBuffer cb = vk_upload_cmd();
        vk_full_barrier(cb);   // (the upload's writes -> this read)
        VkImageCopy c = {.srcSubresource = {b->aspect, 0, 0, 1}, .dstSubresource = {d->aspect, 0, 0, 1}, .extent = {b->width, b->height, 1}};
        vkCmdCopyImage(cb, b->image, VK_IMAGE_LAYOUT_GENERAL, d->image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
        vk_full_barrier(cb);   // (-> the draws' samples)
        b->twin_valid = true;
    }
    return vk_image_view(d, VK_IMAGE_VIEW_TYPE_2D, fmt, 0, 1, 0, 1, sw, aspect);
}

VkImageView vk_view(NvnTexture *t, const NvnTextureView *v, bool has_view, bool render, int want_textype) {
    bool first = !g_view_on && !render && !t->backend && g_title_phase && first_budget_ns() && !alias_writer(t);
    if (first && first_defer(t)) return vk_dummy_view(want_textype);
    uint64_t f0 = first ? SDL_GetTicksNS() : 0;
    TexBackend *b = g_view_on ? g_view_on : vk_tex(t, render);
    if (first) first_ns += SDL_GetTicksNS() - f0;
    if (!has_view) v = NULL;
    VkFormat fmt = b->format;
    bool color = !(b->aspect & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT));
    if (b != t->backend && color && vk_format(t->format) != fmt &&
        format_supports(vk_format(t->format), VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT))
        fmt = vk_format(t->format);   // another object's image (alias, same texel size): read it in this object's format
    if (v && (v->setmask & 4) && color) {
        VkFormat vf = fmt_of(v->format).vf;
        if (vk_format(t->format) == fmt) fmt = vf;  // (fallback images keep their own format)
        else if (b->astc_bw && v->format >= 0x79 && v->format <= 0x94 && v->format != t->format) {
            // decoded ASTC (an RGBA8 image, or BC3 with compact textures; mutable): an sRGB / UNORM view of it keeps that
            // reading of the texels
            bool vs = v->format >= 0x87;
            if (vs != b->astc_srgb)
                fmt = b->astc_bc3 ? (vs ? VK_FORMAT_BC3_SRGB_BLOCK : VK_FORMAT_BC3_UNORM_BLOCK) : vs ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
            static uint32_t seen[32];
            static int nseen;
            uint32_t key = t->format << 16 | v->format, i = 0;
            while (i < (uint32_t)nseen && seen[i] != key) i++;
            if (i == (uint32_t)nseen && nseen < 32) {
                seen[nseen++] = key;
                LOG("[tex] decoded ASTC texture fmt %#x viewed as %#x (%s)\n", t->format, v->format,
                    vs != b->astc_srgb ? (vs ? "sRGB view of UNORM data" : "UNORM view of sRGB data") : "same colour space");
            }
        }
    }
    if (fmt != b->format && is_bc(fmt) && is_bc(b->format) && bc_class(fmt) != bc_class(b->format) && bc_class_views()) {
        // A BC view of a BC image of another kind (kak.cap: BC5 views of BC1 images and back): no Vulkan image can be
        // viewed across BC kinds (VUID-VkImageViewCreateInfo-image-01761; NVIDIA accepts it, AMD need not). The texels are
        // the image's own: they are read in its format. LARECOMP_BC_CLASS_VIEW=0: the view's format (old).
        static uint32_t seen[32];
        static int nseen;
        uint32_t key = (uint32_t)fmt << 16 | (uint32_t)b->format, i = 0;
        while (i < (uint32_t)nseen && seen[i] != key) i++;
        if (i == (uint32_t)nseen && nseen < 32) {
            seen[nseen++] = key;
            LOG("[tex] a view in BC format %d of a BC image in format %d (another BC kind, nvn fmt %#x): read in the image's format\n",
                fmt, b->format, t->format);
        }
        fmt = b->format;
    }
    if (color && fmt >= VK_FORMAT_D16_UNORM && fmt <= VK_FORMAT_D32_SFLOAT_S8_UINT)   // (a depth reading of a colour image: a depth-format
        fmt = dac_colour(fmt) ? dac_colour(fmt) : b->format;                         // target made colour, renderer_texture_target_kind)
    uint32_t lv0 = v && (v->setmask & 1) ? v->base_level : 0;
    uint32_t nlv = v && (v->setmask & 1) && v->num_levels ? v->num_levels : b->levels - (lv0 < b->levels ? lv0 : 0);
    uint32_t ly0 = v && (v->setmask & 2) ? v->min_layer : 0;
    uint32_t nly = v && (v->setmask & 2) && v->num_layers ? v->num_layers : b->layers - (ly0 < b->layers ? ly0 : 0);
    if (lv0 >= b->levels) lv0 = b->levels - 1;
    if (lv0 + nlv > b->levels) nlv = b->levels - lv0;
    if (ly0 >= b->layers) ly0 = b->layers - 1;
    if (ly0 + nly > b->layers) nly = b->layers - ly0;
    if (render) {
        // An image created without attachment usage (block-compressed formats: BC4 64^3 volumes the game fills by
        // rendering encoded blocks through an uncompressed view) must never be rendered to: such a view is invalid and the
        // GPU wrote past the image. Dropped, logged per texture.
        VkImageUsageFlags att = color ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT : VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        if (color && is_bc(b->format) && v && (v->setmask & 4)) {  // encoded blocks through an uncompressed view
            TexBackend *s = block_shadow(t, b, fmt);
            if (s) b = s;   // (same levels / slices / layers; extents in blocks)
            if (s && lv0 >= s->levels) return VK_NULL_HANDLE;  // (2D: the shadow has fewer levels than the BC chain)
        }
        bool slice3d = b->base_type == VK_IMAGE_VIEW_TYPE_3D;
        if (!(b->usage & att) || (slice3d && !(b->cflags & VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT))) {
            static uint64_t seen[64];
            static int nseen;
            bool fresh = true;
            for (int i = 0; i < nseen && fresh; i++) fresh = seen[i] != t->addr;
            if (fresh && nseen < 64) {
                seen[nseen++] = t->addr;
                LOG("[vk] render target not renderable, dropped: %ux%ux%u nvn fmt %#x (view fmt %#x) vk fmt %d usage %#x flags %#x addr %llx\n",
                    t->width, t->height, t->depth, t->format, v && (v->setmask & 4) ? v->format : 0, b->format, b->usage, b->cflags,
                    (unsigned long long)t->addr);
            }
            return VK_NULL_HANDLE;
        }
        VkImageViewType type = b->base_type == VK_IMAGE_VIEW_TYPE_3D ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
        if (fmt_of(v && (v->setmask & 4) ? v->format : t->format).perm) {
            // an attachment has no swizzle: rendered texels land in the view format's own channel order, which the
            // sampled views (perm) then read permuted. Not seen in TotK (the RGB5A1 / RGBA4 images are CPU-written)
            static int once;
            if (!once++) LOG("[vk] render target in a permuted 16-bit format (nvn fmt %#x, LARECOMP_FMT16): sampled back with "
                             "red and blue swapped\n", t->format);
        }
        if (b->base_type == VK_IMAGE_VIEW_TYPE_3D) {
            // 3D render target. One depth slice of one level through a 2D view (the image is created
            // 2D_ARRAY_COMPATIBLE); the slice is the NVN view's min layer (the clamp above zeroes it: b->layers is 1).
            // Layered rendering into all slices at once (VS-written layer) is not supported: slice 0 only, logged.
            // LARECOMP_NO_3D_RT=1: not rendered (the old behaviour).
            uint32_t slices = t->depth >> lv0 ? t->depth >> lv0 : 1;
            uint32_t z = v && (v->setmask & 2) ? v->min_layer : 0;
            uint32_t nz = v && (v->setmask & 2) && v->num_layers ? v->num_layers : slices;
            static int nseen, off = -1;
            static uint64_t seen[512];
            if (off < 0) off = getenv("LARECOMP_NO_3D_RT") != NULL;
            uint64_t key = t->addr ^ ((uint64_t)lv0 << 56) ^ ((uint64_t)z << 44);
            bool fresh = true;  // one line per distinct volume / level / slice
            for (int i = 0; i < nseen && fresh; i++) fresh = seen[i] != key;
            if (fresh && nseen < 512) seen[nseen++] = key;
            if (fresh && nseen < 512)
                LOG("[vk] 3D render target %ux%ux%u fmt %#x addr %llx level %u slice %u (+%u of %u)%s\n", t->width, t->height, t->depth,
                    t->format, (unsigned long long)t->addr, lv0, z, nz, slices, off ? ": NOT rendered (LARECOMP_NO_3D_RT)" : "");
            if (off || z >= slices) return VK_NULL_HANDLE;
            return vk_image_view(b, VK_IMAGE_VIEW_TYPE_2D, fmt, lv0, 1, z, 1, IDENTITY, b->aspect);
        }
        return vk_image_view(b, type, fmt, lv0, 1, ly0, 1, IDENTITY, b->aspect);
    }
    VkImageAspectFlags aspect = b->aspect & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : b->aspect;
    VkComponentMapping sw = IDENTITY;
    if (!(b->aspect & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) && b->samples == 1) {
        const uint8_t *s = v && (v->setmask & 8) ? v->swizzle : t->swizzle;
        uint8_t c[4];
        for (int i = 0; i < 4; i++) c[i] = s[i] <= 5 ? s[i] : (uint8_t)(2 + i);
        uint8_t perm = fmt_of(v && (v->setmask & 4) ? v->format : t->format).perm;
        if (perm) {  // the format's channels sit elsewhere in the view (LARECOMP_FMT16): r, g, b, a read from there
            static const uint8_t ph[3][4] = {{0, 1, 2, 3}, {2, 1, 0, 3}, {3, 2, 1, 0}};
            for (int i = 0; i < 4; i++) {
                if (!(s[0] || s[1] || s[2] || s[3])) c[i] = (uint8_t)(2 + i);
                if (c[i] >= 2) c[i] = (uint8_t)(2 + ph[perm][c[i] - 2]);
            }
        }
        if (c[0] || c[1] || c[2] || c[3]) {  // never set: identity
            if (fmt_of(t->format).alpha_one)
                for (int i = 0; i < 4; i++) if (c[i] == 5) c[i] = 1;
            // NVN swizzle 0 zero, 1 one, 2..5 r,g,b,a == VkComponentSwizzle - 1
            sw = (VkComponentMapping){(VkComponentSwizzle)(c[0] + 1), (VkComponentSwizzle)(c[1] + 1), (VkComponentSwizzle)(c[2] + 1),
                                      (VkComponentSwizzle)(c[3] + 1)};
        } else if (fmt_of(t->format).alpha_one) {
            sw.a = VK_COMPONENT_SWIZZLE_ONE;
        }
    }
    VkImageViewType want = view_type_for(want_textype);
    VkImageViewType have = b->base_type;
    if (b->samples > 1) return vk_dummy_view(want_textype);  // multisampled images can't be sampled as 2D
    if (want == have) {
        if (have == VK_IMAGE_VIEW_TYPE_CUBE) nly = 6;
        return vk_image_view(b, want, fmt, lv0, nlv, ly0, nly, sw, aspect);
    }
    if (want == VK_IMAGE_VIEW_TYPE_2D && (have == VK_IMAGE_VIEW_TYPE_2D_ARRAY || have == VK_IMAGE_VIEW_TYPE_CUBE))
        return vk_image_view(b, want, fmt, lv0, nlv, ly0, 1, sw, aspect);
    if (want == VK_IMAGE_VIEW_TYPE_2D_ARRAY && (have == VK_IMAGE_VIEW_TYPE_2D || have == VK_IMAGE_VIEW_TYPE_CUBE))
        return vk_image_view(b, want, fmt, lv0, nlv, ly0, have == VK_IMAGE_VIEW_TYPE_CUBE ? nly : 1, sw, aspect);
    if (want == VK_IMAGE_VIEW_TYPE_CUBE && have == VK_IMAGE_VIEW_TYPE_2D_ARRAY && b->layers >= 6 && b->width == b->height) {
        if (!(b->cflags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT)) {   // (cube_compat_all off: no flag on non-cube arrays)
            static int nlog;
            if (nlog++ < 16) LOG("[tex] cube view of a 2D array without CUBE_COMPATIBLE (%ux%u x%u fmt %#x addr %llx): dummy view "
                                 "(LARECOMP_CUBE_COMPAT=all keeps the flag)\n", t->width, t->height, b->layers, t->format, (unsigned long long)t->addr);
            return vk_dummy_view(want_textype);
        }
        return vk_image_view(b, want, fmt, lv0, nlv, ly0 + 6 <= b->layers ? ly0 : 0, 6, sw, aspect);
    }
    if (want == VK_IMAGE_VIEW_TYPE_1D_ARRAY && have == VK_IMAGE_VIEW_TYPE_1D)
        return vk_image_view(b, want, fmt, lv0, nlv, 0, 1, sw, aspect);
    // A 2D lookup on a 3D texture (public #16: Link's warp dissolve discards against an 8x8x8 pattern sampled so; the
    // dummy view made him vanish at once): Maxwell's 2D instruction leaves the third coordinate 0 = slice 0. A 2D view of
    // a 3D image holds one level (the base level of the binding).
    if (want == VK_IMAGE_VIEW_TYPE_2D && have == VK_IMAGE_VIEW_TYPE_3D && (b->cflags & VK_IMAGE_CREATE_2D_VIEW_COMPATIBLE_BIT_EXT)) {
        static int said;
        if (!said++) LOG("[tex] 2D lookup on a 3D texture (%ux%ux%u fmt %#x): slice 0 through a 2D view\n", t->width, t->height, t->depth, t->format);
        return vk_image_view(b, want, fmt, lv0, 1, 0, 1, sw, aspect);
    }
    static int slice_copy = -1;   // (LARECOMP_NO_2D_OF_3D=1: neither form, the dummy view as before)
    if (slice_copy < 0) slice_copy = !getenv("LARECOMP_NO_2D_OF_3D");
    if (want == VK_IMAGE_VIEW_TYPE_2D && have == VK_IMAGE_VIEW_TYPE_3D && lv0 == 0 && slice_copy) {
        VkImageView sv = slice_twin_view(t, b, fmt, sw, aspect);
        if (sv) return sv;
    }
    static int nlog;
    if (nlog++ < 30) LOG("[tex] type mismatch: want %d have %d (%ux%ux%u target %u fmt %#x layers %u)\n", want, have, t->width,
                         t->height, t->depth, t->target, t->format, b->layers);
    return vk_dummy_view(want_textype);
}

// The dummies' views live outside view_tab: vk_image_view's 16-probe eviction could destroy a cached one there, and the
// dummies' statics would then hand out a dead view.
static VkImageView dummy_view_of(VkImage img, VkImageViewType vt, VkFormat fmt, VkImageAspectFlags aspect, uint32_t layers) {
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = img, .viewType = vt, .format = fmt,
                                .components = IDENTITY, .subresourceRange = {aspect, 0, 1, 0, layers}};
    VkImageView v;
    VK_CHECK(vkCreateImageView(g_vk.dev, &vi, NULL, &v));
    return v;
}
// 1x1 D32 at 0.0 for a depth-compare binding that holds no image a compare can read (exec.c LARECOMP_DREF_DUMMY): 0, as the
// red of the colour dummy those slots had; 2D / 2D array / cube (depth images have no 1D / 3D), NULL for the others
VkImageView vk_dummy_depth_view(int textype) {
    static TexBackend dummies[10];
    static VkImageView views[10];
    if (textype < 0 || textype >= 10) textype = TEXTYPE_2D;
    if (views[textype]) return views[textype];
    VkImageViewType vt = view_type_for(textype);
    if (vt != VK_IMAGE_VIEW_TYPE_2D && vt != VK_IMAGE_VIEW_TYPE_2D_ARRAY && vt != VK_IMAGE_VIEW_TYPE_CUBE && vt != VK_IMAGE_VIEW_TYPE_CUBE_ARRAY)
        return VK_NULL_HANDLE;
    TexBackend *b = &dummies[textype];
    bool cube = vt == VK_IMAGE_VIEW_TYPE_CUBE || vt == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .flags = cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0,
                            .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_D32_SFLOAT, .extent = {1, 1, 1}, .mipLevels = 1,
                            .arrayLayers = cube ? 6 : 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    VK_CHECK(vk_image_create_relieved(&ci, &ai, &b->image, &b->mem, "1x1 depth dummy"));
    VkCommandBuffer cb = vk_upload_cmd();
    vk_image_init_layout(cb, b->image, VK_IMAGE_ASPECT_DEPTH_BIT);
    VkClearDepthStencilValue zero = {0.0f, 0};
    VkImageSubresourceRange all = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, cube ? 6 : 1};
    vkCmdClearDepthStencilImage(cb, b->image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &all);
    b->format = VK_FORMAT_D32_SFLOAT;
    b->aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
    b->levels = 1;
    b->layers = cube ? 6 : 1;
    views[textype] = dummy_view_of(b->image, vt, b->format, VK_IMAGE_ASPECT_DEPTH_BIT, cube ? 6 : 1);
    return views[textype];
}

// 1x1 black textures for unbound slots, per type
VkImageView vk_dummy_view(int textype) {
    static TexBackend dummies[10];
    static VkImageView views[10];
    if (textype < 0 || textype >= 10) textype = TEXTYPE_2D;
    if (views[textype]) return views[textype];
    TexBackend *b = &dummies[textype];
    VkImageViewType vt = view_type_for(textype);
    bool cube = vt == VK_IMAGE_VIEW_TYPE_CUBE || vt == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .flags = cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0,
                            .imageType = vt == VK_IMAGE_VIEW_TYPE_3D ? VK_IMAGE_TYPE_3D
                                       : vt == VK_IMAGE_VIEW_TYPE_1D || vt == VK_IMAGE_VIEW_TYPE_1D_ARRAY ? VK_IMAGE_TYPE_1D : VK_IMAGE_TYPE_2D,
                            .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = {1, 1, 1}, .mipLevels = 1, .arrayLayers = cube ? 6 : 1,
                            .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    VK_CHECK(vk_image_create_relieved(&ci, &ai, &b->image, &b->mem, "1x1 colour dummy"));
    VkCommandBuffer cb = vk_upload_cmd();
    vk_image_init_layout(cb, b->image, VK_IMAGE_ASPECT_COLOR_BIT);
    VkClearColorValue zero = {{0, 0, 0, 0}};
    VkImageSubresourceRange all = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, cube ? 6 : 1};
    vkCmdClearColorImage(cb, b->image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &all);
    b->format = VK_FORMAT_R8G8B8A8_UNORM;
    b->levels = 1;
    b->layers = cube ? 6 : 1;
    views[textype] = dummy_view_of(b->image, vt, b->format, VK_IMAGE_ASPECT_COLOR_BIT, cube ? 6 : 1);
    return views[textype];
}

// ================================================================== samplers
static VkCompareOp compare_op(uint32_t f) {
    switch (f) {
    case 1: return VK_COMPARE_OP_NEVER;
    case 2: return VK_COMPARE_OP_LESS;
    case 3: return VK_COMPARE_OP_EQUAL;
    case 4: return VK_COMPARE_OP_LESS_OR_EQUAL;
    case 5: return VK_COMPARE_OP_GREATER;
    case 6: return VK_COMPARE_OP_NOT_EQUAL;
    case 7: return VK_COMPARE_OP_GREATER_OR_EQUAL;
    default: return VK_COMPARE_OP_ALWAYS;
    }
}

// ---- Parallel ASTC decode ---- Uploads run on the render thread at a texture's first sample, and ASTC is decoded on
// the CPU into RGBA8 (the GPU has no ASTC): the 100% save's first world frames decoded ~860 MB of RGBA8 in one 2 s
// frame (boot gtseed_on, frame 99). A decode of more than ASTC_PAR_MIN blocks is split into chunks of block rows that
// the workers and the render thread share; the render thread returns when every chunk is done and no worker still
// holds the job (the job is static and reused). astc_decode_block keeps no state between calls, so blocks decode
// independently.
#define ASTC_PAR_MIN 4096
#define ASTC_CHUNK_ROWS 4
// src set: each chunk reads its block rows from the guest storage itself (linear rows at `stride`, or block-linear,
// deswizzled per chunk), so neither a whole-level copy nor its deswizzle pass runs on the render thread. kind: 0 the
// ASTC decode above; 1 block rows of a depth-1 level deswizzled into `out` (`rows` per chunk, `bpb` bytes per block);
// 2 hash_tex's 1 MB chunks of `src` (`hn` bytes) into hslot[chunk]. Workers only read guest memory and write `out`
// ranges the render thread allocated before the job, or their own hslot: no ring head / tail, no flush, no b->* field,
// no command recording.
typedef struct { const uint8_t *blocks; uint8_t *out; uint32_t w, h, bw, bh, by, nchunk; bool srgb;
                 const uint8_t *src; bool linear; uint64_t stride; uint32_t bx, bh_log2, bd_log2;
                 uint8_t kind; uint32_t bpb, rows; uint64_t *hslot, hn; } AstcJob;
#define TEXH_CHUNK_LOG2 20
static bool tex_par_check(void);
static void tex_par_check_note(const char *what, bool same);
static __thread uint8_t *aq_tl; static __thread size_t aq_tl_cap;   // (a chunk's block rows)
static AstcJob aq_job;
static _Atomic uint32_t aq_next, aq_done;
static uint64_t aq_gen;
static int aq_active, aq_n = -1;
static pthread_mutex_t aq_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t aq_cv = PTHREAD_COND_INITIALIZER, aq_done_cv = PTHREAD_COND_INITIALIZER;
static void astc_run_chunks(const AstcJob *j) {
    for (;;) {
        uint32_t c = atomic_fetch_add(&aq_next, 1);
        if (c >= j->nchunk) return;
        if (j->kind == 1) {
            uint32_t y0 = c * j->rows;
            tegra_deswizzle_rows(j->src, j->out + (uint64_t)y0 * j->bx * j->bpb, j->bx, j->by, j->bpb, j->bh_log2, j->bd_log2, y0, y0 + j->rows);
            atomic_fetch_add(&aq_done, 1);
            continue;
        }
        if (j->kind == 2) {
            uint64_t o = (uint64_t)c << TEXH_CHUNK_LOG2, n = j->hn - o < (1ull << TEXH_CHUNK_LOG2) ? j->hn - o : 1ull << TEXH_CHUNK_LOG2;
            j->hslot[c] = hash_full(j->src + o, n);
            atomic_fetch_add(&aq_done, 1);
            continue;
        }
        uint32_t j0 = c * ASTC_CHUNK_ROWS, j1 = j0 + ASTC_CHUNK_ROWS < j->by ? j0 + ASTC_CHUNK_ROWS : j->by;
        if (j->src) {
            size_t need = (size_t)(j1 - j0) * j->bx * 16;
            if (need > aq_tl_cap) { free(aq_tl); aq_tl = malloc(need); aq_tl_cap = need; }
            if (j->linear)
                for (uint32_t y = j0; y < j1; y++) memcpy(aq_tl + (size_t)(y - j0) * j->bx * 16, j->src + y * j->stride, (size_t)j->bx * 16);
            else tegra_deswizzle_rows(j->src, aq_tl, j->bx, j->by, 16, j->bh_log2, j->bd_log2, j0, j1);
            // (astc_decode_ldr_rows indexes blocks from row 0: a base whose row j0 is the chunk's first row)
            astc_decode_ldr_rows(aq_tl - (ptrdiff_t)j0 * j->bx * 16, j->w, j->h, j->bw, j->bh, j->srgb, j->out, j0, j1);
        } else astc_decode_ldr_rows(j->blocks, j->w, j->h, j->bw, j->bh, j->srgb, j->out, j0, j1);
        atomic_fetch_add(&aq_done, 1);
    }
}
#ifdef _WIN32
__declspec(dllimport) void *__stdcall GetCurrentThread(void);
__declspec(dllimport) int __stdcall SetThreadPriority(void *thread, int priority);
#endif
static void *astc_worker(void *arg) {
    (void)arg;
    cpu_place_thread(CPU_ANY);   // (the render thread joins these: every core, cpu_place.h)
#ifdef _WIN32
    // the render thread's priority (exec.c async_main: THREAD_PRIORITY_ABOVE_NORMAL), which waits on these: under a busy
    // host the game's threads would otherwise preempt the workers it joins
    SetThreadPriority(GetCurrentThread(), 1);
#endif
    uint64_t seen = 0;
    for (;;) {
        pthread_mutex_lock(&aq_mu);
        while (aq_gen == seen) pthread_cond_wait(&aq_cv, &aq_mu);
        seen = aq_gen;
        AstcJob j = aq_job;
        aq_active++;
        pthread_mutex_unlock(&aq_mu);
        astc_run_chunks(&j);
        pthread_mutex_lock(&aq_mu);
        aq_active--;
        pthread_cond_broadcast(&aq_done_cv);
        pthread_mutex_unlock(&aq_mu);
    }
    return NULL;
}
// LARECOMP_TEX_THREADS=<n> | auto (LARECOMP_ASTC_THREADS is the old name): the pool's workers. auto = logical cores / 4,
// 2..4 (a Deck: 2); 0 = off (every upload's CPU work on the render thread, as before). Default auto on Windows; off
// elsewhere until a Deck A/B decides it. Its own threads, never the shader workers.
static bool astc_par_on(void) {   // (starts the workers once)
    if (aq_n < 0) {
        const char *e = getenv("LARECOMP_TEX_THREADS");
        if (!e) e = getenv("LARECOMP_ASTC_THREADS");
#ifndef _WIN32
        if (!e) e = "0";
#endif
        if (!e || !strcmp(e, "auto")) {
            aq_n = SDL_GetNumLogicalCPUCores() / 4;
            aq_n = aq_n < 2 ? 2 : aq_n > 4 ? 4 : aq_n;
        } else aq_n = atoi(e);
        if (aq_n > 32) aq_n = 32;
        for (int i = 0; i < aq_n; i++) { pthread_t t; if (pthread_create(&t, NULL, astc_worker, NULL)) { aq_n = i; break; } }
        if (aq_n > 0) LOG("[tex] upload hash / deswizzle / ASTC decode on %d worker threads + the render thread (LARECOMP_TEX_THREADS)\n", aq_n);
        tex_par_check();   // (LARECOMP_TEX_THREADS_CHECK: the hash_tex self-test, before any upload uses the pool)
    }
    return aq_n > 0;
}
static void astc_job_run(AstcJob j) {   // (publish, take a share, wait for the rest)
    pthread_mutex_lock(&aq_mu);
    while (aq_active) pthread_cond_wait(&aq_done_cv, &aq_mu);   // (a worker that woke late may still hold the last job)
    aq_job = j;
    atomic_store(&aq_next, 0); atomic_store(&aq_done, 0);
    aq_gen++;
    pthread_cond_broadcast(&aq_cv);
    pthread_mutex_unlock(&aq_mu);
    astc_run_chunks(&aq_job);
    pthread_mutex_lock(&aq_mu);
    while (atomic_load(&aq_done) < aq_job.nchunk || aq_active) pthread_cond_wait(&aq_done_cv, &aq_mu);
    pthread_mutex_unlock(&aq_mu);
}
// A whole level straight from the guest storage (no level copy, no separate deswizzle pass); false when off or small.
static bool astc_decode_par_src(const uint8_t *src, bool linear, uint64_t stride, uint32_t wb, uint32_t hb, uint32_t bh_log2,
                                uint32_t bd_log2, uint32_t w, uint32_t h, uint32_t bw, uint32_t bh, bool srgb, uint8_t *out) {
    if (!astc_par_on() || (uint64_t)wb * hb < ASTC_PAR_MIN) return false;
    astc_job_run((AstcJob){NULL, out, w, h, bw, bh, hb, (hb + ASTC_CHUNK_ROWS - 1) / ASTC_CHUNK_ROWS, srgb, src, linear, stride, wb,
                           bh_log2, bd_log2});
    if (tex_par_check()) {   // (LARECOMP_TEX_THREADS_CHECK: the same level inline, as the fallback below does it)
        uint8_t *blocks = malloc((uint64_t)wb * hb * 16), *ref = malloc((uint64_t)w * h * 4);
        if (blocks && ref) {   // (no memory: that comparison is skipped)
            if (linear) for (uint32_t y = 0; y < hb; y++) memcpy(blocks + (uint64_t)y * wb * 16, src + (uint64_t)y * stride, (size_t)wb * 16);
            else tegra_deswizzle(src, blocks, wb, hb, 1, 16, bh_log2, bd_log2);
            astc_decode_ldr(blocks, w, h, bw, bh, srgb, ref);
            tex_par_check_note("ASTC decode (from guest storage)", !memcmp(ref, out, (size_t)w * h * 4));
        }
        free(blocks); free(ref);
    }
    return true;
}
static void astc_decode_par(const uint8_t *blocks, uint32_t w, uint32_t h, uint32_t bw, uint32_t bh, bool srgb, uint8_t *out) {
    uint32_t bx = (w + bw - 1) / bw, by = (h + bh - 1) / bh;
    if (!astc_par_on() || (uint64_t)bx * by < ASTC_PAR_MIN) { astc_decode_ldr(blocks, w, h, bw, bh, srgb, out); return; }
    astc_job_run((AstcJob){blocks, out, w, h, bw, bh, by, (by + ASTC_CHUNK_ROWS - 1) / ASTC_CHUNK_ROWS, srgb});
    if (tex_par_check()) {
        uint8_t *ref = malloc((uint64_t)w * h * 4);
        if (ref) {   // (no memory: that comparison is skipped)
            astc_decode_ldr(blocks, w, h, bw, bh, srgb, ref);
            tex_par_check_note("ASTC decode", !memcmp(ref, out, (size_t)w * h * 4));
        }
        free(ref);
    }
}

// LARECOMP_TEX_THREADS_CHECK=1 (verification): every parallel deswizzle / hash is done again inline and compared;
// "[tex] TEX_THREADS check" counts mismatches.
static bool tex_par_check(void) {
    static int v = -1;
    if (v < 0) {
        v = getenv("LARECOMP_TEX_THREADS_CHECK") && strcmp(getenv("LARECOMP_TEX_THREADS_CHECK"), "0");
        extern bool tex_hash_selftest(void);
        if (v) tex_hash_selftest();
    }
    return v;
}
static void tex_par_check_note(const char *what, bool same) {   // (what: a string literal, one per job kind)
    static struct { const char *what; uint64_t n, bad; } k[8];
    static uint64_t n, bad;
    int i = 0;
    while (i < 7 && k[i].what && k[i].what != what) i++;
    k[i].what = what; k[i].n++; n++;
    if (!same) { k[i].bad++; bad++; }
    if (!same && bad <= 20) LOG("[tex] TEX_THREADS check: %s differs from the inline result (%llu of %llu)\n", what, (unsigned long long)bad, (unsigned long long)n);
    if (n <= 3 || n % 25 == 0 || (!same && bad <= 20)) {   // (up3_check: 500 never printed in a 160 s boot)
        LOG("[tex] TEX_THREADS check: %llu compared, %llu differ;", (unsigned long long)n, (unsigned long long)bad);
        for (int j = 0; j < 8 && k[j].what; j++) LOG(" %s %llu/%llu", k[j].what, (unsigned long long)k[j].bad, (unsigned long long)k[j].n);
        LOG("\n");
    }
}

// Plain levels of >= 1 MB: deswizzle time per level by size and path ("[tex] deswizzle per level" every 300 such
// levels).
static void desw_note(uint64_t bytes, bool split, uint64_t ticks) {
    static uint64_t n[5][2], tk[5][2], total;
    extern double up_ns_per_tick(void);
    int b = bytes < (2ull << 20) ? 0 : bytes < (4ull << 20) ? 1 : bytes < (8ull << 20) ? 2 : bytes < (16ull << 20) ? 3 : 4;
    n[b][split]++; tk[b][split] += ticks;
    if (++total % 300) return;
    static const char *name[5] = {"1-2 MB", "2-4 MB", "4-8 MB", "8-16 MB", "16+ MB"};
    double r = up_ns_per_tick() / 1e6;
    LOG("[tex] deswizzle per level (ms, inline / split, levels):");
    for (int i = 0; i < 5; i++)
        if (n[i][0] || n[i][1])
            LOG(" %s %.2f / %.2f (%llu / %llu)", name[i], n[i][0] ? tk[i][0] * r / n[i][0] : 0.0, n[i][1] ? tk[i][1] * r / n[i][1] : 0.0,
                (unsigned long long)n[i][0], (unsigned long long)n[i][1]);
    LOG("\n");
}

// A plain depth-1 level of >= 2 MB, block rows split into ~1 MB chunks over the pool; false when the pool is off or the
// level small. dst: ring memory (or the RGB8 scratch) the render thread allocated.
#define TEXD_PAR_MIN (2ull << 20)
static bool deswizzle_par(const uint8_t *src, uint8_t *dst, uint32_t wb, uint32_t hb, uint32_t bpb, uint32_t bh_log2, uint32_t bd_log2) {
    uint64_t row = (uint64_t)wb * bpb;
    if (row * hb < TEXD_PAR_MIN || !astc_par_on()) return false;
    uint32_t rows = (uint32_t)((1ull << 20) / row);
    if (!rows) rows = 1;
    astc_job_run((AstcJob){.out = dst, .by = hb, .nchunk = (hb + rows - 1) / rows, .src = src, .bx = wb, .bh_log2 = bh_log2,
                           .bd_log2 = bd_log2, .kind = 1, .bpb = bpb, .rows = rows});
    uint8_t *ref = tex_par_check() ? malloc(row * hb) : NULL;   // (no memory: that comparison is skipped)
    if (ref) {
        tegra_deswizzle(src, ref, wb, hb, 1, bpb, bh_log2, bd_log2);
        tex_par_check_note("deswizzle", !memcmp(ref, dst, row * hb));
        free(ref);
    }
    return true;
}

// data_hash: hash_full up to 1 MB (as before); above, hash_full of each 1 MB chunk combined in order, the same value
// whether the chunks ran on the pool (from 4 MB) or inline. Only ever compared with data_hash values made here.
#define TEXH_PAR_MIN (4ull << 20)
#define TEXH_UNSET 0x5EED5EED5EED5EEDull
// LARECOMP_TEX_THREADS_CHECK for a pooled hash: each chunk hashed again inline. chk1_depths: 1 of 323 differed. Guest
// memory is live (a re-uploaded texture is one the guest writes), so a chunk that differs is classified before it counts:
// a slot the join left unwritten (TEXH_UNSET) is a pool fault; a chunk whose inline hash moves between two passes is the
// source changing under the check; one that stays put is hashed again on the pool, and only a pool result that still
// differs from the inline one counts as a pool fault.
static void hash_par_check(const uint8_t *p, uint64_t n, uint32_t nc, const uint64_t *hs) {
    const uint64_t ch = 1ull << TEXH_CHUNK_LOG2;
    bool same = true;
    uint32_t moved = 0;
    for (uint32_t c = 0; c < nc; c++) {
        uint64_t o = (uint64_t)c << TEXH_CHUNK_LOG2, len = n - o < ch ? n - o : ch, i1 = hash_full(p + o, len);
        if (hs[c] == i1) continue;
        uint64_t i2 = hash_full(p + o, len), again = TEXH_UNSET;
        const char *why;
        if (hs[c] == TEXH_UNSET) { why = "slot never written (pool fault)"; same = false; }
        else if (i1 != i2) { why = "source moved between inline passes"; moved++; }
        else {   // (the whole job again, so the chunk runs with the same offsets and on whichever thread takes it)
            uint64_t *h2 = malloc((size_t)nc * 8);
            if (h2) {
                for (uint32_t k = 0; k < nc; k++) h2[k] = TEXH_UNSET;
                astc_job_run((AstcJob){.src = p, .nchunk = nc, .kind = 2, .hslot = h2, .hn = n});
                again = h2[c];
                free(h2);
            }
            if (again == i1) { why = "pool agrees on the settled bytes: source moved"; moved++; }
            else { why = "pool differs on settled bytes (pool fault)"; same = false; }
        }
        LOG("[tex] TEX_THREADS check: hash chunk %u/%u of %llu bytes at %p: pool %016llx inline %016llx / %016llx, pool again %016llx: %s\n",
            c, nc, (unsigned long long)n, (const void *)p, (unsigned long long)hs[c], (unsigned long long)i1, (unsigned long long)i2,
            (unsigned long long)again, why);
    }
    tex_par_check_note("hash", same);
    if (moved) tex_par_check_note("hash, source moved (not counted)", true);
}
static uint64_t hash_tex_(const uint8_t *p, uint64_t n, int mode) {   // (mode: -1 by size, 0 inline, 1 pool if on)
    const uint64_t ch = 1ull << TEXH_CHUNK_LOG2;
    if (n <= ch) return hash_full(p, n);
    uint32_t nc = (uint32_t)((n + ch - 1) >> TEXH_CHUNK_LOG2);
    uint64_t stack[64], *hs = nc <= 64 ? stack : malloc((size_t)nc * 8);
    if (!hs) {   // (no memory for the slots: the same combine, inline and running)
        uint64_t h = 0x9E3779B97F4A7C15ull ^ n;
        for (uint32_t c = 0; c < nc; c++) {
            uint64_t o = (uint64_t)c << TEXH_CHUNK_LOG2;
            h = (h ^ hash_full(p + o, n - o < ch ? n - o : ch)) * 0x100000001B3ull; h ^= h >> 29;
        }
        return h ^ (h >> 31);
    }
    bool par = mode != 0 && (mode == 1 || n >= TEXH_PAR_MIN) && astc_par_on();
    bool chk = par && tex_par_check();
    if (chk) for (uint32_t c = 0; c < nc; c++) hs[c] = TEXH_UNSET;   // (a slot the join left unwritten keeps it)
    if (par) astc_job_run((AstcJob){.src = p, .nchunk = nc, .kind = 2, .hslot = hs, .hn = n});
    else for (uint32_t c = 0; c < nc; c++) hs[c] = hash_full(p + ((uint64_t)c << TEXH_CHUNK_LOG2), n - ((uint64_t)c << TEXH_CHUNK_LOG2) < ch ? n - ((uint64_t)c << TEXH_CHUNK_LOG2) : ch);
    if (chk) hash_par_check(p, n, nc, hs);
    uint64_t h = 0x9E3779B97F4A7C15ull ^ n;
    for (uint32_t c = 0; c < nc; c++) { h = (h ^ hs[c]) * 0x100000001B3ull; h ^= h >> 29; }
    if (hs != stack) free(hs);
    return h ^ (h >> 31);
}
static uint64_t hash_tex(const uint8_t *p, uint64_t n) { return hash_tex_(p, n, -1); }
// Sizes around the 1 MB chunk and the 4 MB fan-out edges, inline vs the pool, over pseudo-random bytes "[tex] hash_tex
// self-test". Run once by LARECOMP_TEX_THREADS_CHECK=1 (tex_par_check), callable from tools.
bool tex_hash_selftest(void) {
    const uint64_t M = 1ull << 20, sizes[] = {M - 1, M, M + 1, 2 * M - 1, 2 * M + 1, 4 * M - 1, 4 * M, 4 * M + 1, 5 * M + 7, 64 * M + 3};
    uint64_t cap = 64 * M + 3, x = 0x9E3779B97F4A7C15ull;
    uint8_t *p = malloc(cap);
    if (!p) return false;
    for (uint64_t i = 0; i < cap; i++) { x ^= x << 13; x ^= x >> 7; x ^= x << 17; p[i] = (uint8_t)x; }
    int bad = 0, n = (int)(sizeof sizes / sizeof sizes[0]);
    for (int i = 0; i < n; i++) {
        uint64_t a = hash_tex_(p, sizes[i], 0), b = hash_tex_(p, sizes[i], 1), c = hash_tex_(p, sizes[i], -1);
        if (a != b || a != c) { bad++; LOG("[tex] hash_tex self-test: %llu bytes: inline %016llx, pool %016llx, by size %016llx\n",
                                           (unsigned long long)sizes[i], (unsigned long long)a, (unsigned long long)b, (unsigned long long)c); }
    }
    LOG("[tex] hash_tex self-test: %d sizes, %d differ (pool %s)\n", n, bad, astc_par_on() ? "on" : "off");
    free(p);
    return bad == 0;
}

/* PC filtering: TOTK_ANISO=<n> raises anisotropic filtering to n (at most 16) on mipmapped samplers with linear
 * minification (never on depth-compare, nearest or mipless ones; never below the guest's own); TOTK_LOD_BIAS=<x> adds x
 * to the guest's LOD bias on mipmapped samplers (negative = sharper; a temporal upscaler wants -log2(out/render)). Both
 * read at the start, then live (graphics.anisotropy / graphics.lod_bias, renderer_set_aniso / _lod_bias): a change is
 * taken at the next present, where every sampler is re-made at its next use (g_samp_gen, vk_sampler) and every cached
 * descriptor dropped (exec.c vk_exec_descriptors_stale). vk_samplers_rebuild asks the same for a term of another area
 * (the dlss worker's SR bias).*/
static float g_aniso = -1, g_lod_bias = -1e9f;   // (render thread after the start)
static int g_aniso_req = -1, g_bias_req = -1000000, g_samp_req;   // (settings thread: x 1000; -1 / -1000000: none)
static uint32_t g_samp_gen;
static float pc_aniso(void) {
    if (g_aniso < 0) { const char *e = getenv("TOTK_ANISO"); float v = e ? (float)atof(e) : 16; if (v > 16) v = 16; g_aniso = v; if (v > 1) LOG("[smp] TOTK_ANISO=%.0f\n", (double)v); }
    return g_aniso;
}
static float pc_lod_bias(void) {
    if (g_lod_bias < -1e8f) { const char *e = getenv("TOTK_LOD_BIAS"); g_lod_bias = e ? (float)atof(e) : 0; if (g_lod_bias != 0) LOG("[smp] TOTK_LOD_BIAS=%.2f\n", (double)g_lod_bias); }
    // DLSS SR (ngx.c): the jittered scene's material textures sampled finer, so DLSS has the detail to accumulate. A
    // live read (NAN before the start's scales are known: no term yet); ngx.c calls vk_samplers_rebuild when it changes.
    // The total is kept per g_samp_gen once the term is known: every change of either part re-makes the samplers, which
    // moves g_samp_gen
    static uint32_t gen_cached = ~0u;
    static float total;
    const uint32_t g = __atomic_load_n(&g_samp_gen, __ATOMIC_ACQUIRE);
    if (__atomic_load_n(&gen_cached, __ATOMIC_ACQUIRE) == g) return total;
    const float d = vk_ngx_sr_lod_bias();
    if (isnan(d)) return g_lod_bias;   // (not kept: asked again until known)
    total = g_lod_bias + d;
    __atomic_store_n(&gen_cached, g, __ATOMIC_RELEASE);
    return total;
}
void renderer_set_aniso(const char *v) {
    float a = v ? (float)atof(v) : 16;
    if (a > 16) a = 16;
    if (a < 0) a = 0;
    __atomic_store_n(&g_aniso_req, (int)lroundf(a * 1000.0f), __ATOMIC_RELEASE);
}
void renderer_set_lod_bias(const char *v) { __atomic_store_n(&g_bias_req, (int)lroundf((v ? (float)atof(v) : 0) * 1000.0f), __ATOMIC_RELEASE); }
void vk_samplers_rebuild(void) { __atomic_store_n(&g_samp_req, 1, __ATOMIC_RELEASE); }
// (renderer_set_out_scale, each present: the sampler rows' changes)
static void samplers_tick(void) {
    bool remake = __atomic_exchange_n(&g_samp_req, 0, __ATOMIC_ACQ_REL) != 0;
    const int a = __atomic_exchange_n(&g_aniso_req, -1, __ATOMIC_ACQ_REL), lb = __atomic_exchange_n(&g_bias_req, -1000000, __ATOMIC_ACQ_REL);
    pc_lod_bias();   // (the start's values read)
    static uint32_t epoch_seen;   // (a live render scale or canvas change: DLSS SR's part of the bias follows the new ratio,
    static float d_made;          // to 0 too, e.g. a scene now larger than the output; ngx.c answers from the current sizes)
    const uint32_t ep = vk_render_epoch();
    if (ep != epoch_seen) {
        epoch_seen = ep;
        float d = vk_ngx_sr_lod_bias();
        if (isnan(d)) d = 0;
        if (d != d_made) d_made = d, remake = true;
    }
    if (a >= 0 && a / 1000.0f != pc_aniso()) g_aniso = a / 1000.0f, remake = true;
    if (lb != -1000000 && lb / 1000.0f != g_lod_bias) g_lod_bias = lb / 1000.0f, remake = true;
    if (!remake) return;
    __atomic_add_fetch(&g_samp_gen, 1, __ATOMIC_RELEASE);
    {   // (the term these samplers get: a re-make asked by ngx.c itself counts too)
        const float d = vk_ngx_sr_lod_bias();
        d_made = isnan(d) ? 0 : d;
    }
    extern void vk_exec_descriptors_stale(void);
    vk_exec_descriptors_stale();
    LOG("[smp] samplers re-made (live): aniso %.0f, LOD bias %.2f (the setting's)\n", (double)pc_aniso(), (double)g_lod_bias);
}
/* VkSamplers by translated content: pool entries with the same parameters share one, and an entry re-registered with
 * new parameters no longer leaks the one it had (samplers are never destroyed; the distinct contents are few). */
typedef struct { VkSamplerCreateInfo ci; VkSamplerCustomBorderColorCreateInfoEXT cb; VkSampler s; } samp_ent;
static samp_ent *g_samp_tab; static size_t g_samp_cap, g_samp_n;
static pthread_mutex_t g_samp_mu = PTHREAD_MUTEX_INITIALIZER;
static uint64_t samp_hash(const VkSamplerCreateInfo *ci, const VkSamplerCustomBorderColorCreateInfoEXT *cb) {
    uint64_t h = 1469598103934665603ull; const uint8_t *p = (const uint8_t *)&ci->flags;   /* from flags: not sType / pNext */
    for (size_t i = 0; i < sizeof *ci - offsetof(VkSamplerCreateInfo, flags); i++) h = (h ^ p[i]) * 1099511628211ull;
    if (ci->pNext) for (size_t i = 0; i < 16; i++) h = (h ^ ((const uint8_t *)cb->customBorderColor.float32)[i]) * 1099511628211ull;
    return h;
}
static int samp_same(const samp_ent *e, const VkSamplerCreateInfo *ci, const VkSamplerCustomBorderColorCreateInfoEXT *cb) {
    size_t o = offsetof(VkSamplerCreateInfo, flags);
    if (memcmp((const uint8_t *)&e->ci + o, (const uint8_t *)ci + o, sizeof *ci - o) || !e->ci.pNext != !ci->pNext) return 0;
    return !ci->pNext || !memcmp(e->cb.customBorderColor.float32, cb->customBorderColor.float32, 16);
}
static VkSampler samp_get(const VkSamplerCreateInfo *ci, const VkSamplerCustomBorderColorCreateInfoEXT *cb) {
    pthread_mutex_lock(&g_samp_mu);
    if (g_samp_n * 2 >= g_samp_cap) {   /* open addressing, grown at half full */
        size_t nc = g_samp_cap ? g_samp_cap * 2 : 256; samp_ent *nt = calloc(nc, sizeof *nt);
        for (size_t i = 0; i < g_samp_cap; i++) if (g_samp_tab[i].s) {
            size_t j = samp_hash(&g_samp_tab[i].ci, &g_samp_tab[i].cb) & (nc - 1);
            while (nt[j].s) j = (j + 1) & (nc - 1);
            nt[j] = g_samp_tab[i];
            if (nt[j].ci.pNext) nt[j].ci.pNext = &nt[j].cb;
        }
        free(g_samp_tab); g_samp_tab = nt; g_samp_cap = nc;
    }
    size_t j = samp_hash(ci, cb) & (g_samp_cap - 1);
    while (g_samp_tab[j].s && !samp_same(&g_samp_tab[j], ci, cb)) j = (j + 1) & (g_samp_cap - 1);
    if (!g_samp_tab[j].s) {
        samp_ent *e = &g_samp_tab[j];
        e->ci = *ci; e->cb = *cb;
        if (ci->pNext) e->ci.pNext = &e->cb;   /* the stored copy points at its own border colour */
        VK_CHECK(vkCreateSampler(g_vk.dev, &e->ci, NULL, &e->s));
        g_samp_n++;
    }
    VkSampler s = g_samp_tab[j].s;
    pthread_mutex_unlock(&g_samp_mu);
    return s;
}

// The PC anisotropy floor only where the game samples through its mips (public #48, dd2war 7900 XTX: a bright jagged
// fringe along Link's outline at every 2x-16x, none at "The game's"): a sampler whose LOD range pins one level (lod
// 0..0, 1..1: a lookup / ramp / data read; rule 1) and a texture of one level (exec.c texture_info_; rule 2) keep the
// game's own filter, as on the Switch. There the floor still multi-taps level 0: it smears a lookup whose coordinate
// jumps (a character's shading ramp at the silhouette), and on a minified one-level texture it also cut some aliasing,
// which those now show as the game does. LARECOMP_ANISO_MIPS: unset / 1 both rules, =0 neither (v0.1.8), =sampler rule
// 1 only, =levels rule 2 only (#48's A/B names the rule in one round).
int vk_aniso_mips_rules(void) {   // (bit 0: rule 1, bit 1: rule 2)
    static int r = -1;
    if (r < 0) {
        const char *e = getenv("LARECOMP_ANISO_MIPS");
        r = !e || !*e || *e == '1' ? 3 : *e == '0' ? 0 : !strcmp(e, "sampler") ? 1 : !strcmp(e, "levels") ? 2 : 3;
        if (r != 3)
            LOG("[smp] game's filter on one-level reads: %s (LARECOMP_ANISO_MIPS)\n",
                r == 0 ? "off, the floor everywhere (v0.1.8)" : r == 1 ? "pinned-level samplers only" : "one-level textures only");
    }
    return r;
}
// cmp = false: the same sampler without the depth compare (vk_sampler_plain). pc_over: the PC overrides it takes, PC_ANISO
// (the TOTK_ANISO floor) and / or PC_BIAS (the PC / DLSS LOD bias)
enum { PC_ANISO = 1, PC_BIAS = 2, PC_BOTH = 3 };
static VkSampler sampler_make(const NvnSamplerDesc *d, bool cmp, VkSamplerCreateInfo *out, int pc_over) {
    VkSamplerCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = d->mag_filter == 1 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    uint32_t mf = d->min_filter;
    si.minFilter = (mf == 1 || mf == 3 || mf == 5) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    si.mipmapMode = mf == 4 || mf == 5 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    VkSamplerAddressMode modes[3];
    bool border = false;
    for (int i = 0; i < 3; i++) {
        switch (d->wrap[i]) {
        case 0: case 7: modes[i] = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE; break;
        case 1: modes[i] = VK_SAMPLER_ADDRESS_MODE_REPEAT; break;
        case 2: case 3: modes[i] = g_vk.mirror_clamp ? VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE : VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT; break;
        case 4: case 5: modes[i] = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER; border = true; break;
        case 6: modes[i] = VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT; break;
        default: modes[i] = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        }
    }
    si.addressModeU = modes[0]; si.addressModeV = modes[1]; si.addressModeW = modes[2];
    si.minLod = d->lod_min;
    si.maxLod = d->lod_max > 1000 ? VK_LOD_CLAMP_NONE : d->lod_max;
    if (mf <= 1) si.maxLod = si.minLod + 0.25f;  // not mipmapped: stay on the base level
    if (si.maxLod < si.minLod) si.maxLod = si.minLod;
    si.mipLodBias = d->lod_bias;
    if (d->max_aniso > 1) { si.anisotropyEnable = VK_TRUE; si.maxAnisotropy = fminf(d->max_aniso, 16); }
    static float lodmin = -1;
    if (lodmin < 0) lodmin = getenv("LARECOMP_LOD_MIN") ? (float)atof(getenv("LARECOMP_LOD_MIN")) : 0;
    if (lodmin > 0 && mf > 1) si.minLod = fmaxf(si.minLod, lodmin);
    static int stest = -1;
    if (stest < 0) stest = getenv("LARECOMP_SAMPLER_TEST") != NULL;
    if (stest && mf > 1) { si.minLod = 0; si.maxLod = VK_LOD_CLAMP_NONE; si.anisotropyEnable = VK_TRUE; si.maxAnisotropy = 16; }
    if (mf > 1) {
        float an = pc_aniso();
        const bool mips = !(vk_aniso_mips_rules() & 1) || si.maxLod == VK_LOD_CLAMP_NONE || si.maxLod - si.minLod >= 1.0f;   // (#48)
        if ((pc_over & PC_ANISO) && an > 1 && mips && si.minFilter == VK_FILTER_LINEAR && !d->compare_mode && (!si.anisotropyEnable || si.maxAnisotropy < an)) { si.anisotropyEnable = VK_TRUE; si.maxAnisotropy = an; }
        if (pc_over & PC_BIAS) si.mipLodBias += pc_lod_bias();
    }
    // (only where the device enabled anisotropy, and up to its limit: VUID-VkSamplerCreateInfo-anisotropyEnable-01070 / 01071;
    // every desktop GPU has 16x, so nothing changes there)
    if (si.anisotropyEnable && !g_vk.sampler_aniso) si.anisotropyEnable = VK_FALSE, si.maxAnisotropy = 1;
    if (si.anisotropyEnable && si.maxAnisotropy > g_vk.props.limits.maxSamplerAnisotropy) si.maxAnisotropy = g_vk.props.limits.maxSamplerAnisotropy;
    if (d->compare_mode && cmp) {
        si.compareEnable = VK_TRUE;
        si.compareOp = getenv("LARECOMP_NO_SHADOW") ? VK_COMPARE_OP_ALWAYS : compare_op(d->compare_func);
    }
    VkSamplerCustomBorderColorCreateInfoEXT cb = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CUSTOM_BORDER_COLOR_CREATE_INFO_EXT};
    if (border) {
        const float *c = d->border;
        if (c[0] == 0 && c[1] == 0 && c[2] == 0 && c[3] == 0) si.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
        else if (c[0] == 0 && c[1] == 0 && c[2] == 0 && c[3] == 1) si.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
        else if (c[0] == 1 && c[1] == 1 && c[2] == 1 && c[3] == 1) si.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
        else if (g_vk.custom_border) {
            memcpy(cb.customBorderColor.float32, c, 16);
            si.borderColor = VK_BORDER_COLOR_FLOAT_CUSTOM_EXT;
            si.pNext = &cb;
        } else si.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    }
    if (out) *out = si;
    return samp_get(&si, &cb);
}
// Which descriptors' cached samplers were made since the last live sampler change (g_samp_gen > 0 only): an open-addressed
// set of NvnSamplerDesc pointers, emptied at each change, so NvnSamplerDesc keeps its size. A freed and reused address
// comes back with backend NULL; a full set re-makes through the content table (same VkSampler, a hash slower). Before any
// live change: the old path, untouched.
#define SAMP_SEEN 16384
static NvnSamplerDesc **g_samp_seen;
static uint32_t g_samp_seen_n, g_samp_seen_gen;
static bool samp_seen(NvnSamplerDesc *d, bool add) {   // (g_samp_mu held)
    if (g_samp_seen_gen != g_samp_gen) {
        if (!g_samp_seen) g_samp_seen = calloc(SAMP_SEEN, sizeof *g_samp_seen);
        else memset(g_samp_seen, 0, SAMP_SEEN * sizeof *g_samp_seen);
        g_samp_seen_n = 0, g_samp_seen_gen = g_samp_gen;
    }
    if (!g_samp_seen) return false;
    uint32_t i = (uint32_t)(((uintptr_t)d * 0x9E3779B97F4A7C15ull) >> 50) & (SAMP_SEEN - 1);
    for (; g_samp_seen[i]; i = (i + 1) & (SAMP_SEEN - 1)) if (g_samp_seen[i] == d) return true;
    if (add && g_samp_seen_n < SAMP_SEEN * 3 / 4) g_samp_seen[i] = d, g_samp_seen_n++;
    return false;
}
VkSampler vk_sampler(NvnSamplerDesc *d) {
    if (d->backend) {
        if (!__atomic_load_n(&g_samp_gen, __ATOMIC_ACQUIRE)) return (VkSampler)d->backend;
        pthread_mutex_lock(&g_samp_mu);
        const bool seen = samp_seen(d, false);
        pthread_mutex_unlock(&g_samp_mu);
        if (seen) return (VkSampler)d->backend;
    }
    VkSamplerCreateInfo si;
    VkSampler s = sampler_make(d, true, &si, PC_BOTH);
    d->backend = (void *)s;   // (the old one stays in the content table: samplers are never destroyed, their contents few)
    if (__atomic_load_n(&g_samp_gen, __ATOMIC_ACQUIRE)) {
        pthread_mutex_lock(&g_samp_mu);
        samp_seen(d, true);
        pthread_mutex_unlock(&g_samp_mu);
    }
    {   // measurement (blurry ground, 2026-10-14): the guest's LOD settings per sampler, first 64
        static int nlog;
        if (nlog++ < 64)
            LOG("[smp] %p min %u mag %u wrap %u/%u/%u lod %.3f..%.3f bias %.3f aniso %.0f cmp %u -> vk minLod %.3f maxLod %.3f\n",
                (void *)d, d->min_filter, d->mag_filter, d->wrap[0], d->wrap[1], d->wrap[2], d->lod_min, d->lod_max, d->lod_bias,
                (double)d->max_aniso, d->compare_mode, si.minLod, si.maxLod);
    }
    if (d->compare_mode) {   // (32:9 grey, gate: what a shadow lookup outside the map reads; one line per distinct compare
        // sampler content, at most 32: the border the game set and what Vulkan got: a depth of 0 there compares as in
        // shadow for LESS / LEQUAL, 1 as lit)
        static struct { uint32_t wrap, func; float border[4]; } seen[32];
        static int nseen;
        const uint32_t wrap = d->wrap[0] | d->wrap[1] << 4 | d->wrap[2] << 8;
        pthread_mutex_lock(&g_samp_mu);
        bool dup = false;
        for (int i = 0; i < nseen && !dup; i++)
            dup = seen[i].wrap == wrap && seen[i].func == d->compare_func && !memcmp(seen[i].border, d->border, sizeof seen[i].border);
        if (!dup && nseen < 32) {
            seen[nseen].wrap = wrap, seen[nseen].func = d->compare_func;
            memcpy(seen[nseen].border, d->border, sizeof seen[nseen].border);
            nseen++;
            LOG("[smp] compare sampler %p: wrap %u/%u/%u func %u border %.3f %.3f %.3f %.3f -> vk compareOp %d borderColor %d%s\n",
                (void *)d, d->wrap[0], d->wrap[1], d->wrap[2], d->compare_func, (double)d->border[0], (double)d->border[1],
                (double)d->border[2], (double)d->border[3], (int)si.compareOp, (int)si.borderColor,
                si.borderColor == VK_BORDER_COLOR_FLOAT_CUSTOM_EXT ? " (custom)" : "");
        }
        pthread_mutex_unlock(&g_samp_mu);
    }
    return s;
}
// A compare sampler on a binding the shader samples without a reference: Maxwell's TSC depth compare acts only on its
// depth-compare fetches, while Vulkan leaves a non-Dref sample through a compareEnable sampler undefined (NVIDIA ignores
// the compare. The same sampler without the compare; samp_get keeps one per content, so this is a lookup after the
// first.
VkSampler vk_sampler_plain(NvnSamplerDesc *d) {
    if (!d->compare_mode) return vk_sampler(d);
    return sampler_make(d, false, NULL, PC_BOTH);
}
// The PC LOD bias (TOTK_LOD_BIAS, Options' texture LOD bias) is for the game's material textures, whose whole mip chain
// is uploaded from the game's files. On textures the GPU renders or encodes (render targets, block-compressed images the
// game encodes on the GPU, 3D / cube ones) it moves the sampling to levels the game never wrote for that use: public issue
// #20 (+2: Great Sky Island grass pure red, G = B = 0; a GPU-encoded BC4 3D texture of 6 levels where only level 0 is
// encoded). exec.c texture_info_ takes this twin there: the same sampler without the PC bias (plain: without the compare).
bool vk_pc_lod_bias_on(void) { return pc_lod_bias() != 0; }
// The PC anisotropy floor (TOTK_ANISO, default 16) is scoped the same way: on render targets, depth, half-resolution
// particle buffers and other GPU-written images the game samples them with its own filtering only (public issue #19:
// hard squares on torch embers at 1.0 scale, 3080 and 9070 XT; #17 mist flicker and shadow flicker reports of the same
// class). vk_sampler_nobias's twin drops both PC overrides.
bool vk_pc_sampler_override_on(void) { return pc_lod_bias() != 0 || pc_aniso() > 1; }
// Memoised by the biased sampler, which stands for the entry's content (samp_get: one VkSampler per content, never
// destroyed), so a bind is two loads, not sampler_make's hash and lock on every bind of a render target. Not in
// NvnSamplerDesc itself: capture.c writes the pools' entries as they are, and replays read that layout. The render
// thread's (exec.c texture_info_). aniso: keep the anisotropy floor, drop only the PC bias (vk_sampler_noplus).
static VkSampler sampler_twin(NvnSamplerDesc *d, bool plain, bool aniso) {
    static struct { VkSampler key, twin[2][2]; } memo[1024];
    VkSampler k = vk_sampler(d);
    uintptr_t u = (uintptr_t)k;
    size_t i = (u >> 4 ^ u >> 14) & 1023;
    if (memo[i].key != k) memset(&memo[i], 0, sizeof memo[i]), memo[i].key = k;
    if (!memo[i].twin[aniso][plain]) memo[i].twin[aniso][plain] = sampler_make(d, !plain, NULL, aniso ? PC_ANISO : 0);
    return memo[i].twin[aniso][plain];
}
VkSampler vk_sampler_nobias(NvnSamplerDesc *d, bool plain) { return sampler_twin(d, plain, false); }
// A raised PC bias (above 0: the texture LOD bias slider's blurrier half) is for surfaces. On the effect draws (exec.c
// texture_info_: blended into colour target 0 without a depth write: fire, smoke, sparks, glows) it picks levels where a
// sheet of animation frames bleeds into its neighbours: wider flames with blocky edges beside a cooking pot at +2 (public
// issue #50, RX 7900 XTX; 0 cured it). Those keep the game's own bias, and the anisotropy floor as before.
bool vk_pc_lod_bias_plus(void) { return pc_lod_bias() > 0; }
VkSampler vk_sampler_noplus(NvnSamplerDesc *d, bool plain) { return sampler_twin(d, plain, true); }

VkSampler vk_default_sampler(bool linear) {
    static VkSampler s[2];
    if (!s[linear]) {
        VkSamplerCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                                  .magFilter = linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
                                  .minFilter = linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
                                  .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                  .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                  .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .maxLod = 0.25f};
        VK_CHECK(vkCreateSampler(g_vk.dev, &si, NULL, &s[linear]));
    }
    return s[linear];
}

// exec.c resolve_feedback (an alias of a bound attachment read from a snapshot, no feedback-loop declaration): the view
// vk_view would give `t` (format in t's own reading, swizzle, levels / layers), but of image `b` (the snapshot: a
// MUTABLE_FORMAT copy of the attachment, same texel size)
VkImageView vk_view_on(TexBackend *b, NvnTexture *t, const NvnTextureView *v, bool has_view, int want_textype) {
    g_view_on = b;
    VkImageView r = vk_view(t, v, has_view, false, want_textype);
    g_view_on = NULL;
    return r;
}
