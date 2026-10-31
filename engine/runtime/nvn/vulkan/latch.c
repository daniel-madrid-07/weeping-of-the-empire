// Mouse late latch. The game turns its camera by the mouse motion it read at its camera step, and that frame reaches the
// screen frames later. Here the finished scene (the canvas after the game's upscale draw or DLSS, before the HUD's first
// draw into it) is re-aimed by the motion since that step. A camera turn about the eye is an exact homography of the
// image, with no depth needed, so no constant buffer is patched: the game's TAA history, culling and motion vectors stay
// its own (patching the view in VS cb8, the 10-25 plan, would have desynced them). Aim / first-person only (flags bit
// 0), and off under DLSS frame generation; the screen edge a turn uncovers repeats the border pixels (clamp).
//
// Inputs (games/totk/include/totk/data_page.h, and the latch block): +80 f32 scale x +84 f32 rate x 70/30 = degrees per
// count, yaw and pitch alike (the pitch gain +128 = 70/51); +88 u32 DP_PRESENTS; +92 u32 BEAT (DP_PRESENTS at the last
// camera step); +96 / +104 f64 LAST x / y (the totals that step applied); +144 u32 seq (odd while written), +148 u32
// flags (bit 0: aim / Purah Pad camera / scope), +152 / +160 f64 live totals (the same counter), +168 i64 QPC, written at
// 1 kHz. LAST holds only the latest step, and the game thread runs ahead of the render thread, so it is sampled on the
// game thread at each present push (vk_late_latch_sample) into a ring keyed by BEAT, the latest sample of a BEAT winning
// (the veneer stores BEAT before LAST: a sample between the two stores pairs the new BEAT with the old totals, and a
// later sample of the same BEAT corrects it). The frame drawn after P presents is matched with the step BEAT = P +
// (DP_PRESENTS
// - presents pushed) - lag. The lag and the two turn signs are found from the frames themselves: the G-buffer camera's
// turn from one frame to the next against the consumed totals' change, each candidate scored, while aiming. Or
// LARECOMP_LATE_LATCH=1,<lag>,<yaw sign>,<pitch sign> fixes them.
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "../shim/las.h"
#include "vk.h"
#include "shaders.h"      // (vk_glsl_to_spirv / vk_module: undeclared, the build's -Wno-everything let C take them as int, and
#include "latch_math.h"  // the truncated SPIR-V pointer crashed the first warp in the driver

extern void *totk_data_page_current(void);
uint64_t renderer_present_count(void);

enum { DP_SCALE = 80, DP_RATE = 84, DP_PRESENTS = 88, DP_BEAT = 92, DP_LAST = 96, DP_LSEQ = 144, DP_LFLAGS = 148, DP_LX = 152, DP_LY = 160 };
#define LL_MAX_DEG 6.0   // (a larger turn in one frame is a cut or a mismatch: no warp)

static int ll_on = -1, ll_lag = -1, ll_sx, ll_sy;   // (-1 / 0: calibrated)

bool vk_late_latch_on(void) {
    if (ll_on < 0) {
        const char *e = getenv("LARECOMP_LATE_LATCH");
        ll_on = e && *e && *e != '0';
        if (ll_on) {
            const char *p = strchr(e, ',');
            if (p) {
                ll_lag = atoi(p + 1);
                if (ll_lag < 0 || ll_lag >= LL_LAGS) ll_lag = -1;
                if ((p = strchr(p + 1, ','))) {
                    ll_sx = atoi(p + 1) < 0 ? -1 : 1;
                    if ((p = strchr(p + 1, ','))) ll_sy = atoi(p + 1) < 0 ? -1 : 1;
                }
            }
            LOG("[latch] late latch on: the scene re-aimed by the mouse motion its camera missed, aim only (lag %s, signs %s)\n",
                ll_lag >= 0 ? "set" : "calibrated", ll_sx && ll_sy ? "set" : "calibrated");
        }
    }
    return ll_on;
}

