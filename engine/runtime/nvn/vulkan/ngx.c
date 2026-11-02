// ngx.c: NVIDIA NGX, the runtime behind DLSS Super Resolution / DLAA and DLSS Frame Generation. Driven directly, without Streamline or
// Reflex, on the instance and device device.c creates. SDK: DLSS 310.9.1 (engine/third_party/dlss: headers, nvsdk_ngx_d.lib for our
// dynamic CRT, licence).
//
// TOTK_DLSS=1 (default off): the instance and device get the extensions NGX asks for (Super Resolution and Frame
// Generation), NGX is initialised on our device and its capability map is logged ("[dlss]" lines). Nothing is evaluated
// yet. The runtime DLLs (nvngx_dlss.dll, nvngx_dlssg.dll) are looked up beside totk.exe, then in TOTK_DLSS_DIR (default
// <work>/engine/dlss, where they are kept out of git).
#ifdef _WIN32   // (NGX is Windows-only here: the Linux / Deck build gets the stubs at the end, DLSS unavailable)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include <SDL3/SDL.h>

#include "../shim/las.h"
#include "vk.h"
#include "../../../third_party/dlss/include/nvsdk_ngx_vk.h"
#include "../../../third_party/dlss/include/nvsdk_ngx_defs_dlssg.h"
#include "../../../third_party/dlss/include/nvsdk_ngx_helpers_vk.h"

// NGX keys its per-application data and over-the-air model updates on this; minted once for the engine, never change it
#define NGX_PROJECT_ID "6f0f6a2e-3c1b-4d8e-9a57-1b2e5c7d9f40"
#define NGX_ENGINE_VERSION "1.0"

static int g_on = -1;
static bool g_ready;
static NVSDK_NGX_Parameter *g_caps;
static wchar_t g_data[MAX_PATH], g_dirs[2][MAX_PATH];
static const wchar_t *g_dir_list[2];
static NVSDK_NGX_FeatureCommonInfo g_common;

// The switches: TOTK_DLSS=sr | fg (SR and DLSS-G) | fgtest, or the Options rows' own variables (runtime/pc_dlss.cpp):
// TOTK_DLSS_SR=1 / TOTK_DLSS_FG=1 turn either on alone, TOTK_DLSS_SR=0 keeps SR off under TOTK_DLSS=fg.
static int dlss_env(const char *name) {   // 1 on, 0 off, -1 unset
    const char *e = getenv(name);
    return !e || !*e ? -1 : !strcmp(e, "0") || !strcmp(e, "off") ? 0 : 1;
}

static bool dlss_mode(const char *m) {
    const char *e = getenv("TOTK_DLSS");
    return e && !strcmp(e, m);
}

// AMD FSR 3.1 (the ffx.c, any Vulkan GPU) behind the same inputs: TOTK_FSR_SR=1 / TOTK_FSR_FG=1, or the rows' "fsr"
// values (pc_dlss.cpp). Each path picks its backend; NGX starts only for a DLSS one.
static int g_ngx_env = -1;        // NGX wanted at the start: its extensions and Init with the device
static bool g_sr_ffx, g_fg_ffx;   // SR / FG through FidelityFX instead of NGX
static bool g_sr_dlaa;            // DLSS SR with the scene at the output's size (vk_ngx_scene_scale): DLAA
static bool g_fg_ffx_hl;          // FidelityFX's FG context takes the HUD-less image (as g_fg_hl for NGX's)
extern int rt_fleet(void);  // (rt_main.cpp, TOTK_FLEET=1)
static bool ngx_env(void) {
    if (g_ngx_env < 0) {
        const char *e = getenv("TOTK_DLSS");
        g_ngx_env = !rt_fleet() && ((e && *e && strcmp(e, "0") != 0) || dlss_env("TOTK_DLSS_SR") == 1 || dlss_env("TOTK_DLSS_FG") == 1);
    }
    return g_ngx_env;
}

// The temporal path wanted (DLSS or FSR): exec.c latches the G-buffer's camera for it
bool vk_ngx_on(void) {
    if (g_on < 0) g_on = ngx_env() || (!rt_fleet() && (dlss_env("TOTK_FSR_SR") == 1 || dlss_env("TOTK_FSR_FG") == 1));
    return g_on;
}

bool vk_ngx_ready(void) { return g_ready; }

static void ngx_log(const char *msg, NVSDK_NGX_Logging_Level lvl, NVSDK_NGX_Feature src) {
    (void)src;
    if (!msg) return;
    size_t n = strlen(msg);
    while (n && (msg[n - 1] == '\n' || msg[n - 1] == '\r')) n--;
    LOG("[dlss] ngx[%d]: %.*s\n", (int)lvl, (int)n, msg);
}

// The C-side identity NGX reads before and at Init: project id, data path (NGX writes its logs there: the exe's folder),
// DLL search paths. TOTK_DLSS_LOG=1: NGX's own verbose log into ours.
static void discovery(NVSDK_NGX_FeatureDiscoveryInfo *d, NVSDK_NGX_Feature feature) {
    if (!g_dir_list[0]) {
        DWORD n = GetModuleFileNameW(NULL, g_data, MAX_PATH);
        while (n && g_data[n - 1] != L'\\' && g_data[n - 1] != L'/') n--;
        g_data[n ? n - 1 : 0] = 0;
        wcscpy(g_dirs[0], g_data);
        // TOTK_DLSS_DIR: where the feature DLLs are (boot.py and a release's player.txt: <work>\engine\dlss, which
        // tools/engine/build.py fills); unset, the exe's folder only
        const char *dir = getenv("TOTK_DLSS_DIR");
        if (dir && *dir) MultiByteToWideChar(CP_UTF8, 0, dir, -1, g_dirs[1], MAX_PATH);
        else wcscpy(g_dirs[1], g_data);
        g_dir_list[0] = g_dirs[0];
        g_dir_list[1] = g_dirs[1];
        bool verbose = getenv("TOTK_DLSS_LOG") != NULL;
        g_common.PathListInfo.Path = g_dir_list;
        g_common.PathListInfo.Length = 2;
        g_common.LoggingInfo.LoggingCallback = verbose ? ngx_log : NULL;
        g_common.LoggingInfo.MinimumLoggingLevel = verbose ? NVSDK_NGX_LOGGING_LEVEL_VERBOSE : NVSDK_NGX_LOGGING_LEVEL_OFF;
        g_common.LoggingInfo.DisableOtherLoggingSinks = false;
    }
    memset(d, 0, sizeof *d);
    d->SDKVersion = NVSDK_NGX_Version_API;
    d->FeatureID = feature;
    d->Identifier.IdentifierType = NVSDK_NGX_Application_Identifier_Type_Project_Id;
    d->Identifier.v.ProjectDesc.ProjectId = NGX_PROJECT_ID;
    d->Identifier.v.ProjectDesc.EngineType = NVSDK_NGX_ENGINE_TYPE_CUSTOM;
    d->Identifier.v.ProjectDesc.EngineVersion = NGX_ENGINE_VERSION;
    d->ApplicationDataPath = g_data;
    d->FeatureInfo = &g_common;
}

static const NVSDK_NGX_Feature k_features[2] = {NVSDK_NGX_Feature_SuperSampling, NVSDK_NGX_Feature_FrameGeneration};
static const char *const k_feature_names[2] = {"super resolution", "frame generation"};

static void add_unique(const char **ext, uint32_t *n, uint32_t room, const char *name) {
    for (uint32_t i = 0; i < *n; i++) if (!strcmp(ext[i], name)) return;
    if (*n < room) ext[(*n)++] = name;
    else LOG("[dlss] no room for extension %s\n", name);
}

// Instance extensions both features ask for (NGX's arrays are static for the process, so the names stay valid).
uint32_t vk_ngx_instance_exts(const char **ext, uint32_t n, uint32_t room) {
    if (!ngx_env()) return n;
    for (int f = 0; f < 2; f++) {
        NVSDK_NGX_FeatureDiscoveryInfo d;
        discovery(&d, k_features[f]);
        uint32_t cnt = 0;
        VkExtensionProperties *props = NULL;
        NVSDK_NGX_Result r = NVSDK_NGX_VULKAN_GetFeatureInstanceExtensionRequirements(&d, &cnt, &props);
        if (NVSDK_NGX_FAILED(r)) { LOG("[dlss] %s: instance extension query failed (0x%x)\n", k_feature_names[f], (unsigned)r); continue; }
        for (uint32_t i = 0; i < cnt; i++) {
            LOG("[dlss] %s needs instance extension %s\n", k_feature_names[f], props[i].extensionName);
            add_unique(ext, &n, room, props[i].extensionName);
        }
    }
    return n;
}

// Device extensions both features ask for on this GPU, the ones the device has; also whether the GPU can run each at all
// (GetFeatureRequirements, answered before Init: an unsupported machine never loads the feature DLLs). DLSS off at the
// start, on an NVIDIA GPU: the extensions NGX asked for on RTX GPUs (310.x: the log's "needs device extension" lines) are
// enabled if the device has them, without asking NGX (nothing of NGX runs, no DLL is looked up), so the Options rows can
// turn DLSS on later (ngx_init, from vk_ngx_prepare). Enabling them changes nothing by itself. No instance extension is
// added for it: the only one NGX 310.x asks is VK_KHR_get_physical_device_properties2, core in device.c's Vulkan 1.3
// instance.
static bool g_live_exts;   // (DLSS off at the start: they were all there)
void vk_ngx_device_exts(const VkExtensionProperties *ep, uint32_t ne, const char **ext, uint32_t *n_ext, uint32_t room) {
    if (!ngx_env()) {
        static const char *const known[] = {"VK_NVX_binary_import", "VK_NVX_image_view_handle", "VK_KHR_buffer_device_address",
                                            "VK_KHR_push_descriptor"};
        if (g_vk.props.vendorID != 0x10DE) return;
        int have_all = 1;
        for (int i = 0; i < 4; i++) {
            bool have = false;
            for (uint32_t j = 0; j < ne && !have; j++) have = !strcmp(ep[j].extensionName, known[i]);
            if (have) add_unique(ext, n_ext, room, known[i]);
            have_all &= have;
        }
        g_live_exts = have_all;
        return;
    }
    for (int f = 0; f < 2; f++) {
        NVSDK_NGX_FeatureDiscoveryInfo d;
        discovery(&d, k_features[f]);
        NVSDK_NGX_FeatureRequirement req;
        memset(&req, 0, sizeof req);
        NVSDK_NGX_Result r = NVSDK_NGX_VULKAN_GetFeatureRequirements(g_vk.inst, g_vk.phys, &d, &req);
        if (NVSDK_NGX_FAILED(r)) LOG("[dlss] %s: requirement query failed (0x%x)\n", k_feature_names[f], (unsigned)r);
        else LOG("[dlss] %s: %s (support bits 0x%x, min arch 0x%x, min OS %s)\n", k_feature_names[f],
                 req.FeatureSupported == NVSDK_NGX_FeatureSupportResult_Supported ? "supported" : "NOT supported",
                 (unsigned)req.FeatureSupported, (unsigned)req.MinHWArchitecture, req.MinOSVersion);
        uint32_t cnt = 0;
        VkExtensionProperties *props = NULL;
        r = NVSDK_NGX_VULKAN_GetFeatureDeviceExtensionRequirements(g_vk.inst, g_vk.phys, &d, &cnt, &props);
        if (NVSDK_NGX_FAILED(r)) { LOG("[dlss] %s: device extension query failed (0x%x)\n", k_feature_names[f], (unsigned)r); continue; }
        for (uint32_t i = 0; i < cnt; i++) {
            bool have = false;
            for (uint32_t j = 0; j < ne && !have; j++) have = !strcmp(ep[j].extensionName, props[i].extensionName);
            LOG("[dlss] %s needs device extension %s%s\n", k_feature_names[f], props[i].extensionName, have ? "" : " (MISSING)");
            if (have) add_unique(ext, n_ext, room, props[i].extensionName);
        }
    }
}

static int cap_i(const char *name, int dflt) {
    int v = dflt;
    if (g_caps && NVSDK_NGX_Parameter_GetI(g_caps, name, &v) != NVSDK_NGX_Result_Success) v = dflt;
    return v;
}

static unsigned cap_u(const char *name, unsigned dflt) {
    unsigned v = dflt;
    if (g_caps && NVSDK_NGX_Parameter_GetUI(g_caps, name, &v) != NVSDK_NGX_Result_Success) v = dflt;
    return v;
}

// After vkCreateDevice: Init on our device, then the capability map (what the driver, GPU and DLLs agree can run).
// On this thread, before the game starts: NGX on a thread of its own while the game booted crashed in the driver
// under nvngx_dlss's feature creation, as the render thread imported the guest heap.
static int fg_frames(void);   // (DLSS-G section)

// Turned on in game (vk_ngx_prepare, the render thread between frames, the thread every feature is made on): ~1 s, once.
static bool ngx_init(void);
void vk_ngx_device_ready(void) {
    if (ngx_env()) ngx_init();
    // (FSR: its DLL loaded here, on this thread, before the render and game threads ask vk_ngx_sr_on / fg_on)
    if (!rt_fleet() && (dlss_env("TOTK_FSR_SR") == 1 || dlss_env("TOTK_FSR_FG") == 1)) (void)vk_ffx_sr_ready();
}

static bool ngx_init(void) {
    static bool tried;
    if (tried) return g_ready;
    tried = true;
    NVSDK_NGX_FeatureDiscoveryInfo d;
    discovery(&d, NVSDK_NGX_Feature_SuperSampling);
    uint64_t t0 = SDL_GetTicksNS();
    NVSDK_NGX_Result r = NVSDK_NGX_VULKAN_Init_with_ProjectID(NGX_PROJECT_ID, NVSDK_NGX_ENGINE_TYPE_CUSTOM, NGX_ENGINE_VERSION,
                                                              g_data, g_vk.inst, g_vk.phys, g_vk.dev,
                                                              vkGetInstanceProcAddr, vkGetDeviceProcAddr, &g_common,
                                                              NVSDK_NGX_Version_API);
    if (NVSDK_NGX_FAILED(r)) { LOG("[dlss] NGX init failed (0x%x): DLSS off\n", (unsigned)r); return false; }
    r = NVSDK_NGX_VULKAN_GetCapabilityParameters(&g_caps);
    if (NVSDK_NGX_FAILED(r)) { LOG("[dlss] capability parameters failed (0x%x): DLSS off\n", (unsigned)r); return false; }
    g_ready = true;
    int sr = cap_i(NVSDK_NGX_Parameter_SuperSampling_Available, 0);
    int fg = cap_i(NVSDK_NGX_Parameter_FrameGeneration_Available, 0);
    LOG("[dlss] NGX up in %.0f ms: super resolution %s (init result 0x%x%s), frame generation %s (init result 0x%x%s, "
        "up to %u generated per rendered frame)\n", (SDL_GetTicksNS() - t0) / 1e6,
        sr ? "available" : "unavailable", (unsigned)cap_i(NVSDK_NGX_Parameter_SuperSampling_FeatureInitResult, 0),
        cap_i(NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, 0) ? ", needs a newer driver" : "",
        fg ? "available" : "unavailable", (unsigned)cap_i(NVSDK_NGX_Parameter_FrameGeneration_FeatureInitResult, 0),
        cap_i(NVSDK_NGX_Parameter_FrameGeneration_NeedsUpdatedDriver, 0) ? ", needs a newer driver" : "",
        cap_u(NVSDK_NGX_DLSSG_Parameter_MultiFrameCountMax, 1));
    if (vk_ngx_fg_on()) fg_frames();   // (the count and its line here, on this thread: the pacer reads it from the start)
    return true;
}

