// Vulkan textures: NVN formats, images, change-detected uploads from guest memory, views and samplers
// (counterpart of the texture half of metal.m and the view helpers of renderer.m).
#include <SDL3/SDL_timer.h>
#include <float.h>
#include <stddef.h>
#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#ifdef _MSC_VER
#include <intrin.h>
#else
#include <x86intrin.h>
#endif

#include "vk.h"
#include "../../cpu_place.h"   // nxrecomp: which cores each pool runs on
#include "shaders.h"
#include "astc_comp.h"
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

// ================================================================== output resolution / per-frame clears
// Same rules as metal.m (see the comments there).
static float g_rs_x = 1, g_rs_y = 1;
void renderer_get_scale(float *sx, float *sy) { *sx = g_rs_x; *sy = g_rs_y; }
void renderer_set_scale(float sx, float sy) { g_rs_x = sx; g_rs_y = sy; }
uint64_t renderer_present_count(void);

// Output canvas (TOTK_OUT_SCALE, 2026-10-17): TotK's 1920x1080 targets (the canvas its upscale pass writes and the UI draws on,
// the window textures, the menus' layers and 3D model buffers) at the window's resolution (device.c: viewport height / 1080), so
// the scene reaches the window in one resample (exec.c, the upscale pass) and the UI draws at full size. On by default since
// 2026-10-27; TOTK_OUT_SCALE=0 / the Options row graphics.native_ui = Off: the old stretched canvas. Active only when the
// window's 16:9 picture is taller than 1080 lines when first asked (the game's first 1920x1080 target or the upscale program,
// after the start's window mode is applied): at 1080 or less the canvas is the game's own size anyway, and every target and the
// upscale pass stay exactly as they were. Decided once, like the textures' sizes: a window made larger later keeps the start's
// choice until a restart.
static float g_out_s = 1;
static int out_scale_on(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("TOTK_OUT_SCALE"); on = !(e && !strcmp(e, "0")); }
    return on;
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
    const char *how = "the window", *off = "off, TOTK_OUT_SCALE=0", *wide = getenv("TOTK_OUTPUT_RES");
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
// device.c, every present: the decision by the first one at the latest (the scale stays the start's: above)
void renderer_set_out_scale(float s) { (void)s; renderer_out_scale_active(); }

// Shadow maps: square depth render targets (TotK: the 800x800 x4 sun cascades, D32) rendered at TOTK_SHADOW_SCALE x
// their size (default 2; 1 = the game's own). The user: "theres still alot of shimmering on the shadows". Shaders see
// the guest size through the rscale table like every scaled target (translate.c).
static float shadow_scale(void) {
    static float s = -1;
    if (s < 0) {
        const char *e = getenv("TOTK_SHADOW_SCALE");
        s = e && *e ? (float)atof(e) : 2.0f;
        if (!(s >= 1.0f && s <= 4.0f)) s = 1.0f;
    }
    return s;
}
// Promoted targets: TotK renders some non-compressible targets in one pass with a scaled one of the same size (fs
// dd9edc34: an R8 1600x900 mask with the scene's depth buffer, every frame; fs 7e405a3d: R11G11B10F + R8 800x450 while
// loading). begin_pass renders the smallest attachment extent, so at TOTK_RES_SCALE=2 the pass used a quarter of the
// scaled attachment. exec.c begin_pass promotes the unscaled one (storage address + size) to the scene's scale from its
// next backend (TotK re-creates its targets every frame). LARECOMP_NO_PROMOTE=1: off. The shadow-map kind too
// (2026-10-30: a square 512x512 D32 classed as a shadow map, at TOTK_SHADOW_SCALE 4 = 2048x2048, drawn with an unscaled
// R8 512x512 colour target: fs c509b81e / 6c4e6d40 / 915cd25e / 123a608c rendered at 1.0 into the top-left sixteenth of
// the depth, testing against the wrong texels): the colour target takes the depth's kind. LARECOMP_PROMO_SHADOW=0: scene
// scale only (old).
#define PROMO_MAX 64
static struct { uint64_t addr; uint32_t w, h; int kind; } g_promo[PROMO_MAX];
static int g_npromo;
static int tex_promoted(const NvnTexture *t) {   // (the kind it was promoted to, 0 = not promoted)
    int n = __atomic_load_n(&g_npromo, __ATOMIC_ACQUIRE);
    for (int i = 0; i < n; i++)
        if (g_promo[i].addr == t->addr && g_promo[i].w == t->width && g_promo[i].h == t->height) return g_promo[i].kind;
    return 0;
}
void renderer_texture_promote_kind(NvnTexture *t, int kind) {
    static int off = -1;
    if (off < 0) off = getenv("LARECOMP_NO_PROMOTE") != NULL;
    if (off || !t || !t->addr || t->samples > 1 || t->target != 1 || g_npromo >= PROMO_MAX || tex_promoted(t)) return;
    g_promo[g_npromo].addr = t->addr; g_promo[g_npromo].w = t->width; g_promo[g_npromo].h = t->height; g_promo[g_npromo].kind = kind;
    __atomic_store_n(&g_npromo, g_npromo + 1, __ATOMIC_RELEASE);
    LOG("[tex] promoted to the %s scale (rendered next to a scaled target of its size): addr %llx %ux%u f%#x flags %#x\n",
        kind == 2 ? "shadow maps'" : "scene", (unsigned long long)t->addr, t->width, t->height, t->format, t->flags);
}
void renderer_texture_promote(NvnTexture *t) { renderer_texture_promote_kind(t, 1); }
static void destroy_image(TexBackend *b);
static void create_image(NvnTexture *t, TexBackend *b);
// (exec.c begin_pass, outside a render pass) a promoted target whose object kept its unscaled backend is reallocated
// at the scene's scale, cleared to zero
bool renderer_texture_apply_promotion(NvnTexture *t) {
    TexBackend *b = t ? (TexBackend *)t->backend : NULL;
    if (!b || b->scalable || !g_npromo || !b->image || b->samples > 1 || b->blk || b->pending_rgba || !tex_promoted(t)) return false;
    extern uint32_t vk_view_gen;
    destroy_image(b);
    b->scale_kind = tex_promoted(t);
    b->scalable = true;
    create_image(t, b);
    vk_view_gen++;   // (exec.c's per-frame view memo is keyed by the texture object)
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
        LOG("[tex] promoted target reallocated: %p addr %llx %ux%u f%#x -> %ux%u\n", (void *)t, (unsigned long long)t->addr, t->width,
            t->height, t->format, b->width, b->height);
    return true;
}

