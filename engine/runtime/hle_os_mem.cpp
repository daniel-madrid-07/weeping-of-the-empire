// HLE: nn::os memory (heap, memory blocks, pages, QueryMemoryInfo) and nn::mem::StandardAllocator.
#include "hle.h"
#include "nxr_game.h"   // (NXR_GAME_TOTK: forms TotK's build leaves out)
#include "host_wine.h"
#include "alias_overlap.h"
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <algorithm>
#include <vector>
#include <map>
#include <atomic>
#include <chrono>
#ifndef _WIN32
#include <sys/mman.h>
// runtime/linux/plat_posix.cpp: the range in a driver-owned GTT buffer (amdgpu) instead of host memory the GPU imports as userptr
extern "C" int rt_gpu_bo_map(void* addr, size_t size, int prot);
extern "C" int rt_gpu_bo_available(void);
#endif

// ---- process memory model (Switch application pool ~3.25 GB)
static const u64 kAvailable = 0xD0000000ull;
static const u64 kProgram   = 0x5000000ull;      // main image + stacks, rough
static u64 g_heap_base, g_heap_size, g_heap_used;
// for the Vulkan backend (runtime/nvn/shim/las.h): the heap is imported as one GPU buffer (VK_EXT_external_memory_host)
extern "C" u64 g_rt_heap_base;
u64 g_rt_heap_base;
extern "C" u64 kernel_heap_size(void) { return __atomic_load_n(&g_heap_used, __ATOMIC_ACQUIRE); }   // committed bytes from g_rt_heap_base
static u64 g_heap_holes;   // (FreeMemoryBlock decommitted a block below the top: [base, base + used) has holes)
extern "C" int heap_committed_whole(void) { return __atomic_load_n(&g_heap_holes, __ATOMIC_RELAXED) == 0; }   // (frame_snap.c)
static std::mutex g_heap_mtx;

