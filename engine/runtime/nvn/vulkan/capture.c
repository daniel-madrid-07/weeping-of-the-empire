// Render-thread capture for tools/engine/replay. See capture.h for the format.
#include "capture.h"
#include <windows.h>
#include "../tegra.h"   // (tegra_total_size: a texture's guest storage, LARECOMP_CAPTURE_FULL)

extern uint64_t g_rt_heap_base, g_alias_import_base, g_alias_import_size;
uint64_t kernel_heap_size(void);
uint64_t nvn_present_count(void);

static int g_on = -1;               // LARECOMP_CAPTURE set
static bool g_started, g_done;
static FILE *g_f;
static uint64_t g_frame_ms, g_frame_bytes0, g_frame_lists0;
static uint64_t g_at, g_at_ms, g_t0_ms, g_frames, g_start_present, g_presents_seen, g_lists, g_bytes;
static SRWLOCK g_mu = SRWLOCK_INIT;

// ---- writer
static void put(uint32_t tag, uint32_t id, const void *a, uint64_t na, const void *b, uint64_t nb) {
    CapRec h = {tag, id, na + nb};
    fwrite(&h, sizeof h, 1, g_f);
    if (na) fwrite(a, 1, na, g_f);
    if (nb) fwrite(b, 1, nb, g_f);
    static const uint8_t z[8];
    uint64_t pad = (8 - (na + nb) % 8) % 8;
    if (pad) fwrite(z, 1, pad, g_f);
    g_bytes += sizeof h + na + nb + pad;
}

// ---- ids: host pointer -> 1-based id per kind (open addressing; never shrinks)
typedef struct { const void *p; uint32_t id; } IdEnt;
static IdEnt *g_ids[CAPK_N]; static uint32_t g_idcap[CAPK_N], g_nid[CAPK_N];
static uint32_t id_find(int kind, const void *p, bool *fresh) {
    if (fresh) *fresh = false;
    if (!p) return 0;
    if ((g_nid[kind] + 1) * 2 > g_idcap[kind]) {
        IdEnt *o = g_ids[kind]; uint32_t oc = g_idcap[kind];
        g_idcap[kind] = oc ? oc * 2 : 1024;
        g_ids[kind] = calloc(g_idcap[kind], sizeof(IdEnt));
        for (uint32_t i = 0; i < oc; i++) if (o[i].p) {
            uint32_t s = (uint32_t)(((uintptr_t)o[i].p >> 4) * 0x9E3779B97F4A7C15ull >> 32) & (g_idcap[kind] - 1);
            while (g_ids[kind][s].p) s = (s + 1) & (g_idcap[kind] - 1);
            g_ids[kind][s] = o[i];
        }
        free(o);
    }
    uint32_t s = (uint32_t)(((uintptr_t)p >> 4) * 0x9E3779B97F4A7C15ull >> 32) & (g_idcap[kind] - 1);
    while (g_ids[kind][s].p && g_ids[kind][s].p != p) s = (s + 1) & (g_idcap[kind] - 1);
    if (!g_ids[kind][s].p) { g_ids[kind][s].p = p; g_ids[kind][s].id = ++g_nid[kind]; if (fresh) *fresh = true; }
    return g_ids[kind][s].id;
}