// A scaled depth drawn with a colour target's level of the same guest size but a larger host extent (2026-10-31: fs 6b2da6dd
// draws level 1 of the 400x225 colour target with a standalone 200x112 depth; on the host that level is floor(450 / 2) = 225
// rows, the depth 2 x 112 = 224, so the render area cut the level's last row, never written in that pass, and the bloom
// chain pulled the stale row in as a cyan band at the bottom; at 1.333 the same pass was the ALLOC_CEIL case). exec.c
// begin_pass records the colour level's host extent here; the depth is then allocated at least that large (its own ratio
// follows the allocation, as every scaled image's). LARECOMP_DEPTH_GROW=0: off.
#define GROW_MAX 64
static struct { uint64_t addr; uint32_t w, h, fmt, hw, hh; } g_grow[GROW_MAX];
static int g_ngrow;
static bool tex_grown(const NvnTexture *t, uint32_t *hw, uint32_t *hh) {
    int n = __atomic_load_n(&g_ngrow, __ATOMIC_ACQUIRE);
    for (int i = 0; i < n; i++)
        if (g_grow[i].addr == t->addr && g_grow[i].w == t->width && g_grow[i].h == t->height && g_grow[i].fmt == t->format) { *hw = g_grow[i].hw; *hh = g_grow[i].hh; return true; }
    return false;
}
bool renderer_texture_grow(NvnTexture *t, uint32_t hw, uint32_t hh) {
    static int off = -1;
    if (off < 0) { const char *e = getenv("LARECOMP_DEPTH_GROW"); off = e && !strcmp(e, "0"); }
    TexBackend *b = t ? (TexBackend *)t->backend : NULL;
    if (off || !b || !b->scalable || !b->image || b->samples > 1 || b->blk || b->pending_rgba || t->target != 1) return false;
    if (b->width >= hw && b->height >= hh) return false;
    uint32_t ow, oh;
    if (!tex_grown(t, &ow, &oh)) {
        if (g_ngrow >= GROW_MAX) return false;
        g_grow[g_ngrow].addr = t->addr; g_grow[g_ngrow].w = t->width; g_grow[g_ngrow].h = t->height; g_grow[g_ngrow].fmt = t->format;
        g_grow[g_ngrow].hw = hw; g_grow[g_ngrow].hh = hh;
        __atomic_store_n(&g_ngrow, g_ngrow + 1, __ATOMIC_RELEASE);
    } else {   // (a larger level seen later: the table keeps the largest)
        for (int i = 0; i < g_ngrow; i++)
            if (g_grow[i].addr == t->addr && g_grow[i].w == t->width && g_grow[i].h == t->height && g_grow[i].fmt == t->format) {
                if (hw > g_grow[i].hw) g_grow[i].hw = hw;
                if (hh > g_grow[i].hh) g_grow[i].hh = hh;
            }
    }
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

// 0 = rendered at the game's size, 1 = 16:9 colour/depth target (output scale), 2 = shadow map
static int tex_scale_kind(const NvnTexture *t) {
    // (cached: a getenv per call - per sampled texture per draw - was 0.24 ms a frame of the render thread in ucrtbase,
    // the a1 boot at ~99 fps)
    static int no_scale = -1;
    if (no_scale < 0) no_scale = getenv("LARECOMP_NO_SCALE") != NULL;
    if (no_scale) return 0;
    // (every 1920x1080 2D target, compressible or not, colour or depth: the inventory draws 6 targets at once, a compressible
    // one at the scene's scale next to the canvas's -> only the top-left 2/3 drawn)
    if (t->samples <= 1 && t->target == 1 && t->width == 1920 && t->height == 1080 && renderer_out_scale_active()) {
        static int nlog;
        if (nlog++ < 8) LOG("[tex] output canvas scale: addr %llx f%#x flags %#x\n", (unsigned long long)t->addr, t->format, t->flags);
        return 3;
    }
    if (t->samples <= 1 && t->target == 1 && g_npromo) { const int pk = tex_promoted(t); if (pk) return pk; }
    if (t->samples > 1 || !(t->flags & 8)) return 0;
    if ((t->target == 1 || t->target == 4) && t->width == t->height && t->width >= 256 && renderer_format_is_depth(t->format) &&
        shadow_scale() != 1.0f)
        return 2;
    // 2D and 2D-array targets: a pass mixes them (TotK's 400x225 shadow-mask pass has a 2-layer array target next to 2D
    // ones), and begin_pass renders the minimum attachment extent, so an unscaled array clipped the pass to the top-left
    // 1/scale^2 of the screen (TOTK_RES_SCALE=2 lit only the top-left quarter)
    if ((t->target != 1 && t->target != 4) || t->width < 32 || t->height < 16) return 0;
    float a = (float)t->width / (float)t->height;
    return a > 1.74f && a < 1.81f ? 1 : 0;
}
static bool tex_scalable(const NvnTexture *t) { return tex_scale_kind(t) != 0; }
static float kind_sx(int kind) { return kind == 1 ? g_rs_x : kind == 2 ? shadow_scale() : kind == 3 ? g_out_s : 1.0f; }
static float kind_sy(int kind) { return kind == 1 ? g_rs_y : kind == 2 ? shadow_scale() : kind == 3 ? g_out_s : 1.0f; }

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

void vk_collect_dead(void) {
    uint64_t done = vk_completed_seq();
    int k = 0;
    for (int i = 0; i < ndead; i++) {
        if (dead[i].seq > done) { dead[k++] = dead[i]; continue; }
        if (dead[i].view) vkDestroyImageView(g_vk.dev, dead[i].view, NULL);
        else if (dead[i].buf) vmaDestroyBuffer(g_vk.vma, dead[i].buf, dead[i].mem);
        else vmaDestroyImage(g_vk.vma, dead[i].img, dead[i].mem);
    }
    ndead = k;
}

uint64_t prof_img_n, prof_img_ns;  // (exec.c LARECOMP_VK_PROF) images created and the time spent creating them
uint64_t cum_img_n, cum_img_ns;    // the same, never reset (exec.c hitch log)
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
static bool dac_listed(const NvnTexture *t);
static VkFormat dac_colour(VkFormat f);
static void create_image(NvnTexture *t, TexBackend *b) {
    extern uint64_t prof_up_ns;   // (~0: profiling off)
    uint64_t t0 = SDL_GetTicksNS();
    create_image_(t, b);
    uint64_t dt = SDL_GetTicksNS() - t0;
    __atomic_add_fetch(&cum_img_n, 1, __ATOMIC_RELAXED); __atomic_add_fetch(&cum_img_ns, dt, __ATOMIC_RELAXED);
    if (prof_up_ns != ~0ull) { prof_img_n++; prof_img_ns += dt; }
}
static void create_image_(NvnTexture *t, TexBackend *b) {
    b->snap_tag = 0;   // (exec.c snapshot delta: a new image is no snapshot's mirror)
    VkFmt mf = fmt_of(t->format);
    b->sx = b->scalable ? kind_sx(b->scale_kind) : 1;
    b->sy = b->scalable ? kind_sy(b->scale_kind) : 1;
    uint32_t tw = t->width ? t->width : 1, th = t->height ? t->height : 1;
    if (b->scalable) {
        // Rounded UP: every guest pixel a scaled image partly covers exists on the host. Rounded to nearest, a 200x112 depth at
        // 1.333 was 267x149 while the 400x225 colour target's level 1 it is drawn with is 266x150, so the render area (the smaller)
        // cut the level's last row: never written in that pass (fs 6b2da6dd, every frame at 1.333; exact at 2.0), stale content a
        // bilinear read then pulls in at the region's bottom edge. Since 1d0fa529 every mapping uses the allocation's own ratio, so
        // the extra partial row / column is consistent. LARECOMP_ALLOC_CEIL=0: nearest.
        static int aceil = -1;
        if (aceil < 0) { const char *e = getenv("LARECOMP_ALLOC_CEIL"); aceil = !e || strcmp(e, "0"); }
        if (aceil) { tw = (uint32_t)ceilf(tw * b->sx - 1e-3f); th = (uint32_t)ceilf(th * b->sy - 1e-3f); }
        else { tw = (uint32_t)lroundf(tw * b->sx); th = (uint32_t)lroundf(th * b->sy); }
        if (tw < 1) tw = 1;
        if (th < 1) th = 1;
        uint32_t gw, gh;   // (renderer_texture_grow: at least its colour level's extent)
        if (g_ngrow && tex_grown(t, &gw, &gh)) { if (gw > tw) tw = gw; if (gh > th) th = gh; }
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
    VkImageCreateFlags flags = 0;
    if (!depthfmt) flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;  // format views (NVN texture views, raw sRGB views)
    if (type == VK_IMAGE_TYPE_2D && layers >= 6 && tw == th && samples == 1 && (cube_compat_all() || t->target == 8 || t->target == 9))
        flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    // 3D render targets (TotK renders mips / slices of 3D volumes): slices are drawn through 2D views
    if (type == VK_IMAGE_TYPE_3D && (usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)) flags |= VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT;
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .flags = flags, .imageType = type, .format = vf,
                            .extent = {tw, th, depth}, .mipLevels = levels, .arrayLayers = layers,
                            .samples = (VkSampleCountFlagBits)samples, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage,
                            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    extern _Thread_local int vma_shim_tag;
    vma_shim_tag = b->astc_bw ? 1 : 2;
    VkResult r = vmaCreateImage(g_vk.vma, &ci, &ai, &b->image, &b->mem, NULL);
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
    b->format = vf;
    b->usage = ci.usage;
    b->cflags = ci.flags;
    b->width = tw; b->height = th; b->depth = depth; b->layers = layers; b->levels = levels; b->samples = samples;
    b->aspect = depthfmt ? aspect_of(t->format) : VK_IMAGE_ASPECT_COLOR_BIT;
    b->base_type = type == VK_IMAGE_TYPE_1D ? (layers > 1 ? VK_IMAGE_VIEW_TYPE_1D_ARRAY : VK_IMAGE_VIEW_TYPE_1D)
                 : type == VK_IMAGE_TYPE_3D ? VK_IMAGE_VIEW_TYPE_3D
                 : t->target == 8 ? VK_IMAGE_VIEW_TYPE_CUBE : t->target == 9 ? VK_IMAGE_VIEW_TYPE_CUBE_ARRAY
                 : layers > 1 || t->target == 4 || t->target == 6 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    vk_image_init_layout(vk_upload_cmd(), b->image, b->aspect);
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

static void destroy_image(TexBackend *b) {
    views_forget(b);
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
    if (vmaCreateImage(g_vk.vma, &ci, &ai, &d->image, &d->mem, NULL) != VK_SUCCESS) { free(d); return NULL; }
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
    if (off || !texel_bytes(vf) || texel_bytes(vf) != bc_block_bytes(b->format) || b->samples > 1) return NULL;
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
    VkResult sr_ = vmaCreateImage(g_vk.vma, &ci, &ai, &s->image, &s->mem, NULL);
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

// ---- image recycling. TotK initializes and finalizes ~30 transient render textures per frame. A finalized
// texture's image goes to a pool; a new texture of the same shape takes it with its views. Contents are undefined,
// as with reused pool memory on hardware. LARECOMP_VK_NO_RECYCLE=1 disables.
#define RECYCLE_MAX 512
typedef struct { TexBackend *b; uint32_t format, target, w, h, d, levels, samples; bool scalable; float sx, sy; uint64_t seq, addr; bool gpu_owned; } Recycled;
static Recycled g_recycle[RECYCLE_MAX];
static int g_nrecycle;
static uint64_t g_recycle_bytes;   // (device memory held by pooled images: "[vram]" line)
uint64_t vk_recycle_bytes(int *n) { if (n) *n = g_nrecycle; return g_recycle_bytes; }
VkDeviceSize vma_shim_size(VmaAllocation al);
static pthread_mutex_t g_recycle_lock = PTHREAD_MUTEX_INITIALIZER;
uint64_t prof_img_reused, prof_img_same_addr;
uint64_t cum_img_reused, cum_img_unpooled;   // (never reset: device.c's "[vram] images" line, player logs included)
static int recycle_off(void) {
    static int off = -1;
    if (off < 0) off = getenv("LARECOMP_VK_NO_RECYCLE") != NULL;
    return off;
}
static void free_backend(TexBackend *b) { destroy_image(b); free(b->pending_rgba); free(b); }
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
    // Byte cap. Oldest go first. LARECOMP_VK_RECYCLE_MB=<n> (default 512; 0 = no cap).
    static int64_t cap = -1;
    if (cap < 0) { const char *e = getenv("LARECOMP_VK_RECYCLE_MB"); cap = (int64_t)(e ? atol(e) : 512) << 20; }
    TexBackend *evict[8];
    int nev = 0;
    pthread_mutex_lock(&g_recycle_lock);
    g_recycle_bytes += vma_shim_size(b->mem);
    while (g_nrecycle && nev < 8 && (g_nrecycle == RECYCLE_MAX || (cap && g_recycle_bytes > (uint64_t)cap))) {
        evict[nev] = g_recycle[0].b;
        memmove(g_recycle, g_recycle + 1, (size_t)(g_nrecycle - 1) * sizeof *g_recycle);
        g_nrecycle--;
        g_recycle_bytes -= vma_shim_size(evict[nev++]->mem);
    }
    g_recycle[g_nrecycle++] = (Recycled){b, t->format, t->target, t->width, t->height, t->depth, t->levels, t->samples, b->scalable, b->sx, b->sy,
                                         vk_pending_seq(), t->addr, b->gpu_owned};
    pthread_mutex_unlock(&g_recycle_lock);
    for (int i = 0; i < nev; i++) free_backend(evict[i]);
    return true;
}
static bool g_recycle_same;   // (debug log: the last recycle_get kept the same storage's image)
static bool recycle_shape(const Recycled *r, const NvnTexture *t, bool sc) {
    uint32_t gw, gh;   // (a grown depth, renderer_texture_grow: never handed an image of the size before it grew; the
                       // game re-creates the object every frame and the pool kept giving it the short one)
    if (sc && g_ngrow && tex_grown(t, &gw, &gh) && (r->b->width < gw || r->b->height < gh)) return false;
    return r->format == t->format && r->target == t->target && r->w == t->width && r->h == t->height && r->d == t->depth &&
           r->levels == t->levels && r->samples == t->samples && r->scalable == sc &&
           (!sc || (r->sx == kind_sx(tex_scale_kind(t)) && r->sy == kind_sy(tex_scale_kind(t))));
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
    pthread_mutex_lock(&g_recycle_lock);
    int pick = -1;
    if (!any && t->addr)
        for (int i = g_nrecycle - 1; i >= 0; i--) {
            Recycled *r = &g_recycle[i];
            if (r->addr == t->addr && recycle_shape(r, t, sc) && (r->gpu_owned || r->seq < cur)) { pick = i; same = true; owned = r->gpu_owned; break; }
        }
    if (pick < 0) {
        for (int k = 0; k < g_nrecycle; k++) {
            int i = any ? g_nrecycle - 1 - k : k;   // (old behaviour: newest first)
            Recycled *r = &g_recycle[i];
            if (!recycle_shape(r, t, sc) ||
                r->seq >= cur)   // finalized in the open submission: its last uses may still be recorded after our uploads run
                continue;
            pick = i;
            break;
        }
    }
    if (pick >= 0) {
        b = g_recycle[pick].b;
        g_recycle_bytes -= vma_shim_size(b->mem);
        memmove(&g_recycle[pick], &g_recycle[pick + 1], (size_t)(g_nrecycle - 1 - pick) * sizeof *g_recycle);
        g_nrecycle--;
    }
    pthread_mutex_unlock(&g_recycle_lock);
    if (!b) return NULL;
    prof_img_reused++;
    cum_img_reused++;
    if (same) {   // same memory: keep contents and state (only the live-list slot is new)
        b->reg = 0;   // (write-back state kept: a target re-created over the same storage every frame stays "per-frame";
        b->zero_on_write = false;
        b->gpu_owned = owned;
        prof_img_same_addr++;
        g_recycle_same = true;
        return b;
    }
    // fresh texture state on the same image and views
    b->wb_frame = 0; b->wb_state = 0;
    b->gpu_owned = b->uploaded = b->dynamic = b->cpu_dirty = b->presented = false;
    b->data_hash = b->sample_hash = b->checked_frame = b->full_frame = b->touched_frame = b->frame_cleared = 0;
    b->level_gen = 0; b->tp = 0; b->tp_gen = 0; b->reg = 0;
    vk_full_barrier(vk_upload_cmd());
    // depth / stencil start zeroed as new images do (create_image_): cleared by the first pass that renders to it
    b->zero_on_write = (b->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) != 0 || (b->aspect == VK_IMAGE_ASPECT_STENCIL_BIT && b->format != VK_FORMAT_S8_UINT);
    return b;
}
static void recycle_flush(void) {
    pthread_mutex_lock(&g_recycle_lock);
    int n = g_nrecycle;
    g_nrecycle = 0;
    g_recycle_bytes = 0;
    pthread_mutex_unlock(&g_recycle_lock);
    for (int i = 0; i < n; i++) free_backend(g_recycle[i].b);
}

bool tex_watched(uint64_t addr);
static void residency_restore(NvnTexture *t, TexBackend *b);
static TexBackend *tex_backend(NvnTexture *t) {
    TexBackend *b = t->backend;
    if (b) {
        b->last_use = vk_frame_counter();
        if (b->evicted) residency_restore(t, b);
        if (b->scalable && (b->sx != kind_sx(b->scale_kind) || b->sy != kind_sy(b->scale_kind))) {  // output resolution changed: reallocate
            destroy_image(b);
            create_image(t, b);
        }
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
        create_image(t, b);
    }
    t->backend = b;
    b->last_use = vk_frame_counter();
    live_add(t, b);
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
static NvnTexture *alias_writer(NvnTexture *t) {
    static int off = -1;
    if (off < 0) off = getenv("LARECOMP_VK_NO_ALIAS") != NULL;
    if (off || !t->addr) return NULL;
    uint32_t s = alias_slot(t->addr);
    for (int p = 0; p < 32; p++, s = (s + 1) & (ALIAS_TAB - 1)) {
        if (g_alias[s].addr != t->addr) { if (!g_alias[s].addr) return NULL; continue; }
        NvnTexture *w = g_alias[s].t;
        if (!w || w == t || !w->backend || !((TexBackend *)w->backend)->gpu_owned) return NULL;
        const FormatInfo *fa = t->layout.fmt, *fb = w->layout.fmt;
        if (w->width != t->width || w->height != t->height || w->target != t->target || w->samples != t->samples || !fa || !fb ||
            fa->bpb != fb->bpb || fa->bw != fb->bw || fa->bh != fb->bh ||
            renderer_format_is_depth(w->format) != renderer_format_is_depth(t->format))
            return NULL;
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

bool renderer_texture_gpu_owned(NvnTexture *t) {
    const TexBackend *b = t ? (const TexBackend *)t->backend : NULL;
    return b && b->gpu_owned;
}
// another object's live rendering at t's storage (same size and texel layout) would stand in when t is sampled
bool renderer_texture_has_writer(NvnTexture *t) { return alias_writer(t) != NULL; }
NvnTexture *renderer_texture_alias_writer(NvnTexture *t) { return alias_writer(t); }   // (exec.c resolve_feedback)

static void wb_forget(NvnTexture *t);
void backend_texture_destroyed(NvnTexture *t) {
    extern void vk_preupload_forget(NvnTexture *t);
    vk_preupload_forget(t);   // (preupload.c's queue: before the NvnTexture is freed)
    alias_forget(t);
    wb_forget(t);
    TexBackend *b = t->backend;
    extern bool g_debug_frame;
    if ((g_debug_frame || tex_watched(t->addr)) && b)
        LOG("[tex] finalize %p addr %llx %ux%u f%#x image %p\n", (void *)t, (unsigned long long)t->addr, t->width, t->height, t->format,
            (void *)b->image);
    if (b) {
        live_remove(b);
        if (!recycle_put(t, b)) free_backend(b);
    }
    t->backend = NULL;
}

// Reboot (device.c backend_reset; GPU idle): every game texture and its views go.
void vk_textures_reset(void) {
    int n = g_nlive;
    while (g_nlive > 0) backend_texture_destroyed(g_live[g_nlive - 1]);
    recycle_flush();
    vk_collect_dead();
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
    if (budget_override) budget = budget_override;
    uint64_t headroom = budget / 16 > (256ull << 20) ? budget / 16 : 256ull << 20;
    uint64_t need = budget && usage + headroom > budget ? usage - (budget > 2 * headroom ? budget - 2 * headroom : 0) : 0, freed = 0;
    uint64_t cneed = 0, cavail = 0;
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
    }
#endif
    if (cneed > need) need = cneed;
    res_pressure = need;
    if (need) res_need_ns = now;
    res_pressure_commit = cneed != 0;
    if (!need) return;
    // 1. the recycle pool, oldest first
    int npool = 0;
    while (freed < need) {
        pthread_mutex_lock(&g_recycle_lock);
        TexBackend *old = g_nrecycle ? g_recycle[0].b : NULL;
        if (old) {
            memmove(g_recycle, g_recycle + 1, (size_t)(g_nrecycle - 1) * sizeof *g_recycle);
            g_nrecycle--;
            g_recycle_bytes -= vma_shim_size(old->mem);
        }
        pthread_mutex_unlock(&g_recycle_lock);
        if (!old) break;
        freed += vma_shim_size(old->mem);
        free_backend(old);
        npool++;
    }
    // 2. idle sampled textures, least recently used first (at most 512 MB / 512 textures a round: one view-table scan)
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
    int nev = 0;
    uint64_t evb = 0;
    for (int i = 0; i < nc && freed < need && nev < 512 && evb < (512ull << 20); i++) {
        TexBackend *b = cand[i]->backend;
        uint64_t sz = vma_shim_size(b->mem);
        b->evicted = true;   // (marks it for the view scan below)
        freed += sz; evb += sz; nev++;
    }
    if (nev) {
        views_forget_evicted();
        for (int i = 0; i < nev; i++) {
            TexBackend *b = cand[i]->backend;
            defer((Dead){.img = b->image, .mem = b->mem});
            b->image = VK_NULL_HANDLE; b->mem = VK_NULL_HANDLE;
            b->uploaded = false; b->data_hash = b->sample_hash = 0;
            b->checked_frame = b->full_frame = b->full_seq = 0; b->gpuw_skips = 0;
        }
        res_evicted_n += nev; res_evicted_bytes += evb;
    }
    res_pressure = freed < need ? need - freed : 0;   // (what this round could not free: the mirrors' share, vk_mirror_release_tick)
    if (npool || nev) {
        res_rounds++;
        cool_until = now + 1000000000ull;
        if (res_rounds <= 50 || res_rounds % 50 == 0)
            LOG("[vram] residency: heap %d usage %llu MB of budget %llu MB, commit headroom %llu MB%s: freed %d pooled images, evicted %d "
                "idle textures (%llu MB, idle >= %llu frames); since start evicted %llu (%llu MB), re-created on use %llu (%llu MB)\n", h,
                (unsigned long long)(usage >> 20), (unsigned long long)(budget >> 20), (unsigned long long)(cavail >> 20),
                cneed ? " (low)" : "", npool, nev, (unsigned long long)(evb >> 20), (unsigned long long)idle,
                (unsigned long long)res_evicted_n, (unsigned long long)(res_evicted_bytes >> 20), (unsigned long long)res_restored_n,
                (unsigned long long)(res_restored_bytes >> 20));
    } else {
        static uint64_t nlog_short;
        if (nlog_short++ % 40 == 0)
            LOG("[vram] residency: heap %d usage %llu MB of budget %llu MB, commit headroom %llu MB%s, and nothing idle to evict "
                "(rendered / recent images)\n", h, (unsigned long long)(usage >> 20), (unsigned long long)(budget >> 20),
                (unsigned long long)(cavail >> 20), cneed ? " (low)" : "");
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
    VkCommandBuffer cb = vk_upload_cmd();
    vk_dl_note_upload((void *)b->image, rg.imageExtent.width, rg.imageExtent.height, rg.imageSubresource.mipLevel);
                vkCmdCopyBufferToImage(cb, s.buf, b->image, VK_IMAGE_LAYOUT_GENERAL, 1, &rg);
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
static uint64_t up_tk_hash, up_ns_alloc, up_ns_decode;   // (LARECOMP_UPLOAD_LOG, below; the hash in TSC ticks)
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
typedef struct { VkBuffer buf; VkBufferImageCopy rg; int chk; } AgCopy;
typedef struct { AgCopy *c; uint32_t n, cap; bool bound, fenced; uint64_t used; VkDescriptorSet set; } AgBatch;
// The texels go to a 64 MB device-local scratch, copied into the image whenever it fills (dispatches, barrier,
// copies, barrier, ...): a 121-layer 1024x1024 array is 484 MB of RGBA8, which in staging spans grew the transient
// ring by as much host-visible memory in one frame. Levels over 64 MB (or no scratch) use staging spans.
#define AG_SCR (64ull << 20)
static VkBuffer ag_scr;
static VmaAllocation ag_scr_mem;
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
    VK_CHECK(vkCreateComputePipelines(g_vk.dev, VK_NULL_HANDLE, 1, &ci, NULL, &ag_pipe));
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

// One ASTC level on the GPU: false = not taken (the CPU decodes it).
static bool ag_level(VkCommandBuffer cb, AgBatch *bt, const uint8_t *src, bool linear, uint32_t stride, const LevelLayout *l,
                     const TexBackend *b, uint32_t lv, uint32_t layer) {
    uint32_t nblk = l->wb * l->hb;
    if (l->block_depth_log2 || (linear && (stride & 3)) || (nblk + 63) / 64 > 65535) return false;
    uint32_t rows = 8u << l->block_height_log2;  // block-linear: whole GOB blocks, rows of (wb * 16 + 63) / 64 GOBs
    uint64_t n = linear ? (uint64_t)(l->hb - 1) * stride + l->wb * 16 : (uint64_t)((l->wb * 16 + 63) / 64) * 64 * ((l->hb + rows - 1) / rows * rows);
    uint64_t size = (uint64_t)l->w * l->h * 4;
    bool scr = ag_scr && size <= AG_SCR;
    if (scr && bt->used + size > AG_SCR) ag_copy_out(cb, bt, b);
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
    // one copy command per run of levels in the same buffer (a 121-layer array was 1331 commands, ~80 ms recording)
    VkBufferImageCopy rg[256];
    for (uint32_t i = 0, n = 0; i < bt->n; i++) {
        AgCopy *c = &bt->c[i];
        vk_dl_note_upload((void *)b->image, c->rg.imageExtent.width, c->rg.imageExtent.height, c->rg.imageSubresource.mipLevel);
        rg[n++] = c->rg;
        if (n == 256 || i + 1 == bt->n || bt->c[i + 1].buf != c->buf) {
            vkCmdCopyBufferToImage(cb, c->buf, b->image, VK_IMAGE_LAYOUT_GENERAL, n, rg);
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
    bt->used = 0;
    bt->fenced = false;
}

// After a texture's levels.
static void ag_flush(VkCommandBuffer cb, AgBatch *bt, const TexBackend *b) {
    ag_copy_out(cb, bt, b);
    free(bt->c);
    *bt = (AgBatch){0};
}

// LARECOMP_UPLOAD_LOG=1: every texture upload over 20 ms, split into its parts.
static void upload_texture_(NvnTexture *t, TexBackend *b);
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
    if (b->uploaded && !b->dynamic && !pre_first && ((frame + ((uintptr_t)b >> 6)) & 7) && !cpu_dirty) return;
    if (pre_first) {
        extern void vk_preupload_used(void);
        b->preuploaded = false;
        vk_preupload_used();
    }
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
                else if (ch > 0) full = true;
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
    if (b->uploaded && !cpu_dirty && !always_full && (frame - b->full_frame < (b->dynamic ? 60u : 120u) || pre_first)) {
        extern int heap_ww_changed(uint64_t addr, uint64_t size, uint64_t *seen);
        uint64_t w0 = __rdtsc();
        int ch = heap_ww_changed(t->addr, total, &b->ww_seen);
        cum_up_ww_tk += __rdtsc() - w0;
        if (ch == 0 && !(b->tp > 0 && b->tp_gen != texpatch_generation())) { prof_tex_ww_clean++; return; }
        if (ch > 0) full = true;   // written: hash it now (dynamic or not)
        if (ch < 0 && pre_first) full = true;  // (no watch here: a full check, as the first upload it stands for)
    }
    if (!full && frame - b->full_frame >= 120 && full_hash_budget >= total) { full = true; full_hash_budget -= total; }
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
    if (b->uploaded && h != b->data_hash && !pre_first) b->dynamic = true;   // (texels that came after a pre-upload: its data)
    if (b->uploaded && h != b->data_hash && pre_first) {
        extern void vk_preupload_changed(uint64_t bytes);
        vk_preupload_changed(total);   // (preupload.c's account)
    }
    if (b->tp > 0) b->tp_gen = texpatch_generation();
    b->data_hash = h;
    b->uploaded = true;
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
    VkCommandBuffer cb = vk_upload_cmd();
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
                        VkBufferImageCopy rg = {.bufferOffset = s.off, .bufferRowLength = l.w, .bufferImageHeight = l.h,
                                                .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, lv, layer, 1},
                                                .imageExtent = {l.w < level_w(b, lv) ? l.w : level_w(b, lv), l.h < level_h(b, lv) ? l.h : level_h(b, lv), 1}};
                        vk_dl_note_upload((void *)b->image, rg.imageExtent.width, rg.imageExtent.height, rg.imageSubresource.mipLevel);
                        vkCmdCopyBufferToImage(cb, s.buf, b->image, VK_IMAGE_LAYOUT_GENERAL, 1, &rg);
                        continue;
                    }
                    uint8_t *blocks = malloc((uint64_t)l.wb * l.hb * 16);
                    if (linear) for (uint32_t y = 0; y < l.hb; y++) memcpy(blocks + (uint64_t)y * l.wb * 16, src + (uint64_t)y * t->stride, l.wb * 16);
                    else tegra_deswizzle(src, blocks, l.wb, l.hb, 1, 16, l.block_height_log2, l.block_depth_log2);
                    astc_decode_par(blocks, l.w, l.h, b->astc_bw, b->astc_bh, b->astc_srgb, s.ptr);
                    free(blocks);
                    VkBufferImageCopy rg = {.bufferOffset = s.off, .bufferRowLength = l.w, .bufferImageHeight = l.h,
                                            .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, lv, layer, 1},
                                            .imageExtent = {l.w < level_w(b, lv) ? l.w : level_w(b, lv), l.h < level_h(b, lv) ? l.h : level_h(b, lv), 1}};
                    vk_dl_note_upload((void *)b->image, rg.imageExtent.width, rg.imageExtent.height, rg.imageSubresource.mipLevel);
                    vkCmdCopyBufferToImage(cb, s.buf, b->image, VK_IMAGE_LAYOUT_GENERAL, 1, &rg);
                    continue;
                }
                uint8_t *blocks = malloc((uint64_t)l.wb * l.hb * 16);
                if (linear) for (uint32_t y = 0; y < l.hb; y++) memcpy(blocks + (uint64_t)y * l.wb * 16, src + (uint64_t)y * t->stride, l.wb * 16);
                else if (!deswizzle_par(src, blocks, l.wb, l.hb, 16, l.block_height_log2, l.block_depth_log2))
                    tegra_deswizzle(src, blocks, l.wb, l.hb, 1, 16, l.block_height_log2, l.block_depth_log2);
                if (b->tp > 0 && layer == 0) texpatch_apply(b->tp - 1, lv, blocks, l.w, l.h, l.wb * 16);
                vk_up_kind = 4; VkSpan s = vk_transient((uint64_t)l.w * l.h * 4, 16); vk_up_kind = 0;
                astc_decode_par(blocks, l.w, l.h, b->astc_bw, b->astc_bh, b->astc_srgb, s.ptr);
                free(blocks);
                VkBufferImageCopy rg = {.bufferOffset = s.off, .bufferRowLength = l.w, .bufferImageHeight = l.h,
                                        .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, lv, layer, 1},
                                        .imageExtent = {l.w < level_w(b, lv) ? l.w : level_w(b, lv), l.h < level_h(b, lv) ? l.h : level_h(b, lv), 1}};
                vk_dl_note_upload((void *)b->image, rg.imageExtent.width, rg.imageExtent.height, rg.imageSubresource.mipLevel);
                vkCmdCopyBufferToImage(cb, s.buf, b->image, VK_IMAGE_LAYOUT_GENERAL, 1, &rg);
                continue;
            }
            uint32_t row = l.wb * fi->bpb;
            uint64_t sz = (uint64_t)row * l.hb * l.d;
            bool expand = mf.expand_rgb && fi->bpb == 3;
            uint32_t out_row = expand ? l.wb * 4 : row;
            vk_up_kind = 4; VkSpan s = vk_transient((uint64_t)out_row * l.hb * l.d, 16); vk_up_kind = 0;
            uint8_t *lin = expand ? malloc(sz) : s.ptr;
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
    cum_up_copy_tk += __rdtsc() - uc0;
    cum_up_sent++;
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
static void wb_forget(NvnTexture *t) {   // (queued outside an execute, e.g. by a present, then finalized)
    for (int i = 0; i < g_wb_npending; i++)
        if (g_wb_pending[i] == t) { g_wb_pending[i] = g_wb_pending[--g_wb_npending]; i--; }
}
static int wb_on(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("LARECOMP_VK_WRITEBACK"); on = !(e && *e == '0'); }
    return on;
}
static bool wb_candidate(NvnTexture *t, TexBackend *b) {
    const FormatInfo *fi = t->layout.fmt;
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
    if (b->wb_state == 2 && b->wb_frame != f1) { b->wb_state = 3; return; }  // rendered again in a later frame
    if (!wb_candidate(t, b)) { b->wb_state = 3; return; }
    if (g_wb_npending == (int)(sizeof g_wb_pending / sizeof *g_wb_pending)) return;   // (full: not mid-pass; a later render queues it)
    b->wb_state = 1;
    b->wb_frame = f1;
    g_wb_pending[g_wb_npending++] = t;
}
void vk_writeback_flush(void) {
    if (!g_wb_npending) return;
    extern void renderer_flush(void);
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
    vk_end_pass();
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);
    VkSpan out = vk_readback(need);
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
    renderer_flush();
    for (int i = 0; i < g_wb_npending; i++) {
        NvnTexture *t = g_wb_pending[i];
        TexBackend *b = t->backend;
        const FormatInfo *fi = t->layout.fmt;
        uint32_t levels = t->layout.levels < b->levels ? t->layout.levels : b->levels;
        bool linear = nvn_flags_linear(t->flags);
        uint64_t o = off[i];
        for (uint32_t lv = 0; lv < levels; lv++) {
            LevelLayout l = tegra_level(&t->layout, lv);
            const uint8_t *src = (const uint8_t *)out.ptr + o;
            uint8_t *dst = (uint8_t *)(uintptr_t)t->addr + l.offset;
            uint32_t row = l.wb * fi->bpb;
            if (linear) for (uint32_t y = 0; y < l.hb; y++) memcpy(dst + (uint64_t)y * t->stride, src + (uint64_t)y * row, row);
            else tegra_swizzle(src, dst, l.wb, l.hb, l.d, fi->bpb, l.block_height_log2, l.block_depth_log2);
            o += ((uint64_t)row * l.hb * l.d + 15) & ~15ull;
        }
        b->wb_state = 2;
        g_wb_bytes += o - off[i];
        if (tex_watched(t->addr) || g_wb_n < 4 || !(g_wb_flushes % 64))
            LOG("[wb] %p addr %llx %ux%u f%#x flags %#x lv %u: %llu bytes written back (frame %llu)\n", (void *)t, (unsigned long long)t->addr,
                t->width, t->height, t->format, t->flags, levels, (unsigned long long)(o - off[i]), (unsigned long long)vk_frame_counter());
        g_wb_n++;
    }
    g_wb_ns += SDL_GetTicksNS() - t0;
    g_wb_flushes++;
    if (g_wb_flushes <= 8 || !(g_wb_flushes % 64))
        LOG("[wb] flush %llu: %d textures, %llu KB (total %llu textures, %llu MB, %.1f ms)\n", (unsigned long long)g_wb_flushes,
            g_wb_npending, (unsigned long long)(need >> 10), (unsigned long long)g_wb_n, (unsigned long long)(g_wb_bytes >> 20), g_wb_ns / 1e6);
    g_wb_npending = 0;
}

TexBackend *vk_tex(NvnTexture *t, bool for_render) {
    if (for_render) rt_first_note(t, "render");
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
    }
    TexBackend *b = tex_backend(t);
    if (b->pending_rgba) upload_pending(b);
    if (for_render) { b->gpu_owned = true; b->twin_valid = false; alias_note_writer(t); wb_note_render(t, b); }   // (twin: refreshed after the pass)
    else if (!b->gpu_owned) {
        extern uint64_t prof_up_ns;
        uint64_t t0 = prof_up_ns != ~0ull ? SDL_GetTicksNS() : 0;
        upload_texture(t, b);
        if (prof_up_ns != ~0ull) prof_up_ns += SDL_GetTicksNS() - t0;
    }
    return b;
}