// ================================================================== foundation: camera and depth (logged only so far)
// The game's camera block is VS cb8 of the G-buffer pass: rows 0-2 = view (3x4, row-major, world -> view), rows 4-7 =
// projection (4x4, row-major), [29] = camera position (exec.c torch-flame code); the Mac port reads the same 2,368-byte
// block (nvn_mtl.m:9705-9716: near [56], fov [70], size [72-73]). Kept for this frame and the last, with a cut flagged
// when the camera jumps more than 8 units or turns more than 25 degrees in a frame (Mac totk_taa.m:12). "[dlss] camera"
// every 120 frames and at each cut (first 50): what motion vectors, jitter and the FG camera constants will be made of.
#include <math.h>
#define CAM_FLOATS (2368 / 4)
static float g_cam[2][CAM_FLOATS];
static uint64_t g_cam_frame = UINT64_MAX, g_cam_n, g_cuts;
static uint32_t g_depth_w, g_depth_h;
static bool g_cam_cut = true;   // (the latest latch: no usable previous frame, or a camera cut)

void vk_ngx_gbuffer_draw(const float *cb8, uint64_t size, uint32_t depth_w, uint32_t depth_h, uint64_t frame) {
    if (frame == g_cam_frame) return;   // (first G-buffer draw of the frame only)
    bool have_prev = g_cam_frame != UINT64_MAX && frame <= g_cam_frame + 2;
    g_cam_frame = frame;
    g_cam_n++;   // (latches are numbered: the upscale pass may come in another present count than the G-buffer)
    float *c = g_cam[g_cam_n & 1], *p = g_cam[(g_cam_n & 1) ^ 1];
    size_t n = size < sizeof g_cam[0] ? (size_t)size : sizeof g_cam[0];
    memcpy(c, cb8, n);
    if (n < sizeof g_cam[0]) memset((char *)c + n, 0, sizeof g_cam[0] - n);
    g_depth_w = depth_w;
    g_depth_h = depth_h;
    // forward = -(view row 2), position = [29]
    float dx = c[116] - p[116], dy = c[117] - p[117], dz = c[118] - p[118];
    float move = sqrtf(dx * dx + dy * dy + dz * dz);
    float dot = c[8] * p[8] + c[9] * p[9] + c[10] * p[10];
    float l = sqrtf((c[8] * c[8] + c[9] * c[9] + c[10] * c[10]) * (p[8] * p[8] + p[9] * p[9] + p[10] * p[10]));
    float turn = l > 0 ? acosf(fmaxf(-1.f, fminf(1.f, dot / l))) * 57.29578f : 0;
    bool cut = !have_prev || move > 8.f || turn > 25.f;
    if (cut) g_cuts++;
    g_cam_cut = cut;
    if (g_cam_n % 120 == 1 || (cut && g_cuts <= 50)) {
        const float *P = c + 16;
        float fovy = P[5] ? 2.f * atanf(1.f / fabsf(P[5])) * 57.29578f : 0, aspect = P[0] ? fabsf(P[5] / P[0]) : 0;
        LOG("[dlss] camera frame %llu%s: pos %.2f %.2f %.2f fwd %.3f %.3f %.3f move %.3f turn %.2f deg; fovy %.2f aspect %.4f "
            "P2 %g %g %g %g P3 %g %g %g %g; block [56] %g [70] %g [72] %g [73] %g (%llu bytes); G-buffer depth %ux%u; cuts %llu\n",
            (unsigned long long)frame, cut ? " CUT" : "", c[116], c[117], c[118], -c[8], -c[9], -c[10], move, turn, fovy, aspect,
            P[8], P[9], P[10], P[11], P[12], P[13], P[14], P[15], c[56], c[70], c[72], c[73], (unsigned long long)size,
            g_depth_w, g_depth_h, (unsigned long long)g_cuts);
    }
}

// ================================================================== Super Resolution / DLAA. The feature is created on the first evaluate (the title screen) and again only when the output size or the flags change or the
// scene gets bigger than it (released only after the GPU is done with the old one): a smaller scene (dynamic resolution) is evaluated as a subrect of the feature's size, as DLSS allows, instead of a new feature and its
// hitch. Inputs: scene colour (TotK's upscale pass input is already tonemapped: hdr false), D32 depth, motion vectors at render size (MVLowRes, not jittered, in render pixels unless mv_scale says otherwise); output: a
// storage image at output size. Exposure: DLSS's own (AutoExposure) until the game's exposure value is found (our earlier Rust renderer feeds a 1x1 R32F).
static NVSDK_NGX_Handle *g_sr;
static NVSDK_NGX_Parameter *g_sr_par;
static uint32_t g_sr_rw, g_sr_rh, g_sr_ow, g_sr_oh;
static int g_sr_flags = -1, g_sr_fails;
// Dynamic resolution: a smaller scene is evaluated as a subrect of the feature, but NGX only takes subrects inside the
// feature's quality mode's render range (an 800x450 -> 1080p MaxPerf feature refused a 640x360 subrect with
// 0xbad00005). The feature's quality is picked so its range covers the smallest scene seen so far; a scene below the
// range gets a feature of its own size, and a refused subrect makes every later feature follow the scene's size.
static uint32_t g_sr_minw, g_sr_minh;     // the feature's smallest render size (0: NGX did not say)
static uint32_t g_sr_seen_w, g_sr_seen_h; // the smallest scene seen for this output
static bool g_sr_exact;                   // a subrect was refused: no more subrects

// Halton (2,3), 1-based, over 8 * (output / render)^2 phases (at least 8): DLSS's recommended sequence length. Returns the subpixel
// offset the frame is rendered with, in render pixels, in [-0.5, 0.5): exec.c moves the scene's viewport by it (+x right, +y down the
// image), and vk_ngx_sr_evaluate reports that same shift to DLSS. DLSS takes the shift of the drawn geometry: our earlier Rust renderer
// shifts its projection by -j and reports -j, the sign its dlss_probe measured best (RMSE 0.043 against 0.118 for +j: the wrong sign
// smears a still image by up to a pixel). This port first copied the negation alone and reported -j for a +j shift.
static float halton(uint32_t i, uint32_t b) {
    float f = 1.f, r = 0.f;
    for (; i; i /= b) { f /= (float)b; r += f * (float)(i % b); }
    return r;
}

void vk_ngx_sr_jitter(uint64_t frame, uint32_t render_w, uint32_t out_w, float *jx, float *jy) {
    float ratio = render_w ? (float)out_w / (float)render_w : 1.f;
    uint32_t phases = (uint32_t)ceilf(8.f * ratio * ratio);
    if (phases < 8) phases = 8;
    uint32_t i = (uint32_t)(frame % phases) + 1;
    *jx = halton(i, 2) - 0.5f;
    *jy = halton(i, 3) - 0.5f;
}

// Texture LOD bias for the jittered render (our earlier Rust renderer: log2(render / output) - 1; 0 when not upscaling = DLAA, minus 1).
float vk_ngx_sr_mip_bias(uint32_t render_w, uint32_t out_w) {
    return log2f(out_w ? (float)render_w / (float)out_w : 1.f) - 1.f;
}

// The bias textures.c adds to the game's material textures with SR on (sampler_make's PC bias: CPU-uploaded 2D / 2D-array
// mipmapped textures only, never render targets, like the Options' texture LOD bias). Without it the scene's textures were
// sampled at the mips of the render size and DLSS had no finer detail to accumulate: soft textures under SR. From the
// scales as they are now (the ones vk_ngx_prepare makes the features with): the game's 1600x900 scene at the render
// scale in its 1920x1080 canvas at the output canvas scale (Auto: log2(900 / 1080) - 1 = -1.26; a lower Render
// resolution more). The samplers are re-made when it moves: SR turned on / off and the start's sizes (sr_bias_update),
// a render scale change (live-settings' samplers_tick on vk_render_epoch). None where DLSS will not run, as
// without DLSS a negative bias only aliases: a scene larger than the output (the game's pass draws it,
// vk_ngx_sr_upscale) or an SR feature that failed. TOTK_DLSS_MIP_BIAS=<x> sets it (0: none). FG alone (no jitter): none.
static bool g_sr_want;   // SR itself (g_sr_mode: the upscale pass is taken over, for SR or for FG's inputs; vk_ngx_sr_on)
uint64_t renderer_present_count(void);
void renderer_get_scale(float *sx, float *sy);
float renderer_out_scale(void);
int renderer_out_scale_peek(void);   // textures.c: the canvas decision if made (on), never making it
// (live-settings' samplers_tick re-asks after a vk_render_epoch move, at that present: answered from the sizes as they
// are now, not from the last vk_ngx_prepare's)
static float g_sr_bias_told;   // (the last value reported: sr_bias_update re-makes the samplers when it moves)
float vk_ngx_sr_lod_bias(void) {
    if (!vk_ngx_sr_on() || !g_sr_want || g_sr_fails > 3) return 0;
    static int env = -1;
    static float env_v;
    if (env < 0) { const char *e = getenv("TOTK_DLSS_MIP_BIAS"); env = e && *e; env_v = env ? (float)atof(e) : 0; }
    if (env) return env_v;
    float rx, ry;
    renderer_get_scale(&rx, &ry);
    // (renderer_out_scale_peek: never the canvas decision itself from a sampler, a few draws early; until it is made, x1,
    // and vk_ngx_prepare's sr_bias_update corrects it from the start's second present)
    const float os = renderer_out_scale_peek() ? renderer_out_scale() : 1.f;
    uint32_t rh = (uint32_t)ceilf(900.f * ry - 1e-3f), oh = (uint32_t)ceilf(1080.f * os - 1e-3f);
    return rh > oh ? 0 : vk_ngx_sr_mip_bias(rh, oh);
}
static void sr_bias_update(void) {   // (vk_ngx_prepare: SR turned on / off, the start's sizes known)
    float v = vk_ngx_sr_lod_bias();
    if (v == g_sr_bias_told) return;
    LOG("[dlss] SR texture LOD bias %+.2f (was %+.2f): samplers re-made\n", v, g_sr_bias_told);
    g_sr_bias_told = v;
    vk_samplers_rebuild();
}

static NVSDK_NGX_PerfQuality_Value sr_quality(uint32_t rw, uint32_t ow) {
    float r = rw ? (float)ow / (float)rw : 1.f;
    return r < 1.05f ? NVSDK_NGX_PerfQuality_Value_DLAA : r < 1.6f ? NVSDK_NGX_PerfQuality_Value_MaxQuality
         : r < 1.8f ? NVSDK_NGX_PerfQuality_Value_Balanced : r < 2.5f ? NVSDK_NGX_PerfQuality_Value_MaxPerf
         : NVSDK_NGX_PerfQuality_Value_UltraPerformance;
}

// The render range DLSS takes for quality q at output ow x oh (nvsdk_ngx_helpers.h NGX_DLSS_GET_OPTIMAL_SETTINGS, whose
// header pulls in the D3D helpers): false when NGX cannot say.
static bool sr_range(uint32_t ow, uint32_t oh, NVSDK_NGX_PerfQuality_Value q, unsigned *nw, unsigned *nh, unsigned *xw, unsigned *xh) {
    void *cbk = NULL;
    NVSDK_NGX_Parameter_GetVoidPointer(g_caps, NVSDK_NGX_Parameter_DLSSOptimalSettingsCallback, &cbk);
    if (!cbk) return false;
    NVSDK_NGX_Parameter_SetUI(g_caps, NVSDK_NGX_Parameter_Width, ow);
    NVSDK_NGX_Parameter_SetUI(g_caps, NVSDK_NGX_Parameter_Height, oh);
    NVSDK_NGX_Parameter_SetI(g_caps, NVSDK_NGX_Parameter_PerfQualityValue, q);
    NVSDK_NGX_Parameter_SetI(g_caps, NVSDK_NGX_Parameter_RTXValue, 0);
    if (NVSDK_NGX_FAILED(((PFN_NVSDK_NGX_DLSS_GetOptimalSettingsCallback)cbk)(g_caps))) return false;
    unsigned w = 0, h = 0;
    NVSDK_NGX_Parameter_GetUI(g_caps, NVSDK_NGX_Parameter_OutWidth, &w);
    NVSDK_NGX_Parameter_GetUI(g_caps, NVSDK_NGX_Parameter_OutHeight, &h);
    *nw = *xw = w; *nh = *xh = h;   // (older DLLs: optimal only)
    NVSDK_NGX_Parameter_GetUI(g_caps, NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Max_Render_Width, xw);
    NVSDK_NGX_Parameter_GetUI(g_caps, NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Max_Render_Height, xh);
    NVSDK_NGX_Parameter_GetUI(g_caps, NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Min_Render_Width, nw);
    NVSDK_NGX_Parameter_GetUI(g_caps, NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Min_Render_Height, nh);
    return true;
}

// The quality for a feature rendering rw x rh: sr_quality's, or a lower one whose render range also covers the smallest
// scene seen (and rw x rh). *minw/*minh = the chosen mode's smallest render size (0 when NGX does not report ranges).
static NVSDK_NGX_PerfQuality_Value sr_pick(uint32_t rw, uint32_t rh, uint32_t ow, uint32_t oh, uint32_t *minw, uint32_t *minh) {
    static const NVSDK_NGX_PerfQuality_Value order[] = {NVSDK_NGX_PerfQuality_Value_DLAA, NVSDK_NGX_PerfQuality_Value_MaxQuality,
        NVSDK_NGX_PerfQuality_Value_Balanced, NVSDK_NGX_PerfQuality_Value_MaxPerf, NVSDK_NGX_PerfQuality_Value_UltraPerformance};
    NVSDK_NGX_PerfQuality_Value want = sr_quality(rw, ow);
    int i = 0;
    while (i < 4 && order[i] != want) i++;
    uint32_t lw = g_sr_seen_w && g_sr_seen_w < rw ? g_sr_seen_w : rw, lh = g_sr_seen_h && g_sr_seen_h < rh ? g_sr_seen_h : rh;
    *minw = *minh = 0;
    for (int j = i; g_caps && j < 5; j++) {
        unsigned xw, xh, nw, nh;
        if (!sr_range(ow, oh, order[j], &nw, &nh, &xw, &xh)) continue;
        if (j == i) { *minw = nw; *minh = nh; }
        if (nw <= lw && nh <= lh && xw >= rw && xh >= rh) { *minw = nw; *minh = nh; return order[j]; }
    }
    return want;
}

