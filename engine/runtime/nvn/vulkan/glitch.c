// One-frame glitch detector (TOTK_GLITCH_DETECT=1, default off LEAD_BRIEF_1103 P0: zero visual glitches, proven). Each presented game frame is reduced
// on the GPU to a 160x90 image (the window image blitted to 1280x720, kept in a ring of GL_N, then halved three times), read back once its submission
// completed, and averaged on the CPU into a 16x9 grid of luminance. Frame N is an anomaly when it differs from BOTH neighbours by at least
// TOTK_GLITCH_MIN (mean absolute difference over the grid, 0..1; default 0.02) while the neighbours match each other (their difference at most a
// quarter of that): a frame that reverts. A camera cut or a menu stays (its next frame differs from the one before too); a fade moves a little each
// frame. When the neighbours differ because the camera turns, the three are compared again at their best global shift (glitch_match.h gm_judge: +-8 x
// +-3 read-back pixels a frame = +-128 x +-48 at 2560 wide; integration's review: a 40 px/frame pan hid the see-through frame). Each one is logged
// '[glitch] frame N score S' with that frame's draws, skipped draws, passes, dispatches and occlusion reports (zero counts / kept visible), and its
// window image with both neighbours goes to build/glitch_<N>_{prev,frame,next}.png (GL_MAXLOG per session). Costs, when on: ~30 MB of VRAM, a few blits
// and a 57 KB read back a frame, and a short wait for each saved anomaly.
#include "vk.h"
#include "glitch_match.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GL_N 8        // presented frames kept (1280x720): the neighbours are still there when the next one is judged
#define GL_W 1280
#define GL_H 720
#define GL_SW GM_W    // the read-back image (160x90), compared as a 16x9 grid (glitch_match.h)
#define GL_SH GM_H
#define GL_MAXLOG 50
#define GL_NCTR 8

void vk_glitch_counters(uint64_t c[GL_NCTR]);   // exec.c: cumulative draws, skipped, passes, dispatches, oq reports / zero / kept visible

typedef struct {
    VkImage img;
    VmaAllocation mem;
    uint64_t frame, seq, ctr[GL_NCTR];
    bool used, pending, sig_ok;
    GmSum sum;   // (the read-back's luminance, summed: glitch_match.h)
} GlSlot;

static int g_gl_on = -1;
static float g_gl_min = 0.02f;
static GlSlot g_gl[GL_N];
static VkImage g_gl_h1, g_gl_h2, g_gl_sm;
static VmaAllocation g_gl_h1m, g_gl_h2m, g_gl_smm;
static VkBuffer g_gl_buf, g_gl_png;
static VmaAllocation g_gl_bufm, g_gl_pngm;
static uint8_t *g_gl_map, *g_gl_pngmap;
static bool g_gl_ready, g_gl_failed;
static int g_gl_logged;
static uint64_t g_gl_last_eval;
static struct { bool on; uint64_t seq, frame; } g_gl_pngq;

bool vk_glitch_on(void) {
    if (g_gl_on < 0) {
        const char *e = getenv("TOTK_GLITCH_DETECT");
        g_gl_on = e && *e == '1';
        const char *m = getenv("TOTK_GLITCH_MIN");
        if (m && *m) { float v = (float)atof(m); if (v > 0.001f && v < 1.0f) g_gl_min = v; }
        if (g_gl_on) LOG("[glitch] one-frame anomaly detector on (TOTK_GLITCH_DETECT=1): threshold %.3f, images to build/glitch_*.png\n", g_gl_min);
    }
    return g_gl_on == 1;
}

// The ring's format follows the window's: an sRGB window image blitted into UNORM would store linear values, and the
// threshold is calibrated on gamma-encoded frames (dark-ground differences shrink a lot in linear)
static VkFormat g_gl_fmt = VK_FORMAT_UNDEFINED;
static VkImage gl_image(VkCommandBuffer cb, uint32_t w, uint32_t h, VmaAllocation *mem) {
    VkImageCreateInfo ic = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = g_gl_fmt,
                            .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    VkImage img = VK_NULL_HANDLE;
    if (vmaCreateImage(g_vk.vma, &ic, &ai, &img, mem, NULL) != VK_SUCCESS) return VK_NULL_HANDLE;
    vk_image_init_layout(cb, img, VK_IMAGE_ASPECT_COLOR_BIT);   // (GENERAL from here on)
    return img;
}
static bool gl_buffer(VkDeviceSize size, VkBuffer *buf, VmaAllocation *mem, uint8_t **map) {
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                                  .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT};
    VmaAllocationInfo info;
    if (vmaCreateBuffer(g_vk.vma, &bi, &ai, buf, mem, &info) != VK_SUCCESS) return false;
    *map = (uint8_t *)info.pMappedData;
    return *map != NULL;
}
static bool gl_init(VkCommandBuffer cb) {
    if (g_gl_ready || g_gl_failed) return g_gl_ready;
    bool ok = true;
    for (int i = 0; i < GL_N && ok; i++) ok = (g_gl[i].img = gl_image(cb, GL_W, GL_H, &g_gl[i].mem)) != VK_NULL_HANDLE;
    ok = ok && (g_gl_h1 = gl_image(cb, GL_W / 2, GL_H / 2, &g_gl_h1m)) && (g_gl_h2 = gl_image(cb, GL_W / 4, GL_H / 4, &g_gl_h2m)) &&
         (g_gl_sm = gl_image(cb, GL_SW, GL_SH, &g_gl_smm));
    ok = ok && gl_buffer((VkDeviceSize)GL_N * GL_SW * GL_SH * 4, &g_gl_buf, &g_gl_bufm, &g_gl_map) &&
         gl_buffer((VkDeviceSize)3 * GL_W * GL_H * 4, &g_gl_png, &g_gl_pngm, &g_gl_pngmap);
    if (!ok) {
        g_gl_failed = true;
        LOG("[glitch] could not make its images / buffers: detector off\n");
        return false;
    }
    g_gl_ready = true;
    return true;
}

