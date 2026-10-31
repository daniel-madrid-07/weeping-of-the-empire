// TotK: stand-in for the VMA calls the LASRecomp Vulkan backend makes (shim/vma/vk_mem_alloc.h).
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <vma/vk_mem_alloc.h>

typedef struct SubBlock SubBlock;
struct VmaAllocator_T { VkDevice dev; VkPhysicalDeviceMemoryProperties mp; uint64_t vram_cap; uint32_t vram_heap; SubBlock *blocks; };
// sub: the block it was carved from (at offset), NULL for a resource with its own vkAllocateMemory
struct VmaAllocation_T { VkDeviceMemory mem; void *mapped; uint32_t type; VkDeviceSize size, offset; uint32_t tag; SubBlock *sub; };
// Live bytes per caller tag (Deck d16 2026-10-16: 3 GB asked for in a 1 GB VRAM carve-out; which kind of image holds it?).
// The caller sets vma_shim_tag around a create (textures.c: 1 ASTC decoded to RGBA8, 2 other texture images, 3 block shadows).
_Thread_local int vma_shim_tag;
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
struct SubBlock {
    SubBlock *next;
    VkDeviceMemory mem;
    uint8_t *mapped;
    uint32_t type, kind, nfree, capfree, nlive;
    struct { VkDeviceSize off, size; } *fr;   // free ranges, by offset
};
static pthread_mutex_t sub_mu = PTHREAD_MUTEX_INITIALIZER;
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
static VmaAllocation sub_alloc(VmaAllocator a, uint32_t t, uint32_t kind, VkMemoryRequirements req) {
    VmaAllocation al = NULL;
    VkDeviceSize off = 0, align = req.alignment ? req.alignment : 1;
    pthread_mutex_lock(&sub_mu);
    SubBlock *b = a->blocks;
    for (; b; b = b->next)
        if (b->type == t && b->kind == kind && sub_take(b, req.size, align, &off)) break;
    if (!b) {
        VkMemoryAllocateInfo mi = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = SUB_BLOCK, .memoryTypeIndex = t};
        VkDeviceMemory mem;
        void *mapped = NULL;
        if (vkAllocateMemory(a->dev, &mi, NULL, &mem) == VK_SUCCESS) {
            if ((a->mp.memoryTypes[t].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) &&
                vkMapMemory(a->dev, mem, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS)
                vkFreeMemory(a->dev, mem, NULL);
            else {
                b = calloc(1, sizeof *b);
                b->mem = mem; b->mapped = mapped; b->type = t; b->kind = kind;
                sub_give(b, 0, SUB_BLOCK);
                b->next = a->blocks; a->blocks = b;
                live_count++;
                sub_take(b, req.size, align, &off);   // (fits: req.size <= SUB_MAX, alignments are far below the block)
            }
        }
    }
    if (b) {
        b->nlive++;
        sub_count++;
        al = calloc(1, sizeof *al);
        al->mem = b->mem; al->offset = off; al->sub = b;
        al->mapped = b->mapped ? b->mapped + off : NULL;
    }
    pthread_mutex_unlock(&sub_mu);
    return al;
}
static void sub_free(VmaAllocator a, VmaAllocation al) {
    pthread_mutex_lock(&sub_mu);
    SubBlock *b = al->sub, *o;
    sub_give(b, al->offset, al->size);
    sub_count--;
    if (!--b->nlive) {   // (empty: freed while another block of its type and kind remains)
        for (o = a->blocks; o && (o == b || o->type != b->type || o->kind != b->kind); o = o->next) {}
        if (o) {
            SubBlock **pp = &a->blocks;
            while (*pp != b) pp = &(*pp)->next;
            *pp = b->next;
            if (b->mapped) vkUnmapMemory(a->dev, b->mem);
            vkFreeMemory(a->dev, b->mem, NULL);
            live_count--;
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
    VkMemoryPropertyFlags need = ai->requiredFlags, want = ai->preferredFlags;
    if (host) need |= VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    if (ai->flags & VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT) want |= VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
    else if (ai->usage != VMA_MEMORY_USAGE_AUTO_PREFER_HOST) want |= VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    // A full memory heap fails the allocation of its best type (a Steam Deck has a 1 GB VRAM carve-out next to ~8 GB of GTT): the
    // next best type of the same request is tried before giving up (`need` flags stay required, so the alias mirror's
    // DEVICE_LOCAL buffer still fails cleanly and exec.c falls back).
    uint32_t bits = req.memoryTypeBits;
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
    int t;
    if (sub_on() && req.size <= SUB_MAX && !(ai->flags & VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT)) {
        t = pick_type(a, bits, need, want);   // (the loop's first choice; a full heap goes on to the loop's retries)
        if (t < 0 && host) t = pick_type(a, bits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, want);
        if (t < 0) t = pick_type(a, bits, 0, want);
        if (t >= 0 && (!host || (a->mp.memoryTypes[t].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) &&
            (al = sub_alloc(a, (uint32_t)t, kind, req)))
            goto got;
    }
    for (bool first = true;; first = false) {
        t = pick_type(a, bits, need, want);
        if (first) {   // (first try only: a retry keeps every required flag)
            if (t < 0 && host) t = pick_type(a, bits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, want);
            if (t < 0) t = pick_type(a, bits, 0, want);
        }
        if (t < 0) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        VkMemoryAllocateInfo mi = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size, .memoryTypeIndex = (uint32_t)t};
        al = calloc(1, sizeof *al);
        r = vkAllocateMemory(a->dev, &mi, NULL, &al->mem);
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
    al->type = (uint32_t)t; al->size = req.size; al->tag = (uint32_t)vma_shim_tag & 7;
    tag_bytes[al->tag] += req.size; tag_count[al->tag]++;
    heap_bytes[a->mp.memoryTypes[t].heapIndex] += req.size;
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
