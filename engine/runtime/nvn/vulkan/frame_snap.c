// frame_snap.c: the capture half of the frame snapshot (frameguard, the v0.1.9 gate's one-frame rain streaks / missing roof /
// camera-elsewhere frames, hdr the replay half in exec.c). With the async render thread the replay of a submitted list reads
// the uniform data and CPU-written vertex / instance streams it binds from guest memory when it gets to them, which can be
// after the game has rewritten them for its next list or frame (LARECOMP_ASYNC_EXEC=0 cut the gate's glitch rate ~5x). So at
// each submit (backend_execute, on the game's thread, before the list's AOP_EXEC is queued) the list's bound ranges are
// copied, and the replay of that list reads them from this copy (exec.c vk_snap_src -> guest_copy, and a vertex stream it
// covers is copied instead of bound in place). What is taken: every CMD_BIND_UNIFORM_BUFFERS range of the list and of the
// lists it CALLs (up to LARECOMP_FRAME_SNAP_UBO_KB, default 64: NVN's largest uniform buffer), and every
// CMD_BIND_VERTEX_BUFFERS range up to LARECOMP_FRAME_SNAP_VTX_KB (default 0 = uniform data only, hdr: the gate's COPY_VERTICES
// arm did not clear the glitches, so streams are not the cause; =256 takes the per-frame streams, a large static mesh stays
// live), merged, copied once, at most LARECOMP_FRAME_SNAP_MB (default 32) per submit. A range outside committed memory is left
// live (a stale binding the list never draws with must not fault here), and so is a range in heap pages a shader has written
// (gdev_blk: its bytes come back on the GPU timeline). OFF by default since 11-05 (LARECOMP_FRAME_SNAP=1: on): the cand-0 A/B
// (look 32:9 flicks, 15 min each) ran 22.9 fps with it against 72.7 without, '[framesnap]' 1.1-1.9 s per 2 s on the game's
// threads, and its target (one-frame stale scenes) turned out to be the camera clipping into walls (TOTK_MOUSE_STEP_MAX).
// Windows only; elsewhere nothing is taken (live, as before). '[framesnap]' every ~2 s: submits, ranges, MB copied and the
// game threads' ms, what was left live, and how the replay's own writes were folded in. The replay's own guest writes: on the
// Switch they happen in order before a later list, so the snapshot of list L must hold the writes of every list replayed
// between L's capture and L's replay, and not the game's writes after the capture. Every replay write goes into a ticketed log
// (one sequence for all threads):
//  - frame_snap_write (UPDATE_UNIFORM_BUFFER, ReportCounter, COPY_BUFFER_TO_BUFFER: the bytes are in host memory at once):
//    the current list's snapshot takes them directly; a later list's snapshot folds them in when it becomes current
//    (frame_snap_apply): from the bytes the log kept (writes <= 4 KB), else from live memory, where the replay wrote them
//    in order before it;
//  - frame_snap_note (gdev_writeback's regions, the CPU-read write-backs: they land on the GPU timeline or on another
//    thread): the overlapping snapshot ranges are read live from then on, as without a snapshot.
// A snapshot remembers the log's sequence at its capture; at its apply the entries after it are folded in. If the log (64k
// entries) lost any of them, the whole snapshot is dropped: that list reads live. The copies go to arenas the render thread
// hands back after the list (a pool of up to 64 arenas / 256 MB: about a frame of queued lists, tens of submits): a fresh
// malloc per submit first-touched new pages on the game's thread.
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#endif
#include <SDL3/SDL_timer.h>
#if defined(__x86_64__) || defined(__i386__)
#define fs_pause() __builtin_ia32_pause()
#else
#define fs_pause() ((void)0)
#endif
#include "vk.h"

typedef struct { uint64_t addr, len, off; uint32_t live; } FsRange;   // (live: written on the GPU timeline since: read live)
typedef struct { uint32_t n; FsRange *r; uint8_t *data; uint64_t cap, wseq0; } FrameSnap;   // (cap: the arena's bytes)
typedef struct { FsRange *v; uint32_t n, cap; uint64_t over; } FsVec;