void renderer_mark_gpu_owned(NvnTexture *t) { TexBackend *b = tex_backend(t); b->gpu_owned = true; b->twin_valid = false; }   // (copy / clear destinations)
bool renderer_is_gpu_owned(NvnTexture *t) { return tex_backend(t)->gpu_owned; }
void backend_pool_written(uint64_t addr, uint64_t size) { (void)addr; (void)size; }
// nvnTextureWriteTexels (objects.c): the next use re-hashes the whole texture (a small region can miss the sampled
// hash, and unchanged textures are only checked every 8th frame).
void backend_texture_cpu_written(NvnTexture *t) { if (t->backend) ((TexBackend *)t->backend)->cpu_dirty = true; }

// Host-produced texel data (decoded video, any thread): applied on the render thread at the texture's next use.
static _Atomic unsigned long long video_frames;   // (hle_vi.cpp: a video playing is not a load on screen)
unsigned long long renderer_video_frames(void) { return video_frames; }
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
}

// ================================================================== views
#define VIEW_TAB 16384
typedef struct { TexBackend *b; uint64_t k1, k2; VkImageView view; } ViewEnt;
static ViewEnt view_tab[VIEW_TAB];

// Views may still be referenced by submitted or pending work: destroyed once that has completed.
void vk_views_forget(TexBackend *b) { views_forget(b); }
uint32_t vk_view_gen;  // bumped whenever a cached view may be destroyed (exec.c's per-frame texture memo)
static void views_forget(TexBackend *b) {
    vk_view_gen++;
    for (int i = 0; i < VIEW_TAB; i++)
        if (view_tab[i].b == b) { defer((Dead){.view = view_tab[i].view}); view_tab[i].b = NULL; view_tab[i].view = VK_NULL_HANDLE; }
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
            // decoded ASTC (an RGBA8 image, mutable): an sRGB / UNORM view of it keeps that reading of the texels
            bool vs = v->format >= 0x87;
            if (vs != b->astc_srgb) fmt = vs ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
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
    static int nlog;
    if (nlog++ < 30) LOG("[tex] type mismatch: want %d have %d (%ux%ux%u target %u fmt %#x layers %u)\n", want, have, t->width,
                         t->height, t->depth, t->target, t->format, b->layers);
    return vk_dummy_view(want_textype);
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
    VK_CHECK(vmaCreateImage(g_vk.vma, &ci, &ai, &b->image, &b->mem, NULL));
    VkCommandBuffer cb = vk_upload_cmd();
    vk_image_init_layout(cb, b->image, VK_IMAGE_ASPECT_COLOR_BIT);
    VkClearColorValue zero = {{0, 0, 0, 0}};
    VkImageSubresourceRange all = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, cube ? 6 : 1};
    vkCmdClearColorImage(cb, b->image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &all);
    b->format = VK_FORMAT_R8G8B8A8_UNORM;
    b->levels = 1;
    b->layers = cube ? 6 : 1;
    views[textype] = vk_image_view(b, vt, b->format, 0, 1, 0, cube ? 6 : 1, IDENTITY, VK_IMAGE_ASPECT_COLOR_BIT);
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
 * are read once: samplers made before a change keep theirs.*/
