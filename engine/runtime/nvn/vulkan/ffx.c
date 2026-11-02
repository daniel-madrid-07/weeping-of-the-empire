// AMD FidelityFX Super Resolution 3.1 on any Vulkan GPU. FidelityFX SDK v1.1.4, MIT (engine/third_party/fidelityfx/
// LICENSE.txt): its C API headers are in the tree; the signed runtime amd_fidelityfx_vk.dll is looked up beside totk.exe,
// then in TOTK_FFX_DIR (<work>/engine/ffx: Setup unpacks our release asset setup-ffx.zip there). v2.x is not used: its
// signed DLLs come under a binary-only licence. The backend behind vk.h's VkTemporalInputs (dlss's interface): ngx.c
// makes the inputs from the game's upscale pass and picks NGX or this; nothing here reads a setting.
#include "vk.h"

#ifdef _WIN32
#include <windows.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL3/SDL.h>

#include "../../pc.h"   // (pc_fault_guard: the guarded context creations below)

#include "../../../third_party/fidelityfx/include/ffx_api.h"
#include "../../../third_party/fidelityfx/include/ffx_upscale.h"
// (the Vulkan header names its structs C++-style, without the tag: the typedefs C needs, the header itself untouched)
typedef struct FfxApiResourceDescription FfxApiResourceDescription;
typedef struct FfxApiResource FfxApiResource;
typedef struct VkQueueInfoFFXAPI VkQueueInfoFFXAPI;
#include "../../../third_party/fidelityfx/include/vk/ffx_api_vk.h"
#include "../../../third_party/fidelityfx/include/ffx_framegeneration.h"

static PfnFfxCreateContext p_create;
static PfnFfxDestroyContext p_destroy;
static PfnFfxConfigure p_configure;
static PfnFfxQuery p_query;
static PfnFfxDispatch p_dispatch;
static int g_load = -1;   // -1 not tried, 0 unavailable, 1 loaded

