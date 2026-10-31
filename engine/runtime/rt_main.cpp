// Loader + boot: maps main (and sdk for its data exports) identity-mapped, relocates, builds the function
// table, routes every SDK import through a tracing thunk (HLE if registered, else logged stub returning 0),
// then runs DT_INIT -> nninitStartup -> init_array -> nnMain on a guest thread.
#include "rt.h"
#include "hle.h"
#include "pc.h"
#include "cpu_place.h"
extern "C" void hle_fs_index_romfs_early(void);   // hle_fs.cpp: the RomFS pack's index built ahead of the first open
extern "C" void pc_input_keys_restore(void);   // pc_input.cpp: the accessibility hotkeys back (the boot's end: no atexit)
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <cstring>
#include <vector>
#include <unordered_map>
#include <map>
#include <atomic>
#include <mutex>
#include <utility>
#include <array>
#include <algorithm>
#include <cstdarg>
#include <thread>
#include <chrono>
#ifdef _WIN32
#include <tlhelp32.h>
#include <psapi.h>
#else
#include <sys/resource.h>
#include <ucontext.h>
#include <unistd.h>
#include "linux/plat_posix.h"   // signals, exe location (runtime/linux/plat_posix.cpp)
#endif

struct FtabEnt { u64 off; GuestFn fn; };
extern const FtabEnt g_ftab_init[];
extern const u64 g_ftab_count;
extern const char* const g_imp_names[];
extern const u64 g_imp_count;

// lifted sdk (gen/sdk, gen.py --prefix s --plt-all): runs only where bound (lift whitelist / sdk-internal calls)
extern const FtabEnt g_s_ftab_init[];
extern const u64 g_s_ftab_count;
extern const char* const g_s_imp_names[];
extern const u64 g_s_imp_count;
constexpr u64 kMaxSdkImports = 16384;
GuestFn g_s_imp[kMaxSdkImports];
static GuestFn* g_sftab; static u64 g_sftab_n;         // indexed by (addr - SDK_BASE) >> 2

GuestFn* g_ftab; u64 g_ftab_n;
GuestFn g_imp[kMaxImports];

static const u64 MAIN_BASE = 0x7100000000ull;
static const u64 SDK_BASE  = 0x7180000000ull;          // sdk image: data exports + lifted code (gen/sdk)
static const u64 IMP_FAKE  = MAIN_BASE + 0x5000000ull; // fake code address of import i = IMP_FAKE + 4*i

[[noreturn]] static void die(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap); fputc('\n', stderr); fflush(stderr);
    ExitProcess(1);
}

// ---------------------------------------------------------------- module image
struct Sym { std::string name; u8 type; u16 shndx; u64 value; };
struct Module {
    const char* name; u64 base, size;
    u64 dyn[64] = {};           // DT_* < 64
    std::vector<Sym> syms;
    std::unordered_map<std::string, u64> exports;   // name -> absolute address
};

static void load_module(Module& m, const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) die("cannot open %s", path.c_str());
    fseek(f, 0, SEEK_END); long fsz = ftell(f); fseek(f, 0, SEEK_SET);
    std::vector<u8> tmp(fsz); fread(tmp.data(), 1, fsz, f); fclose(f);
    u32 mod0 = *(u32*)&tmp[4];
    if (memcmp(&tmp[mod0], "MOD0", 4)) die("%s: no MOD0", m.name);
    s32 bss_end = *(s32*)&tmp[mod0 + 12];
    m.size = ((u64)(mod0 + bss_end) + 0xFFFF) & ~0xFFFFull;
    if (m.size < (u64)fsz) m.size = ((u64)fsz + 0xFFFF) & ~0xFFFFull;
    void* p = VirtualAlloc((void*)m.base, m.size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (p != (void*)m.base) die("%s: cannot map at %llx (err %lu)", m.name, m.base, GetLastError());
    memcpy(p, tmp.data(), fsz);
    u64 dyn = m.base + mod0 + *(s32*)&tmp[mod0 + 4];
    for (u64 d = dyn; ; d += 16) {
        u64 tag = *(u64*)d, val = *(u64*)(d + 8);
        if (tag == 0) break;
        if (tag < 64) m.dyn[tag] = val;
    }
    u64 symtab = m.base + m.dyn[6], strtab = m.base + m.dyn[5];
    u32 nsyms = *(u32*)(m.base + m.dyn[4] + 4);          // DT_HASH nchain
    for (u32 i = 0; i < nsyms; i++) {
        u64 s = symtab + i * 24ull;
        Sym y{ std::string((const char*)(strtab + *(u32*)s)), (u8)(*(u8*)(s + 4) & 0xF), *(u16*)(s + 6), *(u64*)(s + 8) };
        if (y.shndx != 0 && !y.name.empty()) m.exports[y.name] = m.base + y.value;
        m.syms.push_back(std::move(y));
    }
    printf("[ld] %s at %llx size %llx syms %u exports %zu\n", m.name, m.base, m.size, nsyms, m.exports.size());
}

static Module g_main{"main", MAIN_BASE}, g_sdk{"sdk", SDK_BASE};
extern "C" void totk_engine_boot(u64 main_base);   // nvn/totk_game_patches.c
static std::unordered_map<std::string, u32> g_imp_index;

// resolve(sym) for a module: own definition, else import (fake code addr / sdk data), else 0
static u64 resolve(Module& m, u32 si, int* unresolved) {
    const Sym& y = m.syms[si];
    if (y.shndx != 0) return m.base + y.value;
    if (&m == &g_main) {
        if (y.type == 1) {                                   // STT_OBJECT: real data from the sdk image
            auto it = g_sdk.exports.find(y.name);
            if (it != g_sdk.exports.end()) return it->second;
        } else {
            auto it = g_imp_index.find(y.name);
            if (it != g_imp_index.end()) return IMP_FAKE + 4ull * it->second;
        }
    } else {
        auto it = g_main.exports.find(y.name);
        if (it != g_main.exports.end()) return it->second;
    }
    (*unresolved)++;
    return 0;
}

static void relocate(Module& m) {
    int unresolved = 0; u64 n = 0;
    const u64 tabs[2][2] = { { m.dyn[7], m.dyn[8] }, { m.dyn[23], m.dyn[2] } };   // RELA/RELASZ, JMPREL/PLTRELSZ
    for (auto& t : tabs) {
        for (u64 r = m.base + t[0]; r < m.base + t[0] + t[1]; r += 24, n++) {
            u64 off = *(u64*)r, info = *(u64*)(r + 8); s64 add = *(s64*)(r + 16);
            u32 type = (u32)info, sym = (u32)(info >> 32);
            u64* where = (u64*)(m.base + off);
            switch (type) {
            case 1027: *where = m.base + add; break;                                       // RELATIVE
            case 257: case 1025: case 1026: *where = resolve(m, sym, &unresolved) + add; break; // ABS64 / GLOB_DAT / JUMP_SLOT
            case 0: break;
            default: die("%s: reloc type %u at %llx", m.name, type, off);
            }
        }
    }
    printf("[ld] %s: %llu relocs, %d unresolved symbol refs\n", m.name, n, unresolved);
}

// ---------------------------------------------------------------- import tracing
static GuestFn g_hle[kMaxImports];
static std::atomic<u64> g_imp_calls[kMaxImports];
static std::atomic<u64> g_imp_seq{0};
thread_local const char* t_hle_cur;      // import / NVN proc currently executing on this thread (crash reports)
static bool g_trace_all = getenv("TOTK_TRACE_ALL") != nullptr;

static std::unordered_map<std::string, GuestFn>& hle_table() { static std::unordered_map<std::string, GuestFn> t; return t; }
HleReg::HleReg(const char* name, GuestFn fn) { hle_table()[name] = fn; }
static std::unordered_map<std::string, std::pair<GuestFn, GuestFn*>>& hle_wraps() {
    static std::unordered_map<std::string, std::pair<GuestFn, GuestFn*>> t; return t;
}
HleWrap::HleWrap(const char* name, GuestFn fn, GuestFn* orig) { hle_wraps()[name] = {fn, orig}; }

// Full call counts only with TOTK_IMP_STATS=1 (or the watchdog). Otherwise a plain read once the first 3 calls are
// logged, and print_stats shows counts capped at 3.
#ifdef TOTK_PGO_GEN
extern "C" int __llvm_profile_write_file(void);   // (compiler-rt profile runtime)
#endif
bool g_imp_count_all = getenv("TOTK_IMP_STATS") != nullptr || getenv("TOTK_WATCHDOG") != nullptr;
static void imp_dispatch(int i, Ctx* c) {
    u64 n = g_imp_calls[i].load(std::memory_order_relaxed);
    if (n < 3 || g_imp_count_all) n = g_imp_calls[i].fetch_add(1, std::memory_order_relaxed) + 1;
    else n = 4;
    if (n <= 3 || g_trace_all) {
        printf("[imp %6llu] %s%s(%llx, %llx, %llx, %llx) lr=%llx\n", (unsigned long long)++g_imp_seq,
               g_hle[i] ? "" : "UNIMPL ", g_imp_names[i], c->x[0], c->x[1], c->x[2], c->x[3], c->x[30]);
        if (!g_hle[i]) fflush(stdout);
    }
    const char* prev = t_hle_cur; t_hle_cur = g_imp_names[i];
    if (g_hle[i]) g_hle[i](c);
    else c->x[0] = 0;
    t_hle_cur = prev;
}
template <int N> static void imp_thunk(Ctx* c) { imp_dispatch(N, c); }
template <int... I> static constexpr std::array<GuestFn, sizeof...(I)> mk_thunks(std::integer_sequence<int, I...>) { return { imp_thunk<I>... }; }
static constexpr auto k_thunks = mk_thunks(std::make_integer_sequence<int, kMaxImports>{});