static NVSDK_NGX_Resource_VK sr_res(const VkNgxImage *im, bool rw, VkImageAspectFlags aspect) {
    VkImageSubresourceRange sr = {aspect, 0, 1, 0, 1};
    return NVSDK_NGX_Create_ImageView_Resource_VK(im->view, im->image, sr, im->format, im->w, im->h, rw);
}

// The feature of the other output size. TotK's menus draw the scene into a 960x540 canvas behind them: a feature whose
// output changes is kept here, and swapped back when that output comes again, so going in and out of a menu costs
// nothing after the first time.
typedef struct { NVSDK_NGX_Handle *h; uint32_t rw, rh, ow, oh, minw, minh; int flags; } SrFeature;
static SrFeature g_sr_spare;

static SrFeature sr_current(void) {
    return (SrFeature){g_sr, g_sr_rw, g_sr_rh, g_sr_ow, g_sr_oh, g_sr_minw, g_sr_minh, g_sr_flags};
}

static void sr_set(SrFeature f) {
    g_sr = f.h; g_sr_rw = f.rw; g_sr_rh = f.rh; g_sr_ow = f.ow; g_sr_oh = f.oh; g_sr_minw = f.minw; g_sr_minh = f.minh;
    g_sr_flags = f.flags;
}

static void sr_release(NVSDK_NGX_Handle *h) {
    if (!h) return;
    vk_wait_seq(vk_pending_seq() - 1);   // (its last evaluate is done)
    NVSDK_NGX_VULKAN_ReleaseFeature(h);
}

static bool sr_create(VkCommandBuffer cb, uint32_t rw, uint32_t rh, uint32_t ow, uint32_t oh, int flags) {
    if (!g_sr_par && NVSDK_NGX_FAILED(NVSDK_NGX_VULKAN_AllocateParameters(&g_sr_par))) { g_sr_fails = 99; return false; }
    if (g_sr && (ow != g_sr_ow || oh != g_sr_oh)) {   // (another output: kept as the spare)
        sr_release(g_sr_spare.h);
        g_sr_spare = sr_current();
    } else sr_release(g_sr);
    g_sr = NULL;
    NVSDK_NGX_DLSS_Create_Params cp;
    memset(&cp, 0, sizeof cp);
    cp.Feature.InWidth = rw;
    cp.Feature.InHeight = rh;
    cp.Feature.InTargetWidth = ow;
    cp.Feature.InTargetHeight = oh;
    uint32_t minw, minh;
    cp.Feature.InPerfQualityValue = sr_pick(rw, rh, ow, oh, &minw, &minh);
    cp.InFeatureCreateFlags = flags;
    uint64_t t0 = SDL_GetTicksNS();
    vkq_drain();
    NVSDK_NGX_Result r = NGX_VULKAN_CREATE_DLSS_EXT1(g_vk.dev, cb, 1, 1, &g_sr, g_sr_par, &cp);
    if (NVSDK_NGX_FAILED(r)) {
        LOG("[dlss] SR feature %ux%u -> %ux%u failed (0x%x)\n", rw, rh, ow, oh, (unsigned)r);
        g_sr = NULL; g_sr_fails++;
        return false;
    }
    g_sr_flags = flags; g_sr_rw = rw; g_sr_rh = rh; g_sr_ow = ow; g_sr_oh = oh; g_sr_minw = minw; g_sr_minh = minh;
    LOG("[dlss] SR feature %ux%u -> %ux%u (quality %d, subrects down to %ux%u%s, flags 0x%x) in %.1f ms\n", rw, rh, ow, oh,
        (int)cp.Feature.InPerfQualityValue, minw, minh, g_sr_exact ? ", not used" : minw ? "" : " unreported", (unsigned)flags,
        (SDL_GetTicksNS() - t0) / 1e6);
    return true;
}

static int sr_flags(bool hdr, bool depth_inverted) {
    return (hdr ? NVSDK_NGX_DLSS_Feature_Flags_IsHDR : 0) | NVSDK_NGX_DLSS_Feature_Flags_MVLowRes | NVSDK_NGX_DLSS_Feature_Flags_AutoExposure
         | (depth_inverted ? NVSDK_NGX_DLSS_Feature_Flags_DepthInverted : 0);
}

bool vk_ngx_sr_evaluate(VkCommandBuffer cb, const VkNgxImage *color, const VkNgxImage *depth, const VkNgxImage *mv,
                        const VkNgxImage *out, float jx, float jy, float mv_scale_x, float mv_scale_y, bool reset,
                        bool depth_inverted, bool hdr, const VkNgxImage *bias) {
    if (!g_ready || g_sr_fails > 3) return false;
    int flags = sr_flags(hdr, depth_inverted);
    bool swapped = false;
    if (g_sr && (out->w != g_sr_ow || out->h != g_sr_oh) && g_sr_spare.h && out->w == g_sr_spare.ow && out->h == g_sr_spare.oh) {
        SrFeature cur = sr_current();   // (back to the other output: its feature is kept)
        sr_set(g_sr_spare);
        g_sr_spare = cur;
        swapped = reset = true;
        static int nswap;
        if (++nswap <= 20) LOG("[dlss] SR feature %ux%u -> %ux%u back in use (kept: %ux%u -> %ux%u)\n", g_sr_rw, g_sr_rh,
                               g_sr_ow, g_sr_oh, cur.rw, cur.rh, cur.ow, cur.oh);
    }
    bool same = g_sr && out->w == g_sr_ow && out->h == g_sr_oh;
    if (!same || swapped || !g_sr_seen_w) g_sr_seen_w = color->w, g_sr_seen_h = color->h;
    if (color->w < g_sr_seen_w) g_sr_seen_w = color->w;
    if (color->h < g_sr_seen_h) g_sr_seen_h = color->h;
    bool sub_ok = !g_sr_exact && color->w >= g_sr_minw && color->h >= g_sr_minh;   // (a subrect NGX takes)
    bool whole = color->w == g_sr_rw && color->h == g_sr_rh;
    if (!same || flags != g_sr_flags || color->w > g_sr_rw || color->h > g_sr_rh || !(whole || sub_ok)) {
        bool grow = same && sub_ok;   // (grows only while subrects work: the largest scene seen so far)
        uint32_t rw = grow && g_sr_rw > color->w ? g_sr_rw : color->w, rh = grow && g_sr_rh > color->h ? g_sr_rh : color->h;
        if (!sr_create(cb, rw, rh, out->w, out->h, flags)) return false;
        reset = true;
    }
    NVSDK_NGX_Resource_VK rc = sr_res(color, false, VK_IMAGE_ASPECT_COLOR_BIT), rd = sr_res(depth, false, VK_IMAGE_ASPECT_DEPTH_BIT);
    NVSDK_NGX_Resource_VK rm = sr_res(mv, false, VK_IMAGE_ASPECT_COLOR_BIT), ro = sr_res(out, true, VK_IMAGE_ASPECT_COLOR_BIT);
    NVSDK_NGX_VK_DLSS_Eval_Params ep;
    memset(&ep, 0, sizeof ep);
    ep.Feature.pInColor = &rc;
    ep.Feature.pInOutput = &ro;
    ep.pInDepth = &rd;
    ep.pInMotionVectors = &rm;
    ep.InJitterOffsetX = jx;   // (the geometry's shift: see halton above)
    ep.InJitterOffsetY = jy;
    ep.InRenderSubrectDimensions.Width = color->w;
    ep.InRenderSubrectDimensions.Height = color->h;
    ep.InReset = reset;
    ep.InMVScaleX = mv_scale_x;
    ep.InMVScaleY = mv_scale_y;
    NVSDK_NGX_Resource_VK rb;
    if (bias) { rb = sr_res(bias, false, VK_IMAGE_ASPECT_COLOR_BIT); ep.pInBiasCurrentColorMask = &rb; }
    NVSDK_NGX_Result r = NGX_VULKAN_EVALUATE_DLSS_EXT(cb, g_sr, g_sr_par, &ep);
    if (NVSDK_NGX_FAILED(r)) {
        if ((color->w != g_sr_rw || color->h != g_sr_rh) && !g_sr_exact) {   // (the next frame makes a feature of its size)
            g_sr_exact = true;
            LOG("[dlss] SR evaluate of a %ux%u subrect of the %ux%u feature failed (0x%x): features follow the scene's size from now on\n",
                color->w, color->h, g_sr_rw, g_sr_rh, (unsigned)r);
            return false;
        }
        if (g_sr_fails++ < 4) LOG("[dlss] SR evaluate failed (0x%x, %ux%u of the %ux%u feature)\n", (unsigned)r, color->w, color->h,
                                  g_sr_rw, g_sr_rh);
        return false;
    }
    return true;
}

// ================================================================== TOTK_DLSS=sr: the game's upscale pass replaced by DLSS
// Motion vectors are made from depth (the game has no velocity buffer): each pixel's depth back to world space through
// this frame's camera, then into the last frame's screen = camera motion only (animated objects get none; DLSS's own
// heuristics have to cover them). One matrix does it: R = (P' V') (P V)^-1, from the two latched camera blocks.
// NDC <-> pixel follows the backend's conventions (exec.c set_viewport: y up in NDC through a negative-height viewport,
// ShaderKey.flip_y / depth_minus_one from the game's window origin and depth mode).
#include "shaders.h"

static void mat_from_cam(const float *c, double m[4][4]) {   // P * V (V: rows 0-2 of the block + [0 0 0 1]; P: rows 4-7)
    double V[4][4] = {{c[0], c[1], c[2], c[3]}, {c[4], c[5], c[6], c[7]}, {c[8], c[9], c[10], c[11]}, {0, 0, 0, 1}};
    const float *P = c + 16;
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) {
            double a = 0;
            for (int k = 0; k < 4; k++) a += P[i * 4 + k] * V[k][j];
            m[i][j] = a;
        }
}

static bool mat_inv(double a[4][4], double r[4][4]) {   // Gauss-Jordan with partial pivoting
    double t[4][8];
    for (int i = 0; i < 4; i++) for (int j = 0; j < 8; j++) t[i][j] = j < 4 ? a[i][j] : (j - 4 == i);
    for (int c = 0; c < 4; c++) {
        int pv = c;
        for (int i = c + 1; i < 4; i++) if (fabs(t[i][c]) > fabs(t[pv][c])) pv = i;
        if (fabs(t[pv][c]) < 1e-12) return false;
        if (pv != c) for (int j = 0; j < 8; j++) { double x = t[c][j]; t[c][j] = t[pv][j]; t[pv][j] = x; }
        double d = t[c][c];
        for (int j = 0; j < 8; j++) t[c][j] /= d;
        for (int i = 0; i < 4; i++) if (i != c) { double f = t[i][c]; for (int j = 0; j < 8; j++) t[i][j] -= f * t[c][j]; }
    }
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) r[i][j] = t[i][j + 4];
    return true;
}

typedef struct { VkImage img; VmaAllocation mem; VkImageView view; uint32_t w, h; VkFormat fmt; } OwnImage;
static VkPipeline g_mv_pipe;
static VkPipelineLayout g_mv_layout;
static VkDescriptorSetLayout g_mv_dsl;
static VkDescriptorPool g_mv_pool;
static VkDescriptorSet g_mv_sets[8];
static OwnImage g_mv, g_out, g_dz[2], g_bias;
static int g_bias_mode = -1;   // TOTK_DLSS_BIAS=0: no bias mask; TOTK_DLSS_SHOW=bias: the mask drawn over the frame (red)
static VkSampler g_mv_smp;
static int g_sr_mode = -1;   // TOTK_DLSS=sr: 1 (SR / DLAA on the upscale pass); TOTK_DLSS=1: NGX up only
static bool g_sr_last;       // evaluated last frame (exec.c jitters this frame's geometry only then)
static bool g_in_last;       // motion vectors and depth made last frame (with or without SR: FG's inputs)
static uint64_t g_in_n;      // how many times
static uint64_t g_sr_frame = UINT64_MAX, g_sr_n, g_sr_evals;   // g_sr_evals: the jitter phase of the scene being drawn
static float g_cam_eval[CAM_FLOATS];   // the camera of the last evaluate (motion vectors go from this one)
static uint64_t g_cam_eval_n;          // and its latch number
static uint64_t g_sr_rej[6];           // evaluates refused: size, no new camera, pipeline, singular, NGX, repeat
static const char *const k_rej[6] = {"colour / depth sizes differ", "no new G-buffer camera", "motion-vector pipeline", "singular camera", "NGX evaluate failed", "second upscale in a frame"};
static void sr_reject(int why, const char *detail) {
    g_sr_last = g_in_last = false;
    uint64_t n = ++g_sr_rej[why];
    if (n <= 3 || (n & (n - 1)) == 0) LOG("[dlss] SR not run (%s%s%s): %llu times\n", k_rej[why], detail ? ": " : "", detail ? detail : "", (unsigned long long)n);
}
static float g_jsx = 1.f, g_jsy = 1.f;   // TOTK_DLSS_JSIGN=<sx>,<sy>: sign of the jitter reported to DLSS (A/B)
// the last SR evaluate's inputs, for FG on the same frame
static TexBackend *g_fg_depth;
static uint32_t g_fg_ow, g_fg_oh;   // the canvas the scene went into (the upscale pass's target)
static float g_fg_jx, g_fg_jy;
static bool g_fg_reset, g_fg_inv, g_fg_flip, g_fg_zminus;
static float g_cam_eval_prev[CAM_FLOATS];
static VkFormat fg_storage_format(VkFormat f);   // (DLSS-G section)
static bool sr_ready(void) { return g_sr_ffx ? vk_ffx_sr_ready() : g_ready; }   // (the chosen backend is up)