static HMODULE load_from(const wchar_t *dir) {
    wchar_t p[MAX_PATH + 32];
    _snwprintf(p, MAX_PATH + 31, L"%ls\\amd_fidelityfx_vk.dll", dir);
    p[MAX_PATH + 31] = 0;
    return LoadLibraryExW(p, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
}

// A crash inside FidelityFX's context creation (public #45, Arc B580 + an AMD iGPU: c0000005 at address 0 inside
// ffxCreateContext, the first FSR use) is caught (create_guarded) and the GPU and driver it happened on are listed in
// cache/ffx_quarantine.txt, one "<vendor> <device> <driver version> <pipeline cache UUID>" line each (as exec.c's
// precompile_stuck.txt names its GPU): FSR is then unavailable there, at once and at later starts, and the game's own
// upscale pass draws. Another driver version, or deleting the file, tries again.
#define FFXQ_PATH "cache/ffx_quarantine.txt"
static char g_dev_key[96];
static const char *dev_key(void) {
    if (!g_dev_key[0]) {
        int at = snprintf(g_dev_key, sizeof g_dev_key, "%04x %04x %08x ", g_vk.props.vendorID, g_vk.props.deviceID,
                          g_vk.props.driverVersion);
        for (int i = 0; i < VK_UUID_SIZE; i++) at += snprintf(g_dev_key + at, sizeof g_dev_key - at, "%02x", g_vk.props.pipelineCacheUUID[i]);
    }
    return g_dev_key;
}
static bool ffxq_listed(void) {
    FILE *f = fopen(FFXQ_PATH, "rb");
    if (!f) return false;
    char line[256];
    bool hit = false;
    while (!hit && fgets(line, sizeof line, f)) hit = !strncmp(line, dev_key(), strlen(dev_key()));
    fclose(f);
    return hit;
}

// The DLL, once: beside totk.exe, then TOTK_FFX_DIR
static bool ffx_load(void) {
    if (g_load >= 0) return g_load == 1;
    g_load = 0;
    if (ffxq_listed()) {
        LOG("[fsr] FidelityFX crashed on this GPU and driver before (%s, %s): FSR unavailable; a new driver, or deleting "
            "the file, tries again\n", g_vk.props.deviceName, FFXQ_PATH);
        return false;
    }
    wchar_t exe[MAX_PATH], dir[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, exe, MAX_PATH);
    while (n && exe[n - 1] != L'\\' && exe[n - 1] != L'/') n--;
    exe[n ? n - 1 : 0] = 0;
    HMODULE m = load_from(exe);
    const char *d = getenv("TOTK_FFX_DIR");
    if (!m && d && *d && MultiByteToWideChar(CP_UTF8, 0, d, -1, dir, MAX_PATH)) m = load_from(dir);
    if (!m) { LOG("[fsr] amd_fidelityfx_vk.dll not found (beside totk.exe or TOTK_FFX_DIR): FSR unavailable\n"); return false; }
    p_create = (PfnFfxCreateContext)(void *)GetProcAddress(m, "ffxCreateContext");
    p_destroy = (PfnFfxDestroyContext)(void *)GetProcAddress(m, "ffxDestroyContext");
    p_configure = (PfnFfxConfigure)(void *)GetProcAddress(m, "ffxConfigure");
    p_query = (PfnFfxQuery)(void *)GetProcAddress(m, "ffxQuery");
    p_dispatch = (PfnFfxDispatch)(void *)GetProcAddress(m, "ffxDispatch");
    if (!p_create || !p_destroy || !p_configure || !p_query || !p_dispatch) {
        LOG("[fsr] amd_fidelityfx_vk.dll lacks the ffx API entry points: FSR unavailable\n");
        return false;
    }
    g_load = 1;
    LOG("[fsr] FidelityFX runtime loaded (amd_fidelityfx_vk.dll, SDK v1.1.4 API)\n");
    return true;
}
static int g_fails, g_fg_fails;   // (failed creates / dispatches: after 4, 'not ready', and ngx.c's picker stops asking)
bool vk_ffx_sr_ready(void) { return ffx_load() && g_fails <= 3; }
bool vk_ffx_fg_ready(void) { return ffx_load() && g_fg_fails <= 3; }

static void ffx_message(uint32_t type, const wchar_t *msg) {
    static int n;
    if (n++ < 64) LOG("[fsr] %s: %ls\n", type == FFX_API_MESSAGE_TYPE_ERROR ? "error" : "warning", msg);
}

// FidelityFX's Vulkan backend looks its device commands up by name through the vkGetDeviceProcAddr it is given (the
// loader's, for our device: the DLL's own physical-device queries go through the same loader, so a second GPU does not
// mix in), KHR / EXT / AMD names among them (vkGetBufferMemoryRequirements2KHR, the debug-utils labels and object names,
// the AMD buffer markers), and calls some without checking for NULL. Our device is Vulkan 1.3 and enables none of those
// extensions: a driver that gives a promoted command's KHR name only with its extension enabled (as the spec says;
// NVIDIA's and AMD's give it anyway) answers NULL. So a KHR / EXT name gets its core command, the labels, names and markers
// (diagnostics only) a no-op, and each name still unresolved one log line.
static VkResult VKAPI_CALL ffx_noop_name(VkDevice d, const VkDebugUtilsObjectNameInfoEXT *i) { (void)d; (void)i; return VK_SUCCESS; }
static void VKAPI_CALL ffx_noop_label(VkCommandBuffer cb, const VkDebugUtilsLabelEXT *l) { (void)cb; (void)l; }
static void VKAPI_CALL ffx_noop_end(VkCommandBuffer cb) { (void)cb; }
static void VKAPI_CALL ffx_noop_marker(VkCommandBuffer cb, VkPipelineStageFlagBits st, VkBuffer b, VkDeviceSize o, uint32_t m) {
    (void)cb; (void)st; (void)b; (void)o; (void)m;
}
static void VKAPI_CALL ffx_noop_marker2(VkCommandBuffer cb, VkPipelineStageFlags2 st, VkBuffer b, VkDeviceSize o, uint32_t m) {
    (void)cb; (void)st; (void)b; (void)o; (void)m;
}
static PFN_vkVoidFunction VKAPI_CALL ffx_device_proc(VkDevice dev, const char *name) {
    PFN_vkVoidFunction f = vkGetDeviceProcAddr(dev, name);
    if (f || !name) return f;
    const size_t n = strlen(name);
    const char *how = "none: NULL";
    if (n > 3 && n < 128 && (!strcmp(name + n - 3, "KHR") || !strcmp(name + n - 3, "EXT"))) {
        char core[128];
        memcpy(core, name, n - 3);
        core[n - 3] = 0;
        if ((f = vkGetDeviceProcAddr(dev, core))) how = "its core command";
    }
    if (!f) {
        static const struct { const char *name; PFN_vkVoidFunction fn; } noop[] = {
            {"vkSetDebugUtilsObjectNameEXT", (PFN_vkVoidFunction)ffx_noop_name},
            {"vkCmdBeginDebugUtilsLabelEXT", (PFN_vkVoidFunction)ffx_noop_label},
            {"vkCmdEndDebugUtilsLabelEXT", (PFN_vkVoidFunction)ffx_noop_end},
            {"vkCmdInsertDebugUtilsLabelEXT", (PFN_vkVoidFunction)ffx_noop_label},
            {"vkCmdWriteBufferMarkerAMD", (PFN_vkVoidFunction)ffx_noop_marker},
            {"vkCmdWriteBufferMarker2AMD", (PFN_vkVoidFunction)ffx_noop_marker2}};
        for (size_t i = 0; i < sizeof noop / sizeof *noop && !f; i++)
            if (!strcmp(name, noop[i].name)) f = noop[i].fn, how = "a no-op";
    }
    static int nlog;
    if (nlog++ < 16) LOG("[fsr] FidelityFX asked for %s, which this device does not give by that name: %s\n", name, how);
    return f;
}

static void backend_desc(struct ffxCreateBackendVKDesc *be) {
    memset(be, 0, sizeof *be);
    be->header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
    be->vkDevice = g_vk.dev;
    be->vkPhysicalDevice = g_vk.phys;
    be->vkDeviceProcAddr = ffx_device_proc;
}

// p_create with an access violation inside it caught (pc_fault_guard lets it reach the __except): the GPU and driver
// listed (FFXQ_PATH), FSR off from then on (g_load 0: every entry here returns false), the context left unmade
static int av_filter(DWORD code) { return code == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH; }
static ffxReturnCode_t create_guarded(ffxContext *c, ffxCreateContextDescHeader *h, const char *what) {
    ffxReturnCode_t r = FFX_API_RETURN_ERROR;
    bool crashed = false;
    pc_fault_guard(1);
    __try {
        r = p_create(c, h, NULL);
    } __except (av_filter(GetExceptionCode())) {
        crashed = true;
    }
    pc_fault_guard(0);
    if (!crashed) return r;
    *c = NULL;
    g_load = 0;
    FILE *f = fopen(FFXQ_PATH, "ab");
    if (f) { fprintf(f, "%s %s\n", dev_key(), g_vk.props.deviceName); fclose(f); }
    LOG("[fsr] FidelityFX crashed making its %s context on %s (access violation inside amd_fidelityfx_vk.dll): FSR off "
        "for this GPU and driver (%s%s), the game's own upscale draws\n", what, g_vk.props.deviceName, FFXQ_PATH,
        f ? "" : ": the file could not be written, this run only");
    return FFX_API_RETURN_ERROR;
}

// a GENERAL-layout image as FFX sees it: state UNORDERED_ACCESS = GENERAL in its Vulkan backend, which puts every user
// resource back to the state given here at the end of a dispatch (ffx_vk.cpp: initialState), so ours stay GENERAL. A
// depth format gets the depth usage (FFX then views its depth aspect).
static FfxApiResource res_of(const VkNgxImage *im, uint32_t usage) {
    FfxApiResource r;
    memset(&r, 0, sizeof r);
    if (!im || !im->image) return r;
    r.resource = (void *)im->image;
    r.state = FFX_API_RESOURCE_STATE_UNORDERED_ACCESS;
    r.description.type = FFX_API_RESOURCE_TYPE_TEXTURE2D;
    r.description.format = ffxApiGetSurfaceFormatVK(im->format);
    r.description.width = im->w;
    r.description.height = im->h;
    r.description.depth = 1;
    r.description.mipCount = 1;
    r.description.usage = usage | (ffxApiIsDepthFormat(im->format) ? FFX_API_RESOURCE_USAGE_DEPTHTARGET : 0);
    return r;
}

// ---- super resolution
static ffxContext g_ctx;
static uint32_t g_rw, g_rh, g_ow, g_oh, g_flags = FFX_UPSCALE_ENABLE_AUTO_EXPOSURE | FFX_UPSCALE_ENABLE_DYNAMIC_RESOLUTION;
static uint64_t g_t_last;

// (FFX's own allocations: outside textures.c's residency budget; a few tens of MB at 1440p, made once per size)
static void ctx_wait(void) {   // (its last dispatch done: as ngx.c's sr_release; not vkDeviceWaitIdle, which races the record
    vk_wait_seq(vk_pending_seq() - 1);  // thread's own submits and cannot see work still in the vkq ring)
}

static void sr_release(void) {
    if (!g_ctx) return;
    ctx_wait();   // (a context is remade only on a size / flag change)
    p_destroy(&g_ctx, NULL);
    g_ctx = NULL;
}

static bool sr_create(uint32_t rw, uint32_t rh, uint32_t ow, uint32_t oh, uint32_t flags) {
    sr_release();
    struct ffxCreateBackendVKDesc be;
    backend_desc(&be);
    struct ffxCreateContextDescUpscale cd;
    memset(&cd, 0, sizeof cd);
    cd.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    cd.header.pNext = &be.header;
    cd.flags = flags;
    cd.maxRenderSize.width = rw; cd.maxRenderSize.height = rh;
    cd.maxUpscaleSize.width = ow; cd.maxUpscaleSize.height = oh;
    cd.fpMessage = ffx_message;
    uint64_t t0 = SDL_GetTicksNS();
    ffxReturnCode_t r = create_guarded(&g_ctx, &cd.header, "upscale");
    if (r != FFX_API_RETURN_OK) {
        g_ctx = NULL;
        g_fails++;
        LOG("[fsr] upscale context %ux%u -> %ux%u failed (%u)\n", rw, rh, ow, oh, (unsigned)r);
        return false;
    }
    g_rw = rw; g_rh = rh; g_ow = ow; g_oh = oh; g_flags = flags;
    LOG("[fsr] FSR 3.1 upscale context %ux%u -> %ux%u (flags 0x%x) in %.1f ms\n", rw, rh, ow, oh, (unsigned)flags,
        (SDL_GetTicksNS() - t0) / 1e6);
    return true;
}

// At known sizes between frames (ngx.c, as vk_ngx_prepare): the context's pipelines are built here, not on the first frame
// that upscales. The depth convention comes with the first evaluate; a different one remakes it once.
bool vk_ffx_sr_prepare(VkCommandBuffer cb, uint32_t rw, uint32_t rh, uint32_t ow, uint32_t oh) {
    (void)cb;
    if (!ffx_load() || g_fails > 3) return false;
    if (g_ctx && g_ow == ow && g_oh == oh && g_rw >= rw && g_rh >= rh) return true;
    return sr_create(rw, rh, ow, oh, g_flags);
}

bool vk_ffx_sr_evaluate(VkCommandBuffer cb, const VkTemporalInputs *in) {
    if (!ffx_load() || g_fails > 3) return false;
    const uint32_t flags = FFX_UPSCALE_ENABLE_AUTO_EXPOSURE | FFX_UPSCALE_ENABLE_DYNAMIC_RESOLUTION
                         | (in->depth_inverted ? FFX_UPSCALE_ENABLE_DEPTH_INVERTED : 0);
    bool reset = in->reset;
    // (as DLSS's feature: the largest scene seen at this output; a larger one or another output makes a new context)
    if (!g_ctx || in->out.w != g_ow || in->out.h != g_oh || flags != g_flags || in->color.w > g_rw || in->color.h > g_rh) {
        const bool same = g_ctx && in->out.w == g_ow && in->out.h == g_oh;
        uint32_t rw = same && g_rw > in->color.w ? g_rw : in->color.w, rh = same && g_rh > in->color.h ? g_rh : in->color.h;
        if (!sr_create(rw, rh, in->out.w, in->out.h, flags)) return false;
        reset = true;
    }
    struct ffxDispatchDescUpscale d;
    memset(&d, 0, sizeof d);
    d.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    d.commandList = (void *)cb;
    d.color = res_of(&in->color, FFX_API_RESOURCE_USAGE_READ_ONLY);
    d.depth = res_of(&in->depth, FFX_API_RESOURCE_USAGE_READ_ONLY);
    d.motionVectors = res_of(&in->mv, FFX_API_RESOURCE_USAGE_READ_ONLY);
    d.reactive = res_of(&in->bias, FFX_API_RESOURCE_USAGE_READ_ONLY);
    d.output = res_of(&in->out, FFX_API_RESOURCE_USAGE_UAV);
    d.jitterOffset.x = in->jx;   // (the geometry's shift in render pixels, as is: no negation)
    d.jitterOffset.y = in->jy;
    d.motionVectorScale.x = in->mv_sx;   // (render pixels: 1, FFX divides by the render size itself)
    d.motionVectorScale.y = in->mv_sy;
    d.renderSize.width = in->color.w; d.renderSize.height = in->color.h;
    d.upscaleSize.width = in->out.w; d.upscaleSize.height = in->out.h;
    d.enableSharpening = false;   // (the Sharpening row's RCAS is separate)
    const uint64_t now = SDL_GetTicksNS();
    d.frameTimeDelta = g_t_last && !reset ? (float)((now - g_t_last) / 1e6) : 16.6f;
    g_t_last = now;
    d.preExposure = 1.f;
    d.reset = reset;
    d.cameraNear = in->near_;
    d.cameraFar = in->far_;
    d.cameraFovAngleVertical = in->fovy;
    d.viewSpaceToMetersFactor = 1.f;
    ffxReturnCode_t r = p_dispatch(&g_ctx, &d.header);
    if (r != FFX_API_RETURN_OK) {
        if (g_fails++ < 4) LOG("[fsr] upscale dispatch failed (%u, %ux%u -> %ux%u)\n", (unsigned)r, in->color.w, in->color.h, in->out.w, in->out.h);
        return false;
    }
    return true;
}

// ---- frame generation: one generated frame between real frames, made into an image of ours and shown by ngx.c's
// presenter path, not through FFX's swapchain: the context is configured without one
// (FFX_FRAMEGENERATION_FLAG_NO_SWAPCHAIN_CONTEXT_NOTIFY, ffx_provider_framegeneration.cpp), and each frame gets the
// prepare dispatch (render-size depth, motion vectors, jitter, camera) then the generation dispatch under the same frameID
// (the provider keeps each prepare by frameID)
static ffxContext g_fgctx;
static uint32_t g_fg_dw, g_fg_dh, g_fg_rw, g_fg_rh, g_fg_flags;
static VkFormat g_fg_fmt, g_fg_hlfmt;
static VkImage g_fg_hlimg;   // (the HUD-less image the context was configured with, or none)
static uint64_t g_fgid, g_fg_t_last;

static void fg_release(void) {
    if (!g_fgctx) return;
    ctx_wait();
    p_destroy(&g_fgctx, NULL);
    g_fgctx = NULL;
}

static bool fg_create(uint32_t dw, uint32_t dh, uint32_t rw, uint32_t rh, VkFormat fmt, VkFormat hlfmt, uint32_t flags) {
    fg_release();
    struct ffxCreateBackendVKDesc be;
    backend_desc(&be);
    struct ffxCreateContextDescFrameGenerationHudless hd;
    memset(&hd, 0, sizeof hd);
    hd.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_HUDLESS;
    hd.header.pNext = &be.header;
    hd.hudlessBackBufferFormat = ffxApiGetSurfaceFormatVK(hlfmt);
    struct ffxCreateContextDescFrameGeneration cd;
    memset(&cd, 0, sizeof cd);
    cd.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
    cd.header.pNext = hlfmt != VK_FORMAT_UNDEFINED ? &hd.header : &be.header;
    cd.flags = flags;
    cd.displaySize.width = dw; cd.displaySize.height = dh;
    cd.maxRenderSize.width = rw; cd.maxRenderSize.height = rh;
    cd.backBufferFormat = ffxApiGetSurfaceFormatVK(fmt);
    uint64_t t0 = SDL_GetTicksNS();
    ffxReturnCode_t r = create_guarded(&g_fgctx, &cd.header, "frame generation");
    if (r != FFX_API_RETURN_OK) {
        g_fgctx = NULL;
        g_fg_fails++;
        LOG("[fsr] frame generation context %ux%u (render %ux%u) failed (%u)\n", dw, dh, rw, rh, (unsigned)r);
        return false;
    }
    g_fg_dw = dw; g_fg_dh = dh; g_fg_rw = rw; g_fg_rh = rh; g_fg_flags = flags; g_fg_fmt = fmt; g_fg_hlfmt = hlfmt;
    g_fg_hlimg = VK_NULL_HANDLE;   // (configured at the first evaluate, with the HUD-less image it brings)
    LOG("[fsr] FSR 3.1 frame generation context %ux%u (render up to %ux%u, format %d%s) in %.1f ms\n", dw, dh, rw, rh,
        (int)fmt, hlfmt != VK_FORMAT_UNDEFINED ? ", HUD-less image" : "", (SDL_GetTicksNS() - t0) / 1e6);
    return true;
}

static bool fg_configure(const VkNgxImage *hudless, uint32_t dw, uint32_t dh) {
    struct ffxConfigureDescFrameGeneration cf;
    memset(&cf, 0, sizeof cf);
    cf.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
    cf.frameGenerationEnabled = true;
    cf.flags = FFX_FRAMEGENERATION_FLAG_NO_SWAPCHAIN_CONTEXT_NOTIFY;
    cf.HUDLessColor = res_of(hudless, FFX_API_RESOURCE_USAGE_READ_ONLY);
    cf.generationRect.width = (int32_t)dw;
    cf.generationRect.height = (int32_t)dh;
    cf.frameID = g_fgid;
    ffxReturnCode_t r = p_configure(&g_fgctx, &cf.header);
    if (r != FFX_API_RETURN_OK) {
        if (g_fg_fails++ < 4) LOG("[fsr] frame generation configure failed (%u)\n", (unsigned)r);
        return false;
    }
    g_fg_hlimg = hudless && hudless->image ? hudless->image : VK_NULL_HANDLE;
    return true;
}

bool vk_ffx_fg_prepare(VkCommandBuffer cb, uint32_t ow, uint32_t oh, uint32_t rw, uint32_t rh, VkFormat fmt, VkFormat hudless_fmt) {
    (void)cb;
    if (!ffx_load() || g_fg_fails > 3) return false;
    if (g_fgctx && g_fg_dw == ow && g_fg_dh == oh && g_fg_rw >= rw && g_fg_rh >= rh && g_fg_fmt == fmt && g_fg_hlfmt == hudless_fmt)
        return true;
    return fg_create(ow, oh, rw, rh, fmt, hudless_fmt, g_fg_flags);
}

int vk_ffx_fg_evaluate(VkCommandBuffer cb, const VkTemporalInputs *in, VkNgxImage *out, int n) {
    if (!ffx_load() || g_fg_fails > 3 || n < 1 || !out) return 0;
    const VkNgxImage *bb = &in->backbuffer, *hl = in->hudless.image ? &in->hudless : NULL;
    const VkFormat hlfmt = hl ? hl->format : VK_FORMAT_UNDEFINED;
    // (a frame without the HUD-less image while the context takes one never gets here: ngx.c skips it, up to 30 frames,
    // and resets the next one, for both backends (dlss af669e14); only a lasting change remakes the context below)
    const uint32_t flags = in->depth_inverted ? FFX_FRAMEGENERATION_ENABLE_DEPTH_INVERTED : 0;
    bool reset = in->reset;
    if (!g_fgctx || bb->w != g_fg_dw || bb->h != g_fg_dh || bb->format != g_fg_fmt || hlfmt != g_fg_hlfmt || flags != g_fg_flags ||
        in->depth.w > g_fg_rw || in->depth.h > g_fg_rh) {
        uint32_t rw = g_fgctx && in->depth.w <= g_fg_rw ? g_fg_rw : in->depth.w, rh = g_fgctx && in->depth.h <= g_fg_rh ? g_fg_rh : in->depth.h;
        if (!fg_create(bb->w, bb->h, rw, rh, bb->format, hlfmt, flags)) return 0;
        reset = true;
    }
    if ((hl ? hl->image : VK_NULL_HANDLE) != g_fg_hlimg || !g_fgid) {
        if (!fg_configure(hl, bb->w, bb->h)) return 0;
        reset = true;
    }
    const uint64_t id = ++g_fgid, now = SDL_GetTicksNS();
    struct ffxDispatchDescFrameGenerationPrepareCameraInfo ci;
    memset(&ci, 0, sizeof ci);
    ci.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_CAMERAINFO;
    memcpy(ci.cameraPosition, in->cam_pos, sizeof ci.cameraPosition);
    memcpy(ci.cameraRight, in->cam_right, sizeof ci.cameraRight);
    memcpy(ci.cameraUp, in->cam_up, sizeof ci.cameraUp);
    memcpy(ci.cameraForward, in->cam_fwd, sizeof ci.cameraForward);
    struct ffxDispatchDescFrameGenerationPrepare pd;
    memset(&pd, 0, sizeof pd);
    pd.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE;
    pd.header.pNext = &ci.header;
    pd.frameID = id;
    pd.commandList = (void *)cb;
    pd.renderSize.width = in->depth.w; pd.renderSize.height = in->depth.h;
    pd.jitterOffset.x = in->jx; pd.jitterOffset.y = in->jy;
    pd.motionVectorScale.x = in->mv_sx; pd.motionVectorScale.y = in->mv_sy;
    pd.frameTimeDelta = g_fg_t_last && !reset ? (float)((now - g_fg_t_last) / 1e6) : 16.6f;
    g_fg_t_last = now;
    pd.unused_reset = reset;
    pd.cameraNear = in->near_;
    pd.cameraFar = in->far_;
    pd.cameraFovAngleVertical = in->fovy;
    pd.viewSpaceToMetersFactor = 1.f;
    pd.depth = res_of(&in->depth, FFX_API_RESOURCE_USAGE_READ_ONLY);
    pd.motionVectors = res_of(&in->mv, FFX_API_RESOURCE_USAGE_READ_ONLY);
    ffxReturnCode_t r = p_dispatch(&g_fgctx, &pd.header);
    if (r != FFX_API_RETURN_OK) {
        if (g_fg_fails++ < 4) LOG("[fsr] frame generation prepare failed (%u)\n", (unsigned)r);
        return 0;
    }
    struct ffxDispatchDescFrameGeneration gd;
    memset(&gd, 0, sizeof gd);
    gd.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION;
    gd.commandList = (void *)cb;
    gd.presentColor = res_of(bb, FFX_API_RESOURCE_USAGE_READ_ONLY);
    gd.outputs[0] = res_of(&out[0], FFX_API_RESOURCE_USAGE_UAV);
    gd.numGeneratedFrames = 1;   // (x2: FSR 3.1 makes one between two real frames)
    gd.reset = reset;
    gd.backbufferTransferFunction = FFX_API_BACKBUFFER_TRANSFER_FUNCTION_SRGB;   // (the game's SDR picture)
    gd.minMaxLuminance[0] = 0.f; gd.minMaxLuminance[1] = 1.f;
    gd.generationRect.width = (int32_t)bb->w;
    gd.generationRect.height = (int32_t)bb->h;
    gd.frameID = id;
    r = p_dispatch(&g_fgctx, &gd.header);
    if (r != FFX_API_RETURN_OK) {
        if (g_fg_fails++ < 4) LOG("[fsr] frame generation dispatch failed (%u)\n", (unsigned)r);
        return 0;
    }
    return 1;
}

void vk_ffx_release(void) {
    if (g_load != 1) return;
    sr_release();
    fg_release();
    g_fgid = 0;
}

#else   // (no FidelityFX runtime off Windows)
bool vk_ffx_sr_ready(void) { return false; }
bool vk_ffx_sr_prepare(VkCommandBuffer cb, uint32_t rw, uint32_t rh, uint32_t ow, uint32_t oh) {
    (void)cb; (void)rw; (void)rh; (void)ow; (void)oh;
    return false;
}
bool vk_ffx_sr_evaluate(VkCommandBuffer cb, const VkTemporalInputs *in) { (void)cb; (void)in; return false; }
bool vk_ffx_fg_ready(void) { return false; }
bool vk_ffx_fg_prepare(VkCommandBuffer cb, uint32_t ow, uint32_t oh, uint32_t rw, uint32_t rh, VkFormat fmt, VkFormat hudless_fmt) {
    (void)cb; (void)ow; (void)oh; (void)rw; (void)rh; (void)fmt; (void)hudless_fmt;
    return false;
}
int vk_ffx_fg_evaluate(VkCommandBuffer cb, const VkTemporalInputs *in, VkNgxImage *out, int n) {
    (void)cb; (void)in; (void)out; (void)n;
    return 0;
}
void vk_ffx_release(void) {}
#endif
