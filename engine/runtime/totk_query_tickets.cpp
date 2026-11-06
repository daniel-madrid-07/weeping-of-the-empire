// totk_query_tickets.cpp: a new query request never gets a result entry a live request still uses.
//
// The game's query managers (the system at [[[GOT 0x4557a28]]+0x128], +0xb00: 10 types; Ultrahand's handle at
// game::component::Player +0x678 is type 0) deliver each request's result into an entry of its slot's result buffer.
// The enqueue main+0x7cba8c (x0 = the manager, x1 = the request, w2 = the slot) picks the two entries (one per side of
// the buffer the pass flips) as tickets: an atomic counter (slot+0x140, slot = [mgr+0x10] + i * 0x150) modulo the entry
// count [mgr+0x24], stored as bytes at request +0x35 / +0x36; then +0x37 = 2 (the pass leaves the request alone for a
// while). Nothing checks that the entry is free. A persistent request (+0x2c != 0, Ultrahand's) is re-delivered into
// its two entries every pass for as long as it lives, so once the counter wraps a newer request shares them and the
// two results overwrite each other: Ultrahand's +0x6b0 then holds another query's target (#51: a dead one crashed).
// The wrap comes per REQUEST, requests come per frame and a hold lasts seconds, so it grows with the frame rate.
//
// The hook keeps the entries apart without knowing the capacity: after the original enqueue it checks each of the
// new request's two entries against the request that last took that entry in this slot (a host-side table, checked
// against the guest's own fields: state 1 queued or 2 delivered; same slot; one of its entries). A taken
// entry is swapped for the next ticket from the game's own counter that no live request holds (at most [mgr+0x24]
// tries; the pass has not looked at the request yet: +0x37 = 2). If every entry is taken the game's ticket stays.
// Counters in the log: '[query-tickets] collisions avoided N / unavoidable M'. Another version runs it where its table
// pins 0x7cba8c (1.4.3: main+0x2a0d3c, pins_v143.json) and places every offset above (totk_field, fields_v143.json;
// else the original). gen.py --hook 7cba8c in GEN_MAIN; on by default, TOTK_QUERY_TICKETS=0 turns it off.
// tools/oracle/test_query_tickets.py.
#include "rt.h"
#include "totk_globals.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <windows.h>   // (Linux: linux/windows.h, rt_posix_readable)

extern "C" u64 g_rt_heap_base;           // hle_os_mem.cpp
extern "C" u64 kernel_heap_size(void);   // hle_os_mem.cpp: committed bytes from g_rt_heap_base