static float pc_aniso(void) {
    static float v = -1;
    if (v < 0) { const char *e = getenv("TOTK_ANISO"); v = e ? (float)atof(e) : 16; if (v > 16) v = 16; if (v > 1) LOG("[smp] TOTK_ANISO=%.0f\n", (double)v); }
    return v;
}
static float pc_lod_bias(void) {
    static int read; static float v;
    if (!read) { const char *e = getenv("TOTK_LOD_BIAS"); v = e ? (float)atof(e) : 0; read = 1; if (v != 0) LOG("[smp] TOTK_LOD_BIAS=%.2f\n", (double)v); }
    return v;
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

VkSampler vk_sampler(NvnSamplerDesc *d) {
    if (d->backend) return (VkSampler)d->backend;
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
        if (an > 1 && si.minFilter == VK_FILTER_LINEAR && !d->compare_mode && (!si.anisotropyEnable || si.maxAnisotropy < an)) { si.anisotropyEnable = VK_TRUE; si.maxAnisotropy = an; }
        si.mipLodBias += pc_lod_bias();
    }
    // (only where the device enabled anisotropy, and up to its limit: VUID-VkSamplerCreateInfo-anisotropyEnable-01070 / 01071;
    // every desktop GPU has 16x, so nothing changes there)
    if (si.anisotropyEnable && !g_vk.sampler_aniso) si.anisotropyEnable = VK_FALSE, si.maxAnisotropy = 1;
    if (si.anisotropyEnable && si.maxAnisotropy > g_vk.props.limits.maxSamplerAnisotropy) si.maxAnisotropy = g_vk.props.limits.maxSamplerAnisotropy;
    if (d->compare_mode) {
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
    VkSampler s = samp_get(&si, &cb);
    d->backend = (void *)s;
    {   // measurement (blurry ground, 2026-10-14): the guest's LOD settings per sampler, first 64
        static int nlog;
        if (nlog++ < 64)
            LOG("[smp] %p min %u mag %u wrap %u/%u/%u lod %.3f..%.3f bias %.3f aniso %.0f cmp %u -> vk minLod %.3f maxLod %.3f\n",
                (void *)d, d->min_filter, d->mag_filter, d->wrap[0], d->wrap[1], d->wrap[2], d->lod_min, d->lod_max, d->lod_bias,
                (double)d->max_aniso, d->compare_mode, si.minLod, si.maxLod);
    }
    return s;
}

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