static inline uint32_t ld32(const uint8_t *p, int o) { return __atomic_load_n((const uint32_t *)(p + o), __ATOMIC_ACQUIRE); }
static inline double ldf(const uint8_t *p, int o) {
    uint64_t v = __atomic_load_n((const uint64_t *)(p + o), __ATOMIC_ACQUIRE);
    double d;
    memcpy(&d, &v, 8);
    return d;
}
static inline float ldf32(const uint8_t *p, int o) {
    uint32_t v = ld32(p, o);
    float f;
    memcpy(&f, &v, 4);
    return f;
}

// The latest camera step: LAST, BEAT, LAST, accepted when the two LAST reads match (the rule).
static bool read_step(const uint8_t *p, uint32_t *b, double *x, double *y) {
    for (int i = 0; i < 4; i++) {
        double x0 = ldf(p, DP_LAST), y0 = ldf(p, DP_LAST + 8);
        uint32_t bb = ld32(p, DP_BEAT);
        double x1 = ldf(p, DP_LAST), y1 = ldf(p, DP_LAST + 8);
        if (x0 == x1 && y0 == y1) { *b = bb; *x = x1; *y = y1; return true; }
    }
    return false;
}

// The live block (the reader: acquire the first seq, the data, an acquire fence, the second seq). false: being written
// four times over, or never written (TOTK_LATE_LATCH off).
static bool read_live(const uint8_t *p, uint32_t *fl, double *x, double *y) {
    for (int i = 0; i < 4; i++) {
        uint32_t s0 = ld32(p, DP_LSEQ);
        if (s0 & 1) continue;
        uint32_t f = __atomic_load_n((const uint32_t *)(p + DP_LFLAGS), __ATOMIC_RELAXED);
        uint64_t vx = __atomic_load_n((const uint64_t *)(p + DP_LX), __ATOMIC_RELAXED);
        uint64_t vy = __atomic_load_n((const uint64_t *)(p + DP_LY), __ATOMIC_RELAXED);
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (__atomic_load_n((const uint32_t *)(p + DP_LSEQ), __ATOMIC_RELAXED) != s0) continue;
        if (!s0) return false;
        *fl = f;
        memcpy(x, &vx, 8);
        memcpy(y, &vy, 8);
        return true;
    }
    return false;
}

// ---- the steps, sampled on the game thread
typedef struct { uint32_t seq, b, fl, pad; uint64_t x, y; } LlSlot;   // (x, y: f64 bits)
static LlSlot ll_ring[16];
static int64_t ll_off;        // DP_PRESENTS - presents pushed (0 when both count every present from the start)
static bool ll_off_set;