bool vk_ngx_sr_on(void) {
    if (g_sr_mode < 0) {
        int sr = dlss_env("TOTK_DLSS_SR");
        bool fg = dlss_mode("fg") || dlss_mode("fgtest") || dlss_env("TOTK_DLSS_FG") == 1 || dlss_env("TOTK_FSR_FG") == 1;
        g_sr_ffx = !rt_fleet() && dlss_env("TOTK_FSR_SR") == 1;
        g_sr_dlaa = !g_sr_ffx && dlss_env("TOTK_DLSS_DLAA") == 1;
        g_sr_want = g_sr_ffx || (sr != 0 && (dlss_mode("sr") || dlss_mode("fg") || dlss_mode("fgtest") || sr == 1));
        g_sr_mode = g_sr_want || fg;
        if (g_sr_ffx) LOG("[dlss] SR through AMD FSR 3.1 (TOTK_FSR_SR, ffx.c)\n");
        const char *js = getenv("TOTK_DLSS_JSIGN");
        if (js) sscanf(js, "%f,%f", &g_jsx, &g_jsy);
        if (g_sr_want) LOG("[dlss] SR on: the game's upscale pass becomes DLSS Super Resolution (DLAA at 1:1); jitter sign %+g %+g\n", g_jsx, g_jsy);
        else if (g_sr_mode) LOG("[dlss] SR off (TOTK_DLSS_SR=0 or frame generation alone): the game's upscale pass draws; motion vectors and depth are made for FG\n");
    }
    return g_sr_mode == 1 && ((g_sr_want && sr_ready()) || (vk_ngx_fg_on()));
}

// DLAA (the Upscaler row's "dlaa", TOTK_DLSS_DLAA=1): the render scale DLSS wants, polled by totk_hooks.c output_scale at
// every present (live-settings): the game's 1600x900 scene drawn at its 1920x1080 canvas's output size, 1.2 x the canvas
// scale (at 1440p 1.6: the scene 2560x1440 = the output, and sr_quality picks DLAA). 0: no opinion, the Render resolution
// row's (its saved choice is untouched and comes back with DLAA off). Plain reads: the render thread, every present.
float vk_ngx_scene_scale(float out_scale) {
    if (g_sr_mode < 0) (void)vk_ngx_sr_on();
    return g_sr_dlaa && g_sr_want && !g_sr_ffx && g_ready ? 1.2f * out_scale : 0.f;
}

// The sub-pixel offset this frame's scene geometry is drawn with (render pixels, +x right, +y down the image); zero when
// DLSS did not run last frame (no jitter without the pass that removes it).
bool vk_ngx_sr_frame_jitter(uint64_t frame, uint32_t render_w, uint32_t out_w, float *jx, float *jy) {
    if (!vk_ngx_sr_on() || !g_sr_last) { *jx = *jy = 0; return false; }
    (void)frame;
    vk_ngx_sr_jitter(g_sr_evals, render_w, g_sr_ow ? g_sr_ow : out_w, jx, jy);   // (the output size of the live feature)
    return true;
}

// Also the "bias current colour" mask (DLSS: 1 = trust this frame's colour over the history): where the depth this pixel
// should have had last frame (reprojected) is not among last frame's depths around where it lands, the pixel shows
// something that moved (Link, an animal, a swinging weapon) or the background it uncovered; there the camera-only motion
// vectors are wrong and the history would smear. Compared as view distance, against the 3x3 neighbourhood (the two
// frames' jitter moves silhouettes by a pixel), 5 % apart at least. Each frame's raw depth is kept for the next (dz).
static const char k_mv_glsl[] =
    "#version 450\n"
    "layout(local_size_x = 8, local_size_y = 8) in;\n"
    "layout(set = 0, binding = 0) uniform sampler2D depth;\n"
    "layout(set = 0, binding = 1, rg16f) uniform writeonly image2D mv;\n"
    "layout(set = 0, binding = 2, r32f) uniform writeonly image2D dz_cur;\n"
    "layout(set = 0, binding = 3, r32f) uniform readonly image2D dz_prev;\n"
    "layout(set = 0, binding = 4, r8) uniform writeonly image2D bias;\n"
    "layout(push_constant) uniform P { mat4 reproj; vec4 sz; uvec4 fl; vec4 lp; } p;\n"
    "float dist(float n) { return abs((p.lp.y - n * p.lp.w) / (n * p.lp.z - p.lp.x)); }\n"
    "void main() {\n"
    "  ivec2 q = ivec2(gl_GlobalInvocationID.xy);\n"
    "  if (q.x >= int(p.sz.x) || q.y >= int(p.sz.y)) return;\n"
    "  float d = texelFetch(depth, q, 0).r;\n"
    "  imageStore(dz_cur, q, vec4(d));\n"
    "  vec2 uv = (vec2(q) + 0.5f) * p.sz.zw;\n"
    "  vec3 ndc = vec3(uv.x * 2.0f - 1.0f, p.fl.x != 0u ? uv.y * 2.0f - 1.0f : 1.0f - uv.y * 2.0f, p.fl.y != 0u ? d * 2.0f - 1.0f : d);\n"
    "  vec4 c = p.reproj * vec4(ndc, 1.0f);\n"
    "  vec2 m = vec2(0.0f);\n"
    "  float b = 0.0f;\n"
    "  if (c.w > 1e-6f) {\n"
    "    vec2 pn = c.xy / c.w;\n"
    "    vec2 puv = vec2(pn.x * 0.5f + 0.5f, p.fl.x != 0u ? pn.y * 0.5f + 0.5f : 0.5f - pn.y * 0.5f);\n"
    "    m = (puv - uv) * p.sz.xy;\n"
    "    ivec2 pq = ivec2(floor(puv * p.sz.xy));\n"
    "    if (p.fl.z != 0u && all(greaterThanEqual(pq, ivec2(1))) && all(lessThan(pq, ivec2(p.sz.xy) - 1))) {\n"
    "      float e = dist(c.z / c.w), lo = 1e30f, hi = 0.0f;\n"
    "      for (int j = -1; j <= 1; j++) for (int i = -1; i <= 1; i++) {\n"
    "        float pd = imageLoad(dz_prev, pq + ivec2(i, j)).r;\n"
    "        float z = dist(p.fl.y != 0u ? pd * 2.0f - 1.0f : pd);\n"
    "        lo = min(lo, z); hi = max(hi, z);\n"
    "      }\n"
    "      b = (e < lo * 0.95f || e > hi * 1.05f) ? 1.0f : 0.0f;\n"
    "    }\n"
    "  }\n"
    "  imageStore(mv, q, vec4(m, 0.0f, 0.0f));\n"
    "  imageStore(bias, q, vec4(b));\n"
    "}\n";

static bool mv_pipeline(void) {
    if (g_mv_pipe) return true;
    size_t nw;
    uint32_t *spv = vk_glsl_to_spirv(k_mv_glsl, STAGE_COMPUTE, "dlss_mv", &nw);
    if (!spv) return false;
    VkShaderModule m = vk_module(spv, nw);
    free(spv);
    VkDescriptorSetLayoutBinding b[5] = {{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}};
    for (uint32_t i = 1; i < 5; i++) b[i] = (VkDescriptorSetLayoutBinding){i, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL};
    VkDescriptorSetLayoutCreateInfo dl = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 5, .pBindings = b};
    VK_CHECK(vkCreateDescriptorSetLayout(g_vk.dev, &dl, NULL, &g_mv_dsl));
    VkPushConstantRange pc = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 112};
    VkPipelineLayoutCreateInfo pl = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &g_mv_dsl,
                                     .pushConstantRangeCount = 1, .pPushConstantRanges = &pc};
    VK_CHECK(vkCreatePipelineLayout(g_vk.dev, &pl, NULL, &g_mv_layout));
    VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                                                .module = m, .pName = "main"},
                                      .layout = g_mv_layout};
    VK_CHECK(vkCreateComputePipelines(g_vk.dev, VK_NULL_HANDLE, 1, &ci, NULL, &g_mv_pipe));
    vkDestroyShaderModule(g_vk.dev, m, NULL);
    VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 8}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 32}};
    VkDescriptorPoolCreateInfo dp = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 8, .poolSizeCount = 2, .pPoolSizes = ps};
    VK_CHECK(vkCreateDescriptorPool(g_vk.dev, &dp, NULL, &g_mv_pool));
    VkDescriptorSetLayout ls[8];
    for (int i = 0; i < 8; i++) ls[i] = g_mv_dsl;
    VkDescriptorSetAllocateInfo a = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = g_mv_pool, .descriptorSetCount = 8, .pSetLayouts = ls};
    VK_CHECK(vkAllocateDescriptorSets(g_vk.dev, &a, g_mv_sets));
    g_mv_smp = vk_default_sampler(false);
    return true;
}

// Our own storage image (motion vectors, DLSS output), recreated on a size change (the old one freed after its last use).
static void own_image(VkCommandBuffer cb, OwnImage *im, uint32_t w, uint32_t h, VkFormat fmt, VkImageUsageFlags usage) {
    if (im->img && im->w == w && im->h == h && im->fmt == fmt) return;
    if (im->img) { vk_defer_view(im->view); vk_defer_image(im->img, im->mem); im->img = VK_NULL_HANDLE; }
    VkImageCreateInfo ic = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = fmt,
                            .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    VK_CHECK(vmaCreateImage(g_vk.vma, &ic, &ai, &im->img, &im->mem, NULL));
    VkImageViewCreateInfo vc = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = im->img, .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                .format = fmt, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VK_CHECK(vkCreateImageView(g_vk.dev, &vc, NULL, &im->view));
    vk_image_init_layout(cb, im->img, VK_IMAGE_ASPECT_COLOR_BIT);
    im->w = w; im->h = h; im->fmt = fmt;
}

static bool depth_inverted(const float *c, bool zminus) {   // reversed Z: a far point gets a smaller depth than a near one
    const float *P = c + 16;
    float d[2], z[2] = {-1.f, -1000.f};   // (view space looks down -Z)
    for (int i = 0; i < 2; i++) {
        float n = P[10] * z[i] + P[11], w = P[14] * z[i] + P[15];
        float ndc = w != 0 ? n / w : 0;
        d[i] = zminus ? ndc * 0.5f + 0.5f : ndc;
    }
    return d[1] < d[0];
}

// The upscale draw, as DLSS: color = the pass's input (render size), depth = this frame's G-buffer depth (same size),
// out = the pass's target (copied from our RGBA16F output). Called outside any render pass on vk_cmd(); false = draw the
// game's pass instead (sizes disagree, no camera, NGX failure). A scene larger than the output (render scale above 1) is
// no upscale, which DLSS refuses (0xbad00005): the game's pass draws it, and the motion vectors and depth are still made
// for frame generation. The camera as FSR (and DLSS-G) takes it: the image's projection, near / far / vertical fov /
// aspect from it
static void cam_peff(const float *c, bool flip, bool zminus, double pe[4][4]);
static void cam_lens(const float *c, bool flip, bool zminus, VkTemporalInputs *ti) {
    cam_peff(c, flip, zminus, ti->view_to_clip);
    const float *P = c + 16;
    ti->near_ = fabsf(P[10] != 1.f ? P[11] / (P[10] - 1.f) : 0.1f);
    ti->far_ = fabsf(P[10] != -1.f ? P[11] / (P[10] + 1.f) : 10000.f);
    ti->fovy = P[5] ? 2.f * atanf(1.f / fabsf(P[5])) : 1.f;
    ti->aspect = P[0] ? fabsf(P[5] / P[0]) : 1.7778f;
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) ti->clip_to_prev_clip[i][j] = i == j;
}