// ---- guest memory: copy only what is committed and readable; emit a range again only when its bytes change
static uint32_t g_rc_gen;   // (capture_exec bumps it: readable_len's region cache)
static uint64_t readable_len(uint64_t addr, uint64_t len) {
    // the last readable regions VirtualQuery returned: a range inside one needs no query (gc-cap3 spent all its capture
    // time in VirtualQuery; per-draw constants are new ranges in the same few heap regions)
    // (the cache is dropped at every execute: regions can be decommitted between lists, not while one is captured)
    static struct { uint64_t lo, hi; } rc[8]; static int rn; static uint32_t rgen;
    if (rgen != g_rc_gen) { memset(rc, 0, sizeof rc); rgen = g_rc_gen; }
    for (int i = 0; i < 8; i++) if (addr >= rc[i].lo && addr + len <= rc[i].hi) return len;
    uint64_t done = 0;
    while (done < len) {
        MEMORY_BASIC_INFORMATION mi;
        if (!VirtualQuery((const void *)(uintptr_t)(addr + done), &mi, sizeof mi)) break;
        if (mi.State != MEM_COMMIT || (mi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) break;
        rc[rn++ & 7].lo = (uint64_t)(uintptr_t)mi.BaseAddress; rc[(rn - 1) & 7].hi = (uint64_t)(uintptr_t)mi.BaseAddress + mi.RegionSize;
        uint64_t end = (uint64_t)(uintptr_t)mi.BaseAddress + mi.RegionSize - (addr + done);
        done += end < len - done ? end : len - done;
    }
    return done;
}
static uint64_t fnv(const void *p, uint64_t n) {   // (word-wise: the byte loop made a capture frame take seconds)
    const uint8_t *b = p; uint64_t h = 1469598103934665603ull ^ n, w;
    uint64_t i = 0;
    for (; i + 8 <= n; i += 8) { memcpy(&w, b + i, 8); h = (h ^ w) * 0x9E3779B97F4A7C15ull; h ^= h >> 29; }
    for (; i < n; i++) h = (h ^ b[i]) * 1099511628211ull;
    return h;
}
typedef struct { uint64_t addr, len, h; } MemEnt;
#define MEMTAB (1u << 18)
static MemEnt *g_mem;
// Each (address, length) is captured the first time a list names it: hashing every named range at every bind kept a
// capture frame from ending (boots gc-w2-base, gc-cap2: the first execute was still being captured a minute later,
// all in cap_mem). Later changes of the bytes (per-frame constants rewritten in place) are not followed: the replay
// times the same copies and dedupes of slightly older data. LARECOMP_CAPTURE_REFRESH=1 hashes again and follows them.
static uint64_t g_memcalls, g_memnew;
static void cap_mem(uint64_t addr, uint64_t len, uint64_t cap) {
    if (!addr || !len) return;
    g_memcalls++;
    static uint64_t tlog;
    if (GetTickCount64() - tlog > 2000) {   // (a capture that does not end its frame says why)
        tlog = GetTickCount64();
        LOG("[capture] progress: %llu lists, %llu ranges named (%llu new), %.1f MB written\n", (unsigned long long)g_lists,
            (unsigned long long)g_memcalls, (unsigned long long)g_memnew, g_bytes / 1e6);
    }
    if (len > cap) len = cap;
    if (!g_mem) g_mem = calloc(MEMTAB, sizeof *g_mem);
    static int refresh = -1;
    if (refresh < 0) { const char *e = getenv("LARECOMP_CAPTURE_REFRESH"); refresh = e && *e == '1'; }
    uint32_t s = (uint32_t)((addr * 0x9E3779B97F4A7C15ull ^ len) >> 40) & (MEMTAB - 1);
    MemEnt *slot = NULL;
    for (uint32_t probe = 0; probe < 64; probe++, s = (s + 1) & (MEMTAB - 1)) {
        MemEnt *e = &g_mem[s];
        if (e->addr == addr && e->len == len) { if (!refresh) return; slot = e; break; }
        if (!e->addr) { e->addr = addr; e->len = len; e->h = 0; slot = e; break; }
    }
    uint64_t n = readable_len(addr, len);
    if (!n) return;
    if (refresh && slot) {
        uint64_t h = fnv((const void *)(uintptr_t)addr, n);
        if (slot->h == h) return;
        slot->h = h;
    }
    g_memnew++;
    put(CAP_MEM, 0, &addr, 8, (const void *)(uintptr_t)addr, n);
}

// LARECOMP_CAPTURE_FULL=1: the capture also holds what the frames read from guest memory that a plain capture leaves to
// the demand-zero heap: the storage of every texture the lists bind (texture pools' entries the draws name, images,
// render targets, copy / clear sources and targets), the heap's vertex buffers, and storage / index ranges whole (up to
// 64 MB each). Textures rendered on the GPU in earlier frames (history targets) cannot be in it: both arms of a replay
// A/B start them at zero. A few frames with LARECOMP_CAPTURE_MAX_MB raised (several GB in a city).
static int cap_full(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("LARECOMP_CAPTURE_FULL"); v = e && *e == '1'; if (v) LOG("[capture] full: texture storage and heap vertex data too\n"); }
    return v;
}
#define FULL_CAP (64ull << 20)
static void cap_tex_storage(const NvnTexture *t) {
    if (!cap_full() || !t || !t->addr) return;
    uint64_t n = tegra_total_size(&t->layout);
    if (n) cap_mem(t->addr, n, 1ull << 31);
}
static const NvnDescPool *g_walk_tpool;   // (the texture pool set last in list order: what texture handles index)
static void cap_handles(const uint64_t *h, uint64_t n) {
    if (!cap_full() || !g_walk_tpool || !g_walk_tpool->entries) return;
    for (uint64_t i = 0; i < n; i++) {
        uint32_t tic = (uint32_t)(h[i] & 0xFFFFF);
        if (h[i] && tic < (uint32_t)g_walk_tpool->count) cap_tex_storage(((const NvnTexEntry *)g_walk_tpool->entries)[tic].tex);
    }
}