void rt_call_indirect(Ctx* c, u64 t) {
    if (t >= SDK_BASE && t < SDK_BASE + g_sdk.size) {
        u64 i = (t - SDK_BASE) >> 2;
        if (i < g_sftab_n && g_sftab[i]) return g_sftab[i](c);
        rt_trap(c, t, "indirect call into sdk image (no lifted function at this address)");
    }
    rt_trap(c, t, "indirect call to unknown target");
}
static thread_local Ctx* t_ctx;     // guest context of this host thread (crash reports, rt_fatal_guest)
// Fatal error from host code (NVN layer etc.): message + the current guest thread's x29 backtrace.
extern "C" [[noreturn]] void rt_fatal_guest(const char* msg) {
    fflush(stdout);
    fprintf(stderr, "[fatal] %s", msg);
    if (Ctx* c = t_ctx) {
        fprintf(stderr, "[fatal] lr=%llx sp=%llx\n", c->x[30], c->sp);
        u64 fp = c->x[29];
        for (int i = 0; i < 24 && fp >= c->sp && fp < c->sp + (256ull << 20) && !(fp & 7); i++) {
            u64 nfp = *(u64*)fp;
            fprintf(stderr, "[fatal]   #%-2d lr=%llx\n", i, *(u64*)(fp + 8));
            if (nfp <= fp) break;
            fp = nfp;
        }
    }
    fflush(stderr);
    ExitProcess(4);
}
// gen.py --trace hook: entry (x0-x7, lr) and ret (x0) of selected functions TOTK_TRACE_MAX lines per function. With
// TOTK_ORACLE_CAPTURE=<dir>, with no lines.
bool oracle_capture_on();
void oracle_capture(Ctx* c, u64 fn);
void rt_trace(Ctx* c, u64 fn, int ret) {
    static const bool cap = oracle_capture_on();
    if (cap) {
        if (!ret) oracle_capture(c, fn);
        return;
    }
    static std::unordered_map<u64, int> n; static std::mutex m;
    static int maxn = getenv("TOTK_TRACE_MAX") ? atoi(getenv("TOTK_TRACE_MAX")) : 200;
    std::lock_guard<std::mutex> l(m);
    if (n[fn * 2 + ret]++ >= maxn) return;
    if (!ret) printf("[trace %5lu] > %llx(%llx, %llx, %llx, %llx, %llx, %llx, %llx, %llx) lr=%llx\n", GetCurrentThreadId(), fn,
                     c->x[0], c->x[1], c->x[2], c->x[3], c->x[4], c->x[5], c->x[6], c->x[7], c->x[30]);
    else printf("[trace %5lu] < %llx = %llx\n", GetCurrentThreadId(), fn, c->x[0]);
}
// gen.py --time hook: rdtsc cycles of each call of selected functions (callees included). Every TOTK_FNTIME_S seconds
// (default 10) one line per function for that window: calls, mean cycles per call over the calls under 2^22 cycles
// (a call preempted mid-way would swamp the mean; those are only counted), and the window's median from a histogram
// of 1/16-octave buckets. Lock-free per slot; the window report is taken by whichever call crosses the boundary.
void rt_time(u64 fn, u64 cyc) {
    struct Slot { std::atomic<u64> fn, calls, sum, slow; std::atomic<u32> hist[23 * 16]; };
    static Slot slots[8];
    static std::atomic<u64> next{0};
    static const u64 period = [] {
        auto a = std::chrono::steady_clock::now(); u64 t0 = __builtin_ia32_rdtsc();
        while (std::chrono::steady_clock::now() - a < std::chrono::milliseconds(50)) {}
        u64 tsc_hz = (__builtin_ia32_rdtsc() - t0) * 20;
        printf("[fntime] tsc %.3f GHz\n", tsc_hz / 1e9);
        return tsc_hz * (getenv("TOTK_FNTIME_S") ? atoi(getenv("TOTK_FNTIME_S")) : 10);
    }();
    Slot* s = nullptr;
    for (Slot& x : slots) {
        u64 cur = x.fn.load(std::memory_order_relaxed);
        if (cur == fn) { s = &x; break; }
        if (cur == 0) { u64 z = 0; if (x.fn.compare_exchange_strong(z, fn) || z == fn) { s = &x; break; } }
    }
    if (!s) return;
    s->calls.fetch_add(1, std::memory_order_relaxed);
    if (cyc >= (1ull << 22)) s->slow.fetch_add(1, std::memory_order_relaxed);
    else {
        s->sum.fetch_add(cyc, std::memory_order_relaxed);
        int o = 63 - __builtin_clzll(cyc | 1), sub = o >= 4 ? (int)((cyc >> (o - 4)) & 15) : 0;
        s->hist[std::min(o, 22) * 16 + sub].fetch_add(1, std::memory_order_relaxed);
    }
    u64 now = __builtin_ia32_rdtsc(), n = next.load(std::memory_order_relaxed);
    if (n == 0) { next.compare_exchange_strong(n, now + period); return; }
    if (now < n || !next.compare_exchange_strong(n, now + period)) return;
    for (Slot& x : slots) {
        if (!x.fn.load()) continue;
        u64 calls = x.calls.exchange(0), sum = x.sum.exchange(0), slow = x.slow.exchange(0), fast = calls - slow, seen = 0;
        double med = 0;
        u32 h[23 * 16];
        for (int i = 0; i < 23 * 16; i++) h[i] = x.hist[i].exchange(0);
        for (int i = 0; i < 23 * 16 && fast; i++)
            if ((seen += h[i]) * 2 >= fast) { int o = i / 16; med = o >= 4 ? (16 + i % 16 + 0.5) * (double)(1ull << (o - 4)) : (double)(1ull << o); break; }
        printf("[fntime] %llx calls %llu mean %.0f median %.0f slow %llu\n", x.fn.load(), calls, fast ? (double)sum / fast : 0.0,
               med, slow);
    }
    fflush(stdout);
}
// TPIDRRO_EL0 for lifted sdk code: Horizon's per-thread 0x200-byte ThreadLocalRegion (sdk keeps its thread pointer
// at +0x1F0). One per host thread (guest threads are host threads).
u64 rt_tpidrro(Ctx*) { alignas(16) static thread_local u8 tlr[0x200]; return (u64)tlr; }
// sdk PLT slot with nothing to bind (no HLE, no main export, no lifted definition)
// The name comes from the caller's `bl <plt stub>` (lr - 4) -> stub's adrp/ldr GOT slot -> JMPREL symbol.
static void sdk_imp_missing(Ctx* c) {
    static char msg[512]; const char* nm = "?";
    u64 lr = c->x[30];
    if (lr >= SDK_BASE + 4 && lr < SDK_BASE + g_sdk.size) {
        u32 bl = *(u32*)(lr - 4);
        if ((bl >> 26) == 0x25) {
            u64 stub = lr - 4 + ((s64)((s32)(bl << 6) >> 6) << 2);
            u32 adrp = *(u32*)stub, ldr = *(u32*)(stub + 4);
            u64 imm = (((adrp >> 29) & 3) | (((adrp >> 5) & 0x7FFFF) << 2)) << 12;
            u64 got = (stub & ~0xFFFull) + imm + ((ldr >> 10) & 0xFFF) * 8 - SDK_BASE;
            for (u64 r = g_sdk.base + g_sdk.dyn[23]; r < g_sdk.base + g_sdk.dyn[23] + g_sdk.dyn[2]; r += 24)
                if (*(u64*)r == got) { nm = g_sdk.syms[*(u64*)(r + 8) >> 32].name.c_str(); break; }
        }
    }
    snprintf(msg, sizeof msg, "sdk import with no binding: %s (caller = lr)", nm);
    rt_trap(c, lr, msg);
}

// ---------------------------------------------------------------- crash reporting
static const char* volatile g_stage = "load";
static volatile u64 g_stage_idx;
static std::vector<std::pair<u64, u64>> g_fnaddr;     // host fn address -> guest offset, sorted

static u64 guest_fn_of(u64 rip) {      // guest offset; sdk functions are tagged with SDK_TAG
    if (g_fnaddr.empty()) {
        for (u64 i = 0; i < g_ftab_count; i++) g_fnaddr.push_back({ (u64)g_ftab_init[i].fn, g_ftab_init[i].off });
        for (u64 i = 0; i < g_s_ftab_count; i++) g_fnaddr.push_back({ (u64)g_s_ftab_init[i].fn, g_s_ftab_init[i].off | (1ull << 40) });
        std::sort(g_fnaddr.begin(), g_fnaddr.end());
    }
    auto it = std::upper_bound(g_fnaddr.begin(), g_fnaddr.end(), std::make_pair(rip, ~0ull));
    return it == g_fnaddr.begin() ? ~0ull : std::prev(it)->second;
}

extern "C" u64 g_rt_heap_base;
extern "C" u64 kernel_heap_size(void);
extern "C" unsigned long long renderer_present_count(void);   // (nvn backend: presents so far)
extern "C" uint64_t SDL_GetTicksNS(void);

// ---------------------------------------------------------------- hardware write watchpoint (diagnostics)
// rt_watch_write(addr): DR0 = 4-byte write watch on every thread that exists now; each hit logs the guest function
// that wrote, the written value and the guest lr (first 40 hits). Used to find which game code rewrites a variable.
#ifndef _WIN32
// Linux: no hardware watchpoints (would need perf_event_open / ptrace on every thread); the TOTK_WATCH / TOTK_VFR_WATCH diagnostics
// are Windows-only.
extern "C" void rt_watch_write(u64 addr) { printf("[watch] write watch on %llx: not available on Linux\n", (unsigned long long)addr); }
#else
static volatile u64 g_watch_addr;
static volatile long g_watch_hits;
static LONG WINAPI on_watch(EXCEPTION_POINTERS* ep) {
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP || !(ep->ContextRecord->Dr6 & 1)) return EXCEPTION_CONTINUE_SEARCH;
    ep->ContextRecord->Dr6 = 0;
    long n = InterlockedIncrement(&g_watch_hits);
    static const long wmax = getenv("TOTK_WATCH_MAX") ? atol(getenv("TOTK_WATCH_MAX")) : 40;   // (TOTK_WATCH_MAX: more hits)
    if (n <= wmax) {
        u64 rip = ep->ContextRecord->Rip, gf = guest_fn_of(rip);
        Ctx* c = t_ctx;
        printf("[watch] #%ld write %08x to %llx by guest fn %s_%llx (host %llx) lr %llx frame %llu tid %lu t %llu\n", n, *(volatile u32*)g_watch_addr,
               (unsigned long long)g_watch_addr, gf >> 40 == 1 ? "s" : "f", gf & 0xFFFFFFFFFFull, rip,
               c ? (unsigned long long)c->x[30] : 0ull, (unsigned long long)renderer_present_count(), GetCurrentThreadId(),
               (unsigned long long)(SDL_GetTicksNS() / 1000));
    }
    return EXCEPTION_CONTINUE_EXECUTION;
}
extern "C" void rt_watch_write(u64 addr) {
    g_watch_addr = addr;
    static bool handler;   // re-arming (TOTK_WATCH) must not stack handlers
    if (!handler) { handler = true; AddVectoredExceptionHandler(1, on_watch); }
    std::thread([addr] {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        THREADENTRY32 te{sizeof te};
        DWORD pid = GetCurrentProcessId(), self = GetCurrentThreadId();
        int n = 0;
        for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
            if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
            HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE, te.th32ThreadID);
            if (!h) continue;
            if (SuspendThread(h) != (DWORD)-1) {
                CONTEXT ctx{};
                ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
                if (GetThreadContext(h, &ctx)) {
                    ctx.Dr0 = addr;
                    ctx.Dr7 = (ctx.Dr7 & ~0xF0003ull) | 1ull | (1ull << 16) | (3ull << 18);  // L0, RW0 = write, LEN0 = 4 bytes
                    if (SetThreadContext(h, &ctx)) n++;
                }
                ResumeThread(h);
            }
            CloseHandle(h);
        }
        CloseHandle(snap);
        printf("[watch] write watch on %llx set in %d threads\n", (unsigned long long)addr, n);
    }).join();
}
#endif

