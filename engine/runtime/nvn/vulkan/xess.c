// Intel XeSS super resolution on any Vulkan GPU (amd-compat, 2026-11-03, options only, default off). XeSS SDK v3.0.2: its
// C API headers are in the tree (engine/third_party/xess/, MIT); the runtime libxess.dll (Intel Simplified Software
// License: redistributed in binary form only, unmodified, with the notice: licenses/XeSS.txt at the public root) is looked
// up beside totk.exe, then in TOTK_XESS_DIR (<work>/engine/xess: Setup unpacks our release asset setup-xess.zip there).
// XeSS frame generation and XeLL are DirectX 12 only, so only the upscaler is here. The backend behind vk.h's
// VkTemporalInputs, as ffx.c: ngx.c makes the inputs from the game's upscale pass and picks NGX, FidelityFX or this;
// nothing here reads a setting. device.c enables what XeSS asks for (instance / device extensions, features) at the start
// whenever the DLL is there, so the Upscaler row can turn it on live.
#include "vk.h"

#ifdef _WIN32
#include <windows.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL3/SDL.h>

#include "../../../third_party/xess/include/xess/xess_vk.h"

typedef xess_result_t (*PfnGetVersion)(xess_version_t *);
typedef xess_result_t (*PfnInstExts)(uint32_t *, const char *const **, uint32_t *);
typedef xess_result_t (*PfnDevExts)(VkInstance, VkPhysicalDevice, uint32_t *, const char *const **);
typedef xess_result_t (*PfnDevFeatures)(VkInstance, VkPhysicalDevice, void **);
typedef xess_result_t (*PfnCreate)(VkInstance, VkPhysicalDevice, VkDevice, xess_context_handle_t *);
typedef xess_result_t (*PfnBuild)(xess_context_handle_t, VkPipelineCache, bool, uint32_t);
typedef xess_result_t (*PfnInit)(xess_context_handle_t, const xess_vk_init_params_t *);
typedef xess_result_t (*PfnExecute)(xess_context_handle_t, VkCommandBuffer, const xess_vk_execute_params_t *);
typedef xess_result_t (*PfnDestroy)(xess_context_handle_t);
typedef xess_result_t (*PfnOptimal)(xess_context_handle_t, const xess_2d_t *, xess_quality_settings_t, xess_2d_t *,
                                    xess_2d_t *, xess_2d_t *);
typedef xess_result_t (*PfnScale)(xess_context_handle_t, float, float);
static PfnGetVersion p_version;
static PfnInstExts p_inst_exts;
static PfnDevExts p_dev_exts;
static PfnDevFeatures p_dev_features;
static PfnCreate p_create;
static PfnBuild p_build;
static PfnInit p_init;
static PfnExecute p_execute;
static PfnDestroy p_destroy;
static PfnOptimal p_optimal;
static PfnScale p_vel_scale;
static int g_load = -1;   // -1 not tried, 0 unavailable, 1 loaded
static bool g_dev_ok;     // (device.c enabled every extension / feature XeSS asked for)

