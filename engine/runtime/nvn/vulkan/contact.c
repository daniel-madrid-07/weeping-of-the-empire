// contact.c: screen-space contact shadows (TOTK_CONTACT_SHADOWS = percent strength, 0 = off: the game's look).
//
// Adapted from totk-pc-recomp v0.3.1 runtime/vk_contact.c and runtime/shaders/contact_shadow.comp:
//   Copyright (c) 2026 Valiantsin Dzerakh, MIT License (THIRD_PARTY_NOTICES.md).
//
// The game fills a full-resolution G-buffer whose targets include a B10G11R11 scene colour, an R32F linear depth (view
// distance / far) and a D32F depth (is_gbuffer), and later composites the lighting into that scene colour in one pass
// whose colour 0 is the scene colour, with a D16 depth of its size, in the same frame (is_composite; the camera is
// copied there). When the pass after the composite
// begins, or a dispatch or copy comes first (contact_flush), a compute shader marches each pixel a short way toward the
// sun through the linear depth and darkens the scene colour where the ray passes behind nearby geometry: the small
// shadows at feet, under stones and in grass that the shadow maps are too coarse for. Transparent surfaces, the sky and
// post-processing come after it, untouched. Inputs from the fragment stage's uniform buffer c4 (0x940 bytes) of the
// newest pass: the projection scales c4[7].x and c4[8].y, the far plane c4[14].y and the first sun cascade's depth row
// c4[44].xyz, whose negation points from a view-space position toward the sun. The scene colour images get STORAGE usage
// while the setting is on (textures.c, contact_wants_storage). TOTK_CONTACT_DEBUG=1: the frame shows the factor in grey.
// Live (graphics.contact_shadows, contact_set_strength): the strength is read at each pass; the pipeline is made the
// first time it is on, and turning it on or off has textures.c remake the scene colour images with or without STORAGE
// between two frames (renderer_tex_remake), as a start with that value would have made them.
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../shim/las.h"
#include "../maxwell/shader.h"
#include "contact.h"
#include "shaders.h"
#include "vk.h"

#define NVN_B10G11R11 0x3F
#define NVN_R32F 0x0A
#define NVN_D16 0x32
#define NVN_D24 0x33
#define NVN_D32F 0x34

static struct {
    int tried, ok, strength, debug, logged, seen;
    VkDescriptorSetLayout dsl;
    VkPipelineLayout layout;
    VkPipeline pipe;
    TexBackend *scene, *depth;   // the last G-buffer's scene colour and linear depth (contact_forget drops them)
    uint32_t w, h;
    uint64_t frame;              // vk_frame_counter() of that G-buffer pass: a composite matches it only in the same frame
    bool pending;                // the composite into scene ran; the march waits for what follows it
    uint64_t c4;
    float cam[7];                // the camera at the composite: c4[7].x, c4[8].y, c4[14].y, c4[44].xyz (copied there)
    uint64_t passes;
} C;

static int g_strength = -1;   // (settings thread writes, the render thread reads; -1: TOTK_CONTACT_SHADOWS not read yet)
static int parse_strength(const char *e) {
    const int s = e && *e ? atoi(e) : 0;
    return s < 0 || s > 100 ? 0 : s;
}
int contact_strength(void) {
    int s = __atomic_load_n(&g_strength, __ATOMIC_ACQUIRE);
    if (s < 0) {
        int expect = -1;
        __atomic_compare_exchange_n(&g_strength, &expect, parse_strength(getenv("TOTK_CONTACT_SHADOWS")), false, __ATOMIC_ACQ_REL,
                                    __ATOMIC_ACQUIRE);
        s = __atomic_load_n(&g_strength, __ATOMIC_ACQUIRE);
    }
    return s;
}
void contact_set_strength(const char *v) {
    const int was = contact_strength(), s = parse_strength(v);
    __atomic_store_n(&g_strength, s, __ATOMIC_RELEASE);
    extern void renderer_tex_remake(void);   // (textures.c: the scene colours' STORAGE follows at the next present)
    if (!was != !s) renderer_tex_remake();
}

// (an r11f_g11f_b10f storage image needs shaderStorageImageExtendedFormats: without it the setting does nothing)
bool contact_wants_storage(VkFormat f) {
    return contact_strength() > 0 && g_vk.storage_ext && f == VK_FORMAT_B10G11R11_UFLOAT_PACK32;
}