#ifndef _WIN32
// Linux fatal-signal handler (installed by main(); plat_posix.cpp runs it on the faulting thread's alternate stack after the
// write-watch tracker declined the fault): the same report as on_crash below, host registers from the ucontext.
static void on_crash_signal(int sig, siginfo_t* si, void* uc) {
    fflush(stdout);
    ucontext_t* ctx = (ucontext_t*)uc;
    u64 rip = (u64)ctx->uc_mcontext.gregs[REG_RIP];
    u64 gf = guest_fn_of(rip);
    fprintf(stderr, "[crash] signal %d (%s) code %d at host %llx (guest fn %s_%llx) addr %llx  stage %s #%llu\n", sig, strsignal(sig), si->si_code,
            (unsigned long long)rip, gf >> 40 == 1 ? "s" : "f", (unsigned long long)(gf & 0xFFFFFFFFFFull), (unsigned long long)(u64)si->si_addr,
            g_stage, (unsigned long long)g_stage_idx);
    fprintf(stderr, "[crash] exe+%llx (load %llx)  in HLE: %s\n", (unsigned long long)(rip - rt_posix_exe_base()),
            (unsigned long long)rt_posix_exe_base(), t_hle_cur ? t_hle_cur : "(none: generated code)");
    if (sig == SIGSEGV || sig == SIGBUS)   // what the faulting page is
        fprintf(stderr, "[crash] fault address %llx is %sreadable; main heap %llx+%llx\n", (unsigned long long)(u64)si->si_addr,
                rt_posix_readable((u64)si->si_addr, 1) ? "" : "NOT ", (unsigned long long)g_rt_heap_base, (unsigned long long)kernel_heap_size());
    static const char* const rn[] = {"r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15", "rdi", "rsi", "rbp", "rbx", "rdx", "rax", "rcx", "rsp", "rip"};
    for (int i = 0; i < 17; i++) fprintf(stderr, "%-3s=%016llx%s", rn[i], (unsigned long long)ctx->uc_mcontext.gregs[i], i % 4 == 3 ? "\n" : "  ");
    fprintf(stderr, "\n");
    if (Ctx* c = t_ctx) {
        for (int i = 0; i < 31; i++) fprintf(stderr, "x%-2d=%016llx%s", i, (unsigned long long)c->x[i], i % 4 == 3 ? "\n" : "  ");
        fprintf(stderr, "  sp=%016llx tpidr=%016llx\n", (unsigned long long)c->sp, (unsigned long long)c->tpidr);
        u64 fp = c->x[29];      // guest backtrace (x29 frame chain on the guest stack)
        for (int i = 0; i < 24 && fp >= c->sp && fp < c->sp + (256ull << 20) && !(fp & 7) && rt_posix_readable(fp, 16); i++) {
            u64 nfp = *(u64*)fp;
            fprintf(stderr, "[crash]   #%-2d lr=%llx\n", i, (unsigned long long)*(u64*)(fp + 8));
            if (nfp <= fp) break;
            fp = nfp;
        }
    }
    fflush(stderr);
    _exit(3);
}
extern "C" void pc_fault_guard(int) {}   // (pc.h) no __try here
#else
// nxrecomp: with TOTK_PC_CRASH=1 (pc_crash.cpp) the report below is also kept as text and written with a minidump into
// the crash folder before the process ends, a stack overflow goes straight there (printing on the overflowed stack
// could fault again), and a C++ exception is left to whoever catches it (the unhandled filter reports it otherwise).
static char g_crash_text[16 << 10];
static size_t g_crash_len;
static void crash_say(const char* fmt, ...) {
    char b[1024];
    va_list ap; va_start(ap, fmt); int n = vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    if (n <= 0) return;
    if (n > (int)sizeof b - 1) n = (int)sizeof b - 1;
    fwrite(b, 1, (size_t)n, stderr);
    if (g_crash_len + (size_t)n < sizeof g_crash_text) { memcpy(g_crash_text + g_crash_len, b, (size_t)n); g_crash_len += (size_t)n; }
}
static thread_local bool t_prof_unwind;   // (prof_unwind) a fault now belongs to the profiler's __except, not here
static thread_local int t_fault_guard;    // (pc_fault_guard, pc.h) an access violation belongs to the caller's __except
extern "C" void pc_fault_guard(int on) { t_fault_guard += on ? 1 : -1; }
static LONG WINAPI on_crash(EXCEPTION_POINTERS* ep) {
    auto* er = ep->ExceptionRecord;
    if (er->ExceptionCode < 0xC0000000u || t_prof_unwind) return EXCEPTION_CONTINUE_SEARCH;
    if (t_fault_guard > 0 && er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
    bool pc = pc_crash_enabled();
    if (pc && er->ExceptionCode == 0xE06D7363u) return EXCEPTION_CONTINUE_SEARCH;
    if (pc && er->ExceptionCode == EXCEPTION_STACK_OVERFLOW) {
        pc_crash_report(ep, "stack overflow", "(stack overflow: no runtime report; the guest registers are in the minidump)\n");
        TerminateProcess(GetCurrentProcess(), 3);
    }
    if (pc) {   // one report. A fault while formatting it (the frame chain reads guest memory) re-enters on this thread:
                // write what there is. Another crashing thread waits (pc_crash_report would block it anyway).
        static volatile LONG owner;
        LONG me = (LONG)GetCurrentThreadId(), prev = InterlockedCompareExchange(&owner, me, 0);
        if (prev == me) {
            g_crash_text[g_crash_len] = 0;
            pc_crash_report(ep, "crash while reporting a crash", g_crash_text);
            TerminateProcess(GetCurrentProcess(), 3);
        }
        if (prev) Sleep(INFINITE);
    }
    fflush(stdout);
    u64 rip = ep->ContextRecord->Rip;
    u64 gf = guest_fn_of(rip);
    crash_say("[crash] code %08lx at host %llx (guest fn %s_%llx) addr %llx  stage %s #%llu\n", er->ExceptionCode, rip,
            gf >> 40 == 1 ? "s" : "f", gf & 0xFFFFFFFFFFull, er->NumberParameters > 1 ? (u64)er->ExceptionInformation[1] : 0ull,
            g_stage, g_stage_idx);
    crash_say("[crash] exe+%llx  in HLE: %s\n", rip - (u64)GetModuleHandleW(nullptr), t_hle_cur ? t_hle_cur : "(none: generated code)");
    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters > 1) {   // what the faulting page is
        MEMORY_BASIC_INFORMATION mi{};
        u64 fa = (u64)er->ExceptionInformation[1];
        if (VirtualQuery((void*)fa, &mi, sizeof mi))
            crash_say("[crash] fault page: state %lx protect %lx region %llx+%llx alloc base %llx (%s)  main heap %llx+%llx\n",
                    mi.State, mi.Protect, (u64)mi.BaseAddress, (u64)mi.RegionSize, (u64)mi.AllocationBase,
                    er->ExceptionInformation[0] == 1 ? "write" : "read", g_rt_heap_base, kernel_heap_size());
    }
    if (Ctx* c = t_ctx) {
        for (int i = 0; i < 31; i++) crash_say("x%-2d=%016llx%s", i, c->x[i], i % 4 == 3 ? "\n" : "  ");
        crash_say("  sp=%016llx tpidr=%016llx\n", c->sp, c->tpidr);
        u64 fp = c->x[29];      // guest backtrace (x29 frame chain on the guest stack)
        for (int i = 0; i < 24 && fp >= c->sp && fp < c->sp + (256ull << 20) && !(fp & 7); i++) {
            u64 nfp = *(u64*)fp;
            crash_say("[crash]   #%-2d lr=%llx\n", i, *(u64*)(fp + 8));
            if (nfp <= fp) break;
            fp = nfp;
        }
    }
    fflush(stderr);
    if (pc) { g_crash_text[g_crash_len] = 0; pc_crash_report(ep, "crash", g_crash_text); }
    pc_input_keys_restore();   // (TOTK_PC_CRASH=0: no report, which restores them first; a no-op after it)
    TerminateProcess(GetCurrentProcess(), 3);
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

// ---------------------------------------------------------------- guest thread
// New guest CPU context. stack_top == 0: allocate a 32 MB guest stack (main thread); else the guest gave one.
Ctx* rt_new_ctx(u64 stack_top, u64* stack_lo, u64* stack_size) {
    Ctx* c = (Ctx*)_aligned_malloc(sizeof(Ctx), 64); memset(c, 0, sizeof(Ctx));
    const u64 tls_sz = 0x10000;
    if (!stack_top) {
        const u64 stack_sz = 32ull << 20;
        u8* stk = (u8*)VirtualAlloc(nullptr, stack_sz, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        stack_top = (u64)stk + stack_sz;
        if (stack_lo) *stack_lo = (u64)stk;
        if (stack_size) *stack_size = stack_sz;
    }
    u8* tls = (u8*)VirtualAlloc(nullptr, tls_sz, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    c->sp = (stack_top - 0x100) & ~15ull; c->tpidr = (u64)tls; c->excl_addr = ~0ull;
    return c;
}
void hle_evring_dump(u64 focus);
void hle_nvn_dump_top(int top);
// Guest thread registry for the watchdog (TOTK_WATCHDOG=secs): where each thread is when the game stops progressing.
struct GuestThreadRec { Ctx* c; const char* const* hle_cur; DWORD tid; };
static std::mutex g_threads_mtx;
static std::vector<GuestThreadRec> g_threads;
void rt_bind_thread(Ctx* c) {
    t_ctx = c;
#ifdef _WIN32
    if (pc_crash_enabled()) { ULONG g = 64 << 10; SetThreadStackGuarantee(&g); }   // nxrecomp: stack for on_crash on overflow
#endif
    std::lock_guard<std::mutex> l(g_threads_mtx);
    g_threads.push_back({c, &t_hle_cur, GetCurrentThreadId()});
}
// A host-only thread (the NVN render thread) sampled by TOTK_PROF like a guest thread: its samples are charged to
// "hle <name>" and its host RVAs go to hostprof.txt under <name> (tools/hostprof.py --hle <name>). No Ctx.
extern "C" void rt_prof_register_host(const char* name) {
    t_hle_cur = name;
    std::lock_guard<std::mutex> l(g_threads_mtx);
    g_threads.push_back({nullptr, &t_hle_cur, GetCurrentThreadId()});
}
// Called by a guest thread before it exits: its Ctx and thread_local t_hle_cur die with it.
void rt_unbind_thread() {
    std::lock_guard<std::mutex> l(g_threads_mtx);
    DWORD me = GetCurrentThreadId();
    for (size_t i = 0; i < g_threads.size(); i++)
        if (g_threads[i].tid == me) { g_threads.erase(g_threads.begin() + i); break; }
}
// Sampling profiler: suspend each guest thread `samples` times (every `ms` ms), map its host RIP to the generated guest
// function (or the HLE import it is in, or "host" for code outside the exe) and print the top entries per thread and
// overall. Answers "busy or parked, and where" for guest-code threads that have no import to show.
#ifdef _WIN32
static u64 g_prof_t0 = GetTickCount64();   // (profile_threads) process start, for profstacks.txt window times
static std::string module_of(u64 rip) {   // (profile_threads) base name of the module holding rip, "?" if none
    static std::map<u64, std::string> cache;
    HMODULE m = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)rip, &m) || !m)
        return "?";
    auto it = cache.find((u64)m);
    if (it != cache.end()) return it->second;
    char path[MAX_PATH] = "";
    GetModuleFileNameA(m, path, MAX_PATH);
    const char* b = strrchr(path, '\\');
    std::string n = b ? b + 1 : path;
    cache[(u64)m] = n;
    return n;
}
// One unwind step of profile_threads' stack walks. RtlVirtualUnwind reads the sampled thread's stack at addresses the
// unwind data computes (rsp + frame size, rbp-based frames), which can leave the committed stack before the walk's
// bounds check sees the new rsp (a1_gerudo_town, 2026-10-24: a read of the reserved page above a stack ended the
// boot). Such a fault ends the walk here: on_crash lets it through while t_prof_unwind is set. No C++ objects (__try).
static bool prof_unwind(DWORD64 ib, PRUNTIME_FUNCTION f, CONTEXT* u) {
    bool ok = true;
    t_prof_unwind = true;
    __try {
        void* hd = nullptr; DWORD64 ef = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, ib, u->Rip, f, u, &hd, &ef, nullptr);
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        ok = false;
    }
    t_prof_unwind = false;
    return ok;
}
// (profile_threads, stack mode) true when rip is inside a blocking wait's syscall stub (ntdll / win32u): the thread is
// parked in the kernel. Its cycles since the last sample were spent BEFORE it blocked, in code the walk no longer sees,
// so profstacks.txt tags that stack with a "[wait]" leaf and stacks.py keeps those cycles out of the function shares
// (2026-10-30: the render thread's condvar wait took 14 % of its cycles in t3_fight_t6, all work done before it slept).
static bool in_wait_stub(u64 rip) {
    static std::vector<u64> stubs;
    static bool init;
    if (!init) {
        init = true;
        static const char* const nt[] = { "NtWaitForSingleObject", "NtWaitForMultipleObjects", "NtWaitForAlertByThreadId",
            "NtDelayExecution", "NtRemoveIoCompletion", "NtRemoveIoCompletionEx", "NtWaitForWorkViaWorkerFactory",
            "NtSignalAndWaitForSingleObject", "NtWaitForKeyedEvent" };
        static const char* const w32[] = { "NtUserMsgWaitForMultipleObjectsEx", "NtUserWaitMessage",
            "NtGdiDdDDIWaitForSynchronizationObjectFromCpu" };
        if (HMODULE m = GetModuleHandleW(L"ntdll.dll"))
            for (const char* n : nt) if (FARPROC p = GetProcAddress(m, n)) stubs.push_back((u64)p);
        if (HMODULE m = GetModuleHandleW(L"win32u.dll"))
            for (const char* n : w32) if (FARPROC p = GetProcAddress(m, n)) stubs.push_back((u64)p);
    }
    for (u64 s : stubs) if (rip - s < 0x20) return true;   // (a stub: mov r10, rcx; mov eax, n; ...; syscall; ret: 0x20 bytes)
    return false;
}
static void profile_threads(int samples, int ms) {
    static u64 exe_lo, exe_hi;
    if (!exe_lo) {
        exe_lo = (u64)GetModuleHandleW(nullptr);
        auto* nt = (IMAGE_NT_HEADERS*)(exe_lo + ((IMAGE_DOS_HEADER*)exe_lo)->e_lfanew);
        exe_hi = exe_lo + nt->OptionalHeader.SizeOfImage;
    }
    struct T { DWORD tid; HANDLE h; const char* const* hle; std::unordered_map<std::string, int> hits; u64 cpu0; u64 cyc; std::string nm; };
    auto cpu_of = [](HANDLE h) -> u64 { FILETIME a, b, k, u; if (!GetThreadTimes(h, &a, &b, &k, &u)) return 0;
        return ((u64)k.dwHighDateTime << 32 | k.dwLowDateTime) + ((u64)u.dwHighDateTime << 32 | u.dwLowDateTime); };  // 100 ns units
    std::vector<T> ts;
    {
        std::lock_guard<std::mutex> l(g_threads_mtx);
        for (auto& g : g_threads)
            if (HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, g.tid))
                ts.push_back({ g.tid, h, g.hle_cur, {}, cpu_of(h), 0, {} });
    }
    // TOTK_PROF_STACKS=1: also walk each sampled thread's whole host stack and append folded stacks to profstacks.txt as
    // "<thread>;<root frame>;...;<leaf frame> <kcycles>", a frame being an exe RVA "e<hex>" or "<module>+<hex>". Only
    // threads that ran since their last sample count, weighted by the cycles they used (QueryThreadCycleTime), so the
    // totals are CPU time, not wall time. tools/engine/cpuprof/stacks.py resolves the RVAs with totk.exe.map. The frames
    // go to a fixed array while the thread is suspended: it may hold the heap lock, so nothing is allocated until it runs
    // again. TOTK_PROF_STACKS=2 (load times): also wall-time stacks, every thread every sample, parked ones too, to
    // profwall.txt as "<thread>;<frames> <samples>": where a loader waits (a fence, a file, a guest event) and on what.
    static const int stacks_mode = getenv("TOTK_PROF_STACKS") ? atoi(getenv("TOTK_PROF_STACKS")) : 0;
    static const bool stacks = stacks_mode != 0, wall_stacks = stacks_mode == 2;
    std::unordered_map<std::string, u64> folded, wallf;
    if (stacks)
        for (auto& t : ts) {
            QueryThreadCycleTime(t.h, &t.cyc);
            PWSTR wd = nullptr;
            if (SUCCEEDED(GetThreadDescription(t.h, &wd)) && wd) {
                for (int i = 0; wd[i] && i < 63; i++) t.nm += wd[i] == ';' || wd[i] == ' ' ? '_' : (char)wd[i];
                LocalFree(wd);
            }
            if (t.nm.empty()) t.nm = "tid" + std::to_string(t.tid);
        }
    constexpr int kMaxFrames = 96;
    u64 fr[kMaxFrames];
    char buf[64];
    // host code samples inside HLE calls (our own runtime: the NVN backend in nvnQueueSubmitCommands, ...): RVA -> count,
    // appended to hostprof.txt as "<hle> <rva hex> <count>"; tools/hostprof.py resolves the RVAs with totk.exe.map
    std::map<std::pair<std::string, u64>, int> host_hits;
    u64 wall0 = GetTickCount64();
    // Paced by a high-resolution timer and bounded by wall time (samples * ms): Sleep(3) follows the process's timer
    // resolution, and at 15.6 ms a 30 s window took 156 s and never wrote its results before the boot ended
    HANDLE pace = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    const u64 wall_end = wall0 + (u64)samples * ms;
    int taken = 0;
    for (int s = 0; s < samples && GetTickCount64() < wall_end; s++, taken++) {
        for (auto& t : ts) {
            // (stack mode) a thread that has not run since its last sample is parked in a wait: counted without suspending
            // it, so the 40-odd idle guest threads cost the game nothing (and their TLS is not read: one may have exited)
            u64 cyc0 = 0;
            if (stacks && !wall_stacks && QueryThreadCycleTime(t.h, &cyc0) && cyc0 == t.cyc) { t.hits["hle (parked)"]++; continue; }
            if (SuspendThread(t.h) == (DWORD)-1) continue;
            CONTEXT cx{}; cx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
            BOOL ok = GetThreadContext(t.h, &cx);
            const char* hle = *t.hle;
            // host code outside the exe inside an HLE call (GPU driver, CRT, kernel waits): unwind to the first exe frame
            // while the thread is still suspended, so the sample is charged to "<hle>@<module>" at that caller
            u64 via = 0;
            std::string mod;
            if (ok && hle && (cx.Rip < exe_lo || cx.Rip >= exe_hi)) {
                mod = module_of(cx.Rip);
                CONTEXT u = cx;
                for (int d = 0; d < 48; d++) {
                    DWORD64 ib = 0;
                    PRUNTIME_FUNCTION f = RtlLookupFunctionEntry(u.Rip, &ib, nullptr);
                    if (!f) {   // leaf: the return address is at [rsp]
                        MEMORY_BASIC_INFORMATION mi;
                        if (!VirtualQuery((void*)u.Rsp, &mi, sizeof mi) || mi.State != MEM_COMMIT) break;
                        u.Rip = *(u64*)u.Rsp; u.Rsp += 8;
                    } else if (!prof_unwind(ib, f, &u)) break;
                    if (!u.Rip) break;
                    if (u.Rip >= exe_lo && u.Rip < exe_hi) { via = u.Rip - exe_lo; break; }
                }
            }
            int nf = 0;
            u64 cyc = 0, dcyc = 0;
            bool ran = stacks && ok && QueryThreadCycleTime(t.h, &cyc) && cyc != t.cyc;
            if (ran || (wall_stacks && ok)) {
                if (ran) { dcyc = cyc - t.cyc; t.cyc = cyc; }
                MEMORY_BASIC_INFORMATION mi;
                u64 slo = 0, shi = 0;    // the stack's committed region bounds every read from [rsp]
                if (VirtualQuery((void*)cx.Rsp, &mi, sizeof mi) && mi.State == MEM_COMMIT) {
                    slo = (u64)mi.BaseAddress; shi = slo + mi.RegionSize;
                }
                CONTEXT u = cx;
                fr[nf++] = u.Rip;
                for (int d = 0; nf < kMaxFrames; d++) {
                    DWORD64 ib = 0;
                    PRUNTIME_FUNCTION f = RtlLookupFunctionEntry(u.Rip, &ib, nullptr);
                    if (!f) {            // a leaf without unwind data, only plausible for the sampled frame itself
                        if (d || u.Rsp < slo || u.Rsp + 8 > shi) break;
                        u.Rip = *(u64*)u.Rsp; u.Rsp += 8;
                    } else if (!prof_unwind(ib, f, &u)) break;
                    if (!u.Rip || u.Rsp < slo || u.Rsp > shi) break;
                    fr[nf++] = u.Rip;
                }
            }
            ResumeThread(t.h);
            if (nf) {
                std::string k = t.nm;
                if (nf == kMaxFrames) k += ";...";
                for (int i = nf - 1; i >= 0; i--) {
                    if (fr[i] >= exe_lo && fr[i] < exe_hi) snprintf(buf, sizeof buf, ";e%llx", (unsigned long long)(fr[i] - exe_lo));
                    else {
                        HMODULE m = nullptr;
                        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)fr[i], &m) && m)
                            snprintf(buf, sizeof buf, ";%s+%llx", module_of(fr[i]).c_str(), (unsigned long long)(fr[i] - (u64)m));
                        else snprintf(buf, sizeof buf, ";?%llx", (unsigned long long)fr[i]);
                    }
                    k += buf;
                }
                if (in_wait_stub(fr[0])) k += ";[wait]";
                if (ran) folded[k] += dcyc / 1000;
                if (wall_stacks) wallf[k]++;
            }
            if (!ok) continue;
            if (hle) {
                t.hits[std::string("hle ") + hle]++;
                if (cx.Rip >= exe_lo && cx.Rip < exe_hi) host_hits[{ hle, cx.Rip - exe_lo }]++;   // (host code inside an HLE call)
                else host_hits[{ std::string(hle) + "@" + mod, via }]++;                          // (outside: charged to its exe caller)
            } else if (cx.Rip < exe_lo || cx.Rip >= exe_hi) t.hits["host (outside exe)"]++;
            else {
                u64 gf = guest_fn_of(cx.Rip);
                snprintf(buf, sizeof buf, "%s_%llx", gf >> 40 == 1 ? "s" : "f", gf & 0xFFFFFFFFFFull);
                t.hits[buf]++;
                // also by exact RVA (hostprof.py + the map): guest_fn_of charges runtime code linked after the last guest
                // function to that function
                host_hits[{ "guest", cx.Rip - exe_lo }]++;
            }
        }
        LARGE_INTEGER due; due.QuadPart = -(LONGLONG)ms * 10000;
        if (pace && SetWaitableTimer(pace, &due, 0, nullptr, nullptr, FALSE)) WaitForSingleObject(pace, INFINITE);
        else Sleep(ms);
    }
    if (pace) CloseHandle(pace);
    samples = taken ? taken : 1;   // (the shares below are of the samples actually taken)
    u64 wall = GetTickCount64() - wall0; if (!wall) wall = 1;
    std::unordered_map<std::string, int> all;
    std::sort(ts.begin(), ts.end(), [&](const T& a, const T& b) { return cpu_of(a.h) - a.cpu0 > cpu_of(b.h) - b.cpu0; });
    u64 cpu_all = 0;
    for (auto& t : ts) {
        u64 cpu_ms = (cpu_of(t.h) - t.cpu0) / 10000; cpu_all += cpu_ms;
        std::vector<std::pair<int, std::string>> v;
        for (auto& [k, n] : t.hits) { v.push_back({ n, k }); if (k.rfind("hle ", 0) && k.rfind("host", 0)) all[k] += n; }
        std::sort(v.rbegin(), v.rend());
        bool busy = (!v.empty() && v[0].second.rfind("hle ", 0) != 0) || cpu_ms * 100 >= wall * 3;  // parked threads sit in one HLE wait
        if (busy) {
            char nm[64] = "";
            PWSTR wd = nullptr;
            if (SUCCEEDED(GetThreadDescription(t.h, &wd)) && wd) {
                int i = 0; for (; wd[i] && i < 63; i++) nm[i] = (char)wd[i]; nm[i] = 0;
                LocalFree(wd);
            }
            printf("[prof] tid %5lu %-24s cpu %3llu%%:", t.tid, nm, cpu_ms * 100 / wall);
            for (size_t i = 0; i < v.size() && i < 4; i++) printf(" %s %d%%", v[i].second.c_str(), v[i].first * 100 / samples);
            printf("\n");
        }
        CloseHandle(t.h);
    }
    std::vector<std::pair<int, std::string>> v;
    for (auto& [k, n] : all) v.push_back({ n, k });
    std::sort(v.rbegin(), v.rend());
    if (FILE* hf = fopen("hostprof.txt", "a")) {
        fprintf(hf, "# sampling %d x %d ms\n", samples, ms);
        for (auto& [k, n] : host_hits) fprintf(hf, "%s %llx %d\n", k.first.c_str(), (unsigned long long)k.second, n);
        fclose(hf);
    }
    if (stacks)
        if (FILE* sf = fopen("profstacks.txt", "a")) {
            fprintf(sf, "# window at %llu ms after start, %d samples every %d ms, %llu ms wall\n",
                    (unsigned long long)(GetTickCount64() - g_prof_t0), samples, ms, (unsigned long long)wall);
            for (auto& [k, n] : folded) if (n) fprintf(sf, "%s %llu\n", k.c_str(), (unsigned long long)n);
            fclose(sf);
        }
    if (wall_stacks)
        if (FILE* sf = fopen("profwall.txt", "a")) {
            fprintf(sf, "# window at %llu ms after start, %d samples every %d ms, %llu ms wall\n",
                    (unsigned long long)(GetTickCount64() - g_prof_t0), samples, ms, (unsigned long long)wall);
            for (auto& [k, n] : wallf) fprintf(sf, "%s %llu\n", k.c_str(), (unsigned long long)n);
            fclose(sf);
        }
    printf("[prof] guest threads cpu total %llu%% of one core over %llu ms\n", cpu_all * 100 / wall, wall);
    printf("[prof] guest code overall (thread-samples of %d):", samples);
    for (size_t i = 0; i < v.size() && i < 15; i++) printf(" %s %d", v[i].second.c_str(), v[i].first);
    printf("\n");
}
// TOTK_HITCH_STACKS=1: the stacks of a stall itself. A 30 s TOTK_PROF window rarely holds one of the warm game-side hitches
// (present's warm Kakariko: 54-81 ms frames, "other" 44-61 ms, no compiles), so this thread keeps a ring of the last ~0.5 s of
// wall samples, every 4 ms, of every guest thread and the render thread (parked ones too: a stall is often a wait), and when
// vk_prof_frame logs a [hitch] frame (LARECOMP_HITCH_MS, default 50) it appends that frame's span (its length + 100 ms before)
// to hitchstacks.txt: "# window hitch..." then, per thread, its CPU (Mcycles) in the span and "<thread>;<root>;...;<leaf>
// <samples>" (profwall.txt's format; tools/engine/cpuprof/hitchstacks.py names the frames). The frames go to a fixed ring
// while the thread is suspended: nothing is allocated then.
static std::atomic<uint64_t> g_hitch_req;   // (frame ms x 10) << 32 | (other ms x 10); 0 = none pending
static std::atomic<uint64_t> g_hitch_frame;
static bool g_hitch_on;
extern "C" void rt_hitch_dump(uint64_t frame, double ms, double other_ms) {
    if (!g_hitch_on) return;
    uint64_t v = (uint64_t)(ms * 10) << 32 | (uint32_t)(other_ms > 0 ? other_ms * 10 : 0), z = 0;
    if (!v) v = 1;
    if (g_hitch_req.compare_exchange_strong(z, v)) g_hitch_frame = frame;   // (one at a time: a burst keeps the first)
}
// (hitch_thread) unwind data found without the loader: RtlLookupFunctionEntry takes the inverted function table's lock
// shared, and a thread suspended inside a DLL load / unload holds it exclusive, so a sampler calling it on a suspended
// target can hang the game. The modules'.pdata tables are read here, outside any suspension (refreshed every ~2 s), and
// searched without a lock; a frame outside them ends the walk. Each module in the table holds a reference
// (GetModuleHandleExW without UNCHANGED_REFCOUNT, released at the next refresh), so none can unload under a lookup and
// leave its.pdata pointer dangling.
struct HitchMod { u64 lo, hi; const RUNTIME_FUNCTION* rf; uint32_t n; HMODULE ref; };
static void hitch_modules(std::vector<HitchMod>& mods) {
    for (const HitchMod& m : mods) FreeLibrary(m.ref);
    mods.clear();
    HMODULE hm[512];
    DWORD need = 0;
    if (!K32EnumProcessModules(GetCurrentProcess(), hm, sizeof hm, &need)) return;
    for (DWORD i = 0; i < need / sizeof(HMODULE) && i < 512; i++) {
        HMODULE ref = nullptr;   // (the reference that keeps it loaded while it is in the table)
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, (LPCWSTR)hm[i], &ref) || !ref) continue;
        const u64 b = (u64)ref;
        auto* nt = (IMAGE_NT_HEADERS*)(b + ((IMAGE_DOS_HEADER*)b)->e_lfanew);
        const IMAGE_DATA_DIRECTORY& d = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
        if (!d.VirtualAddress || d.Size < sizeof(RUNTIME_FUNCTION)) { FreeLibrary(ref); continue; }
        mods.push_back({ b, b + nt->OptionalHeader.SizeOfImage, (const RUNTIME_FUNCTION*)(b + d.VirtualAddress),
                         (uint32_t)(d.Size / sizeof(RUNTIME_FUNCTION)), ref });
    }
    std::sort(mods.begin(), mods.end(), [](const HitchMod& a, const HitchMod& b) { return a.lo < b.lo; });
}
static PRUNTIME_FUNCTION hitch_lookup(const std::vector<HitchMod>& mods, u64 pc, u64* base) {
    size_t lo = 0, hi = mods.size();
    while (lo < hi) { size_t m = (lo + hi) / 2; if (mods[m].lo <= pc) lo = m + 1; else hi = m; }
    if (!lo || pc >= mods[lo - 1].hi) return nullptr;
    const HitchMod& md = mods[lo - 1];
    const uint32_t rva = (uint32_t)(pc - md.lo);
    uint32_t a = 0, b = md.n;
    while (a < b) {   // (sorted by BeginAddress)
        uint32_t m = (a + b) / 2;
        if (md.rf[m].EndAddress <= rva) a = m + 1; else b = m;
    }
    if (a >= md.n || md.rf[a].BeginAddress > rva) return nullptr;
    const RUNTIME_FUNCTION* f = &md.rf[a];
    if (f->UnwindData & 1) f = (const RUNTIME_FUNCTION*)(md.lo + (f->UnwindData & ~1u));   // (an indirect entry)
    *base = md.lo;
    return (PRUNTIME_FUNCTION)f;
}
static void hitch_thread() {
    constexpr int kSlots = 128, kThr = 96, kFr = 40, kEvery = 4, kMaxDumps = 120, kPer10s = 3;
    // fr / nf: the stack; reused = the thread had not run since the previous sample, so that sample's stack is copied
    // and the thread is not suspended (most guest threads are parked: the pass suspends only the running ones)
    struct Smp { u64 t; int n; float pass_us; DWORD tid[kThr]; u64 cyc[kThr]; uint8_t nf[kThr]; u64 fr[kThr][kFr]; };
    static Smp ring[kSlots];   // ~4 MB
    static u64 exe_lo, exe_hi;
    exe_lo = (u64)GetModuleHandleW(nullptr);
    exe_hi = exe_lo + ((IMAGE_NT_HEADERS*)(exe_lo + ((IMAGE_DOS_HEADER*)exe_lo)->e_lfanew))->OptionalHeader.SizeOfImage;
    LARGE_INTEGER qf, q0;
    QueryPerformanceFrequency(&qf);
    QueryPerformanceCounter(&q0);
    auto now_us = [&]() { LARGE_INTEGER q; QueryPerformanceCounter(&q); return (u64)((q.QuadPart - q0.QuadPart) * 1000000 / qf.QuadPart); };
    const u64 start_ms = GetTickCount64() - g_prof_t0;   // (header times: ms after start, as profstacks.txt)
    struct Th { HANDLE h; std::string nm; int last = -1; };   // last: its index in the previous sample
    std::unordered_map<DWORD, Th> th;
    std::vector<DWORD> tids;
    std::vector<HitchMod> mods;
    u64 mods_t = 0;
    HANDLE pace = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    SetThreadDescription(GetCurrentThread(), L"hitch-sampler");
    u64 head = 0;
    int dumps = 0;
    u64 dump_t[kPer10s] = {};   // (the last kPer10s dumps' times, us)
    char buf[64];
    printf("[hitch] stacks on: every %d ms of every guest thread, %d ms kept, dumped to hitchstacks.txt\n", kEvery, kSlots * kEvery);
    for (;;) {
        const u64 t0 = now_us();
        if (!mods_t || t0 - mods_t > 2000000) { hitch_modules(mods); mods_t = t0; }   // (DLLs loaded since)
        tids.clear();
        {
            std::lock_guard<std::mutex> l(g_threads_mtx);
            for (auto& g : g_threads) tids.push_back(g.tid);
        }
        for (DWORD tid : tids)
            if (!th.count(tid))
                if (HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid)) {
                    std::string nm;
                    PWSTR wd = nullptr;
                    if (SUCCEEDED(GetThreadDescription(h, &wd)) && wd) {
                        for (int i = 0; wd[i] && i < 63; i++) nm += wd[i] == ';' || wd[i] == ' ' ? '_' : (char)wd[i];
                        LocalFree(wd);
                    }
                    th[tid] = { h, nm.empty() ? "tid" + std::to_string(tid) : nm };
                }
        Smp& s = ring[head % kSlots];
        const Smp* prev = head ? &ring[(head - 1) % kSlots] : nullptr;
        s.n = 0;
        s.t = t0;
        for (DWORD tid : tids) {
            auto it = th.find(tid);
            if (it == th.end() || s.n >= kThr) continue;
            HANDLE h = it->second.h;
            const int k = s.n;
            s.tid[k] = tid;
            s.nf[k] = 0;
            QueryThreadCycleTime(h, &s.cyc[k]);
            const int pk = it->second.last;
            if (prev && pk >= 0 && pk < prev->n && prev->tid[pk] == tid && prev->cyc[pk] == s.cyc[k]) {   // (parked since)
                s.nf[k] = prev->nf[pk];
                memcpy(s.fr[k], prev->fr[pk], sizeof(u64) * prev->nf[pk]);
            } else if (SuspendThread(h) != (DWORD)-1) {
                CONTEXT cx{}; cx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
                if (GetThreadContext(h, &cx)) {
                    MEMORY_BASIC_INFORMATION mi;
                    u64 slo = 0, shi = 0;
                    if (VirtualQuery((void*)cx.Rsp, &mi, sizeof mi) && mi.State == MEM_COMMIT) { slo = (u64)mi.BaseAddress; shi = slo + mi.RegionSize; }
                    CONTEXT u = cx;
                    int nf = 0;
                    s.fr[k][nf++] = u.Rip;
                    for (int d = 0; nf < kFr; d++) {   // (as profile_threads' walk, with the lock-free lookup)
                        u64 ib = 0;
                        PRUNTIME_FUNCTION f = hitch_lookup(mods, u.Rip, &ib);
                        if (!f) {   // (a leaf without unwind data, or a module the table does not have: the walk ends)
                            if (d || u.Rsp < slo || u.Rsp + 8 > shi) break;
                            u.Rip = *(u64*)u.Rsp; u.Rsp += 8;
                        } else if (!prof_unwind(ib, f, &u)) break;
                        if (!u.Rip || u.Rsp < slo || u.Rsp > shi) break;
                        s.fr[k][nf++] = u.Rip;
                    }
                    s.nf[k] = (uint8_t)nf;
                }
                ResumeThread(h);
            } else continue;
            it->second.last = k;
            s.n++;
        }
        s.pass_us = (float)(now_us() - t0);
        head++;
        // (at most kPer10s dumps in any 10 s and kMaxDumps a run: ft_warm's start-up frames, title + compiles, used up the
        // first 40 before the world, so the arrival's frames had none)
        if (uint64_t req = g_hitch_req.load(); req && dumps < kMaxDumps && (dumps < kPer10s || t0 - dump_t[dumps % kPer10s] > 10000000)) {
            const double fms = (req >> 32) / 10.0, oms = (uint32_t)req / 10.0;
            const u64 now = now_us(), from = now - (u64)(fms + 100) * 1000;
            std::map<std::string, u64> folded;
            std::map<DWORD, std::pair<u64, u64>> cyc;   // first / last cycle count in the span
            std::vector<float> pass;
            for (u64 i = head > kSlots ? head - kSlots : 0; i < head; i++) {
                const Smp& r = ring[i % kSlots];
                if (r.t < from) continue;
                pass.push_back(r.pass_us);
                for (int k = 0; k < r.n; k++) {
                    auto c = cyc.find(r.tid[k]);
                    if (c == cyc.end()) cyc[r.tid[k]] = { r.cyc[k], r.cyc[k] }; else c->second.second = r.cyc[k];
                    auto t = th.find(r.tid[k]);
                    if (!r.nf[k] || t == th.end()) continue;
                    std::string key = t->second.nm;
                    for (int j = r.nf[k] - 1; j >= 0; j--) {
                        const u64 a = r.fr[k][j];
                        HMODULE m = nullptr;
                        if (a >= exe_lo && a < exe_hi) snprintf(buf, sizeof buf, ";e%llx", (unsigned long long)(a - exe_lo));
                        else if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)a, &m) && m)
                            snprintf(buf, sizeof buf, ";%s+%llx", module_of(a).c_str(), (unsigned long long)(a - (u64)m));
                        else snprintf(buf, sizeof buf, ";?%llx", (unsigned long long)a);
                        key += buf;
                    }
                    folded[key]++;
                }
            }
            std::sort(pass.begin(), pass.end());
            const float pmed = pass.empty() ? 0 : pass[pass.size() / 2], pmax = pass.empty() ? 0 : pass.back();
            if (FILE* f = fopen("hitchstacks.txt", "a")) {
                fprintf(f, "# window hitch %d: frame %llu %.1f ms (other %.1f ms) at %llu ms after start, %zu samples every %d ms, "
                        "%llu ms wall; sampler pass median %.0f us, max %.0f us\n",
                        dumps + 1, (unsigned long long)g_hitch_frame.load(), fms, oms, (unsigned long long)(start_ms + now / 1000),
                        pass.size(), kEvery, (unsigned long long)((now - from) / 1000), pmed, pmax);
                for (auto& [tid, c] : cyc)   // (a thread's CPU in the span, Mcycles: hitchstacks.py turns them into ms)
                    if (c.second > c.first) {
                        auto t = th.find(tid);
                        fprintf(f, "# busy %s %.2f Mcycles\n", t != th.end() ? t->second.nm.c_str() : "?", (c.second - c.first) / 1e6);
                    }
                for (auto& [k, n] : folded) fprintf(f, "%s %llu\n", k.c_str(), (unsigned long long)n);
                fclose(f);
            }
            printf("[hitch] stacks of frame %llu (%.0f ms, other %.0f ms): %zu samples to hitchstacks.txt (pass median %.0f us, max %.0f us)\n",
                   (unsigned long long)g_hitch_frame.load(), fms, oms, pass.size(), pmed, pmax);
            dump_t[dumps % kPer10s] = t0;
            dumps++;
            g_hitch_req = 0;
        } else if (req) g_hitch_req = 0;
        for (auto it = th.begin(); it != th.end();)   // (threads gone from g_threads)
            if (std::find(tids.begin(), tids.end(), it->first) == tids.end()) { CloseHandle(it->second.h); it = th.erase(it); }
            else ++it;
        LARGE_INTEGER due; due.QuadPart = -(LONGLONG)kEvery * 10000;
        if (pace && SetWaitableTimer(pace, &due, 0, nullptr, nullptr, FALSE)) WaitForSingleObject(pace, INFINITE);
        else Sleep(kEvery);
    }
}
#else
extern "C" void rt_hitch_dump(uint64_t, double, double) {}
// Linux: TOTK_PROF / TOTK_WATCHDOG sampling needs SuspendThread + GetThreadContext (and a PE module list): not available. The
// watchdog still lists every guest thread's HLE position and guest backtrace below; for host profiles use `perf record -g`.
static void profile_threads(int samples, int ms) {
    static bool told;
    if (!told) { told = true; printf("[prof] host-code sampling (TOTK_PROF) is Windows-only; use perf on Linux\n"); }
    (void)samples; (void)ms;
}
#endif
static void watchdog_dump() {
    profile_threads(100, 20);
#ifdef _WIN32
    auto readable = [](u64 a) {
        MEMORY_BASIC_INFORMATION mi;
        return a && !(a & 7) && VirtualQuery((void*)a, &mi, sizeof mi) && mi.State == MEM_COMMIT &&
               !(mi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) && a + 16 <= (u64)mi.BaseAddress + mi.RegionSize;
    };
#else
    auto readable = [](u64 a) { return a && !(a & 7) && rt_posix_readable(a, 16) != 0; };
#endif
    std::lock_guard<std::mutex> l(g_threads_mtx);
    printf("[watchdog] %zu guest threads\n", g_threads.size());
    for (auto& t : g_threads) {
        if (!t.c) continue;   // (host-only thread: rt_prof_register_host)
        const char* cur = *t.hle_cur;
        printf("[watchdog] tid %5lu in %s lr=%llx sp=%llx bt:", t.tid, cur ? cur : "(guest code)", t.c->x[30], t.c->sp);
        u64 fp = t.c->x[29];
        for (int i = 0; i < 10 && readable(fp) && fp >= t.c->sp; i++) {
            u64 nfp = *(u64*)fp, lr = *(u64*)(fp + 8);
            printf(" %llx", lr);
            if (nfp <= fp) break;
            fp = nfp;
        }
        printf("\n[watchdog]   x19-x28:");
        for (int r = 19; r <= 28; r++) printf(" %llx", t.c->x[r]);
        printf("\n");
        // TEMP (job-system hang): in the waiter f_813358, x21 = pool W: pending [W+0xc0], flags [W+0xbc], list W+0x40
        if (cur && strstr(cur, "WaitLightEvent") && readable(t.c->x[21]) && readable(t.c->x[21] + 0xc8)) {
            const u64* w = (const u64*)t.c->x[21];
            for (int i = 0; i < 26; i += 4)
                printf("[watchdog]   W+%02x: %llx %llx %llx %llx\n", i * 8, w[i], w[i + 1], w[i + 2], w[i + 3]);
            u64 ev = t.c->x[0], s = ev - 0xb0;     // waiter record S: claim [S+8], flags [S+0x10], event S+0xb0, id [S+0xd8]
            if (readable(s) && readable(s + 0xd8)) {
                const u64* p = (const u64*)s;
                printf("[watchdog]   S=%llx: +08 %llx +10 %llx +d8 %llx\n", s, p[1], p[2], p[0xd8 / 8]);
            }
            hle_evring_dump(ev);
        }
    }
    hle_nvn_dump_top(25);
    fflush(stdout);
}
static_assert(offsetof(Ctx, sp) == 256 && offsetof(Ctx, v) == 264, "runtime/nvn/glue.h Cpu mirrors this layout");
// Host -> guest call (NVN memory callbacks etc.): run the guest function at addr on the current context.
extern "C" void guest_call(Ctx* c, u64 addr) { u64 lr = c->x[30]; call_ind(c, addr); c->x[30] = lr; }
void os_register_main_thread(Ctx* c, u64 stack_lo, u64 stack_size);


