// totk_rsdb_memo.cpp: an exact memo of game::PouchActorInfoTable::getRow (main+0xb6afc8, gen.py --hook
// b6afc8 in GEN_MAIN).
//
// What it replaces: getRow(table, &name) builds a pp::rsdb query on its stack (condition type 8, the name as the key,
// no index, no extra conditions) and runs pp::rsdb::TableRange::searchRow (main+0x7a5078), a binary search whose every
// step is a virtual key read plus a byte-wise string compare through the table's vtable. It returns the row pointer, or
// 0. The profile's D1: SensorMgr::updateTargetActor asks it per target actor per frame, the largest single guest item
// in towns (5-7 % of game CPU in Kakariko / Hyrule Castle, cp5 / cx5); 325 call sites in main use it.
//
// Why a memo is exact:
// - the answer is a function of the table's rows and the name's bytes only: searchRow reads the table through its
//   vtable (+0x60 row count, +0x70 index, +0x78 key, +0x80 row) and writes only its own stack frame. The query getRow
//   builds is the same every call;
// - getRow's own cleanup after searchRow (two loops calling vtable +0x18 on the conditions searchRow appended, counts
//   [x29-0xe0] / [sp+0x18]) never runs: both counts are zeroed before the call and searchRow stores only to its own
//   frame (its 7 stores are all sp / x29 relative);
// - the rows live in the table until one of its mutators runs (every vtable slot that writes it, hooked below): each
//   bumps a generation before and after, and every cached answer belongs to one generation and one table pointer;
// - the hook leaves x19-x29 / sp as the original does and returns the row in x0. The original's other effects are its
//   own stack frame below sp and caller-saved registers, which no caller may read after the return.
//
// The hook reads the name's bytes before the original would (only searchRow's compares read them). With a non-empty
// table the original reads the same bytes, so in the game nothing new is touched; on the random states (an unmapped
// name) it faults where the original might not (2026-10-27, harness only).
//
// Each guest thread has its own cache (thread_local: no lock on the hot path). An answer computed while a mutator ran
// on another thread is not kept: the generation is read before and after, and nothing is kept while any mutator is in
// progress (a getRow that ran entirely inside one would have read a half-loaded table).
//
// On by default since the (gate-d1memo2) and the gates passed; it was opt-in until then. TOTK_RSDB_MEMO=0 (or off)
// runs the original only; =verify runs the original on every hit too, compares, and counts mismatches (a "[rsdb-memo]
// MISMATCH" line each, the first 20). TOTK_RSDB_MEMO_STATS=1: a "[rsdb-memo]" summary line every 2^20 getRow calls.
#include "rt.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <unordered_map>

namespace {

constexpr size_t kMaxName = 256;   // longer names (none in the table) go to the original

std::atomic<u64> g_gen{1};          // bumped before and after each mutator
std::atomic<int> g_in_mutator{0};   // mutators running now: no answer is kept meanwhile
std::atomic<long long> g_hits{0}, g_misses{0}, g_bypass{0}, g_mismatch{0}, g_loads{0};

int memo_mode() {
    static const int m = [] {
        const char* e = getenv("TOTK_RSDB_MEMO");
        if (!e || !*e) {
            printf("[rsdb-memo] on (default; TOTK_RSDB_MEMO=0 turns it off)\n");
            return 1;
        }
        const int v = e[0] == '0' || !_stricmp(e, "off") ? 0 : !strcmp(e, "verify") ? 2 : 1;
        printf("[rsdb-memo] TOTK_RSDB_MEMO=%s: %s\n", e, v == 2 ? "verify mode" : v ? "on" : "off");
        return v;
    }();
    return m;
}

struct Hash {
    using is_transparent = void;
    size_t operator()(std::string_view s) const noexcept {   // FNV-1a
        u64 h = 0xcbf29ce484222325ull;
        for (unsigned char ch : s) h = (h ^ ch) * 0x100000001b3ull;
        return (size_t)h;
    }
};
struct Eq {
    using is_transparent = void;
    bool operator()(std::string_view a, std::string_view b) const noexcept { return a == b; }
};

struct Cache {
    u64 gen = 0;
    std::unordered_map<std::string, u64, Hash, Eq> rows;   // key: the table pointer's 8 bytes + the name's bytes
};
thread_local Cache t_cache;

// TOTK_RSDB_MEMO_STATS=1: a summary line every 2^20 getRow calls
void stats_line() {
    static const bool on = [] { const char* e = getenv("TOTK_RSDB_MEMO_STATS"); return e && e[0] == '1'; }();
    if (!on) return;
    static std::atomic<long long> calls{0};
    if ((calls.fetch_add(1, std::memory_order_relaxed) & 0xfffff) != 0xfffff) return;
    printf("[rsdb-memo] getRow: %lld hits, %lld misses, %lld bypassed, %lld mismatches; %lld table loads\n",
           g_hits.load(), g_misses.load(), g_bypass.load(), g_mismatch.load(), g_loads.load());
    fflush(stdout);
}

}  // namespace