void vk_late_latch_sample(int64_t pushed) {
    if (!vk_late_latch_on()) return;
    const uint8_t *p = totk_data_page_current();
    if (!p) return;
    if (pushed >= 0) {
        int64_t o = (int64_t)ld32(p, DP_PRESENTS) - pushed;
        if (!ll_off_set || o != __atomic_load_n(&ll_off, __ATOMIC_RELAXED))
            LOG("[latch] present counters: DP_PRESENTS - presents pushed = %lld\n", (long long)o);
        __atomic_store_n(&ll_off, o, __ATOMIC_RELEASE);
        __atomic_store_n(&ll_off_set, true, __ATOMIC_RELEASE);
    }
    uint32_t b, fl = 0;
    double x, y, lx, ly;
    if (!read_step(p, &b, &x, &y)) return;
    if (!read_live(p, &fl, &lx, &ly)) fl = 0;
    LlSlot *s = &ll_ring[b & 15];
    uint32_t q = s->seq;
    __atomic_store_n(&s->seq, q + 1, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    uint64_t vx, vy;
    memcpy(&vx, &x, 8);
    memcpy(&vy, &y, 8);
    __atomic_store_n(&s->b, b, __ATOMIC_RELAXED);
    __atomic_store_n(&s->fl, fl, __ATOMIC_RELAXED);
    __atomic_store_n(&s->x, vx, __ATOMIC_RELAXED);
    __atomic_store_n(&s->y, vy, __ATOMIC_RELAXED);
    __atomic_store_n(&s->seq, q + 2, __ATOMIC_RELEASE);
}

static bool ring_get(uint32_t b, uint32_t *fl, double *x, double *y) {
    LlSlot *s = &ll_ring[b & 15];
    for (int i = 0; i < 4; i++) {
        uint32_t q = __atomic_load_n(&s->seq, __ATOMIC_ACQUIRE);
        if (q & 1) continue;
        uint32_t sb = __atomic_load_n(&s->b, __ATOMIC_RELAXED), sf = __atomic_load_n(&s->fl, __ATOMIC_RELAXED);
        uint64_t vx = __atomic_load_n(&s->x, __ATOMIC_RELAXED), vy = __atomic_load_n(&s->y, __ATOMIC_RELAXED);
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (__atomic_load_n(&s->seq, __ATOMIC_RELAXED) != q) continue;
        if (!q || sb != b) return false;
        *fl = sf;
        memcpy(x, &vx, 8);
        memcpy(y, &vy, 8);
        return true;
    }
    return false;
}

// ---- the frame's camera (VS cb8 at its first G-buffer draw, as ngx.c: rows 0-2 view, rows 4-7 projection) and the
// calibration (latch_math.h ll_cal_*: per lag and axis, the gain of the camera's turn on the consumed counts)
static float ll_cam[2][32];
static uint64_t ll_cam_pres = UINT64_MAX;   // (presents done when it was latched: the frame it belongs to)
static int ll_cam_i;
static LlCal ll_cal;

// The yaw's degrees per count. The pitch's differs (the veneer's pitch gain +128 is 70/51): the fit finds each axis's
// gain against this one k, ~0.59 for pitch.
static double deg_per_count(const uint8_t *p) { return (double)ldf32(p, DP_SCALE) * ldf32(p, DP_RATE) * 70.0 / 30.0; }

// The lag (-1: not known yet) and each axis's gain (0: that axis is not warped: unfitted, or the fit explains under 95 %
// of the camera's turn, as with a smoothed camera; ll_cal_use). The env's lag and signs, all three set, replace the fit
// with gains of +-1.
static int ll_model(double g[2]) {
    if (ll_lag >= 0 && ll_sx && ll_sy) { g[0] = ll_sx; g[1] = ll_sy; return ll_lag; }
    if (ll_cal.n < 30) return -1;
    int l = ll_lag >= 0 ? ll_lag : ll_cal_lag(&ll_cal);
    for (int a = 0; a < 2; a++) {
        double f = ll_cal_use(&ll_cal, l, a), s = a ? ll_sy : ll_sx;
        g[a] = s && f * s < 0 ? 0 : f;   // (a set sign the fit contradicts)
    }
    return l;
}

void vk_late_latch_camera(const float *cb8, uint64_t size, uint64_t pres_done) {
    if (!vk_late_latch_on() || size < 32 * 4 || pres_done == ll_cam_pres) return;   // (the frame's first G-buffer draw)
    bool next = ll_cam_pres != UINT64_MAX && pres_done == ll_cam_pres + 1;
    ll_cam_i ^= 1;
    float *c = ll_cam[ll_cam_i];
    const float *pc = ll_cam[ll_cam_i ^ 1];
    memcpy(c, cb8, sizeof ll_cam[0]);
    ll_cam_pres = pres_done;
    double m[2] = {ll_cam_yaw(c) - ll_cam_yaw(pc), ll_cam_pitch(c) - ll_cam_pitch(pc)};
    if (m[0] > 180) m[0] -= 360;
    if (m[0] < -180) m[0] += 360;
    if (!next || fabs(m[0]) >= 25 || fabs(m[1]) >= 25) return;   // (ngx.c's cut: a turn over 25 degrees)
    if (!__atomic_load_n(&ll_off_set, __ATOMIC_ACQUIRE) || (ll_lag >= 0 && ll_sx && ll_sy)) return;
    const uint8_t *p = totk_data_page_current();
    if (!p) return;
    double k = deg_per_count(p);
    if (!(k > 0)) return;
    // this frame used the step BEAT = P + off - lag, the frame before BEAT - 1
    int64_t b0 = (int64_t)pres_done + __atomic_load_n(&ll_off, __ATOMIC_ACQUIRE);
    double d[LL_LAGS][2], mx = 0;
    for (int l = 0; l < LL_LAGS; l++) {
        uint32_t f1, f0;
        double x1, y1, x0, y0;
        if (!ring_get((uint32_t)(b0 - l), &f1, &x1, &y1) || !ring_get((uint32_t)(b0 - l - 1), &f0, &x0, &y0) || !(f1 & f0 & 1)) return;
        d[l][0] = k * (x1 - x0);
        d[l][1] = k * (y1 - y0);
        mx = fmax(mx, fmax(fabs(d[l][0]), fabs(d[l][1])));
    }
    if (mx < 0.05) return;   // (the mouse still: nothing to tell the lags apart)
    ll_cal_add(&ll_cal, d, m);
    if (ll_cal.n == 30 || ll_cal.n % 600 == 0) {
        double g[2];
        int l = ll_model(g);
        LOG("[latch] calibration after %u aimed frames: lag %d (rms error a frame: lag 0 %.3f, 1 %.3f, 2 %.3f degrees); fitted gain "
            "yaw %.3f (explains %.0f %%), pitch %.3f (%.0f %%); warping %s; %.5f degrees a count\n", ll_cal.n, l, sqrt(ll_cal_resid(&ll_cal, 0) / ll_cal.n),
            sqrt(ll_cal_resid(&ll_cal, 1) / ll_cal.n), sqrt(ll_cal_resid(&ll_cal, 2) / ll_cal.n), ll_cal_gain(&ll_cal, l, 0),
            100 * ll_cal_r2(&ll_cal, l, 0), ll_cal_gain(&ll_cal, l, 1), 100 * ll_cal_r2(&ll_cal, l, 1),
            g[0] && g[1] ? "both axes" : g[0] ? "yaw only" : g[1] ? "pitch only" : "neither (fits under 95 % or unfitted)", k);
    }
}

// ---- the warp: a fullscreen triangle into the canvas, sampling a copy of it at H * (this pixel's NDC)
static const char k_ll_vs[] =
    "#version 450\n"
    "layout(location = 0) out vec2 uv;\n"
    "void main() {\n"
    "  vec2 q = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));\n"
    "  uv = q;\n"
    "  gl_Position = vec4(q * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n";
static const char k_ll_fs[] =
    "#version 450\n"
    "layout(location = 0) in vec2 uv;\n"
    "layout(location = 0) out vec4 o;\n"
    "layout(set = 0, binding = 0) uniform sampler2D src;\n"
    "layout(push_constant) uniform P { vec4 h0; vec4 h1; vec4 h2; vec4 fl; } p;\n"
    "vec3 lin(vec3 c) { return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(0.04045, c)); }\n"
    "vec3 enc(vec3 c) { return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(0.0031308, c)); }\n"
    "vec4 tap(ivec2 q, ivec2 sz) { vec4 t = texelFetch(src, clamp(q, ivec2(0), sz - 1), 0); return vec4(lin(t.rgb), t.a); }\n"
    "void main() {\n"
    "  vec3 n = vec3(uv.x * 2.0 - 1.0, p.fl.x != 0.0 ? uv.y * 2.0 - 1.0 : 1.0 - uv.y * 2.0, 1.0);\n"
    "  vec3 q = mat3(p.h0.xyz, p.h1.xyz, p.h2.xyz) * n;\n"
    "  vec2 s = q.xy / q.z;\n"
    "  vec2 suv = vec2(s.x * 0.5 + 0.5, p.fl.x != 0.0 ? s.y * 0.5 + 0.5 : 0.5 - s.y * 0.5);\n"
    "  if (p.fl.y == 0.0) { o = textureLod(src, suv, 0.0); return; }\n"
    "  ivec2 sz = textureSize(src, 0);\n"
    "  vec2 t = suv * vec2(sz) - 0.5, f = t - floor(t);\n"
    "  ivec2 i = ivec2(floor(t));\n"
    "  vec4 c = mix(mix(tap(i, sz), tap(i + ivec2(1, 0), sz), f.x), mix(tap(i + ivec2(0, 1), sz), tap(i + ivec2(1, 1), sz), f.x), f.y);\n"
    "  o = vec4(enc(clamp(c.rgb, 0.0, 1.0)), c.a);\n"
    "}\n";

static VkPipelineLayout ll_layout;
static VkDescriptorSetLayout ll_dsl;

static VkPipeline ll_pipeline(VkFormat fmt) {
    static struct { VkFormat f; VkPipeline p; } cache[4];
    static bool failed;
    for (int i = 0; i < 4; i++) if (cache[i].p && cache[i].f == fmt) return cache[i].p;
    if (failed) return VK_NULL_HANDLE;
    if (!ll_layout) {
        VkDescriptorSetLayoutBinding b = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL};
        VkDescriptorSetLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &b};
        VK_CHECK(vkCreateDescriptorSetLayout(g_vk.dev, &li, NULL, &ll_dsl));
        VkPushConstantRange r = {VK_SHADER_STAGE_FRAGMENT_BIT, 0, 64};
        VkPipelineLayoutCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &ll_dsl,
                                         .pushConstantRangeCount = 1, .pPushConstantRanges = &r};
        VK_CHECK(vkCreatePipelineLayout(g_vk.dev, &ci, NULL, &ll_layout));
    }
    size_t nv, nf;
    uint32_t *sv = vk_glsl_to_spirv(k_ll_vs, STAGE_VERTEX, "latch_vs", &nv), *sf = vk_glsl_to_spirv(k_ll_fs, STAGE_FRAGMENT, "latch_fs", &nf);
    if (!sv || !sf) {
        free(sv);
        free(sf);
        failed = true;
        LOG("[latch] warp shaders did not compile: late latch off\n");
        return VK_NULL_HANDLE;
    }
    VkShaderModule mv = vk_module(sv, nv), mf = vk_module(sf, nf);
    free(sv);
    free(sf);
    VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = mv, .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = mf, .pName = "main"}};
    VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                 .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo vs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                 .polygonMode = VK_POLYGON_MODE_FILL, .lineWidth = 1};
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                               .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState cba = {.colorWriteMask = 0xF};
    VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1,
                                              .pAttachments = &cba};
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 2, .pDynamicStates = dyn};
    VkPipelineRenderingCreateInfo ri = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = 1,
                                        .pColorAttachmentFormats = &fmt};
    VkPipelineDepthStencilStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkGraphicsPipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &ri, .stageCount = 2,
                                       .pStages = stages, .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vs,
                                       .pRasterizationState = &rs, .pMultisampleState = &ms, .pDepthStencilState = &ds,
                                       .pColorBlendState = &cb, .pDynamicState = &dy, .layout = ll_layout};
    VkPipeline p;
    VK_CHECK(vkCreateGraphicsPipelines(g_vk.dev, g_vk.pcache, 1, &pi, NULL, &p));
    vkDestroyShaderModule(g_vk.dev, mv, NULL);
    vkDestroyShaderModule(g_vk.dev, mf, NULL);
    for (int i = 0; i < 4; i++) if (!cache[i].p) { cache[i].f = fmt; cache[i].p = p; break; }
    return p;
}