extern const void *(*vk_snap_src)(uint64_t addr, uint64_t size);   // exec.c (the replay half)
bool vk_heap_gpu_written_since(uint64_t addr, uint64_t size, uint64_t since);   // exec.c: a shader wrote a page of it
extern uint64_t g_rt_heap_base;   // hle_os_mem.cpp: the guest heap
uint64_t kernel_heap_size(void);

static int fs_on(void) {
    static int on = -1;
    if (on < 0) {
#ifdef _WIN32
        const char *e = getenv("LARECOMP_FRAME_SNAP");
        on = e && *e == '1';
#else
        on = 0;
#endif
    }
    return on;
}
static uint64_t fs_env_kb(const char *name, uint64_t dflt) { const char *e = getenv(name); return (e && *e ? strtoull(e, NULL, 0) : dflt) << 10; }

static void fs_add(FsVec *v, uint64_t addr, uint64_t len, uint64_t cap) {
    if (!addr || !len) return;
    if (len > cap) { v->over++; return; }
    if (v->n == v->cap) {
        uint32_t nc = v->cap ? v->cap * 2 : 256;
        FsRange *nv = realloc(v->v, nc * sizeof *nv);
        if (!nv) return;
        v->v = nv, v->cap = nc;
    }
    v->v[v->n++] = (FsRange){addr, len, 0};
}
// The list's bound ranges, as exec_list walks it (JUMP to the next chunk, CALL into other lists, END)
static void fs_walk(uint64_t handle, int depth, FsVec *v, uint64_t ucap, uint64_t vcap) {
    const CmdChunk *k = cmd_chunk_for(handle);
    if (!k) return;
    uint64_t p = handle;
    for (int guard = 0; guard < 10000000; guard++) {
        const CmdRec *r = cmd_at(k, p);
        const uint8_t *x = r->extra;
        switch (r->op) {
        case CMD_END: return;
        case CMD_JUMP:
            p = r->x[0];
            if (!(k = cmd_chunk_for(p))) return;
            continue;
        case CMD_CALL:
            for (uint64_t i = 0; i < r->x[0] && depth < 8; i++) fs_walk(((const uint64_t *)x)[i], depth + 1, v, ucap, vcap);
            break;
        case CMD_BIND_UNIFORM_BUFFERS:   // (x0 stage, x1 first, x2 count; extra: count x {u64 address, u64 size})
            for (uint64_t i = 0; i < r->x[2] && i < 18; i++) fs_add(v, ((const uint64_t *)x)[2 * i], ((const uint64_t *)x)[2 * i + 1], ucap);
            break;
        case CMD_BIND_VERTEX_BUFFERS:    // (x0 first, x1 count; extra: count x {u64 address, u64 size})
            for (uint64_t i = 0; vcap && i < r->x[1] && i < 16; i++) fs_add(v, ((const uint64_t *)x)[2 * i], ((const uint64_t *)x)[2 * i + 1], vcap);
            break;
        default: break;
        }
        if (!r->size) return;   // (never seen: a broken record would loop here)
        p += cmd_gsize(r->size);
    }
}
static int fs_cmp(const void *a, const void *b) {
    const uint64_t x = ((const FsRange *)a)->addr, y = ((const FsRange *)b)->addr;
    return x < y ? -1 : x > y;
}
#ifdef _WIN32
// [a, a + len) on committed, readable pages: one VirtualQuery per memory region (the last one is cached). A range inside
// the guest heap's committed part needs none (hdr 11-05, cand0 ring arm 23 vs 73 fps: ~25-50 ms of capture per submit,
// about constant per submit, i.e. not the copies: VirtualQuery's RegionSize walks the page tables from the address to the
// end of a region with one state, and the committed heap is one region of GBs). The heap is committed from its base
// upwards and TotK never decommits below the top (heap_committed_whole: no FreeMemoryBlock hole). LARECOMP_FRAME_SNAP_VQ=1:
// always VirtualQuery (the old path).
int heap_committed_whole(void);   // hle_os_mem.cpp
static bool fs_readable(uint64_t a, uint64_t len, uint64_t *lo, uint64_t *hi) {
    static int vq = -1;
    if (vq < 0) { const char *e = getenv("LARECOMP_FRAME_SNAP_VQ"); vq = e && *e == '1'; }
    const uint64_t hb = g_rt_heap_base, hs = kernel_heap_size();
    if (!vq && hb && a >= hb && a + len <= hb + hs && heap_committed_whole()) return true;
    for (uint64_t p = a, e = a + len; p < e;) {
        if (p >= *lo && p < *hi) { p = *hi; continue; }
        MEMORY_BASIC_INFORMATION mi;
        if (!VirtualQuery((const void *)(uintptr_t)p, &mi, sizeof mi) || mi.State != MEM_COMMIT || (mi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) ||
            !(mi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)))
            return false;
        *lo = (uint64_t)(uintptr_t)mi.BaseAddress, *hi = *lo + mi.RegionSize;
    }
    return true;
}
#endif

