// Texture pre-upload on the loading screen (warmup 2026-10-30, hitch_kak1: the world's first frame after a load carried
// a 112 MB first-use upload of 558 textures, 64.5 ms of backend execute, identify + hash 13.7 ms and deswizzle / staging
// / decode 12.7 ms on the render thread: the only frame over 50 ms in warm Kakariko). A texture the guest initializes
// while the loading screen is up (hle_vi.cpp vi_loading_screen: the render-pass rule's loading state, title left) is
// queued here (backend_texture_created, guest thread); at each game present on the render thread (backend_present), the
// queue's textures from earlier frames get their image and first upload (vk_tex, as a first sampled use would), up to
// LARECOMP_TEX_PREUPLOAD ms of render-thread time a frame (default 6: the loading screen draws 2 passes a frame). Then
// the first draw only checks them: a pre-uploaded texture's first real use skips the 1-in-8 check shortcut (textures.c
// upload_texture_), so a texel the guest wrote after the pre-upload is uploaded again there, as now: what a frame shows
// does not change. Only sampled data: block-compressed (BC / ASTC), or another colour format whose NVN flags say no
// render target, storage image, display or CPU-linear texture (DISPLAY 0x1, IMAGE 0x4, COMPRESSIBLE 0x8, LINEAR 0x10,
// LINEAR_RENDER_TARGET 0x40, ADAPTIVE_ZCULL 0x80), never depth / stencil; none another object renders into, none with an
// image already, none while residency reports VRAM / commit pressure. Only while the loading screen is up: what is left
// when it ends is dropped (its first use uploads it, as before), so no world frame pays for it. Fast-travel arrivals
// take the same path (their loading screen). LARECOMP_TEX_PREUPLOAD=0: off. Each load's account: at its end the skips by
// reason, then over the 240 frames after it the first uploads that were not pre-uploaded, split into created before the
// end (eligible, or not) and after it ("[tex] pre-upload account").
#include <SDL3/SDL_timer.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include "vk.h"
#include "../tegra.h"
#include "../shim/las.h"   // (LOG)

extern int vi_loading_screen(void);   // hle_vi.cpp
extern bool renderer_texture_has_writer(NvnTexture *t);
extern bool backend_async_on(void);

typedef struct { NvnTexture *t; uint64_t frame, seen; } PreEnt;   // (seen: its write-watch epoch, pre_settled)
static pthread_mutex_t pre_mu = PTHREAD_MUTEX_INITIALIZER;
static PreEnt *pre_q;
static int pre_n, pre_cap;
static uint64_t pre_done, pre_bytes, pre_skipped, pre_dropped, pre_first_used;
enum { SK_TAKEN, SK_PRESSURE, SK_IMAGE, SK_GONE, SK_FORMAT, SK_FLAGS, SK_WRITER, SK_N };   // (pre_reject: 0 = taken)
static const char *const sk_name[SK_N] = {"taken", "pressure", "had an image", "finalized", "format", "flags", "alias writer"};
static uint64_t pre_sk[SK_N];
// the load in progress / just ended (render thread): the textures created up to its end have debug ids below end_id
static bool pre_loading;
static uint64_t pre_end_frame, pre_end_id, pre_load_done, pre_load_bytes, pre_left;
static uint64_t fs_n[3], fs_bytes[3], re_n, re_bytes;   // (re_: pre-uploaded, changed at the first use)   // first uploads after the end, not pre-uploaded: [0] made before it, eligible,
                                        // [1] made before it, not eligible, [2] made after it
extern uint64_t vk_preupload_next_id(void);   // objects.c: the next NvnTexture debug id

static int64_t pre_budget_ns(void) {
    static int64_t v = -1;
    if (v < 0) {
        const char *e = getenv("LARECOMP_TEX_PREUPLOAD");
        v = (int64_t)((e ? atof(e) : 6.0) * 1e6);
        if (v < 0) v = 0;
        LOG("[tex] loading-screen pre-upload: %.1f ms a frame%s (LARECOMP_TEX_PREUPLOAD)\n", v / 1e6, v ? "" : ": off");
    }
    return v;
}