// The canvas copy the warp samples (recreated on a size or format change, the old one freed after its last use)
static struct { VkImage img; VmaAllocation mem; VkImageView view; uint32_t w, h; VkFormat fmt; } ll_tmp;

static void tmp_image(VkCommandBuffer cb, uint32_t w, uint32_t h, VkFormat fmt) {
    if (ll_tmp.img && ll_tmp.w == w && ll_tmp.h == h && ll_tmp.fmt == fmt) return;
    if (ll_tmp.img) { vk_defer_view(ll_tmp.view); vk_defer_image(ll_tmp.img, ll_tmp.mem); }
    VkImageCreateInfo ic = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = fmt,
                            .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    VK_CHECK(vmaCreateImage(g_vk.vma, &ic, &ai, &ll_tmp.img, &ll_tmp.mem, NULL));
    VkImageViewCreateInfo vc = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = ll_tmp.img, .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                .format = fmt, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VK_CHECK(vkCreateImageView(g_vk.dev, &vc, NULL, &ll_tmp.view));
    vk_image_init_layout(cb, ll_tmp.img, VK_IMAGE_ASPECT_COLOR_BIT);
    ll_tmp.w = w; ll_tmp.h = h; ll_tmp.fmt = fmt;
}

static VkFormat raw_format(VkFormat f) {   // (sRGB canvases copied and drawn as UNORM: the bytes, no decode / encode)
    return f == VK_FORMAT_R8G8B8A8_SRGB ? VK_FORMAT_R8G8B8A8_UNORM : f == VK_FORMAT_B8G8R8A8_SRGB ? VK_FORMAT_B8G8R8A8_UNORM : f;
}