static HMODULE load_from(const wchar_t *dir) {
    wchar_t p[MAX_PATH + 32];
    _snwprintf(p, MAX_PATH + 31, L"%ls\\libxess.dll", dir);
    p[MAX_PATH + 31] = 0;
    return LoadLibraryExW(p, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
}

// The DLL, once: beside totk.exe, then TOTK_XESS_DIR. Called before the instance exists (device.c's extension lists).
static bool xess_load(void) {
    if (g_load >= 0) return g_load == 1;
    g_load = 0;
    wchar_t exe[MAX_PATH], dir[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, exe, MAX_PATH);
    while (n && exe[n - 1] != L'\\' && exe[n - 1] != L'/') n--;
    exe[n ? n - 1 : 0] = 0;
    HMODULE m = load_from(exe);
    const wchar_t *d = _wgetenv(L"TOTK_XESS_DIR");   /* (wide: getenv's ANSI bytes read as UTF-8 named another folder, #94) */
    if (!m && d && *d && wcslen(d) < MAX_PATH) { wcscpy(dir, d); m = load_from(dir); }
    if (!m) { LOG("[xess] libxess.dll not found (beside totk.exe or TOTK_XESS_DIR): XeSS unavailable\n"); return false; }
#define XQ(var, T, name) var = (T)(void *)GetProcAddress(m, name)
    XQ(p_version, PfnGetVersion, "xessGetVersion");
    XQ(p_inst_exts, PfnInstExts, "xessVKGetRequiredInstanceExtensions");
    XQ(p_dev_exts, PfnDevExts, "xessVKGetRequiredDeviceExtensions");
    XQ(p_dev_features, PfnDevFeatures, "xessVKGetRequiredDeviceFeatures");
    XQ(p_create, PfnCreate, "xessVKCreateContext");
    XQ(p_build, PfnBuild, "xessVKBuildPipelines");
    XQ(p_init, PfnInit, "xessVKInit");
    XQ(p_execute, PfnExecute, "xessVKExecute");
    XQ(p_destroy, PfnDestroy, "xessDestroyContext");
    XQ(p_optimal, PfnOptimal, "xessGetOptimalInputResolution");
    XQ(p_vel_scale, PfnScale, "xessSetVelocityScale");
#undef XQ
    if (!p_version || !p_inst_exts || !p_dev_exts || !p_dev_features || !p_create || !p_build || !p_init || !p_execute ||
        !p_destroy || !p_optimal || !p_vel_scale) {
        LOG("[xess] libxess.dll lacks the XeSS Vulkan entry points: XeSS unavailable\n");
        return false;
    }
    xess_version_t v = {0};
    p_version(&v);
    g_load = 1;
    LOG("[xess] XeSS runtime loaded (libxess.dll %u.%u.%u)\n", (unsigned)v.major, (unsigned)v.minor, (unsigned)v.patch);
    return true;
}

// ---- device.c, at the start (before the instance / device are made)
// Adds the instance extensions XeSS needs to ext[0..n) (cap entries at most), skipping those already there.
uint32_t vk_xess_instance_exts(const char **ext, uint32_t n, uint32_t cap) {
    if (!xess_load()) return n;
    uint32_t cnt = 0, minver = 0;
    const char *const *names = NULL;
    if (p_inst_exts(&cnt, &names, &minver) != XESS_RESULT_SUCCESS) { g_load = 0; return n; }
    // (only names the loader offers: an unknown instance extension would fail vkCreateInstance = no game; without one
    // XeSS is unavailable instead)
    uint32_t na = 0;
    vkEnumerateInstanceExtensionProperties(NULL, &na, NULL);
    VkExtensionProperties *av = calloc(na ? na : 1, sizeof *av);
    if (av) vkEnumerateInstanceExtensionProperties(NULL, &na, av);
    for (uint32_t i = 0; i < cnt && names; i++) {
        bool have = false, offered = false;
        for (uint32_t j = 0; j < n && !have; j++) have = !strcmp(ext[j], names[i]);
        for (uint32_t j = 0; av && j < na && !offered; j++) offered = !strcmp(av[j].extensionName, names[i]);
        if (have) continue;
        if (!offered || n >= cap) {
            LOG("[xess] instance extension %s %s: XeSS unavailable\n", names[i], offered ? "does not fit the list" : "not offered");
            g_load = 0;
            break;
        }
        ext[n++] = names[i];
    }
    free(av);
    if (minver > VK_API_VERSION_1_3) LOG("[xess] XeSS asks for Vulkan %u.%u: XeSS may be unavailable\n",
                                         VK_API_VERSION_MAJOR(minver), VK_API_VERSION_MINOR(minver));
    return n;
}
// The device extensions XeSS needs (names owned by the DLL); 0 when unavailable. device.c enables them only if the GPU has
// all of them and the features vk_xess_features checks (vk_xess_device_ready tells this file).
uint32_t vk_xess_device_exts(const char *const **names) {
    *names = NULL;
    if (!xess_load()) return 0;
    uint32_t cnt = 0;
    if (p_dev_exts(g_vk.inst, g_vk.phys, &cnt, names) != XESS_RESULT_SUCCESS) return 0;
    return cnt;
}
// The features XeSS asks for, read from its own chain (xessVKGetRequiredDeviceFeatures with no chain of ours: it makes one
// we only read), as flags: bit 0 shaderStorageImageWriteWithoutFormat, 1 mutableDescriptorType, 2 shaderInt8,
// 3 shaderIntegerDotProduct, bit 31 a structure we do not know (XeSS then stays off: device.c could not enable it).
uint32_t vk_xess_features(void) {
    if (!xess_load()) return 0;
    void *chain = NULL;
    if (p_dev_features(g_vk.inst, g_vk.phys, &chain) != XESS_RESULT_SUCCESS) return 1u << 31;
    uint32_t want = 0;
    for (const VkBaseOutStructure *s = chain; s; s = s->pNext) {
        switch (s->sType) {
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2: {
            const VkPhysicalDeviceFeatures *f = &((const VkPhysicalDeviceFeatures2 *)s)->features;
            if (f->shaderStorageImageWriteWithoutFormat) want |= 1;
            VkPhysicalDeviceFeatures known = {.shaderStorageImageWriteWithoutFormat = f->shaderStorageImageWriteWithoutFormat};
            if (memcmp(f, &known, sizeof known)) want |= 1u << 31;   // (another core feature: not one we enable for it)
            break;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MUTABLE_DESCRIPTOR_TYPE_FEATURES_EXT:
            if (((const VkPhysicalDeviceMutableDescriptorTypeFeaturesEXT *)s)->mutableDescriptorType) want |= 2;
            break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES:
            if (((const VkPhysicalDeviceVulkan12Features *)s)->shaderInt8) want |= 4;
            break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES:
            if (((const VkPhysicalDeviceShaderFloat16Int8Features *)s)->shaderInt8) want |= 4;
            break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES:
            if (((const VkPhysicalDeviceVulkan13Features *)s)->shaderIntegerDotProduct) want |= 8;
            break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_INTEGER_DOT_PRODUCT_FEATURES:
            if (((const VkPhysicalDeviceShaderIntegerDotProductFeatures *)s)->shaderIntegerDotProduct) want |= 8;
            break;
        default:
            LOG("[xess] XeSS asks for a device feature structure we do not enable (sType %d): XeSS unavailable\n", (int)s->sType);
            want |= 1u << 31;
        }
    }
    return want;
}
void vk_xess_device_ready(bool ok) {   // (device.c: everything asked for is enabled on the device)
    g_dev_ok = ok && g_load == 1;
    if (g_load == 1) LOG("[xess] %s\n", g_dev_ok ? "the device has what XeSS needs: the Upscaler row can choose it"
                                                 : "this GPU lacks a Vulkan extension / feature XeSS needs: XeSS unavailable");
}

// ---- super resolution
static xess_context_handle_t g_ctx;
static uint32_t g_rw, g_rh, g_ow, g_oh, g_flags;
static xess_2d_t g_min, g_max;   // (the preset's input range: a scene size inside it is dynamic resolution, no re-init)
static xess_quality_settings_t g_q;
static int g_fails;   // (failed creates / executes: after 4, 'not ready', and ngx.c's picker stops asking)
static float g_jsx = 1.f, g_jsy = 1.f;   // TOTK_XESS_JSIGN=<sx>,<sy>: sign of the jitter given to XeSS (A/B; hdr: a wrong
                                         // sign shows only as blur, never as an error)
bool vk_xess_sr_ready(void) { return xess_load() && g_dev_ok && g_fails <= 3; }

static void ctx_wait(void) { vk_wait_seq(vk_pending_seq() - 1); }   // (its last execute done: as ffx.c's)
void vk_xess_release(void) {
    if (!g_ctx) return;
    ctx_wait();
    p_destroy(g_ctx);
    g_ctx = NULL;
    g_rw = g_rh = g_ow = g_oh = 0;
    g_min = g_max = (xess_2d_t){0, 0};
}

// The quality preset whose input range holds the scene (out / render): XeSS 2's factors from Native AA (1.0) to Ultra
// Performance (3.0); xessGetOptimalInputResolution gives each preset's min / max input, so the Render resolution row's
// size is used as it is (as FSR's dynamic resolution) when a preset admits it, else the nearest preset's own range.
// *mn / *mx: the chosen preset's range (none admits the scene: that size alone, so only another size re-inits).
static xess_quality_settings_t pick_quality(uint32_t rw, uint32_t rh, uint32_t ow, uint32_t oh, xess_2d_t *mn, xess_2d_t *mx) {
    static const xess_quality_settings_t qs[] = {XESS_QUALITY_SETTING_AA, XESS_QUALITY_SETTING_ULTRA_QUALITY_PLUS,
        XESS_QUALITY_SETTING_ULTRA_QUALITY, XESS_QUALITY_SETTING_QUALITY, XESS_QUALITY_SETTING_BALANCED,
        XESS_QUALITY_SETTING_PERFORMANCE, XESS_QUALITY_SETTING_ULTRA_PERFORMANCE};
    static const float ratio[] = {1.0f, 1.3f, 1.5f, 1.7f, 2.0f, 2.3f, 3.0f};
    const float r = (float)ow / (float)(rw ? rw : 1);
    int best = 0;
    for (int i = 1; i < 7; i++) if (fabsf(ratio[i] - r) < fabsf(ratio[best] - r)) best = i;
    xess_2d_t out = {ow, oh}, opt;
    for (int d = 0; d < 7; d++) {   // (the nearest first, then outwards: the first whose range holds the scene)
        for (int sgn = -1; sgn <= 1; sgn += 2) {
            const int i = best + sgn * d;
            if (i < 0 || i > 6 || (d == 0 && sgn > 0)) continue;
            if (p_optimal(g_ctx, &out, qs[i], &opt, mn, mx) == XESS_RESULT_SUCCESS && rw >= mn->x && rw <= mx->x &&
                rh >= mn->y && rh <= mx->y)
                return qs[i];
        }
    }
    *mn = *mx = (xess_2d_t){rw, rh};
    return qs[best];
}
static bool in_range(uint32_t rw, uint32_t rh) {
    return rw >= g_min.x && rw <= g_max.x && rh >= g_min.y && rh <= g_max.y;
}

static bool sr_init(uint32_t rw, uint32_t rh, uint32_t ow, uint32_t oh, uint32_t flags) {
    const uint64_t t0 = SDL_GetTicksNS();
    if (!g_ctx) {
        if (p_create(g_vk.inst, g_vk.phys, g_vk.dev, &g_ctx) != XESS_RESULT_SUCCESS || !g_ctx) {
            g_ctx = NULL;
            g_fails++;
            LOG("[xess] context creation failed\n");
            return false;
        }
    } else ctx_wait();   // (re-initialised in place: the last execute done)
    xess_2d_t mn, mx;
    const xess_quality_settings_t q = pick_quality(rw, rh, ow, oh, &mn, &mx);
    if (p_build(g_ctx, g_vk.pcache, true, flags) != XESS_RESULT_SUCCESS)   // (blocking, between frames: vk_xess_sr_prepare)
        LOG("[xess] pipeline build reported a failure (initialising anyway)\n");
    xess_vk_init_params_t ip;
    memset(&ip, 0, sizeof ip);
    ip.outputResolution.x = ow, ip.outputResolution.y = oh;
    ip.qualitySetting = q;
    ip.initFlags = flags;
    ip.pipelineCache = g_vk.pcache;
    const xess_result_t r = p_init(g_ctx, &ip);
    if (r != XESS_RESULT_SUCCESS) {
        g_fails++;
        LOG("[xess] init %ux%u -> %ux%u (preset %d, flags 0x%x) failed (%d)\n", rw, rh, ow, oh, (int)q, (unsigned)flags, (int)r);
        return false;
    }
    g_rw = rw, g_rh = rh, g_ow = ow, g_oh = oh, g_flags = flags, g_q = q, g_min = mn, g_max = mx;
    LOG("[xess] XeSS SR %ux%u -> %ux%u, preset %d (input %ux%u..%ux%u), flags 0x%x, in %.1f ms\n", rw, rh, ow, oh, (int)q,
        mn.x, mn.y, mx.x, mx.y, (unsigned)flags, (SDL_GetTicksNS() - t0) / 1e6);
    return true;
}

static uint32_t sr_flags(bool inverted, bool hdr, bool mask) {
    // (low-res motion vectors in render pixels, current -> previous, not jittered: XeSS's defaults, no HIGH_RES_MV /
    // JITTERED_MV / NDC flags; the tonemapped upscale-pass input is LDR, the TOTK_DLSS_HDR scene linear HDR with XeSS's
    // own exposure, as FSR's AUTO_EXPOSURE)
    return (inverted ? XESS_INIT_FLAG_INVERTED_DEPTH : 0) | (mask ? XESS_INIT_FLAG_RESPONSIVE_PIXEL_MASK : 0) |
           (hdr ? XESS_INIT_FLAG_ENABLE_AUTOEXPOSURE : XESS_INIT_FLAG_LDR_INPUT_COLOR);
}

// At known sizes between frames (ngx.c vk_ngx_prepare, as FSR's): the context and its pipelines are made here, not on the
// first frame that upscales. The depth convention and the input kind come with the first execute; a change re-inits once.
bool vk_xess_sr_prepare(VkCommandBuffer cb, uint32_t rw, uint32_t rh, uint32_t ow, uint32_t oh) {
    (void)cb;
    if (!vk_xess_sr_ready()) return false;
    if (g_ctx && g_ow == ow && g_oh == oh && in_range(rw, rh)) return true;
    return sr_init(rw, rh, ow, oh, g_ctx ? g_flags : sr_flags(true, false, true));
}

static xess_vk_image_view_info view_of(const VkNgxImage *im, VkImageAspectFlags aspect) {
    xess_vk_image_view_info v;
    memset(&v, 0, sizeof v);
    if (!im->image) return v;
    v.imageView = im->view, v.image = im->image, v.format = im->format, v.width = im->w, v.height = im->h;
    v.subresourceRange = (VkImageSubresourceRange){aspect, 0, 1, 0, 1};
    return v;
}
static VkImageAspectFlags depth_aspect(VkFormat f) {
    return f == VK_FORMAT_D32_SFLOAT_S8_UINT || f == VK_FORMAT_D24_UNORM_S8_UINT || f == VK_FORMAT_D16_UNORM_S8_UINT
               ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_DEPTH_BIT;
}
// XeSS reads its inputs in SHADER_READ_ONLY_OPTIMAL (ours stay in GENERAL): moved there for the execute and back.
static void inputs_layout(VkCommandBuffer cb, const VkTemporalInputs *in, bool to_read) {
    VkImageMemoryBarrier b[4];
    uint32_t n = 0;
    const VkNgxImage *ims[4] = {&in->color, &in->depth, &in->mv, &in->bias};
    for (int i = 0; i < 4; i++) {
        if (!ims[i]->image) continue;
        b[n++] = (VkImageMemoryBarrier){.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT,
            .dstAccessMask = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT,
            .oldLayout = to_read ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .newLayout = to_read ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = ims[i]->image,
            .subresourceRange = {i == 1 ? depth_aspect(ims[i]->format) : VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    }
    if (n) vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, n, b);
}

bool vk_xess_sr_evaluate(VkCommandBuffer cb, const VkTemporalInputs *in) {
    if (!vk_xess_sr_ready()) return false;
    static int js = -1;
    if (js < 0) {
        const char *e = getenv("TOTK_XESS_JSIGN");
        js = e && sscanf(e, "%f,%f", &g_jsx, &g_jsy) == 2;
        LOG("[xess] jitter sign %+g %+g%s\n", g_jsx, g_jsy, js ? " (TOTK_XESS_JSIGN)" : "");
    }
    const uint32_t flags = sr_flags(in->depth_inverted, in->hdr, in->bias.image != VK_NULL_HANDLE);
    bool reset = in->reset;
    // (as FSR's context: kept while the scene fits the preset's input range at this output, so a dynamic-resolution
    // step is only another inputWidth / inputHeight; outside it, another output or input kind initialises it again)
    if (!g_ctx || in->out.w != g_ow || in->out.h != g_oh || !in_range(in->color.w, in->color.h) || flags != g_flags) {
        if (!sr_init(in->color.w, in->color.h, in->out.w, in->out.h, flags)) return false;
        reset = true;
    }
    p_vel_scale(g_ctx, in->mv_sx, in->mv_sy);   // (render pixels: 1, the same pair FSR gets)
    xess_vk_execute_params_t ep;
    memset(&ep, 0, sizeof ep);
    ep.colorTexture = view_of(&in->color, VK_IMAGE_ASPECT_COLOR_BIT);
    ep.velocityTexture = view_of(&in->mv, VK_IMAGE_ASPECT_COLOR_BIT);
    ep.depthTexture = view_of(&in->depth, VK_IMAGE_ASPECT_DEPTH_BIT);   // (the view: depth aspect alone)
    ep.responsivePixelMaskTexture = view_of(&in->bias, VK_IMAGE_ASPECT_COLOR_BIT);
    ep.outputTexture = view_of(&in->out, VK_IMAGE_ASPECT_COLOR_BIT);
    ep.jitterOffsetX = g_jsx * in->jx;   // (the geometry's shift in render pixels, +x right, +y down: XeSS's convention, as is)
    ep.jitterOffsetY = g_jsy * in->jy;
    ep.exposureScale = 1.f;
    ep.resetHistory = reset;
    ep.inputWidth = in->color.w, ep.inputHeight = in->color.h;
    inputs_layout(cb, in, true);
    const xess_result_t r = p_execute(g_ctx, cb, &ep);
    inputs_layout(cb, in, false);
    if (r != XESS_RESULT_SUCCESS) {
        if (g_fails++ < 4) LOG("[xess] execute failed (%d, %ux%u -> %ux%u)\n", (int)r, in->color.w, in->color.h, in->out.w, in->out.h);
        return false;
    }
    return true;
}

#else   // !_WIN32: no XeSS runtime here; every query answers "unavailable"
uint32_t vk_xess_instance_exts(const char **ext, uint32_t n, uint32_t cap) { (void)ext; (void)cap; return n; }
uint32_t vk_xess_device_exts(const char *const **names) { *names = NULL; return 0; }
uint32_t vk_xess_features(void) { return 0; }
void vk_xess_device_ready(bool ok) { (void)ok; }
bool vk_xess_sr_ready(void) { return false; }
void vk_xess_release(void) {}
bool vk_xess_sr_prepare(VkCommandBuffer cb, uint32_t rw, uint32_t rh, uint32_t ow, uint32_t oh) {
    (void)cb; (void)rw; (void)rh; (void)ow; (void)oh; return false;
}
bool vk_xess_sr_evaluate(VkCommandBuffer cb, const VkTemporalInputs *in) { (void)cb; (void)in; return false; }
#endif