// nvnTextureInitialize (objects.c, the guest's thread)
void backend_texture_created(NvnTexture *t) {
    if (!t || t->target == 10 || (t->flags & 2) || !vi_loading_screen() || !backend_async_on() || !pre_budget_ns()) return;
    pthread_mutex_lock(&pre_mu);
    if (pre_n == pre_cap) {
        int cap = pre_cap ? pre_cap * 2 : 1024;
        PreEnt *q = realloc(pre_q, sizeof *q * (size_t)cap);
        if (q) { pre_q = q; pre_cap = cap; }
    }
    if (pre_n < pre_cap) pre_q[pre_n++] = (PreEnt){t, vk_frame_counter(), 0};
    pthread_mutex_unlock(&pre_mu);
}

// backend_texture_destroyed (textures.c, render thread): before the NvnTexture is freed
void vk_preupload_forget(NvnTexture *t) {
    if (!pre_n) return;   // (a racy read: a push of this very texture cannot be in flight, it is being destroyed)
    pthread_mutex_lock(&pre_mu);
    for (int i = 0; i < pre_n; i++)
        if (pre_q[i].t == t) { pre_q[i] = pre_q[--pre_n]; pre_dropped++; break; }
    pthread_mutex_unlock(&pre_mu);
}

// a pre-uploaded texture met its first real use (textures.c upload_texture_)
void vk_preupload_used(void) { pre_first_used++; }

// a pre-uploaded texture whose texels changed before its first use (textures.c: uploaded again there)
void vk_preupload_changed(uint64_t bytes) { re_n++; re_bytes += bytes; }

// Texels still arriving (kp_A: the entry frame still uploaded 48 MB): a texture is taken only once a look finds its
// pages unwritten since the previous look, a frame or more before (the write watch; where it cannot answer, at once)
static bool pre_settled(PreEnt *e) {
    extern int heap_ww_changed(uint64_t addr, uint64_t size, uint64_t *seen);
    return heap_ww_changed(e->t->addr, tegra_total_size(&e->t->layout), &e->seen) <= 0;
}

// which textures the pre-upload takes (above); 0 = taken, else the SK_ reason
static int pre_kind(NvnTexture *t) {   // (the kind of texture taken: 0, else SK_FORMAT / SK_FLAGS / SK_WRITER)
    const VkFormat f = vk_format(t->format);
    const bool compressed = f >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && f <= VK_FORMAT_ASTC_12x12_SRGB_BLOCK;
    if (f == VK_FORMAT_UNDEFINED || (f >= VK_FORMAT_D16_UNORM && f <= VK_FORMAT_D32_SFLOAT_S8_UINT)) return SK_FORMAT;
    if (!compressed && (t->flags & 0xdd)) return SK_FLAGS;
    if (renderer_texture_has_writer(t)) return SK_WRITER;
    return 0;
}
static int pre_reject(NvnTexture *t, bool pressure) {
    if (pressure) return SK_PRESSURE;
    if (t->backend) return SK_IMAGE;
    if (t->zombie || t->fin_queued || !t->addr) return SK_GONE;
    return pre_kind(t);
}

// a texture's first upload (textures.c upload_texture_, render thread): counted into the load's account when it comes
// within 240 frames of the load's end and was not pre-uploaded
void vk_preupload_first_sight(NvnTexture *t, uint64_t bytes) {
    if (!pre_end_frame || pre_loading) return;
    const uint64_t frame = vk_frame_counter();
    if (frame - pre_end_frame < 240) {
        const int k = t->debug_id >= pre_end_id ? 2 : pre_kind(t) ? 1 : 0;
        fs_n[k]++;
        fs_bytes[k] += bytes;
        return;
    }
    LOG("[tex] pre-upload account, 240 frames after the load: first uploads not pre-uploaded: made before its end and "
        "eligible %llu (%.1f MB), not eligible %llu (%.1f MB), made after it %llu (%.1f MB); pre-uploaded and changed before "
        "the first use %llu (%.1f MB)\n",
        (unsigned long long)fs_n[0], fs_bytes[0] / 1048576.0, (unsigned long long)fs_n[1], fs_bytes[1] / 1048576.0,
        (unsigned long long)fs_n[2], fs_bytes[2] / 1048576.0, (unsigned long long)re_n, re_bytes / 1048576.0);
    pre_end_frame = 0;
}