extern "C" u64 totk_engine_extra_memory(void);   // nvn/totk_game_patches.c: game patches' grown heaps (0 by default)
// nn::os::MemoryInfo: +0 totalAvailableMemorySize, +8 totalUsedMemorySize (read by nninitStartup:
// heap = ((avail - used) & ~2MB) - 0x900000), further fields zero for now.
HLE_AS(os_QueryMemoryInfo, "_ZN2nn2os15QueryMemoryInfoEPNS0_10MemoryInfoE") {
    u64* mi = (u64*)c->x[0];
    memset(mi, 0, 0x38);
    mi[0] = kAvailable + totk_engine_extra_memory(); mi[1] = kProgram + g_heap_used;
    c->x[0] = 0;
}
// Write watch default. Windows: on. Linux: OFF (TOTK_WW=1 turns it on). There it is mprotect + SIGSEGV (runtime/linux/plat_posix.cpp), and
// protecting pages that the Vulkan backend has imported as GPU buffers (VK_EXT_external_memory_host: the guest heap for shader global
// memory, the alias window for vertex / index data) is hostile to amdgpu: a host-pointer import is a userptr BO that is revalidated with
// write access after every PTE change in its range, which fails (EPERM / EFAULT) on read-only pages and costs a walk of the whole range
// (2-3 GB) at the next submit when it succeeds. Off: heap_ww_changed() answers "unknown" (textures.c falls back to its sampled /
// periodic hashing) and exec.c binds the alias window's host import directly (no VRAM mirror: nothing to gain on the Deck's unified memory).
// Windows under Wine / Proton (ntdll exports wine_get_version) is that same Linux kernel and driver below: Wine's write watch and
// VirtualProtect are mprotect too, so OFF there as well (TOTK_WW=1 turns it on, as on Linux).
static bool ww_default() {
#ifdef _WIN32
    if (!host_is_wine()) return true;
    const char* e = getenv("TOTK_WW");
    return e && *e == '1';
#else
    const char* e = getenv("TOTK_WW");
    return e && *e == '1';
#endif
}
static bool g_heap_ww;
static std::vector<u32> g_page_epoch;   // per 4 KiB heap page: epoch of the last check that found it written
static u32 g_ww_epoch;
static u64 g_ww_steps;   // locked sweep steps so far, by any caller (ww_sweep: did anyone run between two of mine?)
static std::mutex g_ww_mtx;
// One GetWriteWatch over a whole range held g_ww_mtx (and the kernel's lock on our address space) for the whole walk,
// commit 111 of 120 GB) and the vsync thread stopped as long. ww_sweep walks [a, e) in steps of at most
// LARECOMP_WW_CHUNK_MB (default 4; 0 = one step), each its own GetWriteWatch under g_ww_mtx, the lock dropped in
// between, so other threads (and the memory manager) get in between steps. Epochs stay exact across the dropped lock:
// a step stamps the pages it found written with an epoch this sweep made itself (a new one if anyone ran since this
// sweep's previous step, so a stamp is always newer than any epoch another caller saw), and *level (what the caller
// may treat as "seen") stops advancing at the first step someone else ran before: a write another caller took after
// that is stamped newer than *level and shows up at the next check. Cost of an interleave: one extra "changed" at the
// next check, never a missed write. on_step(first page, pages) runs under the lock after a step's stamps. A failed
// GetWriteWatch stamps its whole step (as written: never a missed write; the watch bits stay set, so the next sweep
// sees them again).
static u64 ww_chunk_bytes() {
    static const u64 c = [] { const char* e = getenv("LARECOMP_WW_CHUNK_MB"); u64 mb = e ? strtoull(e, nullptr, 0) : 4; return mb ? mb << 20 : ~0ull; }();
    return c;
}
static struct { u64 calls, steps, fails, ns, max_ns; std::chrono::steady_clock::time_point since; } g_ww_st;
template <class F> static void ww_sweep(u64 a, u64 e, u32* level, F&& on_step) {
    static std::vector<void*> hits;
    u64 mine_step = ~0ull;   // g_ww_steps after this sweep's previous step
    u32 mine = 0;            // the epoch this sweep made (0: none yet)
    bool frozen = false;
    const u64 chunk = ww_chunk_bytes();
    for (u64 c = a; c < e;) {
        u64 ce = e - c > chunk ? c + chunk : e, np = (ce - c) >> 12;
        std::lock_guard<std::mutex> l(g_ww_mtx);
        bool other = mine_step != g_ww_steps;   // (first step: always)
        if (other && mine_step != ~0ull) frozen = true;
        if (g_page_epoch.size() < (g_heap_size >> 12)) g_page_epoch.resize(g_heap_size >> 12, 0);
        if (hits.size() < np) hits.resize(np);
        ULONG_PTR count = np;
        DWORD gran = 0;
        auto t0 = std::chrono::steady_clock::now();
        bool ok = GetWriteWatch(WRITE_WATCH_FLAG_RESET, (void*)c, (SIZE_T)(ce - c), hits.data(), &count, &gran) == 0;
        u64 ns = (u64)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();
        if (!ok) {
            for (u64 i = 0; i < np; i++) hits[i] = (void*)(c + (i << 12));
            count = np;
            g_ww_st.fails++;
        }
        if (count) {
            if (!mine || other || g_ww_epoch != mine) mine = ++g_ww_epoch;
            for (ULONG_PTR i = 0; i < count; i++) g_page_epoch[((u64)hits[i] - g_heap_base) >> 12] = mine;
        }
        on_step((c - g_heap_base) >> 12, np);
        if (!frozen) *level = g_ww_epoch;
        mine_step = ++g_ww_steps;
        g_ww_st.steps++;
        g_ww_st.ns += ns;
        if (ns > g_ww_st.max_ns) g_ww_st.max_ns = ns;
        if (c == a) {
            auto now = std::chrono::steady_clock::now();
            if (!g_ww_st.calls++) g_ww_st.since = now;
            double s = std::chrono::duration<double>(now - g_ww_st.since).count();
            if (s >= 2.0) {
                printf("[os] heap write watch, last %.1f s: %llu sweeps, %llu steps, %.1f ms in GetWriteWatch, longest step %.1f ms, %llu failed\n",
                       s, (unsigned long long)g_ww_st.calls, (unsigned long long)g_ww_st.steps, g_ww_st.ns / 1e6, g_ww_st.max_ns / 1e6,
                       (unsigned long long)g_ww_st.fails);
                g_ww_st = {};
                g_ww_st.since = now;
            }
        }
        c = ce;
    }
}
// Were any pages of [addr, addr + size) in the heap written since *seen (the caller's epoch from its previous call)?
// 1 yes, 0 no, -1 unknown (no write watch or outside the committed heap). The watch bits of the range are fetched and
// reset (ww_sweep), and every written page gets a new epoch, so textures sharing pages all see the write (a plain
// per-range reset would hide it from the others). A write after the fetch shows up next time.
// CPU writes only: GPU / DMA writes into the imported heap are not seen (callers keep a periodic full check).
extern "C" int heap_ww_changed(u64 addr, u64 size, u64* seen) {
    if (!g_heap_ww || !size || addr < g_heap_base || addr + size > g_heap_base + g_heap_used) return -1;
    u64 a = addr & ~0xFFFull, e = (addr + size + 0xFFF) & ~0xFFFull;
    u32 newest = 0, level = 0;
    ww_sweep(a, e, &level, [&](u64 first, u64 np) {
        for (u64 i = 0; i < np; i++) if (g_page_epoch[first + i] > newest) newest = g_page_epoch[first + i];   // (windows.h max macro)
    });
    bool changed = newest > *seen;
    *seen = level;
    return changed;
}
// exec.c's VRAM mirror of the heap (vertex / index data): the pages of [addr, addr + len) written since the mirror last
// copied them, or never copied, as (offset from the heap base, length) runs, and marks them copied. Shares the page
// epochs with heap_ww_changed, so neither caller's GetWriteWatch reset hides a write from the other. -1: no write watch
// or outside the committed heap. len 0: probe. CPU writes only (exec.c tracks GPU writes itself).
static std::vector<u32> g_mirror_epoch;   // per page: 0 never copied, else g_ww_epoch + 1 when copied
extern "C" int heap_mirror_take(u64 addr, u64 len, u64* offs, u64* lens, int max, u64* bytes) {
    *bytes = 0;
    if (!g_heap_ww) return -1;
    if (!len) return 0;
    if (addr < g_heap_base || addr >= g_heap_base + g_heap_used) return -1;
    if (addr + len > g_heap_base + g_heap_used) len = g_heap_base + g_heap_used - addr;
    u64 a = addr & ~0xFFFull, e = (addr + len + 0xFFF) & ~0xFFFull;
    int n = 0;
    u32 level = 0;
    // (a page's copy mark is the epoch at its own step + 1: a later stamp of it is newer than that epoch, see ww_sweep)
    ww_sweep(a, e, &level, [&](u64 first, u64 np) {
        if (g_mirror_epoch.size() < (g_heap_size >> 12)) g_mirror_epoch.resize(g_heap_size >> 12, 0);
        for (u64 p = first; p < first + np; p++) {
            u32 m = g_mirror_epoch[p];
            if (m && g_page_epoch[p] < m) continue;
            g_mirror_epoch[p] = g_ww_epoch + 1;
            u64 o = p << 12;
            if (n && offs[n - 1] + lens[n - 1] == o) lens[n - 1] += 4096;
            else if (n == max) lens[n - 1] = o + 4096 - offs[n - 1];
            else { offs[n] = o; lens[n] = 4096; n++; }
            *bytes += 4096;
        }
    });
    return n;
}
HLE_AS(os_SetMemoryHeapSize, "_ZN2nn2os17SetMemoryHeapSizeEm") {
    std::lock_guard<std::mutex> l(g_heap_mtx);
    u64 size = c->x[0];
    if (!g_heap_base && size) {
        // At a fixed address (TOTK_HEAP_ADDR, default 0x1000000000; 0 = anywhere): translated shaders bake the heap base
        // into every global-memory access, so a heap at a new address each run made those shaders miss the SPIR-V and
        // pipeline caches every run = compile hitches every session.
        const char* e = getenv("TOTK_HEAP_ADDR");
        u64 want = e ? strtoull(e, nullptr, 0) : 0x1000000000ull;
        // MEM_WRITE_WATCH (TOTK_HEAP_WW=0: off): textures.c asks heap_ww_changed() whether a texture's pages were written
        // instead of hashing it
#ifdef _WIN32
        const DWORD ww = (getenv("TOTK_HEAP_WW") && !strcmp(getenv("TOTK_HEAP_WW"), "0")) || !ww_default() ? 0 : MEM_WRITE_WATCH;
#else
        // Linux: on when the heap becomes a driver-owned GPU buffer (rt_gpu_bo_map below; mprotect was only hostile to the old
        // userptr import). Deck v4 log: the same two textures hashed ~every frame, 2 s of render-thread time per 600 frames.
        // TOTK_HEAP_WW=1 forces it on, =0 off.
        const char* hw = getenv("TOTK_HEAP_WW");
        const DWORD ww = (hw ? *hw == '1' : rt_gpu_bo_available() != 0) ? MEM_WRITE_WATCH : 0;
#endif
        if (want) g_heap_base = (u64)VirtualAlloc((void*)want, size, MEM_RESERVE | ww, PAGE_READWRITE);
        if (want && !g_heap_base) printf("[os] heap: %llx is taken (err %lu), placing it anywhere\n", want, GetLastError());
        if (!g_heap_base) g_heap_base = (u64)VirtualAlloc(nullptr, size, MEM_RESERVE | ww, PAGE_READWRITE);
        g_heap_ww = g_heap_base && ww;
        if (!g_heap_base) rt_trap(c, c->x[30], "SetMemoryHeapSize: reserve failed");
        g_heap_size = size;
        g_rt_heap_base = g_heap_base;
#ifndef _WIN32
        // (before anything is committed / written: AllocateMemoryBlock mprotects)
        if (rt_gpu_bo_map((void*)g_heap_base, size, PROT_NONE) < 0 && g_heap_ww && !(hw && *hw == '1')) {
            g_heap_ww = false;   // (pages stay unarmed: heap_ww_changed answers "unknown")
            printf("[os] heap write watch off: the heap is not a driver-owned GPU buffer\n");
        }
        printf("[os] heap write watch %s\n", g_heap_ww ? "on" : "off");
#endif
    }
    printf("[os] SetMemoryHeapSize(%llx) -> heap %llx\n", size, g_heap_base);
    c->x[0] = 0;
}
HLE_AS(os_AllocateMemoryBlock, "_ZN2nn2os19AllocateMemoryBlockEPmm") {
    std::lock_guard<std::mutex> l(g_heap_mtx);
    u64 size = (c->x[1] + 0x1FFFFF) & ~0x1FFFFFull;
    if (g_heap_used + size > g_heap_size) { printf("[os] AllocateMemoryBlock(%llx): out of heap\n", size); c->x[0] = 0x1A01; return; }
    u64 a = g_heap_base + g_heap_used;
    if (!VirtualAlloc((void*)a, size, MEM_COMMIT, PAGE_READWRITE)) rt_trap(c, c->x[30], "AllocateMemoryBlock: commit failed");
    __atomic_store_n(&g_heap_used, g_heap_used + size, __ATOMIC_RELEASE);   // (after the commit: kernel_heap_size readers)
    *(u64*)c->x[0] = a;
    printf("[os] AllocateMemoryBlock(%llx) -> %llx\n", size, a);
    c->x[0] = 0;
#if !NXR_GAME_TOTK
}
HLE_AS(os_FreeMemoryBlock, "_ZN2nn2os15FreeMemoryBlockEmm") {
    std::lock_guard<std::mutex> l(g_heap_mtx);
    const u64 a = c->x[0], size = (c->x[1] + 0x1FFFFF) & ~0x1FFFFFull;
    if (a < g_heap_base || a + size > g_heap_base + g_heap_used) { printf("[os] FreeMemoryBlock(%llx, %llx): not a block\n", a, c->x[1]); return; }
    // (the bookkeeping first: frame_snap.c's capture reads [base, base + used) without VirtualQuery while there is no hole,
    // so it must see the smaller top / the hole before the pages go)
    if (a + size == g_heap_base + g_heap_used) __atomic_store_n(&g_heap_used, g_heap_used - size, __ATOMIC_RELEASE);
    else __atomic_add_fetch(&g_heap_holes, 1, __ATOMIC_SEQ_CST);
    VirtualFree((void*)a, size, MEM_DECOMMIT);
    printf("[os] FreeMemoryBlock(%llx, %llx)\n", a, size);
#endif
}
// ---- address regions: carved from ONE reserved range (the Switch "alias" region), lowest address first, like the
// kernel does. Separate VirtualAlloc reservations scatter regions around the main heap; sead's PageHeap grows by many
// 64 KiB regions and its isInclude (f_29b7ed0) is the range [lowest, highest), so that range then covered main-heap
// blocks: sead free() handed ExpHeap blocks to the PageHeap, which FreeMemoryPages'd a main-heap page.
static const u64 kAliasSize = 0x1000000000ull;    // 64 GB, the alias region size for 39-bit Switch processes
static const u64 kRegionAlign = 0x10000;          // PageHeap masks pointers with ~0xFFFF: keep 64 KiB alignment
static u64 g_alias_base, g_alias_size;
static std::map<u64, u64> g_alias_free, g_alias_used;   // base -> size
static std::mutex g_alias_mtx;
static bool in_alias(u64 a, u64 n) { return g_alias_base && a >= g_alias_base && a + n <= g_alias_base + g_alias_size; }
// Import window (TOTK_ALIAS_IMPORT_MB, default 2048, 0 = off): the first W bytes of the alias region stay committed so the
// Vulkan backend can import them as one GPU buffer (VK_EXT_external_memory_host: ~100k vertex copies (~5 GB) per 2 s came
// from alias regions (high-water 518 MB). An import pins its pages, so FreeMemoryPages must not decommit inside the
// window; instead a per-4 KiB "mapped" bitmap keeps the kernel's MapPhysicalMemory semantics: pages that become mapped
// again read as zero, mapped pages keep data. Committed as carved (ram-diet 2026-11-02 Windows, TOTK_ALIAS_GROW_MB,
// default 128, 0 = the whole window at once as before): the window used to be committed whole at the first region, and its
// import pins every page, so all 2048 MB were resident from the title on while the game's regions reached 1103 MB
// (totk-20261102-112823: ~945 MB of zero pages held per session; the user's save 1377 MB). Now it commits in steps as
// AllocateAddressRegion carves past what is committed (regions are carved lowest address first and the window starts at
// the region's base, so the carved part is a prefix), and exec.c imports each new step on its import thread as a buffer of
// its own (alias_import_extent below). Until a step is imported the draws that bind it copy their data, as for data past
// the window. A page past the committed prefix is in no carved region, so the game never reaches it.
extern "C" u64 g_alias_import_base, g_alias_import_size, g_alias_import_committed;
u64 g_alias_import_base, g_alias_import_size;
u64 g_alias_import_committed;                    // window bytes committed so far (a prefix; grows; exec.c reads it unlocked)
static u64 g_alias_grow;                         // its step (= the window: committed whole at once)
// The first commit is TOTK_ALIAS_GROW_FIRST_MB (default 1024): what nearly every session carves, so it is imported once
// on the thread at the title as before, and a session grows by a step or two after it, not ~9 times: each later import
// pins its step while the game runs (a driver call on the import thread).
static u64 g_alias_first;
static u8* g_alias_mapped;                       // 1 bit per 4 KiB page of the window
static u64 g_alias_hw;                           // high-water offset of carved regions (bounds the write-watch scan)
static bool g_alias_ww;                          // reserved with MEM_WRITE_WATCH (exec.c keeps a VRAM mirror of the window)
// hle_fs.cpp's glyph bank: the part of the alias region carved so far (the game's resource heaps live there)
extern "C" void rt_alias_carved(u64* base, u64* size) { *base = g_alias_base; *size = g_alias_hw; }
static bool g_alias_soft;                        // the window is tracked by the soft write watch below (Windows)
static bool g_sw_check;                          // =2: the kernel's write watch too, compared at every take
#ifdef _WIN32
// Soft write watch for the import window (TOTK_ALIAS_SOFT_WW, default on, =0 off; present, for the render thread's 0.20 ms a
// frame of alias_dirty_take_range in Kakariko). GetWriteWatch costs per byte scanned: 1.3-1.4 us for a clean 1 MB chunk with
// the game's threads busy, and ~90 % of the ~145 chunk syncs a frame find a clean chunk. Here the window is reserved without
// MEM_WRITE_WATCH and tracked as runtime/linux/plat_posix.cpp does, with a vectored exception handler and VirtualProtect: per
// 4 KiB page UNARMED (writable, never queried: reported written) / ARMED (read-only, clean) / PENDING (a fault being handled)
// / DIRTY (written since armed, writable). A query is a scan of the state bytes (~0.04 us a chunk) plus one
// VirtualProtect(PAGE_READONLY) per run it re-arms (~3.5 us); the first write to an armed page faults once (3-7 us, on the
// writing thread). The window is never armed as a whole: only pages a query reports are re-armed, so the game's resource
// loads into unbound chunks never fault. Kernel writes (ReadFile into a guest buffer) would fail on an armed page or land
// unseen: hle_fs.cpp brackets them with alias_soft_io, which makes the pages writable and keeps them out of re-arming until
// the read is done. If VirtualProtect ever fails the tracker turns itself off: the window is made writable and the queries
// return -1 (exec.c then copies what it binds, as with TOTK_ALIAS_NO_WATCH). TOTK_ALIAS_SOFT_WW=2 (check, the gate for a
// default flip): the window keeps MEM_WRITE_WATCH as well, every take also asks GetWriteWatch, and pages it saw written that
// the soft tracking holds as clean are counted and logged as missed.
enum : u8 { SW_UNARMED = 0, SW_ARMED = 1, SW_DIRTY = 2, SW_PENDING = 3 };
static u8* g_sw_st;                     // per page of the import window
static std::mutex g_sw_mu;               // queries and kernel writes (the handler is lock-free)
static std::atomic<bool> g_sw_off{false};
static std::atomic<u64> g_sw_faults{0};
static u64 g_sw_queries, g_sw_runs;
static u64 g_sw_miss, g_sw_checked;      // (=2)
static u64 g_sw_io[64][2];               // file reads in flight: window page ranges [first, end) (g_sw_mu)
static int g_sw_nio;
// Re-arming waits for exec.c's first take (the probe, made once the window's Vulkan import is done: a driver that locks the
// pages for write access must not meet read-only ones), and stops again at alias_soft_pause (vk_exec_reset: a re-import).
static bool g_sw_armable;                // (g_sw_mu)
// ... and only pages exec.c has imported (alias_import_extent): a step committed later is imported on its own, after the
// first probe, so its pages stay unarmed until that import is done
static std::atomic<u64> g_sw_arm_end{0};    // window offset: pages at or past it are never armed
// Hot pages: a page found written at SW_HOT takes in a row is left writable (DIRTY, so reported at every take) for SW_COOL
// takes, then armed again; a page the game rewrites every frame would otherwise fault at every take.
static u8* g_sw_heat;
enum : u8 { SW_HOT = 3, SW_COOL = 32 };
static u64 g_sw_hot_skips;
static void sw_panic() {
    if (g_sw_off.exchange(true)) return;
    DWORD o;
    DWORD err = GetLastError();
    BOOL rw = VirtualProtect((void*)g_alias_import_base, (SIZE_T)__atomic_load_n(&g_alias_import_committed, __ATOMIC_ACQUIRE),
                             PAGE_READWRITE, &o);
    printf("[os] alias soft write watch: VirtualProtect failed (err %lu), off for good: the window is copied when bound%s\n", err,
           rw ? "" : "; making it writable failed too: pages still read-only are made writable one by one as they fault");
}
static bool sg_guards(u64 a);   // (the sync guard below: a page it holds read-only is its fault to handle, not this one's)
static LONG CALLBACK sg_fault(EXCEPTION_POINTERS* e);
static LONG CALLBACK sw_fault(EXCEPTION_POINTERS* e) {
    const EXCEPTION_RECORD* r = e->ExceptionRecord;
    if (r->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || r->NumberParameters < 2 || r->ExceptionInformation[0] != 1) return EXCEPTION_CONTINUE_SEARCH;
    u64 off = (u64)r->ExceptionInformation[1] - g_alias_import_base;
    if (!g_sw_st || off >= g_alias_import_size) return EXCEPTION_CONTINUE_SEARCH;
    // (handled here, not passed on: sg_fault may have seen no guard on this page a moment ago, before an arm set it, and
    // left it to this handler)
    if (sg_guards((u64)r->ExceptionInformation[1])) {
        const LONG v = sg_fault(e);
        if (v != EXCEPTION_CONTINUE_SEARCH) return v;
    }
    u64 i = off >> 12;
    void* page = (void*)(g_alias_import_base + (i << 12));
    DWORD o;
    u8 exp = SW_ARMED;
    if (!g_sw_off.load() && __atomic_compare_exchange_n(&g_sw_st[i], &exp, (u8)SW_PENDING, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        // writable first, then DIRTY: a query that sees PENDING reports the page and leaves it alone
        bool rw = VirtualProtect(page, 0x1000, PAGE_READWRITE, &o) || (sw_panic(), VirtualProtect(page, 0x1000, PAGE_READWRITE, &o));
        __atomic_store_n(&g_sw_st[i], (u8)SW_DIRTY, __ATOMIC_RELEASE);
        g_sw_faults.fetch_add(1, std::memory_order_relaxed);
        return rw ? EXCEPTION_CONTINUE_EXECUTION : EXCEPTION_CONTINUE_SEARCH;   // (still read-only: a crash report, not a hang)
    }
    // off: make the page writable and retry only if that worked
    if (g_sw_off.load()) return VirtualProtect(page, 0x1000, PAGE_READWRITE, &o) ? EXCEPTION_CONTINUE_EXECUTION : EXCEPTION_CONTINUE_SEARCH;
    // another thread's fault on the page is being handled, or it was just made writable: retry the write (no VirtualProtect
    // here: a query may be re-arming the page right now, and making it writable behind its back would hide the write)
    return exp == SW_PENDING || exp == SW_DIRTY ? EXCEPTION_CONTINUE_EXECUTION : EXCEPTION_CONTINUE_SEARCH;
}
static bool sw_in_io(u64 p) {   // g_sw_mu held
    for (int k = 0; k < g_sw_nio; k++) if (p >= g_sw_io[k][0] && p < g_sw_io[k][1]) return true;
    return false;
}
// GetWriteWatch(RESET)'s answer for [off, off + len) of the window, from the page states; same output as the callers below
static int sw_take(u64 off, u64 len, u64* offs, u64* lens, int max, u64* bytes) {
    if (g_sw_off.load()) return -1;
    u64 p = off >> 12, pe = (off + len + 0xFFF) >> 12, run0 = 0, run1 = 0;
    int n = 0;
    std::lock_guard<std::mutex> l(g_sw_mu);
    const u64 arm_end = g_sw_arm_end.load(std::memory_order_acquire) >> 12;
    auto flush = [&] {
        DWORD o;
        if (run1 > run0 && !VirtualProtect((void*)(g_alias_import_base + (run0 << 12)), (SIZE_T)((run1 - run0) << 12), PAGE_READONLY, &o)) sw_panic();
        if (run1 > run0) g_sw_runs++;
        run0 = run1 = 0;
    };
    while (p < pe) {
        if (!(p & 7) && p + 8 <= pe && __atomic_load_n((const u64*)&g_sw_st[p], __ATOMIC_ACQUIRE) == 0x0101010101010101ull) { p += 8; continue; }   // 8 clean pages
        u8 s = __atomic_load_n(&g_sw_st[p], __ATOMIC_ACQUIRE);
        if (s == SW_ARMED) { p++; continue; }
        u64 o = p << 12;
        if (n && offs[n - 1] + lens[n - 1] == o) lens[n - 1] += 0x1000;
        else if (n == max) lens[n - 1] = o + 0x1000 - offs[n - 1];
        else { offs[n] = o; lens[n] = 0x1000; n++; }
        bool arm = g_sw_armable && p < arm_end && s != SW_PENDING && !sw_in_io(p);
        if (arm && s == SW_DIRTY) {   // written since it was armed: count the streak, leave a hot page writable for a while
            u8 h = g_sw_heat[p];
            g_sw_heat[p] = h + 1 >= SW_HOT + SW_COOL ? 0 : h + 1;
            if (h >= SW_HOT && h + 1 < SW_HOT + SW_COOL) { arm = false; g_sw_hot_skips++; }
        }
        if (arm) {   // re-arm: state first, then read-only (a write in between is in the caller's copy)
            __atomic_store_n(&g_sw_st[p], (u8)SW_ARMED, __ATOMIC_RELEASE);
            if (run1 == p && run1 > run0) run1 = p + 1; else { flush(); run0 = p; run1 = p + 1; }
        }
        p++;
    }
    flush();
    if (g_sw_off.load()) return -1;
    if (g_sw_check) {
        // =2 (the gate): the kernel's write watch on the same pages, reset after the soft pass. A page it saw written that
        // the soft pass did not report and that is still ARMED now is a write the soft tracking missed (a write racing this
        // take faults, so its page is DIRTY / PENDING here and is reported at the next take: not a miss)
        static std::vector<void*> hits;
        if (hits.size() < (g_alias_import_size >> 12)) hits.resize(g_alias_import_size >> 12);
        ULONG_PTR cnt = (ULONG_PTR)hits.size();
        ULONG gran = 0;
        u64 a = off & ~0xFFFull;
        if (GetWriteWatch(WRITE_WATCH_FLAG_RESET, (void*)(g_alias_import_base + a), (SIZE_T)((pe << 12) - a), hits.data(), &cnt, &gran) == 0)
            for (ULONG_PTR k = 0; k < cnt; k++) {
                u64 o = (u64)hits[k] - g_alias_import_base, q = o >> 12;
                bool rep = false;
                for (int i = 0; i < n && !rep; i++) rep = o >= offs[i] && o < offs[i] + lens[i];
                if (rep || __atomic_load_n(&g_sw_st[q], __ATOMIC_ACQUIRE) != SW_ARMED) continue;
                if (g_sw_miss++ < 16) printf("[os] alias soft write watch MISSED a write: window offset %llx (GetWriteWatch saw it, the page is armed)\n", o);
            }
        g_sw_checked += n;
    }
    for (int i = 0; i < n; i++) *bytes += lens[i];
    g_sw_queries++;
    static u64 t_last, q0, r0, f0, h0;   // every ~2 s, as the [vk] stats line (faults a frame = this line's faults / its frames)
    u64 t = GetTickCount64();
    if (!t_last) t_last = t;
    if (t - t_last >= 2000) {
        char ck[128] = "";
        if (g_sw_check) snprintf(ck, sizeof ck, "; check: %llu pages GetWriteWatch saw written that soft tracking missed (of %llu reported)", g_sw_miss, g_sw_checked);
        u64 f = g_sw_faults.load();
        printf("[os] alias soft write watch, last %.1f s: %llu queries, %llu re-armed runs, %llu faults, %llu hot-page skips%s\n", (t - t_last) / 1000.0,
               g_sw_queries - q0, g_sw_runs - r0, f - f0, g_sw_hot_skips - h0, ck);
        t_last = t; q0 = g_sw_queries; r0 = g_sw_runs; f0 = f; h0 = g_sw_hot_skips;
    }
    return n;
}
// hle_fs.cpp, around a ReadFile into guest memory: begin makes the window's pages in [p, p + n) writable (dirty) and keeps
// them out of re-arming; end lets queries re-arm them again (they stay dirty, so the next query reports what was read)
extern "C" void alias_soft_io(const void* ptr, size_t len, int begin) {
    if (!g_alias_soft || g_sw_off.load() || !len) return;
    u64 a = (u64)ptr, e = a + len, wb = g_alias_import_base, we = wb + g_alias_import_size;
    if (e <= wb || a >= we) return;
    u64 p0 = ((a < wb ? wb : a) - wb) >> 12, p1 = ((e > we ? we : e) - wb + 0xFFF) >> 12;
    std::lock_guard<std::mutex> l(g_sw_mu);
    if (!begin) {
        for (int k = 0; k < g_sw_nio; k++)
            if (g_sw_io[k][0] == p0 && g_sw_io[k][1] == p1) { g_sw_io[k][0] = g_sw_io[--g_sw_nio][0]; g_sw_io[k][1] = g_sw_io[g_sw_nio][1]; break; }
        return;
    }
    if (g_sw_nio == 64) { sw_panic(); return; }   // (64 reads in flight at once: not seen; give up rather than risk a failed read)
    g_sw_io[g_sw_nio][0] = p0; g_sw_io[g_sw_nio][1] = p1; g_sw_nio++;
    for (u64 i = p0; i < p1; i++) {
        u8 exp = SW_ARMED;
        if (__atomic_compare_exchange_n(&g_sw_st[i], &exp, (u8)SW_PENDING, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            DWORD o;
            if (!VirtualProtect((void*)(wb + (i << 12)), 0x1000, PAGE_READWRITE, &o)) { sw_panic(); return; }
            __atomic_store_n(&g_sw_st[i], (u8)SW_DIRTY, __ATOMIC_RELEASE);
        } else
            while (__atomic_load_n(&g_sw_st[i], __ATOMIC_ACQUIRE) == SW_PENDING) YieldProcessor();   // (a fault on it is being handled)
    }
}
// exec.c vk_exec_reset (the window's Vulkan import is dropped and made again later): every page writable and DIRTY, no
// re-arming until the next probe, so the new import never meets read-only pages
extern "C" void alias_soft_pause(void) {
    if (!g_alias_soft || g_sw_off.load()) return;
    std::lock_guard<std::mutex> l(g_sw_mu);
    g_sw_armable = false;
    g_sw_arm_end.store(0, std::memory_order_release);   // (the re-import says again how far it reached)
    DWORD o;
    if (!VirtualProtect((void*)g_alias_import_base, (SIZE_T)__atomic_load_n(&g_alias_import_committed, __ATOMIC_ACQUIRE), PAGE_READWRITE, &o)) {
        sw_panic();
        return;
    }
    for (u64 i = 0, np = g_alias_import_size >> 12; i < np; i++) {
        u8 exp = SW_ARMED;   // (PENDING: its handler stores DIRTY itself)
        __atomic_compare_exchange_n(&g_sw_st[i], &exp, (u8)SW_DIRTY, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
    }
}
// exec.c: its imports of the window now reach `end` bytes (each committed step is imported on the import thread)
extern "C" void alias_import_extent(u64 end) {
    if (!g_alias_soft) return;
    if (end > g_alias_import_size) end = g_alias_import_size;
    u64 cur = g_sw_arm_end.load(std::memory_order_acquire);
    while (end > cur && !g_sw_arm_end.compare_exchange_weak(cur, end, std::memory_order_acq_rel)) {}
}
#include "sync_guard.inc"   // LARECOMP_SYNC_GUARD (objects.c's SyncWait ticket lag): the pages the pending replay reads
#else
extern "C" void alias_soft_io(const void*, size_t, int) {}
extern "C" void alias_soft_pause(void) {}
extern "C" void alias_import_extent(u64) {}
// (the sync guard is Windows-only: LARECOMP_SYNC_GUARD does nothing here, the ticket lag stays as it is)
extern "C" int sync_guard_on(void) { return 0; }
extern "C" void sync_guard_note(u64, u64) {}
extern "C" void sync_guard_note_write(u64, u64) {}
extern "C" void sync_guard_fence(const void*, u64) {}
extern "C" int sync_guard_arm(const void*, u64) { return 0; }
extern "C" void sync_guard_io(const void*, size_t, int) {}
extern "C" void sync_guard_hold(int) {}
#endif
// Dirty ranges of the window since the previous call (GetWriteWatch with reset), as (offset, length) pairs from the
// window base, coalesced; at most `max` ranges (the last one widens to cover the rest). Returns -1 without write watch.
extern "C" int alias_dirty_take(u64* offs, u64* lens, int max, u64* bytes) {
    if (!g_alias_ww || !g_alias_import_size) return -1;
#ifdef _WIN32
    if (g_alias_soft) {   // (probe: nothing to reset, every page is UNARMED or DIRTY = reported at its next query; re-arming may start)
        *bytes = 0;
        u64 span = (g_alias_hw + 0xFFFF) & ~0xFFFFull;
        if (span > g_alias_import_size) span = g_alias_import_size;
        if (max) return sw_take(0, span, offs, lens, max, bytes);
        std::lock_guard<std::mutex> l(g_sw_mu);
        g_sw_armable = true;
        return g_sw_off.load() ? -1 : 0;
    }
#endif
    u64 span = (g_alias_hw + 0xFFFF) & ~0xFFFFull;
    if (span > g_alias_import_size) span = g_alias_import_size;
    *bytes = 0;
    if (!span) return 0;
    if (!max) { ResetWriteWatch((void*)g_alias_import_base, (SIZE_T)span); return 0; }   // (probe: start tracking from now)
    static void** pages = (void**)malloc((g_alias_import_size >> 12) * sizeof(void*));
    ULONG_PTR count = (ULONG_PTR)(g_alias_import_size >> 12);
    ULONG gran = 0;
    if (GetWriteWatch(WRITE_WATCH_FLAG_RESET, (void*)g_alias_import_base, (SIZE_T)span, pages, &count, &gran) != 0) {
        offs[0] = 0; lens[0] = span; *bytes = span;   // (failed: copy all)
        return 1;
    }
    int n = 0;
    for (ULONG_PTR i = 0; i < count; i++) {
        u64 o = (u64)pages[i] - g_alias_import_base;
        if (n && offs[n - 1] + lens[n - 1] == o) { lens[n - 1] += gran; continue; }
        if (n == max) { lens[n - 1] = o + gran - offs[n - 1]; continue; }
        offs[n] = o; lens[n] = gran; n++;
    }
    for (int i = 0; i < n; i++) *bytes += lens[i];
    return n;
}
// The same for [off, off + len) of the window only (exec.c lazy mirror sync: just the 1 MB chunks draws bind). A query
// costs ~0.74 ms per GB whatever the split (scratch wwbench 2026-10-15: 1 GB whole 0.74 ms, 1024 x 1 MB 0.88 ms), so
// scanning only bound chunks saves in proportion to what a list does not bind. Pages outside the range keep their bits.
extern "C" int alias_dirty_take_range(u64 off, u64 len, u64* offs, u64* lens, int max, u64* bytes) {
    *bytes = 0;
    if (!g_alias_ww || !g_alias_import_size || off >= g_alias_import_size) return -1;
    if (off + len > g_alias_import_size) len = g_alias_import_size - off;
#ifdef _WIN32
    if (g_alias_soft) return sw_take(off, len, offs, lens, max, bytes);
#endif
    static void* pages[(64u << 20) >> 12];   // (callers pass at most 64 MB: exec.c's batched runs, ww_chunk_dirty)
    if (len > sizeof pages / sizeof pages[0] * 4096ull) len = sizeof pages / sizeof pages[0] * 4096ull;
    ULONG_PTR count = (ULONG_PTR)(len >> 12);
    ULONG gran = 0;
    if (GetWriteWatch(WRITE_WATCH_FLAG_RESET, (void*)(g_alias_import_base + off), (SIZE_T)len, pages, &count, &gran) != 0) {
        offs[0] = off; lens[0] = len; *bytes = len;   // (failed: copy all of it)
        return 1;
    }
    int n = 0;
    for (ULONG_PTR i = 0; i < count; i++) {
        u64 o = (u64)pages[i] - g_alias_import_base;
        if (n && offs[n - 1] + lens[n - 1] == o) { lens[n - 1] += gran; continue; }
        if (n == max) { lens[n - 1] = o + gran - offs[n - 1]; continue; }
        offs[n] = o; lens[n] = gran; n++;
    }
    for (int i = 0; i < n; i++) *bytes += lens[i];
    return n;
}
static u64 g_alias_zeroed;                       // bytes zero-filled on (re)map, for the log
static bool in_window(u64 a, u64 n) { return g_alias_import_size && a >= g_alias_import_base && a + n <= g_alias_import_base + g_alias_import_size; }
static void window_map(u64 a, u64 n, bool map) {
    for (u64 p = (a - g_alias_import_base) >> 12, e = (a + n - g_alias_import_base + 0xFFF) >> 12; p < e; p++) {
        bool was = g_alias_mapped[p >> 3] >> (p & 7) & 1;
        if (map && !was) { memset((void*)(g_alias_import_base + (p << 12)), 0, 0x1000); g_alias_zeroed += 0x1000; }
        if (map) g_alias_mapped[p >> 3] |= (u8)(1u << (p & 7)); else g_alias_mapped[p >> 3] &= (u8)~(1u << (p & 7));
    }
}
// The window committed through offset `end` (g_alias_mtx held), rounded up to the step, at most the whole window. false =
// the commit failed (no commit charge left): the region is refused, as a commit of its pages outside the window would fail.
static bool window_grow(u64 end) {
    if (end > g_alias_import_size) end = g_alias_import_size;
    const u64 c = g_alias_import_committed;
    if (end <= c) return true;
    u64 to = (end + g_alias_grow - 1) / g_alias_grow * g_alias_grow;
    if (!c && to < g_alias_first) to = g_alias_first;
    if (to > g_alias_import_size) to = g_alias_import_size;
    if (!VirtualAlloc((void*)(g_alias_import_base + c), to - c, MEM_COMMIT, PAGE_READWRITE)) {
        printf("[os] alias import window: commit %llu -> %llu MB failed (err %lu)\n", (unsigned long long)(c >> 20),
               (unsigned long long)(to >> 20), (unsigned long)GetLastError());
        return false;
    }
    __atomic_store_n(&g_alias_import_committed, to, __ATOMIC_RELEASE);
    printf("[os] alias import window: %llu of %llu MB committed (regions reach %llu MB)\n", (unsigned long long)(to >> 20),
           (unsigned long long)(g_alias_import_size >> 20), (unsigned long long)(end >> 20));
    return true;
}
HLE_AS(os_AllocateAddressRegion, "_ZN2nn2os21AllocateAddressRegionEPmm") {
    std::lock_guard<std::mutex> l(g_alias_mtx);
    if (!g_alias_base) {
        const bool watch = !getenv("TOTK_ALIAS_NO_WATCH") && ww_default();
#ifdef _WIN32
        const char* sw = getenv("TOTK_ALIAS_SOFT_WW");
        g_alias_soft = watch && (!sw || !*sw || *sw == '1' || *sw == '2');   // (default on; =0: the kernel's write watch)
        g_sw_check = g_alias_soft && sw && *sw == '2';                       // (=2: both, compared; the soft answer is the one used)
#endif
        for (u64 sz = kAliasSize; sz >= 0x100000000ull && !g_alias_base; sz >>= 1)
            if (void* p = VirtualAlloc(nullptr, sz, MEM_RESERVE | (watch && (!g_alias_soft || g_sw_check) ? MEM_WRITE_WATCH : 0), PAGE_READWRITE)) {
                g_alias_base = (u64)p; g_alias_size = sz; g_alias_ww = watch;
            }
        if (!g_alias_base) rt_trap(c, c->x[30], "AllocateAddressRegion: cannot reserve the alias region");
        g_alias_free[g_alias_base] = g_alias_size;
        printf("[os] alias region %llx + %llx\n", g_alias_base, g_alias_size);
        const char* e = getenv("TOTK_ALIAS_IMPORT_MB");
        u64 w = (e ? strtoull(e, nullptr, 0) : 2048) << 20;  // (1024 until 10-15 23:10: the user's save reached 1377 MB -> 3.7 GB of vertex copies per 2 s)
        if (w > g_alias_size) w = g_alias_size;
#ifdef _WIN32
        const char* ge = getenv("TOTK_ALIAS_GROW_MB");
        g_alias_grow = (ge && *ge ? strtoull(ge, nullptr, 0) : 128) << 20;
        g_alias_grow = (g_alias_grow + 0xFFFFFF) & ~0xFFFFFFull;   // (whole 16 MB mirror blocks in each import)
        const char* fe = getenv("TOTK_ALIAS_GROW_FIRST_MB");
        g_alias_first = (((fe && *fe ? strtoull(fe, nullptr, 0) : 1024) << 20) + 0xFFFFFF) & ~0xFFFFFFull;
        if (g_alias_first > w) g_alias_first = w;
#endif
        if (!g_alias_grow || g_alias_grow >= w) g_alias_grow = w;   // (Linux: whole, the window is one GPU buffer object)
        if (w && (g_alias_grow < w || VirtualAlloc((void*)g_alias_base, w, MEM_COMMIT, PAGE_READWRITE))) {
            if (g_alias_grow == w) g_alias_import_committed = w;
#ifndef _WIN32
            if (!g_alias_ww) rt_gpu_bo_map((void*)g_alias_base, w, PROT_READ | PROT_WRITE);   // (nothing written there yet)
#endif
            g_alias_mapped = (u8*)calloc(w >> 15, 1);
#ifdef _WIN32
            if (g_alias_soft) {
                g_sw_st = (u8*)calloc(w >> 12, 1);   // (all UNARMED: nothing faults until a query re-arms a page)
                g_sw_heat = (u8*)calloc(w >> 12, 1);
                if (!g_sw_heat) { free(g_sw_st); g_sw_st = nullptr; }
                if (g_sw_st) AddVectoredExceptionHandler(1, sw_fault);
                else g_alias_soft = g_alias_ww = false;   // (no watch at all: exec.c copies what it binds)
                printf("[os] alias import window: soft write watch %s (TOTK_ALIAS_SOFT_WW)\n",
                       !g_alias_soft ? "off, no memory for its states" : g_sw_check ? "on, checked against GetWriteWatch" : "on");
            }
#endif
            g_alias_import_base = g_alias_base; g_alias_import_size = w;   // (published last: exec.c reads it unlocked)
            if (g_alias_grow < w)
                printf("[os] alias import window %llx + %llu MB, committed as regions are carved: %llu MB first, then %llu MB steps "
                       "(TOTK_ALIAS_GROW_FIRST_MB, TOTK_ALIAS_GROW_MB)\n", g_alias_base, (unsigned long long)(w >> 20),
                       (unsigned long long)(g_alias_first >> 20), (unsigned long long)(g_alias_grow >> 20));
            else printf("[os] alias import window %llx + %llu MB committed\n", g_alias_base, (unsigned long long)(w >> 20));
        } else if (w) printf("[os] alias import window: commit of %llu MB failed, vertex data there is copied\n", (unsigned long long)(w >> 20));
    }
    u64 size = (c->x[1] + kRegionAlign - 1) & ~(kRegionAlign - 1), got = 0;
    for (auto it = g_alias_free.begin(); it != g_alias_free.end(); ++it) {
        if (it->second < size) continue;
        if (g_alias_import_size && it->first < g_alias_import_base + g_alias_import_size && !window_grow(it->first + size - g_alias_base)) {
            c->x[0] = 0x1A01;   // (window_grow said why)
            return;
        }
        got = it->first; u64 rest = it->second - size;
        g_alias_free.erase(it);
        if (rest) g_alias_free[got + size] = rest;
        break;
    }
    if (!got) { printf("[os] AllocateAddressRegion(%llx): alias region full\n", c->x[1]); c->x[0] = 0x1A01; return; }
    // (a live region overlapping the new one = a double hand-out: never expected, said loudly; public #52)
    if (u64 ob, on; alias_overlap(g_alias_used, got, size, &ob, &on))
        printf("[os] AllocateAddressRegion(%llx) -> %llx OVERLAPS the live region %llx + %llx (lr=%llx)\n", c->x[1], got, ob, on,
               c->x[30]);
    g_alias_used[got] = size;
    if (got + size - g_alias_base > g_alias_hw) g_alias_hw = got + size - g_alias_base;
    static int n; if (n++ < 16 || size >= (1ull << 20)) printf("[os] AllocateAddressRegion(%llx) -> %llx lr=%llx\n", c->x[1], got, c->x[30]);
    static u64 high;  // high-water mark of the alias region (sizing a Vulkan import of it)
    if (got + size - g_alias_base > high + (64ull << 20)) {
        high = got + size - g_alias_base;
        printf("[os] alias region high-water %llu MB (%zu regions, %llu MB zero-filled in the import window)%s\n", (unsigned long long)(high >> 20),
               g_alias_used.size(), (unsigned long long)(g_alias_zeroed >> 20),
               high > g_alias_import_size ? " PAST THE IMPORT WINDOW: vertex data there is copied (raise TOTK_ALIAS_IMPORT_MB)" : "");
    }
    *(u64*)c->x[0] = got; c->x[0] = 0;
}
HLE_AS(os_FreeAddressRegion, "_ZN2nn2os17FreeAddressRegionEm") {
    std::lock_guard<std::mutex> l(g_alias_mtx);
    auto it = g_alias_used.find(c->x[0]);
    if (it == g_alias_used.end()) { printf("[os] FreeAddressRegion(%llx): not a region\n", c->x[0]); c->x[0] = 0; return; }
    u64 a = it->first, n = it->second;
    g_alias_used.erase(it);
    // (logged as the allocations are, the big ones always, so a reused address in the log is told from a double hand-out;
    // public #52)
    static int nfree; if (nfree++ < 16 || n >= (1ull << 20)) printf("[os] FreeAddressRegion(%llx) size %llx lr=%llx\n", a, n, c->x[30]);
    if (in_window(a, n)) window_map(a, n, false);   // the import pins these pages: keep them committed
    else if (g_alias_import_size && a < g_alias_import_base + g_alias_import_size) {   // straddles the window end
        u64 we = g_alias_import_base + g_alias_import_size;
        window_map(a, we - a, false);
        VirtualFree((void*)we, a + n - we, MEM_DECOMMIT);
    } else VirtualFree((void*)a, n, MEM_DECOMMIT);
    auto nx = g_alias_free.lower_bound(a);                  // coalesce with the free neighbours
    if (nx != g_alias_free.end() && nx->first == a + n) { n += nx->second; g_alias_free.erase(nx); }
    auto pv = g_alias_free.lower_bound(a);
    if (pv != g_alias_free.begin() && (--pv, pv->first + pv->second == a)) { pv->second += n; }
    else g_alias_free[a] = n;
    c->x[0] = 0;
}
HLE_AS(os_AllocateMemoryPages, "_ZN2nn2os19AllocateMemoryPagesEmm") {
    static int nlog; if (nlog++ < 64) printf("[os] AllocateMemoryPages(%llx, %llx)\n", c->x[0], c->x[1]);
    u64 a = c->x[0], n = c->x[1];
    if (g_alias_import_size && a >= g_alias_import_base && a < g_alias_import_base + g_alias_import_size) {
        std::lock_guard<std::mutex> l(g_alias_mtx);
        u64 we = g_alias_import_base + g_alias_import_size, wn = a + n <= we ? n : we - a;
        window_map(a, wn, true);                     // (committed already: zero the pages that were unmapped)
        if (wn < n && !VirtualAlloc((void*)we, n - wn, MEM_COMMIT, PAGE_READWRITE)) { c->x[0] = 0x1A01; return; }
        c->x[0] = 0;
        return;
    }
    c->x[0] = VirtualAlloc((void*)a, n, MEM_COMMIT, PAGE_READWRITE) ? 0 : 0x1A01;
}
HLE_AS(os_FreeMemoryPages, "_ZN2nn2os15FreeMemoryPagesEmm") {
    static int nlog; if (nlog++ < 64) printf("[os] FreeMemoryPages(%llx, %llx) lr=%llx\n", c->x[0], c->x[1], c->x[30]);
    if (c->x[0] < g_heap_base + g_heap_size && c->x[0] + c->x[1] > g_heap_base) {   // pages API is for address regions only
        printf("[os] FreeMemoryPages(%llx, %llx) INSIDE the main heap, lr=%llx x19-x23 %llx %llx %llx %llx %llx bt:", c->x[0], c->x[1],
               c->x[30], c->x[19], c->x[20], c->x[21], c->x[22], c->x[23]);
        u64 fp = c->x[29];
        for (int i = 0; i < 12 && fp >= c->sp && fp < c->sp + (1ull << 24) && !(fp & 7); i++) {
            printf(" %llx", ((u64*)fp)[1]);
            if (((u64*)fp)[0] <= fp) break;
            fp = ((u64*)fp)[0];
        }
        printf("\n[os]   page heap x20 range [+e0 %llx, +e8 %llx)  main heap [%llx, %llx)\n", ((u64*)c->x[20])[0xe0 / 8],
               ((u64*)c->x[20])[0xe8 / 8], g_heap_base, g_heap_base + g_heap_size);
        fflush(stdout);
    }
    // The kernel only unmaps pages in the alias region (svcUnmapPhysicalMemory); never decommit anything else.
    if (!in_alias(c->x[0], c->x[1])) { printf("[os] FreeMemoryPages(%llx, %llx) outside the alias region: ignored\n", c->x[0], c->x[1]); c->x[0] = 0; return; }
    u64 a = c->x[0], n = c->x[1];
    if (g_alias_import_size && a >= g_alias_import_base && a < g_alias_import_base + g_alias_import_size) {
        std::lock_guard<std::mutex> l(g_alias_mtx);
        u64 we = g_alias_import_base + g_alias_import_size, wn = a + n <= we ? n : we - a;
        window_map(a, wn, false);                    // the import pins these pages: keep them committed
        if (wn < n) VirtualFree((void*)we, n - wn, MEM_DECOMMIT);
        c->x[0] = 0;
        return;
    }
    VirtualFree((void*)a, n, MEM_DECOMMIT); c->x[0] = 0;
}
static u64 g_tl_alloc, g_tl_free;
HLE_AS(os_SetMemoryAllocatorForThreadLocal, "_ZN2nn2os32SetMemoryAllocatorForThreadLocalEPFPvmmEPFvS1_mE") {
    g_tl_alloc = c->x[0]; g_tl_free = c->x[1];
}

// ---- nn::mem::StandardAllocator, backed by the host heap.
// The guest object is opaque to the game; we keep our state in a host struct whose pointer sits in the
// object's first 8 bytes (+8 holds a magic). Each block has a 16-byte header {size, offset to raw}.
struct StdAlloc { u64 magic; u64 cap; std::atomic<u64> used; };
static const u64 kMagic = 0x434F4C4C41445453ull;
static StdAlloc* sa(Ctx* c) {
    StdAlloc* s = *(StdAlloc**)c->x[0];
    if (!s || s->magic != kMagic) rt_trap(c, c->x[30], "StandardAllocator used before Initialize");
    return s;
}
static void sa_init(Ctx* c, u64 size) {
    StdAlloc* s = new StdAlloc{ kMagic, size, {0} };
    *(StdAlloc**)c->x[0] = s; ((u64*)c->x[0])[1] = kMagic;
}
static u64 blk_alloc(StdAlloc* s, u64 size, u64 align) {
    if (align < 16) align = 16;
    u8* raw = (u8*)malloc(size + align + 16);
    if (!raw) return 0;
    u64 p = ((u64)raw + 16 + align - 1) & ~(align - 1);
    ((u64*)p)[-2] = size; ((u64*)p)[-1] = p - (u64)raw;
    s->used += size;
    return p;
}
static void blk_free(StdAlloc* s, u64 p) {
    if (!p) return;
    s->used -= ((u64*)p)[-2];
    free((u8*)p - ((u64*)p)[-1]);
}
HLE_AS(sa_ctor, "_ZN2nn3mem17StandardAllocatorC1Ev") { memset((void*)c->x[0], 0, 16); }
HLE_AS(sa_ctor2, "_ZN2nn3mem17StandardAllocatorC1EPvm") { sa_init(c, c->x[2]); }
HLE_AS(sa_init3, "_ZN2nn3mem17StandardAllocator10InitializeEPvmb") { sa_init(c, c->x[2]); }
HLE_AS(sa_init2, "_ZN2nn3mem17StandardAllocator10InitializeEPvm") { sa_init(c, c->x[2]); }
HLE_AS(sa_fini, "_ZN2nn3mem17StandardAllocator8FinalizeEv") { }
HLE_AS(sa_alloc1, "_ZN2nn3mem17StandardAllocator8AllocateEm") { c->x[0] = blk_alloc(sa(c), c->x[1], 16); }
HLE_AS(sa_alloc2, "_ZN2nn3mem17StandardAllocator8AllocateEmm") { c->x[0] = blk_alloc(sa(c), c->x[1], c->x[2]); }
HLE_AS(sa_free, "_ZN2nn3mem17StandardAllocator4FreeEPv") { blk_free(sa(c), c->x[1]); }
HLE_AS(sa_realloc, "_ZN2nn3mem17StandardAllocator10ReallocateEPvm") {
    StdAlloc* s = sa(c); u64 old = c->x[1], size = c->x[2];
    if (!old) { c->x[0] = blk_alloc(s, size, 16); return; }
    if (!size) { blk_free(s, old); c->x[0] = 0; return; }
    u64 n = blk_alloc(s, size, 16);
    if (n) { u64 os = ((u64*)old)[-2]; memcpy((void*)n, (void*)old, os < size ? os : size); blk_free(s, old); }
    c->x[0] = n;
}
HLE_AS(sa_shrink, "_ZN2nn3mem17StandardAllocator6ShrinkEPvm") { c->x[0] = 0; }   // keep the block as is
HLE_AS(sa_free_size, "_ZNK2nn3mem17StandardAllocator16GetTotalFreeSizeEv") { StdAlloc* s = sa(c); c->x[0] = s->cap > s->used ? s->cap - s->used : 0; }
HLE_AS(sa_allocatable, "_ZNK2nn3mem17StandardAllocator18GetAllocatableSizeEv") { StdAlloc* s = sa(c); c->x[0] = s->cap > s->used ? s->cap - s->used : 0; }
HLE_AS(sa_dump, "_ZNK2nn3mem17StandardAllocator4DumpEv") { }
HLE_AS(sa_walk, "_ZNK2nn3mem17StandardAllocator19WalkAllocatedBlocksEPFiPvmS2_ES2_") { }
