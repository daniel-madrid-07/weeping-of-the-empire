// TotK: stand-in for the VMA calls the LASRecomp Vulkan backend makes (shim/vma/vk_mem_alloc.h).
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <SDL3/SDL_timer.h>
#include <vma/vk_mem_alloc.h>

typedef struct SubBlock SubBlock;
struct VmaAllocator_T {
    VkDevice dev; VkPhysicalDeviceMemoryProperties mp; uint64_t vram_cap; uint32_t vram_heap; SubBlock *blocks; bool uma;
    bool prio;                                   // VK_EXT_memory_priority: priorities given at allocation (below)
    PFN_vkSetDeviceMemoryPriorityEXT set_prio;   // VK_EXT_pageable_device_local_memory: and kept current per block
};
// sub: the block it was carved from (at offset), NULL for a resource with its own vkAllocateMemory; cold: a sampled texture
// the CPU fills (vma_shim_cold), the rest (render targets, GPU-written images, buffers) hot
struct VmaAllocation_T { VkDeviceMemory mem; void *mapped; uint32_t type; VkDeviceSize size, offset; uint32_t tag; SubBlock *sub; bool cold; };
// Live bytes per caller tag (Deck d16 2026-10-16: 3 GB asked for in a 1 GB VRAM carve-out; which kind of image holds it?).
// The caller sets vma_shim_tag around a create (textures.c: 1 ASTC decoded to RGBA8, 2 other texture images, 3 block shadows).
_Thread_local int vma_shim_tag;
// Residency priority (P1.1: other apps took the card, WDDM demoted ours to system RAM, render targets as readily as textures
// unused for minutes GPU waits 1.3 s per 2 s). textures.c sets vma_shim_cold around the creation of a sampled texture the CPU
// fills (not a render target): those are the bytes a frame can lose to system RAM at the least cost. With
// VK_EXT_memory_priority an allocation of its own gets PRIO_COLD or PRIO_HOT, a block PRIO_BLOCK (neutral: both classes share
// kind 1 / 2 blocks, so a block made by a cold texture can hold render targets later, which a cold priority fixed at creation
// would demote first); with VK_EXT_pageable_device_local_memory a block follows what it holds: PRIO_HOT while any hot
// resource lives in it, PRIO_COLD once only cold ones do (vkSetDeviceMemoryPriorityEXT, when that count crosses 0). device.c
// LARECOMP_MEM_PRIORITY=0: none of it. Cold is the OS default's middle, not below it (public #86, RX 5600 XT on v0.1.9.1, a 4
// GB budget full: at 0.25 the 'minimum' class went first, 128-256 MB of texture blocks the frames sample all the time, paged
// in every frame at 15-24 fps; "not a render target" is no sign of "not in use"): only render targets are raised, and the
// video memory manager picks among the rest by use, as on v0.1.9.
_Thread_local int vma_shim_cold;
#define PRIO_HOT 1.0f
#define PRIO_COLD 0.5f
#define PRIO_BLOCK 0.5f   // (every block at its creation, spares too: the OS default's middle)
static _Atomic uint64_t tag_bytes[8];
static _Atomic uint32_t tag_count[8];
uint64_t vma_shim_tag_bytes(int tag, uint32_t *count) { if (count) *count = tag_count[tag & 7]; return tag_bytes[tag & 7]; }
VkDeviceSize vma_shim_size(VmaAllocation al) { return al ? al->size : 0; }
// live bytes / allocations per memory heap (device.c vk_mem_report, LARECOMP_MEMREPORT_AT)
static _Atomic uint64_t heap_bytes[VK_MAX_MEMORY_HEAPS];
static _Atomic uint32_t heap_count[VK_MAX_MEMORY_HEAPS];
static _Atomic uint32_t live_count;  // all live vkAllocateMemory allocations
uint32_t vma_shim_live(void) { return live_count; }
static _Atomic uint32_t sub_count;    // live resources carved from blocks (in heap_count, not in live_count)
uint32_t vma_shim_sub(void) { return sub_count; }
static uint64_t spare_made, spare_used, spare_dropped, spare_kept;   // (blocks the helper made, taken by an allocation, given
                                                                     // back short, emptied blocks kept as spares)
void vma_shim_spares(uint64_t *made, uint64_t *used) { *made = spare_made; *used = spare_used; }
uint64_t vma_shim_spares_kept(void) { return spare_kept; }   // (device.c [vram] line)
uint32_t vma_shim_heaps(VmaAllocator a, uint64_t *bytes, uint32_t *count, uint64_t *size, uint32_t *flags) {
    for (uint32_t h = 0; h < a->mp.memoryHeapCount; h++) {
        bytes[h] = heap_bytes[h]; count[h] = heap_count[h];
        size[h] = a->mp.memoryHeaps[h].size; flags[h] = a->mp.memoryHeaps[h].flags;
    }
    return a->mp.memoryHeapCount;
}

