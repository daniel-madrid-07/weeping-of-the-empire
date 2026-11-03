// Included by hle_os_mem.cpp (Windows), after the alias window's soft write watch, whose page states it shares; and by tools/engine/sync_guard_test.cpp, which
// defines what it uses from there (the heap and window globals, the soft watch's state bytes and lock, rt_on_guest_thread) and runs it on the CPU, no game
// (tools/engine/test_sync_guard.py). ================================================================== sync guard (LARECOMP_SYNC_GUARD) objects.c's SyncWait
// ticket lag lets the game build frame N+1 while the render thread still replays frame N, and that replay reads guest memory: constant buffer snapshots
// (exec.c guest_copy) and the heap's and the alias window's VRAM mirror snapshots of pages written since their last copy. Data the game refills in place every
// frame, single-buffered against its frame fence (safe on the Switch, where SyncWait returns when the frame is done), can then reach frame N with frame N+1's
// contents, half-written or one frame early. LARECOMP_SYNC_GUARD=1 (default off Windows): at that early release (sync_guard_arm) the pages the replay read
// from guest memory during the previous frame are made read-only, and a game thread's first write to one of them waits until the render thread passes frame
// N's fence (sync_guard_fence lifts the guard there): the exact wait only where and when the game refills data the pending replay still reads, the ticket
// lag's head start everywhere else. Not covered: a page the replay did not read the frame before (data written once, e.g. at a level of detail switch) and
// what the GPU reads live from host memory (the plain SyncWait lag's own assumption). A write from a thread that is not the game's (the render thread, its
// workers, file or audio threads) never waits: its page leaves the guard. A wait ends after LARECOMP_SYNC_GUARD_WAIT_MS (default 200) at most, by lifting the
// whole guard (a hitch, never a hang: a game thread holding a lock the render thread needs); 4 such timeouts within 30 s turn the guard off for good (the
// exact wait from then on; one each after loads, shader builds, is not that). More than LARECOMP_SYNC_GUARD_RUNS page runs (default 1024) or
// LARECOMP_SYNC_GUARD_PAGES pages (default 16384 = 64 MB): not armed, the caller waits for the replay (the exact wait) that frame. Every ~2 s '[sync] guard'
// gives a frame's pages guarded, faults, waits and ms. Window pages keep their soft write watch states: an ARMED page is read-only anyway (the guard only
// takes its fault first), a DIRTY one is made read-only and back; re-armed by a take meanwhile, it stays read-only. A kernel write (ReadFile into guest
// memory) would fail on a read-only page: hle_fs.cpp calls sync_guard_io first, which waits for the lift. The heap is not guarded while exec.c imports it
// (sync_guard_hold: a driver that locks the pages for write access must not meet read-only ones).
extern "C" bool rt_on_guest_thread(void);   // rt_main.cpp: a game thread (it may wait; the render side may not)
extern "C" bool backend_exec_lock_held(void);   // exec.c: this thread holds the exec lock the render thread needs (never waits)
static int g_sg_mode = -1;                // (sync_guard_on)
static bool g_sg_dead;                    // a VirtualProtect failed: off for good (g_sg_lock)
static SRWLOCK g_sg_lock = SRWLOCK_INIT;  // the guard: exclusive to arm / lift / take a page out, shared to look one up
static CONDITION_VARIABLE g_sg_cv = CONDITION_VARIABLE_INIT;   // (a lift: g_sg_gen moved)
static std::atomic<bool> g_sg_active{false};
static u64 g_sg_ticket, g_sg_gen;         // (g_sg_lock) the fence the guard waits for (queue ticket); lifts so far
static std::vector<u64> g_sg_bits[2];     // guarded pages, [0] heap / [1] alias window; sized once (g_sg_lock)
static std::vector<std::pair<u64, u64>> g_sg_runs[2];   // the same as page runs [first, end) (g_sg_lock)
static std::atomic<int> g_sg_heap_hold{0};   // exec.c imports the heap: no heap page is made read-only meanwhile
static std::atomic<const void*> g_sg_key{nullptr};   // the sync whose fences end a frame (the one SyncWait releases early)
static std::atomic<u64> g_sg_passed{0};   // the last of its fences the render thread passed (queue ticket)
static std::atomic<int> g_sg_frames{0};   // its fences passed since it became the frame sync (2: one whole frame's reads)
struct SgSet { std::vector<u64> bits; std::vector<u32> words; };   // pages the replay read: bitmap + its non-zero words
static SgSet g_sg_rec[2][2];              // [set][region]: [g_sg_cur] the frame being replayed, the other the frame before
static SgSet g_sg_wrec[2][2];  // the same for pages the replay itself wrote (left out of the guard
static int g_sg_cur;                      // (g_sg_rec_mu)
static std::mutex g_sg_rec_mu;
static u64 g_sg_io[64][2];                // kernel writes in flight (sync_guard_io): [start, end) addresses, never guarded
static int g_sg_nio;                      // (g_sg_io_mu; 65 = too many to track: no heap page is guarded meanwhile)
static std::mutex g_sg_io_mu;
static struct {
    std::atomic<u64> arms, runs, pages, fresh, over, faults, waits, wait_ns, wait_max, io_waits, others, timeouts, arm_ns, lift_ns, frames;
} g_sg_st;
static u64 g_sg_tmo[4];                   // (g_sg_lock) when the last 4 timeouts were (ns): 4 within 30 s turn the guard off
static u64 sg_now_ns() { return (u64)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
static u64 sg_env(const char* name, u64 dflt) { const char* e = getenv(name); return e && *e ? strtoull(e, nullptr, 0) : dflt; }
static u64 sg_base(int r) { return r ? g_alias_import_base : g_heap_base; }
static u64 sg_size(int r) { return r ? (g_alias_soft ? g_alias_import_size : 0) : __atomic_load_n(&g_heap_used, __ATOMIC_RELAXED); }
static bool sg_page_of(u64 a, int* r, u64* p) {
    for (int k = 0; k < 2; k++) {
        const u64 b = sg_base(k);
        if (b && a >= b && a - b < sg_size(k)) { *r = k; *p = (a - b) >> 12; return true; }
    }
    return false;
}
static bool sg_bit(int r, u64 p) { return (p >> 6) < g_sg_bits[r].size() && (g_sg_bits[r][p >> 6] >> (p & 63) & 1); }
static void sg_bit_set(int r, u64 p, bool on) {
    if ((p >> 6) >= g_sg_bits[r].size()) return;
    if (on) g_sg_bits[r][p >> 6] |= 1ull << (p & 63); else g_sg_bits[r][p >> 6] &= ~(1ull << (p & 63));
}
static bool sg_guards(u64 a) {   // (unlocked peek, sw_fault: the bitmaps are sized before the first arm and never again)
    int r;
    u64 p;
    return g_sg_active.load(std::memory_order_acquire) && sg_page_of(a, &r, &p) && sg_bit(r, p);
}
static void sg_clear(SgSet& s) {
    for (u32 w : s.words) s.bits[w] = 0;
    s.words.clear();
}
static bool sg_protect(int r, u64 p0, u64 p1, DWORD prot) {
    DWORD o;
    return p1 <= p0 || VirtualProtect((void*)(sg_base(r) + (p0 << 12)), (SIZE_T)((p1 - p0) << 12), prot, &o);
}
// [p0, p1) writable again; a run that fails is retried page by page (a page left read-only without its guard bit would
// crash its next write)
static bool sg_unprotect(int r, u64 p0, u64 p1) {
    if (sg_protect(r, p0, p1, PAGE_READWRITE)) return true;
    bool ok = true;
    for (u64 p = p0; p < p1; p++) ok &= sg_protect(r, p, p + 1, PAGE_READWRITE);
    return ok;
}
// Every guarded page writable again (window pages as their soft state wants them), the waiters woken. (g_sg_lock exclusive)
static void sg_lift_locked() {
    const u64 t0 = sg_now_ns();
    bool ok = true;
    for (auto& rn : g_sg_runs[0]) {
        ok &= sg_unprotect(0, rn.first, rn.second);
        for (u64 p = rn.first; p < rn.second; p++) sg_bit_set(0, p, false);
    }
    if (!g_sg_runs[1].empty()) {
        std::lock_guard<std::mutex> l(g_sw_mu);
        u64 r0 = 0, r1 = 0;
        for (auto& rn : g_sg_runs[1])
            for (u64 p = rn.first; p < rn.second; p++) {
                if (!sg_bit(1, p)) continue;
                sg_bit_set(1, p, false);
                const u8 s = __atomic_load_n(&g_sw_st[p], __ATOMIC_ACQUIRE);
                if (!g_sw_off.load() && (s == SW_ARMED || s == SW_PENDING)) continue;   // (read-only as the soft watch wants it)
                if (r1 == p && r1 > r0) { r1++; continue; }
                ok &= sg_unprotect(1, r0, r1);
                r0 = p, r1 = p + 1;
            }
        ok &= sg_unprotect(1, r0, r1);
    }
    if (!ok) {
        static int n;
        if (n++ < 4) printf("[sync] guard: making guarded pages writable again failed (err %lu)\n", GetLastError());
    }
    g_sg_runs[0].clear();
    g_sg_runs[1].clear();
    g_sg_active.store(false, std::memory_order_seq_cst);
    g_sg_gen++;
    WakeAllConditionVariable(&g_sg_cv);
    g_sg_st.lift_ns += sg_now_ns() - t0;
}
// One page out of the guard (a write from a thread that must not wait). (g_sg_lock exclusive)
static void sg_unguard_locked(int r, u64 p) {
    if (!sg_bit(r, p)) return;
    sg_bit_set(r, p, false);
    if (r == 1) {
        std::lock_guard<std::mutex> l(g_sw_mu);
        const u8 s = __atomic_load_n(&g_sw_st[p], __ATOMIC_ACQUIRE);
        if (!g_sw_off.load() && (s == SW_ARMED || s == SW_PENDING)) return;   // (its retried write is the soft watch's fault)
    }
    sg_protect(r, p, p + 1, PAGE_READWRITE);
}
// Wait for the lift (g_sg_lock held shared; released on return). Past the limit the waiter lifts the guard itself.
static void sg_wait_shared(bool io) {
    static const u64 lim = sg_env("LARECOMP_SYNC_GUARD_WAIT_MS", 200);
    const u64 gen = g_sg_gen, t0 = sg_now_ns();
    bool late = false;
    while (g_sg_gen == gen) {
        const u64 el = (sg_now_ns() - t0) / 1000000;
        if (el >= lim) { late = true; break; }
        if (!SleepConditionVariableSRW(&g_sg_cv, &g_sg_lock, (DWORD)(lim - el), CONDITION_VARIABLE_LOCKMODE_SHARED) &&
            GetLastError() != ERROR_TIMEOUT)
            break;   // (failed otherwise: never seen; the write goes ahead)
    }
    ReleaseSRWLockShared(&g_sg_lock);
    if (late) {
        AcquireSRWLockExclusive(&g_sg_lock);
        if (g_sg_gen == gen && g_sg_active.load()) {
            sg_lift_locked();
            g_sg_st.timeouts++;
            const u64 now = sg_now_ns();
            memmove(g_sg_tmo, g_sg_tmo + 1, sizeof g_sg_tmo - sizeof g_sg_tmo[0]);
            g_sg_tmo[3] = now;
            if (g_sg_tmo[0] && now - g_sg_tmo[0] < 30000000000ull) g_sg_dead = true;
            printf("[sync] guard: a game thread waited %llu ms for the render thread to pass the frame fence (LARECOMP_SYNC_GUARD_WAIT_MS): guard lifted%s\n",
                   (unsigned long long)lim, g_sg_dead ? "; 4 times within 30 s: off for good, SyncWait waits for the replay" : "");
        }
        ReleaseSRWLockExclusive(&g_sg_lock);
    }
    const u64 ns = sg_now_ns() - t0;
    (io ? g_sg_st.io_waits : g_sg_st.waits)++;
    g_sg_st.wait_ns += ns;
    for (u64 m = g_sg_st.wait_max.load(); ns > m && !g_sg_st.wait_max.compare_exchange_weak(m, ns);) {}
}
static LONG CALLBACK sg_fault(EXCEPTION_POINTERS* e) {
    const EXCEPTION_RECORD* x = e->ExceptionRecord;
    if (x->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || x->NumberParameters < 2 || x->ExceptionInformation[0] != 1) return EXCEPTION_CONTINUE_SEARCH;
    const u64 a = (u64)x->ExceptionInformation[1];
    int r;
    u64 p;
    if (!sg_page_of(a, &r, &p)) return EXCEPTION_CONTINUE_SEARCH;
    AcquireSRWLockShared(&g_sg_lock);
    g_sg_st.faults++;   // (every fault in the guarded ranges, retries after a lift included)
    if (!sg_bit(r, p)) {
        ReleaseSRWLockShared(&g_sg_lock);
        // (not guarded: a heap page a lift made writable since the fault is written again; window pages are the soft watch's)
        MEMORY_BASIC_INFORMATION mi;
        if (r == 0 && VirtualQuery((void*)a, &mi, sizeof mi) && (mi.Protect & PAGE_READWRITE)) return EXCEPTION_CONTINUE_EXECUTION;
        return EXCEPTION_CONTINUE_SEARCH;
    }
    // (the render thread, its workers, a host thread, or a game thread holding the exec lock the render thread takes at its
    // next execute: never waits, the page leaves the guard)
    if (!rt_on_guest_thread() || backend_exec_lock_held()) {
        ReleaseSRWLockShared(&g_sg_lock);
        AcquireSRWLockExclusive(&g_sg_lock);
        sg_unguard_locked(r, p);
        ReleaseSRWLockExclusive(&g_sg_lock);
        g_sg_st.others++;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    sg_wait_shared(false);
    return EXCEPTION_CONTINUE_EXECUTION;
}
// Every ~2 s (from the arm, on the game thread): per frame armed, the pages guarded and their runs, the faults taken, the
// game writes that waited and for how long, the arm (game thread) and lift (render thread) VirtualProtect time
static void sg_stats() {
    static u64 t_last;
    const u64 t = GetTickCount64();
    if (!t_last) t_last = t;
    if (t - t_last < 2000) return;
    const u64 arms = g_sg_st.arms.exchange(0), frames = g_sg_st.frames.exchange(0);
    const double fr = arms ? (double)arms : 1.0;
    printf("[sync] guard, last %.1f s: %llu of %llu frames armed (not: %llu over the caps, %llu before a whole frame); a frame armed: "
           "%.0f pages guarded in %.1f runs, %.2f faults, %.2f writes waited for %.3f ms, arm %.3f ms + lift %.3f ms; longest wait %.2f ms, "
           "%llu file reads waited, %llu pages left the guard (other threads), %llu waits timed out\n",
           (t - t_last) / 1000.0, (unsigned long long)arms, (unsigned long long)frames, (unsigned long long)g_sg_st.over.exchange(0),
           (unsigned long long)g_sg_st.fresh.exchange(0), g_sg_st.pages.exchange(0) / fr, g_sg_st.runs.exchange(0) / fr,
           g_sg_st.faults.exchange(0) / fr, g_sg_st.waits.exchange(0) / fr, g_sg_st.wait_ns.exchange(0) / 1e6 / fr,
           g_sg_st.arm_ns.exchange(0) / 1e6 / fr, g_sg_st.lift_ns.exchange(0) / 1e6 / fr, g_sg_st.wait_max.exchange(0) / 1e6,
           (unsigned long long)g_sg_st.io_waits.exchange(0), (unsigned long long)g_sg_st.others.exchange(0),
           (unsigned long long)g_sg_st.timeouts.exchange(0));
    t_last = t;
}
extern "C" int sync_guard_on(void) {
    int m = __atomic_load_n(&g_sg_mode, __ATOMIC_ACQUIRE);
    if (m >= 0) return m;
    static std::mutex mu;
    std::lock_guard<std::mutex> l(mu);
    if (g_sg_mode >= 0) return g_sg_mode;
    const char* e = getenv("LARECOMP_SYNC_GUARD");
    m = e && *e == '1';
    if (m) {   // (first in the chain, ahead of sw_fault; registered before any page is guarded)
        AddVectoredExceptionHandler(1, sg_fault);
        printf("[sync] guard on (LARECOMP_SYNC_GUARD=1): a game write to a page the pending replay read last frame waits for it\n");
    }
    __atomic_store_n(&g_sg_mode, m, __ATOMIC_RELEASE);
    return m;
}
static void sg_note(SgSet (&rec)[2][2], u64 addr, u64 len) {
    if (__atomic_load_n(&g_sg_mode, __ATOMIC_RELAXED) != 1 || !len) return;
    for (int r = 0; r < 2; r++) {
        const u64 b = sg_base(r), sz = sg_size(r), e = addr + len;
        if (!b || e <= b || addr >= b + sz) continue;
        const u64 p0 = ((addr < b ? b : addr) - b) >> 12, p1 = ((e > b + sz ? b + sz : e) - b + 0xFFF) >> 12;
        std::lock_guard<std::mutex> l(g_sg_rec_mu);
        SgSet& s = rec[g_sg_cur][r];
        const u64 need = ((r ? g_alias_import_size : g_heap_size) >> 18) + 1;   // (words of 64 pages)
        if (s.bits.size() < need) s.bits.resize(need, 0);
        for (u64 p = p0; p < p1 && (p >> 6) < s.bits.size(); p++) {
            u64& w = s.bits[p >> 6];
            if (!w) s.words.push_back((u32)(p >> 6));
            w |= 1ull << (p & 63);
        }
    }
}
// exec.c, the replay (render thread): it read [addr, addr + len) of guest memory for the frame being replayed
extern "C" void sync_guard_note(u64 addr, u64 len) { sg_note(g_sg_rec, addr, len); }
// exec.c, the replay (render thread): it wrote [addr, addr + len) itself (inline uniform updates, reports, buffer copies):
// those pages are not guarded at the next arm (each would fault on the render thread and leave the guard anyway)
extern "C" void sync_guard_note_write(u64 addr, u64 len) { sg_note(g_sg_wrec, addr, len); }
// exec.c (render thread): the queue passed fence `ticket` of sync `key`. Of the frame sync: one frame's reads end, the
// guard waiting for this fence (or an earlier one) is lifted.
extern "C" void sync_guard_fence(const void* key, u64 ticket) {
    if (__atomic_load_n(&g_sg_mode, __ATOMIC_RELAXED) != 1 || key != g_sg_key.load(std::memory_order_acquire)) return;
    {
        std::lock_guard<std::mutex> l(g_sg_rec_mu);
        g_sg_cur ^= 1;
        for (SgSet& s : g_sg_rec[g_sg_cur]) sg_clear(s);
        for (SgSet& s : g_sg_wrec[g_sg_cur]) sg_clear(s);
        if (g_sg_frames.load() < 2) g_sg_frames++;
    }
    g_sg_passed.store(ticket, std::memory_order_seq_cst);
    if (!g_sg_active.load(std::memory_order_seq_cst)) return;   // (arm checks g_sg_passed after it sets active: one of us lifts)
    AcquireSRWLockExclusive(&g_sg_lock);
    if (g_sg_active.load() && ticket >= g_sg_ticket) sg_lift_locked();
    ReleaseSRWLockExclusive(&g_sg_lock);
}
// The previous frame's reads as page runs, made read-only and guarded. (g_sg_lock exclusive) False: not armed.
static bool sg_arm_locked() {
    static const u64 cap = sg_env("LARECOMP_SYNC_GUARD_RUNS", 1024), pcap = sg_env("LARECOMP_SYNC_GUARD_PAGES", 16384);
    size_t nruns = 0;
    u64 npages = 0;
    {
        std::lock_guard<std::mutex> l(g_sg_rec_mu);
        for (int r = 0; r < 2; r++) {
            SgSet& s = g_sg_rec[g_sg_cur ^ 1][r];
            const SgSet& ws = g_sg_wrec[g_sg_cur ^ 1][r];
            auto& runs = g_sg_runs[r];
            runs.clear();
            std::sort(s.words.begin(), s.words.end());
            for (u32 w : s.words)
                for (u64 bits = s.bits[w] & ~(w < ws.bits.size() ? ws.bits[w] : 0); bits; bits &= bits - 1) {
                    const u64 p = ((u64)w << 6) + (u64)__builtin_ctzll(bits);
                    if (!runs.empty() && runs.back().second == p) runs.back().second++;
                    else runs.push_back({p, p + 1});
                    npages++;
                }
            nruns += runs.size();
        }
    }
    if (nruns > cap || npages > pcap) {
        g_sg_runs[0].clear();
        g_sg_runs[1].clear();
        g_sg_st.over++;
        return false;
    }
    if (g_sg_bits[0].empty() && g_heap_size) g_sg_bits[0].resize((g_heap_size >> 18) + 1, 0);
    if (g_sg_bits[1].empty() && g_alias_soft && g_alias_import_size) g_sg_bits[1].resize((g_alias_import_size >> 18) + 1, 0);
    bool ok = true;
    u64 pages = 0;
    // heap: read-only now (not while exec.c imports it)
    if (g_sg_heap_hold.load() || g_sg_bits[0].empty()) g_sg_runs[0].clear();
    const u64 used = __atomic_load_n(&g_heap_used, __ATOMIC_RELAXED) >> 12;
    // (held until the pages are read-only and this arm is done: a kernel write registered before is left out, one starting
    // after finds the guard bits under g_sg_lock, sync_guard_io)
    std::lock_guard<std::mutex> io(g_sg_io_mu);
    auto in_io = [](u64 a, u64 e) {
        if (g_sg_nio > 64) return true;
        for (int k = 0; k < g_sg_nio; k++) if (a < g_sg_io[k][1] && g_sg_io[k][0] < e) return true;
        return false;
    };
    for (auto& rn : g_sg_runs[0]) {
        if (rn.second > used) rn.second = used;
        if (rn.first < rn.second && in_io(g_heap_base + (rn.first << 12), g_heap_base + (rn.second << 12))) rn.second = rn.first;   // (left out whole)
        if (rn.first >= rn.second) { rn.second = rn.first; continue; }
        if (!(ok = sg_protect(0, rn.first, rn.second, PAGE_READONLY))) { rn.second = rn.first; break; }
        for (u64 p = rn.first; p < rn.second; p++) sg_bit_set(0, p, true);
        pages += rn.second - rn.first;
    }
    // alias window: pages the soft watch can arm; ARMED ones are read-only already
    if (ok && !g_sg_runs[1].empty()) {
        std::lock_guard<std::mutex> l(g_sw_mu);
        if (g_sw_off.load() || !g_sw_armable || !g_sw_st || g_sg_bits[1].empty()) g_sg_runs[1].clear();
        static std::vector<std::pair<u64, u64>> kept;
        kept.clear();
        const u64 arm_end = g_sw_arm_end.load(std::memory_order_acquire) >> 12;
        u64 r0 = 0, r1 = 0;
        for (auto& rn : g_sg_runs[1])
            for (u64 p = rn.first; p < rn.second && p < arm_end && ok; p++) {
                const u8 s = __atomic_load_n(&g_sw_st[p], __ATOMIC_ACQUIRE);
                const u64 pa = g_alias_import_base + (p << 12);
                if (s == SW_PENDING || sw_in_io(p) || in_io(pa, pa + 0x1000)) continue;
                sg_bit_set(1, p, true);
                pages++;
                if (!kept.empty() && kept.back().second == p) kept.back().second++; else kept.push_back({p, p + 1});
                if (s == SW_ARMED) continue;
                if (r1 == p && r1 > r0) { r1++; continue; }
                ok = sg_protect(1, r0, r1, PAGE_READONLY);
                r0 = p, r1 = p + 1;
            }
        if (ok) ok = sg_protect(1, r0, r1, PAGE_READONLY);
        g_sg_runs[1].swap(kept);
    }
    if (!ok) {   // (pages made read-only so far are made writable again by the lift)
        g_sg_dead = true;
        printf("[sync] guard: VirtualProtect failed (err %lu): off for good, SyncWait waits for the replay\n", GetLastError());
        return false;
    }
    g_sg_st.arms++;
    g_sg_st.runs += g_sg_runs[0].size() + g_sg_runs[1].size();
    g_sg_st.pages += pages;
    return true;
}
// objects.c, SyncWait's ticket lag: the game is let go before the replay passed fence `ticket` of sync `key`. 1: guarded
// until it does; 0: not (off, the first frame of a sync, too many runs, failed): the caller waits for that fence.
extern "C" int sync_guard_arm(const void* key, u64 ticket) {
    if (sync_guard_on() != 1) return 0;
    g_sg_st.frames++;
    if (g_sg_key.exchange(key, std::memory_order_acq_rel) != key) {   // (a new frame sync: what was read so far is no one frame's)
        std::lock_guard<std::mutex> l(g_sg_rec_mu);
        for (auto& set : g_sg_rec) for (SgSet& s : set) sg_clear(s);
        for (auto& set : g_sg_wrec) for (SgSet& s : set) sg_clear(s);
        g_sg_frames = 0;
        g_sg_st.fresh++;
        return 0;
    }
    if (g_sg_frames.load() < 2) { g_sg_st.fresh++; return 0; }   // (the reads before then are part of a frame)
    if (g_sg_passed.load(std::memory_order_seq_cst) >= ticket) { sg_stats(); return 1; }   // (the replay is past it already: nothing to guard)
    const u64 t0 = sg_now_ns();
    AcquireSRWLockExclusive(&g_sg_lock);
    if (g_sg_active.load()) sg_lift_locked();   // (left over: a waiter that timed out lifts, a fence never passed: never seen)
    bool armed = !g_sg_dead && sg_arm_locked();
    if (armed) {
        g_sg_ticket = ticket;
        g_sg_active.store(true, std::memory_order_seq_cst);
        if (g_sg_passed.load(std::memory_order_seq_cst) >= ticket) sg_lift_locked();   // (the replay passed it meanwhile)
    } else if (!g_sg_runs[0].empty() || !g_sg_runs[1].empty())
        sg_lift_locked();   // (a failed arm: what it guarded)
    g_sg_st.arm_ns += sg_now_ns() - t0;
    ReleaseSRWLockExclusive(&g_sg_lock);
    sg_stats();
    return armed;
}
// hle_fs.cpp / the cheat VM, around a kernel write into [ptr, ptr + len) of guest memory (ReadFile, WriteProcessMemory):
// a read-only page would fail it. begin: guarded pages there are waited for (a game thread) or leave the guard (any
// other), and the range is never guarded until end.
extern "C" void sync_guard_io(const void* ptr, size_t len, int begin) {
    if (!len || __atomic_load_n(&g_sg_mode, __ATOMIC_RELAXED) != 1) return;
    const u64 a = (u64)ptr, e = a + len;
    {
        std::lock_guard<std::mutex> l(g_sg_io_mu);
        if (!begin) {
            for (int k = 0; k < g_sg_nio && g_sg_nio <= 64; k++)
                if (g_sg_io[k][0] == a && g_sg_io[k][1] == e) { g_sg_io[k][0] = g_sg_io[g_sg_nio - 1][0]; g_sg_io[k][1] = g_sg_io[--g_sg_nio][1]; break; }
            if (g_sg_nio > 64) g_sg_nio = 0;   // (untracked ones ended or not: they were never guarded, start over; ok to over-guard one later)
            return;
        }
        if (g_sg_nio >= 64) g_sg_nio = 65;
        else { g_sg_io[g_sg_nio][0] = a; g_sg_io[g_sg_nio][1] = e; g_sg_nio++; }
    }
    AcquireSRWLockShared(&g_sg_lock);   // (an arm under way is waited for: its bits are all set by then)
    bool any = false;
    if (!g_sg_active.load()) { ReleaseSRWLockShared(&g_sg_lock); return; }   // (no bits outside an armed guard)
    for (int r = 0; r < 2 && !any; r++) {
        const u64 b = sg_base(r), sz = sg_size(r);
        if (!b || e <= b || a >= b + sz) continue;
        for (u64 p = ((a < b ? b : a) - b) >> 12, pe = ((e > b + sz ? b + sz : e) - b + 0xFFF) >> 12; p < pe && !any; p++) any = sg_bit(r, p);
    }
    if (!any) { ReleaseSRWLockShared(&g_sg_lock); return; }
    if (rt_on_guest_thread() && !backend_exec_lock_held()) { sg_wait_shared(true); return; }
    ReleaseSRWLockShared(&g_sg_lock);   // (a host thread's read: its pages leave the guard)
    AcquireSRWLockExclusive(&g_sg_lock);
    for (int r = 0; r < 2; r++) {
        const u64 b = sg_base(r), sz = sg_size(r);
        if (!b || e <= b || a >= b + sz) continue;
        for (u64 p = ((a < b ? b : a) - b) >> 12, pe = ((e > b + sz ? b + sz : e) - b + 0xFFF) >> 12; p < pe; p++) sg_unguard_locked(r, p);
    }
    ReleaseSRWLockExclusive(&g_sg_lock);
}
// exec.c around an import of the heap (1 before, 0 after; vk_exec_reset 1 then 0): the guard is lifted, the heap stays
// writable until it is done
extern "C" void sync_guard_hold(int on) {
    if (!on) { g_sg_heap_hold--; return; }
    g_sg_heap_hold++;
    // (always through the lock: an arm in progress read the hold as 0 and may be making heap pages read-only right now)
    AcquireSRWLockExclusive(&g_sg_lock);
    if (g_sg_active.load()) sg_lift_locked();
    ReleaseSRWLockExclusive(&g_sg_lock);
}
