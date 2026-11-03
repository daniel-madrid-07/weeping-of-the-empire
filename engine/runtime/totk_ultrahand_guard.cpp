// totk_ultrahand_guard.cpp: Ultrahand's per-frame read of its bound target skips a target that is no longer a live
// actor.
//
// The player's Ultrahand handle (game::component::Player +0x678) is a query request: +0x6a0 is its state (1 queued,
// 2 result delivered), +0x6b0 points at the result entry, whose first word is the target actor. Player's per-frame
// update main+0x15eca7c (x0 = the Player component; via ExecutePlayerUltraHand's calc) does, when the state is 2:
// a = [[+0x6b0]], then BaseProcLink::set(+0x648, a) (its answer ignored) and a status bit on a's component
// ([[a+0x220]+0xb0] +0x8c). Nothing checks that a is still alive. The result entry lives in the query manager's
// result ring, indexed by a request counter (enqueue main+0x7cba8c, delivery 0x9b2e98), so what a delivered result
// points at can be stale: both #51 reports crash in that read (a host pointer; a freed actor's component).
//
// The hook runs the game's own liveness test before the read: p, a and a's BaseProcLinkData ([a+0x1a8]) readable
// (the guest heap at once, else the OS's view: live memory is never refused for being off the heap), the link data
// pointing back at a (+0x40) with its id (+0x48 == [a+0x10]), a's state ([a+0x24]) below 7 (being deleted). A target
// that fails is hidden for this call: +0x6b0 is swapped (CAS) to a static zero entry and swapped back afterwards
// unless the game bound a new one, so the update takes its own no-target exit (a == 0: 0x15ecb10 -> 0x15ecb38); a live
// one runs the original untouched. Skipping costs one status bit on a dead object. A slot reused by another live actor
// passes (no crash: the bit lands on that actor for a frame). Each newly skipped target is logged once (up to 20
// lines); the first call also logs the Ultrahand query manager's capacities ('[query] type0 slots N results M').
// 1.0.0 only (another version: the original). 1.4.3 needs a pin for 0x15eca7c (pins_v143.json) and the Player /
// BaseProc / BaseProcLinkData offsets above through totk_field (fields_v143.json) before it can run there.
// gen.py --hook 15eca7c in GEN_MAIN; on by default, TOTK_ULTRAHAND_GUARD=0 turns it off.
// tools/oracle/test_ultrahand_guard.py.
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
constexpr u64 kState = 0x6a0, kResult = 0x6b0;                          // the Player component's handle
constexpr u64 kProcId = 0x10, kProcState = 0x24, kLinkData = 0x1a8;      // a BaseProc
constexpr u64 kDataProc = 0x40, kDataId = 0x48;                          // its BaseProcLinkData
alignas(8) const u64 g_zero_entry = 0;   // the result the update is shown for a dead target: its first word, 0, is
                                         // 'no target' to the game (0x15ecb10)

// [p, p + n) readable: the guest heap at once, else the OS's view (never refuse live memory for being off the heap)
bool readable(u64 p, u64 n) {
    if (!p || (p & 7)) return false;
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

// the result entry p as the read would take it: true when the read may go ahead (no target, or a live one)
bool target_ok(u64 p) {
    if (!readable(p, 8)) return false;
    const u64 a = ld64(p);
    if (!a || (a & 1)) return true;   // (the original skips these itself)
    if (!readable(a, kLinkData + 8)) return false;
    const u64 d = ld64(a + kLinkData);
    if (!readable(d, kDataId + 4)) return false;
    return ld64(d + kDataProc) == a && ld32(d + kDataId) == ld32(a + kProcId) && ld32(a + kProcState) < 7;
}

bool enabled() {
    static const bool on = [] {
        const char *e = getenv("TOTK_ULTRAHAND_GUARD");
        const bool v = !e || strcmp(e, "0");
        printf("[ultrahand-guard] Ultrahand's bound target %s\n", !kHere ? "read as the game (1.0.0 only)"
               : v ? "checked alive before its read (the default)" : "read as the game (TOTK_ULTRAHAND_GUARD=0)");
        return v && kHere;
    }();
    return on;
}

// once: the Ultrahand query manager's capacities (#51's result-ring question), read as the game reaches it:
// [[[GOT 0x4557a28]]+0x128] is the query system (0x15ecae0), its +0xb00 the managers, type 0 at +8 (0x13b093c);
// [mgr+0x20] the request slots, [mgr+0x24] the result entries (the enqueue 0x7cba8c takes its tickets modulo them)
void say_query_capacity() {
    static std::atomic_flag said = ATOMIC_FLAG_INIT;
    if (!kHere || said.test_and_set()) return;
    u64 sys = 0, mgrs = 0, mgr = 0;
    const u64 owner = totk_global(0x4557a28);
    if (readable(owner + 0x128, 8)) sys = ld64(owner + 0x128);
    if (readable(sys + 0xb00, 8)) mgrs = ld64(sys + 0xb00);
    if (readable(mgrs + 8, 8)) mgr = ld64(mgrs + 8);
    if (readable(mgr + 0x20, 8))
        printf("[query] type0 slots %u results %u\n", ld32(mgr + 0x20), ld32(mgr + 0x24));
    else
        printf("[query] type0 manager not reachable yet (system %llx)\n", (unsigned long long)sys);
}
}  // namespace

#ifdef _WIN32
void f_15eca7c_orig(Ctx *__restrict c);
void ultrahand_guard_orig_stub(Ctx *__restrict) {}
#pragma comment(linker, "/alternatename:?f_15eca7c_orig@@YAXPEIAUCtx@@@Z=?ultrahand_guard_orig_stub@@YAXPEIAUCtx@@@Z")
#else
__attribute__((weak)) void f_15eca7c_orig(Ctx *__restrict c);
#endif

void hook_f_15eca7c(Ctx *__restrict c) {   // the Player component's Ultrahand update (this)
#ifndef _WIN32
    if (!f_15eca7c_orig) return;
#endif
    const u64 pl = c->x[0];
    say_query_capacity();
    if (!enabled() || !readable(pl, kResult + 8) || ld32(pl + kState) != 2) { f_15eca7c_orig(c); return; }
    u64 *const slot = (u64 *)(uintptr_t)(pl + kResult);
    u64 p = __atomic_load_n(slot, __ATOMIC_ACQUIRE);
    if (!p || target_ok(p)) { f_15eca7c_orig(c); return; }
    const u64 zero = (u64)(uintptr_t)&g_zero_entry;
    if (!__atomic_compare_exchange_n(slot, &p, zero, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        f_15eca7c_orig(c);   // (the game bound a new result just now: its own read)
        return;
    }
    static std::atomic<u64> skipped{0}, lines{0}, last_p{0}, last_a{0};
    const u64 a = readable(p, 8) ? ld64(p) : 0, n = skipped.fetch_add(1) + 1;
    const bool fresh = (p != last_p.exchange(p)) | (a != last_a.exchange(a));   // (both swapped every time)
    if (fresh && lines.fetch_add(1) < 20)
        printf("[ultrahand-guard] skipped a dead Ultrahand target (result %llx -> %llx, %llu frames skipped so far); "
               "the game would have read it\n", (unsigned long long)p, (unsigned long long)a, (unsigned long long)n);
    f_15eca7c_orig(c);
    u64 expect = zero;   // put the result back unless the game bound a new one meanwhile
    __atomic_compare_exchange_n(slot, &expect, p, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}