static atomic_ullong st_submits, st_ranges, st_bytes, st_ns, st_over, st_gone, st_cut, st_patched, st_lived, st_lost, st_gpuw;
static FrameSnap *g_cur;   // (render thread: the snapshot of the list being replayed)
static atomic_int g_fs_used;   // (a snapshot was taken: the replay's writes are logged from then on)

// ---- the replay's guest writes, ticketed (any thread)
enum { FW_N = 1 << 16 };
typedef struct { atomic_ullong seq; uint64_t addr, len, dpos; int gpu; } FsWrite;   // (seq 0 while being written; dpos: the
                                                                                     // bytes in the data ring + 1, 0 none)
static FsWrite g_fw[FW_N];
static atomic_ullong g_fw_seq;   // (the last ticket given)
// The bytes of small CPU writes (<= FWD_MAX: inline uniform updates, 16-byte reports) kept with them: the fold at a later
// list's apply takes these, not the live bytes, which the game may have rewritten since that list's submit. Big writes, or
// bytes the ring has overwritten since, fold from live memory.
enum { FWD_N = 4 << 20, FWD_MAX = 4096 };
static uint8_t g_fwd[FWD_N];
static atomic_ullong g_fwd_pos;   // (bytes ever reserved)
static void fw_log(uint64_t addr, uint64_t len, int gpu, const void *src) {
    uint64_t dpos = 0;
    if (!gpu && src && len <= FWD_MAX) {
        const uint64_t at = atomic_fetch_add(&g_fwd_pos, len), o = at % FWD_N, k = FWD_N - o < len ? FWD_N - o : len;
        memcpy(g_fwd + o, src, k);
        if (k < len) memcpy(g_fwd, (const uint8_t *)src + k, len - k);
        dpos = at + 1;
    }
    const uint64_t q = atomic_fetch_add(&g_fw_seq, 1) + 1;
    FsWrite *w = &g_fw[q & (FW_N - 1)];
    atomic_store_explicit(&w->seq, 0, memory_order_relaxed);
    atomic_thread_fence(memory_order_release);   // (seqlock: the 0 is visible before any of the new fields)
    w->addr = addr, w->len = len, w->gpu = gpu, w->dpos = dpos;
    atomic_store_explicit(&w->seq, q, memory_order_release);
}
// a logged write's kept bytes into buf (len <= FWD_MAX): false if it has none or the ring overwrote them meanwhile
static bool fw_bytes(uint64_t dpos, uint64_t len, uint8_t *buf) {
    if (!dpos) return false;
    const uint64_t at = dpos - 1, o = at % FWD_N, k = FWD_N - o < len ? FWD_N - o : len;
    memcpy(buf, g_fwd + o, k);
    if (k < len) memcpy(buf + k, g_fwd, len - k);
    atomic_thread_fence(memory_order_acquire);
    return atomic_load(&g_fwd_pos) - at <= FWD_N;   // (still within the ring after the copy: not overwritten)
}
// the first range whose end passes addr (the ranges are sorted and disjoint)
static uint32_t fs_first(const FrameSnap *s, uint64_t addr) {
    uint32_t lo = 0, hi = s->n;
    while (lo < hi) { const uint32_t m = (lo + hi) / 2; if (s->r[m].addr + s->r[m].len <= addr) lo = m + 1; else hi = m; }
    return lo;
}
// [addr, addr + len) of snapshot s: its bytes copied from src (gpu = 0), or its ranges read live from now (gpu = 1)
static void fs_fold(FrameSnap *s, uint64_t addr, uint64_t len, const uint8_t *src, int gpu) {
    for (uint32_t i = fs_first(s, addr); i < s->n && s->r[i].addr < addr + len; i++) {
        FsRange *r = &s->r[i];
        if (gpu) { r->live = 1; continue; }
        const uint64_t a = addr > r->addr ? addr : r->addr, e = addr + len < r->addr + r->len ? addr + len : r->addr + r->len;
        if (a < e) memcpy(s->data + r->off + (a - r->addr), src + (a - addr), e - a);
    }
}