// A key without the hooks never calls these and has no f_*_orig: on Windows the linker takes the stub
// (/alternatename), elsewhere the weak references stay null (as totk_pc_vk.cpp).
void rsdb_memo_orig_stub(Ctx* __restrict) {}
#ifdef _WIN32
#define RSDB_ORIG(off)                                                                                                 \
    void f_##off##_orig(Ctx* __restrict c);                                                                            \
    __pragma(comment(linker, "/alternatename:?f_" #off "_orig@@YAXPEIAUCtx@@@Z=?rsdb_memo_orig_stub@@YAXPEIAUCtx@@@Z"))
#define RSDB_HAVE(off) true
#else
#define RSDB_ORIG(off) __attribute__((weak)) void f_##off##_orig(Ctx* __restrict c);
#define RSDB_HAVE(off) (f_##off##_orig != nullptr)
#endif

// The table's mutators, every one a slot of PouchActorInfoTable's vtable (main+0x42c0730; none has a direct caller):
// each drops every cached answer, before and after it runs, and nothing is kept while it runs. The destructors cover
// a table freed and another allocated at the same address. pp::TableBase's destructor is shared by every rsdb table:
// a few more drops at load time, no harm.
#define RSDB_MUTATOR(off)                                                                                              \
    RSDB_ORIG(off)                                                                                                     \
    void hook_f_##off(Ctx* __restrict c) {                                                                             \
        if (!RSDB_HAVE(off)) return;                                                                                   \
        g_in_mutator.fetch_add(1, std::memory_order_acq_rel);                                                          \
        g_gen.fetch_add(1, std::memory_order_acq_rel);                                                                 \
        f_##off##_orig(c);                                                                                             \
        g_gen.fetch_add(1, std::memory_order_acq_rel);                                                                 \
        g_in_mutator.fetch_sub(1, std::memory_order_acq_rel);                                                          \
        g_loads.fetch_add(1, std::memory_order_relaxed);                                                               \
    }
RSDB_MUTATOR(75718c)  // PouchActorInfoTable::loadByml, vtable +0x38
RSDB_MUTATOR(18fb7d0) // PouchActorInfoTable::loadBymlIterArray, +0x40
RSDB_MUTATOR(18fbf18) // the slot between loadBymlIterArray and format, +0x48
RSDB_MUTATOR(18fbf68) // PouchActorInfoTable::format, +0x50
RSDB_MUTATOR(18fc0ac) // PouchActorInfoTable::finalize, +0x58
RSDB_MUTATOR(279a77c) // PouchActorInfoTable::~PouchActorInfoTable, +0x30
RSDB_MUTATOR(2798a1c) // pp::TableBase::~TableBase, +0x28

RSDB_ORIG(b6afc8)

// PouchActorInfoTable::getRow(this, const SafeString& name) -> row or 0
void hook_f_b6afc8(Ctx* __restrict c) {
    if (!RSDB_HAVE(b6afc8)) return;
    const int mode = memo_mode();
    if (!mode) return f_b6afc8_orig(c);
    stats_line();
    const u64 table = c->x[0], name = c->x[1] ? ld64(c->x[1]) : 0;
    char key[8 + kMaxName];
    size_t n = 0;
    if (name) {
        memcpy(key, &table, 8);
        for (; n < kMaxName; n++) {
            const char ch = (char)ld8(name + n);
            if (!ch) break;
            key[8 + n] = ch;
        }
    }
    if (!name || n == kMaxName) {
        g_bypass.fetch_add(1, std::memory_order_relaxed);
        return f_b6afc8_orig(c);
    }
    const std::string_view k(key, 8 + n);
    const u64 gen = g_gen.load(std::memory_order_acquire);
    Cache& cache = t_cache;
    if (cache.gen != gen) {
        cache.rows.clear();
        cache.gen = gen;
    }
    if (const auto it = cache.rows.find(k); it != cache.rows.end()) {
        g_hits.fetch_add(1, std::memory_order_relaxed);
        if (mode == 2) {
            f_b6afc8_orig(c);
            if (c->x[0] != it->second && g_mismatch.fetch_add(1) < 20)
                printf("[rsdb-memo] MISMATCH table %#llx name \"%.*s\": cached %#llx, original %#llx\n",
                       (unsigned long long)table, (int)n, key + 8, (unsigned long long)it->second,
                       (unsigned long long)c->x[0]);
            return;
        }
        c->x[0] = it->second;
        return;
    }
    g_misses.fetch_add(1, std::memory_order_relaxed);
    f_b6afc8_orig(c);
    if (g_gen.load(std::memory_order_acquire) == gen && g_in_mutator.load(std::memory_order_acquire) == 0)
        cache.rows.emplace(std::string(k), c->x[0]);
}