static DWORD WINAPI boot_thread(LPVOID) {
    u64 slo = 0, ssz = 0;
    Ctx* c = rt_new_ctx(0, &slo, &ssz); rt_bind_thread(c);
    os_register_main_thread(c, slo, ssz);
    g_stage = "DT_INIT";
    u64 init = MAIN_BASE + g_main.dyn[12];
    printf("[boot] DT_INIT %llx\n", init); fflush(stdout);
    guest_call(c, init);
    auto e = g_main.exports.find("nninitStartup");
    if (e == g_main.exports.end()) die("no nninitStartup export");
    printf("[boot] nninitStartup %llx\n", e->second); fflush(stdout);
    g_stage = "nninitStartup"; guest_call(c, e->second);
    g_stage = "init_array";
    u64 ia = MAIN_BASE + g_main.dyn[25], ian = g_main.dyn[27] / 8;
    printf("[boot] init_array %llu entries\n", ian); fflush(stdout);
    for (u64 i = 0; i < ian; i++) {
        u64 fn = ((u64*)ia)[i];
        if (fn == 0 || fn == ~0ull) continue;
        c->x[0] = 0; c->x[1] = 0; c->x[2] = 0; g_stage_idx = i;
        guest_call(c, fn);
        if ((i + 1) % 1000 == 0) { printf("[boot] init_array %llu/%llu\n", i + 1, ian); fflush(stdout); }
    }
    e = g_main.exports.find("nnMain");
    printf("[boot] nnMain entered (%llx)\n", e->second); fflush(stdout); g_stage = "nnMain";
    guest_call(c, e->second);
    printf("[boot] nnMain returned x0=%llx\n", c->x[0]);
    return 0;
}