namespace {
constexpr u64 kMgrCount = totk_field(QueryMgr, 0x8), kMgrSlots = totk_field(QueryMgr, 0x10),
              kMgrEntries = totk_field(QueryMgr, 0x24);                                  // a query manager
constexpr u64 kSlotTicket = totk_field(QuerySlot, 0x140);                                // a request slot
// the slot's size: a mov immediate in the enqueue, not a field; in both builds the ticket counter sits 0x10 before
// the slot's end (1.0.0: +0x140 of 0x150, 1.4.3: +0x148 of 0x158; the slot grew by 8 from +0xb8 on)
constexpr u64 kSlotSize = kSlotTicket + 0x10;
constexpr u64 kState = totk_field(QueryRequest, 0x28), kSlotIdx = totk_field(QueryRequest, 0x34),
              kEntry0 = totk_field(QueryRequest, 0x35), kWait = totk_field(QueryRequest, 0x37);   // a request
// (another version: the hook runs where its table placed every field it reads, else the game's tickets as before)
constexpr bool kHere = totk_field_known(QueryMgr, 0x8) && totk_field_known(QueryMgr, 0x10) &&
                       totk_field_known(QueryMgr, 0x24) && totk_field_known(QuerySlot, 0x140) &&
                       totk_field_known(QueryRequest, 0x28) && totk_field_known(QueryRequest, 0x34) &&
                       totk_field_known(QueryRequest, 0x35) && totk_field_known(QueryRequest, 0x37);
constexpr u32 kMaxEntries = 256;   // (the entries are bytes in the request)

struct Table { u64 owner[kMaxEntries]; };
std::mutex g_mu;
std::unordered_map<u64, Table> g_tables;   // slot -> the request that last took each entry
std::atomic<u64> g_avoided{0}, g_unavoidable{0};

bool readable(u64 p, u64 n) {   // the guest heap at once, else the OS's view
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

// o (the request that last took entry e of slot index si) still uses it: queued, or delivered
bool holds(u64 o, u64 self, u8 si, u32 e) {
    if (!o || o == self || !readable(o, kWait + 1)) return false;
    const u32 st = ld32(o + kState);
    if (st != 1 && st != 2) return false;
    if (ld8(o + kSlotIdx) != si || (ld8(o + kEntry0) != e && ld8(o + kEntry0 + 1) != e)) return false;
    return true;
}

bool enabled() {
    static const bool on = [] {
        const char *e = getenv("TOTK_QUERY_TICKETS");
        const bool v = !e || strcmp(e, "0");
        printf("[query-tickets] result entries %s\n", !kHere ? "as the game (its fields are not placed in this version)"
               : v ? "kept apart for live requests (the default)" : "as the game (TOTK_QUERY_TICKETS=0)");
        return v && kHere;
    }();
    return on;
}

void say(u64 n, const char *what) {   // at 1, 10, 100, ...
    for (u64 p = 1; p <= n; p *= 10)
        if (p == n) {
            printf("[query-tickets] collisions avoided %llu / unavoidable %llu (%s)\n",
                   (unsigned long long)g_avoided.load(), (unsigned long long)g_unavoidable.load(), what);
            return;
        }
}
}  // namespace

#ifdef _WIN32
void f_7cba8c_orig(Ctx *__restrict c);
void query_tickets_orig_stub(Ctx *__restrict) {}
#pragma comment(linker, "/alternatename:?f_7cba8c_orig@@YAXPEIAUCtx@@@Z=?query_tickets_orig_stub@@YAXPEIAUCtx@@@Z")
#else
__attribute__((weak)) void f_7cba8c_orig(Ctx *__restrict c);
#endif

void hook_f_7cba8c(Ctx *__restrict c) {   // the query manager's enqueue (this, the request, the slot)
#ifndef _WIN32
    if (!f_7cba8c_orig) return;
#endif
    const u64 mgr = c->x[0], h = c->x[1];
    const u8 si = (u8)c->x[2];
    const bool on = enabled() && readable(h, kWait + 1) && readable(mgr, kMgrEntries + 4);
    const u32 before = on ? ld32(h + kState) : ~0u;
    f_7cba8c_orig(c);
    if (before != 0 || ld32(h + kState) == 0 || ld8(h + kWait) != 2) return;   // (not a fresh enqueue)
    const u32 cap = ld32(mgr + kMgrEntries), count = ld32(mgr + kMgrCount);
    if (cap < 2 || cap > kMaxEntries) return;
    const u64 slot = ld64(mgr + kMgrSlots) + (u64)(count > si ? si : 0) * kSlotSize;
    if (!readable(slot, kSlotSize)) return;
    u32 e[2] = {ld8(h + kEntry0), ld8(h + kEntry0 + 1)};
    std::lock_guard<std::mutex> l(g_mu);
    Table &t = g_tables[slot];
    for (int k = 0; k < 2; k++) {
        if (e[k] >= cap || !holds(t.owner[e[k]], h, si, e[k])) continue;
        bool moved = false;
        for (u32 n = 0; n < cap && !moved; n++) {
            const u32 v = __atomic_fetch_add((u32 *)(uintptr_t)(slot + kSlotTicket), 1u, __ATOMIC_ACQ_REL) % cap;
            if (v == e[1 - k] || holds(t.owner[v], h, si, v)) continue;
            e[k] = v;
            st8(h + kEntry0 + k, (u8)v);
            moved = true;
        }
        if (moved) say(++g_avoided, "a live request's entry");
        else say(++g_unavoidable, "every entry in use: the game's ticket kept");
    }
    for (u32 v : e)
        if (v < cap) t.owner[v] = h;
}