// ---- objects (emitted before the first list that names them)
static uint32_t cap_mempool(const NvnPool *p) {
    bool fresh; uint32_t id = id_find(CAPK_MEMPOOL, p, &fresh);
    if (fresh) put(CAP_MEMPOOL, id, p, sizeof *p, NULL, 0);
    return id;
}
static uint32_t cap_tex(const NvnTexture *t) {
    bool fresh; uint32_t id = id_find(CAPK_TEX, t, &fresh);
    if (!fresh) return id;
    NvnTexture c = *t;
    c.pool = (NvnPool *)(uintptr_t)cap_mempool(t->pool);
    c.backend = NULL; c.zombie = c.zombie_gpu = c.fin_queued = c.release_queued = false;
    put(CAP_TEX, id, &c, sizeof c, NULL, 0);
    return id;
}
static uint32_t cap_prog(const NvnProgram *p) {
    bool fresh; uint32_t id = id_find(CAPK_PROG, p, &fresh);
    if (!fresh) return id;
    NvnProgram c = *p;
    c.backend = NULL;
    for (int i = 0; i < 6; i++) {
        c.stages[i].backend = NULL;
        const NvnShaderStage *s = &p->stages[i];
        if (!s->code_addr) continue;
        uint64_t cmax = s->const_offset ? s->const_offset : 0x10000, csz = s->const_size < 0x10000 ? s->const_size : 0x10000;
        uint64_t n = s->const_offset + csz > cmax ? s->const_offset + csz : cmax;
        cap_mem(s->code_addr, n, 0x80000);
        if (s->control) cap_mem((uint64_t)(uintptr_t)s->control, 0x800, 0x800);
    }
    put(CAP_PROG, id, &c, sizeof c, NULL, 0);
    return id;
}
// descriptor pools: contents compared against what was last written, re-emitted on change
typedef struct { const NvnDescPool *p; bool sampler; void *last; size_t lastn; } PoolWatch;
static PoolWatch g_pw[64]; static int g_npw;
static void cap_pool_contents(PoolWatch *w) {
    const NvnDescPool *p = w->p;
    size_t es = w->sampler ? sizeof(NvnSamplerDesc) : sizeof(NvnTexEntry), n = (size_t)(p->count > 0 ? p->count : 0) * es;
    if (w->last && w->lastn == n && p->entries && !memcmp(w->last, p->entries, n)) return;
    free(w->last); w->last = malloc(n ? n : 1); w->lastn = n;
    if (n && p->entries) memcpy(w->last, p->entries, n);
    uint8_t *c = malloc(n ? n : 1);
    if (n && p->entries) memcpy(c, p->entries, n);
    for (int i = 0; i < p->count; i++) {
        if (w->sampler) ((NvnSamplerDesc *)c)[i].backend = NULL;
        else { NvnTexEntry *e = &((NvnTexEntry *)c)[i]; e->tex = (NvnTexture *)(uintptr_t)(e->tex ? cap_tex(e->tex) : 0); }
    }
    CapPoolHdr h = {p->addr, p->count, 0};
    uint32_t id = id_find(w->sampler ? CAPK_SPOOL : CAPK_TPOOL, p, NULL);
    put(w->sampler ? CAP_SPOOL : CAP_TPOOL, id, &h, sizeof h, c, n);
    free(c);
}
static uint32_t cap_dpool(const NvnDescPool *p, bool sampler) {
    if (!p) return 0;
    bool fresh; uint32_t id = id_find(sampler ? CAPK_SPOOL : CAPK_TPOOL, p, &fresh);
    if (fresh && g_npw < 64) { g_pw[g_npw] = (PoolWatch){p, sampler, NULL, 0}; cap_pool_contents(&g_pw[g_npw]); g_npw++; }
    return id;
}
static uint64_t id_of(int kind, const void *p) {
    switch (kind) {
    case CAPK_TEX: return p ? cap_tex(p) : 0;
    case CAPK_PROG: return p ? cap_prog(p) : 0;
    case CAPK_TPOOL: return cap_dpool(p, false);
    case CAPK_SPOOL: return cap_dpool(p, true);
    default: return 0;
    }
}