VkResult vmaCreateAllocator(const VmaAllocatorCreateInfo *ci, VmaAllocator *out) {
    VmaAllocator a = calloc(1, sizeof *a);
    a->dev = ci->device;
    vkGetPhysicalDeviceMemoryProperties(ci->physicalDevice, &a->mp);
    // VRAM cap on an integrated GPU (Steam Deck: a 1 GB carve-out of the shared RAM). amdgpu does not fail a VRAM allocation when
    // the carve-out is full: it evicts other buffers (ours and the compositor's) to GTT and moves them back at later submits. Deck
    // v2 log: VRAM 1016/1024 MB, then a 6 s stall inside vkQueueSubmit2 with the whole Deck lagging. There VRAM is the same memory
    // as GTT, so past the cap allocations simply take the other heap's types. LARECOMP_VRAM_CAP_MB=<MB> (0 = no cap); default on an
    // integrated GPU: the device-local heap minus 384 MB, at least half of it.
    VkPhysicalDeviceProperties pp;
    vkGetPhysicalDeviceProperties(ci->physicalDevice, &pp);
    for (uint32_t h = 0; h < a->mp.memoryHeapCount; h++)
        if ((a->mp.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) &&
            (!a->vram_cap || a->mp.memoryHeaps[h].size > a->mp.memoryHeaps[a->vram_heap].size))
            a->vram_heap = h, a->vram_cap = 1;
    uint64_t hs = a->vram_cap ? a->mp.memoryHeaps[a->vram_heap].size : 0, cap = 0;
    const char *e = getenv("LARECOMP_VRAM_CAP_MB");
    if (e) cap = strtoull(e, NULL, 0) << 20;
    else if (pp.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU && hs) cap = hs - (hs / 2 < (384ull << 20) ? hs / 2 : 384ull << 20);
    a->vram_cap = cap;
    a->uma = pp.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU;
    a->prio = (ci->flags & VMA_ALLOCATOR_CREATE_EXT_MEMORY_PRIORITY_BIT) != 0;
    if (a->prio && (ci->flags & VMA_ALLOCATOR_CREATE_SHIM_PAGEABLE_BIT))
        a->set_prio = (PFN_vkSetDeviceMemoryPriorityEXT)vkGetDeviceProcAddr(ci->device, "vkSetDeviceMemoryPriorityEXT");
    if (cap) fprintf(stderr, "[vma] VRAM cap %llu MB of the %llu MB device-local heap %u (LARECOMP_VRAM_CAP_MB): allocations past it go to the other heap\n",
                     (unsigned long long)(cap >> 20), (unsigned long long)(hs >> 20), a->vram_heap);
    *out = a;
    return VK_SUCCESS;
}