// 8-bit canvases hold gamma-encoded bytes (sRGB, or UNORM after the game's tonemap): the shader filters them in linear
// light (four taps decoded, blended, encoded) instead of bilinear on the bytes, which darkens edges
static bool gamma_bytes(VkFormat f) { return f == VK_FORMAT_R8G8B8A8_UNORM || f == VK_FORMAT_B8G8R8A8_UNORM; }

static struct {
    uint64_t frames, warped, at_present, still, subpx, no_aim, no_live, no_step, no_cam, big, gain_off, calib, fail;
    double sum_deg, max_deg, max_big;
} ll_st;

// TOTK_LATPROBE (the latency.py): a line for every frame the latch looks at: its TOTK_PRESENT_LOG 'p' index
// (renderer_present_count: the p row is written before it counts this present), when the live totals were read
// (SDL_GetTicksNS, the present log's clock) and the turn applied, or why none. latprobe dates t1 by the camera step, which
// the latch does not move: these lines date the warped image instead.
static void ll_probe(uint64_t ts, const char *why, double yaw, double pitch) {
    static int on = -1;
    if (on < 0) on = getenv("TOTK_LATPROBE") != NULL;
    if (on)
        LOG("[latch-f] %llu sampled %llu yaw %.4f pitch %.4f warped %s\n", (unsigned long long)renderer_present_count(),
            (unsigned long long)ts, yaw, pitch, why);
}

