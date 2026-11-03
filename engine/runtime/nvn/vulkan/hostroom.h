// hostroom.h: room on a host-visible heap for our host-memory imports and the transient ring (exec.c host_import,
// vk_transient). Header-only so tools/engine/hostroom_test.c checks it without a GPU (tools/engine/test_hostroom.py).
// sadyungboi 2026-11-02 (GTX 1660 6 GB, 8 GB RAM, driver 561.09): heap 1 (host memory the GPU reads) is 4053 MB; the guest
// heap's import (2168 MB) and the alias window's (1024 -> 1536 MB in 128 MB steps) are counted against it by the driver,
// outside the allocator's books ("16 MB in use"), so at a fast travel's load a 64 MB ring chunk found no host-visible
// type with room (the 256 MB BAR heap was full too) and vmaCreateBuffer's failure was FATAL. The driver refused it with
// 3720 MB of ours on the heap: its real limit sits some 300 MB below the heap's size.
#pragma once
#include <stdint.h>

// Bytes one more optional import may take on a heap, keeping `keep` bytes free: the heap's size minus everything of ours
// on it (imports + allocations), and with VK_EXT_memory_budget (budget != 0) also the OS budget minus its usage, the
// smaller of the two (the budget's usage may or may not count imported host memory: either way this stays the lower).
// exec.c also takes off what a not-optional import still needs (the guest heap's part not imported yet): the same as a
// larger keep.
static inline uint64_t hostroom_left(uint64_t heap, uint64_t ours, uint64_t budget, uint64_t usage, uint64_t keep) {
    uint64_t room = heap > ours ? heap - ours : 0;
    if (budget) {
        const uint64_t b = budget > usage ? budget - usage : 0;
        if (b < room) room = b;
    }
    return room > keep ? room - keep : 0;
}

// An optional import of `want` bytes cut to `room`, aligned down to `align` (a power of two); below `min`: 0, not imported.
static inline uint64_t hostroom_cut(uint64_t want, uint64_t room, uint64_t align, uint64_t min) {
    uint64_t got = want < room ? want : room;
    got &= ~(align - 1);
    return got < min ? 0 : got;
}

// The ring chunk sizes to try for a request of `size` bytes, largest first, into out[] (at most `max`): the usual chunk
// (or the request, when larger), then halves down to the request rounded up to 1 MB, then that rounded request itself.
// Smaller chunks still fit where a full heap refuses the usual one.
static inline int hostroom_chunk_sizes(uint64_t size, uint64_t chunk, uint64_t *out, int max) {
    const uint64_t mb = 1ull << 20, need = size ? (size + mb - 1) & ~(mb - 1) : mb;
    int n = 0;
    if (n < max) out[n++] = size > chunk ? size : chunk;
    if (size > chunk) return n;
    for (uint64_t s = chunk / 2; s >= need && n < max; s /= 2) out[n++] = s;
    if (n < max && out[n - 1] > need) out[n++] = need;
    return n;
}