// ---- the arenas
enum { FS_POOL_N = 64 };
#define FS_POOL_BYTES (256ull << 20)
static FrameSnap *g_pool[FS_POOL_N];
static int g_npool;
static uint64_t g_pool_bytes;
static pthread_mutex_t g_pool_mu = PTHREAD_MUTEX_INITIALIZER;
static FrameSnap *fs_alloc(uint64_t need) {   // (game thread)
    FrameSnap *s = NULL;
    pthread_mutex_lock(&g_pool_mu);
    int best = -1;
    for (int i = 0; i < g_npool; i++) if (g_pool[i]->cap >= need && (best < 0 || g_pool[i]->cap < g_pool[best]->cap)) best = i;
    if (best >= 0) s = g_pool[best], g_pool[best] = g_pool[--g_npool], g_pool_bytes -= s->cap;
    pthread_mutex_unlock(&g_pool_mu);
    if (!s) {
        const uint64_t cap = need + (need >> 2);   // (a quarter more: the next frames' lists grow a little)
        if (!(s = malloc(cap))) return NULL;
        s->cap = cap;
    }
    return s;
}
static void fs_free(FrameSnap *s) {   // (render thread)
    if (!s) return;
    pthread_mutex_lock(&g_pool_mu);
    if (g_npool < FS_POOL_N && g_pool_bytes + s->cap <= FS_POOL_BYTES) g_pool[g_npool++] = s, g_pool_bytes += s->cap, s = NULL;
    else if (g_npool) {   // (full: keep the larger, within the byte cap)
        int small = 0;
        for (int i = 1; i < g_npool; i++) if (g_pool[i]->cap < g_pool[small]->cap) small = i;
        if (g_pool[small]->cap < s->cap && g_pool_bytes - g_pool[small]->cap + s->cap <= FS_POOL_BYTES) {
            FrameSnap *t = g_pool[small];
            g_pool_bytes += s->cap - t->cap, g_pool[small] = s, s = t;
        }
    }
    pthread_mutex_unlock(&g_pool_mu);
    free(s);
}

// exec.c vk_snap_src: the snapshot's bytes of [addr, addr + size), or NULL (not taken: read live)
static const void *fs_src(uint64_t addr, uint64_t size) {
    const FrameSnap *s = g_cur;
    if (!s || !s->n) return NULL;
    uint32_t lo = 0, hi = s->n;   // (the last range starting at or before addr)
    while (hi - lo > 1) { const uint32_t m = (lo + hi) / 2; if (s->r[m].addr <= addr) lo = m; else hi = m; }
    const FsRange *r = &s->r[lo];
    return !r->live && r->addr <= addr && addr + size <= r->addr + r->len ? s->data + r->off + (addr - r->addr) : NULL;
}