extern "C" int ui_run(void);
extern "C" uint64_t nvn_present_count(void);
extern "C" uint64_t vk_exec_passes_total(void);                 // nvn/vulkan/exec.c
extern "C" void hle_fs_read_stats(unsigned long long out[4]);   // hle_fs.cpp: bytes, reads, opens, ns in reads
// Seconds since the process was created (Windows: its creation time, so loading the exe image counts; elsewhere: since
// the first call, which main makes first thing). TOTK_LOADTRACE's clock.
static double rt_uptime_s() {
    static const auto t0 = std::chrono::steady_clock::now();
    static const double before = [] {
#ifdef _WIN32
        FILETIME c, e, k, u, now;
        GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u); GetSystemTimePreciseAsFileTime(&now);
        auto q = [](FILETIME f) { return (u64)f.dwHighDateTime << 32 | f.dwLowDateTime; };
        return (double)(q(now) - q(c)) / 1e7;
#else
        return 0.0;
#endif
    }();
    return before + std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}
static void print_stats() {
    fflush(stdout);
    std::vector<std::pair<u64, int>> top;
    for (u32 i = 0; i < g_imp_count; i++) if (g_imp_calls[i]) top.push_back({ g_imp_calls[i].load(), (int)i });
    std::sort(top.rbegin(), top.rend());
    printf("[stats] %zu distinct imports called%s\n", top.size(), g_imp_count_all ? "" : " (counts capped at 3; TOTK_IMP_STATS=1 counts all)");
    for (auto& [n, i] : top) printf("  %10llu %s%s\n", n, g_hle[i] ? "" : "UNIMPL ", g_imp_names[i]);
}

