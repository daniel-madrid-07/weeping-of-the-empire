// Executes recorded NVN command lists with Vulkan (port of renderer.m; same structure, same behaviour).
#include <SDL3/SDL_cpuinfo.h>
#include <SDL3/SDL_timer.h>
#include <float.h>
#include <math.h>
#include <stdarg.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _MSC_VER
#include <intrin.h>
#else
#include <x86intrin.h>
#endif
#ifndef _WIN32
#include <sys/resource.h>
#include <time.h>
#ifndef RUSAGE_THREAD
#define RUSAGE_THREAD 1   // (Linux; the header hides it without _GNU_SOURCE)
#endif
#endif

#include "../shim/las.h"  // TotK: was ../../kernel.h
#include "../../cpu_place.h"   // nxrecomp: which cores each pool runs on
#include "shaders.h"
#include "spvz.h"   // (the SPIR-V cache's compressed entries: the precompile's last worker compacts old ones)
#include "vk.h"
#include "../shim/las.h"  // TotK: was ../renderer.h
#include "../shim/las.h"  // TotK: was ../../settings.h

uint64_t g_skip_prog_frame, g_skip_pipe_frame, g_skip_comp_frame, g_draws_frame, g_skip_full_frame, g_skip_prog_full;
// device.c: 30 frames in a row were held for missing programs / pipelines / computes, so this frame's draws and dispatches
// build or wait for theirs (<= 3 s each, like a bake) and the frame is shown complete instead of with its skips.
// bake_timeouts: waits that gave up (logged "[bake] timeout"). A plain bool: device.c sets it at present and draw_ /
// dispatch_compute read it on that same thread (the render thread through AOP_PRESENT, the game thread in sync mode);
// replay and present must stay on one thread for that. g_hold_force_wait_ns: what the forced frame spent waiting /
// building (device.c's "[hold]... forced" line, for §10).
bool g_hold_force;
uint64_t g_hold_force_wait_ns;
static uint64_t bake_timeouts;
uint64_t renderer_present_count(void);
// Items whose wait gave up once are not waited for again: they skip at once (still counted as timeouts) until they
// finish.
#define BAKE_DEAD 64
static const void *bake_dead[BAKE_DEAD];
static bool bake_is_dead(const void *p) {
    for (int i = 0; i < BAKE_DEAD; i++) if (bake_dead[i] == p) return true;
    return false;
}
static void bake_timeout(const char *what, uint64_t key, const void *p) {
    bake_timeouts++;
    if (!bake_is_dead(p)) { static int rr; bake_dead[rr++ & (BAKE_DEAD - 1)] = p; }
    if (bake_timeouts <= 50 || !(bake_timeouts % 100))
        LOG("[bake] timeout: %s %llx still building after 3 s, skipped; not waited for again (%llu so far, frame %llu)\n", what,
            (unsigned long long)key, (unsigned long long)bake_timeouts, (unsigned long long)renderer_present_count());
}
// What skipped draws / dispatches waited for, up to 8 distinct, for device.c's "[hold]" line: kind 'c' compute, 'f' a
// fullscreen / tile pass's program, 'p' another program; key = the precompile record's content key (the u64 a seed
// record starts with, little-endian), or the code address when the precompile is off. device.c clears it.
uint64_t g_hold_keys[8];
char g_hold_kinds[8];
int g_hold_nkeys;
static void hold_key_note(char kind, uint64_t key) {
    for (int i = 0; i < g_hold_nkeys; i++) if (g_hold_keys[i] == key) return;
    if (g_hold_nkeys < 8) { g_hold_keys[g_hold_nkeys] = key; g_hold_kinds[g_hold_nkeys++] = kind; return; }
    if (kind == 'p') return;
    for (int i = 0; i < 8; i++)   // (full: a compute / fullscreen one, which hold in every mode, displaces another program)
        if (g_hold_kinds[i] == 'p') { g_hold_keys[i] = key; g_hold_kinds[i] = kind; return; }
}
// drawing thread's own compile time (TOTK_HITCH, see hitch_skip): "draw-thread compile" in the stats line
static NvnTexture *g_dlss_depth;   // (ngx.c foundation: the G-buffer depth the last DLSS upscale used; jittered draws)
static NvnTexture *g_dlss_cand[4];  // (G-buffer-like depth targets seen lately, and the present count they were last seen)
static uint64_t g_dlss_cand_at[4];
static uint64_t stat_dt_link_ns, stat_dt_prog_ns, stat_dt_links, stat_dt_progs, stat_link_deferred, stat_prog_deferred, stat_link_claimed;  // per presented frame: skips of work still being built (device.c holds such frames)
static bool g_render_off;   // SetRenderEnableConditional evaluated false (LARECOMP_COND_RENDER=1)
static uint64_t g_cond_evals, g_cond_off, g_cond_skipped, g_cond_flips;
static uint64_t prof_bake_waits, prof_bake_wait_ns;  // bake draws (3D / BC targets) that waited for their program
static bool g_draw_bake;
static uint64_t prof_pipe_inline, prof_pipe_waited, prof_pipe_wait_ns;  // (LARECOMP_PIPE_INLINE) draws that built / waited for their pipeline
// why draws were skipped (stats line "skip why"): 0 no program bound, 1 program not built, 2 LARECOMP_SKIP_FSHASH / LAS hash,
// 3 unsupported topology, 4 pipeline not built, 5 LAS item cursor, 6 compute not built
static uint64_t stat_skip_why[8];
static int pbn_cause = 2;                 // who asked for the next pass barrier: the vk_end_pass line, 1 = game barrier, 2 = start
static uint32_t stat_pbn_cause[8192];     // pass barriers per cause in the stats window (3 = LARECOMP_BARRIERS=pass forced it)
static bool pass_barrier_needed = true;   // (begin_pass: a new command buffer starts with a full barrier, begin_cb)
static bool pass_unbarriered;             // the open pass skipped its barrier
static bool game_bar_since_fb = true;      // a game CMD_BARRIER since the last feedback barrier (LARECOMP_FEEDBACK_GAMEBAR)
static uint64_t stat_fb_elided;
static uint64_t stat_bar_bits[512], stat_bar_inpass;   // game CMD_BARRIER: count per bits value (x[0] & 0x1ff), inside a pass
static uint64_t stat_count_fs;            // LARECOMP_COUNT_FSHASH=<fs hash>: that shader's draws per stats window (scene tag)
static bool pass_barrier_fresh;           // a full barrier was the last thing recorded (resolve_feedback): the pass needs no other
static bool pass_barrier_frag;            // LARECOMP_FEEDBACK_NARROW: a fragment barrier was the last thing recorded (the restarted pass needs no full one)
static bool pass_elided;                  // LARECOMP_PASS_ELIDE=2: the open pass skipped BAR_PASS's barrier as a small pass (its draws are hazard-checked)
static uint64_t stat_pass_elided, stat_pass_fresh;   // LARECOMP_PASS_ELIDE: passes begun without BAR_PASS's barrier: small, right after a full one
static uint32_t bar_epoch = 1;            // full barriers recorded in the main command buffers so far (vk_full_barrier)
static uint64_t stat_hazard_barriers;     // LARECOMP_BARRIERS=tracked: barriers for a texture / storage read after a write
static uint64_t stat_pass_barriers, stat_pass_nobarrier, stat_game_barriers, stat_game_barrier_breaks;
static uint64_t stat_draws, stat_skipped, stat_passes, stat_dispatches, stat_wait_ns, stat_flushes, stat_feedback_copies, stat_feedback_live, stat_depth_war, stat_fb_selfw, stat_fb_alias;
static uint64_t stat_fb_selfreuse;
static uint64_t stat_snap_delta, stat_snap_delta_px, stat_snap_delta_full_px, stat_snap_tiles_n;   // (LARECOMP_SNAP_DELTA: copies, their pixels, the whole images' pixels)
static uint64_t stat_fbl_moves;   // (g_vk.fbl_layout: attachments moved to the feedback-loop layout for a pass)
static uint64_t stat_oq_reports, stat_oq_hidden, stat_oq_visible_fallback;   // (occlusion queries: see oq_report)   // (LARECOMP_FEEDBACK_SELFCOPY=2: same-draw read + write copies not retaken, see resolve_feedback)
static uint64_t stat_upload_bytes;
// Render passes begun since the start, never reset (any thread reads it): hle_vi.cpp tells loading screens (a few passes
// a frame) from gameplay (100+) to keep an unlocked frame rate off the loads
static uint64_t g_passes_total;
uint64_t vk_exec_passes_total(void) { return __atomic_load_n(&g_passes_total, __ATOMIC_RELAXED); }
int vk_up_kind;                  // (stats) what the vk_transient bytes being allocated are for: 0 other, 1 uniform (guest range),
                                  // 2 vertex, 3 index, 4 texture, 5 uniform driver constants (c0), 6 shader constants (c1), 7 zeros
static uint64_t stat_up_kind[8];  // upload bytes per kind in the stats window ("upload N MB (u/v/i/t/o MB)")
// LARECOMP_VK_PROF=1: render-thread time per category in the fps log (exec = whole command lists, draw = draw(),
// sets = descriptor sets incl. cbuf copies, vtx = index scan + vertex copies, submit = vkQueueSubmit2)
static int prof = -1;
static uint64_t prof_set_alloc, prof_set_cbuf, prof_set_fill, prof_set_update, prof_set_writes;  // (stage_set zones)
static uint64_t prof_set_calls[6], prof_set_same[6], prof_set_samedyn[6];  // (VK_PROF) stage_set calls per stage, same set / + same dynamic offsets as the previous call of that stage
extern uint64_t prof_memo_hit[6], prof_memo_bad[6], prof_memo_why[6][10], prof_memo_other[6][8];
// (VK_PROF) where the shader constants (c1) come from: zero-copy, heap mirror, or copies and why; guest_copy outcomes
static int hm_why;   // (VK_PROF) why heap_mirror_bind refused: 1 shader-written page, 2 past the mirror, 3 straddles, 4 no block, 5 off, 6 cold (LARECOMP_MIRROR_HOT)
static uint64_t prof_hmw[7], prof_c1[8], prof_c1_b[8], prof_gc_hit, prof_gc_new, prof_gc_new_b, prof_gc_evict, prof_gc_evict_b;   // (stage_set inputs memo, defined before stage_set)
#define C1P_BLOCK (4ull << 20)   // (LARECOMP_C1_PERSIST's arena, c1_persist: block size, blocks / entries in use, counters)
static int c1p_nblk, c1p_n;
static uint64_t prof_c1p_new, prof_c1p_changed, prof_c1p_forgot, prof_c1p_kept;
static void c1_persist_forget(const NvnProgram *np);
static uint64_t prof_set_reused, prof_dyn_set, prof_dyn_skip, prof_pipe_bind, prof_pipe_skip;  // (stage_set reuse, draw_ dynamic state / pipeline cache)
static uint64_t prof_set_allocs, prof_set_xsub, prof_set_shlayout;   // (G6 count: stage_set allocations and what would have avoided them)
static uint64_t prof_exec, prof_draw, prof_sets, prof_vtx, prof_submit, prof_pre, prof_key, prof_dyn, prof_tex, prof_up, prof_gpu;
uint64_t prof_vx_copy_n, prof_vx_copy_bytes, prof_vx_low_bytes, prof_vx_low_region, prof_vx_straddle, prof_vx_inflight;  // vertex streams copied instead of bound from the heap import
uint64_t prof_mst_n, prof_mst_bytes, prof_mst_shared, prof_mst_refused, prof_mst_refused_bytes;  // ranges across a mirror block edge (mirror_straddle)
static uint64_t gmem_size;
static int ubo_zc;                            // (tentative: defined with cbuf_info, LARECOMP_UBO_ZEROCOPY)
typedef struct { uint64_t lo, hi; } ZcRange;
static ZcRange zc_excl[64];                   // (see cbuf_info: buffers the game updates inline)
static int zc_nexcl;
static uint64_t stat_ubo_zc, stat_ubo_copy;
static VkQueryPool prof_qp;
static uint64_t cum_exec, cum_wait, cum_draws, cum_submits, cum_tex_up;  // never reset: hitch log
static uint64_t cum_dt_link_ns, cum_dt_prog_ns;   // (hitch_note: the drawing thread's own pipeline links / program compiles)
static uint64_t cum_exec_cpu, cum_exec_ivcsw;   // (Linux) render thread on-CPU ns and involuntary switches inside exec: hitch log
static _Atomic int cum_prog_builds;
// (VK_PROF) draw_'s "pre" zone split (user's PC hitches 2026-10-18: 4.1 of 4.5 s of draw time in "pre", no compiles):
// program lookup, fs hash + debug, feedback, snapshots + begin_pass, topology; gps_ns = get_program_slow's zones in this
// draw: content key, claim (pc_mu), record (cache/code files), translate
static uint64_t prof_pz[5], gps_ns[4];
// (VK_PROF=2) this presented frame, for the "[hitch] frame N first use" line: draw time, draws and pre-zone split by
// whether the draw's program was first drawn this frame [1] or not [0]; get_program_slow zones; reset by vk_prof_frame
static uint64_t hf_draw_ns[2], hf_draws[2], hf_pz[2][5], hf_gps[4];
static bool g_draw_newprog;   // (draw_: this draw's program was first drawn this frame)
static uint32_t prof_end_line[4096];  // passes ended, by the exec.c line that ended them (0: submit / present)
uint64_t prof_views_created;  // textures.c vk_image_view cache misses
uint64_t prof_up_ns = ~0ull;  // textures.c upload checks (~0: profiling off)
// Profiling clock: the TSC scaled to SDL_GetTicksNS's epoch (calibrated in vk_exec_init). The ~35 zone reads per draw through
// SDL_GetTicksNS (vDSO clock_gettime) were 24 % of the Deck render thread's perf samples with VK_PROF on.
static uint64_t prof_tsc0, prof_ns0;
static double prof_ns_per_tick = 1;
static inline uint64_t prof_now(void) { return prof_ns0 + (uint64_t)((double)(__rdtsc() - prof_tsc0) * prof_ns_per_tick); }
#define PROF_T0() uint64_t prof_t0_ = prof ? prof_now() : 0
#define PROF_ADD(v) do { if (prof) v += prof_now() - prof_t0_; } while (0)
// Per-draw CPU zones (draw / draw_, stage_set, get_program_slow) only with LARECOMP_VK_PROF=2: ~150k rdtsc reads a frame
// in Kakariko, 1.08 ms of the render thread, which every measurement boot (=1) carried and players don't. =1 keeps the
// per-submission / per-pass / per-window figures and GPU time; the per-draw breakdowns in the "[prof] stage sets" /
// "draw" lines read 0 there.
#define PROFD (prof > 1)
#define PROF_T0D() uint64_t prof_t0_ = PROFD ? prof_now() : 0
#define PROF_ADDD(v) do { if (PROFD) v += prof_now() - prof_t0_; } while (0)
static _Atomic int stat_compiles;
static uint64_t op_counts[CMD_COUNT];
static uint64_t op_ns[CMD_COUNT], op_t0, op_n2[CMD_COUNT];  // (LARECOMP_VK_PROF) render-thread time per command kind
static int op_last;
static uint64_t prof_binds_skipped, prof_binds_done;  // (LARECOMP_VK_PROF) vertex/index binds skipped as redundant
static uint64_t prof_bp, prof_bp_view, prof_bp_end, prof_bp_n, prof_bpz[4];  // bpz: vk_cmd, barrier, dl_note, BeginRendering  // (LARECOMP_VK_PROF) begin_pass: total, vk_view, previous pass end
bool g_debug_frame;
static char stats_text[2048];   // (640 cut off the tail of the VK_PROF line)
void vk_collect_dead(void);
static void gmem_collect_dead(void);   // (heap imports replaced by a larger one: freed once their last user completed)
static bool pc_readable(uint64_t a, uint64_t n);   // (shader precompile, below)
static bool ind_log(void);   // (LARECOMP_INDIRECT_LOG, at exec_list)
static void ind_note_writes(int stage);
static uint64_t ind_wdraw, ind_wdisp;

// ================================================================== submissions
// A submission = optional upload command buffer + the main one, signalling the timeline with its number.
#define SUBS 16   // (8 -> 16 for chunked submission, LARECOMP_CHUNK_DRAWS)
typedef struct {
    VkCommandBuffer main, upload;
    bool main_begun, upload_begun, ts;  // ts: GPU timestamps written (profiling)
    uint64_t seq;
    VkDescriptorPool pools[16];
    int npools, cur_pool;
    // (LARECOMP_VKQ_SETS: the record thread allocates from pools[]; the render thread's own allocations (vk_alloc_set:
    // post / present, sets not deferred) take these, so they no longer drain the ring first: 0.07-0.08 ms a frame of
    // vkq_drain under vk_alloc_set)
    VkDescriptorPool dpools[16];
    int ndpools, cur_dpool;
} Sub;
static Sub subs[SUBS];
static int cur_sub;
static uint64_t submit_seq;  // last submitted
// Chunked submission (LARECOMP_CHUNK_DRAWS=<n>, default 300, 0 = off: median 50.8 -> 59.9 fps, game fence wait 6.0 -> 1.1 ms, gdev traffic 163 ->
// 546 MB per 2 s): TotK hands over ~1 big command list per frame and the GPU got it only at the end of backend_execute, so the game's fence wait
// (nvnSyncWait, ~4.8 ms per frame) covered translation + GPU serially. With chunks, begin_pass submits what was recorded so far once n
// draws/dispatches are in it, and the GPU runs while the rest is translated.
static uint32_t chunk_draws;
static uint64_t prof_chunks;
static long chunk_ab = -1;
static bool chunk_ab_on = true;
// A chunk boundary is ours, not the game's: nothing guest-visible waits on it. Device-window regions stay in VRAM
// across the chunks of one list (no copy-back + re-upload per chunk) and are copied back by the next real
// submission. A submission holding a FenceSync always copies back. LARECOMP_CHUNK_NO_CARRY=1: copy back at every
// chunk.
static bool g_chunk_submit;
static uint64_t g_fence_seq_mark;
static bool gdev_carry_ok(void);
static void gdev_carry(uint64_t from, uint64_t to);
static uint64_t gw_chain0, gw_chain_end0, gw_chain_end;   // (vk_heap_settled_seq)
static int chunk_limit(void) {
    static int n = -1;
    // Default ON (300) since 2026-10-15 17:38, PC froze) stopped once every command buffer starts with a full barrier
    // (begin_cb). LARECOMP_CHUNK_DRAWS=0: off.
    if (n < 0) { const char *e = getenv("LARECOMP_CHUNK_DRAWS"); n = e && *e ? atoi(e) : 300; if (n < 0) n = 0; }
    // LARECOMP_CHUNK_AB=<frames>: chunking on and off in alternating blocks of that many presented frames (same scene
    // A/B in one run; scripted camera moves differ between runs). Off blocks log "[chunk] off" in the stats window.
    // The decision is latched per guest list (backend_execute) so a list never switches mode half-way.
    if (chunk_ab < 0) { const char *e = getenv("LARECOMP_CHUNK_AB"); chunk_ab = e ? atol(e) : 0; }
    if (n && chunk_ab > 0 && !chunk_ab_on) return 0;
    return n;
}
// Alias mirror epoch: one guest command list (backend_execute). The guest finished its CPU writes for a list before
// submitting it, so the VRAM mirror is synced once per list, not once per Vulkan submission (a sync costs ~1.3 ms).
// Outside an execute: once per submission, as before.
static uint64_t alias_epoch = 1, alias_sync_epoch;
static uint64_t exec_start_seq;  // pending submission when the current guest list started
static bool in_execute;
// nvnSyncWait timing (stats "| syncwait"): how long before the wait the waited submission went to the GPU, and how long
// the wait took. Age ~0 + a long wait = the game waits on work the GPU only just received (chunking helps).
static uint64_t sub_t[64];   // submit time by seq % 64
static uint64_t sw_n, sw_age_ns, sw_wait_ns;
static pthread_mutex_t seq_lock = PTHREAD_MUTEX_INITIALIZER;
static bool in_pass;

// Device window for the storage buffers shaders write (gdev_* below); LARECOMP_VK_NO_GDEV=1: off.
#define GDEV_SIZE (64ull << 20)
static uint64_t gdev_window_size(void) {
    static int off = -1;
    if (off < 0) off = getenv("LARECOMP_VK_NO_GDEV") != NULL;
    return off || !g_vk.host_import ? 0 : GDEV_SIZE;
}
static void gdev_writeback(VkCommandBuffer cb);
static void gdev_check_prev(void);  // (LARECOMP_GDEV_CHECK)
static void gdev_shadow_sync(VkCommandBuffer up);
static uint64_t chk_words, chk_clob;
static bool gdev_chk_on(void);
static VkBuffer chk_buf[2];
static uint64_t gdev_map(uint64_t addr, uint64_t size, bool write);
static uint64_t gdev_stat_in, gdev_stat_regions, gdev_stat_snap, gdev_stat_host;  // (stats line)
static int gdev_n;

uint64_t vk_completed_seq(void) {
    static _Atomic uint64_t last;   // (after a device loss the query fails: keep the last value it gave)
    uint64_t v = 0;
    if (vkGetSemaphoreCounterValue(g_vk.dev, g_vk.timeline, &v) == VK_SUCCESS) last = v;
    return last;
}

// GPU wait time per call site (file:line), printed in the 2-s stats as "| waits site ms/count ..."
#define WSITES 32
static struct { const char *file; int line; _Atomic uint64_t ns, n; } wsite[WSITES];
static pthread_mutex_t wsite_mu = PTHREAD_MUTEX_INITIALIZER;
static void wsite_add(const char *file, int line, uint64_t ns) {
    int i = 0;
    for (; i < WSITES && wsite[i].file; i++)
        if (wsite[i].line == line && wsite[i].file == file) break;
    if (i == WSITES) return;
    if (!wsite[i].file) {
        pthread_mutex_lock(&wsite_mu);
        for (i = 0; i < WSITES && wsite[i].file && !(wsite[i].line == line && wsite[i].file == file); i++) {}
        if (i < WSITES && !wsite[i].file) { wsite[i].line = line; wsite[i].file = file; }
        pthread_mutex_unlock(&wsite_mu);
        if (i == WSITES) return;
    }
    wsite[i].ns += ns;
    wsite[i].n++;
}

static void hang_report(uint64_t hung);   // (GPU hang breadcrumbs, below)
static void hang_checkpoints(void);
void vk_wait_seq_at(uint64_t seq, const char *file, int line) {
    if (!seq) return;
    VkSemaphoreWaitInfo wi = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, .semaphoreCount = 1, .pSemaphores = &g_vk.timeline,
                              .pValues = &seq};
    // A GPU that has not finished a submission after 3 s is hung (no TotK GPU job takes that long): close the process
    // at once instead of piling more work on it (user 2026-10-15: tests must close themselves when the PC starts to
    // freeze). LARECOMP_GPU_HANG_MS=<ms> (0 = old 10 s wait, keep running).
    static long hang_ms = -1;
    if (hang_ms < 0) { const char *e = getenv("LARECOMP_GPU_HANG_MS"); hang_ms = e ? atol(e) : 3000; }
    uint64_t t0 = SDL_GetTicksNS();
    VkResult r = vkWaitSemaphores(g_vk.dev, &wi, (hang_ms > 0 ? (uint64_t)hang_ms : 10000ull) * 1000000ull);
    uint64_t dt = SDL_GetTicksNS() - t0;
    stat_wait_ns += dt;
    cum_wait += dt;
    wsite_add(file, line, dt);
    if (r == VK_TIMEOUT && hang_ms > 0) {
        // The stuck work is in the first unfinished submission, not necessarily the one waited for (2026-10-17 17:00:
        // the wait was for an empty flush, "0 draws, 0 dispatches", so the report named nothing)
        uint64_t done = vk_completed_seq();
        LOG("[vk] GPU HANG: submission %llu not finished after %ld ms (wait at %s:%d); GPU done up to %llu\n", (unsigned long long)seq,
            hang_ms, file, line, (unsigned long long)done);
        extern void vk_devlost_dump(void);
        vk_devlost_dump();
        hang_report(done < seq ? done + 1 : seq);
        // Closing at once does not free the GPU (2026-10-17: the process lived on until the driver reset the GPU
        // 97 s later, Windows TdrDelay = 100 s here), so wait: if the GPU finishes, the game carries on; if the driver
        // resets it, the checkpoints say which operation it was stuck on. LARECOMP_GPU_HANG_WAIT_S=<s> (default 150).
        static long wait_s = -1;
        if (wait_s < 0) { const char *e = getenv("LARECOMP_GPU_HANG_WAIT_S"); wait_s = e ? atol(e) : 150; }
        LOG("[hang] waiting up to %ld s for the GPU to finish or be reset\n", wait_s);
        fflush(stdout);
        uint64_t h0 = SDL_GetTicksNS();
        // LARECOMP_HANG_IDLE=1 (diagnostic): wait with vkDeviceWaitIdle instead, so a layer that watches for device lost
        // there (LunarG crash diagnostic layer) writes its dump
        if (getenv("LARECOMP_HANG_IDLE")) { vkq_drain(); r = vkDeviceWaitIdle(g_vk.dev); }
        else
            for (long i = 0; i < wait_s; i++) {
                r = vkWaitSemaphores(g_vk.dev, &wi, 1000000000ull);
                if (r != VK_TIMEOUT) break;
            }
        double hs = (SDL_GetTicksNS() - h0) / 1e9 + hang_ms / 1e3;
        if (r == VK_SUCCESS) {
            // A driver reset (TDR, ~100 s here) can signal the semaphore too: 2026-10-17 17:00 "finished after 102.5 s",
            // then the next submit failed with device lost and no checkpoints were printed. Ask whether the device is lost.
            uint64_t v = 0;
            VkResult lr = vkGetSemaphoreCounterValue(g_vk.dev, g_vk.timeline, &v);
            if (lr == VK_ERROR_DEVICE_LOST) {
                LOG("[hang] after %.1f s the semaphore is signalled but the device is lost (driver reset); closing the process\n", hs);
                hang_checkpoints();
                vk_device_fault_report();
                fflush(stdout);
                fflush(stderr);
                _exit(5);
            }
            LOG("[hang] the GPU finished submission %llu after %.1f s: carrying on\n", (unsigned long long)seq, hs);
            fflush(stdout);
            return;
        }
        LOG("[hang] after %.1f s: %s; closing the process\n", hs, r == VK_ERROR_DEVICE_LOST ? "the driver reset the GPU (device lost)" : "still stuck");
        hang_checkpoints();
        vk_device_fault_report();
        fflush(stdout);
        fflush(stderr);
        _exit(5);
    }
    if (r == VK_TIMEOUT) LOG("[vk] GPU did not finish submission %llu within 10 s (device lost?)\n", (unsigned long long)seq);
    else if (r != VK_SUCCESS) {
        extern void vk_devlost_dump(void);
        vk_devlost_dump();
        // (2026-10-19: after a reboot the driver resets a hung GPU in ~2 s, before the 3-s hang report above fires)
        uint64_t done = vk_completed_seq();
        hang_report(done < seq ? done + 1 : seq);
        hang_checkpoints();
        vk_device_fault_report();
        FATAL("vkWaitSemaphores: %d (device lost)\n", r);
    }
}

uint64_t vk_pending_seq(void) { return submit_seq + 1; }

static Sub *sub(void) {
    Sub *s = &subs[cur_sub];
    if (s->seq != submit_seq + 1) {  // slot reused: its previous submission must be done
        vk_wait_seq(s->seq);
        if (s->ts) {  // (profiling) GPU time of that submission
            uint64_t t[2];
            if (vkGetQueryPoolResults(g_vk.dev, prof_qp, (uint32_t)cur_sub * 2, 2, sizeof t, t, 8, VK_QUERY_RESULT_64_BIT) == VK_SUCCESS && t[1] > t[0])
                prof_gpu += (uint64_t)((double)(t[1] - t[0]) * g_vk.props.limits.timestampPeriod);
            s->ts = false;
        }
        for (int i = 0; i < s->npools; i++) vkResetDescriptorPool(g_vk.dev, s->pools[i], 0);
        for (int i = 0; i < s->ndpools; i++) vkResetDescriptorPool(g_vk.dev, s->dpools[i], 0);
        s->cur_pool = s->cur_dpool = 0;
        s->seq = submit_seq + 1;
        s->main_begun = s->upload_begun = false;
    }
    return s;
}

static void begin_cb(VkCommandBuffer *cb, bool *begun) {
    if (*begun) return;
    if (!*cb) {
        VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = g_vk.pool,
                                          .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
        vkq_drain();   // (the record thread uses the same command pool)
        VK_CHECK(vkAllocateCommandBuffers(g_vk.dev, &ai, cb));
    }
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    VK_CHECK(vkBeginCommandBuffer(*cb, &bi));
    *begun = true;
    // Order against everything submitted before (a barrier's first scope covers earlier submissions on the queue). The
    // upload buffer used to start with its copies at once: texture uploads and guest -> device-window copies could run
    // while the PREVIOUS submission's shaders still read / wrote the same images and window regions. Rare when one
    // submission covers a whole frame; every time with chunked submission. LARECOMP_NO_CB_BARRIER=1: off (old).
    static int nob = -1;
    if (nob < 0) nob = getenv("LARECOMP_NO_CB_BARRIER") != NULL;
    if (!nob) vk_full_barrier(*cb);
}

static void gp_mark(VkCommandBuffer cb, const char *fmt, ...);
static void dsub_begin(VkCommandBuffer cb);   // (the DRS clock's GPU busy time per submission, see dts_poll)
static void dsub_end(VkCommandBuffer cb);
VkCommandBuffer vk_cmd(void) {
    Sub *s = sub();
    if (!s->main_begun) {
        begin_cb(&s->main, &s->main_begun);
        dsub_begin(s->main);
        gp_mark(s->main, "(submission start)");
        if (prof && prof_qp) {
            vkCmdResetQueryPool(s->main, prof_qp, (uint32_t)cur_sub * 2, 2);
            vkCmdWriteTimestamp2(s->main, VK_PIPELINE_STAGE_2_NONE, prof_qp, (uint32_t)cur_sub * 2);
            s->ts = true;
        }
    }
    return s->main;
}

VkCommandBuffer vk_upload_cmd(void) {
    Sub *s = sub();
    begin_cb(&s->upload, &s->upload_begun);
    return s->upload;
}

void vk_full_barrier(VkCommandBuffer cb) {
    if (cb == sub()->main) bar_epoch++;
    // LARECOMP_BARRIER_ACCESS=narrow (A/B, 2026-10-18: uncapped + GPU-bound on the Deck, no barriers = +5 % but tracked
    // = -1.5 %): the device-side write -> read kinds only instead of MEMORY_WRITE -> MEMORY_READ|WRITE (cache flush cost?)
    static int narrow = -1;
    if (narrow < 0) { const char *e = getenv("LARECOMP_BARRIER_ACCESS"); narrow = e && !strcmp(e, "narrow"); }
    const VkAccessFlags2 wr = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                              VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT;
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           .srcAccessMask = narrow ? wr : VK_ACCESS_2_MEMORY_WRITE_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           .dstAccessMask = narrow ? wr | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                                                         VK_ACCESS_2_UNIFORM_READ_BIT | VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT |
                                                         VK_ACCESS_2_INDEX_READ_BIT | VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT |
                                                         VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                                         VK_ACCESS_2_TRANSFER_READ_BIT
                                                  : VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    vkCmdPipelineBarrier2(cb, &di);
}

// LARECOMP_FEEDBACK_NARROW: a feedback barrier (a draw samples an attachment written since the last one: G-buffer decals, ~155 a frame in Kakariko) orders fragment work
// only, as the game's own barrier does there (NVN ORDER_FRAGMENTS | INVALIDATE_TEXTURE, bits 0x12): the ended pass's writes (attachments, depth, storage) before the next
// fragments' reads and attachment work, vertex work free to overlap (a draw whose vertex stage samples the attachment gets the full barrier: resolve_feedback walks the
// vertex stage's textures first); and the restarted pass skips BAR_PASS's full barrier (nothing else was recorded in between: our own work sets pass_barrier_needed).
// gc4k at 4K Kakariko: barriers cost ~1.8 ms of 6.1 ms GPU (LARECOMP_BARRIERS=none: 4.27), the feedback ones splitting the 8-target G-buffer pass ~25x.
static int feedback_narrow(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("LARECOMP_FEEDBACK_NARROW"); on = !e || *e != '0'; if (on) LOG("[vk] feedback barriers: fragment work only (LARECOMP_FEEDBACK_NARROW)\n"); }
    return on;
}
static void fragment_barrier(VkCommandBuffer cb) {   // (no bar_epoch step: what was written before it stays a hazard for the
    // hazard checks, which cover vertex-stage reads too)
    const VkPipelineStageFlags2 frag = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                                       VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT,   // (all the pass's work: it ended)
                           .srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                                            VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                           .dstStageMask = frag,
                           .dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                                            VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT |
                                            VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                            VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    vkCmdPipelineBarrier2(cb, &di);
}

static void ring_retire(void);

// ---- LARECOMP_GPUPROF_AT=<frame>: GPU time per pass / dispatch / copy of that frame, logged as "[gpuprof]".
// A timestamp (ALL_COMMANDS: after everything before it) at each mark; a row = the work since the previous mark.
bool g_gpuprof_frame;
volatile int g_gpuprof_request;   // (vk_prof_frame, LARECOMP_GPUPROF_SLOW: profile the next frame)
static VkQueryPool gp_qp;
static int gp_n;
#define GP_MAX 16384  // (512 until 2026-10-16: the Deck profile of a gameplay frame stopped at 511 marks; 4096 until 2026-10-17: d24 stopped at 10.9 of ~25 ms)
static char gp_label[GP_MAX][112];
static uint32_t gp_draws[GP_MAX];
// LARECOMP_VK_DEVLOST_TRACE=1: the last 64 GPU operations, dumped on device loss (with LARECOMP_SYNC_SUBMIT=1 the
// dump ends with the command list that faulted).
static char dl_ring[64][128];
static unsigned dl_n;
static int dl_on = -1;
static void dl_note(const char *fmt, ...) {
    if (dl_on < 0) dl_on = getenv("LARECOMP_VK_DEVLOST_TRACE") ? atoi(getenv("LARECOMP_VK_DEVLOST_TRACE")) : 0;
    if (!dl_on) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(dl_ring[dl_n++ & 63], sizeof dl_ring[0], fmt, ap);
    va_end(ap);
}
void vk_dl_note_upload(void *img, uint32_t w, uint32_t h, uint32_t lv) { dl_note("  upload %p %ux%u lv%u", img, w, h, lv); }
void vk_devlost_dump(void) {
    if (!dl_on) return;
    LOG("[vk] last GPU operations before the device loss (oldest first):\n");
    for (unsigned i = dl_n > 64 ? dl_n - 64 : 0; i < dl_n; i++) LOG("[vk]   %s\n", dl_ring[i & 63]);
}
// Hang breadcrumbs (2026-10-17: the user's play sessions froze the PC three times with "[vk] GPU HANG" and nothing
// else, because the trace above is off in play): every draw / dispatch / pass / copy goes into a ring (a few stores)
// with its submission number, and with VK_NV_device_diagnostic_checkpoints it is also a checkpoint whose marker is
// its ring position, so after a hang the driver names the last operation each pipeline stage reached. hang_report
// prints both. (With LARECOMP_VKQ=1 commands are recorded later on another thread: ring only, no checkpoints.)
typedef struct { uint64_t seq, a, b; uint32_t kind, x, y, z; } HangOp;
enum { HOP_DRAW = 1, HOP_DISPATCH, HOP_PASS, HOP_COPY, HOP_SUBMIT };
// draw: a = fs hash, b = vs code, x = vertices/indices, y = instances; dispatch: a = cs code, b = threads, x y z = groups;
// pass: x = colour targets, y z = size; copy: a = source, b = destination, y z = size; submit: a = seq, x = waits
enum { HANG_OPS = 1 << 16 };
static HangOp hang_ops[HANG_OPS];
static uint64_t hang_n;   // render thread
static int hang_ready;
static PFN_vkCmdSetCheckpointNV hang_cp;
// AMD (g_vk.amd_marker): every operation writes its ring position + 1 at the top of the pipe (offset 0: the newest
// operation started) and the bottom (offset 4: the newest that finished) into a host-visible buffer
static PFN_vkCmdWriteBufferMarkerAMD hang_mk;
static VkBuffer hang_mbuf;
static VmaAllocation hang_mmem;
static volatile uint32_t *hang_mptr;
static void hang_marker_init(void) {
    PFN_vkCmdWriteBufferMarkerAMD fn = (PFN_vkCmdWriteBufferMarkerAMD)vkGetDeviceProcAddr(g_vk.dev, "vkCmdWriteBufferMarkerAMD");
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 256, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                                  .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT,
                                  .requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT};
    VmaAllocationInfo info;
    if (!fn || vmaCreateBuffer(g_vk.vma, &bi, &ai, &hang_mbuf, &hang_mmem, &info) != VK_SUCCESS || !info.pMappedData) return;
    hang_mptr = info.pMappedData;
    hang_mptr[0] = hang_mptr[1] = 0;
    hang_mk = fn;
    LOG("[vk] GPU hang breadcrumbs: AMD buffer markers (top / bottom of pipe per operation)\n");
}
static inline void hang_op(VkCommandBuffer cb, uint32_t kind, uint64_t a, uint64_t b, uint32_t x, uint32_t y, uint32_t z) {
    if (!hang_ready) {
        hang_ready = 1;
        if (g_vk.checkpoints) hang_cp = (PFN_vkCmdSetCheckpointNV)vkGetDeviceProcAddr(g_vk.dev, "vkCmdSetCheckpointNV");
        if (g_vk.amd_marker) hang_marker_init();
    }
    HangOp *o = &hang_ops[hang_n & (HANG_OPS - 1)];
    o->seq = submit_seq + 1, o->a = a, o->b = b, o->kind = kind, o->x = x, o->y = y, o->z = z;
    hang_n++;
    if (hang_cp && cb && !vkq_enabled()) hang_cp(cb, (void *)(uintptr_t)hang_n);   // (marker = ring position + 1)
    if (hang_mk && cb) {   // (queued in order with the operation's own commands when the recording thread is on)
        q_mark(hang_mk, cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, hang_mbuf, 0, (uint32_t)hang_n);
        q_mark(hang_mk, cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, hang_mbuf, 4, (uint32_t)hang_n);
    }
}
static void hang_op_log(const char *tag, uint64_t pos) {   // pos = ring position
    if (pos >= hang_n || hang_n - pos > HANG_OPS) { LOG("[hang] %s: #%llu (no longer in the ring)\n", tag, (unsigned long long)pos); return; }
    const HangOp *o = &hang_ops[pos & (HANG_OPS - 1)];
    switch (o->kind) {
    case HOP_DRAW: LOG("[hang] %s: #%llu submission %llu draw fs %016llx vs %llx, %u vertices x %u instances | fb live %u snap %u%s%s%s%s%s\n", tag,
                       (unsigned long long)pos, (unsigned long long)o->seq, (unsigned long long)o->a, (unsigned long long)o->b, o->x, o->y,
                       o->z & 15, o->z >> 4 & 15, o->z >> 8 & 1 ? " +break" : "", o->z >> 9 & 1 ? " +selfwrite" : "",
                       o->z >> 10 & 1 ? " +depth" : "", o->z >> 11 & 1 ? " +copied" : "", o->z >> 12 & 1 ? " +hazard" : ""); break;
    case HOP_DISPATCH: LOG("[hang] %s: #%llu submission %llu dispatch cs %llx %ux%ux%u groups (%llu threads)\n", tag,
                           (unsigned long long)pos, (unsigned long long)o->seq, (unsigned long long)o->a, o->x, o->y, o->z, (unsigned long long)o->b); break;
    case HOP_PASS: LOG("[hang] %s: #%llu submission %llu pass begin (%u colour targets, %ux%u)\n", tag, (unsigned long long)pos,
                       (unsigned long long)o->seq, o->x, o->y, o->z); break;
    case HOP_COPY: LOG("[hang] %s: #%llu submission %llu copy/blit %llx -> %llx %ux%u\n", tag, (unsigned long long)pos,
                       (unsigned long long)o->seq, (unsigned long long)o->a, (unsigned long long)o->b, o->y, o->z); break;
    case HOP_SUBMIT: LOG("[hang] %s: #%llu submit %llu (wait semaphores %u)\n", tag, (unsigned long long)pos, (unsigned long long)o->a, o->x); break;
    default: LOG("[hang] %s: #%llu kind %u\n", tag, (unsigned long long)pos, o->kind);
    }
}
// What the stuck submissions held: per submission the counts, the biggest draw, every dispatch and the fragment
// shaders used (fs hash = the name tools/shadertool and LARECOMP_SKIP_FSHASH use).
static void hang_report(uint64_t hung) {
    uint64_t from = hang_n > HANG_OPS ? hang_n - HANG_OPS : 0, newest = submit_seq + 1;
    LOG("[hang] stuck on submission %llu; newest %llu (open), %llu operations recorded\n", (unsigned long long)hung,
        (unsigned long long)newest, (unsigned long long)hang_n);
    for (uint64_t s = hung; s <= newest && s < hung + 6; s++) {
        unsigned nd = 0, nc = 0, np = 0, ncp = 0, nfs = 0, nall = 0, nlive = 0, nself = 0, nselfp = 0;
        uint64_t big = 0, bigpos = 0, fs[24], selfpos[4];
        unsigned fsn[24];
        for (uint64_t i = from; i < hang_n; i++) {
            const HangOp *o = &hang_ops[i & (HANG_OPS - 1)];
            if (o->seq != s) continue;
            nall++;
            if (o->kind == HOP_DRAW) {
                nd++;
                nlive += (o->z & 15) != 0;
                if (o->z >> 9 & 1) { nself++; if (nselfp < 4) selfpos[nselfp++] = i; }
                if ((uint64_t)o->x * o->y >= big) big = (uint64_t)o->x * o->y, bigpos = i;
                unsigned k = 0;
                while (k < nfs && fs[k] != o->a) k++;
                if (k < nfs) fsn[k]++;
                else if (nfs < 24) fs[nfs] = o->a, fsn[nfs++] = 1;
            } else if (o->kind == HOP_DISPATCH) {
                if (nc++ < 16) hang_op_log("  dispatch", i);
            } else if (o->kind == HOP_PASS) np++;
            else if (o->kind == HOP_COPY) ncp++;
        }
        double age = s <= submit_seq && submit_seq - s < 64 ? (SDL_GetTicksNS() - sub_t[s & 63]) / 1e6 : -1;
        // (the submit's local wall-clock time, to the ms: the hang reports of two processes then order which one stopped
        // first, the hung context, and which was collateral of the reset)
        char wall[32] = "?";
        if (age >= 0) {
#ifdef _WIN32
            FILETIME ft;
            GetSystemTimePreciseAsFileTime(&ft);
            ULARGE_INTEGER u = {.LowPart = ft.dwLowDateTime, .HighPart = ft.dwHighDateTime};
            u.QuadPart -= (uint64_t)(age * 10000.0);
            FILETIME sf = {u.LowPart, u.HighPart}, lf;
            SYSTEMTIME tm;
            FileTimeToLocalFileTime(&sf, &lf);
            FileTimeToSystemTime(&lf, &tm);
            snprintf(wall, sizeof wall, "%02u:%02u:%02u.%03u", tm.wHour, tm.wMinute, tm.wSecond, tm.wMilliseconds);
#else
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            int64_t ms = (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000 - (int64_t)age;
            time_t sec = (time_t)(ms / 1000);
            struct tm lt;
            localtime_r(&sec, &lt);
            size_t k = strftime(wall, sizeof wall, "%H:%M:%S", &lt);
            snprintf(wall + k, sizeof wall - k, ".%03d", (int)(ms % 1000));
#endif
        }
        LOG("[hang] submission %llu: %u draws, %u dispatches, %u passes, %u copies (%u ring entries; submitted %.0f ms ago, at %s)\n",
            (unsigned long long)s, nd, nc, np, ncp, nall, age, wall);
        if (nd) hang_op_log("  biggest draw", bigpos);
        if (nlive) LOG("[hang]   %u draws sampled a bound attachment live, %u of them also writing it (+selfwrite)%s\n", nlive, nself,
                       nself ? "; the first:" : "");
        for (unsigned k = 0; k < nselfp; k++) hang_op_log("  selfwrite draw", selfpos[k]);
        char line[24 * 24 + 8];
        int w = 0;
        for (unsigned k = 0; k < nfs && w < (int)sizeof line - 24; k++) w += snprintf(line + w, sizeof line - w, " %016llx x%u", (unsigned long long)fs[k], fsn[k]);
        if (nfs) LOG("[hang]   fragment shaders:%s%s\n", line, nfs == 24 ? " ..." : "");
    }
    fflush(stdout);
}
// After the device is lost: the driver's last checkpoint per pipeline stage (= ring positions) + what was around them.
static void hang_checkpoints(void) {
    if (hang_mptr) {   // AMD buffer markers: the newest operation the GPU started / finished (32-bit ring positions + 1)
        uint32_t top = hang_mptr[0], bot = hang_mptr[1];
        uint64_t base = hang_n & ~0xFFFFFFFFull, t = base | top, b = base | bot;   // (back to 64 bits: within 2^32 of hang_n)
        if (t > hang_n) t -= 1ull << 32;
        if (b > hang_n) b -= 1ull << 32;
        LOG("[hang] AMD buffer markers: started #%llu, finished #%llu (of %llu)\n", (unsigned long long)t, (unsigned long long)b,
            (unsigned long long)hang_n);
        if (t) hang_op_log("last started", t - 1);
        if (b) hang_op_log("last finished", b - 1);
        if (b && t > b && t - b <= 512)
            for (uint64_t p = b; p < t; p++) hang_op_log("  in flight", p);
        fflush(stdout);
    }
    if (!hang_cp) return;
    PFN_vkGetQueueCheckpointDataNV get = (PFN_vkGetQueueCheckpointDataNV)vkGetDeviceProcAddr(g_vk.dev, "vkGetQueueCheckpointDataNV");
    if (!get) return;
    uint32_t n = 0;
    get(g_vk.queue, &n, NULL);
    VkCheckpointDataNV cd[16];
    if (n > 16) n = 16;
    for (uint32_t i = 0; i < n; i++) cd[i] = (VkCheckpointDataNV){.sType = VK_STRUCTURE_TYPE_CHECKPOINT_DATA_NV};
    get(g_vk.queue, &n, cd);
    LOG("[hang] driver checkpoints: %u\n", n);
    uint64_t lo = UINT64_MAX, hi = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint64_t m = (uint64_t)(uintptr_t)cd[i].pCheckpointMarker;
        char tag[64];
        snprintf(tag, sizeof tag, "last reached stage %#x", (unsigned)cd[i].stage);
        if (!m) { LOG("[hang] %s: none\n", tag); continue; }
        hang_op_log(tag, m - 1);
        for (uint64_t p = m - 1 > 3 ? m - 4 : 0; p < m - 1; p++) hang_op_log("  before", p);
        for (uint64_t p = m; p < m + 3 && p < hang_n; p++) hang_op_log("  after", p);
        if (m - 1 < lo) lo = m - 1;
        if (m - 1 > hi) hi = m - 1;
    }
    // The marker goes in before its operation, so the culprit is between the oldest stage's mark (bottom of pipe: all
    // work before it finished) and the newest (top of pipe: all work before it started). 10-18: 33 operations.
    if (lo < hi && hi - lo <= 512)
        for (uint64_t p = lo; p <= hi; p++) hang_op_log("  in flight", p);
    fflush(stdout);
}
static void gp_mark(VkCommandBuffer cb, const char *fmt, ...) {
    if (!g_gpuprof_frame || gp_n >= GP_MAX - 1) return;
    if (!gp_qp) {
        VkQueryPoolCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = GP_MAX};
        if (vkCreateQueryPool(g_vk.dev, &qi, NULL, &gp_qp) != VK_SUCCESS) { g_gpuprof_frame = false; return; }
    }
    if (!gp_n) vkCmdResetQueryPool(cb, gp_qp, 0, GP_MAX);
    vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, gp_qp, (uint32_t)gp_n);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(gp_label[gp_n], sizeof gp_label[0], fmt, ap);
    va_end(ap);
    gp_draws[gp_n] = 0;
    gp_n++;
}
// LARECOMP_GPUPROF_DRAWS=1: the profiled frame also gets a timestamp before every draw, and the report adds the GPU time
// per fragment shader hash (sum over the frame's draws, top 30). Timestamps between draws keep the GPU from overlapping
// them, so the per-draw times are each draw's cost on its own (the frame total grows); rank by them, don't add them up.
static bool gp_per_draw(void) {
    static int on = -1;
    if (on < 0) on = getenv("LARECOMP_GPUPROF_DRAWS") && strcmp(getenv("LARECOMP_GPUPROF_DRAWS"), "0");
    return on;
}
void renderer_flush(void);
void vk_gpuprof_report(void) {
    if (!gp_n) return;
    renderer_flush();
    static uint64_t t[GP_MAX];
    if (vkGetQueryPoolResults(g_vk.dev, gp_qp, 0, (uint32_t)gp_n, sizeof t, t, 8, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) == VK_SUCCESS) {
        double per = g_vk.props.limits.timestampPeriod / 1e6, total = (t[gp_n - 1] - t[0]) * per;
        LOG("[gpuprof] %d marks, %.2f ms first to last (rows: the work after each mark, until the next)\n", gp_n, total);
        for (int i = 0; i + 1 < gp_n; i++) {
            double ms = (t[i + 1] - t[i]) * per;
            LOG("[gpuprof] %6.3f ms %5.1f%%  %s%s", ms, 100 * ms / (total > 0 ? total : 1), gp_label[i], gp_draws[i] ? "" : "\n");
            if (gp_draws[i]) LOG(", %u draws\n", gp_draws[i]);
        }
        {  // per kind: rows with the same label summed (a pass label is its targets' count,
            // size and format; dispatches by local size), top 25 by GPU ms. Per-draw rows are left to the table below.
            typedef struct { char k[112]; double ms; uint32_t n; } GpKind;
            static GpKind kind[512];
            int nk = 0;
            for (int i = 0; i + 1 < gp_n; i++) {
                if (!strncmp(gp_label[i], "draw fs", 7)) continue;
                char k[112];
                const char *loc = strstr(gp_label[i], "(local");
                if (!strncmp(gp_label[i], "dispatch", 8) && loc) snprintf(k, sizeof k, "dispatch %s", loc);
                else snprintf(k, sizeof k, "%s", gp_label[i]);
                int a = 0;
                while (a < nk && strcmp(kind[a].k, k)) a++;
                if (a == nk) { if (nk == 512) continue; memcpy(kind[nk].k, k, sizeof k); kind[nk].ms = 0; kind[nk].n = 0; nk++; }
                kind[a].ms += (t[i + 1] - t[i]) * per; kind[a].n++;
            }
            for (int a = 0; a < nk; a++)
                for (int b = a + 1; b < nk; b++)
                    if (kind[b].ms > kind[a].ms) { GpKind x = kind[a]; kind[a] = kind[b]; kind[b] = x; }
            LOG("[gpuprof] per kind (%d kinds), top 25:\n", nk);
            for (int a = 0; a < nk && a < 25; a++)
                LOG("[gpuprof]   %7.3f ms %5.1f%% x%-4u %s\n", kind[a].ms, 100 * kind[a].ms / (total > 0 ? total : 1), kind[a].n, kind[a].k);
        }
        if (gp_per_draw()) {   // per fragment shader: draws, GPU ms (the rows that start with "draw fs")
            typedef struct { uint64_t h; double ms, max; uint32_t n; } GpAgg;
            static GpAgg agg[1024];
            int na = 0;
            double all = 0;
            for (int i = 0; i + 1 < gp_n; i++) {
                unsigned long long h;
                if (sscanf(gp_label[i], "draw fs %llx", &h) != 1) continue;
                double ms = (t[i + 1] - t[i]) * per;
                all += ms;
                int k = 0;
                while (k < na && agg[k].h != h) k++;
                if (k == na) { if (na == 1024) continue; agg[na].h = h; agg[na].ms = agg[na].max = 0; agg[na].n = 0; na++; }
                agg[k].ms += ms; agg[k].n++;
                if (ms > agg[k].max) agg[k].max = ms;
            }
            for (int a = 0; a < na; a++)   // (sort by total, descending)
                for (int b = a + 1; b < na; b++)
                    if (agg[b].ms > agg[a].ms) { GpAgg x = agg[a]; agg[a] = agg[b]; agg[b] = x; }
            LOG("[gpuprof] per fs (draws timed one by one: %.2f ms over %d shaders):\n", all, na);
            for (int a = 0; a < na && a < 30; a++)
                LOG("[gpuprof]   fs %016llx %7.3f ms %5.1f%% %5u draws, max %.3f ms\n", (unsigned long long)agg[a].h, agg[a].ms,
                    100 * agg[a].ms / (all > 0 ? all : 1), agg[a].n, agg[a].max);
        }
    }
    gp_n = 0;
}

// TOTK_TEST_DEVLOST=<present#>[,<present#>...] (device-lost recovery tests): at that presented frame the submission ends
// with one compute dispatch that never finishes (a loop on a push constant that stays 0) = a real GPU hang / TDR.
uint64_t renderer_present_count(void);
static void devlost_test(VkCommandBuffer cb) {
    static uint64_t at[8];
    static int n = -1, next;
    if (n < 0) {
        n = 0;
        for (const char *e = getenv("TOTK_TEST_DEVLOST"); e && *e && n < 8;) {
            char *end;
            at[n++] = strtoull(e, &end, 10);
            if (*end != ',') break;
            e = end + 1;
        }
    }
    if (next >= n || renderer_present_count() < at[next]) return;
    next++;
    static VkPipeline pipe;
    static VkPipelineLayout layout;
    if (!pipe) {
        static const char *src = "#version 450\nlayout(local_size_x = 32) in;\nlayout(push_constant) uniform P { uint go; } p;\n"
                                 "shared uint s;\nvoid main() { uint i = 0u; while (p.go == 0u) { atomicAdd(s, 1u); if (++i == 0xFFFFFFF0u) i = 0u; } }\n";
        size_t nw;
        uint32_t *spv = vk_glsl_to_spirv(src, STAGE_COMPUTE, "devlost_test", &nw);
        if (!spv) return;
        VkShaderModule m = vk_module(spv, nw);
        free(spv);
        VkPushConstantRange pc = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 4};
        VkPipelineLayoutCreateInfo pl = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .pushConstantRangeCount = 1, .pPushConstantRanges = &pc};
        VK_CHECK(vkCreatePipelineLayout(g_vk.dev, &pl, NULL, &layout));
        VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                          .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                                                    .module = m, .pName = "main"},
                                          .layout = layout};
        VK_CHECK(vkCreateComputePipelines(g_vk.dev, VK_NULL_HANDLE, 1, &ci, NULL, &pipe));
        vkDestroyShaderModule(g_vk.dev, m, NULL);
    }
    uint32_t go = 0;
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 4, &go);
    vkCmdDispatch(cb, 1, 1, 1);
    LOG("[vk] TOTK_TEST_DEVLOST: endless dispatch recorded at present %llu (submission %llu)\n",
        (unsigned long long)renderer_present_count(), (unsigned long long)submit_seq + 1);
}

static int vkq_submit_deferred(const VkCommandBuffer *cbs, int n, uint64_t value);   // (vkq_impl.h: LARECOMP_VKQ_SUBMIT)
// Submit the pending work (if any). wait/signal: binary semaphores for presentation (may be null).
void vk_submit_extra(VkSemaphore wait, VkSemaphore signal, VkCommandBuffer extra) {
    (void)extra;
    vk_end_pass();
    gdev_check_prev();
    Sub *s = sub();
    if (!s->main_begun && !s->upload_begun && !wait && !signal) return;
    VkCommandBufferSubmitInfo cbs[2];
    int n = 0;
    if (s->upload_begun) {
        gdev_shadow_sync(s->upload);
        vk_full_barrier(s->upload);  // uploads complete before anything in the main buffer reads them
        VK_CHECK(vkEndCommandBuffer(s->upload));
        cbs[n++] = (VkCommandBufferSubmitInfo){.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = s->upload};
    }
    vk_cmd();  // (an empty main buffer still carries the semaphores)
    uint64_t this_seq = submit_seq + 1;
    bool carry = g_chunk_submit && g_fence_seq_mark != this_seq && gdev_carry_ok();
    // (GPU profile: the row of a submission's last draw used to run on to the next submission's start, so GPU idle + the
    // next submission's uploads (its upload buffer runs first) counted as that draw: d9 "38 ms" draws of fs 2a84561f)
    devlost_test(s->main);
    gp_mark(s->main, "(submission end: gdev write-back)");
    if (!carry) gdev_writeback(s->main);
    if (carry) { if (!gw_chain0) gw_chain0 = this_seq; }   // (vk_heap_settled_seq)
    else if (gw_chain0) { gw_chain_end0 = gw_chain0; gw_chain_end = this_seq; gw_chain0 = 0; }
    gp_mark(s->main, "(after the submission: GPU idle + the next one's uploads, until its start)");
    if (s->ts) vkCmdWriteTimestamp2(s->main, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, prof_qp, (uint32_t)cur_sub * 2 + 1);
    dsub_end(s->main);
    VK_CHECK(vkEndCommandBuffer(s->main));
    cbs[n++] = (VkCommandBufferSubmitInfo){.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = s->main};
    VkSemaphoreSubmitInfo sig[2] = {{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = g_vk.timeline,
                                     .value = submit_seq + 1, .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT},
                                    {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = signal,
                                     .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT}};
    VkSemaphoreSubmitInfo wt = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = wait,
                                .stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT};
    VkSubmitInfo2 si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2, .commandBufferInfoCount = (uint32_t)n, .pCommandBufferInfos = cbs,
                        .signalSemaphoreInfoCount = signal ? 2 : 1, .pSignalSemaphoreInfos = sig,
                        .waitSemaphoreInfoCount = wait ? 1 : 0, .pWaitSemaphoreInfos = &wt};
    {
        PROF_T0();
        dl_note("submit %llu", (unsigned long long)submit_seq + 1);
        hang_op(NULL, HOP_SUBMIT, submit_seq + 1, 0, wait ? 1u : 0u, 0, 0);
        extern const char *volatile g_rt_op;
        extern volatile uint64_t g_rt_op_ns;
        VkCommandBuffer dcb[2] = {cbs[0].commandBuffer, n > 1 ? cbs[1].commandBuffer : VK_NULL_HANDLE};
        VkResult sr = VK_SUCCESS;
        if (!wait && !signal && vkq_submit_deferred(dcb, n, submit_seq + 1)) goto submitted;   // (the record thread submits)
        vkq_drain();   // (LARECOMP_VKQ: the record thread finishes both command buffers first)
        g_rt_op = "vkQueueSubmit2", g_rt_op_ns = SDL_GetTicksNS();
        sr = vkQueueSubmit2(g_vk.queue, 1, &si, VK_NULL_HANDLE);
        g_rt_op = NULL;
        if (sr == VK_ERROR_DEVICE_LOST) {   // (the hang path above can miss a reset: name the stuck operation here too)
            vk_devlost_dump();
            hang_report(vk_completed_seq() + 1);
            hang_checkpoints();   // (VK_CHECK below adds the fault report)
        }
        VK_CHECK(sr);
    submitted:
        PROF_ADD(prof_submit);
    }
    pthread_mutex_lock(&seq_lock);
    submit_seq++;
    sub_t[submit_seq & 63] = SDL_GetTicksNS();
    pthread_mutex_unlock(&seq_lock);
    if (carry) gdev_carry(this_seq, submit_seq + 1);
    cur_sub = (cur_sub + 1) % SUBS;
    chunk_draws = 0;
    stat_flushes++;
    cum_submits++;
    ring_retire();
    vk_collect_dead();
    gmem_collect_dead();
}

void renderer_submit(void) { vk_submit_extra(VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE); }
// At a game fence (nvnQueueFenceSync): end the open submission. The game rewrites its buffers once a SyncWait on the fence
// returns, and with SYNC_LAG that could happen while the fence's submission was still open: its later lists then shared the
// per-submission state made for the earlier ones (guest copies, device-window regions, mirror uploads) = UI text drawn with
// the previous layout for single frames. With this, one submission only holds data the game cannot have rewritten yet.
// LARECOMP_FENCE_SUBMIT=0: old.
void renderer_fence_submit(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("LARECOMP_FENCE_SUBMIT"); on = !(e && *e == '0'); }
    if (!on) return;
    backend_exec_lock();
    renderer_submit();
    backend_exec_unlock();
}

void renderer_flush(void) {
    renderer_submit();
    vk_wait_seq(submit_seq);
}

void renderer_sync(void) {
    pthread_mutex_lock(&seq_lock);
    uint64_t s = submit_seq;
    pthread_mutex_unlock(&seq_lock);
    vk_wait_seq(s);
}

uint64_t renderer_fence_seq(void) {
    pthread_mutex_lock(&seq_lock);
    uint64_t q = submit_seq + (subs[cur_sub].main_begun || subs[cur_sub].upload_begun ? 1 : 0);
    pthread_mutex_unlock(&seq_lock);
    return q;
}

static uint64_t stat_sync_early;  // SyncWaits on a fence whose submission was still open (returned before it ran)

// LARECOMP_RACECHECK=1 (2026-10-20, Deck HUD chaos and rare 3D pops): guest memory the GPU reads in place (uniforms with
// UBO_ZEROCOPY, heap vertex / index data on an iGPU, the alias window without its mirror) is hashed when its draw is
// recorded and again when a SyncWait has seen its submission done, before the game is released. A change in between = the
// game rewrote data the GPU may still have been reading (the SyncWait lag returns one fence early). GPU writes into guest
// memory by later submissions also count, so compare with LARECOMP_SYNC_LAG=0 (the floor). Logs "[race]" lines.
// "cpu" = changes no shader write of a submission in between covers (gw_overlap): the guest CPU rewrote it in flight.
enum { RC_N = 1 << 16, RC_DEDUP = 4096, RC_K = 4 };
static int rc_on = -1;
static struct { uint64_t addr, size, seq, h, d0; int kind; } rc_e[RC_N];
static uint64_t rc_head, rc_tail, rc_n[RC_K], rc_chg[RC_K], rc_chg_b[RC_K], rc_cpu[RC_K], rc_drop, rc_waits;
static pthread_mutex_t rc_lock = PTHREAD_MUTEX_INITIALIZER;
static const char *const rc_kind[RC_K] = {"uniform", "vertex/index", "alias window", "mirror GPU-copied page"};
static int gw_overlap(uint64_t addr, uint64_t size, uint64_t lo, uint64_t hi);
static uint64_t rc_hash(uint64_t addr, uint64_t size) {   // every word up to 16 KB, else 2048 spread over the range
    const uint8_t *p = (const uint8_t *)(uintptr_t)addr;
    uint64_t h = size * 0x9E3779B97F4A7C15ull, w, n = size / 8, m = n < 2048 ? n : 2048;
    for (uint64_t k = 0; k < m; k++) {
        memcpy(&w, p + (n <= 2048 ? k : k * (n - 1) / (m - 1)) * 8, 8);
        h = (h ^ w) * 0x100000001B3ull;
    }
    return h;
}
static void rc_record(int kind, uint64_t addr, uint64_t size) {   // (render thread)
    if (rc_on < 0) {
        const char *e = getenv("LARECOMP_RACECHECK");
        rc_on = e && strcmp(e, "0");
        if (rc_on) LOG("[race] in-place guest data checked at each SyncWait (LARECOMP_RACECHECK)\n");
    }
    if (!rc_on || size < 8) return;
    static struct { uint64_t addr, size, seq; } dd[RC_DEDUP];   // once per range and submission
    uint64_t seq = vk_pending_seq(), k = addr * 0x9E3779B97F4A7C15ull ^ size;
    uint32_t sl = (uint32_t)(k ^ k >> 29) & (RC_DEDUP - 1);
    if (dd[sl].addr == addr && dd[sl].size == size && dd[sl].seq == seq) return;
    dd[sl].addr = addr; dd[sl].size = size; dd[sl].seq = seq;
    uint64_t h = rc_hash(addr, size), d0 = vk_completed_seq();
    pthread_mutex_lock(&rc_lock);
    if (rc_head - rc_tail == RC_N) rc_drop++;
    else rc_e[rc_head++ % RC_N] = (typeof(rc_e[0])){addr, size, seq, h, d0, kind};
    pthread_mutex_unlock(&rc_lock);
}
static void rc_check(uint64_t done) {   // (game thread inside SyncWait: submissions <= done have finished on the GPU)
    if (rc_on <= 0) return;
    static int nlog;
    pthread_mutex_lock(&rc_lock);
    while (rc_tail < rc_head && rc_e[rc_tail % RC_N].seq <= done) {
        typeof(rc_e[0]) *e = &rc_e[rc_tail++ % RC_N];
        rc_n[e->kind]++;
        if (rc_hash(e->addr, e->size) == e->h) continue;
        rc_chg[e->kind]++;
        rc_chg_b[e->kind] += e->size;
        int gw = gw_overlap(e->addr, e->size, e->d0, done);   // (record thread's ring read from here: diagnostic only)
        if (!gw) rc_cpu[e->kind]++;
        if (nlog++ < 60)
            LOG("[race] frame %llu: %s %llx +%llx (submission %llu) changed before the GPU had finished it (%s)\n",
                (unsigned long long)renderer_present_count(), rc_kind[e->kind], (unsigned long long)e->addr,
                (unsigned long long)e->size, (unsigned long long)e->seq, gw > 0 ? "shader-written bytes" : gw ? "unknown" : "cpu");
    }
    if (++rc_waits % 600 == 0) {
        LOG("[race] frame %llu, last 600 SyncWaits: uniform %llu changed (cpu %llu) of %llu (%llu KB) | vertex/index %llu (cpu %llu) "
            "of %llu (%llu KB) | alias %llu (cpu %llu) of %llu (%llu KB) | mirror gpu page %llu (cpu %llu) of %llu (%llu KB) | dropped %llu\n",
            (unsigned long long)renderer_present_count(),
            (unsigned long long)rc_chg[0], (unsigned long long)rc_cpu[0], (unsigned long long)rc_n[0], (unsigned long long)(rc_chg_b[0] >> 10),
            (unsigned long long)rc_chg[1], (unsigned long long)rc_cpu[1], (unsigned long long)rc_n[1], (unsigned long long)(rc_chg_b[1] >> 10),
            (unsigned long long)rc_chg[2], (unsigned long long)rc_cpu[2], (unsigned long long)rc_n[2], (unsigned long long)(rc_chg_b[2] >> 10),
            (unsigned long long)rc_chg[3], (unsigned long long)rc_cpu[3], (unsigned long long)rc_n[3], (unsigned long long)(rc_chg_b[3] >> 10),
            (unsigned long long)rc_drop);
        memset(rc_n, 0, sizeof rc_n); memset(rc_chg, 0, sizeof rc_chg); memset(rc_chg_b, 0, sizeof rc_chg_b);
        memset(rc_cpu, 0, sizeof rc_cpu); rc_drop = 0;
    }
    pthread_mutex_unlock(&rc_lock);
}

void renderer_wait_seq(uint64_t seq) {
    if (!seq) return;
    pthread_mutex_lock(&seq_lock);
    uint64_t s = submit_seq;
    pthread_mutex_unlock(&seq_lock);
    if (seq > s) {
        stat_sync_early++;
        static int nlog;
        if (nlog++ < 12) LOG("[vk] SyncWait on open submission %llu (submitted %llu): returns early\n", (unsigned long long)seq,
                             (unsigned long long)s);
    }
    uint64_t w = seq > s ? s : seq, t0 = SDL_GetTicksNS();
    // LARECOMP_LOG_SYNCWAIT=<frame>: from that presented frame on, 40 lines "[syncwait] frame F seq W (newest submitted S,
    // GPU done C) age A ms wait B ms" = which submission the game waits for and how far behind the GPU is.
    static int64_t lsw_from = -2;
    static int lsw_n;
    if (lsw_from == -2) { const char *e = getenv("LARECOMP_LOG_SYNCWAIT"); lsw_from = e ? atoll(e) : -1; }
    uint64_t done0 = lsw_from >= 0 ? vk_completed_seq() : 0;
    if (w && vk_completed_seq() < w) {
        sw_n++;
        sw_age_ns += s - w < 64 ? t0 - sub_t[w & 63] : 0;
        vk_wait_seq(w);  // not submitted yet (other thread): everything submitted so far
        sw_wait_ns += SDL_GetTicksNS() - t0;
    }
    if (lsw_from >= 0 && renderer_present_count() >= (uint64_t)lsw_from && lsw_n < 40) {
        lsw_n++;
        LOG("[syncwait] frame %llu seq %llu (asked %llu, newest submitted %llu, GPU done %llu) age %.2f ms wait %.2f ms\n",
            (unsigned long long)renderer_present_count(), (unsigned long long)w, (unsigned long long)seq, (unsigned long long)s,
            (unsigned long long)done0, s - w < 64 ? (t0 - sub_t[w & 63]) / 1e6 : -1.0, (SDL_GetTicksNS() - t0) / 1e6);
    }
    rc_check(w);
}

static VkDescriptorSet alloc_set_from(VkDescriptorPool *pools, int *npools, int *cur_pool, VkDescriptorSetLayout layout);
static VkDescriptorSet alloc_set_in(Sub *s, VkDescriptorSetLayout layout) { return alloc_set_from(s->pools, &s->npools, &s->cur_pool, layout); }
static int vkq_sets_env(void);   // (vkq_impl.h: LARECOMP_VKQ_SETS)
static VkDescriptorSet vkq_set_deferred(VkDescriptorSetLayout layout, const VkWriteDescriptorSet *w, int n);
VkDescriptorSet vk_alloc_set(VkDescriptorSetLayout layout) {
    Sub *s = sub();
    // (with deferred sets the record thread owns pools[]: the render thread allocates from its own dpools[], no drain)
    if (vkq_sets_env()) return alloc_set_from(s->dpools, &s->ndpools, &s->cur_dpool, layout);
    return alloc_set_in(s, layout);
}
static VkDescriptorSet alloc_set_from(VkDescriptorPool *pools, int *npools, int *cur_pool, VkDescriptorSetLayout layout) {
    for (;;) {
        if (*cur_pool == *npools) {
            if (*npools == 16) FATAL("descriptor pools exhausted\n");
            // (storage buffers: the heap and the device window, plus the heap's other windows where it needs them)
            const uint32_t nsb = 4096u * (maxwell_gmem_window ? GMEM_WIN_MAX : 1u);
            VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 32768}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nsb},
                                            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 32768},
                                            {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1024}, {VK_DESCRIPTOR_TYPE_SAMPLER, 1024},
                                            {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1024},
                                            {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 32768}};
            VkDescriptorPoolCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 8192,
                                             .poolSizeCount = (uint32_t)(sizeof sizes / sizeof *sizes), .pPoolSizes = sizes};
            VK_CHECK(vkCreateDescriptorPool(g_vk.dev, &ci, NULL, &pools[(*npools)++]));
        }
        VkDescriptorSetAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                          .descriptorPool = pools[*cur_pool], .descriptorSetCount = 1, .pSetLayouts = &layout};
        VkDescriptorSet set;
        VkResult r = vkAllocateDescriptorSets(g_vk.dev, &ai, &set);
        if (r == VK_SUCCESS) return set;
        (*cur_pool)++;
    }
}

// ================================================================== transient memory ring
// 64 MB host-visible chunks (device-local when the GPU exposes resizable BAR); a chunk is reused once every
// submission that allocated from it has completed.
#define CHUNK_SIZE (64ull << 20)
typedef struct { VkBuffer buf; VmaAllocation mem; uint8_t *ptr; uint64_t size, used, seq; } Chunk;
static Chunk *chunks;
static int nchunks, active = -1;

static int new_chunk(uint64_t size) {
    chunks = realloc(chunks, (nchunks + 1) * sizeof *chunks);
    Chunk *c = &chunks[nchunks];
    memset(c, 0, sizeof *c);
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size,
                             .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                                      VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                      VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    // Integrated GPU (Steam Deck): host memory (GTT, write-combined). d17 2026-10-16: the ring grew to ~18 chunks (1.1 GB) in the
    // "device-local" heap, which there is a 1 GB VRAM carve-out + GTT placed first come first served (VRAM asked 3.1 GB, 985 MB
    // resident), so one-shot upload data competed with render targets for VRAM. LARECOMP_VK_CHUNK_DEVICE=1: device-local as before.
    static int chunk_host = -1;
    if (chunk_host < 0)
        chunk_host = g_vk.props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU && !getenv("LARECOMP_VK_CHUNK_DEVICE");
    VmaAllocationCreateInfo ai = {.usage = chunk_host ? VMA_MEMORY_USAGE_AUTO_PREFER_HOST : VMA_MEMORY_USAGE_AUTO,
                                  .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT};
    VmaAllocationInfo info;
    VK_CHECK(vmaCreateBuffer(g_vk.vma, &bi, &ai, &c->buf, &c->mem, &info));
    if (nchunks == 0) LOG("[vk] transient ring chunks: memory type %u (%s)\n", info.memoryType, chunk_host ? "host, integrated GPU" : "device-local preferred");
    c->ptr = info.pMappedData;
    c->size = size;
    return nchunks++;
}

static void ring_retire(void) {}  // chunks are recycled lazily in vk_transient

VkSpan vk_transient(uint64_t size, uint64_t align) {
    if (align < 16) align = 16;
    uint64_t pending = vk_pending_seq();
    if (active >= 0) {
        Chunk *c = &chunks[active];
        uint64_t off = (c->used + align - 1) & ~(align - 1);
        if (off + size <= c->size) {
            c->used = off + size;
            c->seq = pending;
            stat_upload_bytes += size;
            stat_up_kind[vk_up_kind] += size;
            if (vk_up_kind == 4) cum_tex_up += size;
            return (VkSpan){c->buf, off, c->ptr + off};
        }
    }
    uint64_t done = vk_completed_seq();
    int pick = -1;
    bool keep_list = in_execute && chunk_limit();  // (guest_copy shares copies across the chunks of the list)
    for (int i = 0; i < nchunks; i++)
        if (i != active && chunks[i].seq <= done && chunks[i].size >= size && !(keep_list && chunks[i].seq >= exec_start_seq)) { pick = i; break; }
    if (pick < 0) pick = new_chunk(size > CHUNK_SIZE ? size : CHUNK_SIZE);
    active = pick;
    chunks[pick].used = size;
    chunks[pick].seq = pending;
    stat_upload_bytes += size;
    stat_up_kind[vk_up_kind] += size;
    if (vk_up_kind == 4) cum_tex_up += size;
    return (VkSpan){chunks[pick].buf, 0, chunks[pick].ptr};
}

// GPU -> CPU copies: the ring is write-combined (often device-local over the BAR), where CPU reads crawl (a
// 1280x720 screenshot took 7 s). Readbacks use one host-cached buffer instead. Every user waits for the GPU and
// reads the data before the next readback (render thread), so a single grow-only buffer suffices.
VkSpan vk_readback(uint64_t size) {
    static VkBuffer buf;
    static VmaAllocation mem;
    static uint8_t *ptr;
    static uint64_t cap;
    if (size > cap) {
        if (buf) { vk_wait_seq(submit_seq); vmaDestroyBuffer(g_vk.vma, buf, mem); }
        cap = (size + (16 << 20) - 1) & ~((16ull << 20) - 1);
        VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = cap, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                                      .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT};
        VmaAllocationInfo info;
        VK_CHECK(vmaCreateBuffer(g_vk.vma, &bi, &ai, &buf, &mem, &info));
        ptr = info.pMappedData;
    }
    return (VkSpan){buf, 0, ptr};
}

// (VK_PROF) who allocates "other" (kind 0) upload bytes: caller return address, as an offset from vk_transient_copy
static struct { uintptr_t ra; uint64_t bytes, n; } up0_site[16];
static void up0_note(uintptr_t ra, uint64_t size) {
    int i = 0;
    for (; i < 16 && up0_site[i].ra && up0_site[i].ra != ra; i++) {}
    if (i == 16) return;
    up0_site[i].ra = ra; up0_site[i].bytes += size; up0_site[i].n++;
}
VkSpan vk_transient_copy(const void *data, uint64_t size, uint64_t align) {
    if (prof > 0 && vk_up_kind == 0) up0_note((uintptr_t)__builtin_return_address(0), size);
    VkSpan s = vk_transient(size ? size : 16, align);
    if (size) memcpy(s.ptr, data, size);
    return s;
}

// Per-draw constants that usually repeat the previous draw's (scale table, tint, driver constants): reuse that copy
// while it's identical, within one submission. slot: one per kind of data.
enum { DEDUPE_RSCALE_V, DEDUPE_RSCALE_F, DEDUPE_TINT, DEDUPE_DRV_V, DEDUPE_DRV_F, DEDUPE_DRV_C, DEDUPE_SLOTS };
static VkSpan copy_dedupe(int slot, const void *data, uint64_t size, uint64_t align) {
    static struct { uint64_t seq, size; uint8_t data[0x800]; VkSpan span; } last[DEDUPE_SLOTS];
    uint64_t seq = vk_pending_seq();
    if (size > sizeof last[0].data) return vk_transient_copy(data, size, align);
    if (last[slot].seq == seq && last[slot].size == size && !memcmp(last[slot].data, data, size)) return last[slot].span;
    VkSpan s = vk_transient_copy(data, size, align);
    last[slot].seq = seq; last[slot].size = size; memcpy(last[slot].data, data, size); last[slot].span = s;
    return s;
}

// LARECOMP_VX_STATS=1 (with VK_PROF): what the vertex copies are, per 4 KB heap page - copied already in this submission,
// unchanged since an earlier submission copied it (a VRAM mirror would skip it), or changed / new. "Unchanged" is sampled
// (32 words per page), so it can over-count pages changed only between the samples.
uint64_t prof_vxp_dup, prof_vxp_same, prof_vxp_new;
static void vx_page_stats(uint64_t addr, uint64_t size, uint64_t pend) {
    static int on = -1;
    static uint64_t *pseq, *phash, npages;
    if (on < 0) {
        on = getenv("LARECOMP_VX_STATS") != NULL;
        npages = (4ull << 30) >> 12;  // (heap import <= 4 GB)
        if (on) { pseq = calloc(npages, 8); phash = calloc(npages, 8); on = pseq && phash; }
    }
    if (!on || addr < HEAP_BASE) return;
    for (uint64_t a = addr, end = addr + size; a < end;) {
        uint64_t p = (a - HEAP_BASE) >> 12, pe = HEAP_BASE + ((p + 1) << 12), n = (pe < end ? pe : end) - a;
        if (p >= npages) return;
        if (pseq[p] == pend) prof_vxp_dup += n;
        else {
            const uint64_t *w = (const uint64_t *)(uintptr_t)(HEAP_BASE + (p << 12));
            uint64_t h = 0;
            for (int i = 0; i < 512; i += 16) h = (h ^ w[i]) * 0x9E3779B97F4A7C15ull;
            if (pseq[p] && phash[p] == h) prof_vxp_same += n; else prof_vxp_new += n;
            pseq[p] = pend; phash[p] = h;
        }
        a += n;
    }
}

// Guest memory copied once per submission: draws binding the same range share the copy.
#define GCOPY_TAB 16384   // (cx5_kakariko: ~4,400 new ranges a list; at 4096 slots collisions re-copied ~40 MB per 2 s)
static struct { uint64_t addr, size, seq, used; VkSpan span; } gcopy[GCOPY_TAB];
static uint64_t gcopy_gen;  // bumped to drop every shared copy (guest memory written mid-list)
static void guest_copy_flush(void) { gcopy_gen++; }
static inline uint32_t gcopy_slot(uint64_t addr, uint64_t size) {
    uint64_t h = (addr * 0x9E3779B97F4A7C15ull) ^ (size * 0xC2B2AE3D27D4EB4Full);
    return (uint32_t)(h ^ (h >> 32)) & (GCOPY_TAB - 1);
}
// LARECOMP_RT_LOOKUP: the render thread's per-binding table lookups that miss the cache, the same answers sooner
// (gc63p_kak, render thread: gcopy's slot probe 2.0 %, gdev_find's region scan 0.7 %, set_tab's probe 0.5 %). stage_set
// prefetches the gcopy slots and the written-page marks of its uniform ranges and its set_tab entry; gdev_find keeps its
// last answers until the region table changes (gdev_gen); draw_ prefetches guest_direct's memo slots of its index and
// vertex ranges (gd_prefetch_draw).
static int rt_lookup(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("LARECOMP_RT_LOOKUP"); on = !e || *e != '0'; LOG("[vk] render-thread lookups: prefetch + region cache %s (LARECOMP_RT_LOOKUP)\n", on ? "on" : "off"); }
    return on;
}
// With chunked submission the copies are shared by every chunk of one guest list (nothing writes guest memory within a
// list, gcopy_gen otherwise): the key is the list, and a reuse in a later chunk keeps the ring chunk alive until then.
static VkSpan guest_copy(uint64_t addr, uint64_t size) {
    uint64_t pend = vk_pending_seq();
    // An inline UpdateUniformBuffer bumps gcopy_gen. In zero-copy mode every buffer it ever wrote is in zc_excl, so copies of
    // other ranges cannot go stale and keep being shared (d55_dbg: ~281 inline updates per frame re-copied a 64 KB block of
    // shader constants 132 times = 8 MB per frame).
    uint64_t gen = gcopy_gen;
    if (ubo_zc > 0) {
        gen = 0;
        for (int i = 0; i < zc_nexcl; i++) if (addr < zc_excl[i].hi && addr + size > zc_excl[i].lo) { gen = gcopy_gen; break; }
    }
    // Shared per guest list (LARECOMP_GCOPY_PER_SUB=1: per submission, old): a submission can hold several lists, and the
    // game rewrites its buffers between them once a SyncWait returns (SYNC_LAG: it can return while that submission is
    // still open) - a later list then drew with the earlier list's copy.
    static int per_sub = -1;
    if (per_sub < 0) per_sub = getenv("LARECOMP_GCOPY_PER_SUB") != NULL;
    uint64_t seq = (in_execute && (!per_sub || chunk_limit()) ? (alias_epoch | (1ull << 39)) : pend) + (gen << 40);
    uint32_t slot = gcopy_slot(addr, size);
    if (gcopy[slot].seq == seq && gcopy[slot].addr == addr && gcopy[slot].size == size) {
        if (prof) prof_gc_hit++;
        if (gcopy[slot].used != pend) {
            gcopy[slot].used = pend;
            for (int i = 0; i < nchunks; i++)
                if (chunks[i].buf == gcopy[slot].span.buf && chunks[i].seq < pend) { chunks[i].seq = pend; break; }
        }
        return gcopy[slot].span;
    }
    if (prof) {   // (a slot already holding this list's copy of another range = a hash collision evicting it)
        if (gcopy[slot].seq == seq) prof_gc_evict++, prof_gc_evict_b += size; else prof_gc_new++, prof_gc_new_b += size;
    }
    VkSpan s = vk_transient_copy((const void *)(uintptr_t)addr, size, 256);
    gcopy[slot].addr = addr; gcopy[slot].size = size; gcopy[slot].seq = seq; gcopy[slot].span = s; gcopy[slot].used = pend;
    if (prof && vk_up_kind == 2) vx_page_stats(addr, size, pend);
    return s;
}

// ================================================================== worker threads (shader / pipeline builds)
// First-use compiles run off the render thread; draws needing them are skipped until ready (as on Metal).
// LARECOMP_SYNC_SHADERS=1 builds on first use, blocking (deterministic tests).
// kind: 't' translate, 'p' program compile, 'l' pipeline, 'c' compute; id: the fs hash (cs code address) for the build log
typedef struct Job { void (*fn)(void *); void *arg; struct Job *next; uint64_t tq, id; char kind; bool hurried; } Job;
// The game's own translations / builds queued or running: the precompile workers pause while any are (pc_yield), so a
// shader the game is waiting for never queues behind the backlog.
static _Atomic int g_jobs_pending;
// A queue is FIFO, except that jobs a held frame waits for (job_hurry) run first, in the order they were hurried: urg is
// the last of them.
typedef struct { Job *head, *tail, *urg; pthread_mutex_t lock; pthread_cond_t cond; } JobQ;
static JobQ g_jq = {NULL, NULL, NULL, PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER};   // workers
static JobQ g_tq = {NULL, NULL, NULL, PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER};   // translator

// TOTK_BUILD_LOG=1: a "[build]" line per job of the game's own (not the precompile's): what it built, for which fs (cs:
// code address), how long it queued and ran, the frame it finished in, the jobs still pending, and whether a held frame
// hurried it; "[build] hurry" lines name what a held frame waited for.
static bool build_log(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("TOTK_BUILD_LOG"); v = e && strcmp(e, "0"); }
    return v;
}
static void job_note(const Job *j, uint64_t ts) {
    static _Atomic int n;
    if (atomic_fetch_add(&n, 1) >= 5000) return;
    static const char *const what[] = {"translate", "program", "pipeline", "compute"};
    int k = j->kind == 't' ? 0 : j->kind == 'p' ? 1 : j->kind == 'l' ? 2 : 3;
    uint64_t now = SDL_GetTicksNS();
    LOG("[build] %s %s %016llx: queued %.1f ms, ran %.1f ms, frame %llu, %d pending%s\n", what[k], k == 3 ? "cs" : "fs",
        (unsigned long long)j->id, (ts - j->tq) / 1e6, (now - ts) / 1e6, (unsigned long long)renderer_present_count(),
        atomic_load(&g_jobs_pending) - 1, j->hurried ? ", hurried" : "");
}

static void jq_push(JobQ *q, Job *j, bool front) {   // (q->lock held)
    if (front) {
        Job **at = q->urg ? &q->urg->next : &q->head;
        j->next = *at;
        *at = j;
        q->urg = j;
        j->hurried = true;
        if (!j->next) q->tail = j;
    } else {
        j->next = NULL;
        if (q->tail) q->tail->next = j; else q->head = j;
        q->tail = j;
    }
}
static void jq_add(JobQ *q, void (*fn)(void *), void *arg, char kind, uint64_t id, bool front) {
    Job *j = malloc(sizeof *j);
    *j = (Job){fn, arg, NULL, build_log() ? SDL_GetTicksNS() : 0, id, kind, false};
    atomic_fetch_add(&g_jobs_pending, 1);
    pthread_mutex_lock(&q->lock);
    jq_push(q, j, front);
    pthread_cond_signal(&q->cond);
    pthread_mutex_unlock(&q->lock);
}
static void jq_serve(JobQ *q) {
    for (;;) {
        pthread_mutex_lock(&q->lock);
        while (!q->head) pthread_cond_wait(&q->cond, &q->lock);
        Job *j = q->head;
        q->head = j->next;
        if (!q->head) q->tail = NULL;
        if (q->urg == j) q->urg = NULL;
        pthread_mutex_unlock(&q->lock);
        uint64_t ts = j->tq ? SDL_GetTicksNS() : 0;
        j->fn(j->arg);
        if (ts) job_note(j, ts);
        free(j);
        atomic_fetch_sub(&g_jobs_pending, 1);
    }
}
// Moves arg's job ahead of the not-hurried ones; returns its position before (-1: not queued: running, done, or not yet
// queued).
static int jq_hurry(JobQ *q, const void *arg) {
    pthread_mutex_lock(&q->lock);
    int pos = 0;
    Job *prev = NULL, *j = q->head;
    for (; j && j->arg != arg; prev = j, j = j->next) pos++;
    if (j && !j->hurried) {
        if (prev) prev->next = j->next; else q->head = j->next;
        if (q->tail == j) q->tail = prev;
        jq_push(q, j, true);
    }
    pthread_mutex_unlock(&q->lock);
    return j ? pos : -1;
}

static void *worker(void *a) {
    (void)a;
    cpu_place_thread(CPU_ANY);   // (draws wait for these: every core, cpu_place.h)
#ifndef _WIN32
    { extern int pthread_setname_np(pthread_t, const char *); pthread_setname_np(pthread_self(), "vk-pipeline"); }   // ([cpu] line)
#endif
    jq_serve(&g_jq);
    return NULL;
}

static bool g_bake_sync;  // draw_(): the draw renders into a 3D / BC target (one-shot bake): its program builds now
static bool sync_shaders(void) {
    static int v = -1;
    if (v < 0) v = getenv("LARECOMP_SYNC_SHADERS") != NULL;
    return v || g_bake_sync;
}

// Shader translation: one dedicated thread (translating on the game thread cost up to ~90 ms per frame on entering an
// area), serialised by translate_lock unless the thread says otherwise (translate_unlocked, below). Compiles go on to the workers.
static pthread_mutex_t translate_lock = PTHREAD_MUTEX_INITIALIZER;  // (compute shaders translate on the game thread)
// maxwell_translate (under translate_lock) with the stage's compiler constants (c1, csize bytes after the code) in view:
// translate.c reads BRX jump tables from them. csize 0 (archive programs have only their code): every slot a possible target.
static ShaderInfo *translate_stage(uint64_t code, uint64_t cmax, uint64_t csize, int stage, const ShaderKey *key) {
    maxwell_const_data = csize ? (const uint8_t *)(uintptr_t)(code + cmax) : NULL;
    maxwell_const_size = (size_t)csize;
    ShaderInfo *si = maxwell_translate((const uint8_t *)(uintptr_t)code, (size_t)cmax, stage, key);
    maxwell_const_data = NULL;
    maxwell_const_size = 0;
    return si;
}
// no translate_lock on this thread: pa_job with LARECOMP_PARALLEL_TRANSLATE=1, the translators with TOTK_TRANSLATE_THREADS > 1,
// the first-start pass's workers (TOTK_PRECOMPILE_UNLOCKED, on) (the translator's state is thread-local: MAXWELL_TLS, the RSC2
// / RSC2L rings, prelude_text; its other statics are single-word getenv caches and atomic log counters, and translate_program's
// own are read once, tp_env)
static __thread bool translate_unlocked;
// TOTK_TRANSLATE_THREADS=<n> (1-8; 1 = one translator under translate_lock, as before): a fight's first sighting queues
// dozens of programs at ~2.4 ms each, and in hold mode the frame waits for all of them. The game thread's compute
// translations and the drawing thread's own keep the lock. auto scales with the processors: a quarter of them, 1..4 (4
// on 16 threads, the measured arm: queue wait p90 65 -> 0.5 ms; 2 on 8, 1 on 4-6). The soak it waited for: compilebench
// --translate-stress, 243,200 translations on 16 unlocked threads, 0 differences.
static int translate_threads(void) {
    static int n = -1;
    if (n < 0) {
        const char *e = getenv("TOTK_TRANSLATE_THREADS");
        const bool aut = !e || !*e || !strcmp(e, "auto");
        int v = aut ? SDL_GetNumLogicalCPUCores() / 4 : atoi(e);
        if (aut && v > 4) v = 4;
        n = v < 1 ? 1 : v > 8 ? 8 : v;
    }
    return n;
}
static void *translator(void *a) {
    (void)a;
    cpu_place_thread(CPU_ANY);   // (draws wait for these: every core, cpu_place.h)
#ifndef _WIN32
    { extern int pthread_setname_np(pthread_t, const char *); pthread_setname_np(pthread_self(), "vk-translate"); }   // ([cpu] line)
#endif
    translate_unlocked = translate_threads() > 1;
    jq_serve(&g_tq);
    return NULL;
}
static bool sync_shaders(void);
static void run_translate(void (*fn)(void *), void *arg, uint64_t id) {
    if (sync_shaders()) { fn(arg); return; }
    static bool started;
    if (!started) {
        started = true;
        if (translate_threads() > 1) LOG("[vk] %d translator threads (TOTK_TRANSLATE_THREADS)\n", translate_threads());
        for (int i = 0; i < translate_threads(); i++) { pthread_t t; pthread_create(&t, NULL, translator, NULL); }
    }
    jq_add(&g_tq, fn, arg, 't', id, false);
}

static void run_job(void (*fn)(void *), void *arg, char kind, uint64_t id, bool front) {
    if (sync_shaders()) { fn(arg); return; }
    static bool started;
    if (!started) {
        started = true;
        // first entry into an area builds dozens of pipelines at once: use most of the CPU (draws wait for them), all
        // but the game's 3 cores and the drawing thread; at least 4 (a frame held for them gains nothing from idle cores)
        int n = SDL_GetNumLogicalCPUCores() - 4;
        if (n < 4) n = 4;
        if (n > 16) n = 16;
        LOG("[shader] %d pipeline workers (%d logical CPUs)\n", n, SDL_GetNumLogicalCPUCores());
        for (int i = 0; i < n; i++) { pthread_t t; pthread_create(&t, NULL, worker, NULL); }
    }
    jq_add(&g_jq, fn, arg, kind, id, front);
}

// TOTK_HITCH_HURRY (default on; =0 off, A/B): a draw or dispatch whose skip holds the frame in either mode (a fullscreen /
// tile pass, <= 6 vertices, or a compute dispatch) moves its translation / build ahead of the queued ones: a fight's first
// sighting queues dozens at once, and the held frames waited for the whole FIFO in front of the one they need. Once a
// frame per object.
static bool hurry_on(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("TOTK_HITCH_HURRY"); v = !e || strcmp(e, "0"); }
    return v;
}
static void job_hurry(const void *arg, const char *what, uint64_t id) {
    int t = jq_hurry(&g_tq, arg), w = t < 0 ? jq_hurry(&g_jq, arg) : -1;
    static int n;
    if (!build_log() || n++ >= 1000) return;
    if (t < 0 && w < 0)
        LOG("[build] hurry %s %016llx, frame %llu: not queued (running, or between translation and build)\n", what,
            (unsigned long long)id, (unsigned long long)renderer_present_count());
    else
        LOG("[build] hurry %s %016llx, frame %llu: %s queue, %d jobs ahead of it\n", what, (unsigned long long)id,
            (unsigned long long)renderer_present_count(), t >= 0 ? "translate" : "worker", t >= 0 ? t : w);
}

// ================================================================== state
typedef struct {
    NvnTexture *color[8];
    NvnTextureView cview[8];
    uint8_t has_cview[8];
    int ncolor;
    NvnTexture *depth;
    NvnTextureView dview;
    uint8_t has_dview;
} RtState;

typedef struct { uint64_t addr, size; } Range;

static struct {
    RtState rt;
    bool rt_dirty;
    NvnProgram *program;
    uint8_t blend[8][8];
    uint32_t channel_mask;
    uint8_t color_state[4], ds[8], ms[0x18], poly[4];
    float poly_offset[3];
    uint8_t depth_clamp;  // nvnCommandBufferSetDepthClamp
    uint32_t attrib[16];
    int nattrib;
    uint32_t stream[16][2];
    int nstream;
    Range vbuf[16], ubuf[6][18], sbuf[6][16];
    uint64_t tex[6][32], img[6][8];
    NvnDescPool *tpool, *spool;
    float viewport[16][4];
    int32_t scissor[16][4];
    float depth_range[16][2];
    float blend_color[4];
    uint32_t stencil_ref[2], stencil_mask[2], stencil_vmask[2];
    bool prim_restart;
    uint32_t restart_index;
    uint64_t copy_row_stride, copy_image_stride;
} st;

// ---- capture / replay of the binding state (capture.h): st with its host pointers as ids and back
#include "capture.h"
size_t exec_state_size(void) { return sizeof st; }
void exec_state_save(void *out, uint64_t (*id_of)(int kind, const void *p)) {
    __typeof__(st) c = st;
    for (int i = 0; i < 8; i++) c.rt.color[i] = (NvnTexture *)(uintptr_t)id_of(CAPK_TEX, st.rt.color[i]);
    c.rt.depth = (NvnTexture *)(uintptr_t)id_of(CAPK_TEX, st.rt.depth);
    c.program = (NvnProgram *)(uintptr_t)id_of(CAPK_PROG, st.program);
    c.tpool = (NvnDescPool *)(uintptr_t)id_of(CAPK_TPOOL, st.tpool);
    c.spool = (NvnDescPool *)(uintptr_t)id_of(CAPK_SPOOL, st.spool);
    memcpy(out, &c, sizeof c);
}
static void bind_gen_all(void);
void exec_state_load(const void *in, void *(*ptr_of)(int kind, uint64_t id)) {
    bind_gen_all();   // (every binding may differ: no stage_set memo may answer by generation)
    memcpy(&st, in, sizeof st);
    for (int i = 0; i < 8; i++) st.rt.color[i] = ptr_of(CAPK_TEX, (uint64_t)(uintptr_t)st.rt.color[i]);
    st.rt.depth = ptr_of(CAPK_TEX, (uint64_t)(uintptr_t)st.rt.depth);
    st.program = ptr_of(CAPK_PROG, (uint64_t)(uintptr_t)st.program);
    st.tpool = ptr_of(CAPK_TPOOL, (uint64_t)(uintptr_t)st.tpool);
    st.spool = ptr_of(CAPK_SPOOL, (uint64_t)(uintptr_t)st.spool);
    st.rt_dirty = true;
}

uint32_t nvn_window_origin(void);
uint32_t nvn_depth_mode(void);
bool renderer_origin_lower_left(void) { return nvn_window_origin() == 0; }

// Called once per presented frame: frames over 50 ms are logged with where their time went (always on since 2026-10-18, user:
// stutters in play "w the new shader precompile... or other stuff like texture streaming"; was LARECOMP_VK_PROF only).
// present_ns: time in backend_present's acquire/blit/present. "other" = the game's own threads (game file reads = streaming).
static uint32_t g_drs_w, g_drs_h;   // this frame's G-buffer size (begin_pass: a pass with >= 4 colour targets)
void vk_prof_frame(uint64_t frame, uint64_t present_ns) {
    static uint64_t last_t, l_exec, l_wait, l_draws, l_subs, l_tex, l_fs, l_img, l_img_ns, l_op[CMD_COUNT], l_opc[CMD_COUNT], l_cpu, l_ivcsw;
    static int l_progs;
    extern uint64_t cum_img_n, cum_img_ns;   // (textures.c create_image)
    extern uint64_t cum_up_ww_tk, cum_up_hash_tk, cum_up_copy_tk, cum_up_full, cum_up_sent;   // (textures.c upload_texture_)
    extern double up_ns_per_tick(void);
    static uint64_t l_up[5];
    extern uint64_t cum_view_tk, cum_view_n;   // (textures.c vk_image_view misses)
    static uint64_t l_dtl, l_dtp, l_vn, l_vtk;
    extern unsigned long long hle_fs_read_bytes(void);
    uint64_t now = SDL_GetTicksNS(), fs = hle_fs_read_bytes();
    int progs = atomic_load(&cum_prog_builds);
    static uint64_t hitch_ns;  // LARECOMP_HITCH_MS: log threshold (default 50 ms; 20 shows the late frames of a 60 fps run)
    if (!hitch_ns) hitch_ns = (getenv("LARECOMP_HITCH_MS") ? (uint64_t)atoi(getenv("LARECOMP_HITCH_MS")) : 50) * 1000000ull;
    if (last_t && now - last_t > hitch_ns) {
        uint64_t dt = now - last_t, ex = cum_exec - l_exec, wt = cum_wait - l_wait;
        LOG("[hitch] frame %llu: %.0f ms = backend exec %.1f (of which GPU waits %.1f) + present %.1f + other %.1f; %llu draws, "
            "%llu submits, compiles %d pending, new programs %d, texture upload %.1f MB, game file reads %.1f MB, images %llu (%.1f ms)\n",
            (unsigned long long)frame, dt / 1e6, ex / 1e6, wt / 1e6, present_ns / 1e6,
            ((double)dt - ex - present_ns) / 1e6, (unsigned long long)(cum_draws - l_draws), (unsigned long long)(cum_submits - l_subs),
            atomic_load(&stat_compiles), progs - l_progs, (cum_tex_up - l_tex) / 1048576.0, (fs - l_fs) / 1048576.0,
            (unsigned long long)(cum_img_n - l_img), (cum_img_ns - l_img_ns) / 1e6);
        extern void rt_hitch_dump(uint64_t frame, double ms, double other_ms);   // (rt_main.cpp, TOTK_HITCH_STACKS: its stacks)
        rt_hitch_dump(frame, dt / 1e6, ((double)dt - ex - present_ns) / 1e6);
        LOG("[hitch] frame %llu uploads (render thread): write watch %.1f ms, identify + hash %.1f ms (%llu full), deswizzle / "
            "staging / decode %.1f ms (%llu textures sent)\n", (unsigned long long)frame,
            (cum_up_ww_tk - l_up[0]) * up_ns_per_tick() / 1e6, (cum_up_hash_tk - l_up[1]) * up_ns_per_tick() / 1e6,
            (unsigned long long)(cum_up_full - l_up[3]), (cum_up_copy_tk - l_up[2]) * up_ns_per_tick() / 1e6,
            (unsigned long long)(cum_up_sent - l_up[4]));
        if (PROFD) {  // (VK_PROF=2) first-use split: draws of programs first drawn this frame vs the rest
            const uint64_t *a = hf_pz[1], *b = hf_pz[0];
            LOG("[hitch] frame %llu first use: first-drawn programs %llu draws %.1f ms (pre: program %.1f fs/debug %.1f feedback %.1f "
                "snapshots/begin_pass %.1f topology %.1f), other %llu draws %.1f ms (pre: program %.1f fs/debug %.1f feedback %.1f "
                "snapshots/begin_pass %.1f topology %.1f); new programs: key %.1f claim %.1f record %.1f translate %.1f ms; "
                "drawing-thread links %.1f, compiles %.1f ms; views %llu (%.1f ms)\n", (unsigned long long)frame,
                (unsigned long long)hf_draws[1], hf_draw_ns[1] / 1e6, a[0] / 1e6, a[1] / 1e6, a[2] / 1e6, a[3] / 1e6, a[4] / 1e6,
                (unsigned long long)hf_draws[0], hf_draw_ns[0] / 1e6, b[0] / 1e6, b[1] / 1e6, b[2] / 1e6, b[3] / 1e6, b[4] / 1e6,
                hf_gps[0] / 1e6, hf_gps[1] / 1e6, hf_gps[2] / 1e6, hf_gps[3] / 1e6, (cum_dt_link_ns - l_dtl) / 1e6,
                (cum_dt_prog_ns - l_dtp) / 1e6, (unsigned long long)(cum_view_n - l_vn), (cum_view_tk - l_vtk) * up_ns_per_tick() / 1e6);
        }
#ifndef _WIN32
        LOG("[hitch] frame %llu exec on-CPU %.1f of %.1f ms, %llu involuntary switches\n", (unsigned long long)frame,
            (cum_exec_cpu - l_cpu) / 1e6, ex / 1e6, (unsigned long long)(cum_exec_ivcsw - l_ivcsw));
#else
        if (prof) LOG("[hitch] frame %llu exec on-CPU %.1f of %.1f ms\n", (unsigned long long)frame, (cum_exec_cpu - l_cpu) / 1e6, ex / 1e6);
#endif
        if (prof) {   // this frame's heaviest command kinds (nvn.h CMD_* index), for the periodic exec-side hitches (d84)
            uint64_t d[CMD_COUNT];
            for (int i = 0; i < CMD_COUNT; i++) d[i] = op_ns[i] >= l_op[i] ? op_ns[i] - l_op[i] : op_ns[i];   // op_ns resets every 2 s
            LOG("[hitch] frame %llu ops (kind:ms/count):", (unsigned long long)frame);
            for (int rank = 0; rank < 5; rank++) {
                int best = -1;
                for (int i = 0; i < CMD_COUNT; i++) if (d[i] && (best < 0 || d[i] > d[best])) best = i;
                if (best < 0) break;
                LOG(" %d:%.1f/%llu", best, d[best] / 1e6, (unsigned long long)(op_counts[best] - l_opc[best]));
                d[best] = 0;
            }
            LOG("\n");
        }
    }
    if (prof) { memcpy(l_op, op_ns, sizeof l_op); memcpy(l_opc, op_counts, sizeof l_opc); }
    l_cpu = cum_exec_cpu; l_ivcsw = cum_exec_ivcsw;
    l_tex = cum_tex_up; l_fs = fs; l_progs = progs; l_img = cum_img_n; l_img_ns = cum_img_ns;
    l_up[0] = cum_up_ww_tk; l_up[1] = cum_up_hash_tk; l_up[2] = cum_up_copy_tk; l_up[3] = cum_up_full; l_up[4] = cum_up_sent;
    l_dtl = cum_dt_link_ns; l_dtp = cum_dt_prog_ns; l_vn = cum_view_n; l_vtk = cum_view_tk;
    if (PROFD) { memset(hf_draw_ns, 0, sizeof hf_draw_ns); memset(hf_draws, 0, sizeof hf_draws); memset(hf_pz, 0, sizeof hf_pz); memset(hf_gps, 0, sizeof hf_gps); }
    {   // "[drs]": the 3D resolution (G-buffer size, begin_pass) whenever it changes, for the game's dynamic resolution
        static uint32_t lw, lh;
        static int n;
        if (g_drs_w && (g_drs_w != lw || g_drs_h != lh) && n < 500) {
            LOG("[drs] frame %llu: 3D %ux%u\n", (unsigned long long)frame, g_drs_w, g_drs_h);
            n++; lw = g_drs_w; lh = g_drs_h;
        }
        g_drs_w = g_drs_h = 0;
    }
    {  // LARECOMP_DRS_STRESS_LOG=1: once a second, gsys::GPUStressAnalyzer's state next to
        // "[drs]": ModelMgr (singleton through the GOT at main + 0x4561ad8) +0x6c (the analyzer's enable bit 0), and entry 0
        // of *(ModelMgr + 0x60) (0x588 bytes each) at +0x28 / +0x48 / +0x60. A live analyzer could lower the G-buffer scale
        // under the floor (H0), fade an AA component (H1) or drop a world pass (H2).
        static int on = -1;
        static uint64_t last;
        if (on < 0) { const char *e = getenv("LARECOMP_DRS_STRESS_LOG"); on = e && *e == '1'; }
        const uint64_t now = SDL_GetTicksNS();
        if (on && now - last >= 1000000000ull) {
            last = now;
            extern void *totk_main_base(void);
            const uint8_t *mb = totk_main_base();
            uint64_t got = mb ? (uint64_t)(uintptr_t)(mb + 0x4561ad8) : 0, var = 0, mm = 0, ents = 0;
            if (got && pc_readable(got, 8)) var = *(const uint64_t *)(uintptr_t)got;
            if (var && pc_readable(var, 8)) mm = *(const uint64_t *)(uintptr_t)var;
            if (mm && pc_readable(mm, 0x70)) ents = *(const uint64_t *)(uintptr_t)(mm + 0x60);
            const uint32_t en = mm && pc_readable(mm, 0x70) ? *(const uint32_t *)(uintptr_t)(mm + 0x6c) : 0;
            uint32_t e28 = 0, e48 = 0, e60 = 0;
            const bool eok = ents && pc_readable(ents, 0x588);
            if (eok) {
                e28 = *(const uint32_t *)(uintptr_t)(ents + 0x28); e48 = *(const uint32_t *)(uintptr_t)(ents + 0x48);
                e60 = *(const uint32_t *)(uintptr_t)(ents + 0x60);
            }
            float f28, f48, f60;
            memcpy(&f28, &e28, 4); memcpy(&f48, &e48, 4); memcpy(&f60, &e60, 4);
            LOG("[drs] frame %llu: stress analyzer: GOT %llx -> %llx -> ModelMgr %llx, +0x6c %08x (enable bit %u), entries %llx%s: "
                "+0x28 %08x (%g) +0x48 %08x (%g) +0x60 %08x (%g)\n", (unsigned long long)frame, (unsigned long long)got,
                (unsigned long long)var, (unsigned long long)mm, en, en & 1, (unsigned long long)ents, eok ? "" : " (unreadable)",
                e28, f28, e48, f48, e60, f60);
        }
    }
    // LARECOMP_GPUPROF_SLOW=<ms>: GPU-profile the frame after one slower than <ms> (up to 4 times, >= 120 frames apart;
    // device.c also saves the image and a [mem] report). For the Deck's intermittent ~200 ms GPU frames.
    static long slow_ms = -1;
    static int slow_n;
    static uint64_t slow_last;
    if (slow_ms < 0) slow_ms = getenv("LARECOMP_GPUPROF_SLOW") ? atol(getenv("LARECOMP_GPUPROF_SLOW")) : 0;
    if (slow_ms > 0 && last_t && now - last_t > (uint64_t)slow_ms * 1000000ull && slow_n < 4 && (!slow_n || frame > slow_last + 120)) {
        extern volatile int g_gpuprof_request;
        slow_n++;
        slow_last = frame;
        g_gpuprof_request = 1;
        LOG("[gpuprof] frame %llu took %.0f ms: profiling the next frame (slow trigger %d of 4)\n", (unsigned long long)frame,
            (now - last_t) / 1e6, slow_n);
    }
    last_t = now; l_exec = cum_exec; l_wait = cum_wait; l_draws = cum_draws; l_subs = cum_submits;
}

static void fbc_report(void);
static uint64_t stat_vp_strips;   // (vp_exact: partial edge strip draws, in the stats line)
const char *renderer_stats(void) {
    static int dumps;
    if (dumps++ < 3) {
        LOG("[vk] command counts:");
        for (int i = 0; i < CMD_COUNT; i++) if (op_counts[i]) LOG(" %d:%llu", i, (unsigned long long)op_counts[i]);
        LOG("\n");
    }
    snprintf(stats_text, sizeof stats_text,
             "%llu draws, %llu skipped, %llu passes, %llu dispatches, %llu flushes, wait %.0fms, compile %d, fbcopy %llu (live barriers %llu, depth WAR %llu, self-write %llu, alias %llu), upload %.1fMB (u %.0f c0 %.0f c1 %.0f z %.0f v %.0f i %.0f t %.0f o %.0f), gdev %d regions %.1fMB in (snapshot %.1f, host fallbacks %llu)",
             (unsigned long long)stat_draws, (unsigned long long)stat_skipped, (unsigned long long)stat_passes,
             (unsigned long long)stat_dispatches, (unsigned long long)stat_flushes, (double)stat_wait_ns / 1e6,
             atomic_exchange(&stat_compiles, 0), (unsigned long long)stat_feedback_copies, (unsigned long long)stat_feedback_live, (unsigned long long)stat_depth_war, (unsigned long long)stat_fb_selfw, (unsigned long long)stat_fb_alias, (double)stat_upload_bytes / 1048576.0, stat_up_kind[1] / 1048576.0, stat_up_kind[5] / 1048576.0, stat_up_kind[6] / 1048576.0,
             stat_up_kind[7] / 1048576.0, stat_up_kind[2] / 1048576.0, stat_up_kind[3] / 1048576.0,
             stat_up_kind[4] / 1048576.0, stat_up_kind[0] / 1048576.0,
             gdev_n, (double)gdev_stat_in / 1048576.0, (double)gdev_stat_snap / 1048576.0, (unsigned long long)gdev_stat_host);
    stat_feedback_copies = stat_feedback_live = stat_depth_war = stat_fb_selfw = stat_fb_alias = stat_upload_bytes = 0;
    memset(stat_up_kind, 0, sizeof stat_up_kind);
    gdev_stat_in = gdev_stat_snap = gdev_stat_host = 0;
    {
        size_t n = strlen(stats_text);
        snprintf(stats_text + n, sizeof stats_text - n, ", pass barriers %llu (skipped %llu; game barriers %llu, %llu mid-pass breaks; feedback elided %llu; hazard barriers %llu; elided small %llu, after a full one %llu)",
                 (unsigned long long)stat_pass_barriers, (unsigned long long)stat_pass_nobarrier, (unsigned long long)stat_game_barriers,
                 (unsigned long long)stat_game_barrier_breaks, (unsigned long long)stat_fb_elided, (unsigned long long)stat_hazard_barriers,
                 (unsigned long long)stat_pass_elided, (unsigned long long)stat_pass_fresh);
        stat_pass_barriers = stat_pass_nobarrier = stat_game_barriers = stat_game_barrier_breaks = stat_fb_elided = stat_hazard_barriers = 0;
        stat_pass_elided = stat_pass_fresh = 0;
        n = strlen(stats_text);
        snprintf(stats_text + n, sizeof stats_text - n, ", vp_exact edge strips %llu", (unsigned long long)stat_vp_strips);
        stat_vp_strips = 0;
        n = strlen(stats_text);
        snprintf(stats_text + n, sizeof stats_text - n, " | game barrier bits (in pass %llu):", (unsigned long long)stat_bar_inpass);
        stat_bar_inpass = 0;
        for (int k = 0; k < 4; k++) {   // (top 4 values)
            int best = -1;
            for (int b = 0; b < 512; b++) if (stat_bar_bits[b] && (best < 0 || stat_bar_bits[b] > stat_bar_bits[best])) best = b;
            if (best < 0) break;
            n = strlen(stats_text);
            snprintf(stats_text + n, sizeof stats_text - n, " %#x x%llu", best, (unsigned long long)stat_bar_bits[best]);
            stat_bar_bits[best] = 0;
        }
        memset(stat_bar_bits, 0, sizeof stat_bar_bits);
        n = strlen(stats_text);
        { extern void vkq_stats(char *out, size_t n); vkq_stats(stats_text + n, sizeof stats_text - n); n = strlen(stats_text); }
        if (getenv("LARECOMP_COUNT_FSHASH")) {
            snprintf(stats_text + n, sizeof stats_text - n, " | counted fs draws %llu", (unsigned long long)stat_count_fs);
            stat_count_fs = 0;
            n = strlen(stats_text);
        }
        snprintf(stats_text + n, sizeof stats_text - n, " | pass barrier causes:");
        for (int k = 0; k < 6; k++) {   // (top 6: exec.c line of the vk_end_pass, game = game barrier, start, forced)
            int best = -1;
            for (int b = 0; b < 8192; b++) if (stat_pbn_cause[b] && (best < 0 || stat_pbn_cause[b] > stat_pbn_cause[best])) best = b;
            if (best < 0) break;
            n = strlen(stats_text);
            if (best == 1) snprintf(stats_text + n, sizeof stats_text - n, " game x%u", stat_pbn_cause[best]);
            else if (best == 2) snprintf(stats_text + n, sizeof stats_text - n, " start x%u", stat_pbn_cause[best]);
            else if (best == 3) snprintf(stats_text + n, sizeof stats_text - n, " forced x%u", stat_pbn_cause[best]);
            else snprintf(stats_text + n, sizeof stats_text - n, " L%d x%u", best, stat_pbn_cause[best]);
            stat_pbn_cause[best] = 0;
        }
        memset(stat_pbn_cause, 0, sizeof stat_pbn_cause);
    }
    if (stat_oq_reports) {
        size_t n = strlen(stats_text);
        snprintf(stats_text + n, sizeof stats_text - n, ", occlusion queries %llu (hidden %llu, kept visible %llu)", (unsigned long long)stat_oq_reports,
                 (unsigned long long)stat_oq_hidden, (unsigned long long)stat_oq_visible_fallback);
        stat_oq_reports = stat_oq_hidden = stat_oq_visible_fallback = 0;
    }
    if (stat_fbl_moves) {
        size_t n = strlen(stats_text);
        snprintf(stats_text + n, sizeof stats_text - n, ", feedback-loop layout moves %llu", (unsigned long long)stat_fbl_moves);
        stat_fbl_moves = 0;
    }
    if (stat_snap_delta) {
        size_t n = strlen(stats_text);
        snprintf(stats_text + n, sizeof stats_text - n, ", snapshot deltas %llu (%.1f %% of their pixels copied)", (unsigned long long)stat_snap_delta,
                 stat_snap_delta_full_px ? 100.0 * (double)stat_snap_delta_px / (double)stat_snap_delta_full_px : 0.0);
        stat_snap_delta = stat_snap_delta_px = stat_snap_delta_full_px = 0;
        if (stat_snap_tiles_n) {
            n = strlen(stats_text);
            snprintf(stats_text + n, sizeof stats_text - n, ", by marked tiles %llu", (unsigned long long)stat_snap_tiles_n);
            stat_snap_tiles_n = 0;
        }
    }
    if (stat_fb_selfreuse) {
        size_t n = strlen(stats_text);
        snprintf(stats_text + n, sizeof stats_text - n, ", self-copy reused %llu", (unsigned long long)stat_fb_selfreuse);
        stat_fb_selfreuse = 0;
    }
    if (stat_sync_early) {
        size_t n = strlen(stats_text);
        snprintf(stats_text + n, sizeof stats_text - n, ", early syncwaits %llu", (unsigned long long)stat_sync_early);
        stat_sync_early = 0;
    }
    {   // programs the game asked for that no precompile had built: translate / SPIR-V + modules time (first-sight cost)
        extern _Atomic int stat_prog_builds;
        extern _Atomic uint64_t stat_prog_translate_ns, stat_prog_compile_ns;
        int nb = atomic_exchange(&stat_prog_builds, 0);
        uint64_t tn = atomic_exchange(&stat_prog_translate_ns, 0), cn = atomic_exchange(&stat_prog_compile_ns, 0);
        if (nb) {
            size_t n = strlen(stats_text);
            snprintf(stats_text + n, sizeof stats_text - n, ", new programs %d (translate %.0f ms, spirv+modules+libs %.0f ms)", nb, tn / 1e6, cn / 1e6);
        }
        extern _Atomic int stat_gpl_links;
        extern _Atomic uint64_t stat_gpl_link_ns;
        int nl = atomic_exchange(&stat_gpl_links, 0);
        uint64_t ln = atomic_exchange(&stat_gpl_link_ns, 0);
        if (nl) {
            size_t n = strlen(stats_text);
            snprintf(stats_text + n, sizeof stats_text - n, ", linked %d pipelines (%.1f ms)", nl, ln / 1e6);
        }
        if (stat_dt_links || stat_dt_progs || stat_link_deferred || stat_prog_deferred) {
            size_t n = strlen(stats_text);
            snprintf(stats_text + n, sizeof stats_text - n, ", draw-thread compile %.1f ms (links %llu %.1f ms, %llu of them queued ones claimed; "
                     "programs %llu %.1f ms; to workers: links %llu, programs %llu; bake timeouts %llu)", (stat_dt_link_ns + stat_dt_prog_ns) / 1e6,
                     (unsigned long long)stat_dt_links, stat_dt_link_ns / 1e6, (unsigned long long)stat_link_claimed,
                     (unsigned long long)stat_dt_progs, stat_dt_prog_ns / 1e6,
                     (unsigned long long)stat_link_deferred, (unsigned long long)stat_prog_deferred, (unsigned long long)bake_timeouts);
            stat_dt_link_ns = stat_dt_prog_ns = stat_dt_links = stat_dt_progs = stat_link_deferred = stat_prog_deferred = stat_link_claimed = 0;
        }
    }
    if (chk_words) {
        size_t n = strlen(stats_text);
        snprintf(stats_text + n, sizeof stats_text - n, ", gdev-check %llu words changed / %llu overwritten", (unsigned long long)chk_words,
                 (unsigned long long)chk_clob);
        chk_words = chk_clob = 0;
    }
    if (prof) {
        fbc_report();
        LOG("[prof] stage sets: alloc %.0f cbuf %.0f fill %.0f update %.0f ms, %llu writes, %llu sets reused | dynamic state %llu set, %llu skipped\n",
            prof_set_alloc / 1e6, prof_set_cbuf / 1e6, prof_set_fill / 1e6, prof_set_update / 1e6, (unsigned long long)prof_set_writes,
            (unsigned long long)prof_set_reused, (unsigned long long)prof_dyn_set, (unsigned long long)prof_dyn_skip);
        LOG("[prof] pipelines: %llu bound, %llu rebinds skipped\n", (unsigned long long)prof_pipe_bind, (unsigned long long)prof_pipe_skip);
        LOG("[prof] stage sets allocated %llu: an identical one from an earlier submission (no view destroyed since) %llu, "
            "this submission under another program's equal layout %llu (G6 count)\n", (unsigned long long)prof_set_allocs,
            (unsigned long long)prof_set_xsub, (unsigned long long)prof_set_shlayout);
        prof_set_allocs = prof_set_xsub = prof_set_shlayout = 0;
        {   // G5 hit rates, and how often their generations moved (pool entry writes, views destroyed, alias writers)
            extern uint64_t prof_tic_hit, prof_tic_miss, prof_tic_wmiss, prof_gd_hit, prof_gd_miss;
            extern uint32_t g_pool_gen, vk_view_gen, vk_alias_gen;
            static uint32_t lpg, lvg, lag;
            uint32_t pg = __atomic_load_n(&g_pool_gen, __ATOMIC_RELAXED);
            LOG("[prof] G5 caches: texture_info hits %llu, misses %llu (of which a changed writer %llu); guest_direct hits %llu, "
                "misses %llu | generations moved: pool %u, views %u, alias writers %u\n", (unsigned long long)prof_tic_hit,
                (unsigned long long)prof_tic_miss, (unsigned long long)prof_tic_wmiss, (unsigned long long)prof_gd_hit,
                (unsigned long long)prof_gd_miss, pg - lpg, vk_view_gen - lvg, vk_alias_gen - lag);
            lpg = pg; lvg = vk_view_gen; lag = vk_alias_gen;
            prof_tic_hit = prof_tic_miss = prof_tic_wmiss = prof_gd_hit = prof_gd_miss = 0;
        }
        LOG("[prof] stage_set same as the stage's previous call: vs %llu/%llu (+dyn %llu), fs %llu/%llu (+dyn %llu), cs %llu/%llu (+dyn %llu)\n",
            (unsigned long long)prof_set_same[0], (unsigned long long)prof_set_calls[0], (unsigned long long)prof_set_samedyn[0],
            (unsigned long long)prof_set_same[1], (unsigned long long)prof_set_calls[1], (unsigned long long)prof_set_samedyn[1],
            (unsigned long long)prof_set_same[5], (unsigned long long)prof_set_calls[5], (unsigned long long)prof_set_samedyn[5]);
        LOG("[prof] stage_set inputs memo (would skip / of which built a different set): vs %llu/%llu, fs %llu/%llu, cs %llu/%llu\n",
            (unsigned long long)prof_memo_hit[0], (unsigned long long)prof_memo_bad[0], (unsigned long long)prof_memo_hit[1],
            (unsigned long long)prof_memo_bad[1], (unsigned long long)prof_memo_hit[5], (unsigned long long)prof_memo_bad[5]);
        LOG("[prof] c1 constants: alias mirror %llu (%.1f MB), zero-copy %llu, heap mirror %llu (%.1f MB), copied: below heap %llu (%.1f MB), misaligned %llu (%.1f MB), "
            "mirror refused %llu (%.1f MB), mirror off %llu | guest_copy: hits %llu, new %llu (%.1f MB), collisions %llu (%.1f MB)\n",
            (unsigned long long)prof_c1[6], prof_c1_b[6] / 1048576.0, (unsigned long long)prof_c1[0], (unsigned long long)prof_c1[1], prof_c1_b[1] / 1048576.0, (unsigned long long)prof_c1[2], prof_c1_b[2] / 1048576.0,
            (unsigned long long)prof_c1[3], prof_c1_b[3] / 1048576.0, (unsigned long long)prof_c1[4], prof_c1_b[4] / 1048576.0, (unsigned long long)prof_c1[5],
            (unsigned long long)prof_gc_hit, (unsigned long long)prof_gc_new, prof_gc_new_b / 1048576.0, (unsigned long long)prof_gc_evict, prof_gc_evict_b / 1048576.0);
        LOG("[prof] c1 mirror refusals: shader-written page %llu, past the mirror %llu, straddle %llu, no block %llu, mirror off %llu, cold %llu | "
            "of the copied, persistent (LARECOMP_C1_PERSIST) %llu (%.1f MB not copied), new %llu, forgotten %llu (kept %llu), changed %llu, %d entries in %d MB\n",
            (unsigned long long)prof_hmw[1], (unsigned long long)prof_hmw[2], (unsigned long long)prof_hmw[3], (unsigned long long)prof_hmw[4], (unsigned long long)prof_hmw[5], (unsigned long long)prof_hmw[6],
            (unsigned long long)prof_c1[7], prof_c1_b[7] / 1048576.0, (unsigned long long)prof_c1p_new, (unsigned long long)prof_c1p_forgot, (unsigned long long)prof_c1p_kept, (unsigned long long)prof_c1p_changed, c1p_n,
            (int)(c1p_nblk * (C1P_BLOCK >> 20)));
        prof_c1p_new = prof_c1p_forgot = prof_c1p_kept = 0;
        memset(prof_hmw, 0, sizeof prof_hmw);
        memset(prof_c1, 0, sizeof prof_c1); memset(prof_c1_b, 0, sizeof prof_c1_b);
        prof_gc_hit = prof_gc_new = prof_gc_new_b = prof_gc_evict = prof_gc_evict_b = 0;
        for (int sg = 0; sg < 2; sg++) {
            uint64_t *y = prof_memo_why[sg];
            LOG("[prof] stage_set memo misses with the same set, %s: gen %llu, seq/frame %llu, views/pool entries %llu, prog/pools %llu, tex %llu, ubuf %llu, sbuf %llu, pre %llu, other %llu, global memory (SET_MEMO_GMEM=0) %llu\n",
                sg ? "fs" : "vs", (unsigned long long)y[0], (unsigned long long)y[1], (unsigned long long)y[2], (unsigned long long)y[3], (unsigned long long)y[4],
                (unsigned long long)y[5], (unsigned long long)y[6], (unsigned long long)y[7], (unsigned long long)y[8], (unsigned long long)y[9]);
        }
        for (int sg = 0; sg < 2; sg++) {
            const uint64_t *o = prof_memo_other[sg];
            LOG("[prof] stage_set 'other' misses, %s: feedback %llu, pause substitution %llu, rscale row 0 %llu, tint %llu, info / set %llu, groups %llu, pre-resolved %llu, bind / storage generations %llu\n",
                sg ? "fs" : "vs", (unsigned long long)o[0], (unsigned long long)o[1], (unsigned long long)o[2], (unsigned long long)o[3],
                (unsigned long long)o[4], (unsigned long long)o[5], (unsigned long long)o[6], (unsigned long long)o[7]);
        }
        memset(prof_memo_other, 0, sizeof prof_memo_other);
        memset(prof_memo_why, 0, sizeof prof_memo_why);
        memset(prof_memo_hit, 0, sizeof prof_memo_hit); memset(prof_memo_bad, 0, sizeof prof_memo_bad);
        memset(prof_set_calls, 0, sizeof prof_set_calls); memset(prof_set_same, 0, sizeof prof_set_same); memset(prof_set_samedyn, 0, sizeof prof_set_samedyn);
        prof_pipe_bind = prof_pipe_skip = 0;
        prof_set_alloc = prof_set_cbuf = prof_set_fill = prof_set_update = prof_set_writes = prof_set_reused = 0;
        prof_dyn_set = prof_dyn_skip = 0;
        prof_up = prof_up_ns;
        prof_up_ns = 0;
        size_t n = strlen(stats_text);
        snprintf(stats_text + n, sizeof stats_text - n, " | gpu %.0f cpu exec %.0f draw %.0f (pre %.0f key %.0f dyn %.0f sets %.0f tex %.0f (upload %.0f) vtx %.0f) submit %.0f ms",
                 prof_gpu / 1e6, prof_exec / 1e6, prof_draw / 1e6, prof_pre / 1e6, prof_key / 1e6, prof_dyn / 1e6, prof_sets / 1e6,
                 prof_tex / 1e6, prof_up / 1e6, prof_vtx / 1e6, prof_submit / 1e6);
        n = strlen(stats_text);
        snprintf(stats_text + n, sizeof stats_text - n, " | pre: program %.0f fs/debug %.0f feedback %.0f begin_pass %.0f topology %.0f ms",
                 prof_pz[0] / 1e6, prof_pz[1] / 1e6, prof_pz[2] / 1e6, prof_pz[3] / 1e6, prof_pz[4] / 1e6);
        memset(prof_pz, 0, sizeof prof_pz);
        extern uint64_t prof_tex_checks, prof_tex_samples, prof_tex_full, prof_tex_full_bytes, prof_tex_dyn, prof_tex_ww_clean, prof_tex_gpuw_skip;
        n = strlen(stats_text);
        snprintf(stats_text + n, sizeof stats_text - n, " | vtx copies %llu (%.1f MB, below heap %.1f MB, region %llx0000000, gmem %.0f MB, window straddles %llu)",
                 (unsigned long long)prof_vx_copy_n, prof_vx_copy_bytes / 1e6, prof_vx_low_bytes / 1e6,
                 (unsigned long long)prof_vx_low_region, gmem_size / 1e6, (unsigned long long)prof_vx_straddle);
        n = strlen(stats_text);
        snprintf(stats_text + n, sizeof stats_text - n, " | mirror block edges: joined %llu (%.1f MB, %llu shared), copied %llu (%.1f MB)",
                 (unsigned long long)prof_mst_n, prof_mst_bytes / 1e6, (unsigned long long)prof_mst_shared,
                 (unsigned long long)prof_mst_refused, prof_mst_refused_bytes / 1e6);
        prof_vx_copy_n = prof_vx_copy_bytes = prof_vx_low_bytes = prof_vx_straddle = 0;
        prof_mst_n = prof_mst_bytes = prof_mst_shared = prof_mst_refused = prof_mst_refused_bytes = 0;
        for (int i = 0; i < 16 && up0_site[i].ra; i++) {
            if (up0_site[i].bytes < (1u << 20)) continue;
            n = strlen(stats_text);
            snprintf(stats_text + n, sizeof stats_text - n, " | up0 vk_transient_copy%+lld: %.0f MB / %llu",
                     (long long)(up0_site[i].ra - (uintptr_t)&vk_transient_copy), up0_site[i].bytes / 1e6, (unsigned long long)up0_site[i].n);
        }
        memset(up0_site, 0, sizeof up0_site);
        if (prof_vx_inflight) {
            n = strlen(stats_text);
            snprintf(stats_text + n, sizeof stats_text - n, " | vtx in place (written in flight) %llu", (unsigned long long)prof_vx_inflight);
            prof_vx_inflight = 0;
        }
        if (prof_vxp_dup + prof_vxp_same + prof_vxp_new) {
            n = strlen(stats_text);
            snprintf(stats_text + n, sizeof stats_text - n, " | vx pages: dup in submission %.0f MB, unchanged %.0f MB, changed/new %.0f MB",
                     prof_vxp_dup / 1e6, prof_vxp_same / 1e6, prof_vxp_new / 1e6);
            prof_vxp_dup = prof_vxp_same = prof_vxp_new = 0;
        }
        extern uint64_t prof_alias_syncs, prof_alias_bytes, prof_alias_ranges, prof_alias_ns, prof_hm_syncs, prof_hm_bytes, prof_hm_ranges, prof_hm_ns, prof_hm_vol;
        n = strlen(stats_text);
        extern uint64_t prof_ww_calls, prof_ww_batched;
        snprintf(stats_text + n, sizeof stats_text - n, " | alias mirror syncs %llu, %.1f MB in %llu ranges, %.1f ms; mirror write-watch queries %llu, chunk syncs from a batch %llu",
                 (unsigned long long)prof_alias_syncs, prof_alias_bytes / 1e6, (unsigned long long)prof_alias_ranges, prof_alias_ns / 1e6,
                 (unsigned long long)prof_ww_calls, (unsigned long long)prof_ww_batched);
        prof_ww_calls = prof_ww_batched = 0;
        prof_alias_syncs = prof_alias_bytes = prof_alias_ranges = prof_alias_ns = 0;
        if (prof_hm_syncs) {
            n = strlen(stats_text);
            snprintf(stats_text + n, sizeof stats_text - n, " | heap mirror syncs %llu, %.1f MB in %llu ranges, %.1f ms, %llu pages rewritten within a submission",
                     (unsigned long long)prof_hm_syncs, prof_hm_bytes / 1e6, (unsigned long long)prof_hm_ranges, prof_hm_ns / 1e6,
                     (unsigned long long)prof_hm_vol);
            prof_hm_syncs = prof_hm_bytes = prof_hm_ranges = prof_hm_ns = prof_hm_vol = 0;
        }
        n = strlen(stats_text);
        snprintf(stats_text + n, sizeof stats_text - n, " | pipeline waits: built inline %llu, waited %llu, %.1f ms",
                 (unsigned long long)prof_pipe_inline, (unsigned long long)prof_pipe_waited, prof_pipe_wait_ns / 1e6);
        prof_pipe_inline = prof_pipe_waited = prof_pipe_wait_ns = 0;
        if (prof_bake_waits) {
            n = strlen(stats_text);
            snprintf(stats_text + n, sizeof stats_text - n, " | bake program waits %llu, %.1f ms", (unsigned long long)prof_bake_waits, prof_bake_wait_ns / 1e6);
            prof_bake_waits = prof_bake_wait_ns = 0;
        }
        n = strlen(stats_text);
        snprintf(stats_text + n, sizeof stats_text - n, " | cond render evals %llu, false %llu, skipped cmds %llu, flips %llu",
                 (unsigned long long)g_cond_evals, (unsigned long long)g_cond_off, (unsigned long long)g_cond_skipped,
                 (unsigned long long)g_cond_flips);
        g_cond_evals = g_cond_off = g_cond_skipped = g_cond_flips = 0;
        n = strlen(stats_text);
        snprintf(stats_text + n, sizeof stats_text - n, " | chunks %llu | syncwait %llu blocking, age %.1f ms, wait %.1f ms avg | waits",
                 (unsigned long long)prof_chunks, (unsigned long long)sw_n, sw_n ? sw_age_ns / 1e6 / sw_n : 0.0,
                 sw_n ? sw_wait_ns / 1e6 / sw_n : 0.0);
        prof_chunks = sw_n = sw_age_ns = sw_wait_ns = 0;
        if (ubo_zc > 0 || stat_ubo_zc) {
            n = strlen(stats_text);
            snprintf(stats_text + n, sizeof stats_text - n, " | ubo zero-copy %llu, copied %llu", (unsigned long long)stat_ubo_zc,
                     (unsigned long long)stat_ubo_copy);
            stat_ubo_zc = stat_ubo_copy = 0;
        }
        for (int i = 0; i < WSITES && wsite[i].file; i++) {
            uint64_t ns = atomic_exchange(&wsite[i].ns, 0), cnt = atomic_exchange(&wsite[i].n, 0);
            if (ns < 1000000) continue;
            const char *f = wsite[i].file, *b1 = strrchr(f, '/'), *b2 = strrchr(f, '\\');
            if (b2 > b1) b1 = b2;
            n = strlen(stats_text);
            snprintf(stats_text + n, sizeof stats_text - n, " %s:%d %.0fms/%llu", b1 ? b1 + 1 : f, wsite[i].line, ns / 1e6,
                     (unsigned long long)cnt);
        }
        n = strlen(stats_text);
        snprintf(stats_text + n, sizeof stats_text - n, " | tex checks %llu samples %llu full %llu (%.1f MB, dynamic %llu) ww clean %llu, periodic rehash skipped (no GPU write) %llu",
                 (unsigned long long)prof_tex_checks, (unsigned long long)prof_tex_samples, (unsigned long long)prof_tex_full,
                 prof_tex_full_bytes / 1048576.0, (unsigned long long)prof_tex_dyn, (unsigned long long)prof_tex_ww_clean, (unsigned long long)prof_tex_gpuw_skip);
        n = strlen(stats_text);
        snprintf(stats_text + n, sizeof stats_text - n, " | pass ends:");
        for (int l = 0; l < 4096; l++)
            if (prof_end_line[l] > 20) { n = strlen(stats_text); snprintf(stats_text + n, sizeof stats_text - n, " %d:%u", l, prof_end_line[l]); }
        memset(prof_end_line, 0, sizeof prof_end_line);
        n = strlen(stats_text);
        snprintf(stats_text + n, sizeof stats_text - n, " | views created %llu", (unsigned long long)prof_views_created);
        prof_views_created = 0;
        {   // command kinds by render-thread self time (from the record to the next one), with their counts
            LOG("[prof] begin_pass %llu: %.0f ms (prev end %.0f, views %.0f)\n", (unsigned long long)prof_bp_n, prof_bp / 1e6, prof_bp_end / 1e6,
                prof_bp_view / 1e6);
            LOG("[prof] begin_pass zones: vk_cmd %.0f barrier %.0f dl_note %.0f BeginRendering %.0f ms\n", prof_bpz[0] / 1e6,
                prof_bpz[1] / 1e6, prof_bpz[2] / 1e6, prof_bpz[3] / 1e6);
            memset(prof_bpz, 0, sizeof prof_bpz);
            prof_bp = prof_bp_view = prof_bp_end = prof_bp_n = 0;
            extern uint64_t prof_img_n, prof_img_ns;
            extern uint64_t prof_img_reused, prof_img_same_addr;
            LOG("[prof] images created %llu: %.0f ms, reused %llu (same storage %llu)\n", (unsigned long long)prof_img_n, prof_img_ns / 1e6,
                (unsigned long long)prof_img_reused, (unsigned long long)prof_img_same_addr);
            prof_img_n = prof_img_ns = prof_img_reused = prof_img_same_addr = 0;
            LOG("[prof] vertex/index binds: %llu done, %llu skipped (same as bound)\n", (unsigned long long)prof_binds_done,
                (unsigned long long)prof_binds_skipped);
            prof_binds_done = prof_binds_skipped = 0;
            LOG("[prof] ops (kind:ms/count):");
            for (int rank = 0; rank < 12; rank++) {
                int best = -1;
                for (int i = 0; i < CMD_COUNT; i++) if (op_ns[i] && (best < 0 || op_ns[i] > op_ns[best])) best = i;
                if (best < 0) break;
                LOG(" %d:%.0f/%llu", best, op_ns[best] / 1e6, (unsigned long long)(op_counts[best] - op_n2[best]));
                op_ns[best] = 0;
            }
            LOG("\n");
            memset(op_ns, 0, sizeof op_ns);
            memcpy(op_n2, op_counts, sizeof op_n2);
        }
        prof_tex_checks = prof_tex_samples = prof_tex_full = prof_tex_full_bytes = prof_tex_dyn = prof_tex_ww_clean = prof_tex_gpuw_skip = 0;
        prof_exec = prof_draw = prof_sets = prof_vtx = prof_submit = prof_pre = prof_key = prof_dyn = prof_tex = prof_up = prof_gpu = 0;
    }
    if (stat_skipped) {
        LOG("[vk] skip why: no program %llu, program not built %llu, hash %llu, topology %llu, pipeline not built %llu, cursor %llu, compute %llu\n",
            (unsigned long long)stat_skip_why[0], (unsigned long long)stat_skip_why[1], (unsigned long long)stat_skip_why[2],
            (unsigned long long)stat_skip_why[3], (unsigned long long)stat_skip_why[4], (unsigned long long)stat_skip_why[5],
            (unsigned long long)stat_skip_why[6]);
        memset(stat_skip_why, 0, sizeof stat_skip_why);
    }
    stat_draws = stat_skipped = stat_passes = stat_dispatches = stat_flushes = stat_wait_ns = 0;
    return stats_text;
}

static uint64_t g_pk_gen;   // (draw_'s pipeline-key fast path, LARECOMP_PK_FAST: bumped when its state inputs change)
// LARECOMP_PK_GEN_NARROW: exec_list bumps g_pk_gen only for the bytes the key reads (draw_ "pipeline"): the polygon bind's mode byte
// poly[2] (cull, face, bias are dynamic state), the multisample bind's ms[0xC] / ms[0xD] (A2C, its dither), the colour bind's byte 0
// (blend enables), a blend bind only while its target's enable bit is set (the key copies blend[i] only then; a later enable bumps
// through the colour bind), the targets only when they differ, depth clamp only when it changes. TotK re-issues those binds with
// other bytes changed between most draws: the fast path hit 16 % of draws in gc63s_kak. LARECOMP_PK_FAST=2 checks it.
static int g_pk_narrow = -1;
enum { PKB_DEFAULTS, PKB_BLEND, PKB_CHANNEL, PKB_COLOR, PKB_MS, PKB_POLY, PKB_ATTRIB, PKB_STREAM, PKB_RT, PKB_CLAMP, PKB_N };
static uint64_t g_pk_bumps[PKB_N];   // (bumps per command kind, in the fast path's log line)
#define PK_BUMP(kind) (g_pk_gen++, g_pk_bumps[kind]++)
static bool pk_narrow(void) {
    if (g_pk_narrow < 0) { const char *e = getenv("LARECOMP_PK_GEN_NARROW"); g_pk_narrow = !e || *e != '0'; if (g_pk_narrow) LOG("[vk] pipeline-key generation: only the bytes the key reads\n"); }
    return g_pk_narrow;
}
static void bind_gen_all(void);
static void st_defaults(void) {
    PK_BUMP(PKB_DEFAULTS);
    bind_gen_all();
    memset(&st, 0, sizeof st);
    for (int i = 0; i < 16; i++) {
        st.viewport[i][2] = 1280; st.viewport[i][3] = 720;
        st.scissor[i][2] = 16384; st.scissor[i][3] = 16384;
        st.depth_range[i][1] = 1.0f;
    }
    st.channel_mask = 0xFFFFFFFF;
    st.stencil_mask[0] = st.stencil_mask[1] = 0xFF;
    st.stencil_vmask[0] = st.stencil_vmask[1] = 0xFF;
    st.ds[1] = 1; st.ds[2] = 2;
    st.poly[1] = 1; st.poly[2] = 2;
    for (int i = 0; i < 8; i++) { st.blend[i][1] = 2; st.blend[i][2] = 1; st.blend[i][3] = 2; st.blend[i][4] = 1; st.blend[i][5] = 1; st.blend[i][6] = 1; }
}

static void import_limits(void);
void vk_exec_init(void) {
    import_limits();   // (maxwell_gmem_window: every translation, from the first, addresses the heap through the windows)
    // (=0: off; run.sh defaults it to 1; boot.py sets 1.) =2 also times every command record by kind ("[prof] ops", the
    // ops of "[hitch]" lines): two clock reads per record were most of =1's cost - the replay's render thread +0.7 ms a
    // frame of ~5.6 at 4K with them, so boots measured the render thread ~12 % slower than the game runs without the switch.
    const char *pe = getenv("LARECOMP_VK_PROF");
    prof = !pe || !strcmp(pe, "0") ? 0 : atoi(pe) >= 2 ? 2 : 1;
    if (prof) {   // (prof_now: TSC ticks per ns over ~20 ms)
        uint64_t n0 = SDL_GetTicksNS(), c0 = __rdtsc(), n1;
        while ((n1 = SDL_GetTicksNS()) - n0 < 20000000ull) {}
        uint64_t c1 = __rdtsc();
        prof_ns_per_tick = (double)(n1 - n0) / (double)(c1 - c0);
        prof_tsc0 = c1; prof_ns0 = n1;
    }
    if (prof) {
        prof_up_ns = 0;
        VkQueryPoolCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = SUBS * 2};
        if (vkCreateQueryPool(g_vk.dev, &qi, NULL, &prof_qp) != VK_SUCCESS) prof_qp = VK_NULL_HANDLE;
    }
    st_defaults();
}

void renderer_texture_scale(NvnTexture *t, float *sx, float *sy);
void renderer_texture_sample_scale(NvnTexture *t, float *sx, float *sy);
void renderer_texture_promote(NvnTexture *t);
void renderer_texture_promote_kind(NvnTexture *t, int kind);
bool renderer_texture_presented(NvnTexture *t);
bool renderer_texture_first_write(NvnTexture *t);
bool renderer_texture_level_fresh(NvnTexture *t);
bool renderer_texture_frame_fresh(NvnTexture *t);
void renderer_texture_mark_presented(NvnTexture *t);
uint64_t renderer_present_count(void);
void renderer_save_texture_tex(TexBackend *b, const char *path);
static float pass_sx = 1, pass_sy = 1;
static float vp_sx = 1, vp_sy = 1, vp_ox = 0, vp_oy = 0;
static float tex_sc[2][64][3];   // [2]: 1 = a D16 image sampled with depth compare (translate.c d16q)
static uint32_t tex_wh[2][64][2];

// ================================================================== render passes
static VkFormat pass_color[8], pass_depth, pass_stencil;
static uint32_t pass_ncolor, pass_samples = 1, pass_w, pass_h;
static int debug_pass_no;

// Passes into block shadows (textures.c block_shadow): after the pass, the rendered level / slice is copied into the BC
// image (size-compatible copy: extent in the shadow's texels = blocks of the BC image).
typedef struct { TexBackend *bc, *shadow; uint32_t level, z; } BlkCopy;
static BlkCopy pass_blk[8];
static int pass_nblk;
static TexBackend *pass_twin[8];   // (colour targets of this pass with a depth twin: textures.c vk_depth_twin_copy after it)
static int pass_ntwin;
static uint64_t stat_blk_copies;

static void blk_copy_back(VkCommandBuffer cb) {
    // The twin is refreshed when a draw / dispatch next compare-samples it (twins_refresh), not after every pass: TotK draws its
    // 1600x1600 x4 D16 shadow cascades (colour images off NVIDIA: textures.c renderer_texture_target_kind) in a pass per layer,
    // and each pass copied the whole array. LARECOMP_TWIN_LAZY=0: a copy after each pass (old).
    static int lazy = -1;
    if (lazy < 0) { const char *e = getenv("LARECOMP_TWIN_LAZY"); lazy = !(e && *e == '0'); }
    for (int i = 0; i < pass_ntwin; i++) {
        if (lazy) pass_twin[i]->twin_valid = false;
        else vk_depth_twin_copy(cb, pass_twin[i]);
    }
    pass_ntwin = 0;
    if (!pass_nblk) return;
    vk_full_barrier(cb);
    for (int i = 0; i < pass_nblk; i++) {
        const BlkCopy *c = &pass_blk[i];
        TexBackend *bc = c->bc, *s = c->shadow;
        if (!bc || !s || !bc->image || !s->image || c->level >= s->levels || c->level >= bc->levels) continue;
        bool is3d = s->base_type == VK_IMAGE_VIEW_TYPE_3D;
        uint32_t sw = s->width >> c->level, sh = s->height >> c->level, sd = is3d ? s->depth >> c->level : s->layers;
        uint32_t bw = bc->width >> c->level, bh = bc->height >> c->level;
        if (!sw) sw = 1;
        if (!sh) sh = 1;
        if (!sd) sd = 1;
        // the BC level must hold whole blocks (levels below 4x4 are partial blocks: not copied, logged once)
        if (bw < 4 || bh < 4 || bw != sw * 4 || bh != sh * 4 || c->z >= sd) {
            static int nlog;
            if (nlog++ < 10) LOG("[vk] block shadow copy skipped: level %u %ux%u (blocks %ux%u) slice %u of %u\n", c->level, bw, bh, sw, sh, c->z, sd);
            continue;
        }
        VkImageCopy rg = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, c->level, is3d ? 0 : c->z, 1},
                          .srcOffset = {0, 0, is3d ? (int32_t)c->z : 0},
                          .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, c->level, is3d ? 0 : c->z, 1},
                          .dstOffset = {0, 0, is3d ? (int32_t)c->z : 0}, .extent = {sw, sh, 1}};
        vkCmdCopyImage(cb, s->image, VK_IMAGE_LAYOUT_GENERAL, bc->image, VK_IMAGE_LAYOUT_GENERAL, 1, &rg);
        if (++stat_blk_copies <= 4 || !(stat_blk_copies & 255))
            LOG("[vk] block shadow copy #%llu: level %u slice %u, %ux%u blocks -> BC %ux%u\n", (unsigned long long)stat_blk_copies,
                c->level, c->z, sw, sh, bw, bh);
    }
    pass_nblk = 0;
    vk_full_barrier(cb);
}

// ---- occlusion queries. TotK brackets a box draw with nvnCommandBufferResetCounter(1) and ReportCounter(1, slot) (agl::cull::OcclusionQueryDrawer::draw_ from drawModelSceneOpa; drawModelSceneFinalImage's
// call is a debug box display without them) and engine::lod::OcclusionQuery::calc reads the slot's raw u64 one or two frames later from a 3-slot ring, no wait, no availability: visible = nonzero (actor
// LOD, particles, lights / flames). Until now every report said "visible" (+4096), so nothing the game culls by query was ever culled: a performance cut, whose only risk is popping. Now the scope's draws
// run inside a Vulkan occlusion query (one per pass segment: a query cannot cross a render pass), the slot gets "visible" at record time as before, and the real count replaces it once the submission
// completed (oq_poll, render thread). A late result therefore reads visible, never hidden; a scope with a skipped draw (pipeline not built yet) or more segments / queries than the ring holds stays visible.
// LARECOMP_OCCLUSION=visible: the old stub; =zero: always 0.
#define OQ_MAX 4096
#define OQ_SEG 8
static VkQueryPool oq_pool;
static int oq_mode = -1;                 // 0 zero, 1 visible (stub), 2 queries
static bool oq_busy[OQ_MAX];             // reset, in use until its result is read
static uint32_t oq_next;
static bool oq_scope, oq_unsure;         // ResetCounter(1) seen and no report yet; a draw of it was skipped / no query free
static int oq_open = -1;                 // the query running in the open pass
static uint32_t oq_q[OQ_SEG], oq_nq;     // the scope's queries
static uint64_t oq_skip0;                // stat_skipped at the reset
typedef struct { uint64_t *out; uint64_t seq; uint32_t q[OQ_SEG], n; } OqPending;
#define OQ_PEND 2048
static OqPending oq_pend[OQ_PEND];
static unsigned oq_ph, oq_pt;            // head (oldest), tail
static int oq_on(void) {
    if (oq_mode < 0) {
        const char *e = getenv("LARECOMP_OCCLUSION");
        oq_mode = e && !strcmp(e, "zero") ? 0 : e && !strcmp(e, "visible") ? 1 : 2;
        if (oq_mode == 2 && !g_vk.host_qreset) { LOG("[vk] occlusion queries: no hostQueryReset, reports stay 'visible'\n"); oq_mode = 1; }
        if (oq_mode == 2) {
            VkQueryPoolCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_OCCLUSION, .queryCount = OQ_MAX};
            if (vkCreateQueryPool(g_vk.dev, &qi, NULL, &oq_pool) != VK_SUCCESS) { oq_pool = VK_NULL_HANDLE; oq_mode = 1; }
            else vkResetQueryPool(g_vk.dev, oq_pool, 0, OQ_MAX);
        }
        LOG("[vk] occlusion queries: %s (LARECOMP_OCCLUSION)\n", oq_mode == 2 ? "real (Vulkan queries, results when the submission completed)"
                                                                 : oq_mode == 1 ? "always visible" : "always zero");
    }
    return oq_mode;
}
static void oq_end_open(VkCommandBuffer cb) {   // (before the pass ends, and at the report)
    if (oq_open < 0) return;
    vkCmdEndQuery(cb, oq_pool, (uint32_t)oq_open);
    oq_open = -1;
}
// draw_, right before its vkCmdDraw*: the scope's query for this pass segment
static void oq_draw(VkCommandBuffer cb) {
    if (!oq_scope || oq_open >= 0 || oq_unsure) return;
    if (!in_pass) { oq_unsure = true; return; }   // (only inside our own passes: vk_end_pass_at closes the query)
    if (oq_nq == OQ_SEG || oq_busy[oq_next]) { oq_unsure = true; return; }
    uint32_t q = oq_next;
    oq_next = (oq_next + 1) % OQ_MAX;
    oq_busy[q] = true;
    oq_q[oq_nq++] = q;
    vkCmdBeginQuery(cb, oq_pool, q, 0);   // (not PRECISE: TotK only tests nonzero)
    oq_open = (int)q;
}
static void oq_free(const uint32_t *q, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) { vkResetQueryPool(g_vk.dev, oq_pool, q[i], 1); oq_busy[q[i]] = false; }
}
// Render thread, at each report and each list: completed submissions' results into their slots
static void oq_poll(void) {
    if (oq_ph == oq_pt) return;
    const uint64_t done = vk_completed_seq();
    while (oq_ph != oq_pt) {
        OqPending *p = &oq_pend[oq_ph % OQ_PEND];
        if (p->seq > done) break;
        uint64_t sum = 0, v[OQ_SEG];
        bool ok = true;
        for (uint32_t i = 0; i < p->n && ok; i++) {
            ok = vkGetQueryPoolResults(g_vk.dev, oq_pool, p->q[i], 1, sizeof v[i], &v[i], sizeof v[i], VK_QUERY_RESULT_64_BIT) == VK_SUCCESS;
            sum += ok ? v[i] : 0;
        }
        if (ok && p->out) { p->out[0] = sum; stat_oq_hidden += sum == 0; }   // (not ready although completed: keep "visible")
        oq_free(p->q, p->n);
        oq_ph++;
    }
}
static void oq_reset_counter(uint32_t type) {
    if (type != 1 || oq_on() != 2) return;
    oq_poll();
    if (oq_scope && oq_open >= 0 && in_pass) oq_end_open(vk_cmd());   // (a reset without a report: the earlier scope is dropped)
    oq_open = -1;
    if (oq_scope && oq_nq && (oq_pt - oq_ph) < OQ_PEND) {   // (its queries are in the open submission: freed once it completed)
        OqPending *p = &oq_pend[oq_pt++ % OQ_PEND];
        p->out = NULL; p->seq = vk_pending_seq(); p->n = oq_nq; memcpy(p->q, oq_q, sizeof oq_q);
    }
    oq_scope = true; oq_unsure = false; oq_nq = 0;
    oq_skip0 = stat_skipped;
}
// ReportCounter(1, out) in mode 2: the value written now is the "visible" one; the query's count replaces it later
static void oq_report(uint64_t *out) {
    if (!oq_scope) return;
    stat_oq_reports++;
    // the game reuses its 3-slot ring every 3 frames: an older report of this slot still pending (a deep queue, a hitch)
    // must not write its count over this newer one (a stale 0 would cull something visible now); its queries are still
    // freed by oq_poll
    if (out)
        for (unsigned i = oq_ph; i != oq_pt; i++) if (oq_pend[i % OQ_PEND].out == out) oq_pend[i % OQ_PEND].out = NULL;
    // (pending ring full: wait for its oldest submission rather than leak this scope's begun queries)
    if (oq_nq && (oq_pt - oq_ph) >= OQ_PEND && oq_pend[oq_ph % OQ_PEND].seq < vk_pending_seq()) { vk_wait_seq(oq_pend[oq_ph % OQ_PEND].seq); oq_poll(); }
    if (oq_open >= 0) {
        if (in_pass) oq_end_open(vk_cmd());
        else oq_open = -1;   // (cannot happen: the pass end closes it)
    }
    if (stat_skipped != oq_skip0) oq_unsure = true;
    oq_scope = false;
    if (oq_unsure || !oq_nq || !out || (oq_pt - oq_ph) >= OQ_PEND) {
        stat_oq_visible_fallback += oq_unsure || (oq_pt - oq_ph) >= OQ_PEND;
        // a scope whose queries began must still have them read back (they are submitted with the commands): park them
        // with no slot so oq_poll frees them after the submission
        if (oq_nq && (oq_pt - oq_ph) < OQ_PEND) {
            OqPending *p = &oq_pend[oq_pt++ % OQ_PEND];
            p->out = NULL; p->seq = vk_pending_seq(); p->n = oq_nq; memcpy(p->q, oq_q, sizeof oq_q);
        }
        if (!oq_unsure && !oq_nq && out) out[0] = 0;   // (no draw in the scope at all: nothing passed, as on the Switch)
        oq_nq = 0;
        return;
    }
    OqPending *p = &oq_pend[oq_pt++ % OQ_PEND];
    p->out = out; p->seq = vk_pending_seq(); p->n = oq_nq; memcpy(p->q, oq_q, sizeof oq_q);
    oq_nq = 0;
}
static void oq_device_reset(void) {   // (vk_exec_reset: the device and its pool are gone)
    oq_pool = VK_NULL_HANDLE; oq_mode = -1; memset(oq_busy, 0, sizeof oq_busy); oq_next = 0; oq_scope = oq_unsure = false;
    oq_open = -1; oq_nq = 0; oq_ph = oq_pt = 0;
}

// ---- GPU time for the game's dynamic-resolution clock: gsys::SystemTask::layerStartEndPerf_ brackets a layer with
// ReportCounter type 16 then type 0 at its start and type 0 at its end (one single-buffered slot each, rewritten every
// frame), and GPUStressChecker's stress = end - start of the two type-0 slots; ResolutionMgr::calcDynamicResolution lowers
// the scene when that looks slow. Our reports carried the host time at which the render thread translates them = our CPU
// time. While the player wants dynamic resolution (the game's own, or a floor below 100 %: at 100 % the clock holds
// instead, see CMD_REPORT_COUNTER) those three reports write nothing when recorded: the two type 0 record Vulkan
// timestamps, and once the layer's submission completed all three slots are written together, start = the host time of its
// record h0, end = h0 + (GPU end - GPU start) x the clock factor. The game so always reads one completed layer's consistent
// pair. Anything unpaired or beyond the rings gets the host values at once, as before. LARECOMP_DRS_GPU_TS=0: host time
// only (old).
#define DTS_MAX 1024
static VkQueryPool dts_pool;
static int dts_mode = -1;
static bool dts_busy[DTS_MAX];
static uint32_t dts_next;
typedef struct { uint64_t *o16, *o0, *o1, seq, h16, h0, h1; double k; uint32_t q0, q1; uint64_t seq0; } DTsPending;   // (seq0: the start's submission)
#define DTS_PEND 1024
static DTsPending dts_pend[DTS_PEND];
static unsigned dts_ph, dts_pt;
static uint64_t *dts_o16, *dts_o0, dts_h16, dts_h0;   // the open layer's type-16 / start slots and their host values
static bool dts_have16, dts_have_start;
static uint32_t dts_q0;
static uint64_t dts_seq0;
// GPU busy time per submission: each main command buffer gets a timestamp at its start and its end, and a layer's GPU time
// is the sum of its submissions' busy spans clipped to [start, end]. A span not known (out of the 64-entry window): the
// plain interval.
#define DSUB 64
static VkQueryPool dsub_pool;
static uint64_t dsub_rec[DSUB], dsub_seq[DSUB], dsub_b[DSUB], dsub_e[DSUB], dsub_done;
static bool dsub_live;   // (the last type 0 / 16 report took the live clock: no spans are recorded while the hold is on)
static void dsub_begin(VkCommandBuffer cb) {
    if (dts_mode != 1 || !dsub_pool || !dsub_live) return;
    const uint64_t q = vk_pending_seq();
    if (dsub_rec[q % DSUB]) return;   // (its slot not harvested yet: this submission's layers fall back to the plain interval)
    dsub_rec[q % DSUB] = q;
    vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_NONE, dsub_pool, (uint32_t)(q % DSUB) * 2);
}
static void dsub_end(VkCommandBuffer cb) {
    const uint64_t q = vk_pending_seq();
    if (dts_mode != 1 || !dsub_pool || dsub_rec[q % DSUB] != q) return;
    vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, dsub_pool, (uint32_t)(q % DSUB) * 2 + 1);
}
static void dsub_harvest(uint64_t done) {   // completed submissions' spans into dsub_b / dsub_e, their queries reset
    if (!dsub_pool || done == dsub_done) return;
    for (uint32_t i = 0; i < DSUB; i++) {
        const uint64_t q = dsub_rec[i];
        if (!q || q > done) continue;
        uint64_t t[2];
        if (vkGetQueryPoolResults(g_vk.dev, dsub_pool, i * 2, 2, sizeof t, t, 8, VK_QUERY_RESULT_64_BIT) == VK_SUCCESS && t[1] >= t[0]) {
            dsub_b[i] = t[0]; dsub_e[i] = t[1]; dsub_seq[i] = q;
        }
        vkResetQueryPool(g_vk.dev, dsub_pool, i * 2, 2);
        dsub_rec[i] = 0;
    }
    dsub_done = done;
}
static uint64_t dsub_busy(uint64_t s0, uint64_t s1, uint64_t t0, uint64_t t1) {   // GPU ticks busy within [t0, t1]
    if (s1 < s0 || s1 - s0 >= DSUB) return t1 - t0;
    uint64_t busy = 0, last = t0;   // (spans in submission order may overlap on the queue: each starts after the last one's end)
    for (uint64_t q = s0; q <= s1; q++) {
        const uint32_t i = (uint32_t)(q % DSUB);
        if (dsub_seq[i] != q) return t1 - t0;
        uint64_t lo = dsub_b[i] > t0 ? dsub_b[i] : t0;
        const uint64_t hi = dsub_e[i] < t1 ? dsub_e[i] : t1;
        if (lo < last) lo = last;
        if (hi > lo) { busy += hi - lo; last = hi; }
    }
    return busy < t1 - t0 ? busy : t1 - t0;
}
static int dts_on(void) {
    if (dts_mode < 0) {
        const char *e = getenv("LARECOMP_DRS_GPU_TS");
        dts_mode = !(e && *e == '0') && g_vk.host_qreset && g_vk.props.limits.timestampComputeAndGraphics;
        if (dts_mode) {
            VkQueryPoolCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = DTS_MAX};
            if (vkCreateQueryPool(g_vk.dev, &qi, NULL, &dts_pool) != VK_SUCCESS) { dts_pool = VK_NULL_HANDLE; dts_mode = 0; }
            else vkResetQueryPool(g_vk.dev, dts_pool, 0, DTS_MAX);
            VkQueryPoolCreateInfo si = qi;
            si.queryCount = DSUB * 2;
            if (dts_mode && vkCreateQueryPool(g_vk.dev, &si, NULL, &dsub_pool) == VK_SUCCESS) vkResetQueryPool(g_vk.dev, dsub_pool, 0, DSUB * 2);
            else dsub_pool = VK_NULL_HANDLE;
        }
        LOG("[vk] dynamic resolution's GPU clock: %s (LARECOMP_DRS_GPU_TS)\n", dts_mode ? "GPU intervals" : "host time at translation");
    }
    return dts_mode;
}
static void dts_free(uint32_t q) { vkResetQueryPool(g_vk.dev, dts_pool, q, 1); dts_busy[q] = false; }
static void dts_poll(void) {
    const uint64_t done = vk_completed_seq();
    dsub_harvest(done);  // (always: the spans' queries are reset here, layers pending or not)
    if (dts_ph == dts_pt) return;
    const double per = g_vk.props.limits.timestampPeriod;
    while (dts_ph != dts_pt) {
        DTsPending *p = &dts_pend[dts_ph % DTS_PEND];
        if (p->seq > done) break;
        uint64_t t0 = 0, t1 = 0;
        const bool ok = vkGetQueryPoolResults(g_vk.dev, dts_pool, p->q0, 1, sizeof t0, &t0, sizeof t0, VK_QUERY_RESULT_64_BIT) == VK_SUCCESS &&
                        vkGetQueryPoolResults(g_vk.dev, dts_pool, p->q1, 1, sizeof t1, &t1, sizeof t1, VK_QUERY_RESULT_64_BIT) == VK_SUCCESS && t1 >= t0;
        if (p->o16) p->o16[1] = p->h16;
        if (p->o0) p->o0[1] = p->h0;
        if (p->o1) p->o1[1] = ok ? p->h0 + (uint64_t)((double)dsub_busy(p->seq0, p->seq, t0, t1) * per * p->k) : p->h1;   // (no result: the host end)
        dts_free(p->q0); dts_free(p->q1);
        dts_ph++;
    }
}
static bool dts_take(uint32_t *q) {   // a free timestamp query (and room for its pending entry), written here
    if (dts_busy[dts_next] || dts_pt - dts_ph >= DTS_PEND - 1) return false;
    *q = dts_next;
    dts_next = (dts_next + 1) % DTS_MAX;
    dts_busy[*q] = true;
    vkCmdWriteTimestamp2(vk_cmd(), VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, dts_pool, *q);
    return true;
}
static void dts_flush_open(void) {   // an unfinished layer: its slots get their host values now, its query is released later
    if (dts_have16 && dts_o16) dts_o16[1] = dts_h16;
    if (dts_have_start) {
        if (dts_o16) dts_o16[1] = dts_h16;
        if (dts_o0) dts_o0[1] = dts_h0;
        // (the query is in the open submission: released by the poll once it completed, with no slot to write)
        dts_pend[dts_pt++ % DTS_PEND] = (DTsPending){NULL, NULL, NULL, vk_pending_seq(), 0, 0, 0, 1, dts_q0, dts_q0, };
    }
    dts_have16 = dts_have_start = false;
}
// CMD_REPORT_COUNTER with the live clock, before the record-time write: true = this report's slot is written here or
// later by the poll (the caller then leaves out[1] alone); `host` is the value the caller would write
static bool dts_claim(uint32_t type, uint64_t *out, uint64_t host, double k) {
    if (dts_on() != 1 || !out) return false;
    dts_poll();
    if (type == 16) {   // a layer begins
        dts_flush_open();
        dts_o16 = out; dts_h16 = host; dts_have16 = true;
        return true;
    }
    if (type != 0) return false;
    // (paired by address, not order: a layer's record holds type 16 at +0x10, its start at +0x28, its end at +0x40, so
    // interleaved layers cannot pair one's start with another's end)
    if (dts_have16 && out == dts_o16 + 3) {   // the layer's start
        dts_have16 = false;
        if (!dts_take(&dts_q0)) { dts_o16[1] = dts_h16; return false; }
        dts_o0 = out; dts_h0 = host; dts_have_start = true; dts_seq0 = vk_pending_seq();
        return true;
    }
    if (!dts_have_start || out != dts_o0 + 3) return false;   // (a type 0 outside a layer: host time at once)
    uint32_t q1;
    if (!dts_take(&q1)) { dts_flush_open(); return false; }
    dts_pend[dts_pt++ % DTS_PEND] = (DTsPending){dts_o16, dts_o0, out, vk_pending_seq(), dts_h16, dts_h0, host, k, dts_q0, q1, dts_seq0};
    dts_have_start = false;
    return true;
}
static void dts_device_reset(void) {
    dts_pool = VK_NULL_HANDLE; dts_mode = -1; memset(dts_busy, 0, sizeof dts_busy); dts_next = 0; dts_ph = dts_pt = 0;
    dsub_pool = VK_NULL_HANDLE; memset(dsub_rec, 0, sizeof dsub_rec); memset(dsub_seq, 0, sizeof dsub_seq); dsub_done = 0; dsub_live = false;
    dts_have16 = dts_have_start = false;
}

static void vk_end_pass_at(int line);
// LARECOMP_PASS_IMAGE_BARRIERS=1 (A/B, 2026-10-19 NVIDIA decal hang): like Eden's Scheduler::EndRenderPass, an image
// memory barrier (GENERAL -> GENERAL, attachment writes -> all reads/writes) on every attachment image of a pass right
// after it ends, besides our global memory barriers (equivalent by the spec; drivers may treat them differently).
static VkImage pass_img[9];
static VkImageAspectFlags pass_img_aspect[9];
static int n_pass_img;
// LARECOMP_SNAP_DELTA (default on, =0 off): a snapshot retaken from the image it mirrors (the one it copied last, same
// frame) copies only the box the draws wrote since: the scissors of draw_'s draws in passes that render into it (begin_pass
// lists those images, pd_b). Any other write since (copy, clear, dispatch, CPU upload, our own passes: snap_ndgen, the
// same causes that move tex_bar_epoch), an implicit clear, a block shadow, a new image or another snapshot of it ends the
// mirror and the next copy is whole. Exact: the texels outside the box are the ones the copy already holds. It pays when
// the scissor is a sub-rect of the target (the game's dynamic resolution); with full-target scissors the box is the image.
static uint64_t snap_ndgen = 1, snap_tagc;
static const char *snap_nd_why = "?";   // (LARECOMP_SNAP_TRACE: the last cause)
static TexBackend *pd_b[9];   // (begin_pass: this pass's images that are a snapshot's mirror)
static int pd_n;
static int snap_trace_on(void) {   // (LARECOMP_SNAP_TRACE=1: a "[snaptrace]" line per snapshot copy and per mirror a pass ends)
    static int on = -1;
    if (on < 0) on = getenv("LARECOMP_SNAP_TRACE") && *getenv("LARECOMP_SNAP_TRACE") == '1';
    return on;
}
static int snap_delta_on(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("LARECOMP_SNAP_DELTA"); on = !e || strcmp(e, "0"); }
    return on;
}
// LARECOMP_SNAP_MARK=1 (with LARECOMP_SNAP_DELTA; off by default for now): the fragment shaders that read a target they write (the
// self-write draws that need snapshots: G-buffer decals) get a variant marking the 32x32 tile of every fragment they write
// (ShaderKey.snap_mark, SMark buffer: one uint per tile, cleared once a frame). A mirror whose every write since its copy was such
// a draw takes a compute copy of the marked tiles instead of the scissor box: the decals cover a few % of the target, so the
// retaken snapshots move a few % of their bytes. The marks of the whole frame are shared (every marked draw, every target): more
// tiles than needed, never fewer. A variant not built yet, or any other draw into the image, falls back to the box.
static int smark_on(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("LARECOMP_SNAP_MARK"); on = snap_delta_on() && e && *e == '1'; if (on) LOG("[vk] feedback snapshots: marked tiles only (LARECOMP_SNAP_MARK)\n"); }
    return on;
}
static uint64_t smark_fs[256];   // (fragment code addresses that wrote a mirrored image: the self-write draws and their neighbours)
static int smark_nfs;
static bool smark_fs_has(uint64_t code) {
    for (int i = 0; i < smark_nfs; i++) if (smark_fs[i] == code) return true;
    return false;
}
#define SMARK_ROWS 256
#define SMARK_BYTES (256u * SMARK_ROWS * 4u)
static VkBuffer smark_buf;
static VmaAllocation smark_mem;
static uint64_t smark_frame = ~0ull;
static VkDescriptorBufferInfo smark_info(void) {
    if (!smark_buf) {
        VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = SMARK_BYTES,
                                 .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        VK_CHECK(vmaCreateBuffer(g_vk.vma, &bi, &ai, &smark_buf, &smark_mem, NULL));
    }
    return (VkDescriptorBufferInfo){smark_buf, 0, SMARK_BYTES};
}
// (begin_pass, outside the pass: the frame's first pass clears the marks, between full barriers)
static void smark_frame_start(VkCommandBuffer cb) {
    if (!smark_on() || smark_frame == vk_frame_counter()) return;
    smark_frame = vk_frame_counter();
    smark_info();
    vk_full_barrier(cb);
    vkCmdFillBuffer(cb, smark_buf, 0, SMARK_BYTES, 0);
    vk_full_barrier(cb);
}
static VkPipeline smark_pipe[3];   // (texel 4, 8, 16 bytes)
static VkPipelineLayout smark_pl;
static VkDescriptorSetLayout smark_dsl;
static VkPipeline smark_pipeline(int k) {
    if (smark_pipe[k]) return smark_pipe[k];
    if (!smark_dsl) {
        VkDescriptorSetLayoutBinding b[3] = {{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
                                             {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
                                             {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}};
        VkDescriptorSetLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 3, .pBindings = b};
        VK_CHECK(vkCreateDescriptorSetLayout(g_vk.dev, &li, NULL, &smark_dsl));
        VkPushConstantRange pc = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 8};
        VkPipelineLayoutCreateInfo pl = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &smark_dsl,
                                         .pushConstantRangeCount = 1, .pPushConstantRanges = &pc};
        VK_CHECK(vkCreatePipelineLayout(g_vk.dev, &pl, NULL, &smark_pl));
    }
    static const char *fmt[3] = {"r32ui", "rg32ui", "rgba32ui"};
    char src[1024];
    snprintf(src, sizeof src,
             "#version 450\n"
             "layout(local_size_x = 8, local_size_y = 8) in;\n"
             "layout(set = 0, binding = 0) uniform usampler2D src_;\n"
             "layout(set = 0, binding = 1, %s) uniform writeonly uimage2D dst_;\n"
             "layout(std430, set = 0, binding = 2) readonly buffer M { uint m[]; } mk_;\n"
             "layout(push_constant) uniform P { uint w, h; } pc_;\n"
             "void main() {\n"
             "  uvec2 t = gl_WorkGroupID.xy;\n"
             "  if (mk_.m[min(t.y, 255u) * 256u + min(t.x, 255u)] == 0u) return;\n"
             "  for (uint dy = 0u; dy < 4u; dy++)\n"
             "    for (uint dx = 0u; dx < 4u; dx++) {\n"
             "      uvec2 p = t * 32u + gl_LocalInvocationID.xy * 4u + uvec2(dx, dy);\n"
             "      if (p.x < pc_.w && p.y < pc_.h) imageStore(dst_, ivec2(p), texelFetch(src_, ivec2(p), 0));\n"
             "    }\n"
             "}\n", fmt[k]);
    size_t nw;
    uint32_t *spv = vk_glsl_to_spirv(src, STAGE_COMPUTE, "snap_tiles", &nw);
    if (!spv) return VK_NULL_HANDLE;
    VkShaderModule m = vk_module(spv, nw);
    free(spv);
    VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                                                .module = m, .pName = "main"},
                                      .layout = smark_pl};
    if (vkCreateComputePipelines(g_vk.dev, VK_NULL_HANDLE, 1, &ci, NULL, &smark_pipe[k]) != VK_SUCCESS) smark_pipe[k] = VK_NULL_HANDLE;
    vkDestroyShaderModule(g_vk.dev, m, NULL);
    return smark_pipe[k];
}
static int smark_kind(VkFormat f) {   // (the uint view a texel of f copies through; -1: not by tiles)
    extern uint32_t vk_host_texel_bytes(VkFormat f);
    uint32_t n = vk_host_texel_bytes(f);
    return n == 4 ? 0 : n == 8 ? 1 : n == 16 ? 2 : -1;
}
// the marked tiles of src into dst (both GENERAL; after the caller's full barrier); false: not possible, copy otherwise
static bool smark_copy(VkCommandBuffer cb, TexBackend *src, TexBackend *dst) {
    static const VkFormat uf[3] = {VK_FORMAT_R32_UINT, VK_FORMAT_R32G32_UINT, VK_FORMAT_R32G32B32A32_UINT};
    int k = smark_kind(src->format);
    if (k < 0 || !(dst->usage & VK_IMAGE_USAGE_STORAGE_BIT)) return false;
    VkPipeline pipe = smark_pipeline(k);
    if (!pipe) return false;
    static const VkComponentMapping id = {0};
    VkImageView sv = vk_image_view(src, VK_IMAGE_VIEW_TYPE_2D, uf[k], 0, 1, 0, 1, id, VK_IMAGE_ASPECT_COLOR_BIT);
    VkImageView dv = vk_image_view(dst, VK_IMAGE_VIEW_TYPE_2D, uf[k], 0, 1, 0, 1, id, VK_IMAGE_ASPECT_COLOR_BIT);
    if (!sv || !dv) return false;
    VkDescriptorSet ds = vk_alloc_set(smark_dsl);
    VkDescriptorImageInfo si = {vk_default_sampler(false), sv, VK_IMAGE_LAYOUT_GENERAL}, di = {VK_NULL_HANDLE, dv, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorBufferInfo bi = smark_info();
    VkWriteDescriptorSet w[3] = {
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 0, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &si},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 1, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &di},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 2, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &bi}};
    vkUpdateDescriptorSets(g_vk.dev, 3, w, 0, NULL);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, smark_pl, 0, 1, &ds, 0, NULL);
    uint32_t pc[2] = {src->width, src->height};
    vkCmdPushConstants(cb, smark_pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, 8, pc);
    vkCmdDispatch(cb, (src->width + 31) / 32, (src->height + 31) / 32, 1);
    return true;
}
static void snap_box_add(TexBackend *b, VkRect2D r) {
    if (!r.extent.width || !r.extent.height) return;
    int32_t x0 = r.offset.x, y0 = r.offset.y, x1 = x0 + (int32_t)r.extent.width, y1 = y0 + (int32_t)r.extent.height;
    int32_t *q = b->snap_box;
    if (q[2] <= q[0] || q[3] <= q[1]) { q[0] = x0; q[1] = y0; q[2] = x1; q[3] = y1; return; }
    if (x0 < q[0]) q[0] = x0;
    if (y0 < q[1]) q[1] = y0;
    if (x1 > q[2]) q[2] = x1;
    if (y1 > q[3]) q[3] = y1;
}
// g_vk.fbl_layout (the feedback-loop layout extension alone, AMD): images moved to ATTACHMENT_FEEDBACK_LOOP_OPTIMAL for
// the open pass because a draw samples them while they are bound; back to GENERAL when the pass ends (vk_end_pass_at), so
// everything else (copies, clears, other passes' descriptors) keeps seeing GENERAL. pass_fbl: their aspects in this pass.
static TexBackend *fbl_imgs[9];
static int n_fbl;
static bool fbl_hold;   // (resolve_feedback restarting the pass for its own reads: keep the moved images where they are)
static VkImageAspectFlags pass_fbl;
static TexBackend *feedback_fbl[4];   // (feedback_view[k] is a live read of that image in FEEDBACK_LOOP_OPTIMAL: texture_info_'s descriptor layout)
static uint64_t g_set_gen;
static void bind_gen_all(void);
static void fbl_move(VkCommandBuffer cb, TexBackend *b, bool in) {
    VkImageMemoryBarrier2 ib = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
        .oldLayout = in ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT,
        .newLayout = in ? VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT : VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = b->image,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS}};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &ib};
    vkCmdPipelineBarrier2(cb, &di);
    b->fbl_in = in;
    // (descriptors name the layout: texture_info's per-frame cache, the view memo and stage_set's memos built in the other
    // one are stale; also for draws that reach the image through another object, which resolve_feedback does not see)
    extern uint32_t vk_view_gen;
    vk_view_gen++; g_set_gen++; bind_gen_all();
}
static bool depth_live_in_pass;   // a draw of the open pass sampled the bound depth attachment live (resolve_feedback)
// Attachments of game textures are declared as possible feedback loops (device.c g_vk.feedback_loop; textures.c gives them the
// usage bit): TotK's decals sample the targets they render to while bound.
static const VkAttachmentFeedbackLoopInfoEXT fb_loop_info = {.sType = VK_STRUCTURE_TYPE_ATTACHMENT_FEEDBACK_LOOP_INFO_EXT, .feedbackLoopEnable = VK_TRUE};
static void debug_save_depth(TexBackend *b, const char *path);
static uint32_t dump_layer;   // pass dumps: the array layer the target view renders to (read by raw_view)
void vk_end_pass(void) { vk_end_pass_at(0); }
#define vk_end_pass() vk_end_pass_at(__LINE__)
static void vk_end_pass_at(int line) {
    if (line >= 0) { pass_barrier_needed = true; pbn_cause = line < 8192 ? line : 0; }   // (our own work outside a pass follows: the next pass waits for it)
    if (!in_pass) return;
    if (prof && line < 4096) prof_end_line[line]++;
    oq_end_open(vk_cmd());   // (an occlusion query cannot outlive its render pass: the scope's next draw begins another)
    vkCmdEndRendering(vk_cmd());
    in_pass = false;
    pass_fbl = 0;
    pd_n = 0;
    if (n_fbl && !fbl_hold) {   // (g_vk.fbl_layout: the pass's feedback-loop images back to GENERAL)
        for (int i = 0; i < n_fbl; i++) if (fbl_imgs[i]->fbl_in) fbl_move(vk_cmd(), fbl_imgs[i], false);
        n_fbl = 0;
    }
    depth_live_in_pass = false;
    static int imgbar = -1;
    if (imgbar < 0) { const char *e = getenv("LARECOMP_PASS_IMAGE_BARRIERS"); imgbar = e && *e == '1'; }
    if (imgbar && n_pass_img) {
        VkImageMemoryBarrier2 ib[9];
        for (int i = 0; i < n_pass_img; i++)
            ib[i] = (VkImageMemoryBarrier2){.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                .srcStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT |
                                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                .srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                .dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT |
                                 VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                 VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT,
                .oldLayout = VK_IMAGE_LAYOUT_GENERAL, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = pass_img[i], .subresourceRange = {pass_img_aspect[i], 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS}};
        VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = (uint32_t)n_pass_img, .pImageMemoryBarriers = ib};
        vkCmdPipelineBarrier2(vk_cmd(), &di);
    }
    n_pass_img = 0;
    blk_copy_back(vk_cmd());
    static int nopng = -1;   // LARECOMP_DEBUG_NOPNG=1: debug frames log only (no pass dumps; long LARECOMP_DEBUG_FRAMES runs)
    if (nopng < 0) nopng = getenv("LARECOMP_DEBUG_NOPNG") != NULL;
    if (g_debug_frame && !nopng && st.rt.ncolor && st.rt.color[0] && st.rt.color[0]->backend) {
        char path[256];
        static uint64_t debug_pass_frame = ~0ull;
        static int multi = -1;
        if (multi < 0) multi = getenv("LARECOMP_DEBUG_FRAMES") != NULL;
        if (multi && renderer_present_count() != debug_pass_frame) { debug_pass_frame = renderer_present_count(); debug_pass_no = 0; }
        if (multi) snprintf(path, sizeof path, "build/pass_%llu_%03d.png", (unsigned long long)debug_pass_frame, debug_pass_no++);
        else snprintf(path, sizeof path, "build/pass_%03d.png", debug_pass_no++);
        RtState rts = st.rt;   // (saving ends the pass and may touch st)
        // each target's own array layer (an 800x450x2 cave-ambient array is written as layer 0 + layer 1 of one image;
        // dumping layer 0 for both showed two identical images)
#define RT_LAYER(i) (rts.has_cview[i] && (rts.cview[i].setmask & 2) ? rts.cview[i].min_layer : 0u)
        LOG("[vk] pass dump %s: rt0 %p addr %llx layer %u\n", path, (void *)st.rt.color[0], (unsigned long long)st.rt.color[0]->addr, RT_LAYER(0));
        dump_layer = RT_LAYER(0);
        renderer_save_texture_tex(rts.color[0]->backend, path);
        static int allrt = -1;
        if (allrt < 0) allrt = getenv("LARECOMP_DEBUG_ALLRT") != NULL;   // also color targets 1..7: pass_NNN_c<i>.png
        for (int i = 1; allrt && i < rts.ncolor; i++)
            if (rts.color[i] && rts.color[i]->backend) {
                char p2[300];
                snprintf(p2, sizeof p2, "%.*s_c%d.png", (int)(strlen(path) - 4), path, i);
                dump_layer = RT_LAYER(i);
                if (dump_layer) LOG("[vk] pass dump %s: rt%d layer %u\n", p2, i, dump_layer);
                renderer_save_texture_tex(rts.color[i]->backend, p2);
            }
        dump_layer = 0;
#undef RT_LAYER
    }
    // LARECOMP_DEBUG_DEPTH=1 (debug frame): depth-only passes (shadow maps) too -> build/pass_<frame>_NNN_depth.pgm
    static int ddepth = -1;
    if (ddepth < 0) ddepth = getenv("LARECOMP_DEBUG_DEPTH") != NULL;
    if (ddepth && g_debug_frame && !st.rt.ncolor && st.rt.depth && st.rt.depth->backend) {
        char path[256];
        snprintf(path, sizeof path, "build/pass_%llu_%03d_depth.pgm", (unsigned long long)renderer_present_count(), debug_pass_no++);
        LOG("[vk] depth pass dump %s: depth %p addr %llx f%#x\n", path, (void *)st.rt.depth, (unsigned long long)st.rt.depth->addr,
            st.rt.depth->format);
        debug_save_depth(st.rt.depth->backend, path);
    }
}

static uint32_t level_extent(uint32_t v, uint32_t lv) { v >>= lv; return v ? v : 1; }
// The pass scale of an attachment rendered at mip level lv: its level's host extent over its guest extent, per axis. A level of a scaled image is the scaled level 0 halved (2133x1200 ->...
// -> 266x150), not the guest level times the scale (200x112 x 1.333 = 266.7x149.3), so the guest's full-level viewport mapped with the nominal scale left the level's last row unwritten (row
// 149 of 150 at 1.333; at 2.4: 269 of 270, at 2: 224 of 225). A bloom chain that overwrites a level by draws and adds into it never reset that row, 2026-10-26: full width, bottom only,
// saturated, growing over the session). Level 0 kept the nominal scale until 10-30 (lround(guest x s) is never more than half a pixel past guest x s, so its last row was covered), but
// coverage is not the only thing that has to match. A scaled attachment's pass scale is its allocation's own ratio, host extent / guest extent of the level, at level 0 too: the nominal scale
// put a 1600-wide guest target's viewport at 2133.33 (2134 once snapped, 77ad18d5) on a 2133-wide image that shaders then sample at uv * 2133, so content drifted up to 1 px toward the right
// / bottom edge, more along a post chain. LARECOMP_SCALE0_NOMINAL=1: level 0 at the nominal scale (old).
static void attachment_scale(NvnTexture *t, TexBackend *b, uint32_t lv, float *sx, float *sy) {
    renderer_texture_scale(t, sx, sy);
    static int nominal0 = -1;
    if (nominal0 < 0) { const char *e = getenv("LARECOMP_SCALE0_NOMINAL"); nominal0 = e && *e == '1'; }
    if ((!lv && nominal0) || (*sx == 1 && *sy == 1)) return;
    *sx = (float)level_extent(b->width, lv) / (float)level_extent(t->width, lv);
    *sy = (float)level_extent(b->height, lv) / (float)level_extent(t->height, lv);
}

// Pass-begin barriers (2026-10-17, Deck d24 standing frame: ~400 passes per frame, each began with a full ALL_COMMANDS
// barrier = a GPU drain; TotK's 132 independent 64x64 cube-filter passes (fs 544d1de3, 4 taps per pixel) took 16-25 us each,
// 2.1 ms per frame, mostly drain + ramp). NVN orders render-to-texture -> sampling only through the game's own
// nvnCommandBufferBarrier (Maxwell does too), so a pass now starts with the barrier only when something since the last one
// needs it: a game CMD_BARRIER, or any of our own work outside a pass (copies, dispatches, snapshots, present: everything
// that ends the pass through vk_end_pass, see vk_end_pass_at). Consecutive passes with only draws between them run without
// one (attachment write-after-write across passes stays in submission order on the GPU). LARECOMP_PASS_BARRIER=1: every pass
// (old). (pass_barrier_needed, pass_unbarriered, stat_pass_*: declared at the top with the other stats) d25 OBSERVED: TotK
// issues ~1.2 barriers per draw (331k per 2 s, 267k draws), so the game-barrier gating ("game") skipped only 2772 of 20163
// pass barriers: no gain. "tracked" ignores the game's barriers and orders by hazard, like DXVK: the guest storage ranges of
// the attachments written since the last full barrier are kept (wr_mark at pass begin); a draw that samples a texture
// overlapping one of them, or reads storage after a storage / image write in this epoch, gets a full barrier first
// (resolve_feedback); everything else (pass changes, write-after-write on an attachment) runs without one. Our own work
// outside passes still barriers before itself and before the next pass (vk_end_pass_at). LARECOMP_BARRIERS = pass (default
// for now: a full barrier before every pass, the old way) | game | tracked | none (none: only our own work's barriers, no
// feedback barriers: a measurement of the upper bound, wrong pictures expected). LARECOMP_PASS_BARRIER=1 = pass.
enum { BAR_PASS, BAR_GAME, BAR_TRACKED, BAR_NONE };
static int barrier_mode(void) {
    static int m = -1;
    if (m < 0) {
        const char *e = getenv("LARECOMP_BARRIERS"), *o = getenv("LARECOMP_PASS_BARRIER");
        m = !e ? BAR_PASS : !strcmp(e, "game") ? BAR_GAME : !strcmp(e, "tracked") ? BAR_TRACKED : !strcmp(e, "none") ? BAR_NONE : BAR_PASS;
        if (o && *o == '1') m = BAR_PASS;
        LOG("[vk] barrier mode: %s\n", m == BAR_PASS ? "pass" : m == BAR_GAME ? "game" : m == BAR_TRACKED ? "tracked" : "none");
    }
    return m;
}
// Written-in-this-epoch marks: the attachment's backend image (wr_epoch) and its guest storage address in a small table
// (textures that alias the same storage through another object / backend start at the same address). d28 OBSERVED with a
// 64-entry range list: it overflowed (hundreds of 64x64 passes per epoch) -> every sampled texture counted as written:
// 29581 hazard barriers per 2 s, 46036 passes (twice the old count).
#define WR_TAB 4096
static struct { uint64_t addr; uint32_t epoch; } wr_tab[WR_TAB];
static uint32_t gmem_wr_epoch;                       // a draw wrote storage / images in that epoch
static uint32_t gmem_rd_epoch;                       // (LARECOMP_PASS_ELIDE=2) a draw read storage / images in that epoch
static void wr_mark(NvnTexture *t) {
    if (!t) return;
    TexBackend *b = t->backend;
    if (b) b->wr_epoch = bar_epoch;
    TexBackend *rb = vk_render_backend(t);
    if (rb && rb != b) rb->wr_epoch = bar_epoch;
    if (!t->addr) return;
    uint32_t h = (uint32_t)((t->addr >> 8) * 0x9E3779B1u) & (WR_TAB - 1);
    for (int k = 0; k < 8; k++) {   // (linear probe; a slot from an older epoch is free)
        uint32_t i = (h + (uint32_t)k) & (WR_TAB - 1);
        if (wr_tab[i].addr == t->addr || wr_tab[i].epoch != bar_epoch) { wr_tab[i].addr = t->addr; wr_tab[i].epoch = bar_epoch; return; }
    }
    wr_tab[h].addr = t->addr;   // (table full around h: overwrite; a lost mark could miss a hazard, 8 probes make it rare)
    wr_tab[h].epoch = bar_epoch;
}
static bool wr_hazard(NvnTexture *t) {
    if (!t) return false;
    TexBackend *b = t->backend;
    if (b && b->wr_epoch == bar_epoch) return true;
    if (!t->addr) return false;
    uint32_t h = (uint32_t)((t->addr >> 8) * 0x9E3779B1u) & (WR_TAB - 1);
    for (int k = 0; k < 8; k++) {
        uint32_t i = (h + (uint32_t)k) & (WR_TAB - 1);
        if (wr_tab[i].addr == t->addr && wr_tab[i].epoch == bar_epoch) return true;
    }
    return false;
}

// LARECOMP_PASS_ELIDE. 1: a pass begun right after a full barrier with nothing recorded since (a feedback or hazard split)
// skips BAR_PASS's second one. 2: also small passes (<= 128x128 at their level) begin without one when nothing they touch is
// a hazard: no storage / image written since the last full barrier, and none of their attachments' (image, level, layer)s
// written, nor their images sampled, since it (write-after-write and write-after-read across passes are not left to
// submission order). Their draws then take tracked mode's check: a sampled texture written since the last full barrier, or
// storage read after a storage write, ends the pass behind a full barrier (before the pass begins when it is the first draw:
// resolve_feedback runs before begin_pass). gc4k at 4K Kakariko: ~80 passes <= 128x128 a frame (64x64 x72 = the probe
// filters of fs 544d1de3), 8-9 us each against ~1-2 us of draws: 0.64-0.70 ms a frame of pass begin / end and full barriers.
static int pass_elide(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("LARECOMP_PASS_ELIDE");
        on = !e || !*e ? 2 : (*e == '1' || *e == '2') ? *e - '0' : 0;
        if (on && getenv("LARECOMP_NO_FEEDBACK")) on = 1;   // (no hazard checks then: resolve_feedback returns first)
        if (on) LOG("[vk] pass elision: %s\n", on == 2 ? "after a full barrier + small passes" : "after a full barrier");
    }
    return on;
}
static void att_level_layer(int i, uint32_t *lv, uint32_t *z) {   // (i == st.rt.ncolor: depth)
    const NvnTextureView *v = i < st.rt.ncolor ? &st.rt.cview[i] : &st.rt.dview;
    bool hv = i < st.rt.ncolor ? st.rt.has_cview[i] : st.rt.has_dview;
    *lv = hv && (v->setmask & 1) ? v->base_level : 0;
    *z = hv && (v->setmask & 2) ? v->min_layer : 0;
}
static bool pass_small(void) {   // the pass st.rt sets up: its first attachment's guest extent at its level
    for (int i = 0; i <= st.rt.ncolor; i++) {
        NvnTexture *t = i < st.rt.ncolor ? st.rt.color[i] : st.rt.depth;
        if (!t) continue;
        uint32_t lv, z;
        att_level_layer(i, &lv, &z);
        return level_extent(t->width, lv) <= 128 && level_extent(t->height, lv) <= 128;
    }
    return false;
}
// attachment (image, level, layer)s written since the last full barrier (overflow: every attachment counts as written)
#define EW_TAB 64
static struct { TexBackend *b; uint32_t lv, z; } ew_tab[EW_TAB];
static uint32_t ew_n, ew_epoch;
static void ew_add(TexBackend *b, uint32_t lv, uint32_t z) {
    if (ew_epoch != bar_epoch) { ew_epoch = bar_epoch; ew_n = 0; }
    for (uint32_t k = 0; k < ew_n && k < EW_TAB; k++) if (ew_tab[k].b == b && ew_tab[k].lv == lv && ew_tab[k].z == z) return;
    if (ew_n < EW_TAB) ew_tab[ew_n] = (__typeof__(ew_tab[0])){b, lv, z};
    if (ew_n <= EW_TAB) ew_n++;
}
static bool ew_has(TexBackend *b, uint32_t lv, uint32_t z) {
    if (ew_epoch != bar_epoch) return false;
    if (ew_n > EW_TAB) return true;
    for (uint32_t k = 0; k < ew_n; k++) if (ew_tab[k].b == b && ew_tab[k].lv == lv && ew_tab[k].z == z) return true;
    return false;
}
static bool small_pass_elidable(void) {
    if (gmem_wr_epoch == bar_epoch || !pass_small()) return false;
    for (int i = 0; i <= st.rt.ncolor; i++) {
        NvnTexture *t = i < st.rt.ncolor ? st.rt.color[i] : st.rt.depth;
        if (!t || !t->backend) continue;   // (no image yet: nothing wrote or sampled it)
        TexBackend *b = t->backend, *rb = vk_render_backend(t);
        uint32_t lv, z;
        att_level_layer(i, &lv, &z);
        if (b->rd_epoch == bar_epoch || (rb && rb->rd_epoch == bar_epoch) || ew_has(rb ? rb : b, lv, z)) return false;
    }
    return true;
}

static uint64_t g_cur_fs_hash;   // (tentative; defined with the program state below)

// The pool is reset before the frame's first pass (a reset cannot be recorded inside one).
typedef struct { uint64_t fs, cb4, cb8; uint32_t n, inst; } E1Draw;
static VkQueryPool e1_qp; static int e1_n, e1_armed, e1_on = -1; static E1Draw e1_d[8192];
int vkq_enabled(void);
// (vkCmdBeginQuery / EndQuery are not in the deferred recorder (vkq.h): with LARECOMP_VKQ on they would race the record
// thread on the same command buffer: boot gte1_occl lost the device so. The test needs LARECOMP_VKQ=0.)
static bool e1_enabled(void) {
    if (e1_on < 0) {
        const char *e = getenv("LARECOMP_E1_OCCL"); e1_on = e && strcmp(e, "0");
        if (e1_on && vkq_enabled()) { LOG("[e1] LARECOMP_E1_OCCL needs LARECOMP_VKQ=0: off\n"); e1_on = 0; }
    }
    return e1_on;
}
void renderer_flush(void);
static void e1_report(void) {
    renderer_flush();
    static uint64_t res[8192];
    if (vkGetQueryPoolResults(g_vk.dev, e1_qp, 0, (uint32_t)e1_n, sizeof res, res, 8, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) != VK_SUCCESS) {
        LOG("[e1] occlusion results unavailable\n"); e1_n = 0; e1_armed = 0; return;
    }
    int zero = 0;
    for (int i = 0; i < e1_n; i++) zero += res[i] == 0;
    LOG("[e1] occlusion: %d draws in the debug frame, %d passed no sample\n", e1_n, zero);
    for (int i = 0; i < e1_n; i++)
        if (!res[i]) LOG("[e1]   0 samples: fs %016llx count %u inst %u cb4 %llx cb8 %llx\n", (unsigned long long)e1_d[i].fs, e1_d[i].n,
                         e1_d[i].inst, (unsigned long long)e1_d[i].cb4, (unsigned long long)e1_d[i].cb8);
    if (getenv("LARECOMP_E1_SUMS")) {   // (measurement: samples per fragment shader over the debug frame)
        static struct { uint64_t fs, s, n; } agg[512];
        int na = 0;
        for (int i = 0; i < e1_n; i++) {
            int k = 0;
            while (k < na && agg[k].fs != e1_d[i].fs) k++;
            if (k == na && na < 512) agg[na++] = (typeof(agg[0])){e1_d[i].fs, 0, 0};
            if (k < na) agg[k].s += res[i], agg[k].n++;
        }
        for (int k = 0; k < na; k++) LOG("[e1sum] fs %016llx draws %llu samples %llu\n", (unsigned long long)agg[k].fs, (unsigned long long)agg[k].n, (unsigned long long)agg[k].s);
    }
    e1_n = 0; e1_armed = 0;
}
static void begin_pass(void) {
    if (in_pass && !st.rt_dirty) return;
    uint64_t bp_t0 = prof ? prof_now() : 0, bp_tv = 0;
    vk_end_pass_at(-1);   // (-1: this pass change alone does not ask for a barrier)
    if (prof) { prof_bp_end += prof_now() - bp_t0; prof_bp_n++; }
    if (dl_on == 2) renderer_flush();  // (LARECOMP_VK_DEVLOST_TRACE=2: each pass its own submission)
    else if (chunk_limit() && chunk_draws >= (uint32_t)chunk_limit()) { prof_chunks++; g_chunk_submit = true; renderer_submit(); g_chunk_submit = false; }
    st.rt_dirty = false;
    if (st.rt.ncolor >= 4 && st.rt.color[0] && st.rt.color[0]->width > g_drs_w) { g_drs_w = st.rt.color[0]->width; g_drs_h = st.rt.color[0]->height; }
    uint64_t z0 = prof ? prof_now() : 0;
    VkCommandBuffer cb = vk_cmd();
    uint64_t z1 = prof ? prof_now() : 0;
    int pe = barrier_mode() == BAR_PASS ? pass_elide() : 0;
    bool fresh_skip = pe && pass_barrier_fresh, small_skip = false;
    if (!pass_barrier_needed && pe == 2 && !fresh_skip && !pass_barrier_frag) small_skip = small_pass_elidable();
    if (pass_barrier_needed || (barrier_mode() == BAR_PASS && !pass_barrier_frag && !fresh_skip && !small_skip)) {
        stat_pbn_cause[pass_barrier_needed ? pbn_cause : 3]++;
        vk_full_barrier(cb);  // passes, copies and dispatches see each other's results
        pass_barrier_needed = false;
        pass_unbarriered = false;
        stat_pass_barriers++;
    } else {
        pass_unbarriered = !pass_barrier_fresh;
        stat_pass_nobarrier++;
        stat_pass_fresh += fresh_skip;
        stat_pass_elided += small_skip;
    }
    pass_elided = small_skip;
    pass_barrier_fresh = pass_barrier_frag = false;
    uint64_t z2 = prof ? prof_now() : 0;
    {
        NvnTexture *t0n = st.rt.ncolor ? st.rt.color[0] : NULL;
        dl_note("pass %d color %ux%u fmt %#x depth %p", st.rt.ncolor, t0n ? t0n->width : 0, t0n ? t0n->height : 0, t0n ? t0n->format : 0,
                (void *)st.rt.depth);
        hang_op(cb, HOP_PASS, 0, 0, (uint32_t)st.rt.ncolor, t0n ? t0n->width : 0, t0n ? t0n->height : 0);
    }
    if (prof) { prof_bpz[0] += z1 - z0; prof_bpz[1] += z2 - z1; prof_bpz[2] += prof_now() - z2; }
    if (g_gpuprof_frame) {
        NvnTexture *t0 = st.rt.ncolor ? st.rt.color[0] : NULL;
        gp_mark(cb, "pass: %d color %ux%u fmt %#x, depth %s", st.rt.ncolor, t0 ? t0->width : 0, t0 ? t0->height : 0,
                t0 ? t0->format : 0, st.rt.depth ? "yes" : "no");
    }
    {   // (textures.c) a promoted target that kept its unscaled backend: reallocated at the scene's scale before use
        extern bool renderer_texture_apply_promotion(NvnTexture *t);
        extern bool renderer_texture_target_kind(NvnTexture *t, bool colour);   // (a depth-format texture as a colour target)
        for (int i = 0; i <= st.rt.ncolor; i++) {
            NvnTexture *t = i < st.rt.ncolor ? st.rt.color[i] : st.rt.depth;
            if (t && t->backend) renderer_texture_apply_promotion(t);
            if (t) renderer_texture_target_kind(t, i < st.rt.ncolor);   // (also before its first image: made in the right kind)
        }
        // (textures.c renderer_texture_grow) a scaled level-0 depth of the colour level's guest size but 1-2 host rows /
        // columns smaller than that level (its mip chain rounds an odd size up: 450 / 2 = 225 against 2 x 112): grown to
        // the level, so the render area covers all of it
        extern bool renderer_texture_grow(NvnTexture *t, uint32_t hw, uint32_t hh);
        NvnTexture *dt = st.rt.depth, *ct = st.rt.ncolor ? st.rt.color[0] : NULL;
        const uint32_t dlv = st.rt.has_dview && (st.rt.dview.setmask & 1) ? st.rt.dview.base_level : 0;
        const uint32_t clv = st.rt.ncolor && st.rt.has_cview[0] && (st.rt.cview[0].setmask & 1) ? st.rt.cview[0].base_level : 0;
        if (dt && ct && dt->backend && ct->backend && !dlv && clv) {
            const TexBackend *db = dt->backend, *cb2 = ct->backend;
            const uint32_t cw = level_extent(cb2->width, clv), ch = level_extent(cb2->height, clv);
            const uint32_t gw = level_extent(ct->width, clv), gh = level_extent(ct->height, clv);
            if (db->scalable && cb2->scalable && dt->width == gw && dt->height == gh && (db->width < cw || db->height < ch) &&
                cw <= db->width + 2 && ch <= db->height + 2)
                renderer_texture_grow(dt, cw > db->width ? cw : db->width, ch > db->height ? ch : db->height);
        }
    }
    VkRenderingAttachmentInfo ca[8], da = {0}, sa = {0};
    uint32_t w = 0, minw = UINT32_MAX, minh = UINT32_MAX;  // w: first attachment seen
    pass_ncolor = (uint32_t)st.rt.ncolor;
    pass_samples = 1;
    pass_nblk = 0;
    pass_ntwin = 0;
    n_pass_img = 0;
    for (int i = 0; i < st.rt.ncolor; i++) {
        NvnTexture *t = st.rt.color[i];
        ca[i] = (VkRenderingAttachmentInfo){.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        pass_color[i] = VK_FORMAT_UNDEFINED;
        if (!t) continue;
        renderer_mark_gpu_owned(t);
        uint64_t tv0 = prof ? prof_now() : 0;
        VkImageView v = vk_view(t, &st.rt.cview[i], st.rt.has_cview[i], true, TEXTYPE_2D);
        if (prof) bp_tv += prof_now() - tv0;
        TexBackend *b = vk_render_backend(t);   // (a BC target draws into its block shadow: textures.c block_shadow)
        if (!v) continue;
        if (b != t->backend && pass_nblk < 8) {
            uint32_t lv = st.rt.has_cview[i] && (st.rt.cview[i].setmask & 1) ? st.rt.cview[i].base_level : 0;
            uint32_t z = st.rt.has_cview[i] && (st.rt.cview[i].setmask & 2) ? st.rt.cview[i].min_layer : 0;
            pass_blk[pass_nblk++] = (BlkCopy){t->backend, b, lv, z};
        }
        if (b->twin && pass_ntwin < 8) pass_twin[pass_ntwin++] = b;
        ca[i].imageView = v;
        pass_img[n_pass_img] = b->image; pass_img_aspect[n_pass_img++] = VK_IMAGE_ASPECT_COLOR_BIT;
        ca[i].imageLayout = b->fbl_in ? VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT : VK_IMAGE_LAYOUT_GENERAL;
        if (b->fbl_in) pass_fbl |= VK_IMAGE_ASPECT_COLOR_BIT;
        ca[i].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        ca[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        if (g_vk.feedback_loop && (b->usage & VK_IMAGE_USAGE_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT)) ca[i].pNext = &fb_loop_info;   // (draws may sample it)
        bool fresh = (int)renderer_texture_level_fresh(t) | (int)renderer_texture_frame_fresh(t);  // both: each marks its state
        if (renderer_texture_first_write(t) || fresh) {
            ca[i].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            ca[i].clearValue.color.float32[3] = fresh ? 0.0f : 1.0f;
            if (g_debug_frame) LOG("[pass] implicit clear rt%d %p addr %llx %ux%u (%s)\n", i, (void *)t, (unsigned long long)t->addr,
                                   t->width, t->height, fresh ? "fresh" : "first write");
        }
        pass_color[i] = st.rt.has_cview[i] && (st.rt.cview[i].setmask & 4) && vk_format(t->format) == b->format
                      ? vk_format(st.rt.cview[i].format) : b->format;
        uint32_t lv = st.rt.has_cview[i] && (st.rt.cview[i].setmask & 1) ? st.rt.cview[i].base_level : 0;
        uint32_t ew = level_extent(b->width, lv), eh = level_extent(b->height, lv);
        if (!w) { attachment_scale(t, b, lv, &pass_sx, &pass_sy); w = ew; }
        if (ew < minw) minw = ew;
        if (eh < minh) minh = eh;
        pass_samples = b->samples;
    }
    pass_depth = pass_stencil = VK_FORMAT_UNDEFINED;
    bool has_d = false, has_s = false;
    if (st.rt.depth) {
        renderer_mark_gpu_owned(st.rt.depth);
        uint64_t tv0 = prof ? prof_now() : 0;
        VkImageView v = vk_view(st.rt.depth, &st.rt.dview, st.rt.has_dview, true, TEXTYPE_2D);
        if (prof) bp_tv += prof_now() - tv0;
        TexBackend *b = st.rt.depth->backend;
        if (v && renderer_format_is_depth(st.rt.depth->format)) {
            VkRenderingAttachmentInfo a = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = v,
                                           .imageLayout = VK_IMAGE_LAYOUT_GENERAL, .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
                                           .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
            if (g_vk.feedback_loop && (b->usage & VK_IMAGE_USAGE_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT)) a.pNext = &fb_loop_info;
            if (b->zero_on_write) {   // recycled depth image (textures.c recycle_get): zeroed like a new one
                a.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
                a.clearValue.depthStencil = (VkClearDepthStencilValue){0.0f, 0};
                b->zero_on_write = false;
                if (g_debug_frame) LOG("[pass] implicit depth clear %p addr %llx %ux%u (recycled)\n", (void *)st.rt.depth,
                                       (unsigned long long)st.rt.depth->addr, st.rt.depth->width, st.rt.depth->height);
            }
            pass_img[n_pass_img] = b->image; pass_img_aspect[n_pass_img++] = b->aspect;
            if (b->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) { da = a; has_d = true; pass_depth = b->format; }
            if (b->aspect & VK_IMAGE_ASPECT_STENCIL_BIT) { sa = a; has_s = true; pass_stencil = b->format; }
            uint32_t lv = st.rt.has_dview && (st.rt.dview.setmask & 1) ? st.rt.dview.base_level : 0;
            uint32_t ew = level_extent(b->width, lv), eh = level_extent(b->height, lv);
            if (!w) { attachment_scale(st.rt.depth, b, lv, &pass_sx, &pass_sy); w = ew; }
            if (ew < minw) minw = ew;
            if (eh < minh) minh = eh;
            if (!pass_ncolor) pass_samples = b->samples;
        }
    }
    if (!w) { pass_sx = pass_sy = 1; minw = minh = 16; }
    {   // attachments at different scales: the render area is the smallest one, so the scaled viewport gets clipped
        // (logged at the 1st, 2nd, 4th, 8th ... pass of each fs hash + attachment size, so later frames show up too)
        static struct { uint64_t key, n; } mixed_seen[32];
        for (int i = 0; i <= pass_ncolor; i++) {
            NvnTexture *t = i < pass_ncolor ? st.rt.color[i] : st.rt.depth;
            float sx = 1, sy = 1;
            if (!t || !t->backend) continue;
            const NvnTextureView *v = i < pass_ncolor ? &st.rt.cview[i] : &st.rt.dview;
            bool hv = i < pass_ncolor ? st.rt.has_cview[i] : st.rt.has_dview;
            attachment_scale(t, t->backend, hv && (v->setmask & 1) ? v->base_level : 0, &sx, &sy);   // (as the pass scale: per level)
            if (sx == pass_sx && sy == pass_sy) continue;
            uint64_t key = g_cur_fs_hash ^ ((uint64_t)t->width << 40 | (uint64_t)t->height << 20 | (uint64_t)i), n = 0;
            for (int k = 0; k < 32; k++)
                if (mixed_seen[k].key == key || !mixed_seen[k].n) { mixed_seen[k].key = key; n = ++mixed_seen[k].n; break; }
            if (n && !(n & (n - 1))) {
                NvnTexture *t0 = pass_ncolor ? st.rt.color[0] : st.rt.depth;   // (the attachment that set the pass scale)
                LOG("[pass] mixed scales: attachment %s%d %ux%u fmt 0x%x tgt %u flags 0x%x at %.2f, pass at %.2f -> render area %ux%u "
                    "(rt0 %ux%u fmt 0x%x flags 0x%x addr %llx, fs %016llx, frame %llu, %d colour)\n",
                    i < pass_ncolor ? "rt" : "depth", i, t->width, t->height, t->format, t->target, t->flags, sx, pass_sx, minw, minh,
                    t0 ? t0->width : 0, t0 ? t0->height : 0, t0 ? t0->format : 0, t0 ? t0->flags : 0,
                    t0 ? (unsigned long long)t0->addr : 0ull, (unsigned long long)g_cur_fs_hash,
                    (unsigned long long)renderer_present_count(), pass_ncolor);
                LOG("[pass]   (mixed pass #%llu of this kind)\n", (unsigned long long)n);
            }
        }
        // (textures.c renderer_texture_promote) an unscaled 2D attachment of the same guest size as one at the scene's
        // scale renders at that scale from its next backend
        extern void renderer_get_scale(float *sx, float *sy);
        float rsx, rsy;
        renderer_get_scale(&rsx, &rsy);
        NvnTexture *big = NULL;
        for (int i = 0; i <= pass_ncolor && (rsx != 1 || rsy != 1); i++) {
            NvnTexture *t = i < pass_ncolor ? st.rt.color[i] : st.rt.depth;
            float sx = 1, sy = 1;
            if (!t || !t->backend) continue;
            renderer_texture_scale(t, &sx, &sy);
            if (sx == rsx && sy == rsy && t->target == 1) { big = t; break; }
        }
        for (int i = 0; big && i <= pass_ncolor; i++) {
            NvnTexture *t = i < pass_ncolor ? st.rt.color[i] : st.rt.depth;
            float sx = 1, sy = 1;
            if (!t || !t->backend || t == big) continue;
            renderer_texture_scale(t, &sx, &sy);
            if (sx == 1 && sy == 1 && t->width == big->width && t->height == big->height) renderer_texture_promote(t);
        }
        // (the shadow-map kind: a square depth at TOTK_SHADOW_SCALE drawn with an unscaled colour target of its size)
        static int promo_shadow = -1;
        if (promo_shadow < 0) { const char *e = getenv("LARECOMP_PROMO_SHADOW"); promo_shadow = !e || strcmp(e, "0"); }
        TexBackend *db = st.rt.depth ? st.rt.depth->backend : NULL;
        if (!big && promo_shadow && db && db->scalable && db->scale_kind == 2 && st.rt.depth->target == 1)
            for (int i = 0; i < pass_ncolor; i++) {
                NvnTexture *t = st.rt.color[i];
                float sx = 1, sy = 1;
                if (!t || !t->backend) continue;
                renderer_texture_scale(t, &sx, &sy);
                if (sx == 1 && sy == 1 && t->width == st.rt.depth->width && t->height == st.rt.depth->height) renderer_texture_promote_kind(t, 2);
            }
    }
    pass_w = minw; pass_h = minh;
    smark_frame_start(cb);   // (LARECOMP_SNAP_MARK)
    pd_n = 0;   // (LARECOMP_SNAP_DELTA: draw_ adds its scissor to these images' boxes)
    for (int i = 0; i <= st.rt.ncolor; i++) {
        NvnTexture *t = i < st.rt.ncolor ? st.rt.color[i] : st.rt.depth;
        if (!t || !t->backend) continue;
        TexBackend *rb = i < st.rt.ncolor ? vk_render_backend(t) : t->backend;
        if (!rb) continue;
        const bool clr = i < st.rt.ncolor ? ca[i].loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR
                                          : (has_d && da.loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR) || (has_s && sa.loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR);
        if (rb != t->backend || clr) {   // (block shadow copied back / cleared)
            if (rb->snap_tag && snap_trace_on()) LOG("[snaptrace] kill img %p in begin_pass (%s)\n", (void *)rb->image, clr ? "implicit clear" : "block shadow");
            rb->snap_tag = 0; ((TexBackend *)t->backend)->snap_tag = 0; continue;
        }
        if (!rb->snap_tag) continue;
        uint32_t lv, z;
        att_level_layer(i, &lv, &z);
        if (lv || z) continue;   // (not level 0 / layer 0: what a one-level snapshot holds is not written)
        if (pd_n < 9) pd_b[pd_n++] = rb;
        else rb->snap_tag = 0;
    }
    if (barrier_mode() == BAR_TRACKED || pe == 2) {   // (after the barrier above: these are written in the current epoch)
        for (int i = 0; i <= st.rt.ncolor; i++) {
            NvnTexture *t = i < st.rt.ncolor ? st.rt.color[i] : st.rt.depth;
            if (!t) continue;
            wr_mark(t);
            if (pe == 2 && t->backend) {
                uint32_t lv, z;
                att_level_layer(i, &lv, &z);
                TexBackend *rb = vk_render_backend(t);
                ew_add(rb ? rb : t->backend, lv, z);
            }
        }
    }
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, {minw, minh}}, .layerCount = 1,
                          .colorAttachmentCount = pass_ncolor, .pColorAttachments = ca,
                          .pDepthAttachment = has_d ? &da : NULL, .pStencilAttachment = has_s ? &sa : NULL};
    uint64_t z3 = prof ? prof_now() : 0;
    if (g_debug_frame && e1_enabled() && !e1_armed) {
        if (!e1_qp) {
            VkQueryPoolCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_OCCLUSION, .queryCount = 8192};
            if (vkCreateQueryPool(g_vk.dev, &qi, NULL, &e1_qp) != VK_SUCCESS) e1_on = 0;
        }
        if (e1_qp) { vkCmdResetQueryPool(cb, e1_qp, 0, 8192); e1_armed = 1; e1_n = 0; }
    }
    vkCmdBeginRendering(cb, &ri);
    in_pass = true;
    if (prof) { prof_bpz[3] += prof_now() - z3; prof_bp += prof_now() - bp_t0; prof_bp_view += bp_tv; }
    vp_sx = pass_sx; vp_sy = pass_sy; vp_ox = 0; vp_oy = 0;
    stat_passes++;
    __atomic_fetch_add(&g_passes_total, 1, __ATOMIC_RELAXED);
}

// lroundf without the ucrtbase call (the §10 cut (e), render-bound; with -march=x86-64-v3 floor / ceil are already roundsd,
// lroundf's half away from zero is not): truncate, then the exact remainder decides. Equal to lroundf for every finite float
// below 2^31 (all 2,650,800,126 checked against an exact float64 reference, 2026-10-26).
static inline long lroundf_inline(float v) {
    // (exact without -ffast-math, which could reassociate v - t: below 2^24 the fraction is representable, above it v is
    // whole. NaN, inf and |v| >= 2^31 are undefined here as for lroundf; pass_size's scales are finite render-target sizes)
    const float t = truncf(v), d = v - t;
    return (long)(fabsf(d) >= 0.5f ? t + copysignf(1.0f, v) : t);
}
static void pass_size(int *w, int *h) {
    *w = *h = 0;
    for (int i = 0; i < st.rt.ncolor && !*w; i++)
        if (st.rt.color[i]) { *w = (int)st.rt.color[i]->width; *h = (int)st.rt.color[i]->height; }
    if (!*w && st.rt.depth) { *w = (int)st.rt.depth->width; *h = (int)st.rt.depth->height; }
    *w = (int)lroundf_inline(*w * pass_sx);
    *h = (int)lroundf_inline(*h * pass_sy);
}

static VkRect2D scissor_rect(void) {
    int w, h;
    pass_size(&w, &h);
    if (!w) return (VkRect2D){{0, 0}, {pass_w, pass_h}};
    int x = st.scissor[0][0], y = st.scissor[0][1], sw = st.scissor[0][2], sh = st.scissor[0][3];
    if (vp_sx != 1 || vp_sy != 1 || vp_ox != 0 || vp_oy != 0) {
        double x0 = floor(x * (double)vp_sx + vp_ox), y0 = floor(y * (double)vp_sy + vp_oy);
        double x1 = ceil(((double)x + sw) * vp_sx + vp_ox), y1 = ceil(((double)y + sh) * vp_sy + vp_oy);
        if (sw >= 16384) x1 = 1e9;
        if (sh >= 16384) y1 = 1e9;
        if (x1 > 1e9) x1 = 1e9;
        if (y1 > 1e9) y1 = 1e9;
        x = (int)x0; y = (int)y0; sw = (int)(x1 - x0); sh = (int)(y1 - y0);
    }
    if (x < 0) { sw += x; x = 0; }
    if (y < 0) { sh += y; y = 0; }
    if (w > (int)pass_w) w = (int)pass_w;
    if (h > (int)pass_h) h = (int)pass_h;
    static float sc_clamp = -1;   // (LARECOMP_SCISSOR_CLAMP=<0..1> test knob: a dynamic-resolution-like sub-rect for the snapshot delta A/B)
    if (sc_clamp < 0) { const char *e = getenv("LARECOMP_SCISSOR_CLAMP"); sc_clamp = e && atof(e) > 0 && atof(e) < 1 ? (float)atof(e) : 0; }
    if (sc_clamp) { w = (int)(w * sc_clamp); h = (int)(h * sc_clamp); }
    if (x >= w || y >= h) { x = y = 0; sw = sh = 0; }
    if (x + sw > w) sw = w - x;
    if (y + sh > h) sh = h - y;
    if (sw < 0) sw = 0;
    if (sh < 0) sh = 0;
    return (VkRect2D){{x, y}, {(uint32_t)sw, (uint32_t)sh}};
}

// Viewport / scissor as last set by draw_ in this command buffer (dyn_cache_vs: the same command buffer + submission;
// dyn_invalidate clears them): a draw with the same values skips the call.
static struct { VkCommandBuffer cb; uint64_t seq; bool vp_ok, sc_ok; VkViewport vp; VkRect2D sc; } vsc;
static bool vsc_same_cb(VkCommandBuffer cb) {
    uint64_t q = vk_pending_seq();
    if (vsc.cb != cb || vsc.seq != q) { vsc.cb = cb; vsc.seq = q; vsc.vp_ok = vsc.sc_ok = false; }
    return true;
}
static uint64_t g_cur_fs_hash;   // (tentative; defined with the program state below)
static void apply_scissor(void) {
    VkRect2D r = scissor_rect();
    // LARECOMP_TINY_SCISSOR_FS=<fs hash> (measurement): that shader's draws get a 1x1 scissor, so its vertex work still runs
    // but almost no fragments do. A/B with the GPU profile: cost gone = fragment-bound, cost kept = vertex / setup-bound.
    // "all" = every draw (Deck power split: GPU fragment watts vs render-thread CPU, same CPU work).
    static uint64_t tiny = 1;
    if (tiny == 1) {
        const char *e = getenv("LARECOMP_TINY_SCISSOR_FS");
        tiny = !e ? 0 : !strcmp(e, "all") ? 2 : strtoull(e, NULL, 16);
    }
    if (tiny && (tiny == 2 || g_cur_fs_hash == tiny)) r = (VkRect2D){{0, 0}, {1, 1}};
    VkCommandBuffer cb = vk_cmd();
    vsc_same_cb(cb);
    if (vsc.sc_ok && !memcmp(&vsc.sc, &r, sizeof r)) return;
    vkCmdSetScissor(cb, 0, 1, &r);
    vsc.sc = r; vsc.sc_ok = true;
}

// Metal convention (y up in NDC, origin top-left) through a negative-height viewport.
// LARECOMP_VP_EXACT (default on, =0 off): see draw_'s viewport snap. translate.c reads the same switch for the VS epilogue.
bool vk_vp_exact(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("LARECOMP_VP_EXACT"); on = !e || strcmp(e, "0"); }
    return on;
}

static void set_viewport(float x, float y, float w, float h, float zn, float zf) {
    if (w <= 0) w = 1;
    if (h == 0) h = 1;
    VkViewport vp = {x, y + h, w, -h, fminf(fmaxf(zn, 0), 1), fminf(fmaxf(zf, 0), 1)};
    VkCommandBuffer cb = vk_cmd();
    vsc_same_cb(cb);
    if (vsc.vp_ok && !memcmp(&vsc.vp, &vp, sizeof vp)) return;
    vkCmdSetViewport(cb, 0, 1, &vp);
    vsc.vp = vp; vsc.vp_ok = true;
}

// ================================================================== clears
static VkPipelineLayout pc_layout(void) {  // push constants only (clears)
    static VkPipelineLayout l;
    if (!l) {
        VkPushConstantRange r = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32};
        VkPipelineLayoutCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .pushConstantRangeCount = 1,
                                         .pPushConstantRanges = &r};
        VK_CHECK(vkCreatePipelineLayout(g_vk.dev, &ci, NULL, &l));
    }
    return l;
}

static const VkDynamicState DYN[] = {
    VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_DEPTH_BIAS, VK_DYNAMIC_STATE_BLEND_CONSTANTS,
    VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE,
    VK_DYNAMIC_STATE_CULL_MODE, VK_DYNAMIC_STATE_FRONT_FACE, VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY,
    VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE, VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE, VK_DYNAMIC_STATE_DEPTH_COMPARE_OP,
    VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE, VK_DYNAMIC_STATE_STENCIL_OP, VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE,
    VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE, VK_DYNAMIC_STATE_ATTACHMENT_FEEDBACK_LOOP_ENABLE_EXT};
#define NDYN (sizeof DYN / sizeof DYN[0] - (g_vk.feedback_loop ? 0 : 1))   // (the last one only with device.c's feedback loops)

// Dynamic state draw_ used to set in full on every draw: a value equal to the one the command buffer already holds is
// skipped. Keyed like bind_check (command buffer + submission); dyn_defaults and the blit / clear pipeline binds call
// dyn_invalidate() (the blit pipelines have only viewport / scissor dynamic, so binding one leaves the rest undefined).
// LARECOMP_VK_DYN_CACHE=0: everything every draw, as before.
static struct {
    VkCommandBuffer cb;
    uint64_t seq;
    bool valid, sten_valid;
    bool dt, dw, sten, bias_on, restart;
    VkCompareOp dop;
    VkCullModeFlags cull;
    VkFrontFace front;
    VkPrimitiveTopology topo;
    float blend[4], bias[3];
    uint32_t sop[2], cmask[2], wmask[2], ref[2];
    VkPipeline pipe;   // graphics pipeline bound last by draw_ (VK_NULL_HANDLE: unknown)
    VkImageAspectFlags fbl;   // attachment feedback loop aspects (g_vk.feedback_loop)
    int8_t clamp;   // dynamic depth clamp (vk_dyn_clamp); -1 after a pipeline with a static clamp was bound
} dyn;
static void dyn_invalidate(void) { dyn.valid = false; dyn.pipe = VK_NULL_HANDLE; vsc.vp_ok = vsc.sc_ok = false; }
void vk_dyn_invalidate(void) { dyn_invalidate(); }   // (device.c: its own viewport / scissor / pipeline in this command buffer)
static bool dyn_begin(VkCommandBuffer cb) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("LARECOMP_VK_DYN_CACHE"); on = !e || strcmp(e, "0"); }
    uint64_t q = vk_pending_seq();
    if (!on || dyn.cb != cb || dyn.seq != q) { dyn.cb = cb; dyn.seq = q; dyn.valid = false; dyn.pipe = VK_NULL_HANDLE; }
    if (!dyn.valid) dyn.sten_valid = false;
    return dyn.valid;
}
static void dyn_end(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("LARECOMP_VK_DYN_CACHE"); on = !e || strcmp(e, "0"); }
    dyn.valid = on;
}

// Fixed-function state that isn't part of a pipeline is set explicitly on every bind (it persists across pipelines).
static void dyn_defaults(VkCommandBuffer cb) {
    dyn_invalidate();
    vkCmdSetDepthBias(cb, 0, 0, 0);
    vkCmdSetDepthBiasEnable(cb, VK_FALSE);
    float bc[4] = {0, 0, 0, 0};
    vkCmdSetBlendConstants(cb, bc);
    vkCmdSetStencilCompareMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, 0xFF);
    vkCmdSetStencilWriteMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, 0xFF);
    vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_AND_BACK, 0);
    vkCmdSetCullMode(cb, VK_CULL_MODE_NONE);
    vkCmdSetFrontFace(cb, VK_FRONT_FACE_COUNTER_CLOCKWISE);
    vkCmdSetPrimitiveTopology(cb, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    vkCmdSetDepthTestEnable(cb, VK_FALSE);
    vkCmdSetDepthWriteEnable(cb, VK_FALSE);
    vkCmdSetDepthCompareOp(cb, VK_COMPARE_OP_ALWAYS);
    vkCmdSetStencilTestEnable(cb, VK_FALSE);
    vkCmdSetStencilOp(cb, VK_STENCIL_FACE_FRONT_AND_BACK, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP,
                      VK_COMPARE_OP_ALWAYS);
    vkCmdSetPrimitiveRestartEnable(cb, VK_FALSE);
}

typedef struct { uint64_t key; VkPipeline pipe; } PipeEnt;
#define CLEAR_TAB 256
static PipeEnt clear_tab[CLEAR_TAB];

static VkPipeline clear_pipeline(int index, int kind, uint32_t mask, bool depth, bool stencil) {
    uint64_t key = 0xcbf29ce484222325ull;
#define MIX(v) do { uint64_t v_ = (uint64_t)(v); for (int b_ = 0; b_ < 8; b_++) { key ^= (v_ >> (b_ * 8)) & 0xFF; key *= 0x100000001B3ull; } } while (0)
    MIX(index); MIX(kind); MIX(mask); MIX(depth); MIX(stencil); MIX(pass_ncolor); MIX(pass_depth); MIX(pass_stencil); MIX(pass_samples);
    for (uint32_t i = 0; i < pass_ncolor; i++) MIX(pass_color[i]);
    uint32_t slot = (uint32_t)key & (CLEAR_TAB - 1);
    for (int p = 0; p < CLEAR_TAB; p++, slot = (slot + 1) & (CLEAR_TAB - 1)) {
        if (clear_tab[slot].key == key) return clear_tab[slot].pipe;
        if (!clear_tab[slot].key) break;
    }
    char fsname[32];
    if (index >= 0) snprintf(fsname, sizeof fsname, "clear.frag.%c%d", kind == 1 ? 'i' : kind == 2 ? 'u' : 'f', index);
    else snprintf(fsname, sizeof fsname, "clear.frag.none");
    VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT,
         .module = vk_builtin_shader("clear.vert"), .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
         .module = vk_builtin_shader(fsname), .pName = "main"}};
    VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                 .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo vs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                 .polygonMode = VK_POLYGON_MODE_FILL, .lineWidth = 1};
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                               .rasterizationSamples = (VkSampleCountFlagBits)pass_samples};
    VkPipelineDepthStencilStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState cba[8] = {0};
    for (uint32_t i = 0; i < pass_ncolor; i++)
        if ((int)i == index) cba[i].colorWriteMask = mask & 0xF;
    VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                              .attachmentCount = pass_ncolor, .pAttachments = cba};
    VkPipelineDynamicStateCreateInfo dy = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                           .dynamicStateCount = NDYN, .pDynamicStates = DYN};
    VkPipelineRenderingCreateInfo rinfo = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = pass_ncolor,
                                           .pColorAttachmentFormats = pass_color, .depthAttachmentFormat = pass_depth,
                                           .stencilAttachmentFormat = pass_stencil};
    VkGraphicsPipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &rinfo, .stageCount = 2,
                                       .pStages = stages, .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vs,
                                       .pRasterizationState = &rs, .pMultisampleState = &ms, .pDepthStencilState = &ds,
                                       .pColorBlendState = &cb, .pDynamicState = &dy, .layout = pc_layout()};
    VkPipeline p = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(g_vk.dev, g_vk.pcache, 1, &pi, NULL, &p) != VK_SUCCESS) LOG("[vk] clear pipeline failed\n");
    clear_tab[slot].key = key;
    clear_tab[slot].pipe = p;
    return p;
}

static void clear(int index, int kind, const void *color, uint32_t mask, bool depth, float dval, bool stencil, uint32_t sval,
                  uint32_t smask) {
    begin_pass();
    for (int i = 0; i <= st.rt.ncolor; i++) {   // (LARECOMP_SNAP_DELTA: this clear may write any of the pass's attachments)
        NvnTexture *t = i < st.rt.ncolor ? st.rt.color[i] : st.rt.depth;
        if (!t || !t->backend) continue;
        TexBackend *rb = i < st.rt.ncolor ? vk_render_backend(t) : t->backend;
        if (rb && rb->snap_tag && snap_trace_on()) LOG("[snaptrace] kill img %p by clear()\n", (void *)rb->image);
        if (rb) rb->snap_tag = 0;
        ((TexBackend *)t->backend)->snap_tag = 0;
    }
    VkCommandBuffer cb = vk_cmd();
    vp_sx = pass_sx; vp_sy = pass_sy; vp_ox = 0; vp_oy = 0;
    VkRect2D rect = scissor_rect();
    if (!rect.extent.width || !rect.extent.height) return;
    if (index >= 0 && (index >= (int)pass_ncolor || pass_color[index] == VK_FORMAT_UNDEFINED)) return;
    bool simple = (index < 0 || (mask & 0xF) == 0xF) && (!stencil || (smask & 0xFF) == 0xFF);
    if (simple) {
        VkClearAttachment a[2];
        uint32_t n = 0;
        if (index >= 0) {
            a[n] = (VkClearAttachment){.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .colorAttachment = (uint32_t)index};
            memcpy(a[n].clearValue.color.float32, color, 16);
            n++;
        }
        VkImageAspectFlags dsa = (depth && pass_depth ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) | (stencil && pass_stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
        if (dsa) a[n++] = (VkClearAttachment){.aspectMask = dsa, .clearValue.depthStencil = {dval, sval}};
        VkClearRect cr = {rect, 0, 1};
        if (n) vkCmdClearAttachments(cb, n, a, 1, &cr);
        return;
    }
    if (pass_fbl) {   // (g_vk.fbl_layout: the clear pipeline has no feedback-loop flag: restart the pass with GENERAL layouts)
        vk_end_pass();
        begin_pass();
        cb = vk_cmd();
    }
    VkPipeline p = clear_pipeline(index, kind, mask, depth, stencil);
    if (!p) return;
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    dyn_defaults(cb);
    if (depth) { vkCmdSetDepthTestEnable(cb, VK_TRUE); vkCmdSetDepthWriteEnable(cb, VK_TRUE); }
    if (stencil) {
        vkCmdSetStencilTestEnable(cb, VK_TRUE);
        vkCmdSetStencilOp(cb, VK_STENCIL_FACE_FRONT_AND_BACK, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_REPLACE, VK_STENCIL_OP_REPLACE,
                          VK_COMPARE_OP_ALWAYS);
        vkCmdSetStencilWriteMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, smask);
        vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_AND_BACK, sval);
    }
    int w, h;
    pass_size(&w, &h);
    VkViewport vp = {0, 0, (float)(w ? w : (int)pass_w), (float)(h ? h : (int)pass_h), 0, 1};
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &rect);
    float pc[8] = {0};
    if (color) memcpy(pc, color, 16);
    pc[4] = dval;
    vkCmdPushConstants(cb, pc_layout(), VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32, pc);
    vkCmdDraw(cb, 3, 1, 0, 0);
}

// ================================================================== programs
#define MAX_BIND 96
typedef struct {
    ShaderInfo *vi, *fi;
    uint64_t code[2], cmax[2];  // translation inputs (copied: translated on the translator thread)
    uint64_t csize[2];          // compiler constants (c1) after the code, bytes (0: none / archive program; translate_stage)
    ShaderKey vkey, fkey;
    VkShaderModule vs, fs;
    VkBindingInfo binds[MAX_BIND];
    int nbinds;
    VkDescriptorSetLayout set_layout[2];
    VkPipelineLayout layout;
    VkPipelineLayout lib_layout[2];   // GPL shared libraries: {set 0} for the VS library, {-, set 1} for the FS library
    bool vs_uses_vid;
    _Atomic int state;  // 0 building, 1 ready, 2 failed
    uint64_t ckey;        // content key (code hashes + keys) for the shader precompile; 0 = not recorded
    bool inline_compile;  // precompile thread: compile on the calling thread, not on the job pool
    bool translate_only;  // vk_translate_stress: translated, nothing compiled
    bool sync_try;        // game's first sight within the sync budget: translated on the drawing thread, compiled there too if its SPIR-V is cached
    bool glsl_saved;      // debug_draw saved its GLSL under this session's code addresses
    VkPipeline lib_pre, lib_frag;   // GPL: pre-rasterization (VS, fill, no depth clamp) / fragment shader (1 sample) libraries
    VkPipeline lib_frag_none;       // GPL: the same fragment library without the shader (fs_elidable pipelines; made on first use)
    _Atomic int lib_state;          // 0 none, 2 ready, 3 failed (g_vk.gpl)
    bool modules_dropped;           // vs / fs destroyed after the libraries: reload from the SPIR-V cache by spv_key
    uint64_t spv_key[2];
    _Atomic bool hurry;             // a held frame waits for it: its build queues ahead (job_hurry)
    bool build_now;                 // first seen by a fullscreen / tile draw (TOTK_HITCH_FULL_BUILD): built on the drawing thread
    uint64_t hurry_f;               // (drawing thread: the frame it was last hurried in)
    uint64_t first_draw_f;          // (VK_PROF=2, drawing thread: the presented frame of its first draw, +1)
} Prog;

// Uniform buffers are bound as UNIFORM_BUFFER_DYNAMIC (the span's offset is given at vkCmdBindDescriptorSets): a stage's
// set then depends on the buffers, ranges, views and samplers only, so draws of one material with per-object constants
// share it (stage_set's reuse table) instead of vkAllocateDescriptorSets + vkUpdateDescriptorSets per draw. Game cbufs
// (bindings 3..17) first, then the per-draw driver data (0, 18, 19), constants (1) last, up to
// maxDescriptorSetUniformBuffersDynamic per layout; the rest stay plain. LARECOMP_VK_DYN_UBO=0: all plain (old).
static int dyn_ubo_on(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("LARECOMP_VK_DYN_UBO"); on = !e || strcmp(e, "0"); }
    return on;
}
static void make_dynamic_ubos(Prog *p) {
    if (!dyn_ubo_on()) return;
    uint32_t lim = g_vk.props.limits.maxDescriptorSetUniformBuffersDynamic, n = 0, total = 0;
    static const int pass_of[20] = {1, 2, 3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1};  // 0 game, 1 driver, 2 constants
    for (int pass = 0; pass < 3; pass++)
        for (int i = 0; i < p->nbinds; i++) {
            VkBindingInfo *b = &p->binds[i];
            if (b->type != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER) continue;
            int k = b->binding < 20 ? pass_of[b->binding] : 3;
            if (pass == 0) total++;
            if (k != pass || n >= lim) continue;
            b->type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
            n++;
        }
    static int nlog;
    if (n < total && nlog++ < 8) LOG("[vk] program with %u uniform buffers: %u dynamic (limit %u), the rest plain\n", total, n, lim);
}
// Shared stage libraries (2026-10-17, user: "speed up shader compiling while keeping the same amount of shaders"): the
// archive's 18325 programs hold only 13346 distinct vertex and 8320 distinct fragment shaders, but each program's VS / FS
// library was created with the program's whole layout (VS set + FS set), so the driver compiled a VS again for every FS
// it was paired with. With independent sets (DXVK's layout) each library sees only its own stage's set, so the same
// shader + set gives the same library and the driver's pipeline cache returns it instead of compiling (up to 36650 ->
// 21666 compiles; FS GLSL also depends on the VS outputs). Linked and monolithic pipelines use the full layout, which
// carries the flag too (descriptor binds must match it). LARECOMP_GPL_SHARED=0: the old per-program layouts.
static bool gpl_shared(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("LARECOMP_GPL_SHARED"); v = g_vk.gpl && !(e && *e == '0'); }
    return v;
}
static VkPipelineLayout make_layout(Prog *p, int nsets, VkShaderStageFlags s0, VkShaderStageFlags s1) {
    make_dynamic_ubos(p);
    p->set_layout[0] = vk_set_layout(p->binds, p->nbinds, 0, s0);
    if (nsets > 1) p->set_layout[1] = vk_set_layout(p->binds, p->nbinds, 1, s1);
    bool ind = nsets == 2 && gpl_shared();
    VkPipelineLayoutCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = (uint32_t)nsets,
                                     .pSetLayouts = p->set_layout,
                                     .flags = ind ? VK_PIPELINE_LAYOUT_CREATE_INDEPENDENT_SETS_BIT_EXT : 0};
    VkPipelineLayout l;
    VK_CHECK(vkCreatePipelineLayout(g_vk.dev, &ci, NULL, &l));
    if (ind) {
        VkDescriptorSetLayout fs_only[2] = {VK_NULL_HANDLE, p->set_layout[1]};
        ci.setLayoutCount = 1;
        VK_CHECK(vkCreatePipelineLayout(g_vk.dev, &ci, NULL, &p->lib_layout[0]));
        ci.setLayoutCount = 2;
        ci.pSetLayouts = fs_only;
        VK_CHECK(vkCreatePipelineLayout(g_vk.dev, &ci, NULL, &p->lib_layout[1]));
    }
    return l;
}

static void save_glsl(const char *src, uint64_t addr, const char *suffix) {
    // LARECOMP_SAVE_GLSL=1: save only (no pc annotations, so the SPIR-V cache still hits; the launcher sets it for F9)
    if (!getenv("LARECOMP_SAVE_MSL") && !getenv("LARECOMP_SAVE_GLSL")) return;
    mkdir("build/glsl", 0755);
    char path[256];
    snprintf(path, sizeof path, "build/glsl/%llx_%s.glsl", (unsigned long long)addr, suffix);
    FILE *f = fopen(path, "w");
    if (f) { fputs(src, f); fclose(f); }
}

// ================================================================== graphics pipeline libraries (g_vk.gpl)
// DXVK's answer to first-draw stutter. Each program's shaders are compiled once, when the program is built (by the
// game's request or by the precompile), into a pre-rasterization library (VS) and a fragment-shader library (FS). A new
// draw state then only fast-links them with small vertex-input / fragment-output libraries (cached by state) on the
// drawing thread, instead of a full pipeline compile on a worker while the draw is skipped and the frame held.
// Pipelines with line fill, multisampling or alpha-to-coverage keep the monolithic path; depth clamp too unless it is
// dynamic in the pre-rasterization library (vk_dyn_clamp, device.c; draw_ sets it per linked pipeline).
// LARECOMP_NO_GPL=1: off (device.c). Stats line: "linked N pipelines (ms)".
extern bool vk_dyn_clamp;
static __thread bool pa_tm_on;            // (archive warm-up stage timers: see compile_program)
static __thread bool gpl_driver_cache;    // (archive warm-up: libraries into the driver's own cache only, see pa_job)
static __thread uint64_t pa_tm[8];
static _Atomic uint64_t pa_tm_sum[8], pa_tm_n;
_Atomic int stat_gpl_links;
_Atomic uint64_t stat_gpl_link_ns;
static VkPipeline gpl_create(VkGraphicsPipelineCreateInfo *pi, VkGraphicsPipelineLibraryFlagsEXT what) {
    VkGraphicsPipelineLibraryCreateInfoEXT li = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT,
                                                 .pNext = pi->pNext, .flags = what};
    pi->pNext = &li;
    VkPipelineCreationFeedback fb = {0};   // (warm-up stage timers: was it a pipeline-cache hit?)
    VkPipelineCreationFeedbackCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_CREATION_FEEDBACK_CREATE_INFO,
                                                .pNext = pi->pNext, .pPipelineCreationFeedback = &fb};
    if (pa_tm_on) pi->pNext = &fci;
    pi->flags |= VK_PIPELINE_CREATE_LIBRARY_BIT_KHR;
    VkPipeline p = VK_NULL_HANDLE;
    VkResult r = vkCreateGraphicsPipelines(g_vk.dev, gpl_driver_cache ? VK_NULL_HANDLE : g_vk.pcache, 1, pi, NULL, &p);
    if (pa_tm_on && r == VK_SUCCESS) {
        pa_tm[7]++;
        if ((fb.flags & VK_PIPELINE_CREATION_FEEDBACK_VALID_BIT) &&
            (fb.flags & VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT)) pa_tm[6]++;
    }
    if (r != VK_SUCCESS) {
        static int n;
        if (n++ < 10) LOG("[vk] pipeline library %#x failed: %d\n", (unsigned)what, r);
        return VK_NULL_HANDLE;
    }
    return p;
}
// Translated stages with warp ops (ShaderInfo.uses_subgroup) run in 32-wide subgroups where the device can require it
// (subgroup size control; device.c g_vk.sg32_stages, empty where the size is always 32, NVIDIA), Intel 8-32.
// Elsewhere the translator's maxwell_wave_safe forms keep a warp inside its 32-lane half.
static const VkPipelineShaderStageRequiredSubgroupSizeCreateInfo sg32_info = {
    .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO, .requiredSubgroupSize = 32};
static const void *sg_pin(const ShaderInfo *si, VkShaderStageFlags stage) {
    return si && si->uses_subgroup && (g_vk.sg32_stages & stage) ? &sg32_info : NULL;
}
static void gpl_program_libs(Prog *p) {
    VkPipelineRenderingCreateInfo ri = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    VkPipelineDynamicStateCreateInfo dy = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                           .dynamicStateCount = NDYN, .pDynamicStates = DYN};
    VkPipelineShaderStageCreateInfo vs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT,
                                          .module = p->vs, .pName = "main", .pNext = sg_pin(p->vi, VK_SHADER_STAGE_VERTEX_BIT)};
    VkPipelineShaderStageCreateInfo fs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
                                          .module = p->fs, .pName = "main", .pNext = sg_pin(p->fi, VK_SHADER_STAGE_FRAGMENT_BIT)};
    VkPipelineViewportStateCreateInfo vp = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                 .polygonMode = VK_POLYGON_MODE_FILL, .lineWidth = 1};
    VkDynamicState dyn_pre[sizeof DYN / sizeof DYN[0] + 1];
    memcpy(dyn_pre, DYN, sizeof DYN);
    VkPipelineDynamicStateCreateInfo dyp = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                            .dynamicStateCount = NDYN, .pDynamicStates = dyn_pre};
    if (vk_dyn_clamp) dyn_pre[dyp.dynamicStateCount++] = VK_DYNAMIC_STATE_DEPTH_CLAMP_ENABLE_EXT;
    VkGraphicsPipelineCreateInfo a = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &ri, .stageCount = 1, .pStages = &vs,
                                      .pViewportState = &vp, .pRasterizationState = &rs, .pDynamicState = &dyp,
                                      .layout = p->lib_layout[0] ? p->lib_layout[0] : p->layout};
    p->lib_pre = gpl_create(&a, VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT);
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                               .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkPipelineDepthStencilStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkGraphicsPipelineCreateInfo b = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &ri, .stageCount = 1, .pStages = &fs,
                                      .pMultisampleState = &ms, .pDepthStencilState = &ds, .pDynamicState = &dy,
                                      .layout = p->lib_layout[1] ? p->lib_layout[1] : p->layout};
    p->lib_frag = p->lib_pre ? gpl_create(&b, VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT) : VK_NULL_HANDLE;
    atomic_store(&p->lib_state, p->lib_pre && p->lib_frag ? 2 : 3);
}

static uint64_t hmix(uint64_t h, const void *p, size_t n);
// A stage's code hash as draw_ computes it (0x200 bytes after the 0x30 header), but never past code + constants: recorded
// and archived programs are exact-size host copies (U22, d71_imm: SIGSEGV at boot reading the page after one).
static uint64_t prog_code_hash(const Prog *p, int i) {
    uint64_t n = p->cmax[i] + p->csize[i];
    return hmix(1469598103934665603ull, (const void *)(uintptr_t)(p->code[i] + 0x30), n >= 0x230 ? 0x200 : n > 0x30 ? n - 0x30 : 0);
}
// job_hurry for a program (or a compute program: Prog is its first member), once a frame; id names it in the build log
static void prog_hurry(Prog *p, const char *what, uint64_t id) {
    uint64_t f = renderer_present_count();
    if (!hurry_on() || p->hurry_f == f) return;
    p->hurry_f = f;
    atomic_store(&p->hurry, true);   // (before the queues are searched: a translation finishing now queues its build first)
    job_hurry(p, what, id);
}
_Atomic int stat_prog_builds;   // (game-requested programs only; the precompile's own builds are not counted)
_Atomic uint64_t stat_prog_translate_ns, stat_prog_compile_ns;
// Archive warm-up stage timers (2026-10-17, user: "any way u can speed up shader compiling while keeping the same amount of
// shaders"): per program, thread time in each stage, summed over the warm-up workers and logged with each warm-up record
// ("[precompile] warm-up stages"). 0 lock wait, 1 translate (under translate_lock), 2 GLSL -> SPIR-V (disk cache or
// shaderc), 3 modules + reflection + layout, 4 GPL libraries (the driver's compile); 5 = stages whose SPIR-V was cached;
// 6 = libraries the driver reported as pipeline-cache hits (creation feedback), 7 = libraries created (declared above
// gpl_create).
// LARECOMP_GLSL_SWAP=<dir> (debug): <dir>/<fs hash>_vs.glsl / _fs.glsl replace that program's translated GLSL; the
// translated text goes to <dir>/<fs hash>_<stage>.orig.glsl. Descriptors follow the swapped SPIR-V's reflection.
// (2026-10-18: which stage of fs d4201cbc hangs the NVIDIA driver)
static void glsl_swap(char **msl, const char *fs, const char *stage) {
    const char *dir = getenv("LARECOMP_GLSL_SWAP");
    if (!dir || !*msl) return;
    char path[512];
    snprintf(path, sizeof path, "%s/%s_%s.glsl", dir, fs, stage);
    FILE *f = fopen(path, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *s = malloc(n + 1);
    s[fread(s, 1, n, f)] = 0;
    fclose(f);
    snprintf(path, sizeof path, "%s/%s_%s.orig.glsl", dir, fs, stage);
    if ((f = fopen(path, "wb"))) { fputs(*msl, f); fclose(f); }
    free(*msl);
    *msl = s;
    LOG("[shader] fs %s: %s GLSL swapped (LARECOMP_GLSL_SWAP)\n", fs, stage);
}
static void full_est_feed(uint64_t dt);
static void compile_program(void *arg) {
    Prog *p = arg;
    uint64_t t0 = SDL_GetTicksNS();
    if (pa_tm_on) pa_tm[5] += vk_spirv_cached(p->vi->msl, STAGE_VERTEX) + vk_spirv_cached(p->fi->msl, STAGE_FRAGMENT);
    size_t nv = 0, nf = 0;
    char nv_[40], nf_[40];  // names carry the guest code hash (fs hashes as in draw_; SPIR-V cache miss log)
    snprintf(nv_, sizeof nv_, "vs_%016llx", (unsigned long long)prog_code_hash(p, 0));
    snprintf(nf_, sizeof nf_, "fs_%016llx", (unsigned long long)prog_code_hash(p, 1));
    glsl_swap(&p->vi->msl, nf_ + 3, "vs");
    glsl_swap(&p->fi->msl, nf_ + 3, "fs");
    uint32_t *sv = vk_glsl_to_spirv(p->vi->msl, STAGE_VERTEX, nv_, &nv);
    uint32_t *sf = sv ? vk_glsl_to_spirv(p->fi->msl, STAGE_FRAGMENT, nf_, &nf) : NULL;
    if (!sv || !sf) { free(sv); free(sf); atomic_store(&p->state, 2); return; }
    uint64_t t1 = SDL_GetTicksNS();
    p->nbinds = vk_reflect(sv, nv, p->binds, MAX_BIND);
    p->nbinds += vk_reflect(sf, nf, p->binds + p->nbinds, MAX_BIND - p->nbinds);
    p->vs = vk_module(sv, nv);
    p->fs = vk_module(sf, nf);
    free(sv);
    free(sf);
    p->layout = make_layout(p, 2, VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_FRAGMENT_BIT);
    uint64_t t2 = SDL_GetTicksNS();
    if (g_vk.gpl && p->vs && p->fs) gpl_program_libs(p);
    if (pa_tm_on) { uint64_t t3 = SDL_GetTicksNS(); pa_tm[2] += t1 - t0; pa_tm[3] += t2 - t1; pa_tm[4] += t3 - t2; }
    bool ok = p->vs && p->fs;
    // With libraries, the modules and the GLSL are only needed again for a monolithic pipeline (line fill, depth clamp,
    // MSAA, A2C): drop them (~0.5 MB per program; ~10k programs are precompiled) and reload the SPIR-V from the disk
    // cache then (build_pipeline_body). LARECOMP_KEEP_MODULES=1: keep them (old).
    static int keep = -1;
    // (LARECOMP_SAVE_GLSL, set by the launcher for F9, no longer keeps them: the F9 frame re-translates dropped programs)
    if (keep < 0) keep = getenv("LARECOMP_KEEP_MODULES") || getenv("LARECOMP_SAVE_MSL") || getenv("LARECOMP_VK_NO_SPIRV_CACHE");
    if (ok && !keep && atomic_load(&p->lib_state) == 2 && vk_spirv_cached(p->vi->msl, STAGE_VERTEX) &&
        vk_spirv_cached(p->fi->msl, STAGE_FRAGMENT)) {
        p->spv_key[0] = vk_spirv_key(p->vi->msl, STAGE_VERTEX);
        p->spv_key[1] = vk_spirv_key(p->fi->msl, STAGE_FRAGMENT);
        vkDestroyShaderModule(g_vk.dev, p->vs, NULL);
        vkDestroyShaderModule(g_vk.dev, p->fs, NULL);
        p->vs = p->fs = VK_NULL_HANDLE;
        p->modules_dropped = true;
        free(p->vi->msl);
        free(p->fi->msl);
        p->vi->msl = p->fi->msl = NULL;
    }
    atomic_fetch_add(&stat_compiles, 1);
    if (!p->inline_compile) atomic_fetch_add(&stat_prog_compile_ns, SDL_GetTicksNS() - t0);
    // (a game program the workers built: the full-build mean follows the driver cache as it warms: once a cold boot
    // pushed it past the budget nothing inline would update it again; + 3 ms for its translation)
    if (!p->inline_compile && !p->build_now && ok) full_est_feed(SDL_GetTicksNS() - t0 + 3000000);
    atomic_store(&p->state, ok ? 1 : 2);
}

static uint64_t hmix(uint64_t h, const void *p, size_t n);
// First sight of a program (2026-10-17, user: "i still experience shader lag"; their 10-15 PC session held frames 36 times
// / 100 frames for programs still being built): a draw whose program was not built is skipped and the frame held. Most
// first-sight programs now have their SPIR-V in the disk cache and their pipeline libraries in the driver cache (archive
// warm-up, recordings), so the drawing thread translates and builds them itself while the time spent so in the last
// 50 ms stays under LARECOMP_PROG_SYNC_MS (default 8; 0 = off: the old skip + hold); a SPIR-V cache miss (a GLSL compile)
// still goes to the workers. "[newprog]" lines: each game-requested program's cost and path (first 300).
static uint64_t g_psync_win_t, g_psync_win_ns;   // (drawing thread)
// TOTK_HITCH=skip: the drawing thread spends at most TOTK_HITCH_MS (default 4) per presented frame on compiles, GPL links
// and program builds together, and a frame is not held for the rest. A fresh pipeline whose link would overrun the budget
// is linked on a worker and its draws are skipped meanwhile; a program build that would overrun it (cost = the running
// mean of earlier ones) goes to the translator. device.c then holds a frame only for what cannot be skipped: fullscreen /
// tile passes (<= 6 vertices) and compute. Both modes count the drawing thread's own compile time ("draw-thread compile"
// in the stats line).
int hitch_skip(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("TOTK_HITCH"); v = e && !strcmp(e, "skip"); if (v) LOG("[vk] TOTK_HITCH=skip: draws wait for compiles, frames do not\n"); }
    return v;
}
static uint64_t hitch_budget_ns(void) {
    static int64_t b = -1;
    if (b < 0) { const char *e = getenv("TOTK_HITCH_MS"); b = (int64_t)((e ? atof(e) : 4.0) * 1e6); }
    return (uint64_t)b;
}
static uint64_t g_hitch_f, g_hitch_ns, g_link_est = 1000000, g_prog_est = 3000000;   // (drawing thread; estimates = running means)
static uint64_t hitch_spent(void) {
    uint64_t f = renderer_present_count();
    if (f != g_hitch_f) { g_hitch_f = f; g_hitch_ns = 0; }
    return g_hitch_ns;
}
static bool hitch_fits(uint64_t est) { return !hitch_skip() || hitch_spent() + est <= hitch_budget_ns(); }
static void hitch_note(uint64_t dt, bool link) {
    hitch_spent();
    g_hitch_ns += dt;
    if (link) { stat_dt_link_ns += dt; stat_dt_links++; cum_dt_link_ns += dt; g_link_est = (g_link_est * 7 + dt) / 8; }
    else { stat_dt_prog_ns += dt; stat_dt_progs++; cum_dt_prog_ns += dt; g_prog_est = (g_prog_est * 7 + dt) / 8; }
}
// TOTK_PROG_SYNC_ADAPT (default on; =0: always as below): translating on the drawing thread pays only when the program's
// SPIR-V is in the disk cache (built there, no hold). Programs in precompile.bin / the seed are built at startup and never
// get here, so with the archive warm-up off nearly every one misses: the drawing thread translated for nothing (3.1 ms
// each, 5-7 in a fight's first frame, 97 ms hold) and the frame held anyway. While fewer than 1 in 4 of its recent tries
// hit, new programs go to the translator (TOTK_TRANSLATE_THREADS), 1 in 8 still tried here.
static uint32_t g_psync_tries = 2, g_psync_hits = 2, g_psync_probe;   // (drawing thread; halved at 64)
static bool g_psync_off;
static bool prog_sync_worth(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("TOTK_PROG_SYNC_ADAPT"); on = !e || strcmp(e, "0"); }
    bool off = on && g_psync_hits * 4 < g_psync_tries;
    if (off != g_psync_off) {
        g_psync_off = off;
        LOG("[newprog] drawing-thread translation %s: %u of the last %u had cached SPIR-V%s\n", off ? "paused" : "back on",
            g_psync_hits, g_psync_tries, off ? "; new programs go to the translator, 1 in 8 still tried here" : "");
    }
    return !off || (++g_psync_probe & 7) == 0;
}
// TOTK_HITCH_FULL_BUILD=1: a program first seen by a draw of <= 6 vertices is translated and built on the drawing thread
// whatever its SPIR-V cache state (~10-15 ms once, instead of the frame held until the translator and a worker get to
// it); a compute program's build likewise. Budget: per presented frame, inline full builds while the time spent on them
// plus the running mean of one fits TOTK_HITCH_FULL_MS (default 16); the rest takes the hold path as before. Cold, the
// mean grows and few fit; warm (~11 ms) one or two a frame. Compute builds keep their own mean: under the default 16 ms
// a compute builds inline only once its builds are cheap. (drawing thread: the draw asking get_program has <= 6
// vertices. draw is get_program's only caller today; __thread so a second caller on another thread could never take
// get_program_slow's inline branch and spend this frame's budget.)
static __thread bool g_prog_full_draw;
static uint64_t g_full_f, g_full_ns;   // (drawing thread: this frame's inline full builds)
static _Atomic uint64_t g_full_est = 15000000;   // running mean of a full build: inline ones and the workers' (full_est_feed)
static _Atomic uint64_t g_full_est_cs = 40000000;   // (the same for compute builds)
static bool full_build_on(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("TOTK_HITCH_FULL_BUILD");
        v = e && strcmp(e, "0");
        if (v) LOG("[vk] TOTK_HITCH_FULL_BUILD: fullscreen / tile and compute programs build on first sight\n");
    }
    return v;
}
static bool full_build_fits(bool cs) {
    static int64_t cap = -1;
    if (cap < 0) { const char *e = getenv("TOTK_HITCH_FULL_MS"); cap = (int64_t)((e ? atof(e) : 16.0) * 1e6); }
    uint64_t f = renderer_present_count();
    if (f != g_full_f) { g_full_f = f; g_full_ns = 0; }
    return full_build_on() && g_full_ns + atomic_load_explicit(cs ? &g_full_est_cs : &g_full_est, memory_order_relaxed) <= (uint64_t)cap;
}
static void full_est_feed_(_Atomic uint64_t *est, uint64_t dt) {   // (any thread; a lost update between two workers only drops one sample)
    uint64_t e = atomic_load_explicit(est, memory_order_relaxed);
    atomic_store_explicit(est, (e * 7 + dt) / 8, memory_order_relaxed);
}
static void full_est_feed(uint64_t dt) { full_est_feed_(&g_full_est, dt); }
static void full_build_note(uint64_t dt, bool cs) {   // (after an inline full build: this frame's spend and the mean)
    g_full_ns += dt;
    full_est_feed_(cs ? &g_full_est_cs : &g_full_est, dt);
}
static bool prog_sync_ok(void) {
    static long budget_ms = -1;
    if (budget_ms < 0) { const char *e = getenv("LARECOMP_PROG_SYNC_MS"); budget_ms = e ? atol(e) : 8; }
    if (!budget_ms || sync_shaders()) return false;
    if (!prog_sync_worth()) return false;
    if (hitch_skip()) {   // (the build must fit what is left of the frame's budget, not just start inside it)
        if (hitch_fits(g_prog_est)) return true;
        stat_prog_deferred++;
        return false;
    }
    // LARECOMP_PROG_SYNC_PER_FRAME: the budget is per presented frame. Per 50 ms of wall time a long frame opens a new budget every 50 ms
    // (4K boot gt4k_def, frame 762: 71 programs translated on the drawing thread, 382 ms); per frame the drawing thread spends at most the
    // budget plus one build, the rest goes to the translator and the frame waits for it as for any skipped program (held).
    static int per_frame = -1;
    if (per_frame < 0) { const char *e = getenv("LARECOMP_PROG_SYNC_PER_FRAME"); per_frame = !e || strcmp(e, "0"); }
    if (per_frame) {
        uint64_t f = renderer_present_count();
        if (f != g_psync_win_t) { g_psync_win_t = f; g_psync_win_ns = 0; }
        return g_psync_win_ns < (uint64_t)budget_ms * 1000000ull;
    }
    uint64_t now = SDL_GetTicksNS();
    if (now - g_psync_win_t > 50000000ull) { g_psync_win_t = now; g_psync_win_ns = 0; }
    return g_psync_win_ns < (uint64_t)budget_ms * 1000000ull;
}
static void prog_sync_note(const Prog *p, uint64_t t0, bool built) {
    uint64_t dt = SDL_GetTicksNS() - t0;
    g_psync_win_ns += dt;
    hitch_note(dt, false);
    if (!p->build_now) {   // (forced builds say nothing about the SPIR-V cache)
        g_psync_tries++;
        g_psync_hits += built;
        if (g_psync_tries >= 64) { g_psync_tries /= 2; g_psync_hits /= 2; }
    }
    static int n;
    if (n++ < 300)
        LOG("[newprog] vs %llx fs %llx: %s in %.1f ms\n", (unsigned long long)p->code[0], (unsigned long long)p->code[1],
            p->build_now ? "built on the drawing thread (fullscreen / tile pass, TOTK_HITCH_FULL_BUILD)"
            : built ? "built on the drawing thread (SPIR-V cached)" : "translated; SPIR-V not cached -> workers", dt / 1e6);
}
// TotK's upscale pass (fs 32845c90c668fdbb, FSR 1 EASU 1600x900 -> 1920x1080) replaced when the scene or the canvas is
// scaled: one resample from the scene's real pixels to the canvas's (TOTK_RES_SCALE 2 + TOTK_OUT_SCALE at 1440p = 3200x1800
// -> 2560x1440), instead of point samples on the 1600x900 grid. Catmull-Rom, widened to the source footprint when it
// shrinks; averaged through x / (1 + max(x)) so HDR highlights do not ring. Same bindings as the translated shader:
// cb3[0].xy = EASU con0 (input viewport / output size, guest pixels), rscale[0] = target scale + offset, rscale[1] = t0's.
static const char upscale_resample_glsl[] =
    "#version 460\n"
    "layout(std140, set = 1, binding = 3) uniform CB3 { uvec4 cb3[16]; };\n"
    "layout(std140, set = 1, binding = 18) uniform RScale { vec4 rscale[65]; };\n"
    "layout(set = 1, binding = 32) uniform sampler2D t0;\n"
    "layout(location = 0) out vec4 c0;\n"
    "float cr(float x) {\n"
    "  x = abs(x);\n"
    "  return x < 1.0f ? (1.5f * x - 2.5f) * x * x + 1.0f : x < 2.0f ? ((-0.5f * x + 2.5f) * x - 4.0f) * x + 2.0f : 0.0f;\n"
    "}\n"
    "void main() {\n"
    "  vec2 ratio = uintBitsToFloat(cb3[0].xy);\n"
    "  vec2 g = (gl_FragCoord.xy - rscale[0].zw) / rscale[0].xy;\n"
    "  vec2 p = g * ratio * rscale[1].xy;\n"
    "  vec2 f = max(ratio * rscale[1].xy / rscale[0].xy, vec2(1.0f));\n"
    "  ivec2 mx = textureSize(t0, 0) - 1;\n"
    "  ivec2 a = ivec2(floor(p - 2.0f * f)), b = min(ivec2(ceil(p + 2.0f * f)), a + 16);\n"
    "  vec4 acc = vec4(0.0f); float ws = 0.0f;\n"
    "  for (int y = a.y; y <= b.y; y++) {\n"
    "    float wy = cr((float(y) + 0.5f - p.y) / f.y);\n"
    "    if (wy == 0.0f) continue;\n"
    "    for (int x = a.x; x <= b.x; x++) {\n"
    "      float w = wy * cr((float(x) + 0.5f - p.x) / f.x);\n"
    "      if (w == 0.0f) continue;\n"
    "      vec4 v = texelFetch(t0, clamp(ivec2(x, y), ivec2(0), mx), 0);\n"
    "      v.rgb = max(v.rgb, vec3(0.0f));\n"
    "      v.rgb /= 1.0f + max(max(v.r, v.g), v.b);\n"
    "      acc += w * v; ws += w;\n"
    "    }\n"
    "  }\n"
    "  acc /= max(ws, 1e-6f);\n"
    "  acc.rgb = clamp(acc.rgb, vec3(0.0f), vec3(0.999f));\n"
    "  acc.rgb /= 1.0f - max(max(acc.r, acc.g), acc.b);\n"
    "  c0 = vec4(clamp(acc.rgb, vec3(0.0f), vec3(1.0f)), 1.0f);\n"
    "}\n";

// TOTK_SCENE_UPSCALE=easu: the same replacement pass with FSR 1 EASU (the engine's easu.frag, edge-adaptive:
// sharper edges at 900p -> 2160p than Catmull-Rom) when the scene is magnified; shrinking keeps the Catmull-Rom
// footprint. Taps go through the same reversible x / (1 + max(x)) so HDR highlights neither ring nor dominate the
// edge direction. Same bindings and mapping as upscale_resample_glsl.
static const char upscale_easu_glsl[] =
    "#version 460\n"
    "layout(std140, set = 1, binding = 3) uniform CB3 { uvec4 cb3[16]; };\n"
    "layout(std140, set = 1, binding = 18) uniform RScale { vec4 rscale[65]; };\n"
    "layout(set = 1, binding = 32) uniform sampler2D t0;\n"
    "layout(location = 0) out vec4 c0;\n"
    "ivec2 mx;\n"
    "vec3 tm(vec3 v) { v = max(v, vec3(0.0f)); return v / (1.0f + max(max(v.r, v.g), v.b)); }\n"
    "vec3 tap(ivec2 q) { return tm(texelFetch(t0, clamp(q, ivec2(0), mx), 0).rgb); }\n"
    "float lum(vec3 c) { return c.g + 0.5f * (c.r + c.b); }\n"
    "void setf(inout vec2 dir, inout float len, float w, float lA, float lB, float lC, float lD, float lE) {\n"
    "  float dx = lD - lB, dy = lE - lA;\n"
    "  float lx = clamp(abs(dx) / max(max(abs(lD - lC), abs(lC - lB)), 1.0f / 65536.0f), 0.0f, 1.0f);\n"
    "  float ly = clamp(abs(dy) / max(max(abs(lE - lC), abs(lC - lA)), 1.0f / 65536.0f), 0.0f, 1.0f);\n"
    "  dir += vec2(dx, dy) * w; len += (lx * lx + ly * ly) * w;\n"
    "}\n"
    "void tapf(inout vec3 ac, inout float aw, vec2 off, vec2 dir, vec2 len2, float lob, float clp, vec3 c) {\n"
    "  vec2 v = vec2(dot(off, dir), dot(off, vec2(-dir.y, dir.x))) * len2;\n"
    "  float d2 = min(dot(v, v), clp);\n"
    "  float wb = 0.4f * d2 - 1.0f, wa = lob * d2 - 1.0f;\n"
    "  wb *= wb; wa *= wa;\n"
    "  float w = (1.5625f * wb - 0.5625f) * wa;\n"
    "  ac += c * w; aw += w;\n"
    "}\n"
    "float cr(float x) {\n"
    "  x = abs(x);\n"
    "  return x < 1.0f ? (1.5f * x - 2.5f) * x * x + 1.0f : x < 2.0f ? ((-0.5f * x + 2.5f) * x - 4.0f) * x + 2.0f : 0.0f;\n"
    "}\n"
    "void main() {\n"
    "  vec2 ratio = uintBitsToFloat(cb3[0].xy);\n"
    "  vec2 g = (gl_FragCoord.xy - rscale[0].zw) / rscale[0].xy;\n"
    "  vec2 p = g * ratio * rscale[1].xy;\n"
    "  vec2 f = ratio * rscale[1].xy / rscale[0].xy;\n"
    "  mx = textureSize(t0, 0) - 1;\n"
    "  vec3 r;\n"
    "  if (f.x > 1.0f || f.y > 1.0f) {\n"
    "    f = max(f, vec2(1.0f));\n"
    "    ivec2 a = ivec2(floor(p - 2.0f * f)), b = min(ivec2(ceil(p + 2.0f * f)), a + 16);\n"
    "    vec3 acc = vec3(0.0f); float ws = 0.0f;\n"
    "    for (int y = a.y; y <= b.y; y++) {\n"
    "      float wy = cr((float(y) + 0.5f - p.y) / f.y);\n"
    "      if (wy == 0.0f) continue;\n"
    "      for (int x = a.x; x <= b.x; x++) {\n"
    "        float w = wy * cr((float(x) + 0.5f - p.x) / f.x);\n"
    "        if (w == 0.0f) continue;\n"
    "        acc += w * tap(ivec2(x, y)); ws += w;\n"
    "      }\n"
    "    }\n"
    "    r = acc / max(ws, 1e-6f);\n"
    "  } else {\n"
    "    vec2 pp = p - 0.5f;\n"
    "    vec2 fpf = floor(pp); pp -= fpf;\n"
    "    ivec2 fp = ivec2(fpf);\n"
    "    vec3 bC = tap(fp + ivec2(0, -1)), cC = tap(fp + ivec2(1, -1));\n"
    "    vec3 eC = tap(fp + ivec2(-1, 0)), fC = tap(fp), gC = tap(fp + ivec2(1, 0)), hC = tap(fp + ivec2(2, 0));\n"
    "    vec3 iC = tap(fp + ivec2(-1, 1)), jC = tap(fp + ivec2(0, 1)), kC = tap(fp + ivec2(1, 1)), lC = tap(fp + ivec2(2, 1));\n"
    "    vec3 nC = tap(fp + ivec2(0, 2)), oC = tap(fp + ivec2(1, 2));\n"
    "    float bL = lum(bC), cL = lum(cC), eL = lum(eC), fL = lum(fC), gL = lum(gC), hL = lum(hC);\n"
    "    float iL = lum(iC), jL = lum(jC), kL = lum(kC), lL = lum(lC), nL = lum(nC), oL = lum(oC);\n"
    "    vec2 dir = vec2(0.0f); float len = 0.0f;\n"
    "    setf(dir, len, (1.0f - pp.x) * (1.0f - pp.y), bL, eL, fL, gL, jL);\n"
    "    setf(dir, len, pp.x * (1.0f - pp.y), cL, fL, gL, hL, kL);\n"
    "    setf(dir, len, (1.0f - pp.x) * pp.y, fL, iL, jL, kL, nL);\n"
    "    setf(dir, len, pp.x * pp.y, gL, jL, kL, lL, oL);\n"
    "    float dr = dot(dir, dir);\n"
    "    dir = dr < 1.0f / 32768.0f ? vec2(1.0f, 0.0f) : dir * inversesqrt(dr);\n"
    "    len = len * 0.5f; len *= len;\n"
    "    float stretch = dot(dir, dir) / max(abs(dir.x), abs(dir.y));\n"
    "    vec2 len2 = vec2(1.0f + (stretch - 1.0f) * len, 1.0f - 0.5f * len);\n"
    "    float lob = 0.5f - 0.29f * len, clp = 1.0f / lob;\n"
    "    vec3 mn = min(min(fC, gC), min(jC, kC)), mxc = max(max(fC, gC), max(jC, kC));\n"
    "    vec3 ac = vec3(0.0f); float aw = 0.0f;\n"
    "    tapf(ac, aw, vec2(0.0f, -1.0f) - pp, dir, len2, lob, clp, bC);\n"
    "    tapf(ac, aw, vec2(1.0f, -1.0f) - pp, dir, len2, lob, clp, cC);\n"
    "    tapf(ac, aw, vec2(-1.0f, 1.0f) - pp, dir, len2, lob, clp, iC);\n"
    "    tapf(ac, aw, vec2(0.0f, 1.0f) - pp, dir, len2, lob, clp, jC);\n"
    "    tapf(ac, aw, vec2(0.0f, 0.0f) - pp, dir, len2, lob, clp, fC);\n"
    "    tapf(ac, aw, vec2(-1.0f, 0.0f) - pp, dir, len2, lob, clp, eC);\n"
    "    tapf(ac, aw, vec2(1.0f, 1.0f) - pp, dir, len2, lob, clp, kC);\n"
    "    tapf(ac, aw, vec2(2.0f, 1.0f) - pp, dir, len2, lob, clp, lC);\n"
    "    tapf(ac, aw, vec2(2.0f, 0.0f) - pp, dir, len2, lob, clp, hC);\n"
    "    tapf(ac, aw, vec2(1.0f, 0.0f) - pp, dir, len2, lob, clp, gC);\n"
    "    tapf(ac, aw, vec2(1.0f, 2.0f) - pp, dir, len2, lob, clp, oC);\n"
    "    tapf(ac, aw, vec2(0.0f, 2.0f) - pp, dir, len2, lob, clp, nC);\n"
    "    r = min(mxc, max(mn, ac / aw));\n"
    "  }\n"
    "  r = clamp(r, vec3(0.0f), vec3(0.999f));\n"
    "  r /= 1.0f - max(max(r.r, r.g), r.b);\n"
    "  c0 = vec4(clamp(r, vec3(0.0f), vec3(1.0f)), 1.0f);\n"
    "}\n";

// translate_program's debug and option variables: read once for every translator thread, published with a release store
// (the first-start pass translates on several threads without translate_lock, TOTK_PRECOMPILE_UNLOCKED: no shared state
// written per translation). The meaning of each is at its use below.
static uint64_t tp_vh; static int tp_vvid = -1;                                  // LARECOMP_DEBUG_VS
static uint64_t tp_th; static int tp_tx, tp_ty, tp_tx2 = -1, tp_ty2 = -1;        // LARECOMP_DEBUG_SHADER
static int tp_nsh; static uint64_t tp_sh[64];                                    // LARECOMP_DEBUG_SOLID
static uint64_t tp_nd;                                                           // LARECOMP_NO_DISCARD
static int tp_ssaa, tp_easu;                                                     // TOTK_SSAA, TOTK_SCENE_UPSCALE
static _Atomic int tp_env_ready;
static pthread_mutex_t tp_env_mu = PTHREAD_MUTEX_INITIALIZER;
static void tp_env(void) {
    if (atomic_load_explicit(&tp_env_ready, memory_order_acquire)) return;
    pthread_mutex_lock(&tp_env_mu);
    if (!atomic_load_explicit(&tp_env_ready, memory_order_relaxed)) {
        const char *e = getenv("LARECOMP_DEBUG_VS");
        char *q = NULL;
        tp_vh = e ? strtoull(e, &q, 16) : 0;
        if (q && *q == ',') tp_vvid = (int)strtol(q + 1, NULL, 10);
        if ((e = getenv("LARECOMP_DEBUG_SHADER"))) {
            tp_th = strtoull(e, &q, 16);
            if (*q == ',') { tp_tx = (int)strtol(q + 1, &q, 10); if (*q == ',') tp_ty = (int)strtol(q + 1, &q, 10); }
            if (*q == ',') { tp_tx2 = (int)strtol(q + 1, &q, 10); if (*q == ',') tp_ty2 = (int)strtol(q + 1, &q, 10); }
        }
        for (e = getenv("LARECOMP_DEBUG_SOLID"); e && *e && tp_nsh < 64;) {
            tp_sh[tp_nsh++] = strtoull(e, (char **)&e, 16);
            while (*e == ',') e++;
        }
        e = getenv("LARECOMP_NO_DISCARD");
        tp_nd = e && strcmp(e, "1") ? strtoull(e, NULL, 16) : 0;
        const char *rs = getenv("TOTK_RES_SCALE");
        bool scaled = rs && *rs && strcmp(rs, "1") && strcmp(rs, "1.0");
        e = getenv("TOTK_SSAA");
        // (-2: 1 or 2 by TOTK_OUT_SCALE, decided at the upscale program, translate_program)
        tp_ssaa = e && !strcmp(e, "0") ? 0 : e && !strcmp(e, "fetch") ? 1 : scaled ? 2 : -2;
        e = getenv("TOTK_SCENE_UPSCALE");
        tp_easu = e && !strcmp(e, "easu");
        if (tp_easu) LOG("[shader] scene upscale: FSR 1 EASU (TOTK_SCENE_UPSCALE=easu)\n");
        atomic_store_explicit(&tp_env_ready, 1, memory_order_release);
    }
    pthread_mutex_unlock(&tp_env_mu);
}

static void translate_program(void *arg) {
    Prog *p = arg;
    bool upscale_resample = false;
    tp_env();
    uint64_t t0 = SDL_GetTicksNS();
    if (!translate_unlocked) pthread_mutex_lock(&translate_lock);
    uint64_t tl = SDL_GetTicksNS();
    maxwell_gmem_base = HEAP_BASE;
    maxwell_gmem_size = HEAP_MAX;
    maxwell_gdev_size = gdev_window_size();
    {   // LARECOMP_DEBUG_VS=<fs hash hex>[,vid]: the vertex shader of the programs with that fragment shader prints the
        // inputs + clip position of every vertex far off screen (or w <= 0) as "[vtrace]..." lines, and with vid every
        // register write of gl_VertexIndex == vid ("[trace] pc r<n>=hex"). Needs the same validation printf env as
        // LARECOMP_DEBUG_SHADER. (2026-10-15: vertical streaks from the terrain grass shader, fs 657fb1ab0f780c67)
        if (tp_vh && p->code[1] && prog_code_hash(p, 1) == tp_vh) {
            extern MAXWELL_TLS bool maxwell_vtrace_next;
            extern MAXWELL_TLS int maxwell_vtrace_vid;
            maxwell_vtrace_next = true; maxwell_vtrace_vid = tp_vvid;
        }
    }
    p->vi = translate_stage(p->code[0], p->cmax[0], p->csize[0], STAGE_VERTEX, &p->vkey);
    if (p->vi->msl) {
        p->fkey.vs_out_mask = p->vi->out_mask;
        {   // LARECOMP_DEBUG_SHADER=<fs hash hex>,x,y: that fragment shader prints every register write at pixel (x, y)
            // (needs LARECOMP_VK_VALIDATION=1 + VK_LAYER_PRINTF_ONLY_PRESET=1 VK_LAYER_PRINTF_TO_STDOUT=1)
            // (optional second pixel: <hash>,x,y,x2,y2)
            if (tp_th && p->code[1] && prog_code_hash(p, 1) == tp_th) {
                extern MAXWELL_TLS int maxwell_trace_x2, maxwell_trace_y2;
                maxwell_trace_next = true; maxwell_trace_x = tp_tx; maxwell_trace_y = tp_ty; maxwell_trace_x2 = tp_tx2; maxwell_trace_y2 = tp_ty2;
            }
            // LARECOMP_DEBUG_SOLID=<fs hash hex>[,<hash>...] (up to 64): those fragment shaders write a solid colour and never
            // discard: one hash magenta into c0 (as before); a list gives the i-th hash colour ((i & 3) / 3, (i >> 2 & 3) / 3,
            // (i >> 4 & 3) / 3) in every float target, so the pass dumps (LARECOMP_DEBUG_FRAME) name the shader by the colour.
            const int nsh = tp_nsh;
            uint64_t ph1 = nsh && p->code[1] ? prog_code_hash(p, 1) : 0;
            for (int i = 0; i < nsh; i++) {
                if (ph1 != tp_sh[i]) continue;
                extern MAXWELL_TLS bool maxwell_solid_next;
                extern MAXWELL_TLS float maxwell_solid_rgb[3];
                extern MAXWELL_TLS bool maxwell_solid_all;
                maxwell_solid_next = true;
                maxwell_solid_all = nsh > 1;
                maxwell_solid_rgb[0] = nsh > 1 ? (i & 3) / 3.0f : 1.0f;
                maxwell_solid_rgb[1] = nsh > 1 ? (i >> 2 & 3) / 3.0f : 0.0f;
                maxwell_solid_rgb[2] = nsh > 1 ? (i >> 4 & 3) / 3.0f : 1.0f;
                LOG("[shader] fs hash %016llx: solid (%.2f %.2f %.2f)%s (LARECOMP_DEBUG_SOLID #%d)\n", (unsigned long long)ph1, maxwell_solid_rgb[0],
                    maxwell_solid_rgb[1], maxwell_solid_rgb[2], nsh > 1 ? " in every target" : "", i);
                break;
            }
            const uint64_t nd = tp_nd;  // LARECOMP_NO_DISCARD=<fs hash hex>: that fragment shader never discards (A/B)
            if (nd && p->code[1] && prog_code_hash(p, 1) == nd) {
                extern MAXWELL_TLS bool maxwell_nodiscard_next;
                maxwell_nodiscard_next = true;
                LOG("[shader] fs hash %016llx: discard disabled (LARECOMP_NO_DISCARD)\n", (unsigned long long)nd);
            }
            // TotK's upscale pass (FSR 1 EASU, scene 1600x900 -> 1920x1080) reads its 12 taps as single texels at guest
            // coordinates, so with TOTK_RES_SCALE only one texel of each scaled block reached the screen: the same
            // stair-steps at every scale. Its fetches average the whole block instead = supersampling. With TOTK_RES_SCALE
            // != 1 or TOTK_OUT_SCALE the whole pass becomes one resample to the canvas (upscale_resample_glsl).
            // TOTK_SSAA=fetch: the averaged fetches only; 0: the old pass. (TOTK_OUT_SCALE's part is decided here, at the
            // upscale program's own translation, not in tp_env: it needs the start's window mode, textures.c
            // renderer_out_scale_active, itself a once: nothing shared is written)
            int ssaa = tp_ssaa;
            if (ssaa && p->code[1] && prog_code_hash(p, 1) == 0x32845c90c668fdbbull) {
                if (ssaa == -2) {   // (unscaled by TOTK_RES_SCALE, no TOTK_SSAA: the resample iff the canvas is scaled)
                    extern int renderer_out_scale_active(void);
                    ssaa = renderer_out_scale_active() ? 2 : 1;
                }
                extern MAXWELL_TLS bool maxwell_ssfetch_next;
                maxwell_ssfetch_next = true;
                upscale_resample = ssaa == 2;
                LOG("[shader] fs hash 32845c90c668fdbb (upscale): %s\n", upscale_resample ? "one resample to the canvas" : "supersampled fetches");
            }
        }
        p->fi = translate_stage(p->code[1], p->cmax[1], p->csize[1], STAGE_FRAGMENT, &p->fkey);
        // (the translated shader's bindings stay: cb3, rscale and t0 are declared there too)
        if (upscale_resample && p->fi->msl && p->fi->ntex >= 1 && p->fi->uses_rscale && (p->fi->cbuf_mask & (1u << 3))) {
            free(p->fi->msl);
            p->fi->msl = strdup(tp_easu ? upscale_easu_glsl : upscale_resample_glsl);
        }
    }
    if (!translate_unlocked) pthread_mutex_unlock(&translate_lock);
    if (pa_tm_on) { pa_tm[0] += tl - t0; pa_tm[1] += SDL_GetTicksNS() - tl; }
    if (p->translate_only) return;   // (vk_translate_stress: the translation is all it compares)
    if (!p->inline_compile) {
        atomic_fetch_add(&stat_prog_builds, 1);
        atomic_fetch_add(&cum_prog_builds, 1);
        atomic_fetch_add(&stat_prog_translate_ns, SDL_GetTicksNS() - t0);   // (includes waiting for translate_lock)
    }
    if (!p->vi->msl) {
        LOG("[shader] VS translate failed: %s (code %llx)\n", p->vi->error, (unsigned long long)p->code[0]);
        atomic_store(&p->state, 2);
        return;
    }
    if (!p->fi->msl) {
        LOG("[shader] FS translate failed: %s\n", p->fi->error);
        atomic_store(&p->state, 2);
        return;
    }
    p->vs_uses_vid = strstr(p->vi->msl, "gl_VertexIndex") != NULL;
    if (!p->inline_compile) {   // not the precompile's ~10k programs (the launcher's SAVE_GLSL wrote ~20k files at boot)
        save_glsl(p->vi->msl, p->code[0], "vs");
        save_glsl(p->fi->msl, p->code[1], "fs");
    }
    if (p->inline_compile) compile_program(p);
    else if (p->sync_try && (p->build_now || (vk_spirv_cached(p->vi->msl, STAGE_VERTEX) && vk_spirv_cached(p->fi->msl, STAGE_FRAGMENT)))) {
        compile_program(p);   // (SPIR-V + driver cache hits after the archive warm-up: no skipped draw, no held frame)
        prog_sync_note(p, t0, true);
    } else {
        if (p->sync_try) prog_sync_note(p, t0, false);
        run_job(compile_program, p, 'p', build_log() ? prog_code_hash(p, 1) : 0, atomic_load(&p->hurry));
    }
}

#define PROG_TAB 16384
static struct { uint64_t key; Prog *prog; } prog_tab[PROG_TAB];

// shader precompile (below the pipeline code)
static bool pc_on(void);
static void pc_start(void);
static uint64_t pc_prog_key(const NvnProgram *np, const ShaderKey *vkey, const ShaderKey *fkey, uint64_t code_hash[2], uint64_t cmax[2]);
static Prog *pc_claim_prog(uint64_t ckey, Prog *fresh);
static void pc_record_prog(const Prog *p, const uint64_t code_hash[2]);

// Per-draw memos (render thread only): consecutive draws mostly repeat the previous draw's program and pipeline, but
// each one re-hashed the shader keys byte by byte and the ~700-byte pipeline key (pre 0.70 + key 0.50 us a draw). The
// memo compares the same inputs by value, so a hit returns what the table lookup would. vk_exec_reset bumps
// g_memo_gen (tables cleared).
static uint32_t g_memo_gen;
static Prog *get_program_slow(NvnProgram *np, ShaderKey *vkey, ShaderKey *fkey);
static Prog *get_program(NvnProgram *np, ShaderKey *vkey, ShaderKey *fkey) {
    if (!(np->stage_mask & 1) || !(np->stage_mask & 2)) return NULL;
    static struct { NvnProgram *np; uint64_t code[2]; uint32_t mask, gen; ShaderKey vkey; uint8_t rt[8], a2cd, smk; Prog *p; } memo;
    if (memo.p && memo.gen == g_memo_gen && memo.np == np && memo.code[0] == np->stages[0].code_addr &&
        memo.code[1] == np->stages[1].code_addr && memo.mask == np->stage_mask && !memcmp(&memo.vkey, vkey, sizeof *vkey) &&
        !memcmp(memo.rt, fkey->rt_kind, 8) && memo.a2cd == fkey->a2c_dither && memo.smk == fkey->snap_mark)
        return memo.p;
    Prog *res = get_program_slow(np, vkey, fkey);
    memo.np = np; memo.code[0] = np->stages[0].code_addr; memo.code[1] = np->stages[1].code_addr; memo.mask = np->stage_mask;
    memo.gen = g_memo_gen; memo.vkey = *vkey; memcpy(memo.rt, fkey->rt_kind, 8); memo.a2cd = fkey->a2c_dither; memo.smk = fkey->snap_mark; memo.p = res;
    return res;
}
static Prog *get_program_slow(NvnProgram *np, ShaderKey *vkey, ShaderKey *fkey) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (int i = 0; i < 2; i++) { uint64_t a = np->stages[i].code_addr; for (int k = 0; k < 8; k++) { h ^= (a >> (k * 8)) & 0xFF; h *= 0x100000001B3ull; } }
    const uint8_t *vb = (const uint8_t *)vkey;
    for (size_t i = 0; i < sizeof *vkey; i++) { h ^= vb[i]; h *= 0x100000001B3ull; }
    for (int i = 0; i < 8; i++) { h ^= fkey->rt_kind[i]; h *= 0x100000001B3ull; }
    if (fkey->a2c_dither) { h ^= 0xA2C0u; h *= 0x100000001B3ull; }
    if (fkey->snap_mark) { h ^= 0x5A4Bu; h *= 0x100000001B3ull; }    // (LARECOMP_SNAP_MARK, likewise)
    if (!h) h = 1;
    uint32_t slot = (uint32_t)(h ^ (h >> 31)) & (PROG_TAB - 1);
    while (prog_tab[slot].key && prog_tab[slot].key != h) slot = (slot + 1) & (PROG_TAB - 1);
    if (prog_tab[slot].key) return prog_tab[slot].prog;
    c1_persist_forget(np);
    uint64_t gz = PROFD ? prof_now() : 0;
#define GPS_ZONE(i) do { if (PROFD) { uint64_t n_ = prof_now(); gps_ns[i] += n_ - gz; gz = n_; } } while (0)
    // same code + keys under another address, or built ahead by the shader precompile: share that program
    uint64_t chash[2], cmax[2], ck = 0;
    if (pc_on() && !fkey->snap_mark) { pc_start(); ck = pc_prog_key(np, vkey, fkey, chash, cmax); }   // (pc_ckey has no snap_mark)
    GPS_ZONE(0);
    Prog *p = calloc(1, sizeof *p);
    for (int i = 0; i < 2; i++) {
        p->code[i] = np->stages[i].code_addr;
        p->cmax[i] = np->stages[i].const_offset ? np->stages[i].const_offset : 0x10000;
        p->csize[i] = np->stages[i].const_offset ? (np->stages[i].const_size < 0x10000 ? np->stages[i].const_size : 0x10000) : 0;
    }
    p->vkey = *vkey;
    p->fkey = *fkey;
    p->ckey = ck;
    if (ck) {
        Prog *q = pc_claim_prog(ck, p);
        GPS_ZONE(1);
        if (q != p) {
            // q from a PC_PROG record (no compiler constants): record this one with them for the next session; if q is
            // built and a BRX table was unreadable (every instruction a switch case: Deck d40 cloud VS, 5.5 ms a draw),
            // use this program instead, translated with its constants
            bool q_c1 = q->csize[0] || q->csize[1], p_c1 = p->csize[0] || p->csize[1];
            if (!q_c1 && p_c1) pc_record_prog(p, chash);
            bool blind = !q_c1 && p_c1 && atomic_load(&q->state) == 1 && ((q->vi && q->vi->brx_blind) || (q->fi && q->fi->brx_blind));
            if (!blind) { free(p); prog_tab[slot].key = h; prog_tab[slot].prog = q; return q; }
            LOG("[precompile] program %016llx: precompiled without constants, BRX blind -> translated again\n", (unsigned long long)ck);
        } else pc_record_prog(p, chash);
        GPS_ZONE(2);
    }
    prog_tab[slot].key = h;
    prog_tab[slot].prog = p;
    if (g_prog_full_draw && !sync_shaders() && full_build_fits(false)) {
        uint64_t t0 = SDL_GetTicksNS();
        p->sync_try = p->build_now = true;
        translate_program(p);
        full_build_note(SDL_GetTicksNS() - t0, false);
    }
    else if (prog_sync_ok()) { p->sync_try = true; translate_program(p); }
    else run_translate(translate_program, p, build_log() ? prog_code_hash(p, 1) : 0);
    GPS_ZONE(3);
#undef GPS_ZONE
    return p;
}

// ================================================================== vertex formats / fixed function tables
static int attr_kind(uint32_t fmt) {
    switch (fmt) {
    case 0x03: case 0x08: case 0x0B: case 0x0F: case 0x14: case 0x17: case 0x1B: case 0x20: case 0x23:
    case 0x27: case 0x2C: case 0x2F: case 0x3E:
        return 2;
    case 0x04: case 0x09: case 0x0C: case 0x10: case 0x15: case 0x18: case 0x1C: case 0x21: case 0x24:
    case 0x28: case 0x2D: case 0x30:
        return 1;
    default: return 0;
    }
}

static VkFormat vertex_format(uint32_t f) {
    switch (f) {
    case 0x01: return VK_FORMAT_R8_UNORM;
    case 0x02: return VK_FORMAT_R8_SNORM;
    case 0x03: return VK_FORMAT_R8_UINT;
    case 0x04: return VK_FORMAT_R8_SINT;
    case 0x05: return VK_FORMAT_R16_SFLOAT;
    case 0x06: return VK_FORMAT_R16_UNORM;
    case 0x07: return VK_FORMAT_R16_SNORM;
    case 0x08: return VK_FORMAT_R16_UINT;
    case 0x09: return VK_FORMAT_R16_SINT;
    case 0x0A: return VK_FORMAT_R32_SFLOAT;
    case 0x0B: return VK_FORMAT_R32_UINT;
    case 0x0C: return VK_FORMAT_R32_SINT;
    case 0x0D: return VK_FORMAT_R8G8_UNORM;
    case 0x0E: return VK_FORMAT_R8G8_SNORM;
    case 0x0F: return VK_FORMAT_R8G8_UINT;
    case 0x10: return VK_FORMAT_R8G8_SINT;
    case 0x11: return VK_FORMAT_R16G16_SFLOAT;
    case 0x12: return VK_FORMAT_R16G16_UNORM;
    case 0x13: return VK_FORMAT_R16G16_SNORM;
    case 0x14: return VK_FORMAT_R16G16_UINT;
    case 0x15: return VK_FORMAT_R16G16_SINT;
    case 0x16: return VK_FORMAT_R32G32_SFLOAT;
    case 0x17: return VK_FORMAT_R32G32_UINT;
    case 0x18: return VK_FORMAT_R32G32_SINT;
    case 0x19: return VK_FORMAT_R8G8B8_UNORM;
    case 0x1A: return VK_FORMAT_R8G8B8_SNORM;
    case 0x1B: return VK_FORMAT_R8G8B8_UINT;
    case 0x1C: return VK_FORMAT_R8G8B8_SINT;
    case 0x1D: return VK_FORMAT_R16G16B16_SFLOAT;
    case 0x1E: return VK_FORMAT_R16G16B16_UNORM;
    case 0x1F: return VK_FORMAT_R16G16B16_SNORM;
    case 0x20: return VK_FORMAT_R16G16B16_UINT;
    case 0x21: return VK_FORMAT_R16G16B16_SINT;
    case 0x22: return VK_FORMAT_R32G32B32_SFLOAT;
    case 0x23: return VK_FORMAT_R32G32B32_UINT;
    case 0x24: return VK_FORMAT_R32G32B32_SINT;
    case 0x25: return VK_FORMAT_R8G8B8A8_UNORM;
    case 0x26: return VK_FORMAT_R8G8B8A8_SNORM;
    case 0x27: return VK_FORMAT_R8G8B8A8_UINT;
    case 0x28: return VK_FORMAT_R8G8B8A8_SINT;
    case 0x29: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case 0x2A: return VK_FORMAT_R16G16B16A16_UNORM;
    case 0x2B: return VK_FORMAT_R16G16B16A16_SNORM;
    case 0x2C: return VK_FORMAT_R16G16B16A16_UINT;
    case 0x2D: return VK_FORMAT_R16G16B16A16_SINT;
    case 0x2E: return VK_FORMAT_R32G32B32A32_SFLOAT;
    case 0x2F: return VK_FORMAT_R32G32B32A32_UINT;
    case 0x30: return VK_FORMAT_R32G32B32A32_SINT;
    case 0x3D: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case 0x69: return VK_FORMAT_A2B10G10R10_SNORM_PACK32;
    case 0x3F: return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
    case 0x40: return VK_FORMAT_E5B9G9R9_UFLOAT_PACK32;
    // integer-to-float formats (NVN *_UI2F / *_I2F pairs, formats.c sizes): the vertex fetch converts the integer to
    // float = Vulkan USCALED / SSCALED. 0x64 RGBA8_I2F fell to the RGBA32F default before (16-byte reads of a 4-byte
    // attribute: TotK falling leaves / drips drawn as long vertical spikes). No 32-bit scaled formats in Vulkan.
    case 0x51: return VK_FORMAT_R8_USCALED;
    case 0x52: return VK_FORMAT_R8_SSCALED;
    case 0x53: return VK_FORMAT_R16_USCALED;
    case 0x54: return VK_FORMAT_R16_SSCALED;
    case 0x57: return VK_FORMAT_R8G8_USCALED;
    case 0x58: return VK_FORMAT_R8G8_SSCALED;
    case 0x59: return VK_FORMAT_R16G16_USCALED;
    case 0x5A: return VK_FORMAT_R16G16_SSCALED;
    case 0x5D: return VK_FORMAT_R8G8B8_USCALED;
    case 0x5E: return VK_FORMAT_R8G8B8_SSCALED;
    case 0x5F: return VK_FORMAT_R16G16B16_USCALED;
    case 0x60: return VK_FORMAT_R16G16B16_SSCALED;
    case 0x63: return VK_FORMAT_R8G8B8A8_USCALED;
    case 0x64: return VK_FORMAT_R8G8B8A8_SSCALED;
    case 0x65: return VK_FORMAT_R16G16B16A16_USCALED;
    case 0x66: return VK_FORMAT_R16G16B16A16_SSCALED;
    default: { static uint8_t told[256]; if (!told[f & 0xFF]) { told[f & 0xFF] = 1; LOG("[vk] vertex format %#x unknown: read as RGBA32F\n", f); } }
        return VK_FORMAT_R32G32B32A32_SFLOAT;
    }
}

// 3-component 8/16-bit vertex formats aren't universally supported: read 4 components instead.
static VkFormat vertex_format_supported(VkFormat f) {
    static uint8_t known[256];  // 1 ok, 2 not
    uint32_t i = f < 256 ? f : 0;
    if (!known[i]) {
        VkFormatProperties p;
        vkGetPhysicalDeviceFormatProperties(g_vk.phys, f, &p);
        known[i] = (p.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT) ? 1 : 2;
    }
    if (known[i] == 1) return f;
    { static uint8_t told[256]; if (!told[i]++) LOG("[vk] vertex format %d unsupported: read as 4 components\n", f); }
    switch (f) {
    case VK_FORMAT_R8G8B8_UNORM: return VK_FORMAT_R8G8B8A8_UNORM;
    case VK_FORMAT_R8G8B8_SNORM: return VK_FORMAT_R8G8B8A8_SNORM;
    case VK_FORMAT_R8G8B8_UINT: return VK_FORMAT_R8G8B8A8_UINT;
    case VK_FORMAT_R8G8B8_SINT: return VK_FORMAT_R8G8B8A8_SINT;
    case VK_FORMAT_R16G16B16_SFLOAT: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case VK_FORMAT_R16G16B16_UNORM: return VK_FORMAT_R16G16B16A16_UNORM;
    case VK_FORMAT_R16G16B16_SNORM: return VK_FORMAT_R16G16B16A16_SNORM;
    case VK_FORMAT_R16G16B16_UINT: return VK_FORMAT_R16G16B16A16_UINT;
    case VK_FORMAT_R16G16B16_SINT: return VK_FORMAT_R16G16B16A16_SINT;
    case VK_FORMAT_R8G8B8_USCALED: return VK_FORMAT_R8G8B8A8_USCALED;
    case VK_FORMAT_R8G8B8_SSCALED: return VK_FORMAT_R8G8B8A8_SSCALED;
    case VK_FORMAT_R16G16B16_USCALED: return VK_FORMAT_R16G16B16A16_USCALED;
    case VK_FORMAT_R16G16B16_SSCALED: return VK_FORMAT_R16G16B16A16_SSCALED;
    default: return VK_FORMAT_R32G32B32A32_SFLOAT;
    }
}

static VkBlendFactor blend_factor(uint8_t f) {
    if (f >= 0x10 && f <= 0x13) { static uint8_t seen; if (!(seen & (1u << (f - 0x10)))) { seen |= (uint8_t)(1u << (f - 0x10)); LOG("[vk] blend factor %#x (SRC1, dual-source) used: fs %016llx\n", f, (unsigned long long)g_cur_fs_hash); } }
    switch (f) {
    case 1: return VK_BLEND_FACTOR_ZERO;
    case 2: return VK_BLEND_FACTOR_ONE;
    case 3: return VK_BLEND_FACTOR_SRC_COLOR;
    case 4: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case 5: return VK_BLEND_FACTOR_SRC_ALPHA;
    case 6: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case 7: return VK_BLEND_FACTOR_DST_ALPHA;
    case 8: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case 9: return VK_BLEND_FACTOR_DST_COLOR;
    case 10: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case 11: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    // (no dualSrcBlend on the device: the second source falls back to the first; device.c warns once)
    case 0x10: return g_vk.dual_src_blend ? VK_BLEND_FACTOR_SRC1_COLOR : VK_BLEND_FACTOR_SRC_COLOR;
    case 0x11: return g_vk.dual_src_blend ? VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR : VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case 0x12: return g_vk.dual_src_blend ? VK_BLEND_FACTOR_SRC1_ALPHA : VK_BLEND_FACTOR_SRC_ALPHA;
    case 0x13: return g_vk.dual_src_blend ? VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA : VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case 0x61: return VK_BLEND_FACTOR_CONSTANT_COLOR;
    case 0x62: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case 0x63: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
    case 0x64: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    default: {
        static uint8_t warned[256];
        if (!warned[f]++) LOG("[vk] unknown blend factor %#x\n", f);
        return VK_BLEND_FACTOR_ONE;
    }
    }
}

static VkBlendOp blend_op(uint8_t e) {
    switch (e) {
    case 2: return VK_BLEND_OP_SUBTRACT;
    case 3: return VK_BLEND_OP_REVERSE_SUBTRACT;
    case 4: return VK_BLEND_OP_MIN;
    case 5: return VK_BLEND_OP_MAX;
    default: return VK_BLEND_OP_ADD;
    }
}

static VkCompareOp compare_fn(uint8_t f) {
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

static VkStencilOp stencil_op(uint8_t o) {
    switch (o) {
    case 2: return VK_STENCIL_OP_ZERO;
    case 3: return VK_STENCIL_OP_REPLACE;
    case 4: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
    case 5: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
    case 6: return VK_STENCIL_OP_INVERT;
    case 7: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
    case 8: return VK_STENCIL_OP_DECREMENT_AND_WRAP;
    default: return VK_STENCIL_OP_KEEP;
    }
}

// colour target i's NVN format as the pass draws it: the view's format when the view sets one (BC volumes are drawn
// through RGBA16UI views: integer outputs, no blending)
static uint32_t rt_view_format(int i) {
    return st.rt.has_cview[i] && (st.rt.cview[i].setmask & 4) ? st.rt.cview[i].format : st.rt.color[i]->format;
}

static int rt_kind(uint32_t f) {
    switch (f) {
    case 0x03: case 0x08: case 0x0B: case 0x0F: case 0x14: case 0x17: case 0x27: case 0x2C: case 0x2F: case 0x3E: return 2;
    case 0x04: case 0x09: case 0x0C: case 0x10: case 0x15: case 0x18: case 0x28: case 0x2D: case 0x30: return 1;
    default: return 0;
    }
}

// ================================================================== graphics pipelines
// Everything a pipeline bakes in, captured by value so it can be built on a worker thread.
#define ZERO_BINDING 16
typedef struct {
    Prog *prog;
    VkFormat color[8], depth, stencil;
    uint32_t ncolor, samples;
    uint32_t attr_fmt[32], attr_off[32], attr_bind[32];
    uint32_t attr_mask;
    uint32_t stride[17], divisor[17], used_streams;
    uint8_t blend[8][8];
    uint8_t blend_on, a2c, topo_class, poly_fill, depth_clamp;
    uint8_t fbl;   // (g_vk.fbl_layout: the pass's feedback-loop attachment aspects -> the pipeline's feedback-loop flags; 0 otherwise)
    uint32_t write_mask[8];
} PipeKey;

// Colour-format slots past ncolor are not part of the pipeline (VkPipelineRenderingCreateInfo takes ncolor formats) but
// pass_color[] keeps stale formats there from earlier passes: identical pipelines got new keys and were rebuilt (61 % of
// cache/precompile.bin's 7019 pipelines, 2026-10-15; each first-sight build held a frame = the user's micro stutters).
static void pipekey_norm(PipeKey *k) {
    for (uint32_t i = k->ncolor; i < 8; i++) k->color[i] = VK_FORMAT_UNDEFINED;
}
typedef struct { PipeKey k; uint64_t hash; VkPipeline pipe; _Atomic int state, claimed; uint64_t skip_t0, hurry_f; uint32_t nskip; bool linked; } Pipe;  // state: 0 building, 1 ready, 2 failed; claimed: a builder took it; skip_t0/nskip: [pipeskip] (drawing thread); linked: GPL fast link (dynamic depth clamp)
#define PIPE_TAB 32768
static struct { uint64_t key; Pipe *pipe; } pipe_tab[PIPE_TAB];

// 8 bytes a step (PipeKey is ~700 bytes, hashed per draw); in-memory tables only - hmix values are compared with
// stored constants (fragment-shader hashes) and must not change.
static uint64_t hash_words(uint64_t h, const void *p, size_t n) {
    const uint8_t *b = p;
    for (; n >= 8; n -= 8, b += 8) { uint64_t w; memcpy(&w, b, 8); h = (h ^ w) * 0x9E3779B97F4A7C15ull; h ^= h >> 29; }
    for (; n; n--, b++) { h = (h ^ *b) * 0x100000001B3ull; }
    return h ^ (h >> 32);
}
// hash_fast: four independent lanes of 8 bytes (32 a step), combined and finished with fmix64. hash_words is one
// multiply's latency per 8 bytes (~88 dependent steps for a PipeKey); the per-draw pipeline-key hash on pk_memo misses and
// texture_info_'s view hash were 0.10-0.11 ms a frame in Kakariko. Only for values that live in memory (pipe_tab keys at
// every place they are computed, the view memo): hash_words also keys persisted things (program code hashes,
// precompile.bin records) and must not change. pipe_tab compares only this 64-bit value, so the lanes are seeded apart and
// the result fully mixed.
static inline uint64_t hw_rotl(uint64_t x, int r) { return (x << r) | (x >> (64 - r)); }
static uint64_t hash_fast(uint64_t h, const void *p, size_t n) {
    const uint8_t *b = p;
    uint64_t a0 = h, a1 = h ^ 0x632BE59BD9B4E019ull, a2 = h ^ 0x8CB92BA72F3D8DD7ull, a3 = h ^ 0x9AE16A3B2F90404Full;
    for (; n >= 32; n -= 32, b += 32) {
        uint64_t w[4];
        memcpy(w, b, 32);
        a0 = (a0 ^ w[0]) * 0x9E3779B97F4A7C15ull; a0 ^= a0 >> 29;
        a1 = (a1 ^ w[1]) * 0xC2B2AE3D27D4EB4Full; a1 ^= a1 >> 29;
        a2 = (a2 ^ w[2]) * 0x165667B19E3779F9ull; a2 ^= a2 >> 29;
        a3 = (a3 ^ w[3]) * 0xFF51AFD7ED558CCDull; a3 ^= a3 >> 29;
    }
    h = a0 ^ hw_rotl(a1, 17) ^ hw_rotl(a2, 31) ^ hw_rotl(a3, 47);
    for (; n >= 8; n -= 8, b += 8) { uint64_t w; memcpy(&w, b, 8); h = (h ^ w) * 0x9E3779B97F4A7C15ull; h ^= h >> 29; }
    for (; n; n--, b++) { h = (h ^ *b) * 0x100000001B3ull; }
    h ^= h >> 33; h *= 0xFF51AFD7ED558CCDull; h ^= h >> 33; h *= 0xC4CEB9FE1A85EC53ull; h ^= h >> 33;   // (fmix64)
    return h;
}

// LARECOMP_HASH_FAST=0: hash_words at those places instead (A/B); decided once, so every site agrees for the whole run
static uint64_t hash_mem(uint64_t h, const void *p, size_t n) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("LARECOMP_HASH_FAST"); on = !(e && *e == '0'); }
    return on ? hash_fast(h, p, n) : hash_words(h, p, n);
}
static uint64_t hmix(uint64_t h, const void *p, size_t n) {
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 0x100000001B3ull; }
    return h;
}

static VkPrimitiveTopology topo_class_base(uint8_t c) {
    return c == 0 ? VK_PRIMITIVE_TOPOLOGY_POINT_LIST : c == 1 ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
}

static void build_pipeline_body(Pipe *pp);
static void build_pipeline(void *arg) {  // (worker job; a draw may have claimed and built it inline already)
    Pipe *pp = arg;
    int unclaimed = 0;
    if (atomic_compare_exchange_strong(&pp->claimed, &unclaimed, 1)) build_pipeline_body(pp);
}
// ---- GPL link (see gpl_program_libs): vertex-input and fragment-output libraries cached by their state
static bool gpl_linkable(const PipeKey *k) {
    if (k->fbl) return false;   // (feedback-loop pipelines are built whole: the flag on the linked libraries is not used)
    return g_vk.gpl && k->prog && atomic_load(&k->prog->lib_state) == 2 && k->poly_fill && (!k->depth_clamp || vk_dyn_clamp) &&
           k->samples == VK_SAMPLE_COUNT_1_BIT && !k->a2c;
}
typedef struct {
    uint32_t attr_fmt[32], attr_off[32], attr_bind[32], attr_mask, stride[17], divisor[17], used_streams, topo_class;
} GplVi;
typedef struct {
    VkFormat color[8], depth, stencil;
    uint32_t ncolor, write_mask[8];
    uint8_t blend[8][8], blend_on;
} GplFo;
#define GPL_TAB 8192
static struct { uint64_t key; VkPipeline lib; } gpl_vi_tab[GPL_TAB], gpl_fo_tab[GPL_TAB];
static pthread_mutex_t gpl_mu = PTHREAD_MUTEX_INITIALIZER;
static VkPipeline gpl_cached(uint64_t h, bool fo, VkPipeline (*make)(const PipeKey *), const PipeKey *k) {
    if (!h) h = 1;
    __typeof__(gpl_vi_tab[0]) *t = fo ? gpl_fo_tab : gpl_vi_tab;
    pthread_mutex_lock(&gpl_mu);
    uint32_t i = (uint32_t)(h ^ (h >> 29)) & (GPL_TAB - 1);
    int probes = 0;
    while (t[i].key && t[i].key != h && probes++ < GPL_TAB) i = (i + 1) & (GPL_TAB - 1);
    VkPipeline r = VK_NULL_HANDLE;
    if (t[i].key == h) r = t[i].lib;
    else if (!t[i].key) { r = make(k); if (r) { t[i].key = h; t[i].lib = r; } }   // (cheap: no shaders; made under the lock)
    pthread_mutex_unlock(&gpl_mu);
    return r;
}
static VkPipeline gpl_make_vi(const PipeKey *k) {
    VkVertexInputAttributeDescription va[32];
    VkVertexInputBindingDescription vb[17];
    VkVertexInputBindingDivisorDescriptionEXT vd[17];
    uint32_t na = 0, nb = 0, nd = 0;
    for (int i = 0; i < 32; i++)
        if (k->attr_mask & (1u << i)) va[na++] = (VkVertexInputAttributeDescription){(uint32_t)i, k->attr_bind[i], k->attr_fmt[i], k->attr_off[i]};
    for (uint32_t s = 0; s < 17; s++) {
        if (!(k->used_streams & (1u << s))) continue;
        bool inst = k->divisor[s] != 0;
        vb[nb++] = (VkVertexInputBindingDescription){s, k->stride[s], inst ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX};
        if (inst && k->divisor[s] > 1 && g_vk.divisor) vd[nd++] = (VkVertexInputBindingDivisorDescriptionEXT){s, k->divisor[s]};
    }
    VkPipelineVertexInputDivisorStateCreateInfoEXT dvi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO_EXT,
                                                          .vertexBindingDivisorCount = nd, .pVertexBindingDivisors = vd};
    VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, .pNext = nd ? &dvi : NULL,
                                               .vertexBindingDescriptionCount = nb, .pVertexBindingDescriptions = vb,
                                               .vertexAttributeDescriptionCount = na, .pVertexAttributeDescriptions = va};
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                 .topology = topo_class_base(k->topo_class)};
    VkPipelineDynamicStateCreateInfo dy = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = NDYN, .pDynamicStates = DYN};
    VkGraphicsPipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pVertexInputState = &vi,
                                       .pInputAssemblyState = &ia, .pDynamicState = &dy};
    return gpl_create(&pi, VK_GRAPHICS_PIPELINE_LIBRARY_VERTEX_INPUT_INTERFACE_BIT_EXT);
}
static VkPipeline gpl_make_fo(const PipeKey *k) {
    VkPipelineColorBlendAttachmentState cba[8] = {0};
    for (uint32_t i = 0; i < k->ncolor; i++) {
        cba[i].colorWriteMask = k->write_mask[i];
        if ((k->blend_on >> i) & 1) {
            const uint8_t *b = k->blend[i];
            cba[i].blendEnable = VK_TRUE;
            cba[i].srcColorBlendFactor = blend_factor(b[1]);
            cba[i].dstColorBlendFactor = blend_factor(b[2]);
            cba[i].srcAlphaBlendFactor = blend_factor(b[3]);
            cba[i].dstAlphaBlendFactor = blend_factor(b[4]);
            cba[i].colorBlendOp = blend_op(b[5]);
            cba[i].alphaBlendOp = blend_op(b[6]);
        }
    }
    VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = k->ncolor,
                                              .pAttachments = cba};
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                               .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkPipelineDynamicStateCreateInfo dy = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = NDYN, .pDynamicStates = DYN};
    VkPipelineRenderingCreateInfo ri = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = k->ncolor,
                                        .pColorAttachmentFormats = k->color, .depthAttachmentFormat = k->depth,
                                        .stencilAttachmentFormat = k->stencil};
    VkGraphicsPipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &ri, .pColorBlendState = &cb,
                                       .pMultisampleState = &ms, .pDynamicState = &dy};
    return gpl_create(&pi, VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_OUTPUT_INTERFACE_BIT_EXT);
}
// Depth-only passes (Depths 4K: the shadow cascades, 2.94 of 10.42 ms of GPU): with no colour target, a fragment shader
// without discard, depth, gmem or image writes changes nothing, but the fast link (no link-time optimisation) cannot drop
// it: its fragment library was compiled before the output interface was known, so it ran for every covered texel.
// Such pipelines link a shader-less fragment library (monolithic builds: the VS alone). The program and ncolor are both in
// the pipeline key, so no key changes. dep.cap replay on the real driver: the links succeed, no validation message.
// LARECOMP_FS_ELIDE=0: off.
static bool fs_elidable(const PipeKey *k) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("LARECOMP_FS_ELIDE"); on = !(e && *e == '0'); }
    const ShaderInfo *fi = k->prog ? k->prog->fi : NULL;
    if (!on || k->ncolor || k->a2c || !fi || fi->uses_discard || fi->writes_depth || fi->writes_gmem) return false;
    for (int i = 0; i < fi->ntex && i < 64; i++) if (fi->tex[i].is_image) return false;
    return true;
}
static _Atomic int stat_fs_elided;
static void fs_elided_note(const PipeKey *k, const char *how) {
    int n = atomic_fetch_add(&stat_fs_elided, 1) + 1;
    if (n <= 4 || (n & (n - 1)) == 0)
        LOG("[vk] depth-only pipeline %d without its fragment shader (fs %016llx, %s)\n", n,
            (unsigned long long)prog_code_hash(k->prog, 1), how);
}
static VkPipeline gpl_frag_none(Prog *p) {   // (any thread: the first maker wins, a loser destroys its copy)
    VkPipeline l = __atomic_load_n(&p->lib_frag_none, __ATOMIC_ACQUIRE);
    if (l) return l;
    VkPipelineRenderingCreateInfo ri = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    VkPipelineDynamicStateCreateInfo dy = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                           .dynamicStateCount = NDYN, .pDynamicStates = DYN};
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                               .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkPipelineDepthStencilStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkGraphicsPipelineCreateInfo b = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &ri,
                                      .pMultisampleState = &ms, .pDepthStencilState = &ds, .pDynamicState = &dy,
                                      .layout = p->lib_layout[1] ? p->lib_layout[1] : p->layout};
    VkPipeline mine = gpl_create(&b, VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT);
    if (!mine) return VK_NULL_HANDLE;
    VkPipeline expect = VK_NULL_HANDLE;
    if (__atomic_compare_exchange_n(&p->lib_frag_none, &expect, mine, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return mine;
    vkDestroyPipeline(g_vk.dev, mine, NULL);
    return expect;
}
static bool gpl_link(Pipe *pp) {
    const PipeKey *k = &pp->k;
    VkPipeline frag = k->prog->lib_frag;
    bool elide = fs_elidable(k);
    if (elide) {
        VkPipeline none = gpl_frag_none(k->prog);
        if (none) frag = none;
        else elide = false;
    }
    GplVi v;
    memset(&v, 0, sizeof v);
    for (int i = 0; i < 32; i++)
        if (k->attr_mask & (1u << i)) { v.attr_fmt[i] = k->attr_fmt[i]; v.attr_off[i] = k->attr_off[i]; v.attr_bind[i] = k->attr_bind[i]; }
    for (int s = 0; s < 17; s++)
        if (k->used_streams & (1u << s)) { v.stride[s] = k->stride[s]; v.divisor[s] = k->divisor[s]; }
    v.attr_mask = k->attr_mask;
    v.used_streams = k->used_streams;
    v.topo_class = k->topo_class;
    GplFo f;
    memset(&f, 0, sizeof f);
    f.ncolor = k->ncolor;
    for (uint32_t i = 0; i < k->ncolor && i < 8; i++) {
        f.color[i] = k->color[i];
        f.write_mask[i] = k->write_mask[i];
        if ((k->blend_on >> i) & 1) memcpy(f.blend[i], k->blend[i], 8);
    }
    f.blend_on = k->blend_on;
    f.depth = k->depth;
    f.stencil = k->stencil;
    VkPipeline libs[4] = {gpl_cached(hash_words(0x243f6a8885a308d3ull, &v, sizeof v), false, gpl_make_vi, k), k->prog->lib_pre,
                          frag, gpl_cached(hash_words(0x13198a2e03707344ull, &f, sizeof f), true, gpl_make_fo, k)};
    if (!libs[0] || !libs[3]) return false;
    VkPipelineLibraryCreateInfoKHR lci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR, .libraryCount = 4, .pLibraries = libs};
    VkGraphicsPipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &lci, .layout = k->prog->layout};
    uint64_t t0 = SDL_GetTicksNS();
    VkResult r = vkCreateGraphicsPipelines(g_vk.dev, VK_NULL_HANDLE, 1, &pi, NULL, &pp->pipe);   // fast link (no LTO)
    atomic_fetch_add(&stat_gpl_links, 1);
    atomic_fetch_add(&stat_gpl_link_ns, SDL_GetTicksNS() - t0);
    if (r != VK_SUCCESS) {
        static int n;
        if (n++ < 10) LOG("[vk] pipeline link failed: %d (monolithic build instead)%s\n", r, elide ? " (shader-less fragment library)" : "");
        return false;
    }
    if (elide) fs_elided_note(k, "linked");
    pp->linked = true;
    atomic_store(&pp->state, 1);
    return true;
}

static void build_pipeline_body(Pipe *pp) {
    PipeKey *k = &pp->k;
    if (gpl_linkable(k) && gpl_link(pp)) return;
    VkShaderModule mod[2] = {k->prog->vs, k->prog->fs}, tmp[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    if (k->prog->modules_dropped) {   // (compile_program dropped them after building the libraries)
        for (int s = 0; s < 2; s++) {
            size_t nw = 0;
            uint32_t *spv = vk_spirv_load(k->prog->spv_key[s], &nw);
            if (spv) { tmp[s] = vk_module(spv, nw); free(spv); }
            mod[s] = tmp[s];
        }
        if (!mod[0] || !mod[1]) {
            LOG("[vk] pipeline: program modules not in the SPIR-V cache any more (%016llx / %016llx)\n",
                (unsigned long long)k->prog->spv_key[0], (unsigned long long)k->prog->spv_key[1]);
            for (int s = 0; s < 2; s++) if (tmp[s]) vkDestroyShaderModule(g_vk.dev, tmp[s], NULL);
            atomic_store(&pp->state, 2);
            return;
        }
    }
    VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = mod[0], .pName = "main",
         .pNext = sg_pin(k->prog->vi, VK_SHADER_STAGE_VERTEX_BIT)},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = mod[1], .pName = "main",
         .pNext = sg_pin(k->prog->fi, VK_SHADER_STAGE_FRAGMENT_BIT)}};
    VkVertexInputAttributeDescription va[32];
    VkVertexInputBindingDescription vb[17];
    VkVertexInputBindingDivisorDescriptionEXT vd[17];
    uint32_t na = 0, nb = 0, nd = 0;
    for (int i = 0; i < 32; i++)
        if (k->attr_mask & (1u << i)) va[na++] = (VkVertexInputAttributeDescription){(uint32_t)i, k->attr_bind[i], k->attr_fmt[i], k->attr_off[i]};
    for (uint32_t s = 0; s < 17; s++) {
        if (!(k->used_streams & (1u << s))) continue;
        bool inst = k->divisor[s] != 0;
        vb[nb++] = (VkVertexInputBindingDescription){s, k->stride[s], inst ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX};
        if (inst && k->divisor[s] > 1 && g_vk.divisor) vd[nd++] = (VkVertexInputBindingDivisorDescriptionEXT){s, k->divisor[s]};
    }
    VkPipelineVertexInputDivisorStateCreateInfoEXT dvi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO_EXT,
                                                          .vertexBindingDivisorCount = nd, .pVertexBindingDivisors = vd};
    VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, .pNext = nd ? &dvi : NULL,
                                               .vertexBindingDescriptionCount = nb, .pVertexBindingDescriptions = vb,
                                               .vertexAttributeDescriptionCount = na, .pVertexAttributeDescriptions = va};
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                 .topology = topo_class_base(k->topo_class)};
    VkPipelineViewportStateCreateInfo vs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .depthClampEnable = k->depth_clamp,
                                                 .polygonMode = k->poly_fill ? VK_POLYGON_MODE_FILL : VK_POLYGON_MODE_LINE, .lineWidth = 1};
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                               .rasterizationSamples = (VkSampleCountFlagBits)k->samples, .alphaToCoverageEnable = k->a2c};
    VkPipelineDepthStencilStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState cba[8] = {0};
    for (uint32_t i = 0; i < k->ncolor; i++) {
        cba[i].colorWriteMask = k->write_mask[i];
        if ((k->blend_on >> i) & 1) {
            const uint8_t *b = k->blend[i];
            cba[i].blendEnable = VK_TRUE;
            cba[i].srcColorBlendFactor = blend_factor(b[1]);
            cba[i].dstColorBlendFactor = blend_factor(b[2]);
            cba[i].srcAlphaBlendFactor = blend_factor(b[3]);
            cba[i].dstAlphaBlendFactor = blend_factor(b[4]);
            cba[i].colorBlendOp = blend_op(b[5]);
            cba[i].alphaBlendOp = blend_op(b[6]);
        }
    }
    VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                              .attachmentCount = k->ncolor, .pAttachments = cba};
    VkPipelineDynamicStateCreateInfo dy = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                           .dynamicStateCount = NDYN, .pDynamicStates = DYN};
    VkPipelineRenderingCreateInfo ri = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = k->ncolor,
                                        .pColorAttachmentFormats = k->color, .depthAttachmentFormat = k->depth,
                                        .stencilAttachmentFormat = k->stencil};
    const bool elide = fs_elidable(k);   // (the VS alone)
    VkGraphicsPipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &ri, .stageCount = elide ? 1 : 2,
                                       .pStages = stages, .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vs,
                                       .pRasterizationState = &rs, .pMultisampleState = &ms, .pDepthStencilState = &ds,
                                       .pColorBlendState = &cb, .pDynamicState = &dy, .layout = k->prog->layout};
    if (k->fbl & VK_IMAGE_ASPECT_COLOR_BIT) pi.flags |= VK_PIPELINE_CREATE_COLOR_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT;   // (g_vk.fbl_layout)
    VkResult r = vkCreateGraphicsPipelines(g_vk.dev, g_vk.pcache, 1, &pi, NULL, &pp->pipe);
    for (int s = 0; s < 2; s++) if (tmp[s]) vkDestroyShaderModule(g_vk.dev, tmp[s], NULL);
    if (r != VK_SUCCESS) {
        static int n;
        if (n++ < 10) LOG("[vk] pipeline creation failed: %d%s\n", r, elide ? " (without its fragment shader)" : "");
    } else if (elide) fs_elided_note(k, "monolithic");
    atomic_fetch_add(&stat_compiles, 1);
    atomic_store(&pp->state, r == VK_SUCCESS ? 1 : 2);
}

// ================================================================== shader precompile
// (user 2026-10-15: "make a thing to precompile all shaders for a smooth experience")
// Every graphics program the game uses (its Maxwell code, saved by content hash as cache/code/<hash>.bin) and every
// pipeline state it draws with are appended to cache/precompile.bin. At the next launch, background threads translate
// those programs and build their pipelines (from the warm VkPipelineCache) before the game asks for them; a draw finds
// them by content (code hashes + shader keys), not by guest address. Pipelines never seen before still build on first
// sight (the frame is held). TOTK_PRECOMPILE=0: off (no recording, no precompile). Progress: vk_precompile_progress
// (title bar + banner), "[precompile]" log lines. Compute programs too (PC_COMP, since 2026-10-17).
#ifdef _WIN32
__declspec(dllimport) void *__stdcall GetCurrentThread(void);
__declspec(dllimport) int __stdcall SetThreadPriority(void *thread, int priority);
#endif
enum { PC_MAGIC = 0x31435054u, PC_HEADER = 1, PC_PROG = 2, PC_PIPE = 3, PC_PROG2 = 4, PC_COMP = 5 };
typedef struct { uint32_t magic, kind, size, pad; } PcHdr;   // then `size` payload bytes
typedef struct { uint64_t heap_base, gdev_size, prog_size, pipe_size; } PcInfo;
typedef struct { uint64_t ckey, code_hash[2], cmax[2]; ShaderKey vkey, fkey; } PcProg;
typedef struct { uint64_t ckey; PipeKey k; } PcPipe;          // k.prog = NULL
// PC_PROG2: + each stage's compiler constants size; the bytes are in cache/code/<code hash>.c1. Without them
// translate.c cannot read BRX jump tables and makes every instruction a switch case: Deck d40, the cloud VS
// 7f..3f1e00 precompiled from a PC_PROG record had 2534 cases (34 when the game built it) = 5.5 ms per cloud draw.
// Programs known only from PC_PROG records are recorded again as PC_PROG2 when the game uses them.
typedef struct { PcProg p; uint64_t csize[2]; } PcProg2;
#define PC_SEEN_C1 0x9e3779b97f4a7c15ull   // (pc_seen key of a PC_PROG2 record: ckey ^ this)
// PC_COMP: a compute program (code in cache/code/<hash>.bin +.c1, workgroup size). The user's PC session of 10-15 held
// frames 20 times for nothing but a compute shader's first dispatch: compute was never recorded.
typedef struct { uint64_t ckey, code_hash, cmax, csize; uint32_t local[3], pad; } PcComp;
#define PC_SEEN_CS 0xc0335a3e5ull          // (pc_seen key of a PC_COMP record: ckey ^ this)
static PcComp *pc_comps;
static int pc_ncomps;
static _Atomic int pc_next_comp, pc_comps_done;
static void pc_comp_build(const PcComp *r);   // (compute section)

static pthread_mutex_t pc_mu = PTHREAD_MUTEX_INITIALIZER;
#define PC_CTAB 16384
static struct { uint64_t key; Prog *prog; } pc_ctab[PC_CTAB];   // content key -> program (pc_mu)
#define PC_PTAB 65536
static struct { uint64_t key; Pipe *pipe; } pc_ptab[PC_PTAB];   // draw hash -> precompiled pipeline (pc_mu)
#define PC_SEEN 131072
static uint64_t pc_seen[PC_SEEN];   // recorded program content keys / pipeline recipe keys (game thread after pc_start)
static int pc_nseen;
static FILE *pc_out;
static PcProg2 *pc_progs;   // (PC_PROG records: csize 0)
static PcPipe *pc_pipes;
static int pc_nprogs, pc_npipes;
static _Atomic int pc_next_prog, pc_progs_done, pc_next_pipe, pc_pipes_done, pc_running;
static int pc_nthreads, pc_nthreads_normal;   // (the cold pass's extra workers: widx >= pc_nthreads_normal)
static _Atomic int pc_cold_running;            // the first-start pass is building (pc_cold_decide, below)
static int pc_cold;
// The first-start pass's workers translate without translate_lock (TOTK_PRECOMPILE_UNLOCKED=0: with it: the lock was
// 4.16 of each program's ~20 ms on 7 workers, om_cold8). The translator keeps its state per thread (MAXWELL_TLS, the
// RSC2 / RSC2L rings) and translate_program's option caches are read once (tp_env); vk_translate_stress (compilebench
// --translate-stress) compares every recorded program's translation on N unlocked threads with one locked thread. Set in
// pc_cold_decide, before the workers start
static bool pc_unlocked;
static void pc_cold_done(void);
static uint64_t pc_t0;

static bool pc_on(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("TOTK_PRECOMPILE"); on = !(e && !strcmp(e, "0")); }
    return on;
}
static bool pc_seen_add(uint64_t k) {   // true if new
    if (!k) k = 1;
    uint32_t i = (uint32_t)(k ^ (k >> 31)) & (PC_SEEN - 1);
    while (pc_seen[i] && pc_seen[i] != k) i = (i + 1) & (PC_SEEN - 1);
    if (pc_seen[i]) return false;
    if (pc_nseen > PC_SEEN * 3 / 4) return false;   // (full: stop recording)
    pc_seen[i] = k;
    pc_nseen++;
    return true;
}
static uint64_t pc_info_heap(void) { return HEAP_BASE; }

// the whole range is committed, readable host memory (guest heap, alias regions)
static bool pc_readable(uint64_t a, uint64_t n) {
    uint64_t lo = HEAP_BASE, hi = HEAP_BASE + kernel_heap_size();
    if (lo && a >= lo && a + n <= hi) return true;
#ifdef _WIN32
    // ReadProcessMemory probe, cost ~ n like the POSIX one. VirtualQuery walked the whole committed region to compute
    // RegionSize: 3.7 ms per GB, 12 ms per GB with MEM_WRITE_WATCH (scratchpad vq.c / rpm.c 2026-10-18: 2 GB 24.5 ms vs
    // 0.011 ms for a 64 KB probe) = ~45 ms per new program on the user's PC, the open-world hitches every ~2 s.
    char probe[8192];   // (windows.h comes in through pthread.h)
    while (n) {
        SIZE_T k = n < sizeof probe ? (SIZE_T)n : sizeof probe, got = 0;
        if (!ReadProcessMemory(GetCurrentProcess(), (LPCVOID)(uintptr_t)a, probe, k, &got) || got != k) return false;
        a += k;
        n -= k;
    }
    return true;
#else
    extern int rt_posix_readable(unsigned long long a, unsigned long long n);   // runtime/linux/plat_posix.cpp (write(2) probe into a pipe: EFAULT, not a signal)
    return rt_posix_readable(a, n) != 0;
#endif
}
static _Atomic int pc_rejects;

static uint64_t pc_ckey(const uint64_t code_hash[2], const ShaderKey *vkey, const ShaderKey *fkey);
// content key: code bytes of both stages (committed host memory only) + every shader key field
static uint64_t pc_prog_key(const NvnProgram *np, const ShaderKey *vkey, const ShaderKey *fkey, uint64_t code_hash[2], uint64_t cmax[2]) {
    for (int i = 0; i < 2; i++) {
        uint64_t a = np->stages[i].code_addr;
        cmax[i] = np->stages[i].const_offset ? np->stages[i].const_offset : 0x10000;
        if (cmax[i] > 0x100000 || !pc_readable(a, cmax[i])) {
            if (pc_rejects++ < 8) LOG("[precompile] not recorded: stage %d code %llx size %llx (heap %llx +%llx)\n", i,
                                      (unsigned long long)a, (unsigned long long)cmax[i], (unsigned long long)HEAP_BASE, (unsigned long long)kernel_heap_size());
            return 0;
        }
        code_hash[i] = hash_words(0x6a09e667f3bcc908ull ^ cmax[i], (const void *)(uintptr_t)a, (size_t)cmax[i]);
    }
    return pc_ckey(code_hash, vkey, fkey);
}
static uint64_t pc_ckey(const uint64_t code_hash[2], const ShaderKey *vkey, const ShaderKey *fkey) {
    const ShaderKey *ks[2] = {vkey, fkey};
    uint64_t h = hash_words(0x510e527fade682d1ull, code_hash, 16);
    for (int i = 0; i < 2; i++) {
        h = hash_words(h, ks[i]->attr_kind, 32);
        h = hash_words(h, ks[i]->rt_kind, 8);
        uint32_t f[4] = {ks[i]->depth_minus_one, ks[i]->flip_y, i == 0 ? ks[i]->vs_out_mask : 0, ks[i]->glsl};
        h = hash_words(h, f, 16);
    }
    return h ? h : 1;
}
static Prog *pc_claim_prog(uint64_t ckey, Prog *fresh) {   // the program already under this key, or `fresh` (now registered)
    pthread_mutex_lock(&pc_mu);
    uint32_t i = (uint32_t)(ckey ^ (ckey >> 29)) & (PC_CTAB - 1);
    int probes = 0;
    while (pc_ctab[i].key && pc_ctab[i].key != ckey && probes++ < PC_CTAB) i = (i + 1) & (PC_CTAB - 1);
    Prog *r = fresh;
    if (pc_ctab[i].key == ckey) r = pc_ctab[i].prog;
    else if (!pc_ctab[i].key) { pc_ctab[i].key = ckey; pc_ctab[i].prog = fresh; }
    pthread_mutex_unlock(&pc_mu);
    return r;
}
static Prog *pc_find_prog(uint64_t ckey) {
    pthread_mutex_lock(&pc_mu);
    uint32_t i = (uint32_t)(ckey ^ (ckey >> 29)) & (PC_CTAB - 1);
    int probes = 0;
    while (pc_ctab[i].key && pc_ctab[i].key != ckey && probes++ < PC_CTAB) i = (i + 1) & (PC_CTAB - 1);
    Prog *r = pc_ctab[i].key == ckey ? pc_ctab[i].prog : NULL;
    pthread_mutex_unlock(&pc_mu);
    return r;
}
static Pipe *pc_take_pipe(uint64_t ph) {
    if (!pc_on()) return NULL;
    pthread_mutex_lock(&pc_mu);
    uint32_t i = (uint32_t)(ph ^ (ph >> 29)) & (PC_PTAB - 1);
    int probes = 0;
    while (pc_ptab[i].key && pc_ptab[i].key != ph && probes++ < PC_PTAB) i = (i + 1) & (PC_PTAB - 1);
    Pipe *r = pc_ptab[i].key == ph ? pc_ptab[i].pipe : NULL;
    pthread_mutex_unlock(&pc_mu);
    return r;
}
static void pc_put_pipe(uint64_t ph, Pipe *pp) {
    pthread_mutex_lock(&pc_mu);
    uint32_t i = (uint32_t)(ph ^ (ph >> 29)) & (PC_PTAB - 1);
    int probes = 0;
    while (pc_ptab[i].key && pc_ptab[i].key != ph && probes++ < PC_PTAB) i = (i + 1) & (PC_PTAB - 1);
    if (!pc_ptab[i].key) { pc_ptab[i].key = ph; pc_ptab[i].pipe = pp; }
    pthread_mutex_unlock(&pc_mu);
}

// Precompile records go through one writer thread (pc_record_prog's probes and creates of cache/code files cost 2.2-2.4
// ms per new program on the render thread, up to 8 opens / creates each, and a fight start brings 50-111 new programs a
// frame). The recording thread copies the guest bytes into the job and queues it; the writer probes / writes the code
// and constants files and appends the record to precompile.bin, all in queue order (every pc_write goes through the
// queue, so the file's order is the recording order). A record still queued when the process ends is lost, as the
// unflushed tail of a torn write was.
typedef struct PcJob {
    struct PcJob *next;
    uint32_t kind, size;
    uint64_t hash[2];             // code / constants file names (code_hash)
    uint8_t *code[2], *c1[2];     // copies; NULL = no file
    uint64_t ncode[2], nc1[2];
    int32_t c1_at[2];             // byte offset in payload of the csize field set when that constants file is there
    uint8_t payload[];
} PcJob;
static PcJob *pcw_head, *pcw_tail;
static pthread_mutex_t pcw_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t pcw_cv = PTHREAD_COND_INITIALIZER;
static void pc_write_now(uint32_t kind, const void *payload, uint32_t size);
static void pc_writer_free(PcJob *j);
static bool pc_file_put(const char *ext, uint64_t hash, const uint8_t *b, uint64_t n, bool same_size) {
    char path[64];   // (an existing code file is kept as it is; a constants file only if its size matches)
    snprintf(path, sizeof path, "cache/code/%016llx.%s", (unsigned long long)hash, ext);
    FILE *f = fopen(path, "rb");
    if (f) {
        long have = 0;
        if (same_size) { fseek(f, 0, SEEK_END); have = ftell(f); }
        fclose(f);
        if (!same_size || (uint64_t)have == n) return true;
    }
    if (!(f = fopen(path, "wb"))) return false;
    bool ok = fwrite(b, 1, (size_t)n, f) == n;
    fclose(f);
    if (!ok) remove(path);
    return ok;
}
static void pc_writer_one(PcJob *j) {   // (the writer thread; the queueing thread only if the writer could not start)
    static bool dirs;
    if (!dirs) { dirs = true; mkdir("cache", 0755); mkdir("cache/code", 0755); }
    bool ok = true;   // (a code file that cannot be written drops the record, as before; a constants file only its csize)
    for (int i = 0; i < 2 && ok; i++) if (j->code[i]) ok = pc_file_put("bin", j->hash[i], j->code[i], j->ncode[i], false);
    for (int i = 0; i < 2 && ok; i++)
        if (j->c1[i] && pc_file_put("c1", j->hash[i], j->c1[i], j->nc1[i], true)) memcpy(j->payload + j->c1_at[i], &j->nc1[i], 8);
    if (ok) pc_write_now(j->kind, j->payload, j->size);
    pc_writer_free(j);
}
static void *pc_writer(void *arg) {
    (void)arg;
    cpu_place_thread(CPU_HELPER);   // (nothing waits on it: the E-cores / the other CCD, cpu_place.h)
    SetThreadPriority(GetCurrentThread(), -1);   // (THREAD_PRIORITY_BELOW_NORMAL: file I/O, nothing waits on it)
    for (;;) {
        pthread_mutex_lock(&pcw_mu);
        while (!pcw_head) pthread_cond_wait(&pcw_cv, &pcw_mu);
        PcJob *j = pcw_head;
        if (!(pcw_head = j->next)) pcw_tail = NULL;
        pthread_mutex_unlock(&pcw_mu);
        pc_writer_one(j);
    }
    return NULL;
}
static PcJob *pc_job_new(uint32_t kind, const void *payload, uint32_t size) {
    PcJob *j = calloc(1, sizeof *j + size);
    if (!j) return NULL;
    j->kind = kind; j->size = size; j->c1_at[0] = j->c1_at[1] = -1;
    memcpy(j->payload, payload, size);
    return j;
}
static bool pc_job_copy(uint8_t **dst, uint64_t *n, uint64_t addr, uint64_t size) {   // (guest bytes, now)
    if (!(*dst = malloc((size_t)size))) return false;
    memcpy(*dst, (const void *)(uintptr_t)addr, (size_t)size);
    *n = size;
    return true;
}
static void pc_writer_free(PcJob *j) {
    for (int i = 0; i < 2; i++) { free(j->code[i]); free(j->c1[i]); }
    free(j);
}
static void pc_job_queue(PcJob *j) {
    static bool started;
    pthread_mutex_lock(&pcw_mu);
    if (!started) { pthread_t t; started = pthread_create(&t, NULL, pc_writer, NULL) == 0; }
    if (!started) {   // (no thread: written here, as before; under the lock, since pc_write comes from several threads)
        pc_writer_one(j);
        pthread_mutex_unlock(&pcw_mu);
        return;
    }
    j->next = NULL;
    if (pcw_tail) pcw_tail->next = j; else pcw_head = j;
    pcw_tail = j;
    pthread_cond_signal(&pcw_cv);
    pthread_mutex_unlock(&pcw_mu);
}
static void pc_write(uint32_t kind, const void *payload, uint32_t size) {
    PcJob *j = pc_job_new(kind, payload, size);
    if (j) pc_job_queue(j);
}
static void pc_write_now(uint32_t kind, const void *payload, uint32_t size) {   // (writer thread)
    if (!pc_out) {
        mkdir("cache", 0755);
        pc_out = fopen("cache/precompile.bin", "ab");
        if (!pc_out) return;
        if (ftell(pc_out) == 0) {
            PcInfo in = {pc_info_heap(), gdev_window_size(), sizeof(PcProg), sizeof(PipeKey)};
            PcHdr h = {PC_MAGIC, PC_HEADER, sizeof in, 0};
            fwrite(&h, sizeof h, 1, pc_out);
            fwrite(&in, sizeof in, 1, pc_out);
        }
    }
    PcHdr h = {PC_MAGIC, kind, size, 0};
    fwrite(&h, sizeof h, 1, pc_out);
    fwrite(payload, size, 1, pc_out);
    fflush(pc_out);   // (the process ends with TerminateProcess: no buffered tail)
}
static void pc_record_prog(const Prog *p, const uint64_t code_hash[2]) {
    if (!p->ckey || !pc_seen_add(p->ckey ^ PC_SEEN_C1)) return;
    pc_seen_add(p->ckey);
    PcProg2 r;
    memset(&r, 0, sizeof r);   // (csize[i] is set by the writer once that constants file is there)
    r.p.ckey = p->ckey;
    for (int i = 0; i < 2; i++) { r.p.code_hash[i] = code_hash[i]; r.p.cmax[i] = p->cmax[i]; }
    r.p.vkey = p->vkey;
    r.p.fkey = p->fkey;
    PcJob *j = pc_job_new(PC_PROG2, &r, sizeof r);
    if (!j) return;
    for (int i = 0; i < 2; i++) {
        j->hash[i] = code_hash[i];
        if (!pc_job_copy(&j->code[i], &j->ncode[i], p->code[i], p->cmax[i])) { pc_writer_free(j); return; }
        uint64_t n = p->csize[i];   // the compiler constants (c1) right after the code
        if (n && pc_readable(p->code[i] + p->cmax[i], n) && pc_job_copy(&j->c1[i], &j->nc1[i], p->code[i] + p->cmax[i], n))
            j->c1_at[i] = (int32_t)(offsetof(PcProg2, csize) + i * sizeof(uint64_t));
    }
    pc_job_queue(j);
}
static void pc_record_pipe(uint64_t ckey, const PipeKey *k) {
    PcPipe r;
    memset(&r, 0, sizeof r);
    r.ckey = ckey;
    r.k = *k;
    r.k.prog = NULL;
    uint64_t rk = hash_words(ckey, &r.k, sizeof r.k);
    if (!pc_seen_add(rk ^ 0x5bd1e995ull)) return;
    pc_write(PC_PIPE, &r, sizeof r);
}

// TOTK_PRECOMPILE_SEED=<dir>: a read-only precompile.bin + code/ shipped with the build (programs, pipelines and compute
// programs recorded on other machines and sessions), loaded on every start next to cache/precompile.bin. Default:
// cache-seed/ next to cache/ when it has a precompile.bin (the PC build dir); TOTK_PRECOMPILE_SEED= (empty) = none.
static const char *pc_seed_dir(void) {
    static const char *d = (const char *)1;
    if (d == (const char *)1) {
        d = getenv("TOTK_PRECOMPILE_SEED");
        struct stat sb;
        if (!d && stat("cache-seed/precompile.bin", &sb) == 0) d = "cache-seed";
        if (d && !*d) d = NULL;
    }
    return d;
}
static FILE *pc_code_open(uint64_t hash, const char *ext) {   // cache/code/<hash>.<ext>, else the seed's
    char path[600];
    snprintf(path, sizeof path, "cache/code/%016llx.%s", (unsigned long long)hash, ext);
    FILE *f = fopen(path, "rb");
    if (!f && pc_seed_dir()) {
        snprintf(path, sizeof path, "%s/code/%016llx.%s", pc_seed_dir(), (unsigned long long)hash, ext);
        f = fopen(path, "rb");
    }
    return f;
}
static uint8_t *pc_load_code(uint64_t hash, uint64_t size) {
    FILE *f = pc_code_open(hash, "bin");
    if (!f) return NULL;
    uint8_t *b = malloc((size_t)size);
    bool ok = b && fread(b, 1, (size_t)size, f) == size;
    fclose(f);
    if (!ok) { free(b); return NULL; }
    return b;   // (kept: the program's code pointer)
}
// code (size bytes) followed by its compiler constants (cache/code/<hash>.c1, *csize bytes; *csize = 0 when missing)
static uint8_t *pc_load_code_c1(uint64_t hash, uint64_t size, uint64_t *csize) {
    if (!*csize) return pc_load_code(hash, size);
    FILE *c = pc_code_open(hash, "bin"), *k = pc_code_open(hash, "c1");
    uint8_t *b = c ? malloc((size_t)(size + *csize)) : NULL;
    bool ok = b && fread(b, 1, (size_t)size, c) == size;
    if (ok && !(k && fread(b + size, 1, (size_t)*csize, k) == *csize)) *csize = 0;   // (no constants: code only)
    if (c) fclose(c);
    if (k) fclose(k);
    if (!ok) { free(b); return NULL; }
    return b;
}

// ---- archive precompile (user 2026-10-15: "at the start of every playthrough, compiles every major shader in the game,
// it can run in the background during the opening"). tools/shader_pack.py packs every VS+FS program of the game's shader
// archives (romfs/Shader/*.bfsha: 8712 programs, 9131 stage blobs; material = 8412) into cache/archive_shaders.bin.
// After the recorded programs and pipelines, the workers translate each one with the key the game will most likely use
// and compile its SPIR-V into the disk cache (only what is missing). No modules are kept (~17k stages x ~140 KB SPIR-V).
// A program the game asks for later then costs translation + a cache read instead of a GLSL compile. Measured
// 2026-10-15: 864 of 1813 recorded programs come from these archives (analysis/bfsha/ match_runtime_output.txt); the
// rest (post effects, particles, UI) are covered by the recording only. Version 2: tools/shader_pack.py packs the
// program variations of Shader/ and Effect/ (18,423 programs) with each stage's compiler constants, so a program
// translates as the game's own (BRX tables) and its content key matches. TOTK_PRECOMPILE_ARCHIVE=0: off.
static uint8_t *pa_file;
static uint32_t pa_nblobs, pa_nprogs;
static const uint32_t *pa_blob;   // pa_nblobs x {offset, code size, constants size, 0}
static const uint8_t *pa_prog;    // pa_nprogs x {u32 vs, u32 fs, u8 attr_kind[32], u32 source}
// + the archives' compute programs ({u32 blob, u32 workgroup size[3]}: 116): appended to the recorded compute programs
// (pc_comps, PcComp.pad = blob + 1: the code is in the pack) and built in full by the compute phase, so a first
// dispatch never holds a frame (pcA2 held for compute 6 times after the warm-up)
static uint64_t pc_comp_key(uint64_t code_hash, const uint32_t local[3]);
static const uint8_t *pa_code;
static uint64_t *pa_known;        // sorted content keys of the recorded programs (built in full by the program phase)
static int pa_nknown;
static _Atomic int pa_next, pa_done, pa_compiled, pa_cached, pa_recorded, pa_failed;
static _Atomic uint64_t pa_translate_ns, pa_spirv_ns;
static uint64_t pa_t0;
static bool pa_full;   // TOTK_PRECOMPILE_ARCHIVE=full: complete programs (modules + GPL libraries), not just SPIR-V
static bool pa_warm;   // default (GPL on): libraries built once, then destroyed; =spirv: SPIR-V only
// Where a warm library goes: the driver's own shader cache only (VK_NULL_HANDLE), not our pipeline cache. Into ours, the
// archive made cache/vk_pipelines*.bin 889 MB (pcA1: 18,423 programs; the file is loaded into RAM at every start and
// copied whole at every save); the game's own build of a warm program then misses our cache and hits the driver's
// (NVIDIA, AMD and Intel keep one on disk). Ours holds what the game drew. =pcache: into ours, as before.
static bool pa_into_pcache;
enum { PA_PROG = 44 };

static int pa_cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}
static void pa_load(void) {
    const char *e = getenv("TOTK_PRECOMPILE_ARCHIVE");
    if (e && !strcmp(e, "0")) return;
    pa_full = e && !strcmp(e, "full");
    pa_warm = !(e && !strcmp(e, "spirv"));
    pa_into_pcache = e && !strcmp(e, "pcache");
    // TOTK_ARCHIVE_SHADERS=<file>: the pack in the work folder (package.py: game-derived, so a path, never copied)
    const char *af = getenv("TOTK_ARCHIVE_SHADERS");
    FILE *f = fopen(af && *af ? af : "cache/archive_shaders.bin", "rb");
    if (!f && af && *af) LOG("[precompile] TOTK_ARCHIVE_SHADERS=%s: not readable\n", af);
    if (!f) f = fopen("cache/archive_shaders.bin", "rb");
    if (!f && pc_seed_dir()) {   // (shipped with the build: the Deck package had none, so its archive warm-up never ran)
        char path[600];
        snprintf(path, sizeof path, "%s/archive_shaders.bin", pc_seed_dir());
        f = fopen(path, "rb");
    }
    if (!f) { LOG("[precompile] no cache/archive_shaders.bin (tools/shader_pack.py): archive precompile off\n"); return; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *d = n > 16 ? malloc((size_t)n) : NULL;
    bool ok = d && fread(d, 1, (size_t)n, f) == (size_t)n;
    fclose(f);
    uint32_t h[4];
    if (ok) memcpy(h, d, 16);
    ok = ok && h[0] == 0x50534b54u && h[1] == 2;   // 'TKSP' v2
    uint64_t o = 16 + (uint64_t)h[2] * 16 + (uint64_t)h[3] * PA_PROG;
    uint32_t nm = 0;
    if (ok && o + 4 <= (uint64_t)n) { memcpy(&nm, d + o, 4); o += 4 + (uint64_t)nm * 64; }
    uint32_t nc = 0;
    const uint64_t oc = o + 4;   // (the compute table)
    if (ok && o + 4 <= (uint64_t)n) { memcpy(&nc, d + o, 4); o += 4 + (uint64_t)nc * 16; }
    ok = ok && o <= (uint64_t)n;
    for (uint32_t i = 0; ok && i < nc; i++) {
        uint32_t c[4];
        memcpy(c, d + oc + (size_t)i * 16, 16);
        ok = c[0] < h[2] && c[1] && c[2] && c[3];
    }
    for (uint32_t i = 0; ok && i < h[2]; i++) {   // every blob inside the file
        uint32_t b[3];
        memcpy(b, d + 16 + i * 16, 12);
        ok = b[1] >= 0x80 && b[2] <= 0x10000 && o + b[0] + b[1] + b[2] <= (uint64_t)n;
    }
    for (uint32_t i = 0; ok && i < h[3]; i++) {   // every program's blobs
        uint32_t ix[2];
        memcpy(ix, d + 16 + (size_t)h[2] * 16 + (size_t)i * PA_PROG, 8);
        ok = ix[0] < h[2] && ix[1] < h[2];
    }
    if (!ok) { free(d); LOG("[precompile] cache/archive_shaders.bin: bad file or another version (tools/shader_pack.py), archive precompile off\n"); return; }
    pa_file = d;
    pa_nblobs = h[2];
    pa_nprogs = h[3];
    pa_blob = (const uint32_t *)(d + 16);
    pa_prog = d + 16 + (size_t)pa_nblobs * 16;
    pa_code = d + o;
    pa_known = malloc(sizeof *pa_known * (pc_nprogs + 1));
    for (int i = 0; i < pc_nprogs; i++) pa_known[pa_nknown++] = pc_progs[i].p.ckey;
    qsort(pa_known, pa_nknown, sizeof *pa_known, pa_cmp_u64);
    int ncs = 0;
    if (nc) {   // (pc_comps grows by the exact count: pc_load_file, with its own capacity, never runs after this)
        pc_comps = realloc(pc_comps, sizeof *pc_comps * (size_t)(pc_ncomps + (int)nc));
        for (uint32_t i = 0; i < nc; i++) {
            uint32_t c[4];
            memcpy(c, d + oc + (size_t)i * 16, 16);
            PcComp r = {.cmax = pa_blob[c[0] * 4 + 1], .csize = pa_blob[c[0] * 4 + 2], .pad = c[0] + 1};
            memcpy(r.local, c + 1, sizeof r.local);
            r.code_hash = hash_words(0x6a09e667f3bcc908ull ^ r.cmax, pa_code + pa_blob[c[0] * 4], (size_t)r.cmax);
            r.ckey = pc_comp_key(r.code_hash, r.local);
            if (pc_seen_add(r.ckey ^ PC_SEEN_CS)) { pc_comps[pc_ncomps++] = r; ncs++; }   // (recorded ones are there already)
        }
    }
    LOG("[precompile] archive: %u programs, %u stage blobs (%.1f MB), %u compute programs (%d not recorded)\n", pa_nprogs,
        pa_nblobs, (n - (long)o) / 1048576.0, nc, ncs);
    // TOTK_WARMUP_START=<index> (compile-speed A/B runs): start at that program, so each run compiles programs no earlier
    // run put into the driver's caches
    const char *st = getenv("TOTK_WARMUP_START");
    if (st && atoi(st) > 0 && atoi(st) < (int)pa_nprogs) atomic_store(&pa_next, atoi(st));
}
static void pa_warm_load(void);
static uint32_t pa_nwarmed;   // (archive programs in cache/archive_warm.bin, below)
static void pa_load_all(void) {
    pa_load();
    if (pa_nprogs) {
        pa_warm_load();
        LOG("[precompile] archive warm-up record: %u programs already warmed (skipped)\n", pa_nwarmed);
    }
}
// Warm-up record (user 2026-10-15: "stop clearing the cache for every new build"). The GPL warm-up rebuilt all 16354
// archive programs every launch (the user's 22:16 session: 280 s on 5 threads, pipeline cache +242 KB = nearly all
// driver-cache hits, "0 already cached" because the warm path never checked). cache/archive_warm.bin lists the content
// keys already warmed (into the driver's cache, or with =pcache into cache/vk_pipelines.bin: then valid only with that
// file loaded). Keys are committed after
// the pipeline cache that contains them is on disk (device.c vk_pipeline_cache_save -> vk_archive_warm_commit).
// TOTK_PRECOMPILE_REWARM=1 ignores the record. The record used to be tied to translate.c's build time, so every rebuild
// re-warmed all 16k programs (user 22:59: "its compiling AGAIN"). Now it survives rebuilds: a translator fix changes the
// GLSL of a few programs, which then compile at first use (SPIR-V cache miss) as before the warm-up existed; programs that
// failed to translate are never recorded, so they are retried every launch. Bump PA_WARM_EPOCH only for a change that
// alters the GLSL of most programs (shared prologue, SPIR-V options); the float-controls mode is part of the stamp.
static uint64_t *pa_warmed;   // sorted, from the file (pa_nwarmed entries)
static uint64_t *pa_wnew;     // warmed this session, in completion order
static uint32_t pa_nwnew, pa_capwnew, pa_ncommitted;
static pthread_mutex_t pa_wlock = PTHREAD_MUTEX_INITIALIZER;
enum { PA_WARM_STAMP = 32 };
#define PA_WARM_EPOCH "warm epoch 4"   // (2: 2026-10-19, drops the stale shader generations from the pipeline cache once;
                                       // 3: 2026-10-20, translate.c builtin bit casts change nearly every program's GLSL;
                                       // 4: 2026-10-20, one precise pf_, fmin/fmax/saturate as selects, integer attributes)
static void pa_warm_stamp(char *s) {
    extern int vk_spirv_float_preserve(void);
    snprintf(s, PA_WARM_STAMP, "%s%s%s%s", PA_WARM_EPOCH, vk_spirv_float_preserve() ? " fp" : "", gpl_shared() ? " gs" : "",
             g_vk.feedback_loop ? " fl" : "");   // (gs: shared stage libraries)
}
// The record also holds only for the engine build and driver that wrote it (cache/archive_warm.key = pc_cold_key, the
// first-start pass's key): a new build's translator can change any program's GLSL, and a program the record calls warm
// would then compile at the game's first draw of it. A new build warms the whole archive again; what did not change is
// a SPIR-V cache and driver cache hit (~3 ms a program, background). The stamp above is different: a new stamp also
// drops the pipeline cache (device.c), a new build does not.
static char pc_cold_key[160];
// The key's last line names the form that wrote the record: a SPIR-V-only record (no GPL, or LARECOMP_NO_GPL)
// loaded with GPL would call programs warm whose libraries were never built.
static const char *pa_warm_form(void) {
    return !pa_warm || !g_vk.gpl ? "SPIR-V only" : pa_into_pcache ? "libraries in our pipeline cache"
                                                                  : "libraries in the driver's cache";
}
static void pa_warm_key(char *k, size_t n) { snprintf(k, n, "%sform %s\n", pc_cold_key, pa_warm_form()); }
static bool pa_warm_build_ok(void) {
    char have[256] = "", want[256];
    FILE *f = fopen("cache/archive_warm.key", "rb");
    if (f) { size_t r = fread(have, 1, sizeof have - 1, f); have[r] = 0; fclose(f); }
    pa_warm_key(want, sizeof want);
    return pc_cold_key[0] && !strcmp(have, want);
}
static void pa_warm_load(void) {
    extern size_t g_vk_pcache_loaded;
    // (warmed into our pipeline cache: valid only with it loaded; into the driver's: the key below has the driver version)
    if (getenv("TOTK_PRECOMPILE_REWARM") || (pa_into_pcache && !g_vk_pcache_loaded)) return;
    if (!pa_warm_build_ok()) {
        LOG("[precompile] archive warm-up record from another engine build / GPU / driver: warming again\n");
        return;
    }
    FILE *f = fopen("cache/archive_warm.bin", "rb");
    if (!f) return;
    char hdr[8 + PA_WARM_STAMP], want[PA_WARM_STAMP] = {0};
    uint32_t n = 0;
    pa_warm_stamp(want);
    bool ok = fread(hdr, 1, sizeof hdr, f) == sizeof hdr && !memcmp(hdr, "TKWM\1\0\0\0", 8) && fread(&n, 4, 1, f) == 1;
    if (ok && memcmp(hdr + 8, want, PA_WARM_STAMP)) {
        LOG("[precompile] archive warm-up record stamp \"%.32s\", want \"%s\": warming again\n", hdr + 8, want);
        ok = false;
    }
    if (ok && n) {
        pa_warmed = malloc(sizeof *pa_warmed * n);
        ok = fread(pa_warmed, 8, n, f) == n;
        if (ok) pa_nwarmed = n;
        else { free(pa_warmed); pa_warmed = NULL; }
    }
    fclose(f);
}
// device.c, before loading the pipeline cache: a record with another stamp means every archive program is rebuilt in a
// new form, so the cached pipelines are a previous generation that only costs RAM (2026-10-19: a 5.3 GB file of 3
// generations = ~7.5 GB of driver memory plus a 5.3 GB copy per save -> low-RAM watchdog kills).
bool vk_archive_warm_stale(void) {
    FILE *f = fopen("cache/archive_warm.bin", "rb");
    if (!f) return false;
    char hdr[8 + PA_WARM_STAMP], want[PA_WARM_STAMP] = {0};
    pa_warm_stamp(want);
    bool stale = fread(hdr, 1, sizeof hdr, f) == sizeof hdr && !memcmp(hdr, "TKWM\1\0\0\0", 8) && memcmp(hdr + 8, want, PA_WARM_STAMP);
    fclose(f);
    return stale;
}
// device.c, right after a stale record dropped the old pipeline cache: an empty record with the current stamp. Without it the
// old stamp stayed until a session committed its first archive program (first save 3 min into the warm-up), so every
// shorter session after an update threw away the cache the previous one saved (Deck 2026-10-20: five test runs in a row
// "previous shader generation, starting empty").
void vk_archive_warm_reset(void) {
    char hdr[8 + PA_WARM_STAMP] = "TKWM\1\0\0\0";
    uint32_t zero = 0;
    pa_warm_stamp(hdr + 8);
    FILE *f = fopen("cache/archive_warm.bin.tmp", "wb");
    bool ok = f && fwrite(hdr, 1, sizeof hdr, f) == sizeof hdr && fwrite(&zero, 4, 1, f) == 1;
    if (f) fclose(f);
    if (ok) { remove("cache/archive_warm.bin"); ok = rename("cache/archive_warm.bin.tmp", "cache/archive_warm.bin") == 0; }
    LOG("[precompile] archive warm-up record reset to the current shader generation%s\n", ok ? "" : " (write FAILED)");
}
static bool pa_is_warmed(uint64_t ck) { return pa_nwarmed && bsearch(&ck, pa_warmed, pa_nwarmed, sizeof ck, pa_cmp_u64); }
static void pa_warm_add(uint64_t ck) {
    pthread_mutex_lock(&pa_wlock);
    if (pa_nwnew == pa_capwnew) { pa_capwnew = pa_capwnew ? pa_capwnew * 2 : 4096; pa_wnew = realloc(pa_wnew, sizeof *pa_wnew * pa_capwnew); }
    pa_wnew[pa_nwnew++] = ck;
    pthread_mutex_unlock(&pa_wlock);
}
// "[precompile] warm-up stages": per built program, ms of thread time in each stage (see pa_tm), and translate_lock's busy
// share of the wall time since the warm-up began (near 100 % = the single translator limits the warm-up)
static void pa_tm_log(void) {
    uint64_t n = atomic_load(&pa_tm_n);
    if (!n) return;
    double w = (SDL_GetTicksNS() - (pa_t0 ? pa_t0 : pc_t0)) / 1e9, d = (double)n * 1e6, s[8];
    for (int k = 0; k < 8; k++) s[k] = (double)atomic_load(&pa_tm_sum[k]);
    if (w <= 0) w = 1;
    LOG("[precompile] warm-up stages: %llu programs in %.0f s (%.1f/s): ms per program: lock wait %.1f, translate %.1f, "
        "GLSL->SPIR-V %.1f (%.0f %% of stages cached), modules %.1f, GPL libraries %.1f (%.0f %% of %.0f cache hits%s); "
        "translate_lock busy %.0f %%\n",
        (unsigned long long)n, w, n / w, s[0] / d, s[1] / d, s[2] / d, s[5] * 50.0 / n, s[3] / d, s[4] / d,
        s[7] ? s[6] * 100.0 / s[7] : 0.0, s[7], gpl_shared() ? ", shared" : "", s[1] / 1e9 / w * 100.0);
}
// device.c: before reading the pipeline cache data (keys warmed so far are inside it) ...
uint32_t vk_archive_warm_snapshot(void) {
    pthread_mutex_lock(&pa_wlock);
    uint32_t n = pa_nwnew;
    pthread_mutex_unlock(&pa_wlock);
    return n;
}
// ... and after that data is on disk (or the cache did not change since the last file): record them.
void vk_archive_warm_commit(uint32_t n) {
    pthread_mutex_lock(&pa_wlock);
    if (n > pa_ncommitted) {
        uint32_t total = pa_nwarmed + n;
        uint64_t *all = malloc(sizeof *all * total);
        if (pa_nwarmed) memcpy(all, pa_warmed, sizeof *all * pa_nwarmed);
        memcpy(all + pa_nwarmed, pa_wnew, sizeof *all * n);
        qsort(all, total, sizeof *all, pa_cmp_u64);
        char hdr[8 + PA_WARM_STAMP] = "TKWM\1\0\0\0";
        pa_warm_stamp(hdr + 8);
        FILE *f = fopen("cache/archive_warm.bin.tmp", "wb");
        bool ok = f && fwrite(hdr, 1, sizeof hdr, f) == sizeof hdr && fwrite(&total, 4, 1, f) == 1 &&
                  fwrite(all, 8, total, f) == total;
        if (f) fclose(f);
        if (ok) { remove("cache/archive_warm.bin"); ok = rename("cache/archive_warm.bin.tmp", "cache/archive_warm.bin") == 0; }
        if (ok) pa_ncommitted = n;
        static bool key_written;   // (after the record: a crash in between leaves the old key = a warm-up again, not a gap)
        if (ok && !key_written && pc_cold_key[0]) {
            FILE *k = fopen("cache/archive_warm.key.tmp", "wb");
            char key[256];
            pa_warm_key(key, sizeof key);
            bool kw = k && fputs(key, k) >= 0;
            if (k) kw = fclose(k) == 0 && kw;
            if (kw) { remove("cache/archive_warm.key"); key_written = rename("cache/archive_warm.key.tmp", "cache/archive_warm.key") == 0; }
        }
        // (the form: warm in the driver's cache relies on its disk cache; a user who turned that off gets the
        // first-sight builds back, and this line says which form the record trusted)
        LOG("[precompile] archive warm-up record: %u programs, %s%s\n", total, pa_warm_form(), ok ? "" : " (write FAILED)");
        pa_tm_log();
        free(all);
    }
    pthread_mutex_unlock(&pa_wlock);
}

// (the governor's A/B, below: whether a job is working when a frame begins)
static _Atomic int pa_busy;
static void pa_work_begin(void) { atomic_fetch_add(&pa_busy, 1); }
static void pa_work_end(void) { atomic_fetch_add(&pa_busy, -1); }
static void pa_job(int i) {
    const uint8_t *r = pa_prog + (size_t)i * PA_PROG;
    uint32_t ix[2];
    memcpy(ix, r, 8);
    if (ix[0] >= pa_nblobs || ix[1] >= pa_nblobs) { atomic_fetch_add(&pa_failed, 1); return; }
    ShaderKey vkey = {0}, fkey = {0};   // as draw_() builds them
    memcpy(vkey.attr_kind, r + 8, 32);
    vkey.glsl = fkey.glsl = 1;
    vkey.flip_y = fkey.flip_y = nvn_window_origin() == 0;
    vkey.depth_minus_one = nvn_depth_mode() == 0;
    const uint8_t *code[2];
    uint64_t cmax[2], csize[2], chash[2];
    for (int s = 0; s < 2; s++) {   // (as pc_prog_key: the code up to the constants' offset; the constants after it)
        code[s] = pa_code + pa_blob[ix[s] * 4];
        cmax[s] = pa_blob[ix[s] * 4 + 1];
        csize[s] = pa_blob[ix[s] * 4 + 2];
        chash[s] = hash_words(0x6a09e667f3bcc908ull ^ cmax[s], code[s], (size_t)cmax[s]);
    }
    uint64_t ck = pc_ckey(chash, &vkey, &fkey);
    if (bsearch(&ck, pa_known, pa_nknown, sizeof ck, pa_cmp_u64)) { atomic_fetch_add(&pa_recorded, 1); return; }
    // (the record also serves the SPIR-V-only form, a GPU without GPL: else every start translated the whole pack again,
    // ~60 s under translate_lock beside the game's own translator)
    if (!pa_full && pa_is_warmed(ck)) { atomic_fetch_add(&pa_cached, 1); return; }
    pa_work_begin();   // (the governor's frame A/B: a job's real work, not the skips above)
    static int par = -1;
    // (on, =0 off: the archive's workers translate without translate_lock, as the first-start pass; pcA1 on 15
    // threads: translate_lock busy 50 %, 5 ms of lock wait a program. Also the SPIR-V form's: a play worker at
    // THREAD_PRIORITY_IDLE preempted inside the lock on a saturated PC would hold the game's translator until Windows'
    // starvation boost, ~4 s)
    if (par < 0) { const char *e = getenv("LARECOMP_PARALLEL_TRANSLATE"); par = !(e && *e == '0'); }
    if (pa_full || (pa_warm && g_vk.gpl)) {
        // full: a complete program (modules -> pipeline libraries) the game's draws pick up by content key (~1.3 MB of
        // driver memory per program, so ~10 GB for the archive - opt-in only). warm (default with GPL): the same
        // build, then everything is destroyed again; the compiled libraries stay in the driver's own shader cache
        // (pa_into_pcache: ours), so the game's own build of the program later is a hit.
        Prog *p = calloc(1, sizeof *p);
        p->vkey = vkey;
        p->fkey = fkey;
        p->ckey = ck;
        p->inline_compile = true;
        for (int s = 0; s < 2; s++) { p->code[s] = (uint64_t)(uintptr_t)code[s]; p->cmax[s] = cmax[s]; p->csize[s] = csize[s]; }   // (pa_file stays)
        if (pa_full && pc_claim_prog(ck, p) != p) { free(p); atomic_fetch_add(&pa_recorded, 1); pa_work_end(); return; }
        uint64_t t0 = SDL_GetTicksNS();
        memset(pa_tm, 0, sizeof pa_tm);
        pa_tm_on = true;
        translate_unlocked = par;
        gpl_driver_cache = !pa_full && !pa_into_pcache;
        translate_program(p);
        gpl_driver_cache = false;
        translate_unlocked = false;
        pa_tm_on = false;
        for (int k = 0; k < 8; k++) atomic_fetch_add(&pa_tm_sum[k], pa_tm[k]);
        atomic_fetch_add(&pa_tm_n, 1);
        atomic_fetch_add(&pa_spirv_ns, SDL_GetTicksNS() - t0);
        atomic_fetch_add(atomic_load(&p->state) == 1 ? &pa_compiled : &pa_failed, 1);
        if (!pa_full && atomic_load(&p->state) == 1 && p->lib_pre && p->lib_frag) pa_warm_add(ck);
        if (!pa_full) {
            if (p->lib_pre) vkDestroyPipeline(g_vk.dev, p->lib_pre, NULL);
            if (p->lib_frag) vkDestroyPipeline(g_vk.dev, p->lib_frag, NULL);
            if (p->lib_frag_none) vkDestroyPipeline(g_vk.dev, p->lib_frag_none, NULL);
            if (p->vs) vkDestroyShaderModule(g_vk.dev, p->vs, NULL);
            if (p->fs) vkDestroyShaderModule(g_vk.dev, p->fs, NULL);
            if (p->layout) vkDestroyPipelineLayout(g_vk.dev, p->layout, NULL);
            for (int s = 0; s < 2; s++) if (p->lib_layout[s]) vkDestroyPipelineLayout(g_vk.dev, p->lib_layout[s], NULL);
            extern bool vk_set_layouts_shared(void);   // (shared layouts belong to shaders.c's cache: G6)
            for (int s = 0; s < 2 && !vk_set_layouts_shared(); s++) if (p->set_layout[s]) vkDestroyDescriptorSetLayout(g_vk.dev, p->set_layout[s], NULL);
            maxwell_free(p->vi);
            maxwell_free(p->fi);
            free(p);
        }
        pa_work_end();
        return;
    }
    uint64_t t0 = SDL_GetTicksNS();
    if (!par) pthread_mutex_lock(&translate_lock);   // (maxwell_gmem_* are thread-local: MAXWELL_TLS)
    maxwell_gmem_base = HEAP_BASE;
    maxwell_gmem_size = HEAP_MAX;
    maxwell_gdev_size = gdev_window_size();
    ShaderInfo *vi = translate_stage((uint64_t)(uintptr_t)code[0], cmax[0], csize[0], STAGE_VERTEX, &vkey), *fi = NULL;
    if (vi->msl) {
        fkey.vs_out_mask = vi->out_mask;
        fi = translate_stage((uint64_t)(uintptr_t)code[1], cmax[1], csize[1], STAGE_FRAGMENT, &fkey);
    }
    if (!par) pthread_mutex_unlock(&translate_lock);
    uint64_t t1 = SDL_GetTicksNS();
    atomic_fetch_add(&pa_translate_ns, t1 - t0);
    if (!vi->msl || !fi || !fi->msl) {
        if (atomic_fetch_add(&pa_failed, 1) < 5)
            LOG("[precompile] archive program %d (blobs %u/%u): translate failed: %s\n", i, ix[0], ix[1], !vi->msl ? vi->error : fi ? fi->error : "?");
    } else if (vk_spirv_cached(vi->msl, STAGE_VERTEX) && vk_spirv_cached(fi->msl, STAGE_FRAGMENT)) {
        atomic_fetch_add(&pa_cached, 1);
        pa_warm_add(ck);
    } else {
        size_t nw;
        uint32_t *sv = vk_glsl_to_spirv(vi->msl, STAGE_VERTEX, "archive_vs", &nw);
        uint32_t *sf = vk_glsl_to_spirv(fi->msl, STAGE_FRAGMENT, "archive_fs", &nw);
        atomic_fetch_add(sv && sf ? &pa_compiled : &pa_failed, 1);
        if (sv && sf) pa_warm_add(ck);
        free(sv);
        free(sf);
    }
    atomic_fetch_add(&pa_spirv_ns, SDL_GetTicksNS() - t1);
    maxwell_free(vi);
    maxwell_free(fi);
    pa_work_end();
}

// TOTK_WARMUP=1: warm-up only. The archive warm-up runs on every CPU but one, unthrottled, then the pipeline cache is
// saved and the process exits ("[precompile] warm-up finished"). Deck s1_seed: the warm-up in the background of play took
// 90 % of a core and the Deck fell from a locked 60 to 55-57 fps, so the in-play warm-up is paced (the governor below)
// and the warm-up mode does the bulk once (tools/linux/warmup.sh). "Play now" (user 2026-10-20: "add an option to start
// game right now and the compilation will keep going in the background"): A on the warm-up screen (hle_hid.cpp) releases
// the warm-up mode in place. Normal pacing returns, nothing exits at the end, and the archive goes on as in any session
// (the governor).
static _Atomic int pa_released;
static bool pa_warmup_mode(void) {
    static int m = -1;
    if (m < 0) { const char *e = getenv("TOTK_WARMUP"); m = e && *e && strcmp(e, "0"); }
    return m && !atomic_load(&pa_released);
}
int vk_precompile_warmup_active(void) { return pa_warmup_mode(); }   // (hle_vi.cpp pacing, hle_hid.cpp "play now")
void vk_precompile_play_now(void) {
    if (!pa_warmup_mode()) return;
    atomic_store(&pa_released, 1);
    LOG("[precompile] play now: warm-up released at %u of %u archive programs, compiling on in the background\n",
        (unsigned)atomic_load(&pa_done), (unsigned)pa_nprogs);
}
static void pc_yield(void) {   // (pause while the game's own shader jobs are queued or running)
    while (atomic_load(&g_jobs_pending) > 0) SDL_Delay(3);
}
// "[precompile] stages": the recorded programs' thread time per stage (pa_tm, as the archive warm-up's line) and the
// pipelines' build_pipeline_body time: what a start with a loaded pipeline cache still spends
static _Atomic uint64_t pc_tm_sum[8], pc_tm_n, pc_pipe_ns, pc_progs_t;
static void pc_tm_log(void) {
    uint64_t n = atomic_load(&pc_tm_n);
    double d = n ? (double)n * 1e6 : 1, s[8], tp = atomic_load(&pc_progs_t);
    for (int k = 0; k < 8; k++) s[k] = (double)atomic_load(&pc_tm_sum[k]);
    LOG("[precompile] stages: %llu programs built (%.1f s to the last), ms per program: lock wait %.2f, translate %.2f, "
        "GLSL->SPIR-V %.2f (%.0f %% of stages cached), modules %.2f, GPL libraries %.2f (%.0f %% of %.0f cache hits); "
        "translate %.1f s in all; pipelines %.1f s of thread time (%.2f ms each)\n",
        (unsigned long long)n, tp ? (tp - pc_t0) / 1e9 : 0.0, s[0] / d, s[1] / d, s[2] / d, n ? s[5] * 50.0 / n : 0.0, s[3] / d,
        s[4] / d, s[7] ? s[6] * 100.0 / s[7] : 0.0, s[7], s[1] / 1e9, atomic_load(&pc_pipe_ns) / 1e9,
        pc_npipes ? atomic_load(&pc_pipe_ns) / 1e6 / pc_npipes : 0.0);
}
// The precompile workers have cores the game does not run on: helpers placed (cpu_place.h) AND the game's threads kept
// off them. A hybrid part with fewer than 6 P-cores (i5-1235U 2P+8E, Lunar Lake 4P+4E) places helpers on the E-cores but
// leaves the game unrestricted, so there they share the game's cores and the plain counts below apply.
static bool pc_own_cores(void) {
    return cpu_place_active() && cpu_place_count(CPU_HOT) < cpu_place_count(CPU_ANY);
}
// The first-start pass's extra workers stop after their current item once the title is left (hle_fs.cpp
// g_title_phase): on a hybrid / own-cores PC they share the game's cores, where below-normal priority does not help
// against an SMT sibling, so play never has them. The usual workers finish the queue.
extern volatile int g_title_phase;
static bool pc_extra_stop(int widx) {
    if (widx < pc_nthreads_normal || g_title_phase) return false;
    static _Atomic int told;
    if (!atomic_exchange(&told, 1)) {
        int left = (pc_ncomps - atomic_load(&pc_next_comp)) + (pc_nprogs - atomic_load(&pc_next_prog)) +
                   (pc_npipes - atomic_load(&pc_next_pipe));
        LOG("[precompile] title left: the %d extra workers stop; %d items left to the usual %d\n",
            pc_nthreads - pc_nthreads_normal, left > 0 ? left : 0, pc_nthreads_normal);
    }
    return true;
}
// The archive warm-up's governor (warmup 2026-10-30). bc11267d warmed the archive on the usual workers (4 on 16 CPUs) at
// full duty whatever the game did, a fresh cache: 68.7 fps / 1 % low 40 against 105 / 49 without it, render thread 11.1
// ms a frame against 7.2, game CPU 5.7 cores against 3.8, for minutes (5,748 of 18,423 programs after 90 s). Their
// LOWEST priority could not help: every game thread outranks it already, and with ~4 of 16 CPUs in use what slows the
// game is a shared core (SMT sibling), cache and clock, not CPU time. So the warm-up works where nothing is played and
// creeps where something is:
// - the title, or the game held (no present for 1 s: focus loss): every CPU but one;
// - a loading screen (hle_vi's render-pass rule): the usual count (CPUs / 4: as many as bc11267d had, so a load is no
//   slower than with it);
// - a menu that pauses the world (pause menu, item popup, Purah Pad, map, options, recipe book, save list): CPUs / 8;
// - play: from ONE worker at THREAD_PRIORITY_IDLE, as far as an A/B the frames themselves run allows. The play workers
//   start one job each at the start of every cycle (the mean job time x 100 / duty, 0.1 to 2 s), all together, so the
//   frames split into stretches with jobs and without that alternate fast. Each game present books the frame by the
//   state it began in. Every 90 + 90 frames the means compare: job frames 2 % longer or more, or more hitches with jobs
//   than without (rate twice it + 0.5 points, 4 at least, over ~2,000 frames), halve the workers, then the duty; below 5
//   %: none for 20 s, doubling while it keeps costing (to 5 min). Under 0.5 % longer: duty + 10 up to the cap
//   (TOTK_PRECOMPILE_ARCHIVE_DUTY, default 50), then one worker more up to the loading screens' count. Capped or
//   GPU-bound frames do not lengthen, so there it climbs; a CPU-bound or busy PC whose frames do gets less: on a quiet
//   host warm-up off 136.5 fps / 1 % low 86, 25 % duty 137.4 / 89, full duty (render pinned) 133.5 / 88; on a busy host
//   (63 % other load) full duty 68.7 / 40: what costs is a loaded PC and the render thread's core, which is what the A/B
//   sees. Offline (tools/engine/sim_warm_governor.py, 137 fps, 5-20 % frame noise, 0.5 % game hitches): no cost -> 1-2
//   workers at ~45 %; a cost per worker of 0.5-5 % -> 0.4-2 % frame time in all; 15-50 % -> under 0.6 % (held).
// The game's own shader jobs pause every worker (pc_yield), as before. The record (cache/archive_warm.bin) carries what
// is done across launches: nothing is warmed twice. TOTK_PRECOMPILE_ARCHIVE_GOVERN=0: bc11267d's pacing (the usual
// workers at full duty, 20 % on <= 8 CPUs; the first-start pass's extra ones on the title), for A/Bs.
#ifdef _WIN32
#define PA_PRIO_IDLE -15   // THREAD_PRIORITY_IDLE
#else
#define PA_PRIO_IDLE -4    // (the shim: nice +20, the kernel's 19)
#endif
enum { PA_W_PLAY, PA_W_MENU, PA_W_LOAD, PA_W_FREE };
static const char *const pa_where_name[] = {"play", "menu", "loading screen", "title / held"};
static _Atomic int pa_where = PA_W_FREE;   // (from the game presents, vk_warm_present)
static _Atomic uint64_t pa_present_ns;     // the last game present
static _Atomic int pa_duty = 20;           // play: the cycle's share of a job (pa_play_cycle)
static _Atomic int pa_play_n = 1;          // play: workers
static _Atomic uint64_t pa_hold_until;     // play: no job before this
static _Atomic uint64_t pa_job_ns = 60000000;   // play: the mean job time (real work only)
static int pa_duty_max = 50, pa_duty_old;   // (pa_duty_old: TOTK_PRECOMPILE_ARCHIVE_GOVERN=0's duty)
static int pa_nload = 1;                     // workers on a loading screen: the usual count (pc_start_workers)
static _Atomic int pa_govern = -1;         // (pc_start_workers, before the workers: pa_govern_init)
static void pa_govern_init(void) {
    const char *g = getenv("TOTK_PRECOMPILE_ARCHIVE_GOVERN"), *e = getenv("TOTK_PRECOMPILE_ARCHIVE_DUTY");
    if (e && *e) pa_duty_max = atoi(e) < 1 ? 1 : atoi(e) > 100 ? 100 : atoi(e);
    pa_duty_old = e && *e ? pa_duty_max : SDL_GetNumLogicalCPUCores() <= 8 && !pc_own_cores() ? 20 : 100;
    if (pa_duty_max < atomic_load(&pa_duty)) atomic_store(&pa_duty, pa_duty_max);
    atomic_store(&pa_govern, !(g && !strcmp(g, "0")));
    LOG("[precompile] archive warm-up governor %s (play: one worker, duty up to %d %%)\n",
        atomic_load(&pa_govern) ? "on" : "off (TOTK_PRECOMPILE_ARCHIVE_GOVERN=0)", pa_duty_max);
}
// hle_vi.cpp load_track, every game present (game thread): where the game is (gameplay: its render-pass rule; menu: a
// world-pausing menu is up), and the frame A/B
void vk_warm_present(int gameplay, int menu) {
    if (!pa_nprogs || atomic_load(&pa_govern) <= 0 || atomic_load(&pa_next) >= (int)pa_nprogs) return;
    const uint64_t now = SDL_GetTicksNS(), last = atomic_exchange(&pa_present_ns, now);
    const int w = g_title_phase ? PA_W_FREE : !gameplay ? PA_W_LOAD : menu ? PA_W_MENU : PA_W_PLAY;
    const int was = atomic_exchange(&pa_where, w);
    static bool began_on;   // (a job was working when this frame began: the last present)
    static double ema, s_on, s_off, n_on, n_off, h_on, h_off, f_on, f_off;
    static uint64_t since;
    static int logs, holds;
    const bool on = began_on;
    began_on = atomic_load(&pa_busy) > 0;
    if (w != was && logs < 300) {
        logs++;
        LOG("[precompile] warm-up: %s (%d of %u archive programs)\n", pa_where_name[w], atomic_load(&pa_done), pa_nprogs);
    }
    const double dt = last ? (double)(now - last) : 0;
    const bool held = now < atomic_load(&pa_hold_until);
    if (w != PA_W_PLAY || w != was || !last || dt > 250e6 || held) {   // (held: no job, nothing to compare)
        s_on = s_off = n_on = n_off = 0;
        since = now;
        return;
    }
    const bool hitch = ema && dt > 2 * ema;   // (counted below, not in the means)
    h_on *= 0.9995; h_off *= 0.9995; f_on *= 0.9995; f_off *= 0.9995;
    if (on) { f_on++; h_on += hitch; } else { f_off++; h_off += hitch; }
    if (hitch) return;
    ema = ema ? ema + (dt - ema) / 16 : dt;
    if (on) { s_on += dt; n_on++; } else { s_off += dt; n_off++; }
    if (now - since > 30000000000ull) { s_on = s_off = n_on = n_off = 0; since = now; return; }   // (too few of one kind)
    if (n_on < 90 || n_off < 90) return;
    const double r = (s_on / n_on) / (s_off / n_off);
    const bool hitchy = h_on >= 4 && h_on / f_on > 2 * h_off / (f_off > 1 ? f_off : 1) + 0.005;
    if (hitchy) h_on = h_off = f_on = f_off = 0;
    const int d = atomic_load(&pa_duty), n = atomic_load(&pa_play_n);
    int nd = d, nn = n, hold_s = 0;
    if (r > 1.02 || hitchy) {
        if (n > 1) nn = n / 2;
        else nd = d / 2;
    } else if (r < 1.005) {
        if (d < pa_duty_max) nd = d + 10 > pa_duty_max ? pa_duty_max : d + 10;
        else if (n < pa_nload) nn = n + 1;
    }
    if (nd < 5) {   // (none for 20 s, doubling while it keeps costing, up to 5 min)
        nd = 5 > pa_duty_max ? pa_duty_max : 5;
        hold_s = 20 << (holds < 4 ? holds : 4);
        hold_s = hold_s > 300 ? 300 : hold_s;
        holds++;
        atomic_store(&pa_hold_until, now + (uint64_t)hold_s * 1000000000ull);
    } else if (r <= 1.02 && !hitchy) holds = 0;
    atomic_store(&pa_duty, nd);
    atomic_store(&pa_play_n, nn);
    static uint64_t last_log;
    if ((nd != d || nn != n || hold_s || now - last_log > 60000000000ull) && logs < 300) {
        logs++;
        last_log = now;
        char hs[32] = "";
        if (hold_s) snprintf(hs, sizeof hs, ", none for %d s", hold_s);
        LOG("[precompile] warm-up governor: frames %.2f ms with jobs, %.2f ms without (%+.1f %%%s): %d -> %d workers, duty "
            "%d -> %d %%%s (%d of %u)\n",
            s_on / n_on / 1e6, s_off / n_off / 1e6, (r - 1) * 100, hitchy ? ", more hitches with jobs" : "", n, nn, d, nd, hs,
            atomic_load(&pa_done), pa_nprogs);
    }
    s_on = s_off = n_on = n_off = 0;
    since = now;
}
// The warm-up record waits for our pipeline cache's save only where the warm work goes into it (=pcache, =full); the
// libraries in the driver's own cache (the default) and the SPIR-V form's disk cache are there when a job ends. Then the
// workers commit it every 30 s themselves, and device.c's saver keeps its usual pace (vk_precompile_fills_pcache): with
// the governor the warm-up can last hours of play, and a save copies the whole cache (1.8 GB).
static bool pa_own_record(void) { return !pa_full && !pa_into_pcache; }
int vk_precompile_fills_pcache(void) {
    if (atomic_load(&pc_running) <= 0) return 0;
    const bool recorded = atomic_load(&pc_comps_done) + atomic_load(&pc_progs_done) + atomic_load(&pc_pipes_done) <
                          pc_ncomps + pc_nprogs + pc_npipes;
    return recorded || (pa_nprogs && !pa_own_record());
}
// How many workers may take an archive job now (widx below it), at what duty, and whether it is play (*play)
static int pa_allowed(int *duty, bool *play) {
    const int ncpu = SDL_GetNumLogicalCPUCores(), all = ncpu > 2 ? ncpu - 1 : 1;
    *duty = 100;
    *play = false;
    if (pa_warmup_mode()) return all;
    if (atomic_load(&pa_govern) <= 0) {
        if (g_title_phase) return pc_nthreads;
        *duty = pa_duty_old;
        return pc_nthreads_normal;
    }
    const uint64_t now = SDL_GetTicksNS(), last = atomic_load(&pa_present_ns);
    int w = atomic_load(&pa_where);
    if (g_title_phase || (last && now - last > 1000000000ull)) w = PA_W_FREE;
    switch (w) {
    case PA_W_FREE: return all;
    case PA_W_LOAD: return pa_nload;
    case PA_W_MENU: return ncpu / 8 > 1 ? ncpu / 8 : 1;
    default:
        *duty = atomic_load(&pa_duty);
        *play = true;
        return now >= atomic_load(&pa_hold_until) ? atomic_load(&pa_play_n) : 0;
    }
}
// play: the cycle every play worker starts one job in, together (the mean job time x 100 / duty, 0.1 to 2 s)
static uint64_t pa_play_cycle(int duty) {
    uint64_t c = atomic_load(&pa_job_ns) * 100 / (uint64_t)(duty > 0 ? duty : 1);
    return c < 100000000ull ? 100000000ull : c > 2000000000ull ? 2000000000ull : c;
}
static void *pc_worker(void *arg) {
    const int widx = (int)(intptr_t)arg;   // (worker index: the extra workers are the last ones; pa_allowed counts from 0)
    // (ahead of the game, nothing waits on it: the E-cores / the other CCD, cpu_place.h; the first-start pass's extra
    // workers on every core, they exist to use them)
    cpu_place_thread(widx < pc_nthreads_normal ? CPU_HELPER : CPU_ANY);
#ifndef _WIN32
    { extern int pthread_setname_np(pthread_t, const char *); pthread_setname_np(pthread_self(), "vk-precompile"); }   // ([cpu] line)
#endif
    // THREAD_PRIORITY_BELOW_NORMAL: the game's own builds go first (Linux: runtime/linux shim = nice +5 for this thread;
    // matters on the Steam Deck's 4 cores, where these workers otherwise compete with the game's threads)
    SetThreadPriority(GetCurrentThread(), -1);
    for (;;) {   // compute programs first (few; each one's first dispatch otherwise holds a frame)
        pc_yield();
        if (pc_extra_stop(widx)) goto extra_out;
        int i = atomic_fetch_add(&pc_next_comp, 1);
        if (i >= pc_ncomps) break;
        pc_comp_build(&pc_comps[i]);
        atomic_fetch_add(&pc_comps_done, 1);
    }
    for (;;) {   // programs: translated + compiled on this thread (translation serialised by translate_lock, but not in the first-start pass unless TOTK_PRECOMPILE_UNLOCKED=0)
        pc_yield();
        if (pc_extra_stop(widx)) goto extra_out;
        int i = atomic_fetch_add(&pc_next_prog, 1);
        if (i >= pc_nprogs) break;
        const PcProg *r = &pc_progs[i].p;
        Prog *p = calloc(1, sizeof *p);
        p->vkey = r->vkey;
        p->fkey = r->fkey;
        p->ckey = r->ckey;
        p->inline_compile = true;
        bool ok = true;
        for (int s = 0; s < 2; s++) {
            uint64_t cs = pc_progs[i].csize[s];
            uint8_t *b = pc_load_code_c1(r->code_hash[s], r->cmax[s], &cs);
            if (!b) { ok = false; break; }
            p->code[s] = (uint64_t)(uintptr_t)b;
            p->cmax[s] = r->cmax[s];
            p->csize[s] = cs;
        }
        if (ok && pc_claim_prog(r->ckey, p) == p) {
            memset(pa_tm, 0, sizeof pa_tm);
            pa_tm_on = true;
            translate_unlocked = pc_unlocked && atomic_load(&pc_cold_running);   // (the first-start pass only)
            translate_program(p);
            translate_unlocked = false;
            pa_tm_on = false;
            for (int k = 0; k < 8; k++) atomic_fetch_add(&pc_tm_sum[k], pa_tm[k]);
            atomic_fetch_add(&pc_tm_n, 1);
        } else free(p);   // (the game got there first; its program serves the pipelines below)
        atomic_fetch_add(&pc_progs_done, 1);
    }
    while (atomic_load(&pc_progs_done) < pc_nprogs) SDL_Delay(5);
    { uint64_t z = 0; atomic_compare_exchange_strong(&pc_progs_t, &z, SDL_GetTicksNS()); }
    for (;;) {   // pipelines, keyed exactly as draw() keys them
        pc_yield();
        if (pc_extra_stop(widx)) goto extra_out;
        int i = atomic_fetch_add(&pc_next_pipe, 1);
        if (i >= pc_npipes) break;
        const PcPipe *r = &pc_pipes[i];
        Prog *p = pc_find_prog(r->ckey);
        uint64_t t0 = SDL_GetTicksNS();
        while (p && atomic_load(&p->state) == 0 && SDL_GetTicksNS() - t0 < 10000000000ull) SDL_Delay(2);
        if (p && atomic_load(&p->state) == 1) {
            Pipe *pp = calloc(1, sizeof *pp);
            pp->k = r->k;
            pp->k.prog = p;
            uint64_t ph = hash_mem(0xcbf29ce484222325ull, &pp->k, sizeof pp->k);
            if (!ph) ph = 1;
            pp->claimed = 1;
            if (pc_take_pipe(ph)) free(pp);
            else {
                pc_put_pipe(ph, pp);
                uint64_t tb = SDL_GetTicksNS();
                build_pipeline_body(pp);
                atomic_fetch_add(&pc_pipe_ns, SDL_GetTicksNS() - tb);
            }
        }
        atomic_fetch_add(&pc_pipes_done, 1);
    }
extra_out:;
    static _Atomic int phase1_done;
    if (atomic_fetch_add(&phase1_done, 1) == pc_nthreads - 1) {
        LOG("[precompile] done: %d programs, %d pipelines in %.1f s\n", pc_nprogs, pc_npipes, (SDL_GetTicksNS() - pc_t0) / 1e9);
        pc_tm_log();
        pa_t0 = SDL_GetTicksNS();
        if (pc_cold) pc_cold_done();
    }
    // archive programs last, as the governor allows (pa_allowed); a worker above the count waits for the next burst
    if (pa_nprogs) {
        int prio = 0;
        uint64_t served = UINT64_MAX;   // (play: the cycle this worker took its job in)
        for (;;) {
            pc_yield();
            if (atomic_load(&pa_next) >= (int)pa_nprogs) break;
            int duty;
            bool play;
            const int allow = pa_allowed(&duty, &play);
            const int want = play ? PA_PRIO_IDLE : -2;   // (else THREAD_PRIORITY_LOWEST, Linux shim: nice +10)
            if (want != prio) { SetThreadPriority(GetCurrentThread(), want); prio = want; }
            if (widx >= allow) { SDL_Delay(play ? 50 : 100); continue; }
            if (play) {   // one job a cycle, at its start (a worker late into it waits for the next)
                const uint64_t now = SDL_GetTicksNS(), c = pa_play_cycle(duty), idx = now / c;
                if (idx == served || now - idx * c > c * (uint64_t)duty / 100) {
                    const uint64_t wait = (idx + 1) * c - now;
                    SDL_Delay((uint32_t)(wait / 1000000ull < 50 ? wait / 1000000ull + 1 : 50));
                    continue;
                }
                served = idx;
            }
            int i = atomic_fetch_add(&pa_next, 1);
            if (i >= (int)pa_nprogs) break;
            uint64_t t0 = SDL_GetTicksNS();
            pa_job(i);
            atomic_fetch_add(&pa_done, 1);
            const uint64_t took = SDL_GetTicksNS() - t0;
            if (took > 1000000ull) {   // (a job that worked: the play cycle's mean, 1/8 each)
                const uint64_t m = atomic_load(&pa_job_ns);
                atomic_store(&pa_job_ns, m - m / 8 + took / 8);
            }
            {   // the stage timers every 30 s too (a short test run can end before any warm-up record is written); the
                // record itself too where it does not wait for our pipeline cache's save (pa_own_record)
                static _Atomic uint64_t last_log;
                uint64_t now = SDL_GetTicksNS(), l = atomic_load(&last_log);
                if (!l) atomic_compare_exchange_strong(&last_log, &l, now);
                else if (now - l > 30000000000ull && atomic_compare_exchange_strong(&last_log, &l, now)) {
                    if (pa_own_record()) vk_archive_warm_commit(vk_archive_warm_snapshot());   // (logs the timers)
                    else pa_tm_log();
                }
            }
            if (!play && duty < 100) {   // (TOTK_PRECOMPILE_ARCHIVE_GOVERN=0: idle (100 - duty) / duty times the job's time)
                uint64_t ms = took / 1000000ull * (uint64_t)(100 - duty) / (uint64_t)duty;
                if (ms) SDL_Delay((uint32_t)(ms < 2000 ? ms : 2000));
            }
        }
    }
    bool last = atomic_fetch_sub(&pc_running, 1) == 1;
    if (last && pa_nprogs)
        LOG("[precompile] archive done: %u programs in %.1f s: %d compiled to SPIR-V, %d already cached, %d recorded (built in "
            "full above), %d failed; thread time translate %.1f s, SPIR-V %.1f s\n", pa_nprogs,
            (SDL_GetTicksNS() - (pa_t0 ? pa_t0 : pc_t0)) / 1e9, atomic_load(&pa_compiled), atomic_load(&pa_cached),
            atomic_load(&pa_recorded), atomic_load(&pa_failed), atomic_load(&pa_translate_ns) / 1e9, atomic_load(&pa_spirv_ns) / 1e9);
    if (last && pa_nprogs && pa_own_record()) vk_archive_warm_commit(vk_archive_warm_snapshot());
    if (last) pa_tm_log();
    if (last) {   // (the SPIR-V cache's raw entries from older builds compressed in place, once: spvz.h)
        SetThreadPriority(GetCurrentThread(), PA_PRIO_IDLE);
        size_t raw = 0, packed = 0;
        const uint64_t t0 = SDL_GetTicksNS();
        const int n = spvz_compact("cache/spirv", &raw, &packed);
        if (n)
            LOG("[precompile] SPIR-V cache: %d raw entries compressed in place, %.1f MB -> %.1f MB, in %.1f s\n", n,
                raw / 1048576.0, packed / 1048576.0, (SDL_GetTicksNS() - t0) / 1e9);
    }
    if (last && pa_file && !pa_full) {   // (warm programs were destroyed: nothing points into the pack any more, ~140 MB)
        free(pa_file);
        pa_file = NULL;
        pa_blob = NULL;
        pa_prog = pa_code = NULL;   // (pa_nprogs stays: the progress totals count it; pc_start runs once, so no second
                                    // archive loop can index the freed tables)
    }
    if (last && pa_warmup_mode()) {
        extern void vk_pipeline_cache_save(void);
        vk_pipeline_cache_save();   // (commits the warm-up record too)
        LOG("[precompile] warm-up finished: exiting (TOTK_WARMUP)\n");
        fflush(stdout); fflush(stderr);
        _exit(0);
    }
    return NULL;
}

static void pc_start_workers(void);
static int pc_load_file(const char *path);

// The first start on this PC: a pipeline cache the driver did not accept is not the test (some drivers keep their own):
// another GPU, driver or engine build than the ones that last finished this pass is (cache/precompile_cold.key: vendor,
// device, driver version, pipeline cache UUID, the exe's size and time); then the driver has none of this build's
// pipelines and every recorded program and pipeline (the package's seed and this PC's own) builds from nothing. They
// build on every logical CPU but one instead of the usual 1..6 threads (1 on <= 8 CPUs: a whole seed took minutes
// there, and play met its pipelines unbuilt: held frames), at the workers' usual below-normal priority, so the game's
// threads still come first; done long before the title is left on any core count. The archive warm-up after it and
// every later start: as before. TOTK_PRECOMPILE_COLD=0 never (boot.py: harness boots share the host's cores), =1 always
// (a measurement).
#ifndef _WIN32
#include <unistd.h>   // (readlink)
#endif
static void pc_cold_decide(void) {   // (pc_cold_key: declared with the archive warm-up record, which keys on it too)
    extern size_t g_vk_pcache_loaded;
    int at = snprintf(pc_cold_key, sizeof pc_cold_key, "%04x %04x %08x ", g_vk.props.vendorID, g_vk.props.deviceID,
                      g_vk.props.driverVersion);
    for (int i = 0; i < VK_UUID_SIZE; i++)
        at += snprintf(pc_cold_key + at, sizeof pc_cold_key - at, "%02x", g_vk.props.pipelineCacheUUID[i]);
    char exe[600] = "";
#ifdef _WIN32
    GetModuleFileNameA(NULL, exe, sizeof exe);
#else
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    exe[n > 0 ? n : 0] = 0;
#endif
    struct stat st;
    if (exe[0] && !stat(exe, &st))
        snprintf(pc_cold_key + at, sizeof pc_cold_key - at, " %llx-%llx\n", (unsigned long long)st.st_size,
                 (unsigned long long)st.st_mtime);
    char have[160] = "";
    FILE *f = fopen("cache/precompile_cold.key", "rb");
    if (f) { size_t r = fread(have, 1, sizeof have - 1, f); have[r] = 0; fclose(f); }
    const char *e = getenv("TOTK_PRECOMPILE_COLD");
    const bool changed = strcmp(have, pc_cold_key) != 0;
    pc_cold = e && *e ? strcmp(e, "0") != 0 : changed;
    const char *u = getenv("TOTK_PRECOMPILE_UNLOCKED");
    pc_unlocked = pc_cold && !(u && !strcmp(u, "0"));
    if (pc_unlocked) LOG("[precompile] first-start pass: translating without translate_lock (TOTK_PRECOMPILE_UNLOCKED=0: with it)\n");
    LOG("[precompile] first-start pass %s (GPU / driver / engine build %s; pipeline cache %s%s)\n", pc_cold ? "on" : "off",
        changed ? "new" : "as when it last ran", g_vk_pcache_loaded ? "loaded" : "empty", e && *e ? "; TOTK_PRECOMPILE_COLD" : "");
}
static void pc_cold_done(void) {   // (every recorded program and pipeline built: a later start of this build is warm)
    LOG("[precompile] first-start pass done: %d programs, %d pipelines, %d compute programs in %.1f s on %d threads\n",
        pc_nprogs, pc_npipes, pc_ncomps, (SDL_GetTicksNS() - pc_t0) / 1e9, pc_nthreads);
    extern void vk_pipeline_cache_save(void);
    vk_pipeline_cache_save();   // (in the cache now, should the game end before its own save)
    FILE *f = fopen("cache/precompile_cold.key.tmp", "wb");
    if (f) {
        fputs(pc_cold_key, f);
        fclose(f);
        remove("cache/precompile_cold.key");
        rename("cache/precompile_cold.key.tmp", "cache/precompile_cold.key");
    }
    atomic_store(&pc_cold_running, 0);
}
// Loads cache/precompile.bin (first program request: the heap base the translator bakes in is known by then) and
// starts the workers. A file written for another heap base / gdev window / record layout is set aside as .old.
static void pc_start(void) {
    static bool started;
    if (started) return;
    started = true;
    pc_t0 = SDL_GetTicksNS();
    pc_cold_decide();
    int own = pc_load_file("cache/precompile.bin");
    if (own < 0) LOG("[precompile] no cache/precompile.bin yet: recording this session\n");
    if (own == 0) {
        LOG("[precompile] cache/precompile.bin was written for another heap base / layout: set aside, recording anew\n");
        remove("cache/precompile.bin.old");
        rename("cache/precompile.bin", "cache/precompile.bin.old");
    }
    if (pc_seed_dir()) {
        char path[600];
        snprintf(path, sizeof path, "%s/precompile.bin", pc_seed_dir());
        int np = pc_nprogs, nl = pc_npipes, nc = pc_ncomps;
        int r = pc_load_file(path);
        LOG("[precompile] seed %s: %s, +%d programs, +%d pipelines, +%d compute programs\n", path,
            r < 0 ? "missing" : r ? "loaded" : "another heap base / layout, ignored", pc_nprogs - np, pc_npipes - nl, pc_ncomps - nc);
    }
    pc_start_workers();
}
// one precompile.bin into pc_progs / pc_pipes / pc_comps (records seen before are skipped): 1 loaded, 0 written for
// another heap base / gdev window / record layout, -1 missing or unreadable
static int pc_load_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *d = n > 0 ? malloc((size_t)n) : NULL;
    bool ok = d && fread(d, 1, (size_t)n, f) == (size_t)n;
    fclose(f);
    if (!ok) { free(d); return -1; }
    PcInfo want = {pc_info_heap(), gdev_window_size(), sizeof(PcProg), sizeof(PipeKey)};
    static int cap_p, cap_l, cap_c;
    bool good = false;
    for (int pass = 0; pass < 2; pass++)   // PC_PROG2 records + pipelines first; then PC_PROG ones no PC_PROG2 replaced
    for (long o = 0; o + (long)sizeof(PcHdr) <= n;) {
        PcHdr h;
        memcpy(&h, d + o, sizeof h);
        if (h.magic != PC_MAGIC || o + (long)sizeof h + (long)h.size > n) break;   // (torn tail)
        const uint8_t *pl = d + o + sizeof h;
        if (h.kind == PC_HEADER && h.size == sizeof(PcInfo)) good = !memcmp(pl, &want, sizeof want);
        else if (good && ((pass == 0 && h.kind == PC_PROG2 && h.size == sizeof(PcProg2)) ||
                          (pass == 1 && h.kind == PC_PROG && h.size == sizeof(PcProg)))) {
            if (pc_nprogs == cap_p) { cap_p = cap_p ? cap_p * 2 : 1024; pc_progs = realloc(pc_progs, cap_p * sizeof *pc_progs); }
            memset(&pc_progs[pc_nprogs], 0, sizeof *pc_progs);
            memcpy(&pc_progs[pc_nprogs], pl, h.size);
            uint64_t ck = pc_progs[pc_nprogs].p.ckey;
            if (pc_seen_add(ck)) { if (pass == 0) pc_seen_add(ck ^ PC_SEEN_C1); pc_nprogs++; }
        } else if (good && pass == 0 && h.kind == PC_PIPE && h.size == sizeof(PcPipe)) {
            if (pc_npipes == cap_l) { cap_l = cap_l ? cap_l * 2 : 4096; pc_pipes = realloc(pc_pipes, cap_l * sizeof *pc_pipes); }
            memcpy(&pc_pipes[pc_npipes], pl, sizeof(PcPipe));
            pipekey_norm(&pc_pipes[pc_npipes].k);   // (records written before the fix carry stale slots: dedupe them)
            uint64_t rk = hash_words(pc_pipes[pc_npipes].ckey, &pc_pipes[pc_npipes].k, sizeof(PipeKey));
            if (pc_seen_add(rk ^ 0x5bd1e995ull)) pc_npipes++;
        } else if (good && pass == 0 && h.kind == PC_COMP && h.size == sizeof(PcComp)) {
            if (pc_ncomps == cap_c) { cap_c = cap_c ? cap_c * 2 : 256; pc_comps = realloc(pc_comps, cap_c * sizeof *pc_comps); }
            memcpy(&pc_comps[pc_ncomps], pl, sizeof(PcComp));
            if (pc_seen_add(pc_comps[pc_ncomps].ckey ^ PC_SEEN_CS)) pc_ncomps++;
        }
        o += sizeof h + h.size;
    }
    free(d);
    return good;
}
static void pc_start_workers(void) {
    pa_load_all();
    pa_govern_init();
    int ncpu = SDL_GetNumLogicalCPUCores(), nt = ncpu / 4;
    if (nt < 2) nt = 2;
    if (nt > 6) nt = 6;
    if (ncpu <= 8) nt = 1;   // (Steam Deck: 4 cores / 8 threads, the game's own threads need them)
    // the helpers have cores of their own (E-cores, the CCD without the V-Cache; cpu_place.h, these workers move there):
    // one worker per helper processor, 1..8, since none of them is a core the game runs on (a 12900K: 8, not 6)
    if (pc_own_cores()) { nt = cpu_place_count(CPU_HELPER); nt = nt < 1 ? 1 : nt > 8 ? 8 : nt; }
    pa_nload = nt;   // (before the warm-up mode's count: after "play now" a load gets the usual one)
    if (pa_warmup_mode()) nt = ncpu > 2 ? ncpu - 1 : 1;
    pc_nthreads_normal = nt;
    // (the first-start pass: every core but one, then nt; with the archive alone too: a PC with no seed and no records
    // yet, e.g. a Builder package, warms the game's shaders on the title, not on one worker at 20 % during play)
    if (pc_cold && (pc_nprogs || pc_npipes || pc_ncomps || pa_nprogs)) {
        int all = ncpu > 2 ? ncpu - 1 : 1;
        if (all > nt) nt = all;
        atomic_store(&pc_cold_running, 1);
    }
    // (archive programs left to warm: the governor's bursts (title, focus loss) take every core but one; the extra
    // workers help the title's pass, then wait for those bursts)
    if (atomic_load(&pa_govern) > 0 && pa_nprogs > pa_nwarmed && !pa_warmup_mode()) {
        int all = ncpu > 2 ? ncpu - 1 : 1;
        if (all > nt) nt = all;
    }
    LOG("[precompile] %d programs, %d pipelines, %d compute programs from earlier sessions, %u archive programs: building on %d threads%s\n",
        pc_nprogs, pc_npipes, pc_ncomps, pa_nprogs, nt, nt > pc_nthreads_normal ? (pc_cold ? " (first-start pass; then the usual ones)" : " (the archive's bursts; the usual ones otherwise)") : "");
    if (!pc_nprogs && !pc_npipes && !pa_nprogs && !pc_ncomps) return;
    pc_nthreads = nt;
    atomic_store(&pc_running, nt);
    for (int i = 0; i < nt; i++) { pthread_t t; pthread_create(&t, NULL, pc_worker, (void *)(intptr_t)i); }
}

// 1 while the precompile runs; *done / *total count programs + pipelines + archive programs (device.c title bar,
// hle_osd.cpp banner)
int vk_precompile_progress(int *done, int *total) {
    *total = pc_ncomps + pc_nprogs + pc_npipes + (int)pa_nprogs;
    *done = atomic_load(&pc_comps_done) + atomic_load(&pc_progs_done) + atomic_load(&pc_pipes_done) + atomic_load(&pa_done);
    return atomic_load(&pc_running) > 0;
}
// the recorded programs + pipelines only (the part a hitch-free start needs; mostly cache loads): the in-game banner.
// The archive warm-up after it runs silently in the background (title bar only).
int vk_precompile_recorded_progress(int *done, int *total) {
    *total = pc_ncomps + pc_nprogs + pc_npipes;
    *done = atomic_load(&pc_comps_done) + atomic_load(&pc_progs_done) + atomic_load(&pc_pipes_done);
    return atomic_load(&pc_running) > 0 && *done < *total;
}
// 1 while the first-start pass builds (hle_osd.cpp: its own text over the recorded progress)
int vk_precompile_cold_active(void) { return atomic_load(&pc_cold_running); }
// TOTK_WARMUP (tools/linux/warmup.sh): the archive warm-up's progress for its own banner (hle_osd.cpp)
int vk_precompile_warmup_progress(int *done, int *total) {
    if (!pa_warmup_mode() || !pa_nprogs) return 0;
    *total = (int)pa_nprogs;
    *done = atomic_load(&pa_done);
    return atomic_load(&pc_running) > 0;
}

// ================================================================== feedback snapshots (renderer.m resolve_feedback)
static NvnTexture *feedback_tex[4];
static VkImageView feedback_view[4];
static TexBackend *feedback_snapb[4];   // (an alias read from a snapshot: texture_info_ builds its view there with the texture's own format / swizzle / levels)
static int nfeedback;
// This draw's feedback handling, for the hang ring (2026-10-19 NVIDIA decal hang: which way the stuck draws sampled
// attachments): bits 0-3 live attachments, 4-7 snapshots bound, 8 pass break + barrier for live sampling, 9 a live
// colour target the draw also writes, 10 live depth, 11 snapshot copied now, 12 hazard barrier.
static uint32_t fb_bits;
static uint64_t g_cur_fs_hash;
static NvnTexture *snap_tex[8];
static bool snap_ok[8];
static const char *snap_why[8];
static const char *snap_reason = "?";
// Texture-visibility epoch (LARECOMP_FEEDBACK_SELFCOPY=2): bumped by every game barrier with bits and by every write that
// is not a draw (copies, dispatches, clears: snapshots_invalidate(NULL); CPU re-uploads and our own passes:
// vk_feedback_epoch_bump); a Snap keeps the epoch and frame its copy was taken in.
static uint64_t tex_bar_epoch = 1;
extern uint64_t vk_frame_counter(void);
// (writes outside the guest's command stream: textures.c's CPU re-uploads, our SR / late-latch / post passes)
void vk_feedback_epoch_bump(void) { tex_bar_epoch++; snap_ndgen++; snap_nd_why = "epoch-bump"; }
// (nd false: the caller ends the mirrors it writes itself, see LARECOMP_SNAP_DELTA: clears end their attachments' in clear(),
// a dispatch ends them all only when it stores to images)
static void snapshots_invalidate_(NvnTexture *t, bool nd) {
    if (!t) { tex_bar_epoch++; if (nd) snap_ndgen++, snap_nd_why = snap_reason; }
    for (int i = 0; i < 8; i++) if ((!t || snap_tex[i] == t) && snap_ok[i]) { snap_ok[i] = false; snap_why[i] = snap_reason; }
}
static void snapshots_invalidate(NvnTexture *t) { snapshots_invalidate_(t, true); }

// LARECOMP_VK_PROF: feedback copies per (fragment shader, copied image, what invalidated the last snapshot), printed
// as "[fbcopy]" with each stats window (looking down at the Sky Island cloud sea: ~1170 copies per frame).
typedef struct { uint64_t fs; uint32_t w, h, fmt, levels; const char *why; bool depth; uint64_t n; } FbcKey;
static FbcKey fbc[48];
static int fbc_n;
static uint64_t fbc_other;
static void fbc_note(uint64_t fs, TexBackend *src, const char *why, bool depth) {
    for (int i = 0; i < fbc_n; i++)
        if (fbc[i].fs == fs && fbc[i].w == src->width && fbc[i].h == src->height && fbc[i].fmt == (uint32_t)src->format &&
            fbc[i].why == why && fbc[i].depth == depth) { fbc[i].n++; return; }
    if (fbc_n == 48) { fbc_other++; return; }
    fbc[fbc_n++] = (FbcKey){fs, src->width, src->height, (uint32_t)src->format, src->levels, why, depth, 1};
}
static void fbc_report(void) {
    if (!fbc_n) return;
    for (int i = 1; i < fbc_n; i++)
        for (int j = i; j > 0 && fbc[j].n > fbc[j - 1].n; j--) { FbcKey t = fbc[j]; fbc[j] = fbc[j - 1]; fbc[j - 1] = t; }
    LOG("[fbcopy] top:");
    for (int i = 0; i < fbc_n && i < 8; i++)
        LOG(" fs %016llx %ux%u fmt %u lv %u %s (%s) x%llu;", (unsigned long long)fbc[i].fs, fbc[i].w, fbc[i].h, fbc[i].fmt, fbc[i].levels,
            fbc[i].depth ? "depth" : "color", fbc[i].why ? fbc[i].why : "?", (unsigned long long)fbc[i].n);
    LOG(" (%d keys, %llu untracked)\n", fbc_n, (unsigned long long)fbc_other);
    fbc_n = 0;
    fbc_other = 0;
}

typedef struct { NvnTexture *key; TexBackend copy; uint64_t bar, frm, used, draw; bool mut; uint64_t tag, nd, mfrm; VkImage mimg; } Snap;   // (tag / nd / mfrm / mimg: LARECOMP_SNAP_DELTA, the image it mirrors as of snap_ndgen nd in frame mfrm)   // (mut: made MUTABLE_FORMAT, for alias views)   // (bar / frm: tex_bar_epoch and frame of its last copy, 0 = none; used / draw: snapshot_image's LRU clock and the resolve_feedback call that last used it)
static uint64_t snap_clock, snap_draw;
static Snap snaps[16];
static int nsnaps;
static Snap *snap_last;   // (snapshot_image's slot)

static NvnTexture *shader_texture_object(int stage, ShaderTexture *st_) {
    int slot = ((int)st_->handle_offset - 0x20) / 8;
    uint64_t h = (slot >= 0 && slot < 32) ? st.tex[stage][slot] : 0;
    uint32_t tic = h & 0xFFFFF;
    if (!h || !st.tpool || tic >= (uint32_t)st.tpool->count) return NULL;
    NvnTexture *t = ((NvnTexEntry *)st.tpool->entries)[tic].tex;
    if (t && t->zombie && !t->backend) {
        // finalized while registered (objects.c nvnTextureFinalize): usable only through another object's live
        // rendering at its storage, or (CPU-written texture) by uploading its storage again if that is still readable
        extern bool renderer_texture_has_writer(NvnTexture *t);
        if (!renderer_texture_has_writer(t) && (t->zombie_gpu || !pc_readable(t->addr, tegra_total_size(&t->layout)))) return NULL;
    }
    return t;
}

static bool is_attachment(NvnTexture *t) {
    if (!t) return false;
    if (t == st.rt.depth) return true;
    for (int i = 0; i < st.rt.ncolor; i++) if (t == st.rt.color[i]) return true;
    return false;
}

// Mip levels a feedback snapshot keeps. TotK's bloom renders each level of one 320x180 texture while sampling the level
// next to it (textureLod); a level-0-only copy made every step read level 0 = a blocky box of glow around bright lights
// (torch, user 2026-10-15). LARECOMP_SNAP_ONE_LEVEL=1: level 0 only (the old behaviour).
static uint32_t snap_levels(TexBackend *src) {
    static int one = -1;
    if (one < 0) one = getenv("LARECOMP_SNAP_ONE_LEVEL") != NULL;
    return one || src->samples > 1 || src->levels < 1 ? 1 : src->levels;
}

// Host texel bytes of the uncompressed colour formats an alias snapshot can be viewed in; 0 = not handled (resolve_feedback
// then keeps the live read: a view of another texel size of the copy is invalid)
static uint32_t host_texel_bytes(VkFormat f);
uint32_t vk_host_texel_bytes(VkFormat f) { return host_texel_bytes(f); }
static uint32_t host_texel_bytes(VkFormat f) {
    switch (f) {
    case VK_FORMAT_R8_UNORM: case VK_FORMAT_R8_SNORM: case VK_FORMAT_R8_UINT: case VK_FORMAT_R8_SINT: case VK_FORMAT_R8_SRGB:
        return 1;
    case VK_FORMAT_R8G8_UNORM: case VK_FORMAT_R8G8_SNORM: case VK_FORMAT_R8G8_UINT: case VK_FORMAT_R8G8_SINT: case VK_FORMAT_R8G8_SRGB:
    case VK_FORMAT_R16_UNORM: case VK_FORMAT_R16_SNORM: case VK_FORMAT_R16_UINT: case VK_FORMAT_R16_SINT: case VK_FORMAT_R16_SFLOAT:
    case VK_FORMAT_R5G6B5_UNORM_PACK16: case VK_FORMAT_B5G6R5_UNORM_PACK16: case VK_FORMAT_R4G4B4A4_UNORM_PACK16:
    case VK_FORMAT_B4G4R4A4_UNORM_PACK16: case VK_FORMAT_R5G5B5A1_UNORM_PACK16: case VK_FORMAT_B5G5R5A1_UNORM_PACK16:
    case VK_FORMAT_A1R5G5B5_UNORM_PACK16:
        return 2;
    case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R8G8B8A8_SNORM: case VK_FORMAT_R8G8B8A8_UINT: case VK_FORMAT_R8G8B8A8_SINT:
    case VK_FORMAT_R8G8B8A8_SRGB: case VK_FORMAT_B8G8R8A8_UNORM: case VK_FORMAT_B8G8R8A8_SRGB: case VK_FORMAT_A8B8G8R8_UNORM_PACK32:
    case VK_FORMAT_A8B8G8R8_SRGB_PACK32: case VK_FORMAT_A2B10G10R10_UNORM_PACK32: case VK_FORMAT_A2B10G10R10_UINT_PACK32:
    case VK_FORMAT_A2R10G10B10_UNORM_PACK32: case VK_FORMAT_R16G16_UNORM: case VK_FORMAT_R16G16_SNORM: case VK_FORMAT_R16G16_UINT:
    case VK_FORMAT_R16G16_SINT: case VK_FORMAT_R16G16_SFLOAT: case VK_FORMAT_R32_UINT: case VK_FORMAT_R32_SINT: case VK_FORMAT_R32_SFLOAT:
    case VK_FORMAT_B10G11R11_UFLOAT_PACK32: case VK_FORMAT_E5B9G9R9_UFLOAT_PACK32:
        return 4;
    case VK_FORMAT_R16G16B16A16_UNORM: case VK_FORMAT_R16G16B16A16_SNORM: case VK_FORMAT_R16G16B16A16_UINT:
    case VK_FORMAT_R16G16B16A16_SINT: case VK_FORMAT_R16G16B16A16_SFLOAT: case VK_FORMAT_R32G32_UINT: case VK_FORMAT_R32G32_SINT:
    case VK_FORMAT_R32G32_SFLOAT:
        return 8;
    case VK_FORMAT_R32G32B32A32_UINT: case VK_FORMAT_R32G32B32A32_SINT: case VK_FORMAT_R32G32B32A32_SFLOAT:
        return 16;
    default:
        return 0;
    }
}
static TexBackend *snapshot_image(NvnTexture *t, TexBackend *src, bool mut) {
    Snap *s = NULL;
    for (int i = 0; i < nsnaps; i++) if (snaps[i].key == t) s = &snaps[i];
    uint32_t nlv = snap_levels(src);
    // (LARECOMP_SNAP_DELTA: the slot that mirrors this image wins over this key's own one, which another snapshot of the
    // image (another texture object, an alias view) may have left behind: TotK samples its G-buffer through several)
    for (int i = 0; snap_delta_on() && src->snap_tag && i < nsnaps; i++)
        if (&snaps[i] != s && snaps[i].tag == src->snap_tag && snaps[i].mimg == src->image && snaps[i].copy.image && snaps[i].draw != snap_draw &&
            snaps[i].copy.levels == nlv && (!mut || snaps[i].mut)) {
            if (s && s->draw != snap_draw) s->key = NULL;   // (its image stays for the next key of its size)
            s = &snaps[i]; s->key = t;
            break;
        }
    if (!s) {
        // A new key takes the least recently used slot whose image already has the source's size / format / levels /
        // samples, so the image is reused, not reallocated: TotK re-creates its targets every frame (a new object each
        // time), and the round robin below handed each one a slot of another size, 71 dedicated vkAllocateMemory +
        // vkFreeMemory of 6.5 MB snapshots in 56 frames (render thread, NtGdiDdDDI*Allocation). Never a slot this draw
        // already uses. LARECOMP_SNAP_REUSE=0: round robin only (old).
        static int reuse = -1;
        if (reuse < 0) { const char *e = getenv("LARECOMP_SNAP_REUSE"); reuse = !e || strcmp(e, "0"); }
        // (LARECOMP_SNAP_DELTA: a slot that mirrored an image this frame goes last: taking it ends that mirror; a new slot first)
        const uint64_t fr_now = vk_frame_counter();
        for (int pass = 0; reuse && !s && pass < 2; pass++) {
            for (int i = 0; i < nsnaps; i++) {
                const TexBackend *c = &snaps[i].copy;
                const bool live = snap_delta_on() && snaps[i].tag && snaps[i].mfrm == fr_now;
                if (c->image && snaps[i].draw != snap_draw && c->width == src->width && c->height == src->height && c->format == src->format &&
                    c->levels == nlv && c->samples == src->samples && c->aspect == src->aspect && (!mut || snaps[i].mut) && live == (pass == 1) &&
                    (!s || snaps[i].used < s->used))
                    s = &snaps[i];
            }
            if (!s && !pass && snap_delta_on() && nsnaps < 16) s = &snaps[nsnaps++];
        }
        if (!s && nsnaps < 16) s = &snaps[nsnaps++];
        for (int i = 0; !s && i < 16; i++) {   // (least recently used, not this draw's)
            if (snaps[i].draw != snap_draw && (!s || snaps[i].used < s->used)) s = &snaps[i];
        }
        if (!s) { static int rr; s = &snaps[rr++ & 15]; }   // (16 snapshots in one draw: cannot happen, nfeedback <= 4)
        s->key = t;
        s->bar = s->frm = 0;
    }
    s->used = ++snap_clock; s->draw = snap_draw;
    TexBackend *d = &s->copy;
    const bool smk = smark_on() && nlv == 1 && src->samples == 1 && smark_kind(src->format) >= 0 &&
                     !(src->aspect & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT));
    if (d->image && (d->width != src->width || d->height != src->height || d->format != src->format || d->levels != nlv ||
                     d->samples != src->samples || d->aspect != src->aspect || (mut && !s->mut) ||
                     (smk && !(d->usage & VK_IMAGE_USAGE_STORAGE_BIT)))) {
        vk_views_forget(d);  // (resolution change, or a mutable image now needed)
        s->tag = 0;
        vk_defer_image(d->image, d->mem);
        d->image = VK_NULL_HANDLE;
        d->mem = VK_NULL_HANDLE;
    }
    if (!d->image) {
        VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = src->format,
                                .extent = {src->width, src->height, 1}, .mipLevels = nlv, .arrayLayers = 1,
                                .samples = (VkSampleCountFlagBits)src->samples, .tiling = VK_IMAGE_TILING_OPTIMAL,
                                .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                .flags = mut ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT : 0};
        if (smk) {   // (LARECOMP_SNAP_MARK: written through an r32ui / rg32ui / rgba32ui storage view by the tile copy)
            ci.usage |= VK_IMAGE_USAGE_STORAGE_BIT;
            ci.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
        }
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        VK_CHECK(vmaCreateImage(g_vk.vma, &ci, &ai, &d->image, &d->mem, NULL));
        s->mut = mut || smk;
        d->usage = ci.usage; d->cflags = ci.flags;
        d->format = src->format; d->width = src->width; d->height = src->height; d->depth = 1; d->layers = 1; d->levels = nlv;
        d->samples = src->samples; d->aspect = src->aspect; d->base_type = VK_IMAGE_VIEW_TYPE_2D;
        vk_image_init_layout(vk_cmd(), d->image, d->aspect);
    }
    s->bar = tex_bar_epoch; s->frm = vk_frame_counter();   // (the caller copies the whole image into it now, or its delta)
    snap_last = s;
    return d;
}

// ================================================================== pause-screen capture (TotK workaround) User
// 2026-10-15: "the frozen ui still makes everything black. also happens whjen switching weapons or items". When a
// pause UI opens (ability wheel, weapon / item switch), the game draws NO 3D scene in that first frame and then copies
// the canvas (fs 8d0bd2a0, cond-gated, into a canvas-sized RGB10A2 texture) as the frozen background it blurs and
// shows behind the menu. On Switch that copy shows the world; here it was the HUD over black. WHY the game skips the
// scene in that frame on our side is unknown (not pacing; not cross-queue order: one queue). Workaround: the canvas is
// copied just before its first clear of each frame (= the previous frame, scene + HUD), and a canvas-sized copy OF the
// canvas into a texture that is not presented, in a frame with no scene yet (< 1000 draws), samples that copy instead.
// "[pause]..." logs each use. LARECOMP_NO_PAUSE_KEEP=1: off.
static NvnTexture *pk_canvas;          // what the present copy (fs 8d0bd2a0 into a presented texture) reads
static TexBackend pk_keep;
// (2026-10-17, user: "the background goes back to that exact snapshot... a lot darker... eventually its just black": the
// kept canvas was black: the frames before a capture can hold no scene, and the wheel's own blurred background, a
// scalable texture, counted as "scene", so dimmed wheel frames were kept and captured again.) pk_keep is now the canvas
// of the last frame the scene reached it, kept at the next clear. Scene = the docked upscale or any draw into the canvas
// after >= 1000 draws in the frame: handheld mode (the Deck) composites its 1280x720 scene with another shader, and at
// scale 1 nothing is scalable (why the Deck and PC 1x were always black). Deck d_wheel_fix 2 s windows: scene frames
// ~3400 draws each, weapon-menu frames ~180, wheel / capture frames fewer.
static bool pk_scene_since_clear;       // the scene reached the canvas since its last clear
static bool pk_valid;                   // pk_keep holds such a canvas
static uint64_t pk_scene_frame = ~0ull; // present count of the last scene draw into the canvas (see pk_draw)
static uint64_t pk_cap_frame = ~0ull;   // present count of the first frame with a capture copy since the scene left (~0: none yet)
// (2026-10-30: every icon smeared into fading copies toward the zoom centre.) The Purah Pad's map passes the 1000-draw scene
// test, and copies the canvas into a 1920x1080 f0x3d target before drawing it, every frame: each map frame was kept as "the
// scene" and substituted into the next frame's copy, so the finished map, icons and blur included, fed back into itself. A
// canvas drawn in a frame that copied the canvas BEFORE drawing it is a menu that captures every frame, not the world: never
// substitute it (the copy then reads the canvas as the game left it, as on Switch).
static uint64_t pk_precap_frame = ~0ull; // present count of the last capture copy made before the scene reached the canvas
static bool pk_scene_menu;              // the scene since the last clear was drawn in such a frame
static bool pk_keep_menu;               // pk_keep holds such a canvas
static NvnTexture *pk_subst_tex;        // (this draw only) sample pk_keep instead of this texture
static VkImageView pk_subst_view;
static bool pk_on(void) { static int on = -1; if (on < 0) on = getenv("LARECOMP_NO_PAUSE_KEEP") == NULL; return on; }

static void pk_take(NvnTexture *t) {   // (CMD_CLEAR_COLOR, before the clear)
    if (!pk_on() || t != pk_canvas) return;
    bool scene = pk_scene_since_clear;
    pk_scene_since_clear = false;
    if (!scene) return;
    TexBackend *src = vk_tex(t, true);
    if (!src || !src->image || src->samples > 1 || (src->aspect & VK_IMAGE_ASPECT_DEPTH_BIT)) return;
    TexBackend *d = &pk_keep;
    if (d->image && (d->width != src->width || d->height != src->height || d->format != src->format)) {
        vk_views_forget(d);
        vk_defer_image(d->image, d->mem);
        d->image = VK_NULL_HANDLE;
        d->mem = VK_NULL_HANDLE;
    }
    if (!d->image) {
        VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = src->format,
                                .extent = {src->width, src->height, 1}, .mipLevels = 1, .arrayLayers = 1,
                                .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                                .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        VK_CHECK(vmaCreateImage(g_vk.vma, &ci, &ai, &d->image, &d->mem, NULL));
        d->format = src->format; d->width = src->width; d->height = src->height; d->depth = 1; d->layers = 1; d->levels = 1;
        d->samples = 1; d->aspect = src->aspect; d->base_type = VK_IMAGE_VIEW_TYPE_2D;
        vk_image_init_layout(vk_cmd(), d->image, d->aspect);
    }
    vk_end_pass();
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);
    VkImageCopy rg = {.srcSubresource = {src->aspect, 0, 0, 1}, .dstSubresource = {src->aspect, 0, 0, 1}, .extent = {src->width, src->height, 1}};
    vkCmdCopyImage(cb, src->image, VK_IMAGE_LAYOUT_GENERAL, d->image, VK_IMAGE_LAYOUT_GENERAL, 1, &rg);
    vk_full_barrier(cb);   // (the clear that follows writes the canvas)
    st.rt_dirty = true;
    pk_valid = true;
    pk_keep_menu = pk_scene_menu;
    static int plog = -1;
    if (plog < 0) plog = getenv("LARECOMP_PAUSE_DUMP") != NULL;
    if (plog && plog++ < 400) {
        extern uint64_t g_draws_frame;
        LOG("[pause] take: frame %llu, canvas %ux%u kept after %llu draws\n", (unsigned long long)renderer_present_count(), src->width, src->height,
            (unsigned long long)g_draws_frame);
    }
}

// Frames since the scene last reached the canvas (render thread; hle_osd.cpp shows the Graphics menu hint while a pause /
// System menu holds the picture); 0 = not known yet.
uint64_t vk_scene_idle_frames(void) { return pk_scene_frame == ~0ull ? 0 : renderer_present_count() - pk_scene_frame; }
// hle_hid.cpp screen_track: presents since the docked upscale last drew into the canvas, ~0 if it never has. A busy menu
// passes the 1000-draw test above (the Purah Pad's zoomed-out Surface map with its icons, an inventory tab switch), the
// upscale only runs for the world. TotK's scene -> canvas passes: EASU (fs 32845c90c668fdbb) at 1600x900; at the dynamic
// resolution's 1280x720 tier fs 6dc5011326ac42a2 instead. Before it was known, SR never ran and "the upscale" looked idle
// for as long as the game held that tier.
static inline bool is_upscale_fs(uint64_t h) { return h == 0x32845c90c668fdbbull || h == 0x6dc5011326ac42a2ull; }
static uint64_t pk_upscale_frame = ~0ull;
uint64_t vk_upscale_idle_frames(void) { return pk_upscale_frame == ~0ull ? ~0ull : renderer_present_count() - pk_upscale_frame; }

static void pk_draw(Prog *pr) {   // (draw_, before the fragment textures resolve)
    pk_subst_tex = NULL;
    if (!pk_on() || !st.rt.ncolor || !st.rt.color[0]) return;
    extern uint64_t g_draws_frame;
    if (pk_canvas && st.rt.color[0] == pk_canvas && (is_upscale_fs(g_cur_fs_hash) || g_draws_frame >= 1000)) {
        pk_scene_frame = renderer_present_count();   // (the scene arriving)
        if (is_upscale_fs(g_cur_fs_hash)) pk_upscale_frame = pk_scene_frame;
        pk_scene_menu = pk_precap_frame == pk_scene_frame;
        pk_scene_since_clear = true;
        pk_cap_frame = ~0ull;
    }
    if (g_cur_fs_hash != 0x8d0bd2a0046fd06aull || pr->fi->ntex < 1) return;
    NvnTexture *src = shader_texture_object(1, &pr->fi->tex[0]), *dst = st.rt.color[0];
    // (the capture frame itself is presented black at 144 fps, one frame per wheel opening. Presenting the kept canvas from
    // this copy did not change it, so something later in that frame writes the image.)
    if (renderer_texture_presented(dst)) { pk_canvas = src; return; }   // the present copy: learn the canvas
    if (!src || src != pk_canvas || dst == src) return;
    // the capture copy: a 16:9 target at least as large as the canvas (docked: 1920x1080 = the canvas; handheld, Deck
    // d_wheel_fix2: the 1280x720 canvas into a 1920x1080 capture texture). Not the title screen's 520x40 strips.
    float da = dst->height ? (float)dst->width / (float)dst->height : 0;
    bool capture = da > 1.74f && da < 1.81f && dst->width >= src->width && dst->height >= src->height;
    // only in the first frame WITH A CAPTURE COPY since the scene left (all of its capture copies: 2 in the user's frame
    // 3824). Menus that copy the canvas every frame (user's 144 fps session 16:5x: frames 5142+ copies into f0x3d + f0x3f,
    // 37 / 355 draws) keep their own copies after that, as before the 2026-10-17 marker change. (Not "the first frame
    // without a scene": at 144 fps the wheel opens with a 2-draw frame (72x72 copy) and captures in the next one: not
    // substituted, black.)
    uint64_t now = renderer_present_count();
    if (capture && !pk_scene_since_clear) pk_precap_frame = now;
    // (the wheel captures <= 2 frames after the scene left; later first captures are titles / loads: Deck d_menu_fix5 frame 393)
    if (capture && !pk_scene_since_clear && pk_cap_frame == ~0ull && now - pk_scene_frame <= 3) pk_cap_frame = now;
    bool first = pk_cap_frame == now;
    if (!capture || !first || !pk_keep.image || !pk_valid || pk_keep_menu || pk_scene_since_clear) {
        static uint32_t seen[16];   // (a canvas copy in a frame with no scene that is not substituted: once per target size)
        static int nseen;
        uint32_t key = dst->width << 16 | dst->height;
        bool known = false;
        for (int i = 0; i < nseen; i++) if (seen[i] == key) known = true;
        if (!pk_scene_since_clear && first && !known && nseen < 16) {
            seen[nseen++] = key;
            LOG("[pause] frame %llu: canvas %ux%u copy into %ux%u f%#x after %llu draws NOT substituted (%s)\n",
                (unsigned long long)renderer_present_count(), src->width, src->height, dst->width, dst->height, dst->format,
                (unsigned long long)g_draws_frame, !capture ? "not a capture-sized target" :
                pk_keep_menu && pk_valid ? "the kept canvas is a menu frame that copies the canvas: it would feed back" : "no kept scene canvas yet");
        }
        return;
    }
    static const VkComponentMapping id = {0};
    pk_subst_tex = src;
    pk_subst_view = vk_image_view(&pk_keep, VK_IMAGE_VIEW_TYPE_2D, pk_keep.format, 0, 1, 0, 1, id, pk_keep.aspect);
    static int nl;
    static uint64_t nsub;   // (past the 40-line cap a per-frame substitution looked like a one-off: the map zoom blur)
    nsub++;
    if (nl++ < 40)
        LOG("[pause] frame %llu: canvas copy into %ux%u f%#x after %llu draws (no scene this frame) -> uses the previous frame\n",
            (unsigned long long)renderer_present_count(), dst->width, dst->height, dst->format, (unsigned long long)g_draws_frame);
    else if ((nsub & 255) == 0)
        LOG("[pause] frame %llu: %llu canvas copies substituted so far\n", (unsigned long long)renderer_present_count(), (unsigned long long)nsub);
    static int pdump = -1;   // LARECOMP_PAUSE_DUMP=1: the kept canvas of the first 4 substitutions -> build/pk_keep_<frame>.png
    if (pdump < 0) pdump = getenv("LARECOMP_PAUSE_DUMP") != NULL;
    if (pdump && pdump++ <= 4) {
        extern void renderer_save_texture_tex(TexBackend *b, const char *path);
        char path[64];
        snprintf(path, sizeof path, "build/pk_keep_%llu.png", (unsigned long long)renderer_present_count());
        renderer_save_texture_tex(&pk_keep, path);
        st.rt_dirty = true;
    }
}

// (debug frames) a depth image's depth values as a greyscale PGM, stretched over its own min..max
static void debug_save_depth(TexBackend *b, const char *path) {
    if (!b || !b->image || !(b->aspect & VK_IMAGE_ASPECT_DEPTH_BIT)) return;
    bool d16 = b->format == VK_FORMAT_D16_UNORM || b->format == VK_FORMAT_D16_UNORM_S8_UINT;
    bool d24 = b->format == VK_FORMAT_X8_D24_UNORM_PACK32 || b->format == VK_FORMAT_D24_UNORM_S8_UINT;
    if (!d16 && !d24 && b->format != VK_FORMAT_D32_SFLOAT_S8_UINT && b->format != VK_FORMAT_D32_SFLOAT) return;
    uint64_t n = (uint64_t)b->width * b->height;
    VkSpan out = vk_readback(n * 4);
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);
    VkBufferImageCopy c = {.bufferOffset = out.off, .imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1},
                           .imageExtent = {b->width, b->height, 1}};
    vkCmdCopyImageToBuffer(cb, b->image, VK_IMAGE_LAYOUT_GENERAL, out.buf, 1, &c);
    renderer_flush();
    float *d = out.ptr;   // (depth-aspect copies: D16 = 2 bytes per texel, D24 = low 24 bits of 4, D32 = float)
    if (d16) for (uint64_t i = n; i-- > 0;) d[i] = ((const uint16_t *)out.ptr)[i] / 65535.0f;
    else if (d24) for (uint64_t i = 0; i < n; i++) d[i] = (((const uint32_t *)out.ptr)[i] & 0xFFFFFF) / 16777215.0f;
    float lo = 1e30f, hi = -1e30f;
    for (uint64_t i = 0; i < n; i++) { if (d[i] < lo) lo = d[i]; if (d[i] > hi) hi = d[i]; }
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P5 %u %u 255\n", b->width, b->height);
    for (uint64_t i = 0; i < n; i++) fputc(hi > lo ? (int)((d[i] - lo) / (hi - lo) * 255.0f) : 0, f);
    fclose(f);
    LOG("[vk] saved %s (%ux%u depth %.6f..%.6f)\n", path, b->width, b->height, lo, hi);
}

static VkImageAspectFlags fb_live_aspects;   // aspects of the bound attachments this draw samples live (draw_: feedback loop state)
// Depth twins (textures.c) of the images a draw / dispatch compare-samples: copied before the pass begins when their
// creation or a write outside a pass (copy, upload, clear) left them stale. Passes into them refresh them as they
// end (blk_copy_back). Feedback-substituted reads keep their own view.
static void twins_refresh(ShaderInfo *const *infos, const int *stages, int n, bool fb) {
    extern NvnTexture *renderer_texture_alias_writer(NvnTexture *t);
    for (int sg = 0; sg < n; sg++)
        for (int i = 0; i < infos[sg]->ntex; i++) {
            ShaderTexture *stx = &infos[sg]->tex[i];
            if (!stx->shadow || stx->is_image) continue;
            NvnTexture *t0 = shader_texture_object(stages[sg], stx);
            if (!t0) continue;
            bool subst = false;
            for (int k = 0; fb && k < nfeedback; k++) subst |= feedback_tex[k] == t0;
            NvnTexture *w = renderer_texture_alias_writer(t0);
            TexBackend *b = (w ? w : t0)->backend;
            // (an attachment of the open pass is refreshed as the pass ends; splitting the pass for it on every draw
            // would not help: its begin_pass marks the twin stale again)
            if (subst || !b || is_attachment(w ? w : t0) || !vk_depth_twin_stale(b)) continue;
            vk_end_pass();
            vk_depth_twin_copy(vk_cmd(), b);
            st.rt_dirty = true;
        }
}

// LARECOMP_SELFW_STATS=1: the draws that sample a colour target they also write, per fragment program: count, the
// texture slot, the target, how much of it the scissor leaves (the region a copy limited to the draw would take) and
// whether the target blends. Logged every 30 s, the 24 most frequent: which programs an own-pixel read or a region copy
// must serve instead of SELFCOPY's full-target copy (4K Kakariko 129.5 -> 29.8 fps).
static uint64_t selfw_copies, selfw_reused;   // (self-write draws' snapshot copies taken / shared: per-frame lines below)
static void selfw_note(Prog *pr, int slot, const NvnTexture *t, int rt) {
    {   // per frame (the first 16, then every 300th): self-write draws, the copies a SELFCOPY mode took for them, reuses
        static uint64_t fr = ~0ull, nd, c0, r0;
        const uint64_t f = vk_frame_counter();
        if (f != fr) {
            if (fr != ~0ull && (fr < 16 || fr % 300 == 0))
                LOG("[selfw] frame %llu: %llu self-write draws, %llu copies, %llu reused\n", (unsigned long long)fr, (unsigned long long)nd,
                    (unsigned long long)(selfw_copies - c0), (unsigned long long)(selfw_reused - r0));
            fr = f; nd = 0; c0 = selfw_copies; r0 = selfw_reused;
        }
        nd++;
    }
    static struct { Prog *pr; uint64_t fs, n, full, area_pm; int slot, rt; uint32_t fmt, w, h; bool blend; } tab[512];
    static uint64_t last_ns, total;
    unsigned h = (unsigned)(((uintptr_t)pr >> 4) * 0x9E3779B1u) >> 23;
    while (tab[h].pr && (tab[h].pr != pr || tab[h].slot != slot)) h = (h + 1) & 511;
    if (!tab[h].pr) {
        tab[h].pr = pr; tab[h].slot = slot; tab[h].rt = rt; tab[h].fs = prog_code_hash(pr, 1);
        tab[h].fmt = t->format; tab[h].w = t->width; tab[h].h = t->height;
    }
    tab[h].blend |= (st.color_state[0] >> rt) & 1;
    int64_t sx = st.scissor[0][0], sy = st.scissor[0][1], sw = st.scissor[0][2], sh = st.scissor[0][3];
    int64_t x0 = sx < 0 ? 0 : sx, y0 = sy < 0 ? 0 : sy, x1 = sw <= 0 ? t->width : sx + sw, y1 = sh <= 0 ? t->height : sy + sh;
    if (x1 > t->width) x1 = t->width;
    if (y1 > t->height) y1 = t->height;
    uint64_t pm = x1 > x0 && y1 > y0 && t->width && t->height ? (uint64_t)((x1 - x0) * (y1 - y0)) * 1000 / ((uint64_t)t->width * t->height) : 0;
    tab[h].n++; tab[h].area_pm += pm; tab[h].full += pm >= 999; total++;
    uint64_t now = SDL_GetTicksNS();
    static bool at8;   // (also once at frame 8: a replay ends long before 30 s)
    const bool f8 = !at8 && vk_frame_counter() >= 8;
    if (now - last_ns < 30000000000ull && !f8) return;
    if (f8) at8 = true;
    last_ns = now;
    int idx[24], ni = 0;
    bool taken[512] = {0};
    for (; ni < 24; ni++) {   // (the 24 biggest counts, one pass each)
        int best = -1;
        for (int k = 0; k < 512; k++)
            if (tab[k].pr && !taken[k] && (best < 0 || tab[k].n > tab[best].n)) best = k;
        if (best < 0) break;
        taken[best] = true;
        idx[ni] = best;
    }
    LOG("[selfw] %llu same-draw colour read + write draws so far, top programs:\n", (unsigned long long)total);
    for (int j = 0; j < ni; j++) {
        const typeof(tab[0]) *e = &tab[idx[j]];
        LOG("[selfw]   fs %016llx (code %llx) slot %d -> rt %d %ux%u fmt %#x%s: %llu draws, scissor %.1f %% of the target on average, full in %llu\n",
            (unsigned long long)e->fs, (unsigned long long)e->pr->code[1], e->slot, e->rt, e->w, e->h, e->fmt, e->blend ? " blended" : "", (unsigned long long)e->n,
            e->area_pm / 10.0 / (double)e->n, (unsigned long long)e->full);
    }
}
static void resolve_feedback(Prog *pr) {
    nfeedback = 0;
    snap_draw++;   // (snapshot_image: slots this draw uses are not handed to another key within it)
    fb_bits = 0;
    fb_live_aspects = 0;
    static int off = -1;
    if (off < 0) off = getenv("LARECOMP_NO_FEEDBACK") != NULL;
    if (off) return;
    ShaderInfo *infos[2] = {pr->vi, pr->fi};
    int bm = barrier_mode();
    bool fb_done = false;   // a full barrier was recorded for this draw already (one covers every sampled image)
    bool snap_bar = false;  // the barrier before this draw's snapshot copies is recorded (the next copies need none)
    int pe = bm == BAR_PASS ? pass_elide() : 0;
    // (LARECOMP_PASS_ELIDE=2: a draw in a small pass that began without the barrier, or the first draw of one about to;
    // begin_pass decides with the same small_pass_elidable, after this draw's own marks below)
    bool check = bm == BAR_TRACKED ||
                 (pe == 2 && (in_pass && !st.rt_dirty ? pass_elided
                                                      : !pass_barrier_needed && !pass_barrier_fresh && !pass_barrier_frag && pass_small()));
    bool wr_g = false, rd_g = false;   // (storage / image writes and reads of this draw: hazard modes only, off the default path)
    if (bm == BAR_TRACKED || pe == 2) {
        wr_g = pr->vi->writes_gmem || pr->fi->writes_gmem;
        rd_g = pr->vi->uses_gmem || pr->fi->uses_gmem;
        for (int sg = 0; sg < 2; sg++)
            for (int i = 0; i < infos[sg]->ntex; i++) if (infos[sg]->tex[i].is_image) wr_g = rd_g = true;
    }
    if (check) {
        // (read after write; and, a storage / image write after storage reads since the last full barrier)
        bool hz = (gmem_wr_epoch == bar_epoch && rd_g) || (pe == 2 && wr_g && gmem_rd_epoch == bar_epoch);
        for (int sg = 0; sg < 2 && !hz; sg++)
            for (int i = 0; i < infos[sg]->ntex && !hz; i++) {
                if (infos[sg]->tex[i].is_image) { hz = gmem_wr_epoch == bar_epoch; continue; }
                NvnTexture *t = shader_texture_object(sg, &infos[sg]->tex[i]);
                if (t && !is_attachment(t) && wr_hazard(t)) hz = true;
            }
        if (hz) {
            vk_end_pass();
            VkCommandBuffer cb = vk_cmd();
            gp_mark(cb, "hazard barrier");
            vk_full_barrier(cb);
            pass_barrier_needed = false;
            pass_barrier_fresh = true;
            st.rt_dirty = true;
            fb_done = true;
            stat_hazard_barriers++;
            fb_bits |= 1u << 12;
        }
    }
    for (int sg = 0; sg < 2; sg++)
        for (int i = 0; i < infos[sg]->ntex; i++) {
            NvnTexture *t0 = shader_texture_object(sg, &infos[sg]->tex[i]);
            // t0 may sample another object's image over the same storage (textures.c alias: TotK samples its G-buffer
            // through second texture objects); that image's attachment object is what this pass writes. Missed before
            // 2026-10-19: those reads got no barrier while the pass wrote the image.
            extern NvnTexture *renderer_texture_alias_writer(NvnTexture *t);
            NvnTexture *t = t0 && !is_attachment(t0) ? renderer_texture_alias_writer(t0) : t0;
            if (!is_attachment(t)) continue;
            bool alias = t != t0;
            stat_fb_alias += alias;
            bool have = false;
            for (int k = 0; k < nfeedback; k++) have |= feedback_tex[k] == t0;
            if (have || nfeedback == 4) continue;
            static int nosnap = -1;
            if (nosnap < 0) nosnap = getenv("LARECOMP_NO_SNAPSHOT_REUSE") != NULL;
            int si = -1;
            for (int k = 0; k < 8; k++) if (snap_tex[k] == t) si = k;
            bool self_write = t == st.rt.depth && st.ds[0] && st.ds[1];
            // A colour target this draw also writes: same-draw read + write is a data race in Vulkan even in a declared
            // feedback loop. LARECOMP_FEEDBACK_SELFCOPY=1 samples a copy for those (A/B, 2026-10-19 NVIDIA decal hang).
            bool self_write_c = false;
            for (int k = 0; k < st.rt.ncolor; k++)
                if (st.rt.color[k] == t && ((pr->fi->out_mask & st.channel_mask) >> (k * 4) & 0xF)) self_write_c = true;
            stat_fb_selfw += self_write_c;
            if (self_write_c && smark_nfs < 256 && smark_on() && !smark_fs_has(pr->code[1])) smark_fs[smark_nfs++] = pr->code[1];   // (LARECOMP_SNAP_MARK)
            static int selfw_stats = -1;
            if (selfw_stats < 0) selfw_stats = getenv("LARECOMP_SELFW_STATS") && *getenv("LARECOMP_SELFW_STATS") == '1';
            if (self_write_c && selfw_stats) {
                int rk = 0;
                while (rk < st.rt.ncolor && st.rt.color[rk] != t) rk++;
                selfw_note(pr, i, t, rk);   // (i: the program's texture index, as the GLSL names it)
            }
            static int selfcopy = -1;
            if (selfcopy < 0) {  // (was getenv() != NULL: =0 turned it on too)
                const char *e = getenv("LARECOMP_FEEDBACK_SELFCOPY");
                // (default 2 since the controlled Fire Temple repro; =0: live same-draw reads, the old default)
                selfcopy = !e || !*e ? 2 : *e == '0' ? 0 : *e == '2' ? 2 : 1;
                if (selfcopy) LOG("[vk] same-draw colour read + write: sampled from a copy (LARECOMP_FEEDBACK_SELFCOPY=%d)\n", selfcopy);
            }
            TexBackend *src = vk_tex(t, true);
            Snap *existing = NULL;
            for (int k = 0; k < nsnaps; k++) if (snaps[k].key == t && snaps[k].copy.image) existing = &snaps[k];
            if (existing) { existing->used = ++snap_clock; existing->draw = snap_draw; }   // (bound by this draw: not another key's)
            VkImageAspectFlags sa = src->aspect & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : src->aspect;
            static const VkComponentMapping id = {0};
            // Colour attachments are sampled LIVE, like Eden (BarrierFeedbackLoop: end the render pass, barrier, sample the
            // attachment itself) instead of copying the whole image per draw. TotK's G-buffer decals read the targets they
            // write: looking down at the Sky Island = ~575 copies of 1600x900 images per frame, 58 % of the GPU frame. The
            // pass break is needed only after a write since the last one (snap_ok). LARECOMP_FEEDBACK_COPY=1: copy into a
            // snapshot (the old way; depth attachments always do).
            static int fcopy = -1;
            if (fcopy < 0) fcopy = getenv("LARECOMP_FEEDBACK_COPY") != NULL;
            // Depth attachments a draw samples WITHOUT writing depth (2026-10-17, Deck d24: 6 copies of the 1280x720 D32 per
            // frame at 150 us each for fs dd9edc34 / 8c7a04f0) are sampled live too; a draw that writes the depth it samples
            // still copies. LARECOMP_DEPTH_FEEDBACK_COPY=1: always copy depth (old).
            static int dcopy = -1;
            if (dcopy < 0) dcopy = getenv("LARECOMP_DEPTH_FEEDBACK_COPY") != NULL;
            // LARECOMP_COLOR_FEEDBACK_COPY=1: colour targets always copy, depth as above (A/B, 2026-10-19 decal hang)
            static int ccopy = -1;
            if (ccopy < 0) ccopy = getenv("LARECOMP_COLOR_FEEDBACK_COPY") != NULL;
            // (NVN formats compared: a depth / stencil substitute's alias is not "another format") (live sampling needs
            // the feedback-loop declaration, g_vk.feedback_loop: without its three extensions a bound attachment sampled
            // in GENERAL is undefined, so those drivers copy) Another-format alias of a bound attachment (TotK samples its
            // G-buffer through second texture objects): live only with the feedback-loop declaration. Without it (AMD
            // Windows: only the layout extension: kak.cap 1014 alias reads a frame) a live read while the pass writes the
            // image is undefined (VUID-vkCmdDraw-None-06538): those take the snapshot, made MUTABLE_FORMAT and viewed in
            // the alias's format (same storage, same texel size). A depth / colour alias pair stays live (no copy between
            // the aspects).
            const VkFormat afmt = alias ? vk_format(t0->format) : src->format;
            const bool alias_fmt = alias && afmt != vk_format(t->format);
            const bool alias_snap = alias_fmt && !g_vk.feedback_loop && afmt != VK_FORMAT_UNDEFINED &&
                                    !(src->aspect & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) &&
                                    !renderer_format_is_depth(t0->format) && src->samples == 1 &&
                                    host_texel_bytes(afmt) && host_texel_bytes(afmt) == host_texel_bytes(src->format);
            const VkFormat vfmt = alias_snap ? afmt : src->format;   // (the snapshot view's format)
            // g_vk.fbl_layout (the layout extension alone, AMD): a colour, single-sample, own-format read is live too, with
            // the image in ATTACHMENT_FEEDBACK_LOOP_OPTIMAL for the pass (fbl_move) and the pass's pipelines flagged; a draw
            // that also writes what it samples keeps SELFCOPY's snapshot, depth keeps its copy
            const bool fbl_live = g_vk.fbl_layout && !alias && !fcopy && !ccopy && !(selfcopy && self_write_c) && src->samples == 1 &&
                                  !(src->aspect & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) &&
                                  src->base_type != VK_IMAGE_VIEW_TYPE_3D && (src->fbl_in || n_fbl < 9) &&
                                  // (the bound image itself, made with the feedback-loop usage: a block-compressed target renders into its
                                  // shadow, so its own image is no attachment and cannot take the layout)
                                  (src->usage & VK_IMAGE_USAGE_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT) && vk_render_backend(t) == src;
            if (fbl_live || (alias_fmt && !alias_snap) || (g_vk.feedback_loop && !fcopy && !(ccopy && !(src->aspect & VK_IMAGE_ASPECT_DEPTH_BIT)) && !(selfcopy && self_write_c) && (!(src->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) || (!dcopy && !self_write)))) {   // (an alias read in another format stays live: its view comes from vk_view)
                // LARECOMP_FEEDBACK_GAMEBAR=1 (experiment, 2026-10-17): Maxwell's texture cache does not see ROP writes
                // without the game's own nvnCommandBufferBarrier either, so a sampled attachment re-written since the last
                // feedback barrier gets a new one only when the game issued a barrier since (else it reads the image as is).
                static int fb_gamebar = -1;
                if (fb_gamebar < 0) { const char *e = getenv("LARECOMP_FEEDBACK_GAMEBAR"); fb_gamebar = e && *e == '1'; }
                bool need = si < 0 || !snap_ok[si];
                if (need && fb_gamebar && si >= 0 && !game_bar_since_fb) { need = false; stat_fb_elided++; }
                if (need && bm == BAR_NONE) { need = false; stat_fb_elided++; }
                if (need) {
                    game_bar_since_fb = false;
                    if (!fb_done) {
                        stat_feedback_live++;
                        fb_bits |= 1u << 8;
                        fbl_hold = true;   // (the images this draw moved stay in FEEDBACK_LOOP_OPTIMAL across its own restart)
                        vk_end_pass();
                        fbl_hold = false;
                        VkCommandBuffer cb = vk_cmd();
                        gp_mark(cb, "feedback barrier %ux%u fmt %d", src->width, src->height, src->format);
                        if (feedback_narrow() && sg == 1) { fragment_barrier(cb); pass_barrier_frag = true; }   // (a vertex-stage read: full)
                        else vk_full_barrier(cb);
                        pass_barrier_needed = false;   // (just issued, nothing recorded before the pass begins again)
                        pass_barrier_fresh = true;
                        st.rt_dirty = true;
                        fb_done = true;
                    }
                    if (si < 0) { static int rr2; si = rr2++ & 7; snap_tex[si] = t; }
                    snap_ok[si] = true;
                }
                if (fbl_live && !src->fbl_in) {   // (g_vk.fbl_layout: the pass restarts with this attachment in the feedback-loop layout)
                    fbl_hold = true;
                    vk_end_pass();
                    fbl_hold = false;
                    fbl_move(vk_cmd(), src, true);
                    fbl_imgs[n_fbl++] = src;
                    st.rt_dirty = true;
                    stat_fbl_moves++;
                }
                if (src->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) depth_live_in_pass = true;
                fb_live_aspects |= src->aspect & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
                feedback_tex[nfeedback] = t0;
                // (no live 2D view of a 3D target: that needs VK_EXT_image_2d_view_of_3d and would read slice 0; the sample
                // takes the texture's own view instead.)
                feedback_fbl[nfeedback] = fbl_live ? src : NULL, feedback_snapb[nfeedback] = NULL, feedback_view[nfeedback++] = alias || src->base_type == VK_IMAGE_VIEW_TYPE_3D ? VK_NULL_HANDLE
                                           : vk_image_view(src, VK_IMAGE_VIEW_TYPE_2D, src->format, 0, src->levels, 0, 1, id, sa);
                fb_bits += 1;
                if (self_write_c) fb_bits |= 1u << 9;
                if (src->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) fb_bits |= 1u << 10;
                continue;
            }
            if (!nosnap && si >= 0 && (snap_ok[si] || self_write) && existing && (!alias_snap || existing->mut)) {
                feedback_tex[nfeedback] = t0;
                feedback_fbl[nfeedback] = NULL, feedback_snapb[nfeedback] = alias_snap ? &existing->copy : NULL, feedback_view[nfeedback++] = vk_image_view(&existing->copy, VK_IMAGE_VIEW_TYPE_2D, vfmt, 0, existing->copy.levels, 0, 1, id, sa);
                fb_bits += 1u << 4;
                continue;
            }
            // LARECOMP_FEEDBACK_SELFCOPY=2: Maxwell's texture cache sees ROP writes only after the game's own barrier, so a
            // same-draw read + write samples what the target held at the last game barrier; =1 takes a full copy before every
            // such draw (~300 a frame, 4K Kakariko 129.5 -> 29.8 fps). Here the snapshot is retaken only when the game
            // barriered (or a copy / dispatch / clear wrote anything) since it was copied, or a new frame began; between
            // those, the self-write draws share it (they read the pre-barrier picture, which is what the hardware guarantees;
            // =1's pre-draw picture is one of the values it may also show). (the stamp travels with the snapshot image, not
            // the snap_tex slot, which the live path hands to other textures; and a target reallocated mid-frame keeps its
            // key, so the copy must still match it)
            if (selfcopy == 2 && self_write_c && !nosnap && existing && existing->bar == tex_bar_epoch && existing->frm == vk_frame_counter() &&
                existing->copy.width == src->width && existing->copy.height == src->height && existing->copy.format == src->format &&
                existing->copy.levels == snap_levels(src) && (!alias_snap || existing->mut)) {
                stat_fb_selfreuse++; selfw_reused++;
                feedback_tex[nfeedback] = t0;
                feedback_fbl[nfeedback] = NULL, feedback_snapb[nfeedback] = alias_snap ? &existing->copy : NULL, feedback_view[nfeedback++] = vk_image_view(&existing->copy, VK_IMAGE_VIEW_TYPE_2D, vfmt, 0, existing->copy.levels, 0, 1, id, sa);
                fb_bits += 1u << 4;
                continue;
            }
            stat_feedback_copies++;
            selfw_copies += self_write_c;
            fb_bits += 1u << 4;
            fb_bits |= 1u << 11;
            fbc_note(g_cur_fs_hash, src, si < 0 || !existing ? "new" : snap_why[si], t == st.rt.depth);
            fbl_hold = true;   // (g_vk.fbl_layout: an earlier live read of this draw keeps its image in the feedback-loop layout)
            vk_end_pass();
            fbl_hold = false;
            if (src->fbl_in) {   // (the copy reads it in GENERAL; this draw's live reads of it then say GENERAL too)
                fbl_move(vk_cmd(), src, false);
                for (int k = 0; k < nfeedback; k++) if (feedback_fbl[k] == src) feedback_fbl[k] = NULL;
            }
            TexBackend *dst = snapshot_image(t, src, alias_snap);
            Snap *sn = snap_last;
            const bool dshape = snap_delta_on() && !g_debug_frame && dst->levels == 1 && src->samples == 1 && src->layers == 1 && src->depth <= 1;
            const bool delta = dshape && sn->tag && sn->tag == src->snap_tag && sn->mimg == src->image && sn->nd == snap_ndgen &&
                               sn->mfrm == vk_frame_counter();
            VkImageCopy drg = {.srcSubresource = {src->aspect, 0, 0, 1}, .dstSubresource = {src->aspect, 0, 0, 1}};
            if (delta) {
                int32_t x0 = src->snap_box[0] > 0 ? src->snap_box[0] : 0, y0 = src->snap_box[1] > 0 ? src->snap_box[1] : 0;
                int32_t x1 = src->snap_box[2] < (int32_t)src->width ? src->snap_box[2] : (int32_t)src->width;
                int32_t y1 = src->snap_box[3] < (int32_t)src->height ? src->snap_box[3] : (int32_t)src->height;
                if (x1 > x0 && y1 > y0) drg.srcOffset = drg.dstOffset = (VkOffset3D){x0, y0, 0}, drg.extent = (VkExtent3D){(uint32_t)(x1 - x0), (uint32_t)(y1 - y0), 1};
                stat_snap_delta++; stat_snap_delta_px += (uint64_t)drg.extent.width * drg.extent.height;
                stat_snap_delta_full_px += (uint64_t)src->width * src->height;
            }
            if (snap_trace_on())
                LOG("[snaptrace] f%llu img %p %ux%u fmt %d lv %u fs %016llx self %d %s box %d,%d-%d,%d%s%s\n", (unsigned long long)vk_frame_counter(),
                    (void *)src->image, src->width, src->height, src->format, dst->levels, (unsigned long long)g_cur_fs_hash, (int)self_write_c,
                    delta ? "DELTA" : !dshape ? "shape" : !(sn->tag && sn->tag == src->snap_tag && sn->mimg == src->image) ? "nomirror"
                          : sn->mfrm != vk_frame_counter() ? "frame" : "ndgen",
                    src->snap_box[0], src->snap_box[1], src->snap_box[2], src->snap_box[3],
                    !delta && dshape && sn->tag && sn->tag == src->snap_tag && sn->mfrm == vk_frame_counter() ? " cause " : "",
                    !delta && dshape && sn->tag && sn->tag == src->snap_tag && sn->mfrm == vk_frame_counter() ? snap_nd_why : "");
            const bool tiles = delta && drg.extent.width && smark_on() && src->smark_ok;   // (LARECOMP_SNAP_MARK)
            if (dshape) { sn->tag = src->snap_tag = ++snap_tagc; sn->mimg = src->image; sn->nd = snap_ndgen; sn->mfrm = vk_frame_counter(); }
            else sn->tag = 0;
            src->smark_ok = true;
            src->snap_box[0] = src->snap_box[1] = src->snap_box[2] = src->snap_box[3] = 0;
            VkCommandBuffer cb = vk_cmd();
            // (one barrier for all of this draw's copies: between them only copies into other snapshot images were recorded,
            // which no copy here reads; the G-buffer decals copy three targets back to back. LARECOMP_SNAP_BAR_EACH=1: old)
            static int bar_each = -1;
            if (bar_each < 0) bar_each = getenv("LARECOMP_SNAP_BAR_EACH") && *getenv("LARECOMP_SNAP_BAR_EACH") == '1';
            if ((!snap_bar || bar_each || g_debug_frame) && (!delta || drg.extent.width)) vk_full_barrier(cb), snap_bar = true;
            gp_mark(cb, "feedback snapshot %ux%u fmt %d%s", src->width, src->height, src->format, delta ? " (delta)" : "");
            VkImageCopy rg[16];
            uint32_t nlv = dst->levels < 16 ? dst->levels : 16;
            for (uint32_t l = 0; l < nlv; l++) {
                uint32_t w = src->width >> l, h = src->height >> l;
                rg[l] = (VkImageCopy){.srcSubresource = {src->aspect, l, 0, 1}, .dstSubresource = {src->aspect, l, 0, 1},
                                      .extent = {w ? w : 1, h ? h : 1, 1}};
            }
            if (tiles && smark_copy(cb, src, dst)) stat_snap_tiles_n++;
            else if (delta) { if (drg.extent.width) vkCmdCopyImage(cb, src->image, VK_IMAGE_LAYOUT_GENERAL, dst->image, VK_IMAGE_LAYOUT_GENERAL, 1, &drg); }
            else vkCmdCopyImage(cb, src->image, VK_IMAGE_LAYOUT_GENERAL, dst->image, VK_IMAGE_LAYOUT_GENERAL, nlv, rg);
            if (g_debug_frame && (src->aspect & VK_IMAGE_ASPECT_DEPTH_BIT)) {
                static int nd;
                char path[128];
                snprintf(path, sizeof path, "build/fbdepth_%03d_src.pgm", nd);
                debug_save_depth(src, path);
                snprintf(path, sizeof path, "build/fbdepth_%03d_snap.pgm", nd++);
                debug_save_depth(dst, path);
                cb = vk_cmd();
            }
            feedback_tex[nfeedback] = t0;
            feedback_fbl[nfeedback] = NULL, feedback_snapb[nfeedback] = alias_snap ? dst : NULL, feedback_view[nfeedback++] = vk_image_view(dst, VK_IMAGE_VIEW_TYPE_2D, vfmt, 0, dst->levels, 0, 1, id, sa);
            if (si < 0) { static int rr; si = rr++ & 7; snap_tex[si] = t; }
            snap_ok[si] = true;
            st.rt_dirty = true;
        }
    if (bm == BAR_TRACKED || pe == 2) {
        if (wr_g) gmem_wr_epoch = bar_epoch;   // (this draw runs in the current epoch)
        if (rd_g) gmem_rd_epoch = bar_epoch;
    }
    if (pe == 2)  // (read marks, with the epoch the draw really reads in: after any barrier this function recorded)
        for (int sg = 0; sg < 2; sg++)
            for (int i = 0; i < infos[sg]->ntex; i++) {
                NvnTexture *t0 = shader_texture_object(sg, &infos[sg]->tex[i]);
                if (!t0) continue;
                extern NvnTexture *renderer_texture_alias_writer(NvnTexture *t);
                NvnTexture *t = !is_attachment(t0) ? renderer_texture_alias_writer(t0) : t0;
                TexBackend *b0 = t0->backend, *b1 = t && t != t0 ? t->backend : NULL;
                if (b0) b0->rd_epoch = bar_epoch;
                if (b1) b1->rd_epoch = bar_epoch;
            }
}

// ================================================================== resource binding
static const void *ubo_last_ptr;   // (LARECOMP_CBDUMP: the host copy behind the last span)
static VkDescriptorBufferInfo ubo_span(VkSpan s, uint64_t size) {
    uint64_t max = g_vk.props.limits.maxUniformBufferRange;
    ubo_last_ptr = s.ptr;
    return (VkDescriptorBufferInfo){s.buf, s.off, size > max ? max : size};
}

// LARECOMP_CBDUMP=<dir> with LARECOMP_DRAWLOG_FS (debug, 2026-10-19 NVIDIA hang): the first logged draw's uniform buffers go
// to <dir>/vs_cb<binding>.bin / fs_cb<binding>.bin exactly as bound, and its draw record + indices to <dir>/draw.txt,
// so scratchpad drawh.exe can replay the draw outside the game.
static int cbdump_n;
static void cbdump_write(int stage, int k, const void *p, uint64_t size) {
    char path[512];
    snprintf(path, sizeof path, "%s/%s_cb%d.bin", getenv("LARECOMP_CBDUMP"), stage == 1 ? "fs" : "vs", k);
    FILE *f = p ? fopen(path, "wb") : NULL;
    if (f) { fwrite(p, 1, size, f); fclose(f); }
    LOG("[cbdump] st%d binding %d: %llu bytes %s\n", stage, k, (unsigned long long)size, f ? "written" : "NOT written");
}

// LARECOMP_UBO_ZEROCOPY=1 (2026-10-17, Deck: the render thread is the limit with SyncWait lag, UBO snapshots = cbuf ~1.9 ms of
// its ~18.6 ms per frame): the game's uniform buffers are bound straight from the imported guest heap (gmem_buf, dynamic offset
// = address - heap base) instead of copied per submission. The GPU then reads them when it runs, like the Switch does; an
// inline UpdateUniformBuffer (CMD_UPDATE_UNIFORM_BUFFER, writes guest memory mid-list) turns it off for the rest of the run.
static VkBuffer gmem_buf;
static uint64_t gmem_size;
static VkDescriptorBufferInfo gmem_info(void);
static int ubo_zc = -1;
static uint64_t stat_ubo_zc, stat_ubo_copy;
// Uniform buffers the game updates inline (d36: two nvnCommandBufferUpdateUniformBuffer at boot, buffer 100e588000 +64 KB):
// those stay snapshots; the rest keeps zero-copy. More than 64 such buffers: zero-copy off.
// Constant buffer k of a stage (renderer.m bind_cbufs), copied into transient memory.
// Uniform data bound straight from the imported guest heap instead of a per-submission copy (the GPU reads guest memory
// at execution time, as Maxwell does; the game keeps it alive until its fences). OFF by default everywhere since 2026-10-20:
// with the SyncWait lag the game rewrites its UI uniforms before the GPU reads them, so on the Deck HUD elements (hearts,
// sword icon, panels) were drawn screen-sized for single frames outdoors (user video; flick_score2 run B 2037 flash frames
// in 159 s with it, run C 0 with copies; the user watched C: no UI flicker). Its gain was ~1 % (Deck d53/d54 58.9 / 58.2
// fps vs 58.2 / 57.8 copied). LARECOMP_UBO_ZEROCOPY=1 / 2 turns it on. Buffers the game updates inline (UpdateUniformBuffer)
// stay snapshots (zc_excl).
static bool heap_mirror_bind(uint64_t addr, uint64_t size, VkBuffer *buf, VkDeviceSize *off);
static bool alias_mirror_bind(uint64_t addr, uint64_t size, VkBuffer *buf, VkDeviceSize *off);
// LARECOMP_UBO_ZEROCOPY=2 (discrete GPU, off by default): bound from the VRAM heap mirror instead.
static bool ubo_zc_span(uint64_t addr, uint64_t size, VkSpan *out) {
    if (ubo_zc < 0) {
        const char *e = getenv("LARECOMP_UBO_ZEROCOPY");
        ubo_zc = !g_vk.host_import ? 0 : e ? atoi(e) : 0;
        if (ubo_zc) LOG("[vk] uniform buffers + shader constants bound from the guest heap%s (LARECOMP_UBO_ZEROCOPY=0: copies)\n",
                        ubo_zc == 2 ? "'s VRAM mirror" : "");
    }
    if (ubo_zc <= 0 || !addr) return false;
    if (!gmem_buf) gmem_info();
    uint64_t al = g_vk.props.limits.minUniformBufferOffsetAlignment ? g_vk.props.limits.minUniformBufferOffsetAlignment : 256;
    for (int i = 0; i < zc_nexcl; i++) if (addr < zc_excl[i].hi && addr + size > zc_excl[i].lo) return false;
    if (!gmem_buf || addr < HEAP_BASE || addr + size > HEAP_BASE + gmem_size || (addr - HEAP_BASE) % al) return false;
    if (ubo_zc == 2) {
        VkBuffer b;
        VkDeviceSize o;
        if (!heap_mirror_bind(addr, size, &b, &o)) return false;
        *out = (VkSpan){b, o, NULL};
        return true;
    }
    *out = (VkSpan){gmem_buf, addr - HEAP_BASE, NULL};
    rc_record(0, addr, size);
    return true;
}
// The bytes cbuf_info_ snapshots of a bound uniform range r (r.addr != 0) at binding k >= 3
static inline uint64_t ubo_size(const ShaderInfo *si, int k, Range r) {
    uint64_t size = si->cbuf_size[k] ? si->cbuf_size[k] : 256;
    if ((si->cbuf_dynamic & (1u << k)) && r.size) size = r.size > 0x10000 ? 0x10000 : r.size;
    if (size > r.size && r.size) size = r.size;
    if (size < 16) size = 16;
    return size;
}

// LARECOMP_C1_PERSIST (present, cut (b); default 1 (Kakariko at a 120 cap: 614-735 MB a stats window not copied, render
// CPU 3.96 against 4.00 ms/frame, c1p4_k1a / k0b); =0 off, the per-list copies; =2 check: binds the per-list copy as
// before and compares the guest bytes with the persistent copy at every hit, "[c1p] changed" lines): compiler constants
// the heap mirror refuses were re-copied into the transient ring by every command list. They are part of the shader
// blob, and programs are keyed by code address already (prog_tab, get_program_slow; both reset only by vk_exec_reset),
// so one copy per (address, size), written once into a grow-only arena that is never recycled, serves every list: the
// GPU reads the same bytes, with no per-list copy, no ring space and a stable offset (more stage_set memo / set_tab
// hits).
#define C1P_TAB 16384
#define C1P_MAX (16ull << 20)   // (arena cap: past it, misses go back to the per-list copies. Host-visible: without resizable
                                // BAR it can land in the 256 MB BAR heap with the ring, so it is no bigger than the table's
                                // 3/4 load (~12k entries of ~1.3 KB aligned: 736 MB / 697k binds))
static struct { uint64_t addr, size, hash; VkSpan span; void *shadow; } c1p_tab[C1P_TAB];   // (hash: of the bytes copied)
static struct { VkBuffer buf; VmaAllocation mem; uint8_t *ptr; uint64_t used; } c1p_blk[C1P_MAX / C1P_BLOCK];
static int c1_persist_mode(void) {
    static int m = -1;
    if (m < 0) { const char *e = getenv("LARECOMP_C1_PERSIST"); m = e ? atoi(e) : 1; LOG("[vk] c1 persistent copies %d (LARECOMP_C1_PERSIST=0: per-list copies)\n", m); }
    return m;
}
// (keyed by address alone, so that c1_persist_forget finds every size; a forgotten entry keeps its address, size 0, and
// stays in the probe chain)
static uint32_t c1p_slot(uint64_t addr) { uint64_t h = addr * 0x9E3779B97F4A7C15ull; return (uint32_t)(h ^ (h >> 32)) & (C1P_TAB - 1); }
// A program made for a code address (a prog_tab / compute table miss): the code there may be new (a recycled shader pool),
// translated from the new bytes, so constants whose guest bytes no longer hash as their copy did are copied again (the next
// insert for the address reuses the slot). A new vkey / fkey variant of the same code keeps its entry.
static void c1_persist_forget(const NvnProgram *np) {
    if (!c1p_n) return;
    for (int st = 0; st < 6; st++) {
        if (!(np->stage_mask >> st & 1) || !np->stages[st].code_addr) continue;
        uint64_t addr = np->stages[st].code_addr + np->stages[st].const_offset;
        uint32_t slot = c1p_slot(addr);
        for (int probe = 0; probe < 16 && c1p_tab[slot].addr; probe++, slot = (slot + 1) & (C1P_TAB - 1)) {
            uint64_t sz = c1p_tab[slot].size;
            if (c1p_tab[slot].addr != addr || !sz) continue;
            if (hash_words(sz, (const void *)(uintptr_t)addr, sz) == c1p_tab[slot].hash) prof_c1p_kept++;
            else { c1p_tab[slot].size = 0; prof_c1p_forgot++; }
        }
    }
}
static bool c1_persist(uint64_t addr, uint64_t size, VkSpan *out) {
    uint64_t al = g_vk.props.limits.minUniformBufferOffsetAlignment ? g_vk.props.limits.minUniformBufferOffsetAlignment : 256;
    uint32_t slot = c1p_slot(addr), reuse = UINT32_MAX, empty = UINT32_MAX;
    for (int probe = 0; probe < 16; probe++, slot = (slot + 1) & (C1P_TAB - 1)) {
        if (!c1p_tab[slot].addr) { empty = slot; break; }
        if (c1p_tab[slot].addr != addr) continue;
        if (!c1p_tab[slot].size) { if (reuse == UINT32_MAX) reuse = slot; continue; }   // (forgotten: refilled below)
        if (c1p_tab[slot].size != size) continue;
        if (c1p_tab[slot].shadow && memcmp(c1p_tab[slot].shadow, (const void *)(uintptr_t)addr, size)) {
            if (prof_c1p_changed++ < 20)
                LOG("[c1p] changed: constants %llx +%llu differ from their first copy\n", (unsigned long long)addr, (unsigned long long)size);
            memcpy(c1p_tab[slot].shadow, (const void *)(uintptr_t)addr, size);
        }
        *out = c1p_tab[slot].span;
        return true;
    }
    // a miss: one copy into the arena, in this address's forgotten slot or the chain's empty one (none, the table past 3/4
    // full or the arena at its cap: the per-list copy)
    if (reuse != UINT32_MAX) slot = reuse;
    else if (empty != UINT32_MAX && c1p_n < C1P_TAB * 3 / 4) slot = empty;
    else return false;
    {
        uint64_t need = (size + al - 1) / al * al;
        if (!c1p_nblk || c1p_blk[c1p_nblk - 1].used + need > C1P_BLOCK) {
            if (c1p_nblk == (int)(C1P_MAX / C1P_BLOCK) || need > C1P_BLOCK) return false;
            VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = C1P_BLOCK, .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT};
            VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO,   // (device-local with resizable BAR, as the ring)
                                          .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT};
            VmaAllocationInfo info;
            if (vmaCreateBuffer(g_vk.vma, &bi, &ai, &c1p_blk[c1p_nblk].buf, &c1p_blk[c1p_nblk].mem, &info) != VK_SUCCESS) return false;
            c1p_blk[c1p_nblk].ptr = info.pMappedData;
            c1p_blk[c1p_nblk].used = 0;
            c1p_nblk++;
        }
        typeof(c1p_blk[0]) *b = &c1p_blk[c1p_nblk - 1];
        memcpy(b->ptr + b->used, (const void *)(uintptr_t)addr, size);
        c1p_tab[slot].addr = addr; c1p_tab[slot].size = size;
        c1p_tab[slot].hash = hash_words(size, (const void *)(uintptr_t)addr, size);
        c1p_tab[slot].span = (VkSpan){b->buf, b->used, b->ptr + b->used};
        free(c1p_tab[slot].shadow); c1p_tab[slot].shadow = NULL;
        if (c1_persist_mode() == 2 && (c1p_tab[slot].shadow = malloc(size))) memcpy(c1p_tab[slot].shadow, (const void *)(uintptr_t)addr, size);
        b->used += need;
        if (reuse == UINT32_MAX) c1p_n++;
        prof_c1p_new++;
        stat_upload_bytes += size;
        stat_up_kind[6] += size;
        *out = c1p_tab[slot].span;
        return true;
    }
}
// (vk_exec_reset, GPU idle: programs are dropped by code address, so are their constants; the arena is kept and reused)
static void c1_persist_reset(void) {
    for (int i = 0; i < C1P_TAB; i++) free(c1p_tab[i].shadow);
    memset(c1p_tab, 0, sizeof c1p_tab);
    for (int i = 0; i < c1p_nblk; i++) c1p_blk[i].used = 0;
    c1p_n = 0;
}
static VkDescriptorBufferInfo cbuf_info_(int stage, ShaderInfo *si, int k, uint32_t gx, uint32_t gy, uint32_t gz);
static VkDescriptorBufferInfo cbuf_info(int stage, ShaderInfo *si, int k, uint32_t gx, uint32_t gy, uint32_t gz) {
    ubo_last_ptr = NULL;
    vk_up_kind = 1; VkDescriptorBufferInfo r = cbuf_info_(stage, si, k, gx, gy, gz); vk_up_kind = 0;
    if (cbdump_n) cbdump_write(stage, k, ubo_last_ptr, r.range);
    return r;
}
static uint64_t g_sb0_bound;   // (LARECOMP_COUNT_DUMP) VS storage buffer #0's driver-table address in the last draw
static uint64_t g_tsb_gen[6];   // (defined with stage_set's memo, LARECOMP_SET_TSGEN)
static int set_tsgen(void), set_bgen(void);
static void tsgen_bad(const char *what, int stage);
static VkDescriptorBufferInfo cbuf_info_(int stage, ShaderInfo *si, int k, uint32_t gx, uint32_t gy, uint32_t gz) {
    static const uint8_t zeros[0x10000];
    if (k == 0) vk_up_kind = 5; else if (k == 1) vk_up_kind = 6;
    if (k == 0 && stage != 5 && !si->uses_gmem) {
        // (graphics, no global memory: the contents are just st.sbuf[stage]'s 16 ranges at a per-stage offset. Same
        // ranges and size as the previous call for this stage in this submission: the same copy, without zeroing and
        // comparing the 2 KB block)
        static struct { uint64_t seq; uint64_t size, tsgen; int stage; Range sb[16]; VkDescriptorBufferInfo bi; } dm[2];
        int d = stage == 1;
        uint64_t size = si->cbuf_size[0] > 0x800 ? 0x800 : (si->cbuf_size[0] ? si->cbuf_size[0] : 256);
        uint64_t q = vk_pending_seq();
        if (dm[d].seq == q && dm[d].size == size && !cbdump_n) {
            // (LARECOMP_SET_TSGEN: the stage's storage buffers unchanged since dm[d] was built for it, by generation)
            int g = set_tsgen();
            bool known = g && set_bgen() && dm[d].stage == stage && dm[d].tsgen == g_tsb_gen[stage];
            if (known && g == 1) return dm[d].bi;
            bool same = !memcmp(dm[d].sb, st.sbuf[stage], sizeof dm[d].sb);
            if (known && !same) tsgen_bad("driver-table", stage);
            if (same) { dm[d].stage = stage; dm[d].tsgen = g_tsb_gen[stage]; return dm[d].bi; }
        }
        uint32_t drv[0x800 / 4] = {0};
        uint32_t base = 0x110 + 0x100 * (stage == 1 ? 4 : 0);
        for (int i = 0; i < 16; i++) {
            uint32_t *e = &drv[(base + 16 * i) / 4];
            e[0] = (uint32_t)st.sbuf[stage][i].addr; e[1] = (uint32_t)(st.sbuf[stage][i].addr >> 32); e[2] = (uint32_t)st.sbuf[stage][i].size;
        }
        dm[d].bi = ubo_span(copy_dedupe(stage == 1 ? DEDUPE_DRV_F : DEDUPE_DRV_V, drv, size, 256), size);
        dm[d].seq = q; dm[d].size = size; memcpy(dm[d].sb, st.sbuf[stage], sizeof dm[d].sb);
        dm[d].stage = stage; dm[d].tsgen = g_tsb_gen[stage];
        return dm[d].bi;
    }
    if (k == 0) {
        uint32_t drv[0x800 / 4] = {0};
        if (stage == 5) {
            drv[0x200 / 4] = gx; drv[0x204 / 4] = gy; drv[0x208 / 4] = gz;
            for (int i = 0; i < 16; i++) {
                uint32_t *u = &drv[(0x210 + 16 * i) / 4];
                u[0] = (uint32_t)st.ubuf[5][i].addr; u[1] = (uint32_t)(st.ubuf[5][i].addr >> 32); u[2] = (uint32_t)st.ubuf[5][i].size;
                uint32_t *e = &drv[(0x310 + 16 * i) / 4];
                uint64_t a = si->uses_gmem ? gdev_map(st.sbuf[5][i].addr, st.sbuf[5][i].size, si->writes_gmem) : st.sbuf[5][i].addr;
                e[0] = (uint32_t)a; e[1] = (uint32_t)(a >> 32); e[2] = (uint32_t)st.sbuf[5][i].size;
                if (g_debug_frame && si->uses_gmem && st.sbuf[5][i].addr)
                    LOG("[ssbo] cs #%d %llx +%llx %s -> %s\n", i, (unsigned long long)st.sbuf[5][i].addr, (unsigned long long)st.sbuf[5][i].size,
                        si->writes_gmem ? "rw" : "ro", a != st.sbuf[5][i].addr ? "window" : "host");
            }
        } else {
            uint32_t base = 0x110 + 0x100 * (stage == 1 ? 4 : 0);
            for (int i = 0; i < 16; i++) {
                uint32_t *e = &drv[(base + 16 * i) / 4];
                uint64_t a = si->uses_gmem ? gdev_map(st.sbuf[stage][i].addr, st.sbuf[stage][i].size, si->writes_gmem) : st.sbuf[stage][i].addr;
                if (stage == 0 && i == 0) g_sb0_bound = a;
                e[0] = (uint32_t)a;
                e[1] = (uint32_t)(a >> 32);
                e[2] = (uint32_t)st.sbuf[stage][i].size;
                if (g_debug_frame && si->uses_gmem && st.sbuf[stage][i].addr)
                    LOG("[ssbo] st%d #%d %llx +%llx %s -> %s\n", stage, i, (unsigned long long)st.sbuf[stage][i].addr, (unsigned long long)st.sbuf[stage][i].size,
                        si->writes_gmem ? "rw" : "ro", a != st.sbuf[stage][i].addr ? "window" : "host");
            }
        }
        uint64_t size = si->cbuf_size[0] > sizeof drv ? sizeof drv : (si->cbuf_size[0] ? si->cbuf_size[0] : 256);
        if (size < 0x500 && stage == 5) size = 0x500;
        return ubo_span(copy_dedupe(stage == 5 ? DEDUPE_DRV_C : stage == 1 ? DEDUPE_DRV_F : DEDUPE_DRV_V, drv, size, 256), size);
    }
    if (k == 1) {  // compiler constants stored after the shader code
        NvnShaderStage *ss = &st.program->stages[stage];
        uint64_t size = ss->const_size ? ss->const_size : 16;
        if (size < si->cbuf_size[1]) size = si->cbuf_size[1];
        VkSpan zs;   // (d54: re-copied every submission = 340 MB per 2 s on the Deck; the code memory does not change)
        if (ubo_zc_span(ss->code_addr + ss->const_offset, size, &zs)) { if (prof) prof_c1[0]++, prof_c1_b[0] += size; return ubo_span(zs, size); }
        // Discrete GPU: from the heap's VRAM mirror (LARECOMP_C1_MIRROR=0: copies). Unlike per-frame uniforms these never
        // change after the shader loads, so the mirror's sync timing can't matter.
        static int c1m = -1;
        if (c1m < 0) { const char *e = getenv("LARECOMP_C1_MIRROR"); c1m = e ? *e == '1' : g_vk.props.deviceType != VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU; }
        uint64_t ca = ss->code_addr + ss->const_offset, al = g_vk.props.limits.minUniformBufferOffsetAlignment ? g_vk.props.limits.minUniformBufferOffsetAlignment : 256;
        VkBuffer mb;
        VkDeviceSize mo;
        if (c1m && ca >= HEAP_BASE && (ca - HEAP_BASE) % al == 0 && heap_mirror_bind(ca, size, &mb, &mo)) { if (prof) prof_c1[1]++, prof_c1_b[1] += size; return ubo_span((VkSpan){mb, mo, NULL}, size); }
        // LARECOMP_C1_ALIAS=1: constants in the alias window from its VRAM mirror. OFF by default until it is cleared for
        // the 2026-10-19 15:35 GPU hang
        static int c1a = -1;
        if (c1a < 0) { const char *e = getenv("LARECOMP_C1_ALIAS"); c1a = e && *e == '1'; }
        if (c1m && c1a && ca % al == 0 && alias_mirror_bind(ca, size, &mb, &mo)) { if (prof) prof_c1[6]++, prof_c1_b[6] += size; return ubo_span((VkSpan){mb, mo, NULL}, size); }
        if (prof) { int why = !c1m ? 5 : ca < HEAP_BASE ? 2 : (ca - HEAP_BASE) % al ? 3 : 4; prof_c1[why]++; prof_c1_b[why] += size; if (why == 4) prof_hmw[hm_why]++; }
        // (outside the heap only: there a shader can write the page, heap_mirror_bind's first refusal)
        int c1p = c1_persist_mode();
        if (c1p && !(ca >= HEAP_BASE && ca < HEAP_BASE + gmem_size) && c1_persist(ca, size, &zs) && c1p == 1) { if (prof) prof_c1[7]++, prof_c1_b[7] += size; return ubo_span(zs, size); }
        if (g_debug_frame) LOG("[upcopy] c1 st%d %llx +%llu\n", stage, (unsigned long long)(ss->code_addr + ss->const_offset), (unsigned long long)size);
        return ubo_span(guest_copy(ss->code_addr + ss->const_offset, size), size);  // (constant: shared per submission)
    }
    int idx = k - 3;  // cbuf_base
    Range r = (idx >= 0 && idx < 18) ? st.ubuf[stage][idx] : (Range){0, 0};
    if (!r.addr) { uint64_t size = si->cbuf_size[k] ? si->cbuf_size[k] : 256; vk_up_kind = 7; return ubo_span(vk_transient_copy(zeros, size, 256), size); }
    uint64_t size = ubo_size(si, k, r);
    VkSpan zs;
    if (ubo_zc_span(r.addr, size, &zs)) { stat_ubo_zc++; return ubo_span(zs, size); }
    if (ubo_zc > 0) stat_ubo_copy++;
    if (g_debug_frame) LOG("[upcopy] cb%d st%d %llx +%llu%s\n", idx, stage, (unsigned long long)r.addr, (unsigned long long)size,
                           (si->cbuf_dynamic & (1u << k)) ? " dynamic" : "");
    // Logged with the fragment shader's hash; if it ever fires, such ranges want binding in place instead of copying.
    if (gw_overlap(r.addr, size, submit_seq, ~0ull) == 1) {
        static uint64_t nv7;
        if (nv7++ < 20 || nv7 % 10000 == 0)
            LOG("[v7] constant range %llx +%llu (cb%d stage %d, fs %016llx) is written by a shader of the open submission: copied before it runs (%llu so far)\n",
                (unsigned long long)r.addr, (unsigned long long)size, idx, stage, (unsigned long long)g_cur_fs_hash, (unsigned long long)nv7);
    }
    // snapshot: later CPU writes must not leak in. Shared by the draws of one submission binding the same range:
    // within a command list nothing writes guest memory (no inline UpdateUniformBuffer: that writes through below
    // and drops the shared copies).
    return ubo_span(guest_copy(r.addr, size), size);
}

// Global memory (shader LDG/STG, maxwell_gmem_base = HEAP_BASE): the guest heap imported as a GPU buffer
// (VK_EXT_external_memory_host), zero-copy like metal.m's g_guest_buffer, so compute results land in guest
// memory. The import pins its pages, so it covers only the heap the game set up (SetHeapSize), made on first use.
// LARECOMP_VK_NO_GMEM=1: no import, reads return zero.
static VkBuffer gmem_buf;
static VkDeviceMemory gmem_mem;
static uint64_t gmem_size;

#ifndef _WIN32
#include <unistd.h>
int rt_gpu_bo_find(unsigned long long addr, unsigned long long size, unsigned long long *block_size);
#endif
// Linux / amdgpu: a range runtime/linux/plat_posix.cpp put in a driver-owned GTT buffer (rt_gpu_bo_map: the guest heap, the alias
// window) is imported by dma-buf, whole: 1 imported (*got = the block size), 0 not such a range, -1 the import failed.
// A host-pointer import of the same range would be a userptr buffer (MMU notifier: Deck v2's 6 s kernel stall in vkQueueSubmit2).
static int dmabuf_import(uint64_t base, uint64_t size, uint64_t *got, VkBuffer *pbuf, VkDeviceMemory *pmem) {
#ifdef _WIN32
    (void)base, (void)size, (void)got, (void)pbuf, (void)pmem;
    return 0;
#else
    unsigned long long bsize = 0;
    int fd = rt_gpu_bo_find(base, size, &bsize);
    if (fd < 0) return 0;
    PFN_vkGetMemoryFdPropertiesKHR fdprops =
        g_vk.dmabuf_import ? (PFN_vkGetMemoryFdPropertiesKHR)vkGetDeviceProcAddr(g_vk.dev, "vkGetMemoryFdPropertiesKHR") : NULL;
    VkMemoryFdPropertiesKHR mp = {.sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
    if (!fdprops || fdprops(g_vk.dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, fd, &mp) != VK_SUCCESS || !mp.memoryTypeBits) {
        LOG("[vk] dma-buf import of %llx + %llu MB: not supported by the driver\n", (unsigned long long)base, bsize >> 20);
        return -1;
    }
    int dfd = dup(fd);  // (a successful import owns the fd it was given)
    VkImportMemoryFdInfoKHR imp = {.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
                                   .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, .fd = dfd};
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &imp, .allocationSize = bsize,
                               .memoryTypeIndex = (uint32_t)__builtin_ctz(mp.memoryTypeBits)};
    VkDeviceMemory mem;
    VkResult r = dfd < 0 ? VK_ERROR_TOO_MANY_OBJECTS : vkAllocateMemory(g_vk.dev, &ai, NULL, &mem);
    if (r != VK_SUCCESS) {
        if (dfd >= 0) close(dfd);
        LOG("[vk] dma-buf import of %llx + %llu MB failed (%d)\n", (unsigned long long)base, bsize >> 20, (int)r);
        return -1;
    }
    VkExternalMemoryBufferCreateInfo eb = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
                                           .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .pNext = &eb, .size = bsize,
                             .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                      VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                                      VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT};
    VkBuffer buf;
    VkMemoryRequirements req = {0};
    if (vkCreateBuffer(g_vk.dev, &bi, NULL, &buf) != VK_SUCCESS) { vkFreeMemory(g_vk.dev, mem, NULL); return -1; }
    vkGetBufferMemoryRequirements(g_vk.dev, buf, &req);
    if (!(req.memoryTypeBits & (1u << ai.memoryTypeIndex)) || req.size > bsize || vkBindBufferMemory(g_vk.dev, buf, mem, 0) != VK_SUCCESS) {
        LOG("[vk] dma-buf import of %llx + %llu MB: buffer does not fit the memory (types %x, need %llu MB)\n", (unsigned long long)base,
            bsize >> 20, req.memoryTypeBits, (unsigned long long)(req.size >> 20));
        vkDestroyBuffer(g_vk.dev, buf, NULL);
        vkFreeMemory(g_vk.dev, mem, NULL);
        return -1;
    }
    *pbuf = buf;
    *pmem = mem;
    if (got) *got = bsize;
    LOG("[vk] %llx + %llu MB imported by dma-buf (driver-owned GTT buffer, memory type %u)\n", (unsigned long long)base, bsize >> 20,
        ai.memoryTypeIndex);
    return 1;
#endif
}

// Imports [base, base + size) of committed host memory as one buffer (storage / vertex / index / transfer); *got (may be NULL)
// = the buffer's size (larger than size for a driver-owned block, imported whole).
// The device's limits on one imported buffer: one allocation (maxMemoryAllocationSize), one buffer (maxBufferSize) and
// one storage descriptor (maxStorageBufferRange). AMD's Windows driver (RDNA2, 26.10.1) allows 2 GiB for the first two,
// SwiftShader 1 GiB; the guest heap is up to 3.25 GB (Kakariko: 3148 MB). Where one buffer cannot hold it, the heap is
// imported as windows of g_gwin bytes (gmem_import), which translated shaders pick by address (maxwell_gmem_window);
// an import larger than the device takes is still cut (host_import), loudly: memory past it reads zero.
static uint64_t g_imp_max, g_sbuf_range, g_imp_align, g_gwin;
static void import_limits(void) {
    if (g_sbuf_range) return;
    VkPhysicalDeviceExternalMemoryHostPropertiesEXT hp = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT};
    VkPhysicalDeviceMaintenance4Properties m4 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_4_PROPERTIES,
                                                 .pNext = g_vk.host_import ? &hp : NULL};
    VkPhysicalDeviceMaintenance3Properties m3 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_3_PROPERTIES, .pNext = &m4};
    VkPhysicalDeviceProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &m3};
    vkGetPhysicalDeviceProperties2(g_vk.phys, &p2);
    g_imp_max = m3.maxMemoryAllocationSize ? m3.maxMemoryAllocationSize : ~0ull;
    if (m4.maxBufferSize && m4.maxBufferSize < g_imp_max) g_imp_max = m4.maxBufferSize;
    g_sbuf_range = p2.properties.limits.maxStorageBufferRange ? p2.properties.limits.maxStorageBufferRange : 0xFFFFFFFFull;
    g_imp_align = hp.minImportedHostPointerAlignment ? hp.minImportedHostPointerAlignment : 4096;
    uint64_t one = g_imp_max < g_sbuf_range ? g_imp_max : g_sbuf_range;   // (the most one window can be)
    uint64_t gran = g_imp_align > 0x10000 ? g_imp_align : 0x10000;
    g_gwin = one >= HEAP_MAX || getenv("LARECOMP_GMEM_ONE") ? 0 : one & ~(gran - 1);
    if (getenv("LARECOMP_GMEM_WINDOW")) g_gwin = strtoull(getenv("LARECOMP_GMEM_WINDOW"), NULL, 0) << 20 & ~(gran - 1);   // (MB; a check on NVIDIA)
    if (g_gwin && (HEAP_MAX + g_gwin - 1) / g_gwin > GMEM_WIN_MAX) g_gwin = (HEAP_MAX + GMEM_WIN_MAX - 1) / GMEM_WIN_MAX + gran - 1 & ~(gran - 1);
    maxwell_gmem_window = g_gwin;
    LOG("[vk] import limits: one allocation / buffer <= %llu MB, a storage buffer range <= %llu MB, host pointers aligned to %llu; "
        "guest heap: %s%llu%s\n", (unsigned long long)(g_imp_max >> 20), (unsigned long long)(g_sbuf_range >> 20),
        (unsigned long long)g_imp_align, g_gwin ? "windows of " : "one buffer", (unsigned long long)(g_gwin >> 20), g_gwin ? " MB" : "");
    if (!g_gwin) return;
    if (g_gwin > g_imp_max || g_gwin > g_sbuf_range)
        LOG("[vk] WARNING: LARECOMP_GMEM_WINDOW is larger than the device allows: the windows are cut (memory past a cut reads zero)\n");
}
static bool host_import(uint64_t base, uint64_t size, uint64_t *got, VkBuffer *pbuf, VkDeviceMemory *pmem) {
    import_limits();
    if ((base | size) & (g_imp_align - 1)) {
        LOG("[vk] warning: host import %llx + %llu not aligned to the device's %llu: refused\n", (unsigned long long)base,
            (unsigned long long)size, (unsigned long long)g_imp_align);
        return false;
    }
    if (size > g_imp_max) {
        static int n;
        if (n++ < 4) LOG("[vk] WARNING: host import of %llu MB cut to the device's largest allocation, %llu MB: guest memory past it "
                         "reads zero in shaders (expect wrong pictures)\n", (unsigned long long)(size >> 20), (unsigned long long)(g_imp_max >> 20));
        size = g_imp_max & ~(g_imp_align - 1);
    }
    int d = dmabuf_import(base, size, got, pbuf, pmem);
    if (d) return d > 0;  // (a userptr import of a driver-owned mapping cannot work)
    if (got) *got = size;
    PFN_vkGetMemoryHostPointerPropertiesEXT props =
        (PFN_vkGetMemoryHostPointerPropertiesEXT)vkGetDeviceProcAddr(g_vk.dev, "vkGetMemoryHostPointerPropertiesEXT");
    VkMemoryHostPointerPropertiesEXT mp = {.sType = VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
    if (!props || props(g_vk.dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, (void *)(uintptr_t)base, &mp) != VK_SUCCESS ||
        !mp.memoryTypeBits)
        return false;
    VkImportMemoryHostPointerInfoEXT imp = {.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT,
                                            .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
                                            .pHostPointer = (void *)(uintptr_t)base};
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &imp, .allocationSize = size,
                               .memoryTypeIndex = (uint32_t)__builtin_ctz(mp.memoryTypeBits)};
    VkDeviceMemory mem;
    if (vkAllocateMemory(g_vk.dev, &ai, NULL, &mem) != VK_SUCCESS) return false;
    VkExternalMemoryBufferCreateInfo eb = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
                                           .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT};
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .pNext = &eb, .size = size,
                             .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                      VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                                      VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT};
    VkBuffer buf;
    if (vkCreateBuffer(g_vk.dev, &bi, NULL, &buf) != VK_SUCCESS) { vkFreeMemory(g_vk.dev, mem, NULL); return false; }
    vkBindBufferMemory(g_vk.dev, buf, mem, 0);
    *pbuf = buf;
    *pmem = mem;
    return true;
}

// A heap import replaced by a larger one (the heap grew) is freed once the submission being recorded when it was
// replaced has completed: draws and descriptor sets of that submission may already name it (direct vertex / index binds,
// set_tab), and with VKQ their commands can still be in the ring. (16: one re-import can replace up to GMEM_WIN_MAX
// windows)
static struct { VkBuffer buf; VkDeviceMemory mem; uint64_t seq; } gmem_dead[16];
static int gmem_ndead;
// The heap's windows past the first (import_limits: where one buffer cannot hold it). Window 0 is gmem_buf / gmem_size,
// which every other user of the import (zero-copy vertex / index / uniform binds, the device window, the VRAM mirror)
// keeps taking: memory past it is copied there, as memory outside the import always was. Only shader global memory
// (gmem_info / gmem_win_info) reaches the other windows.
static VkBuffer gmem_wbuf[GMEM_WIN_MAX];
static VkDeviceMemory gmem_wmem[GMEM_WIN_MAX];
static uint64_t gmem_wsize[GMEM_WIN_MAX];
static uint64_t gmem_asked;   // heap size the windows were last imported for (no retry until the heap grows past it)
static void gmem_collect_dead(void) {
    if (!gmem_ndead) return;
    uint64_t done = vk_completed_seq();
    int k = 0;
    for (int i = 0; i < gmem_ndead; i++) {
        if (gmem_dead[i].seq > done) { gmem_dead[k++] = gmem_dead[i]; continue; }
        vkDestroyBuffer(g_vk.dev, gmem_dead[i].buf, NULL);
        vkFreeMemory(g_vk.dev, gmem_dead[i].mem, NULL);
    }
    gmem_ndead = k;
}
static void gmem_retire(VkBuffer buf, VkDeviceMemory mem) {   // (the heap grew: the old import may still be in use)
    gmem_collect_dead();
    if (gmem_ndead == 16) {   // (16 replaced imports in flight: never seen) the oldest's submission, if submitted, else all submitted
        vk_wait_seq(gmem_dead[0].seq <= submit_seq ? gmem_dead[0].seq : submit_seq);
        gmem_collect_dead();
        if (gmem_ndead == 16) FATAL("[vk] guest heap re-imported 16 times within one submission\n");
    }
    gmem_dead[gmem_ndead++] = (typeof(gmem_dead[0])){buf, mem, vk_pending_seq()};
}
// The heap [HEAP_BASE, + size) imported: one buffer, or (g_gwin) windows of g_gwin bytes; a window that already covers
// its part is kept. False = the first window failed (no global memory at all).
static bool gmem_import(uint64_t size) {
    gmem_asked = size;
    uint64_t win = g_gwin && size > g_gwin ? g_gwin : size;
    int nw = (int)((size + win - 1) / win);
    if (nw > GMEM_WIN_MAX) nw = GMEM_WIN_MAX;   // (import_limits sizes the windows so that HEAP_MAX fits)
    for (int k = 0; k < nw; k++) {
        uint64_t ws = size - (uint64_t)k * win < win ? size - (uint64_t)k * win : win, got = ws;
        if (ws <= (k ? gmem_wsize[k] : gmem_size)) continue;
        VkBuffer buf;
        VkDeviceMemory mem;
        if (!host_import(HEAP_BASE + (uint64_t)k * win, ws, &got, &buf, &mem)) {
            if (!k) return false;
            LOG("[vk] WARNING: guest heap window %d (%llu MB at +%llu MB) not imported: shader global memory there reads zero\n", k,
                (unsigned long long)(ws >> 20), (unsigned long long)((uint64_t)k * win >> 20));
            break;
        }
        if (!k) {
            if (gmem_buf) gmem_retire(gmem_buf, gmem_mem);
            gmem_buf = buf, gmem_mem = mem, gmem_size = got;
        } else {
            if (gmem_wbuf[k]) gmem_retire(gmem_wbuf[k], gmem_wmem[k]);
            gmem_wbuf[k] = buf, gmem_wmem[k] = mem, gmem_wsize[k] = got;
        }
        if (nw > 1) LOG("[vk] guest heap window %d imported for shader global memory: %llu MB at +%llu MB\n", k,
                        (unsigned long long)(got >> 20), (unsigned long long)((uint64_t)k * win >> 20));
    }
    if (nw == 1) LOG("[vk] guest heap imported for shader global memory: %llu MB\n", (unsigned long long)(gmem_size >> 20));
    return true;
}

static uint64_t gmem_tried;  // heap size the import was last attempted for (gmem_info imports / re-imports)

static VkDescriptorBufferInfo gmem_info(void) {
    static int off = -1;
    if (off < 0) {
        off = getenv("LARECOMP_VK_NO_GMEM") != NULL || !g_vk.host_import;
        if (!g_vk.host_import)   // (the guest heap as a GPU buffer needs VK_EXT_external_memory_host or dma-buf imports)
            LOG("[vk] WARNING: no host-memory import on this device: shader global memory reads return zero (expect wrong pictures)\n");
    }
    uint64_t want = (kernel_heap_size() + 0xFFFF) & ~0xFFFFull;
    if (want > HEAP_MAX) want = HEAP_MAX;
    // (gmem_asked, not gmem_size: an import cut to the device's limit, or a heap in windows, is not retried at every
    // call: the cut one used to be re-imported each time, 8 in one submission = FATAL, on AMD past a 2 GiB heap)
    if (!off && want > gmem_asked && want > gmem_size && !gmem_import(want)) {
        LOG("[vk] guest heap import failed (%llu MB): shader global memory reads return zero\n", (unsigned long long)(want >> 20));
        off = 1;
    }
    if (gmem_buf) {   // (a range past maxStorageBufferRange is invalid: clamped, logged once)
        uint64_t range = gmem_size;
        if (range > g_sbuf_range) {
            static int n;
            if (!n++) LOG("[vk] WARNING: guest heap buffer %llu MB > the device's storage buffer range %llu MB: shaders see the first "
                          "%llu MB, the rest reads zero\n", (unsigned long long)(gmem_size >> 20), (unsigned long long)(g_sbuf_range >> 20),
                          (unsigned long long)(g_sbuf_range >> 20));
            range = g_sbuf_range & ~3ull;
        }
        return (VkDescriptorBufferInfo){gmem_buf, 0, range};
    }
    static uint32_t zero[1024];
    VkSpan s = vk_transient_copy(zero, sizeof zero, 256);
    return (VkDescriptorBufferInfo){s.buf, s.off, sizeof zero};
}
// Window k (1 .. GMEM_WIN_MAX - 1) of the heap, for binding GMEM_WIN_BUFFER_INDEX + k - 1 (a zero buffer until imported)
static VkDescriptorBufferInfo gmem_win_info(int k) {
    gmem_info();   // (imports every window the heap needs, if not yet)
    if (k > 0 && k < GMEM_WIN_MAX && gmem_wbuf[k])
        return (VkDescriptorBufferInfo){gmem_wbuf[k], 0, gmem_wsize[k] < g_sbuf_range ? gmem_wsize[k] : g_sbuf_range & ~3ull};
    static uint32_t zero[1024];
    VkSpan s = vk_transient_copy(zero, sizeof zero, 256);
    return (VkDescriptorBufferInfo){s.buf, s.off, sizeof zero};
}

// ---- Device window. The driver cbuf's storage buffer table points such a buffer at HEAP_BASE + HEAP_MAX + offset,
// which translated shaders route to gdev_ (maxwell_gdev_size). Host memory stays the truth between submissions: a
// region's first use in a submission copies host -> VRAM in the upload command buffer (runs before the main one),
// and the end of the submission copies it back (gdev_writeback), so CPU reads after the fence and CPU writes before
// submit both hold.
#define GDEV_REGION_MAX (16ull << 20)
typedef struct { uint64_t addr, size, off, seq, up; } GdevRegion;  // seq: the submission it was copied in for (or
// carried into, chunked submission); up: the submission whose upload buffer copied it in
static GdevRegion gdev_reg[256];
static int gdev_n;
static uint64_t gdev_top, gdev_upload_seq, gdev_stat_in, gdev_stat_regions, gdev_stat_snap, gdev_stat_host;
static int gdev_half;               // the window half new regions go into
static uint64_t gdev_half_seq[2];   // per half: the last submission that copied a region into it
static VkBuffer gdev_buf;
static VmaAllocation gdev_alloc;

static bool gdev_ready(void) {
    static bool failed;
    if (failed || !maxwell_gdev_size || !gmem_buf) return false;
    if (!gdev_buf) {
        VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = GDEV_SIZE,
                                 .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                          VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, .requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT};
        if (vmaCreateBuffer(g_vk.vma, &bi, &ai, &gdev_buf, &gdev_alloc, NULL) != VK_SUCCESS) {
            gdev_buf = VK_NULL_HANDLE;
            failed = true;
            LOG("[vk] device window: no %llu MB of VRAM, written storage buffers stay in host memory\n", GDEV_SIZE >> 20);
            return false;
        }
        LOG("[vk] device window for written storage buffers: %llu MB VRAM\n", GDEV_SIZE >> 20);
    }
    return true;
}

static VkDescriptorBufferInfo gdev_info(void) {
    if (gdev_ready()) return (VkDescriptorBufferInfo){gdev_buf, 0, GDEV_SIZE};
    static uint32_t zero[1024];
    VkSpan s = vk_transient_copy(zero, sizeof zero, 256);
    return (VkDescriptorBufferInfo){s.buf, s.off, sizeof zero};
}

// Diff copy-back (default LARECOMP_GDEV_BLIND=1 = the old whole-region copy). The guest CPU keeps writing host memory
// while a submission runs. A VRAM shadow of each region as uploaded (window -> shadow at the end of the upload buffer)
// lets the copy-back compute pass write only the words the GPU changed; host words the GPU left alone keep what the CPU
// wrote meanwhile.
static VkBuffer gdev_shadow, wb_set_gmem;
static VmaAllocation gdev_shadow_alloc;
static VkPipeline wb_pipe;
static VkPipelineLayout wb_layout;
static VkDescriptorSetLayout wb_dsl;
static VkDescriptorPool wb_pool;
static VkDescriptorSet wb_set;
static int wb_mode = -1;  // 1 diff, 0 blind

static bool wb_init(void) {
    if (wb_mode >= 0) return wb_mode == 1;
    wb_mode = 0;
    static int blind = -1;
    if (blind < 0) blind = getenv("LARECOMP_GDEV_BLIND") != NULL;
    if (blind) return false;
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = GDEV_SIZE,
                             .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, .requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT};
    if (vmaCreateBuffer(g_vk.vma, &bi, &ai, &gdev_shadow, &gdev_shadow_alloc, NULL) != VK_SUCCESS) {
        gdev_shadow = VK_NULL_HANDLE;
        LOG("[vk] device window: no VRAM for the copy-back shadow, blind copy-back\n");
        return false;
    }
    static const char *src =
        "#version 450\n"
        "layout(local_size_x = 256) in;\n"
        "layout(std430, set = 0, binding = 0) readonly buffer Win { uint w[]; } win;\n"
        "layout(std430, set = 0, binding = 1) readonly buffer Sh { uint w[]; } sh;\n"
        "layout(std430, set = 0, binding = 2) buffer Host { uint w[]; } host;\n"
        "layout(push_constant) uniform P { uint win_off, host_off, words; } p;  // (in words)\n"
        "void main() {\n"
        "    uint i = gl_GlobalInvocationID.x;\n"
        "    if (i >= p.words) return;\n"
        "    uint v = win.w[p.win_off + i];\n"
        "    if (v != sh.w[p.win_off + i]) host.w[p.host_off + i] = v;\n"
        "}\n";
    size_t nw;
    uint32_t *spv = vk_glsl_to_spirv(src, STAGE_COMPUTE, "gdev_copy_back", &nw);
    if (!spv) return false;
    VkShaderModule m = vk_module(spv, nw);
    free(spv);
    VkDescriptorSetLayoutBinding b[3];
    for (uint32_t i = 0; i < 3; i++) b[i] = (VkDescriptorSetLayoutBinding){i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL};
    VkDescriptorSetLayoutCreateInfo dl = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 3, .pBindings = b};
    VK_CHECK(vkCreateDescriptorSetLayout(g_vk.dev, &dl, NULL, &wb_dsl));
    VkPushConstantRange pc = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 12};
    VkPipelineLayoutCreateInfo pl = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &wb_dsl,
                                     .pushConstantRangeCount = 1, .pPushConstantRanges = &pc};
    VK_CHECK(vkCreatePipelineLayout(g_vk.dev, &pl, NULL, &wb_layout));
    VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                                                .module = m, .pName = "main"},
                                      .layout = wb_layout};
    VK_CHECK(vkCreateComputePipelines(g_vk.dev, VK_NULL_HANDLE, 1, &ci, NULL, &wb_pipe));
    vkDestroyShaderModule(g_vk.dev, m, NULL);
    VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3 * 64};
    VkDescriptorPoolCreateInfo dp = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 64, .poolSizeCount = 1, .pPoolSizes = &ps};
    VK_CHECK(vkCreateDescriptorPool(g_vk.dev, &dp, NULL, &wb_pool));
    wb_mode = 1;
    LOG("[vk] device window: diff copy-back (only the words the GPU changed go back to host memory)\n");
    return true;
}

// The copy-back set for the current heap import (a new one when the import is replaced; old sets stay valid for the
// submissions still using them).
static bool wb_set_ready(void) {
    if (wb_set && wb_set_gmem == gmem_buf) return true;
    VkDescriptorSetAllocateInfo a = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = wb_pool, .descriptorSetCount = 1,
                                     .pSetLayouts = &wb_dsl};
    VkDescriptorSet set;
    if (vkAllocateDescriptorSets(g_vk.dev, &a, &set) != VK_SUCCESS) {
        static int told;
        if (!told++) LOG("[vk] device window: copy-back descriptor pool exhausted, blind copy-back\n");
        return false;
    }
    VkDescriptorBufferInfo bi[3] = {{gdev_buf, 0, GDEV_SIZE}, {gdev_shadow, 0, GDEV_SIZE}, {gmem_buf, 0, gmem_size}};
    VkWriteDescriptorSet w[3];
    for (uint32_t i = 0; i < 3; i++)
        w[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = i, .descriptorCount = 1,
                                      .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &bi[i]};
    vkUpdateDescriptorSets(g_vk.dev, 3, w, 0, NULL);
    wb_set = set;
    wb_set_gmem = gmem_buf;
    return true;
}

// End of the upload buffer: the shadow takes the regions as uploaded (window -> shadow, VRAM to VRAM, so a CPU write
// racing the upload can't make the two copies differ).
static void gdev_shadow_sync(VkCommandBuffer up) {
    if (wb_mode != 1) return;
    uint64_t cur = submit_seq + 1;
    VkBufferCopy bc[256];
    uint32_t n = 0;
    for (int i = 0; i < gdev_n; i++)  // (carried regions keep the shadow of their first upload: the final diff covers every chunk)
        if (gdev_reg[i].seq == cur && gdev_reg[i].up == cur) bc[n++] = (VkBufferCopy){gdev_reg[i].off, gdev_reg[i].off, gdev_reg[i].size};
    if (!n) return;
    vk_full_barrier(up);
    vkCmdCopyBuffer(up, gdev_buf, gdev_shadow, n, bc);
}

// Regions used by the open submission (copies of their placement), for guest_direct's per-draw lookups: the region
// table keeps up to 256 stale regions, and scanning it for every index / vertex binding cost ~1 us per draw.
static struct { uint64_t addr, size, off; } gdev_lv[256];
static int gdev_lv_n;
static uint64_t gdev_lv_seq;
static uint32_t gdev_gen;   // bumped wherever gdev_reg gains, loses or moves a region (gdev_find's cache)
static GdevRegion *gdev_find(uint64_t addr, uint64_t size) {  // the region containing [addr, addr + size)
    if (rt_lookup()) {   // (the first match of the scan below, remembered until the table changes)
        static struct { uint64_t addr, size; uint32_t gen; int i; } fc[16];
        typeof(fc[0]) *e = &fc[((addr >> 8) ^ size) & 15];
        if (e->gen == gdev_gen + 1 && e->addr == addr && e->size == size) return e->i < 0 ? NULL : &gdev_reg[e->i];
        int i = 0;
        while (i < gdev_n && !(addr >= gdev_reg[i].addr && addr + size <= gdev_reg[i].addr + gdev_reg[i].size)) i++;
        *e = (typeof(fc[0])){addr, size, gdev_gen + 1, i < gdev_n ? i : -1};
        return i < gdev_n ? &gdev_reg[i] : NULL;
    }
    for (int i = 0; i < gdev_n; i++)
        if (addr >= gdev_reg[i].addr && addr + size <= gdev_reg[i].addr + gdev_reg[i].size) return &gdev_reg[i];
    return NULL;
}

// The address a storage buffer binding gets in the driver table: its window alias when a shader writes it or it
// lies in a region already in the window, else the guest address unchanged.
// LARECOMP_GDEV_HOST=<hex sizes,...> (bisecting tool): storage buffer bindings of those sizes, and every binding that
// overlaps memory such a binding covered, stay in host memory (consistently, so readers and writers agree).
static bool gdev_host_rule(uint64_t addr, uint64_t size) {
    static uint64_t sizes[32], ex_a[512], ex_s[512];
    static int nsizes = -1, nex;
    if (nsizes < 0) {
        nsizes = 0;
        for (const char *p = getenv("LARECOMP_GDEV_HOST"); p && *p && nsizes < 32;) {
            sizes[nsizes++] = strtoull(p, (char **)&p, 16);
            while (*p == ',') p++;
        }
        if (nsizes) LOG("[vk] device window: %d binding sizes kept in host memory (LARECOMP_GDEV_HOST)\n", nsizes);
    }
    if (!nsizes) return false;
    static long from = -1;  // LARECOMP_GDEV_HOST_AT=<present>: only from then on (a slow rule would desync scripted input)
    if (from < 0) { const char *e = getenv("LARECOMP_GDEV_HOST_AT"); from = e ? atol(e) : 0; }
    extern uint64_t renderer_present_count(void);
    if (from && renderer_present_count() < (uint64_t)from) return false;
    for (int i = 0; i < nex; i++) if (addr < ex_a[i] + ex_s[i] && ex_a[i] < addr + size) return true;
    for (int i = 0; i < nsizes; i++)
        if (size == sizes[i]) {
            if (nex < 512) { ex_a[nex] = addr; ex_s[nex] = size; nex++; }
            return true;
        }
    return false;
}

// Per 64 KB heap block: the last submission that bound it as a written storage buffer. guest_direct's heap copies read host
// memory at record time, so a range a submission still in flight writes (window copy-back or direct) is bound in place.
// Shader-written heap memory, per 4 KB page (submission seq + 1 of the last write). 64 KB blocks swept the game's
// CPU-written UI data in with the shader-written buffers beside it: read from host memory on the GPU timeline, after the
// game had already rewritten it for its next frame (UI flicker).
#define GDEV_SH 12
#define GDEV_BLKS ((4ull << 30) >> GDEV_SH)
static uint64_t gdev_blk[GDEV_BLKS];
static uint64_t g_gd_gen;   // guest_direct's memo: bumped when written-page marks or window regions change (G5)
// textures.c (LARECOMP_TEX_GPUW): may a shader have written any page of [addr, addr + size) in submission `since` or
// later? gdev_map marks the storage buffers a writing shader has bound (in place or through the device window's
// copy-back); CPU-side writes (render-target write-back, CopyTextureToBuffer, CopyBufferToBuffer, counters) show in
// write watch. Not marked: a store through a pointer the shader did not take from that table (raw gmem_ address:
// translate.c STG / RED / ATOM); textures.c still hashes every 4th period for it. Outside the heap: unknown, true.
bool vk_heap_gpu_written_since(uint64_t addr, uint64_t size, uint64_t since) {
    if (!size || addr < HEAP_BASE || (addr - HEAP_BASE + size - 1) >> GDEV_SH >= GDEV_BLKS) return true;
    for (uint64_t b = (addr - HEAP_BASE) >> GDEV_SH, be = (addr - HEAP_BASE + size - 1) >> GDEV_SH; b <= be; b++)
        if (gdev_blk[b] >= since) return true;
    return false;
}
// Chunked submissions carry their window write-back to the next one (gdev_carry) while gdev_blk keeps the writing
// submission's mark: the writes of a chain [gw_chain0, end] reach host memory only when its last submission completes
// (vk_submit_extra; declared by gdev_carry's). 0: no chain open.
// The oldest submission whose shader writes into the heap may not be in host memory yet: a texture hashed now has seen
// everything older (textures.c full_seq, the `since` above). The open submission's would be one too late: a write of a
// submission still running lands after the hash and its mark is older than the open one.
uint64_t vk_heap_settled_seq(void) {
    uint64_t s = vk_completed_seq() + 1;
    if (gw_chain0 && gw_chain0 < s) s = gw_chain0;
    if (gw_chain_end >= s && gw_chain_end0 < s) s = gw_chain_end0;
    return s;
}
// The exact storage buffer ranges bound for writing shaders, with the submission whose end puts them in host memory (carried
// with chunked submissions, gdev_carry). gdev_blk's 4 KB pages swept the game's CPU-written dynamic vertex data in with small
// shader-written buffers sharing its pages (Deck RACECHECK R4: 1010ef2e20 +2c0 bound in place, ~20 changes per 600
// SyncWaits, beside written 1010eb2x80 +10): the in-flight tests below use these ranges (LARECOMP_LIVE_PAGES=1: pages only).
enum { GW_N = 8192 };
static struct { uint64_t addr, size, seq; } gw_e[GW_N];
static uint64_t gw_head, gw_lost;   // gw_lost: the newest submission among entries the ring overwrote
static void gw_push(uint64_t addr, uint64_t size, uint64_t seq) {
    for (uint64_t k = gw_head; k > 0 && k + 16 > gw_head; k--) {   // (bindings alternate among a few buffers)
        const typeof(gw_e[0]) *p = &gw_e[(k - 1) % GW_N];
        if (p->addr == addr && p->size == size && p->seq == seq) return;
    }
    if (gw_head >= GW_N && gw_e[gw_head % GW_N].seq > gw_lost) gw_lost = gw_e[gw_head % GW_N].seq;
    gw_e[gw_head++ % GW_N] = (typeof(gw_e[0])){addr, size, seq};
}
// 1: a submission in (lo, hi] writes a byte of the range, 0: none, -1: unknown (the ring lost it)
static int gw_overlap(uint64_t addr, uint64_t size, uint64_t lo, uint64_t hi) {
    // (pages first: gdev_blk holds each page's newest writing submission, so none newer than lo means no entry in (lo:
    // gc-kak1, Kakariko, cbuf_info 1.2 ms a frame walking the open submission's entries)
    if (addr >= HEAP_BASE && size && size <= (64u << GDEV_SH)) {
        uint64_t top = 0;
        for (uint64_t b = (addr - HEAP_BASE) >> GDEV_SH, be = (addr + size - 1 - HEAP_BASE) >> GDEV_SH; b <= be && b < GDEV_BLKS; b++)
            if (gdev_blk[b] > top) top = gdev_blk[b];
        if (top <= lo) return 0;
    }
    if (gw_lost > lo) return -1;
    for (uint64_t k = gw_head; k > 0 && k + GW_N > gw_head; k--) {
        const typeof(gw_e[0]) *p = &gw_e[(k - 1) % GW_N];
        if (p->seq <= lo) break;   // (submissions never decrease along the ring)
        if (p->seq <= hi && addr < p->addr + p->size && p->addr < addr + size) return 1;
    }
    return 0;
}
static bool live_pages(void) {
    static int on = -1;
    if (on < 0) on = getenv("LARECOMP_LIVE_PAGES") != NULL;
    return on;
}
static uint64_t gdev_map(uint64_t addr, uint64_t size, bool write) {
    if (write && addr >= HEAP_BASE && size) {
        static uint64_t la, ls, lseq;
        if (la != addr || ls != size || lseq != submit_seq + 1) {
            la = addr; ls = size; lseq = submit_seq + 1;
            for (uint64_t b = (addr - HEAP_BASE) >> GDEV_SH, be = (addr + size - 1 - HEAP_BASE) >> GDEV_SH; b <= be && b < GDEV_BLKS; b++) gdev_blk[b] = lseq;
            g_gd_gen++;
            gw_push(addr, size, lseq);
        }
    }
    if (!addr || !size || addr < HEAP_BASE || addr + size > HEAP_BASE + gmem_size || !gdev_ready()) return addr;
    if (gdev_host_rule(addr, size)) return addr;
    uint64_t cur = submit_seq + 1;
    GdevRegion *r = gdev_find(addr, size);
    const uint64_t half = GDEV_SIZE / 2;
    static int oldfull = -1;   // LARECOMP_GDEV_OLDFULL=1: the old window-full rule (A/B)
    if (oldfull < 0) oldfull = getenv("LARECOMP_GDEV_OLDFULL") != NULL;
    // an older region in the other half is re-made in this one: a half is then only pinned by submissions that copied into it
    // while it was the current half, so switching back to it waits for nothing recent
    if (r && !oldfull && r->seq != cur && (int)(r->off >= half) != gdev_half) { *r = gdev_reg[--gdev_n]; r = NULL; gdev_gen++; }
    if (!r) {
        // Storage buffers shaders only read go through the window too (default since 2026-10-19; LARECOMP_GDEV_READS=0 = read
        // in place): filled from a record-time snapshot below, where in place they were read on the GPU timeline, after the
        // game could have rewritten them (UI flicker). Eden keeps every storage buffer in VRAM; the diff copy-back leaves
        // their host words alone.
        static int reads = -1;
        if (reads < 0) { const char *e = getenv("LARECOMP_GDEV_READS"); reads = !(e && *e == '0') && wb_init(); }   // (needs the diff copy-back)
        if ((!write && !reads) || size > GDEV_REGION_MAX) return addr;
        for (int i = 0; i < gdev_n; i++)
            if (addr < gdev_reg[i].addr + gdev_reg[i].size && gdev_reg[i].addr < addr + size) {
                if (gdev_reg[i].seq == cur && !write) {  // read-only use: its own snapshot copy is safe unless a shader of
                    // this submission writes the overlap (the UI's ring buffer slices overlap: left in host memory they were read
                    // on the GPU timeline after the game rewrote them = minimap text flicker)
                    uint64_t lo = addr > gdev_reg[i].addr ? addr : gdev_reg[i].addr, hi = addr + size < gdev_reg[i].addr + gdev_reg[i].size ? addr + size : gdev_reg[i].addr + gdev_reg[i].size;
                    bool wr = false;
                    for (uint64_t b = (lo - HEAP_BASE) >> GDEV_SH, be = (hi - 1 - HEAP_BASE) >> GDEV_SH; b <= be && b < GDEV_BLKS && !wr; b++) wr = gdev_blk[b] == cur;
                    if (!wr) continue;
                }
                if (gdev_reg[i].seq == cur) {  // overlaps one in use this submission: two copies would race
                    static int nlog;
                    if (nlog++ < 8 || g_debug_frame)
                        LOG("[vk] device window: %llx +%llx overlaps live region %llx +%llx, left in host memory\n", (unsigned long long)addr,
                            (unsigned long long)size, (unsigned long long)gdev_reg[i].addr, (unsigned long long)gdev_reg[i].size);
                    gdev_stat_host++;
                    return addr;
                }
                gdev_reg[i--] = gdev_reg[--gdev_n];  // stale (copied back already): drop it
                gdev_gen++;
            }
        // Two halves used in turn. The old "full: reset" needed no region of the current submission in use, which mid-frame
        // never held: from the first fill on every new region silently stayed in host memory, read on the GPU timeline after
        // the game had rewritten it (UI ring slices: minimap text flicker with SyncWait lag).
        if (gdev_n == 256 && !oldfull) {   // table full: older regions are only a lookup cache (their window space is guarded per half)
            for (int i = 0; i < gdev_n; i++) if (gdev_reg[i].seq != cur) gdev_reg[i--] = gdev_reg[--gdev_n];
            gdev_gen++;
        }
        uint64_t off = (gdev_top + 255) & ~255ull;
        if (oldfull && (off + size > GDEV_SIZE || gdev_n == 256)) {
            for (int i = 0; i < gdev_n; i++) if (gdev_reg[i].seq == cur) { gdev_stat_host++; return addr; }
            vk_wait_seq(submit_seq);
            gdev_n = 0;
            gdev_gen++;
            off = 0;
        } else if (off + size > (uint64_t)(gdev_half + 1) * half) {  // this half is full: switch to the other one
            int h = !gdev_half;
            if (gdev_half_seq[h] == cur) off = ~0ull;   // (one submission filled both halves)
            else {
                if (gdev_half_seq[h] > vk_completed_seq()) vk_wait_seq(gdev_half_seq[h]);
                for (int i = 0; i < gdev_n; i++) if ((int)(gdev_reg[i].off >= half) == h) gdev_reg[i--] = gdev_reg[--gdev_n];
                gdev_gen++;
                gdev_half = h;
                off = (uint64_t)h * half;
            }
        }
        if (off == ~0ull || gdev_n == 256 || size > half) {   // (rare: counted in the stats line)
            static int nlog;
            if (nlog++ < 8) LOG("[vk] device window: %llx +%llx left in host memory (%d regions, half %d)\n", (unsigned long long)addr,
                                (unsigned long long)size, gdev_n, gdev_half);
            gdev_stat_host++;
            return addr;
        }
        r = &gdev_reg[gdev_n++];
        *r = (GdevRegion){addr, size, off, 0, 0};
        gdev_gen++;
        gdev_top = off + size;
        gdev_stat_regions++;
    }
    if (r->seq != cur) {  // first use in this submission: host -> VRAM, after the previous submission's copy-back
        VkCommandBuffer up = vk_upload_cmd();
        if (gdev_upload_seq != cur) { vk_full_barrier(up); gdev_upload_seq = cur; }
        VkBufferCopy bc = {r->addr - HEAP_BASE, r->off, r->size};
        // Read-only use: snapshot the CPU's data now, as the vertex mirror does (copied on the GPU timeline it can already
        // hold the game's next frame: SyncWait lets the game run one fence ahead of the GPU). Pages a shader wrote in a
        // submission still in flight reach host memory by its copy-back on the GPU timeline: those stay GPU copies.
        bool snap = !write;
        static int snap_all = -1;  // LARECOMP_GDEV_SNAP_ALL=1 (diagnostic, can drop in-flight GPU writes): snapshot every read-only use
        if (snap_all < 0) snap_all = getenv("LARECOMP_GDEV_SNAP_ALL") != NULL;
        uint64_t done = snap ? (snap_all ? ~0ull : vk_completed_seq()) : 0;
        for (uint64_t b = (r->addr - HEAP_BASE) >> GDEV_SH, be = (r->addr + r->size - 1 - HEAP_BASE) >> GDEV_SH; snap && b <= be && b < GDEV_BLKS; b++)
            if (gdev_blk[b] > done) snap = false;
        if (snap) {
            int k0 = vk_up_kind;
            vk_up_kind = 2;
            VkSpan sp = vk_transient(r->size, 256);
            vk_up_kind = k0;
            memcpy(sp.ptr, (const void *)(uintptr_t)r->addr, r->size);
            bc.srcOffset = sp.off;
            vkCmdCopyBuffer(up, sp.buf, gdev_buf, 1, &bc);
            gdev_stat_snap += r->size;
        } else
            vkCmdCopyBuffer(up, gmem_buf, gdev_buf, 1, &bc);
        if (gdev_chk_on()) vkCmdCopyBuffer(up, gmem_buf, chk_buf[0], 1, &bc);  // (LARECOMP_GDEV_CHECK: host at the upload)
        wb_init();  // (first use: the copy-back pipeline + shadow; gdev_shadow_sync fills the shadow at the end of the upload buffer)
        r->seq = cur;
        r->up = cur;
        gdev_half_seq[r->off >= GDEV_SIZE / 2] = cur;
        if (gdev_lv_seq != cur) { gdev_lv_seq = cur; gdev_lv_n = 0; }
        if (gdev_lv_n < 256) { gdev_lv[gdev_lv_n].addr = r->addr; gdev_lv[gdev_lv_n].size = r->size; gdev_lv[gdev_lv_n].off = r->off; gdev_lv_n++; }
        gdev_stat_in += r->size;
    }
    return HEAP_BASE + HEAP_MAX + r->off + (addr - r->addr);
}

// LARECOMP_GDEV_CHECK=1 (diagnostic): does host memory of a region change while the GPU runs the submission that has
// it in the window (guest CPU writes, or GPU writes that bypass the window)? The blind copy-back below would overwrite
// them. Host contents are snapshotted on the GPU timeline at the upload (chk 0) and just before the copy-back (chk 1);
// the next submit waits for that submission and compares ("[gdev-check]" lines).
static int gdev_chk = -1;
static VkBuffer chk_buf[2];
static VmaAllocation chk_mem[2];
static uint8_t *chk_ptr[2];
static struct { uint64_t addr, size, off; } chk_pend[256];
static int chk_n;
static uint64_t chk_seq, chk_words, chk_clob;
static bool gdev_chk_on(void) {
    if (gdev_chk < 0) {
        gdev_chk = getenv("LARECOMP_GDEV_CHECK") != NULL;
        for (int i = 0; gdev_chk && i < 2; i++) {
            VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = GDEV_SIZE, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
            VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                                          .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT};
            VmaAllocationInfo info;
            VK_CHECK(vmaCreateBuffer(g_vk.vma, &bi, &ai, &chk_buf[i], &chk_mem[i], &info));
            chk_ptr[i] = info.pMappedData;
        }
    }
    return gdev_chk;
}
static void gdev_check_prev(void) {
    if (!chk_n) return;
    vk_wait_seq(chk_seq);
    static int nlog;
    for (int k = 0; k < chk_n; k++) {
        const uint32_t *a = (const uint32_t *)(chk_ptr[0] + chk_pend[k].off), *b = (const uint32_t *)(chk_ptr[1] + chk_pend[k].off);
        const uint32_t *now = (const uint32_t *)(uintptr_t)chk_pend[k].addr;
        uint64_t words = chk_pend[k].size / 4, diff = 0, clob = 0, first = ~0ull;
        for (uint64_t i = 0; i < words; i++)
            if (a[i] != b[i]) { diff++; if (now[i] != b[i]) { clob++; if (first == ~0ull) first = i; } }
        chk_words += diff;
        chk_clob += clob;
        if (diff && (nlog++ < 400 || g_debug_frame))
            LOG("[gdev-check] seq %llu region %llx +%llx: host changed in %llu words while in the window, %llu overwritten by the copy-back"
                "%s (first +%llx: start %08x changed %08x final %08x)\n", (unsigned long long)chk_seq, (unsigned long long)chk_pend[k].addr,
                (unsigned long long)chk_pend[k].size, (unsigned long long)diff, (unsigned long long)clob, "",
                (unsigned long long)(first == ~0ull ? 0 : first * 4), first == ~0ull ? 0 : a[first], first == ~0ull ? 0 : b[first],
                first == ~0ull ? 0 : now[first]);
    }
    chk_n = 0;
}

// End of a submission's main command buffer (no pass open): the regions it used go back to host memory.
static bool gdev_carry_ok(void) {
    static int off = -1;
    if (off < 0) off = getenv("LARECOMP_CHUNK_NO_CARRY") != NULL;
    return !off && wb_mode == 1 && !gdev_chk_on();
}
static void gdev_carry(uint64_t from, uint64_t to) {
    for (int i = 0; i < gdev_n; i++) if (gdev_reg[i].seq == from) gdev_reg[i].seq = to;
    for (uint64_t k = gw_head; k > 0 && k + GW_N > gw_head && gw_e[(k - 1) % GW_N].seq >= from; k--)   // (newest entries)
        if (gw_e[(k - 1) % GW_N].seq == from) gw_e[(k - 1) % GW_N].seq = to;
    if (gdev_lv_seq == from) gdev_lv_seq = to;
    g_gd_gen++;
}

static void gdev_writeback(VkCommandBuffer cb) {
    uint64_t cur = submit_seq + 1;
    VkBufferCopy bc[256];
    uint32_t n = 0;
    for (int i = 0; i < gdev_n; i++)
        if (gdev_reg[i].seq == cur) bc[n++] = (VkBufferCopy){gdev_reg[i].off, gdev_reg[i].addr - HEAP_BASE, gdev_reg[i].size};
    if (!n) return;
    vk_full_barrier(cb);
    if (gdev_chk_on()) {  // host as it is right before the copy-back
        VkBufferCopy hc[256];
        for (uint32_t i = 0; i < n; i++) {
            hc[i] = (VkBufferCopy){bc[i].dstOffset, bc[i].srcOffset, bc[i].size};
            chk_pend[i].addr = bc[i].dstOffset + HEAP_BASE; chk_pend[i].size = bc[i].size; chk_pend[i].off = bc[i].srcOffset;
        }
        vkCmdCopyBuffer(cb, gmem_buf, chk_buf[1], n, hc);
        vk_full_barrier(cb);
        chk_n = (int)n;
        chk_seq = cur;
    }
    if (wb_mode == 1 && wb_set_ready()) {  // diff copy-back: one dispatch per region; unaligned regions copied whole
        VkBufferCopy whole[256];
        uint32_t nw = 0;
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, wb_pipe);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, wb_layout, 0, 1, &wb_set, 0, NULL);
        for (uint32_t i = 0; i < n; i++) {
            if (((bc[i].dstOffset | bc[i].size) & 3) || bc[i].dstOffset / 4 > 0xFFFFFFFFull) { whole[nw++] = bc[i]; continue; }
            uint32_t pc[3] = {(uint32_t)(bc[i].srcOffset / 4), (uint32_t)(bc[i].dstOffset / 4), (uint32_t)(bc[i].size / 4)};
            vkCmdPushConstants(cb, wb_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, pc);
            vkCmdDispatch(cb, (pc[2] + 255) / 256, 1, 1);
        }
        if (nw) vkCmdCopyBuffer(cb, gdev_buf, gmem_buf, nw, whole);
    } else
        vkCmdCopyBuffer(cb, gdev_buf, gmem_buf, n, bc);
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT | VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT | VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    vkCmdPipelineBarrier2(cb, &di);
}

// Vertex / index data read in place from the imported guest heap (renderer.m bind_guest does the same with
// g_guest_buffer): no per-draw copy, no index scan for the copy size. The game fences its CPU writes as on the Switch.
// Falls back to copies outside the heap or without the import LARECOMP_VK_COPY_VERTICES=1 forces copies. The alias
// region's import window (hle_os_mem.cpp, TOTK_ALIAS_IMPORT_MB): sead PageHeap memory where TotK keeps much of its
// vertex / index data. Imported once, on first use; shaders never write it through the device window (gdev covers the
// heap only): bound in place, the GPU read ~40 MB of vertex data per frame over PCIe (GPU busy 575 -> 1250 ms per 2 s,
// fps unchanged). So draws read a VRAM mirror of the window instead; the pages the guest wrote since the previous
// submission (Windows write watch, alias_dirty_take) are copied sysmem -> VRAM at the head of each submission that binds
// alias data (LARECOMP_VK_ALIAS_MIRROR=0: bind the sysmem import directly). Off by default on an integrated GPU (Steam
// Deck APU, 2026-10-16): its "VRAM" is the same RAM the import already sits in, so the mirror would only cost 2 GB of
// the 16 shared and a copy per submission (LARECOMP_VK_ALIAS_MIRROR=1 forces it on).
extern uint64_t g_alias_import_base, g_alias_import_size;
extern int alias_dirty_take(uint64_t *offs, uint64_t *lens, int max, uint64_t *bytes);
extern int alias_dirty_take_range(uint64_t off, uint64_t len, uint64_t *offs, uint64_t *lens, int max, uint64_t *bytes);
extern int heap_mirror_take(uint64_t addr, uint64_t len, uint64_t *offs, uint64_t *lens, int max, uint64_t *bytes);

// the mirrors' write-watch queries, batched per command list (LARECOMP_WW_BATCH=1 OFF by default: kg5 off / on, Kakariko,
// exclusive: queries -62 % but alias sync 53 -> 49 ms per 2 s, ~0.02 ms a frame, the dirty-page scan and copy being the
// cost, not the calls; not worth moving the query to the list's start). Each list syncs every chunk it binds once, with one
// GetWriteWatch each (cx5_kakariko ~150 alias chunk syncs a frame). At a list's first sync, the chunks the previous list
// synced are queried in contiguous runs, one call per run (at most 64 MB); the dirty pages go to a per-chunk pending bitmap
// that a chunk's sync consumes instead of querying. A chunk this list does not bind keeps its pages pending for the next
// list that does, so no write is lost; only the moment of the query moves to the list's start (before its first bind, as
// the per-chunk query was for the first chunk).
typedef int (*WwTake)(uint64_t off, uint64_t len, uint64_t *offs, uint64_t *lens, int max, uint64_t *bytes);
static int ww_take_alias(uint64_t off, uint64_t len, uint64_t *offs, uint64_t *lens, int max, uint64_t *bytes) {
    return alias_dirty_take_range(off, len, offs, lens, max, bytes);
}
static int ww_take_heap(uint64_t off, uint64_t len, uint64_t *offs, uint64_t *lens, int max, uint64_t *bytes) {
    return heap_mirror_take(HEAP_BASE + off, len, offs, lens, max, bytes);   // (offsets come back from the heap base)
}
typedef struct {
    uint64_t *pend;      // 256 bits per 1 MB chunk: pages a query found written, not yet synced
    uint64_t *fetched;   // per chunk: the list whose batch queried it
    uint64_t nch, key;   // key: the list cur[] belongs to
    uint32_t ncur, cur[1024];
} WwBatch;
static WwBatch ww_alias, ww_heap;
uint64_t prof_ww_calls, prof_ww_batched;   // (VK_PROF: queries made, chunk syncs served from a batch)
static int ww_batch_on(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("LARECOMP_WW_BATCH"); on = e && *e != '0'; }
    return on;
}
static void ww_query(WwBatch *w, WwTake take, uint64_t off, uint64_t len, uint64_t total, uint64_t *bytes) {
    static uint64_t offs[4096], lens[4096];
    uint64_t b = 0;
    int n = take(off, len, offs, lens, 4096, &b);
    prof_ww_calls++;
    if (n < 0) { offs[0] = off; lens[0] = len; n = 1; b = len; }   // (no write watch / outside the heap: all of it)
    *bytes += b;
    for (int i = 0; i < n; i++)
        for (uint64_t o = offs[i] & ~0xFFFull, e = offs[i] + lens[i] < total ? offs[i] + lens[i] : total; o < e; o += 4096)
            w->pend[o >> 18] |= 1ull << ((o >> 12) & 63);   // (page o >> 12: word o >> 18, 4 words a chunk)
}
static int ww_cmp(const void *a, const void *b) { uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b; return x < y ? -1 : x > y; }
// The dirty pages of chunk ch (1 MB at ch << 20 of a window of `total` bytes) for list `key`: pg[i] = 1 for page i written
// since its last sync. *bytes: what the queries reported (prof). false: no memory for the tables (caller queries itself).
static bool ww_chunk_dirty(WwBatch *w, WwTake take, uint64_t ch, uint64_t key, uint64_t total, uint8_t pg[256], uint64_t *bytes) {
    if (!w->pend) {
        w->nch = (total + (1u << 20) - 1) >> 20;
        w->pend = calloc(w->nch * 4, 8);
        w->fetched = calloc(w->nch, 8);
        if (!w->pend || !w->fetched) { free(w->pend); free(w->fetched); w->pend = w->fetched = NULL; return false; }
    }
    if (ch >= w->nch) return false;
    if (ww_batch_on()) {
        if (w->key != key) {   // this list's first sync: the previous list's chunks, one query per contiguous run
            uint32_t np = w->ncur;
            static uint32_t prev[1024];
            memcpy(prev, w->cur, np * 4);
            w->ncur = 0; w->key = key;
            qsort(prev, np, 4, ww_cmp);
            for (uint32_t i = 0; i < np;) {
                uint32_t j = i;
                while (j + 1 < np && prev[j + 1] == prev[j] + 1 && prev[j + 1] - prev[i] < 64) j++;
                if (prev[j] < w->nch) {
                    uint64_t off = (uint64_t)prev[i] << 20, end = ((uint64_t)prev[j] + 1) << 20;
                    ww_query(w, take, off, (end < total ? end : total) - off, total, bytes);
                    for (uint32_t k = prev[i]; k <= prev[j]; k++) w->fetched[k] = key;
                }
                i = j + 1;
            }
        }
        if (w->ncur < 1024) w->cur[w->ncur++] = (uint32_t)ch;
    }
    if (w->fetched[ch] == key) prof_ww_batched++;
    else {
        uint64_t off = ch << 20, len = total - off < (1u << 20) ? total - off : (1u << 20);
        ww_query(w, take, off, len, total, bytes);
    }
    uint64_t *bw = &w->pend[ch * 4];
    for (int i = 0; i < 256; i++) pg[i] = bw[i >> 6] >> (i & 63) & 1;
    bw[0] = bw[1] = bw[2] = bw[3] = 0;
    return true;
}
static VkBuffer alias_buf, alias_dev;   // (alias_dev: non-null = the VRAM mirror is on; its memory is in al_blk blocks)
static VkDeviceMemory alias_mem;
// The VRAM mirrors are made of 16 MB blocks, each allocated when a draw first binds a range in it. A range that
// straddles two blocks is copied per draw.
#define MBLK_SH 24
static uint64_t g_mirror_blk_bytes[2];   // allocated block bytes: heap, alias
static uint64_t g_mirror_remade[2];      // blocks made again after vk_mirror_release_tick freed them: heap, alias
static VkBuffer mirror_block(VkBuffer *bl, VmaAllocation *bm, uint64_t i, uint64_t len, VkBufferUsageFlags usage, int which) {
    if (!bl[i]) {
        VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = len, .usage = usage};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, .requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT};
        if (vmaCreateBuffer(g_vk.vma, &bi, &ai, &bl[i], &bm[i], NULL) != VK_SUCCESS) bl[i] = VK_NULL_HANDLE;
        else g_mirror_blk_bytes[which] += len;
    }
    return bl[i];
}
// LARECOMP_MIRROR_HOT=<n> (default 3, race mode 2026-10-30; 0 = off, 8 GB cards): the GPU never needs a mirror (the CPU
// never reads one: guest data reaches a block by GPU copies, GPU writes go back through the device window), it only saves
// the per-draw copy of a range bound often. While VRAM or commit is short (residency reported pressure in the last 10 s:
// vk_residency_need_ns, the need before its own frees), a heap / alias block that has no mirror is made only once draws
// bound it in n different submissions of one 2 s window; until then those draws copy their range, the per-draw snapshot
// that straddling ranges and volatile pages already take (the same bytes: no visual change, only that copy's CPU time).
// Without pressure, blocks are made on first use as before. A range bound once in a while (a far area's vertices, a menu's
// constants) then holds no 16 MB block, and LARECOMP_MIRROR_RELEASE does not free and re-make it.
static int mirror_hot_n = -1;
static bool mirror_short;               // (pressure in the last 10 s; set by vk_mirror_release_tick)
static uint64_t *mh_sub[2], *mh_t0[2], mh_nblk[2];   // per block: last submission counted, its window's start (ns)
static uint32_t *mh_cnt[2];                            // per block: submissions counted in the window
static uint64_t prof_mh_refused[2], prof_mh_made[2];   // binds copied for a cold block / blocks made once hot: heap, alias
static bool mirror_hot_on(void) {
    if (mirror_hot_n < 0) {
        const char *e = getenv("LARECOMP_MIRROR_HOT");
        mirror_hot_n = e ? atoi(e) : 3;
        if (mirror_hot_n > 0) LOG("[vram] mirror blocks under pressure: made after %d submissions bound them in 2 s (LARECOMP_MIRROR_HOT)\n", mirror_hot_n);
    }
    return mirror_hot_n > 0;
}
// May a bind make block k of mirror w (0 heap, 1 alias, nblk blocks)? false = the draw copies its range.
static bool mirror_hot(int w, uint64_t k, uint64_t nblk) {
    if (!mirror_short || !mirror_hot_on()) return true;
    if (mh_nblk[w] != nblk) {   // (first use, or the alias mirror made again after vk_exec_reset)
        free(mh_sub[w]); free(mh_t0[w]); free(mh_cnt[w]);
        mh_sub[w] = calloc(nblk, 8); mh_t0[w] = calloc(nblk, 8); mh_cnt[w] = calloc(nblk, 4);
        mh_nblk[w] = mh_sub[w] && mh_t0[w] && mh_cnt[w] ? nblk : 0;
    }
    if (k >= mh_nblk[w]) return true;
    uint64_t cur = submit_seq + 1;
    if (mh_sub[w][k] != cur) {
        uint64_t now = SDL_GetTicksNS();
        mh_sub[w][k] = cur;
        if (now - mh_t0[w][k] > 2000000000ull) { mh_t0[w][k] = now; mh_cnt[w][k] = 0; }
        mh_cnt[w][k]++;
    }
    if (mh_cnt[w][k] >= (uint32_t)mirror_hot_n) { prof_mh_made[w]++; return true; }
    prof_mh_refused[w]++;
    return false;
}
// LARECOMP_MIRROR_STRADDLE: a range across a block edge is joined from its blocks by the GPU (upload buffer, after the
// blocks' syncs, into a ring span) instead of copied by the CPU for every list that binds it. gc-w2-base (open world, 2
// s): 4209 vertex copies of 1830 MB, the ones logged an 853 KB alias-window range 0x10ad7d0 into a block's 16 MB; the
// replay's profile had 29% of the render thread in vk_transient_copy's memcpy into the write-combined ring.
static uint64_t mirror_writes;   // bumped by every copy recorded into a mirror block (a join reading them needs a barrier)
static bool mirror_join_read;    // a join read the blocks since: the next write into one waits for it
static void mirror_write(VkCommandBuffer up) {
    mirror_writes++;
    if (mirror_join_read) { vk_full_barrier(up); mirror_join_read = false; }
}
static bool mirror_straddle_on(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("LARECOMP_MIRROR_STRADDLE"); on = !e || *e != '0'; if (on) LOG("[vk] mirror block edges: joined on the GPU\n"); }
    return on;
}
#define MST_MAX (32ull << 20)   // (bigger ranges are copied as before)
// [off, off + size) of a mirror whose blocks bl[] all exist and are synced for this list (key): one span holding it.
static VkSpan mirror_straddle(VkBuffer *bl, uint64_t off, uint64_t size, uint64_t key) {
    static struct { VkBuffer *bl; uint64_t off, size, key, seq; VkSpan span; } memo[64];
    uint64_t pend = vk_pending_seq();
    unsigned h = (unsigned)((off >> 4) ^ (off >> 16) ^ size) & 63;
    if (memo[h].bl == bl && memo[h].off == off && memo[h].size == size && memo[h].key == key && memo[h].seq == pend) {
        prof_mst_shared++;
        return memo[h].span;
    }
    int k0 = vk_up_kind;
    vk_up_kind = 2;
    VkSpan sp = vk_transient(size, 256);
    vk_up_kind = k0;
    VkCommandBuffer up = vk_upload_cmd();
    static uint64_t seen_writes = ~0ull, seen_seq;
    if (seen_writes != mirror_writes || seen_seq != pend) { vk_full_barrier(up); seen_writes = mirror_writes; seen_seq = pend; }
    for (uint64_t o = off, e = off + size; o < e;) {
        uint64_t k = o >> MBLK_SH, ke = (k + 1) << MBLK_SH < e ? (k + 1) << MBLK_SH : e;
        vkCmdCopyBuffer(up, bl[k], sp.buf, 1, &(VkBufferCopy){o - (k << MBLK_SH), sp.off + (o - off), ke - o});
        o = ke;
    }
    mirror_join_read = true;
    prof_mst_n++;
    prof_mst_bytes += size;
    memo[h].bl = bl; memo[h].off = off; memo[h].size = size; memo[h].key = key; memo[h].seq = pend; memo[h].span = sp;
    return sp;
}
static VkBuffer *al_blk;
static VmaAllocation *al_blk_mem;
static uint64_t al_nblk;
static uint64_t *al_blk_use;   // per block: the last submission that bound or wrote it (mirror_release_tick)
static uint8_t *al_blk_rel;    // released (counted when re-made)
static uint64_t alias_sync_seq;
static uint64_t *alias_chunk_key, alias_chunk_n;  // lazy sync: per 1 MB chunk, the list / submission key it was last synced for
// Lazy sync snapshots the dirty pages at the sync (CPU copy into upload staging), as the heap mirror does: copied from the
// import when the GPU ran the upload, they could already hold the game's next frame (SyncWait lets the game run a fence
// ahead of the GPU): the minimap's coordinate text was drawn with another string's glyph vertices for single frames. Pages
// rewritten between two lists of one submission stay out of the mirror for the rest of it (draws binding them copy) and
// catch up at the next submission (al_pg_*: as hm_pg_* in heap_mirror).
static uint64_t *al_pg_sub, *al_pg_vol, *al_chunk_vol;
static uint8_t *al_pg_stale;
uint64_t prof_alias_syncs, prof_alias_bytes, prof_alias_ranges, prof_alias_ns;
static int alias_state;  // 0 untried, 2 importing on its thread (alias_import_kick), 1 imported, -1 failed / off
// The window's import pins all of it in the driver (vkAllocateMemory of a host pointer, 2 GB): 323 ms on the render
// thread in the title's first frame that drew alias data. So it runs on its own thread, started by the first execute
// after hle_os_mem committed the window (alias_import_kick), long before that frame; until it is done alias data is
// copied, as without the import. The mirror setup (the write-watch probe, the block tables) stays here, at the first bind
// after it finished. LARECOMP_ALIAS_IMPORT_ASYNC=0: imported at the first bind, on the render thread, as before.
static _Atomic int alias_imp_done;   // (the import thread: 1 imported, -1 failed)
static pthread_t alias_imp_thr;
static VkBuffer alias_imp_buf;
static VkDeviceMemory alias_imp_mem;
static uint64_t alias_imp_ns;
static void *alias_import_main(void *a) {
    (void)a;
    uint64_t t0 = SDL_GetTicksNS();
    bool ok = host_import(g_alias_import_base, g_alias_import_size, NULL, &alias_imp_buf, &alias_imp_mem);
    alias_imp_ns = SDL_GetTicksNS() - t0;
    atomic_store(&alias_imp_done, ok ? 1 : -1);
    return NULL;
}
static void alias_import_kick(void) {   // (each execute, render thread: one compare once started or decided)
    if (alias_state || !g_alias_import_size) return;
    static int on = -1;
    if (on < 0) { const char *e = getenv("LARECOMP_ALIAS_IMPORT_ASYNC"); on = !e || *e != '0'; }
    if (!on || !g_vk.host_import || getenv("LARECOMP_VK_NO_ALIAS_IMPORT")) return;   // (alias_direct decides, as before)
    atomic_store(&alias_imp_done, 0);
    if (pthread_create(&alias_imp_thr, NULL, alias_import_main, NULL) == 0) alias_state = 2;
}
static bool alias_direct(uint64_t addr, uint64_t size, VkBuffer *buf, VkDeviceSize *off) {
    uint64_t base = g_alias_import_base, wsize = g_alias_import_size;
    if (!wsize || addr < base || addr + size > base + wsize || alias_state < 0) return false;
    if (alias_state != 1) {
        if (alias_state == 2) {
            int d = atomic_load(&alias_imp_done);
            if (!d) return false;   // (still importing: copied)
            pthread_join(alias_imp_thr, NULL);
            if (d > 0) { alias_buf = alias_imp_buf; alias_mem = alias_imp_mem; }
            alias_state = d;
        } else
            alias_state = g_vk.host_import && !getenv("LARECOMP_VK_NO_ALIAS_IMPORT") && host_import(base, wsize, NULL, &alias_buf, &alias_mem) ? 1 : -1;
        char how[64] = "";
        if (alias_imp_ns) snprintf(how, sizeof how, " (imported on its own thread in %.0f ms)", alias_imp_ns / 1e6);
        LOG("[vk] alias window import %llx + %llu MB: %s%s\n", (unsigned long long)base, (unsigned long long)(wsize >> 20),
            alias_state > 0 ? "vertex / index data there bound in place" : "off or failed, copied", how);
        if (alias_state < 0) return false;
        const char *m = getenv("LARECOMP_VK_ALIAS_MIRROR");
        uint64_t probe;
        bool uma = g_vk.props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU;
        if ((m ? strcmp(m, "0") != 0 : !uma) && alias_dirty_take(NULL, NULL, 0, &probe) >= 0) {  // (first take: everything written so far)
            // (blocks: the probe take above reset the watch, so each block copies its whole span when it is made)
            al_nblk = (wsize + (1ull << MBLK_SH) - 1) >> MBLK_SH;
            al_blk = calloc(al_nblk, sizeof *al_blk);
            al_blk_mem = calloc(al_nblk, sizeof *al_blk_mem);
            al_blk_use = calloc(al_nblk, sizeof *al_blk_use);
            al_blk_rel = calloc(al_nblk, 1);
            alias_dev = al_blk && al_blk_mem && al_blk_use && al_blk_rel ? alias_buf : VK_NULL_HANDLE;
            LOG("[vk] alias window VRAM mirror (%llu MB in %llu blocks made on first use): %s\n", (unsigned long long)(wsize >> 20),
                (unsigned long long)al_nblk, alias_dev ? "on" : "off, sysmem");
            alias_sync_seq = submit_seq + 1;
            alias_sync_epoch = alias_epoch;
        }
    }
    // Lazy sync (default LARECOMP_VK_ALIAS_LAZY=0: the whole span at the first alias bind of each list): only the 1 MB
    // chunks this range covers, each once per list (or per submission: the whole-span scan was ~1.1 ms per list (98
    // syncs. Chunks never bound keep their write-watch bits and are copied when a draw first binds them.
    static int lazy = -1;
    if (lazy < 0) { const char *e = getenv("LARECOMP_VK_ALIAS_LAZY"); lazy = !e || strcmp(e, "0"); }
    uint64_t ab = (addr - base) >> MBLK_SH, abase = ab << MBLK_SH, ae = size ? (addr - base + size - 1) >> MBLK_SH : ab;
    VkBuffer mb = VK_NULL_HANDLE;
    if (alias_dev) {
        if (ae != ab && (!lazy || !mirror_straddle_on() || size > MST_MAX)) {  // straddles two mirror blocks: copied
            prof_mst_refused++; prof_mst_refused_bytes += size;
            return false;
        }
        for (uint64_t k = ab; k <= ae; k++)
            if (!al_blk[k] && !mirror_hot(1, k, al_nblk)) return false;   // (cold under pressure: copied, LARECOMP_MIRROR_HOT)
        for (uint64_t k = ab; k <= ae; k++) {
            al_blk_use[k] = submit_seq + 1;
            if (al_blk[k]) continue;
            if (al_blk_rel[k]) { al_blk_rel[k] = 0; g_mirror_remade[1]++; }   // (made again: the whole span copy below, as the first time)
            uint64_t kbase = k << MBLK_SH, blen = wsize - kbase < (1ull << MBLK_SH) ? wsize - kbase : (1ull << MBLK_SH);
            if (!mirror_block(al_blk, al_blk_mem, k, blen, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                              VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, 1))
                return false;
            VkCommandBuffer up = vk_upload_cmd();   // the block's whole span once (pages written since: the syncs below, after it)
            vk_full_barrier(up);
            VkBufferCopy bc = {kbase, 0, blen};
            vkCmdCopyBuffer(up, alias_buf, al_blk[k], 1, &bc);
            vk_full_barrier(up);
            mirror_writes++;
        }
        mb = al_blk[ab];
    }
    if (alias_dev && lazy) {
        static uint64_t barrier_seq;
        uint64_t *chunk_key = alias_chunk_key;
        if (!chunk_key) {
            chunk_key = alias_chunk_key = calloc(alias_chunk_n = (wsize + (1u << 20) - 1) >> 20, sizeof *chunk_key);
            al_chunk_vol = calloc(alias_chunk_n, 8);
            al_pg_sub = calloc(alias_chunk_n << 8, 8);
            al_pg_vol = calloc(alias_chunk_n << 8, 8);
            al_pg_stale = calloc(alias_chunk_n << 8, 1);
            if (!chunk_key || !al_chunk_vol || !al_pg_sub || !al_pg_vol || !al_pg_stale) { alias_chunk_key = NULL; return false; }
        }
        uint64_t cur = submit_seq + 1, key = in_execute ? alias_epoch : cur | (1ull << 62);
        for (uint64_t ch = (addr - base) >> 20, ce = (addr - base + (size ? size - 1 : 0)) >> 20; ch <= ce; ch++) {
            if (chunk_key[ch] == key) continue;
            chunk_key[ch] = key;
            static uint64_t offs[256], lens[256];
            uint64_t t0 = prof ? prof_now() : 0, bytes = 0, cb = ch << 20, clen = wsize - cb < (1u << 20) ? wsize - cb : (1u << 20);
            uint8_t pg[256] = {0};
            if (!ww_chunk_dirty(&ww_alias, ww_take_alias, ch, key, wsize, pg, &bytes)) {
                int n = alias_dirty_take_range(cb, clen, offs, lens, 256, &bytes);
                for (int i = 0; i < n; i++)
                    for (uint64_t o = offs[i]; o < offs[i] + lens[i]; o += 4096) pg[(o - cb) >> 12] = 1;
            }
            uint64_t snap = 0, np = (clen + 4095) >> 12;
            for (uint64_t i = 0; i < np; i++) {
                uint64_t q = (cb >> 12) + i;
                if (!pg[i] && al_pg_stale[q] && al_pg_sub[q] != cur) pg[i] = 1;  // left out last submission: catch up
                if (pg[i] && al_pg_sub[q] == cur) { pg[i] = 0; al_pg_stale[q] = 1; al_pg_vol[q] = cur; al_chunk_vol[ch] = cur; continue; }
                if (pg[i]) { al_pg_sub[q] = cur; al_pg_stale[q] = 0; snap += 4096; }
            }
            int nr = 0;
            uint64_t cbase = cb >> MBLK_SH << MBLK_SH;   // (the chunk's block: another one than the range's first when it straddles)
            if (snap) {
                VkBufferCopy bc[256];
                int k0 = vk_up_kind;
                vk_up_kind = 2;
                VkSpan sp = vk_transient(snap, 256);
                vk_up_kind = k0;
                uint64_t so = 0;
                for (uint64_t i = 0; i < np; i++) {
                    if (!pg[i]) continue;
                    uint64_t o = cb + (i << 12), l = o + 4096 > wsize ? wsize - o : 4096;
                    memcpy((uint8_t *)sp.ptr + so, (const void *)(uintptr_t)(base + o), l);
                    if (nr && bc[nr - 1].dstOffset + bc[nr - 1].size == o - cbase) bc[nr - 1].size += l;
                    else bc[nr++] = (VkBufferCopy){sp.off + so, o - cbase, l};
                    so += l;
                }
                VkCommandBuffer up = vk_upload_cmd();
                if (barrier_seq != cur) {  // once per submission: after the earlier submissions' reads of the mirror
                    vk_full_barrier(up);
                    barrier_seq = cur;
                }
                mirror_write(up);
                vkCmdCopyBuffer(up, sp.buf, al_blk[cb >> MBLK_SH], (uint32_t)nr, bc);
            }
            prof_alias_syncs++;
            prof_alias_bytes += bytes;
            prof_alias_ranges += nr;
            if (prof) prof_alias_ns += prof_now() - t0;
        }
        for (uint64_t ch = (addr - base) >> 20, ce = (addr - base + (size ? size - 1 : 0)) >> 20; ch <= ce; ch++) {
            if (al_chunk_vol[ch] != cur) continue;
            for (uint64_t q = (addr - base) >> 12, qe = (addr - base + (size ? size - 1 : 0)) >> 12; q <= qe; q++)
                if (al_pg_vol[q] == cur) return false;  // rewritten since the mirror got it this submission: copied instead
            break;
        }
        if (ae != ab) {
            VkSpan s = mirror_straddle(al_blk, addr - base, size, key);
            *buf = s.buf;
            *off = s.off;
            return true;
        }
    } else if (alias_dev && (in_execute ? alias_sync_epoch != alias_epoch : alias_sync_seq != submit_seq + 1)) {  // first alias bind of
        // this command list (or submission, outside one): guest writes -> VRAM
        // shortcut: copied on the GPU timeline (not snapshotted like the lazy path); only the LARECOMP_VK_ALIAS_LAZY=0 opt-out
        static uint64_t offs[4096], lens[4096];
        uint64_t t0 = prof ? prof_now() : 0, bytes = 0;
        int n = alias_dirty_take(offs, lens, 4096, &bytes);
        if (n > 0) {
            VkCommandBuffer up = vk_upload_cmd();
            vk_full_barrier(up);  // after the earlier submissions' reads of the mirror
            mirror_write(up);
            for (int i = 0; i < n; i++)
                for (uint64_t o = offs[i], e = offs[i] + lens[i]; o < e;) {
                    uint64_t k = o >> MBLK_SH, ke = ((k + 1) << MBLK_SH) < e ? (k + 1) << MBLK_SH : e;
                    if (al_blk[k]) { vkCmdCopyBuffer(up, alias_buf, al_blk[k], 1, &(VkBufferCopy){o, o - (k << MBLK_SH), ke - o}); al_blk_use[k] = submit_seq + 1; }
                    o = ke;
                }
        }
        alias_sync_seq = submit_seq + 1;
        alias_sync_epoch = alias_epoch;
        prof_alias_syncs++;
        prof_alias_bytes += bytes;
        prof_alias_ranges += n > 0 ? n : 0;
        if (prof) prof_alias_ns += prof_now() - t0;
    }
    *buf = alias_dev ? mb : alias_buf;
    *off = addr - base - (alias_dev ? abase : 0);
    if (!alias_dev) rc_record(2, addr, size);
    return true;
}
// The alias window's VRAM mirror only (never the in-place sysmem import): shader constants live there.
static bool alias_mirror_bind(uint64_t addr, uint64_t size, VkBuffer *buf, VkDeviceSize *off) {
    VkBuffer b;
    VkDeviceSize o;
    if (!alias_direct(addr, size, &b, &o) || !alias_dev || b == alias_buf) return false;
    *buf = b; *off = o;
    return true;
}

// VRAM mirror of the heap for vertex / index data (discrete GPU LARECOMP_VK_NO_HEAP_MIRROR=1, 2 s): the copies were 11 GB
// of pages already copied in that submission + 1.1 GB unchanged since an earlier one + ~5 MB really changed, and vertex
// copies + index scans took 582 ms of the 1926 ms busy render thread. Each 1 MB chunk a draw binds is synced once per
// submission, in the upload buffer: the pages the CPU wrote since the mirror copied them (heap write watch) plus the
// pages a shader wrote since (gdev_blk GPU writes skip the write watch).
extern int heap_mirror_take(uint64_t addr, uint64_t len, uint64_t *offs, uint64_t *lens, int max, uint64_t *bytes);
uint64_t prof_hm_syncs, prof_hm_bytes, prof_hm_ranges, prof_hm_ns;
static int hm_state;  // 0 untried, 1 on, -1 off
static VkBuffer *hm_blk;   // 16 MB blocks made on first use (see mirror_block)
static VmaAllocation *hm_blk_mem;
static uint64_t *hm_blk_use;   // per block: the last submission that bound or wrote it (mirror_release_tick)
static uint8_t *hm_blk_rel, *hm_chunk_full;   // released (counted when re-made) / chunk of a re-made block: all its pages at its next sync
static uint64_t hm_size, *hm_chunk_key, hm_blk_seq[GDEV_BLKS];
// One mirror copy serves a whole submission (its uploads run before it), but the game can rewrite a page between two
// command lists of one submission. The later version then stays out of the mirror (the earlier lists' draws read the
// mirror), the page is volatile for the rest of the submission (draws binding it copy, as before the mirror) and stale
// (snapshotted at its chunk's first sync of a later submission).
static uint64_t *hm_pg_sub, *hm_pg_vol, *hm_chunk_vol;  // per page: submission that last wrote it into the mirror / made it volatile
static uint64_t *hm_pg_gpu;   // (LARECOMP_RACECHECK) per page: submission whose upload copied all of it on the GPU timeline
static uint8_t *hm_pg_stale;
uint64_t prof_hm_vol;
static bool heap_mirror(uint64_t addr, uint64_t size, VkBuffer *buf, VkDeviceSize *off) {
    if (hm_state < 0) { hm_why = 5; return false; }
    if (!hm_state) {
        uint64_t probe;
        hm_state = -1;
        if (!gmem_buf) gmem_info();   // (the heap import: a shader-constant bind can come here before any vertex bind made it)
        if (!getenv("LARECOMP_VK_NO_HEAP_MIRROR") && gmem_buf && heap_mirror_take(HEAP_BASE, 0, NULL, NULL, 0, &probe) >= 0) {
            uint64_t nblk = (gmem_size + (1ull << MBLK_SH) - 1) >> MBLK_SH;
            hm_blk = calloc(nblk, sizeof *hm_blk);
            hm_blk_mem = calloc(nblk, sizeof *hm_blk_mem);
            hm_blk_use = calloc(nblk, sizeof *hm_blk_use);
            hm_blk_rel = calloc(nblk, 1);
            if (hm_blk && hm_blk_mem && hm_blk_use && hm_blk_rel) {
                hm_size = gmem_size;
                uint64_t nch = (hm_size + (1u << 20) - 1) >> 20, npg = nch << 8;
                hm_chunk_key = calloc(nch, 8); hm_chunk_vol = calloc(nch, 8); hm_chunk_full = calloc(nch, 1);
                hm_pg_sub = calloc(npg, 8); hm_pg_vol = calloc(npg, 8); hm_pg_stale = calloc(npg, 1);
                if (getenv("LARECOMP_RACECHECK")) hm_pg_gpu = calloc(npg, 8);
                hm_state = hm_chunk_key && hm_chunk_vol && hm_chunk_full && hm_pg_sub && hm_pg_vol && hm_pg_stale ? 1 : -1;
            }
        }
        LOG("[vk] heap VRAM mirror (%llu MB): %s\n", (unsigned long long)(gmem_size >> 20), hm_state > 0 ? "on" : "off, per-draw copies");
        if (hm_state < 0) return false;
    }
    if (addr + size > HEAP_BASE + hm_size) { hm_why = 2; return false; }
    uint64_t hb = (addr - HEAP_BASE) >> MBLK_SH, hbase = hb << MBLK_SH, he = size ? (addr - HEAP_BASE + size - 1) >> MBLK_SH : hb;
    if (he != hb && (!mirror_straddle_on() || size > MST_MAX)) {  // straddles two mirror blocks: copied
        prof_mst_refused++; prof_mst_refused_bytes += size;
        hm_why = 3;
        return false;
    }
    for (uint64_t k = hb; k <= he; k++)   // (cold under pressure: copied, LARECOMP_MIRROR_HOT)
        if (!hm_blk[k] && !mirror_hot(0, k, (hm_size + (1ull << MBLK_SH) - 1) >> MBLK_SH)) { hm_why = 6; return false; }
    VkBuffer mb = VK_NULL_HANDLE;
    for (uint64_t k = he; k + 1 > hb; k--) {   // (down to hb: mb ends as the first block)
        uint64_t kbase = k << MBLK_SH;
        hm_blk_use[k] = submit_seq + 1;
        if (!hm_blk[k] && hm_blk_rel[k]) { hm_blk_rel[k] = 0; g_mirror_remade[0]++; }
        mb = mirror_block(hm_blk, hm_blk_mem, k, hm_size - kbase < (1ull << MBLK_SH) ? hm_size - kbase : (1ull << MBLK_SH),
                          VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                          VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, 0);
        if (!mb) { hm_why = 4; return false; }
    }
    // once per command list (as the alias mirror): the guest CPU can write between the lists of one submission
    uint64_t cur = submit_seq + 1, key = in_execute ? alias_epoch : cur | (1ull << 62);
    static uint64_t barrier_seq;
    for (uint64_t ch = (addr - HEAP_BASE) >> 20, ce = (addr - HEAP_BASE + (size ? size - 1 : 0)) >> 20; ch <= ce; ch++) {
        if (hm_chunk_key[ch] == key) continue;
        hm_chunk_key[ch] = key;
        uint64_t t0 = prof ? prof_now() : 0, bytes = 0, base = ch << 20, len = hm_size - base < (1u << 20) ? hm_size - base : (1u << 20);
        uint64_t cbase = base >> MBLK_SH << MBLK_SH;   // (the chunk's block: another one than the range's first when it straddles)
        VkBuffer cmb = hm_blk[base >> MBLK_SH];
        // dirty pages of the chunk: 1 = written by the CPU, 2 = in a block a shader wrote (host memory gets that data from
        // the window copy-back on the GPU timeline, so it is copied there). CPU-written pages are snapshotted NOW, as the
        // per-draw copies did: copied from host memory when the GPU runs the upload, they could already hold the game's next
        // frame (the render thread runs behind the game; user 2026-10-19: flicker with the GPU-time copies).
        uint8_t pg[256] = {0};
        if (!ww_chunk_dirty(&ww_heap, ww_take_heap, ch, key, hm_size, pg, &bytes)) {
            static uint64_t offs[128], lens[128];
            int n = heap_mirror_take(HEAP_BASE + base, len, offs, lens, 128, &bytes);
            if (n < 0) { offs[0] = base; lens[0] = len; n = 1; bytes = len; }  // (outside the committed heap / failed: all of it)
            for (int i = 0; i < n; i++)
                for (uint64_t o = offs[i]; o < offs[i] + lens[i]; o += 4096) pg[(o - base) >> 12] = 1;
        }
        if (hm_chunk_full[ch]) {   // (its block was released and made again: the write watch only has what changed since)
            memset(pg, 1, (len + 4095) >> 12);
            bytes = len;
            hm_chunk_full[ch] = 0;
        }
        uint64_t done = vk_completed_seq();
        for (uint64_t bk = base >> GDEV_SH, be = (base + len - 1) >> GDEV_SH; bk <= be; bk++) {
            // (also CPU-written, shader's submission done: host memory holds both (diff copy-back) -> snapshot it)
            uint64_t i = ((bk << GDEV_SH) - base) >> 12;
            if (gdev_blk[bk] && gdev_blk[bk] >= hm_blk_seq[bk] && !(pg[i] == 1 && gdev_blk[bk] <= done)) {
                // The page's CPU bytes are snapshotted now, as above; only the ranges an earlier submission still in flight writes
                // are copied from host memory on the GPU timeline, after the snapshot (3). The whole page that way (2) read the
                // game's dynamic vertex data on it late (LARECOMP_LIVE_PAGES=1, or the ring lost an entry).
                int gw = live_pages() ? -1 : gw_overlap(HEAP_BASE + (bk << GDEV_SH), 1u << GDEV_SH, done, cur - 1);
                pg[i] = gw < 0 ? 2 : gw ? 3 : 1;
                bytes += 1u << GDEV_SH;
            }
            hm_blk_seq[bk] = cur;
        }
        VkBufferCopy bc[512], sc[128];
        int nb = 0, ns = 0, n3 = 0;
        uint64_t snap = 0, np = (len + 4095) >> 12;
        for (uint64_t i = 0; i < np; i++) {
            uint64_t q = (base >> 12) + i;
            if (!pg[i] && hm_pg_stale[q] && hm_pg_sub[q] != cur) pg[i] = 1;  // left out last submission: catch up
            if (pg[i] & 1 && hm_pg_sub[q] == cur) { pg[i] = 0; hm_pg_stale[q] = 1; hm_pg_vol[q] = cur; hm_chunk_vol[ch] = cur; prof_hm_vol++; continue; }
            if (pg[i]) { hm_pg_sub[q] = cur; hm_pg_stale[q] = 0; }
            if (pg[i] == 2 && hm_pg_gpu) hm_pg_gpu[q] = cur;
        }
        for (uint64_t i = 0; i < np; i++) if (pg[i] & 1) snap += 4096;
        VkSpan sp = {0};
        if (snap) { int k0 = vk_up_kind; vk_up_kind = 2; sp = vk_transient(snap, 256); vk_up_kind = k0; }
        uint64_t so = 0;
        for (uint64_t i = 0; i < np; i++) {
            if (!pg[i]) continue;
            uint64_t o = base + (i << 12), l = o + 4096 > hm_size ? hm_size - o : 4096;
            if (pg[i] == 2) {
                if (nb && bc[nb - 1].srcOffset + bc[nb - 1].size == o) bc[nb - 1].size += l;
                else bc[nb++] = (VkBufferCopy){o, o - cbase, l};
                continue;
            }
            memcpy((uint8_t *)sp.ptr + so, (const void *)(uintptr_t)(HEAP_BASE + o), l);
            if (ns && sc[ns - 1].dstOffset + sc[ns - 1].size == o - cbase) sc[ns - 1].size += l;
            else sc[ns++] = (VkBufferCopy){sp.off + so, o - cbase, l};
            so += l;
            if (pg[i] != 3) continue;
            uint64_t iv[8][2];   // the page's in-flight written bytes (merged: one copy per byte)
            int ni = 0;
            for (uint64_t k = gw_head; k > 0 && k + GW_N > gw_head && gw_e[(k - 1) % GW_N].seq > done; k--) {
                const typeof(gw_e[0]) *p = &gw_e[(k - 1) % GW_N];
                uint64_t a = p->addr - HEAP_BASE > o ? p->addr - HEAP_BASE : o, e = p->addr - HEAP_BASE + p->size < o + l ? p->addr - HEAP_BASE + p->size : o + l;
                if (p->seq >= cur || p->addr < HEAP_BASE || a >= e) continue;
                int j = 0;
                for (; j < ni; j++)
                    if (a <= iv[j][1] && iv[j][0] <= e) { iv[j][0] = a < iv[j][0] ? a : iv[j][0]; iv[j][1] = e > iv[j][1] ? e : iv[j][1]; break; }
                if (j == ni && ni < 8) { iv[ni][0] = a; iv[ni][1] = e; ni++; }
                else if (j == ni) { ni = 1; iv[0][0] = o; iv[0][1] = o + l; break; }   // (too many: the whole page, after the snapshot)
            }
            for (int j = 0; j < ni; j++)
                for (int m = j + 1; m < ni; m++)   // (merges made above can now touch a later interval)
                    if (iv[m][0] <= iv[j][1] && iv[j][0] <= iv[m][1]) {
                        iv[j][0] = iv[m][0] < iv[j][0] ? iv[m][0] : iv[j][0]; iv[j][1] = iv[m][1] > iv[j][1] ? iv[m][1] : iv[j][1];
                        iv[m][0] = iv[--ni][0]; iv[m][1] = iv[ni][1]; m = j;
                    }
            if (nb + ni + (int)(np - i - 1) > 512) { bc[nb++] = (VkBufferCopy){o, o - cbase, l}; n3++; continue; }   // (room: 1 per page left)
            for (int j = 0; j < ni; j++) bc[nb++] = (VkBufferCopy){iv[j][0], iv[j][0] - cbase, iv[j][1] - iv[j][0]};
            n3 += ni;
        }
        if (nb || ns) {
            VkCommandBuffer up = vk_upload_cmd();
            if (barrier_seq != cur) { vk_full_barrier(up); barrier_seq = cur; }  // after the earlier submissions' reads of the mirror
            mirror_write(up);
            if (ns) vkCmdCopyBuffer(up, sp.buf, cmb, (uint32_t)ns, sc);
            if (ns && n3) vk_full_barrier(up);   // (written ranges over their page's snapshot)
            if (nb) vkCmdCopyBuffer(up, gmem_buf, cmb, (uint32_t)nb, bc);
        }
        prof_hm_syncs++;
        prof_hm_bytes += bytes;
        prof_hm_ranges += nb + ns;
        if (prof) prof_hm_ns += prof_now() - t0;
    }
    for (uint64_t ch = (addr - HEAP_BASE) >> 20, ce = (addr - HEAP_BASE + (size ? size - 1 : 0)) >> 20; ch <= ce; ch++) {
        if (hm_chunk_vol[ch] != cur) continue;
        for (uint64_t q = (addr - HEAP_BASE) >> 12, qe = (addr - HEAP_BASE + (size ? size - 1 : 0)) >> 12; q <= qe; q++)
            if (hm_pg_vol[q] == cur) return false;  // rewritten since the mirror got it this submission: copied instead
        break;
    }
    for (uint64_t q = (addr - HEAP_BASE) >> 12, qe = (addr - HEAP_BASE + (size ? size - 1 : 0)) >> 12; hm_pg_gpu && q <= qe; q++)
        if (hm_pg_gpu[q] == cur) { rc_record(3, addr, size); break; }   // (read from host memory at GPU time)
    if (he != hb) {
        VkSpan s = mirror_straddle(hm_blk, addr - HEAP_BASE, size, key);
        *buf = s.buf;
        *off = s.off;
        return true;
    }
    *buf = mb;
    *off = addr - HEAP_BASE - hbase;
    return true;
}
// (device.c "[vram]": the VRAM mirrors' sizes; used[0..3] = heap / alias 1 MB chunks ever bound, heap / alias 16 MB blocks
// allocated (MBLK_SH: was a count of 64 MB groups holding a bound chunk))
uint64_t vk_mirror_bytes(uint64_t *alias, int used[4]) {
    if (alias) *alias = g_mirror_blk_bytes[1];
    used[0] = used[1] = used[2] = used[3] = 0;
    for (uint64_t c = 0; hm_chunk_key && c < ((hm_size + (1u << 20) - 1) >> 20); c++) used[0] += hm_chunk_key[c] != 0;
    for (uint64_t c = 0; alias_chunk_key && c < alias_chunk_n; c++) used[1] += alias_chunk_key[c] != 0;
    for (uint64_t k = 0; hm_blk && k < ((hm_size + (1ull << MBLK_SH) - 1) >> MBLK_SH); k++) used[2] += hm_blk[k] != VK_NULL_HANDLE;
    for (uint64_t k = 0; al_blk && k < al_nblk; k++) used[3] += al_blk[k] != VK_NULL_HANDLE;
    return g_mirror_blk_bytes[0];
}

// LARECOMP_MIRROR_RELEASE (default 1 = idle + pressure since 2026-10-30; 2 = pressure only; 0 = off): the heap and alias mirrors' 16 MB blocks were made on first use and kept to the end, so a
// walk across the world left every block it ever bound in VRAM (and, on Windows, in the commit charge). Each block now records the last submission that bound or wrote it; once that submission
// has completed and the block was not used for LARECOMP_MIRROR_IDLE_S (default 30) it is freed, and while residency reports pressure (vk_residency_pressure: over the VRAM budget's headroom or
// under the commit floor) the blocks idle for 2 s or more go too, least recently used first, until the bytes asked for are freed (one such round per 2 s: the textures' eviction answers the same
// figure). A freed block is made again by its next bind: an alias block copies its whole span as the first time; a heap block's chunks take all their pages (CPU snapshot) at their next sync,
// since the write watch only holds what changed after the release. Nothing caches a block across submissions: the guest_direct memo, the straddle memo, the bound vertex / index buffers and the
// descriptor sets (set_tab) only hit within the submission whose binds marked the block. Render thread (device.c, after vk_residency_tick), between executes.
void vk_mirror_release_tick(void) {
    static int mode = -1;
    static uint64_t idle_ns, last_ns, *seen[2], *seen_ns[2], nseen[2], n_rel[2], b_rel[2], n_log;
    static VkBuffer *seen_for[2];
    static uint64_t press_ns;   // (the last pressure round: residency's figure is only re-measured outside its cool-down, so at most one round per 2 s)
    if (mode < 0) {
        const char *e = getenv("LARECOMP_MIRROR_RELEASE");
        mode = e ? atoi(e) : 1;
        e = getenv("LARECOMP_MIRROR_IDLE_S");
        idle_ns = (uint64_t)(e ? atoll(e) : 30) * 1000000000ull;
        if (mode) LOG("[vram] mirror block release: %s (idle %llu s)\n", mode == 2 ? "under pressure only" : "idle + under pressure",
                      (unsigned long long)(idle_ns / 1000000000ull));
    }
    if (mirror_hot_on()) {   // (LARECOMP_MIRROR_HOT: each frame, so a bind sees pressure from residency's last measurement)
        uint64_t t = SDL_GetTicksNS(), pn = vk_residency_need_ns();
        bool s = pn && t - pn < 10000000000ull;
        if (s != mirror_short) {
            mirror_short = s;
            LOG("[vram] mirror blocks %s; so far %llu + %llu binds copied for a cold block, %llu + %llu blocks made once hot (heap + alias)\n",
                s ? "made only once hot (pressure)" : "made on first use again (no pressure for 10 s)",
                (unsigned long long)prof_mh_refused[0], (unsigned long long)prof_mh_refused[1],
                (unsigned long long)prof_mh_made[0], (unsigned long long)prof_mh_made[1]);
        }
    }
    if (!mode) return;
    uint64_t now = SDL_GetTicksNS();
    if (now - last_ns < 250000000ull) return;
    last_ns = now;
    VkBuffer *bl[2] = {hm_blk, al_blk};
    VmaAllocation *bm[2] = {hm_blk_mem, al_blk_mem};
    uint64_t *use[2] = {hm_blk_use, al_blk_use}, nb[2] = {hm_blk ? (hm_size + (1ull << MBLK_SH) - 1) >> MBLK_SH : 0, al_blk ? al_nblk : 0};
    uint64_t span[2] = {hm_size, g_alias_import_size};
    for (int w = 0; w < 2; w++) {
        if (seen_for[w] != bl[w] || nseen[w] != nb[w]) {   // (first tick with this mirror, or the alias mirror made again after vk_exec_reset)
            seen_for[w] = bl[w];
            free(seen[w]); free(seen_ns[w]);
            seen[w] = calloc(nb[w] ? nb[w] : 1, 8); seen_ns[w] = calloc(nb[w] ? nb[w] : 1, 8);
            nseen[w] = seen[w] && seen_ns[w] ? nb[w] : 0;
            for (uint64_t k = 0; k < nseen[w]; k++) seen_ns[w][k] = now;
        }
        for (uint64_t k = 0; k < nseen[w]; k++)
            if (use[w][k] != seen[w][k]) { seen[w][k] = use[w][k]; seen_ns[w][k] = now; }
    }
    uint64_t done = vk_completed_seq(), want = now - press_ns >= 2000000000ull ? vk_residency_pressure(NULL) : 0, freed = 0;
    if (want) press_ns = now;
    int nfree[2] = {0, 0}, npress = 0;
    for (int pass = 0; pass < 2; pass++) {   // 0: idle past LARECOMP_MIRROR_IDLE_S, 1: pressure, oldest first
        if (pass == 0 && mode == 2) continue;
        if (pass == 1 && freed >= want) break;
        for (;;) {
            int bw = -1;
            uint64_t bk = 0, bt = ~0ull, min_idle = pass ? 2000000000ull : idle_ns;
            for (int w = 0; w < 2; w++)
                for (uint64_t k = 0; k < nseen[w]; k++)
                    if (bl[w][k] && use[w][k] <= done && now - seen_ns[w][k] >= min_idle && seen_ns[w][k] < bt) { bw = w; bk = k; bt = seen_ns[w][k]; }
            if (bw < 0) break;
            uint64_t kbase = bk << MBLK_SH, len = span[bw] - kbase < (1ull << MBLK_SH) ? span[bw] - kbase : (1ull << MBLK_SH);
            vmaDestroyBuffer(g_vk.vma, bl[bw][bk], bm[bw][bk]);
            bl[bw][bk] = VK_NULL_HANDLE;
            bm[bw][bk] = NULL;
            g_mirror_blk_bytes[bw] -= len;
            if (bw == 0) {
                hm_blk_rel[bk] = 1;
                for (uint64_t ch = kbase >> 20; ch < (kbase + len + (1u << 20) - 1) >> 20; ch++) hm_chunk_full[ch] = 1;
            } else
                al_blk_rel[bk] = 1;
            nfree[bw]++;
            n_rel[bw]++;
            b_rel[bw] += len;
            freed += len;
            npress += pass;
            if (pass == 1 && freed >= want) break;
        }
    }
    if ((nfree[0] || nfree[1]) && (n_log++ < 50 || n_log % 100 == 0))
        LOG("[vram] mirror release: heap %d, alias %d blocks (%d for pressure %llu MB), %llu MB; mirrors now heap %llu MB, alias %llu MB; "
            "so far %llu + %llu blocks released, %llu + %llu made again\n", nfree[0], nfree[1], npress, (unsigned long long)(want >> 20),
            (unsigned long long)(freed >> 20), (unsigned long long)(g_mirror_blk_bytes[0] >> 20), (unsigned long long)(g_mirror_blk_bytes[1] >> 20),
            (unsigned long long)n_rel[0], (unsigned long long)n_rel[1], (unsigned long long)g_mirror_remade[0], (unsigned long long)g_mirror_remade[1]);
}

// The mirror, unless a shader of this submission writes the range (host memory / the window is newer then).
static bool heap_mirror_bind(uint64_t addr, uint64_t size, VkBuffer *buf, VkDeviceSize *off) {
    if (addr < HEAP_BASE || !size) return false;
    hm_why = 1;
    for (uint64_t bk = (addr - HEAP_BASE) >> GDEV_SH, be = (addr + size - 1 - HEAP_BASE) >> GDEV_SH; bk <= be && bk < GDEV_BLKS; bk++)
        if (gdev_blk[bk] == submit_seq + 1) return false;
    hm_why = 0;
    return heap_mirror(addr, size, buf, off);
}

static bool guest_direct_(uint64_t addr, uint64_t size, VkBuffer *buf, VkDeviceSize *off);
// A repeat call returns the same unless, since the first: a shader of this submission marked pages written (gdev_map: gdev_blk / gw_push), the device window got a region
// (gdev_lv_n / gdev_lv_seq) or an execute-time guest write reset mirror chunks (mirror_guest_written): g_gd_gen and the key cover those. The mirrors sync a chunk once per
// list at the first call, so later calls only re-run the page checks. Not remembered: an in-place bind chosen because a submission in flight writes the range (it turns
// into a copy as the GPU completes). kg5on (Kakariko): guest_direct 0.96 ms a frame.
static bool g_gd_nocache;
uint64_t prof_gd_hit, prof_gd_miss;
static struct { uint64_t addr, size, epoch, seq, lvseq, gen; int lvn; bool valid, ok; VkBuffer buf; VkDeviceSize off; } gd_memo[2048];
static inline uint32_t gd_slot(uint64_t addr, uint64_t size) {
    uint64_t k = (addr * 0x9E3779B97F4A7C15ull) ^ (size * 0xC2B2AE3D27D4EB4Full);
    return (uint32_t)(k ^ k >> 32) & 2047;
}
// LARECOMP_RT_LOOKUP: draw_ prefetches the memo slots of its index and vertex ranges once its pipeline key is known; the
// probes come after both stage_sets (gc63p_kak: guest_direct's slot load was 47 % of its 1.8 % of the render thread)
static void gd_prefetch_draw(const CmdRec *r, bool indexed, uint32_t used_streams) {
    if (indexed && (uint32_t)r->x[1] != 0) {
        uint64_t isz = (uint32_t)r->x[1] == 1 ? 2 : 4;
        const char *e = (const char *)&gd_memo[gd_slot(r->x[3], (uint64_t)(uint32_t)r->x[2] * isz)];
        __builtin_prefetch(e); __builtin_prefetch(e + sizeof gd_memo[0] - 1);
    }
    for (int s = 0; s < 16; s++) {
        if (!(used_streams & (1u << s)) || !st.vbuf[s].addr || !st.vbuf[s].size) continue;
        const char *e = (const char *)&gd_memo[gd_slot(st.vbuf[s].addr, st.vbuf[s].size)];
        __builtin_prefetch(e); __builtin_prefetch(e + sizeof gd_memo[0] - 1);
    }
}
static bool guest_direct(uint64_t addr, uint64_t size, VkBuffer *buf, VkDeviceSize *off) {
    static int mode = -1;
    if (mode < 0) { const char *e = getenv("LARECOMP_GDIRECT_MEMO"); mode = e ? atoi(e) : 1; if (mode != 1) LOG("[vk] guest_direct memo %d\n", mode); }
    if (!mode) return guest_direct_(addr, size, buf, off);
    uint64_t epoch = in_execute ? alias_epoch : 0;
    typeof(gd_memo[0]) *e = &gd_memo[gd_slot(addr, size)];
    bool hit = e->valid && e->addr == addr && e->size == size && e->epoch == epoch && e->seq == submit_seq && e->lvseq == gdev_lv_seq &&
               e->lvn == gdev_lv_n && e->gen == g_gd_gen;
    if (hit && mode == 1) {
        prof_gd_hit++;
        if (e->ok) { *buf = e->buf; *off = e->off; if (e->buf == gmem_buf) rc_record(1, addr, size); }
        return e->ok;
    }
    g_gd_nocache = false;
    uint64_t gen0 = g_gd_gen;
    bool ok = guest_direct_(addr, size, buf, off);
    prof_gd_miss++;
    if (hit) {   // (check mode)
        static uint64_t n, bad;
        n++;
        if (ok != e->ok || (ok && (*buf != e->buf || *off != e->off))) {
            if (bad++ < 20) LOG("[vk] guest_direct memo would have been wrong: %llx +%llx (%llu of %llu)\n", (unsigned long long)addr,
                                (unsigned long long)size, (unsigned long long)bad, (unsigned long long)n);
        }
        if (n % 500000 == 0) LOG("[vk] guest_direct memo check: %llu hits, %llu wrong\n", (unsigned long long)n, (unsigned long long)bad);
    }
    if (g_gd_nocache || g_gd_gen != gen0) { e->valid = false; return ok; }
    *e = (typeof(gd_memo[0])){addr, size, epoch, submit_seq, gdev_lv_seq, g_gd_gen, gdev_lv_n, true, ok, ok ? *buf : VK_NULL_HANDLE, ok ? *off : 0};
    return ok;
}
static bool guest_direct_(uint64_t addr, uint64_t size, VkBuffer *buf, VkDeviceSize *off) {
    static int copy = -1;
    if (copy < 0) copy = getenv("LARECOMP_VK_COPY_VERTICES") != NULL;
    if (copy) return false;
    if (alias_direct(addr, size, buf, off)) return true;
    // Heap vertex / index data copied (dirty pages through the heap mirror LARECOMP_VK_COPY_HEAP_VERTICES=0: bound in
    // place). Discrete GPU, 2x) mean fps 74 -> 108, CPU wait on the GPU 765 -> 8 ms per 2 s - in place, every vertex /
    // instance fetch crossed PCIe. (Not the 2026-10-19 decal hang.) iGPU too since 2026-10-20: in place, the game rewrote
    // small dynamic vertex buffers (0x2c0-0x560 at 0x1010f1xxxx) while the GPU could still read them (SyncWait lag):
    // LARECOMP_RACECHECK Deck R1 580-6234 ranges changed in flight per 600 SyncWaits, R4 copied 14-26 (R3 SYNC_LAG=0 floor
    // 0-98) Deck fps with copies within noise (D 62.2 vs C 61.3 / E 64.1).
    static int copy_heap = -1;
    if (copy_heap < 0) {
        const char *e = getenv("LARECOMP_VK_COPY_HEAP_VERTICES");
        copy_heap = e ? strcmp(e, "0") != 0 : 1;
    }
    if (addr < HEAP_BASE) return false;
    if (addr + size > HEAP_BASE + gmem_size && kernel_heap_size() != gmem_tried) { gmem_tried = kernel_heap_size(); gmem_info(); }
    if (!gmem_buf || addr + size > HEAP_BASE + gmem_size) {
        static int nlog;
        if (prof && nlog++ < 12)
            LOG("[vk] direct bind refused: range %llx +%llx ends past the heap import (%llx +%llx)\n", (unsigned long long)addr,
                (unsigned long long)size, (unsigned long long)HEAP_BASE, (unsigned long long)gmem_size);
        return false;
    }
    for (int i = 0; gdev_lv_seq == submit_seq + 1 && i < gdev_lv_n; i++) {  // written by a shader this submission: in the device window
        const typeof(gdev_lv[0]) *r = &gdev_lv[i];
        if (addr >= r->addr + r->size || r->addr >= addr + size) continue;
        if (addr < r->addr || addr + size > r->addr + r->size) {  // straddles it: (stale) copy
            static int nlog;
            prof_vx_straddle++;
            if (nlog++ < 20 || g_debug_frame)
                LOG("[vk] vertex/index range %llx +%llx straddles live window region %llx +%llx: copied from host (stale)\n",
                    (unsigned long long)addr, (unsigned long long)size, (unsigned long long)r->addr, (unsigned long long)r->size);
            return false;
        }
        if (g_debug_frame) LOG("[vk] vertex/index range %llx +%llx read from the window\n", (unsigned long long)addr, (unsigned long long)size);
        *buf = gdev_buf;
        *off = r->off + (addr - r->addr);
        return true;
    }
    if (copy_heap && heap_mirror_bind(addr, size, buf, off)) return true;
    if (copy_heap) {  // copied unless a submission still in flight writes it (the copy would read host memory too early)
        uint64_t done = 0;
        bool live = false;
        for (uint64_t bk = (addr - HEAP_BASE) >> GDEV_SH, be = (addr + size - 1 - HEAP_BASE) >> GDEV_SH; bk <= be && bk < GDEV_BLKS && !live; bk++)
            if (gdev_blk[bk] && gdev_blk[bk] > (done ? done : (done = vk_completed_seq()))) live = true;
        if (live && !live_pages() && !gw_overlap(addr, size, done, ~0ull)) live = false;   // CPU bytes on a shader-written page
        if (!live) return false;
        prof_vx_inflight++;
        g_gd_nocache = true;   // (in place while a writer is in flight: a copy once it completes)
    }
    *buf = gmem_buf;
    *off = addr - HEAP_BASE;
    rc_record(1, addr, size);
    return true;
}

static int g_cs_n;  // (compute program table below)

// Vertex / index buffer bindings are command buffer state (they survive pipeline binds and passes): skip a bind equal
// to the current one. Keyed by (command buffer, submission): the ring reuses command buffer handles.
static VkCommandBuffer bind_cb;
static uint64_t bind_seq;
static VkBuffer bind_vb[17], bind_ib;
static VkDeviceSize bind_vo[17], bind_io;
static VkIndexType bind_it;
static void bind_check(VkCommandBuffer cb) {
    uint64_t q = vk_pending_seq();
    if (bind_cb != cb || bind_seq != q) { bind_cb = cb; bind_seq = q; memset(bind_vb, 0, sizeof bind_vb); bind_ib = VK_NULL_HANDLE; }
}
static void bind_vertex(VkCommandBuffer cb, uint32_t s, VkBuffer b, VkDeviceSize o) {
    bind_check(cb);
    if (bind_vb[s] == b && bind_vo[s] == o) { prof_binds_skipped++; return; }
    bind_vb[s] = b; bind_vo[s] = o;
    prof_binds_done++;
    vkCmdBindVertexBuffers(cb, s, 1, &b, &o);
}
static void bind_index(VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, VkIndexType t) {
    bind_check(cb);
    if (bind_ib == b && bind_io == o && bind_it == t) { prof_binds_skipped++; return; }
    bind_ib = b; bind_io = o; bind_it = t;
    prof_binds_done++;
    vkCmdBindIndexBuffer(cb, b, o, t);
}

// In-place reboot (device.c backend_reset; GPU idle): drop what is keyed by guest addresses - programs by code
// address (the old Prog / Pipe objects are leaked, so pipeline keys holding their pointers can't collide), compute
// programs, shared copies - and the heap import: its pages are replaced (re-imported on first use).
void vk_exec_reset(void) {
    oq_device_reset();
    dts_device_reset();
    memset(prog_tab, 0, sizeof prog_tab);
    memset(pipe_tab, 0, sizeof pipe_tab);
    g_memo_gen++;   // (draw memos: get_program, the pipeline key)
    g_cs_n = 0;
    gcopy_gen++;
    c1_persist_reset();
    if (gmem_buf) {
        vkDestroyBuffer(g_vk.dev, gmem_buf, NULL);
        vkFreeMemory(g_vk.dev, gmem_mem, NULL);
        gmem_buf = VK_NULL_HANDLE;
        gmem_mem = VK_NULL_HANDLE;
    }
    for (int k = 1; k < GMEM_WIN_MAX; k++)
        if (gmem_wbuf[k]) {
            vkDestroyBuffer(g_vk.dev, gmem_wbuf[k], NULL);
            vkFreeMemory(g_vk.dev, gmem_wmem[k], NULL);
            gmem_wbuf[k] = VK_NULL_HANDLE, gmem_wmem[k] = VK_NULL_HANDLE, gmem_wsize[k] = 0;
        }
    gmem_size = gmem_tried = gmem_asked = 0;
    extern void alias_soft_pause(void);   // hle_os_mem.cpp (TOTK_ALIAS_SOFT_WW): the window writable again before a re-import
    alias_soft_pause();
    if (alias_state == 2) {   // (an import still running on its thread: wait for it, then free what it made)
        pthread_join(alias_imp_thr, NULL);
        if (atomic_load(&alias_imp_done) > 0) { vkDestroyBuffer(g_vk.dev, alias_imp_buf, NULL); vkFreeMemory(g_vk.dev, alias_imp_mem, NULL); }
    }
    alias_imp_ns = 0;
    if (alias_buf) {
        vkDestroyBuffer(g_vk.dev, alias_buf, NULL);
        vkFreeMemory(g_vk.dev, alias_mem, NULL);
        alias_buf = VK_NULL_HANDLE;
        alias_mem = VK_NULL_HANDLE;
    }
    if (alias_dev) {
        for (uint64_t i = 0; i < al_nblk; i++) if (al_blk[i]) vmaDestroyBuffer(g_vk.vma, al_blk[i], al_blk_mem[i]);
        free(al_blk);
        free(al_blk_mem);
        free(al_blk_use);
        free(al_blk_rel);
        al_blk = NULL;
        al_blk_mem = NULL;
        al_blk_use = NULL;
        al_blk_rel = NULL;
        al_nblk = 0;
        g_mirror_blk_bytes[1] = 0;
        alias_dev = VK_NULL_HANDLE;
    }
    alias_sync_seq = 0;
    alias_sync_epoch = 0;
    if (alias_chunk_key) memset(alias_chunk_key, 0, alias_chunk_n * sizeof *alias_chunk_key);
    alias_state = 0;
    gdev_n = 0;  // (GPU idle: the window's contents went back to the old heap)
    gdev_gen++;
    gdev_top = 0;
    gdev_half = 0;
    gdev_half_seq[0] = gdev_half_seq[1] = 0;
    gdev_lv_n = 0;
    gdev_lv_seq = 0;
    g_gd_gen++;
    st_defaults();
}

static void texture_info_(int stage, ShaderInfo *si, int i, VkDescriptorImageInfo *out, float sc[3], uint32_t wh[2]);
// Feedback / pause-capture substitutions bypass it.
uint64_t prof_tic_hit, prof_tic_miss, prof_tic_wmiss;   // (VK_PROF "[prof] G5 caches" line; wmiss: dropped by a writer change)
static void texture_info(int stage, ShaderInfo *si, int i, VkDescriptorImageInfo *out, float sc[3], uint32_t wh[2]) {
    static int mode = -1;
    if (mode < 0) { const char *e = getenv("LARECOMP_TEXINFO_CACHE"); mode = e ? atoi(e) : 1; if (mode != 1) LOG("[vk] texture_info cache %d\n", mode); }
    ShaderTexture *st_ = &si->tex[i];
    int slot = ((int)st_->handle_offset - 0x20) / 8;
    uint64_t h = (slot >= 0 && slot < 32) ? st.tex[stage][slot] : 0;
    if (!mode || nfeedback || pk_subst_tex) { texture_info_(stage, si, i, out, sc, wh); return; }
    extern uint32_t g_pool_gen;
    extern uint64_t vk_frame_counter(void);
    // (keyed by vk_view_gen only, not memo_gen: vk_alias_gen moves on every alias-writer change anywhere, ~450 a frame in
    // Kakariko, which emptied the cache (g5off / g5on: no saving). An entry instead keeps its texture's own writer and is
    // re-checked against it when vk_alias_gen moved. No list epoch: texture_info_'s view memo and upload check are per frame.)
    extern uint32_t vk_view_gen, vk_alias_gen;
    extern NvnTexture *renderer_texture_alias_writer(NvnTexture *t);
    static struct { uint64_t h, frame; void *tp, *sp; NvnTexture *tex, *w; uint32_t gen, agen, pool_gen; uint8_t type, valid; VkDescriptorImageInfo ii; float sc[3]; uint32_t wh[2]; } c[4096];
    uint64_t frame = vk_frame_counter();
    uint32_t gen = vk_view_gen, pg = __atomic_load_n(&g_pool_gen, __ATOMIC_RELAXED), tic = (uint32_t)(h & 0xFFFFF);
    uint64_t k = (h * 0x9E3779B97F4A7C15ull) ^ ((uintptr_t)st.tpool >> 4) ^ ((uintptr_t)st.spool << 7) ^ st_->type;
    typeof(c[0]) *e = &c[(k ^ k >> 29) & 4095];
    bool hit = e->valid && e->h == h && e->type == st_->type && e->tp == (void *)st.tpool && e->sp == (void *)st.spool &&
               e->frame == frame && e->gen == gen && e->pool_gen == pg;
    if (hit && e->agen != vk_alias_gen) {   // an alias writer changed somewhere: still this texture's?
        NvnTexture *tex = h && st.tpool && tic < (uint32_t)st.tpool->count ? ((NvnTexEntry *)st.tpool->entries)[tic].tex : NULL;
        if (tex != e->tex || (tex && renderer_texture_alias_writer(tex) != e->w)) hit = false, prof_tic_wmiss++;
        else e->agen = vk_alias_gen;
    }
    if (hit && mode == 1) {
        *out = e->ii; sc[0] = e->sc[0]; sc[1] = e->sc[1]; sc[2] = e->sc[2]; wh[0] = e->wh[0]; wh[1] = e->wh[1];
        prof_tic_hit++;
        return;
    }
    texture_info_(stage, si, i, out, sc, wh);
    if (hit) {   // (check mode)
        static uint64_t n, bad;
        n++;
        if (memcmp(&e->ii, out, sizeof *out) || e->sc[0] != sc[0] || e->sc[1] != sc[1] || e->sc[2] != sc[2] || e->wh[0] != wh[0] || e->wh[1] != wh[1]) {
            if (bad++ < 20) LOG("[vk] texture_info cache would have been wrong: handle %llx type %u (%llu of %llu)\n",
                                (unsigned long long)h, (unsigned)st_->type, (unsigned long long)bad, (unsigned long long)n);
        }
        if (n % 500000 == 0) LOG("[vk] texture_info cache check: %llu hits, %llu wrong\n", (unsigned long long)n, (unsigned long long)bad);
    }
    prof_tic_miss++;
    // (a resolve can itself destroy a view or change the pool's generation: stored only when the key still holds)
    if (vk_view_gen != gen || __atomic_load_n(&g_pool_gen, __ATOMIC_RELAXED) != pg) { e->valid = 0; return; }
    NvnTexture *tex = h && st.tpool && tic < (uint32_t)st.tpool->count ? ((NvnTexEntry *)st.tpool->entries)[tic].tex : NULL;
    e->h = h; e->type = (uint8_t)st_->type; e->tp = (void *)st.tpool; e->sp = (void *)st.spool; e->frame = frame;
    e->tex = tex; e->w = tex ? renderer_texture_alias_writer(tex) : NULL; e->agen = vk_alias_gen;
    e->gen = gen; e->pool_gen = pg; e->ii = *out; e->sc[0] = sc[0]; e->sc[1] = sc[1]; e->sc[2] = sc[2]; e->wh[0] = wh[0]; e->wh[1] = wh[1]; e->valid = 1;
}
static void texture_info_(int stage, ShaderInfo *si, int i, VkDescriptorImageInfo *out, float sc[3], uint32_t wh[2]) {
    ShaderTexture *st_ = &si->tex[i];
    int slot = ((int)st_->handle_offset - 0x20) / 8;  // 64-bit handles from offset 0x20
    uint64_t h = (slot >= 0 && slot < 32) ? st.tex[stage][slot] : 0;
    sc[2] = 0;   // (1: a D16 image compare-sampled here, below: translate.c rounds the reference to the D16 grid)
    VkImageView view = VK_NULL_HANDLE;
    VkSampler smp = VK_NULL_HANDLE;
    // (g_vk.fbl_layout: the descriptor says the layout the viewed guest image is in, FEEDBACK_LOOP_OPTIMAL while a pass holds it
    // there for a live read, whichever path picked the view: an alias read of it too, VUID-vkCmdDrawIndexed-imageLayout-00344)
    TexBackend *vb = NULL;
    uint32_t tic = h & 0xFFFFF, tsc = (uint32_t)(h >> 20) & 0xFFF;
    sc[0] = sc[1] = 1;
    wh[0] = wh[1] = 0;
    if (h && st.tpool && tic < (uint32_t)st.tpool->count) {
        NvnTexEntry *e = &((NvnTexEntry *)st.tpool->entries)[tic];
        NvnTexture *tex = e->tex;  // read once (finalize clears entries; nvn.h backend_exec_lock)
        if (tex) {
            // Per-frame memo: ~2400 lookups per overworld frame recomputed the view (format / range math, cache
            // probe). The first lookup of a frame still runs vk_view (upload check); a view that may have been
            // destroyed (eviction, image re-created) bumps vk_view_gen. LARECOMP_VK_NO_TEX_MEMO=1 disables.
            extern uint32_t vk_view_gen;
            extern uint64_t vk_frame_counter(void);
            static int nomemo = -1;
            if (nomemo < 0) nomemo = getenv("LARECOMP_VK_NO_TEX_MEMO") != NULL;
            // (keyed by the alias writer too: the view shows the writer's image and the scale is the writer's)
            extern NvnTexture *renderer_texture_alias_writer(NvnTexture *t);
            NvnTexture *w = renderer_texture_alias_writer(tex);
            static struct { NvnTexture *tex, *w; uint64_t vh, frame; uint32_t gen; uint8_t type, has_view; VkImageView view; } memo[1024];
            uint64_t vh = e->has_view ? hash_mem(0, &e->view, sizeof e->view) : 0, frame = vk_frame_counter();
            uint32_t ms = (uint32_t)((((uintptr_t)tex >> 4) ^ vh ^ (uint64_t)st_->type * 0x9E3779B97F4A7C15ull) & 1023);
            if (!nomemo && memo[ms].tex == tex && memo[ms].w == w && memo[ms].vh == vh && memo[ms].frame == frame && memo[ms].gen == vk_view_gen &&
                memo[ms].type == st_->type && memo[ms].has_view == e->has_view) {
                view = memo[ms].view;
            } else {
                view = vk_view(tex, &e->view, e->has_view, false, st_->type);
                memo[ms].tex = tex; memo[ms].w = w; memo[ms].vh = vh; memo[ms].frame = frame; memo[ms].gen = vk_view_gen;
                memo[ms].type = (uint8_t)st_->type; memo[ms].has_view = e->has_view; memo[ms].view = view;
            }
            renderer_texture_scale(w ? w : tex, &sc[0], &sc[1]);   // (= renderer_texture_sample_scale, one writer lookup)
            // A level view (base_level L > 0: the bloom chain binds its levels so) of a scaled texture: the slot's scale is
            // level L's own (host level extent / guest level extent), so the shaders' TXQ (guest level 0 = tsize(0) / scale),
            // rsc and RSC2L are exact for it; the nominal scale was one off at some levels (1.333, L 5: 45 / 1.333 = 34, guest
            // 33) Level 0 too (as attachment_scale since 1d0fa529: the draws into it map guest -> host at the allocation's own
            // ratio, 2133 / 1600, not 1.3333). LARECOMP_SCALE0_NOMINAL=1: level 0 nominal (old).
            static int nominal0 = -1;
            if (nominal0 < 0) { const char *ev = getenv("LARECOMP_SCALE0_NOMINAL"); nominal0 = ev && *ev == '1'; }
            const bool lview = e->has_view && (e->view.setmask & 1) && e->view.base_level;
            if ((sc[0] != 1 || sc[1] != 1) && (lview || !nominal0)) {
                NvnTexture *gt = w ? w : tex;
                TexBackend *hb = gt->backend;
                uint32_t L = lview ? e->view.base_level : 0;
                if (hb && L < hb->levels && L < gt->levels) {
                    sc[0] = (float)level_extent(hb->width, L) / (float)level_extent(gt->width, L);
                    sc[1] = (float)level_extent(hb->height, L) / (float)level_extent(gt->height, L);
                }
            }
            wh[0] = tex->width; wh[1] = tex->height;
            vb = (w ? w : tex)->backend;   // (vk_view shows the alias writer's image)
            for (int k = 0; k < nfeedback; k++)
                if (tex == feedback_tex[k] && feedback_view[k] && (st_->type == TEXTYPE_2D || st_->type == TEXTYPE_2D_ARRAY)) {
                    view = feedback_view[k];
                    vb = feedback_fbl[k];   // (a live read's image; NULL: a snapshot copy)
                    if (feedback_snapb[k] && st_->type == TEXTYPE_2D) {   // (alias snapshot: the texture's own format / swizzle / levels)
                        extern VkImageView vk_view_on(TexBackend *b, NvnTexture *t, const NvnTextureView *v, bool has_view, int want_textype);
                        VkImageView av = vk_view_on(feedback_snapb[k], tex, &e->view, e->has_view, st_->type);
                        if (av) view = av;
                    }
                    if (st_->type == TEXTYPE_2D_ARRAY) view = vk_dummy_view(TEXTYPE_2D_ARRAY), vb = NULL;  // (2D snapshot as array: rare)
                }
            if (pk_subst_tex && tex == pk_subst_tex && st_->type == TEXTYPE_2D) view = pk_subst_view, vb = NULL;   // (pause capture)
            if (st_->shadow && view) {  // depth compare on a colour image: its depth twin once filled
                bool subst = pk_subst_tex && tex == pk_subst_tex;
                for (int k = 0; k < nfeedback; k++) subst |= tex == feedback_tex[k];
                TexBackend *sb = (w ? w : tex)->backend;
                VkImageView tv = sb && !subst ? vk_depth_twin_view(sb, &e->view, e->has_view, st_->type) : VK_NULL_HANDLE;
                if (tv) view = tv, vb = NULL;
                if (sb && (sb->format == VK_FORMAT_D16_UNORM || (tv && sb->format == VK_FORMAT_R16_UNORM))) sc[2] = 1;   // (a D16 image / twin)
            }
        }
    }
    if (h && st.spool && tsc < (uint32_t)st.spool->count) smp = vk_sampler(&((NvnSamplerDesc *)st.spool->entries)[tsc]);
    if (!view) view = vk_dummy_view(st_->type), vb = NULL;
    if (!smp) smp = vk_default_sampler(true);
    *out = (VkDescriptorImageInfo){smp, view, vb && vb->fbl_in ? VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT : VK_IMAGE_LAYOUT_GENERAL};
}

// stage_set reuse table: written descriptor sets of the pending submission by content hash (sets live until their
// submission's descriptor pools are reset, sub()).
#define SET_TAB 4096
typedef struct { uint64_t h, seq; VkDescriptorSetLayout layout; VkDescriptorSet set; } SetEnt;
static SetEnt set_tab[SET_TAB];

// Writes the descriptor set of one stage. gx..gz: compute group counts (driver constants).
// pre_ii: texture descriptors already resolved by the caller (draw: fragment textures, needed earlier for the
// viewport decision), or NULL.
// dyn / ndyn: the set's dynamic uniform-buffer offsets in binding order (for vkCmdBindDescriptorSets), appended at
// dyn[*ndyn]; *ndyn is advanced.
#define UBO_DYN(t) ((t) == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || (t) == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC)
// Per-stage memo of stage_set's INPUTS (2026-10-19: 74-78 % of calls build the same set as the stage's previous call,
// ~390 ms of fill + cbuf per 2 s spent finding that out). g_set_gen counts the commands that write the state it reads
// (exec_list). LARECOMP_SET_MEMO=count (default for now): only counts would-skips and violations (would-skip but the
// built set / offsets differ) in the "[prof] stage_set" line.
static uint64_t g_set_gen;
// LARECOMP_SET_BGEN: g_bind_gen[stage] changes only when a bind command changes the bytes of that stage's textures, uniform or storage
// buffers, or the program / texture / sampler pool (exec_list; st_defaults bumps all). A memo whose generation is current skips the ~800
// bytes of content compares (tex, ubuf, sbuf) per stage per draw.
static uint64_t g_bind_gen[6];
// g_tsb_gen[stage]: the same bumps as g_bind_gen except uniform-buffer rebinds (program, pools, textures, storage
// buffers): a memo at the current one holds the stage's tex / sbuf / program / pools (LARECOMP_SET_TSGEN below)
static uint64_t g_tsb_gen[6];
static void bind_gen_all(void) { for (int i = 0; i < 6; i++) g_bind_gen[i]++, g_tsb_gen[i]++; }
static int set_bgen(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("LARECOMP_SET_BGEN"); on = !e || *e != '0'; if (on) LOG("[vk] stage_set memo by binding generation\n"); }
    return on;
}
uint64_t prof_memo_hit[6], prof_memo_bad[6];
// The stage_set memo and the fragment-texture cache hold views and sample scales: valid while no view may have been
// destroyed (vk_view_gen) and no address got a new alias writer (vk_alias_gen; texture_info's view memo keys by the
// writer instead). Both only grow, so their sum changes with either.
static inline uint32_t memo_gen(void) {
    extern uint32_t vk_view_gen, vk_alias_gen;
    return vk_view_gen + vk_alias_gen;
}
typedef struct {
    uint64_t gen, q, epoch, frame, h, bgen;
    uint32_t view_gen, pool_gen, gxyz[3], nd, d[MAX_BIND];
    VkDescriptorSet ds;   // the set the call returned
    Prog *p; VkDescriptorSetLayout layout; uint32_t set; ShaderInfo *si;
    float rs0[4], tint[8]; bool has_tint;
    VkDescriptorImageInfo pre[64]; int npre;
    Range ubuf[18], sbuf[16]; uint64_t tex[32]; void *prog, *tpool, *spool;   // the stage's bound state (contents)
    uint64_t tsgen, pre_ver;   // g_tsb_gen when tex / sbuf / prog / pools were stored or found equal; pre's g_fs_pre_ver (0: none)
    uint64_t wk[MAX_BIND][3]; int nw;   // the writes built last time (binding|type, view|buffer^range, sampler|offset)
} StageMemo;
// LARECOMP_SET_TSGEN: the memo's content compares answered by generations where they are known equal, for the ~270k stage_set
// calls a second whose uniform buffers alone changed (UBOFAST): tex + sbuf (512 bytes) by g_tsb_gen, and the fragment
// textures (pre_ii, 24 bytes each) by the fragment-texture cache's version: pre_ii is a copy of fstc.ii, so a memo stored
// from the same version holds the same bytes (gc63p_kak: the memo's memcmp / memcpy were 1.8 % of the render thread). =2
// compares anyway and logs a generation that said equal where the bytes differ (a bind path that does not bump g_tsb_gen).
static uint64_t g_fs_pre_ver;   // fstc's version of the pre_ii the draw passes to stage_set (0: unknown, compare)
static uint64_t prof_tsgen_bad;
static int set_tsgen(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("LARECOMP_SET_TSGEN"); on = e ? atoi(e) : 1; LOG("[vk] stage_set memo compares by generation %d (LARECOMP_SET_TSGEN)\n", on); }
    return on;
}
static void tsgen_bad(const char *what, int stage) {
    if (prof_tsgen_bad++ < 16) LOG("[vk] SET_TSGEN check: %s generation said equal at stage %d, bytes differ (%llu so far)\n", what, stage, (unsigned long long)prof_tsgen_bad);
}
// The memo's program, pools, textures and storage buffers are the stage's bound ones
static inline bool memo_ts_same(const StageMemo *m, int stage) {
    int g = set_tsgen();
    bool known = g && set_bgen() && m->tsgen == g_tsb_gen[stage];
    if (known && g == 1) return true;
    bool same = m->prog == (void *)st.program && m->tpool == (void *)st.tpool && m->spool == (void *)st.spool &&
                !memcmp(m->tex, st.tex[stage], sizeof m->tex) && !memcmp(m->sbuf, st.sbuf[stage], sizeof m->sbuf);
    if (known && !same) tsgen_bad("binding", stage);
    return same;
}
// The memo's fragment textures are pre_ii's npre (the caller checked m->npre == npre)
static inline bool memo_pre_same(const StageMemo *m, const VkDescriptorImageInfo *pre_ii, int npre, int stage) {
    if (!npre) return true;
    int g = set_tsgen();
    bool known = g && g_fs_pre_ver && m->pre_ver == g_fs_pre_ver;
    if (known && g == 1) return true;
    bool same = !memcmp(m->pre, pre_ii, (size_t)npre * sizeof *pre_ii);
    if (known && !same) tsgen_bad("fragment-texture", stage);
    return same;
}
// why a call that built the same set as last time missed the memo (count mode): one counter per first differing input
enum { MW_GEN, MW_SEQ, MW_VIEW, MW_PROG, MW_TEX, MW_UBUF, MW_SBUF, MW_PRE, MW_OTHER, MW_GMEM, MW_N };
// LARECOMP_SET_MEMO_GMEM: the memo also skips for shaders using global memory. Their driver table holds gdev_map's window
// addresses, which within one submission are a function of (address, size, write): a region first used in this submission
// is never dropped or re-made before it ends (half switches and table sweeps take only older ones), and the written-page
// bookkeeping repeats nothing (gdev_map's la / ls / lseq). The memo hits only in the same submission and list (m->q,
// m->epoch), so the skipped calls would have built the same table. cx5_kakariko: these were ~40 % of the vertex stage's
// calls (same set, memo refused: "other" before MW_GMEM)
static int memo_gmem(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("LARECOMP_SET_MEMO_GMEM"); on = !(e && *e == '0'); }
    return on;
}
uint64_t prof_memo_why[6][MW_N];
uint64_t prof_memo_other[6][8];   // (MW_OTHER split: feedback, pause substitution, rscale row 0, tint, info / set, groups, pre-resolved textures, bind / storage generations)
static StageMemo g_smemo[6];
// LARECOMP_SET_UBOFAST=2: the fast path's set and offsets against the ones the full build chose (a different set with the
// same writes is fine only if set_tab missed: the check compares offsets, and the set when set_tab reused one) (=2) Each
// disagreement is sorted (present, 2026-10-26):
// - another set, same writes: the fast path's memo set and the one built hold the same descriptors (each uniform buffer
//   already had to land in the memo's buffer and range); set_tab is direct-mapped, so a slot another set took since makes
//   the slow path allocate an identical one. Equivalent, not wrong.
// - another set, other writes: a real disagreement (logged with the writes that differ);
// - other offsets: a dynamic offset differs. The fast path's cbuf_info call and the slow path's second one each copy the
//   range; one that is not deduped (an unbound range's zeros, vk_transient_copy) lands at a new offset with the same
//   bytes. Split by the binding's source: an unbound range (zeros), a bound one whose two copies hold the same bytes, or
//   a bound one with other bytes (logged).
static const void *ubf_fptr[17], *ubf_sptr[17];   // (=2: the host copies behind the fast path's and the build's spans)
static uint64_t ubf_frange[17], ubf_srange[17];
static void ubofast_check(VkDescriptorSet fast, uint32_t nd, const uint32_t *d, VkDescriptorSet built, const uint32_t *dyn, uint32_t ndyn, int stage,
                          const uint32_t *fbnd, uint64_t (*fwk)[3], int fnw, uint64_t (*bwk)[3], int bnw) {
    if (!fast) return;
    static uint64_t n, bad, eq_set, eq_bytes, real_set, off_zero, off_same, off_bound;
    static int nlog, nlog_set;  // (separate budgets: the offset lines once crowded out every real disagreement)
    n++;
    bool offs = nd > ndyn || memcmp(d, dyn + ndyn - nd, (size_t)nd * 4), other = fast != built && built;
    if (offs || other) {
        if (bad++ < 16) LOG("[vk] stage_set uniform-buffer fast path would have been wrong (stage %d, %s; %llu of %llu)\n", stage,
                            other ? "another set" : "other offsets", (unsigned long long)bad, (unsigned long long)n);
        if (other) {
            bool same = fnw == bnw && bwk;
            for (int i = 0; same && i < fnw; i++) {
                const bool dynamic = (VkDescriptorType)(fwk[i][0] >> 32) == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
                same = fwk[i][0] == bwk[i][0] && fwk[i][1] == bwk[i][1] && (dynamic || fwk[i][2] == bwk[i][2]);
            }
            // the build's copy of a uniform buffer in another transient block, same range and bytes: the second copy,
            // not another input
            bool bytes = !same && fnw == bnw && bwk;
            for (int i = 0; bytes && i < fnw; i++) {
                if (!memcmp(fwk[i], bwk[i], sizeof fwk[i])) continue;
                const uint32_t b = (uint32_t)bwk[i][0];
                bytes = fwk[i][0] == bwk[i][0] && (VkDescriptorType)(fwk[i][0] >> 32) == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC && b < 17 &&
                        ubf_fptr[b] && ubf_sptr[b] && ubf_frange[b] == ubf_srange[b] && !memcmp(ubf_fptr[b], ubf_sptr[b], ubf_frange[b]);
            }
            if (same) eq_set++;
            else if (bytes) eq_bytes++;
            else {
                real_set++;
                if (nlog_set < 40) {
                    nlog_set++;
                    LOG("[ubofast] st%d another set, other writes (n %d/%d):", stage, fnw, bnw);
                    for (int i = 0; bwk && i < fnw && i < bnw; i++)
                        if (memcmp(fwk[i], bwk[i], sizeof fwk[i]))
                            LOG(" [b%u t%u %llx/%llx -> %llx/%llx]", (unsigned)bwk[i][0], (unsigned)(bwk[i][0] >> 32), (unsigned long long)fwk[i][1],
                                (unsigned long long)fwk[i][2], (unsigned long long)bwk[i][1], (unsigned long long)bwk[i][2]);
                    LOG("\n");
                }
            }
        }
        if (offs && nd <= ndyn)
            for (uint32_t j = 0; j < nd; j++) {
                if (d[j] == dyn[ndyn - nd + j]) continue;
                const uint32_t b = fbnd[j];
                const bool zero = b >= 3 && b < 17 && !st.ubuf[stage][b - 3].addr;
                if (zero) off_zero++;
                else if (b < 17 && ubf_fptr[b] && ubf_sptr[b] && ubf_frange[b] == ubf_srange[b] && !memcmp(ubf_fptr[b], ubf_sptr[b], ubf_frange[b])) off_same++;
                else {
                    off_bound++;
                    if (nlog < 40) {
                        nlog++;
                        LOG("[ubofast] st%d other offset: binding %u (%s) %x -> %x\n", stage, b, b == 0 ? "driver table" : b == 1 ? "shader constants" :
                            b == 2 ? "binding 2" : "bound range", d[j], dyn[ndyn - nd + j]);
                    }
                }
            }
    }
    if (n % 200000 == 0)
        LOG("[vk] stage_set uniform-buffer fast path check: %llu calls, %llu wrong: another set with the same writes %llu, with a uniform "
            "buffer's same bytes in another block %llu, with other writes %llu; other offsets of unbound (zero) ranges %llu, of bound ranges with the same bytes %llu, with other bytes %llu\n",
            (unsigned long long)n, (unsigned long long)bad, (unsigned long long)eq_set, (unsigned long long)eq_bytes, (unsigned long long)real_set, (unsigned long long)off_zero,
            (unsigned long long)off_same, (unsigned long long)off_bound);
}
static VkDescriptorSet stage_set(Prog *p, VkDescriptorSetLayout layout, uint32_t set, int stage, ShaderInfo *si, const float *rscale_row0,
                                 const float tint[8], uint32_t gx, uint32_t gy, uint32_t gz, const VkDescriptorImageInfo *pre_ii,
                                 uint32_t *dyn, uint32_t *ndyn) {
    uint64_t z0 = PROFD ? prof_now() : 0;
    VkDescriptorSet ds = VK_NULL_HANDLE;   // (allocated below, once the writes are known: an identical set may exist)
    uint64_t z1 = z0, zc = 0;
    VkWriteDescriptorSet w[MAX_BIND];
    VkDescriptorBufferInfo bi[MAX_BIND];
    VkDescriptorImageInfo ii[MAX_BIND];
    int n = 0;
    int sg = stage == 1 ? 1 : 0;
    // (rscale: only rows 0..ntex are filled, copied and bound: the shader reads rscale[0] and rscale[1 + slot], slot < ntex.
    // Was the whole 65-row table, zeroed, compared and copied twice per draw, 2026-10-17)
    float rs[65][4];
    rs[0][0] = 1; rs[0][1] = 1; rs[0][2] = 0; rs[0][3] = 0;
    if (rscale_row0) memcpy(rs[0], rscale_row0, 16);
    StageMemo *m = stage >= 0 && stage < 6 ? &g_smemo[stage] : NULL;
    extern uint32_t vk_view_gen;
    extern uint64_t vk_frame_counter(void);
    uint64_t m_epoch = in_execute ? alias_epoch : 0, m_frame = vk_frame_counter(), m_q = vk_pending_seq();
    int npre = pre_ii ? (si->ntex < 64 ? si->ntex : 64) : 0;
    extern uint32_t g_pool_gen;
    uint32_t m_pool_gen = __atomic_load_n(&g_pool_gen, __ATOMIC_RELAXED);
    // (LARECOMP_SET_MEMO_FB, default on, =0 off: a draw with feedback reads (nfeedback) may hit too when all of its textures
    // came in pre-resolved (the fragment stage's pre_ii, compared below by bytes: fstc refills its version for such draws), so
    // the feedback views (live, snapshot) are part of the key; the vertex stage, resolving its own textures, still misses.
    // t3_fight_t8b: ~43-55k fragment calls per 2-s window missed with the same set for "other")
    static int memo_fb = -1;
    if (memo_fb < 0) { const char *e = getenv("LARECOMP_SET_MEMO_FB"); memo_fb = !(e && *e == '0'); }
    const bool fb_ok = !nfeedback || (memo_fb && npre && npre == si->ntex);
    bool memo_hit = m && fb_ok && !pk_subst_tex && (!si->uses_gmem || memo_gmem()) && m->ds && m->pool_gen == m_pool_gen && m->gen == g_set_gen && m->q == m_q && m->epoch == m_epoch &&
                    m->frame == m_frame && m->view_gen == memo_gen() && m->p == p && m->layout == layout && m->set == set && m->si == si &&
                    m->gxyz[0] == gx && m->gxyz[1] == gy && m->gxyz[2] == gz && !memcmp(m->rs0, rs[0], 16) &&
                    m->has_tint == (tint != NULL) && (!tint || !memcmp(m->tint, tint, 32)) && m->npre == npre &&
                    memo_pre_same(m, pre_ii, npre, stage) &&
                    ((set_bgen() && m->bgen == g_bind_gen[stage]) ||
                     (memo_ts_same(m, stage) && !memcmp(m->ubuf, st.ubuf[stage], sizeof m->ubuf)));
    // Skip (default LARECOMP_SET_MEMO=0 off): the only differences such calls ever built were new copies of the same guest ranges
    // (dyn offsets). Not for shaders using global memory (gdev_map's per-call window bookkeeping) or feedback / capture
    // substitutions.
    static int memo_skip = -1;
    if (memo_skip < 0) { const char *e = getenv("LARECOMP_SET_MEMO"); memo_skip = !e || *e == '1'; }   // (default on; =0 off)
    if (memo_skip && memo_hit) {
        memcpy(dyn + *ndyn, m->d, (size_t)m->nd * 4);
        *ndyn += m->nd;
        prof_memo_hit[stage]++;
        if (PROFD) prof_set_fill += prof_now() - z0;
        return m->ds;
    }
    if (rt_lookup() && stage >= 0 && stage < 6)   // (the uniform ranges cbuf_info_ snapshots below: gcopy slot, gw_overlap's page mark)
        for (int i = 0; i < p->nbinds; i++) {
            const VkBindingInfo *b = &p->binds[i];
            if (b->set != set || !UBO_DYN(b->type) || b->binding < 3 || b->binding >= 17) continue;
            Range r = st.ubuf[stage][b->binding - 3];
            if (!r.addr) continue;
            __builtin_prefetch(&gcopy[gcopy_slot(r.addr, ubo_size(si, (int)b->binding, r))]);
            if (r.addr >= HEAP_BASE && (r.addr - HEAP_BASE) >> GDEV_SH < GDEV_BLKS) __builtin_prefetch(&gdev_blk[(r.addr - HEAP_BASE) >> GDEV_SH]);
        }
    // LARECOMP_SET_UBOFAST: the stage's previous call differed only in its uniform-buffer ranges (TotK's per-draw
    // constants: in w2 most vertex-stage calls missed the memo for that alone). Only the uniform buffers (bindings < 17)
    // are resolved again; when each lands in the same buffer with the same range as the set the memo holds, that set is
    // bound with the new dynamic offsets, without resolving textures, hashing the writes or probing set_tab. =2: resolves
    // the whole set anyway and logs calls where the fast path would have bound another set.
    static int ubofast = -1;
    if (ubofast < 0) { const char *e = getenv("LARECOMP_SET_UBOFAST"); ubofast = e ? atoi(e) : 1; LOG("[vk] stage_set uniform-buffer fast path %d\n", ubofast); }
    VkDescriptorSet ubf_ds = VK_NULL_HANDLE; uint32_t ubf_nd = 0, ubf_d[MAX_BIND], ubf_bnd[MAX_BIND];
    uint64_t ubf_wk[MAX_BIND][3]; int ubf_nw = 0;   // (=2: the memo's writes the fast path would have bound, for ubofast_check)
    if (ubofast && memo_skip && !memo_hit && m && m->nw && m->ds && !nfeedback && !pk_subst_tex && !si->uses_gmem && m->pool_gen == m_pool_gen &&
        m->gen == g_set_gen && m->q == m_q && m->epoch == m_epoch && m->frame == m_frame && m->view_gen == memo_gen() && m->p == p &&
        m->layout == layout && m->set == set && m->si == si && m->gxyz[0] == gx && m->gxyz[1] == gy && m->gxyz[2] == gz &&
        !memcmp(m->rs0, rs[0], 16) && m->has_tint == (tint != NULL) && (!tint || !memcmp(m->tint, tint, 32)) && m->npre == npre &&
        memo_pre_same(m, pre_ii, npre, stage) && memo_ts_same(m, stage)) {
        // the memo's dynamic writes in binding order (as the build below sorts them): new offsets for bindings < 17, the
        // old ones for rscale / tint (18 / 19: same inputs)
        uint32_t bnd[MAX_BIND], j0 = 0; bool ok = true;
        if (ubofast == 2) {  // (this call's captures only: nothing left from another call or stage)
            memset(ubf_fptr, 0, sizeof ubf_fptr), memset(ubf_sptr, 0, sizeof ubf_sptr);
            memset(ubf_frange, 0, sizeof ubf_frange), memset(ubf_srange, 0, sizeof ubf_srange);
        }
        for (int i = 0; i < m->nw && ok; i++) {
            uint32_t b = (uint32_t)m->wk[i][0], t = (uint32_t)(m->wk[i][0] >> 32);
            if (!UBO_DYN((VkDescriptorType)t)) continue;
            uint32_t off;
            if (b < 17) {
                VkDescriptorBufferInfo nb = cbuf_info(stage, si, (int)b, gx, gy, gz);
                if (ubofast == 2) ubf_fptr[b] = ubo_last_ptr, ubf_frange[b] = nb.range;
                uint64_t a1 = (uint64_t)(uintptr_t)nb.buffer ^ nb.range << 40;
                if (t == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC) { ok = a1 == m->wk[i][1]; off = (uint32_t)nb.offset; }
                else { ok = a1 == m->wk[i][1] && nb.offset == m->wk[i][2]; off = 0; }
            } else off = 0;
            if (!ok || t != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC) continue;
            uint32_t j = j0++;
            while (j > 0 && bnd[j - 1] > b) { bnd[j] = bnd[j - 1]; ubf_d[j] = ubf_d[j - 1]; j--; }
            bnd[j] = b; ubf_d[j] = b < 17 ? off : 0xFFFFFFFFu;   // (rscale / tint: filled from the memo below)
        }
        if (ok && j0 == m->nd) {
            for (uint32_t j = 0; j < j0; j++) if (ubf_d[j] == 0xFFFFFFFFu) ubf_d[j] = m->d[j];
            ubf_ds = m->ds; ubf_nd = j0;
            if (ubofast == 2) { memcpy(ubf_bnd, bnd, (size_t)j0 * 4); ubf_nw = m->nw; memcpy(ubf_wk, m->wk, (size_t)m->nw * sizeof ubf_wk[0]); }
            if (ubofast == 1) {
                memcpy(dyn + *ndyn, ubf_d, (size_t)j0 * 4); *ndyn += j0;
                memcpy(m->ubuf, st.ubuf[stage], sizeof m->ubuf); memcpy(m->d, ubf_d, (size_t)j0 * 4); m->bgen = g_bind_gen[stage];
                m->tsgen = g_tsb_gen[stage]; m->pre_ver = npre ? g_fs_pre_ver : 0;   // (both found equal above)
                static uint64_t ubf_hits;
                if (++ubf_hits % 20000000 == 0) LOG("[vk] stage_set uniform-buffer fast path: %llu calls\n", (unsigned long long)ubf_hits);
                if (PROFD) prof_set_fill += prof_now() - z0;
                return m->ds;
            }
        }
    }
    for (int i = 0; i < p->nbinds; i++) {
        VkBindingInfo *b = &p->binds[i];
        if (b->set != set) continue;
        w[n] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = b->binding,
                                      .descriptorCount = 1, .descriptorType = b->type};
        if (b->type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER && b->binding >= 32) {
            int ti = (int)b->binding - 32;
            float sc[3] = {1, 1, 0}; uint32_t wh[2];
            if (ti < si->ntex && pre_ii && ti < 64) { ii[n] = pre_ii[ti]; sc[0] = tex_sc[sg][ti][0]; sc[1] = tex_sc[sg][ti][1]; sc[2] = tex_sc[sg][ti][2]; wh[0] = tex_wh[sg][ti][0]; wh[1] = tex_wh[sg][ti][1]; }
            else if (ti < si->ntex) texture_info(stage, si, ti, &ii[n], sc, wh);
            else { ii[n] = (VkDescriptorImageInfo){vk_default_sampler(true), vk_dummy_view(TEXTYPE_2D), VK_IMAGE_LAYOUT_GENERAL}; sc[0] = sc[1] = 1; wh[0] = wh[1] = 0; }
            if (ti < 64) {
                tex_sc[sg][ti][0] = sc[0]; tex_sc[sg][ti][1] = sc[1]; tex_sc[sg][ti][2] = sc[2];
                tex_wh[sg][ti][0] = wh[0]; tex_wh[sg][ti][1] = wh[1];
            }
            w[n].pImageInfo = &ii[n];
        } else if (UBO_DYN(b->type) && b->binding < 17) {
            uint64_t c0 = PROFD ? prof_now() : 0;
            bi[n] = cbuf_info(stage, si, (int)b->binding, gx, gy, gz);
            if (ubf_ds) ubf_sptr[b->binding] = ubo_last_ptr, ubf_srange[b->binding] = bi[n].range;
            if (PROFD) zc += prof_now() - c0;
            w[n].pBufferInfo = &bi[n];
        } else if (b->type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) {
            bi[n] = b->binding == SMARK_BUFFER_INDEX ? smark_info() : b->binding == GDEV_BUFFER_INDEX ? gdev_info()
                  : b->binding >= GMEM_WIN_BUFFER_INDEX && b->binding < GMEM_WIN_BUFFER_INDEX + GMEM_WIN_MAX - 1
                  ? gmem_win_info((int)b->binding - GMEM_WIN_BUFFER_INDEX + 1) : gmem_info();
            w[n].pBufferInfo = &bi[n];
        } else {
            continue;  // rscale / tint: written below, once the textures (and their scales) are known
        }
        n++;
    }
    for (int i = 0; i < p->nbinds; i++) {
        VkBindingInfo *b = &p->binds[i];
        if (b->set != set || !UBO_DYN(b->type) || (b->binding != 18 && b->binding != 19)) continue;
        w[n] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = b->binding,
                                      .descriptorCount = 1, .descriptorType = b->type};
        if (b->binding == 18) {
            int nt = si->ntex < 64 ? si->ntex : 64;
            for (int t = 0; t < nt; t++) { rs[1 + t][0] = tex_sc[sg][t][0]; rs[1 + t][1] = tex_sc[sg][t][1]; rs[1 + t][2] = tex_sc[sg][t][2]; rs[1 + t][3] = 0; }
            uint64_t rsz = (uint64_t)(1 + nt) * 16;
            bi[n] = ubo_span(copy_dedupe(sg ? DEDUPE_RSCALE_F : DEDUPE_RSCALE_V, rs, rsz, 256), rsz);
        } else {
            float zero[8] = {0};
            bi[n] = ubo_span(copy_dedupe(DEDUPE_TINT, tint ? tint : zero, 32, 256), 32);
        }
        w[n].pBufferInfo = &bi[n];
        n++;
    }
    {   // dynamic uniform buffers: the offset goes to the bind (binding order), the descriptor holds offset 0
        uint32_t nd = 0, bnd[MAX_BIND];
        for (int i = 0; i < n; i++) {
            if (w[i].descriptorType != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC) continue;
            uint32_t j = nd++;
            while (j > 0 && bnd[j - 1] > w[i].dstBinding) { bnd[j] = bnd[j - 1]; dyn[*ndyn + j] = dyn[*ndyn + j - 1]; j--; }
            bnd[j] = w[i].dstBinding;
            dyn[*ndyn + j] = (uint32_t)bi[i].offset;
            bi[i].offset = 0;
        }
        *ndyn += nd;
    }
    uint64_t z2 = PROFD ? prof_now() : 0;
    // Identical set already written in this submission (same layout, same buffers / ranges / views / samplers): bind
    // that one; skips vkAllocateDescriptorSets + vkUpdateDescriptorSets. UBO spans are content- or address-deduped per
    // submission (copy_dedupe, guest_copy), so draws sharing a material share a set. LARECOMP_VK_SET_REUSE=0: a new set
    // every time.
    static int reuse = -1;
    if (reuse < 0) { const char *e = getenv("LARECOMP_VK_SET_REUSE"); reuse = !e || strcmp(e, "0"); }
    uint64_t h = 0, q = vk_pending_seq();
    SetEnt *se = NULL;
    uint64_t wk[MAX_BIND][3];   // (the writes' keys: set_tab's hash, the memo, ubofast_check)
    if (reuse) {
        h = (uint64_t)(uintptr_t)layout * 0x9E3779B97F4A7C15ull ^ (uint64_t)n;
        for (int i = 0; i < n; i++) {
            uint64_t a0 = (uint64_t)w[i].dstBinding | (uint64_t)w[i].descriptorType << 32, a1, a2;
            if (w[i].pImageInfo) { a1 = (uint64_t)(uintptr_t)w[i].pImageInfo->imageView; a2 = (uint64_t)(uintptr_t)w[i].pImageInfo->sampler ^ (uint64_t)w[i].pImageInfo->imageLayout << 56; }
            else { a1 = (uint64_t)(uintptr_t)w[i].pBufferInfo->buffer ^ w[i].pBufferInfo->range << 40; a2 = w[i].pBufferInfo->offset; }
            wk[i][0] = a0; wk[i][1] = a1; wk[i][2] = a2;
            h = (h ^ a0) * 0xFF51AFD7ED558CCDull;
            h = (h ^ a1) * 0xC4CEB9FE1A85EC53ull;
            h = (h ^ a2) * 0x9E3779B97F4A7C15ull;
            h ^= h >> 29;
        }
        se = &set_tab[(h ^ h >> 32) & (SET_TAB - 1)];
        if (rt_lookup()) __builtin_prefetch(se);   // (probed after the memo bookkeeping below)
        if (prof && stage >= 0 && stage < 6) {   // (how often a per-stage memo would hit)
            static struct { uint64_t h, q; VkDescriptorSetLayout layout; uint32_t nd, d[MAX_BIND]; } last[6];
            uint32_t nd = 0;
            for (int i = 0; i < n; i++) if (w[i].descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC) nd++;
            const uint32_t *d = dyn + *ndyn - nd;
            prof_set_calls[stage]++;
            if (last[stage].h == h && last[stage].q == q && last[stage].layout == layout) {
                prof_set_same[stage]++;
                if (last[stage].nd == nd && !memcmp(last[stage].d, d, nd * 4)) prof_set_samedyn[stage]++;
            }
            last[stage].h = h; last[stage].q = q; last[stage].layout = layout; last[stage].nd = nd; memcpy(last[stage].d, d, nd * 4);
        }
        if (m) {   // inputs memo (count mode): a would-skip must have built the same set and offsets as last time
            uint32_t nd = 0;
            for (int i = 0; i < n; i++) if (w[i].descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC) nd++;
            const uint32_t *d = dyn + *ndyn - nd;
            bool same = m->h == h && m->nd == nd && !memcmp(m->d, d, nd * 4);
            if (memo_hit) {
                prof_memo_hit[stage]++;
                // (count mode, without VK_PROF too: would-skips and violations by global memory or not, every 2^18)
                static uint64_t vh[2], vb[2];
                vh[si->uses_gmem != 0]++;
                if (!same) vb[si->uses_gmem != 0]++;
                if (((vh[0] + vh[1]) & ((1u << 18) - 1)) == 0)
                    LOG("[memo] count: would-skip %llu (violations %llu), global-memory shaders %llu (violations %llu)\n",
                        (unsigned long long)vh[0], (unsigned long long)vb[0], (unsigned long long)vh[1], (unsigned long long)vb[1]);
                if (!same) {
                    prof_memo_bad[stage]++;
                    static int nlog;   // the first violations: which write differs (a missing input in the key)
                    if (nlog < 60) {
                        nlog++;
                        LOG("[memo] violation st%d%s n %d/%d nd %u/%u:", stage, si->uses_gmem ? " gmem" : "", m->nw, n, m->nd, nd);
                        for (int i = 0; i < n && i < m->nw; i++)
                            if (memcmp(m->wk[i], wk[i], sizeof wk[i]))
                                LOG(" [b%u t%u %llx/%llx -> %llx/%llx]", (unsigned)wk[i][0], (unsigned)(wk[i][0] >> 32), (unsigned long long)m->wk[i][1],
                                    (unsigned long long)m->wk[i][2], (unsigned long long)wk[i][1], (unsigned long long)wk[i][2]);
                        for (uint32_t i = 0; i < nd && i < m->nd; i++)
                            if (m->d[i] != d[i]) LOG(" [dyn%u %x -> %x]", i, m->d[i], d[i]);
                        LOG("\n");
                    }
                }
            } else if (same && m->p == p && m->layout == layout) {
                int why = si->uses_gmem && !memo_gmem() ? MW_GMEM : m->gen != g_set_gen ? MW_GEN : (m->q != m_q || m->epoch != m_epoch || m->frame != m_frame) ? MW_SEQ :
                          (m->view_gen != memo_gen() || m->pool_gen != m_pool_gen) ? MW_VIEW : (m->prog != (void *)st.program || m->tpool != (void *)st.tpool || m->spool != (void *)st.spool) ? MW_PROG :
                          memcmp(m->tex, st.tex[stage], sizeof m->tex) ? MW_TEX : memcmp(m->ubuf, st.ubuf[stage], sizeof m->ubuf) ? MW_UBUF :
                          memcmp(m->sbuf, st.sbuf[stage], sizeof m->sbuf) ? MW_SBUF : (m->npre != npre || (npre && memcmp(m->pre, pre_ii, (size_t)npre * sizeof *pre_ii))) ? MW_PRE : MW_OTHER;
                prof_memo_why[stage][why]++;
                if (why == MW_OTHER)
                    prof_memo_other[stage][nfeedback ? 0 : pk_subst_tex ? 1 : memcmp(m->rs0, rs[0], 16) ? 2 :
                                           m->has_tint != (tint != NULL) || (tint && memcmp(m->tint, tint, 32)) ? 3 : m->si != si || m->set != set ? 4 :
                                           m->gxyz[0] != gx || m->gxyz[1] != gy || m->gxyz[2] != gz ? 5 : !memo_pre_same(m, pre_ii, npre, stage) ? 6 : 7]++;
            }
            m->gen = g_set_gen; m->q = m_q; m->epoch = m_epoch; m->frame = m_frame; m->h = h; m->view_gen = memo_gen(); m->pool_gen = m_pool_gen;
            m->gxyz[0] = gx; m->gxyz[1] = gy; m->gxyz[2] = gz; m->nd = nd; memcpy(m->d, d, nd * 4);
            m->p = p; m->layout = layout; m->set = set; m->si = si; memcpy(m->rs0, rs[0], 16);
            m->has_tint = tint != NULL; if (tint) memcpy(m->tint, tint, 32);
            m->npre = npre; if (npre) memcpy(m->pre, pre_ii, (size_t)npre * sizeof *pre_ii);
            m->prog = st.program; m->tpool = st.tpool; m->spool = st.spool; m->bgen = g_bind_gen[stage];
            m->tsgen = g_tsb_gen[stage]; m->pre_ver = npre ? g_fs_pre_ver : 0;
            memcpy(m->tex, st.tex[stage], sizeof m->tex); memcpy(m->ubuf, st.ubuf[stage], sizeof m->ubuf); memcpy(m->sbuf, st.sbuf[stage], sizeof m->sbuf);
            m->nw = n; memcpy(m->wk, wk, (size_t)n * sizeof wk[0]);
        }
        if (se->seq == q && se->h == h && se->layout == layout) {
            prof_set_reused++;
            if (PROFD) prof_set_fill += z2 - z1 - zc, prof_set_cbuf += zc;
            if (m) m->ds = se->set;
            ubofast_check(ubf_ds, ubf_nd, ubf_d, se->set, dyn, *ndyn, stage, ubf_bnd, ubf_wk, ubf_nw, wk, n);
            return se->set;
        }
    }
    if (PROFD) {  // G6 count: would this allocation have been avoided by a set_tab that
        // outlives the submission (same writes and layout, no view destroyed since: memo_gen), or by set layouts shared
        // between programs with the same bindings (same writes, another layout of equal content, this submission)?
        static struct { uint64_t h, seq; uint32_t gen; VkDescriptorSetLayout layout; } xs[SET_TAB], sl[SET_TAB];
        uint64_t hw = (uint64_t)n, lh = 0;
        for (int i = 0; i < n; i++) {
            uint64_t a1 = w[i].pImageInfo ? (uint64_t)(uintptr_t)w[i].pImageInfo->imageView ^ (uint64_t)(uintptr_t)w[i].pImageInfo->sampler
                                          : (uint64_t)(uintptr_t)w[i].pBufferInfo->buffer ^ w[i].pBufferInfo->range << 40 ^ w[i].pBufferInfo->offset;
            hw = (hw ^ ((uint64_t)w[i].dstBinding | (uint64_t)w[i].descriptorType << 32)) * 0xFF51AFD7ED558CCDull;
            hw = (hw ^ a1) * 0xC4CEB9FE1A85EC53ull; hw ^= hw >> 29;
        }
        for (int i = 0; i < p->nbinds; i++)
            if (p->binds[i].set == set) lh = (lh ^ (p->binds[i].binding | (uint64_t)p->binds[i].type << 32)) * 0x9E3779B97F4A7C15ull;
        uint32_t g = memo_gen();
        typeof(xs[0]) *x = &xs[(h ^ h >> 32) & (SET_TAB - 1)], *s = &sl[((hw ^ lh) ^ (hw ^ lh) >> 32) & (SET_TAB - 1)];
        prof_set_allocs++;
        if (x->h == h && x->layout == layout && x->seq < q && x->gen == g) prof_set_xsub++;
        if (s->h == (hw ^ lh) && s->seq == q && s->layout != layout) prof_set_shlayout++;
        *x = (typeof(xs[0])){h, q, g, layout};
        *s = (typeof(sl[0])){hw ^ lh, q, g, layout};
    }
    uint64_t z2b;
    VkDescriptorSet vds = vkq_set_deferred(layout, w, n);   // (LARECOMP_VKQ_SETS: a virtual handle, written on vk-record)
    if (vds) {
        ds = vds;
        z2b = PROFD ? prof_now() : 0;
    } else {
        ds = vk_alloc_set(layout);
        for (int i = 0; i < n; i++) w[i].dstSet = ds;
        z2b = PROFD ? prof_now() : 0;
        if (n) vkUpdateDescriptorSets(g_vk.dev, (uint32_t)n, w, 0, NULL);
    }
    if (se) *se = (SetEnt){h, q, layout, ds};
    if (m && reuse) m->ds = ds;
    ubofast_check(ubf_ds, ubf_nd, ubf_d, ds, dyn, *ndyn, stage, ubf_bnd, ubf_wk, ubf_nw, reuse ? wk : NULL, n);
    if (PROFD) {
        uint64_t z3 = prof_now();
        prof_set_alloc += z2b - z2; prof_set_cbuf += zc; prof_set_fill += z2 - z1 - zc; prof_set_update += z3 - z2b;
        prof_set_writes += (uint64_t)n;
    }
    return ds;
}

// ================================================================== compute
typedef struct { Prog p; ShaderInfo *si; VkPipeline pipe; uint32_t local[3]; } CsProg;

static void build_compute(void *arg);
static void build_compute_timed(void *arg) {   // (TOTK_HITCH_FULL_BUILD: a worker's compute build feeds the compute mean)
    uint64_t t0 = SDL_GetTicksNS();
    build_compute(arg);
    if (atomic_load(&((CsProg *)arg)->p.state) != 2) full_est_feed_(&g_full_est_cs, SDL_GetTicksNS() - t0);
}
static void build_compute(void *arg) {
    CsProg *cp = arg;
    size_t n = 0;
    uint32_t *spv = vk_glsl_to_spirv(cp->si->msl, STAGE_COMPUTE, "cs", &n);
    if (!spv) { atomic_store(&cp->p.state, 2); return; }
    cp->p.nbinds = vk_reflect(spv, n, cp->p.binds, MAX_BIND);
    VkShaderModule m = vk_module(spv, n);
    free(spv);
    cp->p.layout = make_layout(&cp->p, 1, VK_SHADER_STAGE_COMPUTE_BIT, 0);
    VkSpecializationMapEntry me[3] = {{0, 0, 4}, {1, 4, 4}, {2, 8, 4}};
    VkSpecializationInfo spec = {3, me, 12, cp->local};
    VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = m, .pName = "main",
                                                .pSpecializationInfo = &spec,
                                                // (a pinned workgroup must fit maxComputeWorkgroupSubgroups x 32 threads; a
                                                // larger one runs unpinned on the wave-safe forms)
                                                .pNext = (uint64_t)cp->local[0] * cp->local[1] * cp->local[2] <= (uint64_t)g_vk.max_wg_subgroups * 32
                                                         ? sg_pin(cp->si, VK_SHADER_STAGE_COMPUTE_BIT) : NULL},
                                      .layout = cp->p.layout};
    VkResult r = vkCreateComputePipelines(g_vk.dev, g_vk.pcache, 1, &ci, NULL, &cp->pipe);
    atomic_fetch_add(&stat_compiles, 1);
    atomic_store(&cp->p.state, r == VK_SUCCESS ? 1 : 2);
    if (r != VK_SUCCESS) LOG("[compute] pipeline failed: %d\n", r);
}

static struct CsEnt { uint64_t addr; CsProg *cp; } g_cs_tab[256];  // compute programs by code address

// compute programs by content (code hash + workgroup size): built ahead by the shader precompile, or the same code at
// another address
#define PC_CSTAB 1024
static struct { uint64_t key; CsProg *cp; } pc_cstab[PC_CSTAB];   // (pc_mu)
static CsProg *pc_cs_get(uint64_t ck, CsProg *put) {   // the program under ck; else registers `put` (if given) and returns it
    pthread_mutex_lock(&pc_mu);
    uint32_t i = (uint32_t)(ck ^ (ck >> 29)) & (PC_CSTAB - 1);
    int probes = 0;
    while (pc_cstab[i].key && pc_cstab[i].key != ck && probes++ < PC_CSTAB) i = (i + 1) & (PC_CSTAB - 1);
    CsProg *r = pc_cstab[i].key == ck ? pc_cstab[i].cp : NULL;
    if (!r && put && !pc_cstab[i].key) { pc_cstab[i].key = ck; pc_cstab[i].cp = put; r = put; }
    pthread_mutex_unlock(&pc_mu);
    return r;
}
static uint64_t pc_comp_key(uint64_t code_hash, const uint32_t local[3]) {
    uint64_t k = hash_words(code_hash ^ PC_SEEN_CS, local, 12);
    return k ? k : 1;
}
// code + compiler constants into cache/code (as pc_record_prog does for graphics programs), then the PC_COMP record
static void pc_record_comp(uint64_t code, uint64_t cmax, uint64_t csize, uint64_t chash, uint64_t ck, const uint32_t local[3]) {
    if (!pc_seen_add(ck ^ PC_SEEN_CS)) return;
    PcComp r;
    memset(&r, 0, sizeof r);   // (csize is set by the writer once the constants file is there: pc_writer)
    r.ckey = ck;
    r.code_hash = chash;
    r.cmax = cmax;
    memcpy(r.local, local, sizeof r.local);
    PcJob *j = pc_job_new(PC_COMP, &r, sizeof r);
    if (!j) return;
    j->hash[0] = chash;
    if (!pc_job_copy(&j->code[0], &j->ncode[0], code, cmax)) { pc_writer_free(j); return; }
    if (csize && pc_readable(code + cmax, csize) && pc_job_copy(&j->c1[0], &j->nc1[0], code + cmax, csize))
        j->c1_at[0] = (int32_t)offsetof(PcComp, csize);
    pc_job_queue(j);
}
// precompile worker: translate + build a recorded compute program
static void pc_comp_build(const PcComp *r) {
    if (pc_cs_get(r->ckey, NULL)) return;
    uint64_t cs = r->csize;
    // (from the archive pack: its code is in pa_file, alive until every worker is past the archive phase)
    const bool in_pack = r->pad && pa_file && r->pad - 1 < pa_nblobs;
    uint8_t *b = in_pack ? (uint8_t *)pa_code + pa_blob[(r->pad - 1) * 4] : pc_load_code_c1(r->code_hash, r->cmax, &cs);
    if (!b) return;
    CsProg *cp = calloc(1, sizeof *cp);
    memcpy(cp->local, r->local, sizeof cp->local);
    ShaderKey k = {0};
    k.glsl = 1;
    pthread_mutex_lock(&translate_lock);
    maxwell_gmem_base = HEAP_BASE;
    maxwell_gmem_size = HEAP_MAX;
    maxwell_gdev_size = gdev_window_size();
    cp->si = translate_stage((uint64_t)(uintptr_t)b, r->cmax, cs, STAGE_COMPUTE, &k);
    pthread_mutex_unlock(&translate_lock);
    if (!in_pack) free(b);
    cp->p.vi = cp->si;
    if (!cp->si->msl || pc_cs_get(r->ckey, cp) != cp) { maxwell_free(cp->si); free(cp); return; }   // (failed, or the game was first)
    build_compute(cp);
}

static CsProg *compute_program(NvnProgram *np) {
    NvnShaderStage *ss = &np->stages[5];
    if (!ss->code_addr) return NULL;
    struct CsEnt *tab = g_cs_tab;
    int n = g_cs_n;
    for (int i = 0; i < n; i++) if (tab[i].addr == ss->code_addr) return tab[i].cp;
    c1_persist_forget(np);
    uint32_t local[3];
    for (int i = 0; i < 3; i++) {
        local[i] = ss->control ? *(const uint32_t *)(ss->control + 0x748 + 4 * i) : 1;
        if (!local[i]) local[i] = 1;
    }
    uint64_t cmax = ss->const_offset ? ss->const_offset : 0x10000;
    uint64_t csize = ss->const_offset ? (ss->const_size < 0x10000 ? ss->const_size : 0x10000) : 0;
    uint64_t chash = 0, ck = 0;
    if (pc_on() && cmax <= 0x100000 && pc_readable(ss->code_addr, cmax)) {
        pc_start();
        chash = hash_words(0x6a09e667f3bcc908ull ^ cmax, (const void *)(uintptr_t)ss->code_addr, (size_t)cmax);
        ck = pc_comp_key(chash, local);
        CsProg *q = pc_cs_get(ck, NULL);
        if (q && atomic_load(&q->p.state) != 2) {   // (built or being built; a failed one is translated again below)
            if (n < 256) { tab[n].addr = ss->code_addr; tab[n].cp = q; g_cs_n = n + 1; }
            return q;
        }
    }
    CsProg *cp = calloc(1, sizeof *cp);
    if (n < 256) { tab[n].addr = ss->code_addr; tab[n].cp = cp; g_cs_n = n + 1; }
    maxwell_gmem_base = HEAP_BASE;
    maxwell_gmem_size = HEAP_MAX;
    maxwell_gdev_size = gdev_window_size();
    ShaderKey k = {0};
    k.glsl = 1;
    pthread_mutex_lock(&translate_lock);
    cp->si = translate_stage(ss->code_addr, cmax, csize, STAGE_COMPUTE, &k);
    pthread_mutex_unlock(&translate_lock);
    cp->p.vi = cp->si;
    if (!cp->si->msl) {
        LOG("[compute] translate failed: %s (code %llx)\n", cp->si->error, (unsigned long long)ss->code_addr);
        static int dumps;  // TotK: what is at the code address
        if (dumps++ < 3) {
            const uint32_t *w = (const uint32_t *)(uintptr_t)ss->code_addr;
            for (int r = 0; r < 8; r++)
                LOG("  code+%02x: %08x %08x %08x %08x\n", r * 16, w[r * 4], w[r * 4 + 1], w[r * 4 + 2], w[r * 4 + 3]);
            if (ss->control) {
                const uint32_t *k = (const uint32_t *)ss->control;
                LOG("  control %p: %08x %08x %08x %08x | +6f0: %08x %08x %08x %08x %08x\n", (void *)ss->control, k[0], k[1], k[2], k[3],
                    k[0x6f0 / 4], k[0x6f4 / 4], k[0x6f8 / 4], k[0x6fc / 4], k[0x700 / 4]);
            }
            uint64_t nz = 0; const uint64_t *q = (const uint64_t *)(uintptr_t)ss->code_addr;
            for (int i = 1; i < 0x2000 / 8; i++) if (q[i]) { nz = (uint64_t)i * 8; break; }
            LOG("  first nonzero qword at code+%llx\n", (unsigned long long)nz);
        }
        atomic_store(&cp->p.state, 2);
        return cp;
    }
    memcpy(cp->local, local, sizeof cp->local);
    save_glsl(cp->si->msl, ss->code_addr, "cs");
    cp->p.ckey = ck;   // ([hold] names it by its seed record key)
    if (ck) { pc_cs_get(ck, cp); pc_record_comp(ss->code_addr, cmax, csize, chash, ck, local); }
    // SPIR-V already in the disk cache: build it here (module + pipeline-cache hit, about a millisecond) instead of
    // skipping the dispatch, which holds the frame (the user's 10-15 PC session: 20 holds for a compute shader alone)
    if (vk_spirv_cached(cp->si->msl, STAGE_COMPUTE)) build_compute(cp);
    else if (full_build_fits(true)) {   // (TOTK_HITCH_FULL_BUILD: a skipped dispatch holds the frame)
        uint64_t t0 = SDL_GetTicksNS();
        build_compute(cp);
        uint64_t dt = SDL_GetTicksNS() - t0;
        full_build_note(dt, true);
        static int n;
        if (n++ < 300) LOG("[newprog] cs %llx: built on the drawing thread (TOTK_HITCH_FULL_BUILD) in %.1f ms\n", (unsigned long long)ss->code_addr, dt / 1e6);
    }
    else run_job(full_build_on() ? build_compute_timed : build_compute, cp, 'c', ss->code_addr, false);
    return cp;
}

static void dispatch_compute(uint32_t gx, uint32_t gy, uint32_t gz) {
    snap_reason = "compute"; snapshots_invalidate_(NULL, false);
    if (!st.program || !gx || !gy || !gz) return;
    CsProg *cp = compute_program(st.program);
    if (ind_log() && cp && cp->si && cp->si->writes_gmem) { ind_wdisp++; ind_note_writes(5); }
    if (g_hold_force && cp && atomic_load(&cp->p.state) == 0 && !bake_is_dead(cp)) {   // (held 30 frames: wait for it, like a bake)
        uint64_t t0 = SDL_GetTicksNS();
        while (atomic_load(&cp->p.state) == 0 && SDL_GetTicksNS() - t0 < 3000000000ull) SDL_Delay(1);
        g_hold_force_wait_ns += SDL_GetTicksNS() - t0;
        if (atomic_load(&cp->p.state) == 0) bake_timeout("compute", st.program->stages[5].code_addr, cp);
    }
    if (!cp || atomic_load(&cp->p.state) != 1) {
        stat_skipped++; stat_skip_why[6]++;
        if (cp && atomic_load(&cp->p.state) == 0) {
            g_skip_comp_frame++;
            hold_key_note('c', cp->p.ckey ? cp->p.ckey : st.program->stages[5].code_addr);
            prog_hurry(&cp->p, "compute cs", st.program->stages[5].code_addr);
        }
        return;
    }
    // more than 2^28 threads in one dispatch is a garbage size, not TotK: skipped instead of hanging the GPU
    uint64_t threads = (uint64_t)gx * gy * gz * cp->local[0] * cp->local[1] * cp->local[2];
    if (threads > (1ull << 28)) {
        static int nlog;
        if (nlog++ < 20) LOG("[vk] dispatch skipped: %ux%ux%u groups of %ux%ux%u (cs %llx)\n", gx, gy, gz, cp->local[0], cp->local[1],
                             cp->local[2], (unsigned long long)st.program->stages[5].code_addr);
        stat_skipped++;
        return;
    }
    vk_end_pass();
    VkCommandBuffer cb = vk_cmd();
    hang_op(cb, HOP_DISPATCH, st.program->stages[5].code_addr, threads, gx, gy, gz);
    vk_full_barrier(cb);
    gp_mark(cb, "dispatch %ux%ux%u (local %ux%ux%u)", gx, gy, gz, cp->local[0], cp->local[1], cp->local[2]);
    dl_note("dispatch %ux%ux%u cs %llx gmem %d", gx, gy, gz, (unsigned long long)st.program->stages[5].code_addr, cp->si->uses_gmem);
    nfeedback = 0;
    uint32_t cdyn[MAX_BIND], ncdyn = 0;
    {
        static const int cstage = 5;
        twins_refresh(&cp->si, &cstage, 1, false);
    }
    VkDescriptorSet ds = stage_set(&cp->p, cp->p.set_layout[0], 0, 5, cp->si, NULL, NULL, gx, gy, gz, NULL, cdyn, &ncdyn);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, cp->pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, cp->p.layout, 0, 1, &ds, ncdyn, cdyn);
    for (int k = 0; k < cp->si->ntex; k++)   // (LARECOMP_SNAP_DELTA: only image stores write images; storage buffers are guest memory)
        if (cp->si->tex[k].is_image) { snap_ndgen++; snap_nd_why = "image-store dispatch"; break; }
    vkCmdDispatch(cb, gx, gy, gz);
    stat_dispatches++; chunk_draws++;
    if (g_debug_frame)   // (with LARECOMP_SAVE_MSL=1 its GLSL is build/glsl/<cs>_cs.glsl)
        LOG("[dispatch] %ux%ux%u local %ux%ux%u cs %llx gmem %d ntex %d\n", gx, gy, gz, cp->local[0], cp->local[1], cp->local[2],
            (unsigned long long)st.program->stages[5].code_addr, cp->si->uses_gmem, cp->si->ntex);
}

// ================================================================== draws
static void debug_draw(Prog *pr, CmdRec *r, bool indexed);
static void debug_probe(Prog *pr);
static void debug_probe_row(Prog *pr);
static bool probe_dt, probe_dw;    // (debug_probe: the depth state draw() set for the probed draw)
static VkCompareOp probe_dop;

// Quads / fans / polygons as a 32-bit triangle index list.
static uint32_t *expand_indices(uint32_t mode, uint32_t count, const void *indices, int itype, uint32_t first, uint32_t *out_count) {
    uint32_t tris = mode == 7 ? count / 4 * 2 : count >= 3 ? count - 2 : 0;
    uint32_t *out = malloc(tris * 3 * 4 + 4);
    uint32_t n = 0;
#define IDX(i) (indices ? (itype == 0 ? ((const uint8_t *)indices)[i] : itype == 1 ? ((const uint16_t *)indices)[i] : ((const uint32_t *)indices)[i]) : first + (i))
    if (mode == 7) {
        for (uint32_t q = 0; q + 3 < count; q += 4) {
            out[n++] = IDX(q); out[n++] = IDX(q + 1); out[n++] = IDX(q + 2);
            out[n++] = IDX(q); out[n++] = IDX(q + 2); out[n++] = IDX(q + 3);
        }
    } else {
        for (uint32_t i = 1; i + 1 < count; i++) { out[n++] = IDX(0); out[n++] = IDX(i); out[n++] = IDX(i + 1); }
    }
#undef IDX
    *out_count = n;
    return out;
}

static bool topology(uint32_t mode, VkPrimitiveTopology *t, uint8_t *cls) {
    switch (mode) {
    case 0: *t = VK_PRIMITIVE_TOPOLOGY_POINT_LIST; *cls = 0; return true;
    case 1: *t = VK_PRIMITIVE_TOPOLOGY_LINE_LIST; *cls = 1; return true;
    case 3: *t = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP; *cls = 1; return true;
    case 4: *t = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; *cls = 2; return true;
    case 5: *t = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP; *cls = 2; return true;
    default: *t = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; *cls = 2; return false;
    }
}

static void draw_(CmdRec *r, bool indexed);
static int flame_probe_x = -1, flame_probe_y;  // (LARECOMP_FLAME_AUTO: the emitter pixel the probe reads)
static bool flame_nodepth;  // (LARECOMP_FLAME_AUTO=...,ab second capture: the torch flame draw without depth test)
static void draw(CmdRec *r, bool indexed) {
    // LARECOMP_NULL_DRAWS=1 (measurement, 2026-10-17): every draw dropped before any work -> the fps the guest CPU alone
    // allows (render thread nearly idle, GPU near idle so the CPU gets the Deck's power budget)
    static int null_draws = -1;
    if (null_draws < 0) null_draws = getenv("LARECOMP_NULL_DRAWS") != NULL;
    if (null_draws) { stat_skipped++; return; }
    PROF_T0D();
    g_draw_newprog = false;
    draw_(r, indexed);
    PROF_ADDD(prof_draw);
    if (PROFD) {
        hf_draw_ns[g_draw_newprog] += prof_now() - prof_t0_; hf_draws[g_draw_newprog]++;
        for (int i = 0; i < 4; i++) hf_gps[i] += gps_ns[i];
    }
}
static uint64_t prof_tp_;
// LARECOMP_LATE_LATCH: the canvas the frame's upscale draw went into, until it is warped (draw_ / vk_late_latch_present)
static NvnTexture *ll_canvas;
static uint64_t ll_frame;
static void late_latch_fire(bool at_present) {
    vk_feedback_epoch_bump();   // (the warp rewrites the canvas outside the guest's commands)
    NvnTexture *c = ll_canvas;
    ll_canvas = NULL;
    if (!c || renderer_present_count() != ll_frame) return;
    vk_end_pass();
    gp_mark(vk_cmd(), "late latch warp");   // (LARECOMP_GPUPROF: its GPU time next to the latency it buys)
    vk_late_latch_warp(vk_cmd(), vk_render_backend(c), backend_async_presents_done(), nvn_window_origin() == 0, at_present);
    gp_mark(vk_cmd(), "after the late latch warp");
    dyn_invalidate();
}
void vk_late_latch_present(void) { if (ll_canvas) late_latch_fire(true); }
static void draw_(CmdRec *r, bool indexed) {
    if (PROFD) { prof_tp_ = prof_now(); memset(gps_ns, 0, sizeof gps_ns); }
    if (!st.program) { stat_skipped++; stat_skip_why[0]++; return; }
    if (!g_debug_frame && e1_armed && e1_n) e1_report();
    if (st.program->stage_mask & 0x1C) {
        static uint64_t seen[32]; static int nseen;
        uint64_t k = st.program->stages[1].code_addr ^ (uint64_t)st.program->stage_mask << 56;
        int known = 0;
        for (int i = 0; i < nseen; i++) known |= seen[i] == k;
        if (!known && nseen < 32) {
            seen[nseen++] = k;
            LOG("[stages] program with stage mask %#x drawn as vertex + fragment only: mode %llu, count %llu, instances %llu, fs %llx\n",
                st.program->stage_mask, (unsigned long long)r->x[0], (unsigned long long)r->x[2], (unsigned long long)r->x[3],
                (unsigned long long)st.program->stages[1].code_addr);
        }
    }
    ShaderKey vkey = {0}, fkey = {0};
    vkey.glsl = fkey.glsl = 1;
    vkey.flip_y = fkey.flip_y = nvn_window_origin() == 0;
    vkey.depth_minus_one = nvn_depth_mode() == 0;
    {   // only attributes the vertex shader reads (SPH imap: 4 bits per generic attribute at SPH + 0x18) take part in the
        // key: the translator declares just those, and the archive precompile (tools/shader_pack.py) keys them the same way
        const uint8_t *sph = st.program->stages[0].code_addr && (st.program->stage_mask & 1)
                                 ? (const uint8_t *)(uintptr_t)(st.program->stages[0].code_addr + 0x30) : NULL;
        for (int i = 0; i < st.nattrib && i < 32; i++)
            if (!sph || ((sph[0x18 + i / 2] >> ((i & 1) * 4)) & 0xF)) vkey.attr_kind[i] = (uint8_t)attr_kind(st.attrib[i] & 0xFF);
    }
    for (int i = 0; i < st.rt.ncolor; i++) if (st.rt.color[i]) fkey.rt_kind[i] = (uint8_t)rt_kind(rt_view_format(i));
    // Vulkan's A2C at 1x keeps only alpha >= 0.5, which bared the far trees. Multisampled targets keep the pipeline's
    // A2C.
    static int a2cd = -1;
    if (a2cd < 0) { const char *e = getenv("TOTK_A2C_DITHER"); a2cd = e && strcmp(e, "0"); if (a2cd) LOG("[vk] TOTK_A2C_DITHER: alpha to coverage dithered in the shader with one sample\n"); }
    bool a2c_dither = false;
    if (a2cd && st.ms[0xC] && st.ms[0xD]) {
        bool msaa = st.rt.depth && st.rt.depth->samples > 1;
        for (int i = 0; i < st.rt.ncolor; i++) if (st.rt.color[i] && st.rt.color[i]->samples > 1) msaa = true;
        if (!msaa) a2c_dither = fkey.a2c_dither = 1;
    }
    if (smark_nfs && smark_on() && smark_fs_has(st.program->stages[1].code_addr)) fkey.snap_mark = 1;   // (LARECOMP_SNAP_MARK)
    // One-shot bakes (3D volumes, BC volumes the game encodes on the GPU at load, and nothing redraws them) never skip:
    // program and pipeline are built / waited for (<= 3 s). Screen passes keep the async skip + held frame.
    // LARECOMP_BAKE_ASYNC=1: bakes skip like the rest (old).
    static int bake_async = -1;
    if (bake_async < 0) bake_async = getenv("LARECOMP_BAKE_ASYNC") != NULL;
    // LARECOMP_BAKE_SMALL: small 2D colour targets (<= 512x512 texels) count as bakes too. The pause menu renders each
    // fused weapon's icon once as it opens (144x144 sRGB target, read back to guest memory, copied by the CPU into the
    // texture the menu samples), while that menu's programs are new: gc-e5-trace6 skipped a draw into a 384x384 and a
    // 216x216 target at frame 671, the first icon rendered at 673 and its tile stayed empty for the whole menu;
    // gc-e5-trace4 skipped 1-9 draws each into 128x128.. 384x384 targets at frames 619-700. Nothing redraws an icon. With
    // this on (gc-e5-trace5) no draw into a small target was skipped. Screen-sized passes keep the skip + held frame. (A
    // red first version of an icon that turns grey seconds later is not this: it shows with nothing skipped.)
    static int bake_small = -1;
    if (bake_small < 0) { const char *e = getenv("LARECOMP_BAKE_SMALL"); bake_small = !e || *e != '0'; if (bake_small) LOG("[vk] small 2D targets build their programs before drawing (LARECOMP_BAKE_SMALL)\n"); }
    g_draw_bake = false;
    for (int i = 0; i < st.rt.ncolor && !bake_async; i++) {
        NvnTexture *t = st.rt.color[i];
        if (t && (t->target == 2 || (t->format >= 0x41 && t->format <= 0x50))) g_draw_bake = true;
        if (t && bake_small && t->target == 1 && (uint64_t)t->width * t->height <= 512u * 512u) g_draw_bake = true;
    }
    g_bake_sync = g_draw_bake || g_hold_force;
    g_prog_full_draw = r->x[2] <= 6;
    Prog *pr = get_program(st.program, &vkey, &fkey);
    // (LARECOMP_SNAP_MARK: the program without the marks, asked for only when the variant or its pipeline is not ready,
    // so a ready variant keeps get_program's one-entry memo)
    bool smark_variant = false;
    ShaderKey smark_fk0;
    if (fkey.snap_mark) {
        smark_fk0 = fkey;
        smark_fk0.snap_mark = 0;
        if (pr && atomic_load(&pr->state) == 2)   // (the variant failed: not asked for again)
            for (int i = 0; i < smark_nfs; i++) if (smark_fs[i] == st.program->stages[1].code_addr) smark_fs[i] = smark_fs[--smark_nfs];
        if (!pr || atomic_load(&pr->state) != 1) pr = get_program(st.program, &vkey, &smark_fk0);   // (built aside; drawn as before)
        else smark_variant = true;
    }
    if (ind_log() && pr && pr->vi && pr->fi && (pr->vi->writes_gmem || pr->fi->writes_gmem)) { ind_wdraw++; ind_note_writes(0); ind_note_writes(1); }
    g_prog_full_draw = false;
    g_bake_sync = false;
    if (PROFD && pr) {   // ("[hitch] ... first use": precompiled programs count from their first draw too)
        uint64_t f1 = renderer_present_count() + 1;
        if (!pr->first_draw_f) pr->first_draw_f = f1;
        g_draw_newprog = pr->first_draw_f == f1;
    }
    if ((g_draw_bake || g_hold_force) && pr && atomic_load(&pr->state) == 0 && !bake_is_dead(pr)) {   // (queued earlier, or by the precompile: wait for it)
        uint64_t t0 = SDL_GetTicksNS();
        while (atomic_load(&pr->state) == 0 && SDL_GetTicksNS() - t0 < 3000000000ull) SDL_Delay(1);
        prof_bake_waits++;
        prof_bake_wait_ns += SDL_GetTicksNS() - t0;
        if (g_hold_force) g_hold_force_wait_ns += SDL_GetTicksNS() - t0;
        if (atomic_load(&pr->state) == 0) bake_timeout("program", pr->ckey ? pr->ckey : pr->code[1], pr);
    }
    uint64_t pz1 = PROFD ? prof_now() : 0;
    if (!pr || atomic_load(&pr->state) != 1) {
        stat_skipped++; stat_skip_why[1]++;
        if (pr && atomic_load(&pr->state) == 0) {
            g_skip_prog_frame++; g_skip_prog_full += r->x[2] <= 6;
            hold_key_note(r->x[2] <= 6 ? 'f' : 'p', pr->ckey ? pr->ckey : pr->code[1]);
            if (r->x[2] <= 6) prog_hurry(pr, "program fs", build_log() ? prog_code_hash(pr, 1) : 0);
        }
        return;
    }
    {   // fragment-shader hash: interior backdrop detection (widescreen), skip lists, item twinkle (renderer.m)
        static uint64_t last_fs, last_hash;
        uint64_t fs = st.program->stages[1].code_addr;
        if (fs != last_fs) {
            // (cached per code address: the fragment shader changes on most draws and the 512-byte byte-wise hash was
            // ~9 % of draw_ on the Deck, d47 perf annotate. Two code words check that the address still holds the same
            // shader; the hash itself must not change: hmix values are compared with stored constants.)
            // (2048 entries at a multiplicative index: 512 at (fs >> 4 ^ fs >> 13) missed ~100 times a frame in Kakariko,
            // ~0.5 us each; the cached value is the same hmix, so nothing drawn changes)
            static struct { uint64_t fs, w0, w1, h; uint32_t gen; } fsc[2048];
            last_fs = fs;
            if (fs) {
                const uint64_t *cw = (const uint64_t *)(uintptr_t)(fs + 0x30);
                uint32_t i = (uint32_t)((fs * 0x9E3779B97F4A7C15ull) >> 53);
                if (fsc[i].fs != fs || fsc[i].gen != g_memo_gen || fsc[i].w0 != cw[10] || fsc[i].w1 != cw[63])
                    fsc[i] = (__typeof__(fsc[0])){fs, cw[10], cw[63], hmix(1469598103934665603ull, cw, 0x200), g_memo_gen};
                last_hash = fsc[i].h;
            } else last_hash = 0;
        }
        static uint64_t skip[16];
        static int nskip = -1;
        if (nskip < 0) {
            nskip = 0;
            for (const char *p = getenv("LARECOMP_SKIP_FSHASH"); p && *p && nskip < 16;) {
                skip[nskip++] = strtoull(p, (char **)&p, 16);
                while (*p == ',') p++;
            }
        }
        for (int i = 0; i < nskip; i++) if (last_hash == skip[i]) { stat_skipped++; stat_skip_why[2]++; return; }
        extern bool pot_level_active(void);
        if (last_hash == 0xa2177dbb72197bd9ull && pot_level_active()) { stat_skipped++; stat_skip_why[2]++; return; }
        if (g_debug_frame && getenv("LARECOMP_LOG_FSHASH")) LOG("[fshash] fs=%llx hash %016llx\n", (unsigned long long)fs, (unsigned long long)last_hash);
        g_cur_fs_hash = last_hash;
        {   // TOTK_DLSS_TRACE=<present>: every draw from the upscale pass to the present, in that frame and the next (where
            // the HUD lands: the canvas or the presented image; device.c names the presented one)
            static long tr = -2;
            static uint64_t tr_from;
            if (tr == -2) { const char *e = getenv("TOTK_DLSS_TRACE"); tr = e ? atol(e) : -1; }
            uint64_t pc = renderer_present_count();
            if (tr >= 0 && pc >= (uint64_t)tr && pc <= (uint64_t)tr + 1) {
                if (is_upscale_fs(last_hash)) tr_from = pc + 1;
                TexBackend *tb = st.rt.color[0] ? vk_render_backend(st.rt.color[0]) : NULL;
                if (tr_from == pc + 1)
                    LOG("[dlss-trace] present %llu: fs %016llx -> colour 0 %p %ux%u format %d (%d targets, depth %ux%u)\n",
                        (unsigned long long)pc, (unsigned long long)last_hash, (void *)st.rt.color[0], tb ? tb->width : 0,
                        tb ? tb->height : 0, tb ? (int)tb->format : 0, pass_ncolor, st.rt.depth ? st.rt.depth->width : 0,
                        st.rt.depth ? st.rt.depth->height : 0);
            }
        }
        // SR did not run (FG only: the scene is larger than the canvas): the game's upscale draw went in, and the next draw
        // into the canvas is the HUD's first, so the canvas is the HUD-less image then (ngx.c keeps it for DLSS-G). Draws
        // into other targets may come first; they leave the canvas alone. LARECOMP_LATE_LATCH (latch.c): the canvas re-aimed
        // by the mouse motion the frame's camera missed, after the game's upscale draw (or DLSS in its place) and before the
        // HUD's first draw into it (and before FG keeps it HUD-less); a frame with no later draw into the canvas is warped
        // at its present (vk_late_latch_present). Either upscale pass arms it (EASU, or 6dc5 at the dynamic resolution's
        // 1280x720 tier), 6dc5 only into a wider target: at 1600x900 the game runs it 1:1 into an intermediate before EASU
        bool upscale_pass = is_upscale_fs(last_hash);
        if (ll_canvas && !upscale_pass && st.rt.color[0] == ll_canvas) late_latch_fire(false);
        if (upscale_pass && st.rt.color[0] && vk_late_latch_on()) {
            NvnTexture *src = last_hash == 0x6dc5011326ac42a2ull && pr->fi->ntex >= 1 ? shader_texture_object(1, &pr->fi->tex[0]) : NULL;
            if (last_hash != 0x6dc5011326ac42a2ull || (src && st.rt.color[0]->width > src->width)) {
                ll_canvas = st.rt.color[0];
                ll_frame = renderer_present_count();
            }
        }
        static NvnTexture *hl_canvas;
        static uint64_t hl_frame;
        if (hl_canvas && renderer_present_count() != hl_frame) hl_canvas = NULL;
        if (hl_canvas && !upscale_pass && st.rt.color[0] == hl_canvas) {
            vk_end_pass();
            vk_ngx_fg_keep_hudless(vk_cmd(), vk_render_backend(hl_canvas), hl_frame);
            dyn_invalidate();
            hl_canvas = NULL;
        }
        // (Any other draw that samples a texture of the latest G-buffer depth's size into a larger target is named once
        // per shader, 40 at most, to find the pass the game uses there. TOTK_DLSS_HUNT=1 only.)
        static int dlss_hunt = -1;
        if (dlss_hunt < 0) dlss_hunt = getenv("TOTK_DLSS_HUNT") && strcmp(getenv("TOTK_DLSS_HUNT"), "0");
        if (dlss_hunt && !upscale_pass && g_dlss_cand[0] && st.rt.color[0] && pr->fi->ntex >= 1 && vk_ngx_sr_on()) {
            int li = 0;
            for (int i = 1; i < 4; i++) if (g_dlss_cand[i] && g_dlss_cand_at[i] > g_dlss_cand_at[li]) li = i;
            NvnTexture *g = g_dlss_cand[li], *rt = st.rt.color[0];
            if (rt->width > g->width && rt->height > g->height) {
                NvnTexture *src = shader_texture_object(1, &pr->fi->tex[0]);
                static uint64_t seen[40];
                static int nseen;
                int i = 0;
                while (i < nseen && seen[i] != last_hash) i++;
                if (src && src->width == g->width && src->height == g->height && i == nseen && nseen < 40) {
                    seen[nseen++] = last_hash;
                    LOG("[dlss] unhooked upscale-like draw, frame %llu: fs %016llx (code %llx) samples %ux%u (the G-buffer's size) into %ux%u f%#x, %llu vertices\n",
                        (unsigned long long)renderer_present_count(), (unsigned long long)last_hash,
                        (unsigned long long)st.program->stages[1].code_addr, src->width, src->height, rt->width,
                        rt->height, rt->format, (unsigned long long)r->x[2]);
                }
            }
        }
        // TOTK_DLSS=sr (ngx.c): the game's upscale pass (scene -> canvas) is replaced by DLSS Super Resolution on that
        // scene colour, the frame's G-buffer depth and motion vectors made from it; the draw is dropped when it ran
        if (upscale_pass && g_dlss_cand[0] && st.rt.color[0] && pr->fi->ntex >= 1 && vk_ngx_sr_on()) {
            NvnTexture *src = shader_texture_object(1, &pr->fi->tex[0]);
            // (fs 6dc5011326ac42a2 is the scene -> canvas pass only at the 1280x720 tier; at 1600x900 the game runs it
            // 1:1 into a 1600x900 target before EASU: SR took that one, as DLAA, and refused EASU after it)
            if (src && last_hash == 0x6dc5011326ac42a2ull && st.rt.color[0]->width <= src->width) src = NULL;
            if (src) {
                // the depth of the scene's size, latest first (the size changes with dynamic resolution; a later
                // quarter-size pass has its own)
                int bi = -1;
                for (int i = 0; i < 4; i++)
                    if (g_dlss_cand[i] && g_dlss_cand[i]->width == src->width && g_dlss_cand[i]->height == src->height &&
                        (bi < 0 || g_dlss_cand_at[i] > g_dlss_cand_at[bi]))
                        bi = i;
                if (bi >= 0) g_dlss_depth = g_dlss_cand[bi];
                else if (!g_dlss_depth) g_dlss_depth = g_dlss_cand[0];   // (SR refuses it, naming both sizes)
                vk_end_pass();
                vk_feedback_epoch_bump();   // (SR writes the canvas outside a draw)
                bool ok = vk_ngx_sr_upscale(vk_cmd(), vk_render_backend(src), vk_render_backend(g_dlss_depth), vk_render_backend(st.rt.color[0]),
                                            renderer_present_count(), nvn_window_origin() == 0, nvn_depth_mode() == 0);
                dyn_invalidate();
                if (ok) return;
                hl_canvas = st.rt.color[0];
                hl_frame = renderer_present_count();
            }
        }
        extern void widescreen_backdrop_seen(uint64_t hash);
        if (last_hash) widescreen_backdrop_seen(last_hash);
        extern void widescreen_scene_ubo(const float *u, uint64_t size);  // (the camera's view-projection)
        if (st.ubuf[0][5].addr) widescreen_scene_ubo((const float *)(uintptr_t)st.ubuf[0][5].addr, st.ubuf[0][5].size);
    }
    // DLSS foundation (ngx.c, TOTK_DLSS only): the frame's camera = VS cb8 at its first G-buffer draw (>= 3 colour targets and
    // a D32F depth, as the Mac port picks it), and the depth targets such passes draw into. The upscale picks the one of its
    // scene's size: a later quarter-size pass has 3 colour targets + D32F too, and with dynamic resolution the G-buffer's own
    // size changes
    if ((vk_ngx_on() || vk_late_latch_on()) && pass_ncolor >= 3 && pass_depth == VK_FORMAT_D32_SFLOAT && st.rt.depth && st.ubuf[0][5].addr &&
        st.ubuf[0][5].size >= 30 * 16) {
        uint64_t now = renderer_present_count();
        int slot = 0;
        for (int i = 0; i < 4; i++) {
            if (g_dlss_cand[i] == st.rt.depth) { slot = i; break; }
            if (g_dlss_cand_at[i] < g_dlss_cand_at[slot]) slot = i;
        }
        g_dlss_cand[slot] = st.rt.depth;
        g_dlss_cand_at[slot] = now + 1;
        // the camera from the G-buffer's pass, not the quarter-size one (half as wide), unless no G-buffer came lately
        static uint32_t latch_w;
        static uint64_t latch_at;
        if (now > latch_at + 2 || st.rt.depth->width * 4 >= latch_w * 3) {
            latch_w = st.rt.depth->width;
            latch_at = now;
            if (vk_ngx_on())
                vk_ngx_gbuffer_draw((const float *)(uintptr_t)st.ubuf[0][5].addr, st.ubuf[0][5].size, st.rt.depth->width, st.rt.depth->height, now);
            vk_late_latch_camera((const float *)(uintptr_t)st.ubuf[0][5].addr, st.ubuf[0][5].size, backend_async_presents_done());
        }
    }
    flame_nodepth = false;
    if (g_cur_fs_hash == 0x85b366a0e972a9bcull && st.ubuf[0][7].addr && st.ubuf[0][5].size >= 8 * 16) {
        // TotK torch flame: the emitter's screen pixel and distance (cb10 [4..7] = emitter matrix, translation in .w;
        // VS cb8 rows 0-2 = view, rows 4-7 = projection, as the flame VS uses them). LARECOMP_FLAME_AUTO=<metres>[,ab]:
        // the first time the flame is drawn at least that far in front of the camera and on screen, dump the next frame
        // and probe the emitter's pixel; ",ab" adds a second capture (>= 90 frames later) with the flame's depth test off.
        const float *e = (const float *)(uintptr_t)st.ubuf[0][7].addr, *m = (const float *)(uintptr_t)st.ubuf[0][5].addr;
        float v[4] = {0, 0, 0, 1}, c[4];
        for (int i = 0; i < 3; i++) v[i] = m[i * 4] * e[19] + m[i * 4 + 1] * e[23] + m[i * 4 + 2] * e[27] + m[i * 4 + 3];
        for (int i = 0; i < 4; i++) c[i] = m[(4 + i) * 4] * v[0] + m[(4 + i) * 4 + 1] * v[1] + m[(4 + i) * 4 + 2] * v[2] + m[(4 + i) * 4 + 3];
        float nx = c[3] ? c[0] / c[3] : 0, ny = c[3] ? c[1] / c[3] : 0, dist = -v[2];
        int px = (int)(st.viewport[0][0] + (nx * 0.5f + 0.5f) * st.viewport[0][2]);
        int py = (int)(st.viewport[0][1] + (0.5f - 0.5f * ny) * st.viewport[0][3]);
        static float auto_min = -1, auto_dy, auto_near;
        static long auto_after;
        static int auto_ab, captures;
        static uint64_t last_capture;
        if (auto_min < 0) {
            const char *a = getenv("LARECOMP_FLAME_AUTO");
            auto_min = a ? (float)atof(a) : 0;
            auto_ab = a && strstr(a, "ab") != NULL;
            const char *d = a ? strstr(a, "dy=") : NULL;  // ",dy=<metres>": probe the world point that far above/below the emitter
            auto_dy = d ? (float)atof(d + 3) : 0;
            const char *nr = a ? strstr(a, "near=") : NULL;  // ",near=<m>": capture 1 = flame closer than that, capture 2 = far
            auto_near = nr ? (float)atof(nr + 5) : 0;
            const char *af = a ? strstr(a, "after=") : NULL;  // ",after=<present>": no capture before that present
            auto_after = af ? atol(af + 6) : 0;
        }
        int qx = px, qy = py > 15 ? py - 15 : 0;  // (default: above the torch head, where the flame burns)
        if (auto_dy) {
            float w[4] = {0, 0, 0, 1}, q[4];
            for (int i = 0; i < 3; i++) w[i] = m[i * 4] * e[19] + m[i * 4 + 1] * (e[23] + auto_dy) + m[i * 4 + 2] * e[27] + m[i * 4 + 3];
            for (int i = 0; i < 4; i++) q[i] = m[(4 + i) * 4] * w[0] + m[(4 + i) * 4 + 1] * w[1] + m[(4 + i) * 4 + 2] * w[2] + m[(4 + i) * 4 + 3];
            if (q[3] > 0) {
                qx = (int)(st.viewport[0][0] + (q[0] / q[3] * 0.5f + 0.5f) * st.viewport[0][2]);
                qy = (int)(st.viewport[0][1] + (0.5f - 0.5f * q[1] / q[3]) * st.viewport[0][3]);
            }
        }
        bool on_screen = c[3] > 0 && px >= 0 && py >= 0 && px < (int)st.viewport[0][2] && py < (int)st.viewport[0][3];
        {   // game-speed gauge: the effect clock (cb10[2][0]) against wall time, once per 60 presents
            static uint64_t last_p;
            uint64_t p = renderer_present_count();
            if (p / 60 != last_p / 60) {
                last_p = p;
                const float *cm = st.ubuf[0][5].size >= 30 * 16 ? m + 116 : NULL;  // camera position (VS cb8[29])
                LOG("[speed] present %llu wall %.3f s effect time %g cam %g %g %g\n", (unsigned long long)p, SDL_GetTicksNS() / 1e9,
                    e[8], cm ? cm[0] : 0.f, cm ? cm[1] : 0.f, cm ? cm[2] : 0.f);
            }
        }
        if (g_debug_frame) {
            LOG("[flame] emitter view %g %g %g clip %g %g %g %g -> pixel %d,%d dist %g%s\n", v[0], v[1], v[2], c[0], c[1], c[2], c[3], px,
                py, dist, on_screen ? "" : " (off screen)");
            flame_nodepth = auto_ab && captures == 2;
            if (flame_nodepth) LOG("[flame] capture 2: depth test off for this draw\n");
            if (st.rt.ncolor && st.rt.color[0] && st.rt.color[0]->backend) {  // its colour target before the draw (after: below)
                char path[96];
                snprintf(path, sizeof path, "build/pass_flame_%llu_before.png", (unsigned long long)renderer_present_count());
                renderer_save_texture_tex((TexBackend *)st.rt.color[0]->backend, path);
            }
        } else if (auto_min > 0 && on_screen && (long)renderer_present_count() >= auto_after &&
                   (auto_near && !captures ? dist <= auto_near : dist >= auto_min) &&
                   captures < (auto_ab || auto_near ? 2 : 1) &&
                   (!captures || renderer_present_count() >= last_capture + 90)) {
            extern volatile int g_debug_frame_request;
            g_debug_frame_request = 1;
            flame_probe_x = qx; flame_probe_y = qy;
            last_capture = renderer_present_count();
            captures++;
            LOG("[flame] auto capture %d at present %llu: emitter %g m away at pixel %d,%d -> dumping + probing pixel %d,%d of the next frame\n",
                captures, (unsigned long long)last_capture, dist, px, py, qx, qy);
        }
    }
    if (g_debug_frame && g_cur_fs_hash == 0x85b366a0e972a9bcull && st.ubuf[0][7].addr) {  // TotK torch flame: VS cb10 (emitter
        const float *e = (const float *)(uintptr_t)st.ubuf[0][7].addr;                     // constants; cb10[3][0] = alpha)
        LOG("[flame] vs cb10 %llx +%llx:", (unsigned long long)st.ubuf[0][7].addr, (unsigned long long)st.ubuf[0][7].size);
        for (int i = 0; i < 32 && (uint64_t)i * 4 < st.ubuf[0][7].size; i++) LOG(" %g", e[i]);
        LOG("\n");
        if (st.ubuf[1][5].size >= 31 * 16 && st.ubuf[1][6].size >= 141 * 16) {  // FS soft-particle depth fade inputs
            const float *c8 = (const float *)(uintptr_t)st.ubuf[1][5].addr + 30 * 4, *c9 = (const float *)(uintptr_t)st.ubuf[1][6].addr;
            LOG("[flame] fs cb8[30] %g %g %g %g | cb9[139] %g %g %g %g | cb9[140] %g %g %g %g\n", c8[0], c8[1], c8[2], c8[3],
                c9[139 * 4], c9[139 * 4 + 1], c9[139 * 4 + 2], c9[139 * 4 + 3], c9[140 * 4], c9[140 * 4 + 1], c9[140 * 4 + 2], c9[140 * 4 + 3]);
        }
        if (st.ubuf[0][5].size >= 30 * 16) {  // VS cb8[28] (camera forward, used in the behind-camera test) and cb8[29] (camera pos)
            const float *c = (const float *)(uintptr_t)st.ubuf[0][5].addr;
            float dx = e[19] - c[29 * 4], dy = e[23] - c[29 * 4 + 1], dz = e[27] - c[29 * 4 + 2];
            LOG("[flame] vs cb8[28] %g %g %g %g | cb8[29] %g %g %g %g | emitter-camera %g %g %g, along cb8[28] %g\n", c[112], c[113],
                c[114], c[115], c[116], c[117], c[118], c[119], dx, dy, dz, dx * c[112] + dy * c[113] + dz * c[114]);
        }
        LOG("[flame] draw x0..x6 %llx %llx %llx %llx %llx %llx %llx\n", (unsigned long long)r->x[0], (unsigned long long)r->x[1],
            (unsigned long long)r->x[2], (unsigned long long)r->x[3], (unsigned long long)r->x[4], (unsigned long long)r->x[5],
            (unsigned long long)r->x[6]);
        for (int i = 0; i < 16; i++) {  // vertex attributes (a4.w / a5.w = particle lifetime / birth time): first 12 elements as floats
            if (!(pr->vi->attr_mask & (1u << i))) continue;
            uint32_t a = i < st.nattrib ? st.attrib[i] : 0, s = (a >> 8) & 0x1F, off = a >> 13;
            uint32_t stride = s < (uint32_t)st.nstream ? st.stream[s][0] : 0, div = s < (uint32_t)st.nstream ? st.stream[s][1] : 0;
            LOG("[flame] a%d fmt %#x stream %u off %u stride %u div %u vbuf %llx +%llx:", i, a & 0xFF, s, off, stride, div,
                (unsigned long long)st.vbuf[s & 15].addr, (unsigned long long)st.vbuf[s & 15].size);
            for (uint32_t k = 0; k < 12 && st.vbuf[s & 15].addr; k++) {
                uint64_t o = (uint64_t)k * stride + off;
                if (o + 16 > st.vbuf[s & 15].size) break;
                const float *f = (const float *)(uintptr_t)(st.vbuf[s & 15].addr + o);
                LOG(" (%g %g %g %g)", f[0], f[1], f[2], f[3]);
                if (!stride) break;
            }
            LOG("\n");
        }
    }
    if (g_debug_frame && g_cur_fs_hash == 0x18ef69743ba92eabull) {  // TotK point-light volume (torch light): FS cb3[10..15], cb4[0..7]
        for (int k = 0; k < 2; k++) {                                // (cb4[5] = light position, cb4[0].y = 1/r^2, cb4[3..7] = colour/params)
            const Range *ub = &st.ubuf[1][k];
            int from = k ? 0 : 10, to = k ? 8 : 16;
            LOG("[light] fs cb%d %llx +%llx:", k + 3, (unsigned long long)ub->addr, (unsigned long long)ub->size);
            for (int i = from; i < to && ub->addr && (uint64_t)(i + 1) * 16 <= ub->size; i++) {
                const float *f = (const float *)(uintptr_t)(ub->addr + i * 16);
                LOG(" [%d] %g %g %g %g", i, f[0], f[1], f[2], f[3]);
            }
            LOG("\n");
        }
    }
    if (g_debug_frame && g_cur_fs_hash == 0x3a703939f30c38beull) {  // TotK tile lighting: the per-tile words its VS reads
        static int dumped;   // (vertex storage buffer 4 + 0x41c0 + 16 * tile; 25 x 15 tiles of 64 px; all work so far flushed first)
        if (!dumped++ && st.sbuf[0][4].addr) {
            renderer_flush();
            // +0x1c0: the grid the tile-classification pass reads (w1 == 1: tile processed) and writes (w0, w2, w3); +0x41c0: the
            // grid it writes for the lighting VS
            for (int g = 0; g < 2; g++) {
                const uint32_t *t = (const uint32_t *)(uintptr_t)(st.sbuf[0][4].addr + (g ? 0x41c0 : 0x1c0));
                LOG("[tiles] buffer %llx +%llx grid +%x, words w0 w1 w2 w3 per tile (row: 25 tiles of 64 px)\n",
                    (unsigned long long)st.sbuf[0][4].addr, (unsigned long long)st.sbuf[0][4].size, g ? 0x41c0 : 0x1c0);
                for (int y = 0; y < 15; y++) {
                    char line[25 * 5 + 1], *o = line;
                    for (int x = 0; x < 25; x++, o += 5) {
                        const uint32_t *w = t + (y * 25 + x) * 4;
                        snprintf(o, 6, "%x%x%x%x ", w[0] & 15, w[1] & 15, w[2] & 15, w[3] & 15);
                    }
                    LOG("[tiles] g%d y%02d %s\n", g, y, line);
                }
            }
        }
    }
    if (g_debug_frame) debug_draw(pr, r, indexed);
    else {   // LARECOMP_DRAWLOG_FS=<fs hash hex>: the debug-frame draw line for that shader's first 8 draws (2026-10-18 hang)
        static uint64_t lh = 1;
        static int ln;
        if (lh == 1) { const char *e = getenv("LARECOMP_DRAWLOG_FS"); lh = e ? strtoull(e, NULL, 16) : 0; }
        if (lh && g_cur_fs_hash == lh && ln < 8) {
            ln++;
            debug_draw(pr, r, indexed);
            const char *cd = getenv("LARECOMP_CBDUMP");
            char path[512];
            FILE *f;
            if (ln == 1 && cd && (snprintf(path, sizeof path, "%s/draw.txt", cd), f = fopen(path, "w"))) {
                fprintf(f, "indexed %d x", indexed);
                for (int i = 0; i < 8; i++) fprintf(f, " %llx", (unsigned long long)r->x[i]);
                fprintf(f, "\nindices");
                uint32_t isz = r->x[1] == 1 ? 2 : r->x[1] == 0 ? 1 : 4;   // (x[1]: 0 = u8, 1 = u16, 2 = u32)
                for (uint32_t i = 0; indexed && r->x[3] && i < (uint32_t)r->x[2] && i < 64; i++) {
                    const uint8_t *s = (const uint8_t *)(uintptr_t)r->x[3] + (size_t)i * isz;
                    fprintf(f, " %u", isz == 1 ? *s : isz == 2 ? *(const uint16_t *)s : *(const uint32_t *)s);
                }
                fprintf(f, "\n");
                fclose(f);
                cbdump_n = 1;   // (cleared once both stages' sets are built below)
            }
        }
    }
    uint64_t pz2 = PROFD ? prof_now() : 0;
    // A depth write after draws of this pass sampled that depth attachment live: end the pass + barrier first, so their
    // texture reads finish before the depth writes start. Without it the reads and writes overlap inside one render
    // pass instance (undefined NVIDIA 617.14 hung the GPU in the decal pass).
    if (depth_live_in_pass && in_pass && st.rt.depth && st.ds[0] && st.ds[1]) {
        vk_end_pass();
        VkCommandBuffer wcb = vk_cmd();
        gp_mark(wcb, "depth write after live sampling");
        vk_full_barrier(wcb);
        pass_barrier_needed = false;
        pass_barrier_fresh = true;
        st.rt_dirty = true;
        stat_depth_war++;
    }
    resolve_feedback(pr);
    {
        ShaderInfo *const tinfos[2] = {pr->vi, pr->fi};
        static const int tstages[2] = {0, 1};
        twins_refresh(tinfos, tstages, 2, true);
    }
    uint64_t pz3 = PROFD ? prof_now() : 0;
    snap_reason = "draw-depthwrite";
    if (st.rt.depth && st.ds[0] && st.ds[1]) snapshots_invalidate(st.rt.depth);
    snap_reason = "draw-colorwrite";
    for (int i = 0; i < st.rt.ncolor; i++)
        if (st.rt.color[i] && ((st.channel_mask >> (i * 4)) & 0xF)) snapshots_invalidate(st.rt.color[i]);
    begin_pass();
    VkCommandBuffer cb = vk_cmd();
    uint64_t pz4 = PROFD ? prof_now() : 0;

    uint32_t mode = (uint32_t)r->x[0];
    VkPrimitiveTopology topo;
    uint8_t cls;
    bool native = topology(mode, &topo, &cls);
    if (!native && mode != 6 && mode != 7 && mode != 9) { stat_skipped++; stat_skip_why[3]++; static int nl; if (nl++ < 20) LOG("[skip] topology mode %u (fs hash %016llx, count %llu, rt0 %ux%u f%#x)\n", mode, (unsigned long long)g_cur_fs_hash, (unsigned long long)r->x[2], st.rt.ncolor && st.rt.color[0] ? st.rt.color[0]->width : 0, st.rt.ncolor && st.rt.color[0] ? st.rt.color[0]->height : 0, st.rt.ncolor && st.rt.color[0] ? st.rt.color[0]->format : 0); return; }
    if (cls == 1) {   // line draws: drawn 1 px wide (nvnCommandBufferSetLineWidth 2.25 / 4.5 is not executed, cmdbuf.c): do any follow it?
        static uint64_t nline;
        if (nline++ < 8 || nline % 10000 == 0)
            LOG("[vk] line draw %llu: mode %u, count %llu, fs hash %016llx, rt0 %ux%u\n", (unsigned long long)nline, mode, (unsigned long long)r->x[2],
                (unsigned long long)g_cur_fs_hash, st.rt.ncolor && st.rt.color[0] ? st.rt.color[0]->width : 0, st.rt.ncolor && st.rt.color[0] ? st.rt.color[0]->height : 0);
    }

    uint64_t prof_tk_ = PROFD ? prof_now() : 0;
    if (PROFD) {
        prof_pre += prof_tk_ - prof_tp_;
        prof_pz[0] += pz1 - prof_tp_; prof_pz[1] += pz2 - pz1; prof_pz[2] += pz3 - pz2; prof_pz[3] += pz4 - pz3; prof_pz[4] += prof_tk_ - pz4;
        uint64_t *hz = hf_pz[g_draw_newprog];
        hz[0] += pz1 - prof_tp_; hz[1] += pz2 - pz1; hz[2] += pz3 - pz2; hz[3] += pz4 - pz3; hz[4] += prof_tk_ - pz4;
        static int nslow;
        if (prof_tk_ - prof_tp_ > 20000000ull && nslow++ < 300)
            LOG("[slowdraw] frame %llu: pre %.1f ms = program %.1f (new: key %.1f claim %.1f record %.1f translate %.1f) + fs/debug %.1f "
                "+ feedback %.1f + snapshots/begin_pass %.1f + topology %.1f; vs %llx fs %llx, textures %d+%d, rt0 %ux%u f%#x\n",
                (unsigned long long)renderer_present_count(), (prof_tk_ - prof_tp_) / 1e6, (pz1 - prof_tp_) / 1e6, gps_ns[0] / 1e6,
                gps_ns[1] / 1e6, gps_ns[2] / 1e6, gps_ns[3] / 1e6, (pz2 - pz1) / 1e6, (pz3 - pz2) / 1e6, (pz4 - pz3) / 1e6,
                (prof_tk_ - pz4) / 1e6, (unsigned long long)st.program->stages[0].code_addr, (unsigned long long)st.program->stages[1].code_addr,
                pr->vi->ntex, pr->fi->ntex, st.rt.ncolor && st.rt.color[0] ? st.rt.color[0]->width : 0,
                st.rt.ncolor && st.rt.color[0] ? st.rt.color[0]->height : 0, st.rt.ncolor && st.rt.color[0] ? st.rt.color[0]->format : 0);
    }
    // ---- pipeline LARECOMP_PK_FAST: the key is a function of the program, the topology class, the pass's formats and samples and of st's vertex /
    // blend / colour / multisample / polygon / depth-clamp / target state. exec_list bumps g_pk_gen only when one of those commands changes its bytes
    // (TotK re-issues identical binds per draw), so a draw with the same program, class, pass and generation as the last one reuses its pipeline
    // without building, comparing and hashing the ~700-byte key. =2 builds the key anyway and counts draws where the fast path would have been wrong.
smark_reselect:;
    static int pk_fast = -1;
    if (pk_fast < 0) { const char *e = getenv("LARECOMP_PK_FAST"); pk_fast = e ? atoi(e) : 1; if (pk_fast) LOG("[vk] pipeline-key fast path %d\n", pk_fast); }
    static struct { Pipe *pp; Prog *pr; uint64_t gen; uint32_t mgen, ncolor, samples; uint8_t cls; VkFormat color[8], depth, stencil; VkImageAspectFlags fbl; } pkf;
    static uint64_t pkf_hits, pkf_bad, pkf_miss[4];   // (misses: program, generation, memo generation, class / pass)
    bool pkf_same = pk_fast && pkf.pp && pkf.gen == g_pk_gen && pkf.mgen == g_memo_gen && pkf.pr == pr && pkf.cls == cls &&
                    pkf.ncolor == pass_ncolor && pkf.samples == pass_samples && pkf.depth == pass_depth && pkf.stencil == pass_stencil && pkf.fbl == pass_fbl &&
                    !memcmp(pkf.color, pass_color, sizeof pkf.color);
    static struct { PipeKey k; Pipe *pp; uint32_t gen; } pk_memo;   // (k is memset first: padding compares equal)
    Pipe *pp;
    const PipeKey *kp = &pk_memo.k;   // the draw's key: pk_memo.k always holds the last one built (pkf.pp's)
    if (pkf_same && pk_fast == 1) { pp = pkf.pp; pkf_hits++; goto pipe_known; }
    if (!pkf_same && pk_fast && pkf.pp) pkf_miss[pkf.pr != pr ? 0 : pkf.gen != g_pk_gen ? 1 : pkf.mgen != g_memo_gen ? 2 : 3]++;
    PipeKey k;
    memset(&k, 0, sizeof k);
    kp = &k;
    k.prog = pr;
    k.ncolor = pass_ncolor;
    memcpy(k.color, pass_color, sizeof k.color);
    pipekey_norm(&k);
    k.depth = pass_depth;
    k.stencil = pass_stencil;
    k.samples = pass_samples;
    for (int i = 0; i < 32; i++) {
        if (!(pr->vi->attr_mask & (1u << i))) continue;
        k.attr_mask |= 1u << i;
        uint32_t a = i < st.nattrib ? st.attrib[i] : 0x2E;
        uint32_t fmt = a & 0xFF, stream = (a >> 8) & 0x1F, off = a >> 13;
        if (i >= st.nattrib || fmt == 0 || stream >= 16) {  // unbound attribute: (0, 0, 0, 1) from a constant stream
            k.attr_fmt[i] = VK_FORMAT_R32G32B32A32_SFLOAT;
            k.attr_bind[i] = ZERO_BINDING;
            k.used_streams |= 1u << ZERO_BINDING;
            continue;
        }
        k.attr_fmt[i] = vertex_format_supported(vertex_format(fmt));
        k.attr_off[i] = off;
        k.attr_bind[i] = stream;
        k.used_streams |= 1u << stream;
    }
    for (int s = 0; s < 16; s++) {
        if (!(k.used_streams & (1u << s))) continue;
        k.stride[s] = s < st.nstream ? st.stream[s][0] : 0;
        k.divisor[s] = s < st.nstream && k.stride[s] ? st.stream[s][1] : 0;
        if (k.divisor[s] > 1 && !g_vk.divisor) k.divisor[s] = 1;
    }
    for (uint32_t i = 0; i < k.ncolor && i < 8; i++) {
        if (!st.rt.color[i] || k.color[i] == VK_FORMAT_UNDEFINED) continue;
        // Targets the fragment shader doesn't output keep their contents on Metal, but are undefined in Vulkan
        // (NVIDIA writes black): mask them. Foliage depth pre-passes left black silhouettes (user 2026-10-08).
        k.write_mask[i] = ((pr->fi->out_mask >> (i * 4)) & 0xF) ? (st.channel_mask >> (i * 4)) & 0xF : 0;
        if (((st.color_state[0] >> i) & 1) && !rt_kind(rt_view_format(i))) {
            k.blend_on |= (uint8_t)(1u << i);
            memcpy(k.blend[i], st.blend[i], 8);
        }
    }
    k.a2c = st.ms[0xC] != 0 && !a2c_dither;
    k.topo_class = cls;
    k.poly_fill = st.poly[2] != 1 || !g_vk.fill_nonsolid;   // (no fillModeNonSolid: wireframe draws fill)
    static int e1_clamp = -1;
    if (e1_clamp < 0) e1_clamp = getenv("LARECOMP_FORCE_DEPTH_CLAMP") && strcmp(getenv("LARECOMP_FORCE_DEPTH_CLAMP"), "0");
    k.depth_clamp = (st.depth_clamp || e1_clamp) && g_vk.depth_clamp;
    k.fbl = (uint8_t)pass_fbl;   // (0 unless g_vk.fbl_layout)   // (no depthClamp feature: clamped draws clip)
    if (pk_memo.pp && pk_memo.gen == g_memo_gen && !memcmp(&k, &pk_memo.k, sizeof k)) pp = pk_memo.pp;
    else {
    uint64_t ph = hash_mem(0xcbf29ce484222325ull, &k, sizeof k);
    if (!ph) ph = 1;
    uint32_t pslot = (uint32_t)(ph ^ (ph >> 29)) & (PIPE_TAB - 1);
    while (pipe_tab[pslot].key && pipe_tab[pslot].key != ph) pslot = (pslot + 1) & (PIPE_TAB - 1);
    pp = pipe_tab[pslot].pipe;
    if (!pp) {
        pp = pc_take_pipe(ph);   // built (or being built) by the shader precompile
        bool fresh = !pp;
        if (fresh) { pp = calloc(1, sizeof *pp); pp->k = k; }
        pipe_tab[pslot].key = ph;
        pipe_tab[pslot].pipe = pp;
        if (fresh && gpl_linkable(&k) && hitch_fits(g_link_est)) {   // fast link here: no skip
            pp->claimed = 1;
            uint64_t t0 = SDL_GetTicksNS();
            build_pipeline_body(pp);
            hitch_note(SDL_GetTicksNS() - t0, true);
        } else if (fresh) {
            if (gpl_linkable(&k)) stat_link_deferred++;   // (TOTK_HITCH=skip, over the frame's budget)
            run_job(build_pipeline, pp, 'l', build_log() && pp->k.prog ? prog_code_hash(pp->k.prog, 1) : 0, false);
        }
        if (pr->ckey) pc_record_pipe(pr->ckey, &k);
    }
    pk_memo.k = k; pk_memo.pp = pp; pk_memo.gen = g_memo_gen;
    }
    if (pkf_same && pp != pkf.pp && pkf_bad++ < 16)
        LOG("[vk] pipeline-key fast path would have been wrong (%llu so far): program %p, gen %llu\n", (unsigned long long)pkf_bad, (void *)pr,
            (unsigned long long)g_pk_gen);
    if (pk_fast) {
        pkf.pp = pp; pkf.pr = pr; pkf.gen = g_pk_gen; pkf.mgen = g_memo_gen; pkf.cls = cls; pkf.ncolor = pass_ncolor; pkf.samples = pass_samples;
        pkf.depth = pass_depth; pkf.stencil = pass_stencil; pkf.fbl = pass_fbl; memcpy(pkf.color, pass_color, sizeof pkf.color);
        if (pkf_same) pkf_hits++;
    }
pipe_known:;
    if (rt_lookup()) gd_prefetch_draw(r, indexed, kp->used_streams);
    // (every 200k draws, at most one a minute: every 200k alone was 1.5 lines a second in play; the wrongs log apart)
    static uint64_t pkf_log, pkf_log_ns;
    if (pk_fast && pkf_hits - pkf_log >= 200000) {
        pkf_log = pkf_hits;
        const uint64_t t = SDL_GetTicksNS();
        if (!pkf_log_ns || t - pkf_log_ns >= 60000000000ull) {
            pkf_log_ns = t;
            const uint64_t *b = g_pk_bumps;
            LOG("[vk] pipeline-key fast path: %llu draws (%llu wrong); misses: program %llu, generation %llu, memo %llu, class / pass %llu; "
                "generation bumps: defaults %llu, blend %llu, channel %llu, colour %llu, multisample %llu, polygon %llu, attrib %llu, stream %llu, "
                "targets %llu, depth clamp %llu\n", (unsigned long long)pkf_hits, (unsigned long long)pkf_bad, (unsigned long long)pkf_miss[0],
                (unsigned long long)pkf_miss[1], (unsigned long long)pkf_miss[2], (unsigned long long)pkf_miss[3], (unsigned long long)b[PKB_DEFAULTS],
                (unsigned long long)b[PKB_BLEND], (unsigned long long)b[PKB_CHANNEL], (unsigned long long)b[PKB_COLOR], (unsigned long long)b[PKB_MS],
                (unsigned long long)b[PKB_POLY], (unsigned long long)b[PKB_ATTRIB], (unsigned long long)b[PKB_STREAM], (unsigned long long)b[PKB_RT],
                (unsigned long long)b[PKB_CLAMP]);
        }
    }
    // A draw whose pipeline is still being built is skipped; device.c then does not show that frame (a skipped final pass
    // made whole frames black). LARECOMP_PIPE_INLINE=1: the draw builds the pipeline itself or waits for the worker (<=
    // 100 ms) instead; that stalled 1.9-2.7 s on bursts of new pipelines. "[vk] pipeline waits" in the stats line.
    static int pipe_inline = -1;
    if (pipe_inline < 0) pipe_inline = getenv("LARECOMP_PIPE_INLINE") != NULL;
    // A queued pipeline no worker has claimed yet whose program's libraries are ready (the precompile's, or one queued over
    // TOTK_HITCH=skip's budget) is fast-linked here as well (~1 ms) instead of skipping its draw and holding the frame. One
    // a worker is building is waited for only by bakes and forced frames. LARECOMP_LINK_CLAIM=0: no claim here (old). Per
    // presented frame the claimed links stop at LARECOMP_LINK_CLAIM_MS (default 4) of link time: past about a frame's time
    // one held frame is cheaper than linking serially what the workers link in parallel.
    static int link_claim = -1;
    static uint64_t claim_budget, claim_f, claim_ns;
    if (link_claim < 0) {
        const char *e = getenv("LARECOMP_LINK_CLAIM"), *m = getenv("LARECOMP_LINK_CLAIM_MS");
        link_claim = !e || strcmp(e, "0");
        claim_budget = (uint64_t)((m ? atof(m) : 4.0) * 1e6);
    }
    if (claim_f != renderer_present_count()) { claim_f = renderer_present_count(); claim_ns = 0; }
    bool force = pipe_inline || g_draw_bake || g_hold_force;
    bool dead = force && bake_is_dead(pp);
    if (atomic_load(&pp->state) == 0 && !dead &&
        (force || (link_claim && claim_ns < claim_budget && !hitch_skip() && gpl_linkable(&pp->k)))) {
        uint64_t t0 = SDL_GetTicksNS();
        int unclaimed = 0;
        if (atomic_compare_exchange_strong(&pp->claimed, &unclaimed, 1)) {
            build_pipeline_body(pp);
            if (force) prof_pipe_inline++;
            else { stat_link_claimed++; claim_ns += SDL_GetTicksNS() - t0; hitch_note(SDL_GetTicksNS() - t0, true); }
        } else if (force) {
            uint64_t limit = g_draw_bake || g_hold_force ? 3000000000ull : 100000000ull;
            while (atomic_load(&pp->state) == 0 && SDL_GetTicksNS() - t0 < limit) SDL_Delay(1);
            prof_pipe_waited++;
            if (atomic_load(&pp->state) == 0 && limit > 100000000ull)
                bake_timeout("pipeline", pp->k.prog ? pp->k.prog->code[1] : 0, pp);
        }
        if (force) prof_pipe_wait_ns += SDL_GetTicksNS() - t0;
        if (g_hold_force) g_hold_force_wait_ns += SDL_GetTicksNS() - t0;
    }
    // (g_skip_full_frame: the skipped draw has <= 6 vertices = a fullscreen / tile pass; device.c always holds those)
    // "[pipeskip]" (first 300): why a skipped pipeline was not fast-linked, then how long its draws were missing. Frames with
    // <= LARECOMP_HOLD_MIN such skips are shown (user's free play 2026-10-20 07:31: 39 shown frames, "buildings disappear").
    static int ps_log;
    if (atomic_load(&pp->state) != 1 && smark_variant) {   // (LARECOMP_SNAP_MARK: no skip for it)
        smark_variant = false;
        Prog *p0 = get_program(st.program, &vkey, &smark_fk0);
        if (p0 && atomic_load(&p0->state) == 1) { pr = p0; goto smark_reselect; }
    }
    if (atomic_load(&pp->state) != 1) {
        stat_skipped++; stat_skip_why[4]++;
        if (atomic_load(&pp->state) == 0) {
            g_skip_pipe_frame++; g_skip_full_frame += r->x[2] <= 6;
            if (r->x[2] <= 6 && hurry_on() && pp->hurry_f != renderer_present_count()) {
                pp->hurry_f = renderer_present_count();
                job_hurry(pp, "pipeline fs", build_log() && pp->k.prog ? prog_code_hash(pp->k.prog, 1) : 0);
            }
            if (!pp->nskip++ && ps_log < 300) {
                ps_log++;
                pp->skip_t0 = SDL_GetTicksNS();
                const Prog *pg = pp->k.prog;
                LOG("[pipeskip] frame %llu fs %llx: libs %d, depth clamp %d, line fill %d, samples %d, a2c %d, verts %llu\n",
                    (unsigned long long)renderer_present_count(), pg ? (unsigned long long)pg->code[1] : 0ull,
                    pg ? atomic_load(&pg->lib_state) : -1, pp->k.depth_clamp, !pp->k.poly_fill, (int)pp->k.samples, pp->k.a2c,
                    (unsigned long long)r->x[2]);
            }
        }
        return;
    }
    if (pp->skip_t0) {
        LOG("[pipeskip] frame %llu fs %llx ready after %.1f ms, %u draws skipped\n", (unsigned long long)renderer_present_count(),
            pp->k.prog ? (unsigned long long)pp->k.prog->code[1] : 0ull, (SDL_GetTicksNS() - pp->skip_t0) / 1e6, pp->nskip);
        pp->skip_t0 = 0;
    }
    // Same pipeline as the one bound last in this command buffer: no rebind. Tracked with the dynamic-state cache
    // (dyn_begin keys it).
    dyn_begin(cb);
    if (dyn.pipe != pp->pipe) {
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pp->pipe);
        dyn.pipe = pp->pipe;
        if (!pp->linked) dyn.clamp = -1;   // (a static clamp leaves the dynamic value undefined)
        prof_pipe_bind++;
    } else prof_pipe_skip++;
    uint64_t prof_td_ = PROFD ? prof_now() : 0;
    if (PROFD) prof_key += prof_td_ - prof_tk_;

    // ---- depth / stencil (dynamic)
    static int nodt = -1;
    if (nodt < 0) nodt = getenv("LARECOMP_NO_DEPTH_TEST") != NULL;
    static uint64_t nodt_hash = 1;   // LARECOMP_NO_DEPTH_FS=<fs hash>: depth test off for that fragment shader only (A/B)
    if (nodt_hash == 1) { const char *e = getenv("LARECOMP_NO_DEPTH_FS"); nodt_hash = e ? strtoull(e, NULL, 16) : 0; }
    bool dt = st.ds[0] && !nodt && pass_depth && !flame_nodepth && !(nodt_hash && g_cur_fs_hash == nodt_hash);
    bool dv = dyn_begin(cb);
#define DYN_SET(cond, assign, call) do { if (!dv || (cond)) { assign; call; prof_dyn_set++; } else prof_dyn_skip++; } while (0)
    bool dw = dt && st.ds[1];
    DYN_SET(dyn.dt != dt, dyn.dt = dt, vkCmdSetDepthTestEnable(cb, dt));
    DYN_SET(dyn.dw != dw, dyn.dw = dw, vkCmdSetDepthWriteEnable(cb, dw));
    if (vk_dyn_clamp && pp->linked) {   // (same signature as the feedback-loop setter: queued in order through q_set_fbl)
        static PFN_vkCmdSetDepthClampEnableEXT set_clamp;
        if (!set_clamp) set_clamp = (PFN_vkCmdSetDepthClampEnableEXT)vkGetDeviceProcAddr(g_vk.dev, "vkCmdSetDepthClampEnableEXT");
        int8_t c = pp->k.depth_clamp;
        if (set_clamp) DYN_SET(dyn.clamp != c, dyn.clamp = c, q_set_fbl((PFN_vkCmdSetAttachmentFeedbackLoopEnableEXT)set_clamp, cb, c));
    }
    if (g_vk.feedback_loop) {   // (declares this draw's live attachment sampling: device.c / begin_pass fb_loop_info)
        static PFN_vkCmdSetAttachmentFeedbackLoopEnableEXT set_fbl;
        if (!set_fbl) set_fbl = (PFN_vkCmdSetAttachmentFeedbackLoopEnableEXT)vkGetDeviceProcAddr(g_vk.dev, "vkCmdSetAttachmentFeedbackLoopEnableEXT");
        if (set_fbl) DYN_SET(dyn.fbl != fb_live_aspects, dyn.fbl = fb_live_aspects, q_set_fbl(set_fbl, cb, fb_live_aspects));
    }
    VkCompareOp dop = dt ? compare_fn(st.ds[2]) : VK_COMPARE_OP_ALWAYS;
    // EQUAL against a depth pre-pass written by a different vertex shader (foliage, trees) needs both shaders to land
    // on identical depths. They do since the translator stopped using unpackHalf2x16 (NVIDIA ran the half math after
    // it in fp16, differently per shader: black slashes in foliage). LARECOMP_VK_EQUAL_TOL=1 brings back the old
    // workaround: LESS_OR_EQUAL with a few ulps of bias towards the camera, which also let walls paint over the
    // plants stuck flat on them (beach) - so it is off.
    static int tol = -1;
    if (tol < 0) tol = getenv("LARECOMP_VK_EQUAL_TOL") != NULL;
    bool equal_tol = dop == VK_COMPARE_OP_EQUAL && tol;
    if (equal_tol) dop = VK_COMPARE_OP_LESS_OR_EQUAL;
    DYN_SET(dyn.dop != dop, dyn.dop = dop, vkCmdSetDepthCompareOp(cb, dop));
    probe_dt = dt; probe_dw = dw; probe_dop = dop;
    bool sten = st.ds[3] && pass_stencil;
    DYN_SET(dyn.sten != sten, dyn.sten = sten, vkCmdSetStencilTestEnable(cb, sten));
    if (sten) {
        uint32_t ops = st.ds[5] | st.ds[6] << 8 | st.ds[7] << 16;
        bool sv = dv && dyn.sten_valid;
        for (int f = 0; f < 2; f++) {
            uint32_t o = f ? ops >> 12 : ops;
            VkStencilFaceFlags face = f ? VK_STENCIL_FACE_BACK_BIT : VK_STENCIL_FACE_FRONT_BIT;
            uint32_t sop = (o & 0xFFF) | (uint32_t)(f ? st.ds[4] >> 4 : st.ds[4] & 0xF) << 12;
            uint32_t cm = st.stencil_vmask[f] & 0xFF, wm = st.stencil_mask[f] & 0xFF, ref = st.stencil_ref[f];
            if (!sv || dyn.sop[f] != sop) {
                vkCmdSetStencilOp(cb, face, stencil_op(o & 0xF), stencil_op((o >> 8) & 0xF), stencil_op((o >> 4) & 0xF),
                                  compare_fn(f ? st.ds[4] >> 4 : st.ds[4] & 0xF));
                dyn.sop[f] = sop;
            }
            if (!sv || dyn.cmask[f] != cm) { vkCmdSetStencilCompareMask(cb, face, cm); dyn.cmask[f] = cm; }
            if (!sv || dyn.wmask[f] != wm) { vkCmdSetStencilWriteMask(cb, face, wm); dyn.wmask[f] = wm; }
            if (!sv || dyn.ref[f] != ref) { vkCmdSetStencilReference(cb, face, ref); dyn.ref[f] = ref; }
        }
        dyn.sten_valid = true;
    }
    DYN_SET(memcmp(dyn.blend, st.blend_color, sizeof dyn.blend), memcpy(dyn.blend, st.blend_color, sizeof dyn.blend),
            vkCmdSetBlendConstants(cb, st.blend_color));

    // ---- rasterizer
    uint8_t cull = st.poly[0];
    VkCullModeFlags cm = cull == 1 ? VK_CULL_MODE_FRONT_BIT : cull == 2 ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
    DYN_SET(dyn.cull != cm, dyn.cull = cm, vkCmdSetCullMode(cb, cm));
    bool ccw = st.poly[1] == 1;
    static int flipw = -1;  // LARECOMP_VK_FLIP_WINDING: invert the front-face mapping (A/B against Metal)
    if (flipw < 0) flipw = getenv("LARECOMP_VK_FLIP_WINDING") != NULL;
    bool front_ccw = (ccw != vkey.flip_y) != (flipw != 0);
    VkFrontFace ff = front_ccw ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
    DYN_SET(dyn.front != ff, dyn.front = ff, vkCmdSetFrontFace(cb, ff));
    VkPrimitiveTopology tp = native ? topo : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    DYN_SET(dyn.topo != tp, dyn.topo = tp, vkCmdSetPrimitiveTopology(cb, tp));
    bool restart = st.prim_restart && native && (mode == 3 || mode == 5);
    DYN_SET(dyn.restart != restart, dyn.restart = restart, vkCmdSetPrimitiveRestartEnable(cb, restart));
    {
        float f[3] = {0, 0, 0};
        static int off = -1;
        if (off < 0) off = getenv("LARECOMP_NO_DEPTH_BIAS") != NULL;
        bool on = (st.poly[3] & 4) && !off;
        if (on) memcpy(f, st.poly_offset, sizeof f);
        if (equal_tol) { f[1] -= 8; on = true; }  // (see the EQUAL comment above: a few depth ulps nearer)
        DYN_SET(dyn.bias_on != on, dyn.bias_on = on, vkCmdSetDepthBiasEnable(cb, on));
        DYN_SET(memcmp(dyn.bias, f, sizeof f), memcpy(dyn.bias, f, sizeof f), vkCmdSetDepthBias(cb, f[1], f[2], f[0]));
    }
#undef DYN_SET
    dyn_end();

    // ---- textures first (their scales feed the widescreen decision and the rscale table), then the viewport
    float *vp = st.viewport[0];
    float zn = st.depth_range[0][0], zf = st.depth_range[0][1];
    vp_sx = pass_sx; vp_sy = pass_sy; vp_ox = 0; vp_oy = 0;
    {   // the Items page's cursor brackets while the cursor is on the companion card (runtime/hle/pets.c)
        extern bool pets_hide_item_cursor(void);
        extern bool texpatch_is_item_cursor(int id);
        if (pets_hide_item_cursor())
            for (int i = 0; i < pr->fi->ntex; i++) {
                NvnTexture *t = shader_texture_object(1, &pr->fi->tex[i]);
                const TexBackend *b = t ? (const TexBackend *)t->backend : NULL;
                if (b && b->tp > 0 && texpatch_is_item_cursor(b->tp - 1)) { stat_skipped++; stat_skip_why[5]++; return; }
            }
    }
    float tint[8] = {0};
    if (pr->fi->uses_tint) {
        extern bool pot_tint(const float *m, float tint[8], uint64_t mesh);
        extern bool pot_glow_active(void);
        if (st.ubuf[0][2].addr && st.ubuf[0][2].size >= 48 && pot_glow_active())
            pot_tint((const float *)(uintptr_t)st.ubuf[0][2].addr, tint, st.vbuf[0].addr);
        // the gold Escape Bottle (runtime/hle/escape.c): draws sampling the bottle's item icon / model map
        // and the HUD hearts (heart colour, Cosmetics tab)
        extern int texpatch_bottle_kind(int id);
        extern int texpatch_heart_kind(int id);
        extern void texpatch_heart_tint(int kind, float tint[8]);
        int bottle = 0, heart = 0;
        for (int i = 0; i < pr->fi->ntex; i++) {
            NvnTexture *t = shader_texture_object(1, &pr->fi->tex[i]);
            const TexBackend *b = t ? (const TexBackend *)t->backend : NULL;
            if (b && b->tp > 0 && !bottle) bottle = texpatch_bottle_kind(b->tp - 1);
            if (b && b->tp > 0 && !heart) heart = texpatch_heart_kind(b->tp - 1);
        }
        if (heart) texpatch_heart_tint(heart, tint);
        if (bottle) {
            extern void escape_bottle_tint(int kind, const float *ui, uint32_t ui_size, float tint[8]);
            int u = bottle == 3 ? 3 : 0;  // (the fairy model: its bone matrices, for where it stands - souls.c)
            escape_bottle_tint(bottle, (const float *)(uintptr_t)st.ubuf[0][u].addr, (uint32_t)st.ubuf[0][u].size, tint);
        }
    }
    // fragment textures are bound before deciding the viewport (tex_sc / tex_wh), rscale depends on the decision:
    // build the vertex set now and the fragment set after the decision (stage_set fills tex_sc for stage 1).
    if (PROFD) prof_dyn += prof_now() - prof_td_;
    PROF_T0D();
    uint32_t dyn[2 * MAX_BIND], ndyn = 0;
    PROF_ADDD(prof_sets);
    VkDescriptorImageInfo fii[64];
    uint64_t prof_tx_ = PROFD ? prof_now() : 0;
    pk_draw(pr);
    // LARECOMP_FS_TEXCACHE: the fragment textures resolve to the same descriptors as the last draw's when nothing they depend on changed: same shader info,
    // fragment binding generation (texture handles, pools, program: g_bind_gen), frame, view and pool generations and alias epoch, and no feedback or pets
    // substitution in either draw. Then texture_info (handle decode, view memo hash, sampler) is not run per texture. =2 resolves anyway and logs
    // mismatches.
    static int fs_tc = -1;
    if (fs_tc < 0) { const char *e = getenv("LARECOMP_FS_TEXCACHE"); fs_tc = e ? atoi(e) : 1; if (fs_tc) LOG("[vk] fragment-texture cache %d\n", fs_tc); }
    static struct { ShaderInfo *fi; uint64_t bgen, frame, epoch; uint32_t view_gen, pool_gen; int n; bool valid;
                    VkDescriptorImageInfo ii[64]; float sc[64][3]; uint32_t wh[64][2]; } fstc;
    static uint64_t fstc_hits, fstc_bad, fstc_ver = 1;   // (ver: bumped at every refill of fstc.ii, never 0)
    extern uint32_t vk_view_gen, g_pool_gen;
    extern uint64_t vk_frame_counter(void);
    int fs_n = pr->fi->ntex < 64 ? pr->fi->ntex : 64;
    bool fs_same = fs_tc && fstc.valid && !nfeedback && !pk_subst_tex && fstc.fi == pr->fi && fstc.n == fs_n && fstc.bgen == g_bind_gen[1] &&
                   fstc.frame == vk_frame_counter() && fstc.view_gen == memo_gen() && fstc.pool_gen == __atomic_load_n(&g_pool_gen, __ATOMIC_RELAXED) &&
                   fstc.epoch == (in_execute ? alias_epoch : 0);
    if (fs_same && fs_tc == 1) {
        memcpy(fii, fstc.ii, (size_t)fs_n * sizeof *fii);
        for (int i = 0; i < fs_n; i++) { tex_sc[1][i][0] = fstc.sc[i][0]; tex_sc[1][i][1] = fstc.sc[i][1]; tex_sc[1][i][2] = fstc.sc[i][2]; tex_wh[1][i][0] = fstc.wh[i][0]; tex_wh[1][i][1] = fstc.wh[i][1]; }
        fstc_hits++;
    } else {
        for (int i = 0; i < pr->fi->ntex && i < 64; i++) {
            float sc[3] = {1, 1, 0}; uint32_t wh[2];
            texture_info(1, pr->fi, i, &fii[i], sc, wh);
            tex_sc[1][i][0] = sc[0]; tex_sc[1][i][1] = sc[1]; tex_sc[1][i][2] = sc[2]; tex_wh[1][i][0] = wh[0]; tex_wh[1][i][1] = wh[1];
        }
        if (fs_tc) {
            bool bad = false;
            if (fs_same) {
                bad = memcmp(fii, fstc.ii, (size_t)fs_n * sizeof *fii) != 0;
                for (int i = 0; i < fs_n && !bad; i++)
                    bad = tex_sc[1][i][0] != fstc.sc[i][0] || tex_sc[1][i][1] != fstc.sc[i][1] || tex_sc[1][i][2] != fstc.sc[i][2] || tex_wh[1][i][0] != fstc.wh[i][0] || tex_wh[1][i][1] != fstc.wh[i][1];
                fstc_hits++;
                if (bad && fstc_bad++ < 16) LOG("[vk] fragment-texture cache would have been wrong (%llu so far)\n", (unsigned long long)fstc_bad);
            }
            fstc.valid = !nfeedback && !pk_subst_tex;
            fstc.fi = pr->fi; fstc.n = fs_n; fstc.bgen = g_bind_gen[1]; fstc.frame = vk_frame_counter(); fstc.view_gen = memo_gen();
            fstc.pool_gen = __atomic_load_n(&g_pool_gen, __ATOMIC_RELAXED); fstc.epoch = in_execute ? alias_epoch : 0;
            memcpy(fstc.ii, fii, (size_t)fs_n * sizeof *fii);
            fstc_ver++;
            for (int i = 0; i < fs_n; i++) { fstc.sc[i][0] = tex_sc[1][i][0]; fstc.sc[i][1] = tex_sc[1][i][1]; fstc.sc[i][2] = tex_sc[1][i][2]; fstc.wh[i][0] = tex_wh[1][i][0]; fstc.wh[i][1] = tex_wh[1][i][1]; }
        }
    }
    g_fs_pre_ver = fs_tc ? fstc_ver : 0;   // (with the cache on, fii[0..fs_n) is fstc.ii at this version: stage_set's pre_ii below)
    static uint64_t fstc_log, fstc_log_ns;   // (as the pipeline-key line: every 200k draws, at most one a minute)
    if (fs_tc && fstc_hits - fstc_log >= 200000) {
        fstc_log = fstc_hits;
        const uint64_t t = SDL_GetTicksNS();
        if (!fstc_log_ns || t - fstc_log_ns >= 60000000000ull) { fstc_log_ns = t; LOG("[vk] fragment-texture cache: %llu draws (%llu wrong)\n", (unsigned long long)fstc_hits, (unsigned long long)fstc_bad); }
    }
    pk_subst_tex = NULL;
    if (PROFD) prof_tex += prof_now() - prof_tx_;
    if (fabsf(pass_sx - pass_sy) > 0.01f * pass_sy && st.rt.ncolor && renderer_texture_presented(st.rt.color[0])) {
        // Draws into the swapchain image (see renderer.m): composites fill the window, UI goes in a centred 16:9 box.
        bool reads_scene = false;
        for (int i = 0; i < pr->fi->ntex && i < 64; i++)
            if ((tex_sc[1][i][0] != 1 || tex_sc[1][i][1] != 1) && tex_wh[1][i][0] == st.rt.color[0]->width &&
                tex_wh[1][i][1] == st.rt.color[0]->height)
                reads_scene = true;
        bool fullscreen_quad = r->x[2] <= 6 && st.viewport[0][0] == 0 && st.viewport[0][1] == 0 &&
                               st.viewport[0][2] >= st.rt.color[0]->width && st.viewport[0][3] >= st.rt.color[0]->height;
        bool fullscreen_fill = pr->fi->ntex == 0 && fullscreen_quad;
        if (fullscreen_quad && !reads_scene && pr->fi->ntex == 1 && (tex_sc[1][0][0] != 1 || tex_sc[1][0][1] != 1)) reads_scene = true;
        static uint64_t wide_frame = ~0ull;
        if (reads_scene) wide_frame = renderer_present_count();
        bool fade_fill = false;
        if (fullscreen_fill && st.ubuf[1][0].addr && st.ubuf[1][0].size >= 12) {
            const float *c = (const float *)(uintptr_t)st.ubuf[1][0].addr;
            bool black = c[0] < 0.01f && c[1] < 0.01f && c[2] < 0.01f, white = c[0] > 0.99f && c[1] > 0.99f && c[2] > 0.99f;
            fade_fill = black || white;
        }
        bool composite = reads_scene || fade_fill || (fullscreen_fill && wide_frame == renderer_present_count());
        if (!composite) wide_frame = ~0ull;
        if (!composite) {
            int pw, ph;
            pass_size(&pw, &ph);
            float u = fminf(pass_sx, pass_sy);
            vp_sx = vp_sy = u;
            vp_ox = (pw - st.rt.color[0]->width * u) * 0.5f;
            vp_oy = (ph - st.rt.color[0]->height * u) * 0.5f;
        }
    }
    bool snapped = false;   // (the viewport below was rounded out to whole pixels: rscale[0] follows it)
    float snap_rs[4];
    float vs_row0[4];       // (vp_exact: the vertex stage's rscale[0], clip-space map from the exact viewport into the snapped one)
    bool vs_exact = false;
    VkRect2D pe_inner = {{0, 0}, {0, 0}}, pe_strip[4];   // (vp_exact, <= 6 vertices: inner pixels / partial edge strips)
    int pe_n = -1;          // (-1: no edge split for this draw)
    {   // camera-attached effects sized for 16:9 are widened with the view (runtime/hle/widescreen.c)
        extern float widescreen_effect_stretch(uint64_t fs_hash);
        float k = widescreen_effect_stretch(g_cur_fs_hash);
        float vx = vp[0] * vp_sx + vp_ox, vw = vp[2] * vp_sx;
        if (k > 1.0f) { vx -= vw * (k - 1) / 2; vw *= k; }
        float vy = vp[1] * vp_sy + vp_oy, vh = vp[3] * vp_sy;
        bool jittered = false;
        if (g_dlss_depth && st.rt.depth == g_dlss_depth) {   // TOTK_DLSS=sr: the scene's geometry drawn with the frame's jitter
            float jx, jy;
            TexBackend *db = st.rt.depth->backend;
            if (db && vk_ngx_sr_frame_jitter(renderer_present_count(), db->width, 0, &jx, &jy)) { vx += jx; vy += jy; jittered = true; }
        }
        // A scaled viewport whose edge falls inside a pixel leaves that pixel unwritten when its centre is outside: the
        // last row / column of a guest rectangle that does not map to whole pixels. The scissor already rounds outward
        // (scissor_rect); the viewport now does too: its edges are moved out to whole pixels, so every pixel the guest
        // rectangle touches is drawn (the content stretched by less than a pixel). Integer scales (1.0 included) and
        // whole-pixel edges are unchanged; a jittered viewport (DLSS) and a widened one are left as they are.
        // LARECOMP_VP_SNAP=0: the old mapping.
        static int vp_snap = -1;
        if (vp_snap < 0) { const char *e = getenv("LARECOMP_VP_SNAP"); vp_snap = !e || strcmp(e, "0"); }
        if (vp_snap && !jittered && k <= 1.0f && (vp_sx != 1 || vp_sy != 1)) {
            const float ex = vx, ey = vy, ew = vw, eh = vh;   // (the exact viewport, before the snap)
            const float x1 = vx + vw, y1 = vy + vh;
            const float fx0 = floorf(vx + 1e-3f), fy0 = floorf(vy + 1e-3f), cx1 = ceilf(x1 - 1e-3f), cy1 = ceilf(y1 - 1e-3f);
            if (vw > 0 && cx1 > fx0) { vx = fx0; vw = cx1 - fx0; }
            if (vh > 0 && cy1 > fy0) { vy = fy0; vh = cy1 - fy0; }
            // (the shaders' fragcoord -> guest mapping, rscale[0], follows the snapped viewport: reconstruction from
            // gl_FragCoord next to depth drawn with the snap stays exact)
            if (vp[2] > 0 && vp[3] > 0) {
                snap_rs[0] = vw / vp[2]; snap_rs[1] = vh / vp[3];
                snap_rs[2] = vx - vp[0] * snap_rs[0]; snap_rs[3] = vy - vp[1] * snap_rs[1];
                snapped = true;
            }
            // The snap stretched the content by up to a pixel per pass, and passes sample each other with guest UVs, so the
            // stretch compounded along post chains: the stair-stepped dotted rims of the scale0 class (gate1x) in every DRS
            // frame. With vp_exact (default; LARECOMP_VP_EXACT=0 = the stretched mapping) the snapped rectangle stays
            // (coverage: a full-screen triangle still reaches the last partial pixel, clamp-to-edge content), but the vertex
            // stage maps clip space back onto the EXACT viewport (translate.c's VS epilogue: xy = xy * rscale[0].xy +
            // rscale[0].zw * w), so geometry lands where the exact scale puts it and rscale[0] stays exact. Per axis, viewport
            // origin o and extent e (Vulkan: host = o + (n + 1)
            // * e / 2; y has o = y + h, e = -h): n' = k n + b, k = e_exact / e_snap, b = k - 1 + 2 (o_exact - o_snap) / e_snap.
            // Coverage of the partial edge pixels (the band: an unwritten last row under an additive bloom chain). Geometry (>
            // 6 vertices) runs past the viewport, so the snapped rectangle's partial pixels get its exact content. A
            // full-screen primitive (<= 6 vertices: a triangle reaching NDC 3 on its +1 sides, a quad ending at exactly +-1)
            // ends on the exact edge on some sides, where those pixels' centres lie outside it. So such a draw is split: its
            // inner pixels (centre inside the exact rectangle) get the exact map, and the partial edge strips get the old
            // stretched map (VS identity, fragment rscale[0] = snap_rs), one more draw per strip, scissored. Each pixel is
            // drawn once whatever the primitive's shape, so additive passes do not double up, and the strips hold exactly what
            // they held before vp_exact.
            const uint32_t nvert = (uint32_t)r->x[2];
            if (snapped && vk_vp_exact()) {
                vs_row0[0] = ew / vw;
                vs_row0[2] = vs_row0[0] - 1.0f + 2.0f * (ex - vx) / vw;
                vs_row0[1] = eh / vh;
                vs_row0[3] = vs_row0[1] - 1.0f - 2.0f * ((ey + eh) - (vy + vh)) / vh;
                vs_exact = true;
                const uint32_t ninst = indexed ? ((uint32_t)r->x[6] ? (uint32_t)r->x[6] : 1) : ((uint32_t)r->x[4] ? (uint32_t)r->x[4] : 1);
                // (instanced small draws = particles: not full-screen, no split; a VS that stores to memory would repeat its
                // stores in each strip: no split either)
                if (nvert <= 6 && ninst == 1 && !pr->vi->writes_gmem) {
                    // inner pixels: centre p + 0.5 inside [e0, e1) per axis; snapped rectangle [s0, s1)
                    const int sx0 = (int)vx, sy0 = (int)vy, sx1 = (int)(vx + vw), sy1 = (int)(vy + vh);
                    int ix0 = (int)ceilf(ex - 0.5f), iy0 = (int)ceilf(ey - 0.5f);
                    int ix1 = (int)floorf(ex + ew - 0.5f) + 1, iy1 = (int)floorf(ey + eh - 0.5f) + 1;
                    if (ix0 < sx0) ix0 = sx0;
                    if (iy0 < sy0) iy0 = sy0;
                    if (ix1 > sx1) ix1 = sx1;
                    if (iy1 > sy1) iy1 = sy1;
                    if (ix1 > ix0 && iy1 > iy0) {
                        pe_inner = (VkRect2D){{ix0, iy0}, {(uint32_t)(ix1 - ix0), (uint32_t)(iy1 - iy0)}};
                        pe_n = 0;   // rows take the full snapped width, columns the inner height: no pixel twice
                        if (iy0 > sy0) pe_strip[pe_n++] = (VkRect2D){{sx0, sy0}, {(uint32_t)(sx1 - sx0), (uint32_t)(iy0 - sy0)}};
                        if (sy1 > iy1) pe_strip[pe_n++] = (VkRect2D){{sx0, iy1}, {(uint32_t)(sx1 - sx0), (uint32_t)(sy1 - iy1)}};
                        if (ix0 > sx0) pe_strip[pe_n++] = (VkRect2D){{sx0, iy0}, {(uint32_t)(ix0 - sx0), (uint32_t)(iy1 - iy0)}};
                        if (sx1 > ix1) pe_strip[pe_n++] = (VkRect2D){{ix1, iy0}, {(uint32_t)(sx1 - ix1), (uint32_t)(iy1 - iy0)}};
                        if (!pe_n) pe_n = -1;   // (every edge whole after all: nothing to split)
                    } else vs_exact = false;   // (a sub-pixel rectangle: the old mapping)
                }
            }
        }
        set_viewport(vx, vy, vw, vh, zn, zf);
        if (g_debug_frame) {
            VkRect2D sr = scissor_rect();
            LOG("[draw]   vp %.2f %.2f %.2f %.2f (stretch %.3f, fs hash %016llx) scissor %d %d %u %u | guest sc %d %d %d %d | pass %ux%u s %.4f %.4f\n",
                vx, vp[1] * vp_sy + vp_oy, vw, vp[3] * vp_sy, k, (unsigned long long)g_cur_fs_hash, sr.offset.x, sr.offset.y,
                sr.extent.width, sr.extent.height, st.scissor[0][0], st.scissor[0][1], st.scissor[0][2], st.scissor[0][3], pass_w, pass_h,
                vp_sx, vp_sy);
        }
    }
    apply_scissor();
    VkRect2D pe_sc = scissor_rect();   // (vp_exact split: the game's scissor, which every part is clipped to)
    for (int k = 0; k < pd_n; k++) if (pd_b[k]->snap_tag) snap_box_add(pd_b[k], pe_sc);   // (LARECOMP_SNAP_DELTA)
    if (!pr->fi->uses_smark && pd_n) {   // (LARECOMP_SNAP_MARK: a write it did not mark; its variant from the next sight on)
        for (int k = 0; k < pd_n; k++) pd_b[k]->smark_ok = false;
        // (LARECOMP_SNAP_MARK_LEARN=1: every writer of a mirrored image gets the variant too: snapshots 1.25 -> 2.3-3.6 ms and the
        // draws +1-6 ms (storage stores in the G-buffer fill shaders, the frame's marks near full), so off: only the self-write shaders
        // mark, 1.25 -> 0.83 ms)
        static int learn = -1;
        if (learn < 0) { const char *e = getenv("LARECOMP_SNAP_MARK_LEARN"); learn = e && *e == '1'; }
        if (learn && smark_on() && smark_nfs < 256 && !smark_fs_has(pr->code[1])) smark_fs[smark_nfs++] = pr->code[1];
    }
    {   // (a draw with image stores writes images outside its pass: no mirror holds after it)
        static Prog *ilast;
        static bool iimg;
        if (pr != ilast) {
            ilast = pr; iimg = false;
            for (int k = 0; k < pr->vi->ntex; k++) iimg |= pr->vi->tex[k].is_image;
            for (int k = 0; k < pr->fi->ntex; k++) iimg |= pr->fi->tex[k].is_image;
        }
        if (iimg) snap_ndgen++, snap_nd_why = "image-store draw";
    }
    if (pe_n > 0) {   // the main draw: inner pixels only
        VkRect2D in = pe_inner;
        int x0 = in.offset.x > pe_sc.offset.x ? in.offset.x : pe_sc.offset.x, y0 = in.offset.y > pe_sc.offset.y ? in.offset.y : pe_sc.offset.y;
        int x1 = (int)(in.offset.x + in.extent.width), y1 = (int)(in.offset.y + in.extent.height);
        int cx1 = (int)(pe_sc.offset.x + pe_sc.extent.width), cy1 = (int)(pe_sc.offset.y + pe_sc.extent.height);
        if (x1 > cx1) x1 = cx1;
        if (y1 > cy1) y1 = cy1;
        VkRect2D isc = x1 > x0 && y1 > y0 ? (VkRect2D){{x0, y0}, {(uint32_t)(x1 - x0), (uint32_t)(y1 - y0)}} : (VkRect2D){{0, 0}, {0, 0}};
        VkCommandBuffer scb = vk_cmd();
        vsc_same_cb(scb);
        vkCmdSetScissor(scb, 0, 1, &isc);
        vsc.sc = isc; vsc.sc_ok = true;
    }
    float rs0[4] = {vp_sx, vp_sy, vp_ox, vp_oy};
    if (snapped && !vs_exact) memcpy(rs0, snap_rs, sizeof rs0);
    uint64_t prof_t1_ = PROFD ? prof_now() : 0;
    // (built after the viewport decision: its rscale[0] is the vp_exact map; still before the fragment set, dyn's order)
    VkDescriptorSet vset = stage_set(pr, pr->set_layout[0], 0, 0, pr->vi, vs_exact ? vs_row0 : NULL, NULL, 0, 0, 0, NULL, dyn, &ndyn);
    uint32_t ndyn_v = ndyn;
    VkDescriptorSet fset = stage_set(pr, pr->set_layout[1], 1, 1, pr->fi, rs0, tint, 0, 0, 0, fii, dyn, &ndyn);
    cbdump_n = 0;
    if (PROFD) prof_sets += prof_now() - prof_t1_;
    VkDescriptorSet sets[2] = {vset, fset};
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pr->layout, 0, 2, sets, ndyn, dyn);

    uint64_t prof_t2_ = PROFD ? prof_now() : 0;
    // ---- indices (copied; the largest index bounds the vertex data copied below)
    uint32_t first = 0, count = (uint32_t)r->x[2], inst, base_inst, max_index = 0;
    int32_t base_vertex = 0;
    VkSpan ib = {0};
    VkIndexType itype_vk = VK_INDEX_TYPE_UINT32;
    uint32_t icount = 0;
    bool use_index = indexed || !native;
    const void *scan_src = NULL;  // deferred index scan (direct index buffer)
    uint32_t scan_isz = 0, scan_restart = 0;
    vk_up_kind = 3;
    if (!indexed) {
        first = (uint32_t)r->x[1]; base_inst = (uint32_t)r->x[3]; inst = (uint32_t)r->x[4] ? (uint32_t)r->x[4] : 1;
        max_index = first + count;
        if (!native) {
            uint32_t *e = expand_indices(mode, count, NULL, 0, first, &icount);
            ib = vk_transient_copy(e, (uint64_t)icount * 4 + 4, 4);
            free(e);
        }
    } else {
        uint32_t itype = (uint32_t)r->x[1];
        uint64_t iaddr = r->x[3];
        base_vertex = (int32_t)r->x[4];
        base_inst = (uint32_t)r->x[5];
        inst = (uint32_t)r->x[6] ? (uint32_t)r->x[6] : 1;
        const void *src = (const void *)(uintptr_t)iaddr;
        uint32_t restart = st.prim_restart ? st.restart_index : 0xFFFFFFFFu;
        if (native && itype != 0) {
            uint32_t isz = itype == 1 ? 2 : 4;
            itype_vk = itype == 1 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
            icount = count;
            if (iaddr % isz == 0 && guest_direct(iaddr, (uint64_t)count * isz, &ib.buf, &ib.off)) {
                scan_src = src; scan_isz = isz; scan_restart = restart;  // max_index only if a stream gets copied
            } else {
                ib = vk_transient_copy(src, (uint64_t)count * isz + 4, 4);
                if (isz == 2) { const uint16_t *s = src; for (uint32_t i = 0; i < count; i++) if (s[i] > max_index && s[i] != (uint16_t)restart) max_index = s[i]; }
                else { const uint32_t *s = src; for (uint32_t i = 0; i < count; i++) if (s[i] > max_index && s[i] != restart) max_index = s[i]; }
            }
        } else if (native) {  // 8-bit indices: widen to 16
            ib = vk_transient((uint64_t)count * 2 + 4, 4);
            uint16_t *w = ib.ptr;
            const uint8_t *s = src;
            for (uint32_t i = 0; i < count; i++) { w[i] = s[i] == 0xFF && st.prim_restart ? 0xFFFF : s[i]; if (s[i] > max_index && w[i] != 0xFFFF) max_index = s[i]; }
            itype_vk = VK_INDEX_TYPE_UINT16;
            icount = count;
        } else {
            uint32_t *e = expand_indices(mode, count, src, (int)itype, 0, &icount);
            for (uint32_t i = 0; i < icount; i++) if (e[i] > max_index) max_index = e[i];
            ib = vk_transient_copy(e, (uint64_t)icount * 4 + 4, 4);
            free(e);
        }
        if (!scan_src) {
            max_index += 1;
            if (base_vertex > 0) max_index += (uint32_t)base_vertex;
        }
    }
    vk_up_kind = 0;
    if (use_index) bind_index(cb, ib.buf, ib.off, itype_vk);

    // ---- vertex streams: the range this draw can read, copied once per submission
    for (int s = 0; s < 17; s++) {
        if (!(kp->used_streams & (1u << s))) continue;
        VkSpan sp;
        if (s == ZERO_BINDING) {
            static const float zero[4] = {0, 0, 0, 1};
            sp = vk_transient_copy(zero, 16, 16);
        } else {
            Range v = st.vbuf[s];
            VkBuffer dbuf;
            VkDeviceSize doff;
            if (v.addr && v.size && guest_direct(v.addr, v.size, &dbuf, &doff)) {
                bind_vertex(cb, (uint32_t)s, dbuf, doff);
                continue;
            }
            if (scan_src) {  // copying after all: the index scan bounds the copy
                if (scan_isz == 2) { const uint16_t *q = scan_src; for (uint32_t i = 0; i < icount; i++) if (q[i] > max_index && q[i] != (uint16_t)scan_restart) max_index = q[i]; }
                else { const uint32_t *q = scan_src; for (uint32_t i = 0; i < icount; i++) if (q[i] > max_index && q[i] != scan_restart) max_index = q[i]; }
                max_index += 1;
                if (base_vertex > 0) max_index += (uint32_t)base_vertex;
                scan_src = NULL;
            }
            uint64_t stride = kp->stride[s], need;
            if (!stride) need = 256;
            else if (kp->divisor[s]) need = ((uint64_t)base_inst + inst + kp->divisor[s] - 1) / kp->divisor[s] * stride + 256;
            else need = (uint64_t)max_index * stride + 256;
            if (v.size && need > v.size) need = v.size;
            if (prof && v.addr) {  // why the direct (zero-copy) bind was refused: below the heap / past the import / window
                extern uint64_t prof_vx_copy_n, prof_vx_copy_bytes, prof_vx_low_bytes, prof_vx_low_region;
                prof_vx_copy_n++; prof_vx_copy_bytes += need;
                if (v.addr < HEAP_BASE) { prof_vx_low_bytes += need; prof_vx_low_region = v.addr >> 28; }
            }
            vk_up_kind = 2;
            if (!v.addr) { static const uint8_t zero[256]; sp = vk_transient_copy(zero, 256, 16); }
            else sp = guest_copy(v.addr, need ? need : 16);
            vk_up_kind = 0;
        }
        VkDeviceSize off = sp.off;
        bind_vertex(cb, (uint32_t)s, sp.buf, off);
    }

    if (PROFD) prof_vtx += prof_now() - prof_t2_;
    // ---- draw
    // Instanced draws with item spots among their instances (bushsanity: bushes are drawn in batches, each instance's
    // model matrix in vertex storage buffer 0, indexed by instance id): drawn as runs of instances, gold runs tinted
    // (as renderer.m; gl_InstanceIndex includes the base instance, so a run [b + k, b + k + n) draws exactly those).
    enum { MAX_RUNS = 64 };
    uint32_t run_at[MAX_RUNS], run_n[MAX_RUNS];
    bool run_gold[MAX_RUNS];
    int nruns = 0;
    if (pr->fi->uses_tint && inst > 1 && st.sbuf[0][0].addr) {
        extern bool pot_glow_active(void);
        extern bool pot_instance_gold(const float *m);
        uint64_t recs = st.sbuf[0][0].size / 64;
        if (pot_glow_active() && (uint64_t)base_inst + inst <= recs) {
            const float *m = (const float *)(uintptr_t)st.sbuf[0][0].addr;
            for (uint32_t i = 0; i < inst; i++) {
                bool g = pot_instance_gold(m + (size_t)(base_inst + i) * 16);
                if (nruns && run_gold[nruns - 1] == g) { run_n[nruns - 1]++; continue; }
                if (nruns == MAX_RUNS) { nruns = 0; break; }  // (too fragmented: draw as is)
                run_at[nruns] = i; run_n[nruns] = 1; run_gold[nruns] = g; nruns++;
            }
            if (nruns == 1 && !run_gold[0]) nruns = 0;
        }
    }
    if (!nruns) { run_at[0] = 0; run_n[0] = inst; nruns = 1; run_gold[0] = false; }
    // more than 2^28 vertices in one draw is a garbage count, not TotK: skipped instead of hanging the GPU
    bool huge = (uint64_t)(use_index ? icount : count) * inst > (1ull << 28);
    if (huge) {
        static int nlog;
        if (nlog++ < 20) LOG("[vk] draw skipped: %u vertices x %u instances (fs %016llx)\n", use_index ? icount : count, inst,
                             (unsigned long long)g_cur_fs_hash);
        nruns = 0;
    }
    hang_op(cb, HOP_DRAW, g_cur_fs_hash, st.program->stages[0].code_addr, use_index ? icount : count, inst, fb_bits);
    if (g_gpuprof_frame && gp_per_draw())   // (LARECOMP_GPUPROF_DRAWS=1: a timestamp before every draw; see vk_gpuprof_report)
        gp_mark(cb, "draw fs %016llx n %u inst %u", (unsigned long long)g_cur_fs_hash, use_index ? icount : count, inst);
    if (nruns && oq_scope) oq_draw(cb);   // (occlusion query scope: its query for this pass segment)
    int e1_q = -1;
    if (g_debug_frame && e1_armed && e1_n < 8192 && nruns && oq_open < 0) {
        e1_q = e1_n++;
        e1_d[e1_q] = (E1Draw){g_cur_fs_hash, st.ubuf[0][1].addr, st.ubuf[0][5].addr, use_index ? icount : count, inst};
        vkCmdBeginQuery(cb, e1_qp, (uint32_t)e1_q, 0);
    }
    for (int ri = 0; ri < nruns; ri++) {
        if (nruns > 1 || run_gold[0]) {  // this run's tint: a fragment set of its own
            float rtint[8] = {0};
            extern void pot_gold_tint(float tint[8]);
            if (run_gold[ri]) pot_gold_tint(rtint);
            uint32_t rdyn[MAX_BIND], nrdyn = 0;
            VkDescriptorSet rset = stage_set(pr, pr->set_layout[1], 1, 1, pr->fi, rs0, rtint, 0, 0, 0, fii, rdyn, &nrdyn);
            vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pr->layout, 1, 1, &rset, nrdyn, rdyn);
        }
        if (!use_index) vkCmdDraw(cb, count, run_n[ri], first, base_inst + run_at[ri]);
        else vkCmdDrawIndexed(cb, icount, run_n[ri], 0, base_vertex, base_inst + run_at[ri]);
    }
    if (pe_n > 0 && nruns) {   // vp_exact split: the partial edge strips with the old stretched map (see the viewport block)
        uint32_t sdyn[2 * MAX_BIND], nsdyn = 0;
        VkDescriptorSet ssets[2];
        ssets[0] = stage_set(pr, pr->set_layout[0], 0, 0, pr->vi, NULL, NULL, 0, 0, 0, NULL, sdyn, &nsdyn);
        ssets[1] = stage_set(pr, pr->set_layout[1], 1, 1, pr->fi, snap_rs, tint, 0, 0, 0, fii, sdyn, &nsdyn);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pr->layout, 0, 2, ssets, nsdyn, sdyn);
        const int cx1 = (int)(pe_sc.offset.x + pe_sc.extent.width), cy1 = (int)(pe_sc.offset.y + pe_sc.extent.height);
        for (int si = 0; si < pe_n; si++) {
            int x0 = pe_strip[si].offset.x, y0 = pe_strip[si].offset.y;
            int x1 = x0 + (int)pe_strip[si].extent.width, y1 = y0 + (int)pe_strip[si].extent.height;
            if (x0 < pe_sc.offset.x) x0 = pe_sc.offset.x;
            if (y0 < pe_sc.offset.y) y0 = pe_sc.offset.y;
            if (x1 > cx1) x1 = cx1;
            if (y1 > cy1) y1 = cy1;
            if (x1 <= x0 || y1 <= y0) continue;
            VkRect2D ssc = {{x0, y0}, {(uint32_t)(x1 - x0), (uint32_t)(y1 - y0)}};
            vkCmdSetScissor(cb, 0, 1, &ssc);
            for (int ri = 0; ri < nruns; ri++) {   // (gold-tinted runs: the strips use the draw's first tint; pot icons never snap)
                if (!use_index) vkCmdDraw(cb, count, run_n[ri], first, base_inst + run_at[ri]);
                else vkCmdDrawIndexed(cb, icount, run_n[ri], 0, base_vertex, base_inst + run_at[ri]);
            }
            stat_vp_strips++;
        }
        vsc.sc_ok = false;   // (the next draw sets its scissor again)
    }
    if (e1_q >= 0) vkCmdEndQuery(cb, e1_qp, (uint32_t)e1_q);
    stat_draws++; g_draws_frame++; chunk_draws++;
    {
        static uint64_t cnt_hash = 1;
        if (cnt_hash == 1) { const char *e = getenv("LARECOMP_COUNT_FSHASH"); cnt_hash = e ? strtoull(e, NULL, 16) : 0; }
        if (cnt_hash && g_cur_fs_hash == cnt_hash) {
            stat_count_fs++;
            // LARECOMP_COUNT_TRIGGER=gpuprof|debug: the first frame (from present 1500 on) that draws it 3+ times gets the next
            // frame GPU-profiled (+ a slow_N.png shot) / debug-dumped once (TotK's weather decides when the clouds are up)
            static uint64_t cf_frame, cf_n;
            static int trig = -1, fired;
            if (trig < 0) { const char *e = getenv("LARECOMP_COUNT_TRIGGER"); trig = !e ? 0 : !strcmp(e, "gpuprof") ? 1 : !strcmp(e, "debug") ? 2 : 0; }
            uint64_t p = renderer_present_count();
            // LARECOMP_COUNT_LOG=<file>: per frame "frame draws inst_sum vertex_sum" of that shader (game-side input check)
            static FILE *cl; static int cl_init; static uint64_t cl_inst, cl_cnt;
            if (!cl_init) { cl_init = 1; const char *e = getenv("LARECOMP_COUNT_LOG"); if (e) cl = fopen(e, "w"); }
            // 5th column "late": draws whose VS storage buffer #0 bytes changed between record time and this shader's first draw of
            // the next frame (the game double-buffers the UI ring: a change within one frame = written after it submitted)
            extern uint64_t g_op_push_ns;
            static uint64_t lw_a[32], lw_s[32], lw_t[32], lw_lag[32], cl_maxlag; static uint8_t lw_d[32][512]; static int lw_n, lw_log;
            static uint64_t late_addr = 1;   // LARECOMP_LATE_ADDR=<hex>: log every late write of the draw at that address
            if (late_addr == 1) { const char *e = getenv("LARECOMP_LATE_ADDR"); late_addr = e ? strtoull(e, NULL, 16) : 0; }
            if (p != cf_frame && cl && cf_n) {
                int late = 0;
                for (int i = 0; i < lw_n; i++)
                    if (memcmp(lw_d[i], (const void *)(uintptr_t)lw_a[i], lw_s[i])) {
                        late++;
                        if (lw_log < 40 || lw_a[i] == late_addr) {
                            lw_log++;
                            const uint32_t *o = (const uint32_t *)lw_d[i], *nw = (const uint32_t *)(uintptr_t)lw_a[i];
                            int w = 0, nd = 0;
                            while (w < (int)(lw_s[i] / 4) - 1 && o[w] == nw[w]) w++;
                            for (int j = 0; j < (int)(lw_s[i] / 4); j++) nd += o[j] != nw[j];
                            LOG("[late] frame %llu draw %d %llx +%llu: word %d %08x -> %08x (%d words differ) snap t %llu check t %llu queue lag %llu us\n",
                                (unsigned long long)cf_frame, i, (unsigned long long)lw_a[i], (unsigned long long)lw_s[i], w, o[w], nw[w], nd,
                                (unsigned long long)lw_t[i], (unsigned long long)(SDL_GetTicksNS() / 1000), (unsigned long long)lw_lag[i]);
                        }
                    }
                fprintf(cl, "%llu %llu %llu %llu %d %llu\n", (unsigned long long)cf_frame, (unsigned long long)cf_n,
                        (unsigned long long)cl_inst, (unsigned long long)cl_cnt, late, (unsigned long long)cl_maxlag);
            }
            if (p != cf_frame) { cf_frame = p; cf_n = 0; cl_inst = cl_cnt = 0; lw_n = 0; cl_maxlag = 0; }
            if (cl && lw_n < 32 && st.sbuf[0][0].addr) {
                lw_a[lw_n] = st.sbuf[0][0].addr;
                lw_s[lw_n] = st.sbuf[0][0].size < 512 ? st.sbuf[0][0].size : 512;
                memcpy(lw_d[lw_n], (const void *)(uintptr_t)lw_a[lw_n], lw_s[lw_n]);
                lw_t[lw_n] = SDL_GetTicksNS() / 1000;
                lw_lag[lw_n] = g_op_push_ns ? lw_t[lw_n] - g_op_push_ns / 1000 : 0;
                if (lw_lag[lw_n] > cl_maxlag) cl_maxlag = lw_lag[lw_n];
                lw_n++;
            }
            cl_inst += inst; cl_cnt += use_index ? icount : count;
            // LARECOMP_COUNT_DUMP=<file>: per draw u64 frame, u32 inst, u32 bound-in-host-memory, u64 sbuf0 addr, then the first
            // 512 bytes of VS storage buffer #0 as host memory holds them now (record time)
            // (kept in memory and written once at present LARECOMP_COUNT_DUMP_AT, default 11000: file writes per draw changed the timing)
            static FILE *cd; static int cd_init; static uint8_t *cd_buf; static size_t cd_len; static uint64_t cd_at;
            if (!cd_init) {
                cd_init = 1;
                const char *e = getenv("LARECOMP_COUNT_DUMP"), *at = getenv("LARECOMP_COUNT_DUMP_AT");
                if (e && (cd_buf = malloc(64u << 20))) cd = fopen(e, "wb");
                cd_at = at ? strtoull(at, NULL, 10) : 11000;
            }
            if (cd && st.sbuf[0][0].addr) {
                struct { uint64_t f; uint32_t inst, host; uint64_t a; } h = {p, inst, g_sb0_bound == st.sbuf[0][0].addr, st.sbuf[0][0].addr};
                if (cd_len + sizeof h + 512 <= (64u << 20) && p < cd_at) {
                    memcpy(cd_buf + cd_len, &h, sizeof h);
                    memset(cd_buf + cd_len + sizeof h, 0, 512);
                    memcpy(cd_buf + cd_len + sizeof h, (const void *)(uintptr_t)st.sbuf[0][0].addr, st.sbuf[0][0].size < 512 ? st.sbuf[0][0].size : 512);
                    cd_len += sizeof h + 512;
                } else {
                    fwrite(cd_buf, 1, cd_len, cd);
                    fclose(cd);
                    cd = NULL;
                    LOG("[count] storage buffer dump written: %zu bytes\n", cd_len);
                }
            }
            if (++cf_n == 3 && trig && !fired && p >= 1500) {
                fired = 1;
                if (trig == 1) { extern volatile int g_gpuprof_request; g_gpuprof_request = 1; }
                else { extern volatile int g_debug_frame_request; g_debug_frame_request = 1; }
                LOG("[count] fs %016llx drawn 3 times in frame %llu -> %s the next frame\n", (unsigned long long)cnt_hash,
                    (unsigned long long)p, trig == 1 ? "GPU profile" : "debug dump of");
            }
        }
    }
    cum_draws++;
    if (g_debug_frame) { debug_probe(pr); debug_probe_row(pr); }
    if (g_debug_frame && g_cur_fs_hash == 0x85b366a0e972a9bcull && st.rt.ncolor && st.rt.color[0] && st.rt.color[0]->backend) {
        char path[96];  // (LARECOMP_FLAME_AUTO / torch flame: its colour target after the draw)
        snprintf(path, sizeof path, "build/pass_flame_%llu_after.png", (unsigned long long)renderer_present_count());
        renderer_save_texture_tex((TexBackend *)st.rt.color[0]->backend, path);
    }
    if (g_gpuprof_frame && gp_n) {
        if (!gp_draws[gp_n - 1]) {  // first draw of the row: its fragment shader hash
            size_t l = strlen(gp_label[gp_n - 1]);
            snprintf(gp_label[gp_n - 1] + l, sizeof gp_label[0] - l, ", first fs %016llx", (unsigned long long)g_cur_fs_hash);
        }
        gp_draws[gp_n - 1]++;
    }
}

// LARECOMP_DEBUG_PROBE=x,y (debug frame): after each draw, texel (x, y) of every colour target big enough (its own
// allocation's pixels) -> "[probe] #<draw> fs <hash> vs/fs <addr> rt<i> <w>x<h> f<fmt>: <old hex> -> <new hex>" when it
// changed. Finds the draw that writes a given pixel. Ends the open pass without a dump (the next draw begins it again,
// LOAD) and waits for the GPU after every draw: debug frames only.
static int probe_auto_x = -1, probe_auto_y;  // LARECOMP_DEBUG_PROBE=auto: first NaN/Inf pixel a pass dump found
// (flame_probe_x / _y: set by LARECOMP_FLAME_AUTO, used when LARECOMP_DEBUG_PROBE is not set)
static uint64_t probe_auto_frame;
static void debug_probe(Prog *pr) {
    static int px = -2, py, ndraw, automode, prel;
    if (px == -2) {  // "x,yr": x, y in 1600x900 scene units, scaled to each target (half-res targets too)
        const char *e = getenv("LARECOMP_DEBUG_PROBE");
        px = -1;
        automode = e && !strcmp(e, "auto");
        if (e && !automode) { px = atoi(e); const char *cm = strchr(e, ','); py = cm ? atoi(cm + 1) : 0; prel = strchr(e, 'r') != NULL; }
    }
    if (automode) {  // (with LARECOMP_DEBUG_FRAMES=2: frame N's dumps find the pixel, frame N + 1 probes it)
        if (probe_auto_x < 0 || renderer_present_count() == probe_auto_frame) return;
        px = probe_auto_x; py = probe_auto_y;
    }
    int qx = px, qy = py;
    bool rel = prel;  // (LARECOMP_FLAME_AUTO pixels are in 1600x900 scene units: scaled to each target's own size)
    if (qx < 0 && flame_probe_x >= 0) { qx = flame_probe_x; qy = flame_probe_y; rel = true; }
    if (qx < 0 || !st.rt.ncolor) return;
    ndraw++;
    if (in_pass) { oq_end_open(vk_cmd()); vkCmdEndRendering(vk_cmd()); in_pass = false; blk_copy_back(vk_cmd()); }
    RtState rts = st.rt;
    VkSpan s = vk_readback(16 * 9);
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);
    bool any = false;
    for (int i = 0; i < rts.ncolor && i < 8; i++) {
        TexBackend *b = rts.color[i] ? (TexBackend *)rts.color[i]->backend : NULL;
        if (!b || !b->image || (b->aspect & ~VK_IMAGE_ASPECT_COLOR_BIT)) continue;
        int tx = rel ? (int)((int64_t)qx * b->width / 1600) : qx, ty = rel ? (int)((int64_t)qy * b->height / 900) : qy;
        if ((uint32_t)tx >= b->width || (uint32_t)ty >= b->height) continue;
        VkBufferImageCopy rg = {.bufferOffset = s.off + (VkDeviceSize)i * 16, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                .imageOffset = {tx, ty, 0}, .imageExtent = {1, 1, 1}};
        vkCmdCopyImageToBuffer(cb, b->image, VK_IMAGE_LAYOUT_GENERAL, s.buf, 1, &rg);
        any = true;
    }
    // the depth attachment's texel too (depth aspect only; slot 8): material-class depth buffers (TotK lighting quads use
    // depth EQUAL against a class depth written by gl_FragDepth)
    TexBackend *db = rts.depth ? (TexBackend *)rts.depth->backend : NULL;
    int dtx = 0, dty = 0;
    bool dok = db && db->image && (db->aspect & VK_IMAGE_ASPECT_DEPTH_BIT);
    if (dok) {
        dtx = rel ? (int)((int64_t)qx * db->width / 1600) : qx; dty = rel ? (int)((int64_t)qy * db->height / 900) : qy;
        dok = (uint32_t)dtx < db->width && (uint32_t)dty < db->height;
    }
    if (dok) {
        VkBufferImageCopy rg = {.bufferOffset = s.off + 8 * 16, .imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1},
                                .imageOffset = {dtx, dty, 0}, .imageExtent = {1, 1, 1}};
        vkCmdCopyImageToBuffer(cb, db->image, VK_IMAGE_LAYOUT_GENERAL, s.buf, 1, &rg);
        any = true;
    }
    if (!any) return;
    renderer_flush();
    if (dok) {
        static const void *dlast_t; static uint32_t dlast;
        uint32_t dv = 0;
        memcpy(&dv, (const uint8_t *)s.ptr + 8 * 16, rts.depth->format == 0x32 && db->format == VK_FORMAT_D16_UNORM ? 2 : 4);
        if (dlast_t != rts.depth || dlast != dv) {
            double df = db->format == VK_FORMAT_D16_UNORM ? dv / 65535.0 : (double)*(float *)&dv;
            LOG("[probe] #%d fs %016llx depth f%#x %ux%u: %08x (%.9f)\n", ndraw, (unsigned long long)g_cur_fs_hash,
                rts.depth->format, db->width, db->height, dv, df);
            dlast_t = rts.depth; dlast = dv;
        }
    }
    static struct { const void *t; uint8_t v[16]; } last[64];
    for (int i = 0; i < rts.ncolor && i < 8; i++) {
        NvnTexture *t = rts.color[i];
        TexBackend *b = t ? (TexBackend *)t->backend : NULL;
        if (!b || !b->image || (b->aspect & ~VK_IMAGE_ASPECT_COLOR_BIT)) continue;
        int tx = rel ? (int)((int64_t)qx * b->width / 1600) : qx, ty = rel ? (int)((int64_t)qy * b->height / 900) : qy;
        if ((uint32_t)tx >= b->width || (uint32_t)ty >= b->height) continue;
        const FormatInfo *fi = nvn_format_info(t->format);
        int n = fi && fi->bpb && fi->bpb <= 16 ? fi->bpb : 4;
        const uint8_t *v = (const uint8_t *)s.ptr + i * 16;
        int k = 0;
        while (k < 64 && last[k].t && last[k].t != t) k++;
        if (k == 64) k = 63;
        bool same = last[k].t == t && !memcmp(last[k].v, v, n);
        if (!same) {
            static int state_logged = -1;
            if (state_logged != ndraw) {
                state_logged = ndraw;
                LOG("[probe] #%d fs %016llx state: depth test %d write %d op %d (ds %02x %02x %02x) pass depth fmt %d viewport z %.6f..%.6f\n",
                    ndraw, (unsigned long long)g_cur_fs_hash, probe_dt, probe_dw, (int)probe_dop, st.ds[0], st.ds[1], st.ds[2],
                    (int)pass_depth, st.depth_range[0][0], st.depth_range[0][1]);
            }
            char o[40] = "", nw[40] = "";
            for (int j = 0; j < n; j++) { sprintf(o + j * 2, "%02x", last[k].t == t ? last[k].v[j] : 0); sprintf(nw + j * 2, "%02x", v[j]); }
            LOG("[probe] #%d fs %016llx vs %llx fs %llx rt%d %ux%u f%#x: %s -> %s\n", ndraw, (unsigned long long)g_cur_fs_hash,
                (unsigned long long)st.program->stages[0].code_addr, (unsigned long long)st.program->stages[1].code_addr, i, b->width,
                b->height, t->format, last[k].t == t ? o : "(first)", nw);
            last[k].t = t;
            memcpy(last[k].v, v, n);
        }
    }
    (void)pr;
}

// LARECOMP_DEBUG_PROBE_ROW=<y>[,<y>...] (debug frame, up to 4 rows; y in 1600x900 scene units, scaled to each target):
// after each draw, the whole rows y of every colour target -> "[rowprobe] #<draw> fs <hash> vs/fs <addr> rt<i> <w>x<h>
// f<fmt> y <y>: <n> px in <k> runs: a-b..." for each target row the draw changed. Finds the draws that write thin
// features (the vertical streaks, user 2026-10-15: the same thin x range on rows far apart) without knowing in advance
// where one lands. Waits for the GPU after every draw: debug frames only.
static void debug_probe_row(Prog *pr) {
    static int nrows = -1, ry[4], ndraw;
    if (nrows < 0) {
        nrows = 0;
        const char *e = getenv("LARECOMP_DEBUG_PROBE_ROW");
        for (const char *p = e; p && *p && nrows < 4;) {
            ry[nrows++] = (int)strtol(p, (char **)&p, 10);
            if (*p == ',') p++; else break;
        }
    }
    if (!nrows || !st.rt.ncolor) return;
    ndraw++;
    if (in_pass) { oq_end_open(vk_cmd()); vkCmdEndRendering(vk_cmd()); in_pass = false; blk_copy_back(vk_cmd()); }
    RtState rts = st.rt;
    enum { MAXW = 4096, STRIDE = MAXW * 16 };
    VkSpan s = vk_readback((uint64_t)STRIDE * 8 * 4);
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);
    int tw[8] = {0}, th[8] = {0}, bpp[8] = {0};
    bool any = false;
    for (int i = 0; i < rts.ncolor && i < 8; i++) {
        TexBackend *b = rts.color[i] ? (TexBackend *)rts.color[i]->backend : NULL;
        if (!b || !b->image || (b->aspect & ~VK_IMAGE_ASPECT_COLOR_BIT) || b->width > MAXW) continue;
        const FormatInfo *fi = nvn_format_info(rts.color[i]->format);
        bpp[i] = fi && fi->bpb && fi->bpb <= 16 ? fi->bpb : 4;
        for (int r = 0; r < nrows; r++) {
            int ty = (int)((int64_t)ry[r] * b->height / 900);
            if ((uint32_t)ty >= b->height) continue;
            VkBufferImageCopy rg = {.bufferOffset = s.off + (VkDeviceSize)(r * 8 + i) * STRIDE,
                                    .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageOffset = {0, ty, 0},
                                    .imageExtent = {b->width, 1, 1}};
            vkCmdCopyImageToBuffer(cb, b->image, VK_IMAGE_LAYOUT_GENERAL, s.buf, 1, &rg);
        }
        tw[i] = (int)b->width; th[i] = (int)b->height;
        any = true;
    }
    if (!any) return;
    renderer_flush();
    static struct { const void *t; int row; uint8_t *data; int w; } last[1024];
    for (int i = 0; i < rts.ncolor && i < 8; i++) {
        if (!tw[i]) continue;
        NvnTexture *t = rts.color[i];
        for (int r = 0; r < nrows; r++) {
            int ty = (int)((int64_t)ry[r] * th[i] / 900);
            if (ty >= th[i]) continue;
            int k = 0;
            while (k < 1024 && last[k].t && !(last[k].t == t && last[k].row == r)) k++;
            if (k == 1024) continue;  // (was: reuse slot 63 -> compares unrelated rows)
            if (!last[k].data) last[k].data = malloc(STRIDE);
            const uint8_t *v = (const uint8_t *)s.ptr + (size_t)(r * 8 + i) * STRIDE;
            int w = tw[i], n = bpp[i];
            if (last[k].t == t && last[k].w == w) {
                int nchg = 0, nruns = 0, start = -1, rl = 0;
                char runs[600] = "";
                for (int x = 0; x <= w; x++) {
                    bool c = x < w && memcmp(v + (size_t)x * n, last[k].data + (size_t)x * n, n) != 0;
                    if (c) { nchg++; if (start < 0) start = x; }
                    else if (start >= 0) {
                        nruns++;
                        if (rl < (int)sizeof runs - 24) rl += snprintf(runs + rl, sizeof runs - rl, " %d-%d", start, x - 1);
                        start = -1;
                    }
                }
                if (nchg)
                    LOG("[rowprobe] #%d fs %016llx vs %llx fs %llx rt%d %dx%d f%#x y %d: %d px in %d runs:%s\n", ndraw,
                        (unsigned long long)g_cur_fs_hash, (unsigned long long)st.program->stages[0].code_addr,
                        (unsigned long long)st.program->stages[1].code_addr, i, w, th[i], t->format, ty, nchg, nruns, runs);
            }
            last[k].t = t; last[k].row = r; last[k].w = w;
            memcpy(last[k].data, v, (size_t)w * n);
        }
    }
    (void)pr;
}

static void debug_draw(Prog *pr, CmdRec *r, bool indexed) {
    // LARECOMP_SAVE_GLSL: also every program this debug frame draws (programs built by the precompile replay or an
    // earlier translation were saved under that session's code addresses, not this one's)
    if (getenv("LARECOMP_SAVE_GLSL") && pr->vi && pr->fi && !pr->glsl_saved) {
        pr->glsl_saved = true;
        if (pr->vi->msl && pr->fi->msl) {
            save_glsl(pr->vi->msl, st.program->stages[0].code_addr, "vs");
            save_glsl(pr->fi->msl, st.program->stages[1].code_addr, "fs");
        } else {   // GLSL dropped after the pipeline libraries were built (compile_program): translate again to save it
            pthread_mutex_lock(&translate_lock);
            maxwell_gmem_base = HEAP_BASE;
            maxwell_gmem_size = HEAP_MAX;
            maxwell_gdev_size = gdev_window_size();
            ShaderInfo *vi = translate_stage(pr->code[0], pr->cmax[0], pr->csize[0], STAGE_VERTEX, &pr->vkey);
            ShaderInfo *fi = translate_stage(pr->code[1], pr->cmax[1], pr->csize[1], STAGE_FRAGMENT, &pr->fkey);
            pthread_mutex_unlock(&translate_lock);
            if (vi->msl) save_glsl(vi->msl, st.program->stages[0].code_addr, "vs");
            if (fi->msl) save_glsl(fi->msl, st.program->stages[1].code_addr, "fs");
            maxwell_free(vi);
            maxwell_free(fi);
        }
    }
    LOG("[draw] %s inst %llu mode %llu count %llu prog vs=%llx fs=%llx | vs cb %#x fs cb %#x | rt %d", indexed ? "e" : "a",
        (unsigned long long)(indexed ? r->x[6] : r->x[4]), (unsigned long long)r->x[0], (unsigned long long)r->x[2],
        (unsigned long long)st.program->stages[0].code_addr, (unsigned long long)st.program->stages[1].code_addr,
        pr->vi->cbuf_mask, pr->fi->cbuf_mask, st.rt.ncolor);
    for (int i = 0; i < st.rt.ncolor; i++) if (st.rt.color[i]) LOG(" %ux%u/f%#x", st.rt.color[i]->width, st.rt.color[i]->height, st.rt.color[i]->format);
    if (st.rt.depth) LOG(" d%#x %p", st.rt.depth->format, (void *)st.rt.depth);
    LOG(" | vp %.0f %.0f %.0f %.0f | attrs %#x blend %#x ds %02x%02x%02x poly %02x%02x chmask %08x%s\n", st.viewport[0][0], st.viewport[0][1],
        st.viewport[0][2], st.viewport[0][3], pr->vi->attr_mask, st.color_state[0], st.ds[0], st.ds[1], st.ds[2], st.poly[0], st.poly[1],
        st.channel_mask, st.depth_clamp ? " clamp" : "");
    // LARECOMP_DEBUG_CB=v11.25,v4.147,f3.7: vec4 <index> of shader cbuf <n> of the vertex (v) / fragment (f) stage, per draw
    if (getenv("LARECOMP_DEBUG_CB")) {
        const char *e = getenv("LARECOMP_DEBUG_CB");
        while (*e) {
            char sg = *e++;
            int c = (int)strtol(e, (char **)&e, 10), idx = 0;
            if (*e == '.') idx = (int)strtol(e + 1, (char **)&e, 10);
            while (*e == ',' || *e == ' ') e++;
            int stg = sg == 'f' ? 1 : 0;
            ShaderInfo *si = stg ? pr->fi : pr->vi;
            if (c == 1 && si && st.program) {  // compiler constants after the code (cbuf_info k == 1)
                const NvnShaderStage *ss = &st.program->stages[stg];
                const uint32_t *u = (const uint32_t *)(uintptr_t)(ss->code_addr + ss->const_offset + (uint64_t)idx * 16);
                LOG("[draw]   cb %c1[%d] (code %llx const off %#x size %#x, shader reads %#x) = %08x %08x %08x %08x\n", sg, idx,
                    (unsigned long long)ss->code_addr, ss->const_offset, ss->const_size, si->cbuf_size[1], u[0], u[1], u[2], u[3]);
                continue;
            }
            if (c < 3 || c >= 18 || !si || !((si->cbuf_mask >> c) & 1)) continue;
            const Range *ub = &st.ubuf[stg][c - 3];
            if (!ub->addr || (uint32_t)(idx * 16 + 16) > ub->size) continue;
            const uint32_t *u = (const uint32_t *)(uintptr_t)(ub->addr + (uint64_t)idx * 16);
            float f[4];
            memcpy(f, u, 16);
            LOG("[draw]   cb %c%d[%d] = %.9g %.9g %.9g %.9g (%08x %08x %08x %08x)\n", sg, c, idx, f[0], f[1], f[2], f[3], u[0], u[1],
                u[2], u[3]);
        }
    }
    if (getenv("LARECOMP_DEBUG_DATA")) {  // vertex streams 0-1 and fragment cbufs: the first floats
        for (int i = 0; i < 32; i++) {
            if (!(pr->vi->attr_mask & (1u << i))) continue;
            uint32_t a = i < st.nattrib ? st.attrib[i] : 0, s = (a >> 8) & 0x1F;
            LOG("[draw]   attr%d fmt %#x stream %u off %u stride %u -> vk %d\n", i, a & 0xFF, s, a >> 13,
                s < (uint32_t)st.nstream ? st.stream[s][0] : 0, (int)vertex_format(a & 0xFF));
        }
        for (int i = 0; i < st.rt.ncolor; i++)
            if (st.rt.color[i])
                LOG("[draw]   rt%d %p addr %llx tgt %u depth %u | view %d set %#x layer %u+%u level %u\n", i, (void *)st.rt.color[i],
                    (unsigned long long)st.rt.color[i]->addr, st.rt.color[i]->target, st.rt.color[i]->depth, st.rt.has_cview[i],
                    st.rt.has_cview[i] ? st.rt.cview[i].setmask : 0, st.rt.has_cview[i] ? st.rt.cview[i].min_layer : 0,
                    st.rt.has_cview[i] ? st.rt.cview[i].num_layers : 0, st.rt.has_cview[i] ? st.rt.cview[i].base_level : 0);
        if (st.vbuf[0].addr) {
            const uint8_t *v = (const uint8_t *)(uintptr_t)st.vbuf[0].addr;
            LOG("[draw]   vbuf0 hex:");
            for (int k = 0; k < 48; k++) LOG("%s%02x", k % 4 ? "" : " ", v[k]);
            LOG("\n");
        }
        for (int s = 0; s < 2; s++) {
            if (!st.vbuf[s].addr) continue;
            const float *v = (const float *)(uintptr_t)st.vbuf[s].addr;
            LOG("[draw]   vbuf%d %llx size %llu:", s, (unsigned long long)st.vbuf[s].addr, (unsigned long long)st.vbuf[s].size);
            for (int k = 0; k < 16; k++) LOG(" %.4g", v[k]);
            LOG("\n");
        }
        for (int c = 3; c < 18; c++) {  // shader cbuf c = st.ubuf[c - 3] (cbuf_info: 0 driver, 1 compiler constants)
            const Range *ub = &st.ubuf[1][c - 3];
            if (!((pr->fi->cbuf_mask >> c) & 1) || !ub->addr) continue;
            const float *u = (const float *)(uintptr_t)ub->addr;
            LOG("[draw]   fcb%d %llx size %u:", c, (unsigned long long)ub->addr, (unsigned)ub->size);
            for (int k = 0; k < 96 && k * 4 < (int)ub->size; k++) LOG(" %.6g", u[k]);
            LOG("\n");
        }
        for (int c = 3; c < 18; c++) {  // vertex-stage cbufs (same numbering)
            const Range *ub = &st.ubuf[0][c - 3];
            if (!((pr->vi->cbuf_mask >> c) & 1) || !ub->addr) continue;
            const float *u = (const float *)(uintptr_t)ub->addr;
            LOG("[draw]   vcb%d %llx size %u:", c, (unsigned long long)ub->addr, (unsigned)ub->size);
            for (int k = 0; k < 96 && k * 4 < (int)ub->size; k++) LOG(" %.6g", u[k]);
            LOG("\n");
        }
    }
    for (int i = 0; i < pr->fi->ntex && i < 64; i++) {  // fragment textures: guest size / format, allocation, scale
        NvnTexture *t = shader_texture_object(1, &pr->fi->tex[i]);
        const TexBackend *b = t ? (const TexBackend *)t->backend : NULL;
        if (!t) {   // (why: handle slot, raw handle, texture pool size; F9 capture 72819: UI blur source had 3 of these)
            int hs = ((int)pr->fi->tex[i].handle_offset - 0x20) / 8;
            uint64_t hv = hs >= 0 && hs < 32 ? st.tex[1][hs] : 0;
            LOG("[draw]   tex%d: none (handle offset %#x slot %d handle %llx tic %u pool %d%s)\n", i, pr->fi->tex[i].handle_offset, hs,
                (unsigned long long)hv, (unsigned)(hv & 0xFFFFF), st.tpool ? st.tpool->count : -1, pr->fi->tex[i].bindless ? " bindless" : "");
            uint32_t tic = (uint32_t)(hv & 0xFFFFF);
            if (st.tpool && tic < (uint32_t)st.tpool->count) {
                const NvnTexEntry *e = &((const NvnTexEntry *)st.tpool->entries)[tic];
                LOG("[draw]     slot %u: registered %u times, last at frame %lld: %ux%u f%#x addr %llx '%s'; finalized at frame %lld (now %llu)\n",
                    tic, e->dbg_regs, (long long)e->dbg_reg_frame - 1, e->dbg_w, e->dbg_h, e->dbg_fmt, (unsigned long long)e->dbg_addr,
                    e->dbg_label, (long long)e->dbg_fin_frame - 1, (unsigned long long)renderer_present_count());
            }
            for (unsigned k = 0; k < 1024 && k < g_tex_fin_n; k++) {  // (was that slot emptied by nvnTextureFinalize?)
                const NvnFinRec *r = &g_tex_fin[(g_tex_fin_n - 1 - k) & 1023];
                if (r->tic != (hv & 0xFFFFF)) continue;
                LOG("[draw]     tic %u was %ux%u f%#x addr %llx '%s', finalized at frame %llu (now %llu, %u finalizes ago)\n", r->tic, r->w,
                    r->h, r->fmt, (unsigned long long)r->addr, r->label, (unsigned long long)r->frame,
                    (unsigned long long)renderer_present_count(), k);
                break;
            }
            continue;
        }
        int memnz = -1;   // (non-zero bytes in the first 256 of its storage: CPU data present? same as upload_texture reads)
        if (b && !b->gpu_owned && t->addr) { memnz = 0; for (int k = 0; k < 256; k++) memnz += ((const uint8_t *)(uintptr_t)t->addr)[k] != 0; }
        LOG("[draw]   tex%d: %p %ux%u f%#x lv %u -> %ux%u scale %.4f %.4f%s addr %llx tgt %u depth %u layers %u | owned %d up %d dyn %d mem nz %d/256\n", i,
            (void *)t, t->width, t->height, t->format, b ? b->levels : 0, b ? b->width : 0, b ? b->height : 0, b ? b->sx : 0, b ? b->sy : 0,
            b && b->scalable ? " scalable" : "", (unsigned long long)t->addr, t->target, t->depth, b ? b->layers : 0, b ? b->gpu_owned : -1,
            b ? b->uploaded : -1, b ? b->dynamic : -1, memnz);
        // LARECOMP_DEBUG_TEX=<fs hash hex>: contents of each texture that fragment shader samples ([passstat] line); ends the open pass
        const char *dt = getenv("LARECOMP_DEBUG_TEX");
        if (b && dt && strtoull(dt, NULL, 16) == g_cur_fs_hash) {
            static int tex_dump_no;
            char path[96];
            snprintf(path, sizeof path, "build/pass_tex_%03d_%d.png", tex_dump_no++, i);
            renderer_save_texture_tex((TexBackend *)b, path);
        }
    }
    for (int i = 0; i < pr->vi->ntex && i < 64; i++) {  // vertex textures (displacement / height maps: the 2026-10-15 streaks)
        NvnTexture *t = shader_texture_object(0, &pr->vi->tex[i]);
        const TexBackend *b = t ? (const TexBackend *)t->backend : NULL;
        if (!t) { LOG("[draw]   vtex%d: none (handle offset %#x)\n", i, pr->vi->tex[i].handle_offset); continue; }
        int memnz = -1;
        if (b && !b->gpu_owned && t->addr) { memnz = 0; for (int k = 0; k < 256; k++) memnz += ((const uint8_t *)(uintptr_t)t->addr)[k] != 0; }
        LOG("[draw]   vtex%d: %p %ux%u f%#x lv %u -> %ux%u%s addr %llx tgt %u depth %u layers %u | owned %d up %d dyn %d mem nz %d/256 | smp %llx\n", i,
            (void *)t, t->width, t->height, t->format, b ? b->levels : 0, b ? b->width : 0, b ? b->height : 0,
            b && b->scalable ? " scalable" : "", (unsigned long long)t->addr, t->target, t->depth, b ? b->layers : 0, b ? b->gpu_owned : -1,
            b ? b->uploaded : -1, b ? b->dynamic : -1, memnz, (unsigned long long)pr->vi->tex[i].handle_offset);
        const char *dt = getenv("LARECOMP_DEBUG_VTEX");   // =<fs hash hex>: dump that program's vertex textures
        if (b && dt && strtoull(dt, NULL, 16) == g_cur_fs_hash) {
            static int vtex_dump_no;
            char path[96];
            snprintf(path, sizeof path, "build/pass_vtex_%03d_%d.png", vtex_dump_no++, i);
            renderer_save_texture_tex((TexBackend *)b, path);
        }
    }
}

// ================================================================== texture copies
typedef struct { uint64_t tex; NvnTextureView view; uint8_t has_view; int32_t region[6]; } TexRefRec;

static void copy_region_dims(NvnTexture *t, const TexRefRec *ref, const FormatInfo **fi, int32_t rg[6], int *level,
                             uint64_t *row, uint64_t *img) {
    *fi = nvn_format_info(t->format);
    memcpy(rg, ref->region, sizeof ref->region);
    if (!rg[3]) rg[3] = (int32_t)t->width;
    if (!rg[4]) rg[4] = (int32_t)t->height;
    if (!rg[5]) rg[5] = 1;
    *level = ref->has_view && (ref->view.setmask & 1) ? ref->view.base_level : 0;
    uint32_t wb = ((uint32_t)rg[3] + (*fi)->bw - 1) / (*fi)->bw, hb = ((uint32_t)rg[4] + (*fi)->bh - 1) / (*fi)->bh;
    *row = st.copy_row_stride ? st.copy_row_stride : (uint64_t)wb * (*fi)->bpb;
    *img = st.copy_image_stride ? st.copy_image_stride : *row * hb;
}

static void copy_buffer_to_texture(uint64_t src, const TexRefRec *dst) {
    snap_reason = "buf2tex"; snapshots_invalidate(NULL);
    NvnTexture *t = (NvnTexture *)(uintptr_t)dst->tex;
    if (!t || !src) return;
    vk_end_pass();
    const FormatInfo *fi;
    int32_t rg[6];
    int level;
    uint64_t row, img;
    copy_region_dims(t, dst, &fi, rg, &level, &row, &img);
    if (!fi || !fi->bpb) return;
    extern bool tex_watched(uint64_t addr);
    if (tex_watched(t->addr))
        LOG("[texw] buffer %llx -> texture addr %llx level %d region %d,%d,%d %dx%dx%d row %llu img %llu\n", (unsigned long long)src,
            (unsigned long long)t->addr, level, rg[0], rg[1], rg[2], rg[3], rg[4], rg[5], (unsigned long long)row, (unsigned long long)img);
    TexBackend *b = vk_tex(t, false);  // prior CPU contents uploaded first
    extern void rt_first_note(NvnTexture *t, const char *how);
    rt_first_note(t, "buf2tex");
    renderer_mark_gpu_owned(t);
    if (b->aspect == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) return;
    if (b->astc_bw) {  // (blocks would need the CPU decode: not seen in the game yet)
        static int nlog;
        if (nlog++ < 5) LOG("[vk] buffer -> ASTC texture copy skipped (%ux%u)\n", t->width, t->height);
        return;
    }
    bool array = b->layers > 1 && b->depth == 1;
    uint32_t hb = ((uint32_t)rg[4] + fi->bh - 1) / fi->bh, wb = ((uint32_t)rg[3] + fi->bw - 1) / fi->bw;
    uint32_t out_bpb = fi->bpb == 3 ? 4 : fi->bpb;
    uint32_t nz = (uint32_t)rg[5];
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);
    uint64_t out_row = (uint64_t)wb * out_bpb;
    vk_up_kind = 4; VkSpan s = vk_transient(out_row * hb * nz, 16); vk_up_kind = 0;
    for (uint32_t z = 0; z < nz; z++)
        for (uint32_t y = 0; y < hb; y++) {
            const uint8_t *sr = (const uint8_t *)(uintptr_t)(src + (uint64_t)z * img + (uint64_t)y * row);
            uint8_t *dr = (uint8_t *)s.ptr + ((uint64_t)z * hb + y) * out_row;
            if (out_bpb == fi->bpb) memcpy(dr, sr, out_row);
            else for (uint32_t x = 0; x < wb; x++) { memcpy(dr + x * 4, sr + x * 3, 3); dr[x * 4 + 3] = 255; }
        }
    VkBufferImageCopy c = {.bufferOffset = s.off, .bufferRowLength = wb * fi->bw, .bufferImageHeight = hb * fi->bh,
                           .imageSubresource = {b->aspect & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : b->aspect,
                                                (uint32_t)level, array ? (uint32_t)rg[2] : 0, array ? nz : 1},
                           .imageOffset = {rg[0], rg[1], array ? 0 : rg[2]},
                           .imageExtent = {(uint32_t)rg[3], (uint32_t)rg[4], array ? 1 : nz}};
    {   // (a scaled target gets the guest-size upload unscaled, in its top-left corner: not known to happen; this says if it does)
        float usx, usy;
        renderer_texture_scale(t, &usx, &usy);
        static int nscaled;
        if ((usx != 1 || usy != 1) && nscaled++ < 8)
            LOG("[vk] buffer -> SCALED texture copy (unscaled at host): addr %llx %ux%u f%#x scale %.3f %.3f level %d region %d,%d %dx%d\n",
                (unsigned long long)t->addr, t->width, t->height, t->format, usx, usy, level, rg[0], rg[1], rg[3], rg[4]);
    }
    vkCmdCopyBufferToImage(cb, s.buf, b->image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
}

// That write sets their write-watch bits again, but a batched query earlier in the list (ww_chunk_dirty,
// LARECOMP_WW_BATCH) had marked them fetched, and a chunk already synced in this list was skipped by its chunk key
// (before the batch too): either way a later draw read the mirror without the copied data, one list late. Pages
// synced twice in a submission go volatile (copied per draw) as for any other rewrite. ~0: no list or submission has
// that key.
static void mirror_guest_written(uint64_t addr, uint64_t size) {
    if (!size) return;
    g_gd_gen++;
    uint64_t ab = g_alias_import_base, as = g_alias_import_size;
    if (alias_chunk_key && as && addr < ab + as && addr + size > ab) {
        uint64_t lo = addr > ab ? addr - ab : 0, hi = (addr + size < ab + as ? addr + size : ab + as) - ab;
        for (uint64_t ch = lo >> 20; ch <= (hi - 1) >> 20 && ch < alias_chunk_n; ch++) {
            alias_chunk_key[ch] = ~0ull;
            if (ww_alias.fetched && ch < ww_alias.nch) ww_alias.fetched[ch] = 0;
        }
    }
    if (hm_chunk_key && hm_size && addr < HEAP_BASE + hm_size && addr + size > HEAP_BASE) {
        uint64_t lo = addr > HEAP_BASE ? addr - HEAP_BASE : 0, hi = (addr + size < HEAP_BASE + hm_size ? addr + size : HEAP_BASE + hm_size) - HEAP_BASE;
        for (uint64_t ch = lo >> 20; ch <= (hi - 1) >> 20 && ch < ((hm_size + (1u << 20) - 1) >> 20); ch++) {
            hm_chunk_key[ch] = ~0ull;
            if (ww_heap.fetched && ch < ww_heap.nch) ww_heap.fetched[ch] = 0;
        }
    }
}
static void copy_texture_to_buffer(const TexRefRec *src, uint64_t dst) {
    NvnTexture *t = (NvnTexture *)(uintptr_t)src->tex;
    if (!t || !dst || dst < HEAP_BASE) return;
    if (g_debug_frame) LOG("[readback] tex %ux%u f%#x -> %llx\n", t->width, t->height, t->format, (unsigned long long)dst);
    vk_end_pass();
    const FormatInfo *fi;
    int32_t rg[6];
    int level;
    uint64_t row, img;
    copy_region_dims(t, src, &fi, rg, &level, &row, &img);
    if (!fi || !fi->bpb || fi->bpb == 3 || row % fi->bpb) return;
    TexBackend *b = vk_tex(t, false);
    float tsx, tsy;
    renderer_texture_sample_scale(t, &tsx, &tsy);
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);
    VkImage srcimg = b->image;
    VmaAllocation small_mem = NULL;
    if ((tsx != 1 || tsy != 1) && renderer_format_is_depth(t->format)) {
        static int said;
        if (!said++)
            LOG("[readback] scaled depth %ux%u f%#x (x%.2f): the guest-sized region of the scaled image is copied, not filtered\n",
                t->width, t->height, t->format, tsx);
    }
    uint32_t rlevel = (uint32_t)level;   // (the level the copy below reads: 0 of the filtered image)
    // the CPU expects guest-sized texels: filter the scaled allocation's level down first. Level > 0 too (it read the
    // scaled level at guest offsets: the top-left part of a bigger image, 2026-10-27); 2D, one layer only.
    if ((tsx != 1 || tsy != 1) && !renderer_format_is_depth(t->format) && b->layers == 1 && b->depth == 1 && (uint32_t)level < b->levels) {
        const uint32_t gw = level_extent(t->width, level), gh = level_extent(t->height, level);
        const uint32_t hw = level_extent(b->width, level), hh = level_extent(b->height, level);
        if (level > 0) {
            static int nlog;
            if (nlog++ < 8) LOG("[vk] readback of level %d of a scaled texture (%ux%u -> guest %ux%u, addr %llx f%#x): filtered down\n", level, hw, hh, gw, gh,
                                (unsigned long long)t->addr, t->format);
        }
        VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = b->format,
                                .extent = {gw, gh, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                                .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        VK_CHECK(vmaCreateImage(g_vk.vma, &ci, &ai, &srcimg, &small_mem, NULL));
        vk_image_init_layout(cb, srcimg, VK_IMAGE_ASPECT_COLOR_BIT);
        VkImageBlit bl = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, (uint32_t)level, 0, 1}, .srcOffsets = {{0, 0, 0}, {(int32_t)hw, (int32_t)hh, 1}},
                          .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstOffsets = {{0, 0, 0}, {(int32_t)gw, (int32_t)gh, 1}}};
        // (integer formats cannot be filtered: nearest, as copy_texture)
        vkCmdBlitImage(cb, b->image, VK_IMAGE_LAYOUT_GENERAL, srcimg, VK_IMAGE_LAYOUT_GENERAL, 1, &bl, rt_kind(t->format) ? VK_FILTER_NEAREST : VK_FILTER_LINEAR);
        vk_full_barrier(cb);
        rlevel = 0;
    } else if ((tsx != 1 || tsy != 1) && !renderer_format_is_depth(t->format) && (b->layers > 1 || b->depth > 1)) {
        static int nlog;   // (a scaled array / 3D readback reads the scaled image at guest offsets: says whether one happens)
        if (nlog++ < 8) LOG("[vk] readback of a scaled %s texture (addr %llx %ux%u, %u layers, depth %u, level %d) NOT filtered\n",
                            b->layers > 1 ? "array" : "3D", (unsigned long long)t->addr, t->width, t->height, b->layers, b->depth, level);
    }
    bool array = b->layers > 1 && b->depth == 1;
    uint32_t nz = (uint32_t)rg[5];
    uint64_t total = img * nz;
    VkSpan out = vk_readback(total);
    VkBufferImageCopy c = {.bufferOffset = out.off, .bufferRowLength = (uint32_t)(row / fi->bpb) * fi->bw,
                           .bufferImageHeight = (uint32_t)(img / row) * fi->bh,
                           .imageSubresource = {b->aspect & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : b->aspect,
                                                rlevel, array ? (uint32_t)rg[2] : 0, array ? nz : 1},
                           .imageOffset = {rg[0], rg[1], array ? 0 : rg[2]},
                           .imageExtent = {(uint32_t)rg[3], (uint32_t)rg[4], array ? 1 : nz}};
    vkCmdCopyImageToBuffer(cb, srcimg, VK_IMAGE_LAYOUT_GENERAL, out.buf, 1, &c);
    renderer_flush();  // the CPU may read the result right after the fence
    memcpy((void *)(uintptr_t)dst, out.ptr, total);
    mirror_guest_written(dst, total);
    if (small_mem) vmaDestroyImage(g_vk.vma, srcimg, small_mem);
}

static void copy_texture(const TexRefRec *src, const TexRefRec *dst, uint64_t flags) {
    snap_reason = "tex2tex"; snapshots_invalidate(NULL);
    NvnTexture *st_ = (NvnTexture *)(uintptr_t)src->tex, *dt = (NvnTexture *)(uintptr_t)dst->tex;
    if (!st_ || !dt) return;
    extern bool tex_watched(uint64_t addr);
    if (tex_watched(dt->addr) || tex_watched(st_->addr))
        LOG("[texw] copy texture %llx %ux%ux%u -> %llx %ux%ux%u (src lv %d, dst lv %d, region z %d+%d)\n", (unsigned long long)st_->addr,
            st_->width, st_->height, st_->depth, (unsigned long long)dt->addr, dt->width, dt->height, dt->depth,
            src->has_view && (src->view.setmask & 1) ? src->view.base_level : 0, dst->has_view && (dst->view.setmask & 1) ? dst->view.base_level : 0,
            dst->region[2], dst->region[5]);
    vk_end_pass();
    if (dl_on == 2) renderer_flush();  // (LARECOMP_VK_DEVLOST_TRACE=2: each copy its own submission, waited on)
    gp_mark(vk_cmd(), "copy texture %ux%u -> %ux%u", st_->width, st_->height, dt->width, dt->height);
    dl_note("copy texture %ux%u f%#x -> %ux%u f%#x (%p -> %p)", st_->width, st_->height, st_->format, dt->width, dt->height,
            dt->format, (void *)st_, (void *)dt);
    hang_op(vk_cmd(), HOP_COPY, st_->addr, dt->addr, 0, dt->width, dt->height);
    TexBackend *s = vk_tex(st_, false), *d = vk_tex(dt, true);
    renderer_mark_gpu_owned(dt);
    if (renderer_texture_presented(dt) && st_->width == dt->width && st_->height == dt->height) renderer_texture_mark_presented(st_);
    renderer_texture_first_write(dt);
    renderer_texture_level_fresh(dt);
    const int32_t *sr = src->region, *dr = dst->region;
    float ssx, ssy, dsx, dsy;
    renderer_texture_sample_scale(st_, &ssx, &ssy);
    renderer_texture_scale(dt, &dsx, &dsy);
    uint32_t slv = src->has_view && (src->view.setmask & 1) ? src->view.base_level : 0;
    uint32_t dlv = dst->has_view && (dst->view.setmask & 1) ? dst->view.base_level : 0;
    // Scaled regions (the sweep after the green band, 2026-10-27): a level > 0 takes the level's own scale (host level
    // extent / guest level extent, as attachment_scale), not the nominal one, which fell a row short of the level (1.333,
    // level 5: 33 guest rows -> 44 of 45); a region's two ends are rounded, not its origin and size apart (lround(x0 s) +
    // lround(w s) missed lround((x0 + w) s) by one at DRS / atlas sub-rect edges); and a region that reaches the guest
    // level's edge reaches the host level's. (level 0 too, as attachment_scale: the draws that wrote it used the
    // allocation's own ratio)
    static int nominal0 = -1;
    if (nominal0 < 0) { const char *e = getenv("LARECOMP_SCALE0_NOMINAL"); nominal0 = e && *e == '1'; }
    if ((slv || !nominal0) && (ssx != 1 || ssy != 1) && slv < s->levels) {
        ssx = (float)level_extent(s->width, slv) / (float)level_extent(st_->width, slv);
        ssy = (float)level_extent(s->height, slv) / (float)level_extent(st_->height, slv);
    }
    if ((dlv || !nominal0) && (dsx != 1 || dsy != 1) && dlv < d->levels) {
        dsx = (float)level_extent(d->width, dlv) / (float)level_extent(dt->width, dlv);
        dsy = (float)level_extent(d->height, dlv) / (float)level_extent(dt->height, dlv);
    }
    // Both ends round outward, as draws' viewports and the scissor do: a region edge inside a host pixel takes that pixel. Rounded to
    // nearest, a DRS sub-rect's last partial row (75.22 of 90 at 1.333) was never written by the copy while the snapped draws into the
    // same level did write it, so an additive draw after a copy that should have replaced the row grows it every frame. Same-scale
    // copies get the same rectangle on both sides, so they stay plain copies. The source rounds outward only when its scale matches the
    // destination's (per axis, within 0.01: a level's own scale 1.336 vs 1.333): there it keeps the two sizes equal; a cross-scale blit
    // (scaled -> 1x, an unscaled depth pyramid) keeps nearest source ends, so NEAREST picks drift no more than before.
    // LARECOMP_COPY_SNAP=0: nearest everywhere (the old mapping).
    static int copy_snap = -1;
    if (copy_snap < 0) { const char *e = getenv("LARECOMP_COPY_SNAP"); copy_snap = !e || strcmp(e, "0"); }
    const bool sox = copy_snap && fabsf(ssx - dsx) < 0.01f, soy = copy_snap && fabsf(ssy - dsy) < 0.01f;
    #define REGION_O(o, sc, out) ((out) && (sc) != 1 ? (int)floorf((float)(o) * (sc) + 1e-3f) : (int)lroundf((float)(o) * (sc)))
    int sx0 = REGION_O(sr[0], ssx, sox), sy0 = REGION_O(sr[1], ssy, soy);
    int dx0 = REGION_O(dr[0], dsx, copy_snap), dy0 = REGION_O(dr[1], dsy, copy_snap);
    #undef REGION_O
    #define REGION_END(o, n, sc, glv, hlv, out) ((uint32_t)(o) + (n) >= (glv) ? (int)(hlv) \
        : (out) && (sc) != 1 ? (int)ceilf(((float)(o) + (n)) * (sc) - 1e-3f) : (int)lroundf(((float)(o) + (n)) * (sc)))
    int sw = sr[3] ? REGION_END(sr[0], sr[3], ssx, level_extent(st_->width, slv), level_extent(s->width, slv), sox) - sx0 : (int)s->width;
    int sh = sr[4] ? REGION_END(sr[1], sr[4], ssy, level_extent(st_->height, slv), level_extent(s->height, slv), soy) - sy0 : (int)s->height;
    int dw = dr[3] ? REGION_END(dr[0], dr[3], dsx, level_extent(dt->width, dlv), level_extent(d->width, dlv), copy_snap) - dx0 : (int)d->width;
    int dh = dr[4] ? REGION_END(dr[1], dr[4], dsy, level_extent(dt->height, dlv), level_extent(d->height, dlv), copy_snap) - dy0 : (int)d->height;
    #undef REGION_END
    uint32_t sly = src->has_view && (src->view.setmask & 2) ? src->view.min_layer : 0;
    uint32_t dly = dst->has_view && (dst->view.setmask & 2) ? dst->view.min_layer : 0;
    if (slv >= s->levels || dlv >= d->levels || sly >= s->layers || dly >= d->layers) return;
    {   // whole-image regions are the level's extent, and every region stays inside its level: scaled allocations
        // (internal resolution below 1x) round guest regions past the edge, and out-of-bounds copies / blits are
        // undefined in Vulkan (garbage that changed every frame on NVIDIA: flicker in the depth pyramid's users)
        int slw = (int)level_extent(s->width, slv), slh = (int)level_extent(s->height, slv);
        int dlw = (int)level_extent(d->width, dlv), dlh = (int)level_extent(d->height, dlv);
        if (!sr[3]) sw = slw - sx0;
        if (!sr[4]) sh = slh - sy0;
        if (!dr[3]) dw = dlw - dx0;
        if (!dr[4]) dh = dlh - dy0;
        if (sx0 + sw > slw) sw = slw - sx0;
        if (sy0 + sh > slh) sh = slh - sy0;
        if (dx0 + dw > dlw) dw = dlw - dx0;
        if (dy0 + dh > dlh) dh = dlh - dy0;
        if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return;
    }
    if (g_debug_frame)
        LOG("[copytex] %p %ux%u f%#x lv%u (%d,%d %dx%d) [%ux%u] -> %p %ux%u f%#x lv%u (%d,%d %dx%d) [%ux%u] phys (%d,%d %dx%d) -> (%d,%d %dx%d) flags %llx\n",
            (void *)st_, st_->width, st_->height, st_->format, slv, sr[0], sr[1], sr[3], sr[4], s->width, s->height, (void *)dt, dt->width,
            dt->height, dt->format, dlv, dr[0], dr[1], dr[3], dr[4], d->width, d->height, sx0, sy0, sw, sh, dx0, dy0, dw, dh,
            (unsigned long long)flags);
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);
    VkImageAspectFlags sa = s->aspect, da = d->aspect;
    if (s->samples > 1 && d->samples == 1 && sw == dw && sh == dh && s->format == d->format && !(sa & VK_IMAGE_ASPECT_DEPTH_BIT)) {
        VkImageResolve rv = {.srcSubresource = {sa, slv, sly, 1}, .srcOffset = {sx0, sy0, 0}, .dstSubresource = {da, dlv, dly, 1},
                             .dstOffset = {dx0, dy0, 0}, .extent = {(uint32_t)sw, (uint32_t)sh, 1}};
        vkCmdResolveImage(cb, s->image, VK_IMAGE_LAYOUT_GENERAL, d->image, VK_IMAGE_LAYOUT_GENERAL, 1, &rv);
        return;
    }
    if (s->samples != d->samples) return;
    if (sw == dw && sh == dh && s->format == d->format) {
        VkImageCopy c = {.srcSubresource = {sa, slv, sly, 1}, .srcOffset = {sx0, sy0, 0}, .dstSubresource = {da, dlv, dly, 1},
                         .dstOffset = {dx0, dy0, 0}, .extent = {(uint32_t)sw, (uint32_t)sh, 1}};
        vkCmdCopyImage(cb, s->image, VK_IMAGE_LAYOUT_GENERAL, d->image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
        return;
    }
    // scaled / converting copy: the game builds its depth pyramid this way (nearest), colour copies filter if asked
    bool depth = (sa | da) & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
    if (depth && s->format != d->format) return;
    VkImageBlit bl = {.srcSubresource = {depth ? (sa & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : sa) : sa, slv, sly, 1},
                      .srcOffsets = {{sx0, sy0, 0}, {sx0 + sw, sy0 + sh, 1}},
                      .dstSubresource = {depth ? (da & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : da) : da, dlv, dly, 1},
                      .dstOffsets = {{dx0, dy0, 0}, {dx0 + dw, dy0 + dh, 1}}};
    bool linear = (flags & 1) && !depth && !rt_kind(st_->format);
    dl_note("  blit %p lv%u/%u %ux%u (%d,%d %dx%d) -> %p lv%u/%u %ux%u (%d,%d %dx%d) %s", (void *)s->image, slv, s->levels, s->width,
            s->height, sx0, sy0, sw, sh, (void *)d->image, dlv, d->levels, d->width, d->height, dx0, dy0, dw, dh, linear ? "linear" : "nearest");
    vkCmdBlitImage(cb, s->image, VK_IMAGE_LAYOUT_GENERAL, d->image, VK_IMAGE_LAYOUT_GENERAL, 1, &bl, linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
}

// ================================================================== present helpers (device.c)
static VkPipelineLayout present_layout;
static VkDescriptorSetLayout present_set_layout;

static VkPipeline present_pipeline(VkFormat fmt) {
    static struct { VkFormat f; VkPipeline p; } cache[4];
    for (int i = 0; i < 4; i++) if (cache[i].p && cache[i].f == fmt) return cache[i].p;
    if (!present_layout) {
        VkDescriptorSetLayoutBinding b = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL};
        VkDescriptorSetLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &b};
        VK_CHECK(vkCreateDescriptorSetLayout(g_vk.dev, &li, NULL, &present_set_layout));
        VkPushConstantRange r = {VK_SHADER_STAGE_VERTEX_BIT, 0, 32};
        VkPipelineLayoutCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                         .pSetLayouts = &present_set_layout, .pushConstantRangeCount = 1, .pPushConstantRanges = &r};
        VK_CHECK(vkCreatePipelineLayout(g_vk.dev, &ci, NULL, &present_layout));
    }
    VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT,
         .module = vk_builtin_shader("present.vert"), .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
         .module = vk_builtin_shader("present.frag"), .pName = "main"}};
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
                                       .pColorBlendState = &cb, .pDynamicState = &dy, .layout = present_layout};
    VkPipeline p;
    VK_CHECK(vkCreateGraphicsPipelines(g_vk.dev, g_vk.pcache, 1, &pi, NULL, &p));
    for (int i = 0; i < 4; i++) if (!cache[i].p) { cache[i].f = fmt; cache[i].p = p; break; }
    return p;
}

// Scanout shows the bytes the game wrote: sRGB images are read through a UNORM view (no decode), like metal.m.
static VkImageView raw_view(TexBackend *b) {
    VkFormat f = b->format == VK_FORMAT_R8G8B8A8_SRGB ? VK_FORMAT_R8G8B8A8_UNORM
               : b->format == VK_FORMAT_B8G8R8A8_SRGB ? VK_FORMAT_B8G8R8A8_UNORM : b->format;
    static const VkComponentMapping id = {0};
    uint32_t ly = dump_layer < b->layers ? dump_layer : 0;
    return vk_image_view(b, VK_IMAGE_VIEW_TYPE_2D, f, 0, 1, ly, 1, id, VK_IMAGE_ASPECT_COLOR_BIT);
}

// ---- post passes (Video tab "Anti-aliasing" / "Upscaler"): SMAA 1x at the game image's size, then AMD FSR 1 (EASU
// upscale to the image's size in the window, RCAS sharpen) when it is smaller than its place in the window, as
// metal.m does with MetalFX. Each pass: a fullscreen triangle into an RGBA8 image (gamma-encoded, like the input);
// up to three inputs (bindings 0-2), sampled linear / clamp.
static VkPipelineLayout post_layout;
static VkDescriptorSetLayout post_set_layout;

static VkPipeline post_pipeline(const char *fs) {
    static struct { const char *fs; VkPipeline p; } cache[8];
    for (int i = 0; i < 8; i++) if (cache[i].p && cache[i].fs == fs) return cache[i].p;
    if (!post_layout) {
        VkDescriptorSetLayoutBinding b[3];
        for (uint32_t i = 0; i < 3; i++)
            b[i] = (VkDescriptorSetLayoutBinding){i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL};
        VkDescriptorSetLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 3, .pBindings = b};
        VK_CHECK(vkCreateDescriptorSetLayout(g_vk.dev, &li, NULL, &post_set_layout));
        VkPushConstantRange r = {VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32};
        VkPipelineLayoutCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                         .pSetLayouts = &post_set_layout, .pushConstantRangeCount = 1, .pPushConstantRanges = &r};
        VK_CHECK(vkCreatePipelineLayout(g_vk.dev, &ci, NULL, &post_layout));
    }
    VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT,
         .module = vk_builtin_shader("post.vert"), .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
         .module = vk_builtin_shader(fs), .pName = "main"}};
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
    VkFormat fmt = VK_FORMAT_R8G8B8A8_UNORM;
    VkPipelineRenderingCreateInfo ri = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = 1,
                                        .pColorAttachmentFormats = &fmt};
    VkPipelineDepthStencilStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkGraphicsPipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &ri, .stageCount = 2,
                                       .pStages = stages, .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vs,
                                       .pRasterizationState = &rs, .pMultisampleState = &ms, .pDepthStencilState = &ds,
                                       .pColorBlendState = &cb, .pDynamicState = &dy, .layout = post_layout};
    VkPipeline p;
    VK_CHECK(vkCreateGraphicsPipelines(g_vk.dev, g_vk.pcache, 1, &pi, NULL, &p));
    for (int i = 0; i < 8; i++) if (!cache[i].p) { cache[i].fs = fs; cache[i].p = p; break; }
    return p;
}

// Post image `i` at w x h (RGBA8, kept until the size changes). 0, 1: SMAA edges / weights, 2: SMAA output,
// 3, 4: FSR EASU / RCAS.
static TexBackend *post_image(int i, uint32_t w, uint32_t h) {
    static TexBackend img[5];
    TexBackend *d = &img[i];
    if (d->image && (d->width != w || d->height != h)) {
        vk_views_forget(d);
        vk_defer_image(d->image, d->mem);
        d->image = VK_NULL_HANDLE;
        d->mem = VK_NULL_HANDLE;
    }
    if (!d->image) {
        VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                                .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1,
                                .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                                .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        VK_CHECK(vmaCreateImage(g_vk.vma, &ci, &ai, &d->image, &d->mem, NULL));
        d->format = VK_FORMAT_R8G8B8A8_UNORM; d->width = w; d->height = h; d->depth = 1; d->layers = 1; d->levels = 1;
        d->samples = 1; d->aspect = VK_IMAGE_ASPECT_COLOR_BIT; d->base_type = VK_IMAGE_VIEW_TYPE_2D;
        vk_image_init_layout(vk_cmd(), d->image, d->aspect);
    }
    return d;
}

static VkImageView post_view(TexBackend *d) {
    static const VkComponentMapping id = {0};
    return vk_image_view(d, VK_IMAGE_VIEW_TYPE_2D, d->format, 0, 1, 0, 1, id, VK_IMAGE_ASPECT_COLOR_BIT);
}

// SMAA's lookup textures (runtime/nvn/smaa_data.c): 0 = area (160 x 560 R8G8), 1 = search (64 x 16 R8), uploaded once.
static VkImageView smaa_lut(int i) {
    static TexBackend t[2];
    TexBackend *d = &t[i];
    if (!d->image) {
        extern const unsigned char *smaa_area_tex(void);
        extern const unsigned char *smaa_search_tex(void);
        uint32_t w = i ? 64 : 160, h = i ? 16 : 560, bpp = i ? 1 : 2;
        VkFormat f = i ? VK_FORMAT_R8_UNORM : VK_FORMAT_R8G8_UNORM;
        VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = f,
                                .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                                .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        VK_CHECK(vmaCreateImage(g_vk.vma, &ci, &ai, &d->image, &d->mem, NULL));
        d->format = f; d->width = w; d->height = h; d->depth = 1; d->layers = 1; d->levels = 1;
        d->samples = 1; d->aspect = VK_IMAGE_ASPECT_COLOR_BIT; d->base_type = VK_IMAGE_VIEW_TYPE_2D;
        VkCommandBuffer cb = vk_cmd();
        vk_image_init_layout(cb, d->image, d->aspect);
        VkSpan s = vk_transient_copy(i ? smaa_search_tex() : smaa_area_tex(), (uint64_t)w * h * bpp, 16);
        VkBufferImageCopy c = {.bufferOffset = s.off, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {w, h, 1}};
        vkCmdCopyBufferToImage(cb, s.buf, d->image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    }
    return post_view(d);
}

// One post pass (outside the window image's rendering): srcs (bindings 0..n-1) -> dst, fragment shader `fs`.
// clear: start from zero (SMAA's edge pass only writes edge pixels; its weights / output read them).
static VkRect2D g_post_area;   // post_pass draws only here when set (the presented crop of the image)
static void post_pass(VkCommandBuffer cb, const char *fs, const VkImageView *srcs, int n, TexBackend *dst, const float pc[8],
                      bool clear) {
    vk_feedback_epoch_bump();   // (our own pass: its target may be a guest image a self-write draw samples)
    VkPipeline pipe = post_pipeline(fs);  // (before the rendering begins: created inside it, the NVIDIA driver crashed)
    gp_mark(cb, "post %s %ux%u", fs, dst->width, dst->height);
    vk_full_barrier(cb);
    VkRenderingAttachmentInfo ca = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = post_view(dst),
                                    .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
                                    .loadOp = clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                    .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, {dst->width, dst->height}},
                          .layerCount = 1, .colorAttachmentCount = 1, .pColorAttachments = &ca};
    if (g_post_area.extent.width) ri.renderArea = g_post_area;
    vkCmdBeginRendering(cb, &ri);
    VkDescriptorSet ds = vk_alloc_set(post_set_layout);
    VkDescriptorImageInfo ii[3];
    VkWriteDescriptorSet w[3];
    for (int i = 0; i < n; i++) {
        ii[i] = (VkDescriptorImageInfo){vk_default_sampler(true), srcs[i], VK_IMAGE_LAYOUT_GENERAL};
        w[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = (uint32_t)i,
                                      .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                      .pImageInfo = &ii[i]};
    }
    vkUpdateDescriptorSets(g_vk.dev, (uint32_t)n, w, 0, NULL);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    dyn_invalidate();  // (viewport / scissor only dynamic: the rest of the dynamic state is undefined after this)
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, post_layout, 0, 1, &ds, 0, NULL);
    VkViewport vp = {0, 0, (float)dst->width, (float)dst->height, 0, 1};
    VkRect2D sc = g_post_area.extent.width ? g_post_area : (VkRect2D){{0, 0}, {dst->width, dst->height}};
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &sc);
    vkCmdPushConstants(cb, post_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32, pc);
    vkCmdDraw(cb, 3, 1, 0, 0);
    vkCmdEndRendering(cb);
}

// Live apply of the post-process rows: graphics.antialiasing, graphics.window_upscaler and graphics.sharpening take effect
// from the next presented frame, with the same values as their env variables (TOTK_AA smaa | none, TOTK_UPSCALE fsr |
// none, TOTK_SHARPEN <stops> | "" = off). NULL leaves that one as it is. Any thread (the F10 menu on the render thread,
// the game's Options on the game thread): relaxed atomic stores, post_process loads each once per frame. The SMAA tables
// upload on their first use, the FSR / RCAS passes are per-frame: nothing to rebuild.
static float g_sharpen = -2;   // (-2: not read yet; -1: off)
void renderer_apply_post(const char *aa, const char *upscale, const char *sharpen) {
    if (aa) __atomic_store_n(&g_settings.antialias, !strcmp(aa, "smaa") ? AA_SMAA : AA_OFF, __ATOMIC_RELAXED);
    if (upscale) __atomic_store_n(&g_settings.upscaler, !strcmp(upscale, "fsr") ? UPSCALE_FSR1 : UPSCALE_BILINEAR, __ATOMIC_RELAXED);
    if (sharpen) { float v = *sharpen ? (float)atof(sharpen) : -1; __atomic_store(&g_sharpen, &v, __ATOMIC_RELAXED); }
    LOG("[post] live: antialiasing %s, upscaler %s, sharpening %.2f\n", g_settings.antialias == AA_SMAA ? "smaa" : "none",
        g_settings.upscaler == UPSCALE_FSR1 ? "fsr" : "none", g_sharpen < 0 ? -1.0 : g_sharpen);
}
// The view the present samples: the game image, or its anti-aliased / upscaled version. ow x oh: the whole image's
// size in the window (its crop is drawn into the viewport).
static VkImageView post_process(VkCommandBuffer cb, TexBackend *b, double ow_d, double oh_d, const float crop[4]) {
    VkImageView v = raw_view(b);
    bool smaa = __atomic_load_n(&g_settings.antialias, __ATOMIC_RELAXED) == AA_SMAA;   // (renderer_apply_post: any thread)
    uint32_t ow = (uint32_t)lround(ow_d), oh = (uint32_t)lround(oh_d);
    if (ow > 16384) ow = 16384;
    if (oh > 16384) oh = 16384;
    bool fsr = __atomic_load_n(&g_settings.upscaler, __ATOMIC_RELAXED) == UPSCALE_FSR1 && (ow > b->width + 1 || oh > b->height + 1);  // (an upscale only)
    // TOTK_SHARPEN=<stops> (Graphics menu "Sharpening"): FSR 1 RCAS on the presented image, also without an upscale (the
    // Deck shows the 1280x720 canvas 1:1, where the FSR path above never runs). Includes the HUD: the game composites it
    // into the canvas before we see it. Unset: off, and the FSR path keeps AMD's default 0.2.
    if (g_sharpen == -2) { const char *e = getenv("TOTK_SHARPEN"); g_sharpen = e && *e ? (float)atof(e) : -1; }
    float sharpen;   // (renderer_apply_post changes it live, from any thread)
    __atomic_load(&g_sharpen, &sharpen, __ATOMIC_RELAXED);
    bool rcas = sharpen >= 0 && !fsr;
    if (!smaa && !fsr && !rcas) return v;
    extern void vk_frame_suspend(void);
    extern void vk_frame_resume(void);
    vk_frame_suspend();
    if (smaa) {  // SMAA 1x: luma edges -> blending weights -> neighbourhood blending
        float w = (float)b->width, h = (float)b->height;
        float m[8] = {1.0f / w, 1.0f / h, w, h};
        TexBackend *edges = post_image(0, b->width, b->height), *weights = post_image(1, b->width, b->height);
        TexBackend *out = post_image(2, b->width, b->height);
        VkImageView in[3] = {v};
        post_pass(cb, "smaa_edge.frag", in, 1, edges, m, true);
        VkImageView wi[3] = {post_view(edges), smaa_lut(0), smaa_lut(1)};
        post_pass(cb, "smaa_weight.frag", wi, 3, weights, m, true);
        VkImageView bi[3] = {v, post_view(weights)};
        post_pass(cb, "smaa_blend.frag", bi, 2, out, m, false);
        v = post_view(out);
    }
    if (fsr) {
        float iw = (float)b->width, ih = (float)b->height;
        TexBackend *e = post_image(3, ow, oh), *r = post_image(4, ow, oh);
        float pe[8] = {iw / ow, ih / oh, 0.5f * iw / ow - 0.5f, 0.5f * ih / oh - 0.5f, iw, ih};
        VkImageView ei[3] = {v};
        post_pass(cb, "easu.frag", ei, 1, e, pe, false);
        float pr[8] = {exp2f(-(sharpen >= 0 ? sharpen : 0.2f)), 0, 0, 0, (float)ow, (float)oh};  // (0.2 stops: AMD's default)
        VkImageView ri_[3] = {post_view(e)};
        post_pass(cb, "rcas.frag", ri_, 1, r, pr, false);
        v = post_view(r);
    }
    if (rcas) {   // at the window's size when that is smaller than the image (sharpen what is seen, not what is shrunk)
        uint32_t rw = ow && ow < b->width ? ow : b->width, rh = oh && oh < b->height ? oh : b->height;
        TexBackend *r = post_image(4, rw, rh);
        float pr[8] = {exp2f(-sharpen), rw < b->width || rh < b->height ? 1.0f : 0.0f, 0, 0, (float)b->width, (float)b->height,
                       (float)rw, (float)rh};
        VkImageView ri_[3] = {v};
        // only the presented crop (Deck handheld: the 1280x720 corner of a 1920x1080 image), +1 texel for the bilinear edge
        int32_t x0 = (int32_t)(crop[0] * rw), y0 = (int32_t)(crop[1] * rh);
        uint32_t cw = (uint32_t)ceilf(crop[2] * rw) + 1, ch = (uint32_t)ceilf(crop[3] * rh) + 1;
        g_post_area = (VkRect2D){{x0, y0}, {cw < rw - x0 ? cw : rw - x0, ch < rh - y0 ? ch : rh - y0}};
        post_pass(cb, "rcas.frag", ri_, 1, r, pr, false);
        g_post_area = (VkRect2D){0};
        v = post_view(r);
    }
    vk_full_barrier(cb);
    vk_frame_resume();
    {
        static uint32_t piw, pih, pow_, poh;
        static int psm = -1, pfs = -1, prc = -1;
        if (b->width != piw || b->height != pih || ow != pow_ || oh != poh || smaa != psm || fsr != pfs || rcas != prc)
            LOG("[post] %ux%u%s%s%s -> %ux%u\n", b->width, b->height, smaa ? " smaa" : "", fsr ? " fsr1" : "", rcas ? " rcas" : "", ow, oh);
        piw = b->width; pih = b->height; pow_ = ow; poh = oh; psm = smaa; pfs = fsr; prc = rcas;
    }
    return v;
}

// Inside the window image's rendering (device.c vk_frame_begin): the game image into (vx, vy, vw, vh).
void vk_present_draw(VkCommandBuffer cb, TexBackend *b, float vx, float vy, float vw, float vh, const float pv[8]) {
    if (!b || !b->image || (b->aspect & ~VK_IMAGE_ASPECT_COLOR_BIT)) return;
    VkImageView src = post_process(cb, b, pv[6] > 0 ? vw / pv[6] : vw, pv[7] > 0 ? vh / pv[7] : vh, pv + 4);
    VkPipeline p = present_pipeline(g_vk.sc_format);
    VkDescriptorSet ds = vk_alloc_set(present_set_layout);
    VkDescriptorImageInfo ii = {vk_default_sampler(true), src, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet w = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 0, .descriptorCount = 1,
                              .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii};
    vkUpdateDescriptorSets(g_vk.dev, 1, &w, 0, NULL);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    dyn_invalidate();
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, present_layout, 0, 1, &ds, 0, NULL);
    VkViewport vp = {vx, vy, vw, vh, 0, 1};
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdPushConstants(cb, present_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, 32, pv);
    gp_mark(cb, "present draw %.0fx%.0f", (double)vw, (double)vh);
    vkCmdDraw(cb, 3, 1, 0, 0);
    VkViewport full = {0, 0, (float)g_vk.sc_extent.width, (float)g_vk.sc_extent.height, 0, 1};
    vkCmdSetViewport(cb, 0, 1, &full);
}

// Screenshot of a game texture (presented crop applied to build/frame_* shots), via the present shader into RGBA8.
void renderer_save_texture_tex(TexBackend *b, const char *path) {
    if (!b || !b->image || (b->aspect & ~VK_IMAGE_ASPECT_COLOR_BIT)) return;
    extern const float *vk_shot_crop(void);
    float crop[4] = {0, 0, 1, 1};
    if (strstr(path, "frame_")) memcpy(crop, vk_shot_crop(), sizeof crop);
    uint32_t w = (uint32_t)(b->width * crop[2]), h = (uint32_t)(b->height * crop[3]);
    if (!w || !h) return;
    vk_end_pass();
    // pass dumps go through RGBA32F: the PNG is clamped, the [passstat] line has the real values (HDR, NaN)
    bool fl = strstr(path, "pass_") != NULL;
    VkFormat ifmt = fl ? VK_FORMAT_R32G32B32A32_SFLOAT : VK_FORMAT_R8G8B8A8_UNORM;
    uint32_t bpp = fl ? 16 : 4;
    VkImage img;
    VmaAllocation mem;
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = ifmt,
                            .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    VK_CHECK(vmaCreateImage(g_vk.vma, &ci, &ai, &img, &mem, NULL));
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);
    vk_image_init_layout(cb, img, VK_IMAGE_ASPECT_COLOR_BIT);
    VkImageViewCreateInfo vci = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = img, .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                 .format = ifmt, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkImageView tv;
    VK_CHECK(vkCreateImageView(g_vk.dev, &vci, NULL, &tv));
    VkRenderingAttachmentInfo ca = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = tv, .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
                                    .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, {w, h}}, .layerCount = 1,
                          .colorAttachmentCount = 1, .pColorAttachments = &ca};
    vkCmdBeginRendering(cb, &ri);
    VkPipeline p = present_pipeline(ifmt);
    VkDescriptorSet ds = vk_alloc_set(present_set_layout);
    VkDescriptorImageInfo ii = {vk_default_sampler(true), raw_view(b), VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet wr = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 0, .descriptorCount = 1,
                               .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii};
    vkUpdateDescriptorSets(g_vk.dev, 1, &wr, 0, NULL);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    dyn_invalidate();
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, present_layout, 0, 1, &ds, 0, NULL);
    VkViewport vp = {0, 0, (float)w, (float)h, 0, 1};
    VkRect2D sc = {{0, 0}, {w, h}};
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &sc);
    float pv[8] = {renderer_origin_lower_left() && !strstr(path, "window") ? 1.0f : 0.0f, 0, 0, 0, crop[0], crop[1], crop[2], crop[3]};
    vkCmdPushConstants(cb, present_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, 32, pv);
    vkCmdDraw(cb, 3, 1, 0, 0);
    vkCmdEndRendering(cb);
    vk_full_barrier(cb);
    VkSpan out = vk_readback((uint64_t)w * h * bpp);
    VkBufferImageCopy c = {.bufferOffset = out.off, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {w, h, 1}};
    vkCmdCopyImageToBuffer(cb, img, VK_IMAGE_LAYOUT_GENERAL, out.buf, 1, &c);
    renderer_flush();
    if (fl) {
        const float *px = (const float *)out.ptr;
        uint8_t *rgba = malloc((size_t)w * h * 4);
        double sum[4] = {0};
        float mx[4] = {0};
        uint64_t bad = 0, nz = 0;
        char where[160] = "";  // first NaN/Inf pixels (painted magenta in the PNG)
        int nwhere = 0;
        for (uint64_t i = 0; i < (uint64_t)w * h; i++) {
            bool any = false, badpx = false;
            for (int c = 0; c < 4; c++) {
                float v = px[i * 4 + c];
                if (v != v || v > 3.0e38f || v < -3.0e38f) { bad++; v = 0; badpx = true; }
                sum[c] += v;
                if (v > mx[c]) mx[c] = v;
                if (c < 3 && v != 0) any = true;
                rgba[i * 4 + c] = (uint8_t)(v <= 0 ? 0 : v >= 1 ? 255 : v * 255.0f + 0.5f);
            }
            if (badpx) {
                rgba[i * 4] = 255; rgba[i * 4 + 1] = 0; rgba[i * 4 + 2] = 255; rgba[i * 4 + 3] = 255;
                if (!nwhere && probe_auto_x < 0 && strstr(path, "pass_")) {
                    probe_auto_x = (int)(i % w); probe_auto_y = (int)(i / w); probe_auto_frame = renderer_present_count();
                }
                if (nwhere < 6) { size_t l = strlen(where); snprintf(where + l, sizeof where - l, " %llu,%llu", (unsigned long long)(i % w), (unsigned long long)(i / w)); nwhere++; }
            }
            nz += any;
        }
        double n = (double)w * h;
        LOG("[passstat] %s %ux%u mean %.4g %.4g %.4g %.4g max %.4g %.4g %.4g %.4g nonzero %.1f%% naninf %llu%s%s\n", path, w, h,
            sum[0] / n, sum[1] / n, sum[2] / n, sum[3] / n, mx[0], mx[1], mx[2], mx[3], 100.0 * nz / n, (unsigned long long)bad,
            nwhere ? " at" : "", where);
        vk_save_png(path, rgba, w, h);
        free(rgba);
    } else {
        vk_save_png(path, out.ptr, w, h);
    }
    vkDestroyImageView(g_vk.dev, tv, NULL);
    vmaDestroyImage(g_vk.vma, img, mem);
}

// ---- LARECOMP_REUSE_STATS=1: how much of the guest's command stream repeats from frame to frame. A guest pass = the
// records from one SetRenderTargets to the next (state bound before it counts in the pass before); its key = that record's
// bytes + its occurrence in the frame. Two hashes per pass: exact (every record's bytes, deferred texture handles read from
// guest memory) and no-UBO (uniform-buffer addresses and inline uniform data masked: what would still differ if only the
// uniforms change). Each is matched against the same key's hashes in the last 4 frames. The render thread's time is
// credited to the pass a record belongs to (rdtsc from one record to the next inside a list; a list's last record goes
// uncounted). Logged every 240 frames as "[reuse]".
static int reuse_stats_on(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("LARECOMP_REUSE_STATS"); on = e && *e == '1'; if (on) LOG("[reuse] command reuse stats on\n"); }
    return on;
}
#define RU_TAB 16384
static struct { uint64_t key, hx[4], hs[4], fr[4]; } ru_tab[RU_TAB];
static struct {
    uint64_t frame, key, hx, hs, ticks, t_last, draws;
    uint32_t nocc; struct { uint64_t k; uint32_t n; } occ[2048];
    uint64_t w_frames, w_passes, w_draws, w_ticks, w_ns0, w_tsc0, w_frame0, w_full;
    uint64_t w_p[4], w_d[4], w_t[4];   // passes / draws / ticks matched: [0] exact, previous frame; [1] exact, one of the last 4;
                                       // [2] no-UBO, previous frame; [3] no-UBO, one of the last 4
} ru;
static inline uint64_t ru_mix(uint64_t h, uint64_t w) { h = (h ^ w) * 0x9E3779B97F4A7C15ull; return h ^ (h >> 29); }
static void ru_pass_end(void) {
    if (!ru.key) return;
    uint64_t f = ru.frame, key = ru.key;
    uint32_t h = (uint32_t)(key * 0x9E3779B1u) & (RU_TAB - 1), i = h;
    for (int k = 0; k < 32; k++) {   // (linear probe; a slot unused for 8 frames is free)
        uint32_t j = (h + (uint32_t)k) & (RU_TAB - 1);
        uint64_t last = ru_tab[j].fr[0] > ru_tab[j].fr[1] ? ru_tab[j].fr[0] : ru_tab[j].fr[1];
        last = last > ru_tab[j].fr[2] ? last : ru_tab[j].fr[2]; last = last > ru_tab[j].fr[3] ? last : ru_tab[j].fr[3];
        if (ru_tab[j].key == key) { i = j; break; }
        if (!ru_tab[j].key || last + 8 < f) { i = j; memset(&ru_tab[j], 0, sizeof ru_tab[j]); ru_tab[j].key = key; break; }
        if (k == 31) { ru.w_full++; i = h; memset(&ru_tab[h], 0, sizeof ru_tab[h]); ru_tab[h].key = key; }
    }
    bool m[4] = {0};
    for (uint64_t a = 1; a <= 4 && a <= f; a++) {
        uint32_t s = (uint32_t)((f - a) & 3);
        if (ru_tab[i].fr[s] != f - a) continue;
        if (ru_tab[i].hx[s] == ru.hx) { m[1] = true; if (a == 1) m[0] = true; }
        if (ru_tab[i].hs[s] == ru.hs) { m[3] = true; if (a == 1) m[2] = true; }
    }
    uint32_t s = (uint32_t)(f & 3);
    ru_tab[i].hx[s] = ru.hx; ru_tab[i].hs[s] = ru.hs; ru_tab[i].fr[s] = f;
    ru.w_passes++; ru.w_draws += ru.draws; ru.w_ticks += ru.ticks;
    for (int k = 0; k < 4; k++) if (m[k]) { ru.w_p[k]++; ru.w_d[k] += ru.draws; ru.w_t[k] += ru.ticks; }
    ru.key = 0;
}
static void ru_log(void) {
    uint64_t ns = SDL_GetTicksNS(), tsc = __rdtsc();
    double nspt = tsc > ru.w_tsc0 ? (double)(ns - ru.w_ns0) / (double)(tsc - ru.w_tsc0) : 0, fr = ru.w_frames ? (double)ru.w_frames : 1;
    double pp = ru.w_passes ? 100.0 / ru.w_passes : 0, dd = ru.w_draws ? 100.0 / ru.w_draws : 0, tt = ru.w_ticks ? 100.0 / ru.w_ticks : 0;
    LOG("[reuse] %llu frames: %.0f passes, %.0f draws, %.3f ms of the render thread a frame in passes | exact, previous frame: "
        "%.1f %% of passes, %.1f %% of draws, %.3f ms | exact, last 4: %.1f / %.1f %%, %.3f ms | no-UBO, previous: %.1f / %.1f %%, "
        "%.3f ms | no-UBO, last 4: %.1f / %.1f %%, %.3f ms%s\n",
        (unsigned long long)ru.w_frames, ru.w_passes / fr, ru.w_draws / fr, ru.w_ticks * nspt / 1e6 / fr,
        ru.w_p[0] * pp, ru.w_d[0] * dd, ru.w_t[0] * nspt / 1e6 / fr, ru.w_p[1] * pp, ru.w_d[1] * dd, ru.w_t[1] * nspt / 1e6 / fr,
        ru.w_p[2] * pp, ru.w_d[2] * dd, ru.w_t[2] * nspt / 1e6 / fr, ru.w_p[3] * pp, ru.w_d[3] * dd, ru.w_t[3] * nspt / 1e6 / fr,
        ru.w_full ? " (pass table full: some passes unmatched)" : "");
    (void)tt;
    ru.w_frames = ru.w_passes = ru.w_draws = ru.w_ticks = ru.w_full = 0;
    memset(ru.w_p, 0, sizeof ru.w_p); memset(ru.w_d, 0, sizeof ru.w_d); memset(ru.w_t, 0, sizeof ru.w_t);
    ru.w_ns0 = ns; ru.w_tsc0 = tsc;
}
static void ru_record(const CmdRec *r, int depth, bool list_start) {
    uint64_t t = __rdtsc();
    if (ru.t_last && !list_start) ru.ticks += t - ru.t_last;
    ru.t_last = t;
    (void)depth;
    const uint64_t *w = (const uint64_t *)r;
    uint32_t nw = r->size / 8;
    if (r->op == CMD_SET_RENDER_TARGETS) {
        ru_pass_end();
        uint64_t f = renderer_present_count();
        if (f != ru.frame) {
            if (ru.frame) ru.w_frames += f - ru.frame < 4 ? f - ru.frame : 1;
            ru.frame = f; ru.nocc = 0;
            if (!ru.w_tsc0) { ru.w_ns0 = SDL_GetTicksNS(); ru.w_tsc0 = t; }
            if (ru.w_frames >= 240) ru_log();
        }
        uint64_t k = 0x51ED27;
        for (uint32_t i = 0; i < nw; i++) k = ru_mix(k, w[i]);
        uint32_t n = 0, j = 0;
        for (; j < ru.nocc; j++) if (ru.occ[j].k == k) { n = ++ru.occ[j].n; break; }
        if (j == ru.nocc && ru.nocc < 2048) { ru.occ[ru.nocc].k = k; ru.occ[ru.nocc++].n = 0; }
        ru.key = ru_mix(k, n) | 1;
        ru.hx = ru.hs = 0; ru.ticks = 0; ru.draws = 0;
        return;
    }
    if (r->op == CMD_CALL || r->op == CMD_JUMP || r->op == CMD_END) return;
    if (r->op == CMD_DRAW_ARRAYS || r->op == CMD_DRAW_ELEMENTS) ru.draws++;
    uint64_t hx = ru.hx, hs = ru.hs;
    for (uint32_t i = 0; i < nw; i++) hx = ru_mix(hx, w[i]);
    if (r->op == CMD_BIND_TEXTURES_DEFERRED && r->x[3])
        for (uint64_t i = 0; i < r->x[2] && i < 32; i++) hx = ru_mix(hx, ((const uint64_t *)(uintptr_t)r->x[3])[i]);
    if (r->op == CMD_UPDATE_UNIFORM_BUFFER) hs = ru_mix(ru_mix(hs, r->op), r->x[3]);
    else if (r->op == CMD_BIND_UNIFORM_BUFFERS) {
        hs = ru_mix(ru_mix(ru_mix(ru_mix(hs, r->op), r->x[0]), r->x[1]), r->x[2]);
        for (uint64_t i = 0; i < r->x[2] && i < 18; i++) hs = ru_mix(hs, ((const uint64_t *)r->extra)[i * 2 + 1]);   // (sizes)
    } else {
        for (uint32_t i = 0; i < nw; i++) hs = ru_mix(hs, w[i]);
        if (r->op == CMD_BIND_TEXTURES_DEFERRED && r->x[3])
            for (uint64_t i = 0; i < r->x[2] && i < 32; i++) hs = ru_mix(hs, ((const uint64_t *)(uintptr_t)r->x[3])[i]);
    }
    ru.hx = hx; ru.hs = hs;
}

// ================================================================== executor (renderer.m exec_list)
// LARECOMP_INDIRECT_LOG=1: per list, the indirect draws / dispatches (not executed here) and whether their argument
// address falls in a storage range that a global-memory-writing draw or dispatch earlier in the same list had bound: a
// "[indirect]" line every 60 lists
static struct { uint64_t lo, hi; } ind_w[1024];
static int ind_nw, ind_on = -1;
static uint64_t ind_n[3], ind_hit[3], ind_lists;
static bool ind_log(void) { if (ind_on < 0) { const char *e = getenv("LARECOMP_INDIRECT_LOG"); ind_on = e && *e == '1'; } return ind_on; }
static void ind_note_writes(int stage) {   // (a writing draw / dispatch: every storage range its stage has bound)
    for (int i = 0; i < 16 && ind_nw < 1024; i++)
        if (st.sbuf[stage][i].addr && st.sbuf[stage][i].size) { ind_w[ind_nw].lo = st.sbuf[stage][i].addr; ind_w[ind_nw++].hi = st.sbuf[stage][i].addr + st.sbuf[stage][i].size; }
}
static void ind_note_indirect(int kind, uint64_t addr) {   // (0 DrawElementsIndirect, 1 DrawArraysIndirect, 2 DispatchComputeIndirect)
    // (always: the first of each kind is logged, as the stubs are, so they are dropped, not executed, until a scene shows
    // one)
    static bool told[3];
    if (!told[kind]) { told[kind] = true; LOG("[nvn] %s at %llx: not executed\n", kind == 0 ? "DrawElementsIndirect" : kind == 1 ? "DrawArraysIndirect" : "DispatchComputeIndirect", (unsigned long long)addr); }
    if (!ind_log()) return;
    ind_n[kind]++;
    for (int i = 0; i < ind_nw; i++) if (addr >= ind_w[i].lo && addr < ind_w[i].hi) { ind_hit[kind]++; break; }
}
static void exec_list(uint64_t handle, int depth) {
    if (!depth && ind_log()) {
        if (ind_lists && ind_lists % 60 == 0) {
            LOG("[indirect] over 60 lists: DrawElementsIndirect %llu (args in a range written earlier in its list: %llu), DrawArraysIndirect %llu (%llu), "
                "DispatchComputeIndirect %llu (%llu); global-memory-writing draws %llu, dispatches %llu\n",
                (unsigned long long)ind_n[0], (unsigned long long)ind_hit[0], (unsigned long long)ind_n[1], (unsigned long long)ind_hit[1],
                (unsigned long long)ind_n[2], (unsigned long long)ind_hit[2], (unsigned long long)ind_wdraw, (unsigned long long)ind_wdisp);
            memset(ind_n, 0, sizeof ind_n); memset(ind_hit, 0, sizeof ind_hit); ind_wdraw = ind_wdisp = 0;
        }
        ind_lists++; ind_nw = 0;
    }
    uint64_t p = handle;
    if (!depth && oq_ph != oq_pt) oq_poll();
    if (!depth && (dts_ph != dts_pt || dsub_pool)) dts_poll();   // (the DRS clock's GPU timestamps, likewise)   // (occlusion results of completed submissions into their slots)
    const CmdChunk *k = cmd_chunk_for(handle);   // records live in the host shadow of their guest chunk (nvn.h)
    if (!k) { static int n; if (n++ < 8) LOG("[nvn] exec: unknown command handle %llx\n", (unsigned long long)handle); return; }
    // The chunk's base / shadow in locals (k-> was re-read every record), and the records a few slots ahead prefetched.
    const uint8_t *khost = k->host;
    uint64_t kbase = k->base;
    // LARECOMP_EXEC_PREFETCH=<bytes> (default 512): how far ahead (the first read of each record, exec.c op_counts,
    // was 0.18 ms a frame of misses in Kakariko with 512; a distance to A/B)
    static int pfd = -1;
    if (pfd < 0) { const char *e = getenv("LARECOMP_EXEC_PREFETCH"); pfd = e && atoi(e) >= 64 ? atoi(e) & ~63 : 512; }
    const int ru_on = reuse_stats_on();
    bool ru_first = true;
    for (int guard = 0; guard < 10000000; guard++) {
        CmdRec *r = (CmdRec *)(khost + (p - kbase) * CMD_SHADOW_K);
        __builtin_prefetch((const char *)r + pfd, 0, 3);
        __builtin_prefetch((const char *)r + pfd + 64, 0, 3);
        void *x = r->extra;
        if (r->op < CMD_COUNT) op_counts[r->op]++;
        if (ru_on) { ru_record(r, depth, ru_first && !depth); ru_first = false; }
        static int skip_all = -1;   // LARECOMP_SKIP_ALL_DRAWS=1: no draws or clears at all (upper bound: fps with exec ~free)
        if (skip_all < 0) skip_all = getenv("LARECOMP_SKIP_ALL_DRAWS") != NULL;
        if ((g_render_off || skip_all) && (r->op == CMD_DRAW_ARRAYS || r->op == CMD_DRAW_ELEMENTS || r->op == CMD_CLEAR_COLOR ||
                             r->op == CMD_CLEAR_DEPTH_STENCIL || r->op == CMD_CLEAR_TEXTURE || r->op == CMD_DRAW_TEXTURE)) {
            g_cond_skipped++;
            if (g_debug_frame) LOG("[cond] op %u skipped (render disabled)\n", r->op);
            p += cmd_gsize(r->size);
            continue;
        }
        if (prof > 1 && op_t0) { op_ns[op_last] += prof_now() - op_t0; op_t0 = 0; }   // (LARECOMP_VK_PROF=2) time per command kind
        if (prof > 1 && r->op != CMD_CALL && r->op != CMD_JUMP && r->op != CMD_END && r->op < CMD_COUNT) { op_last = r->op; op_t0 = prof_now(); }
        // every command that writes state stage_set reads (bindings, pools, program, render targets, inline UBO data) (bindings
        // / pools / program are compared by content in stage_set: the game re-issues identical binds per draw)
        if (r->op == CMD_SET_RENDER_TARGETS || r->op == CMD_CLEAR_TEXTURE || r->op == CMD_UPDATE_UNIFORM_BUFFER) g_set_gen++;
        switch (r->op) {
        case CMD_END: return;
        case CMD_JUMP:
            p = r->x[0];
            if (!(k = cmd_chunk_for(p))) { LOG("[nvn] exec: JUMP to unknown chunk %llx\n", (unsigned long long)p); return; }
            khost = k->host;
            kbase = k->base;
            continue;
        case CMD_CALL: {
            const uint64_t *h = x;
            for (uint64_t i = 0; i < r->x[0] && depth < 8; i++) exec_list(h[i], depth + 1);
            break;
        }
        case CMD_BIND_PROGRAM: if (st.program != (NvnProgram *)(uintptr_t)r->x[0]) bind_gen_all(); st.program = (NvnProgram *)(uintptr_t)r->x[0]; break;
        case CMD_BIND_BLEND: {
            uint8_t *b = x;
            if (memcmp(st.blend[b[0] & 7], b, 8) && (!pk_narrow() || (st.color_state[0] >> (b[0] & 7)) & 1)) PK_BUMP(PKB_BLEND);
            memcpy(st.blend[b[0] & 7], b, 8);
            break;
        }
        case CMD_BIND_CHANNEL_MASK: if (memcmp(&st.channel_mask, x, 4)) PK_BUMP(PKB_CHANNEL); memcpy(&st.channel_mask, x, 4); break;
        case CMD_BIND_COLOR:
            if (pk_narrow() ? st.color_state[0] != ((uint8_t *)x)[0] : memcmp(st.color_state, x, 4) != 0) PK_BUMP(PKB_COLOR);
            memcpy(st.color_state, x, 4);
            break;
        case CMD_BIND_DEPTH_STENCIL: memcpy(st.ds, x, 8); break;
        case CMD_BIND_MULTISAMPLE:
            if (pk_narrow() ? st.ms[0xC] != ((uint8_t *)x)[0xC] || st.ms[0xD] != ((uint8_t *)x)[0xD] : memcmp(st.ms, x, 0x18) != 0) PK_BUMP(PKB_MS);
            memcpy(st.ms, x, 0x18);
            break;
        case CMD_BIND_POLYGON:
            if (pk_narrow() ? st.poly[2] != ((uint8_t *)x)[2] : memcmp(st.poly, x, 4) != 0) PK_BUMP(PKB_POLY);
            memcpy(st.poly, x, 4);
            break;
        case CMD_BIND_VERTEX_ATTRIB: {
            int n = (int)r->x[0] > 16 ? 16 : (int)r->x[0];
            if (n != st.nattrib || memcmp(st.attrib, x, (size_t)n * 4)) PK_BUMP(PKB_ATTRIB);
            st.nattrib = n;
            memcpy(st.attrib, x, st.nattrib * 4);
            break;
        }
        case CMD_BIND_VERTEX_STREAM: {
            int n = (int)r->x[0] > 16 ? 16 : (int)r->x[0];
            if (n != st.nstream || memcmp(st.stream, x, (size_t)n * 8)) PK_BUMP(PKB_STREAM);
            st.nstream = n;
            memcpy(st.stream, x, st.nstream * 8);
            break;
        }
        case CMD_BIND_VERTEX_BUFFERS:
            for (uint64_t i = 0; i < r->x[1] && r->x[0] + i < 16; i++) memcpy(&st.vbuf[r->x[0] + i], (uint8_t *)x + i * 16, 16);
            break;
        case CMD_BIND_UNIFORM_BUFFERS:
            for (uint64_t i = 0; i < r->x[2] && r->x[1] + i < 18 && r->x[0] < 6; i++) {
                if (memcmp(&st.ubuf[r->x[0]][r->x[1] + i], (uint8_t *)x + i * 16, 16)) g_bind_gen[r->x[0]]++;
                memcpy(&st.ubuf[r->x[0]][r->x[1] + i], (uint8_t *)x + i * 16, 16);
            }
            break;
        case CMD_BIND_STORAGE_BUFFERS:
            for (uint64_t i = 0; i < r->x[2] && r->x[1] + i < 16 && r->x[0] < 6; i++) {
                if (memcmp(&st.sbuf[r->x[0]][r->x[1] + i], (uint8_t *)x + i * 16, 16)) g_bind_gen[r->x[0]]++, g_tsb_gen[r->x[0]]++;
                memcpy(&st.sbuf[r->x[0]][r->x[1] + i], (uint8_t *)x + i * 16, 16);
            }
            break;
        case CMD_BIND_TEXTURES:
            for (uint64_t i = 0; i < r->x[2] && r->x[1] + i < 32 && r->x[0] < 6; i++) {
                if (memcmp(&st.tex[r->x[0]][r->x[1] + i], (uint8_t *)x + i * 8, 8)) g_bind_gen[r->x[0]]++, g_tsb_gen[r->x[0]]++;
                memcpy(&st.tex[r->x[0]][r->x[1] + i], (uint8_t *)x + i * 8, 8);
            }
            break;
        case CMD_BIND_TEXTURES_DEFERRED:   // handles read now, from guest memory (identity-mapped)
            for (uint64_t i = 0; i < r->x[2] && r->x[1] + i < 32 && r->x[0] < 6 && r->x[3]; i++) {
                if (memcmp(&st.tex[r->x[0]][r->x[1] + i], (const uint8_t *)(uintptr_t)r->x[3] + i * 8, 8)) g_bind_gen[r->x[0]]++, g_tsb_gen[r->x[0]]++;
                memcpy(&st.tex[r->x[0]][r->x[1] + i], (const uint8_t *)(uintptr_t)r->x[3] + i * 8, 8);
            }
            break;
        case CMD_BIND_IMAGES:
            for (uint64_t i = 0; i < r->x[2] && r->x[1] + i < 8 && r->x[0] < 6; i++)
                memcpy(&st.img[r->x[0]][r->x[1] + i], (uint8_t *)x + i * 8, 8);
            break;
        case CMD_SET_TEXTURE_POOL: if (st.tpool != (NvnDescPool *)(uintptr_t)r->x[0]) bind_gen_all(); st.tpool = (NvnDescPool *)(uintptr_t)r->x[0]; break;
        case CMD_SET_SAMPLER_POOL: if (st.spool != (NvnDescPool *)(uintptr_t)r->x[0]) bind_gen_all(); st.spool = (NvnDescPool *)(uintptr_t)r->x[0]; break;
        case CMD_SET_RENDER_TARGETS: {
            if (!pk_narrow()) PK_BUMP(PKB_RT);   // (the targets' view formats feed write masks and blend: LARECOMP_PK_FAST)
            const struct { uint64_t tex[8]; NvnTextureView views[8]; uint8_t has_view[8]; uint64_t depth;
                           NvnTextureView dview; uint8_t has_dview; } *rt = x;
            RtState prev = st.rt;  // the same targets again: keep the pass (a restart = full barrier + reload)
            memset(&st.rt, 0, sizeof st.rt);
            st.rt.ncolor = (int)r->x[0] > 8 ? 8 : (int)r->x[0];
            for (int i = 0; i < st.rt.ncolor; i++) {
                st.rt.color[i] = (NvnTexture *)(uintptr_t)rt->tex[i];
                st.rt.cview[i] = rt->views[i];
                st.rt.has_cview[i] = rt->has_view[i];
            }
            st.rt.depth = (NvnTexture *)(uintptr_t)rt->depth;
            st.rt.dview = rt->dview;
            st.rt.has_dview = rt->has_dview;
            static int always = -1;
            if (always < 0) always = getenv("LARECOMP_VK_PASS_RESTART") != NULL;
            bool rt_new = memcmp(&prev, &st.rt, sizeof prev) != 0;
            if (rt_new && pk_narrow()) PK_BUMP(PKB_RT);
            if (always || rt_new) st.rt_dirty = true;
            break;
        }
        case CMD_SET_VIEWPORTS:
            for (uint64_t i = 0; i < r->x[1] && r->x[0] + i < 16; i++) memcpy(st.viewport[r->x[0] + i], (uint8_t *)x + i * 16, 16);
            break;
        case CMD_SET_SCISSORS:
            for (uint64_t i = 0; i < r->x[1] && r->x[0] + i < 16; i++) memcpy(st.scissor[r->x[0] + i], (uint8_t *)x + i * 16, 16);
            break;
        case CMD_SET_DEPTH_RANGES:
            for (uint64_t i = 0; i < r->x[1] && r->x[0] + i < 16; i++) memcpy(st.depth_range[r->x[0] + i], (uint8_t *)x + i * 8, 8);
            break;
        case CMD_SET_BLEND_COLOR: memcpy(st.blend_color, x, 16); break;
        case CMD_SET_STENCIL: {
            int face = (int)r->x[0];
            for (int f = 0; f < 2; f++) {
                if (!(face & (1 << f))) continue;
                if (r->x[6] == 0) st.stencil_ref[f] = (uint32_t)r->x[1];
                else if (r->x[6] == 1) st.stencil_mask[f] = (uint32_t)r->x[1];
                else st.stencil_vmask[f] = (uint32_t)r->x[1];
            }
            break;
        }
        case CMD_SET_POLYGON_OFFSET: memcpy(st.poly_offset, r->f, sizeof st.poly_offset); break;
        case CMD_SET_DEPTH_CLAMP: {
            static int off = -1;
            if (off < 0) off = getenv("LARECOMP_NO_DEPTH_CLAMP") != NULL;
            bool dc = r->x[0] != 0 && !off;
            if (!pk_narrow() || dc != st.depth_clamp) PK_BUMP(PKB_CLAMP);
            st.depth_clamp = dc;
            break;
        }
        case CMD_SET_PRIMITIVE_RESTART: st.prim_restart = r->x[0] != 0; st.restart_index = (uint32_t)r->x[1]; break;
        case CMD_SET_COPY_STRIDES: st.copy_row_stride = r->x[0]; st.copy_image_stride = r->x[1]; break;
        case CMD_CLEAR_COLOR: {
            snap_reason = "clear-color"; snapshots_invalidate_(NULL, false);
            int index = (int)r->x[0];
            // LARECOMP_SKIP_CCLEAR=<addr hex>:<n>[,<n>...][;<addr>:...]: skip the n-th clears (1-based, per frame) of those
            // textures (diagnostic A/B)
            static uint64_t skip_addr[8];
            static uint32_t skip_mask[8], skip_count[8];
            static int skip_init, nskip;
            static uint64_t skip_frame;
            if (!skip_init) {
                skip_init = 1;
                const char *e = getenv("LARECOMP_SKIP_CCLEAR");
                char *p = (char *)e;
                while (p && *p && nskip < 8) {
                    skip_addr[nskip] = strtoull(p, &p, 16);
                    while (*p == ':' || *p == ',') { int n = (int)strtol(p + 1, &p, 10); if (n >= 1 && n <= 32) skip_mask[nskip] |= 1u << (n - 1); }
                    nskip++;
                    if (*p == ';') p++; else break;
                }
            }
            bool skip = false;
            if (nskip && index < st.rt.ncolor && st.rt.color[index]) {
                if (renderer_present_count() != skip_frame) { skip_frame = renderer_present_count(); memset(skip_count, 0, sizeof skip_count); }
                for (int k = 0; k < nskip; k++)
                    if (st.rt.color[index]->addr == skip_addr[k]) {
                        skip_count[k]++;
                        skip = skip_count[k] <= 32 && (skip_mask[k] >> (skip_count[k] - 1) & 1);
                    }
            }
            static int log_clears = -1;
            if (log_clears < 0) log_clears = getenv("LARECOMP_LOG_CLEARS") != NULL;
            if ((g_debug_frame || log_clears) && index < st.rt.ncolor && st.rt.color[index]) {
                const uint32_t *cv = x;
                LOG("[cclear] frame %llu idx %d %p addr %llx %ux%u f%#x mask %#x kind %d color %08x %08x %08x %08x scissor %d %d %d %d"
                    " | view %d set %#x layer %u+%u level %u%s\n",
                    (unsigned long long)renderer_present_count(), index, (void *)st.rt.color[index],
                    (unsigned long long)st.rt.color[index]->addr, st.rt.color[index]->width, st.rt.color[index]->height,
                    st.rt.color[index]->format, (uint32_t)r->x[2], (int)r->x[5], cv[0], cv[1], cv[2], cv[3], st.scissor[0][0],
                    st.scissor[0][1], st.scissor[0][2], st.scissor[0][3], st.rt.has_cview[index],
                    st.rt.has_cview[index] ? st.rt.cview[index].setmask : 0, st.rt.has_cview[index] ? st.rt.cview[index].min_layer : 0,
                    st.rt.has_cview[index] ? st.rt.cview[index].num_layers : 0, st.rt.has_cview[index] ? st.rt.cview[index].base_level : 0,
                    skip ? " SKIPPED" : "");
            }
            if (!skip && index < st.rt.ncolor && st.rt.color[index]) {
                pk_take(st.rt.color[index]);   // (pause capture: keep the canvas before its per-frame clear)
                clear(index, (int)r->x[5], x, (uint32_t)r->x[2], false, 0, false, 0, 0);
            }
            break;
        }
        case CMD_CLEAR_TEXTURE: {
            // ClearTexture(cmd, texture, view, region, color, mask): a render-target clear of the texture alone (renderer.m)
            const struct { uint64_t tex; NvnTextureView view; uint8_t has_view; int32_t region[6]; float color[4]; } *ct = x;
            NvnTexture *t = (NvnTexture *)(uintptr_t)ct->tex;
            if (!t) break;
            snap_reason = "clear-texture"; snapshots_invalidate_(NULL, false);
            RtState saved = st.rt;
            int32_t sc[4];
            memcpy(sc, st.scissor[0], sizeof sc);
            vk_end_pass();
            memset(&st.rt, 0, sizeof st.rt);
            st.rt.ncolor = 1;
            st.rt.color[0] = t;
            if (ct->has_view) { st.rt.cview[0] = ct->view; st.rt.has_cview[0] = 1; }
            st.rt_dirty = true;
            const int32_t *rg = ct->region;
            if (rg[3] > 0 && rg[4] > 0) { st.scissor[0][0] = rg[0]; st.scissor[0][1] = rg[1]; st.scissor[0][2] = rg[3]; st.scissor[0][3] = rg[4]; }
            else { st.scissor[0][0] = st.scissor[0][1] = 0; st.scissor[0][2] = st.scissor[0][3] = 16384; }
            clear(0, (int)r->x[6], ct->color, (uint32_t)r->x[4] ? (uint32_t)r->x[4] : 0xF, false, 0, false, 0, 0);
            vk_end_pass();
            st.rt = saved;
            memcpy(st.scissor[0], sc, sizeof sc);
            st.rt_dirty = true;
            break;
        }
        case CMD_CLEAR_DEPTH_STENCIL:
            snap_reason = "clear-depth"; snapshots_invalidate_(NULL, false);
            if (st.rt.depth) {
                bool has_stencil = renderer_format_has_stencil(st.rt.depth->format);
                if (g_debug_frame)
                    LOG("[dclear] depth %p f%#x %ux%u: depth %d = %.9f, stencil %d = %#x mask %#x\n", (void *)st.rt.depth,
                        st.rt.depth->format, st.rt.depth->width, st.rt.depth->height, r->x[0] != 0, r->f[0], r->x[2] != 0,
                        (uint32_t)r->x[1], (uint32_t)r->x[2]);
                clear(-1, 0, NULL, 0, r->x[0] != 0, r->f[0], has_stencil && r->x[2] != 0, (uint32_t)r->x[1], (uint32_t)r->x[2]);
            }
            break;
        case CMD_SET_RENDER_ENABLE: {
            // x[0] 0/1 = SetRenderEnable; 2/3 = SetRenderEnableConditional mode 0 (render if equal) / 1 (if not equal) on the
            // two counters at x[1] and x[1] + 16 (cmdbuf.c). Evaluated from guest memory at execution time (values written by
            // GPU work of the same submission may be stale). Applied by default (TotK clears its half-res lighting targets
            // once per frame this way; ignoring it wiped them = black cave); LARECOMP_COND_RENDER=0 = ignore (old).
            static int apply = -1;
            if (apply < 0) apply = getenv("LARECOMP_COND_RENDER") ? atoi(getenv("LARECOMP_COND_RENDER")) : 1;
            if (r->x[0] < 2) { g_render_off = false; break; }
            const uint64_t *ctr = (const uint64_t *)(uintptr_t)r->x[1];
            bool equal = ctr && ctr[0] == ctr[2];
            bool render = r->x[0] == 2 ? equal : !equal;
            g_cond_evals++;
            if (!render) g_cond_off++;
            {   // per-address result pattern of each frame (bit i = eval i rendered); "[condflip]" when it differs from the
                // address's previous frame (fog popping in and out with camera moves, user 2026-10-15). Max 300 lines.
                static struct { uint64_t addr, frame; uint32_t pat, n, ppat, pn; bool prev; } ct[128];
                static int nct, nlog;
                uint64_t fr = renderer_present_count();
                int i = 0;
                while (i < nct && ct[i].addr != r->x[1]) i++;
                if (i == nct && nct < 128) { ct[nct] = (__typeof__(ct[0])){r->x[1], fr, 0, 0, 0, 0, false}; nct++; }
                if (i < nct) {
                    if (ct[i].frame != fr) {
                        uint32_t k = ct[i].n < ct[i].pn ? ct[i].n : ct[i].pn, m = k >= 32 ? ~0u : (1u << k) - 1;
                        if (ct[i].prev && ((ct[i].pat ^ ct[i].ppat) & m)) {
                            g_cond_flips++;
                            uint64_t bk = r->x[1] >= HEAP_BASE && ((r->x[1] - HEAP_BASE) >> GDEV_SH) < GDEV_BLKS ? (r->x[1] - HEAP_BASE) >> GDEV_SH : 0;
                            if (nlog++ < 300)
                                LOG("[condflip] addr %llx frame %llu: %u evals pattern %#x (prev frame %u evals %#x) | now %llx vs %llx mode %llu | shader-written blk seq %llu (submit %llu)\n",
                                    (unsigned long long)r->x[1], (unsigned long long)ct[i].frame, ct[i].n, ct[i].pat, ct[i].pn, ct[i].ppat,
                                    ctr ? (unsigned long long)ctr[0] : 0ull, ctr ? (unsigned long long)ctr[2] : 0ull,
                                    (unsigned long long)(r->x[0] - 2), (unsigned long long)gdev_blk[bk], (unsigned long long)submit_seq);
                        }
                        ct[i].ppat = ct[i].pat, ct[i].pn = ct[i].n, ct[i].prev = true;
                        ct[i].pat = 0, ct[i].n = 0, ct[i].frame = fr;
                    }
                    if (ct[i].n < 32) ct[i].pat |= (uint32_t)render << ct[i].n;
                    ct[i].n++;
                }
            }
            static int log_cond = -1;
            if (log_cond < 0) log_cond = getenv("LARECOMP_LOG_COND") != NULL;
            if (g_debug_frame || (log_cond && g_cond_evals < 400))
                LOG("[cond] frame %llu mode %llu addr %llx: %llx (ts %llx) vs %llx (ts %llx) -> %s%s\n",
                    (unsigned long long)renderer_present_count(), (unsigned long long)(r->x[0] - 2), (unsigned long long)r->x[1],
                    ctr ? (unsigned long long)ctr[0] : 0ull, ctr ? (unsigned long long)ctr[1] : 0ull,
                    ctr ? (unsigned long long)ctr[2] : 0ull, ctr ? (unsigned long long)ctr[3] : 0ull, render ? "render" : "SKIP",
                    apply ? "" : " (not applied)");
            g_render_off = apply && !render;
            break;
        }
        case CMD_DISPATCH: dispatch_compute((uint32_t)r->x[0], (uint32_t)r->x[1], (uint32_t)r->x[2]); break;
        case CMD_DRAW_ELEMENTS_INDIRECT: ind_note_indirect(0, r->x[3]); break;   // (x: mode, type, index buffer, indirect buffer)
        case CMD_DRAW_ARRAYS_INDIRECT: ind_note_indirect(1, r->x[1]); break;     // (x: mode, indirect buffer)
        case CMD_DISPATCH_INDIRECT: ind_note_indirect(2, r->x[0]); break;
        case CMD_DRAW_ARRAYS: draw(r, false); break;
        case CMD_DRAW_ELEMENTS: draw(r, true); break;
        case CMD_UPDATE_UNIFORM_BUFFER:
            memcpy((void *)(uintptr_t)(r->x[0] + r->x[2]), x, r->x[3]);
            // (uniform buffers reach the heap mirror only with LARECOMP_UBO_ZEROCOPY=2; otherwise they are the snapshots
            // flushed below, and ~280 inline updates a frame (d55) would each re-sync a mirror chunk for nothing)
            if (ubo_zc == 2) mirror_guest_written(r->x[0] + r->x[2], r->x[3]);
            guest_copy_flush();  // (cbuf copies shared within the submission are stale now)
            if (ubo_zc > 0) {    // (that buffer goes back to snapshots; earlier draws of this list may read the new data once)
                uint64_t lo = r->x[0], hi = r->x[0] + (r->x[1] > r->x[2] + r->x[3] ? r->x[1] : r->x[2] + r->x[3]);
                bool have = false;
                for (int i = 0; i < zc_nexcl; i++) if (lo >= zc_excl[i].lo && hi <= zc_excl[i].hi) { have = true; break; }
                if (!have && zc_nexcl < 64) {
                    zc_excl[zc_nexcl].lo = lo; zc_excl[zc_nexcl].hi = hi; zc_nexcl++;
                    LOG("[vk] inline UpdateUniformBuffer (%llx +%llu): buffer %llx-%llx stays snapshot (%d such)\n",
                        (unsigned long long)(r->x[0] + r->x[2]), (unsigned long long)r->x[3], (unsigned long long)lo,
                        (unsigned long long)hi, zc_nexcl);
                } else if (!have) {
                    ubo_zc = 0;
                    LOG("[vk] inline UpdateUniformBuffer: more than 64 buffers, uniform buffer zero-copy off\n");
                }
            }
            break;
        case CMD_FENCE_SYNC: {
            static int fence_flush = -1;
            if (fence_flush < 0) fence_flush = getenv("LARECOMP_FENCE_FLUSH") ? 1 : 0;
            if (fence_flush) renderer_flush();
            extern void nvn_sync_set_seq(uint64_t guest, uint64_t seq);
            if (chunk_limit()) { vk_cmd(); g_fence_seq_mark = vk_pending_seq(); }  // (the copy-back happens in this submission)
            if (r->x[1]) nvn_sync_set_seq(r->x[1], renderer_fence_seq());
            break;
        }
        case CMD_BARRIER: {   // nvnCommandBufferBarrier: the next pass begins with a full barrier (see begin_pass)
            stat_game_barriers++;
            stat_bar_bits[r->x[0] & 0x1ff]++;
            if (in_pass) stat_bar_inpass++;
            // d27 OBSERVED: 316121 of 331k game barriers per 2 s carry no bits at all (no ordering, no invalidation: a no-op);
            // the rest are 0x12 (ORDER_FRAGMENTS | INVALIDATE_TEXTURE) ~120 per frame, 0x11 and 0x101 ~7 per frame.
            if (!(r->x[0] & 0x1ff)) break;
            game_bar_since_fb = true;
            tex_bar_epoch++;
            if (barrier_mode() != BAR_GAME) break;
            pass_barrier_needed = true;
            pbn_cause = 1;
            if (in_pass && pass_unbarriered) {   // (this pass skipped its barrier: what it reads next must see the earlier passes)
                st.rt_dirty = true;
                stat_game_barrier_breaks++;
            }
            break;
        }
        case CMD_REPORT_COUNTER: {
            // The game times its GPU work with these (two per frame) and drops to 30 fps when that looks slow;
            // the value is our translation time, not GPU time. LARECOMP_VK_COUNTER=frozen: the same timestamp for
            // every counter within a presented frame (experiment).
            uint64_t *out = (uint64_t *)(uintptr_t)r->x[1];
            uint32_t type = (uint32_t)r->x[0];
            static uint32_t seen;
            if (type < 32 && !(seen & (1u << type))) { seen |= 1u << type; LOG("[nvn] ReportCounter type %u\n", type); }
            // LARECOMP_VK_COUNTER=scale:<k> (probe, 2026-10-18): the reported clock runs k times faster, so every interval
            // the game measures looks k times longer -> its dynamic resolution sees a 30-fps budget overrun at 90 fps (k 3).
            // TOTK_DYNRES=1 (Graphics menu "Dynamic resolution"): k = TOTK_FPS / 30, so the game's resolution scaler aims at
            // the chosen frame rate. d76_k3 (OBSERVED, Deck 1400 MHz, TOTK_FPS=90): 3D 1024x576 in the heavy standing scene,
            // 90.0 fps in all 55 windows (native: 84.5), HUD unscaled, game speed 1.0x (Cold Resistance timer).
            static int frozen = -1;
            static double cscale = 1;
            if (frozen < 0) {
                const char *e = getenv("LARECOMP_VK_COUNTER"), *dr = getenv("TOTK_DYNRES"), *fp = getenv("TOTK_FPS");
                frozen = e && !strcmp(e, "frozen");
                if (e && !strncmp(e, "scale:", 6) && atof(e + 6) > 0) cscale = atof(e + 6);
                else if (dr && atoi(dr) > 0 && fp && atoi(fp) > 30) cscale = atoi(fp) / 30.0;
                if (cscale != 1) LOG("[vk] counter clock x%.2f (dynamic resolution aims at %.0f fps)\n", cscale, 30 * cscale);
            }
            static uint64_t cbase;
            if (!cbase) cbase = SDL_GetTicksNS();
            static uint64_t frame_seen = ~0ull, frame_ts;
            extern uint64_t vk_frame_counter(void);
            if (frozen && vk_frame_counter() != frame_seen) { frame_seen = vk_frame_counter(); frame_ts = SDL_GetTicksNS(); }
            // type 1 = SAMPLES_PASSED: TotK resets it (nvnCommandBufferResetCounter(1)) and reads it back as an occlusion
            // query. Reporting 0 made every queried light / flame "occluded": Zelda's torch lit the hall only while the
            // camera was close to it (user 2026-10-14 22:40). Until real Vulkan occlusion queries exist, report "visible":
            // a count that grows by 4096 per report (so both "since reset" and "end - begin" reads are > 0).
            // LARECOMP_OCCLUSION=zero = the old behaviour (A/B).
            // (since 2026-10-30 real queries replace that value once the submission completed: oq_report)
            static uint64_t samples;
            uint64_t v = 0;
            if (type == 1 && oq_on() != 0) v = samples += 4096;
            // LARECOMP_COUNTER_TYPES=<t>[,<t>...] (probe): scale only those counter types (TotK reports 0, 1 and 16). d82
            // (OBSERVED): with the x3 clock, 64 frames >= 14 ms in the light stretch (spaced in multiples of 60 frames,
            // "other" = game threads +4 ms, 0 images, no resolution change) vs 18 unscaled: which reads cause them?
            // d83 (OBSERVED, walk, light-stretch frames >= 14 ms after f5600): all types 64, type 0 only 31 (heavy stretch
            // 90.0, same [drs] steps = type 0 is the resolution scaler's clock), type 16 only 34 (heavy 84.0: no DRS),
            // none 18. So TOTK_DYNRES scales type 0 only; the explicit scale:<k> probe keeps scaling all of them.
            static uint32_t tmask;
            if (!tmask) {
                const char *e = getenv("LARECOMP_COUNTER_TYPES"), *ve = getenv("LARECOMP_VK_COUNTER");
                tmask = e ? 0x80000000u : ve && !strncmp(ve, "scale:", 6) ? ~0u : 0x80000001u;
                for (const char *p = e; p && *p;) { long t = strtol(p, (char **)&p, 10); if (t >= 0 && t < 31) tmask |= 1u << t; if (*p == ',') p++; else break; }
            }
            double k = type < 31 && (tmask & (1u << type)) ? cscale : 1;
            // Type 0 is the game's dynamic-resolution clock (d83 above). Its reports are written when the RENDER THREAD
            // translates them, so the game measured our translation time (plus any wait in it) as GPU time: render-thread bound
            // frames looked GPU-bound and the scene dropped to 0.75 (1200x675) on an idle RTX 5080. With the Graphics menu's
            // dynamic resolution off (TOTK_DYNRES unset / 0) type 0 now holds one timestamp per presented frame, so the game's
            // scaler always sees time to spare and keeps its full resolution; with it on, the scaled clock as before.
            // LARECOMP_DRS_CLOCK=real: the live clock (old). (not keyed on TOTK_DYNRES, read once at start: the Settings row
            // switching On -> Off mid-session sets the floor to 100 % with TOTK_DYNRES still 1, and the scene could still drop
            // until a restart; at a fixed size the scaled clock serves nothing, so the live floor alone decides)
            static int drs_hold = -1;   // (the switch; the floor itself is read per report: the Options menu changes it live)
            if (drs_hold < 0) {
                const char *e = getenv("LARECOMP_DRS_CLOCK");
                drs_hold = !(e && !strcmp(e, "real"));
                if (drs_hold) LOG("[vk] fixed scene size: the game's resolution clock (counter type 0) holds per frame (LARECOMP_DRS_CLOCK)\n");
            }
            // (only while the minimum-resolution floor is 100 % = fixed size: the player who asked for the game's dynamic
            // resolution, or a floor below 100 %, keeps the live clock)
            extern float totk_dynamic_res_current(void);
            // (types 0 AND 16: agl::fctr::GPUStressChecker takes its GPU time between a type-16 and a type-0 report;
            // holding one of the two alone would give it a wrong, even negative, interval)
            const bool hold0 = drs_hold && (type == 0 || type == 16) && totk_dynamic_res_current() >= 1.0f;
            if (hold0 && vk_frame_counter() != frame_seen) { frame_seen = vk_frame_counter(); frame_ts = SDL_GetTicksNS(); }
            const bool hold = frozen || hold0;
            const uint64_t now_ns = SDL_GetTicksNS();
            const uint64_t host_v = hold ? frame_ts : cbase + (uint64_t)((double)(now_ns - cbase) * k);
            // (dynamic resolution wanted: a layer's three reports are written together once its GPU interval is known)
            if ((type == 0 || type == 16) && hold && dts_mode == 1 && (dts_have16 || dts_have_start || dts_ph != dts_pt)) {
                // (the floor went back to 100 %: no stale layer, and no pending completion writing over the held values)
                dts_flush_open();
                for (unsigned i = dts_ph; i != dts_pt; i++) dts_pend[i % DTS_PEND].o16 = dts_pend[i % DTS_PEND].o0 = dts_pend[i % DTS_PEND].o1 = NULL;
            }
            if (type == 0 || type == 16) dsub_live = !hold;
            const bool owned = (type == 0 || type == 16) && !hold && dts_claim(type, out, host_v, k);
            if (out) { out[0] = v; if (!owned) out[1] = host_v; }
            if (type == 1 && oq_mode == 2) oq_report(out);
            if (g_debug_frame) LOG("[counter] type %u -> %llx value %llu\n", type, (unsigned long long)r->x[1], (unsigned long long)v);
            break;
        }
        case CMD_RESET_COUNTER: oq_reset_counter((uint32_t)r->x[0]); break;
        case CMD_COPY_TEXTURE_TO_TEXTURE: { const TexRefRec *t = x; copy_texture(&t[0], &t[1], r->x[6]); break; }
        case CMD_DOWNSAMPLE: { const TexRefRec *t = x; copy_texture(&t[0], &t[1], 1); break; }
        case CMD_COPY_BUFFER_TO_TEXTURE: copy_buffer_to_texture(r->x[0], x); break;
        case CMD_COPY_TEXTURE_TO_BUFFER: copy_texture_to_buffer(x, r->x[3]); break;
        case CMD_COPY_BUFFER_TO_BUFFER:
            if (g_debug_frame) LOG("[bufcopy] %llx -> %llx size %llu\n", (unsigned long long)r->x[0], (unsigned long long)r->x[1], (unsigned long long)r->x[2]);
            {   // LARECOMP_TEX_WATCH_FMT: memory copies into / out of watched textures' storage
                extern bool tex_watched_range(uint64_t a, uint64_t n);
                extern uint64_t vk_frame_counter(void);
                static int nl;
                if (nl < 600 && (tex_watched_range(r->x[1], r->x[2]) || tex_watched_range(r->x[0], r->x[2]))) {
                    nl++;
                    LOG("[texw] bufcopy %llx -> %llx size %llu frame %llu\n", (unsigned long long)r->x[0], (unsigned long long)r->x[1],
                        (unsigned long long)r->x[2], (unsigned long long)vk_frame_counter());
                }
            }
            renderer_flush();
            memmove((void *)(uintptr_t)r->x[1], (void *)(uintptr_t)r->x[0], r->x[2]);
            mirror_guest_written(r->x[1], r->x[2]);
            break;
        default: {   // (not executed: logged once per op, and every time in a debug frame)
            static uint64_t seen[CMD_COUNT];
            if (r->op < CMD_COUNT && (!seen[r->op]++ || g_debug_frame))
                LOG("[cmd] op %u not executed (%llu so far): x %llx %llx %llx %llx %llx\n", r->op, (unsigned long long)seen[r->op],
                    (unsigned long long)r->x[0], (unsigned long long)r->x[1], (unsigned long long)r->x[2], (unsigned long long)r->x[3],
                    (unsigned long long)r->x[4]);
            break;
        }
        }
        p += cmd_gsize(r->size);
    }
}

static pthread_mutex_t exec_mu = PTHREAD_MUTEX_INITIALIZER;
void backend_exec_lock(void) { pthread_mutex_lock(&exec_mu); }
void backend_exec_unlock(void) { pthread_mutex_unlock(&exec_mu); }

static void deferred_frees(void);
static void backend_execute_now(uint64_t handle) {
    PROF_T0();
    uint64_t hx0 = SDL_GetTicksNS();   // ([hitch] log, always on)
#ifndef _WIN32
    // on-CPU time vs wall time of exec: a slow exec that ran the whole time = slower CPU (SMT sibling, clock); one that
    // did not = preempted (d86: every op 1.7-3x slower on the periodic hitch frames)
    struct timespec c0; struct rusage r0;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &c0); getrusage(RUSAGE_THREAD, &r0);
#else
    ULONG64 cy0 = 0;   // (Windows, VK_PROF only: thread cycles in TSC ticks, scaled by prof_now's calibration)
    if (prof) QueryThreadCycleTime(GetCurrentThread(), &cy0);
#endif
    backend_exec_lock();
    alias_import_kick();
    deferred_frees();   // (LARECOMP_DEFER_FREE)
    if (capture_on()) capture_exec(handle);
    alias_epoch++;
    exec_start_seq = vk_pending_seq();
    if (chunk_ab > 0) {
        extern uint64_t renderer_present_count(void);
        chunk_ab_on = (renderer_present_count() / (uint64_t)chunk_ab) % 2 == 0;
    }
    in_execute = true;
    exec_list(handle, 0);
    vk_writeback_flush();
    in_execute = false;
    backend_exec_unlock();
    if (prof && op_t0) { op_ns[op_last] += prof_now() - op_t0; op_t0 = 0; }
    PROF_ADD(prof_exec);
    cum_exec += SDL_GetTicksNS() - hx0;
#ifndef _WIN32
    struct timespec c1; struct rusage r1;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &c1); getrusage(RUSAGE_THREAD, &r1);
    cum_exec_cpu += (uint64_t)((c1.tv_sec - c0.tv_sec) * 1000000000ll + (c1.tv_nsec - c0.tv_nsec));
    cum_exec_ivcsw += (uint64_t)(r1.ru_nivcsw - r0.ru_nivcsw);
#else
    ULONG64 cy1 = 0;
    if (prof) { QueryThreadCycleTime(GetCurrentThread(), &cy1); cum_exec_cpu += (uint64_t)((double)(cy1 - cy0) * prof_ns_per_tick); }
#endif
    static int sync_submit = -1;
    if (sync_submit < 0) sync_submit = getenv("LARECOMP_SYNC_SUBMIT") ? 1 : 0;
    if (sync_submit) renderer_flush();
    else renderer_submit();
}

// ---- async execute. The game's Presentation Thread called nvnQueueSubmitCommands and we translated + submitted the
// lists inside that call. LARECOMP_ASYNC_EXEC=1: submits, fence syncs, presents and the frees of objects that queued
// lists name by host pointer (cmdbuf.c: render targets, copy textures, programs) go into one ordered queue drained by
// a render thread; the game's thread returns at once, as with a real GPU. nvnSyncWait / nvnQueueFinish wait for the
// queue to pass their fence first. Presents run at most LARECOMP_ASYNC_AHEAD (default 1) frames behind the game. The
// queue grows instead of blocking (finalizers push while holding the exec lock the render thread needs).
enum { AOP_EXEC = 1, AOP_FENCE, AOP_PRESENT, AOP_CALL };
void vk_present_set_frame(uint64_t frame);   // present.c (low latency markers)
uint64_t vk_present_latency_frame(void);
typedef struct { int kind; uint64_t a; void *p; void (*fn)(void *); uint64_t t; } AsyncOp;   // (t: push time, ns)
uint64_t g_op_push_ns;   // (render thread: push time of the op it is running)
static AsyncOp *aq;
static uint64_t aq_cap, aq_head, aq_tail, aq_done, aq_pres_pushed, aq_pres_done;   // head/tail: monotonic op indices
uint64_t backend_async_presents_done(void) { return aq_pres_done; }   // (render thread: the presents it has replayed)
static pthread_mutex_t aq_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t aq_cv = PTHREAD_COND_INITIALIZER, aq_done_cv = PTHREAD_COND_INITIALIZER;
static bool aq_started;
static uint64_t aq_stat_ops, aq_stat_wait_ns, aq_stat_waits, aq_stat_pres_wait_ns, aq_stat_maxdepth;
bool backend_async_on(void) {
    static int on = -1;
    // Default on since 2026-10-24; LARECOMP_ASYNC_EXEC=0: execute on the game's thread, as before. A capture
    // (LARECOMP_CAPTURE, capture.h) stays synchronous unless LARECOMP_ASYNC_EXEC=1 asks otherwise: its frames are cut at
    // the game's presents.
    if (on < 0) { const char *e = getenv("LARECOMP_ASYNC_EXEC"); on = e ? *e != '0' : !getenv("LARECOMP_CAPTURE"); LOG("[async] render thread %s\n", on ? "on" : e ? "off (LARECOMP_ASYNC_EXEC=0)" : "off (capture)"); }
    return on;
}
#ifdef _WIN32
// The render thread alone on one physical core: cx5 saw its CPU per frame vary up to 70 % between boots with the same
// work, and on 4c/8t it ran 1.2-1.3x slower next to a busy SMT sibling. cpu_place_init picked the core (the last SMT
// core of hot, or LARECOMP_RENDER_PIN=<logical cpu>'s; =0 none) and keeps both of its logical processors out of the
// process default and every other set, so no thread of ours lands on the sibling. Soft as CPU Sets are; other
// processes are not affected.
static void render_pin(void) {
    cpu_place_thread(CPU_RENDER);
}
#endif
static void *async_main(void *arg) {
    (void)arg;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    extern void rt_prof_register_host(const char *name);
    rt_prof_register_host("render");   // (TOTK_PROF samples this thread; hostprof.py --hle render)
#ifndef _WIN32
    extern int pthread_setname_np(pthread_t, const char *);
    pthread_setname_np(pthread_self(), "render");   // (Linux "[cpu]" per-thread line)
    extern void host_affinity(int kind, unsigned long long mask);
    host_affinity(1, 0);   // (TOTK_AFFINITY: render side on its own CPUs)
#else
    SetThreadDescription(GetCurrentThread(), L"render");   // (ETW / debugger thread names)
    render_pin();
#endif
    for (;;) {
        pthread_mutex_lock(&aq_mu);
        while (aq_head == aq_tail) pthread_cond_wait(&aq_cv, &aq_mu);
        AsyncOp op = aq[aq_head & (aq_cap - 1)];
        pthread_mutex_unlock(&aq_mu);
        g_op_push_ns = op.t;
        switch (op.kind) {
        case AOP_EXEC: backend_execute_now(op.a); break;
        case AOP_FENCE: {
            NvnSync *s = (NvnSync *)op.p;
            renderer_fence_submit();
            uint64_t q = renderer_fence_seq();
            s->prev_seq = s->rt_seq;
            s->rt_seq = q;
            s->ring_seq[op.a & 3] = q;   // (op.a: the sync's fence number, backend_async_fence)
            s->seq = q;
            s->signaled = 1;
            break;
        }
        case AOP_PRESENT: vk_present_set_frame(op.a); backend_present((NvnTexture *)op.p); break;   // (op.a: low latency's frame)
        case AOP_CALL: backend_exec_lock(); op.fn(op.p); backend_exec_unlock(); break;
        }
        pthread_mutex_lock(&aq_mu);
        aq_head++;
        aq_done = aq_head;
        aq_stat_ops++;
        if (op.kind == AOP_PRESENT) aq_pres_done++;
        pthread_cond_broadcast(&aq_done_cv);
        pthread_mutex_unlock(&aq_mu);
    }
    return NULL;
}
static uint64_t aq_push(AsyncOp op) {
    op.t = SDL_GetTicksNS();
    pthread_mutex_lock(&aq_mu);
    if (!aq_started) {
        aq_started = true;
        aq_cap = 1024;
        aq = calloc(aq_cap, sizeof *aq);
        pthread_t th;
        pthread_create(&th, NULL, async_main, NULL);
        pthread_detach(th);
    }
    if (aq_tail - aq_head == aq_cap) {   // grow: same logical indices in a twice-as-large ring
        AsyncOp *n = calloc(aq_cap * 2, sizeof *n);
        for (uint64_t i = aq_head; i < aq_tail; i++) n[i & (aq_cap * 2 - 1)] = aq[i & (aq_cap - 1)];
        free(aq);
        aq = n;
        aq_cap *= 2;
    }
    aq[aq_tail & (aq_cap - 1)] = op;
    uint64_t ticket = ++aq_tail;
    if (aq_tail - aq_head > aq_stat_maxdepth) aq_stat_maxdepth = aq_tail - aq_head;
    if (op.kind == AOP_PRESENT) aq_pres_pushed++;
    pthread_cond_signal(&aq_cv);
    pthread_mutex_unlock(&aq_mu);
    return ticket;
}
void backend_async_wait(uint64_t ticket) {
    if (!backend_async_on()) return;
    uint64_t t0 = SDL_GetTicksNS();
    pthread_mutex_lock(&aq_mu);
    if (!ticket) ticket = aq_tail;
    bool waited = aq_done < ticket;
    while (aq_done < ticket) pthread_cond_wait(&aq_done_cv, &aq_mu);
    if (waited) { aq_stat_waits++; aq_stat_wait_ns += SDL_GetTicksNS() - t0; }
    pthread_mutex_unlock(&aq_mu);
}
void backend_execute(uint64_t handle) {
    if (backend_async_on()) aq_push((AsyncOp){AOP_EXEC, handle, NULL, NULL});
    else backend_execute_now(handle);
}
// LARECOMP_DEFER_FREE=1: in synchronous execute a finalizer's plain free() is queued and done by the next execute under
// its lock, instead of waiting for the lock on the game's thread: nvnCommandBufferFinalize alone spent 23-24 % of
// ModuleSystemWorker's wall time there. Lists recorded after a finalize cannot name the object, and the execute holding
// the lock now has finished by the time the next one frees it.
static void **g_dfree; static size_t g_ndfree, g_capdfree;
static pthread_mutex_t g_dfree_mu = PTHREAD_MUTEX_INITIALIZER;
static int defer_free_on(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("LARECOMP_DEFER_FREE"); on = e && *e == '1'; if (on) LOG("[vk] finalizer frees deferred to the next execute\n"); }
    return on;
}
static void deferred_frees(void) {   // (under the exec lock)
    if (!g_ndfree) return;
    pthread_mutex_lock(&g_dfree_mu);
    void **v = g_dfree; size_t n = g_ndfree;
    g_dfree = NULL; g_ndfree = g_capdfree = 0;
    pthread_mutex_unlock(&g_dfree_mu);
    for (size_t i = 0; i < n; i++) free(v[i]);
    free(v);
}
void backend_async_call(void (*fn)(void *), void *arg) {
    if (backend_async_on()) aq_push((AsyncOp){AOP_CALL, 0, arg, fn});
    else if (fn == free && defer_free_on()) {
        pthread_mutex_lock(&g_dfree_mu);
        if (g_ndfree == g_capdfree) { g_capdfree = g_capdfree ? g_capdfree * 2 : 256; g_dfree = realloc(g_dfree, g_capdfree * sizeof *g_dfree); }
        g_dfree[g_ndfree++] = arg;
        pthread_mutex_unlock(&g_dfree_mu);
    }
    else { backend_exec_lock(); fn(arg); backend_exec_unlock(); }
}
void backend_async_fence(NvnSync *s) {
    s->signaled = 1;
    s->seq = 0;
    s->prev_ticket = s->ticket;
    s->ticket = aq_push((AsyncOp){AOP_FENCE, ++s->nfence, s, NULL});
}
void backend_async_present(NvnTexture *t) {
    static int ahead = -1;
    if (ahead < 0) { const char *e = getenv("LARECOMP_ASYNC_AHEAD"); ahead = e ? atoi(e) : 1; }
    vk_late_latch_sample((int64_t)aq_pres_pushed);   // (the game's last camera step, before this present and after its throttle)
    aq_push((AsyncOp){AOP_PRESENT, vk_present_latency_frame(), t, NULL});
    uint64_t t0 = SDL_GetTicksNS();
    pthread_mutex_lock(&aq_mu);
    while (aq_pres_pushed - aq_pres_done > (uint64_t)ahead) pthread_cond_wait(&aq_done_cv, &aq_mu);
    if (vk_late_latch_on()) { pthread_mutex_unlock(&aq_mu); vk_late_latch_sample(-1); pthread_mutex_lock(&aq_mu); }
    aq_stat_pres_wait_ns += SDL_GetTicksNS() - t0;
    extern void vk_present_jit_wait(uint64_t ns);   // (present.c: a wait after this frame read its input, low latency)
    vk_present_jit_wait(SDL_GetTicksNS() - t0);
    static uint64_t last_log;
    if (t0 - last_log > 5000000000ull) {   // every 5 s: game-side waits on the render thread
        if (last_log)
            LOG("[async] 5 s: %llu ops (max queue %llu), present throttle %.0f ms, sync/finish waits %llu = %.0f ms\n",
                (unsigned long long)aq_stat_ops, (unsigned long long)aq_stat_maxdepth, aq_stat_pres_wait_ns / 1e6,
                (unsigned long long)aq_stat_waits, aq_stat_wait_ns / 1e6);
        last_log = t0;
        aq_stat_ops = aq_stat_maxdepth = aq_stat_pres_wait_ns = aq_stat_waits = aq_stat_wait_ns = 0;
    }
    pthread_mutex_unlock(&aq_mu);
}

// ---- Compile benchmark: every program and pipeline recorded in the working directory's cache/precompile.bin (+ the
// seed's) built once, without the game, through the precompile workers' own steps (translate_program with inline compile,
// build_pipeline_body), timed per item: lock wait, translate, GLSL -> SPIR-V, modules + layout, GPL libraries (the
// driver's compile) per program; build time and path (GPL fast link or a full pipeline) per pipeline. Measures the
// first-encounter cost without a game boot. Stage times come from the thread-local warm-up timers (pa_tm), so they are
// per item also with several threads.
typedef struct { double ms[6]; uint64_t fs; int spv_cached, libs, libs_hit, ok; } BenchProg;
typedef struct { double ms; uint64_t fs; int fast, ok; } BenchPipe;
static BenchProg *bench_prog; static BenchPipe *bench_pipe;
static _Atomic int bench_next_prog, bench_next_pipe, bench_progs_done;
static void *bench_worker(void *arg) {
    (void)arg;
    pa_tm_on = true;
    for (;;) {
        int i = atomic_fetch_add(&bench_next_prog, 1);
        if (i >= pc_nprogs) break;
        const PcProg *r = &pc_progs[i].p;
        BenchProg *o = &bench_prog[i];
        o->fs = r->code_hash[1];
        Prog *p = calloc(1, sizeof *p);
        p->vkey = r->vkey; p->fkey = r->fkey; p->ckey = r->ckey; p->inline_compile = true;
        bool ok = true;
        for (int st_ = 0; st_ < 2; st_++) {
            uint64_t cs = pc_progs[i].csize[st_];
            uint8_t *b = pc_load_code_c1(r->code_hash[st_], r->cmax[st_], &cs);
            if (!b) { ok = false; break; }
            p->code[st_] = (uint64_t)(uintptr_t)b; p->cmax[st_] = r->cmax[st_]; p->csize[st_] = cs;
        }
        uint64_t before[8];
        memcpy(before, pa_tm, sizeof before);
        uint64_t t0 = SDL_GetTicksNS();
        if (ok && pc_claim_prog(r->ckey, p) == p) translate_program(p);
        else { free(p); p = NULL; }
        o->ms[5] = (SDL_GetTicksNS() - t0) / 1e6;
        for (int k = 0; k < 5; k++) o->ms[k] = (pa_tm[k] - before[k]) / 1e6;
        o->spv_cached = (int)(pa_tm[5] - before[5]); o->libs_hit = (int)(pa_tm[6] - before[6]); o->libs = (int)(pa_tm[7] - before[7]);
        o->ok = p && atomic_load(&p->state) == 1;
        atomic_fetch_add(&bench_progs_done, 1);
    }
    while (atomic_load(&bench_progs_done) < pc_nprogs) SDL_Delay(2);
    for (;;) {
        int i = atomic_fetch_add(&bench_next_pipe, 1);
        if (i >= pc_npipes) break;
        const PcPipe *r = &pc_pipes[i];
        BenchPipe *o = &bench_pipe[i];
        Prog *p = pc_find_prog(r->ckey);
        o->fs = p ? p->code[1] : 0;
        if (!p || atomic_load(&p->state) != 1) continue;
        Pipe *pp = calloc(1, sizeof *pp);
        pp->k = r->k; pp->k.prog = p;
        uint64_t ph = hash_mem(0xcbf29ce484222325ull, &pp->k, sizeof pp->k);
        if (!ph) ph = 1;
        pp->claimed = 1;
        if (pc_take_pipe(ph)) { free(pp); continue; }
        pc_put_pipe(ph, pp);
        o->fast = gpl_linkable(&pp->k);
        uint64_t t0 = SDL_GetTicksNS();
        build_pipeline_body(pp);
        o->ms = (SDL_GetTicksNS() - t0) / 1e6;
        o->ok = atomic_load(&pp->state) == 1;
    }
    return NULL;
}
static int bench_cmp(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }
static void bench_pct(const char *what, double *v, int n) {
    if (!n) { LOG("[bench] %s: none\n", what); return; }
    qsort(v, (size_t)n, sizeof *v, bench_cmp);
    double sum = 0;
    for (int i = 0; i < n; i++) sum += v[i];
    LOG("[bench] %s: %d, total %.0f ms, mean %.2f, p50 %.2f, p90 %.2f, p99 %.2f, max %.2f ms\n", what, n, sum, sum / n, v[n / 2],
        v[(int)(n * 0.9)], v[(int)(n * 0.99)], v[n - 1]);
}
// Translation stress: every recorded program translated once on one thread under translate_lock (the reference), then
// `rounds` times on `threads` threads, those without the lock when `unlocked`; each translation's GLSL and binding
// layout hashed and compared with the reference. Nothing is compiled. Returns the number of programs whose hash ever
// differed (0: the same on every thread and round).
static uint64_t *ts_ref, *ts_got;
static _Atomic int ts_next, ts_diff;
static int ts_unlocked;
static uint64_t ts_hash_info(uint64_t h, const ShaderInfo *si) {
    if (!si) return h;
    if (si->msl) h = hash_mem(h, si->msl, strlen(si->msl));
    if (si->error) h = hash_mem(h, si->error, strlen(si->error));
    h = hash_mem(h, &si->out_mask, sizeof si->out_mask);
    h = hash_mem(h, &si->ntex, sizeof si->ntex);
    h = hash_mem(h, &si->cbuf_mask, sizeof si->cbuf_mask);
    return h;
}
static void *ts_worker(void *arg) {
    uint64_t *out = arg;
    translate_unlocked = ts_unlocked != 0;
    for (;;) {
        int i = atomic_fetch_add(&ts_next, 1);
        if (i >= pc_nprogs) break;
        const PcProg *r = &pc_progs[i].p;
        Prog *p = calloc(1, sizeof *p);
        p->vkey = r->vkey; p->fkey = r->fkey; p->ckey = r->ckey; p->translate_only = true;
        bool ok = true;
        for (int s = 0; s < 2; s++) {
            uint64_t cs = pc_progs[i].csize[s];
            uint8_t *b = pc_load_code_c1(r->code_hash[s], r->cmax[s], &cs);
            if (!b) { ok = false; break; }
            p->code[s] = (uint64_t)(uintptr_t)b; p->cmax[s] = r->cmax[s]; p->csize[s] = cs;
        }
        uint64_t h = 0;
        if (ok) {
            translate_program(p);
            h = ts_hash_info(ts_hash_info(0xcbf29ce484222325ull, p->vi), p->fi);
            if (!h) h = 1;
            maxwell_free(p->vi);
            maxwell_free(p->fi);
        }
        for (int s = 0; s < 2; s++) free((void *)(uintptr_t)p->code[s]);   // (pc_load_code's buffers)
        free(p);
        out[i] = h;
        if (ts_ref && ts_ref[i] != h) atomic_fetch_add(&ts_diff, 1);
    }
    translate_unlocked = false;
    return NULL;
}
static void ts_round(int threads, int unlocked, uint64_t *out) {
    pthread_t th[64];
    if (threads > 64) threads = 64;
    atomic_store(&ts_next, 0);
    ts_unlocked = unlocked;
    for (int i = 0; i < threads; i++) pthread_create(&th[i], NULL, ts_worker, out);
    for (int i = 0; i < threads; i++) pthread_join(th[i], NULL);
}
int vk_translate_stress(int threads, int rounds, int unlocked) {
    if (pc_load_file("cache/precompile.bin") == 0) { LOG("[stress] cache/precompile.bin: another heap base / layout\n"); return -1; }
    if (pc_seed_dir()) { char path[600]; snprintf(path, sizeof path, "%s/precompile.bin", pc_seed_dir()); pc_load_file(path); }
    if (!pc_nprogs) { LOG("[stress] no recorded programs\n"); return -1; }
    ts_ref = calloc((size_t)pc_nprogs, sizeof *ts_ref);
    ts_got = calloc((size_t)pc_nprogs, sizeof *ts_got);
    uint64_t t0 = SDL_GetTicksNS();
    uint64_t *ref = ts_ref;
    ts_ref = NULL;
    ts_round(1, 0, ref);   // the reference: one thread, under the lock
    ts_ref = ref;
    int missing = 0;
    for (int i = 0; i < pc_nprogs; i++) missing += !ref[i];
    LOG("[stress] reference: %d programs on 1 thread (locked) in %.1f s, %d without their code\n", pc_nprogs,
        (SDL_GetTicksNS() - t0) / 1e9, missing);
    int bad_total = 0;
    for (int k = 0; k < rounds; k++) {
        atomic_store(&ts_diff, 0);
        t0 = SDL_GetTicksNS();
        ts_round(threads, unlocked, ts_got);
        const int d = atomic_load(&ts_diff);
        bad_total += d;
        LOG("[stress] round %d: %d programs on %d threads (%s) in %.1f s, %d differ from the reference\n", k + 1,
            pc_nprogs, threads, unlocked ? "unlocked" : "locked", (SDL_GetTicksNS() - t0) / 1e9, d);
        for (int i = 0, shown = 0; i < pc_nprogs && d && shown < 8; i++)
            if (ts_got[i] != ref[i]) {
                LOG("[stress]   program %d (fs %016llx): %016llx, reference %016llx\n", i,
                    (unsigned long long)pc_progs[i].p.code_hash[1], (unsigned long long)ts_got[i], (unsigned long long)ref[i]);
                shown++;
            }
    }
    LOG("[stress] %s: %d differing translations in %d rounds of %d programs\n", bad_total ? "FAIL" : "PASS", bad_total, rounds,
        pc_nprogs);
    return bad_total;
}

int vk_compile_bench(int threads, const char *csv) {
    if (threads < 1) threads = 1;
    pc_t0 = SDL_GetTicksNS();
    int own = pc_load_file("cache/precompile.bin");
    if (own == 0) { LOG("[bench] cache/precompile.bin was written for another heap base / layout: nothing to build\n"); return 1; }
    if (pc_seed_dir()) { char path[600]; snprintf(path, sizeof path, "%s/precompile.bin", pc_seed_dir()); pc_load_file(path); }
    LOG("[bench] %d programs, %d pipelines recorded; %d thread(s)\n", pc_nprogs, pc_npipes, threads);
    if (!pc_nprogs) return 1;
    bench_prog = calloc((size_t)pc_nprogs, sizeof *bench_prog);
    bench_pipe = calloc((size_t)(pc_npipes ? pc_npipes : 1), sizeof *bench_pipe);
    uint64_t t0 = SDL_GetTicksNS();
    pthread_t th[64];
    if (threads > 64) threads = 64;
    for (int i = 0; i < threads; i++) pthread_create(&th[i], NULL, bench_worker, NULL);
    for (int i = 0; i < threads; i++) pthread_join(th[i], NULL);
    double wall = (SDL_GetTicksNS() - t0) / 1e9;
    FILE *f = csv ? fopen(csv, "w") : NULL;
    if (f) fprintf(f, "kind,index,fs,ok,lock_ms,translate_ms,spirv_ms,modules_ms,libs_ms,total_ms,spirv_cached,libs,libs_cache_hit,fast_link\n");
    double *v = malloc(sizeof(double) * (size_t)(pc_nprogs > pc_npipes ? pc_nprogs : pc_npipes) + 8);
    static const char *const stage[6] = {"program lock wait", "program translate", "program GLSL -> SPIR-V", "program modules + layout",
                                         "program GPL libraries (driver)", "program total"};
    int okp = 0, spv = 0, hit = 0, libs = 0;
    for (int i = 0; i < pc_nprogs; i++) {
        const BenchProg *o = &bench_prog[i];
        okp += o->ok; spv += o->spv_cached; hit += o->libs_hit; libs += o->libs;
        if (f) fprintf(f, "prog,%d,%016llx,%d,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%d,%d,%d,\n", i, (unsigned long long)o->fs, o->ok, o->ms[0], o->ms[1],
                       o->ms[2], o->ms[3], o->ms[4], o->ms[5], o->spv_cached, o->libs, o->libs_hit);
    }
    for (int k = 0; k < 6; k++) {
        int n = 0;
        for (int i = 0; i < pc_nprogs; i++) if (bench_prog[i].ok) v[n++] = bench_prog[i].ms[k];
        bench_pct(stage[k], v, n);
    }
    int okl = 0, nfast = 0;
    for (int i = 0; i < pc_npipes; i++) {
        const BenchPipe *o = &bench_pipe[i];
        okl += o->ok; nfast += o->ok && o->fast;
        if (f) fprintf(f, "pipe,%d,%016llx,%d,,,,,,%.3f,,,,%d\n", i, (unsigned long long)o->fs, o->ok, o->ms, o->fast);
    }
    for (int fast = 1; fast >= 0; fast--) {
        int n = 0;
        for (int i = 0; i < pc_npipes; i++) if (bench_pipe[i].ok && bench_pipe[i].fast == fast) v[n++] = bench_pipe[i].ms;
        bench_pct(fast ? "pipeline, GPL fast link" : "pipeline, full build", v, n);
    }
    LOG("[bench] built %d of %d programs, %d of %d pipelines (%d fast-linked) in %.1f s on %d thread(s); SPIR-V cached for %d "
        "stages; GPL libraries %d, %d of them pipeline-cache hits\n", okp, pc_nprogs, okl, pc_npipes, nfast, wall, threads, spv, libs, hit);
    if (f) fclose(f);
    free(v);
    extern void vk_pipeline_cache_save(void);
    vk_pipeline_cache_save();
    return 0;
}

#include "vkq_impl.h"   // (deferred command recording, LARECOMP_VKQ=1)