static const char k_src[] =
    "#version 450\n"
    "layout(local_size_x = 8, local_size_y = 8) in;\n"
    "layout(set = 0, binding = 0) uniform sampler2D lin_depth;\n"
    "layout(set = 0, binding = 1, r11f_g11f_b10f) uniform image2D scene;\n"
    "layout(push_constant) uniform Params { vec4 proj; vec4 light; vec4 size; vec4 opt; } pc;\n"
    "vec3 view_pos(vec2 uv, float z) { vec2 ndc = uv * 2.0 - 1.0; return vec3(ndc.x * z / pc.proj.x, -ndc.y * z / pc.proj.y, -z); }\n"
    "vec2 to_uv(vec3 p) { float z = -p.z; return vec2(0.5 + 0.5 * p.x * pc.proj.x / z, 0.5 - 0.5 * p.y * pc.proj.y / z); }\n"
    "float ign(vec2 px) { return fract(52.9829189 * fract(dot(px, vec2(0.06711056, 0.00583715)))); }\n"
    "void main() {\n"
    "  ivec2 px = ivec2(gl_GlobalInvocationID.xy);\n"
    "  if (px.x >= int(pc.size.x) || px.y >= int(pc.size.y)) return;\n"
    "  float d = texelFetch(lin_depth, px, 0).x;\n"
    "  if (d <= 0.0 || d >= 0.999) return;\n"
    "  float z = d * pc.proj.z;\n"
    "  if (z > 80.0) return;\n"
    "  vec2 uv = (vec2(px) + 0.5) * pc.size.zw;\n"
    "  vec3 p = view_pos(uv, z);\n"
    "  float len = pc.light.w * clamp(z / 10.0, 0.4, 3.0);\n"
    "  float thick = 0.15 + z * 0.01;\n"
    "  const int STEPS = 16;\n"
    "  float step_len = len / float(STEPS), t = step_len * (0.25 + ign(vec2(px)));\n"
    "  float occ = 0.0;\n"
    "  for (int i = 0; i < STEPS; i++, t += step_len) {\n"
    "    vec3 q = p + pc.light.xyz * t;\n"
    "    if (q.z >= -0.05) break;\n"
    "    vec2 quv = to_uv(q);\n"
    "    if (any(lessThan(quv, vec2(0.0))) || any(greaterThan(quv, vec2(1.0)))) break;\n"
    "    float sz = texelFetch(lin_depth, ivec2(quv * pc.size.xy), 0).x * pc.proj.z;\n"
    "    float delta = -q.z - sz;\n"
    "    if (delta > 0.02 + z * 0.002 && delta < thick) { occ = 1.0 - float(i) / float(STEPS); break; }\n"
    "  }\n"
    "  float fade = 1.0 - smoothstep(40.0, 80.0, z);\n"
    "  float k = 1.0 - pc.proj.w * occ * fade;\n"
    "  if (pc.opt.x > 0.5) { imageStore(scene, px, vec4(k, k, k, 1.0)); return; }\n"
    "  if (k >= 0.999) return;\n"
    "  vec4 c = imageLoad(scene, px);\n"
    "  imageStore(scene, px, vec4(c.rgb * k, c.a));\n"
    "}\n";

static bool init(void) {
    C.tried = 1;
    C.strength = contact_strength();
    if (!C.strength) return false;
    if (!g_vk.storage_ext) {
        printf("[vk] contact shadows off: this GPU has no shaderStorageImageExtendedFormats\n");
        return false;
    }
    const char *e = getenv("TOTK_CONTACT_DEBUG");
    C.debug = e && *e && strcmp(e, "0");
    VkDescriptorSetLayoutBinding b[2] = {{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
                                         {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}};
    VkDescriptorSetLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 2, .pBindings = b};
    VkPushConstantRange pr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 64};
    size_t nw = 0;
    uint32_t *spv = vk_glsl_to_spirv(k_src, STAGE_COMPUTE, "contact_shadow", &nw);
    if (!spv || vkCreateDescriptorSetLayout(g_vk.dev, &li, NULL, &C.dsl) != VK_SUCCESS) {
        free(spv);
        printf("[vk] contact shadows off: their pipeline could not be created\n");
        return false;
    }
    VkPipelineLayoutCreateInfo pl = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &C.dsl,
                                     .pushConstantRangeCount = 1, .pPushConstantRanges = &pr};
    VkShaderModule m = vk_module(spv, nw);
    free(spv);
    VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = m, .pName = "main"}};
    bool ok = m && vkCreatePipelineLayout(g_vk.dev, &pl, NULL, &C.layout) == VK_SUCCESS &&
              (ci.layout = C.layout, vkCreateComputePipelines(g_vk.dev, VK_NULL_HANDLE, 1, &ci, NULL, &C.pipe) == VK_SUCCESS);
    if (m) vkDestroyShaderModule(g_vk.dev, m, NULL);
    if (!ok) {
        printf("[vk] contact shadows off: their pipeline could not be created\n");
        return false;
    }
    printf("[vk] contact shadows on, strength %d%%%s\n", C.strength, C.debug ? " (debug view)" : "");
    return C.ok = 1;
}

// A G-buffer pass: a B10G11R11 scene colour (with STORAGE), an R32F linear depth and a D32F depth, all one size, >= 640 wide
static bool is_gbuffer(const ContactPass *p, TexBackend **scene, TexBackend **depth) {
    *scene = *depth = NULL;
    if (p->w < 640 || (p->depth_nvn != NVN_D32F && p->depth_nvn != NVN_D24)) return false;   // (both D32_SFLOAT here)
    for (int i = 0; i < p->ncolor && i < 8; i++) {
        TexBackend *b = p->color[i];
        if (!b || b->width != p->w || b->height != p->h) continue;
        if (p->color_nvn[i] == NVN_B10G11R11 && !*scene) *scene = b;
        if (p->color_nvn[i] == NVN_R32F && !*depth) *depth = b;
    }
    return *scene && *depth && ((*scene)->usage & VK_IMAGE_USAGE_STORAGE_BIT);
}