// ---- lists
#define TEXREF_SIZE ((uint32_t)sizeof(CapTexRef))
typedef struct { uint8_t *b; size_t n, cap; } Buf;
static void buf_add(Buf *o, const void *p, size_t n) {
    if (o->n + n > o->cap) { o->cap = (o->n + n) * 2 + 4096; o->b = realloc(o->b, o->cap); }
    memcpy(o->b + o->n, p, n); o->n += n;
}
static uint32_t g_list_id;
// lists already captured during the current top-level execute (a CALLed list runs unchanged within one execute)
#define LMEMO 4096
static struct { uint64_t handle; uint32_t id, exec; } g_lmemo[LMEMO];
static uint32_t g_exec_no;
static uint64_t g_bad_recs;
static uint32_t cap_list(uint64_t handle, int depth) {
    const CmdChunk *k = cmd_chunk_for(handle);
    if (!k || depth > 8) return 0;
    uint32_t ms = (uint32_t)((handle >> 3) * 0x9E3779B97F4A7C15ull >> 40) & (LMEMO - 1);
    if (g_lmemo[ms].handle == handle && g_lmemo[ms].exec == g_exec_no) return g_lmemo[ms].id;
    Buf o = {0};
    uint64_t p = handle;
    for (int guard = 0; guard < 4000000; guard++) {
        CmdRec *r = cmd_at(k, p);
        if (r->size < sizeof(CmdRec) || r->op >= CMD_COUNT) {   // (not a record: end the list here, as exec_list would loop)
            if (g_bad_recs++ < 8) LOG("[capture] list %llx: bad record at %llx (op %u size %u): list cut\n", (unsigned long long)handle,
                                      (unsigned long long)p, r->op, r->size);
            CmdRec e = {CMD_END, (uint32_t)sizeof(CmdRec)};
            buf_add(&o, &e, sizeof e);
            break;
        }
        uint32_t size = r->size;
        if (r->op == CMD_JUMP) {
            p = r->x[0];
            if (!(k = cmd_chunk_for(p))) break;
            continue;
        }
        size_t at = o.n;
        buf_add(&o, r, size);
        CmdRec *c = (CmdRec *)(o.b + at);
        uint8_t *x = c->extra;
        switch (r->op) {
        case CMD_CALL: {
            const uint64_t *h = (const uint64_t *)r->extra;
            for (uint64_t i = 0; i < r->x[0] && (8 + i * 8) <= size - sizeof(CmdRec); i++) {
                uint32_t id = cap_list(h[i], depth + 1);
                c = (CmdRec *)(o.b + at); x = c->extra;   // (o may have moved? no: callee lists use their own Buf)
                ((uint64_t *)x)[i] = id;
            }
            break;
        }
        case CMD_BIND_PROGRAM: c->x[0] = cap_prog((const NvnProgram *)(uintptr_t)r->x[0]); break;
        case CMD_SET_TEXTURE_POOL:
            g_walk_tpool = (const NvnDescPool *)(uintptr_t)r->x[0];
            c->x[0] = cap_dpool(g_walk_tpool, false);
            break;
        case CMD_BIND_TEXTURES: case CMD_BIND_IMAGES:
            cap_handles((const uint64_t *)r->extra, r->x[2] < 32 ? r->x[2] : 32);
            break;
        case CMD_SET_SAMPLER_POOL: c->x[0] = cap_dpool((const NvnDescPool *)(uintptr_t)r->x[0], true); break;
        case CMD_SET_RENDER_TARGETS: {
            CapRtRec *rt = (CapRtRec *)x;
            for (int i = 0; i < 8; i++) {
                cap_tex_storage((const NvnTexture *)(uintptr_t)rt->tex[i]);
                rt->tex[i] = rt->tex[i] ? cap_tex((const NvnTexture *)(uintptr_t)rt->tex[i]) : 0;
            }
            cap_tex_storage((const NvnTexture *)(uintptr_t)rt->depth);
            rt->depth = rt->depth ? cap_tex((const NvnTexture *)(uintptr_t)rt->depth) : 0;
            break;
        }
        case CMD_CLEAR_TEXTURE: case CMD_COPY_BUFFER_TO_TEXTURE: case CMD_COPY_TEXTURE_TO_BUFFER: {
            uint64_t *t = (uint64_t *)x;
            cap_tex_storage((const NvnTexture *)(uintptr_t)*t);
            *t = *t ? cap_tex((const NvnTexture *)(uintptr_t)*t) : 0;
            break;
        }
        case CMD_COPY_TEXTURE_TO_TEXTURE: case CMD_DOWNSAMPLE: {
            for (int j = 0; j < 2; j++) {
                uint64_t *t = (uint64_t *)(x + j * TEXREF_SIZE);
                cap_tex_storage((const NvnTexture *)(uintptr_t)*t);
                *t = *t ? cap_tex((const NvnTexture *)(uintptr_t)*t) : 0;
            }
            break;
        }
        case CMD_FENCE_SYNC: c->x[1] = 0; break;   // (the guest NvnSync: nothing waits on it in the replay)
        case CMD_BIND_UNIFORM_BUFFERS: case CMD_BIND_STORAGE_BUFFERS: {
            const uint64_t *rg = (const uint64_t *)r->extra;
            for (uint64_t i = 0; i < r->x[2] && 16 * (i + 1) <= size - sizeof(CmdRec); i++)
                cap_mem(rg[2 * i], rg[2 * i + 1], r->op == CMD_BIND_UNIFORM_BUFFERS ? 0x10000 : cap_full() ? FULL_CAP : 0x4000);
            break;
        }
        case CMD_BIND_VERTEX_BUFFERS: {   // outside the heap they are copied on the CPU (guest_copy): those must exist
            const uint64_t *rg = (const uint64_t *)r->extra;
            for (uint64_t i = 0; i < r->x[1] && 16 * (i + 1) <= size - sizeof(CmdRec); i++)
                if (cap_full()) cap_mem(rg[2 * i], rg[2 * i + 1], FULL_CAP);   // (heap ones too: their bytes are the mesh)
                else if (rg[2 * i] < g_rt_heap_base || rg[2 * i] >= g_rt_heap_base + kernel_heap_size()) cap_mem(rg[2 * i], rg[2 * i + 1], 0x40000);
            break;
        }
        case CMD_DRAW_ELEMENTS: {
            uint64_t isz = r->x[1] == 0 ? 1 : r->x[1] == 1 ? 2 : 4;
            cap_mem(r->x[3], r->x[2] * isz, cap_full() ? FULL_CAP : 0x400000);
            break;
        }
        case CMD_BIND_TEXTURES_DEFERRED:
            cap_mem(r->x[3], r->x[2] * 8, 0x1000);
            if (r->x[3]) cap_handles((const uint64_t *)(uintptr_t)r->x[3], r->x[2] < 32 ? r->x[2] : 32);
            break;
        case CMD_SET_RENDER_ENABLE: if (r->x[0] >= 2) cap_mem(r->x[1], 32, 32); break;
        default: break;
        }
        if (r->op == CMD_END) break;
        p += cmd_gsize(r->size);
    }
    uint32_t id = ++g_list_id;
    put(CAP_LIST, id, o.b, o.n, NULL, 0);
    free(o.b);
    g_lists++;
    g_lmemo[ms].handle = handle; g_lmemo[ms].id = id; g_lmemo[ms].exec = g_exec_no;
    return id;
}

