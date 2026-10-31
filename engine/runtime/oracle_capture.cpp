// Real entry states for the bit-exact gate. A capture key is built with gen.py --trace <the functions>, and the boot runs
// with TOTK_ORACLE_CAPTURE=<dir>. Then rt_trace (rt_main.cpp) sends each traced entry here instead of printing it. At most
// `max` times per function, spaced `period_s` apart after `warmup_s`, the Ctx and the guest pages it reaches are written
// to <dir>/<fn>_<n>.ocap. Those pages are the stack around sp, the pages its registers point to, the function's static
// roots from the config (the image pages its adrp constants name), and two more pointer hops from there, up to `pages`
// pages. tools/oracle/capture.py writes <dir>/capture.cfg and reads the files, and the oracle replays them in Unicorn and
// in the harness instead of random states. Without the env variable, rt_trace is unchanged, and no shipped key has
// --trace.
#ifdef _WIN32
#include "rt.h"

#include <windows.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

u64 rt_tpidrro(Ctx*);

namespace {
constexpr u64 kPage = 0x1000;
constexpr u32 kTlsBytes = 0x200;   // rt_tpidrro's thread_local block (rt_main.cpp)

struct Fn {
    std::atomic<u64> addr{0};
    std::atomic<u64> next_ms{0};
    std::atomic<u32> taken{0};
    std::vector<u64> roots;
};
struct Cfg {
    bool on = false;
    std::string dir;
    u32 max = 8, pages = 96;
    u64 warmup_ms = 90000, period_ms = 30000;
    std::vector<std::pair<u64, u64>> ranges;   // image data + bss, [lo, hi)
    u64 heap_base = 0, start_ms = 0;
    Fn* tab = nullptr;
    u32 mask = 0;
    std::atomic<u32> files{0};
};
Cfg g;

u32 slot_of(u64 a) { return (u32)((a >> 2) * 0x9E3779B97F4A7C15ull >> 40); }

Fn* find(u64 fn) {
    if (!g.tab) return nullptr;
    for (u32 i = slot_of(fn) & g.mask, n = 0; n <= g.mask; i = (i + 1) & g.mask, n++) {
        const u64 a = g.tab[i].addr.load(std::memory_order_relaxed);
        if (a == fn) return &g.tab[i];
        if (!a) return nullptr;
    }
    return nullptr;
}

void load() {
    const char* d = getenv("TOTK_ORACLE_CAPTURE");
    if (!d || !*d) return;
    g.dir = d;
    FILE* f = fopen((g.dir + "\\capture.cfg").c_str(), "r");
    if (!f) {
        printf("[capture] %s\\capture.cfg missing: capture off\n", d);
        return;
    }
    std::vector<std::pair<u64, std::vector<u64>>> fns;
    char line[8192];
    while (fgets(line, sizeof line, f)) {
        char* p = line;
        char kw[16] = {};
        int used = 0;
        if (sscanf(p, "%15s%n", kw, &used) != 1 || kw[0] == '#') continue;
        p += used;
        auto num = [&](u64* v) {
            char* e;
            const u64 r = strtoull(p, &e, 0);
            if (e == p) return false;
            *v = r, p = e;
            return true;
        };
        u64 a = 0, b = 0;
        if (!strcmp(kw, "max") && num(&a)) g.max = (u32)a;
        else if (!strcmp(kw, "pages") && num(&a)) g.pages = (u32)a;
        else if (!strcmp(kw, "warmup_s") && num(&a)) g.warmup_ms = a * 1000;
        else if (!strcmp(kw, "period_s") && num(&a)) g.period_ms = a * 1000;
        else if (!strcmp(kw, "range") && num(&a) && num(&b)) g.ranges.push_back({a, b});
        else if (!strcmp(kw, "fn") && num(&a)) {
            std::vector<u64> roots;
            while (num(&b)) roots.push_back(b & ~(kPage - 1));
            fns.push_back({a, std::move(roots)});
        }
    }
    fclose(f);
    u32 cap = 16;
    while (cap < fns.size() * 2) cap <<= 1;
    g.tab = new Fn[cap];
    g.mask = cap - 1;
    for (auto& [a, roots] : fns) {
        u32 i = slot_of(a) & g.mask;
        while (g.tab[i].addr.load() && g.tab[i].addr.load() != a) i = (i + 1) & g.mask;
        g.tab[i].roots = roots;
        g.tab[i].addr.store(a);
    }
    const char* h = getenv("TOTK_HEAP_ADDR");
    MEMORY_BASIC_INFORMATION mi;
    const u64 want = h ? strtoull(h, nullptr, 0) : 0x1000000000ull;
    if (want && VirtualQuery((void*)want, &mi, sizeof mi)) g.heap_base = (u64)mi.AllocationBase;
    g.start_ms = GetTickCount64();
    g.on = true;
    printf("[capture] on: %zu functions, max %u each, %u pages, warmup %llu s, period %llu s, %zu ranges -> %s\n",
           fns.size(), g.max, g.pages, (unsigned long long)(g.warmup_ms / 1000),
           (unsigned long long)(g.period_ms / 1000), g.ranges.size(), d);
}

// a guest page this capture may copy: committed, readable, and in the guest heap, the sp's own allocation or an
// image's data (host pointers, e.g. Ctx::host, are not guest state)
bool page_ok(u64 page, u64 sp_base) {
    if (page < 0x10000 || page >= (1ull << 47)) return false;
    bool in = false;
    for (auto& [lo, hi] : g.ranges) in |= page >= lo && page < hi;
    MEMORY_BASIC_INFORMATION mi;
    if (!VirtualQuery((void*)page, &mi, sizeof mi) || mi.State != MEM_COMMIT) return false;
    if (mi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) return false;
    const DWORD rd = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                     PAGE_EXECUTE_WRITECOPY;
    if (!(mi.Protect & rd)) return false;
    const u64 base = (u64)mi.AllocationBase;
    return in || (g.heap_base && base == g.heap_base) || (sp_base && base == sp_base);
}

bool copy_page(u64 page, u8* out) {
    __try {
        memcpy(out, (const void*)page, kPage);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

struct Snap {
    std::vector<u64> addr;
    std::vector<u8> data;
    std::unordered_set<u64> seen;
    u64 sp_base = 0;
    u32 cap = 0;
    const u8* at(u64 a) const {   // a captured byte, or null
        const u64 p = a & ~(kPage - 1);
        for (size_t i = 0; i < addr.size(); i++)
            if (addr[i] == p) return &data[i * kPage + (a - p)];
        return nullptr;
    }
    bool add(u64 a) {   // the page holding a; true when it is (now) in the snapshot
        const u64 p = a & ~(kPage - 1);
        if (seen.count(p)) return at(p) != nullptr;
        if (addr.size() >= cap) return false;
        seen.insert(p);
        if (!page_ok(p, sp_base)) return false;
        data.resize((addr.size() + 1) * kPage);
        if (!copy_page(p, &data[addr.size() * kPage])) {
            data.resize(addr.size() * kPage);
            return false;
        }
        addr.push_back(p);
        return true;
    }
    // the pointers among the words of [a, a + n) that are themselves captured
    void follow(u64 a, u64 n, std::vector<u64>* out) {
        for (u64 o = 0; o + 8 <= n; o += 8) {
            const u8* b = at((a & ~7ull) + o);
            if (!b) continue;
            u64 w;
            memcpy(&w, b, 8);
            if (w >= 0x10000 && w < (1ull << 47) && add(w)) {
                add(w + 0xff);   // an object straddling its page end
                out->push_back(w);
            }
        }
    }
};

void capture(Ctx* c, u64 fn, Fn* s, u32 n) {
    Snap sn;
    sn.cap = g.pages;
    MEMORY_BASIC_INFORMATION mi;
    if (VirtualQuery((void*)c->sp, &mi, sizeof mi)) sn.sp_base = (u64)mi.AllocationBase;
    const u64 sp_page = c->sp & ~(kPage - 1);
    for (u64 p = sp_page - 4 * kPage; p < sp_page + 2 * kPage; p += kPage) sn.add(p);   // its frame and its caller's
    std::vector<u64> d1, d2, d3;
    for (int i = 0; i < 30; i++) {
        const u64 r = c->x[i];
        if (r >= 0x10000 && r < (1ull << 47) && sn.add(r)) {
            sn.add(r + 0x1ff);
            d1.push_back(r);
        }
    }
    if (c->tpidr && sn.add(c->tpidr)) d1.push_back(c->tpidr);
    for (u64 r : s->roots)
        if (sn.add(r)) sn.follow(r, kPage, &d2);   // GOT / globals: every word of the page
    for (u64 r : d1) sn.follow(r, 0x200, &d2);   // the object's fields
    for (u64 r : d2) sn.follow(r, 0x100, &d3);
    const u64 tls = rt_tpidrro(c);
    u8 tlsb[kTlsBytes] = {};
    __try {
        memcpy(tlsb, (const void*)tls, kTlsBytes);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    char name[64];
    snprintf(name, sizeof name, "\\%llx_%u.ocap", (unsigned long long)fn, n);
    FILE* f = fopen((g.dir + name).c_str(), "wb");
    if (!f) return;
    const u32 hdr[2] = {0x5041434fu /* 'OCAP' */, 1};
    const u64 h2[2] = {fn, (u64)GetTickCount64() - g.start_ms};
    const u32 h3[2] = {GetCurrentThreadId(), (u32)sn.addr.size()};
    fwrite(hdr, 4, 2, f);
    fwrite(h2, 8, 2, f);
    fwrite(h3, 4, 2, f);
    fwrite(c->x, 8, 31, f);
    fwrite(&c->sp, 8, 1, f);
    fwrite(c->v, 16, 32, f);
    const u32 tail[4] = {(u32)(c->n & 1) << 31 | (u32)(c->z & 1) << 30 | (u32)(c->c & 1) << 29 | (u32)(c->vf & 1) << 28,
                         c->fpcr, c->fpsr, 0};
    fwrite(tail, 4, 4, f);
    const u64 t2[2] = {c->tpidr, tls};
    fwrite(t2, 8, 2, f);
    fwrite(tlsb, 1, kTlsBytes, f);
    for (size_t i = 0; i < sn.addr.size(); i++) {
        fwrite(&sn.addr[i], 8, 1, f);
        fwrite(&sn.data[i * kPage], 1, kPage, f);
    }
    fclose(f);
    const u32 total = g.files.fetch_add(1) + 1;
    if (total <= 50 || total % 100 == 0)
        printf("[capture] %llx #%u: %zu pages (%u files)\n", (unsigned long long)fn, n, sn.addr.size(), total);
}
}  // namespace

bool oracle_capture_on() {
    static std::once_flag once;
    std::call_once(once, load);
    return g.on;
}

void oracle_capture(Ctx* c, u64 fn) {
    Fn* s = find(fn);
    if (!s) return;
    const u64 now = GetTickCount64();
    u64 due = s->next_ms.load(std::memory_order_relaxed);
    if (now - g.start_ms < g.warmup_ms || now < due) return;
    if (!s->next_ms.compare_exchange_strong(due, now + g.period_ms)) return;   // another thread took this slot
    const u32 n = s->taken.fetch_add(1);
    if (n >= g.max) return;
    capture(c, fn, s, n);
}
#else
#include "rt.h"
bool oracle_capture_on() { return false; }   // (Windows only: the capture reads guest pages with VirtualQuery / SEH)
void oracle_capture(Ctx*, u64) {}
#endif