static void fs_stats(void) {
    static uint64_t t0;
    const uint64_t now = SDL_GetTicksNS();
    if (!t0) t0 = now;
    if (now - t0 < 2000000000ull) return;
    const uint64_t n = atomic_exchange(&st_submits, 0);
    LOG("[framesnap] last %.1f s: %llu submits taken, %llu ranges, %.1f MB copied in %.2f ms on the game's threads; left live: %llu ranges over "
        "the size caps, %llu not committed, %llu shader-written, %llu past the per-submit cap; replay writes folded in at apply %llu, ranges turned live %llu, "
        "snapshots dropped (log lost) %llu\n",
        (now - t0) / 1e9, (unsigned long long)n, (unsigned long long)atomic_exchange(&st_ranges, 0), atomic_exchange(&st_bytes, 0) / 1048576.0,
        atomic_exchange(&st_ns, 0) / 1e6, (unsigned long long)atomic_exchange(&st_over, 0), (unsigned long long)atomic_exchange(&st_gone, 0),
        (unsigned long long)atomic_exchange(&st_gpuw, 0), (unsigned long long)atomic_exchange(&st_cut, 0), (unsigned long long)atomic_exchange(&st_patched, 0),
        (unsigned long long)atomic_exchange(&st_lived, 0), (unsigned long long)atomic_exchange(&st_lost, 0));
    t0 = now;
}