// Sub-allocation: resources up to SUB_MAX share SUB_BLOCK vkAllocateMemory blocks per memory type, buffers and linear
// images apart from optimal images (bufferImageGranularity: the two never share a block). One allocation per resource
// reached 3,951 live allocations in a 30-min drive, against the ~4096 the Windows driver allows, before heap imports.
// First fit over an offset-sorted free list, neighbours merged on free; an empty block is freed unless it is the last of
// its type and kind. Host-visible blocks stay mapped. On by default since 2026-10-25 (LARECOMP_VMA_SUB=0: one allocation
// per resource), a 12-min Kakariko soak 95-129 live, ~2,100-2,250 resources in blocks, no growth, no errors.
enum { SUB_BLOCK = 32 << 20, SUB_MAX = 4 << 20 };
// Big class (stutter sprint, 2026-11-01): optimal images of 4-64 MB (render targets, large arrays) were each their own
// vkAllocateMemory on the render thread: 8-19 ms a block in the rival's numbers. They are carved from 256 MB blocks now
// (kind 2: their own blocks), and a helper thread keeps one empty block per device-local image class ready
// (spare_main), so the render thread does not allocate device memory for them in play. While residency is short
// (vk_residency_tight) no spare is made, the spares go, an empty big block goes at once, and no new big block is made:
// a 4-64 MB image freed from a big block returns nothing to the driver until the whole block is empty, so evictions
// would stop lowering the usage residency reads and it would evict and re-create more; a big-class image that fits no
// existing block gets its own allocation then. LARECOMP_VMA_BIG=0: dedicated allocations above 4 MB again
// LARECOMP_VMA_SPARE=0: no helper.
enum { BIG_BLOCK = 256 << 20, BIG_MAX = 64 << 20 };
static VkDeviceSize block_size(uint32_t kind) { return kind == 2 ? BIG_BLOCK : SUB_BLOCK; }
// the priority chained into a vkAllocateMemory of type t (device-local types only: host memory has no residency to lose);
// NULL when priorities are off
static const void *prio_info(VmaAllocator a, uint32_t t, float p, VkMemoryPriorityAllocateInfoEXT *pi) {
    if (!a->prio || !(a->mp.memoryTypes[t].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) return NULL;
    *pi = (VkMemoryPriorityAllocateInfoEXT){.sType = VK_STRUCTURE_TYPE_MEMORY_PRIORITY_ALLOCATE_INFO_EXT, .priority = p};
    return pi;
}
bool vk_residency_tight(void);   // (textures.c: short of the VRAM budget / commit floor now or within the last 2 s)
struct SubBlock {
    SubBlock *next;
    VkDeviceMemory mem;
    uint8_t *mapped;
    uint32_t type, kind, nfree, capfree, nlive, nhot;   // (nhot: live hot resources, the block's priority)
    float prio;   // (the priority last set; 0: PRIO_BLOCK, as made)
    uint32_t cand, gid;   // (vma_shim_free_gain's scratch, under sub_mu)
    VkDeviceSize live_bytes;
    struct { VkDeviceSize off, size; } *fr;   // free ranges, by offset
};
static pthread_mutex_t sub_mu = PTHREAD_MUTEX_INITIALIZER;
static bool big_on(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("LARECOMP_VMA_BIG"); on = !(e && *e == '0'); }
    return on;
}
// spare[t][kind]: an empty block of memory type t made off the render thread (kinds 1 and 2, device-local types only), and
// [t][SPARE_OWN] one OWN_SPARE allocation for a buffer's own allocation (below) The VRAM mirrors' 16 MB blocks (exec.c
// mirror_block: a device-local buffer each, made when a draw first binds a range in it) are buffers over SUB_MAX: each was
// its own vkAllocateMemory on the render thread, 10-12 ms ("[vma] slow device allocation: 16 MB of type 1... (own
// allocation)": the world's first frame after a load in integration's gate runs f018 / v018c. The helper keeps one such
// allocation ready per device-local type once one was asked for, so a device-local buffer of OWN_SPARE / 2.. OWN_SPARE takes
// it (still its own allocation: the mirrors' release and residency see the same thing), and the helper makes the next.
enum { SPARE_OWN = 3, OWN_SPARE = 16 << 20 };
static struct { VkDeviceMemory mem; bool want; } spare[VK_MAX_MEMORY_TYPES][4];
static VkDeviceSize spare_size(int kind) { return kind == SPARE_OWN ? OWN_SPARE : block_size((uint32_t)kind); }
static pthread_cond_t spare_cv = PTHREAD_COND_INITIALIZER;
static VmaAllocator spare_a;
static bool spare_on(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("LARECOMP_VMA_SPARE"); on = !(e && *e == '0'); }
    return on;
}
// The first resource bound to a new block paid for the whole block on the render thread (camera-turn P0, no allocation
// in those frames: a 2400x2400 x4 shadow map 126 ms, a 1856x1856 x4 one 52 ms, a 128x128 depth 31 ms; while
// vkAllocateMemory itself was 0.3-0.5 ms where it showed): the driver's own work on new device memory looks deferred to
// its first bind. So the helper binds a buffer over the whole spare once, off the render thread, and frees it; the first
// lines say what that cost (evidence for the gate). LARECOMP_VMA_PREBIND=0: not. Measured (v0.1.9 gate g19_see_kak_169,
// RTX 5080): every such bind 0.0 ms, so the deferral is NOT in the bind; a 3072 x 3072 x4 D16 image took 10-16 ms with
// its 72 MB allocated in 0.3 ms: the cost is the depth image's own creation / bind. Kept (free, harmless); the shadow
// rungs and the pool are what avoid it.
static void spare_prebind(VkDeviceMemory mem, uint32_t type, VkDeviceSize size) {
    static int on = -1;
    static uint32_t nlog;
    if (on < 0) { const char *e = getenv("LARECOMP_VMA_PREBIND"); on = !(e && *e == '0'); }
    if (!on) return;
    const uint64_t t0 = SDL_GetTicksNS();
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                             .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VkBuffer buf = VK_NULL_HANDLE;
    if (vkCreateBuffer(spare_a->dev, &bi, NULL, &buf) != VK_SUCCESS) return;
    VkMemoryRequirements rq;
    vkGetBufferMemoryRequirements(spare_a->dev, buf, &rq);
    const bool fit = (rq.memoryTypeBits & (1u << type)) && rq.size <= size;
    const VkResult r = fit ? vkBindBufferMemory(spare_a->dev, buf, mem, 0) : VK_ERROR_UNKNOWN;
    vkDestroyBuffer(spare_a->dev, buf, NULL);
    if (nlog++ < 8)
        fprintf(stderr, "[vma] spare block %llu MB (type %u) bound once off the render thread: %s, %.1f ms\n",
                (unsigned long long)(size >> 20), type, !fit ? "no buffer fits the type" : r == VK_SUCCESS ? "ok" : "bind failed",
                (SDL_GetTicksNS() - t0) / 1e6);
}
static void *spare_main(void *arg) {
    (void)arg;
    pthread_mutex_lock(&sub_mu);
    for (;;) {
        int wt = -1, wk = 0;
        for (int t = 0; t < VK_MAX_MEMORY_TYPES && wt < 0; t++)
            for (int k = 1; k <= SPARE_OWN; k++)
                if (spare[t][k].want && !spare[t][k].mem) { wt = t; wk = k; break; }
        if (wt < 0) { pthread_cond_wait(&spare_cv, &sub_mu); continue; }
        spare[wt][wk].want = false;
        if (vk_residency_tight()) continue;   // (short of memory: hold nothing spare)
        pthread_mutex_unlock(&sub_mu);
        VkMemoryPriorityAllocateInfoEXT pi;
        VkMemoryAllocateInfo mi = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = spare_size(wk),
                                   .memoryTypeIndex = (uint32_t)wt, .pNext = prio_info(spare_a, (uint32_t)wt, PRIO_BLOCK, &pi)};
        VkDeviceMemory mem = VK_NULL_HANDLE;
        VkResult r = vkAllocateMemory(spare_a->dev, &mi, NULL, &mem);
        if (r == VK_SUCCESS) spare_prebind(mem, (uint32_t)wt, mi.allocationSize);
        pthread_mutex_lock(&sub_mu);
        if (r != VK_SUCCESS) continue;
        // (became short meanwhile; or an emptied block took the slot meanwhile: sub_free)
        if (vk_residency_tight() || spare[wt][wk].mem) { vkFreeMemory(spare_a->dev, mem, NULL); continue; }
        spare[wt][wk].mem = mem;
        live_count++;
        spare_made++;
    }
    return NULL;
}
// textures.c res_real_budget, once room is back after a demotion: every device-local block's priority set again, through a
// changed value so the driver passes it on and the video memory manager weighs placement anew. Its own allocations are not
// listed and keep theirs. Pageable only (vkSetDeviceMemoryPriorityEXT). Returns the blocks set.
uint32_t vma_shim_reassert(VmaAllocator a) {
    if (!a->set_prio) return 0;
    uint32_t n = 0;
    pthread_mutex_lock(&sub_mu);
    for (SubBlock *b = a->blocks; b; b = b->next) {
        if (!(a->mp.memoryTypes[b->type].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) continue;
        const float p = b->nhot ? PRIO_HOT : PRIO_COLD;
        a->set_prio(a->dev, b->mem, p - 0.01f);
        a->set_prio(a->dev, b->mem, p);
        b->prio = p;
        n++;
    }
    pthread_mutex_unlock(&sub_mu);
    return n;
}
// textures.c vk_residency_tick, when short: the idle spares go back to the driver
void vma_shim_drop_spares(void) {
    pthread_mutex_lock(&sub_mu);
    for (int t = 0; t < VK_MAX_MEMORY_TYPES; t++)
        for (int k = 1; k <= SPARE_OWN; k++)
            if (spare[t][k].mem) {
                vkFreeMemory(spare_a->dev, spare[t][k].mem, NULL);
                spare[t][k].mem = VK_NULL_HANDLE;
                live_count--;
                spare_dropped++;
            }
    pthread_mutex_unlock(&sub_mu);
}
// textures.c vk_residency_tick: of n idle allocations, which give device memory back when freed. One of its own: grp 0,
// gain its size. A block's: only when all its live allocations are among the n and the emptied block then goes to the
// driver (sub_free: another block of its class remains, or a big one while short): grp = that block's number from 1,
// gain = the block's size on the first of them, 0 on the others. Any other one (a block that keeps live neighbours:
// freeing it returns nothing): grp 0, gain 0. Returns the groups numbered.
uint32_t vma_shim_free_gain(VmaAllocator a, VmaAllocation const *al, int n, uint32_t *grp, uint64_t *gain) {
    pthread_mutex_lock(&sub_mu);
    for (SubBlock *b = a->blocks; b; b = b->next) b->cand = b->gid = 0;
    for (int i = 0; i < n; i++)
        if (al[i] && al[i]->sub) al[i]->sub->cand++;
    uint32_t ng = 0;
    for (SubBlock *b = a->blocks; b; b = b->next) {
        if (!b->nlive || b->cand < b->nlive) continue;
        SubBlock *o = a->blocks;
        for (; o && (o == b || o->type != b->type || o->kind != b->kind); o = o->next) {}
        if (o || (b->kind == 2 && vk_residency_tight())) b->gid = ++ng;
    }
    for (int i = 0; i < n; i++) {
        SubBlock *b = al[i] ? al[i]->sub : NULL;
        grp[i] = 0, gain[i] = 0;
        if (!al[i]) continue;
        if (!b) { gain[i] = al[i]->size; continue; }
        if (!b->gid) continue;
        grp[i] = b->gid;
        if (b->cand) { gain[i] = block_size(b->kind); b->cand = 0; }   // (the first of the block's: its whole size)
    }
    pthread_mutex_unlock(&sub_mu);
    return ng;
}
// big blocks (kind 2) for the [vram] line: how many, their bytes, the bytes live in them; spares held and dropped
void vma_shim_big(VmaAllocator a, uint32_t *n, uint64_t *bytes, uint64_t *live, uint64_t *spare_bytes, uint64_t *dropped) {
    *n = 0; *bytes = *live = *spare_bytes = 0;
    pthread_mutex_lock(&sub_mu);
    for (SubBlock *b = a->blocks; b; b = b->next)
        if (b->kind == 2) { ++*n; *bytes += BIG_BLOCK; *live += b->live_bytes; }
    for (int t = 0; t < VK_MAX_MEMORY_TYPES; t++)
        for (int k = 1; k <= SPARE_OWN; k++)
            if (spare[t][k].mem) *spare_bytes += spare_size(k);
    *dropped = spare_dropped;
    pthread_mutex_unlock(&sub_mu);
}
// every block of one kind (0 buffers / linear images, 1 optimal images up to SUB_MAX, 2 big; -1 any) on device-local heaps
// (vram) or the others, for the "[vram] slack" line: how many, their bytes, the bytes live in them (the difference is free
// ranges the driver still counts as ours)
void vma_shim_blocks(VmaAllocator a, int kind, bool vram, uint32_t *n, uint64_t *bytes, uint64_t *live) {
    *n = 0; *bytes = *live = 0;
    pthread_mutex_lock(&sub_mu);
    for (SubBlock *b = a->blocks; b; b = b->next)
        if ((kind < 0 || b->kind == (uint32_t)kind) &&
            ((a->mp.memoryHeaps[a->mp.memoryTypes[b->type].heapIndex].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0) == vram)
            ++*n, *bytes += block_size(b->kind), *live += b->live_bytes;
    pthread_mutex_unlock(&sub_mu);
}
// Device memory allocated where a resource is created (not pre-made by spare_main): for the hitch log (exec.c), with a
// line for each slow one and why it was not carved from a block (stream-hitch, 1 image 77.5 ms).
static _Atomic uint64_t sync_n, sync_ns, sync_bytes;
void vma_shim_sync_allocs(uint64_t *n, uint64_t *ns, uint64_t *bytes) { *n = sync_n; *ns = sync_ns; *bytes = sync_bytes; }
static VkResult alloc_timed(VkDevice dev, const VkMemoryAllocateInfo *mi, VkDeviceMemory *mem, const char *why) {
    const uint64_t t0 = SDL_GetTicksNS();
    const VkResult r = vkAllocateMemory(dev, mi, NULL, mem);
    const uint64_t dt = SDL_GetTicksNS() - t0;
    sync_n++; sync_ns += dt; sync_bytes += mi->allocationSize;
    static _Atomic int logged;
    if (dt > 8000000ull && logged < 64) {
        logged++;
        fprintf(stderr, "[vma] slow device allocation: %llu MB of type %u in %.1f ms at creation (%s)\n",
                (unsigned long long)(mi->allocationSize >> 20), mi->memoryTypeIndex, dt / 1e6, why);
    }
    return r;
}
static bool spare_type(VmaAllocator a, uint32_t t, uint32_t kind) {   // (device-local, not host-visible; kinds 1 and 2, SPARE_OWN)
    return kind >= 1 && kind <= SPARE_OWN && spare_on() && !(a->mp.memoryTypes[t].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) &&
           (a->mp.memoryTypes[t].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
}
static void spare_ask(VmaAllocator a, uint32_t t, uint32_t kind) {   // (sub_mu held)
    static bool started;
    if (!spare_type(a, t, kind)) return;
    if (!started) {
        started = true;
        spare_a = a;
        pthread_t th;
        if (pthread_create(&th, NULL, spare_main, NULL)) return;
        pthread_detach(th);
    }
    spare[t][kind].want = true;
    pthread_cond_signal(&spare_cv);
}
static bool sub_on(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("LARECOMP_VMA_SUB"); on = !(e && *e == '0'); }
    return on;
}
static void sub_room(SubBlock *b) {
    if (b->nfree < b->capfree) return;
    b->capfree = b->capfree ? b->capfree * 2 : 16;
    b->fr = realloc(b->fr, b->capfree * sizeof *b->fr);
}
static bool sub_take(SubBlock *b, VkDeviceSize size, VkDeviceSize align, VkDeviceSize *off) {
    for (uint32_t i = 0; i < b->nfree; i++) {
        VkDeviceSize o = b->fr[i].off, e = o + b->fr[i].size, at = (o + align - 1) / align * align;
        if (at + size > e) continue;
        *off = at;
        if (at > o && at + size < e) {   // (pieces left on both sides: one more range)
            sub_room(b);
            memmove(&b->fr[i + 2], &b->fr[i + 1], (b->nfree - i - 1) * sizeof *b->fr);
            b->fr[i].size = at - o;
            b->fr[i + 1].off = at + size; b->fr[i + 1].size = e - at - size;
            b->nfree++;
        } else if (at > o) b->fr[i].size = at - o;
        else if (at + size < e) b->fr[i].off = at + size, b->fr[i].size = e - at - size;
        else { b->nfree--; memmove(&b->fr[i], &b->fr[i + 1], (b->nfree - i) * sizeof *b->fr); }
        return true;
    }
    return false;
}
static void sub_give(SubBlock *b, VkDeviceSize off, VkDeviceSize size) {
    uint32_t i = 0;
    while (i < b->nfree && b->fr[i].off < off) i++;
    bool left = i > 0 && b->fr[i - 1].off + b->fr[i - 1].size == off, right = i < b->nfree && off + size == b->fr[i].off;
    if (left && right) {
        b->fr[i - 1].size += size + b->fr[i].size;
        b->nfree--; memmove(&b->fr[i], &b->fr[i + 1], (b->nfree - i) * sizeof *b->fr);
    } else if (left) b->fr[i - 1].size += size;
    else if (right) b->fr[i].off = off, b->fr[i].size += size;
    else {
        sub_room(b);
        memmove(&b->fr[i + 1], &b->fr[i], (b->nfree - i) * sizeof *b->fr);
        b->fr[i].off = off; b->fr[i].size = size;
        b->nfree++;
    }
}
// a resource carved from a block of memory type t; NULL: no block could take it and none could be made (the caller
// allocates it alone)
// a block's priority from what lives in it (pageable only; sub_mu held)
static void block_prio(VmaAllocator a, SubBlock *b) {
    const float p = b->nhot ? PRIO_HOT : PRIO_COLD;
    if ((b->prio ? b->prio : PRIO_BLOCK) == p) return;   // (unchanged: a cold first resident of a block made at PRIO_BLOCK)
    if (a->set_prio && (a->mp.memoryTypes[b->type].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
        a->set_prio(a->dev, b->mem, p);
        b->prio = p;
    }
}
static VmaAllocation sub_alloc(VmaAllocator a, uint32_t t, uint32_t kind, VkMemoryRequirements req, bool cold) {
    VmaAllocation al = NULL;
    VkDeviceSize off = 0, align = req.alignment ? req.alignment : 1;
    pthread_mutex_lock(&sub_mu);
    SubBlock *b = a->blocks;
    for (; b; b = b->next)
        if (b->type == t && b->kind == kind && sub_take(b, req.size, align, &off)) break;
    if (!b && !(kind == 2 && vk_residency_tight())) {   // (short: no new big block, the caller allocates it alone)
        const VkDeviceSize bs = block_size(kind);
        VkMemoryPriorityAllocateInfoEXT pi;
        VkMemoryAllocateInfo mi = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = bs, .memoryTypeIndex = t,
                                   .pNext = prio_info(a, t, PRIO_BLOCK, &pi)};
        VkDeviceMemory mem = VK_NULL_HANDLE;
        void *mapped = NULL;
        const bool from_spare = kind < 3 && spare[t][kind].mem;   // (made by spare_main: no allocation here)
        if (from_spare) { mem = spare[t][kind].mem; spare[t][kind].mem = VK_NULL_HANDLE; live_count--; spare_used++; }
        if (from_spare || alloc_timed(a->dev, &mi, &mem, kind == 2 ? "new 256 MB block, no spare ready" : "new 32 MB block") == VK_SUCCESS) {
            if ((a->mp.memoryTypes[t].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) &&
                vkMapMemory(a->dev, mem, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS)
                vkFreeMemory(a->dev, mem, NULL);
            else {
                b = calloc(1, sizeof *b);
                b->mem = mem; b->mapped = mapped; b->type = t; b->kind = kind;
                sub_give(b, 0, bs);
                b->next = a->blocks; a->blocks = b;
                live_count++;
                sub_take(b, req.size, align, &off);   // (fits: req.size <= SUB_MAX / BIG_MAX, alignments far below the block)
            }
        }
        spare_ask(a, t, kind);   // (the next block of this class: made off this thread)
    }
    if (b) {
        // (pageable: the first hot resident raises the block; the first resident of an empty block, a spare's PRIO_BLOCK
        // included, sets it to its class)
        const bool first = !b->nlive++;
        if (!cold) b->nhot++;
        if (first || (!cold && b->nhot == 1)) block_prio(a, b);
        b->live_bytes += req.size;
        sub_count++;
        al = calloc(1, sizeof *al);
        al->mem = b->mem; al->offset = off; al->sub = b; al->cold = cold;
        al->mapped = b->mapped ? b->mapped + off : NULL;
    }
    pthread_mutex_unlock(&sub_mu);
    return al;
}
static void sub_free(VmaAllocator a, VmaAllocation al) {
    pthread_mutex_lock(&sub_mu);
    SubBlock *b = al->sub, *o;
    sub_give(b, al->offset, al->size);
    b->live_bytes -= al->size;
    sub_count--;
    if (!al->cold && !--b->nhot && b->nlive > 1) block_prio(a, b);   // (only cold ones left: the block may go first)
    if (!--b->nlive) {   // (empty: freed while another block of its type and kind remains; a big one under pressure too)
        for (o = a->blocks; o && (o == b || o->type != b->type || o->kind != b->kind); o = o->next) {}
        if (o || (b->kind == 2 && vk_residency_tight())) {
            SubBlock **pp = &a->blocks;
            while (*pp != b) pp = &(*pp)->next;
            *pp = b->next;
            // The class's spare when it has none (stream-hitch): the next block of this class then costs no allocation, and
            // this free none either (both on the render thread; streaming churns a class's last-but-one block). Not while
            // short: then it goes back to the driver, as the spares do (vma_shim_drop_spares).
            if (!b->mapped && spare_type(a, b->type, b->kind) && !spare[b->type][b->kind].mem && !vk_residency_tight()) {
                spare[b->type][b->kind].mem = b->mem;   // (live_count: a spare counts as live, as the block did)
                spare_kept++;
            } else {
                if (b->mapped) vkUnmapMemory(a->dev, b->mem);
                vkFreeMemory(a->dev, b->mem, NULL);
                live_count--;
            }
            free(b->fr); free(b);
        }
    }
    pthread_mutex_unlock(&sub_mu);
}

// best memory type: all `need` flags, as many `want` flags as possible, fewest unwanted flags
static int pick_type(VmaAllocator a, uint32_t bits, VkMemoryPropertyFlags need, VkMemoryPropertyFlags want) {
    int best = -1, best_score = -1000;
    for (uint32_t i = 0; i < a->mp.memoryTypeCount; i++) {
        if (!(bits & (1u << i))) continue;
        VkMemoryPropertyFlags f = a->mp.memoryTypes[i].propertyFlags;
        if ((f & need) != need) continue;
        int score = __builtin_popcount(f & want) * 4 - __builtin_popcount(f & ~(need | want));
        if (score > best_score) best_score = score, best = (int)i;
    }
    return best;
}

// kind: 0 buffer or linear image, 1 optimal image (sub-allocated apart)
static VkResult alloc_for(VmaAllocator a, VkMemoryRequirements req, const VmaAllocationCreateInfo *ai, uint32_t kind,
                          VmaAllocation *out, VmaAllocationInfo *info) {
    bool host = ai->flags & (VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT |
                             VMA_ALLOCATION_CREATE_MAPPED_BIT);
    const bool cold = kind == 1 && vma_shim_cold;   // (optimal images only: the caller's word for a CPU-filled sampled texture)
    VkMemoryPropertyFlags need = ai->requiredFlags, want = ai->preferredFlags;
    if (host) need |= VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    if (ai->flags & VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT) want |= VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
    else if (ai->usage != VMA_MEMORY_USAGE_AUTO_PREFER_HOST) want |= VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    // A full memory heap fails the allocation of its best type (a Steam Deck has a 1 GB VRAM carve-out next to ~8 GB of GTT): the
    // next best type of the same request is tried before giving up (`need` flags stay required, so the alias mirror's
    // DEVICE_LOCAL buffer still fails cleanly and exec.c falls back).
    uint32_t bits = req.memoryTypeBits;
    if (ai->memoryTypeBits && !(bits &= ai->memoryTypeBits)) return VK_ERROR_FEATURE_NOT_PRESENT;   // (VMA's rule; was ignored)
    // On an integrated GPU a DEVICE_LOCAL requirement (the device window and its copy-back shadow, the mirrors' blocks, the
    // ASTC scratch) is only where the GPU reads fastest, and every heap there is the same RAM: past the VRAM cap it yields
    // like a preference, so required allocations do not fill a carve-out beyond the cap either (the Deck v2 stall above:
    // amdgpu then evicts the compositor's buffers; under Proton the same kernel driver, a 1 GB carve-out). A discrete GPU
    // keeps the requirement. LARECOMP_UMA_STRICT_LOCAL=1: required on an integrated GPU too.
    static int strict = -1;
    if (strict < 0) { const char *e = getenv("LARECOMP_UMA_STRICT_LOCAL"); strict = e && *e == '1'; }
    if (a->uma && !strict && a->vram_cap && (need & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) && heap_bytes[a->vram_heap] + req.size > a->vram_cap) {
        static int logged;
        if (!logged++) fprintf(stderr, "[vma] VRAM cap reached on an integrated GPU: allocations that ask for device-local memory "
                                       "go to the other heap too (the same RAM; LARECOMP_UMA_STRICT_LOCAL=1: they stay required)\n");
        need &= ~VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    }
    if (a->vram_cap && !(need & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) && heap_bytes[a->vram_heap] + req.size > a->vram_cap) {
        uint32_t other = 0;
        for (uint32_t i = 0; i < a->mp.memoryTypeCount; i++)
            if (a->mp.memoryTypes[i].heapIndex != a->vram_heap) other |= 1u << i;
        if (bits & other) {
            static int logged;
            if (!logged++) fprintf(stderr, "[vma] VRAM cap reached (%llu MB in use): further allocations in the other heap\n",
                                   (unsigned long long)(heap_bytes[a->vram_heap] >> 20));
            bits &= other;
            want &= ~VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        }
    }
    VkResult r;
    VmaAllocation al = NULL;
    VkDeviceSize own_size = 0;   // (the ready OWN_SPARE allocation taken: its real size, for the counts)
    int t;
    if (kind == 1 && req.size > SUB_MAX && req.size <= BIG_MAX && req.alignment <= (1u << 20) && big_on()) kind = 2;
    if (sub_on() && (req.size <= SUB_MAX || kind == 2) && !(ai->flags & VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT)) {
        t = pick_type(a, bits, need, want);   // (the loop's first choice; a full heap goes on to the loop's retries)
        if (t < 0 && host) t = pick_type(a, bits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, want);
        if (t < 0) t = pick_type(a, bits, 0, want);
        if (t >= 0 && (!host || (a->mp.memoryTypes[t].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) &&
            (al = sub_alloc(a, (uint32_t)t, kind, req, cold)))
            goto got;
    }
    for (bool first = true;; first = false) {
        t = pick_type(a, bits, need, want);
        if (first) {   // (first try only: a retry keeps every required flag)
            if (t < 0 && host) t = pick_type(a, bits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, want);
            if (t < 0) t = pick_type(a, bits, 0, want);
        }
        if (t < 0) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        VkMemoryPriorityAllocateInfoEXT pi;
        VkMemoryAllocateInfo mi = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size, .memoryTypeIndex = (uint32_t)t,
                                   .pNext = prio_info(a, (uint32_t)t, cold ? PRIO_COLD : PRIO_HOT, &pi)};
        al = calloc(1, sizeof *al);
        // (a device-local buffer of OWN_SPARE / 2.. OWN_SPARE: the helper's ready allocation, see spare[][SPARE_OWN]; not a
        // smaller one, which would hold up to twice its size: a range's last, shorter mirror block makes its own)
        bool took = false;
        if (kind == 0 && !host && req.size > OWN_SPARE / 2 && req.size <= OWN_SPARE && req.alignment <= OWN_SPARE &&
            spare_type(a, (uint32_t)t, SPARE_OWN)) {
            pthread_mutex_lock(&sub_mu);
            if (spare[t][SPARE_OWN].mem) {
                al->mem = spare[t][SPARE_OWN].mem;
                spare[t][SPARE_OWN].mem = VK_NULL_HANDLE;
                live_count--;   // (counted as live while spare; counted again below)
                spare_used++;
                took = true;
                own_size = OWN_SPARE;
            }
            spare_ask(a, (uint32_t)t, SPARE_OWN);
            pthread_mutex_unlock(&sub_mu);
        }
        if (took && a->set_prio) a->set_prio(a->dev, al->mem, PRIO_HOT);   // (a buffer: the spare's PRIO_BLOCK raised)
        r = took ? VK_SUCCESS : alloc_timed(a->dev, &mi, &al->mem, req.size > BIG_MAX ? "own allocation, over 64 MB"
                                                : kind == 2 && vk_residency_tight() ? "own allocation, VRAM short: no new big block"
                                                : req.alignment > (1u << 20) ? "own allocation, alignment over 1 MB"
                                                : "own allocation");
        if (r == VK_SUCCESS) break;
        free(al);
        if (r != VK_ERROR_OUT_OF_DEVICE_MEMORY && r != VK_ERROR_OUT_OF_HOST_MEMORY) return r;
        {   // (the Deck's VRAM carve-out is 1-4 GB: the log shows when a heap filled and allocations moved to the next type)
            static int logged;
            if (logged++ < 8)
                fprintf(stderr, "[vma] memory type %d (heap %u, %llu MB in use) full for %llu KB: trying the next type\n", t,
                        a->mp.memoryTypes[t].heapIndex, (unsigned long long)(heap_bytes[a->mp.memoryTypes[t].heapIndex] >> 20),
                        (unsigned long long)(req.size >> 10));
        }
        bits &= ~(1u << t);
        if (!bits) return r;
    }
    if (host) {
        r = vkMapMemory(a->dev, al->mem, 0, VK_WHOLE_SIZE, 0, &al->mapped);
        if (r != VK_SUCCESS) { vkFreeMemory(a->dev, al->mem, NULL); free(al); return r; }
    }
    live_count++;
got:
    // (al->size: the memory it holds, a taken OWN_SPARE allocation's whole size, which the counts and its free use; the
    // allocation info keeps the resource's size)
    al->type = (uint32_t)t; al->size = own_size ? own_size : req.size; al->tag = (uint32_t)vma_shim_tag & 7; al->cold = cold;
    tag_bytes[al->tag] += al->size; tag_count[al->tag]++;
    heap_bytes[a->mp.memoryTypes[t].heapIndex] += al->size;
    heap_count[a->mp.memoryTypes[t].heapIndex]++;
    if (info) *info = (VmaAllocationInfo){.memoryType = al->type, .deviceMemory = al->mem, .offset = al->offset, .size = req.size,
                                          .pMappedData = al->mapped};
    *out = al;
    return VK_SUCCESS;
}

VkResult vmaCreateBuffer(VmaAllocator a, const VkBufferCreateInfo *bi, const VmaAllocationCreateInfo *ai, VkBuffer *buf,
                         VmaAllocation *alloc, VmaAllocationInfo *info) {
    VkResult r = vkCreateBuffer(a->dev, bi, NULL, buf);
    if (r != VK_SUCCESS) return r;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(a->dev, *buf, &req);
    if ((r = alloc_for(a, req, ai, 0, alloc, info)) != VK_SUCCESS) { vkDestroyBuffer(a->dev, *buf, NULL); return r; }
    return vkBindBufferMemory(a->dev, *buf, (*alloc)->mem, (*alloc)->offset);
}
VkResult vmaCreateImage(VmaAllocator a, const VkImageCreateInfo *ii, const VmaAllocationCreateInfo *ai, VkImage *img,
                        VmaAllocation *alloc, VmaAllocationInfo *info) {
    VkResult r = vkCreateImage(a->dev, ii, NULL, img);
    if (r != VK_SUCCESS) return r;
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(a->dev, *img, &req);
    if ((r = alloc_for(a, req, ai, ii->tiling == VK_IMAGE_TILING_OPTIMAL, alloc, info)) != VK_SUCCESS) {
        vkDestroyImage(a->dev, *img, NULL);
        return r;
    }
    return vkBindImageMemory(a->dev, *img, (*alloc)->mem, (*alloc)->offset);
}
static void free_alloc(VmaAllocator a, VmaAllocation al) {
    if (!al) return;
    heap_bytes[a->mp.memoryTypes[al->type].heapIndex] -= al->size;
    heap_count[a->mp.memoryTypes[al->type].heapIndex]--;
    tag_bytes[al->tag] -= al->size; tag_count[al->tag]--;
    if (al->sub) sub_free(a, al);
    else {
        live_count--;
        if (al->mapped) vkUnmapMemory(a->dev, al->mem);
        vkFreeMemory(a->dev, al->mem, NULL);
    }
    free(al);
}
void vmaDestroyBuffer(VmaAllocator a, VkBuffer buf, VmaAllocation al) { if (buf) vkDestroyBuffer(a->dev, buf, NULL); free_alloc(a, al); }
void vmaDestroyImage(VmaAllocator a, VkImage img, VmaAllocation al) { if (img) vkDestroyImage(a->dev, img, NULL); free_alloc(a, al); }