// the loading screen ended: what is left is dropped (render thread)
static void pre_load_end(void) {
    pthread_mutex_lock(&pre_mu);
    pre_left = (uint64_t)pre_n;
    pre_n = 0;
    pthread_mutex_unlock(&pre_mu);
    pre_end_frame = vk_frame_counter();
    pre_end_id = vk_preupload_next_id();
    memset(fs_n, 0, sizeof fs_n);
    memset(fs_bytes, 0, sizeof fs_bytes);
    re_n = re_bytes = 0;
    LOG("[tex] pre-upload account, load end: %llu textures, %.1f MB pre-uploaded; %llu left (dropped); skipped: %s %llu, "
        "%s %llu, %s %llu, %s %llu, %s %llu, %s %llu\n",
        (unsigned long long)pre_load_done, pre_load_bytes / 1048576.0, (unsigned long long)pre_left, sk_name[1],
        (unsigned long long)pre_sk[1], sk_name[2], (unsigned long long)pre_sk[2], sk_name[3], (unsigned long long)pre_sk[3],
        sk_name[4], (unsigned long long)pre_sk[4], sk_name[5], (unsigned long long)pre_sk[5], sk_name[6],
        (unsigned long long)pre_sk[6]);
    pre_load_done = pre_load_bytes = 0;
    memset(pre_sk, 0, sizeof pre_sk);
}

// backend_present (device.c), render thread, before the frame. The entries are taken out under the lock and uploaded
// without it (the guest's thread only appends; vk_preupload_forget runs on this thread too, so a taken entry cannot be
// freed meanwhile); what the budget leaves goes back.
void vk_preupload_frame(void) {
    if (!backend_async_on()) return;
    const bool loading = vi_loading_screen() != 0;
    if (loading) pre_loading = true;
    else if (pre_loading) { pre_loading = false; pre_load_end(); }
    if (!pre_n || !loading) return;
    const uint64_t frame = vk_frame_counter(), t0 = SDL_GetTicksNS();
    const int64_t budget = pre_budget_ns();
    const bool pressure = vk_residency_pressure(NULL) > 0;
    enum { BATCH = 256 };
    PreEnt take[BATCH];
    int nt = 0;
    pthread_mutex_lock(&pre_mu);
    for (int i = 0; i < pre_n && nt < BATCH;) {
        if (pre_q[i].frame >= frame) { i++; continue; }   // (initialized this frame: its texels may still be on their way)
        take[nt++] = pre_q[i];
        pre_q[i] = pre_q[--pre_n];
    }
    pthread_mutex_unlock(&pre_mu);
    int done = 0, k = 0;
    uint64_t bytes = 0;
    for (; k < nt && (int64_t)(SDL_GetTicksNS() - t0) < budget; k++) {
        NvnTexture *t = take[k].t;
        const int why = pre_reject(t, pressure);
        if (why) {
            pre_skipped++;
            pre_sk[why]++;
            continue;
        }
        if (!pre_settled(&take[k])) {   // (written since the last look: back, looked at again next frame)
            take[k].frame = frame;
            pthread_mutex_lock(&pre_mu);
            if (pre_n < pre_cap) pre_q[pre_n++] = take[k];
            pthread_mutex_unlock(&pre_mu);
            continue;
        }
        TexBackend *b = vk_tex(t, false);
        if (b && b->uploaded) {
            b->preuploaded = true;
            done++;
            bytes += tegra_total_size(&t->layout);
        }
    }
    if (k < nt) {   // (the budget's leftovers: back for the next frame)
        pthread_mutex_lock(&pre_mu);
        for (; k < nt; k++) {
            if (pre_n == pre_cap) break;   // (cannot happen: they came out of it; dropped, their first use uploads them)
            pre_q[pre_n++] = take[k];
        }
        pthread_mutex_unlock(&pre_mu);
    }
    if (!done) return;
    pre_done += (uint64_t)done;
    pre_bytes += bytes;
    pre_load_done += (uint64_t)done;
    pre_load_bytes += bytes;
    static uint64_t logs;
    if (logs++ < 200)
        LOG("[tex] pre-upload: %d textures, %.1f MB in %.1f ms on the loading screen (%d queued; in all %llu, %.0f MB, %llu "
            "skipped, %llu finalized first, %llu first uses so far)\n",
            done, bytes / 1048576.0, (SDL_GetTicksNS() - t0) / 1e6, pre_n, (unsigned long long)pre_done, pre_bytes / 1048576.0,
            (unsigned long long)pre_skipped, (unsigned long long)pre_dropped, (unsigned long long)pre_first_used);
}