bool capture_on(void) {
    if (g_on < 0) {
        const char *e = getenv("LARECOMP_CAPTURE");
        g_on = e && *e;
        if (g_on) {
            const char *a = getenv("LARECOMP_CAPTURE_AT"), *n = getenv("LARECOMP_CAPTURE_FRAMES");
            char *end = NULL;
            g_at = a ? strtoull(a, &end, 0) : 600; g_frames = n ? strtoull(n, NULL, 0) : 30;
            if (end && *end == 's') { g_at_ms = g_at * 1000; g_at = 0; }   // "<n>s": n seconds after the first execute
            g_t0_ms = GetTickCount64();
            LOG("[capture] %s: %llu frames from %s %llu (synchronous execute only)\n", e, (unsigned long long)g_frames,
                g_at_ms ? "second" : "present", (unsigned long long)(g_at_ms ? g_at_ms / 1000 : g_at));
        }
    }
    return g_on && !g_done;
}
static void finish(void) {
    put(CAP_END, 0, NULL, 0, NULL, 0);
    fclose(g_f); g_f = NULL; g_done = true;
    LOG("[capture] done: %llu frames, %llu lists, %.1f MB\n", (unsigned long long)g_presents_seen, (unsigned long long)g_lists, g_bytes / 1e6);
}
// A capture is game data (shader code, constants, vertex data) and can grow large: never inside a git work tree
// (refused when the target's folder or one above it holds.git), not with less than LARECOMP_CAPTURE_MIN_FREE_GB (45)
// free on its drive, and finished cleanly at LARECOMP_CAPTURE_MAX_MB (1024; 0: no cap). NULL = fine, else why not.
static const char *capture_path_refused(const char *path, char *why, size_t n) {
    char full[MAX_PATH], dir[MAX_PATH];
    if (!GetFullPathNameA(path, sizeof full, full, NULL)) { snprintf(why, n, "no full path"); return why; }
    snprintf(dir, sizeof dir, "%s", full);
    for (char *s = strrchr(dir, '\\'); s; s = strrchr(dir, '\\')) {
        *s = 0;
        char g[MAX_PATH];
        snprintf(g, sizeof g, "%s\\.git", dir);
        if (GetFileAttributesA(g) != INVALID_FILE_ATTRIBUTES) { snprintf(why, n, "inside the git work tree %s (game data stays out of repos)", dir); return why; }
    }
    snprintf(dir, sizeof dir, "%s", full);
    char *s = strrchr(dir, '\\');
    if (s) s[1] = 0;
    ULARGE_INTEGER avail;
    const char *e = getenv("LARECOMP_CAPTURE_MIN_FREE_GB");
    uint64_t min_gb = e ? strtoull(e, NULL, 10) : 45;
    if (GetDiskFreeSpaceExA(dir, &avail, NULL, NULL) && avail.QuadPart < (min_gb << 30)) {
        snprintf(why, n, "%.1f GB free on its drive, under %llu GB", avail.QuadPart / 1073741824.0, (unsigned long long)min_gb);
        return why;
    }
    return NULL;
}
static uint64_t capture_max_bytes(void) {
    static int64_t cap = -1;
    if (cap < 0) { const char *e = getenv("LARECOMP_CAPTURE_MAX_MB"); cap = (int64_t)(e ? strtoull(e, NULL, 10) : 1024) << 20; }
    return (uint64_t)cap;
}
void capture_exec(uint64_t handle) {
    if (!capture_on()) return;
    extern bool backend_async_on(void);
    AcquireSRWLockExclusive(&g_mu);
    if (!g_started && nvn_present_count() >= g_at && GetTickCount64() - g_t0_ms >= g_at_ms) {
        const char *path = getenv("LARECOMP_CAPTURE");
        char why[MAX_PATH + 96];
        if (capture_path_refused(path, why, sizeof why)) {
            LOG("[capture] not writing %s: %s\n", path, why);
            g_done = true; ReleaseSRWLockExclusive(&g_mu); return;
        }
        g_f = fopen(path, "wb");
        if (!g_f) { LOG("[capture] cannot write %s\n", path); g_done = true; ReleaseSRWLockExclusive(&g_mu); return; }
        if (backend_async_on()) LOG("[capture] LARECOMP_ASYNC_EXEC is on: frames are cut at the game's presents, not the render thread's\n");
        fwrite(CAP_MAGIC, 1, 8, g_f); g_bytes = 8;
        CapHeader h = {g_rt_heap_base, kernel_heap_size(), g_frames, nvn_present_count(), g_alias_import_base, g_alias_import_size};
        g_start_present = h.start_present;
        put(CAP_HEADER, 0, &h, sizeof h, NULL, 0);
        size_t n = exec_state_size();
        void *s = malloc(n);
        exec_state_save(s, id_of);
        put(CAP_STATE, 0, s, n, NULL, 0);
        free(s);
        g_started = true;
    }
    if (g_started && !g_done) {
        uint64_t t0 = GetTickCount64();
        g_exec_no++; g_rc_gen++;
        for (int i = 0; i < g_npw; i++) cap_pool_contents(&g_pw[i]);
        uint32_t id = cap_list(handle, 0);
        put(CAP_EXEC, id, NULL, 0, NULL, 0);
        uint64_t dt = GetTickCount64() - t0;
        g_frame_ms += dt;
        // (a full capture writes each new texture and mesh the first time a list names it: its first executes take seconds)
        if (dt > (cap_full() ? 120000u : 5000u)) { LOG("[capture] one execute took %llu ms to capture: stopped\n", (unsigned long long)dt); finish(); }
        else if (capture_max_bytes() && g_bytes > capture_max_bytes()) {
            LOG("[capture] %.1f MB: over LARECOMP_CAPTURE_MAX_MB, stopped after %llu frames\n", g_bytes / 1e6, (unsigned long long)g_presents_seen);
            finish();
        }
    }
    ReleaseSRWLockExclusive(&g_mu);
}
void capture_present(NvnTexture *t) {
    if (!capture_on() || !g_started) return;
    AcquireSRWLockExclusive(&g_mu);
    if (!g_done) {
        put(CAP_PRESENT, t ? cap_tex(t) : 0, NULL, 0, NULL, 0);
        fflush(g_f);
        LOG("[capture] frame %llu: %llu lists, %.1f MB, %llu ms capturing\n", (unsigned long long)g_presents_seen + 1,
            (unsigned long long)(g_lists - g_frame_lists0), (g_bytes - g_frame_bytes0) / 1e6, (unsigned long long)g_frame_ms);
        g_frame_ms = 0; g_frame_bytes0 = g_bytes; g_frame_lists0 = g_lists;
        if (++g_presents_seen >= g_frames) finish();
    }
    ReleaseSRWLockExclusive(&g_mu);
}
