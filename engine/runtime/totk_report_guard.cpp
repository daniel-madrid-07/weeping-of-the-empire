// totk_report_guard.cpp: a model scene's per-object coverage read skips a report row whose index is not a
// row.
//
// gsys::ModelScene::calcGPU_ -> main+0x8442d8 (a GPU-buffer helper: [this+0x68] objects at [this+0x70]) calls
// main+0x844634(obj, side, w x h, rows) for each object. rows = the frame's report buffer (base [[this+0xa0]]+4 plus
// [this+0xa8]; 16-byte rows). The function takes the object's handle h = int16 [obj+0x90 + 2 * side] (-1: none), reads
// n = u32 [rows + h * 32] and follows it to the reference row ([rows + n * 32], its u32 a total), then turns the two
// counters of row 2h+1 into a coverage into obj+0x94. Nothing bounds n: #52's crash had n = 0x3d4ccccd (the float 0.05)
// in that slot, so the read went 32 GB past the buffer.
//
// The hook checks the handle's rows before the original follows them: row 2h, row 2h+1 and the reference row 2n
// readable, n below 0x8000 (h is an int16, so a row pair index is too); a negative handle other than -1 fails
// (the original exits only on -1 and sign-extends the rest). For a failing handle the hook does the original's own
// 'no handle' path itself (0x84463c..0x844644: obj+0x94 = 0, the coverage of an object without a report) and returns,
// so the handle is never written. A valid handle runs the original untouched. The original re-reads row 2h after the
// check: a row rewritten in between can still hand it a bad n (a read-site guard can't close that; the writer would).
// Each newly skipped (object, handle) is logged once (up to 20 lines). 1.0.0 only (another version: the original).
// gen.py --hook 844634 in GEN_MAIN; on by default, TOTK_REPORT_GUARD=0 turns it off.
// tools/oracle/test_report_guard.py.
#include "rt.h"
#include "totk_globals.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <windows.h>   // (Linux: linux/windows.h, rt_posix_readable)

extern "C" u64 g_rt_heap_base;           // hle_os_mem.cpp
extern "C" u64 kernel_heap_size(void);   // hle_os_mem.cpp: committed bytes from g_rt_heap_base

namespace {
#if defined(TOTK_GAME_VERSION) && TOTK_GAME_VERSION != 100
constexpr bool kHere = false;
#else
constexpr bool kHere = true;
#endif
constexpr u64 kHandles = 0x90;     // int16 per buffer side
constexpr u64 kCoverage = 0x94;    // f32: the object's coverage (0 = no report)
constexpr u32 kMaxPair = 0x8000;   // (the handle is an int16: a row pair index stays below this)

// [p, p + n) readable: the guest heap at once, else the OS's view
bool readable(u64 p, u64 n) {
    if (!p || (p & 3)) return false;
    const u64 lo = g_rt_heap_base, size = kernel_heap_size();
    if (lo && p >= lo && p - lo <= size && n <= size - (p - lo)) return true;
#ifdef _WIN32
    for (u64 q = p; q < p + n;) {
        MEMORY_BASIC_INFORMATION mi;
        if (!VirtualQuery((const void *)(uintptr_t)q, &mi, sizeof mi) || mi.State != MEM_COMMIT ||
            (mi.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
            return false;
        q = (u64)(uintptr_t)mi.BaseAddress + mi.RegionSize;
    }
    return true;
#else
    return rt_posix_readable(p, n) != 0;
#endif
}

// the rows handle h names in the report buffer, as the original reads them: true when it may follow them
bool rows_ok(u64 rows, s16 h) {
    if (h < 0) return false;   // (the original sign-extends h: -2 .. -32768 would read before the rows)
    const u64 pair = rows + (u64)h * 32;   // row 2h (its first u32: n) and row 2h+1 (the counters)
    if (!readable(pair, 32)) return false;
    const u32 n = ld32(pair);
    return n < kMaxPair && readable(rows + (u64)n * 32, 4);
}

bool enabled() {
    static const bool on = [] {
        const char *e = getenv("TOTK_REPORT_GUARD");
        const bool v = !e || strcmp(e, "0");
        printf("[report-guard] model scene report rows %s\n", !kHere ? "read as the game (1.0.0 only)"
               : v ? "checked before they are followed (the default)" : "read as the game (TOTK_REPORT_GUARD=0)");
        return v && kHere;
    }();
    return on;
}
}  // namespace

#ifdef _WIN32
void f_844634_orig(Ctx *__restrict c);
void report_guard_orig_stub(Ctx *__restrict) {}
#pragma comment(linker, "/alternatename:?f_844634_orig@@YAXPEIAUCtx@@@Z=?report_guard_orig_stub@@YAXPEIAUCtx@@@Z")
#else
__attribute__((weak)) void f_844634_orig(Ctx *__restrict c);
#endif

void hook_f_844634(Ctx *__restrict c) {   // (object, buffer side, w x h, report rows): the object's coverage
#ifndef _WIN32
    if (!f_844634_orig) return;
#endif
    const u64 obj = c->x[0], rows = c->x[3];
    const u64 slot_at = obj + kHandles + 2 * (u64)(s32)(u32)c->x[1];
    // (the slot is 2-byte aligned: side 1's is at obj+0x92, so check the aligned word that holds it)
    if (!enabled() || !readable(slot_at & ~3ull, 8) || !readable(obj + kCoverage, 4)) { f_844634_orig(c); return; }
    const u16 h = __atomic_load_n((const u16 *)(uintptr_t)slot_at, __ATOMIC_ACQUIRE);
    if (h == 0xffff || rows_ok(rows, (s16)h)) { f_844634_orig(c); return; }   // (-1 only: the original's own exit)
    // the original's 'no handle' path, done here (str wzr, [obj, #0x94]; ret): the handle is never touched, so no
    // other thread ever sees it change
    st32(obj + kCoverage, 0);
    static std::atomic<u64> hidden{0}, lines{0}, last_obj{0}, last_h{0};
    const u64 n = hidden.fetch_add(1) + 1;
    const bool fresh = (obj != last_obj.exchange(obj)) | (h != last_h.exchange(h));
    if (fresh && lines.fetch_add(1) < 20)
        printf("[report-guard] skipped a bad report handle (object %llx, handle %d, rows %llx, %llu calls so far); the "
               "game would have followed it\n", (unsigned long long)obj, (int)(s16)h, (unsigned long long)rows,
               (unsigned long long)n);
}