int main(int argc, char** argv) {
    rt_uptime_s();
    setvbuf(stdout, nullptr, _IOFBF, 1 << 16);
    pc_crash_install();   // nxrecomp: TOTK_PC_CRASH=1 crash reports + minidumps, from here on (on_crash comes later)
    // Idle priority class by default: test runs shared the PC with the user's video playback and made its audio crackle
    // (user 2026-10-14). TOTK_PRIORITY=normal opts out (e.g. for clean perf measurements) and sets the normal class
    // outright: a child inherits BELOW_NORMAL / IDLE from its parent, so "not lowered" still meant a game at the builds'
    // priority, starved to 3-10 fps whenever they filled the CPU.
#ifdef _WIN32
    { const char* pr = getenv("TOTK_PRIORITY");
      const bool normal = pr && !strcmp(pr, "normal");
      SetPriorityClass(GetCurrentProcess(), normal ? NORMAL_PRIORITY_CLASS : IDLE_PRIORITY_CLASS);
      // The player's game (normal class: the launcher, boot.py): opt out of power throttling (EcoQoS), so Windows 11 never
      // runs it on a hybrid CPU's efficiency cores or at a lowered clock when its window is behind another or a second
      // monitor has the focus, and keeps a TOTK_TIMER_1MS resolution then too. No effect on CPUs without E-cores or on
      // Windows 10 before 1709 (the call fails there; logged). Kernel32 by name: SetProcessInformation is Win8+.
      if (normal)
          if (auto spi = (BOOL(WINAPI*)(HANDLE, int, void*, DWORD))GetProcAddress(GetModuleHandleA("kernel32.dll"),
                                                                                  "SetProcessInformation")) {
              struct { ULONG ver, control, state; } st = {1, 0x1 | 0x4, 0};   // EXECUTION_SPEED | IGNORE_TIMER_RESOLUTION, off
              BOOL ok = spi(GetCurrentProcess(), 4 /*ProcessPowerThrottling*/, &st, sizeof st);
              if (!ok) { st.control = 0x1; ok = spi(GetCurrentProcess(), 4, &st, sizeof st); }   // (Windows 10: no timer flag, error 87)
              printf("[os] power throttling off: %s\n", ok ? (st.control & 0x4 ? "speed + timer" : "speed") : "unsupported");
          } }
#else
    // Linux: normal priority by default (this is the game on the Deck); TOTK_PRIORITY=idle = nice 19 for background test runs
    { const char* pr = getenv("TOTK_PRIORITY"); if (pr && strcmp(pr, "idle") == 0) (void)!setpriority(PRIO_PROCESS, 0, 19); }
    rt_posix_install_signals(on_crash_signal);
    signal(SIGPIPE, SIG_IGN);   // a log piped into a closed reader must not end the game
#endif
    // Where the threads run (cpu_place.h): before the first thread, so every thread created from here is hot by default
    // (the guest's, render, present, audio) and the pools tag themselves; a no-op on one kind of core and one L3.
    cpu_place_init();
    hle_fs_index_romfs_early();   // (the RomFS pack / image index, on its own thread)
    // flush once a second: a run killed by `timeout` otherwise loses its last 64 KiB of log
    CloseHandle(CreateThread(nullptr, 0, [](LPVOID) -> DWORD { for (;;) { Sleep(1000); fflush(stdout); fflush(stderr); } }, nullptr, 0, nullptr));
#ifdef _WIN32
    std::string dir = argc > 1 ? argv[1] : getenv("TOTK_IMG") && *getenv("TOTK_IMG") ? getenv("TOTK_IMG") : "C:/totk/extract/img";
#else
    // the decompressed NSO images (tools/nso.py): argv[1], else $TOTK_IMG, else <exe dir>/data/img
    std::string dir = argc > 1 ? argv[1] : getenv("TOTK_IMG") && *getenv("TOTK_IMG") ? getenv("TOTK_IMG") : std::string(rt_posix_exe_dir()) + "/data/img";
#endif
    if (g_imp_count > (u64)kMaxImports) die("too many imports %llu", g_imp_count);
    for (u32 i = 0; i < g_imp_count; i++) g_imp_index[g_imp_names[i]] = i;

    load_module(g_main, dir + "/main.bin");
    load_module(g_sdk, dir + "/sdk.bin");
    relocate(g_sdk);
    relocate(g_main);
    totk_engine_boot(MAIN_BASE);  // TotK game patches' data page; inert unpatched

    // function table: every FDE start, plus the import fake addresses
    g_ftab_n = (IMP_FAKE - MAIN_BASE) / 4 + kMaxImports + kMaxHostFns;
    g_ftab = (GuestFn*)VirtualAlloc(nullptr, g_ftab_n * sizeof(GuestFn), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    for (u64 i = 0; i < g_ftab_count; i++) g_ftab[g_ftab_init[i].off >> 2] = g_ftab_init[i].fn;
    // sdk function table (sdk .text only)
    g_sftab_n = 0;
    for (u64 i = 0; i < g_s_ftab_count; i++) if ((g_s_ftab_init[i].off >> 2) + 1 > g_sftab_n) g_sftab_n = (g_s_ftab_init[i].off >> 2) + 1;
    g_sftab = (GuestFn*)VirtualAlloc(nullptr, g_sftab_n * sizeof(GuestFn), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    for (u64 i = 0; i < g_s_ftab_count; i++) g_sftab[g_s_ftab_init[i].off >> 2] = g_s_ftab_init[i].fn;
    auto lifted = [](const std::string& name) -> GuestFn {     // sdk export -> its lifted function
        auto e = g_sdk.exports.find(name);
        if (e == g_sdk.exports.end() || e->second < SDK_BASE) return nullptr;
        u64 i = (e->second - SDK_BASE) >> 2;
        return i < g_sftab_n ? g_sftab[i] : nullptr;
    };
    // main imports in the lift whitelist (namespace prefixes; TOTK_LIFT overrides, comma-separated) run lifted sdk code
    // instead of HLE: big pure libraries the game uses through many calls (font engine, codecs). "!prefix" excludes
    // (those names keep their HLE): prepo's msgpack builder is lifted, its Save* (a service call) stays HLE.
    std::vector<std::string> lift_prefix, lift_excl;
    {
        std::string s = getenv("TOTK_LIFT") ? getenv("TOTK_LIFT")
                      : "_ZN2nn6fontll,_ZNK2nn6fontll,_ZN2nn4util,_ZNK2nn4util,_ZN2nn5image,_ZNK2nn5image,"
                        "_ZN2nn5prepo,_ZNK2nn5prepo,!_ZN2nn5prepo10PlayReport4Save,!_ZN2nn5prepo28RequestImmediateTransmission";
        for (size_t p = 0, q; p < s.size(); p = q + 1) {
            q = s.find(',', p); if (q == std::string::npos) q = s.size();
            if (q > p) { std::string it = s.substr(p, q - p); if (it[0] == '!') lift_excl.push_back(it.substr(1)); else lift_prefix.push_back(it); }
        }
    }
    auto in_lift = [&](const std::string& nm) {
        for (auto& p : lift_excl) if (nm.compare(0, p.size(), p) == 0) return false;
        for (auto& p : lift_prefix) if (nm.compare(0, p.size(), p) == 0) return true;
        return false;
    };
    int nhle = 0, nlift = 0;
    for (u32 i = 0; i < g_imp_count; i++) {
        std::string nm = g_imp_names[i];
        GuestFn lf = nullptr;
        if (in_lift(nm)) lf = lifted(nm);
        auto it = hle_table().find(nm);
        if (lf) {
            g_hle[i] = lf; nlift++;
            auto w = hle_wraps().find(nm);   // HLE_WRAP: the wrapper in its place, the lifted function its original
            if (w != hle_wraps().end()) { *w->second.second = lf; g_hle[i] = w->second.first; }
        }
        else if (it != hle_table().end()) { g_hle[i] = it->second; nhle++; }
        g_imp[i] = k_thunks[i];
        g_ftab[(IMP_FAKE - MAIN_BASE) / 4 + i] = k_thunks[i];
    }
    printf("[ld] ftab %llu functions, %llu imports (%d HLE, %d lifted sdk, %llu stubbed)\n", g_ftab_count, g_imp_count, nhle,
           nlift, g_imp_count - nhle - nlift);
    // sdk PLT slots: our HLE (same names main uses), else main's own export (rtld resolves main first), else the sdk's
    // lifted definition
    if (g_s_imp_count > kMaxSdkImports) die("too many sdk imports %llu", g_s_imp_count);
    int sh = 0, sm = 0, ss = 0, sx = 0;
    for (u32 i = 0; i < g_s_imp_count; i++) {
        std::string nm = g_s_imp_names[i];
        GuestFn f = nullptr;
        bool white = false;
        white = in_lift(nm);
        auto it = hle_table().find(nm);
        auto me = g_main.exports.find(nm);
        if (!white && it != hle_table().end()) { f = it->second; sh++; }
        else if (me != g_main.exports.end() && ((me->second - MAIN_BASE) >> 2) < g_ftab_n && g_ftab[(me->second - MAIN_BASE) >> 2]) {
            f = g_ftab[(me->second - MAIN_BASE) >> 2]; sm++;
        } else if ((f = lifted(nm))) ss++;
        else { f = sdk_imp_missing; sx++; printf("[ld] sdk unbound: %s\n", nm.c_str()); }
        g_s_imp[i] = f;
    }
    printf("[ld] sdk: %llu lifted functions; %llu PLT slots -> %d HLE, %d main, %d lifted, %d unbound\n", g_s_ftab_count,
           g_s_imp_count, sh, sm, ss, sx);
    fflush(stdout);

#ifdef _WIN32
    AddVectoredExceptionHandler(0, on_crash);
    // nxrecomp: TOTK_PC_CRASHTEST=<s> writes to address 0 from a host thread after <s> seconds, =abort:<s> calls abort():
    // checks the crash report and minidump path (TOTK_PC_CRASH=1) without waiting for a real crash
    if (const char* ct = getenv("TOTK_PC_CRASHTEST")) {
        static bool ct_abort = strncmp(ct, "abort:", 6) == 0;
        static DWORD ct_ms = (DWORD)(atof(ct_abort ? ct + 6 : ct) * 1000);
        CloseHandle(CreateThread(nullptr, 0, [](LPVOID) -> DWORD {
            Sleep(ct_ms);
            fprintf(stderr, "[crash] TOTK_PC_CRASHTEST: %s now\n", ct_abort ? "abort()" : "writing to address 0");
            if (ct_abort) abort();
            static volatile uintptr_t zero;   // (not a literal null: the compiler may turn that into ud2)
            *(volatile int*)zero = 1;
            return 0;
        }, nullptr, 0, nullptr));
    }
#endif
    // TOTK_NVN_PROBE=name1,name2,...: resolve NVN procs through the game's own (lifted) driver loader and print their
    // sdk offsets, to read real driver behaviour (e.g. nvnDeviceGetInteger constants) out of the sdk code.
    if (const char* probe = getenv("TOTK_NVN_PROBE")) {
        Ctx* c = rt_new_ctx(0, nullptr, nullptr); rt_bind_thread(c);
        GuestFn boot = lifted("nvnBootstrapLoader");
        if (!boot) die("probe: no lifted nvnBootstrapLoader");
        static char nm[256];
        strcpy(nm, "nvnDeviceGetProcAddress"); c->x[0] = (u64)nm; c->x[30] = 0; boot(c);
        u64 gpa = c->x[0];
        printf("[probe] nvnBootstrapLoader(nvnDeviceGetProcAddress) = %llx (sdk+%llx)\n", gpa, gpa - SDK_BASE);
        for (std::string s = probe; !s.empty();) {
            size_t q = s.find(','); std::string one = s.substr(0, q);
            strcpy(nm, one.c_str());
            c->x[0] = (u64)nm; boot(c); u64 a1 = c->x[0];
            u64 a2 = 0;
            if (gpa) { c->x[0] = 0; c->x[1] = (u64)nm; c->x[30] = 0; call_ind(c, gpa); a2 = c->x[0]; }
            printf("[probe] %s: bootstrap %llx (sdk+%llx)  getprocaddress %llx (sdk+%llx)\n", nm, a1, a1 - SDK_BASE, a2, a2 - SDK_BASE);
            s = q == std::string::npos ? "" : s.substr(q + 1);
        }
        fflush(stdout);
        return 0;
    }
    HANDLE h = CreateThread(nullptr, 1ull << 30, boot_thread, nullptr, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (const char* w = getenv("TOTK_WATCHDOG"))      // dump every guest thread's position every N seconds (3 times)
        CreateThread(nullptr, 0, [](LPVOID p) -> DWORD {
            for (int i = 0; i < 3; i++) { Sleep((DWORD)(uintptr_t)p * 1000); watchdog_dump(); }
            return 0;
        }, (LPVOID)(uintptr_t)atoi(w), 0, nullptr);
#ifdef _WIN32
    // TOTK_STALL_DUMP=<ms> (stall hunting, present's menu / save bursts): when no frame is presented for <ms> after the
    // first 600 presents, print the HLE call every guest thread sits in, then sample them for 1 s (the [prof] lines; with
    // TOTK_PROF_STACKS=2 wall stacks in profwall.txt, host frames included: a file wait shows its exe caller). 8 stalls.
    if (const char* w = getenv("TOTK_STALL_DUMP"); w && atoi(w) > 0)
        CreateThread(nullptr, 0, [](LPVOID p) -> DWORD {
            const u64 lim = (u64)(uintptr_t)p;
            // (TOTK_STALL_START=1: also before the first 600 presents: a start-up that never presents)
            static const bool from_start = getenv("TOTK_STALL_START") && atoi(getenv("TOTK_STALL_START")) != 0;
            u64 last = renderer_present_count(), t = GetTickCount64();
            for (int n = 0; n < 8;) {
                Sleep(10);
                u64 c = renderer_present_count(), now = GetTickCount64();
                if (c != last || (c < 600 && !from_start)) { last = c; t = now; continue; }
                if (now - t < lim) continue;
                {
                    std::lock_guard<std::mutex> l(g_threads_mtx);
                    printf("[stall] no present for %llu ms after present %llu; guest threads:", (unsigned long long)(now - t), (unsigned long long)c);
                    for (auto& g : g_threads)
                        if (const char* cur = g.hle_cur ? *g.hle_cur : nullptr) printf(" %lu:%s", g.tid, cur);
                    printf("\n");
                }
                profile_threads(50, 20);
                fflush(stdout);
                n++;
                while (renderer_present_count() == c) Sleep(10);
                last = renderer_present_count(); t = GetTickCount64();
            }
            return 0;
        }, (LPVOID)(uintptr_t)atoi(w), 0, nullptr);
#endif
#ifdef TOTK_PGO_GEN
    // PGO instrumented build (mkninja TOTK_PGO=gen): write the profile at TOTK_PGO_DUMP seconds (default 150), since test
    // runs end by timeout and the at-exit write never runs then
    CreateThread(nullptr, 0, [](LPVOID) -> DWORD {
        const char* e = getenv("TOTK_PGO_DUMP");
        Sleep((DWORD)(e ? atoi(e) : 150) * 1000);
        int r = __llvm_profile_write_file();
        printf("[pgo] profile written (%d)\n", r);
        fflush(stdout);
        return 0;
    }, nullptr, 0, nullptr);
#endif
#ifdef _WIN32
    // TOTK_TIMER_1MS=1: hold a 1 ms timer resolution for the whole run. Nothing in the runtime asks for one, so Sleep() and
    // the timed nn::os waits (condition_variable::wait_for) get whatever the process happens to have; at the default 15.6
    // ms every short timed wait costs a tick (n1q, p1: game threads parked in WaitLightEvent / YieldThread / SleepThread at
    // 5-10 fps, guest CPU 20-40 % of a core). winmm by name: no link change.
    if (const char* e = getenv("TOTK_TIMER_1MS"); e && atoi(e))
        if (HMODULE wm = LoadLibraryA("winmm.dll"))
            if (auto tbp = (unsigned(WINAPI*)(unsigned))GetProcAddress(wm, "timeBeginPeriod"))
                printf("[timer] timeBeginPeriod(1) = %u\n", tbp(1));
    // TOTK_TIMER_PROBE=<s>: every <s> seconds, how long a Sleep(1) and a 1 ms high-resolution timer wait really take on a
    // normal thread, the system timer resolution and the process priority class. Tells a coarse timer (Sleep(1) ~15.6 ms,
    // high-res on time) from CPU starvation (both late) from a demoted process (priority class).
    if (const char* p = getenv("TOTK_TIMER_PROBE"); p && atoi(p) > 0)
        CreateThread(nullptr, 0, [](LPVOID p) -> DWORD {
            const DWORD every = (DWORD)(uintptr_t)p * 1000;
            HANDLE hr = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
            typedef LONG(NTAPI * QueryRes)(PULONG, PULONG, PULONG);
            auto qtr = (QueryRes)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryTimerResolution");
            LARGE_INTEGER f; QueryPerformanceFrequency(&f);
            auto now_ms = [&] { LARGE_INTEGER c; QueryPerformanceCounter(&c); return (double)c.QuadPart * 1e3 / f.QuadPart; };
            for (;;) {
                Sleep(every);
                double a = now_ms(); Sleep(1); double s1 = now_ms() - a, h1 = -1;
                LARGE_INTEGER due; due.QuadPart = -10000;
                a = now_ms();
                if (hr && SetWaitableTimer(hr, &due, 0, nullptr, nullptr, FALSE)) { WaitForSingleObject(hr, INFINITE); h1 = now_ms() - a; }
                ULONG coarsest = 0, finest = 0, cur = 0;   // 100 ns units
                if (qtr) qtr(&coarsest, &finest, &cur);
                printf("[timer] t=%.0f s Sleep(1) %.2f ms, high-res 1 ms wait %.2f ms, system resolution %.2f ms, priority class %lx\n",
                       (GetTickCount64() - g_prof_t0) / 1e3, s1, h1, cur / 1e4, GetPriorityClass(GetCurrentProcess()));
                fflush(stdout);
            }
        }, (LPVOID)(uintptr_t)atoi(p), 0, nullptr);
#endif
#ifdef _WIN32
    if (const char* h = getenv("TOTK_HITCH_STACKS"); h && *h && *h != '0') { g_hitch_on = true; std::thread(hitch_thread).detach(); }
#endif
    if (const char* w = getenv("TOTK_PROF"))          // TOTK_PROF=<s>[,<s>...]: sample every guest thread for ~3 s (TOTK_PROF_LEN) at those times
        CreateThread(nullptr, 0, [](LPVOID p) -> DWORD {
            const char* s = (const char*)p;
            while (*s) {
                // at <s> after the process started, or right after the previous window when that one ran late, so
                // TOTK_PROF=0,3,6,... gives back-to-back windows
                DWORD t = (DWORD)atoi(s);
                double wait = t - rt_uptime_s();
                if (wait > 0) Sleep((DWORD)(wait * 1000));
                printf("[prof] --- sampling at %lus ---\n", (unsigned long)t);
                // TOTK_PROF_LEN=<s>: a longer window than the default ~3 s (1000 samples every 3 ms)
                static const int len = getenv("TOTK_PROF_LEN") ? atoi(getenv("TOTK_PROF_LEN")) : 0;
                // (wall stacks suspend and walk all ~60 threads per sample: every 10 ms, not 3, to keep the cost down)
                static const int every = getenv("TOTK_PROF_STACKS") && atoi(getenv("TOTK_PROF_STACKS")) == 2 ? 10 : 3;
                profile_threads((len > 3 ? len : 3) * 1000 / every, every);
                fflush(stdout);
                while (*s && *s != ',') s++;
                if (*s) s++;
            }
            return 0;
        }, (LPVOID)w, 0, nullptr);
    // TOTK_LOADTRACE=<s>: for the first <s> seconds, a line every 100 ms in which something moved (else every 0.5 s): seconds since the
    // process was created, presents (and render passes a present: a loading screen draws under ~20, the world over 100), and the fs
    // reads (MiB, reads, opens, wall ms in reads summed over threads). Every other log line falls between two of these, which dates it
    // to 0.1 s. tools/engine/cpuprof/loads.py reads them.
    if (const char* lt = getenv("TOTK_LOADTRACE"); lt && atoi(lt) > 0)
        std::thread([secs = atoi(lt)] {
            unsigned long long was[4] = {}, s[4];
            u64 was_p = 0, was_pass = 0;
            double said = 0;
            while (rt_uptime_s() < secs) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                hle_fs_read_stats(s);
                u64 p = nvn_present_count(), pass = vk_exec_passes_total();
                // (still a line every 0.5 s when nothing moved: the device and window setup before the first present
                // reads nothing and presents nothing, and its log lines need dates too)
                if (p == was_p && s[1] == was[1] && rt_uptime_s() - said < 0.5) continue;
                said = rt_uptime_s();
                printf("[load] t=%.2f presents %llu +%llu (%.0f passes each) fs %.1f MiB +%.1f, reads %llu +%llu, opens %llu, "
                       "read ms %.1f +%.1f\n", rt_uptime_s(), (unsigned long long)p, (unsigned long long)(p - was_p),
                       p > was_p ? (double)(pass - was_pass) / (p - was_p) : 0.0, s[0] / 1048576.0, (s[0] - was[0]) / 1048576.0,
                       s[1], s[1] - was[1], s[2], s[3] / 1e6, (s[3] - was[3]) / 1e6);
                fflush(stdout);
                memcpy(was, s, sizeof s); was_p = p; was_pass = pass;
            }
        }).detach();
    // The window + Vulkan device live on the main thread (runtime/nvn/vulkan/device.c ui_run: creates them, then pumps
    // window events forever); the game boots on its own thread and its first nvnDeviceInitialize waits for the UI.
    // A watcher ends the process when nnMain returns. TOTK_HEADLESS=1 keeps the old console-only run.
    if (!getenv("TOTK_HEADLESS")) {
        static HANDLE boot; boot = h;
        CreateThread(nullptr, 0, [](LPVOID) -> DWORD { WaitForSingleObject(boot, INFINITE); print_stats(); fflush(stdout); pc_input_keys_restore(); ExitProcess(0); }, nullptr, 0, nullptr);
        return ui_run();
    }
    WaitForSingleObject(h, INFINITE);
    print_stats();
    return 0;
}

// Give a host function a guest code address (after the import thunks) so guest code can call it through a pointer
// (e.g. NVN procs returned by nvnDeviceGetProcAddress).
u64 rt_host_fn_addr(GuestFn fn) {
    static std::atomic<u32> next{0};
    u32 i = next++;
    if (i >= (u32)kMaxHostFns) { fprintf(stderr, "[rt] out of host fn slots\n"); abort(); }
    g_ftab[(IMP_FAKE - MAIN_BASE) / 4 + kMaxImports + i] = fn;
    return IMP_FAKE + 4ull * (kMaxImports + i);
}