// The lighting composite: the remembered scene colour as colour 0, with a D16 depth of its size.
static bool is_composite(const ContactPass *p) {
    return C.scene && C.frame == vk_frame_counter() && p->ncolor >= 1 && p->color[0] == C.scene && p->depth_nvn == NVN_D16 &&
           p->w == C.w && p->h == C.h;
}

void contact_forget(TexBackend *b) {
    if (b && (b == C.scene || b == C.depth)) C.scene = C.depth = NULL, C.pending = false;
}

bool contact_flush(VkCommandBuffer cb) {
    if (!C.pending) return false;
    C.pending = false;
    if (!C.scene || !C.depth) return false;
    float lx = -C.cam[3], ly = -C.cam[4], lz = -C.cam[5], ll = sqrtf(lx * lx + ly * ly + lz * lz);
    float px = C.cam[0], py = C.cam[1], zfar = C.cam[2];
    if (!(ll > 1e-6f) || !(px > 0) || !(py > 0) || !(zfar > 1)) return false;
    static const VkComponentMapping id = {0};
    VkImageView in = vk_image_view(C.depth, VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_R32_SFLOAT, 0, 1, 0, 1, id, VK_IMAGE_ASPECT_COLOR_BIT);
    VkImageView out = vk_image_view(C.scene, VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_B10G11R11_UFLOAT_PACK32, 0, 1, 0, 1, id,
                                    VK_IMAGE_ASPECT_COLOR_BIT);
    if (!in || !out) return false;
    float pcv[16] = {px, py, zfar, (float)C.strength / 100.0f, lx / ll, ly / ll, lz / ll, 0.6f,
                     (float)C.w, (float)C.h, 1.0f / (float)C.w, 1.0f / (float)C.h, (float)C.debug, 0, 0, 0};
    if (!C.logged) {
        C.logged = 1;
        printf("[vk] contact shadows on a %ux%u scene, sun toward %.3f %.3f %.3f (view space), far %g\n", C.w, C.h, pcv[4],
               pcv[5], pcv[6], zfar);
    }
    vk_full_barrier(cb);   // the composite's writes, before the march reads and writes them
    VkDescriptorSet ds = vk_alloc_set(C.dsl);
    VkDescriptorImageInfo ii = {vk_default_sampler(false), in, VK_IMAGE_LAYOUT_GENERAL}, oi = {VK_NULL_HANDLE, out, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet w[2] = {
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 0, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 1, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &oi}};
    vkUpdateDescriptorSets(g_vk.dev, 2, w, 0, NULL);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, C.pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, C.layout, 0, 1, &ds, 0, NULL);
    vkCmdPushConstants(cb, C.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pcv, pcv);
    vkCmdDispatch(cb, (C.w + 7) / 8, (C.h + 7) / 8, 1);
    vk_full_barrier(cb);   // what follows sees the darkened colour
    C.scene->snap_tag = 0;   // (written outside any draw: a feedback snapshot mirroring it is stale; exec.c bumps snap_ndgen)
    return true;
}

bool contact_pass_begin(const ContactPass *p) {
    const int s = contact_strength();
    if (!s) { C.scene = C.depth = NULL, C.pending = false; return false; }   // (turned off live: nothing remembered)
    if (!C.tried && !init()) return false;
    if (!C.ok) return false;
    C.strength = s;
    C.passes++;
    if (p->c4_addr && p->c4_size == 0x940) C.c4 = p->c4_addr;   // (the newest camera block seen at a pass start)
    TexBackend *scene, *depth;
    if (is_gbuffer(p, &scene, &depth)) {
        if (!C.seen++) printf("[vk] contact shadows: G-buffer pass %ux%u found\n", p->w, p->h);
        C.pending = false;
        C.scene = scene, C.depth = depth, C.w = p->w, C.h = p->h, C.frame = vk_frame_counter();
        return false;
    }
    if (is_composite(p)) {
        const float *u = (const float *)(uintptr_t)C.c4;   // (the camera now: the block may be rewritten before the flush)
        if (u) {
            const float cam[7] = {u[7 * 4 + 0], u[8 * 4 + 1], u[14 * 4 + 1], u[44 * 4 + 0], u[44 * 4 + 1], u[44 * 4 + 2], 0};
            memcpy(C.cam, cam, sizeof C.cam);
        }
        C.pending = u != NULL;
        return false;
    }
    if (C.passes == 20000 && !C.seen) printf("[vk] contact shadows: no G-buffer pass seen in 20000 passes: nothing drawn\n");
    return C.pending;   // the caller flushes before this pass begins
}