bool vk_ngx_sr_upscale(VkCommandBuffer cb, TexBackend *color, TexBackend *depth, TexBackend *out, uint64_t frame,
                       bool flip_y, bool zminus) {
    if (!vk_ngx_sr_on() || !color || !depth || !out) return false;
    if (g_sr_frame == frame) { sr_reject(5, NULL); return false; }   // (once a frame)
    if (color->width != depth->width || color->height != depth->height) {
        char d[96];
        snprintf(d, sizeof d, "scene colour %ux%u, G-buffer depth %ux%u", color->width, color->height, depth->width, depth->height);
        sr_reject(0, d);
        return false;
    }
    if (!g_cam_n || g_cam_n == g_cam_eval_n) { sr_reject(1, NULL); return false; }
    if (!mv_pipeline()) { sr_reject(2, NULL); return false; }
    const float *c = g_cam[g_cam_n & 1];
    bool reset = g_cam_cut || !g_in_last || g_cam_n != g_cam_eval_n + 1;
    bool sr_can = g_sr_want && sr_ready() && color->width <= out->width && color->height <= out->height && g_sr_fails <= 3;
    static int told_off;
    if (!sr_can && g_sr_want && !told_off++)
        LOG("[dlss] SR off (scene %ux%u, output %ux%u%s): the game's pass draws; FG still gets motion vectors and depth\n",
            color->width, color->height, out->width, out->height, g_sr_fails > 3 ? ", NGX failed" : "");
    double A[4][4], Ai[4][4], B[4][4], R[4][4];
    mat_from_cam(c, A);
    mat_from_cam(reset ? c : g_cam_eval, B);
    if (!mat_inv(A, Ai)) { sr_reject(3, NULL); return false; }
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) { double s = 0; for (int k = 0; k < 4; k++) s += B[i][k] * Ai[k][j]; R[i][j] = s; }
    uint32_t w = color->width, h = color->height;
    own_image(cb, &g_mv, w, h, VK_FORMAT_R16G16_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    if (g_bias_mode < 0) {
        const char *e = getenv("TOTK_DLSS_BIAS"), *s = getenv("TOTK_DLSS_SHOW");
        g_bias_mode = e && !strcmp(e, "0") ? 0 : s && !strcmp(s, "bias") ? 2 : 1;
        LOG("[dlss] SR bias-current-colour mask %s\n", g_bias_mode == 0 ? "off (TOTK_DLSS_BIAS=0)" : g_bias_mode == 2 ? "on, drawn over the frame (TOTK_DLSS_SHOW=bias)" : "on");
    }
    bool dz_new = g_dz[0].w != w || g_dz[0].h != h;
    for (int i = 0; i < 2; i++) own_image(cb, &g_dz[i], w, h, VK_FORMAT_R32_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT);
    own_image(cb, &g_bias, w, h, VK_FORMAT_R8_UNORM, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    OwnImage *dzc = &g_dz[g_in_n & 1], *dzp = &g_dz[(g_in_n & 1) ^ 1];
    own_image(cb, &g_out, out->width, out->height, VK_FORMAT_R16G16B16A16_SFLOAT,
              VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    VkComponentMapping idm = {0};
    VkImageView dview = vk_image_view(depth, VK_IMAGE_VIEW_TYPE_2D, depth->format, 0, 1, 0, 1, idm, VK_IMAGE_ASPECT_DEPTH_BIT);
    VkImageView cview = vk_image_view(color, VK_IMAGE_VIEW_TYPE_2D, color->format, 0, 1, 0, 1, idm, VK_IMAGE_ASPECT_COLOR_BIT);
    VkDescriptorSet set = g_mv_sets[g_sr_n++ & 7];   // (a set is rewritten 8 frames after its last use)
    VkDescriptorImageInfo di[5] = {{g_mv_smp, dview, VK_IMAGE_LAYOUT_GENERAL}, {VK_NULL_HANDLE, g_mv.view, VK_IMAGE_LAYOUT_GENERAL},
                                   {VK_NULL_HANDLE, dzc->view, VK_IMAGE_LAYOUT_GENERAL}, {VK_NULL_HANDLE, dzp->view, VK_IMAGE_LAYOUT_GENERAL},
                                   {VK_NULL_HANDLE, g_bias.view, VK_IMAGE_LAYOUT_GENERAL}};
    VkWriteDescriptorSet wr[5];
    for (uint32_t i = 0; i < 5; i++)
        wr[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = i, .descriptorCount = 1,
                                       .descriptorType = i ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                       .pImageInfo = &di[i]};
    vkUpdateDescriptorSets(g_vk.dev, 5, wr, 0, NULL);
    struct { float m[16]; float sz[4]; uint32_t fl[4]; float lp[4]; } pc;
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) pc.m[j * 4 + i] = (float)R[i][j];   // (GLSL mat4: column-major)
    pc.sz[0] = (float)w; pc.sz[1] = (float)h; pc.sz[2] = 1.f / w; pc.sz[3] = 1.f / h;
    // the mask needs last frame's depth (dz of the previous evaluate, same size) and no cut
    pc.fl[0] = flip_y; pc.fl[1] = zminus; pc.fl[2] = g_bias_mode && !reset && !dz_new; pc.fl[3] = 0;
    const float *Pp = (reset ? c : g_cam_eval) + 16;   // (the previous camera's projection: its depth -> view distance)
    pc.lp[0] = Pp[10]; pc.lp[1] = Pp[11]; pc.lp[2] = Pp[14]; pc.lp[3] = Pp[15];
    vk_full_barrier(cb);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_mv_pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_mv_layout, 0, 1, &set, 0, NULL);
    vkCmdPushConstants(cb, g_mv_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
    vkCmdDispatch(cb, (w + 7) / 8, (h + 7) / 8, 1);
    vk_full_barrier(cb);
    vkq_drain();   // (NGX records into cb directly: everything queued before it first)
    float jx, jy;
    vk_ngx_sr_jitter(g_sr_evals, w, out->width, &jx, &jy);
    if (!g_sr_last) jx = jy = 0;   // (this frame was drawn without jitter)
    bool inv = depth_inverted(c, zminus);
    if (!sr_can) {   // (FG's inputs only; no jitter next frame)
        g_sr_frame = frame;
        g_sr_last = false;
        g_in_last = true;
        g_in_n++;
        memcpy(g_cam_eval_prev, g_cam_eval, sizeof g_cam_eval);
        memcpy(g_cam_eval, c, sizeof g_cam_eval);
        g_fg_depth = depth; g_fg_jx = jx; g_fg_jy = jy; g_fg_reset = reset; g_fg_inv = inv; g_fg_flip = flip_y; g_fg_zminus = zminus; g_fg_ow = out->width; g_fg_oh = out->height;
        g_cam_eval_n = g_cam_n;
        return false;
    }
    VkNgxImage ic = {color->image, cview, color->format, w, h}, idp = {depth->image, dview, depth->format, w, h};
    VkNgxImage im = {g_mv.img, g_mv.view, g_mv.fmt, w, h}, io = {g_out.img, g_out.view, g_out.fmt, g_out.w, g_out.h};
    VkNgxImage ib = {g_bias.img, g_bias.view, g_bias.fmt, w, h};
    bool ok;
    if (g_sr_ffx) {   // (AMD FSR 3.1, ffx.c: the same inputs)
        VkTemporalInputs ti;
        memset(&ti, 0, sizeof ti);
        ti.color = ic, ti.depth = idp, ti.mv = im, ti.out = io;
        if (g_bias_mode) ti.bias = ib;
        ti.jx = g_jsx * jx, ti.jy = g_jsy * jy, ti.mv_sx = ti.mv_sy = 1.f;
        ti.reset = reset, ti.depth_inverted = inv, ti.frame = frame;
        cam_lens(c, flip_y, zminus, &ti);
        ok = vk_ffx_sr_evaluate(cb, &ti);
        if (!ok) g_sr_fails++;
    } else ok = vk_ngx_sr_evaluate(cb, &ic, &idp, &im, &io, g_jsx * jx, g_jsy * jy, 1.f, 1.f, reset, inv, false, g_bias_mode ? &ib : NULL);
    if (!ok) {
        sr_reject(4, NULL);
        return false;
    }
    vk_full_barrier(cb);
    if (g_bias_mode == 2) {   // TOTK_DLSS_SHOW=bias: the mask (red) over DLSS's output, scaled to it
        VkImageBlit mb = {{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {{0, 0, 0}, {(int32_t)w, (int32_t)h, 1}},
                          {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {{0, 0, 0}, {(int32_t)g_out.w, (int32_t)g_out.h, 1}}};
        vkCmdBlitImage(cb, g_bias.img, VK_IMAGE_LAYOUT_GENERAL, g_out.img, VK_IMAGE_LAYOUT_GENERAL, 1, &mb, VK_FILTER_NEAREST);
        vk_full_barrier(cb);
    }
    VkImageBlit bl = {{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {{0, 0, 0}, {(int32_t)g_out.w, (int32_t)g_out.h, 1}},
                      {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {{0, 0, 0}, {(int32_t)out->width, (int32_t)out->height, 1}}};
    vkCmdBlitImage(cb, g_out.img, VK_IMAGE_LAYOUT_GENERAL, out->image, VK_IMAGE_LAYOUT_GENERAL, 1, &bl, VK_FILTER_NEAREST);
    vk_full_barrier(cb);
    vk_ngx_fg_keep_hudless(cb, out, frame);
    g_sr_frame = frame;
    g_sr_last = g_in_last = true;
    g_sr_evals++;
    g_in_n++;
    memcpy(g_cam_eval_prev, g_cam_eval, sizeof g_cam_eval);
    memcpy(g_cam_eval, c, sizeof g_cam_eval);
    g_fg_depth = depth; g_fg_jx = jx; g_fg_jy = jy; g_fg_reset = reset; g_fg_inv = inv; g_fg_flip = flip_y; g_fg_zminus = zminus; g_fg_ow = out->width; g_fg_oh = out->height;
    g_cam_eval_n = g_cam_n;
    static uint64_t nlog, nprinted;   // (every 300th evaluate and each reset, 200 lines at most)
    if ((nlog++ % 300 == 0 || reset) && nprinted++ < 200)
        LOG("[dlss] SR frame %llu: %ux%u -> %ux%u, depth %s%s, flip_y %d, jitter %+.3f %+.3f%s\n", (unsigned long long)frame,
            w, h, out->width, out->height, inv ? "inverted" : "forward", zminus ? " (-1..1)" : "", flip_y, jx, jy, reset ? ", reset" : "");
    return true;
}

// ================================================================== DLSS Frame Generation (DLSS-G). The SDK's C++ helper
// (nvsdk_ngx_helpers_dlssg_vk.h) uses default member initialisers, so the parameters are set by name here. TOTK_DLSS=fgtest (implies sr):
// each presented frame is replaced by the frame DLSS-G generates halfway between the last real frame and this one. Nothing is paced or
// presented twice yet: this is to judge the inputs (motion vectors, depth, the five matrices, jitter) by eye before the presenter work.
// The game's final image is copied into our own image of a storage-capable format of its size class, the generated frame is copied back
// over it, and the normal present draws. Conventions (our earlier Rust renderer, measured there): matrices in the SDK's row-major
// post-multiplied layout = a column-vector matrix stored column-major (arr[i][j] = M[j][i]); clip space of the image (y up = top row, z =
// the depth buffer's value), so the game's projection gets the backend's y flip and -1..1 -> 0..1 depth remap first; motion vectors in uv
// (ours are pixels: mvec scale 1/w, 1/h); jitter in clip space (2 jx / w, -2 jy / h); reset only with a camera cut. Menus and loading
// screens: FG runs only on a frame SR ran on (the game drew its scene) TotK's menus and loading screens draw none (hle_hid.cpp
// screen_track's "scene idle"), so they are shown as they are, once. The first FG frame after such a gap is a reset, so nothing is
// interpolated across a menu. HUD-less image (our earlier Rust renderer's DLSSG.HUDLess, without its separate UI layer: TotK draws the
// HUD into the same canvas the upscale pass writes): the canvas copied right after DLSS SR wrote the scene into it, before the HUD lands
// on top. DLSS-G takes what differs between it and the final image as UI and does not move it with the scene. TOTK_DLSS_HUDLESS=0: off.
// TOTK_DLSS_SHOW=hudless: every real frame shows the HUD-less copy instead (to check on screenshots that it is the same scene without the
// HUD; with a post pass between the upscale and the HUD, DLSS-G would take the whole frame as UI).
static int g_fg_mode = -1;
static NVSDK_NGX_Handle *g_fg;
static NVSDK_NGX_Parameter *g_fg_par;
static uint32_t g_fg_w, g_fg_h, g_fg_rw, g_fg_rh;
static VkFormat g_fg_fmt;
static bool g_fg_hl;                   // the live feature was created with the HUD-less image
// TOTK_DLSS_FG_FRAMES=n (1-3, at most what the GPU offers; default 1; the Options row graphics.frame_gen's x2 / x3 / x4): n
// generated frames per real one (DLSS 4 multi frame generation: one evaluate per MultiFrameIndex), shown at k / (n + 1) of
// the real interval. The pacer (hle_vi.cpp) may ask for fewer: the frame cap counts the frames shown, and real frames stay at
// 30 or more (vk_ngx_fg_set_active)
#define FG_MAX 3
static OwnImage g_fg_in, g_fg_out[FG_MAX], g_fg_hud;
static int g_fg_n = -1;
static volatile LONG g_fg_active;   // (the pacer's count, 1..g_fg_n; 0: g_fg_n)
static int g_fg_fails, g_fg_hl_mode = -1, g_fg_show = -1;
static uint64_t g_fg_id, g_fg_sr_evals;
static uint64_t g_fg_hud_frame = UINT64_MAX, g_fg_last_frame = UINT64_MAX;   // (present counts: HUD-less copy, last FG frame)
static VkImage g_fg_hud_src;           // the canvas the HUD-less copy came from

// The generated frames per real one the player asked for, at most what the GPU offers (x3 / x4 need multi frame
// generation: RTX 50; NGX says 1 elsewhere, and x2 is used). Once NGX is up (its capability map).
static void fg_set_frames(int want) {   // (NGX up: its capability map)
    int max = (int)cap_u(NVSDK_NGX_DLSSG_Parameter_MultiFrameCountMax, 1);
    g_fg_n = want < 1 ? 1 : want > FG_MAX ? FG_MAX : want;
    if (g_fg_n > max) g_fg_n = max < 1 ? 1 : max;
    LOG("[dlss] FG: %d generated frame%s per real frame (x%d; asked %d, GPU up to %d)%s\n", g_fg_n, g_fg_n > 1 ? "s" : "",
        g_fg_n + 1, want, max, g_fg_n < want ? ": this GPU has no multi frame generation for more" : "");
}
static int fg_frames(void) {
    if (g_fg_ffx) return 1;   // (FSR 3.1: one generated frame)
    if (g_fg_n < 0 && g_ready) {
        const char *e = getenv("TOTK_DLSS_FG_FRAMES");
        fg_set_frames(e && *e ? atoi(e) : 1);
    }
    return g_fg_n < 1 ? 1 : g_fg_n;
}

// hle_vi.cpp's pacer: how many to generate now (the frame cap counts the frames shown; fewer than asked only where the
// real frames would fall under 30)
int vk_ngx_fg_frames(void) { return vk_ngx_fg_on() ? fg_frames() : 0; }
void vk_ngx_fg_set_active(int n) { InterlockedExchange(&g_fg_active, n); }
static int fg_frames_now(void) {
    int n = fg_frames(), a = (int)InterlockedCompareExchange(&g_fg_active, 0, 0);
    return a >= 1 && a < n ? a : n;
}

bool vk_ngx_fg_on(void) {
    if (g_fg_mode < 0) {
        g_fg_ffx = !rt_fleet() && dlss_env("TOTK_FSR_FG") == 1 && !dlss_mode("fgtest");
        g_fg_mode = dlss_mode("fgtest") ? 2 : dlss_mode("fg") || dlss_env("TOTK_DLSS_FG") == 1 || g_fg_ffx ? 1 : 0;
        if (g_fg_ffx) LOG("[dlss] frame generation through AMD FSR 3.1 (TOTK_FSR_FG, ffx.c): x2\n");
        const char *h = getenv("TOTK_DLSS_HUDLESS"), *s = getenv("TOTK_DLSS_SHOW");
        g_fg_hl_mode = !(h && !strcmp(h, "0"));
        g_fg_show = s && !strcmp(s, "hudless");
        if (g_fg_mode == 2) LOG("[dlss] TOTK_DLSS=fgtest: every frame shown is DLSS-G's generated one (SR on too); inputs check only\n");
        if (g_fg_mode == 1) LOG("[dlss] frame generation on: DLSS-G frames shown between real frames\n");
        if (g_fg_mode) LOG("[dlss] FG HUD-less image %s%s\n", g_fg_hl_mode ? "on" : "off (TOTK_DLSS_HUDLESS=0)",
                           g_fg_show ? "; TOTK_DLSS_SHOW=hudless: real frames show it" : "");
    }
    return g_fg_mode && (g_fg_ffx ? vk_ffx_fg_ready() : g_ready);
}

static void copy_image(VkCommandBuffer cb, VkImage src, VkImage dst, uint32_t w, uint32_t h);
static VkFormat g_fg_pres_fmt;   // the presented image's storage twin (from the last FG frame; 0: none yet)

// Right after the scene went into the canvas (vk_ngx_sr_upscale, or exec.c after the game's own upscale draw when SR is
// off): keep the HUD-less image of this frame. TotK draws its UI into that canvas (B10G11R11 float) and a copy pass (fs
// 8d0bd2a0046fd06a) then moves it into the presented RGBA8 image, so the HUD-less image is the canvas as the scene left
// it, converted to the presented format the same way: a blit.
void vk_ngx_fg_keep_hudless(VkCommandBuffer cb, TexBackend *out, uint64_t frame) {
    if (!vk_ngx_fg_on() || !g_fg_hl_mode) return;
    VkFormat fmt = g_fg_pres_fmt ? g_fg_pres_fmt : VK_FORMAT_R8G8B8A8_UNORM;
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(g_vk.phys, out->format, &fp);
    bool same = fg_storage_format(out->format) == fmt;
    if (out->samples > 1 || (!same && !(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT))) {
        static int told;
        if (!told++) LOG("[dlss] FG HUD-less image: the canvas (%ux%u format %d, %u samples) cannot be copied: not kept\n",
                         out->width, out->height, (int)out->format, out->samples);
        return;
    }
    static int told_how;
    if (!told_how++) LOG("[dlss] FG HUD-less image: canvas %ux%u format %d -> format %d (%s)\n", out->width, out->height,
                         (int)out->format, (int)fmt, same ? "copied" : "blitted");
    own_image(cb, &g_fg_hud, out->width, out->height, fmt, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT
                                                           | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT);
    if (same) copy_image(cb, out->image, g_fg_hud.img, out->width, out->height);
    else {
        VkImageBlit bl = {{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {{0, 0, 0}, {(int32_t)out->width, (int32_t)out->height, 1}},
                          {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {{0, 0, 0}, {(int32_t)out->width, (int32_t)out->height, 1}}};
        vkCmdBlitImage(cb, out->image, VK_IMAGE_LAYOUT_GENERAL, g_fg_hud.img, VK_IMAGE_LAYOUT_GENERAL, 1, &bl, VK_FILTER_NEAREST);
    }
    vk_full_barrier(cb);
    g_fg_hud_frame = frame;
    g_fg_hud_src = out->image;
}

// After vk_ngx_fg_frame returned true (TOTK_DLSS=fg): b gets the generated frame (generated) or this frame's real image
// back (from our copy of it), for backend_present's two presents.
void vk_ngx_fg_show(VkCommandBuffer cb, TexBackend *b, int generated) {
    vk_full_barrier(cb);
    copy_image(cb, generated ? g_fg_out[generated - 1].img : g_fg_in.img, b->image, b->width, b->height);
    vk_full_barrier(cb);
}

static VkFormat fg_storage_format(VkFormat f) {   // same size class, storage-capable, copyable with vkCmdCopyImage
    switch (f) {
    case VK_FORMAT_R8G8B8A8_SRGB: case VK_FORMAT_R8G8B8A8_UNORM: return VK_FORMAT_R8G8B8A8_UNORM;
    case VK_FORMAT_B8G8R8A8_SRGB: case VK_FORMAT_B8G8R8A8_UNORM: return VK_FORMAT_B8G8R8A8_UNORM;
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return f;
    case VK_FORMAT_R16G16B16A16_SFLOAT: return f;
    default: return VK_FORMAT_UNDEFINED;
    }
}

// The game's projection as the image sees it: Peff = Z * F * P (F: the backend's y flip, Z: depth -1..1 -> 0..1)
static void cam_peff(const float *c, bool flip, bool zminus, double pe[4][4]) {
    const float *P = c + 16;
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) pe[i][j] = P[i * 4 + j];
    if (flip) for (int j = 0; j < 4; j++) pe[1][j] = -pe[1][j];
    if (zminus) for (int j = 0; j < 4; j++) pe[2][j] = 0.5 * pe[2][j] + 0.5 * pe[3][j];
}

static void cam_vp(const float *c, bool flip, bool zminus, double m[4][4]) {
    double pe[4][4], V[4][4] = {{c[0], c[1], c[2], c[3]}, {c[4], c[5], c[6], c[7]}, {c[8], c[9], c[10], c[11]}, {0, 0, 0, 1}};
    cam_peff(c, flip, zminus, pe);
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) { double s = 0; for (int k = 0; k < 4; k++) s += pe[i][k] * V[k][j]; m[i][j] = s; }
}

static void fg_set_mat(const char *name, double m[4][4], float store[4][4]) {   // arr[i][j] = M[j][i]
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) store[i][j] = (float)m[j][i];
    NVSDK_NGX_Parameter_SetVoidPointer(g_fg_par, name, store);
}

// Created at the largest motion / depth size seen so far, with DLSS-G's dynamic resolution on: a smaller scene is
// passed as subrects of that size (the SR feature does the same).
static bool fg_create(VkCommandBuffer cb, uint32_t w, uint32_t h, VkFormat fmt, uint32_t rw, uint32_t rh, bool hl) {
    bool same = g_fg && w == g_fg_w && h == g_fg_h && fmt == g_fg_fmt && hl == g_fg_hl;
    if (same && rw <= g_fg_rw && rh <= g_fg_rh) return true;
    if (same) { rw = rw > g_fg_rw ? rw : g_fg_rw; rh = rh > g_fg_rh ? rh : g_fg_rh; }
    if (!g_fg_par && NVSDK_NGX_FAILED(NVSDK_NGX_VULKAN_AllocateParameters(&g_fg_par))) { g_fg_fails = 99; return false; }
    if (g_fg) { vk_wait_seq(vk_pending_seq() - 1); NVSDK_NGX_VULKAN_ReleaseFeature(g_fg); g_fg = NULL; }
    NVSDK_NGX_Parameter *p = g_fg_par;
    NVSDK_NGX_Parameter_SetUI(p, NVSDK_NGX_Parameter_CreationNodeMask, 1);
    NVSDK_NGX_Parameter_SetUI(p, NVSDK_NGX_Parameter_VisibilityNodeMask, 1);
    NVSDK_NGX_Parameter_SetUI(p, NVSDK_NGX_Parameter_Width, w);
    NVSDK_NGX_Parameter_SetUI(p, NVSDK_NGX_Parameter_Height, h);
    NVSDK_NGX_Parameter_SetUI(p, NVSDK_NGX_DLSSG_Parameter_Width, w);
    NVSDK_NGX_Parameter_SetUI(p, NVSDK_NGX_DLSSG_Parameter_Height, h);
    NVSDK_NGX_Parameter_SetUI(p, NVSDK_NGX_DLSSG_Parameter_BackbufferFormat, (unsigned)fmt);
    NVSDK_NGX_Parameter_SetUI(p, NVSDK_NGX_DLSSG_Parameter_InternalWidth, rw);
    NVSDK_NGX_Parameter_SetUI(p, NVSDK_NGX_DLSSG_Parameter_InternalHeight, rh);
    NVSDK_NGX_Parameter_SetUI(p, NVSDK_NGX_DLSSG_Parameter_DynamicResolution, 1);
    NVSDK_NGX_Parameter_SetUI(p, NVSDK_NGX_DLSSG_Parameter_ResourceAlwaysProvided_Flags, hl ? NVSDK_NGX_DLSSG_ResourceFlags_HUDLess : 0);
    NVSDK_NGX_Parameter_SetUI(p, NVSDK_NGX_DLSSG_Parameter_ResourceNeverProvided_Flags,
                              (hl ? 0 : NVSDK_NGX_DLSSG_ResourceFlags_HUDLess) | NVSDK_NGX_DLSSG_ResourceFlags_UI | NVSDK_NGX_DLSSG_ResourceFlags_UIAlpha |
                              NVSDK_NGX_DLSSG_ResourceFlags_BidirectionalDistortionField | NVSDK_NGX_DLSSG_ResourceFlags_OutputReal |
                              NVSDK_NGX_DLSSG_ResourceFlags_OutputDisableInterpolation);
    NVSDK_NGX_Parameter_SetUI(p, NVSDK_NGX_DLSSG_Parameter_UserInterfaceRecompositionEnabled, 0);
    uint64_t t0 = SDL_GetTicksNS();
    vkq_drain();  // (NGX records into cb directly: everything queued before it first)
    NVSDK_NGX_Result r = NVSDK_NGX_VULKAN_CreateFeature1(g_vk.dev, cb, NVSDK_NGX_Feature_FrameGeneration, p, &g_fg);
    if (NVSDK_NGX_FAILED(r)) {
        LOG("[dlss] FG feature %ux%u (format %d, motion %ux%u) failed (0x%x)\n", w, h, (int)fmt, rw, rh, (unsigned)r);
        g_fg = NULL; g_fg_fails++;
        return false;
    }
    NVSDK_NGX_Parameter_SetUI(p, NVSDK_NGX_DLSSG_Parameter_ColorBuffersHDR, 0);
    NVSDK_NGX_Parameter_SetUI(p, NVSDK_NGX_DLSSG_Parameter_MvecJittered, 0);
    NVSDK_NGX_Parameter_SetUI(p, NVSDK_NGX_DLSSG_Parameter_MvecDilated, 0);
    NVSDK_NGX_Parameter_SetUI(p, NVSDK_NGX_DLSSG_Parameter_MenuDetectionEnabled, 0);
    NVSDK_NGX_Parameter_SetUI(p, NVSDK_NGX_DLSSG_Parameter_OrthoProjection, 0);
    NVSDK_NGX_Parameter_SetUI(p, NVSDK_NGX_DLSSG_Parameter_AutomodeOverrideReset, 0);
    NVSDK_NGX_Parameter_SetF(p, NVSDK_NGX_DLSSG_Parameter_MinRelativeLinearDepthObjectSeparation, 40.f);
    NVSDK_NGX_Parameter_SetF(p, NVSDK_NGX_DLSSG_Parameter_CameraPinholeOffsetX, 0.f);
    NVSDK_NGX_Parameter_SetF(p, NVSDK_NGX_DLSSG_Parameter_CameraPinholeOffsetY, 0.f);
    g_fg_w = w; g_fg_h = h; g_fg_fmt = fmt; g_fg_rw = rw; g_fg_rh = rh; g_fg_hl = hl;
    LOG("[dlss] FG feature %ux%u (format %d, motion and depth %ux%u%s) in %.1f ms\n", w, h, (int)fmt, rw, rh,
        hl ? ", HUD-less image" : "", (SDL_GetTicksNS() - t0) / 1e6);
    return true;
}

static void copy_image(VkCommandBuffer cb, VkImage src, VkImage dst, uint32_t w, uint32_t h) {
    VkImageCopy c = {{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {w, h, 1}};
    vkCmdCopyImage(cb, src, VK_IMAGE_LAYOUT_GENERAL, dst, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
}

// ================================================================== the game's FXAA under DLSS SR TotK's FXAA is two
// full-screen passes after the bloom (the rival totk-pc-recomp v0.3.1 found them by diffing frames with the game's own
// switch, ModelSceneExtensionParam EnableFXAA: runtime/vk_dlaa.c): one copies the scene into a scratch target, the other
// (fs 6dc5011326ac42a2 here) draws the anti-aliased result back from it. At the 1280x720 tier that draw also scales the
// scene into the canvas, and SR takes it whole (exec.c); at the 1600x900 tier it runs 1:1 before EASU, so DLSS was handed
// an FXAA'd scene: edges and sub-pixel detail already smeared, which the jittered history can no longer recover. DLSS
// anti-aliases by itself (the rival turns FXAA off with DLAA), so while SR runs that draw becomes a copy of its input: the
// scene as it was before FXAA, the same bytes the target already holds (the first pass copied it from there).
// TOTK_DLSS_FXAA=1 keeps the game's FXAA. false: draw it (other sizes or formats, SR not running).
bool vk_ngx_sr_fxaa_bypass(VkCommandBuffer cb, TexBackend *src, TexBackend *dst) {
    static int keep = -1;
    if (keep < 0) {
        const char *e = getenv("TOTK_DLSS_FXAA");
        keep = e && *e && strcmp(e, "0");
        if (keep) LOG("[dlss] TOTK_DLSS_FXAA=1: the game's FXAA stays under SR\n");
    }
    if (keep || !vk_ngx_sr_on() || !g_sr_want || !g_sr_last || !src || !dst || src == dst) return false;
    if (src->width != dst->width || src->height != dst->height || src->samples > 1 || dst->samples > 1 ||
        src->aspect != VK_IMAGE_ASPECT_COLOR_BIT || dst->aspect != VK_IMAGE_ASPECT_COLOR_BIT)
        return false;
    // (another format: FXAA draws. A blit would re-quantise the scene through the scratch's format)
    if (src->format != dst->format) return false;
    vk_full_barrier(cb);
    copy_image(cb, src->image, dst->image, dst->width, dst->height);
    vk_full_barrier(cb);
    static int told;
    if (!told++) LOG("[dlss] the game's FXAA (fs 6dc5011326ac42a2, %ux%u) left out while SR runs: DLSS gets the scene before it "
                     "(copied; TOTK_DLSS_FXAA=1 keeps it)\n", dst->width, dst->height);
    return true;
}

// ================================================================== the features made before the first scene Made at
// their first use, they stalled that frame: SR 501 ms + FG 979 ms on the title's first 3D frame. They are made instead
// at an early present, on the start's black screens, at the sizes the scene will have: output = the game's 1920x1080
// canvas at the output canvas scale (textures.c kind 3), scene, depth and motion = the game's 1600x900 scene at the
// render scale (kind 1), both rounded up like the targets (textures.c create_image_); forward depth (TotK's) and the
// presented RGBA8 image. A smaller (dynamic-resolution) scene is a subrect of these; anything else (another canvas: an
// ultrawide TOTK_OUTPUT_RES, a scene larger than 1600x900) makes its own at first use, as before. backend_present,
// outside any pass, once, from its third present (the scales are set at each present). TOTK_DLSS_PREPARE=0: at first use
// only.
static uint32_t scaled_up(uint32_t n, float s) {   // (textures.c create_image_'s rounding)
    uint32_t v = (uint32_t)ceilf((float)n * s - 1e-3f);
    return v ? v : 1;
}

// Live rows (pc_dlss.cpp: graphics.dlss, graphics.frame_gen and its x2 / x3 / x4): a row's apply posts the new state
// (vk_ngx_request, any thread) and the render thread takes it here, between frames, where every feature is made: NGX
// started if it was not (~1 s, once; DLSS off at the start, the NVIDIA extensions it needs were enabled anyway,
// vk_ngx_device_exts), a feature turned off released with its images, one turned on made at once at the scene's sizes
// (below), the texture bias and the samplers following SR. A render scale change (live-settings: vk_render_epoch) makes
// them again at the new sizes here too, not on the scene's frame. TOTK_DLSS=fgtest is not live.
static volatile LONG g_req_sr = -1, g_req_fg = -1;   // (-1: nothing asked; vk_ngx_request's values)
// sr: 0 off, 1 DLSS, 2 FSR; fg: 0 off, 1..3 DLSS generated frames, VK_NGX_FG_FSR FSR x2 (-1: unchanged)
void vk_ngx_request(int sr, int fg) {
    if (sr >= 0) InterlockedExchange(&g_req_sr, sr > 3 ? 1 : sr);
    if (fg >= 0) InterlockedExchange(&g_req_fg, fg == VK_NGX_FG_FSR ? fg : fg > FG_MAX ? FG_MAX : fg);
}

static void own_free(OwnImage *im) {
    if (im->img) { vk_defer_view(im->view); vk_defer_image(im->img, im->mem); }
    memset(im, 0, sizeof *im);
}

// (vk_ngx_prepare runs in backend_present, after this frame's SR evaluate went into the open submission: sr_release waits
// for the last submitted one only, so that submission goes out first. FG evaluates after prepare.)
void renderer_submit(void);
static void sr_release_live(NVSDK_NGX_Handle *h) {
    if (!h) return;
    renderer_submit();
    sr_release(h);
}

// A path's backend can only be one its machine has: DLSS needs NGX (started with the device, or started now if the
// NVIDIA extensions it needs were enabled at the start), FSR needs FidelityFX's DLL. A choice that cannot run is off, said.
static bool ngx_can(void) {
    if (g_ready) return true;
    if (!ngx_env() && !g_live_exts) {
        static int told;
        if (!told++) LOG("[dlss] DLSS turned on in game, but this GPU started without what NGX needs (not NVIDIA, or extensions missing)\n");
        return false;
    }
    return ngx_init();
}

static bool apply_request(void) {   // true: a feature was turned on or off, its backend or the count changed
    int sr = (int)InterlockedExchange(&g_req_sr, -1), fg = (int)InterlockedExchange(&g_req_fg, -1);
    if (sr < 0 && fg < 0) return false;
    (void)vk_ngx_on(), (void)vk_ngx_sr_on(), (void)vk_ngx_fg_on();   // (the start's state first)
    if (g_fg_mode == 2) fg = -1;
    bool want_sr = sr >= 0 ? sr != 0 : g_sr_want, sr_ffx = sr >= 0 ? sr == 2 : g_sr_ffx, dlaa = sr >= 0 ? sr == 3 : g_sr_dlaa;
    bool fg_ffx = fg >= 0 ? fg == VK_NGX_FG_FSR : g_fg_ffx;
    int want_fg = fg >= 0 ? (fg == VK_NGX_FG_FSR ? 1 : fg) : g_fg_mode ? fg_frames() : 0;
    if (want_sr && !(sr_ffx ? vk_ffx_sr_ready() : ngx_can())) want_sr = false;
    if (want_fg && !(fg_ffx ? vk_ffx_fg_ready() : ngx_can())) want_fg = 0;
    bool changed = false, ffx_off = false;
    if (want_sr != g_sr_want || (want_sr && sr_ffx != g_sr_ffx)) {   // (the old backend's SR goes)
        if (g_sr_want && !g_sr_ffx) {
            sr_release_live(g_sr);
            sr_release_live(g_sr_spare.h);
            g_sr = NULL, g_sr_flags = -1, g_sr_exact = false, g_sr_seen_w = g_sr_seen_h = 0;
            memset(&g_sr_spare, 0, sizeof g_sr_spare);
        }
        ffx_off |= g_sr_want && g_sr_ffx;
        if (!want_sr) own_free(&g_out);
        g_sr_want = want_sr, g_sr_ffx = want_sr && sr_ffx, g_sr_fails = 0, changed = true;
        g_sr_last = false;   // (no jitter until the upscaler has run again)
        LOG("[dlss] SR turned %s in game\n", !want_sr ? "off" : sr_ffx ? "on (FSR)" : dlaa ? "on (DLAA)" : "on (DLSS)");
    }
    dlaa = dlaa && g_sr_want && !g_sr_ffx;
    if (dlaa != g_sr_dlaa) {   // (the render scale follows at the next present: totk_hooks.c output_scale, vk_render_epoch)
        g_sr_dlaa = dlaa, changed = true;
        LOG("[dlss] DLAA %s in game: the scene %s\n", dlaa ? "on" : "off", dlaa ? "at the output's size" : "back at Render resolution");
    }
    if ((want_fg != 0) != (g_fg_mode != 0) || (want_fg && (fg_ffx != g_fg_ffx || (!fg_ffx && want_fg != g_fg_n)))) {
        if (g_fg && (!want_fg || fg_ffx)) { vk_wait_seq(vk_pending_seq() - 1); NVSDK_NGX_VULKAN_ReleaseFeature(g_fg); g_fg = NULL; }
        ffx_off |= g_fg_mode && g_fg_ffx && (!want_fg || !fg_ffx);
        if (!want_fg) {
            own_free(&g_fg_in), own_free(&g_fg_hud);
            for (int i = 0; i < FG_MAX; i++) own_free(&g_fg_out[i]);
            g_fg_hud_frame = UINT64_MAX;
        }
        g_fg_ffx = want_fg && fg_ffx;
        if (want_fg && !fg_ffx) fg_set_frames(want_fg);
        g_fg_mode = want_fg ? 1 : 0, g_fg_fails = 0, g_fg_last_frame = UINT64_MAX, changed = true;   // (the next FG frame: reset)
        LOG("[dlss] frame generation turned %s in game\n", !want_fg ? "off" : fg_ffx ? "on (FSR x2)" : "on (DLSS)");
    }
    if (ffx_off) {   // (FidelityFX's contexts, both: the one still wanted is made again below; the open frame out first)
        renderer_submit();
        vk_ffx_release();
        g_fg_ffx_hl = false;
    }
    g_sr_mode = g_sr_want || g_fg_mode;
    g_on = g_sr_mode;
    if (!g_sr_mode) {   // (nothing left that reads the motion vectors and depth)
        own_free(&g_mv), own_free(&g_dz[0]), own_free(&g_dz[1]), own_free(&g_bias);
        g_in_last = false, g_fg_depth = NULL;
    }
    return changed;
}

void vk_ngx_prepare(VkCommandBuffer cb, const TexBackend *b) {
    static int off = -1;
    static bool pending = true;   // (the start's; then each change)
    static uint32_t epoch;
    if (off < 0) {
        const char *e = getenv("TOTK_DLSS_PREPARE"), *wide = getenv("TOTK_OUTPUT_RES");
        off = (e && !strcmp(e, "0")) || (wide && *wide && strcmp(wide, "0"));
        epoch = vk_render_epoch();
    }
    if (apply_request()) pending = true, cb = vk_cmd();   // (it may have submitted the frame so far: sr_release_live)
    uint32_t ep = vk_render_epoch();
    if (ep != epoch) epoch = ep, pending = true;
    bool sr = vk_ngx_sr_on() && g_sr_want, fg = vk_ngx_fg_on();
    if (!pending || (!sr && !fg) || renderer_present_count() < 2) return;
    pending = false;
    float rx, ry, os = renderer_out_scale();
    renderer_get_scale(&rx, &ry);
    uint32_t ow = scaled_up(1920, os), oh = scaled_up(1080, os), rw = scaled_up(1600, rx), rh = scaled_up(900, ry);
    uint64_t t0 = SDL_GetTicksNS();
    bool sr_ok = false, fg_ok = false, made = false;
    if (!off && sr && g_sr_ffx && rw <= ow && rh <= oh) {
        renderer_submit();   // (a remade context releases the old one, which this frame's upscale used: out first)
        cb = vk_cmd();
        sr_ok = vk_ffx_sr_prepare(cb, rw, rh, ow, oh), made = true;
    }
    else if (!off && sr && rw <= ow && rh <= oh && !(g_sr && g_sr_rw == rw && g_sr_rh == rh && g_sr_ow == ow && g_sr_oh == oh)) {
        g_sr_seen_w = rw, g_sr_seen_h = rh;   // (a scene larger than the output is not upscaled: vk_ngx_sr_upscale)
        if (g_sr) {   // (sr_create releases the live feature or the spare: the open submission evaluated it, out first)
            renderer_submit();
            cb = vk_cmd();
        }
        sr_ok = sr_create(cb, rw, rh, ow, oh, sr_flags(false, false)), made = true;
    }
    // (the presented image's own format: the last FG frame's, this present's when canvas-sized, else TotK's RGBA8)
    VkFormat fmt = g_fg_pres_fmt;
    if (!fmt && b && b->width == ow && b->height == oh && b->samples <= 1) fmt = fg_storage_format(b->format);
    if (fmt == VK_FORMAT_UNDEFINED) fmt = VK_FORMAT_R8G8B8A8_UNORM;
    if (!off && fg && g_fg_ffx) {
        fg_ok = vk_ffx_fg_prepare(cb, ow, oh, rw, rh, fmt, g_fg_hl_mode ? fmt : VK_FORMAT_UNDEFINED), made = true;
        g_fg_ffx_hl = fg_ok && g_fg_hl_mode;
    }
    else if (!off && fg && !(g_fg && g_fg_w == ow && g_fg_h == oh && g_fg_rw >= rw && g_fg_rh >= rh))
        fg_ok = fg_create(cb, ow, oh, fmt, rw, rh, g_fg_hl_mode), made = true;
    if (made)
        LOG("[dlss] features made between frames (present %llu): output %ux%u (x%.3f), scene %ux%u (x%.3f)%s%s (FG format %d) in "
            "%.0f ms\n", (unsigned long long)renderer_present_count(), ow, oh, os, rw, rh, rx,
            sr ? sr_ok ? g_sr_ffx ? ", SR (FSR)" : ", SR" : ", SR failed" : "",
            fg ? fg_ok ? g_fg_ffx ? ", FG (FSR)" : ", FG" : ", FG failed" : "", (int)fmt, (SDL_GetTicksNS() - t0) / 1e6);
    sr_bias_update();
}

// backend_present, outside any pass, before the swapchain frame: b = the game's final image of this frame.
int vk_ngx_fg_frame(VkCommandBuffer cb, TexBackend *b) {
    if (!vk_ngx_fg_on() || !b || g_fg_fails > 3) return 0;
    if (g_in_n == g_fg_sr_evals || !g_in_last || !g_fg_depth) return 0;   // (this frame's motion vectors and depth are made)
    g_fg_sr_evals = g_in_n;
    // TotK's menus draw the scene into a smaller canvas (960x540) behind the menu: not the presented image, so the menu
    // would be interpolated as scene and there is no HUD-less image of its size. Such frames count as frames without a
    // scene
    if (g_fg_ow != b->width || g_fg_oh != b->height) {
        static int told;
        if (!told++) LOG("[dlss] FG off while the scene's canvas (%ux%u) is not the presented image (%ux%u): a menu over the scene\n",
                         g_fg_ow, g_fg_oh, b->width, b->height);
        return 0;
    }
    // a gap since the last FG frame (presents without a scene: a menu, a loading screen, held frames): start over
    bool gap = g_fg_last_frame == UINT64_MAX || g_sr_frame != g_fg_last_frame + 1;
    static uint64_t ngaps;
    if (gap && g_fg_last_frame != UINT64_MAX && ++ngaps <= 100)
        LOG("[dlss] FG resumes after %llu presents without a scene: reset (%llu gaps)\n",
            (unsigned long long)(g_sr_frame - g_fg_last_frame - 1), (unsigned long long)ngaps);
    g_fg_last_frame = g_sr_frame;
    bool reset = g_fg_reset || gap;
    VkFormat fmt = fg_storage_format(b->format);
    if (fmt == VK_FORMAT_UNDEFINED || b->samples > 1) {
        static int told;
        if (!told++) LOG("[dlss] FG off: presented image format %d (samples %u) has no storage twin here\n", (int)b->format, b->samples);
        g_fg_fails = 99;
        return 0;
    }
    g_fg_pres_fmt = fmt;
    uint32_t w = b->width, h = b->height, rw = g_mv.w, rh = g_mv.h;
    // the HUD-less copy is of this frame and matches the presented image (the canvas itself, or a copy of its size)
    bool hl = g_fg_hl_mode && g_fg_hud_frame == g_sr_frame && g_fg_hud.w == w && g_fg_hud.h == h && g_fg_hud.fmt == fmt;
    static int told_hl;
    if (!told_hl++) LOG("[dlss] FG HUD-less image: %s (canvas %ux%u, presented %ux%u format %d, %s)\n", hl ? "used" : "not used",
                        g_fg_hud.w, g_fg_hud.h, w, h, (int)b->format, g_fg_hud_src == b->image ? "the same image" : "another image");
    // a frame without the HUD-less image while the feature takes one: skipped (and the next one starts over), not a
    // 300 ms recreation; only when it stays missing is the feature made without it
    static int hl_miss;
    if (hl) hl_miss = 0;
    // (FSR too: ffx.c remakes its context, a device wait, whenever the HUD-less image comes or goes)
    else if ((g_fg_ffx ? g_fg_ffx_hl : g_fg && g_fg_hl) && ++hl_miss < 30) { g_fg_last_frame = UINT64_MAX; return 0; }
    if (!g_fg_ffx && !fg_create(cb, w, h, fmt, rw, rh, hl)) return 0;   // (FSR: ffx.c makes its context at the evaluate)
    own_image(cb, &g_fg_in, w, h, fmt, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_STORAGE_BIT);
    const int nact = fg_frames_now();   // (a new count, after a frame cap change: the history starts over)
    static int nlast;
    if (nact != nlast) {
        if (nlast) LOG("[dlss] FG: %d generated frame%s per real frame now (the frame cap)\n", nact, nact > 1 ? "s" : "");
        reset = true;
        nlast = nact;
    }
    for (int i = 0; i < fg_frames(); i++)   // (FSR: g_fg_n unset, one)
        own_image(cb, &g_fg_out[i], w, h, fmt, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    vk_full_barrier(cb);
    copy_image(cb, b->image, g_fg_in.img, w, h);
    vk_full_barrier(cb);
    // camera: this frame = the last SR evaluate's, previous = the one before it
    static float m_v2c[4][4], m_c2v[4][4], m_c2p[4][4], m_p2c[4][4], m_lens[4][4];
    double pe[4][4], pei[4][4], A[4][4], Ai[4][4], B[4][4], R[4][4], Ri[4][4], I[4][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}};
    const float *c = g_cam_eval, *pv = reset ? g_cam_eval : g_cam_eval_prev;
    cam_peff(c, g_fg_flip, g_fg_zminus, pe);
    cam_vp(c, g_fg_flip, g_fg_zminus, A);
    cam_vp(pv, g_fg_flip, g_fg_zminus, B);
    if (!mat_inv(pe, pei) || !mat_inv(A, Ai)) return 0;
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) { double s = 0; for (int k = 0; k < 4; k++) s += B[i][k] * Ai[k][j]; R[i][j] = s; }
    if (!mat_inv(R, Ri)) return 0;
    const float *P = c + 16;
    float n = P[10] != 1.f ? P[11] / (P[10] - 1.f) : 0.1f, f = P[10] != -1.f ? P[11] / (P[10] + 1.f) : 10000.f;
    if (g_fg_ffx) {   // (AMD FSR 3.1 frame generation, ffx.c: one frame into g_fg_out[0] from the same inputs)
        VkTemporalInputs ti;
        memset(&ti, 0, sizeof ti);
        VkComponentMapping fidm = {0};
        VkImageView fdv = vk_image_view(g_fg_depth, VK_IMAGE_VIEW_TYPE_2D, g_fg_depth->format, 0, 1, 0, 1, fidm, VK_IMAGE_ASPECT_DEPTH_BIT);
        ti.depth = (VkNgxImage){g_fg_depth->image, fdv, g_fg_depth->format, rw, rh};
        ti.mv = (VkNgxImage){g_mv.img, g_mv.view, g_mv.fmt, rw, rh};
        ti.backbuffer = (VkNgxImage){g_fg_in.img, g_fg_in.view, fmt, w, h};
        if (hl) ti.hudless = (VkNgxImage){g_fg_hud.img, g_fg_hud.view, g_fg_hud.fmt, w, h};
        ti.jx = g_fg_jx, ti.jy = g_fg_jy, ti.mv_sx = ti.mv_sy = 1.f;
        ti.reset = reset, ti.depth_inverted = g_fg_inv, ti.frame = g_fg_id + 1;
        cam_lens(c, g_fg_flip, g_fg_zminus, &ti);
        for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) ti.clip_to_prev_clip[i][j] = R[i][j];
        for (int i = 0; i < 3; i++) ti.cam_pos[i] = c[116 + i], ti.cam_right[i] = c[i], ti.cam_up[i] = c[4 + i], ti.cam_fwd[i] = -c[8 + i];
        VkNgxImage fo = {g_fg_out[0].img, g_fg_out[0].view, g_fg_out[0].fmt, w, h};
        vkq_drain();
        // (0: a failure, which ffx.c counts and logs itself, or a frame it skipped: no frame, and the next one resets)
        if (vk_ffx_fg_evaluate(cb, &ti, &fo, 1) < 1) { g_fg_last_frame = UINT64_MAX; return 0; }
        g_fg_ffx_hl = hl;
        ++g_fg_id;
        goto generated;
    }
    NVSDK_NGX_Parameter *p = g_fg_par;
    fg_set_mat(NVSDK_NGX_DLSSG_Parameter_CameraViewToClip, pe, m_v2c);
    fg_set_mat(NVSDK_NGX_DLSSG_Parameter_ClipToCameraView, pei, m_c2v);
    fg_set_mat(NVSDK_NGX_DLSSG_Parameter_ClipToPrevClip, R, m_c2p);
    fg_set_mat(NVSDK_NGX_DLSSG_Parameter_PrevClipToClip, Ri, m_p2c);
    fg_set_mat(NVSDK_NGX_DLSSG_Parameter_ClipToLensClip, I, m_lens);
    NVSDK_NGX_Parameter_SetF(p, NVSDK_NGX_DLSSG_Parameter_CameraNear, fabsf(n));
    NVSDK_NGX_Parameter_SetF(p, NVSDK_NGX_DLSSG_Parameter_CameraFar, fabsf(f));
    NVSDK_NGX_Parameter_SetF(p, NVSDK_NGX_DLSSG_Parameter_CameraFOV, P[5] ? 2.f * atanf(1.f / fabsf(P[5])) : 1.f);
    NVSDK_NGX_Parameter_SetF(p, NVSDK_NGX_DLSSG_Parameter_CameraAspectRatio, P[0] ? fabsf(P[5] / P[0]) : 1.7778f);
    NVSDK_NGX_Parameter_SetF(p, NVSDK_NGX_DLSSG_Parameter_CameraPosX, c[116]);
    NVSDK_NGX_Parameter_SetF(p, NVSDK_NGX_DLSSG_Parameter_CameraPosY, c[117]);
    NVSDK_NGX_Parameter_SetF(p, NVSDK_NGX_DLSSG_Parameter_CameraPosZ, c[118]);
    NVSDK_NGX_Parameter_SetF(p, NVSDK_NGX_DLSSG_Parameter_CameraRightX, c[0]);   // (view rows: right, up, back)
    NVSDK_NGX_Parameter_SetF(p, NVSDK_NGX_DLSSG_Parameter_CameraRightY, c[1]);
    NVSDK_NGX_Parameter_SetF(p, NVSDK_NGX_DLSSG_Parameter_CameraRightZ, c[2]);
    NVSDK_NGX_Parameter_SetF(p, NVSDK_NGX_DLSSG_Parameter_CameraUpX, c[4]);
    NVSDK_NGX_Parameter_SetF(p, NVSDK_NGX_DLSSG_Parameter_CameraUpY, c[5]);
    NVSDK_NGX_Parameter_SetF(p, NVSDK_NGX_DLSSG_Parameter_CameraUpZ, c[6]);
    NVSDK_NGX_Parameter_SetF(p, NVSDK_NGX_DLSSG_Parameter_CameraFwdX, -c[8]);
    NVSDK_NGX_Parameter_SetF(p, NVSDK_NGX_DLSSG_Parameter_CameraFwdY, -c[9]);
    NVSDK_NGX_Parameter_SetF(p, NVSDK_NGX_DLSSG_Parameter_CameraFwdZ, -c[10]);
    NVSDK_NGX_Parameter_SetF(p, NVSDK_NGX_DLSSG_Parameter_JitterOffsetX, 2.f * g_fg_jx / (float)rw);
    NVSDK_NGX_Parameter_SetF(p, NVSDK_NGX_DLSSG_Parameter_JitterOffsetY, -2.f * g_fg_jy / (float)rh);
    NVSDK_NGX_Parameter_SetF(p, NVSDK_NGX_DLSSG_Parameter_MvecScaleX, 1.f / (float)rw);
    NVSDK_NGX_Parameter_SetF(p, NVSDK_NGX_DLSSG_Parameter_MvecScaleY, 1.f / (float)rh);
    NVSDK_NGX_Parameter_SetUI(p, NVSDK_NGX_DLSSG_Parameter_DepthInverted, g_fg_inv);
    NVSDK_NGX_Parameter_SetUI(p, NVSDK_NGX_DLSSG_Parameter_CameraMotionIncluded, 1);
    NVSDK_NGX_Parameter_SetUI(p, NVSDK_NGX_DLSSG_Parameter_NotRenderingGameFrames, 0);
    NVSDK_NGX_Parameter_SetUI(p, NVSDK_NGX_DLSSG_Parameter_MultiFrameCount, (unsigned)nact);
    NVSDK_NGX_Parameter_SetULL(p, NVSDK_NGX_DLSSG_Parameter_BackbufferFrameID, ++g_fg_id);
    NVSDK_NGX_Parameter_SetUI(p, NVSDK_NGX_DLSSG_Parameter_InputBackbufferSubrectWidth, w);
    NVSDK_NGX_Parameter_SetUI(p, NVSDK_NGX_DLSSG_Parameter_InputBackbufferSubrectHeight, h);
    NVSDK_NGX_Parameter_SetUI(p, NVSDK_NGX_DLSSG_Parameter_OutputInterpolatedSubrectWidth, w);
    NVSDK_NGX_Parameter_SetUI(p, NVSDK_NGX_DLSSG_Parameter_OutputInterpolatedSubrectHeight, h);
    NVSDK_NGX_Parameter_SetUI(p, NVSDK_NGX_DLSSG_Parameter_MVecsSubrectWidth, rw);
    NVSDK_NGX_Parameter_SetUI(p, NVSDK_NGX_DLSSG_Parameter_MVecsSubrectHeight, rh);
    NVSDK_NGX_Parameter_SetUI(p, NVSDK_NGX_DLSSG_Parameter_DepthSubrectWidth, rw);
    NVSDK_NGX_Parameter_SetUI(p, NVSDK_NGX_DLSSG_Parameter_DepthSubrectHeight, rh);
    VkComponentMapping idm = {0};
    VkImageView dview = vk_image_view(g_fg_depth, VK_IMAGE_VIEW_TYPE_2D, g_fg_depth->format, 0, 1, 0, 1, idm, VK_IMAGE_ASPECT_DEPTH_BIT);
    VkImageSubresourceRange cr = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}, dr = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    NVSDK_NGX_Resource_VK rb = NVSDK_NGX_Create_ImageView_Resource_VK(g_fg_in.view, g_fg_in.img, cr, fmt, w, h, false);
    NVSDK_NGX_Resource_VK rd = NVSDK_NGX_Create_ImageView_Resource_VK(dview, g_fg_depth->image, dr, g_fg_depth->format, rw, rh, false);
    NVSDK_NGX_Resource_VK rm = NVSDK_NGX_Create_ImageView_Resource_VK(g_mv.view, g_mv.img, cr, g_mv.fmt, rw, rh, false);
    NVSDK_NGX_Parameter_SetVoidPointer(p, NVSDK_NGX_DLSSG_Parameter_Backbuffer, &rb);
    NVSDK_NGX_Parameter_SetVoidPointer(p, NVSDK_NGX_DLSSG_Parameter_Depth, &rd);
    NVSDK_NGX_Parameter_SetVoidPointer(p, NVSDK_NGX_DLSSG_Parameter_MVecs, &rm);
    NVSDK_NGX_Resource_VK rhl = NVSDK_NGX_Create_ImageView_Resource_VK(g_fg_hud.view, g_fg_hud.img, cr, fmt, w, h, false);
    NVSDK_NGX_Parameter_SetVoidPointer(p, NVSDK_NGX_DLSSG_Parameter_HUDLess, hl ? &rhl : NULL);
    NVSDK_NGX_Parameter_SetVoidPointer(p, NVSDK_NGX_DLSSG_Parameter_UI, NULL);
    vkq_drain();
    for (int i = 0; i < nact; i++) {   // (generated frame i + 1 of n; the reset with the first only)
        NVSDK_NGX_Resource_VK ro = NVSDK_NGX_Create_ImageView_Resource_VK(g_fg_out[i].view, g_fg_out[i].img, cr, fmt, w, h, true);
        NVSDK_NGX_Parameter_SetVoidPointer(p, NVSDK_NGX_DLSSG_Parameter_OutputInterpolated, &ro);
        NVSDK_NGX_Parameter_SetUI(p, NVSDK_NGX_DLSSG_Parameter_MultiFrameIndex, (unsigned)i + 1);
        NVSDK_NGX_Parameter_SetUI(p, NVSDK_NGX_DLSSG_Parameter_Reset, reset && i == 0);
        NVSDK_NGX_Result r = NVSDK_NGX_VULKAN_EvaluateFeature_C(cb, g_fg, p, NULL);
        if (NVSDK_NGX_FAILED(r)) {
            if (g_fg_fails++ < 4) LOG("[dlss] FG evaluate %d of %d failed (0x%x)\n", i + 1, nact, (unsigned)r);
            return 0;
        }
    }
generated:
    if (g_fg_mode == 2 || (g_fg_show && hl)) {   // fgtest: shown instead of the real frame; TOTK_DLSS_SHOW=hudless
        vk_full_barrier(cb);
        copy_image(cb, g_fg_mode == 2 ? g_fg_out[0].img : g_fg_hud.img, b->image, w, h);
        if (g_fg_mode == 1) copy_image(cb, g_fg_hud.img, g_fg_in.img, w, h);   // (the real present shows it too)
        vk_full_barrier(cb);
    }
    static uint64_t nlog, nprinted;
    if ((nlog++ % 300 == 0 || reset) && nprinted++ < 200)
        LOG("[dlss] FG frame %llu: %ux%u from motion %ux%u, near %.3f far %.0f, jitter %+.3f %+.3f%s%s\n", (unsigned long long)g_fg_id, w, h, rw, rh,
            fabsf(n), fabsf(f), g_fg_jx, g_fg_jy, reset ? ", reset" : "", hl ? ", HUD-less" : "");
    return g_fg_mode == 1 ? nact : 0;
}

#else   // !_WIN32: no NGX; every query answers "unavailable" and nothing is replaced
#include <stdbool.h>
#include "vk.h"
bool vk_ngx_on(void) { return false; }
bool vk_ngx_ready(void) { return false; }
uint32_t vk_ngx_instance_exts(const char **ext, uint32_t n, uint32_t room) { (void)ext; (void)room; return n; }
void vk_ngx_device_exts(const VkExtensionProperties *ep, uint32_t ne, const char **ext, uint32_t *n_ext, uint32_t room) {
    (void)ep; (void)ne; (void)ext; (void)n_ext; (void)room;
}
void vk_ngx_device_ready(void) {}
void vk_ngx_gbuffer_draw(const float *cb8, uint64_t size, uint32_t depth_w, uint32_t depth_h, uint64_t frame) {
    (void)cb8; (void)size; (void)depth_w; (void)depth_h; (void)frame;
}
void vk_ngx_sr_jitter(uint64_t frame, uint32_t render_w, uint32_t out_w, float *jx, float *jy) {
    (void)frame; (void)render_w; (void)out_w; *jx = *jy = 0;
}
float vk_ngx_sr_mip_bias(uint32_t render_w, uint32_t out_w) { (void)render_w; (void)out_w; return 0; }
float vk_ngx_sr_lod_bias(void) { return 0; }
bool vk_ngx_sr_evaluate(VkCommandBuffer cb, const VkNgxImage *color, const VkNgxImage *depth, const VkNgxImage *mv,
                        const VkNgxImage *out, float jx, float jy, float mv_scale_x, float mv_scale_y, bool reset,
                        bool depth_inverted, bool hdr, const VkNgxImage *bias) {
    (void)cb; (void)color; (void)depth; (void)mv; (void)out; (void)jx; (void)jy; (void)mv_scale_x; (void)mv_scale_y;
    (void)reset; (void)depth_inverted; (void)hdr; (void)bias;
    return false;
}
bool vk_ngx_sr_on(void) { return false; }
bool vk_ngx_sr_frame_jitter(uint64_t frame, uint32_t render_w, uint32_t out_w, float *jx, float *jy) {
    (void)frame; (void)render_w; (void)out_w; *jx = *jy = 0;
    return false;
}
bool vk_ngx_sr_upscale(VkCommandBuffer cb, TexBackend *color, TexBackend *depth, TexBackend *out, uint64_t frame,
                       bool flip_y, bool zminus) {
    (void)cb; (void)color; (void)depth; (void)out; (void)frame; (void)flip_y; (void)zminus;
    return false;
}
bool vk_ngx_fg_on(void) { return false; }
int vk_ngx_fg_frames(void) { return 0; }
void vk_ngx_fg_set_active(int n) { (void)n; }
void vk_ngx_prepare(VkCommandBuffer cb, const TexBackend *b) { (void)cb; (void)b; }
void vk_ngx_request(int sr, int fg) { (void)sr; (void)fg; }
float vk_ngx_scene_scale(float out_scale) { (void)out_scale; return 0.f; }
int vk_ngx_fg_frame(VkCommandBuffer cb, TexBackend *b) { (void)cb; (void)b; return 0; }
void vk_ngx_fg_show(VkCommandBuffer cb, TexBackend *b, int generated) { (void)cb; (void)b; (void)generated; }
void vk_ngx_fg_keep_hudless(VkCommandBuffer cb, TexBackend *out, uint64_t frame) { (void)cb; (void)out; (void)frame; }
bool vk_ngx_sr_fxaa_bypass(VkCommandBuffer cb, TexBackend *src, TexBackend *dst) { (void)cb; (void)src; (void)dst; return false; }
#endif