// exec.c backend_execute (async, the game's thread): the list's snapshot, or NULL (off / nothing bound)
void *frame_snap_take(uint64_t handle) {
    if (!fs_on()) return NULL;
    static uint64_t ucap, vcap, scap;
    if (!ucap) {
        ucap = fs_env_kb("LARECOMP_FRAME_SNAP_UBO_KB", 64), vcap = fs_env_kb("LARECOMP_FRAME_SNAP_VTX_KB", 0);
        scap = fs_env_kb("LARECOMP_FRAME_SNAP_MB", 32) << 10;
        __atomic_store_n(&vk_snap_src, fs_src, __ATOMIC_RELEASE);
        LOG("[framesnap] on: each submitted list's uniform ranges (<= %llu KB)%s copied at the submit, its replay reads them from there "
            "(LARECOMP_FRAME_SNAP=1; off by default)\n", (unsigned long long)(ucap >> 10), vcap ? " and vertex streams" : "");
    }
    atomic_store(&g_fs_used, 1);
    const uint64_t t0 = SDL_GetTicksNS();
    // (the log's sequence BEFORE the copy: a replay write racing the copy is folded in again at apply, never missed)
    const uint64_t wseq0 = atomic_load(&g_fw_seq);
    static _Thread_local FsVec v;   // (per game thread: several may submit)
    v.n = 0, v.over = 0;
    fs_walk(handle, 0, &v, ucap, vcap);
    atomic_fetch_add(&st_over, v.over);
    FrameSnap *s = NULL;
    if (v.n) {
        qsort(v.v, v.n, sizeof *v.v, fs_cmp);
        uint32_t m = 0;   // (merged in place: overlapping or touching ranges become one)
        for (uint32_t i = 0; i < v.n; i++) {
            if (m && v.v[i].addr <= v.v[m - 1].addr + v.v[m - 1].len) {
                const uint64_t e = v.v[i].addr + v.v[i].len;
                if (e > v.v[m - 1].addr + v.v[m - 1].len) v.v[m - 1].len = e - v.v[m - 1].addr;
            } else v.v[m++] = v.v[i];
        }
        uint64_t total = 0, lo = 0, hi = 0;
        uint32_t k = 0;   // (kept: committed, within the per-submit cap)
        for (uint32_t i = 0; i < m; i++) {
#ifdef _WIN32
            if (!fs_readable(v.v[i].addr, v.v[i].len, &lo, &hi)) { atomic_fetch_add(&st_gone, 1); continue; }
#endif
            // (a range in heap pages a shader has written: its bytes reach host memory on the GPU timeline, maybe after
            // this copy; read live as before, hdr's interface: CPU-written memory only)
            const uint64_t hb = g_rt_heap_base, hs = kernel_heap_size();
            if (hb && v.v[i].addr >= hb && v.v[i].addr + v.v[i].len <= hb + hs && vk_heap_gpu_written_since(v.v[i].addr, v.v[i].len, 1)) {
                atomic_fetch_add(&st_gpuw, 1);
                continue;
            }
            if (total + v.v[i].len > scap) { atomic_fetch_add(&st_cut, 1); continue; }
            v.v[i].off = total;
            total += v.v[i].len;
            v.v[k++] = v.v[i];
        }
        if (k && (s = fs_alloc(sizeof *s + k * sizeof(FsRange) + total))) {
            s->n = k;
            s->wseq0 = wseq0;
            s->r = (FsRange *)(s + 1);
            s->data = (uint8_t *)(s->r + k);
            memcpy(s->r, v.v, k * sizeof(FsRange));
            for (uint32_t i = 0; i < k; i++) memcpy(s->data + s->r[i].off, (const void *)(uintptr_t)s->r[i].addr, s->r[i].len);
            atomic_fetch_add(&st_ranges, k);
            atomic_fetch_add(&st_bytes, total);
        }
    }
    atomic_fetch_add(&st_submits, 1);
    atomic_fetch_add(&st_ns, SDL_GetTicksNS() - t0);
    fs_stats();
    return s;
}
// exec.c async_main, AOP_EXEC (render thread): the list's snapshot is current while it is replayed; NULL after it (back to
// the pool). Becoming current, it folds in the replay's writes since its capture (the lists replayed in between).
static _Thread_local int t_render;   // (the render thread: the one that applies)
void frame_snap_apply(void *snap) {
    t_render = 1;
    if (g_cur && g_cur != snap) fs_free(g_cur);
    g_cur = snap;
    FrameSnap *s = snap;
    if (!s) return;
    const uint64_t end = atomic_load(&g_fw_seq);
    if (end - s->wseq0 >= FW_N) { s->n = 0; atomic_fetch_add(&st_lost, 1); return; }   // (the log lost entries: live)
    for (uint64_t q = s->wseq0 + 1; q <= end; q++) {
        FsWrite *w = &g_fw[q & (FW_N - 1)];
        uint64_t sq = atomic_load_explicit(&w->seq, memory_order_acquire);
        for (int t = 0; sq != q && (int64_t)(sq - q) < 0 && t < 4096; t++)   // (ticket taken, entry not yet published:
            fs_pause(), sq = atomic_load_explicit(&w->seq, memory_order_acquire);   //  another thread's note, a few ns)
        if (sq != q) { s->n = 0; atomic_fetch_add(&st_lost, 1); return; }
        const uint64_t addr = w->addr, len = w->len, dpos = w->dpos;
        const int gpu = w->gpu;
        atomic_thread_fence(memory_order_acquire);   // (seqlock: the fields are read before the second seq load)
        if (atomic_load_explicit(&w->seq, memory_order_relaxed) != q) { s->n = 0; atomic_fetch_add(&st_lost, 1); return; }
        // (CPU writes: their own bytes when kept, else the live bytes the replay wrote there in order)
        static uint8_t kept[FWD_MAX];
        const bool own = !gpu && len <= FWD_MAX && fw_bytes(dpos, len, kept);
        fs_fold(s, addr, len, own ? kept : (const uint8_t *)(uintptr_t)addr, gpu);
        atomic_fetch_add(gpu ? &st_lived : &st_patched, 1);
    }
}
// exec.c, the replay writing guest memory itself with the bytes at hand (UPDATE_UNIFORM_BUFFER, ReportCounter,
// COPY_BUFFER_TO_BUFFER): the current list's snapshot takes them now; later lists' at their apply (logged)
void frame_snap_write(uint64_t addr, const void *src, uint64_t len) {
    if (!len || !atomic_load_explicit(&g_fs_used, memory_order_relaxed)) return;
    fw_log(addr, len, 0, src);
    if (g_cur) fs_fold(g_cur, addr, len, src, 0);
}
// exec.c / textures.c: a guest write the snapshot cannot hold (on the GPU timeline: gdev_writeback; on another thread:
// the CPU-read write-backs): the overlapping ranges of the current and of every later-applied snapshot are read live
void frame_snap_note(uint64_t addr, uint64_t len) {
    if (!len || !atomic_load_explicit(&g_fs_used, memory_order_relaxed)) return;
    fw_log(addr, len, 1, NULL);
    if (t_render && g_cur) fs_fold(g_cur, addr, len, NULL, 1);   // (g_cur is the render thread's)
}