// at_present: no draw into the canvas came after the upscale (the HUD hidden), backend_present's call
void vk_late_latch_warp(VkCommandBuffer cb, TexBackend *canvas, uint64_t pres_done, bool flip_y, bool at_present) {
    if (!vk_late_latch_on() || !canvas || !canvas->image || canvas->samples > 1 || (canvas->aspect & ~VK_IMAGE_ASPECT_COLOR_BIT)) return;
    // DLSS frame generation builds its frames from the unwarped camera's motion vectors (and, with no HUD draw, from a
    // HUD-less image taken before the warp): generated frames would show the old view between two warped ones
    if (vk_ngx_fg_on()) {
        static int told;
        if (!told++) LOG("[latch] off while DLSS frame generation is on (its frames come from the unwarped camera)\n");
        return;
    }
    ll_st.frames++;
    if (ll_st.frames % 1200 == 0) {
        LOG("[latch] %llu frames: warped %llu (mean %.3f, max %.2f degrees; %llu at the present, no HUD draw), mouse still %llu, "
            "under half a pixel %llu, not aiming %llu, no live totals %llu, no step for the frame %llu, no camera %llu, over %.0f "
            "degrees %llu (largest %.1f), fits refused %llu, calibrating %llu, failed %llu\n",
            (unsigned long long)ll_st.frames, (unsigned long long)ll_st.warped, ll_st.warped ? ll_st.sum_deg / ll_st.warped : 0,
            ll_st.max_deg, (unsigned long long)ll_st.at_present, (unsigned long long)ll_st.still, (unsigned long long)ll_st.subpx,
            (unsigned long long)ll_st.no_aim, (unsigned long long)ll_st.no_live, (unsigned long long)ll_st.no_step,
            (unsigned long long)ll_st.no_cam, LL_MAX_DEG, (unsigned long long)ll_st.big, ll_st.max_big,
            (unsigned long long)ll_st.gain_off, (unsigned long long)ll_st.calib, (unsigned long long)ll_st.fail);
        uint64_t f = ll_st.frames;
        memset(&ll_st, 0, sizeof ll_st);
        ll_st.frames = f;
    }
    const uint8_t *p = totk_data_page_current();
    uint32_t fl, sfl;
    double lx, ly, sx_, sy_;
    uint64_t ts = SDL_GetTicksNS();
    if (!p || !read_live(p, &fl, &lx, &ly)) { ll_st.no_live++; ll_probe(ts, "no-live", 0, 0); return; }
    if (!(fl & 1)) { ll_st.no_aim++; ll_probe(ts, "not-aiming", 0, 0); return; }
    if (ll_cam_pres != pres_done) { ll_st.no_cam++; ll_probe(ts, "no-camera", 0, 0); return; }
    double g[2];
    int lag = ll_model(g);
    if (lag < 0 || !__atomic_load_n(&ll_off_set, __ATOMIC_ACQUIRE)) { ll_st.calib++; ll_probe(ts, "calibrating", 0, 0); return; }
    if (!g[0] && !g[1]) { ll_st.gain_off++; ll_probe(ts, "fit-refused", 0, 0); return; }
    uint32_t b = (uint32_t)((int64_t)pres_done + __atomic_load_n(&ll_off, __ATOMIC_ACQUIRE) - lag);
    if (!ring_get(b, &sfl, &sx_, &sy_) || !(sfl & 1)) { ll_st.no_step++; ll_probe(ts, "no-step", 0, 0); return; }
    double k = deg_per_count(p), yaw = g[0] * k * (lx - sx_), pitch = g[1] * k * (ly - sy_);
    double mag = sqrt(yaw * yaw + pitch * pitch);
    if (!(k > 0) || mag < 0.002) { ll_st.still++; ll_probe(ts, "still", yaw, pitch); return; }
    if (mag > LL_MAX_DEG) { ll_st.big++; ll_st.max_big = fmax(ll_st.max_big, mag); ll_probe(ts, "too-big", yaw, pitch); return; }
    double H[3][3];
    uint32_t w = canvas->width, h = canvas->height;
    if (!ll_homography(ll_cam[ll_cam_i], yaw, pitch, H)) { ll_st.fail++; ll_probe(ts, "failed", yaw, pitch); return; }
    if (ll_max_shift_px(H, w, h) < 0.5) { ll_st.subpx++; ll_probe(ts, "subpixel", yaw, pitch); return; }
    VkFormat fmt = raw_format(canvas->format);
    VkPipeline pipe = ll_pipeline(fmt);
    if (!pipe) { ll_st.fail++; ll_probe(ts, "failed", yaw, pitch); return; }
    tmp_image(cb, w, h, fmt);
    static const VkComponentMapping id = {0};
    VkImageView cv = vk_image_view(canvas, VK_IMAGE_VIEW_TYPE_2D, fmt, 0, 1, 0, 1, id, VK_IMAGE_ASPECT_COLOR_BIT);
    vk_full_barrier(cb);
    VkImageCopy rg = {{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {w, h, 1}};
    vkCmdCopyImage(cb, canvas->image, VK_IMAGE_LAYOUT_GENERAL, ll_tmp.img, VK_IMAGE_LAYOUT_GENERAL, 1, &rg);
    vk_full_barrier(cb);
    VkDescriptorSet ds = vk_alloc_set(ll_dsl);
    VkDescriptorImageInfo ii = {vk_default_sampler(true), ll_tmp.view, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet wr = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 0, .descriptorCount = 1,
                               .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii};
    vkUpdateDescriptorSets(g_vk.dev, 1, &wr, 0, NULL);
    VkRenderingAttachmentInfo ca = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = cv, .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
                                    .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, {w, h}}, .layerCount = 1,
                          .colorAttachmentCount = 1, .pColorAttachments = &ca};
    vkCmdBeginRendering(cb, &ri);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, ll_layout, 0, 1, &ds, 0, NULL);
    VkViewport vp = {0, 0, (float)w, (float)h, 0, 1};
    VkRect2D sc = {{0, 0}, {w, h}};
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &sc);
    float pc[16] = {0};
    for (int col = 0; col < 3; col++) for (int row = 0; row < 3; row++) pc[col * 4 + row] = (float)H[row][col];   // (GLSL mat3: columns)
    pc[12] = flip_y;
    pc[13] = gamma_bytes(fmt);
    vkCmdPushConstants(cb, ll_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, pc);
    vkCmdDraw(cb, 3, 1, 0, 0);
    vkCmdEndRendering(cb);
    vk_full_barrier(cb);
    ll_st.warped++;
    ll_probe(ts, "1", yaw, pitch);
    ll_st.at_present += at_present;
    ll_st.sum_deg += mag;
    if (mag > ll_st.max_deg) ll_st.max_deg = mag;
}