static void gl_layout(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                               .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
                               .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                               .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                               .oldLayout = from, .newLayout = to, .image = img, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
                               .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b};
    vkCmdPipelineBarrier2(cb, &di);
}
static void gl_blit(VkCommandBuffer cb, VkImage src, VkImageLayout sl, uint32_t sw, uint32_t sh, VkImage dst, uint32_t dw, uint32_t dh) {
    VkImageBlit r = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .srcOffsets = {{0, 0, 0}, {(int32_t)sw, (int32_t)sh, 1}},
                     .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstOffsets = {{0, 0, 0}, {(int32_t)dw, (int32_t)dh, 1}}};
    vkCmdBlitImage(cb, src, sl, dst, VK_IMAGE_LAYOUT_GENERAL, 1, &r, VK_FILTER_LINEAR);
    vk_full_barrier(cb);
}

static GlSlot *gl_find(uint64_t frame) {
    GlSlot *s = &g_gl[frame % GL_N];
    return s->used && s->frame == frame ? s : NULL;
}

// Completed read-backs into their grids, and the saved anomaly's images into PNGs (render thread, each presented frame)
static void gl_poll(void) {
    const uint64_t done = vk_completed_seq();
    for (int i = 0; i < GL_N; i++) {
        GlSlot *s = &g_gl[i];
        if (!s->pending || s->seq > done) continue;
        const uint8_t *px = g_gl_map + (size_t)i * GL_SW * GL_SH * 4;
        static float lum[GL_SW * GL_SH];
        for (int k = 0; k < GL_SW * GL_SH; k++) lum[k] = (0.2126f * px[4 * k] + 0.7152f * px[4 * k + 1] + 0.0722f * px[4 * k + 2]) / 255.0f;
        gm_sum(lum, &s->sum);
        s->pending = false, s->sig_ok = true;
    }
    if (g_gl_pngq.on && g_gl_pngq.seq <= done) {
        static const char *const part[3] = {"prev", "frame", "next"};
        char path[96];
        for (int k = 0; k < 3; k++) {
            snprintf(path, sizeof path, "build/glitch_%06llu_%s.png", (unsigned long long)g_gl_pngq.frame, part[k]);
            vk_save_png(path, g_gl_pngmap + (size_t)k * GL_W * GL_H * 4, GL_W, GL_H);
        }
        g_gl_pngq.on = false;
    }
}

// Judges each frame whose next one has its grid: logs an anomaly and asks for its images
static void gl_eval(VkCommandBuffer cb) {
    for (int i = 0; i < GL_N; i++) {
        const GlSlot *nx = &g_gl[i];
        if (!nx->used || !nx->sig_ok || nx->frame < 2) continue;
        const uint64_t f = nx->frame - 1;
        if (f <= g_gl_last_eval) continue;
        const GlSlot *s = gl_find(f), *pv = gl_find(f - 1);
        if (!s || !pv || !s->sig_ok || !pv->sig_ok) continue;
        g_gl_last_eval = f;
        float d[3];
        bool moved;
        const float score = gm_judge(&pv->sum, &s->sum, &nx->sum, g_gl_min, d, &moved);
        if (score <= 0) continue;
        const float dp = d[0], dn = d[1], dnb = d[2];
        uint64_t c[GL_NCTR];
        for (int k = 0; k < GL_NCTR; k++) c[k] = s->ctr[k] - pv->ctr[k];
        {   // Its AGE (fps360, 2026-11-03: the 'camera elsewhere' frames are stale images, an older frame's scene or a whole
            // older frame): which kept older frame N-k (k >= 2) it matches best, over the whole grid and over the central
            // cells only (the HUD sits on the edges: a central match with the edges unlike = an old scene under a current HUD)
            float bw = 1, bc = 1;
            int kw = 0, kc = 0;
            for (int k = 2; k < GL_N - 1; k++) {
                const GlSlot *o = gl_find(f - (uint64_t)k);
                if (!o || !o->sig_ok) continue;
                const float w = gm_dist(&s->sum, &o->sum, 0, 0), m = gm_dist_part(&s->sum, &o->sum, true);
                if (w < bw) bw = w, kw = k;
                if (m < bc) bc = m, kc = k;
            }
            const float pc = gm_dist_part(&s->sum, &pv->sum, true), pe = gm_dist_part(&s->sum, &pv->sum, false);
            const GlSlot *oc = kc ? gl_find(f - (uint64_t)kc) : NULL;
            const float oe = oc ? gm_dist_part(&s->sum, &oc->sum, false) : 1;
            LOG("[glitch]   frame %llu's age: whole frame closest to N-%d (%.3f, vs N-1 %.3f); centre closest to N-%d (%.3f, vs N-1 %.3f); "
                "edges / HUD vs N-1 %.3f, vs N-%d %.3f (%s)\n", (unsigned long long)f, kw, bw, dp, kc, bc, pc, pe, kc, oe,
                pe <= oe ? "HUD current" : "HUD old too");
        }
        LOG("[glitch] frame %llu score %.1f: differs %.3f / %.3f from its neighbours, which differ %.3f%s | draws %llu, skipped %llu, "
            "passes %llu, dispatches %llu | occlusion reports %llu: zero %llu, kept visible %llu%s\n", (unsigned long long)f, score,
            dp, dn, dnb, moved ? " (compared after the camera's move)" : "", (unsigned long long)c[0], (unsigned long long)c[1], (unsigned long long)c[2], (unsigned long long)c[3],
            (unsigned long long)c[4], (unsigned long long)c[5], (unsigned long long)c[6],
            g_gl_logged < GL_MAXLOG && !g_gl_pngq.on ? ", images saved" : "");
        if (g_gl_logged >= GL_MAXLOG || g_gl_pngq.on) continue;
        g_gl_logged++;
        const GlSlot *trio[3] = {pv, s, nx};
        for (int k = 0; k < 3; k++) {
            VkBufferImageCopy rg = {.bufferOffset = (VkDeviceSize)k * GL_W * GL_H * 4, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                    .imageExtent = {GL_W, GL_H, 1}};
            vkCmdCopyImageToBuffer(cb, trio[k]->img, VK_IMAGE_LAYOUT_GENERAL, g_gl_png, 1, &rg);
        }
        vk_full_barrier(cb);
        g_gl_pngq.on = true, g_gl_pngq.seq = vk_pending_seq(), g_gl_pngq.frame = f;
    }
}

// device.c vk_frame_end, a presented game frame (not a generated one), after the window image was drawn and before its
// present: sc in COLOR_ATTACHMENT_OPTIMAL, left so
void vk_glitch_frame(VkCommandBuffer cb, VkImage sc, VkFormat sc_fmt, uint32_t w, uint32_t h, uint64_t frame) {
    if (!vk_glitch_on() || !sc || !w || !h) return;
    const bool srgb = sc_fmt == VK_FORMAT_B8G8R8A8_SRGB || sc_fmt == VK_FORMAT_R8G8B8A8_SRGB;
    const VkFormat want = srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
    if (g_gl_fmt == VK_FORMAT_UNDEFINED) g_gl_fmt = want;
    if (want != g_gl_fmt) {   // (the window changed between sRGB and not: the ring was made for the other)
        static int told;
        if (!told++) LOG("[glitch] the window image changed format (%d): detector paused\n", (int)sc_fmt);
        return;
    }
    if (sc_fmt == VK_FORMAT_A2B10G10R10_UNORM_PACK32 || sc_fmt == VK_FORMAT_A2R10G10B10_UNORM_PACK32 ||
        sc_fmt == VK_FORMAT_R16G16B16A16_SFLOAT) {   // (the HDR10 / scRGB formats vk_present_pick_format takes)
        static int told;
        if (!told++) LOG("[glitch] HDR window image (format %d): the threshold is not calibrated for it\n", (int)sc_fmt);
    }
    if (!gl_init(cb)) return;
    gl_poll();
    gl_eval(cb);
    GlSlot *s = &g_gl[frame % GL_N];
    gl_layout(cb, sc, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    gl_blit(cb, sc, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, w, h, s->img, GL_W, GL_H);
    gl_layout(cb, sc, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    gl_blit(cb, s->img, VK_IMAGE_LAYOUT_GENERAL, GL_W, GL_H, g_gl_h1, GL_W / 2, GL_H / 2);
    gl_blit(cb, g_gl_h1, VK_IMAGE_LAYOUT_GENERAL, GL_W / 2, GL_H / 2, g_gl_h2, GL_W / 4, GL_H / 4);
    gl_blit(cb, g_gl_h2, VK_IMAGE_LAYOUT_GENERAL, GL_W / 4, GL_H / 4, g_gl_sm, GL_SW, GL_SH);
    VkBufferImageCopy rg = {.bufferOffset = (VkDeviceSize)(frame % GL_N) * GL_SW * GL_SH * 4,
                            .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {GL_SW, GL_SH, 1}};
    vkCmdCopyImageToBuffer(cb, g_gl_sm, VK_IMAGE_LAYOUT_GENERAL, g_gl_buf, 1, &rg);
    vk_full_barrier(cb);
    s->used = true, s->frame = frame, s->seq = vk_pending_seq(), s->pending = true, s->sig_ok = false;
    vk_glitch_counters(s->ctr);
}
